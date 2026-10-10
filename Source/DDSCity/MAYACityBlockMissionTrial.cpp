#include "MAYACityBlockMissionTrial.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryParcel.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryGameMode.h"
#include "Vehicles/CitySampleVehicleBase.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

namespace MayaBlockTrial
{
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& P)
{ return {MakeShared<FJsonValueNumber>(P.X),MakeShared<FJsonValueNumber>(P.Y),MakeShared<FJsonValueNumber>(P.Z)}; }
}

AMAYACityBlockMissionTrial::AMAYACityBlockMissionTrial()
{ PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PostUpdateWork; }
void AMAYACityBlockMissionTrial::Configure(AMAYANativeDemoPawn* R,AMAYADeliveryDirector* D,const FMAYACityBlockRouteDefinition& Content)
{ Robot=R;Director=D;Definition=Content; }
bool AMAYACityBlockMissionTrial::Check(bool Passed,const FString& Name)
{ auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),Name);J->SetBoolField(TEXT("passed"),Passed);Checks.Add(MakeShared<FJsonValueObject>(J));return Passed; }
bool AMAYACityBlockMissionTrial::Drive(float T,float S,bool Brake)
{
    ++DriveCalls;LastRequestedThrottle=T;LastRequestedSteer=S;LastRequestedBrake=Brake;
    LastCommandWorldSeconds=GetWorld()->GetTimeSeconds();LastCommandFrame=GFrameCounter;
    LastCommandSteeringMode=Brake?TEXT("braked"):TEXT("authored_anchor_feedback_turn");
    bLastCommandAccepted=Robot.IsValid()&&Robot->SubmitDriveCommand(T,S,Brake,EMAYADriveCommandOrigin::NativeAutomation,.25f,false);return bLastCommandAccepted;
}
bool AMAYACityBlockMissionTrial::Quiet(const FMAYADeliveryRobotObservation& O) const
{ return O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&Robot.IsValid()&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked(); }
bool AMAYACityBlockMissionTrial::AlignmentRest(const FMAYADeliveryRobotObservation& O) const
{
    // Driving-turn rest is distinct from all-six authoritative service parking.
    // Support bits remain actual current accepted native contacts; no stale hit is revived.
    const uint8 Mask=O.SupportMask&0x3f;int32 Count=0;
    for(int32 W=0;W<6;++W)if(Mask&(1<<W))++Count;
    return O.bKnown&&O.bReady&&O.bUpright&&O.Fault.IsEmpty()
        &&!O.ChassisPose.ContainsNaN()&&!O.VelocityCmps.ContainsNaN()&&!O.AngularVelocityRadps.ContainsNaN()
        &&O.VelocityCmps.Size()<=5.0&&O.AngularVelocityRadps.Size()<=.05
        &&FMath::IsFinite(O.ObservationWorldSeconds)&&O.ObservationWorldSeconds>=0
        &&Count>=5&&(Mask&0x07)!=0&&(Mask&0x38)!=0
        &&O.bDoorsClosedLatched&&Robot.IsValid()&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();
}
bool AMAYACityBlockMissionTrial::AdvanceAuthoredStop(const FMAYADeliveryRobotObservation& O,double Now)
{
    if(!Definition.StopTurns.IsValidIndex(NextAuthoredStop))return false;
    const auto& Stop=Definition.StopTurns[NextAuthoredStop];
    const FVector Anchor=Definition.Points[Stop.PointIndex];
    const FVector Outgoing=Definition.Points[Stop.OutgoingEndpointPointIndex]-Anchor;
    LastAuthoredStopDistanceCm=FVector::Dist2D(O.ChassisPose.GetLocation(),Anchor);
    const bool bFinalBCheckpoint=NextAuthoredStop==5&&Definition.StopTurns.Num()==6;
    const bool bACrestCheckpoint=NextAuthoredStop==2&&Definition.StopTurns.Num()==6
        &&Definition.PointLaneIndices[Stop.PointIndex]==2836
        &&FVector::Dist2D(Anchor,FVector(-24250,-80.72,73))<=.01
        &&Stop.SurfaceAuditSha256==Definition.ApproachSurfaceAuditSha256;
    const bool bCaptureUprightRequired=bFinalBCheckpoint||bACrestCheckpoint;
    const FVector Incoming=(Anchor-Definition.Points[Stop.PointIndex-1]).GetSafeNormal2D();
    const double IncomingAlongCm=FVector::DotProduct(O.ChassisPose.GetLocation()-Anchor,Incoming);
    const double BrakeCaptureRadiusCm=bCaptureUprightRequired?50.:FMath::Max(.5*Stop.ArrivalRadiusCm,Stop.ArrivalRadiusCm-5.);
    if(FollowerMotion==EFollowerMotion::Forward)
    {
        // Route-authored checkpoint, not an angle-triggered turn on a grade.
        // Both crest maneuvers require CURRENT parking-upright for50cm capture.
        // Only final B also waits for its plane;60cm is not a Quiet/flat certificate.
        if(LastAuthoredStopDistanceCm>BrakeCaptureRadiusCm
            ||(bCaptureUprightRequired&&!O.bUpright)||(bFinalBCheckpoint&&IncomingAlongCm<0))return false;
        LastAuthoredBrakeCapture=MakeShared<FJsonObject>();
        LastAuthoredBrakeCapture->SetNumberField(TEXT("worldSeconds"),Now);LastAuthoredBrakeCapture->SetNumberField(TEXT("frame"),double(GFrameCounter));
        LastAuthoredBrakeCapture->SetArrayField(TEXT("actualChassisCm"),MayaBlockTrial::Vec(O.ChassisPose.GetLocation()));
        LastAuthoredBrakeCapture->SetNumberField(TEXT("actualDistanceCm"),LastAuthoredStopDistanceCm);
        LastAuthoredBrakeCapture->SetNumberField(TEXT("brakeCaptureRadiusCm"),BrakeCaptureRadiusCm);
        LastAuthoredBrakeCapture->SetNumberField(TEXT("actualIncomingAlongCm"),IncomingAlongCm);
        LastAuthoredBrakeCapture->SetBoolField(TEXT("incomingPlaneAndUprightRequired"),bFinalBCheckpoint);
        LastAuthoredBrakeCapture->SetBoolField(TEXT("currentUprightRequired"),bCaptureUprightRequired);
        LastAuthoredBrakeCapture->SetBoolField(TEXT("actualUpright"),O.bUpright);LastAuthoredBrakeCapture->SetNumberField(TEXT("actualSupportMask"),O.SupportMask);
        LastAuthoredBrakeCapture->SetNumberField(TEXT("declaredQuietArrivalRadiusCm"),Stop.ArrivalRadiusCm);
        LastAuthoredBrakeCapture->SetBoolField(TEXT("actualQuietAtCapture"),Quiet(O));
        AuthoredStopStarted=Now;AuthoredStopStableSince=-1;
        MaximumAuthoredStopExcursionCm=LastAuthoredStopDistanceCm;
        AuthoredStopOutgoingYawDegrees=Outgoing.Rotation().Yaw;
        FollowerMotion=EFollowerMotion::BrakeForTurn;++FollowerAlignmentEpisodes;
        Record(O,true,TEXT("authored_stage_brake_started"));
    }
    MaximumAuthoredStopExcursionCm=FMath::Max(MaximumAuthoredStopExcursionCm,LastAuthoredStopDistanceCm);
    if(Now-AuthoredStopStarted>15||LastAuthoredStopDistanceCm>Stop.MaximumCentreExcursionCm)
    {Drive();Record(O,true);Finish(false,TEXT("Authored native stop-turn exceeded its15-second or declared actual centre-excursion bound"));return true;}
    const FVector Forward=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
    const FVector ActualOutgoing=Definition.Points[Stop.OutgoingEndpointPointIndex]-O.ChassisPose.GetLocation();
    if(!O.bUpright||Forward.ContainsNaN()||Forward.SizeSquared2D()<=UE_SMALL_NUMBER
        ||ActualOutgoing.ContainsNaN()||ActualOutgoing.SizeSquared2D()<=UE_SMALL_NUMBER)
    {Drive();Record(O,true);Finish(false,TEXT("Authored-stage actual upright/finite outgoing geometry failed"));return true;}
    LastFollowerDesiredYaw=ActualOutgoing.Rotation().Yaw;
    LastFollowerHeadingError=FMath::FindDeltaAngleDegrees(Forward.Rotation().Yaw,LastFollowerDesiredYaw);
    LastFollowerPolicyWorldSeconds=Now;LastFollowerAim=Definition.Points[Stop.OutgoingEndpointPointIndex];
    if(FollowerMotion==EFollowerMotion::BrakeForTurn)
    {
        Drive();
        if(!Quiet(O)){AuthoredStopStableSince=-1;return true;}
        if(LastAuthoredStopDistanceCm>Stop.ArrivalRadiusCm)
        {Record(O,true);Finish(false,TEXT("Actual authored-stage braked arrival exceeded its declared intermediate checkpoint radius"));return true;}
        if(AuthoredStopStableSince<0)AuthoredStopStableSince=Now;
        if(Now-AuthoredStopStableSince<.5)return true;
        AuthoredStopTurnAnchorCm=O.ChassisPose.GetLocation();
        FollowerMotion=EFollowerMotion::Turn;AuthoredStopStableSince=-1;
        Record(O,true,TEXT("authored_stage_actual_stopped_turn_start"));return true;
    }
    if(FollowerMotion==EFollowerMotion::Turn)
    {
        AuthoredStopStableSince=-1;
        if(FMath::Abs(LastFollowerHeadingError)<=3)
        {Drive();FollowerMotion=EFollowerMotion::BrakeAfterTurn;Record(O,true,TEXT("authored_stage_turn_brake_started"));return true;}
        // Exact existing FaceFirst motor primitive; native yaw may translate.
        // Anchor is the explicit authored point; the actual stopped arrival is also retained.
        const float Correction=float(FMath::Clamp(.02*FVector::DotProduct(Anchor-O.ChassisPose.GetLocation(),Forward)
            -.015*O.OpticalForwardCmps,-.35,.35));
        if(!Drive(Correction,LastFollowerHeadingError>0?1.f:-1.f,false))
        {Record(O,true);Finish(false,TEXT("Actual authored-stage raw yaw command refused"));}
        return true;
    }
    Drive();
    if(!Quiet(O)){AuthoredStopStableSince=-1;return true;}
    if(FMath::Abs(LastFollowerHeadingError)>3)
    {FollowerMotion=EFollowerMotion::Turn;AuthoredStopStableSince=-1;return true;}
    if(AuthoredStopStableSince<0)AuthoredStopStableSince=Now;
    if(Now-AuthoredStopStableSince<.5)return true;
    auto Receipt=MakeShared<FJsonObject>();
    Receipt->SetNumberField(TEXT("stageIndex"),NextAuthoredStop);Receipt->SetNumberField(TEXT("pointIndex"),Stop.PointIndex);
    Receipt->SetNumberField(TEXT("outgoingEndpointPointIndex"),Stop.OutgoingEndpointPointIndex);
    Receipt->SetStringField(TEXT("surfaceAuditSha256"),Stop.SurfaceAuditSha256);
    if(LastAuthoredBrakeCapture.IsValid())Receipt->SetObjectField(TEXT("actualBrakeCapture"),LastAuthoredBrakeCapture);
    Receipt->SetNumberField(TEXT("declaredQuietArrivalRadiusCm"),Stop.ArrivalRadiusCm);
    Receipt->SetNumberField(TEXT("actualStoppedArrivalDistanceCm"),FVector::Dist2D(AuthoredStopTurnAnchorCm,Anchor));
    Receipt->SetArrayField(TEXT("authoredAnchorCm"),MayaBlockTrial::Vec(Anchor));Receipt->SetArrayField(TEXT("actualStoppedTurnAnchorCm"),MayaBlockTrial::Vec(AuthoredStopTurnAnchorCm));
    Receipt->SetArrayField(TEXT("actualStoppedEndpointCm"),MayaBlockTrial::Vec(O.ChassisPose.GetLocation()));
    Receipt->SetNumberField(TEXT("startedWorldSeconds"),AuthoredStopStarted);Receipt->SetNumberField(TEXT("completedWorldSeconds"),Now);
    Receipt->SetNumberField(TEXT("actualMaximumCentreExcursionCm"),MaximumAuthoredStopExcursionCm);
    Receipt->SetNumberField(TEXT("declaredMaximumCentreExcursionCm"),Stop.MaximumCentreExcursionCm);
    Receipt->SetNumberField(TEXT("actualFinalHeadingErrorDegrees"),LastFollowerHeadingError);
    Receipt->SetNumberField(TEXT("authoredOutgoingLegYawDegrees"),AuthoredStopOutgoingYawDegrees);
    Receipt->SetNumberField(TEXT("actualEndpointBearingDegrees"),LastFollowerDesiredYaw);
    const FVector Leg=Outgoing.GetSafeNormal2D();
    Receipt->SetNumberField(TEXT("actualSignedCrossTrackFromAuthoredLegCm"),FVector::DotProduct(O.ChassisPose.GetLocation()-Anchor,FVector::CrossProduct(FVector::UpVector,Leg)));
    Receipt->SetNumberField(TEXT("actualSupportMask"),O.SupportMask);Receipt->SetBoolField(TEXT("authoritativeServiceQuiet"),Quiet(O));
    Receipt->SetStringField(TEXT("scope"),TEXT("Real leased native motors/brakes at an authored sampled staging patch. Existing FaceFirst anchor-feedback throttle/raw yaw is permitted to translate in the graded area; actual centre excursion includes arrival residual. Sampled anchor surfaces are not a wholly-flat turn-volume or full-body rotation certificate.15-second stage clock and original30-second progress/job deadline remain live; no pose clamp or resource/reset writes."));
    AuthoredStopReceipts.Add(MakeShared<FJsonValueObject>(Receipt));++NextAuthoredStop;
    FollowerMotion=EFollowerMotion::Forward;AuthoredStopStarted=-1;AuthoredStopStableSince=-1;
    Record(O,true,TEXT("authored_stage_actual_stopped_turn_complete"));return true;
}
bool AMAYACityBlockMissionTrial::CaptureRollingGeometry(const FMAYADeliveryRobotObservation& O)
{
    RollingGeometryCapture=MakeShared<FJsonObject>();
    RollingGeometryCapture->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());RollingGeometryCapture->SetNumberField(TEXT("frame"),double(GFrameCounter));
    RollingGeometryCapture->SetNumberField(TEXT("resetGeneration"),double(O.ResetGeneration));RollingGeometryCapture->SetStringField(TEXT("parcelId"),ParcelId);
    auto Refuse=[&](const TCHAR* Why){RollingGeometryCapture->SetStringField(TEXT("refusal"),Why);return false;};
    TSharedPtr<FJsonObject> Native;const TArray<TSharedPtr<FJsonValue>>* NativeParts=nullptr;
    if(!Quiet(O)||!InventoryIntact()||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()),Native)
        ||!Native.IsValid()||!Native->TryGetArrayField(TEXT("parts"),NativeParts)||!NativeParts||NativeParts->Num()<7)
        return Refuse(TEXT("Loaded Quiet/native seven-part geometry capture unavailable"));
    const FVector Forward=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
    const FVector Lateral=FVector::CrossProduct(FVector::UpVector,Forward);
    if(Forward.ContainsNaN()||Forward.SizeSquared2D()<=UE_SMALL_NUMBER)return Refuse(TEXT("Actual horizontal optical forward is degenerate"));
    FVector BankA=FVector::ZeroVector,BankB=FVector::ZeroVector;TArray<TSharedPtr<FJsonValue>> WheelRows;
    for(int32 W=0;W<6;++W)
    {
        const auto Wheel=(*NativeParts)[W+1]->AsObject();const TSharedPtr<FJsonObject>* COM=nullptr;
        FString WheelRole;bool Simulating=false;double Kg=0,X=0,Y=0,Z=0;
        const FString ExpectedRole=W==0?TEXT("SM_DeliveryRobot_Wheel"):FString::Printf(TEXT("SM_DeliveryRobot_Wheel%d"),W);
        if(!Wheel.IsValid()||!Wheel->TryGetStringField(TEXT("sourceRole"),WheelRole)||WheelRole!=ExpectedRole
            ||!Wheel->TryGetBoolField(TEXT("simulating"),Simulating)||!Simulating||!Wheel->TryGetNumberField(TEXT("nativeMassKg"),Kg)||!FMath::IsFinite(Kg)||Kg<=0
            ||!Wheel->TryGetObjectField(TEXT("nativeCOMWorldCm"),COM)||!COM||!COM->IsValid()
            ||!(*COM)->TryGetNumberField(TEXT("xCm"),X)||!(*COM)->TryGetNumberField(TEXT("yCm"),Y)||!(*COM)->TryGetNumberField(TEXT("zCm"),Z)
            ||!FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Z))return Refuse(TEXT("Actual original wheel native COM/body identity is unavailable"));
        const FVector Center(X,Y,Z);if(W<3)BankA+=Center;else BankB+=Center;
        auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("wheelIndex"),W);Row->SetStringField(TEXT("sourceRole"),WheelRole);Row->SetArrayField(TEXT("nativeCOMWorldCm"),MayaBlockTrial::Vec(Center));Row->SetNumberField(TEXT("nativeMassKg"),Kg);WheelRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    BankA/=3;BankB/=3;const double Separation=FVector::DotProduct(BankB-BankA,Lateral);
    const double HalfTrack=.5*FMath::Abs(Separation);
    if(!FMath::IsFinite(HalfTrack)||HalfTrack<=UE_SMALL_NUMBER)return Refuse(TEXT("Measured actual native COM bank separation is not positive finite"));
    RollingHalfTrackCm=HalfTrack;RollingGeometryCapture->SetNumberField(TEXT("measuredNativeCOMHalfTrackCm"),HalfTrack);
    RollingGeometryCapture->SetNumberField(TEXT("signedBankSeparationCm"),Separation);RollingGeometryCapture->SetArrayField(TEXT("bankMeanNativeCOMWorldCmA"),MayaBlockTrial::Vec(BankA));RollingGeometryCapture->SetArrayField(TEXT("bankMeanNativeCOMWorldCmB"),MayaBlockTrial::Vec(BankB));
    RollingGeometryCapture->SetArrayField(TEXT("opticalForwardHorizontal"),MayaBlockTrial::Vec(Forward));RollingGeometryCapture->SetArrayField(TEXT("positiveRawYawLateral"),MayaBlockTrial::Vec(Lateral));RollingGeometryCapture->SetArrayField(TEXT("actualSixNativeWheelCOMs"),WheelRows);
    RollingGeometryCapture->SetStringField(TEXT("scope"),TEXT("Once at loaded actual Quiet: six original wheel nativeCOMWorldCm from GetCenterOfMass, not component origins; bank-mean projected halftrack for an ideal rolling steering model. Actual wheel slip/contact/torque remains independently observed, not a no-slip prediction or identified solver sample."));
    return true;
}
void AMAYACityBlockMissionTrial::Enter(EStep Next)
{ Step=Next;StepStarted=GetWorld()->GetTimeSeconds();StableSince=-1;if(Next!=EStep::Driving){FollowerMotion=EFollowerMotion::Forward;FollowerStableSince=-1;}Record(Director->Observation(),true); }

void AMAYACityBlockMissionTrial::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    SetActorTickEnabled(false);return;
#endif
    if(!FParse::Param(FCommandLine::Get(),TEXT("MayaV01CityBlockMissionTest"))
        ||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))
        ||!FParse::Value(FCommandLine::Get(),TEXT("MayaV01Output="),Output)||Output.IsEmpty())
    {SetActorTickEnabled(false);return;}
    Output=FPaths::ConvertRelativePathToFull(Output);Started=StepStarted=GetWorld()->GetTimeSeconds();
    if(IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("result.json"))))
    {bFinished=true;SetActorTickEnabled(false);FPlatformMisc::RequestExit(false,TEXT("MAYACityBlockMissionTrial"));return;}
    bool Exclusive=FApp::CanEverRender();
    const TCHAR* Forbidden[]={TEXT("MayaV01BlockRouteSurvey"),TEXT("MayaV01CargoTest"),TEXT("MayaV01CurbTest"),TEXT("MayaV01TrafficEncounterTest"),TEXT("MayaV01OperatingTest"),TEXT("MayaV01DamageTest"),TEXT("MayaV01FullInput"),TEXT("MayaV01ReverseSteerTest"),TEXT("MayaV01CashShiftTest"),TEXT("MayaV01UIScreenTest"),TEXT("MayaV01ObserveInput"),TEXT("MayaV01NewShiftReloadTest"),TEXT("MayaV01RingInteractionTest"),TEXT("MayaV01BoostTest"),TEXT("MayaV01VisualCapture"),TEXT("MayaV01CancelOpeningTest"),TEXT("MayaV01CargoVideo"),TEXT("MayaV01AirLoweringTest"),TEXT("MayaV01DamageCalibration"),TEXT("MayaV01PackageUIScreenSmoke"),TEXT("nullrhi")};
    for(const TCHAR* Flag:Forbidden)Exclusive=Exclusive&&!FParse::Param(FCommandLine::Get(),Flag);
    FString Fixture;Exclusive=Exclusive&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01InitialBattery="),Fixture)&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),Fixture);
    FString Expected=FPaths::Combine(Output,TEXT("GameUserSettings.ini"));
    FString Actual=FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"),nullptr,*FPaths::GeneratedConfigDir()));
    FPaths::NormalizeFilename(Expected);FPaths::NormalizeFilename(Actual);
    if(!Check(Exclusive,TEXT("Exclusive rendered Development fourth-mission test without resource/cargo/traffic fixtures"))
        ||!Check(GConfig&&GConfig->FindConfigFile(GGameUserSettingsIni)&&Actual.Equals(Expected,ESearchCase::IgnoreCase),TEXT("Actual settings destination is isolated attempt file"))
        ||!Check(Robot.IsValid()&&Director.IsValid()&&GetWorld()->GetMapName().Contains(TEXT("Small_City"))&&Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()),TEXT("Actual configured Small City and owned robot/director"))
        ||!Check(Definition.Crossings.Num()==2&&Path.Build(Definition.Points),TEXT("Installed source-pinned two-crossing route content"))
        ||!Check(Definition.StopTurns.Num()==6,TEXT("Six explicit authored stop-turn checkpoints installed")))
    {Finish(false,TEXT("Fourth-mission initialization invariant failed"));return;}
    const auto& S=Director->Snapshot();BaseCash=S.TotalCredits;BaseFees=S.RechargeFeesPaidDollars;BaseCompleted=S.CompletedJobs;ShiftId=S.ShiftInstanceId;
    ScreenshotHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this,&AMAYACityBlockMissionTrial::ScreenshotProcessed);
    Drive();Record(Director->Observation(),true);
}

bool AMAYACityBlockMissionTrial::InventoryIntact() const
{
    if(!Director.IsValid()||!Robot.IsValid()||!Parcel.IsValid()||!ParcelBody.IsValid()
        ||Director->GetParcelCount()!=1||Director->GetSecuredParcelCount()!=1||Director->GetParcel()!=Parcel.Get()
        ||Parcel->GetParcelBody()!=ParcelBody.Get()||Parcel->GetParcelId()!=ParcelId
        ||ParcelBody->GetCollisionEnabled()!=ECollisionEnabled::QueryAndPhysics||!ParcelBody->IsSimulatingPhysics())return false;
    double Kg=0;FTransform Native;
    return Parcel->ObserveMass(Kg)&&FMath::Abs(Kg-1)<=.05&&Parcel->ObserveNativePose(Native);
}

bool AMAYACityBlockMissionTrial::CrossingTrafficClear(int32 I,TSharedPtr<FJsonObject>& Evidence) const
{
    Evidence=MakeShared<FJsonObject>();if(!Definition.Crossings.IsValidIndex(I))return false;
    const auto& C=Definition.Crossings[I];const FVector A=Path.Sample(C.EntryArcCm),B=Path.Sample(C.ExitArcCm);
    const FVector Delta=B-A;const FVector Direction=Delta.GetSafeNormal2D();
    FCollisionObjectQueryParams Objects;Objects.AddObjectTypesToQuery(ECC_Vehicle);Objects.AddObjectTypesToQuery(ECC_PhysicsBody);Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYABlockActualCrossingTraffic),false,Robot.Get());Q.AddIgnoredActor(this);if(Parcel.IsValid())Q.AddIgnoredActor(Parcel.Get());
    TArray<FOverlapResult> Hits;GetWorld()->OverlapMultiByObjectType(Hits,(A+B)*.5+FVector(0,0,60),Direction.Rotation().Quaternion(),Objects,FCollisionShape::MakeBox(FVector(Delta.Size2D()*.5+75,75,70)),Q);
    TArray<TSharedPtr<FJsonValue>> Cars;bool Clear=true;
    for(const auto& H:Hits)if(auto* Car=Cast<ACitySampleVehicleBase>(H.GetActor()))
    {auto* Body=H.GetComponent();if(!Body||!Body->IsPhysicsCollisionEnabled()||Body->GetCollisionResponseToChannel(ECC_PhysicsBody)!=ECR_Block)continue;Clear=false;
        if(Cars.Num()<32){auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("actor"),Car->GetPathName());J->SetStringField(TEXT("component"),Body->GetPathName());J->SetBoolField(TEXT("nativeBodyValid"),Body->GetBodyInstance()&&Body->GetBodyInstance()->IsValidBodyInstance());Cars.Add(MakeShared<FJsonValueObject>(J));}}
    Evidence->SetNumberField(TEXT("crossingIndex"),I);Evidence->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());Evidence->SetBoolField(TEXT("actualVehicleOccupancyClearNow"),Clear);Evidence->SetArrayField(TEXT("actualBlockingVehicleOverlaps"),Cars);
    const auto State=MAYACityBlockRoute::Read(GetWorld(),Definition);
    if(State.Crossings.IsValidIndex(I)){Evidence->SetBoolField(TEXT("signalKnown"),State.Crossings[I].bStateKnown);Evidence->SetBoolField(TEXT("signalOpened"),State.Crossings[I].bOpened);Evidence->SetNumberField(TEXT("currentPeriodIndex"),State.Crossings[I].CurrentPeriodIndex);}
    Evidence->SetStringField(TEXT("scope"),TEXT("Actual current original-vehicle collision occupancy snapshot, not future traffic clearance. Native pedestrian state is read-only advisory; robot does not manufacture waiting-pedestrian demand or require eventual Open."));
    return Clear;
}

void AMAYACityBlockMissionTrial::Tick(float Dt)
{
    Super::Tick(Dt);if(bFinished||Started<0)return;
    const double Now=GetWorld()->GetTimeSeconds();
    if(!Robot.IsValid()||!Director.IsValid()||Now-Started>900){Finish(false,TEXT("Owned mission objects lost or original900-gameplay-second trial deadline reached"));return;}
    if(!PollCapture()||bFinished)return;
    FMAYADeliveryRobotObservation O;
    const bool bObserved = Robot->ReadDeliveryState(O);
    if(!O.Fault.IsEmpty()){Record(O,true);Finish(false,TEXT("Actual native robot fault: ")+O.Fault);return;}
    if(!bObserved||!O.bKnown||!O.bReady)
    {
        Record(O);StableSince=-1;
        if(Step==EStep::Baseline&&Now-StepStarted<=20){Drive();return;}
        Record(O,true);Finish(false,TEXT("Actual native known/ready admission failed outside the original20-second baseline wait"));return;
    }
    const auto& S=Director->Snapshot();Record(O);
    if(S.bDroneTotalled||S.bVehicleContactPending||S.Phase==EMAYADeliveryPhase::Failed||S.BatteryPercent<=0
        ||S.ShiftInstanceId!=ShiftId||S.RechargeFeesPaidDollars!=BaseFees||(ResetGeneration&&O.ResetGeneration!=ResetGeneration))
    {Finish(false,TEXT("Actual mission failure, vehicle adjudication, energy, resource or native identity changed: ")+Director->GetRuntimeStatus());return;}
    if(!JobId.IsEmpty()&&S.JobInstanceId!=JobId){Finish(false,TEXT("Actual fourth mission instance changed"));return;}
    const bool Moving=Step==EStep::Driving||Step==EStep::CrossWait;
    if(Moving&&(!InventoryIntact()||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()))
    {Finish(false,TEXT("Actual locked doors or same-body1kg cargo custody failed during traversal"));return;}
    if(Step==EStep::Baseline)
    {
        Drive();
        if(Now-StepStarted>20){Finish(false,TEXT("Installed fourth offer/actual stopped baseline not ready within20s"));return;}
        if(!Quiet(O)||!Director->IsCityBlockRouteReady()||Director->GetAvailableProfileCount()!=4){StableSince=-1;return;}
        if(StableSince<0)StableSince=Now;if(Now-StableSince<2)return;
        ResetGeneration=O.ResetGeneration;
        while(SelectedOffers<3){if(!Director->RequestSelectOffer(1)){Finish(false,TEXT("Normal safe offer selection refused"));return;}++SelectedOffers;}
        if(!Check(S.Job.ProfileId==TEXT("city_block")&&S.Job.ParcelCount==1&&S.Job.PayloadKg==1&&S.Job.DeadlineSeconds==Definition.DeadlineSeconds,TEXT("Actual fourth offer selected through three normal offer actions, one1kg parcel"))
            ||!Director->RequestAccept()){Finish(false,TEXT("Actual fourth-profile acceptance refused"));return;}
        JobId=Director->Snapshot().JobInstanceId;Check(!JobId.IsEmpty(),TEXT("Real accepted mission instance allocated"));Enter(EStep::Pickup);return;
    }
    if(Step==EStep::Pickup||Step==EStep::Dropoff)
    {
        Drive();const bool Pickup=Step==EStep::Pickup;
        if(S.Phase==(Pickup?EMAYADeliveryPhase::Loading:EMAYADeliveryPhase::Unloading)){Enter(Pickup?EStep::Loading:EStep::Unloading);return;}
        auto* Station=Director->ObjectiveStation();
        if(!Station||FVector::Dist2D(O.ChassisPose.GetLocation(),Station->GetServicePoint())>AMAYADeliveryStation::ObjectiveRingRadiusCm)
        {Finish(false,TEXT("Measured actual robot endpoint is outside its real objective ring"));return;}
        if(Now-StepStarted>20){Finish(false,TEXT("Actual supported/closed service admission exceeded20s"));return;}
        if(!Quiet(O)){StableSince=-1;return;}if(StableSince<0)StableSince=Now;
        if(Now-StableSince>=1){++ServiceCalls;if(!Director->RequestInteract()){Finish(false,TEXT("Normal actual service request refused: ")+Director->GetLastServiceFailureDetail());return;}Enter(Pickup?EStep::Loading:EStep::Unloading);}return;
    }
    if(Step==EStep::Loading)
    {
        Drive();if(Now-StepStarted>16){Finish(false,TEXT("Actual pickup did not reach secured custody within16s: ")+Director->GetLastServiceFailureDetail());return;}
        if(S.Phase==EMAYADeliveryPhase::Accepted&&S.Service.Generation!=0){Finish(false,TEXT("Actual pickup cancelled; original failure retained: ")+Director->GetLastServiceFailureDetail());return;}
        if(S.Phase!=EMAYADeliveryPhase::Carrying)return;
        Parcel=Director->GetParcel();ParcelBody=Parcel.IsValid()?Parcel->GetParcelBody():nullptr;ParcelId=Parcel.IsValid()?Parcel->GetParcelId():FString();
        if(!Check(InventoryIntact()&&Quiet(O),TEXT("Real pickup completed same native1kg parcel with stopped support/latch"))){Finish(false,TEXT("Pickup custody endpoint failed"));return;}
        Enter(EStep::ArrowView);NextArrowCapture=Now;return;
    }
    if(Step==EStep::ArrowView)
    {
        Drive();
        const bool bCustodyIntact=InventoryIntact(), bPhysicalLocks=Robot->AreDeliveryDoorsClosedAndPhysicallyLocked(), bActualQuiet=Quiet(O);
        if((!bCustodyIntact||!bPhysicalLocks||!bActualQuiet)&&!ArrowFirstGuardMiss.IsValid())
        {
            ArrowFirstGuardMiss=MakeShared<FJsonObject>();ArrowFirstGuardMiss->SetNumberField(TEXT("worldSeconds"),Now);
            ArrowFirstGuardMiss->SetBoolField(TEXT("sameNativeCargoCustodyIntact"),bCustodyIntact);ArrowFirstGuardMiss->SetBoolField(TEXT("actualPhysicalClosedLocks"),bPhysicalLocks);ArrowFirstGuardMiss->SetBoolField(TEXT("actualQuiet"),bActualQuiet);
            ArrowFirstGuardMiss->SetBoolField(TEXT("nativeKnown"),O.bKnown);ArrowFirstGuardMiss->SetBoolField(TEXT("nativeReady"),O.bReady);ArrowFirstGuardMiss->SetBoolField(TEXT("stationaryLatched"),O.bDoorsClosedLatched);ArrowFirstGuardMiss->SetNumberField(TEXT("supportMask"),O.SupportMask);
            ArrowFirstGuardMiss->SetStringField(TEXT("cargoEvidence"),Director->ReadCargoInventoryEvidenceJson());ArrowFirstGuardMiss->SetStringField(TEXT("nativeTelemetry"),Robot->ReadTelemetryJson());Record(O,true);
        }
        if(!bCustodyIntact||!bPhysicalLocks){Finish(false,!bCustodyIntact?TEXT("Actual arrow-view same-native cargo custody failed"):TEXT("Actual arrow-view physical closed locks failed"));return;}
        if(Now-StepStarted>12){Record(O,true);Finish(false,TEXT("Actual braked post-load settle/five arrow captures exceeded the original12-second bound"));return;}
        if(!bActualQuiet){StableSince=-1;return;}
        if(StableSince<0)StableSince=Now;if(Now-StableSince<.5)return;
        if(RollingHalfTrackCm<=0&&!Check(CaptureRollingGeometry(O),TEXT("Actual loaded Quiet six-native-COM rolling halftrack captured")))
        {Record(O,true);Finish(false,TEXT("Actual native rolling steering geometry capture failed"));return;}
        if(ArrowCaptureCount<5&&Now>=NextArrowCapture&&PendingCapture.IsEmpty())
        {if(RequestCapture(FString::Printf(TEXT("arrow-%02d"),ArrowCaptureCount))){++ArrowCaptureCount;NextArrowCapture=Now+.8;}}
        if(Now-StepStarted>=3.2&&ArrowCaptureCount==5&&PendingCapture.IsEmpty())
        {Progress=0;LastAdvanceArc=0;LastAdvanceWorldS=Now;Enter(EStep::Driving);}return;
    }
    if(Step==EStep::Driving)
    {
        const auto P=Path.Project(O.ChassisPose.GetLocation());
        if(!P.bValid||P.DistanceSquaredCm>FMath::Square(150.)||O.VelocityCmps.Size()>130)
        {Finish(false,TEXT("Actual course projection150cm or130cm/s diagnostic containment failed"));return;}
        Progress=FMath::Max(Progress,P.ArcCm);
        // This clock stays live during every brake/turn/settle return below.
        if(Progress>=LastAdvanceArc+25){LastAdvanceArc=Progress;LastAdvanceWorldS=Now;}
        if(Now-LastAdvanceWorldS>30){Record(O,true);Finish(false,TEXT("Actual route progress stalled30s without any position/reset/resource shortcut"));return;}
        for(int32 I=0;I<Definition.Crossings.Num();++I)if(!(CompletedCrossingMask&(1<<I)))
        {
            const auto& C=Definition.Crossings[I];
            if(!(CrossingEntranceMask&(1<<I))&&Progress>=C.WaitingBeforeArcCm-45&&Progress<C.EntryArcCm)
            {ActiveCrossing=I;CrossWaitStarted=Now;Enter(EStep::CrossWait);return;}
            if(Progress>C.ExitArcCm+25){CompletedCrossingMask|=1<<I;RequestCapture(FString::Printf(TEXT("crossing-%d-after"),I));}
        }
        auto* Destination=Director->FindStation(S.Job.DropoffStationId);
        if(Destination&&FVector::Dist2D(O.ChassisPose.GetLocation(),Destination->GetServicePoint())<=50)
        {Drive();Enter(EStep::Dropoff);return;}
        if(AdvanceAuthoredStop(O,Now))return;
        FVector Aim=Path.Sample(FMath::Min(Progress+60,Path.Arc.Last()));
        if(Definition.StopTurns.IsValidIndex(NextAuthoredStop))
        {
            const auto& Stop=Definition.StopTurns[NextAuthoredStop];
            const double StopArc=Path.Arc[Stop.PointIndex];
            const FVector StopPoint=Definition.Points[Stop.PointIndex];
            const FVector Incoming=(StopPoint-Definition.Points[Stop.PointIndex-1]).GetSafeNormal2D();
            if(Incoming.ContainsNaN()||Incoming.SizeSquared2D()<=UE_SMALL_NUMBER)
            {Drive();Record(O,true);Finish(false,TEXT("Authored stop incoming tangent is nonfinite or degenerate"));return;}
            if(FVector::DotProduct(O.ChassisPose.GetLocation()-StopPoint,Incoming)>Stop.ArrivalRadiusCm&&FVector::Dist2D(O.ChassisPose.GetLocation(),StopPoint)>Stop.ArrivalRadiusCm)
            {Drive();Record(O,true);Finish(false,TEXT("Actual authored checkpoint passed without its required braked arrival"));return;}
            // Preserve60cm forward lookahead on this incoming straight leg.
            // Never anticipate the outgoing leg or shrink steering radius to zero.
            Aim=Progress+60>StopArc?StopPoint+Incoming*(Progress+60-StopArc):Path.Sample(Progress+60);
        }
        const FVector Forward=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
        const FVector Lateral=FVector::CrossProduct(FVector::UpVector,Forward),Delta=Aim-O.ChassisPose.GetLocation();
        LastFollowerAim=Aim;LastFollowerDesiredYaw=Delta.Rotation().Yaw;
        LastFollowerHeadingError=FMath::FindDeltaAngleDegrees(Forward.Rotation().Yaw,LastFollowerDesiredYaw);LastFollowerPolicyWorldSeconds=Now;
        LastRollingX=FVector::DotProduct(Delta,Forward);LastRollingY=FVector::DotProduct(Delta,Lateral);
        const double DistanceSquared=FMath::Square(LastRollingX)+FMath::Square(LastRollingY);
        if(Forward.ContainsNaN()||Forward.SizeSquared2D()<=UE_SMALL_NUMBER||Delta.ContainsNaN()
            ||!FMath::IsFinite(LastRollingX)||!FMath::IsFinite(LastRollingY)||LastRollingX<=0
            ||!FMath::IsFinite(DistanceSquared)||DistanceSquared<=UE_SMALL_NUMBER||!FMath::IsFinite(RollingHalfTrackCm)||RollingHalfTrackCm<=0)
        {Drive();Record(O,true);Finish(false,TEXT("Actual rolling follower aim is behind, degenerate or nonfinite; braked refusal without pivot/teleport fallback"));return;}
        LastRollingCurvature=2*LastRollingY/DistanceSquared;LastRollingRatio=RollingHalfTrackCm*LastRollingCurvature;
        const bool bFinalBClimb=NextAuthoredStop==5&&Definition.StopTurns.Num()==6;
        LastRollingRawThrottle=bFinalBClimb?.60:FMath::Clamp(.60*(1-FMath::Abs(LastFollowerHeadingError)/100.),.22,.60);
        if(Definition.StopTurns.IsValidIndex(NextAuthoredStop))
        {
            const auto& Stop=Definition.StopTurns[NextAuthoredStop];
            // Current projection, not historic maximum progress; both arc and
            // actual planar distance must be near this declared incoming stop.
            const double IncomingRemaining=FMath::Max(0.,Path.Arc[Stop.PointIndex]-P.ArcCm);
            const double ActualDistance=FVector::Dist2D(O.ChassisPose.GetLocation(),Definition.Points[Stop.PointIndex]);
            if(!FMath::IsFinite(IncomingRemaining)||!FMath::IsFinite(ActualDistance)||!FMath::IsFinite(O.OpticalForwardCmps))
            {Drive();Record(O,true);Finish(false,TEXT("Authored stop approach distance or measured optical-forward speed is nonfinite"));return;}
            if(!bFinalBClimb)
            {
                const double ApproachDistance=FMath::Max(IncomingRemaining,ActualDistance);
                const double SlowFeedbackCap=FMath::Clamp(.12+.012*(20.-O.OpticalForwardCmps),.12,.36);
                const double ApproachCap=FMath::Lerp(SlowFeedbackCap,.60,FMath::Clamp((ApproachDistance-30.)/50.,0.,1.));
                LastRollingRawThrottle=FMath::Min(LastRollingRawThrottle,ApproachCap);
            }
        }
        if(!FMath::IsFinite(LastRollingCurvature)||!FMath::IsFinite(LastRollingRatio)||!FMath::IsFinite(O.AngularVelocityRadps.Z))
        {Drive();Record(O,true);Finish(false,TEXT("Actual heading feedback geometry or native yaw-rate is nonfinite; braked diagnostic refusal"));return;}
        // Final B heading feedback begins only after Q0 completed actual fullQuiet.
        // Existing PD gains also remain on the final post-stage connector.
        const bool bUseHeadingFeedback=bFinalBClimb||NextAuthoredStop==Definition.StopTurns.Num();
        if(!bUseHeadingFeedback&&FMath::Abs(LastRollingRatio)>=1)
        {Drive();Record(O,true);Finish(false,TEXT("Actual rolling curvature requires a stopped/reversed wheel bank; braked diagnostic refusal"));return;}
        LastRollingRawSteer=bUseHeadingFeedback
            ?FMath::Clamp(LastFollowerHeadingError/35.-.5*O.AngularVelocityRadps.Z,-.65,.65)
            :LastRollingRawThrottle*LastRollingRatio;
        LastRollingNormalization=FMath::Max(1.,FMath::Max(FMath::Abs(-LastRollingRawThrottle-LastRollingRawSteer),FMath::Abs(-LastRollingRawThrottle+LastRollingRawSteer)));
        const float T=float(LastRollingRawThrottle/LastRollingNormalization),Steer=float(LastRollingRawSteer/LastRollingNormalization);
        if(!Drive(T,Steer,false)){Finish(false,TEXT("Actual leased native hybrid follower motor command refused"));return;}
        LastCommandSteeringMode=bFinalBClimb?TEXT("final_B_heading_PD_cruise_0.60"):(bUseHeadingFeedback?TEXT("poststage_PD"):TEXT("rolling_T_times_q"));
        return;
    }
    if(Step==EStep::CrossWait)
    {
        Drive();
        if(Now-CrossWaitStarted>60){Finish(false,TEXT("Actual original traffic occupancy/quiet crossing wait exceeded60s"));return;}
        if(!Quiet(O)){StableSince=-1;return;}
        TSharedPtr<FJsonObject> Evidence;const bool Clear=CrossingTrafficClear(ActiveCrossing,Evidence);
        if(!Clear){StableSince=-1;return;}if(StableSince<0)StableSince=Now;
        if(Now-StableSince<1)return;
        Evidence->SetStringField(TEXT("decision"),TEXT("Native signal retained as observed advisory; real vehicle occupancy clear and closed supported stop observed1s"));
        CrossingReceipts.Add(MakeShared<FJsonValueObject>(Evidence));RequestCapture(FString::Printf(TEXT("crossing-%d-before"),ActiveCrossing));
        // Mark only this entrance admission; completion is separately witnessed after its exit.
        CrossingEntranceMask|=1<<ActiveCrossing;LastAdvanceArc=Progress;LastAdvanceWorldS=Now;
        Enter(EStep::Driving);return;
    }
    if(Step==EStep::Unloading)
    {
        Drive();if(Now-StepStarted>16){Finish(false,TEXT("Actual unloading/latched commit exceeded16s: ")+Director->GetLastServiceFailureDetail());return;}
        if(S.Phase==EMAYADeliveryPhase::Carrying){Finish(false,TEXT("Actual unloading cancelled; original failure retained: ")+Director->GetLastServiceFailureDetail());return;}
        if(S.Phase!=EMAYADeliveryPhase::Succeeded)return;
        const bool Good=Quiet(O)&&Director->GetParcelCount()==0&&Director->GetSecuredParcelCount()==0&&S.Result.bSuccess
            &&S.Result.JobInstanceId==JobId&&S.Result.Credits==10&&S.TotalCredits==BaseCash+10&&S.CompletedJobs==BaseCompleted+1
            &&S.RechargeFeesPaidDollars==BaseFees&&CrossingEntranceMask==3&&CompletedCrossingMask==3&&CrossingReceipts.Num()==2
            &&NextAuthoredStop==Definition.StopTurns.Num()&&AuthoredStopReceipts.Num()==Definition.StopTurns.Num();
        if(!Check(Good,TEXT("Real fourth delivery committed once after actual unload/closed latch, two crossings, exact$10 and no fee/reset"))){Finish(false,TEXT("Actual fourth-delivery endpoint/ledger/crossing proof incomplete"));return;}
        Enter(EStep::FinalView);RequestCapture(TEXT("fourth-delivery-complete"));return;
    }
    if(Step==EStep::FinalView)
    {
        Drive();
        if(Now-StepStarted>12){Finish(false,TEXT("Actual final delivery PNG did not complete within12 gameplay seconds"));return;}
        if(!bFinalCaptureCompleted&&PendingCapture.IsEmpty())RequestCapture(TEXT("fourth-delivery-complete"));
        if(bFinalCaptureCompleted)Finish(true,TEXT("One genuine fourth-profile1kg/$10 delivery across both original crossings completed; signals were read-only advisory. Native automation, not human input or a full-shift/product QA claim."));
    }
}

void AMAYACityBlockMissionTrial::Record(const FMAYADeliveryRobotObservation& O,bool Force,const TCHAR* AlignmentEndpoint)
{
    if(!Director.IsValid()||!Robot.IsValid())return;const double Now=GetWorld()->GetTimeSeconds();if(!Force&&Now<NextRecord)return;NextRecord=Now+.5;
    if(Samples.Num()>=2000)return;
    auto J=MakeShared<FJsonObject>();const auto& S=Director->Snapshot();
    J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("step"),int32(Step));J->SetNumberField(TEXT("progressArcCm"),Progress);
    J->SetArrayField(TEXT("chassisCm"),MayaBlockTrial::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("velocityCmps"),MayaBlockTrial::Vec(O.VelocityCmps));J->SetArrayField(TEXT("angularVelocityRadps"),MayaBlockTrial::Vec(O.AngularVelocityRadps));
    J->SetBoolField(TEXT("nativeObservationKnown"),O.bKnown);J->SetBoolField(TEXT("nativeObservationReady"),O.bReady);J->SetStringField(TEXT("nativeObservationFault"),O.Fault);
    J->SetNumberField(TEXT("supportMask"),O.SupportMask);J->SetBoolField(TEXT("stationaryLatched"),O.bDoorsClosedLatched);J->SetBoolField(TEXT("physicalClosedLocks"),Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());J->SetNumberField(TEXT("resetGeneration"),double(O.ResetGeneration));
    J->SetStringField(TEXT("jobInstanceId"),S.JobInstanceId);J->SetStringField(TEXT("shiftInstanceId"),S.ShiftInstanceId);J->SetNumberField(TEXT("missionPhase"),int32(S.Phase));J->SetNumberField(TEXT("cashDollars"),S.TotalCredits);J->SetNumberField(TEXT("energyPercent"),S.BatteryPercent);J->SetNumberField(TEXT("conditionPercent"),S.CargoConditionPercent);J->SetNumberField(TEXT("remainingMissionSeconds"),FMath::Max(0.,S.Job.DeadlineSeconds-S.ElapsedSeconds));J->SetNumberField(TEXT("completedCrossingMask"),CompletedCrossingMask);
    J->SetBoolField(TEXT("diagnosticAlignmentRestQualified"),AlignmentRest(O));J->SetBoolField(TEXT("authoritativeServiceQuietQualified"),Quiet(O));J->SetBoolField(TEXT("actualAllSixGrounded"),O.bGrounded);
    if(AlignmentEndpoint)J->SetStringField(TEXT("alignmentEndpoint"),AlignmentEndpoint);
    J->SetNumberField(TEXT("nextAuthoredStopIndex"),NextAuthoredStop);J->SetNumberField(TEXT("authoredStopStartedWorldSeconds"),AuthoredStopStarted);
    J->SetNumberField(TEXT("authoredStopDistanceCm"),LastAuthoredStopDistanceCm);J->SetNumberField(TEXT("authoredStopMaximumExcursionCm"),MaximumAuthoredStopExcursionCm);
    J->SetBoolField(TEXT("lastNativeCommandAccepted"),bLastCommandAccepted);
    J->SetNumberField(TEXT("lastRequestedThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("lastRequestedRawYawSteer"),LastRequestedSteer);J->SetBoolField(TEXT("lastRequestedBrake"),LastRequestedBrake);
    J->SetNumberField(TEXT("lastCommandWorldSeconds"),LastCommandWorldSeconds);J->SetNumberField(TEXT("lastCommandFrame"),double(LastCommandFrame));
    J->SetStringField(TEXT("lastCommandSteeringMode"),LastCommandSteeringMode);
    if(LastAuthoredBrakeCapture.IsValid())J->SetObjectField(TEXT("lastAuthoredBrakeCapture"),LastAuthoredBrakeCapture);
    J->SetNumberField(TEXT("followerMotion"),int32(FollowerMotion));J->SetNumberField(TEXT("followerAlignmentEpisodes"),FollowerAlignmentEpisodes);J->SetNumberField(TEXT("followerAlignmentStartedWorldSeconds"),FollowerAlignmentStarted);
    J->SetArrayField(TEXT("lastFollowerAimCm"),MayaBlockTrial::Vec(LastFollowerAim));J->SetNumberField(TEXT("lastFollowerDesiredOpticalYawDegrees"),LastFollowerDesiredYaw);J->SetNumberField(TEXT("lastFollowerHeadingErrorDegrees"),LastFollowerHeadingError);J->SetNumberField(TEXT("lastFollowerPolicyWorldSeconds"),LastFollowerPolicyWorldSeconds);
    J->SetNumberField(TEXT("measuredNativeCOMHalfTrackCm"),RollingHalfTrackCm);J->SetNumberField(TEXT("rollingLocalAimForwardCm"),LastRollingX);J->SetNumberField(TEXT("rollingLocalAimLateralCm"),LastRollingY);
    J->SetNumberField(TEXT("rollingCurvaturePerCm"),LastRollingCurvature);J->SetNumberField(TEXT("rollingSteerThrottleRatio"),LastRollingRatio);J->SetNumberField(TEXT("rollingRawThrottle"),LastRollingRawThrottle);J->SetNumberField(TEXT("rollingRawSteer"),LastRollingRawSteer);J->SetNumberField(TEXT("rollingPairedNormalization"),LastRollingNormalization);
    J->SetNumberField(TEXT("rollingPreNormalizationBankRequestA"),-LastRollingRawThrottle-LastRollingRawSteer);J->SetNumberField(TEXT("rollingPreNormalizationBankRequestB"),-LastRollingRawThrottle+LastRollingRawSteer);
    J->SetNumberField(TEXT("lastSubmittedBankRequestA"),-LastRequestedThrottle-LastRequestedSteer);J->SetNumberField(TEXT("lastSubmittedBankRequestB"),-LastRequestedThrottle+LastRequestedSteer);
    J->SetStringField(TEXT("rollingCommandScope"),TEXT("Policy geometry timestamp and accepted float command timestamp are distinct. Signed bank requests are inputs to unchanged native motor law, not measured contact force; actual motor/omega/contact readbacks remain nativeTelemetry."));
    if(Force||Now>=NextHeavy){NextHeavy=Now+5;J->SetStringField(TEXT("actualCargoEvidence"),Director->ReadCargoInventoryEvidenceJson());J->SetStringField(TEXT("nativeTelemetry"),Robot->ReadTelemetryJson());J->SetStringField(TEXT("serviceCommitEvidence"),Director->GetLastServiceCommitEvidenceJson());}
    Samples.Add(MakeShared<FJsonValueObject>(J));
}
bool AMAYACityBlockMissionTrial::RequestCapture(const FString& Name)
{
    if(!PendingCapture.IsEmpty()||FScreenshotRequest::IsScreenshotRequested())return false;
    PendingCapture=FPaths::Combine(Output,Name+TEXT(".png"));CaptureFrame=GFrameCounter;CaptureWorldSeconds=GetWorld()->GetTimeSeconds();CaptureRealSeconds=FPlatformTime::Seconds();bCaptureProcessed=false;CaptureProcessedFrame=0;CaptureProcessedWorldSeconds=-1;
    CaptureSize=GEngine&&GEngine->GameViewport&&GEngine->GameViewport->Viewport?GEngine->GameViewport->Viewport->GetSizeXY():FIntPoint::ZeroValue;
    FScreenshotRequest::RequestScreenshot(PendingCapture,true,false);return true;
}
void AMAYACityBlockMissionTrial::ScreenshotProcessed()
{
    if(PendingCapture.IsEmpty())return;bCaptureProcessed=true;CaptureProcessedFrame=GFrameCounter;CaptureProcessedWorldSeconds=GetWorld()->GetTimeSeconds();
}
bool AMAYACityBlockMissionTrial::PollCapture()
{
    if(PendingCapture.IsEmpty())return true;
    if(FPlatformTime::Seconds()-CaptureRealSeconds>12){Finish(false,TEXT("Engine-processed screenshot exceeded12 real seconds"));return false;}
    if(!bCaptureProcessed||IFileManager::Get().FileSize(*PendingCapture)<=64)return true;
    TArray<uint8> Bytes;const bool Read=FFileHelper::LoadFileToArray(Bytes,*PendingCapture);
    const uint8 Signature[]={137,80,78,71,13,10,26,10};
    auto UInt=[&](int32 I){return(uint32(Bytes[I])<<24)|(uint32(Bytes[I+1])<<16)|(uint32(Bytes[I+2])<<8)|uint32(Bytes[I+3]);};
    if(!Check(Read&&Bytes.Num()>24&&FMemory::Memcmp(Bytes.GetData(),Signature,8)==0&&CaptureSize.X>0&&CaptureSize.Y>0
        &&UInt(16)==uint32(CaptureSize.X)&&UInt(20)==uint32(CaptureSize.Y)&&CaptureProcessedFrame>=CaptureFrame,
        TEXT("Actual engine-processed PNG signature, viewport dimensions and frames: ")+FPaths::GetBaseFilename(PendingCapture)))
    {Finish(false,TEXT("Actual screenshot evidence failed"));return false;}
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("path"),PendingCapture);J->SetNumberField(TEXT("requestFrame"),double(CaptureFrame));J->SetNumberField(TEXT("processedFrame"),double(CaptureProcessedFrame));J->SetNumberField(TEXT("requestWorldSeconds"),CaptureWorldSeconds);J->SetNumberField(TEXT("processedWorldSeconds"),CaptureProcessedWorldSeconds);J->SetNumberField(TEXT("fileObservedWorldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("width"),CaptureSize.X);J->SetNumberField(TEXT("height"),CaptureSize.Y);J->SetBoolField(TEXT("actualFilePresent"),true);
    if(FPaths::GetBaseFilename(PendingCapture)==TEXT("fourth-delivery-complete"))bFinalCaptureCompleted=true;
    Captures.Add(MakeShared<FJsonValueObject>(J));PendingCapture.Empty();return true;
}
void AMAYACityBlockMissionTrial::Finish(bool Success,const FString& Reason)
{
    if(bFinished)return;
    for(const auto& V:Checks)if(!V->AsObject()->GetBoolField(TEXT("passed")))Success=false;
    bFinished=true;Drive();SetActorTickEnabled(false);
    if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("MayaRobotics.NativeCityBlockMission.v1"));J->SetStringField(TEXT("phase"),TEXT("NativeCityBlockMission"));J->SetBoolField(TEXT("terminal"),true);J->SetBoolField(TEXT("success"),Success);J->SetStringField(TEXT("reason"),Reason);J->SetBoolField(TEXT("humanPhysicalInputVerified"),false);J->SetBoolField(TEXT("syntheticResourceWrites"),false);J->SetBoolField(TEXT("teleportsOrSignalWrites"),false);
    J->SetStringField(TEXT("routeId"),Definition.RouteId);J->SetStringField(TEXT("auditReceiptSha256"),Definition.SurveyReceiptSha256);J->SetStringField(TEXT("eastRampAuditReceiptSha256"),Definition.EastRampSurfaceAuditSha256);J->SetNumberField(TEXT("routePointCount"),Definition.Points.Num());J->SetNumberField(TEXT("pathLengthCm"),Path.Arc.IsEmpty()?0:Path.Arc.Last());J->SetNumberField(TEXT("maximumGameplaySeconds"),900);J->SetNumberField(TEXT("unchangedAuthoredJobDeadlineSeconds"),Definition.DeadlineSeconds);
    J->SetNumberField(TEXT("followerAlignmentEpisodes"),FollowerAlignmentEpisodes);
    J->SetNumberField(TEXT("authoredStopTurnCount"),Definition.StopTurns.Num());J->SetArrayField(TEXT("actualAuthoredStopTurnReceipts"),AuthoredStopReceipts);
    J->SetStringField(TEXT("approachSurfaceAuditSha256"),Definition.ApproachSurfaceAuditSha256);J->SetStringField(TEXT("crestSurfaceAuditSha256"),Definition.CrestSurfaceAuditSha256);
    J->SetStringField(TEXT("followerRollingPolicy"),TEXT("Diagnostic hybrid steering: same60cm lookahead and positiveT=clamp(.60*(1-abs(error)/100),.22,.60). Original rolling rawS=T*q applies until the first five authored stages complete. Only the finalQ1 transit (next stop index5 with six installed, after Q0 actual fullQuiet) uses the existing cruise ceilingTraw=.60 and existing heading/yaw-rate PD before unchanged common normalization, omitting angle and near-stop throttle reductions while retaining their finite observation guards. Other five stages keep the original angle/speed-cap laws. This is a prospective native-input authority test, not an identified traction or force-balance correction. Existing independent PD rawS=clamp(actualHeadingErrorDeg/35-.5*actualAngularZradps,-.65,.65) applies on that final B leg and after all six stages on the final connector. No new gain, tilt window, route point or maneuver radius is introduced. The exact selected mode is recorded with each command timestamp. The prospective damping coefficient is0.5 request/(rad/s), not an identified physical time constant. Positive rawyaw agrees with increasing optical yaw. Ideal optical-localk=2y/(x*x+y*y),q=measuredNativeCOMHalfTrack*k remain finite; originalabs(q)<1 admission applies only to the rolling branch. Reversed or stopped wheel-bank requests are explicitly permitted in the final B and post-stage PD branches and recorded after common normalization. Refuse behind/degenerate/nonfinite target or native yaw-rate braked; no synthetic turn/teleport fallback. Jointly normalizeT/S only when bank request would exceed1. Route, production motor/friction/physics, custody,150cm/130cmps, original30s progress and600s mission remain unchanged. Explicit source-audited stop-turns retain60cm incoming-tangent lookahead through the checkpoint, never anticipating the outgoing leg. Outside finalQ1 transit, after the angle-based throttle law capT linearly from.60 at80cm to the measured-speed feedback cap at30cm using max(nonnegative StopArc-currentProjectionArc, actual planar distance). Slow cap=clamp(.12+.012*(20-actualOpticalForwardCmps),.12,.36), retained below30cm with no.22 floor override. Finite actual signed optical speed adapts motor requests;20cmps is a diagnostic feedback target, not a velocity clamp or guaranteed speed. These bounded candidate gains are prospective, not optimality or identified traction claims. After the throttle cap, rolling recomputesS=T*q; selected PD computes its independent measured-heading/yaw-rate request. Both use the unchanged commonT/S normalization. No gain or physical force tuning is added. The other four stops retain25cm brake capture and30cm actual fullQuiet stopped arrival. Only the exact127-audited A crest P2 and141-audited final B checkpoint declare60cm maneuver arrival. A crest capture requires actual distance<=50cm and currentO.bUpright, with no additional plane condition. Final B capture retains actual incoming tangent along>=0, actual distance<=50cm and currentO.bUpright. Otherwise the selected follower continues under the unchanged missed-plane/150cm/30s/native guards. Both maneuver arrivals require actual fullQuiet stopped distance<=60cm;8degree upright, all-six support, stationary latch/physical locks and.5s dwell remain mandatory. The larger diagnostic maneuver radius is not a flat/support/climb-clearance or stopping-distance certificate; no objective/service-ring radius changes. This is diagnostic checkpoint capture in the graded native area, not objective/service-ring or wholly-flat footprint clearance. Missed-stop refusal uses actual signed incoming-tangent displacement past the point and actual distance, never global projection that can freeze at the bend. Reduced native wheel-speed requests are not a stopping-distance guarantee; actual brake/Quiet and existing FaceFirst anchor-feedback raw turn occur only there under declared per-stage excursion/15s bounds. Historical loaded yaw translation is retained, not assumed in-place. Actual heading response/slip convergence is native-only."));
    if(RollingGeometryCapture.IsValid())J->SetObjectField(TEXT("actualLoadedRollingGeometryCapture"),RollingGeometryCapture);
    if(LastAuthoredBrakeCapture.IsValid())J->SetObjectField(TEXT("lastAuthoredBrakeCapture"),LastAuthoredBrakeCapture);
    J->SetNumberField(TEXT("startedWorldSeconds"),Started);J->SetNumberField(TEXT("endWorldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("nativeDriveCalls"),DriveCalls);J->SetNumberField(TEXT("normalServiceRequests"),ServiceCalls);J->SetNumberField(TEXT("crossingEntranceMask"),CrossingEntranceMask);J->SetNumberField(TEXT("completedCrossingMask"),CompletedCrossingMask);J->SetBoolField(TEXT("telemetryTruncated"),Samples.Num()>=2000);
    J->SetArrayField(TEXT("checks"),Checks);J->SetArrayField(TEXT("samples"),Samples);J->SetArrayField(TEXT("actualProcessedCaptures"),Captures);J->SetArrayField(TEXT("crossingAdmissionReceipts"),CrossingReceipts);
    if(ArrowFirstGuardMiss.IsValid())J->SetObjectField(TEXT("firstArrowGuardMiss"),ArrowFirstGuardMiss);
    if(Robot.IsValid())J->SetStringField(TEXT("finalActualNativeTelemetry"),Robot->ReadTelemetryJson());
    if(Director.IsValid())J->SetStringField(TEXT("finalActualCargoEvidence"),Director->ReadCargoInventoryEvidenceJson());
    if(Director.IsValid()){J->SetStringField(TEXT("runtimeStatus"),Director->GetRuntimeStatus());J->SetStringField(TEXT("lastServiceFailure"),Director->GetLastServiceFailureDetail());J->SetStringField(TEXT("lastServiceCommitEvidence"),Director->GetLastServiceCommitEvidenceJson());J->SetNumberField(TEXT("actualCashDollars"),Director->Snapshot().TotalCredits);J->SetNumberField(TEXT("actualEnergyPercent"),Director->Snapshot().BatteryPercent);}
    FString Json;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&Json));FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Output,TEXT("result.json")));UE_LOG(LogTemp,Display,TEXT("MAYA CITY BLOCK MISSION %s: %s"),Success?TEXT("PASS"):TEXT("FAIL"),*Reason);FPlatformMisc::RequestExit(false,TEXT("MAYACityBlockMissionTrial"));
}
void AMAYACityBlockMissionTrial::EndPlay(EEndPlayReason::Type Reason)
{if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);if(Robot.IsValid())Robot->ReleaseDriveCommand();Super::EndPlay(Reason);}
