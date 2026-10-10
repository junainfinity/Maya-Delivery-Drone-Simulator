#include "MAYANavigationNativeTest.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryHUD.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryGameMode.h"
#include "MAYADeliveryParcel.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Kismet/GameplayStatics.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "CoreGlobals.h"

namespace
{
    TSharedPtr<FJsonObject> ParseObject(const FString& Text)
    { TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),J); return J; }
    double Number(const TSharedPtr<FJsonObject>& J,const TCHAR* Field,double Missing=-1)
    { double V=Missing; if(J)J->TryGetNumberField(Field,V); return V; }
    bool Flag(const TSharedPtr<FJsonObject>& J,const TCHAR* Field)
    { bool V=false; if(J)J->TryGetBoolField(Field,V); return V; }
    FString Word(const TSharedPtr<FJsonObject>& J,const TCHAR* Field)
    { FString V; if(J)J->TryGetStringField(Field,V); return V; }
    bool FiniteValue(const TSharedPtr<FJsonValue>& V)
    {
        if(!V)return false;
        if(V->Type==EJson::Number)return FMath::IsFinite(V->AsNumber());
        if(V->Type==EJson::Array){for(const auto& Item:V->AsArray())if(!FiniteValue(Item))return false;}
        if(V->Type==EJson::Object){for(const auto& Pair:V->AsObject()->Values)if(!FiniteValue(Pair.Value))return false;}
        return true;
    }
    bool FiniteObject(const TSharedPtr<FJsonObject>& J)
    { if(!J)return false; for(const auto& Pair:J->Values)if(!FiniteValue(Pair.Value))return false; return true; }
    bool CameraPosesMatch(const TSharedPtr<FJsonObject>& J)
    {
        if(!J->HasTypedField<EJson::Object>(TEXT("calculatedPawnViewWorldPose"))||!J->HasTypedField<EJson::Object>(TEXT("actualCameraManagerWorldPose")))return false;
        const auto A=J->GetObjectField(TEXT("calculatedPawnViewWorldPose")),B=J->GetObjectField(TEXT("actualCameraManagerWorldPose"));
        for(const TCHAR* Field:{TEXT("xCm"),TEXT("yCm"),TEXT("zCm"),TEXT("qx"),TEXT("qy"),TEXT("qz"),TEXT("qw")})if(!A->HasTypedField<EJson::Number>(Field)||!B->HasTypedField<EJson::Number>(Field))return false;
        const FVector PA(Number(A,TEXT("xCm")),Number(A,TEXT("yCm")),Number(A,TEXT("zCm"))),PB(Number(B,TEXT("xCm")),Number(B,TEXT("yCm")),Number(B,TEXT("zCm")));
        const FQuat QA(Number(A,TEXT("qx")),Number(A,TEXT("qy")),Number(A,TEXT("qz")),Number(A,TEXT("qw"))),QB(Number(B,TEXT("qx")),Number(B,TEXT("qy")),Number(B,TEXT("qz")),Number(B,TEXT("qw")));
        return QA.IsNormalized()&&QB.IsNormalized()&&FVector::Dist(PA,PB)<=.2&&FMath::RadiansToDegrees(QA.AngularDistance(QB))<=.1
            &&FMath::Abs(Number(J,TEXT("actualCameraManagerFovDegrees"))-Number(J,TEXT("fovDegrees")))<=.05;
    }
    double OpticalYaw(const FMAYADeliveryRobotObservation& O)
    { return (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw; }
    FIntPoint ViewportSize()
    { return GEngine&&GEngine->GameViewport&&GEngine->GameViewport->Viewport?GEngine->GameViewport->Viewport->GetSizeXY():FIntPoint::ZeroValue; }
}

AMAYANavigationNativeTest::AMAYANavigationNativeTest()
{PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.bTickEvenWhenPaused=true;PrimaryActorTick.TickGroup=TG_PostUpdateWork;}
void AMAYANavigationNativeTest::Configure(AMAYANativeDemoPawn* InRobot,AMAYADeliveryDirector* InDirector,const TArray<FVector>& MeasuredRoute)
{Robot=InRobot;Director=InDirector;Path.Build(MeasuredRoute);}
bool AMAYANavigationNativeTest::Check(bool Passed,const FString& Name)
{auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),Name);J->SetBoolField(TEXT("passed"),Passed);Checks.Add(MakeShared<FJsonValueObject>(J));return Passed;}
void AMAYANavigationNativeTest::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    SetActorTickEnabled(false);return;
#else
    if(!FParse::Param(FCommandLine::Get(),TEXT("MayaV01NavigationTest"))||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))
        ||!FParse::Value(FCommandLine::Get(),TEXT("MayaV01Output="),Output)||Output.IsEmpty()) {SetActorTickEnabled(false);return;}
    bHUDStyleTest=FParse::Param(FCommandLine::Get(),TEXT("MayaV01HUDStyleTest"));
    Output=FPaths::ConvertRelativePathToFull(Output);StartedWorld=GetWorld()->GetTimeSeconds();StartedReal=FPlatformTime::Seconds();Enter(0);
    if(IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("result.json")))){bFinished=true;SetActorTickEnabled(false);FPlatformMisc::RequestExit(false,TEXT("MAYANavigationNativeTest"));return;}
    ActualSettingsPath=FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"),nullptr,*FPaths::GeneratedConfigDir()));FPaths::NormalizeFilename(ActualSettingsPath);
    FString Expected=FPaths::Combine(Output,TEXT("GameUserSettings.ini"));FPaths::NormalizeFilename(Expected);
    PC=Robot.IsValid()?Cast<AMAYADeliveryPlayerController>(Robot->GetController()):nullptr;
    HUD=PC.IsValid()?Cast<AMAYADeliveryHUD>(PC->GetHUD()):nullptr;
    if(!Check(FApp::CanEverRender()&&Robot.IsValid()&&Director.IsValid()&&PC.IsValid()&&HUD.IsValid()
        &&GetWorld()->GetMapName().Contains(TEXT("Small_City"))&&Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()),TEXT("Rendered actual Small City robot/controller/HUD"))
        ||!Check(GConfig&&GConfig->FindConfigFile(GGameUserSettingsIni)&&ActualSettingsPath.Equals(Expected,ESearchCase::IgnoreCase),TEXT("Actual settings resolve to isolated attempt file"))
        ||!Check(Path.Points.Num()>=2&&Path.Arc.Last()>=800,TEXT("Existing measured depot route has bounded 700cm driving room")))
    {Finish(false,TEXT("Navigation initialization refused"));return;}
    ScreenshotHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this,&AMAYANavigationNativeTest::ScreenshotProcessed);
    const auto& S=Director->Snapshot();BaseCash=S.TotalCredits;BaseFees=S.RechargeFeesPaidDollars;BaseCompleted=S.CompletedJobs;ShiftId=S.ShiftInstanceId;InitialProfile=S.Job.ProfileId;
    if(bHUDStyleTest){PC->ConsoleCommand(TEXT("r.SetRes 2560x1440w"),false);Enter(50);}
#endif
}
bool AMAYANavigationNativeTest::Drive(float Throttle,float Steer,bool Brake)
{++DriveCalls;return Robot.IsValid()&&Robot->SubmitDriveCommand(Throttle,Steer,Brake,EMAYADriveCommandOrigin::NativeAutomation,.25f,false);}
bool AMAYANavigationNativeTest::Quiet(const FMAYADeliveryRobotObservation& O) const
{return O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&Robot.IsValid()&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();}
bool AMAYANavigationNativeTest::Key(FKey Value,EInputEvent Event)
{
    const bool Handled=PC.IsValid()&&PC->InputKey(FInputKeyParams(Value,Event,Event==IE_Released?0.0:1.0));
    if(Event==IE_Released)HeldKeys.Remove(Value);else HeldKeys.Add(Value);
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("key"),Value.ToString());J->SetNumberField(TEXT("event"),int32(Event));J->SetBoolField(TEXT("callbackReturnedHandled"),Handled);J->SetNumberField(TEXT("gameFrame"),double(GFrameCounter));J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());Actions.Add(MakeShared<FJsonValueObject>(J));
    if(Event==IE_Pressed&&Value!=EKeys::W){PendingRelease=Value;KeyFrame=GFrameCounter;}return Handled;
}
bool AMAYANavigationNativeTest::Action(FName Name,int32 Direction)
{
    const bool Accepted=PC.IsValid()&&PC->ActivateUIAction(Name,Direction);auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("uiAction"),Name.ToString());J->SetNumberField(TEXT("direction"),Direction);J->SetBoolField(TEXT("accepted"),Accepted);J->SetNumberField(TEXT("gameFrame"),double(GFrameCounter));Actions.Add(MakeShared<FJsonValueObject>(J));return Accepted;
}
void AMAYANavigationNativeTest::Enter(int32 Next)
{Step=Next;StepReal=FPlatformTime::Seconds();StepWorld=GetWorld()->GetTimeSeconds();EnteredFrame=GFrameCounter;StableSince=-1;}

bool AMAYANavigationNativeTest::ObserveMinimap(const FMAYADeliveryRobotObservation& O)
{
    auto J=ParseObject(HUD->ReadMinimapEvidenceJson());
    if(!J||Number(J,TEXT("radarWorldSpanCm"))!=4000||Number(J,TEXT("radarWorldCellSizeCm"))!=50||!Flag(J,TEXT("radarNorthUp"))
        ||Number(J,TEXT("radarActualTracesLastFrame"))<0||Number(J,TEXT("radarActualTracesLastFrame"))>24
        ||Number(J,TEXT("radarSampledCells"))>9216||!FiniteObject(J))return false;
    const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
    if(J->TryGetArrayField(TEXT("radarLastFrameSampleReceipts"),Rows))for(const auto& V:*Rows)
    {
        const auto R=V->AsObject();if(!R)continue;
        const FIntPoint Cell(int32(Number(R,TEXT("worldCellX"))),int32(Number(R,TEXT("worldCellY"))));
        const int32 Kind=int32(Number(R,TEXT("effectiveSurfaceEnum")));const FString Mesh=Word(R,TEXT("knownMesh"));
        bSawRoadIdentity|=Kind==1&&Mesh.StartsWith(TEXT("/Game/Road/Kit_City_Road/SM_ROAD_"));
        bSawSidewalkIdentity|=Kind==2&&(Mesh.StartsWith(TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_"))||Mesh.StartsWith(TEXT("/Game/Megascans/3D_Assets/Modular_Sidewalk_Corner_4m_")));
        UnknownObservations+=!Flag(R,TEXT("latestQueryKnown"));
        if(Step<=1&&InitialCells.Num()<16&&Kind>0&&Flag(R,TEXT("latestQueryKnown")))InitialCells.FindOrAdd(Cell)=R;
        else if(Step>=2)if(const auto* Old=InitialCells.Find(Cell))
        {
            if(Number(R,TEXT("lastAttemptMonotonicSeconds"))>Number(*Old,TEXT("lastAttemptMonotonicSeconds")))++RefreshObservations;
            if(MaximumTranslation>=600&&Word(*Old,TEXT("knownMesh"))==Mesh&&Kind>0&&Number(R,TEXT("lastAttemptMonotonicSeconds"))>Number(*Old,TEXT("lastAttemptMonotonicSeconds")))
            {if(!bOverlapRefreshed)CacheIdentities.Add(MakeShared<FJsonValueObject>(R));bOverlapRefreshed=true;}
        }
    }
    if(Step>=2&&Step<=8)MinimumMovingCacheCount=FMath::Min(MinimumMovingCacheCount,Number(J,TEXT("radarSampledCells")));
    if(Quiet(O)&&FVector2D::Distance(FVector2D(Number(J,TEXT("radarWorldCentreX")),Number(J,TEXT("radarWorldCentreY"))),FVector2D(O.ChassisPose.GetLocation()))>5)return false;
    return true;
}
bool AMAYANavigationNativeTest::ObserveFullMap(const FString& ExpectedView,TSharedPtr<FJsonObject>& Out)
{
    Out=ParseObject(HUD->ReadFullMapEvidenceJson());
    if(!Out||Number(Out,TEXT("lastDrawGameFrame"))<=double(EnteredFrame)||Word(Out,TEXT("view"))!=ExpectedView)return false;
    if(!FiniteObject(Out)||!Flag(Out,TEXT("paused"))||!Flag(Out,TEXT("playerKnown"))||Number(Out,TEXT("roadLanes"))<=0
        ||Number(Out,TEXT("sidewalkLanes"))<=0||Number(Out,TEXT("segments"))<=0||Number(Out,TEXT("pixelsPerCm"))<=0
        ||Number(Out,TEXT("worldMaxX"))<=Number(Out,TEXT("worldMinX"))||Number(Out,TEXT("worldMaxY"))<=Number(Out,TEXT("worldMinY")))return false;
    const auto& O=Director->Observation();const FVector Pos=O.ChassisPose.GetLocation();const double Scale=Number(Out,TEXT("pixelsPerCm"));
    auto Matches=[&](AMAYADeliveryStation* Station,const TCHAR* X,const TCHAR* Y)
    {return Station&&FMath::Abs(Number(Out,X)-(Number(Out,TEXT("playerX"))+(Station->GetServicePoint().X-Pos.X)*Scale))<.1
        &&FMath::Abs(Number(Out,Y)-(Number(Out,TEXT("playerY"))-(Station->GetServicePoint().Y-Pos.Y)*Scale))<.1;};
    auto* Objective=Director->ObjectiveStation();auto* Delivery=Director->FindStation(Director->Snapshot().Job.DropoffStationId);
    if(!Objective||!Delivery||Word(Out,TEXT("objectiveStationId"))!=Objective->StationId||Word(Out,TEXT("deliveryStationId"))!=Delivery->StationId
        ||!Matches(Objective,TEXT("objectiveX"),TEXT("objectiveY"))||!Matches(Delivery,TEXT("deliveryX"),TEXT("deliveryY")))return false;
    const TArray<TSharedPtr<FJsonValue>>* Chargers=nullptr;if(!Out->TryGetArrayField(TEXT("chargers"),Chargers))return false;
    int32 Count=0;for(TActorIterator<AMAYADeliveryStation> It(GetWorld());It;++It)if(Director->FindStation(It->StationId)==*It&&It->IsChargingStation())++Count;
    if(Chargers->Num()!=Count||Count<1)return false;
    TSet<FString> Unique;for(const auto& V:*Chargers){auto R=V->AsObject();const FString Id=Word(R,TEXT("stationId"));auto* Station=Director->FindStation(Id);
        if(!Station||!Station->IsChargingStation()||Unique.Contains(Id))return false;
        if(FMath::Abs(Number(R,TEXT("x"))-(Number(Out,TEXT("playerX"))+(Station->GetServicePoint().X-Pos.X)*Scale))>=.1
            ||FMath::Abs(Number(R,TEXT("y"))-(Number(Out,TEXT("playerY"))-(Station->GetServicePoint().Y-Pos.Y)*Scale))>=.1)return false;Unique.Add(Id);}
    return true;
}
bool AMAYANavigationNativeTest::ObserveCamera(bool Pov,TSharedPtr<FJsonObject>& Out)
{
    Out=ParseObject(Robot->ReadPlayerCameraModeJson());const FString Mode=Pov?TEXT("POV"):TEXT("Chase");
    if(!Out||Word(Out,TEXT("selectedMode"))!=Mode||Word(Out,TEXT("lastCalculatedMode"))!=Mode||Number(Out,TEXT("viewSamples"))<=LastViewSamples)return false;
    if(!FiniteObject(Out)||!CameraPosesMatch(Out)||!Flag(Out,TEXT("calculatedViewKnown"))||!Flag(Out,TEXT("cameraManagerIsRobotView"))
        ||!Out->HasTypedField<EJson::Object>(TEXT("actualCameraManagerWorldPose"))||Number(Out,TEXT("fovDegrees"))<=10||Number(Out,TEXT("fovDegrees"))>=170
        ||Number(Out,TEXT("actualCameraManagerCacheWorldSeconds"))<GetWorld()->GetTimeSeconds()-1)return false;
    if(Pov&&(!Flag(Out,TEXT("povMountKnown"))||Number(Out,TEXT("actualCosmeticFrontClearanceCm"))<2.9999||Number(Out,TEXT("perspectiveNearClipCm"))<=0||FMath::Abs(Number(Out,TEXT("residualMotionFraction"))-.03)>.000001
        ||FMath::Abs(Number(Out,TEXT("residualZCm")))>.200001||FMath::Abs(Number(Out,TEXT("residualPitchDegrees")))>.250001
        ||FMath::Abs(Number(Out,TEXT("residualRollDegrees")))>.250001||FMath::Abs(Number(Out,TEXT("residualYawDegrees")))>.250001))return false;
    return true;
}
void AMAYANavigationNativeTest::Record(const FMAYADeliveryRobotObservation& O,bool Force)
{
    if(Samples.Num()>=256||(!Force&&FPlatformTime::Seconds()<NextRecord))return;NextRecord=FPlatformTime::Seconds()+.5;
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("step"),Step);J->SetNumberField(TEXT("pollGameFrame"),double(GFrameCounter));J->SetNumberField(TEXT("pollWorldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("pollMonotonicSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("x"),O.ChassisPose.GetLocation().X);J->SetNumberField(TEXT("y"),O.ChassisPose.GetLocation().Y);J->SetNumberField(TEXT("opticalYawDegrees"),OpticalYaw(O));J->SetNumberField(TEXT("supportMask"),O.SupportMask);J->SetBoolField(TEXT("quiet"),Quiet(O));J->SetNumberField(TEXT("speedCmps"),O.VelocityCmps.Size());J->SetBoolField(TEXT("known"),O.bKnown);J->SetBoolField(TEXT("ready"),O.bReady);J->SetStringField(TEXT("fault"),O.Fault);J->SetNumberField(TEXT("screen"),PC.IsValid()?int32(PC->GetPlayerScreen()):-1);
    if(HUD.IsValid()){auto Map=ParseObject(HUD->ReadMinimapEvidenceJson());if(Map){auto Summary=MakeShared<FJsonObject>();
        for(const TCHAR* Field:{TEXT("radarSampledCells"),TEXT("radarRoadCells"),TEXT("radarSidewalkCells"),TEXT("radarUnknownCells"),TEXT("radarStructureCells"),TEXT("radarStaleRetainedCells"),TEXT("radarActualTracesLastFrame"),TEXT("radarRefreshTracesLastFrame"),TEXT("radarWorldCentreX"),TEXT("radarWorldCentreY")})Summary->SetNumberField(Field,Number(Map,Field));
        J->SetObjectField(TEXT("minimapCountsAndCentre"),Summary);}}
    if(Director.IsValid()){J->SetNumberField(TEXT("batteryPercent"),Director->Snapshot().BatteryPercent);J->SetStringField(TEXT("profileId"),Director->Snapshot().Job.ProfileId);}
    if(bHUDStyleTest&&HUD.IsValid())
    {auto Draw=ParseObject(HUD->ReadDrivingHUDEvidenceJson());if(Draw)J->SetObjectField(TEXT("actualLastHUDDraw"),Draw);
        const auto& S=Director->Snapshot();J->SetNumberField(TEXT("actualPollPhase"),int32(S.Phase));J->SetStringField(TEXT("actualPollJobId"),S.JobInstanceId);
        J->SetNumberField(TEXT("actualPollSecuredCount"),Director->GetSecuredParcelCount());J->SetNumberField(TEXT("actualPollBatteryPercent"),S.BatteryPercent);
        J->SetNumberField(TEXT("actualPollConditionPercent"),S.CargoConditionPercent);J->SetBoolField(TEXT("actualPhysicalClosedLocks"),Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());}
    Samples.Add(MakeShared<FJsonValueObject>(J));
}
bool AMAYANavigationNativeTest::Capture(const FString& Name)
{
    if(!PendingCapture.IsEmpty())return false;
    PendingHUDStyleView=ParseObject(HUD->ReadDrivingHUDEvidenceJson());
    if(bHUDStyleTest || PC->GetPlayerScreen()==EMAYAPlayerScreen::Gameplay)
    {
        bool Submitted=false,Suppressed=true;
        const bool Good=PendingHUDStyleView && FiniteObject(PendingHUDStyleView)
            &&Word(PendingHUDStyleView,TEXT("theme"))==TEXT("AtlasNoir")
            &&Number(PendingHUDStyleView,TEXT("lastDrawGameFrame"))>=double(GFrameCounter)-2
            &&PendingHUDStyleView->TryGetBoolField(TEXT("telemetrySubmitted"),Submitted)&&Submitted
            &&PendingHUDStyleView->TryGetBoolField(TEXT("telemetrySuppressedByRobot"),Suppressed)&&!Suppressed;
        if(!Check(Good,TEXT("Fresh essential Atlas telemetry draw submission at gameplay capture: ")+Name))
        {Finish(false,TEXT("Actual essential telemetry draw readback missing/suppressed at capture"));return false;}
    }
    if(bHUDStyleTest)PendingHUDStyleCamera=ParseObject(Robot->ReadPlayerCameraModeJson());
    PendingCapture=FPaths::Combine(Output,Name+TEXT(".png"));CaptureSize=ViewportSize();CaptureRequestFrame=GFrameCounter;CaptureRequestWorld=GetWorld()->GetTimeSeconds();CaptureStartedReal=FPlatformTime::Seconds();CaptureProcessedFrame=0;bCaptureProcessed=false;
    FScreenshotRequest::RequestScreenshot(PendingCapture,true,false);return true;
}
void AMAYANavigationNativeTest::ScreenshotProcessed()
{if(!PendingCapture.IsEmpty()){bCaptureProcessed=true;CaptureProcessedFrame=GFrameCounter;CaptureProcessedWorld=GetWorld()->GetTimeSeconds();}}
bool AMAYANavigationNativeTest::PollCapture()
{
    if(PendingCapture.IsEmpty())return true;
    if(FPlatformTime::Seconds()-CaptureStartedReal>8){Finish(false,TEXT("Bounded actual screenshot did not complete"));return false;}
    if(!bCaptureProcessed||IFileManager::Get().FileSize(*PendingCapture)<=64)return false;
    TArray<uint8> Bytes;const bool Read=FFileHelper::LoadFileToArray(Bytes,*PendingCapture);const uint8 Signature[]={137,80,78,71,13,10,26,10};
    auto UInt=[&](int32 I){return(uint32(Bytes[I])<<24)|(uint32(Bytes[I+1])<<16)|(uint32(Bytes[I+2])<<8)|uint32(Bytes[I+3]);};
    if(!Check(Read&&Bytes.Num()>24&&FMemory::Memcmp(Bytes.GetData(),Signature,8)==0&&UInt(16)==uint32(CaptureSize.X)&&UInt(20)==uint32(CaptureSize.Y)&&CaptureProcessedFrame>=CaptureRequestFrame,TEXT("Processed native PNG: ")+FPaths::GetBaseFilename(PendingCapture)))
    {Finish(false,TEXT("Native PNG validation failed"));return false;}
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("path"),PendingCapture);J->SetNumberField(TEXT("requestFrame"),double(CaptureRequestFrame));J->SetNumberField(TEXT("processedFrame"),double(CaptureProcessedFrame));J->SetNumberField(TEXT("requestWorldSeconds"),CaptureRequestWorld);J->SetNumberField(TEXT("processedWorldSeconds"),CaptureProcessedWorld);J->SetNumberField(TEXT("width"),CaptureSize.X);J->SetNumberField(TEXT("height"),CaptureSize.Y);if(PendingHUDStyleView)J->SetObjectField(TEXT("actualHUDDrawReadbackAtRequest"),PendingHUDStyleView);if(bHUDStyleTest&&PendingHUDStyleView){if(PendingHUDStyleCamera)J->SetObjectField(TEXT("actualCameraReadbackAtRequest"),PendingHUDStyleCamera);J->SetStringField(TEXT("parcelActor"),HUDStyleParcel.IsValid()?HUDStyleParcel->GetPathName():TEXT(""));J->SetStringField(TEXT("parcelBody"),HUDStyleParcelBody.IsValid()?HUDStyleParcelBody->GetPathName():TEXT(""));J->SetStringField(TEXT("parcelId"),HUDStyleParcelId);PendingHUDStyleView.Reset();PendingHUDStyleCamera.Reset();}Captures.Add(MakeShared<FJsonValueObject>(J));PendingCapture.Reset();
    // A view-changing key follows the processed image, never the request in the same tick.
    if(CaptureNextKey.IsValid()){if(bCaptureHoldW)Key(EKeys::W,IE_Pressed);const FKey Next=CaptureNextKey;CaptureNextKey=FKey();bCaptureHoldW=false;Key(Next,IE_Pressed);EnteredFrame=GFrameCounter;}
    return true;
}
void AMAYANavigationNativeTest::Tick(float Dt)
{
    Super::Tick(Dt);if(bFinished)return;
    const double Real=FPlatformTime::Seconds(),Now=GetWorld()->GetTimeSeconds();
    if(Real-StartedReal>180||Now-StartedWorld>120){Finish(false,TEXT("Navigation 120 gameplay/180 real second bound"));return;}
    if(!Robot.IsValid()||!Director.IsValid()||!PC.IsValid()||!HUD.IsValid()){Finish(false,TEXT("Owned navigation identities disappeared"));return;}
    if(PendingRelease.IsValid()&&GFrameCounter>KeyFrame){const FKey Release=PendingRelease;PendingRelease=FKey();Key(Release,IE_Released);}
    FMAYADeliveryRobotObservation O;Robot->ReadDeliveryState(O);const auto& S=Director->Snapshot();
    if(bHUDStyleTest){TickHUDStyle(O);return;}
    if(!O.Fault.IsEmpty()||S.bDroneTotalled||S.bVehicleContactPending||S.Phase!=EMAYADeliveryPhase::Available||Director->GetParcelCount()!=0
        ||S.TotalCredits!=BaseCash||S.RechargeFeesPaidDollars!=BaseFees||S.CompletedJobs!=BaseCompleted||S.ShiftInstanceId!=ShiftId)
    {Record(O,true);Finish(false,TEXT("No job/service/resource mutation or actual native fault permitted"));return;}
    if(Step>0&&(!O.bKnown||!O.bReady||O.ResetGeneration!=ResetGeneration||O.ChassisPose.ContainsNaN()||O.VelocityCmps.ContainsNaN()
        ||O.AngularVelocityRadps.ContainsNaN()||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()||O.VelocityCmps.Size()>130))
    {Record(O,true);Finish(false,TEXT("Actual navigation native readiness/locks/reset/speed guard"));return;}
    if(Step>0)
    {
        const auto Projection=Path.Project(O.ChassisPose.GetLocation());
        if(!Projection.bValid||Projection.DistanceSquaredCm>150*150||FVector::Dist2D(O.ChassisPose.GetLocation(),InitialPosition)>1000)
        {Record(O,true);Finish(false,TEXT("Existing 150cm measured-route/1000cm total navigation footprint"));return;}
        MaximumTranslation=FMath::Max(MaximumTranslation,FVector::Dist2D(O.ChassisPose.GetLocation(),InitialPosition));
        if(PC->GetPlayerScreen()==EMAYAPlayerScreen::Gameplay&&!ObserveMinimap(O))
        {Record(O,true);Finish(false,TEXT("Actual minimap finite transform/cache/24-trace guard"));return;}
    }
    if(bMovingPovAdmitted&&Step>=2&&Step<=5)
    {
        TSharedPtr<FJsonObject> Camera;
        if(!ObserveCamera(true,Camera)){Record(O,true);Finish(false,TEXT("Moving POV actual manager/pawn pose,3% caps or front clearance failed"));return;}
        MaximumMovingResidualZ=FMath::Max(MaximumMovingResidualZ,FMath::Abs(Number(Camera,TEXT("residualZCm"))));
        for(const TCHAR* Field:{TEXT("residualPitchDegrees"),TEXT("residualRollDegrees"),TEXT("residualYawDegrees")})MaximumMovingResidualAngle=FMath::Max(MaximumMovingResidualAngle,FMath::Abs(Number(Camera,Field)));
        if(Real>=NextMovingCameraRecord&&MovingCameraSamples.Num()<64)
        {
            NextMovingCameraRecord=Real+.5;auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("pollGameFrame"),double(GFrameCounter));Row->SetNumberField(TEXT("pollWorldSeconds"),Now);Row->SetNumberField(TEXT("speedCmps"),O.VelocityCmps.Size());Row->SetNumberField(TEXT("nativeAngularSpeedRadps"),O.AngularVelocityRadps.Size());Row->SetObjectField(TEXT("actualCameraReadback"),Camera);MovingCameraSamples.Add(MakeShared<FJsonValueObject>(Row));
            TranslatingPovSamples+=O.VelocityCmps.Size()>5;TurningPovSamples+=O.AngularVelocityRadps.Size()>.05;
        }
    }
    Record(O);
    const bool Paused=UGameplayStatics::IsGamePaused(this);
    if(FrozenWorld>=0&&Paused&&(FMath::Abs(Now-FrozenWorld)>.000001||FMath::Abs(S.ElapsedSeconds-FrozenElapsed)>.000001
        ||FMath::Abs(S.BatteryPercent-FrozenBattery)>.000001||FMath::Abs(S.ChargeVisit.ElapsedSeconds-FrozenCharge)>.000001))
    {Finish(false,TEXT("Real map/pause world, delivery, energy or charging clock advanced"));return;}
    if(!Paused&&O.bReady)Drive();
    if(!PollCapture()||bFinished)return;
    if(GFrameCounter<=EnteredFrame+2)return;
    const double Limit=Step==0?20:Step==2?30:Step==4?10:Step==6?18:12;
    if((Step==0?Now-StepWorld:Real-StepReal)>Limit){Record(O,true);Finish(false,FString::Printf(TEXT("Bounded navigation step %d did not admit"),Step));return;}
    switch(Step)
    {
    case 0:
        if(!Quiet(O)){StableSince=-1;return;}if(StableSince<0)StableSince=Now;if(Now-StableSince<2)return;
        if(!Check(PC->HasStartedShift()&&PC->IsStartupMenuBypassedForAutomation()&&PC->GetPlayerScreen()==EMAYAPlayerScreen::Gameplay&&!Paused&&S.JobInstanceId.IsEmpty(),TEXT("Existing AutoTest shift startup remains Available with no accepted job")))
        {Finish(false,TEXT("Ordinary AutoTest baseline did not admit"));return;}
        InitialPosition=O.ChassisPose.GetLocation();ResetGeneration=O.ResetGeneration;StartArc=Path.Project(InitialPosition).ArcCm;GoalArc=StartArc+700;
        if(!Check(GoalArc+50<=Path.Arc.Last(),TEXT("700cm movement remains inside original measured route"))){Finish(false,TEXT("Measured course too short"));return;}
        Enter(1);break;
    case 1:
        if(!Quiet(O)||Real-StepReal<6||!bSawRoadIdentity||!bSawSidewalkIdentity)return;
        InitialMap=ParseObject(HUD->ReadMinimapEvidenceJson());InitialCacheCount=Number(InitialMap,TEXT("radarSampledCells"));
        if(!Check(InitialCacheCount>100&&bSawRoadIdentity&&bSawSidewalkIdentity,TEXT("Actual static road and sidewalk identities populate solid local map"))){Finish(false,TEXT("Initial map evidence incomplete"));return;}
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));
        Capture(TEXT("01-local-initial"));CaptureNextKey=EKeys::C;Enter(2);break;
    case 2:
    {
        if(!bMovingPovAdmitted){TSharedPtr<FJsonObject> Camera;if(!ObserveCamera(true,Camera))return;bMovingPovAdmitted=true;}
        const auto P=Path.Project(O.ChassisPose.GetLocation());const FVector Goal=Path.Sample(GoalArc);
        if(P.ArcCm>=GoalArc-15||FVector::Dist2D(Goal,O.ChassisPose.GetLocation())<=15){Drive();Enter(3);return;}
        const FVector Aim=Path.Sample(FMath::Min(GoalArc,P.ArcCm+100));const double Error=FMath::FindDeltaAngleDegrees(OpticalYaw(O),(Aim-O.ChassisPose.GetLocation()).Rotation().Yaw);
        if(!Drive(.5f,float(FMath::Clamp(Error/35.,-.65,.65)),false)){Finish(false,TEXT("Leased native route drive refused"));return;}break;
    }
    case 3:
        if(!Quiet(O)){StableSince=-1;return;}if(StableSince<0)StableSince=Now;if(Now-StableSince<.5)return;
        if(!Check(MaximumTranslation>=600,TEXT("Actual native translation exceeds old 5m minimap boundary"))){Finish(false,TEXT("Insufficient actual translation"));return;}
        TurnStartYaw=OpticalYaw(O);Enter(4);break;
    case 4:
        MeasuredTurn=FMath::FindDeltaAngleDegrees(TurnStartYaw,OpticalYaw(O));
        if(MeasuredTurn>=24){Drive();Enter(5);return;}
        if(MeasuredTurn< -5||MeasuredTurn>45||!Drive(0,1,false)){Finish(false,TEXT("Bounded real native raw-yaw turn refused"));return;}break;
    case 5:
        if(!Quiet(O)){StableSince=-1;return;}if(StableSince<0)StableSince=Now;if(Now-StableSince<.5)return;
        MeasuredTurn=FMath::FindDeltaAngleDegrees(TurnStartYaw,OpticalYaw(O));
        if(!Check(MeasuredTurn>=20&&MeasuredTurn<=45,TEXT("Actual stopped heading changes at least20deg; yaw translation is recorded"))){Finish(false,TEXT("Measured turn outside scope"));return;}
        if(!Check(TranslatingPovSamples>0&&TurningPovSamples>0,TEXT("Actual fresh POV manager/pawn view and3% bounded residual sampled during native translation and yaw"))){Finish(false,TEXT("Moving POV evidence incomplete"));return;}
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);Enter(6);break;
    case 6:
    {
        TSharedPtr<FJsonObject> RestoredCamera;if(!Quiet(O)||!bOverlapRefreshed||!ObserveCamera(false,RestoredCamera))return;
        MovedMap=ParseObject(HUD->ReadMinimapEvidenceJson());
        if(!Check(MinimumMovingCacheCount>=InitialCacheCount*.5&&bOverlapRefreshed,TEXT("Overlapping fixed world cell/mesh survives actual movement and later refresh; no wholesale reset"))){Finish(false,TEXT("Cache retention not observed"));return;}
        Capture(TEXT("02-local-moved-turned"));Enter(7);break;
    }
    case 7:
        if(!Quiet(O))return;if(!Check(Action(TEXT("ChooseOffer"),1),TEXT("Actual available-offer selection callback accepted"))){Finish(false,TEXT("Offer selection refused"));return;}
        Enter(8);break;
    case 8:
        if(!Quiet(O)||Real-StepReal<1)return;
        OfferMap=ParseObject(HUD->ReadMinimapEvidenceJson());
        if(!Check(S.Job.ProfileId!=InitialProfile&&Number(OfferMap,TEXT("radarSampledCells"))>=Number(MovedMap,TEXT("radarSampledCells"))*.8,TEXT("Offer switch retains player-centred north-up surroundings"))){Finish(false,TEXT("Offer/map continuity failed"));return;}
        Capture(TEXT("03-local-selected-offer"));CaptureNextKey=EKeys::M;bCaptureHoldW=true;Enter(9);break;
    case 9:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::CityMap||!Paused||!Director->IsPaused())return;
        if(FrozenWorld<0){FrozenWorld=Now;FrozenElapsed=S.ElapsedSeconds;FrozenBattery=S.BatteryPercent;FrozenCharge=S.ChargeVisit.ElapsedSeconds;StepReal=Real;}
        if(Real-StepReal<.6||!ObserveFullMap(TEXT("whole_city"),WholeMap))return;
        if(!Check(true,TEXT("M callback opens rendered whole-city map and freezes real world/service/energy clocks"))){return;}
        Capture(TEXT("04-map-whole-city-default"));Enter(10);break;
    case 10:
        if(!Check(Action(TEXT("MapFocus")),TEXT("Actual map delivery-focus action accepted"))){Finish(false,TEXT("Map focus refused"));return;}
        PC->ConsoleCommand(TEXT("r.SetRes 854x480w"),false);Enter(11);break;
    case 11:
        if(ViewportSize()!=FIntPoint(854,480)||!ObserveFullMap(TEXT("delivery"),FocusMap))return;
        if(!Check(Number(FocusMap,TEXT("worldMaxX"))-Number(FocusMap,TEXT("worldMinX"))<Number(WholeMap,TEXT("worldMaxX"))-Number(WholeMap,TEXT("worldMinX")),TEXT("Rendered delivery focus narrows actual city bounds; marker transform and authoritative chargers match"))){Finish(false,TEXT("Delivery-focus bounds did not narrow"));return;}
        Capture(TEXT("05-map-delivery-small"));Enter(12);break;
    case 12:
        if(!Action(TEXT("MapFocus"))){Finish(false,TEXT("Whole-city focus restoration refused"));return;}Enter(13);break;
    case 13:
    {
        TSharedPtr<FJsonObject> Map;if(!ObserveFullMap(TEXT("whole_city"),Map))return;
        Capture(TEXT("06-map-whole-city-small"));CaptureNextKey=EKeys::M;Enter(14);break;
    }
    case 14:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Gameplay||Paused||Director->IsPaused())return;
        FrozenWorld=-1;Key(EKeys::W,IE_Repeat);
        if(!Check(!PC->IsInputKeyDown(EKeys::W),TEXT("Held-through-map W remains suppressed after M closes to Gameplay"))){Finish(false,TEXT("Held drive key leaked through map"));return;}
        // UE5.3 folds equal pressed/released counts into the previous held state.
        Key(EKeys::W,IE_Released);Enter(24);break;
    case 24:
        if(!Check(!PC->IsInputKeyDown(EKeys::W),TEXT("Released W is processed in a separate input frame before repress"))){Finish(false,TEXT("W release frame did not process"));return;}
        Key(EKeys::W,IE_Pressed);Enter(15);break;
    case 15:
        if(!Check(PC->IsInputKeyDown(EKeys::W),TEXT("Real callback release/repress re-enables W; native brake lease protects this input check"))){Finish(false,TEXT("Drive-key release gate did not reopen"));return;}
        Key(EKeys::W,IE_Released);Key(EKeys::P,IE_Pressed);Enter(16);break;
    case 16:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Pause||!Paused||!Director->IsPaused())return;
        FrozenWorld=Now;FrozenElapsed=S.ElapsedSeconds;FrozenBattery=S.BatteryPercent;FrozenCharge=S.ChargeVisit.ElapsedSeconds;
        if(!Check(Action(TEXT("Map")),TEXT("Actual Pause menu Map action accepted; M-from-Pause is not claimed"))){Finish(false,TEXT("Pause Map action refused"));return;}Enter(17);break;
    case 17:
    {
        TSharedPtr<FJsonObject> Map;if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::CityMap||!ObserveFullMap(TEXT("whole_city"),Map)||Real-StepReal<.6)return;
        Capture(TEXT("07-map-from-pause-small"));CaptureNextKey=EKeys::M;Enter(18);break;
    }
    case 18:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Pause||!Paused||!Director->IsPaused())return;
        Check(true,TEXT("M closes nested map to its actual Pause caller without unpausing"));Key(EKeys::P,IE_Pressed);Enter(19);break;
    case 19:
    {
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Gameplay||Paused||Director->IsPaused())return;FrozenWorld=-1;
        auto Camera=ParseObject(Robot->ReadPlayerCameraModeJson());LastViewSamples=Number(Camera,TEXT("viewSamples"));InitialOrbitYaw=Number(Camera,TEXT("preservedChaseYawOffsetDegrees"));InitialOrbitPitch=Number(Camera,TEXT("preservedChasePitchDegrees"));
        if(!Check(!Robot->IsPlayerCameraPov(),TEXT("Initial actual camera is Chase"))){Finish(false,TEXT("Unexpected initial camera selection"));return;}
        Key(EKeys::C,IE_Pressed);Enter(20);break;
    }
    case 20:
        if(!Quiet(O)||!ObserveCamera(true,PovCamera))return;
        Check(true,TEXT("C selects actual fresh POV pawn/manager cache with finite FOV/near plane and3% bounded residual"));Capture(TEXT("08-pov-small"));Enter(21);break;
    case 21:
        LastViewSamples=Number(PovCamera,TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);Enter(22);break;
    case 22:
        if(!Quiet(O)||!ObserveCamera(false,ChaseCamera))return;
        if(!Check(FMath::Abs(Number(ChaseCamera,TEXT("preservedChaseYawOffsetDegrees"))-InitialOrbitYaw)<.000001&&FMath::Abs(Number(ChaseCamera,TEXT("preservedChasePitchDegrees"))-InitialOrbitPitch)<.000001,TEXT("C restores fresh Chase view and preserved orbit"))){Finish(false,TEXT("Chase orbit changed"));return;}
        Capture(TEXT("09-chase-small"));Enter(23);break;
    case 23:
        if(!Quiet(O))return;
        if(!Check(Captures.Num()==9&&Actions.Num()<64&&Samples.Num()<256&&bSawRoadIdentity&&bSawSidewalkIdentity&&bOverlapRefreshed,TEXT("Bounded nine processed views plus real local-map movement/turn/offer and M/C callbacks complete"))){Finish(false,TEXT("Scoped navigation evidence incomplete"));return;}
        Finish(true,TEXT("Scoped native navigation callbacks/readbacks/captures complete; independent pixels and human input remain separate"));break;
    default:Finish(false,TEXT("Unknown bounded navigation state"));break;
    }
}
bool AMAYANavigationNativeTest::ObserveHUDStyle(EMAYADeliveryPhase Expected,TSharedPtr<FJsonObject>& Out) const
{
    Out=ParseObject(HUD->ReadDrivingHUDEvidenceJson());const auto& S=Director->Snapshot();
    bool Submitted=false,Suppressed=true;
    if(!Out||!FiniteObject(Out)||Word(Out,TEXT("schema"))!=TEXT("MayaRobotics.DrivingHUD.ConceptC.v1")
        ||Word(Out,TEXT("theme"))!=TEXT("AtlasNoir")
        ||!Out->TryGetBoolField(TEXT("telemetrySubmitted"),Submitted)||!Submitted
        ||!Out->TryGetBoolField(TEXT("telemetrySuppressedByRobot"),Suppressed)||Suppressed
        ||!Flag(Out,TEXT("speedValueFits"))||Word(Out,TEXT("speedTextDrawMode"))!=TEXT("full_numeric_no_ellipsis")
        ||Number(Out,TEXT("lastDrawGameFrame"))<=double(EnteredFrame)||Number(Out,TEXT("lastDrawGameFrame"))<double(GFrameCounter)-2
        ||int32(Number(Out,TEXT("phase")))!=int32(Expected)||S.Phase!=Expected||Word(Out,TEXT("jobInstanceId"))!=S.JobInstanceId
        ||Number(Out,TEXT("cashDollars"))!=S.TotalCredits||Number(Out,TEXT("securedParcelCount"))!=Director->GetSecuredParcelCount()
        ||FMath::Abs(Number(Out,TEXT("batteryPercent"))-S.BatteryPercent)>.1||Number(Out,TEXT("resetGeneration"))!=double(ResetGeneration))return false;
    if(Expected==EMAYADeliveryPhase::Carrying&&(!Flag(Out,TEXT("cargoConditionKnown"))||FMath::Abs(Number(Out,TEXT("cargoConditionPercent"))-S.CargoConditionPercent)>.0001))return false;
    if(Expected!=EMAYADeliveryPhase::Available&&(!Flag(Out,TEXT("deadlineKnown"))||FMath::Abs(Number(Out,TEXT("deadlineRemainingSeconds"))-FMath::Max(0.,S.Job.DeadlineSeconds-S.ElapsedSeconds))>.5))return false;
    return Number(Out,TEXT("width"))==ViewportSize().X&&Number(Out,TEXT("height"))==ViewportSize().Y;
}
void AMAYANavigationNativeTest::TickHUDStyle(const FMAYADeliveryRobotObservation& O)
{
    const auto& S=Director->Snapshot();const double Now=GetWorld()->GetTimeSeconds(),Real=FPlatformTime::Seconds();
    const bool Paused=UGameplayStatics::IsGamePaused(this);
    if(!O.Fault.IsEmpty()||S.bDroneTotalled||S.bVehicleContactPending||S.Phase==EMAYADeliveryPhase::Failed||S.Phase==EMAYADeliveryPhase::Succeeded
        ||S.TotalCredits!=BaseCash||S.RechargeFeesPaidDollars!=BaseFees||S.CompletedJobs!=BaseCompleted||S.ShiftInstanceId!=ShiftId)
    {Record(O,true);Finish(false,TEXT("HUD-style actual native fault or unrelated economy/terminal change"));return;}
    if(Step>50&&(!O.bKnown||!O.bReady||O.ResetGeneration!=ResetGeneration||O.ChassisPose.ContainsNaN()||O.VelocityCmps.ContainsNaN()
        ||O.AngularVelocityRadps.ContainsNaN()||O.VelocityCmps.Size()>130||FVector::Dist2D(O.ChassisPose.GetLocation(),InitialPosition)>100))
    {Record(O,true);Finish(false,TEXT("HUD-style actual stationary native readiness/reset/100cm scope"));return;}
    if(!HUDStyleJobId.IsEmpty()&&S.JobInstanceId!=HUDStyleJobId){Finish(false,TEXT("HUD-style actual accepted job identity changed"));return;}
    if(!HUDStyleParcelId.IsEmpty()&&(!HUDStyleParcel.IsValid()||!HUDStyleParcelBody.IsValid()||S.Phase!=EMAYADeliveryPhase::Carrying||Director->GetParcelCount()!=1||Director->GetSecuredParcelCount()!=1
        ||Director->GetParcel()!=HUDStyleParcel.Get()||HUDStyleParcel->GetParcelBody()!=HUDStyleParcelBody.Get()||HUDStyleParcel->GetParcelId()!=HUDStyleParcelId
        ||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()||S.CargoConditionPercent!=HUDStyleCondition))
    {Record(O,true);Finish(false,TEXT("HUD-style actual closed custody/parcel/body/condition changed"));return;}
    if(FrozenWorld>=0&&Paused&&(FMath::Abs(Now-FrozenWorld)>.000001||FMath::Abs(S.ElapsedSeconds-FrozenElapsed)>.000001
        ||FMath::Abs(S.BatteryPercent-FrozenBattery)>.000001||FMath::Abs(S.ChargeVisit.ElapsedSeconds-FrozenCharge)>.000001))
    {Finish(false,TEXT("HUD-style actual pause clocks advanced"));return;}
    if(!Paused&&O.bReady)Drive();Record(O);
    if(!PollCapture()||bFinished||GFrameCounter<=EnteredFrame+2)return;
    if((Step==50?Now-StepWorld:Real-StepReal)>(Step==50?20:12)){Record(O,true);Finish(false,FString::Printf(TEXT("HUD-style bounded step %d did not admit"),Step));return;}
    TSharedPtr<FJsonObject> View,Camera;
    switch(Step)
    {
    case 50:
        if(ViewportSize()!=FIntPoint(2560,1440)||!Quiet(O)){StableSince=-1;return;}
        if(StableSince<0)StableSince=Now;if(Now-StableSince<2)return;
        ResetGeneration=O.ResetGeneration;InitialPosition=O.ChassisPose.GetLocation();
        if(!ObserveHUDStyle(EMAYADeliveryPhase::Available,View))return;
        if(!Check(PC->HasStartedShift()&&PC->IsStartupMenuBypassedForAutomation()&&S.JobInstanceId.IsEmpty()&&Director->GetParcelCount()==0&&S.Job.ParcelCount==1
            &&PC->GetPlayerScreen()==EMAYAPlayerScreen::Gameplay&&!Paused,TEXT("HUD-style actual unmodified one-parcel depot offer baseline"))){Finish(false,TEXT("HUD-style baseline refused"));return;}
        Capture(TEXT("01-offer-default-2560"));Enter(51);break;
    case 51:
        if(!Quiet(O)||!Director->GetPlayerPresentation().bCanAccept)return;
        Key(EKeys::J,IE_Pressed);Enter(52);break;
    case 52:
        if(!Quiet(O)||!ObserveHUDStyle(EMAYADeliveryPhase::Accepted,View))return;
        HUDStyleJobId=S.JobInstanceId;Check(!HUDStyleJobId.IsEmpty(),TEXT("Actual J callback accepted the existing job; no fixture injection"));
        Capture(TEXT("02-accepted-default-2560"));Enter(53);break;
    case 53:
        if(!Quiet(O)||!Director->GetPlayerPresentation().bCanInteract)return;
        Key(EKeys::E,IE_Pressed);Enter(54);break;
    case 54:
        // Normal service moves doors/parcels; stationary latch is not a Loading gate.
        if(!ObserveHUDStyle(EMAYADeliveryPhase::Loading,View))return;
        Check(!Word(View,TEXT("serviceStage")).IsEmpty(),TEXT("Actual E callback began normal Loading; current service-stage draw snapshot captured"));
        Capture(TEXT("03-loading-default-2560"));Enter(55);break;
    case 55:
        PC->ConsoleCommand(TEXT("r.SetRes 854x480w"),false);Enter(56);break;
    case 56:
        if(ViewportSize()!=FIntPoint(854,480)||Real-StepReal<.35||!ObserveHUDStyle(EMAYADeliveryPhase::Loading,View))return;
        Capture(TEXT("04-loading-small-854"));Enter(57);break;
    case 57:
        PC->ConsoleCommand(TEXT("r.SetRes 2560x1440w"),false);Enter(58);break;
    case 58:
        if(ViewportSize()!=FIntPoint(2560,1440)||S.Phase!=EMAYADeliveryPhase::Carrying||!Quiet(O)){StableSince=-1;return;}
        if(StableSince<0)StableSince=Now;if(Now-StableSince<1||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        HUDStyleParcel=Director->GetParcel();HUDStyleParcelBody=HUDStyleParcel.IsValid()?HUDStyleParcel->GetParcelBody():nullptr;
        HUDStyleParcelId=HUDStyleParcel.IsValid()?HUDStyleParcel->GetParcelId():FString();HUDStyleCondition=S.CargoConditionPercent;HUDStyleCarryWorld=Now;
        if(!Check(Director->GetParcelCount()==1&&Director->GetSecuredParcelCount()==1&&HUDStyleParcel.IsValid()&&HUDStyleParcelBody.IsValid()&&!HUDStyleParcelId.IsEmpty()
            &&Flag(View,TEXT("cargoConditionKnown"))&&Number(View,TEXT("hintOpacity"))==1&&!Robot->IsPlayerCameraPov(),TEXT("Actual closed one-parcel Carrying HUD and initially present hints; Chase default"))){Finish(false,TEXT("HUD-style normal closed pickup did not admit"));return;}
        Capture(TEXT("05-carrying-chase-default-2560"));Enter(59);break;
    case 59:
        if(!Quiet(O)||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        if(Number(View,TEXT("hintOpacity"))>.55)return;
        if(!Check(Number(View,TEXT("hintOpacity"))>=.40,TEXT("Actual hints fade near6.25 gameplay seconds without fading text/telemetry"))){Finish(false,TEXT("Actual hint fade window missed"));return;}
        Capture(TEXT("06-hints-fading-default"));Enter(60);break;
    case 60:
        if(!Quiet(O)||Now-HUDStyleCarryWorld<8||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        if(!Check(Number(View,TEXT("hintOpacity"))==0,TEXT("Actual temporary hints absent after8 gameplay seconds; telemetry remains"))){Finish(false,TEXT("Actual hints did not expire"));return;}
        Capture(TEXT("07-hints-absent-default"));Enter(61);break;
    case 61:
        Key(EKeys::P,IE_Pressed);Enter(62);break;
    case 62:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Pause||!Paused||!Director->IsPaused())return;
        FrozenWorld=Now;FrozenElapsed=S.ElapsedSeconds;FrozenBattery=S.BatteryPercent;FrozenCharge=S.ChargeVisit.ElapsedSeconds;
        Key(EKeys::P,IE_Pressed);Enter(63);break;
    case 63:
        if(PC->GetPlayerScreen()!=EMAYAPlayerScreen::Gameplay||Paused||Director->IsPaused()||!Quiet(O)||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        FrozenWorld=-1;if(!Check(Number(View,TEXT("hintOpacity"))==1,TEXT("Actual P pause/resume restores temporary hints and preserves loaded job/custody"))){Finish(false,TEXT("Actual resumed hint presentation failed"));return;}
        Capture(TEXT("08-hints-resumed-default"));Enter(64);break;
    case 64:
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);Enter(65);break;
    case 65:
        if(!Quiet(O)||!ObserveCamera(true,Camera)||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        Check(true,TEXT("Actual loaded POV default camera/HUD readback matches fresh manager"));Capture(TEXT("09-carrying-pov-default-2560"));Enter(66);break;
    case 66:
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);PC->ConsoleCommand(TEXT("r.SetRes 854x480w"),false);Enter(67);break;
    case 67:
        if(ViewportSize()!=FIntPoint(854,480)||Real-StepReal<.35||!Quiet(O)||!ObserveCamera(false,Camera)||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        Check(Number(View,TEXT("visibleMinimapDiameterPixels"))>=127.9,TEXT("Actual small loaded HUD retains128px solid minimap and same live values"));
        Capture(TEXT("10-carrying-chase-small-854"));Enter(68);break;
    case 68:
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);Enter(69);break;
    case 69:
        if(!Quiet(O)||!ObserveCamera(true,Camera)||!ObserveHUDStyle(EMAYADeliveryPhase::Carrying,View))return;
        Check(true,TEXT("Actual loaded POV small camera/HUD readback matches fresh manager"));Capture(TEXT("11-carrying-pov-small-854"));Enter(70);break;
    case 70:
        LastViewSamples=Number(ParseObject(Robot->ReadPlayerCameraModeJson()),TEXT("viewSamples"));Key(EKeys::C,IE_Pressed);Enter(71);break;
    case 71:
        if(!Quiet(O)||!ObserveCamera(false,Camera))return;
        if(!Check(Captures.Num()==11&&Actions.Num()<64&&Samples.Num()<256&&HUDStyleParcel.IsValid(),TEXT("Bounded actual offer/pickup/service and four loaded Chase/POV sizes plus hint lifetime captures complete"))){Finish(false,TEXT("HUD-style scoped native evidence incomplete"));return;}
        Finish(true,TEXT("Actual Concept C HUD pickup/readbacks/captures complete; independent pixels remain separate"));break;
    default:Finish(false,TEXT("Unknown bounded HUD-style step"));break;
    }
}
void AMAYANavigationNativeTest::Finish(bool Success,const FString& Reason)
{
    if(bFinished)return;for(const auto& V:Checks)if(!Flag(V->AsObject(),TEXT("passed")))Success=false;
    bFinished=true;for(const FKey& Held:HeldKeys)if(PC.IsValid())PC->InputKey(FInputKeyParams(Held,IE_Released,0.0));HeldKeys.Reset();Drive();SetActorTickEnabled(false);
    if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("MayaRobotics.NavigationNativeTest.v1"));J->SetStringField(TEXT("phase"),TEXT("NativeNavigationTest"));J->SetBoolField(TEXT("terminal"),true);J->SetBoolField(TEXT("success"),Success);J->SetStringField(TEXT("reason"),Reason);J->SetBoolField(TEXT("humanPhysicalInputVerified"),false);J->SetStringField(TEXT("scope"),TEXT("Actual native leased motion and controller M/C press/release/menu callbacks, including POV samples while translating/turning. Camera fraction3% is the declared filter policy; capped sampled residuals are not same-solver-frame attenuation or universal comfort proof. Rendered readbacks and processed PNGs require independent pixel review; no OS keyboard, fullmission or route driveability claim. r.SetRes only affects this diagnostic viewport, no settings Apply/Save or resources writes."));
    if(bHUDStyleTest){J->SetStringField(TEXT("testVariant"),TEXT("ConceptC_HUDStyle"));J->SetStringField(TEXT("scope"),TEXT("Actual J/E/P/C controller callbacks and normal one-parcel pickup only; stationary Loading then physically closed Carrying, default2560x1440/small854x480 Chase/POV and real hint lifetime. No job result/economy fixture, pose/physics/resource write or fulldelivery. r.SetRes is transient diagnostic presentation only; no settings Apply/Save. DrawHUD receipts are actual last draw at request, processed PNG pixels require independent review; no OS keyboard or moving POV claim for this variant."));J->SetStringField(TEXT("actualParcelId"),HUDStyleParcelId);J->SetStringField(TEXT("actualAcceptedJobId"),HUDStyleJobId);}
    J->SetStringField(TEXT("actualSettingsPath"),ActualSettingsPath);J->SetNumberField(TEXT("maximumTranslationCm"),MaximumTranslation);J->SetNumberField(TEXT("stoppedHeadingChangeDegrees"),MeasuredTurn);J->SetNumberField(TEXT("gameplayElapsedSeconds"),GetWorld()->GetTimeSeconds()-StartedWorld);J->SetNumberField(TEXT("realElapsedSeconds"),FPlatformTime::Seconds()-StartedReal);J->SetNumberField(TEXT("gameplayCapSeconds"),120);J->SetNumberField(TEXT("realCapSeconds"),180);J->SetNumberField(TEXT("driveCalls"),DriveCalls);J->SetNumberField(TEXT("initialRetainedCacheCount"),InitialCacheCount);J->SetNumberField(TEXT("minimumMovingCacheCount"),MinimumMovingCacheCount==DBL_MAX?0:MinimumMovingCacheCount);J->SetNumberField(TEXT("unknownReceiptReadObservations"),UnknownObservations);J->SetNumberField(TEXT("refreshReceiptReadObservations"),RefreshObservations);J->SetBoolField(TEXT("sameWorldCellMeshRefreshAfterMovementObserved"),bOverlapRefreshed);
    J->SetArrayField(TEXT("checks"),Checks);J->SetArrayField(TEXT("samples"),Samples);J->SetArrayField(TEXT("actualProcessedCaptures"),Captures);J->SetArrayField(TEXT("controllerCallbacks"),Actions);J->SetArrayField(TEXT("reobservedWorldCellIdentity"),CacheIdentities);
    J->SetArrayField(TEXT("movingCameraSamples"),MovingCameraSamples);J->SetNumberField(TEXT("translatingPovSamples"),TranslatingPovSamples);J->SetNumberField(TEXT("turningPovSamples"),TurningPovSamples);J->SetNumberField(TEXT("maximumMovingResidualZCm"),MaximumMovingResidualZ);J->SetNumberField(TEXT("maximumMovingResidualAngleDegrees"),MaximumMovingResidualAngle);
    TArray<TSharedPtr<FJsonValue>> BeforeCells;for(const auto& Pair:InitialCells)BeforeCells.Add(MakeShared<FJsonValueObject>(Pair.Value));J->SetArrayField(TEXT("initialWorldCellIdentities"),BeforeCells);
    J->SetNumberField(TEXT("cameraPosePositionToleranceCm"),.2);J->SetNumberField(TEXT("cameraPoseAngularToleranceDegrees"),.1);J->SetNumberField(TEXT("cameraFovToleranceDegrees"),.05);J->SetNumberField(TEXT("povFrontClearanceMinimumCm"),3);J->SetNumberField(TEXT("frontClearanceRoundoffToleranceCm"),.0001);
    auto Store=[&](const TCHAR* Name,const TSharedPtr<FJsonObject>& Value){if(Value)J->SetObjectField(Name,Value);};
    Store(TEXT("initialMinimap"),InitialMap);Store(TEXT("movedMinimap"),MovedMap);Store(TEXT("selectedOfferMinimap"),OfferMap);Store(TEXT("wholeCityMap"),WholeMap);Store(TEXT("deliveryFocusedMap"),FocusMap);Store(TEXT("actualPovCamera"),PovCamera);Store(TEXT("actualChaseCamera"),ChaseCamera);
    if(Robot.IsValid())J->SetStringField(TEXT("finalActualNativeTelemetry"),Robot->ReadTelemetryJson());
    FString Json;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&Json));FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Output,TEXT("result.json")));UE_LOG(LogTemp,Display,TEXT("MAYA NAVIGATION %s: %s"),Success?TEXT("PASS"):TEXT("FAIL"),*Reason);FPlatformMisc::RequestExit(false,TEXT("MAYANavigationNativeTest"));
}
void AMAYANavigationNativeTest::EndPlay(EEndPlayReason::Type Reason)
{if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);if(Robot.IsValid())Robot->ReleaseDriveCommand();Super::EndPlay(Reason);}
