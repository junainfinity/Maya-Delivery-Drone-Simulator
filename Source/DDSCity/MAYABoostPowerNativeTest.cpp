#include "MAYABoostPowerNativeTest.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYADeliveryParcel.h"
#include "MAYADeliveryStation.h"
#include "MAYAV01CurbTrial.h"
#include "MAYAStableCameraArm.h"
#include "GameFramework/PlayerController.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "UnrealClient.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "MassAgentComponent.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassEntityQuery.h"
#include "MassCrowdFragments.h"
#include "MassTrafficFragments.h"
namespace MayaBoostTest
{
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{ return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)}; }
TSharedPtr<FJsonObject> Parse(const FString& Text)
{ TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),J); return J; }
double Num(const TSharedPtr<FJsonObject>& J,const TCHAR* Key)
{ double N = 0; if(J)J->TryGetNumberField(Key,N);return N; }
FString Word(const TSharedPtr<FJsonObject>& J,const TCHAR* Key)
{ FString S;if(J)J->TryGetStringField(Key,S);return S; }
}
AMAYABoostPowerNativeTest::AMAYABoostPowerNativeTest()
{ PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PostUpdateWork; }
void AMAYABoostPowerNativeTest::Configure(AMAYANativeDemoPawn* P,AMAYADeliveryDirector* D,const TArray<FVector>& R)
{ Robot=P;Director=D;Route=R;FlatCourseOrigin=R.Num()>0?R[0]:FVector::ZeroVector; }
bool AMAYABoostPowerNativeTest::Check(bool Passed,const FString& Name)
{ auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),Name);J->SetBoolField(TEXT("passed"),Passed);Checks.Add(MakeShared<FJsonValueObject>(J));return Passed; }
bool AMAYABoostPowerNativeTest::VerifySpawnDensity()
{
    const IConsoleVariable* CVar=IConsoleManager::Get().FindConsoleVariable(TEXT("ai.mass.scalability.SpawnDensityMultiplier"));
    const double Density=CVar?double(CVar->GetFloat()):0;
    const bool Finite=CVar&&FMath::IsFinite(Density);
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("step"),Step);J->SetNumberField(TEXT("pass"),Pass);
    J->SetBoolField(TEXT("registeredCVarFound"),CVar!=nullptr);J->SetBoolField(TEXT("finiteEffectiveValue"),Finite);
    if(Finite)J->SetNumberField(TEXT("effectiveSpawnDensityMultiplier"),Density);else J->SetField(TEXT("effectiveSpawnDensityMultiplier"),MakeShared<FJsonValueNull>());
    if(CVar){const uint32 Flags=uint32(CVar->GetFlags());J->SetNumberField(TEXT("actualConsoleVariableFlags"),double(Flags));J->SetNumberField(TEXT("actualSetByMask"),double(Flags&ECVF_SetByMask));J->SetStringField(TEXT("actualSetBySource"),GetConsoleVariableSetByName(EConsoleVariableFlags(Flags&ECVF_SetByMask)));}
    const bool Accepted=Finite&&(bStaticBenchmark?Density==0:Density>0);J->SetBoolField(TEXT("selectedScopeDensityAccepted"),Accepted);LastSpawnDensityEvidence=J;
    return Accepted;
}
void AMAYABoostPowerNativeTest::ObserveAmbientPopulation(const FString& Point,bool bInventory)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("observationPoint"),Point);J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("step"),Step);J->SetNumberField(TEXT("pass"),Pass);
    if(LastSpawnDensityEvidence)J->SetObjectField(TEXT("actualSpawnDensityReadback"),LastSpawnDensityEvidence);
    UClass* CrowdClass=FindObject<UClass>(nullptr,TEXT("/Script/CitySample.CitySampleCrowdCharacter"));
    const bool CrowdClassKnown=CrowdClass&&CrowdClass->IsChildOf(AActor::StaticClass());J->SetBoolField(TEXT("actualNativeCrowdClassFound"),CrowdClassKnown);
    J->SetStringField(TEXT("requestedNativeCrowdClassPath"),TEXT("/Script/CitySample.CitySampleCrowdCharacter"));
    if(CrowdClassKnown)
    {
        J->SetStringField(TEXT("actualResolvedNativeCrowdClassPath"),CrowdClass->GetPathName());
        int32 Crowd=0,ActiveCollision=0,Hidden=0,ActorTick=0,PuppetPaused=0;
        for(TActorIterator<AActor> It(GetWorld());It;++It)
        {auto* A=*It;if(!IsValid(A)||!A->IsA(CrowdClass))continue;++Crowd;if(A->GetActorEnableCollision()&&!A->IsHidden())++ActiveCollision;if(A->IsHidden())++Hidden;if(A->IsActorTickEnabled())++ActorTick;
            if(auto* Agent=A->FindComponentByClass<UMassAgentComponent>())if(Agent->IsPuppetPaused())++PuppetPaused;}
        J->SetNumberField(TEXT("actualCitySampleCrowdCharacterActors"),Crowd);J->SetNumberField(TEXT("visibleCollisionEnabledCrowdActors"),ActiveCollision);J->SetNumberField(TEXT("hiddenCrowdActors"),Hidden);J->SetNumberField(TEXT("actorTickEnabledCrowdActors"),ActorTick);J->SetNumberField(TEXT("puppetPausedCrowdActors"),PuppetPaused);
    }
    else
    {
        J->SetStringField(TEXT("crowdActorInventoryUnavailableReason"),TEXT("Exact registered native CitySample crowd UClass unavailable or not an actor class; no zero/absence inference"));
        for(const TCHAR* Name:{TEXT("actualCitySampleCrowdCharacterActors"),TEXT("visibleCollisionEnabledCrowdActors"),TEXT("hiddenCrowdActors"),TEXT("actorTickEnabledCrowdActors"),TEXT("puppetPausedCrowdActors")})J->SetField(Name,MakeShared<FJsonValueNull>());
    }
    J->SetStringField(TEXT("crowdActorCountScope"),TEXT("Native CitySampleCrowdCharacter class/subclasses in this world. Visible collision-enabled actors are distinguished from hidden/paused representations; hidden or paused alone is not proof of pool membership or universal absence."));
    if(bInventory)
    {
        auto* Subsystem=GetWorld()->GetSubsystem<UMassEntitySubsystem>();J->SetBoolField(TEXT("massEntitySubsystemFound"),Subsystem!=nullptr);
        if(Subsystem)
        {
            auto& Manager=Subsystem->GetMutableEntityManager();const bool Available=!Manager.IsProcessing();J->SetBoolField(TEXT("massInventoryReadAvailable"),Available);
            if(Available)
            {
                auto CountTag=[&](const UScriptStruct* Tag,const TCHAR* Name)
                {FMassEntityQuery Query;Query.AddTagRequirement(*Tag,EMassFragmentPresence::All);J->SetNumberField(Name,Query.GetNumMatchingEntities(Manager));};
                CountTag(FMassCrowdTag::StaticStruct(),TEXT("actualMassCrowdTaggedEntities"));CountTag(FMassTrafficVehicleTag::StaticStruct(),TEXT("actualMassTrafficVehicleTaggedEntities"));CountTag(FMassTrafficParkedVehicleTag::StaticStruct(),TEXT("actualMassParkedVehicleTaggedEntities"));CountTag(FMassTrafficIntersectionTag::StaticStruct(),TEXT("actualMassIntersectionTaggedEntities"));
            }
            else J->SetStringField(TEXT("massInventoryUnavailableReason"),TEXT("Mass entity manager is actively processing; no count read or inferred zero"));
        }
        J->SetStringField(TEXT("massInventoryScope"),TEXT("Read-only current Mass tag counts, including non-actor/ISM representations; tags can overlap. Global requested spawn density can affect crowd and moving traffic, parked generation may replace Count, intersections are independent. No all-ambient-absent or unchanged parked population claim."));
    }
    ++AmbientPopulationSamplesSeen;if(AmbientPopulationObservations.Num()<2048)AmbientPopulationObservations.Add(MakeShared<FJsonValueObject>(J));else bAmbientPopulationTraceTruncated=true;
    LastPopulationWorld=GetWorld()->GetTimeSeconds();
}
bool AMAYABoostPowerNativeTest::Drive(double T,double S,bool Brake,bool Boost,double LeaseSeconds)
{ if(Brake){FlatNearIntegral=0;FlatNearUpdateWorld=-1;}const bool Accepted=Robot&&Robot->SubmitDriveCommand(float(T),float(S),Brake,EMAYADriveCommandOrigin::NativeAutomation,float(LeaseSeconds),Boost);
  if(Accepted){LastRequestedThrottle=double(float(T));LastRequestedSteer=double(float(S));LastDriveWorld=GetWorld()->GetTimeSeconds();LastDriveReal=FPlatformTime::Seconds();LastDriveLeaseSeconds=double(float(LeaseSeconds));}
  if(!Accepted){FlatNearIntegral=0;FlatNearUpdateWorld=-1;if(!bFinished)Finish(false,TEXT("Actual leased motor command refused"));}return Accepted; }
bool AMAYABoostPowerNativeTest::Quiet(const FMAYADeliveryRobotObservation& O) const
{ return O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&Robot&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked(); }
void AMAYABoostPowerNativeTest::Enter(int32 Next,const FVector& Goal)
{
    Step=Next;StepWorld=GetWorld()->GetTimeSeconds();StableWorld=-1;Target=Goal;PositionMode=0;PositionModeWorld=StepWorld;bRequested=false;
    UE_LOG(LogTemp,Display,TEXT("MAYA BOOST phase step=%d pass=%d world=%.3f target=(%.2f,%.2f)"),Step,Pass,StepWorld,Target.X,Target.Y);
}
void AMAYABoostPowerNativeTest::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    SetActorTickEnabled(false);return;
#else
    if(!FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest"))
        ||!FParse::Value(FCommandLine::Get(),TEXT("MayaV01Output="),Output)||Output.IsEmpty())
    {SetActorTickEnabled(false);return;}
    Output=FPaths::ConvertRelativePathToFull(Output);StartedWorld=StepWorld=GetWorld()->GetTimeSeconds();StartedReal=FPlatformTime::Seconds();
    if(IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("result.json"))))
    {bFinished=true;SetActorTickEnabled(false);FPlatformMisc::RequestExit(false,TEXT("MAYABoostPowerNativeTest"));return;}
    bCurbOnly=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostCurbOnly"));bStaticBenchmark=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostStaticBenchmark"));bServiceOnly=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostServiceOnly"));
    const bool Legacy=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostLegacyComparison"));const int32 Modes=int32(bCurbOnly)+int32(bStaticBenchmark)+int32(bServiceOnly);
    const FString CommandLine=FCommandLine::Get();const bool DensityArgument=CommandLine.Contains(TEXT("ai.mass.scalability.SpawnDensityMultiplier"),ESearchCase::IgnoreCase);
    const bool OrdinaryAmbientOverride=DensityArgument||CommandLine.Contains(TEXT("MassTraffic.NumTrafficVehiclesScale"),ESearchCase::IgnoreCase)||CommandLine.Contains(TEXT("MassTraffic.NumParkedVehiclesScale"),ESearchCase::IgnoreCase)||CommandLine.Contains(TEXT("ai.debug.mass.FreezeMovement"),ESearchCase::IgnoreCase);
    if(!Check(Modes<=1&&((bCurbOnly||bServiceOnly)?!Legacy:Legacy),TEXT("Exclusive BoostPower full/curb-only/static-benchmark/service-only selector and legacy policy"))
        ||!Check(!(bCurbOnly||bServiceOnly)||!OrdinaryAmbientOverride,TEXT("Ordinary-city split mode has no ambient spawn-density/motion override"))
        ||!Check(!bStaticBenchmark||DensityArgument,TEXT("Static benchmark explicitly requests early supported ambient spawn density control")))
    {Finish(false,TEXT("Boost diagnostic scope initialization refused"));return;}
    if((bCurbOnly||bStaticBenchmark||bServiceOnly)&&!Check(VerifySpawnDensity(),TEXT("Actual registered effective density matches selected ordinary-positive/static-exact-zero scope")))
    {Finish(false,TEXT("Actual ambient startup density refused"));return;}
    ActualSettingsPath=FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"),nullptr,*FPaths::GeneratedConfigDir()));FPaths::NormalizeFilename(ActualSettingsPath);
    FString Expected=FPaths::Combine(Output,TEXT("GameUserSettings.ini"));FPaths::NormalizeFilename(Expected);
    if(!Check(FApp::CanEverRender()&&Robot&&Director&&Route.Num()>=29&&GetWorld()->GetMapName().Contains(TEXT("Small_City"))
        &&Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()),TEXT("Exclusive rendered original Small City/native robot and measured route"))
        ||!Check(GConfig&&GConfig->FindConfigFile(GGameUserSettingsIni)&&ActualSettingsPath.Equals(Expected,ESearchCase::IgnoreCase),TEXT("Actual isolated settings destination")))
    {Finish(false,TEXT("Native boost initialization refused"));return;}
    Axis=(Route[12]-Route[0]).GetSafeNormal2D();Left=FVector::CrossProduct(FVector::UpVector,Axis);
    if(Axis.ContainsNaN()||Axis.IsNearlyZero()){Finish(false,TEXT("Measured route axis invalid"));return;}
    ScreenshotHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this,&AMAYABoostPowerNativeTest::ScreenshotProcessed);
    if(bStaticBenchmark)ObserveAmbientPopulation(TEXT("diagnostic_initialization"),true);
#endif
}
bool AMAYABoostPowerNativeTest::Custody(int32 Count,bool Bind)
{
    if(!Director||Director->GetParcelCount()!=Count||Director->GetSecuredParcelCount()!=Count)return false;
    for(int32 I=0;I<Count;++I)
    {
        const auto* P=Director->GetParcel(I);if(!P||!P->GetParcelBody())return false;
        if(Bind){ParcelIds[I]=P->GetParcelId();ParcelActors[I]=P->GetPathName();ParcelBodies[I]=P->GetParcelBody()->GetPathName();}
        else if(P->GetParcelId()!=ParcelIds[I]||P->GetPathName()!=ParcelActors[I]||P->GetParcelBody()->GetPathName()!=ParcelBodies[I])return false;
    }
    return true;
}
bool AMAYABoostPowerNativeTest::ReadFlatBounds(const FMAYADeliveryRobotObservation& O,int32 ParcelCount,bool Capture,TSharedPtr<FJsonObject>& J,FBox& Union)
{
    J=MakeShared<FJsonObject>();Union=FBox(ForceInit);int32 RobotCount=0,ValidParcelCount=0;bool Known=true,Inside=true,CargoReference=true;FString Offending;
    J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("pass"),Pass);J->SetNumberField(TEXT("runMode"),RunMode);
    J->SetArrayField(TEXT("chassisPositionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("flatCourseOriginCm"),MayaBoostTest::Vec(FlatCourseOrigin));J->SetArrayField(TEXT("routeAxis"),MayaBoostTest::Vec(Axis));J->SetArrayField(TEXT("routeAcrossAxis"),MayaBoostTest::Vec(Left));
    const double Yaw=FMath::FindDeltaAngleDegrees(Axis.Rotation().Yaw,(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw),R=FMath::DegreesToRadians(Yaw),Co=FMath::Cos(R),Si=FMath::Sin(R);
    J->SetNumberField(TEXT("opticalYawRelativeToRouteDegrees"),Yaw);TArray<TSharedPtr<FJsonValue>> Rows;
    auto Body=[&](UPrimitiveComponent* C,bool Parcel)
    {
        auto Row=MakeShared<FJsonObject>();const FString Path=GetPathNameSafe(C);Row->SetStringField(TEXT("bodyComponent"),Path);Row->SetBoolField(TEXT("actualParcelBody"),Parcel);
        auto* B=C?C->GetBodyInstance(NAME_None,false):nullptr;
        const bool Native=C&&C->IsPhysicsCollisionEnabled()&&B&&B==C->GetBodyInstance()&&B->IsValidBodyInstance();Row->SetBoolField(TEXT("nativeIndependentCollisionBodyKnown"),Native);
        if(!Native){Known=false;if(Offending.IsEmpty())Offending=Path;Rows.Add(MakeShared<FJsonValueObject>(Row));return;}
        const FBox Bounds=B->GetBodyBounds();const bool Finite=Bounds.IsValid&&!Bounds.Min.ContainsNaN()&&!Bounds.Max.ContainsNaN();Row->SetBoolField(TEXT("finiteWorldBounds"),Finite);
        if(!Finite){Known=false;if(Offending.IsEmpty())Offending=Path;Rows.Add(MakeShared<FJsonValueObject>(Row));return;}
        if(Parcel)++ValidParcelCount;Row->SetArrayField(TEXT("worldMinCm"),MayaBoostTest::Vec(Bounds.Min));Row->SetArrayField(TEXT("worldMaxCm"),MayaBoostTest::Vec(Bounds.Max));Union+=Bounds;
        if(!Parcel){++RobotCount;if(Capture)FlatBodyIdentities.Add(Path);else if(!FlatBodyIdentities.Contains(Path)){Known=false;if(Offending.IsEmpty())Offending=Path;}}
        double AMin=1.e30,AMax=-1.e30,CMin=1.e30,CMax=-1.e30,LocalA=0,LocalC=0;
        for(double X:{Bounds.Min.X,Bounds.Max.X})for(double Y:{Bounds.Min.Y,Bounds.Max.Y})
        {
            const FVector Corner(X,Y,O.ChassisPose.GetLocation().Z);const FVector Global=Corner-FlatCourseOrigin,Relative=Corner-O.ChassisPose.GetLocation();
            const double A=FVector::DotProduct(Global,Axis),Cros=FVector::DotProduct(Global,Left),RA=FVector::DotProduct(Relative,Axis),RC=FVector::DotProduct(Relative,Left);
            AMin=FMath::Min(AMin,A);AMax=FMath::Max(AMax,A);CMin=FMath::Min(CMin,Cros);CMax=FMath::Max(CMax,Cros);
            LocalA=FMath::Max(LocalA,FMath::Abs(Co*RA+Si*RC));LocalC=FMath::Max(LocalC,FMath::Abs(-Si*RA+Co*RC));
        }
        Row->SetNumberField(TEXT("routeAlongMinCm"),AMin);Row->SetNumberField(TEXT("routeAlongMaxCm"),AMax);Row->SetNumberField(TEXT("routeAcrossMinCm"),CMin);Row->SetNumberField(TEXT("routeAcrossMaxCm"),CMax);
        Row->SetNumberField(TEXT("referenceYawRemovedAlongHalfCm"),LocalA);Row->SetNumberField(TEXT("referenceYawRemovedAcrossHalfCm"),LocalC);
        const bool Within=Capture||(AMin>=FlatAlongMin&&AMax<=FlatAlongMax&&CMin>=-FlatHalfWidth&&CMax<=FlatHalfWidth&&Bounds.Min.Z>=FlatBodyMinZ&&Bounds.Max.Z<=FlatBodyMaxZ);
        Row->SetBoolField(TEXT("bodyContainmentEvaluated"),!Capture);if(!Capture)Row->SetBoolField(TEXT("insideSurveyedBodyContainmentBand"),Within);Inside=Inside&&Within;if(!Within&&Offending.IsEmpty())Offending=Path;
        if(Capture){FlatReferenceAlongHalf=FMath::Max(FlatReferenceAlongHalf,LocalA);FlatReferenceAcrossHalf=FMath::Max(FlatReferenceAcrossHalf,LocalC);}
        if(Parcel){const bool Contained=LocalA<=FlatReferenceAlongHalf&&LocalC<=FlatReferenceAcrossHalf&&Bounds.Min.Z>=FlatBodyMinZ&&Bounds.Max.Z<=FlatBodyMaxZ;Row->SetBoolField(TEXT("parcelInsideReferenceRobotEnvelopeAndVerticalReserve"),Contained);CargoReference=CargoReference&&Contained;}
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    };
    TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);
    for(UPrimitiveComponent* C:Components){if(!C||!C->IsPhysicsCollisionEnabled())continue;auto* B=C->GetBodyInstance();if(!B||B!=C->GetBodyInstance(NAME_None,false)||!B->IsValidBodyInstance())continue;Body(C,false);}
    for(int32 I=0;I<ParcelCount;++I){const auto* P=Director->GetParcel(I);Body(P?P->GetParcelBody():nullptr,true);}
    Known=Known&&RobotCount==9&&FlatBodyIdentities.Num()==9&&ValidParcelCount==ParcelCount&&Union.IsValid;
    J->SetArrayField(TEXT("actualIndependentBodies"),Rows);J->SetNumberField(TEXT("nativeRobotBodyCount"),RobotCount);J->SetNumberField(TEXT("expectedParcelBodyCount"),ParcelCount);J->SetNumberField(TEXT("actualParcelBodyCount"),ValidParcelCount);J->SetBoolField(TEXT("allNativeBoundsKnown"),Known);J->SetBoolField(TEXT("bodyContainmentEvaluated"),!Capture);if(!Capture)J->SetBoolField(TEXT("allBoundsInsideSurveyedBand"),Inside);J->SetBoolField(TEXT("securedCargoInsideReferenceRobotEnvelope"),CargoReference);J->SetStringField(TEXT("offendingBody"),Offending);
    if(Union.IsValid){J->SetArrayField(TEXT("currentUnionMinCm"),MayaBoostTest::Vec(Union.Min));J->SetArrayField(TEXT("currentUnionMaxCm"),MayaBoostTest::Vec(Union.Max));}
    J->SetStringField(TEXT("scope"),TEXT("Current native independent collision-body world AABB corners projected into the admitted route coordinates; conservative game-thread bounds, not continuous solver sweeps. Cosmetic/welded children excluded. Parcel bodies read physically, not inferred from IDs."));
    return Known&&Inside;
}
bool AMAYABoostPowerNativeTest::FlatRaisedCorridorClear(TSharedPtr<FJsonObject> Evidence) const
{
    const FVector Center=FlatCourseOrigin+Axis*((FlatAlongMin+FlatAlongMax)*.5)+FVector(0,0,(FlatObstacleMinZ+FlatObstacleMaxZ)*.5-FlatCourseOrigin.Z);
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYABoostFlatVolume),false);Q.AddIgnoredActor(this);Q.AddIgnoredActor(Robot);
    for(int32 I=0;I<Director->GetParcelCount();++I)if(const auto* P=Director->GetParcel(I))Q.AddIgnoredActor(P);
    TArray<FOverlapResult> Hits;GetWorld()->OverlapMultiByChannel(Hits,Center,Axis.Rotation().Quaternion(),ECC_PhysicsBody,FCollisionShape::MakeBox(FVector((FlatAlongMax-FlatAlongMin)*.5,FlatHalfWidth,(FlatObstacleMaxZ-FlatObstacleMinZ)*.5)),Q);
    TArray<TSharedPtr<FJsonValue>> Blockers;for(const auto& H:Hits)
    {
        auto* C=H.GetComponent();if(C&&C->IsPhysicsCollisionEnabled()&&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block)
        {
            auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("component"),C->GetPathName());J->SetStringField(TEXT("actor"),GetPathNameSafe(H.GetActor()));
            J->SetNumberField(TEXT("observationWorldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("observationRealSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("observationFrame"),double(GFrameCounter));
            const FVector Position=C->GetComponentLocation(),Velocity=C->GetComponentVelocity();if(!Position.ContainsNaN())J->SetArrayField(TEXT("componentPositionCm"),MayaBoostTest::Vec(Position));if(!Velocity.ContainsNaN())J->SetArrayField(TEXT("componentReportedVelocityCmps"),MayaBoostTest::Vec(Velocity));
            if(const auto* A=H.GetActor()){const FVector AP=A->GetActorLocation(),AV=A->GetVelocity();if(!AP.ContainsNaN())J->SetArrayField(TEXT("actorPositionCm"),MayaBoostTest::Vec(AP));if(!AV.ContainsNaN())J->SetArrayField(TEXT("actorReportedVelocityCmps"),MayaBoostTest::Vec(AV));}
            J->SetNumberField(TEXT("observedOverlapItemIndex"),H.ItemIndex);auto* ISM=Cast<UInstancedStaticMeshComponent>(C);const bool InstanceValid=!ISM||ISM->IsValidInstance(H.ItemIndex);J->SetBoolField(TEXT("overlapComponentIsInstancedStaticMesh"),ISM!=nullptr);if(ISM)J->SetBoolField(TEXT("actualInstanceIndexValidForNativeReadback"),InstanceValid);
            auto* B=InstanceValid?C->GetBodyInstance(NAME_None,true,H.ItemIndex):nullptr;const bool Native=B&&B->IsValidBodyInstance();J->SetBoolField(TEXT("nativeBlockerBodyKnown"),Native);J->SetStringField(TEXT("nativeBodyReadbackSelectionScope"),TEXT("Component-selected NAME_None native body; instanced mesh index must be valid. Exact overlapped body of a non-instanced multi-body component is not asserted."));
            if(Native){const FBox Bounds=B->GetBodyBounds();const FVector BV=B->GetUnrealWorldVelocity();if(Bounds.IsValid&&!Bounds.Min.ContainsNaN()&&!Bounds.Max.ContainsNaN()){J->SetArrayField(TEXT("nativeBodyWorldMinCm"),MayaBoostTest::Vec(Bounds.Min));J->SetArrayField(TEXT("nativeBodyWorldMaxCm"),MayaBoostTest::Vec(Bounds.Max));}if(!BV.ContainsNaN())J->SetArrayField(TEXT("nativeBodyLinearVelocityCmps"),MayaBoostTest::Vec(BV));}
            J->SetStringField(TEXT("motionEvidenceScope"),TEXT("Separately sampled component/actor reported motion and native body bounds/velocity when known; diagnostic readback, not a crossing prediction or an added admission threshold."));Blockers.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    Evidence->SetArrayField(TEXT("raisedCorridorObservedBlockers"),Blockers);Evidence->SetBoolField(TEXT("raisedCorridorCurrentlyClear"),Blockers.Num()==0);
    Evidence->SetStringField(TEXT("raisedQueryScope"),TEXT("PhysicsBody blocking overlap, all mobilities, excludes only this diagnostic and actual owned robot/parcel actors. Vertical band starts Ground+4, separately from support-contact and native body containment bands. Instant query, not a promise of future city clearance."));return Blockers.Num()==0;
}
bool AMAYABoostPowerNativeTest::SurveyFlat(const FMAYADeliveryRobotObservation& O)
{
    FlatReferenceAlongHalf=FlatReferenceAcrossHalf=0;FlatBodyIdentities.Reset();FBox Assembly(ForceInit);TSharedPtr<FJsonObject> Reference;
    if(!ReadFlatBounds(O,0,true,Reference,Assembly)){Survey.Add(MakeShared<FJsonValueObject>(Reference));return false;}
    const double Theta=FMath::DegreesToRadians(17.);
    auto Reach=[&](double A,double B){const double Critical=FMath::Atan2(B,A);return Critical<=Theta?FMath::Sqrt(A*A+B*B):FMath::Max(A,A*FMath::Cos(Theta)+B*FMath::Sin(Theta));};
    const double AlongReach=Reach(FlatReferenceAlongHalf,FlatReferenceAcrossHalf),AcrossReach=Reach(FlatReferenceAcrossHalf,FlatReferenceAlongHalf);
    FlatHalfWidth=FMath::CeilToDouble(27+AcrossReach+5);FlatAlongMin=FMath::FloorToDouble((293-AlongReach-5)/25.)*25;FlatAlongMax=FMath::CeilToDouble((1507+AlongReach+5)/25.)*25;
    FlatBodyMinZ=Assembly.Min.Z-10;FlatBodyMaxZ=Assembly.Max.Z+10;FlatObstacleMinZ=Route[0].Z+4;FlatObstacleMaxZ=FlatBodyMaxZ+5;
    FlatSurveyEnvelope=MakeShared<FJsonObject>();FlatSurveyEnvelope->SetBoolField(TEXT("freshSurveyAdmitted"),false);FlatSurveyEnvelope->SetObjectField(TEXT("actualReferenceBounds"),Reference);FlatSurveyEnvelope->SetNumberField(TEXT("absoluteYawCoveredDegrees"),17);FlatSurveyEnvelope->SetNumberField(TEXT("referenceAlongHalfCm"),FlatReferenceAlongHalf);FlatSurveyEnvelope->SetNumberField(TEXT("referenceAcrossHalfCm"),FlatReferenceAcrossHalf);FlatSurveyEnvelope->SetNumberField(TEXT("maxRotatedAlongReachCm"),AlongReach);FlatSurveyEnvelope->SetNumberField(TEXT("maxRotatedAcrossReachCm"),AcrossReach);
    FlatSurveyEnvelope->SetNumberField(TEXT("centerAlongMinCm"),293);FlatSurveyEnvelope->SetNumberField(TEXT("centerAlongMaxCm"),1507);FlatSurveyEnvelope->SetNumberField(TEXT("centerAcrossHalfBandCm"),27);FlatSurveyEnvelope->SetNumberField(TEXT("exteriorMarginCm"),5);FlatSurveyEnvelope->SetNumberField(TEXT("surveyAlongMinCm"),FlatAlongMin);FlatSurveyEnvelope->SetNumberField(TEXT("surveyAlongMaxCm"),FlatAlongMax);FlatSurveyEnvelope->SetNumberField(TEXT("surveyHalfWidthCm"),FlatHalfWidth);FlatSurveyEnvelope->SetNumberField(TEXT("runtimeBodyMinZCm"),FlatBodyMinZ);FlatSurveyEnvelope->SetNumberField(TEXT("runtimeBodyMaxZCm"),FlatBodyMaxZ);FlatSurveyEnvelope->SetNumberField(TEXT("raisedObstacleMinZCm"),FlatObstacleMinZ);FlatSurveyEnvelope->SetNumberField(TEXT("raisedObstacleMaxZCm"),FlatObstacleMaxZ);FlatSurveyEnvelope->SetNumberField(TEXT("supportGroundZCm"),Route[0].Z);
    FlatSurveyEnvelope->SetStringField(TEXT("derivation"),TEXT("Captured conservative body rectangle removes reference yaw and includes actual asymmetry via maximum absolute corner reach. Analytic support-function extrema cover complete +/-17degree interval (initial2+stop15). Both-sign centers293..1507 and +/-27 include7cm start/pair reserve and20cm lateral allowance; exterior5cm margin and outward25cm along rounding. Body vertical band is actual captured union +/-10cm; raised obstacle band separately Ground+4..bodyMax+5. Cargo native bounds must fit the reference robot footprint/vertical reserve before reuse."));Survey.Add(MakeShared<FJsonValueObject>(FlatSurveyEnvelope));
    FlatSurveyEnvelope->SetStringField(TEXT("recordPurpose"),TEXT("Immutable native shape capture only; no offset0 site queried in this run"));
    const auto ReferenceEnvelope=FlatSurveyEnvelope;
    for(double Offset:{15.,30.})
    {
        FlatCourseOrigin=Route[0]+Left*Offset;FlatSupports.Reset();
        FlatSurveyEnvelope=MakeShared<FJsonObject>(*ReferenceEnvelope);FlatSurveyEnvelope->SetStringField(TEXT("recordPurpose"),TEXT("Fixed-offset whole-corridor first-hit survey"));FlatSurveyEnvelope->SetBoolField(TEXT("freshSurveyAdmitted"),false);FlatSurveyEnvelope->SetArrayField(TEXT("flatCourseOriginCm"),MayaBoostTest::Vec(FlatCourseOrigin));FlatSurveyEnvelope->SetNumberField(TEXT("candidateLateralOffsetCm"),Offset);
        const int32 StartRow=Survey.Num();const bool Admitted=SurveyFlatSite();
        auto Attempt=MakeShared<FJsonObject>();Attempt->SetNumberField(TEXT("fixedCandidateLateralOffsetCm"),Offset);Attempt->SetArrayField(TEXT("candidateOriginCm"),MayaBoostTest::Vec(FlatCourseOrigin));Attempt->SetBoolField(TEXT("wholeSameSizeCorridorAdmitted"),Admitted);Attempt->SetNumberField(TEXT("firstRawSurveyRowIndex"),StartRow);Attempt->SetNumberField(TEXT("rawSurveyRowsRetained"),Survey.Num()-StartRow);Attempt->SetNumberField(TEXT("originalSurfaceIdentitiesObserved"),FlatSupports.Num());Attempt->SetObjectField(TEXT("candidateEnvelopeAndFailure"),FlatSurveyEnvelope);FlatSiteAttempts.Add(MakeShared<FJsonValueObject>(Attempt));
        UE_LOG(LogTemp,Display,TEXT("MAYA BOOST fixed flat site offset=%.0f admitted=%d world=%.3f"),Offset,Admitted?1:0,GetWorld()->GetTimeSeconds());
        if(Admitted){bFlatSiteAdmitted=true;FlatSelectedLateralOffset=Offset;return true;}
    }
    return false;
}
bool AMAYABoostPowerNativeTest::SurveyFlatSite()
{
    Survey.Add(MakeShared<FJsonValueObject>(FlatSurveyEnvelope));
    const int32 AcrossIntervals=FMath::CeilToInt(2*FlatHalfWidth/25.);
    for(double Along=FlatAlongMin;Along<=FlatAlongMax;Along+=25)for(int32 Column=0;Column<=AcrossIntervals;++Column)
    {
        const double Across=FMath::Lerp(-FlatHalfWidth,FlatHalfWidth,double(Column)/AcrossIntervals);const FVector P=FlatCourseOrigin+Axis*Along+Left*Across;FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYABoostFlatSurface),false);Q.AddIgnoredActor(this);Q.AddIgnoredActor(Robot);
        for(int32 I=0;I<Director->GetParcelCount();++I)if(const auto* Parcel=Director->GetParcel(I))Q.AddIgnoredActor(Parcel);
        FHitResult H;const bool Hit=GetWorld()->LineTraceSingleByChannel(H,P+FVector(0,0,160),P-FVector(0,0,150),ECC_Visibility,Q);
        auto* C=H.GetComponent();auto* M=Cast<UStaticMeshComponent>(C);auto* ISM=Cast<UInstancedStaticMeshComponent>(C);const bool Instance=!ISM||ISM->IsValidInstance(H.Item);auto* B=C&&Instance?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
        const FString Mesh=M&&M->GetStaticMesh()?M->GetStaticMesh()->GetPathName():FString();const bool Original=Mesh.StartsWith(TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Sidewalk_"))||Mesh.StartsWith(TEXT("/Game/Road/Kit_City_Road/SM_ROAD_"));
        const bool Valid=Hit&&H.bBlockingHit&&!H.bStartPenetrating&&!H.ImpactPoint.ContainsNaN()&&!H.ImpactNormal.ContainsNaN()&&Original&&C&&C->Mobility==EComponentMobility::Static&&C->IsPhysicsCollisionEnabled()&&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block&&Instance&&B&&B->IsValidBodyInstance()&&H.ImpactNormal.Z>=.99&&FMath::Abs(H.ImpactPoint.Z-FlatCourseOrigin.Z)<=2;
        auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());J->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetArrayField(TEXT("flatCourseOriginCm"),MayaBoostTest::Vec(FlatCourseOrigin));J->SetArrayField(TEXT("traceStartCm"),MayaBoostTest::Vec(P+FVector(0,0,160)));J->SetArrayField(TEXT("traceEndCm"),MayaBoostTest::Vec(P-FVector(0,0,150)));J->SetBoolField(TEXT("actualBlockingHit"),Hit&&H.bBlockingHit);J->SetBoolField(TEXT("actualStartPenetrating"),H.bStartPenetrating);J->SetNumberField(TEXT("actualHitTime"),H.Time);J->SetStringField(TEXT("hitTimeScope"),TEXT("Native normalized segment hit fraction, not elapsed seconds"));J->SetNumberField(TEXT("actualHitDistanceCm"),H.Distance);J->SetArrayField(TEXT("xyCm"),MayaBoostTest::Vec(P));J->SetArrayField(TEXT("actualHitCm"),MayaBoostTest::Vec(H.ImpactPoint));J->SetArrayField(TEXT("normal"),MayaBoostTest::Vec(H.ImpactNormal));J->SetStringField(TEXT("component"),GetPathNameSafe(C));J->SetStringField(TEXT("mesh"),Mesh);J->SetNumberField(TEXT("instance"),H.Item);J->SetBoolField(TEXT("physicalOriginalFlat"),Valid);Survey.Add(MakeShared<FJsonValueObject>(J));if(!Valid){FlatSurveyEnvelope->SetStringField(TEXT("candidateFailureReason"),TEXT("First actual blocking ray hit does not satisfy unchanged original static flat-surface admission"));FlatSurveyEnvelope->SetObjectField(TEXT("failedFirstHit"),J);return false;}FlatSupports.Add(C->GetPathName());
    }
    const bool Clear=FlatRaisedCorridorClear(FlatSurveyEnvelope);FlatSurveyEnvelope->SetBoolField(TEXT("freshSurveyAdmitted"),Clear);if(!Clear)FlatSurveyEnvelope->SetStringField(TEXT("candidateFailureReason"),TEXT("Unchanged full raised body corridor contains an observed blocker"));UE_LOG(LogTemp,Display,TEXT("MAYA BOOST full envelope surveyed along=%.1f..%.1f halfwidth=%.1f bodyZ=%.2f..%.2f clear=%d"),FlatAlongMin,FlatAlongMax,FlatHalfWidth,FlatBodyMinZ,FlatBodyMaxZ,Clear?1:0);return Clear;
}
bool AMAYABoostPowerNativeTest::SixFlatContacts()
{
    const auto J=MayaBoostTest::Parse(Robot->ReadTelemetryJson());const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;LastFlatContactEvidence=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Rows;bool All=true;
    if(!J||!J->TryGetArrayField(TEXT("parts"),Parts)||!Parts||Parts->Num()<7){LastFlatContactEvidence->SetBoolField(TEXT("known"),false);return false;}
    for(int32 I=1;I<=6;++I)
    {
        const auto Part=(*Parts)[I]->AsObject();auto Row=MakeShared<FJsonObject>();bool Known=false,Accepted=false;Part->TryGetBoolField(TEXT("nativeHitKnown"),Known);Part->TryGetBoolField(TEXT("nativeObservedSupportAccepted"),Accepted);
        const TArray<TSharedPtr<FJsonValue>> *Point=nullptr,*Normal=nullptr;const bool Arrays=Part->TryGetArrayField(TEXT("nativeHitPointCm"),Point)&&Point&&Point->Num()==3&&Part->TryGetArrayField(TEXT("nativeHitNormal"),Normal)&&Normal&&Normal->Num()==3;
        bool Finite=Arrays; if(Arrays)for(int32 K=0;K<3;++K)Finite=Finite&&FMath::IsFinite((*Point)[K]->AsNumber())&&FMath::IsFinite((*Normal)[K]->AsNumber());
        const FString Support=MayaBoostTest::Word(Part,TEXT("nativeHitSupport"));const bool Valid=Known&&Accepted&&Finite&&FlatSupports.Contains(Support);All=All&&Valid;
        Row->SetNumberField(TEXT("wheel"),I-1);Row->SetStringField(TEXT("supportComponent"),Support);Row->SetBoolField(TEXT("nativeHitKnown"),Known);Row->SetBoolField(TEXT("nativeSupportAcceptedByOriginalFreshQuietAnchorPolicy"),Accepted);Row->SetBoolField(TEXT("finiteContactPointNormal"),Finite);Row->SetBoolField(TEXT("originalSurveyedFlatContactAccepted"),Valid);if(Arrays){Row->SetArrayField(TEXT("actualNativeHitPointCm"),*Point);Row->SetArrayField(TEXT("actualNativeHitNormal"),*Normal);}Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    LastFlatContactEvidence->SetArrayField(TEXT("actualWheelContacts"),Rows);LastFlatContactEvidence->SetBoolField(TEXT("allSixAccepted"),All);LastFlatContactEvidence->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());LastFlatContactEvidence->SetNumberField(TEXT("frame"),double(GFrameCounter));LastFlatContactEvidence->SetStringField(TEXT("scope"),TEXT("Actual native contact receipts under unchanged production freshness/quiet-anchor policy, original static support identity, finite point/normal. The raw normal/height are evidence only; >=.99/ground+/-2cm remain fresh survey criteria. Not fresh independent ray surfaces; no new standalone wall-time-age gate."));return All;
}
bool AMAYABoostPowerNativeTest::Position(const FMAYADeliveryRobotObservation& O,double Radius,bool Match)
{
    const double Now=GetWorld()->GetTimeSeconds(),Age=Now-StepWorld;const FVector P=O.ChassisPose.GetLocation(),Delta=Target-P;
    const double Along=FVector::DotProduct(Delta,Axis),Cross=FVector::DotProduct(Delta,Left),Distance=Delta.Size2D();LastPositionError=Distance;
    const FVector Optical=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
    // The new-profile paired setup targets the actual recorded previous heading,
    // so two valid opposite axis trims cannot manufacture an unmatched pair.
    const double GoalHeading=(Step==1||Step==5)&&RunMode==0&&Pass%2==1?PairHeading[Pass/2]:Axis.Rotation().Yaw;
    const double Error=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,GoalHeading);
    const double InnerAimRadius=Radius*.4,YawRate=O.AngularVelocityRadps.Z;
    const bool FlatSetup=(Step==1||Step==5)&&RunMode==0;
    const bool NearCorrectionPendingBeforeDecision=bFlatNearBearingCorrectionPending;
    FString NearCorrectionStateReason;
    const bool Interior=Distance<=InnerAimRadius||(FMath::Abs(Along)<=InnerAimRadius&&Distance<=Radius*.8);
    const bool HeadingOutsideGateAndInteriorAbsentObserved=FlatSetup&&Match&&FMath::Abs(Error)>1&&!Interior;
    if(FlatSetup&&ReapproachPass!=Pass){ReapproachPass=Pass;ReapproachAttempts=ReapproachStage=0;ReapproachStallWorld=ReapproachStartedWorld=ReapproachBrakeWorld=-1;}
    if(FlatSetup&&SetupPulsePass!=Pass)
    {SetupPulsePass=Pass;SetupPulseCount=SetupPulseStage=0;SetupNearWorld=SetupPulseStageWorld=SetupPulseStageReal=SetupPulseWorld=SetupPulseReal=-1;SetupPulseDirection=0;SetupPulseStart=SetupPulseReleaseStart=FVector::ZeroVector;}
    if(FlatSetup&&FlatFinePass!=Pass)
    {FlatFinePass=Pass;bFlatFineEnvelopeActive=bFlatFineBrakedRescue=false;FlatFinePoweredResidenceWorld=FlatFineBrakeWorld=FlatFineBrakedResidenceWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=FVector::ZeroVector;}
    auto ResetNearPi=[&](){FlatNearIntegral=0;FlatNearUpdateWorld=-1;};
    if(FlatSetup&&(FlatNearPass!=Pass||FlatNearTarget!=Target))
    {FlatNearPass=Pass;FlatNearTarget=Target;FlatNearPhase=0;FlatNearPhaseWorld=FlatNearYawReal=-1;FlatNearBearing=FlatNearTravelSign=FlatNearYawSign=0;bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("New pass/target abandons prior straight episode");ResetNearPi();FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=P;}
    const bool ActualQuiet=Quiet(O),PhysicalLocks=Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();
    LastPositionGuard=MakeShared<FJsonObject>();LastPositionGuard->SetNumberField(TEXT("worldSeconds"),Now);LastPositionGuard->SetNumberField(TEXT("frame"),double(GFrameCounter));LastPositionGuard->SetNumberField(TEXT("positionMode"),PositionMode);LastPositionGuard->SetNumberField(TEXT("positionModeAgeSeconds"),Now-PositionModeWorld);
    LastPositionGuard->SetArrayField(TEXT("targetCm"),MayaBoostTest::Vec(Target));LastPositionGuard->SetArrayField(TEXT("actualPositionCm"),MayaBoostTest::Vec(P));LastPositionGuard->SetNumberField(TEXT("setupAgeSeconds"),Age);
    LastPositionGuard->SetNumberField(TEXT("actualPlanarDistanceCm"),Distance);LastPositionGuard->SetNumberField(TEXT("alongErrorCm"),Along);LastPositionGuard->SetNumberField(TEXT("crossErrorCm"),Cross);
    LastPositionGuard->SetNumberField(TEXT("outerAcceptanceRadiusCm"),Radius);LastPositionGuard->SetNumberField(TEXT("innerAimRadiusCm"),InnerAimRadius);LastPositionGuard->SetNumberField(TEXT("headingTrimInnerAimDegrees"),FlatSetup?0:.5);LastPositionGuard->SetNumberField(TEXT("targetHeadingDegrees"),GoalHeading);LastPositionGuard->SetNumberField(TEXT("headingErrorDegreesTargetMinusActual"),Error);
    LastPositionGuard->SetNumberField(TEXT("actualLinearSpeedCmps"),O.VelocityCmps.Size());LastPositionGuard->SetNumberField(TEXT("actualAngularSpeedRadps"),O.AngularVelocityRadps.Size());LastPositionGuard->SetNumberField(TEXT("actualYawRateRadps"),YawRate);
    LastPositionGuard->SetBoolField(TEXT("actualQuiet"),ActualQuiet);LastPositionGuard->SetBoolField(TEXT("nativePhysicalLocks"),PhysicalLocks);LastPositionGuard->SetBoolField(TEXT("typedDoorsClosedLatched"),O.bDoorsClosedLatched);LastPositionGuard->SetBoolField(TEXT("known"),O.bKnown);LastPositionGuard->SetBoolField(TEXT("ready"),O.bReady);LastPositionGuard->SetBoolField(TEXT("grounded"),O.bGrounded);LastPositionGuard->SetBoolField(TEXT("upright"),O.bUpright);LastPositionGuard->SetNumberField(TEXT("supportMask"),O.SupportMask);
    LastPositionGuard->SetNumberField(TEXT("door0OmegaRadps"),O.DoorOmegaRadps[0]);LastPositionGuard->SetNumberField(TEXT("door1OmegaRadps"),O.DoorOmegaRadps[1]);LastPositionGuard->SetBoolField(TEXT("positionAccepted"),Distance<=Radius);LastPositionGuard->SetBoolField(TEXT("headingAccepted"),!Match||FMath::Abs(Error)<=1);
    LastPositionGuard->SetNumberField(TEXT("quietDwellStartWorldSeconds"),StableWorld);LastPositionGuard->SetNumberField(TEXT("quietDwellObservedSeconds"),StableWorld>=0?Now-StableWorld:0);
    LastPositionGuard->SetNumberField(TEXT("lastAcceptedRequestedThrottle"),LastRequestedThrottle);LastPositionGuard->SetNumberField(TEXT("lastAcceptedRequestedSteer"),LastRequestedSteer);LastPositionGuard->SetNumberField(TEXT("lastAcceptedCommandWorldSeconds"),LastDriveWorld);
    LastPositionGuard->SetBoolField(TEXT("flatPassSetup"),FlatSetup);LastPositionGuard->SetNumberField(TEXT("reapproachAttemptCount"),ReapproachAttempts);LastPositionGuard->SetNumberField(TEXT("reapproachStage"),ReapproachStage);
    LastPositionGuard->SetBoolField(TEXT("flatSetupInteriorGeometry"),Interior);LastPositionGuard->SetBoolField(TEXT("flatSetupHeadingOutsideGateAndInteriorAbsentObserved"),HeadingOutsideGateAndInteriorAbsentObserved);
    LastPositionGuard->SetArrayField(TEXT("reapproachWaypointCm"),MayaBoostTest::Vec(ReapproachWaypoint));LastPositionGuard->SetArrayField(TEXT("reapproachActualStartCm"),MayaBoostTest::Vec(ReapproachStart));LastPositionGuard->SetNumberField(TEXT("stalledGeometryStartedWorldSeconds"),ReapproachStallWorld);
    LastPositionGuard->SetNumberField(TEXT("flatTrimFixedDirection"),PositionTrimDirection);LastPositionGuard->SetNumberField(TEXT("flatTrimInitialHeadingErrorDegrees"),PositionTrimInitialError);
    LastPositionGuard->SetNumberField(TEXT("flatTrimMaximumMotorSeconds"),.30);LastPositionGuard->SetNumberField(TEXT("flatTrimSignedTargetCrossingStopDegrees"),-.5);
    LastPositionGuard->SetNumberField(TEXT("flatTrimStartedRealSeconds"),PositionTrimStartedReal);LastPositionGuard->SetNumberField(TEXT("flatTrimElapsedRealSeconds"),PositionTrimStartedReal>=0?FPlatformTime::Seconds()-PositionTrimStartedReal:0);
    LastPositionGuard->SetNumberField(TEXT("lastAcceptedCommandRealSeconds"),LastDriveReal);LastPositionGuard->SetNumberField(TEXT("lastAcceptedCommandLeaseSeconds"),LastDriveLeaseSeconds);
    LastPositionGuard->SetNumberField(TEXT("flatSetupNearGoalResidenceStartedWorldSeconds"),SetupNearWorld);LastPositionGuard->SetNumberField(TEXT("flatSetupNearGoalResidenceSeconds"),SetupNearWorld>=0?Now-SetupNearWorld:0);
    LastPositionGuard->SetNumberField(TEXT("flatSetupPulseStage"),SetupPulseStage);LastPositionGuard->SetNumberField(TEXT("flatSetupPulseCount"),SetupPulseCount);LastPositionGuard->SetNumberField(TEXT("flatSetupPulseDirection"),SetupPulseDirection);
    LastPositionGuard->SetArrayField(TEXT("flatSetupPulseActualStartCm"),MayaBoostTest::Vec(SetupPulseStart));LastPositionGuard->SetNumberField(TEXT("flatSetupPulseStartedWorldSeconds"),SetupPulseWorld);LastPositionGuard->SetNumberField(TEXT("flatSetupPulseStartedRealSeconds"),SetupPulseReal);
    LastPositionGuard->SetStringField(TEXT("flatSetupPulsePhaseBeforeDecision"),TEXT("RetiredInactiveTranslationPulseStrategy"));
    LastPositionGuard->SetBoolField(TEXT("flatSetupReleaseActiveBeforeDecision"),SetupPulseStage==4);
    if(SetupPulseStage==4){LastPositionGuard->SetNumberField(TEXT("flatSetupReleaseElapsedWorldSeconds"),Now-SetupPulseStageWorld);LastPositionGuard->SetNumberField(TEXT("flatSetupReleaseElapsedRealSeconds"),FPlatformTime::Seconds()-SetupPulseStageReal);LastPositionGuard->SetArrayField(TEXT("flatSetupReleaseActualStartCm"),MayaBoostTest::Vec(SetupPulseReleaseStart));}
    auto RecordFlatTrim=[&](const TCHAR* Event,bool RequestedBrake)
    {
        ++FlatTrimObservationsSeen;if(FlatTrimObservations.Num()>=2048){bFlatTrimObservationsTruncated=true;return;}
        auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("event"),Event);J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("pass"),Pass);
        J->SetNumberField(TEXT("fixedTrimDirection"),PositionTrimDirection);J->SetNumberField(TEXT("initialHeadingErrorDegrees"),PositionTrimInitialError);J->SetObjectField(TEXT("actualPositionGuard"),LastPositionGuard);
        J->SetNumberField(TEXT("pulseStartedWorldSeconds"),PositionTrimStartedWorld);J->SetNumberField(TEXT("pulseStartedRealSeconds"),PositionTrimStartedReal);J->SetNumberField(TEXT("eventRealSeconds"),FPlatformTime::Seconds());
        J->SetNumberField(TEXT("lastAcceptedCommandRealSeconds"),LastDriveReal);J->SetNumberField(TEXT("lastAcceptedCommandLeaseSeconds"),LastDriveLeaseSeconds);
        J->SetNumberField(TEXT("lastAcceptedRequestedThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("lastAcceptedRequestedSteer"),LastRequestedSteer);J->SetNumberField(TEXT("lastAcceptedCommandWorldSeconds"),LastDriveWorld);J->SetBoolField(TEXT("eventRequestedBrake"),RequestedBrake);
        J->SetStringField(TEXT("actualCachedNativeDecisionBeforeLaterCommand"),Robot->ReadDriveAdmissionEvidenceJson());FlatTrimObservations.Add(MakeShared<FJsonValueObject>(J));
    };
    if(Age>40||FMath::Abs(Cross)>35||!FMath::IsFinite(O.OpticalForwardCmps))
    {ResetNearPi();LastPositionGuard->SetStringField(TEXT("actualNativeAtSetupFailure"),Robot->ReadTelemetryJson());Finish(false,TEXT("Bounded physical setup position/heading did not converge"));return false;}
    auto RecordReapproach=[&](const TCHAR* Event)
    {
        auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("event"),Event);J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("pass"),Pass);J->SetNumberField(TEXT("attempt"),ReapproachAttempts);J->SetNumberField(TEXT("stage"),ReapproachStage);
        J->SetArrayField(TEXT("originalTargetCm"),MayaBoostTest::Vec(Target));J->SetArrayField(TEXT("waypointCm"),MayaBoostTest::Vec(ReapproachWaypoint));J->SetArrayField(TEXT("actualStartCm"),MayaBoostTest::Vec(ReapproachStart));J->SetArrayField(TEXT("actualPositionCm"),MayaBoostTest::Vec(P));
        J->SetNumberField(TEXT("backwardNativeProgressCm"),-FVector::DotProduct(P-ReapproachStart,Axis));J->SetNumberField(TEXT("originalSetupAgeSeconds"),Age);J->SetObjectField(TEXT("actualPositionGuard"),LastPositionGuard);J->SetStringField(TEXT("actualNativeTelemetry"),Robot->ReadTelemetryJson());ReapproachTransitions.Add(MakeShared<FJsonValueObject>(J));
    };
    if(FlatSetup&&ReapproachStage==0&&Distance<=2*Radius)bFlatFineEnvelopeActive=true;
    const double NearRayMissCm=FMath::Abs(Delta.X*Optical.Y-Delta.Y*Optical.X),NearRayAimRadiusCm=Radius*.2;
    const bool NearRayOperandsFinite=FMath::IsFinite(FVector::DotProduct(Delta,Optical))&&FMath::IsFinite(NearRayMissCm)&&FMath::IsFinite(NearRayAimRadiusCm)&&!Optical.ContainsNaN()&&FMath::IsFinite(Optical.SizeSquared2D())&&Optical.SizeSquared2D()>0;
    const bool NearStateFinite=NearRayOperandsFinite&&FMath::IsFinite(FlatNearPhaseWorld)&&FMath::IsFinite(FlatNearYawReal)&&FMath::IsFinite(FlatNearBearing)&&FMath::IsFinite(FlatNearTravelSign)&&FMath::IsFinite(FlatNearYawSign)&&FMath::IsFinite(FlatNearIntegral)&&FMath::IsFinite(FlatNearUpdateWorld)&&!Optical.ContainsNaN()&&FMath::IsFinite(Optical.Rotation().Yaw);
    const bool FineFinite=NearStateFinite&&FMath::IsFinite(Now)&&FMath::IsFinite(FPlatformTime::Seconds())&&FMath::IsFinite(Along)&&FMath::IsFinite(Cross)&&FMath::IsFinite(Distance)&&FMath::IsFinite(Error)&&FMath::IsFinite(O.OpticalForwardCmps)&&!P.ContainsNaN()&&!O.VelocityCmps.ContainsNaN()&&!O.AngularVelocityRadps.ContainsNaN();
    const double FlatNearPreparationRadiusCm=20,FlatNearRetentionRadiusCm=30,FlatPreFineRecoveryRadiusCm=10;
    const int32 FlatNearPhaseBeforeDecision=FlatNearPhase;
    const bool FlatNearEntryEligible=FlatSetup&&ReapproachStage==0&&FlatNearPhase==0&&Distance<=FlatNearPreparationRadiusCm;
    const bool FlatNearRetentionEligible=FlatSetup&&ReapproachStage==0&&FlatNearPhase>=1&&FlatNearPhase<=4&&Distance<=FlatNearRetentionRadiusCm;
    const bool FlatNearDomain=FlatNearEntryEligible||FlatNearRetentionEligible;
    const bool FlatPreFineRecoveryDomain=FlatSetup&&!bFlatFineEnvelopeActive&&ReapproachStage==0&&ReapproachAttempts==0&&Distance<=FlatPreFineRecoveryRadiusCm;
    const bool FlatPreFineRecoveryCancellationActive=FlatSetup&&!bFlatFineEnvelopeActive&&ReapproachStage==0&&bFlatFineBrakedRescue&&Distance>FlatPreFineRecoveryRadiusCm;
    const bool FlatNearCancellationActive=FlatSetup&&ReapproachStage==0&&(FlatNearPhase==5||(!bFlatFineEnvelopeActive&&FlatNearPhase>=1&&FlatNearPhase<=4&&Distance>FlatNearRetentionRadiusCm));
    const bool FlatPreFineCancellationActive=FlatPreFineRecoveryCancellationActive||FlatNearCancellationActive;
    const bool FlatRecoveryObservationRequired=FlatSetup&&(bFlatFineEnvelopeActive||FlatNearDomain||bFlatFineBrakedRescue||FlatNearPhase!=0);
    const bool FlatArrivalPositionEligible=Distance<=Radius;
    const double FlatTranslationBrakeAimRadius=Radius*.3;
    const bool HeadingNeedsTrim=Match&&FMath::Abs(Error)>1;
    const bool FlatExistingBrakedOrYawHandoff=PositionMode==1||PositionMode==2||bFlatFineBrakedRescue;
    // Capture before clearing rescue; an already-braked outer endpoint keeps its handoff.
    const bool FlatArrivalBrakeEligible=FlatArrivalPositionEligible&&(FlatExistingBrakedOrYawHandoff||(!FlatNearDomain&&FlatNearPhase==0&&(Distance<=FlatTranslationBrakeAimRadius||HeadingNeedsTrim)));
    const FString FineNativeDecisionBeforeLaterCommand=FlatRecoveryObservationRequired?Robot->ReadDriveAdmissionEvidenceJson(false):FString();
    const double FineDecisionReal=FPlatformTime::Seconds();
    double NearProjection=FVector::DotProduct(Delta,Optical),NearCurrentBearing=0,NearBearingError=0;
    auto NearRayFeasible=[&](){const double SignedProjection=FlatNearTravelSign*NearProjection;return NearRayOperandsFinite&&FMath::IsFinite(SignedProjection)&&SignedProjection>0&&NearRayMissCm<=NearRayAimRadiusCm;};
    double NearVref=0,NearVelocityError=0,NearP=0,NearIBefore=FlatNearIntegral,NearIAfter=FlatNearIntegral,NearDt=0,NearProposedIncrement=0,NearThrottle=0;
    bool NearAntiWindup=false,NearSaturated=false,NearAuthority=false,NearIntegrated=false;
    FString NearResetReason;
    auto FinePhase=[&]()->const TCHAR*
    {
        if(ReapproachStage>0)return TEXT("OriginalBoundedReapproach");
        if(FlatPreFineRecoveryCancellationActive&&bFlatFineBrakedRescue)return TEXT("PreFineRecovery10cmNativeBrakeCancellation");
        if(FlatNearCancellationActive&&FlatNearPhase!=0)return TEXT("NearActive30cmLatchedPhase5NativeBrakeCancellation");
        if(bFlatFineBrakedRescue)return TEXT("ActualBrakedRescueResidence");
        if(PositionMode==2)return TEXT("OriginalFiniteYawTrim");
        if(PositionMode==1)return TEXT("ArrivalNativeBrakeSettlement");
        if(FlatNearPhase==1)return TEXT("NearBrakeForBearing");
        if(FlatNearPhase==2)return TEXT("NearFiniteBearingYaw");
        if(FlatNearPhase==3)return TEXT("NearBrakeAfterBearing");
        if(FlatNearPhase==4)return TEXT("NearStraightVelocityPi");
        return bFlatFineEnvelopeActive?TEXT("ContinuousFineDrive"):FlatPreFineRecoveryDomain?TEXT("PreFineNearApproachDrive"):TEXT("OriginalDistantApproach");
    };
    auto RecordFine=[&](const TCHAR* Event,double RequestedThrottle,double RequestedSteer,bool RequestedBrake,bool Accepted)
    {
        if(!FlatSetup)return;
        ++FlatFineObservationsSeen;if(FlatFineObservations.Num()>=4096){bFlatFineObservationsTruncated=true;return;}
        auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("event"),Event);J->SetStringField(TEXT("phaseAfterDecision"),FinePhase());J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("decisionRealSecondsBeforeLaterCommand"),FineDecisionReal);J->SetNumberField(TEXT("receiptRealSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("pass"),Pass);
        J->SetArrayField(TEXT("actualPositionCm"),MayaBoostTest::Vec(P));J->SetArrayField(TEXT("actualVelocityCmps"),MayaBoostTest::Vec(O.VelocityCmps));J->SetNumberField(TEXT("actualOpticalForwardCmps"),O.OpticalForwardCmps);J->SetNumberField(TEXT("actualSpeedCmps"),O.VelocityCmps.Size());J->SetNumberField(TEXT("alongErrorCm"),Along);J->SetNumberField(TEXT("crossErrorCm"),Cross);J->SetNumberField(TEXT("actualPlanarDistanceCm"),Distance);J->SetNumberField(TEXT("headingErrorDegrees"),Error);J->SetNumberField(TEXT("unclampedExistingContinuousPdThrottle"),.014*Along-.012*O.OpticalForwardCmps);
        J->SetBoolField(TEXT("fineEnvelopeActiveAfterDecision"),bFlatFineEnvelopeActive);J->SetBoolField(TEXT("brakedRescueAfterDecision"),bFlatFineBrakedRescue);J->SetNumberField(TEXT("poweredPreBrakeResidenceStartedWorldSeconds"),FlatFinePoweredResidenceWorld);J->SetNumberField(TEXT("poweredPreBrakeResidenceSeconds"),FlatFinePoweredResidenceWorld>=0?Now-FlatFinePoweredResidenceWorld:0);J->SetArrayField(TEXT("poweredPreBrakeActualAnchorCm"),MayaBoostTest::Vec(FlatFinePoweredAnchor));J->SetNumberField(TEXT("rescueBrakeStartedWorldSeconds"),FlatFineBrakeWorld);J->SetNumberField(TEXT("brakedResidenceStartedWorldSeconds"),FlatFineBrakedResidenceWorld);J->SetNumberField(TEXT("brakedResidenceSeconds"),FlatFineBrakedResidenceWorld>=0?Now-FlatFineBrakedResidenceWorld:0);J->SetArrayField(TEXT("brakedActualAnchorCm"),MayaBoostTest::Vec(FlatFineBrakedAnchor));
        auto Near=MakeShared<FJsonObject>();auto NearNumber=[&](const TCHAR* Key,double Value){if(FMath::IsFinite(Value))Near->SetNumberField(Key,Value);else Near->SetField(Key,MakeShared<FJsonValueNull>());};NearNumber(TEXT("phaseAfterDecision"),FlatNearPhase);NearNumber(TEXT("phaseStartedWorldSeconds"),FlatNearPhaseWorld);NearNumber(TEXT("yawStartedRealSeconds"),FlatNearYawReal);NearNumber(TEXT("frozenOpticalBearingDegrees"),FlatNearBearing);NearNumber(TEXT("frozenTravelSign"),FlatNearTravelSign);NearNumber(TEXT("frozenYawSign"),FlatNearYawSign);Near->SetBoolField(TEXT("bearingCorrectionPendingBeforeDecision"),NearCorrectionPendingBeforeDecision);Near->SetBoolField(TEXT("bearingCorrectionPendingAfterDecision"),bFlatNearBearingCorrectionPending);Near->SetStringField(TEXT("bearingCorrectionStateChangeReason"),NearCorrectionStateReason);
        NearNumber(TEXT("actualSignedRayProjectionCm"),FlatNearTravelSign*NearProjection);NearNumber(TEXT("actualPlanarRayMissCm"),NearRayMissCm);NearNumber(TEXT("rayAimRadiusCm"),NearRayAimRadiusCm);Near->SetBoolField(TEXT("rayOperandsFiniteAndUnitPlanarOpticalNonzero"),NearRayOperandsFinite);Near->SetBoolField(TEXT("rayCanReachInnerPlanningDisk"),NearRayFeasible());if(!Optical.ContainsNaN())Near->SetArrayField(TEXT("actualUnitPlanarOpticalForward"),MayaBoostTest::Vec(Optical));
        NearNumber(TEXT("actualProjectionCm"),NearProjection);NearNumber(TEXT("currentOpticalTargetBearingDegrees"),NearCurrentBearing);NearNumber(TEXT("bearingErrorDegrees"),NearBearingError);NearNumber(TEXT("actualOpticalVelocityCmps"),O.OpticalForwardCmps);NearNumber(TEXT("referenceVelocityCmps"),NearVref);NearNumber(TEXT("velocityErrorCmps"),NearVelocityError);NearNumber(TEXT("proportionalRequest"),NearP);NearNumber(TEXT("integralBefore"),NearIBefore);NearNumber(TEXT("calculatedIntegralAfterPiUpdate"),NearIAfter);NearNumber(TEXT("integralStateAfterDecision"),FlatNearIntegral);NearNumber(TEXT("actualWorldDtSeconds"),NearDt);NearNumber(TEXT("proposedIntegralIncrement"),NearProposedIncrement);Near->SetBoolField(TEXT("integrationApplied"),NearIntegrated);Near->SetBoolField(TEXT("antiWindupRejected"),NearAntiWindup);Near->SetBoolField(TEXT("requestSaturated"),NearSaturated);Near->SetBoolField(TEXT("directionalAuthorityForPoweredResidence"),NearAuthority);Near->SetStringField(TEXT("resetReason"),NearResetReason);J->SetObjectField(TEXT("nearArrivalController"),Near);
        J->SetNumberField(TEXT("eventRequestedThrottle"),RequestedThrottle);J->SetNumberField(TEXT("eventRequestedSteer"),RequestedSteer);J->SetBoolField(TEXT("eventRequestedBrake"),RequestedBrake);J->SetBoolField(TEXT("eventRequestedBoost"),false);J->SetBoolField(TEXT("eventCommandAccepted"),Accepted);J->SetNumberField(TEXT("lastAcceptedRequestedThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("lastAcceptedRequestedSteer"),LastRequestedSteer);J->SetNumberField(TEXT("lastAcceptedCommandWorldSeconds"),LastDriveWorld);J->SetNumberField(TEXT("lastAcceptedCommandRealSeconds"),LastDriveReal);J->SetNumberField(TEXT("lastAcceptedCommandLeaseSeconds"),LastDriveLeaseSeconds);J->SetObjectField(TEXT("actualPositionGuard"),LastPositionGuard);
        const auto Decision=MayaBoostTest::Parse(FineNativeDecisionBeforeLaterCommand);TSharedPtr<FJsonObject> PT;if(Decision)PT=MayaBoostTest::Parse(MayaBoostTest::Word(Decision,TEXT("wheelFeedbackEvidence")));auto CompactPT=MakeShared<FJsonObject>();
        if(PT)for(const TCHAR* K:{TEXT("matchingActualOutputKnown"),TEXT("productionCallbackRegistered"),TEXT("legacyExternalSelected"),TEXT("generation"),TEXT("revision"),TEXT("consumedCommandSequence"),TEXT("actualCallbackSequence"),TEXT("callbackStartSimSeconds"),TEXT("callbackDtSeconds"),TEXT("actualInputSampleRealSeconds"),TEXT("gtObservationRealSeconds"),TEXT("freshAtApplication"),TEXT("driveAdmittedAtApplication"),TEXT("boostActiveAtApplication"),TEXT("nativeBrakeReadbackVerified"),TEXT("actualClosedLocksAndPose"),TEXT("nineBodiesBound"),TEXT("eightJointIdentities"),TEXT("selectedPeakNm"),TEXT("selectedNoLoadRadps"),TEXT("ownerCurrentAtApplication"),TEXT("nativeStateFinite"),TEXT("evaluationNativeStateFinite"),TEXT("evaluationActualClosedLocks"),TEXT("consumedIssuedRealSeconds"),TEXT("consumedLeaseSeconds"),TEXT("sampledChassisSpeedKnown"),TEXT("sampledChassisSpeedCmps"),TEXT("lowSpeedReserveFactor"),TEXT("actualSameStepEffortFraction"),TEXT("consumedCommand"),TEXT("wheels")})
        {const auto V=PT->Values.FindRef(K);if(V.IsValid())CompactPT->SetField(K,V);}
        J->SetObjectField(TEXT("completedWheelFeedbackBeforeLaterCommand"),CompactPT);J->SetStringField(TEXT("observationScope"),TEXT("Current synchronized setup geometry/optical velocity and separately stamped latest completed PT decision cached before this later command. Compact fields only; no accumulated native histories. Powered-pre-brake residence is not actual stopped evidence; braked residence has an independent clock/anchor."));FlatFineObservations.Add(MakeShared<FJsonValueObject>(J));
    };
    LastPositionGuard->SetNumberField(TEXT("flatNearPreparationRadiusCm"),FlatNearPreparationRadiusCm);LastPositionGuard->SetNumberField(TEXT("flatNearActiveRetentionRadiusCm"),FlatNearRetentionRadiusCm);LastPositionGuard->SetNumberField(TEXT("flatNearPhaseBeforeDecision"),FlatNearPhaseBeforeDecision);LastPositionGuard->SetBoolField(TEXT("flatNearEntryEligibleBeforeDecision"),FlatNearEntryEligible);LastPositionGuard->SetBoolField(TEXT("flatNearActiveRetentionEligibleBeforeDecision"),FlatNearRetentionEligible);LastPositionGuard->SetBoolField(TEXT("flatNearControllerDomainBeforeDecision"),FlatNearDomain);LastPositionGuard->SetBoolField(TEXT("flatNearCancellationLatchedBeforeDecision"),FlatNearPhaseBeforeDecision==5);LastPositionGuard->SetBoolField(TEXT("flatNearCancellationRequiredBeforeDecision"),FlatNearCancellationActive);LastPositionGuard->SetBoolField(TEXT("flatPreFineRecoveryCancellationActiveBeforeDecision"),FlatPreFineRecoveryCancellationActive);
    LastPositionGuard->SetNumberField(TEXT("flatPreFineRecoveryRadiusCm"),FlatPreFineRecoveryRadiusCm);LastPositionGuard->SetBoolField(TEXT("flatPreFineRecoveryDomainBeforeDecision"),FlatPreFineRecoveryDomain);LastPositionGuard->SetBoolField(TEXT("flatPreFineCancellationActiveBeforeDecision"),FlatPreFineCancellationActive);LastPositionGuard->SetBoolField(TEXT("flatRecoveryObservationRequiredBeforeDecision"),FlatRecoveryObservationRequired);
    LastPositionGuard->SetBoolField(TEXT("flatFineEnvelopeActiveBeforeDecision"),bFlatFineEnvelopeActive);LastPositionGuard->SetBoolField(TEXT("flatFineBrakedRescueBeforeDecision"),bFlatFineBrakedRescue);LastPositionGuard->SetBoolField(TEXT("flatFineArrivalPositionEligible"),FlatArrivalPositionEligible);
    LastPositionGuard->SetNumberField(TEXT("flatFineFreshTranslationBrakeAimRadiusCm"),FlatTranslationBrakeAimRadius);LastPositionGuard->SetBoolField(TEXT("flatFineHeadingNeedsTrim"),HeadingNeedsTrim);LastPositionGuard->SetBoolField(TEXT("flatFineExistingBrakedOrYawHandoffBeforeDecision"),FlatExistingBrakedOrYawHandoff);LastPositionGuard->SetBoolField(TEXT("flatFineArrivalBrakeEligibleBeforeDecision"),FlatArrivalBrakeEligible);
    if(FlatRecoveryObservationRequired)
    {
        const bool SixOriginal=SixFlatContacts();LastPositionGuard->SetBoolField(TEXT("flatFineFinite"),FineFinite);LastPositionGuard->SetBoolField(TEXT("flatFineSixOriginalSupports"),SixOriginal);
        if(bFlatFineEnvelopeActive)
        {
            if(!FineFinite||Distance>2*Radius||!O.bKnown||!O.bReady||!O.bGrounded||!O.bUpright||O.SupportMask!=63||!SixOriginal||!PhysicalLocks)
            {ResetNearPi();NearResetReason=TEXT("Hard fine/native guard failure");const bool Accepted=Drive();RecordFine(TEXT("Latched continuous fine envelope lost original native guard; brake requested"),0,0,true,Accepted);Finish(false,TEXT("Continuous flat fine setup lost its latched actual native guards"));return false;}
        }
        else if(!FineFinite||!O.bKnown||!O.bReady||!O.bGrounded||!O.bUpright||O.SupportMask!=63||!SixOriginal||!PhysicalLocks)
        {ResetNearPi();NearResetReason=TEXT("Near native/finite guard failure");const bool Accepted=Drive();RecordFine(TEXT("Pre-fine recovery/cancellation lost original physical native guards; brake requested"),0,0,true,Accepted);Finish(false,TEXT("Pre-fine flat recovery lost original actual native guards"));return false;}
    }
    // Fine containment above is authoritative; pre-fine domain exit never excuses it.
    if(FlatSetup&&!bFlatFineEnvelopeActive&&ReapproachStage==0&&Distance>FlatPreFineRecoveryRadiusCm)
    {
        FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=P;
        if(bFlatFineBrakedRescue)
        {
            ResetNearPi();NearResetReason=TEXT("Pre-fine domain cancellation");
            if(FlatNearPhase!=0&&FlatNearPhase!=1&&FlatNearPhase!=3){FlatNearPhase=1;FlatNearPhaseWorld=Now;}
            const double CancellationBrakeWorld=bFlatFineBrakedRescue?FlatFineBrakeWorld:FlatNearPhaseWorld;
            const bool Accepted=Drive();StableWorld=-1;
            if(Now-CancellationBrakeWorld>=.5&&ActualQuiet)
            {bFlatFineBrakedRescue=false;FlatFineBrakeWorld=-1;FlatNearPhase=0;FlatNearPhaseWorld=FlatNearYawReal=-1;RecordFine(TEXT("Pre-fine domain exit cancellation reached existing brake settlement/Quiet; approach may resume only next tick"),0,0,true,Accepted);}
            else RecordFine(TEXT("Pre-fine domain exit resets both candidates and retains actual native brake under original guards"),0,0,true,Accepted);
            return false;
        }
        FlatFineBrakeWorld=-1;
    }
    if(FlatNearCancellationActive)
    {
        ResetNearPi();NearResetReason=TEXT("Active30cm exit; sticky phase5 brake-only cancellation");
        FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=P;
        if(FlatNearPhase!=5)
        {
            if(FlatNearPhase!=1&&FlatNearPhase!=3)FlatNearPhaseWorld=Now;
            FlatNearPhase=5;FlatNearYawReal=-1;
        }
        const bool Accepted=Drive();StableWorld=-1;
        if(Now-FlatNearPhaseWorld>=.5&&ActualQuiet)
        {FlatNearPhase=0;FlatNearPhaseWorld=FlatNearYawReal=-1;FlatNearBearing=FlatNearTravelSign=FlatNearYawSign=0;bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Completed phase5 domain cancellation abandons straight episode");RecordFine(TEXT("Latched phase5 cancellation settled/Quiet and cleared; later-tick entry only within20cm"),0,0,true,Accepted);}
        else RecordFine(TEXT("Latched phase5 retains native brake/guards even after return inside30 or20cm"),0,0,true,Accepted);
        return false;
    }
    if(FlatSetup&&(bFlatFineEnvelopeActive||FlatPreFineRecoveryDomain))
    {
        if(FlatArrivalBrakeEligible)
        {ResetNearPi();NearResetReason=TEXT("Already-braked outer/final-yaw handoff");bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Actual outer/final handoff abandons straight episode");FlatNearPhase=0;bFlatFineBrakedRescue=false;FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=FlatFineBrakeWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=P;}
        else if(ReapproachAttempts==0&&ReapproachStage==0&&bFlatFineBrakedRescue)
        {
            ResetNearPi();FlatNearPhase=0;NearResetReason=TEXT("Actual braked rescue residence");
            const bool Accepted=Drive();const bool Eligible=Distance<=FlatPreFineRecoveryRadiusCm&&Now-FlatFineBrakeWorld>=.5&&ActualQuiet&&O.VelocityCmps.Size()<=4;
            const double Excursion=FVector::Dist2D(P,FlatFineBrakedAnchor);LastPositionGuard->SetBoolField(TEXT("flatFineBrakedResidenceEligible"),Eligible);LastPositionGuard->SetNumberField(TEXT("flatFineBrakedAnchorExcursionCm"),Excursion);
            if(!Eligible||Excursion>1){FlatFineBrakedResidenceWorld=-1;FlatFineBrakedAnchor=P;}
            else if(FlatFineBrakedResidenceWorld<0){FlatFineBrakedResidenceWorld=Now;FlatFineBrakedAnchor=P;}
            if(Eligible&&FlatFineBrakedResidenceWorld>=0&&Now-FlatFineBrakedResidenceWorld>=1.5)
            {
                const double TargetAlong=FVector::DotProduct(Target-FlatCourseOrigin,Axis),WaypointAlong=FMath::Clamp(TargetAlong-80.,420.,1300.);
                ReapproachWaypoint=Target+Axis*(WaypointAlong-TargetAlong);ReapproachStart=P;ReapproachStartedWorld=Now;ReapproachBrakeWorld=-1;ReapproachAttempts=1;ReapproachStage=1;StableWorld=-1;
                bFlatFineEnvelopeActive=false;bFlatFineBrakedRescue=false;bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Admitted original bounded recovery abandons straight episode");FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=FlatFineBrakeWorld=-1;
                RecordFine(TEXT("Independent actual-braked1.5s residence admitted existing one80cm reapproach; fine envelope suspended"),0,0,true,Accepted);RecordReapproach(TEXT("One bounded physical back-away admitted from independent actual-braked continuous-fine residence"));
            }
            else {StableWorld=-1;RecordFine(TEXT("Actual native brake retained while independent braked-rescue residence is observed"),0,0,true,Accepted);return false;}
        }
        else if(FlatNearPhase!=4){FlatFinePoweredResidenceWorld=-1;FlatFinePoweredAnchor=P;}
    }
    if(FlatSetup&&ReapproachStage>0)
    {
        SetupNearWorld=-1;
        const double BackwardProgress=-FVector::DotProduct(P-ReapproachStart,Axis),WaypointErrorAlong=FVector::DotProduct(P-ReapproachWaypoint,Axis);
        const double WaypointCrossGoalMinusActual=FVector::DotProduct(ReapproachWaypoint-P,Left);
        LastPositionGuard->SetNumberField(TEXT("reapproachAgeSeconds"),Now-ReapproachStartedWorld);LastPositionGuard->SetNumberField(TEXT("reapproachBackwardProgressCm"),BackwardProgress);LastPositionGuard->SetNumberField(TEXT("waypointLongitudinalErrorCm"),WaypointErrorAlong);
        if(Now-ReapproachStartedWorld>10||BackwardProgress< -5||BackwardProgress>100||O.SupportMask!=63||FMath::Abs(FVector::DotProduct(P-FlatCourseOrigin,Left))>35)
        {RecordReapproach(TEXT("Bounded physical back-away failed native time/progress/support/corridor guard"));Finish(false,TEXT("Single physical flat-setup reapproach did not retain10s/100cm/native support/corridor bounds"));return false;}
        if(ReapproachStage==1)
        {
            if(WaypointErrorAlong<=5){Drive();ReapproachStage=2;ReapproachBrakeWorld=Now;StableWorld=-1;RecordReapproach(TEXT("Actual backward longitudinal waypoint reached; braking"));}
            else
            {
                const double HeadingError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,Axis.Rotation().Yaw);
                const double Steer=FMath::Clamp(HeadingError/35. - WaypointCrossGoalMinusActual*.01 - YawRate*.15,-.25,.25);
                Drive(-.35,Steer,false);return false;
            }
        }
        const bool FineWaypointBrakeAccepted=Drive();
        if(Now-ReapproachBrakeWorld>=.5&&ActualQuiet)
        {
            if(StableWorld<0)StableWorld=Now;
            if(Now-StableWorld>=.5){ReapproachStage=0;PositionMode=0;PositionModeWorld=Now;StableWorld=-1;RecordFine(TEXT("Existing braked waypoint dwell completed; continuous correction resumes under original40s clock"),0,0,true,FineWaypointBrakeAccepted);RecordReapproach(TEXT("Actual braked supported waypoint hold; original target resumed with same40s clock"));UE_LOG(LogTemp,Display,TEXT("MAYA BOOST setup reapproach resume pass=%d world=%.3f originalage=%.3f backwardprogress=%.3f"),Pass,Now,Age,BackwardProgress);}
        }
        else StableWorld=-1;
        return false;
    }
    if(FlatNearDomain&&PositionMode==0&&!bFlatFineBrakedRescue&&!FlatArrivalBrakeEligible)
    {
        auto BeginNearBrake=[&](const TCHAR* Reason,int32 Phase=1)
        {ResetNearPi();NearResetReason=Reason;FlatNearPhase=Phase;FlatNearPhaseWorld=Now;FlatNearYawReal=-1;FlatFinePoweredResidenceWorld=-1;FlatFinePoweredAnchor=P;StableWorld=-1;};
        if(FlatNearPhase==0)BeginNearBrake(TEXT("Near controller entry"));
        if(FlatNearPhase==1||FlatNearPhase==3)
        {
            ResetNearPi();NearResetReason=TEXT("Actual near-bearing brake settlement");
            const bool Accepted=Drive();if(!Accepted){ResetNearPi();NearResetReason=TEXT("Brake command rejected");}
            if(Now-FlatNearPhaseWorld<.5||!ActualQuiet)
            {StableWorld=-1;RecordFine(TEXT("Near bearing brake retained until original0.5s/Quiet"),0,0,true,Accepted);return false;}
            // Existing actual-braked outer endpoint has priority over near-zero bearing math.
            if(Distance<=Radius)
            {PositionMode=1;PositionModeWorld=FlatNearPhaseWorld;FlatNearPhase=0;bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Actual braked outer endpoint abandons straight episode");StableWorld=-1;RecordFine(TEXT("Actual braked outer endpoint hands original final heading/dwell"),0,0,true,Accepted);}
            else
            {
                if(FlatNearPhase==3&&bFlatNearBearingCorrectionPending)
                {bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Completed bounded corrective yaw and actual phase3 brake settlement consumed request");}
                FlatNearTravelSign=NearProjection>0?1:NearProjection<0?-1:Along>=0?1:-1;
                FlatNearBearing=(Delta*FlatNearTravelSign).Rotation().Yaw;NearCurrentBearing=FlatNearBearing;
                NearBearingError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,FlatNearBearing);FlatNearYawSign=NearBearingError>0?1:-1;
                if(!FMath::IsFinite(NearProjection)||!FMath::IsFinite(FlatNearBearing)||!FMath::IsFinite(NearBearingError))
                {ResetNearPi();NearResetReason=TEXT("Nonfinite newly selected bearing");RecordFine(TEXT("Finite near-bearing selection guard failed while actual brake retained"),0,0,true,Accepted);Finish(false,TEXT("Continuous flat fine setup lost its actual finite near-controller bearing"));return false;}
                const bool CorrectiveBearingYawRequired=bFlatNearBearingCorrectionPending&&NearBearingError!=0;
                if(bFlatNearBearingCorrectionPending&&NearBearingError==0)
                {bFlatNearBearingCorrectionPending=false;NearCorrectionStateReason=TEXT("Exact-zero actual selected bearing clears correction without invented yaw");}
                FlatNearPhaseWorld=Now;FlatNearYawReal=FPlatformTime::Seconds();
                FlatNearPhase=CorrectiveBearingYawRequired||!NearRayFeasible()?2:4;
                ResetNearPi();NearResetReason=TEXT("Actual settled bearing selection; next-tick motion");
                RecordFine(TEXT("Frozen nearest forward/reverse bearing selected after actual brake"),0,0,true,Accepted);return false;
            }
        }
        if(FlatNearPhase==2)
        {
            ResetNearPi();NearResetReason=TEXT("Finite bearing yaw has no translation integral");NearCurrentBearing=FlatNearBearing;
            NearBearingError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,FlatNearBearing);
            const double Remaining=.30-(FPlatformTime::Seconds()-FlatNearYawReal)-.001;
            if(FlatNearYawSign*NearBearingError<=-.5||Now-FlatNearPhaseWorld>=.30||Remaining<=0)
            {BeginNearBrake(TEXT("Finite frozen-bearing yaw crossing/world/real bound"),3);const bool Accepted=Drive();RecordFine(TEXT("Finite bearing yaw ended; actual native brake requested"),0,0,true,Accepted);return false;}
            const bool Accepted=Drive(0,FlatNearYawSign*.5,false,false,FMath::Min(.25,Remaining));if(!Accepted){ResetNearPi();NearResetReason=TEXT("Bearing-yaw command rejected");}
            StableWorld=-1;RecordFine(TEXT("Fixed normal bearing yaw under original0.30s world/real lease"),0,FlatNearYawSign*.5,false,Accepted);return false;
        }
        if(FlatNearPhase==4)
        {
            // Fresh straight arrival must precede bearing/crossing replanning; no final-axis outer shortcut.
            if(Distance<=FlatTranslationBrakeAimRadius&&O.VelocityCmps.Size()<=4)
            {BeginNearBrake(TEXT("Fresh straight0.3Radius arrival"));const bool Accepted=Drive();RecordFine(TEXT("Straight arrival requested actual brake before original outer handoff"),0,0,true,Accepted);return false;}
            NearCurrentBearing=(Delta*FlatNearTravelSign).Rotation().Yaw;
            NearBearingError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,NearCurrentBearing);
            if(!NearRayFeasible())
            {bFlatNearBearingCorrectionPending=true;NearCorrectionStateReason=TEXT("Actual valid straight episode lost its ray; bounded bearing correction required");BeginNearBrake(TEXT("Actual straight ray misses inner planning disk or frozen direction"));const bool Accepted=Drive();RecordFine(TEXT("Straight ray episode infeasible; actual brake before required bearing correction"),0,0,true,Accepted);return false;}
            NearVref=FMath::Clamp((7./6.)*NearProjection,-3.,3.);NearVelocityError=NearVref-O.OpticalForwardCmps;NearP=.012*NearVelocityError;
            NearIBefore=FlatNearIntegral;NearDt=FlatNearUpdateWorld>=0?Now-FlatNearUpdateWorld:0;
            if(FlatNearUpdateWorld<0){ResetNearPi();FlatNearUpdateWorld=Now;NearResetReason=TEXT("First straight sample; no elapsed integration credit");}
            else if(!FMath::IsFinite(NearDt)||NearDt<=0||NearDt>.25){ResetNearPi();NearResetReason=TEXT("Invalid/nonpositive/over0.25s dt; integral and timestamp reset");}
            else
            {
                NearProposedIncrement=FMath::Clamp(.02*NearVelocityError,-.10,.10)*NearDt;
                double CandidateI=FMath::Clamp(FlatNearIntegral+NearProposedIncrement,-.30,.30);const double CandidateT=NearP+CandidateI;
                NearAntiWindup=FMath::Abs(CandidateT)>.36&&CandidateT*NearVelocityError>0;
                if(NearAntiWindup)CandidateI=FlatNearIntegral;else NearIntegrated=true;
                FlatNearIntegral=CandidateI;FlatNearUpdateWorld=Now;
            }
            NearIAfter=FlatNearIntegral;NearThrottle=FMath::Clamp(NearP+FlatNearIntegral,-.36,.36);NearSaturated=FMath::Abs(NearP+FlatNearIntegral)>=.36;
            if(!FMath::IsFinite(NearProjection)||!FMath::IsFinite(NearCurrentBearing)||!FMath::IsFinite(NearBearingError)||!FMath::IsFinite(NearVref)||!FMath::IsFinite(NearVelocityError)||!FMath::IsFinite(NearP)||!FMath::IsFinite(FlatNearIntegral)||!FMath::IsFinite(NearThrottle))
            {BeginNearBrake(TEXT("Nonfinite near PI operand"));const bool Accepted=Drive();RecordFine(TEXT("Nonfinite near operand requested zero/native brake"),0,0,true,Accepted);Finish(false,TEXT("Continuous flat fine setup lost its actual finite near-controller operands"));return false;}
            NearAuthority=FlatNearTravelSign*FlatNearIntegral>=.30||FlatNearTravelSign*NearThrottle>=.36;
            if(ReapproachAttempts==0)
            {
                const bool Eligible=Distance<=FlatPreFineRecoveryRadiusCm&&NearAuthority&&O.VelocityCmps.Size()<=4;const double Excursion=FVector::Dist2D(P,FlatFinePoweredAnchor);
                LastPositionGuard->SetBoolField(TEXT("flatFinePoweredPreBrakeEligible"),Eligible);LastPositionGuard->SetNumberField(TEXT("flatFinePoweredPreBrakeAnchorExcursionCm"),Excursion);
                if(!Eligible||Excursion>1){FlatFinePoweredResidenceWorld=-1;FlatFinePoweredAnchor=P;}
                else if(FlatFinePoweredResidenceWorld<0){FlatFinePoweredResidenceWorld=Now;FlatFinePoweredAnchor=P;}
                if(Eligible&&FlatFinePoweredResidenceWorld>=0&&Now-FlatFinePoweredResidenceWorld>=1.5)
                {
                    LastPositionGuard->SetNumberField(TEXT("flatFineQualifiedPoweredPreBrakeResidenceSeconds"),Now-FlatFinePoweredResidenceWorld);
                    ResetNearPi();NearResetReason=TEXT("Authority-qualified powered no-progress starts separate actual brake");FlatNearPhase=0;
                    bFlatFineBrakedRescue=true;FlatFineBrakeWorld=Now;FlatFinePoweredResidenceWorld=FlatFineBrakedResidenceWorld=-1;FlatFinePoweredAnchor=FlatFineBrakedAnchor=P;StableWorld=-1;
                    const bool Accepted=Drive();RecordFine(TEXT("Capped straight authority no-progress qualified; actual brake and fresh clocks"),0,0,true,Accepted);return false;
                }
            }
            else {FlatFinePoweredResidenceWorld=-1;FlatFinePoweredAnchor=P;}
            const bool Accepted=Drive(NearThrottle,0,false,false);if(!Accepted){ResetNearPi();NearResetReason=TEXT("Straight command rejected");FlatFinePoweredResidenceWorld=-1;}
            StableWorld=-1;RecordFine(TEXT("Straight bounded optical-velocity PI; equal-bank normal command"),NearThrottle,0,false,Accepted);return false;
        }
    }
    // Game160 braked at the outer5cm boundary, then4.75..5.02cm settling
    // drift repeatedly rearmed translation and reset dwell. Non-flat approach
    // still aims at40% radius; fresh flat translation aims at30% for braking,
    // while heading-invalid outer position and existing braked/yaw handoff stay valid.
    // Retain original outer acceptance,40s deadline and actual braked stop.
    if(PositionMode==0&&(FlatSetup?(FlatArrivalBrakeEligible&&O.VelocityCmps.Size()<=4):Distance<=InnerAimRadius)){PositionMode=1;PositionModeWorld=Now;}
    if(PositionMode==1)
    {
        const bool FineBrakeAccepted=Drive();if(FlatSetup)RecordFine(TEXT("Original arrival native brake settlement/Quiet/dwell"),0,0,true,FineBrakeAccepted);
        // A brake command must have physical settling time before another trim.
        // Game161 rearmed yaw after only.104s of a valid brake transient.
        if(Now-PositionModeWorld<.5){StableWorld=-1;return false;}
        if(!ActualQuiet){StableWorld=-1;return false;}
        if(FlatSetup&&bFlatTrimAwaitingBrakedObservation)
        {RecordFlatTrim(TEXT("Actual supported/quiet braked trim endpoint after unchanged0.5s settling"),true);bFlatTrimAwaitingBrakedObservation=false;}
        if(Distance>Radius)
        {StableWorld=-1;PositionMode=0;PositionModeWorld=Now;FlatFinePoweredResidenceWorld=-1;FlatFinePoweredAnchor=P;if(FlatSetup)RecordFine(TEXT("Actual settled arrival miss returns to continuous correction; no pulse admission"),0,0,true,FineBrakeAccepted);return false;}
        // Heading already inside its actual1degree gate may complete the dwell.
        if(Match&&FMath::Abs(Error)>1)
        {
            StableWorld=-1;PositionMode=2;PositionModeWorld=Now;
            if(FlatSetup){PositionTrimDirection=Error>0?1:-1;PositionTrimInitialError=Error;PositionTrimStartedReal=FPlatformTime::Seconds();PositionTrimStartedWorld=Now;bFlatTrimMotorCommandRecorded=false;RecordFlatTrim(TEXT("Bounded fixed-direction flat trim admitted from actual braked pose"),true);}
            UE_LOG(LogTemp,Display,TEXT("MAYA BOOST finite heading pulse step=%d pass=%d world=%.3f error=%.3f yawrate=%.4f distance=%.3f"),Step,Pass,Now,Error,YawRate,Distance);return false;
        }
        const bool Ready=Distance<=Radius&&(!Match||FMath::Abs(Error)<=1)&&O.VelocityCmps.Size()<=4;
        if(Ready){if(StableWorld<0)StableWorld=Now;return Now-StableWorld>=.5;}
        StableWorld=-1;return false;
    }
    if(PositionMode==2)
    {
        StableWorld=-1;
        // Game164 reached transient inner headings, then braked back outside1deg.
        // Flat setup retains a fixed initial sign with ordinary5Nm authority and
        // <=.30s motor phase; a small signed target crossing can stop it sooner.
        // The actual braked endpoint must still pass the unchanged final gates.
        // Each flat trim lease also expires within the independent real-time
        // budget (1ms submission margin); other commands retain the0.25s lease.
        const double FlatLeaseRemaining=.30-(FPlatformTime::Seconds()-PositionTrimStartedReal)-.001;
        const bool MotorPhaseComplete=FlatSetup?(PositionTrimDirection*Error<=-.5||Now-PositionModeWorld>=.30||FlatLeaseRemaining<=0):(FMath::Abs(Error)<=.5||Now-PositionModeWorld>=.12);
        if(Distance>Radius||MotorPhaseComplete)
        {
            const bool FineBrakeAccepted=Drive();if(FlatSetup){RecordFine(TEXT("Original finite yaw ended; native brake settlement requested"),0,0,true,FineBrakeAccepted);RecordFlatTrim(TEXT("Finite flat trim ended by actual outer position, signed crossing or0.30s bound; brake requested"),true);bFlatTrimAwaitingBrakedObservation=true;}
            PositionMode=1;PositionModeWorld=Now;UE_LOG(LogTemp,Display,TEXT("MAYA BOOST finite pulse brake step=%d pass=%d world=%.3f error=%.3f yawrate=%.4f"),Step,Pass,Now,Error,YawRate);return false;
        }
        const bool Accepted=Drive(0,FlatSetup?PositionTrimDirection*.5:(Error>0?.35:-.35),false,false,FlatSetup?FMath::Min(.25,FlatLeaseRemaining):.25);
        if(FlatSetup)RecordFine(TEXT("Original finite normal yaw trim; continuous fine envelope retained"),0,PositionTrimDirection*.5,false,Accepted);
        if(Accepted&&FlatSetup&&!bFlatTrimMotorCommandRecorded){RecordFlatTrim(TEXT("First fixed normal pure-yaw command accepted"),false);bFlatTrimMotorCommandRecorded=true;}
        return false;
    }
    StableWorld=-1;
    const double Sign=Along>=0?1:-1;const FVector Travel=(Axis*Sign+Left*FMath::Clamp(Cross*.018,-.5,.5)).GetSafeNormal2D();
    const double SteerError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,(Travel*Sign).Rotation().Yaw);
    double T=FMath::Abs(Along)>80?Sign*.6:FMath::Clamp(.014*Along-.012*O.OpticalForwardCmps,-.36,.36);
    // Only low-speed target-directed setup demand gets breakaway feed-forward.
    // Opposing damping/deceleration and the actual braked endpoint stay intact.
    if(!FlatSetup&&FMath::Abs(O.OpticalForwardCmps)<8&&FMath::Abs(Along)>FMath::Max(.25,InnerAimRadius*.25)&&T*Along>0&&FMath::Abs(T)<.25)
        T=Sign*.25;
    const double Steer=FMath::Clamp(SteerError/35.,-.65,.65);const bool FineDriveAccepted=Drive(T,Steer,false);
    if(FlatRecoveryObservationRequired)RecordFine(TEXT("Existing continuous PD and steering without flat breakaway floor"),T,Steer,false,FineDriveAccepted);return false;
}
void AMAYABoostPowerNativeTest::Save(const FMAYADeliveryRobotObservation& O,bool Force)
{
    const double Now=GetWorld()->GetTimeSeconds();if(!Force&&Now-LastSample<.5)return;LastSample=Now;if(Samples.Num()>=1800)return;
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("step"),Step);J->SetNumberField(TEXT("pass"),Pass);J->SetNumberField(TEXT("runMode"),RunMode);J->SetArrayField(TEXT("positionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("velocityCmps"),MayaBoostTest::Vec(O.VelocityCmps));J->SetNumberField(TEXT("opticalForwardCmps"),O.OpticalForwardCmps);J->SetNumberField(TEXT("supportMask"),O.SupportMask);J->SetBoolField(TEXT("boostDriveActive"),O.bBoostDriveActive);J->SetNumberField(TEXT("boostMotorEffortFraction"),O.BoostMotorEffortFraction);J->SetNumberField(TEXT("batteryPercent"),Director->Snapshot().BatteryPercent);J->SetNumberField(TEXT("conditionPercent"),Director->Snapshot().CargoConditionPercent);
    J->SetNumberField(TEXT("lastAcceptedRequestedThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("lastAcceptedRequestedSteer"),LastRequestedSteer);J->SetNumberField(TEXT("lastAcceptedCommandWorldSeconds"),LastDriveWorld);
    if(Force||Samples.Num()%10==0){J->SetStringField(TEXT("nativeAppliedTorqueAndLaterReadback"),Robot->ReadTelemetryJson());J->SetStringField(TEXT("actualCustody"),Director->ReadCargoInventoryEvidenceJson());}Samples.Add(MakeShared<FJsonValueObject>(J));
}
void AMAYABoostPowerNativeTest::CaptureFlatDriveAdmission(const FMAYADeliveryRobotObservation& O)
{
    // Every actual flat driving/stopping test tick, before SubmitDriveCommand.
    // Bound plus explicit truncation prevents silently calling sparse coverage complete.
    CurrentFlatAdmissionEvidence.Reset();
    if (RunMode == 0) return; ++FlatAdmissionTraceSamplesSeen;
    if (FlatAdmissionTrace.Num() >= 12000) { bFlatAdmissionTraceTruncated = true; return; }
    auto J = MayaBoostTest::Parse(Robot->ReadDriveAdmissionEvidenceJson()); if (!J) return;
    J->SetNumberField(TEXT("testObservationWorldSeconds"), GetWorld()->GetTimeSeconds()); J->SetNumberField(TEXT("testObservationFrame"), double(GFrameCounter));
    J->SetNumberField(TEXT("testPass"), Pass); J->SetNumberField(TEXT("testRunMode"), RunMode);
    // O is this later PostUpdateWork body observation, distinct from cached
    // GT door/intent stamps and completed production PT or legacy GT motor stamps.
    const double Sign = (Pass / 2) % 2 == 0 ? 1 : -1;
    J->SetNumberField(TEXT("observedOpticalHeadingDegrees"), (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw);
    J->SetNumberField(TEXT("observedHeadingErrorDegreesTargetMinusActual"), FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw, RunHeading));
    J->SetNumberField(TEXT("observedYawRateRadps"), O.AngularVelocityRadps.Z); J->SetArrayField(TEXT("observedVelocityCmps"), MayaBoostTest::Vec(O.VelocityCmps));
    J->SetNumberField(TEXT("observedSignedTravelCm"), Sign * FVector::DotProduct(O.ChassisPose.GetLocation() - RunStart, Axis));
    J->SetNumberField(TEXT("observedLateralCm"), FVector::DotProduct(O.ChassisPose.GetLocation() - RunStart, Left)); J->SetNumberField(TEXT("observedSupportMask"), O.SupportMask);
    CurrentFlatAdmissionEvidence = J; FlatAdmissionTrace.Add(MakeShared<FJsonValueObject>(J));
}

bool AMAYABoostPowerNativeTest::WaitForFlatLaunchClearance(const FMAYADeliveryRobotObservation& O,bool PositionReady)
{
    if(bFinished)return false;const double Now=GetWorld()->GetTimeSeconds();
    if(PrelaunchWaitPass!=Pass){PrelaunchWaitPass=Pass;PrelaunchClearWorld=PrelaunchWaitWorld=-1;}
    if(!PositionReady&&PrelaunchWaitWorld<0)return false;
    if(PositionReady&&PrelaunchWaitWorld<0)PrelaunchWaitWorld=Now;
    LastPrelaunchClearance=MakeShared<FJsonObject>();auto J=LastPrelaunchClearance;
    J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("pass"),Pass);
    J->SetNumberField(TEXT("originalSetupAgeSeconds"),Now-StepWorld);J->SetNumberField(TEXT("waitStartedWorldSeconds"),PrelaunchWaitWorld);J->SetNumberField(TEXT("waitAgeSeconds"),PrelaunchWaitWorld>=0?Now-PrelaunchWaitWorld:0);J->SetNumberField(TEXT("requiredClearDwellSeconds"),.5);
    J->SetBoolField(TEXT("prelaunchClearDwellAccepted"),false);J->SetBoolField(TEXT("actualPositionReady"),PositionReady);J->SetBoolField(TEXT("actualQuiet"),Quiet(O));J->SetArrayField(TEXT("actualChassisPositionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("actualChassisVelocityCmps"),MayaBoostTest::Vec(O.VelocityCmps));J->SetNumberField(TEXT("actualSupportMask"),O.SupportMask);
    if(LastPositionGuard)J->SetObjectField(TEXT("actualMatchedPositionGuard"),LastPositionGuard);J->SetStringField(TEXT("cachedNativeDecisionBeforeWaitBrakeCommand"),Robot->ReadDriveAdmissionEvidenceJson());
    auto Retain=[&]() {++PrelaunchSamplesSeen;if(PrelaunchClearanceTrace.Num()<4096)PrelaunchClearanceTrace.Add(MakeShared<FJsonValueObject>(J));else bPrelaunchTraceTruncated=true;};
    if(!PositionReady){PrelaunchClearWorld=-1;J->SetBoolField(TEXT("raisedCorridorEvaluated"),false);J->SetStringField(TEXT("waitReason"),TEXT("Position/Quiet readiness lost; clear dwell reset and ordinary positioning continues within original40s"));J->SetNumberField(TEXT("clearDwellStartWorldSeconds"),-1);Retain();return false;}
    const bool SixOriginal=SixFlatContacts();J->SetBoolField(TEXT("sixOriginalNativeSupports"),SixOriginal);J->SetObjectField(TEXT("actualNativeSupportReceipts"),LastFlatContactEvidence);
    if(!Check(SixOriginal&&O.VelocityCmps.Size()<=4,TEXT("Flat pass starts with six actual original supports and stopped body")))
    {J->SetStringField(TEXT("waitReason"),TEXT("Unchanged actual flat-start support/stopped admission refused"));Retain();Finish(false,TEXT("Actual flat start admission refused"));return false;}
    if(!Drive())return false;
    J->SetNumberField(TEXT("acceptedWaitBrakeCommandWorldSeconds"),LastDriveWorld);J->SetNumberField(TEXT("acceptedWaitBrakeCommandRealSeconds"),LastDriveReal);J->SetNumberField(TEXT("acceptedWaitCommandLeaseSeconds"),LastDriveLeaseSeconds);J->SetNumberField(TEXT("acceptedWaitThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("acceptedWaitSteer"),LastRequestedSteer);J->SetBoolField(TEXT("requestedWaitBrake"),true);J->SetBoolField(TEXT("requestedWaitBoost"),false);
    const bool Clear=FlatRaisedCorridorClear(J);J->SetBoolField(TEXT("raisedCorridorEvaluated"),true);
    if(!Clear||!Quiet(O))PrelaunchClearWorld=-1;else if(PrelaunchClearWorld<0)PrelaunchClearWorld=Now;
    const double ClearAge=PrelaunchClearWorld>=0?Now-PrelaunchClearWorld:0;const bool Ready=Clear&&Quiet(O)&&PrelaunchClearWorld>=0&&ClearAge>=.5;
    J->SetNumberField(TEXT("clearDwellStartWorldSeconds"),PrelaunchClearWorld);J->SetNumberField(TEXT("observedContinuousClearDwellSeconds"),ClearAge);J->SetBoolField(TEXT("prelaunchClearDwellAccepted"),Ready);
    J->SetStringField(TEXT("waitReason"),!Clear?TEXT("Existing full raised corridor occupied; hold actual native brakes"):(Ready?TEXT("Fixed clear dwell complete; ordinary profile selection/start may proceed"):TEXT("Hold native brakes for fixed clear dwell")));Retain();
    if(Ready)UE_LOG(LogTemp,Display,TEXT("MAYA BOOST prelaunch clear pass=%d setupage=%.3f waitage=%.3f cleardwell=%.3f"),Pass,Now-StepWorld,Now-PrelaunchWaitWorld,ClearAge);
    return Ready;
}

void AMAYABoostPowerNativeTest::RunPass(const FMAYADeliveryRobotObservation& O)
{
    CaptureFlatDriveAdmission(O);
    const double Now=GetWorld()->GetTimeSeconds();const int32 Pair=Pass/2,Count=Pass<4?0:2;const bool Previous=Pass%2==0;const double Sign=(Pair%2==0)?1:-1;
    if(!Custody(Count)){Finish(false,TEXT("Matched flat pass actual parcel identity/restraint changed"));return;}
    if(RunMode==0)
    {
        // A selected owner can be waiting for a real completed, nonfuture
        // zero-motor/braked solver acknowledgement. Hold the original native
        // brake; the existing setup clock and later physical admission remain.
        if(bFlatProfileSelectionPending)
        {
            Drive();
            if(Now-StepWorld>40){Finish(false,TEXT("Motor ownership acknowledgement exceeded original40s setup bound"));return;}
            if(!Robot->IsNativeBoostComparisonProfileReady(Previous)||!Quiet(O))return;
            bFlatProfileSelectionPending=false;
        }
        Target=Previous?FlatCourseOrigin+Axis*(Sign>0?500:1300):PairStart[Pair];
        if(!WaitForFlatLaunchClearance(O,Position(O,Previous?5:2,true)))return;
        if(Count==2)
        {
            FBox Bounds(ForceInit);TSharedPtr<FJsonObject> CargoAdmission;const bool KnownInside=ReadFlatBounds(O,Count,false,CargoAdmission,Bounds);CargoEnvelopeAdmissions.Add(MakeShared<FJsonValueObject>(CargoAdmission));
            bool CargoFits=false;CargoAdmission->TryGetBoolField(TEXT("securedCargoInsideReferenceRobotEnvelope"),CargoFits);
            if(!KnownInside||!CargoFits){LastFlatBounds=CargoAdmission;Finish(false,TEXT("Actual secured parcel bounds do not fit the admitted reference robot envelope/vertical reserve"));return;}
        }
        if(bStaticBenchmark)
        {if(!Check(VerifySpawnDensity(),TEXT("Effective exact-zero density before actual comparison launch"))){Finish(false,TEXT("Static benchmark density changed before comparison"));return;}
            ObserveAmbientPopulation(Pass==0?TEXT("first_flat_launch"):FString::Printf(TEXT("comparison_launch_%d"),Pass),Previous);}
        if(!Robot->SelectNativeBoostComparisonProfile(Previous)){Finish(false,TEXT("Explicit braked legacy/current motor profile selection refused"));return;}
        if(!Robot->IsNativeBoostComparisonProfileReady(Previous)){bFlatProfileSelectionPending=true;Drive();return;}
        const double Heading=(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
        if(Previous){PairStart[Pair]=O.ChassisPose.GetLocation();PairHeading[Pair]=Heading;}
        else if(!Check(FVector::Dist(PairStart[Pair],O.ChassisPose.GetLocation())<=2&&FMath::Abs(FMath::FindDeltaAngleDegrees(PairHeading[Pair],Heading))<=1,TEXT("Native old/new pair matched pose <=2cm and heading <=1degree")))
        {Finish(false,TEXT("Actual pair alignment mismatch"));return;}
        if(!FMath::IsFinite(Now)||!FMath::IsFinite(O.AngularVelocityRadps.Z)){Finish(false,TEXT("Actual matched start world clock/yaw-rate is nonfinite"));return;}
        FilteredYawRateRadps=O.AngularVelocityRadps.Z;YawFilterWorld=Now;bYawFilterInitialized=true;LastYawFilterUpdate.Reset();
        auto Init=MakeShared<FJsonObject>();Init->SetNumberField(TEXT("pass"),Pass);Init->SetNumberField(TEXT("worldSeconds"),Now);Init->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());Init->SetNumberField(TEXT("frame"),double(GFrameCounter));Init->SetNumberField(TEXT("tauWorldSeconds"),.05);Init->SetNumberField(TEXT("initialActualRawYawRateRadps"),FilteredYawRateRadps);Init->SetNumberField(TEXT("initialFilteredYawRateRadps"),FilteredYawRateRadps);Init->SetArrayField(TEXT("actualMatchedBrakedPositionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));Init->SetBoolField(TEXT("actualQuiet"),Quiet(O));YawFilterInitializations.Add(MakeShared<FJsonValueObject>(Init));
        RunStart=O.ChassisPose.GetLocation();RunHeading=Heading;RunWorld=Now;BrakeStartWorld=-1;BrakeStartEvidence.Reset();BatteryStart=Director->Snapshot().BatteryPercent;BatteryPredicted=0;LastBatteryWorld=Now;Speeds.Reset();RunMode=1;Save(O,true);
        UE_LOG(LogTemp,Display,TEXT("MAYA BOOST pass start index=%d profile=%s parcels=%d direction=%.0f world=%.3f position=(%.2f,%.2f)"),Pass,Previous?TEXT("previous24radps6Nm"):TEXT("new42.4radpsRoad12NmLowSpeedMax18Nm"),Count,Sign,Now,RunStart.X,RunStart.Y);return;
    }
    const double Travel=Sign*FVector::DotProduct(O.ChassisPose.GetLocation()-RunStart,Axis),Lateral=FVector::DotProduct(O.ChassisPose.GetLocation()-RunStart,Left);
    const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,RunHeading);
    const double PassAge=Now-RunWorld;const bool SixOriginal=SixFlatContacts();
    const bool Stopping=RunMode==2;const double TravelLimit=Stopping?1000:900,HeadingLimit=Stopping?15:3,BrakeOverrun=Stopping?Travel-BrakeStartTravel:0;
    FBox CurrentBounds(ForceInit);const bool BoundsInside=ReadFlatBounds(O,Count,false,LastFlatBounds,CurrentBounds);
    const bool CorridorClear=FlatRaisedCorridorClear(LastFlatBounds);LastFlatBounds->SetNumberField(TEXT("activeHeadingLimitDegrees"),HeadingLimit);LastFlatBounds->SetNumberField(TEXT("activeTravelLimitCm"),TravelLimit);LastFlatBounds->SetNumberField(TEXT("brakeAdditionalForwardTravelCm"),BrakeOverrun);
    LastFlatBounds->SetObjectField(TEXT("actualNativeSupportReceipts"),LastFlatContactEvidence);
    ++FlatBoundsSamplesSeen;if(FlatBoundsTrace.Num()<12000)FlatBoundsTrace.Add(MakeShared<FJsonValueObject>(LastFlatBounds));else bFlatBoundsTraceTruncated=true;
    LastFlatGuard=MakeShared<FJsonObject>();LastFlatGuard->SetNumberField(TEXT("worldSeconds"),Now);LastFlatGuard->SetNumberField(TEXT("frame"),double(GFrameCounter));LastFlatGuard->SetNumberField(TEXT("pass"),Pass);
    LastFlatGuard->SetNumberField(TEXT("passAgeSeconds"),PassAge);LastFlatGuard->SetNumberField(TEXT("signedTravelCm"),Travel);LastFlatGuard->SetNumberField(TEXT("lateralCm"),Lateral);LastFlatGuard->SetNumberField(TEXT("headingErrorDegreesTargetMinusActual"),Error);
    LastFlatGuard->SetNumberField(TEXT("startOpticalHeadingDegrees"),RunHeading);LastFlatGuard->SetNumberField(TEXT("actualOpticalHeadingDegrees"),(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw);LastFlatGuard->SetNumberField(TEXT("supportMask"),O.SupportMask);
    LastFlatGuard->SetBoolField(TEXT("ageWithin15Seconds"),PassAge<=15);LastFlatGuard->SetBoolField(TEXT("travelWithinActivePhaseLimit"),Travel<=TravelLimit);LastFlatGuard->SetBoolField(TEXT("drivingTravelBelow900Cm"),Stopping||Travel<=900);LastFlatGuard->SetBoolField(TEXT("backwardExcursionWithin5Cm"),Travel>=-5);
    LastFlatGuard->SetBoolField(TEXT("lateralWithin20Cm"),FMath::Abs(Lateral)<=20);LastFlatGuard->SetBoolField(TEXT("headingWithinActivePhaseLimit"),FMath::Abs(Error)<=HeadingLimit);LastFlatGuard->SetBoolField(TEXT("drivingHeadingWithin3Degrees"),Stopping||FMath::Abs(Error)<=3);LastFlatGuard->SetBoolField(TEXT("sixNativeSupportMask"),O.SupportMask==63);LastFlatGuard->SetBoolField(TEXT("sixSurveyedOriginalSupportIdentities"),SixOriginal);
    LastFlatGuard->SetNumberField(TEXT("runMode"),RunMode);LastFlatGuard->SetNumberField(TEXT("activeTravelLimitCm"),TravelLimit);LastFlatGuard->SetNumberField(TEXT("activeHeadingLimitDegrees"),HeadingLimit);LastFlatGuard->SetNumberField(TEXT("additionalForwardTravelSinceBrakeCm"),BrakeOverrun);LastFlatGuard->SetNumberField(TEXT("additionalBrakeTravelLimitCm"),200);LastFlatGuard->SetBoolField(TEXT("actualAllBodyBoundsInsideSurvey"),BoundsInside);LastFlatGuard->SetBoolField(TEXT("observedRaisedCorridorClear"),CorridorClear);LastFlatGuard->SetObjectField(TEXT("actualCurrentBodyBounds"),LastFlatBounds);LastFlatGuard->SetObjectField(TEXT("actualNativeSupportReceipts"),LastFlatContactEvidence);
    if(PassAge>15||Travel>TravelLimit||Travel< -5||FMath::Abs(Lateral)>20||FMath::Abs(Error)>HeadingLimit||O.SupportMask!=63||!SixOriginal||!BoundsInside||!CorridorClear||(Stopping&&(BrakeStartWorld<0||BrakeOverrun>200)))
    {LastFlatGuard->SetStringField(TEXT("actualNativeAtGuardFailure"),Robot->ReadTelemetryJson());Save(O,true);
        UE_LOG(LogTemp,Warning,TEXT("MAYA BOOST pass guard FAIL index=%d age=%.4f travel=%.3f lateral=%.3f headingerror=%.4f support=%d originalsupports=%d"),Pass,PassAge,Travel,Lateral,Error,int32(O.SupportMask),SixOriginal?1:0);
        Finish(false,Stopping?TEXT("Actual supported stopping exceeded15s total/1000cm total/200cm additional/15degree/20cm/full-support/body-corridor bounds"):TEXT("Actual measured driving exceeded15s/900cm/3degree/20cm/full-support/body-corridor bounds"));return;}
    if(RunMode==1)
    {
        const double SignedSpeed=Sign*FVector::DotProduct(O.VelocityCmps,Axis);if(!FMath::IsFinite(SignedSpeed)||SignedSpeed< -5){Finish(false,TEXT("Actual signed flat speed invalid"));return;}
        Speeds.Add({Now,SignedSpeed,LastRequestedThrottle,LastRequestedSteer,LastDriveWorld});while(Speeds.Num()>2&&Speeds[1].World<Now-1.1)Speeds.RemoveAt(0,1,false);
        const double Dt=Now-LastBatteryWorld;LastBatteryWorld=Now;
        if(Dt>0&&Dt<=.25&&O.bBoostDriveActive&&FMath::IsFinite(O.BoostMotorEffortFraction))
        {const double Speed=O.VelocityCmps.Size(),Basis=FMath::Max(FMath::Clamp(Speed/200.,0.,3.),FMath::Clamp(O.BoostMotorEffortFraction,0.,1.)*FMath::Clamp(1-Speed/20.,0.,1.));BatteryPredicted+=Dt*1.2*(.01+.24*2.25*Basis)*(Count==2?1/.7:1);}
        if(Travel<800)
        {
            // Same fixed physical corridor controller for all eight profiles/load/signs.
            // Native steering is chassis yaw; only lateral correction reverses with travel.
            // Error is target-minus-actual degrees, angular Z is observed rad/s.
            // Game163 kept all motors admitted, but low-speed full-throttle
            // +/-tiny steer left both sides at the unchanged6Nm raw cap. Stronger
            // fixed yaw feedback and .65 bound can reduce one side below that cap.
            // One prospective controller change: only the measured D-term yaw
            // receives the same fixed 0.05-world-second low-pass in all eight runs.
            const double RawYaw=O.AngularVelocityRadps.Z,PreviousFilteredYaw=FilteredYawRateRadps,PreviousFilterWorld=YawFilterWorld,FilterDt=Now-YawFilterWorld;
            if(!bYawFilterInitialized||!FMath::IsFinite(FilterDt)||FilterDt<0||!FMath::IsFinite(RawYaw)||!FMath::IsFinite(PreviousFilteredYaw)){Finish(false,TEXT("Diagnostic yaw filter has invalid finite nonnegative world-time interval/state"));return;}
            const double Alpha=FilterDt>0?1.0-FMath::Exp(-FilterDt/.05):0;FilteredYawRateRadps=PreviousFilteredYaw+Alpha*(RawYaw-PreviousFilteredYaw);YawFilterWorld=Now;
            if(!FMath::IsFinite(Alpha)||!FMath::IsFinite(FilteredYawRateRadps)){Finish(false,TEXT("Diagnostic yaw filter arithmetic is nonfinite"));return;}
            const double UnclampedSteer=Error/6. - Sign*Lateral*.01 - FilteredYawRateRadps*1.5;
            const double Steer=FMath::Clamp(UnclampedSteer,-.65,.65);
            LastYawFilterUpdate=MakeShared<FJsonObject>();auto Filter=LastYawFilterUpdate;Filter->SetNumberField(TEXT("pass"),Pass);Filter->SetNumberField(TEXT("worldSeconds"),Now);Filter->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());Filter->SetNumberField(TEXT("frame"),double(GFrameCounter));Filter->SetNumberField(TEXT("tauWorldSeconds"),.05);Filter->SetNumberField(TEXT("previousFilterWorldSeconds"),PreviousFilterWorld);Filter->SetNumberField(TEXT("dtWorldSeconds"),FilterDt);Filter->SetNumberField(TEXT("alpha"),Alpha);Filter->SetBoolField(TEXT("equalWorldTimestampRetainsState"),FilterDt==0);Filter->SetNumberField(TEXT("rawObservedWorldYawRateRadps"),RawYaw);Filter->SetNumberField(TEXT("previousFilteredWorldYawRateRadps"),PreviousFilteredYaw);Filter->SetNumberField(TEXT("filteredWorldYawRateRadps"),FilteredYawRateRadps);Filter->SetNumberField(TEXT("rawHeadingErrorDegrees"),Error);Filter->SetNumberField(TEXT("rawLateralCm"),Lateral);Filter->SetNumberField(TEXT("signedDirection"),Sign);Filter->SetNumberField(TEXT("unclampedSteeringRequest"),UnclampedSteer);Filter->SetNumberField(TEXT("clampedSteeringRequest"),Steer);Filter->SetBoolField(TEXT("commandAccepted"),false);
            if(CurrentFlatAdmissionEvidence)CurrentFlatAdmissionEvidence->SetObjectField(TEXT("diagnosticYawFilterAndLaterSteeringCommand"),Filter);
            // Prospective v15 diagnostic-only fixed launch ramp, identical all8.
            // It replaces the immediate full step; full signed throttle remains
            // required at both endpoints throughout the final measured1s.
            const double Throttle=Sign*FMath::Clamp(PassAge/.5,0.,1.);
            Filter->SetNumberField(TEXT("requestedThrottle"),Throttle);Filter->SetNumberField(TEXT("commandAttemptRealSeconds"),FPlatformTime::Seconds());
            if(Drive(Throttle,Steer,false,true)){Filter->SetBoolField(TEXT("commandAccepted"),true);Filter->SetNumberField(TEXT("acceptedRequestedThrottle"),LastRequestedThrottle);Filter->SetNumberField(TEXT("acceptedRequestedSteer"),LastRequestedSteer);Filter->SetNumberField(TEXT("acceptedCommandWorldSeconds"),LastDriveWorld);Filter->SetNumberField(TEXT("acceptedCommandRealSeconds"),LastDriveReal);Filter->SetNumberField(TEXT("acceptedCommandLeaseSeconds"),LastDriveLeaseSeconds);}return;
        }
        BrakeStartWorld=Now;BrakeStartTravel=Travel;BrakeStartEvidence=MakeShared<FJsonObject>();BrakeStartEvidence->SetNumberField(TEXT("worldSeconds"),Now);BrakeStartEvidence->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds());BrakeStartEvidence->SetNumberField(TEXT("frame"),double(GFrameCounter));BrakeStartEvidence->SetNumberField(TEXT("signedTravelCm"),Travel);BrakeStartEvidence->SetArrayField(TEXT("actualPositionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));BrakeStartEvidence->SetNumberField(TEXT("actualOpticalHeadingDegrees"),(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw);BrakeStartEvidence->SetArrayField(TEXT("actualLinearVelocityCmps"),MayaBoostTest::Vec(O.VelocityCmps));BrakeStartEvidence->SetNumberField(TEXT("actualYawRateRadps"),O.AngularVelocityRadps.Z);BrakeStartEvidence->SetStringField(TEXT("cachedNativeDecisionBeforeBrakeRequest"),Robot->ReadDriveAdmissionEvidenceJson());
        Drive();RunMode=2;StableWorld=-1;BrakeStartEvidence->SetNumberField(TEXT("acceptedRequestedThrottle"),LastRequestedThrottle);BrakeStartEvidence->SetNumberField(TEXT("acceptedRequestedSteer"),LastRequestedSteer);BrakeStartEvidence->SetNumberField(TEXT("acceptedCommandWorldSeconds"),LastDriveWorld);BrakeStartEvidence->SetNumberField(TEXT("acceptedCommandRealSeconds"),LastDriveReal);BrakeStartEvidence->SetBoolField(TEXT("requestedBrake"),true);BrakeStartEvidence->SetBoolField(TEXT("requestedBoost"),false);BrakeStartEvidence->SetStringField(TEXT("scope"),TEXT("Latched actual800cm measurement-crossing body observation and accepted ordinary brake request. Cached motor decision precedes this request; later native brakes/torques retained every stopping tick through Quiet dwell."));
        bSpeedAcceptancePerformed=true;
        double Weight=0,Sum=0,Square=0,First=0,Second=0,WFirst=0,WSecond=0,FullRawThrottleWeight=0;int32 N=0;bool FullRawThrottleWindow=true;
        for(int32 I=1;I<Speeds.Num();++I)
        {
            const auto& A=Speeds[I-1];const auto& B=Speeds[I];const double Begin=FMath::Max(Now-1,A.World),End=FMath::Min(Now,B.World),DtSample=End-Begin,Span=B.World-A.World;
            if(DtSample<=0||Span<=0)continue;
            const bool FullRawEndpoints=A.RequestedThrottle==Sign&&B.RequestedThrottle==Sign;FullRawThrottleWindow=FullRawThrottleWindow&&FullRawEndpoints;
            if(FullRawEndpoints)FullRawThrottleWeight+=DtSample;
            const double Va=FMath::Lerp(A.Speed,B.Speed,(Begin-A.World)/Span),Vb=FMath::Lerp(A.Speed,B.Speed,(End-A.World)/Span);
            Weight+=DtSample;Sum+=(Va+Vb)*.5*DtSample;Square+=(Va*Va+Va*Vb+Vb*Vb)/3.*DtSample;++N;
            const double W1=FMath::Max(0.,FMath::Min(End,Now-.5)-Begin),W2=DtSample-W1,Vm=FMath::Lerp(Va,Vb,W1/DtSample);
            First+=(Va+Vm)*.5*W1;Second+=(Vm+Vb)*.5*W2;WFirst+=W1;WSecond+=W2;
        }
        const double Mean=Weight>0?Sum/Weight:0,Cv=Mean>0?FMath::Sqrt(FMath::Max(0.,Square/Weight-Mean*Mean))/Mean:1;
        const double HalfChange=WFirst>0&&WSecond>0&&Mean>0?FMath::Abs(First/WFirst-Second/WSecond)/Mean:1;
        UE_LOG(LogTemp,Display,TEXT("MAYA BOOST pass measured index=%d mean=%.3fcmps window=%.3fs intervals=%d CV=%.4f halfchange=%.4f ratio=%.4f"),Pass,Mean,Weight,N,Cv,HalfChange,!Previous&&Means[Pass-1]>0?Mean/Means[Pass-1]:0.);
        auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("pass"),Pass);J->SetNumberField(TEXT("actualParcelCount"),Count);J->SetStringField(TEXT("profile"),Previous?TEXT("Previous24radps6Nm_recreated_currentBatteryLaw"):TEXT("New42.4radpsRoad12NmLowSpeedMax18Nm"));J->SetNumberField(TEXT("signedDirection"),Sign);J->SetArrayField(TEXT("startCm"),MayaBoostTest::Vec(RunStart));J->SetNumberField(TEXT("startHeadingDegrees"),RunHeading);J->SetNumberField(TEXT("runSeconds"),Now-RunWorld);J->SetNumberField(TEXT("travelAtBrakeCm"),Travel);J->SetNumberField(TEXT("steadyWindowSeconds"),Weight);J->SetNumberField(TEXT("steadySampleIntervals"),N);J->SetNumberField(TEXT("meanSignedSpeedCmps"),Mean);J->SetNumberField(TEXT("coefficientVariation"),Cv);J->SetNumberField(TEXT("halfMeanRelativeDifference"),HalfChange);J->SetNumberField(TEXT("actualBatteryLossThroughDrive"),BatteryStart-Director->Snapshot().BatteryPercent);J->SetNumberField(TEXT("currentLawObservationIntegratedLoss"),BatteryPredicted);J->SetStringField(TEXT("batteryScope"),TEXT("Current law on both profiles. Post-update observation integration is approximate, not old native battery or same-solver mechanical energy."));Runs.Add(MakeShared<FJsonValueObject>(J));Means[Pass]=Mean;
        TArray<TSharedPtr<FJsonValue>> Window;for(const auto& V:Speeds){auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("worldSeconds"),V.World);Row->SetNumberField(TEXT("signedSpeedCmps"),V.Speed);Row->SetNumberField(TEXT("lastAcceptedRequestedThrottle"),V.RequestedThrottle);Row->SetNumberField(TEXT("lastAcceptedRequestedSteer"),V.RequestedSteer);Row->SetNumberField(TEXT("lastAcceptedCommandWorldSeconds"),V.CommandWorld);Window.Add(MakeShared<FJsonValueObject>(Row));}
        J->SetNumberField(TEXT("fixedFlatLaunchRampSeconds"),.5);J->SetBoolField(TEXT("finalWindowAllSegmentEndpointsFullSignedRawThrottle"),FullRawThrottleWindow);J->SetNumberField(TEXT("finalWindowFullSignedRawThrottleCoveredSeconds"),FullRawThrottleWeight);
        J->SetStringField(TEXT("fullSignedThrottleWindowScope"),TEXT("Every overlapping speed segment uses exact accepted float throttle equal to signed direction at both original observed endpoints, including endpoints outside the clipped1s boundary. Full-throttle-covered duration and original continuous speed coverage must each be>=0.999s. Commands, cached PrePhysics torque decisions and later body observations remain separately timed."));
        J->SetObjectField(TEXT("actualBrakeStart"),BrakeStartEvidence);J->SetArrayField(TEXT("retainedActualSpeedWindow"),Window);J->SetStringField(TEXT("momentIntegration"),TEXT("Observed piecewise-linear speed clipped to the final1s: exact integral v and v-squared; each half independently clipped. Raw native post-update speed samples retained."));J->SetStringField(TEXT("actualNativeAtDriveEnd"),Robot->ReadTelemetryJson());Save(O,true);
        if(!Check(Weight>=.999&&FullRawThrottleWindow&&FullRawThrottleWeight>=.999,TEXT("Final continuous1second measured segments retain exact accepted full signed raw throttle at both endpoints")))
        {Save(O,true);Finish(false,TEXT("Final speed window did not retain full signed raw throttle over continuous1second"));return;}
        const bool FiniteWindowArithmetic=FMath::IsFinite(Weight)&&FMath::IsFinite(Sum)&&FMath::IsFinite(Square)&&FMath::IsFinite(First)&&FMath::IsFinite(Second)&&FMath::IsFinite(WFirst)&&FMath::IsFinite(WSecond)&&FMath::IsFinite(FullRawThrottleWeight)&&FMath::IsFinite(Mean)&&FMath::IsFinite(Cv)&&FMath::IsFinite(HalfChange);
        const bool WindowIntegrity=FiniteWindowArithmetic&&Weight>=.999&&N>=20&&Mean>0&&O.bBoostDriveActive;
        J->SetBoolField(TEXT("speedWindowFiniteArithmetic"),FiniteWindowArithmetic);J->SetBoolField(TEXT("speedWindowIntegrityAccepted"),WindowIntegrity);J->SetBoolField(TEXT("speedWindowQualifiedForRatio"),false);
        J->SetBoolField(TEXT("speedWindowCoefficientVariationAccepted"),FMath::IsFinite(Cv)&&Cv<=.10);J->SetBoolField(TEXT("speedWindowHalfMeansAccepted"),FMath::IsFinite(HalfChange)&&HalfChange<=.05);
        if(!WindowIntegrity)
        {Check(false,TEXT("Final continuous1second actual boosted speed window >=20 intervals/CV<=10%/halfmeans<=5%"));Save(O,true);Finish(false,TEXT("Actual speed-window integrity lacks finite arithmetic, continuous1s,20 intervals, positive mean or admitted Boost"));return;}
        const bool WindowQualityAccepted=Check(Weight>=.999&&N>=20&&Mean>0&&Cv<=.10&&HalfChange<=.05&&O.bBoostDriveActive,TEXT("Final continuous1second actual boosted speed window >=20 intervals/CV<=10%/halfmeans<=5%"));
        SpeedWindowQualified[Pass]=WindowQualityAccepted;J->SetBoolField(TEXT("speedWindowQualifiedForRatio"),WindowQualityAccepted);
        if(!WindowQualityAccepted)
        {
            ++FailedWindowQualityCount;FailedWindowQualityPasses.Add(Pass);
            UE_LOG(LogTemp,Warning,TEXT("MAYA BOOST speed window FAIL pass=%d CV=%.6f halfchange=%.6f failedcount=%d; existing brake/Quiet observations continue, terminal cannotPASS"),Pass,Cv,HalfChange,FailedWindowQualityCount);
        }
        if(!Previous)
        {
            const bool RatioEligible=SpeedWindowQualified[Pass-1]&&SpeedWindowQualified[Pass];
            J->SetNumberField(TEXT("matchedPairIndex"),Pair);J->SetNumberField(TEXT("actualPreviousMeanSignedSpeedCmps"),Means[Pass-1]);J->SetBoolField(TEXT("previousSpeedWindowQualifiedForRatio"),SpeedWindowQualified[Pass-1]);J->SetBoolField(TEXT("speedRatioEligible"),RatioEligible);J->SetBoolField(TEXT("speedRatioAccepted"),false);
            if(!RatioEligible)
            {
                IneligibleRatioPairs.Add(Pair);Check(false,TEXT("Matched actual speed ratio requires two qualified original speed windows"));
                J->SetStringField(TEXT("speedRatioIneligibilityReason"),TEXT("At least one original final1s window failed finite CV/half-mean quality; no eligible or accepted speed ratio"));
                UE_LOG(LogTemp,Warning,TEXT("MAYA BOOST ratio INELIGIBLE pair=%d previousqualified=%d newqualified=%d; no ratio acceptance evaluated"),Pair,int32(SpeedWindowQualified[Pass-1]),int32(SpeedWindowQualified[Pass]));
            }
            else
            {
                const double Ratio=Mean/Means[Pass-1];++ObservedRatioCount;
                const bool RatioAccepted=Check(Ratio>=1.90&&Ratio<=2.10,TEXT("Matched actual new/previous boosted speed ratio1.90..2.10"));
                J->SetNumberField(TEXT("actualNewToPreviousSpeedRatio"),Ratio);J->SetBoolField(TEXT("speedRatioAccepted"),RatioAccepted);
                if(!RatioAccepted)
                {
                    ++FailedRatioCount;FailedRatioPairs.Add(Pair);
                    // An eligible ratio miss remains permanently failed evidence.
                    UE_LOG(LogTemp,Warning,TEXT("MAYA BOOST ratio FAIL pair=%d actual=%.6f accepted=1.90..2.10 failedcount=%d; safe observations continue, terminal cannotPASS"),Pair,Ratio,FailedRatioCount);
                }
            }
        }
    }
    else
    {
        Drive();if(Quiet(O))
        {if(StableWorld<0)StableWorld=Now;if(Now-StableWorld>=.5){Save(O,true);if(!Robot->SelectNativeBoostComparisonProfile(false)){Finish(false,TEXT("Current profile restoration refused"));return;}if(!Robot->IsNativeBoostComparisonProfileReady(false))return;++Pass;RunMode=0;StepWorld=Now;StableWorld=-1;UE_LOG(LogTemp,Display,TEXT("MAYA BOOST pass stopped completed=%d world=%.3f"),Pass,Now);if(Pass==4)Enter(2,Route[0]);else if(Pass==8){if(bStaticBenchmark){Check(Custody(2),TEXT("Eighth actual stopped benchmark retains both secured parcel identities"));Finish(true,TEXT("Density-controlled eight-window/four-ratio benchmark and supported stops completed"));return;}Enter(6,Director->ObjectiveStation()->GetServicePoint());}}}
        else StableWorld=-1;
    }
}
bool AMAYABoostPowerNativeTest::WaitForCurbSetupClearance(const FMAYADeliveryRobotObservation& O)
{
    const double Now=GetWorld()->GetTimeSeconds(),RealNow=FPlatformTime::Seconds();const auto S=Director->Snapshot();
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),Now);J->SetNumberField(TEXT("realSeconds"),RealNow);J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("step"),Step);
    J->SetNumberField(TEXT("startedWorldSeconds"),CurbSetupStartedWorld);J->SetNumberField(TEXT("startedRealSeconds"),CurbSetupStartedReal);J->SetNumberField(TEXT("elapsedWorldSeconds"),Now-CurbSetupStartedWorld);J->SetNumberField(TEXT("elapsedRealSeconds"),RealNow-CurbSetupStartedReal);J->SetNumberField(TEXT("originalStep10ElapsedSeconds"),Now-StepWorld);
    J->SetBoolField(TEXT("actualQuiet"),Quiet(O));J->SetNumberField(TEXT("actualSupportMask"),O.SupportMask);J->SetNumberField(TEXT("actualLinearSpeedCmps"),O.VelocityCmps.Size());J->SetNumberField(TEXT("actualAngularSpeedRadps"),O.AngularVelocityRadps.Size());J->SetStringField(TEXT("actualJobInstanceId"),S.JobInstanceId);J->SetStringField(TEXT("boundJobInstanceId"),JobId);J->SetStringField(TEXT("actualResetGeneration"),LexToString(O.ResetGeneration));J->SetStringField(TEXT("boundResetGeneration"),LexToString(Reset));
    J->SetNumberField(TEXT("acceptedBrakeCommandWorldSeconds"),LastDriveWorld);J->SetNumberField(TEXT("acceptedBrakeCommandRealSeconds"),LastDriveReal);J->SetNumberField(TEXT("acceptedBrakeLeaseSeconds"),LastDriveLeaseSeconds);J->SetNumberField(TEXT("acceptedRequestedThrottle"),LastRequestedThrottle);J->SetNumberField(TEXT("acceptedRequestedSteer"),LastRequestedSteer);J->SetBoolField(TEXT("requestedBrake"),true);J->SetBoolField(TEXT("requestedBoost"),false);J->SetBoolField(TEXT("queryExecuted"),false);
    auto Retain=[&](const TCHAR* Event)
    {J->SetStringField(TEXT("event"),Event);J->SetNumberField(TEXT("clearDwellStartedWorldSeconds"),CurbSetupClearWorld);J->SetNumberField(TEXT("observedClearQuietSeconds"),CurbSetupClearWorld<0?0:Now-CurbSetupClearWorld);++CurbSetupSamplesSeen;if(CurbSetupObservations.Num()<2048)CurbSetupObservations.Add(MakeShared<FJsonValueObject>(J));else bCurbSetupTraceTruncated=true;};
    const bool Identity=IsValid(Robot)&&IsValid(Director)&&IsValid(Curb)&&S.Phase==EMAYADeliveryPhase::Carrying&&S.Job.ParcelCount==1&&S.JobInstanceId==JobId&&O.ResetGeneration==Reset&&Custody(1,false)&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();
    J->SetBoolField(TEXT("actualBoundCustodyJobResetLocksRetained"),Identity);
    J->SetStringField(TEXT("boundParcelId"),ParcelIds[0]);J->SetStringField(TEXT("boundParcelActor"),ParcelActors[0]);J->SetStringField(TEXT("boundParcelBody"),ParcelBodies[0]);if(const auto* Parcel=Director->GetParcel(0)){J->SetStringField(TEXT("actualParcelId"),Parcel->GetParcelId());J->SetStringField(TEXT("actualParcelActor"),Parcel->GetPathName());J->SetStringField(TEXT("actualParcelBody"),GetPathNameSafe(Parcel->GetParcelBody()));}
    if(!Identity||Route.Num()<13||bCurbConfigureCalled){Retain(TEXT("invalid_authority_or_identity"));Finish(false,TEXT("Original curb setup authority/job/reset/custody/physical-lock state changed"));return false;}
    if(Now-CurbSetupStartedWorld>10||RealNow-CurbSetupStartedReal>10){Retain(TEXT("bounded_wait_timeout"));Finish(false,TEXT("Original curb pedestrian preflight exceeded10world/10real seconds"));return false;}
    const FVector Origin=Route[10],CourseAxis=(Route[12]-Route[0]).GetSafeNormal2D(),Outward(CourseAxis.Y,-CourseAxis.X,0);const FVector Center=Origin+Outward*180+FVector(0,0,36);const FQuat Rotation=CourseAxis.Rotation().Quaternion();
    if(Origin.ContainsNaN()||CourseAxis.ContainsNaN()||CourseAxis.IsNearlyZero()||Center.ContainsNaN()||Rotation.ContainsNaN()||!Rotation.IsNormalized()){Retain(TEXT("nonfinite_query_basis"));Finish(false,TEXT("Original curb preflight query basis unavailable"));return false;}
    J->SetArrayField(TEXT("courseOriginCm"),MayaBoostTest::Vec(Origin));J->SetArrayField(TEXT("courseAxis"),MayaBoostTest::Vec(CourseAxis));J->SetArrayField(TEXT("outward"),MayaBoostTest::Vec(Outward));J->SetArrayField(TEXT("queryCenterCm"),MayaBoostTest::Vec(Center));J->SetArrayField(TEXT("queryHalfExtentsCm"),MayaBoostTest::Vec(FVector(80,260,34)));J->SetArrayField(TEXT("queryRotationQuaternion"),{MakeShared<FJsonValueNumber>(Rotation.X),MakeShared<FJsonValueNumber>(Rotation.Y),MakeShared<FJsonValueNumber>(Rotation.Z),MakeShared<FJsonValueNumber>(Rotation.W)});
    J->SetStringField(TEXT("queryChannel"),TEXT("PhysicsBody"));J->SetStringField(TEXT("requestedMobilityType"),TEXT("Static"));J->SetStringField(TEXT("onlyIgnoredActor"),Curb->GetPathName());J->SetBoolField(TEXT("queryExecuted"),true);
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYACurbCorridorVolume),false);Q.MobilityType=EQueryMobilityType::Static;Q.AddIgnoredActor(Curb);
    TArray<FOverlapResult> Obstacles;GetWorld()->OverlapMultiByChannel(Obstacles,Center,Rotation,ECC_PhysicsBody,FCollisionShape::MakeBox(FVector(80,260,34)),Q);
    UClass* CrowdClass=FindObject<UClass>(nullptr,TEXT("/Script/CitySample.CitySampleCrowdCharacter"));const bool ClassKnown=CrowdClass&&CrowdClass->IsChildOf(AActor::StaticClass());J->SetBoolField(TEXT("actualNativeCrowdClassFound"),ClassKnown);J->SetStringField(TEXT("requestedNativeCrowdClassPath"),TEXT("/Script/CitySample.CitySampleCrowdCharacter"));if(ClassKnown)J->SetStringField(TEXT("actualResolvedNativeCrowdClassPath"),CrowdClass->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Blockers;bool AllVerifiedCrowd=true;
    for(const auto& Hit:Obstacles)
    {
        UPrimitiveComponent* C=Hit.GetComponent();
        if(!C){auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("componentKnown"),false);B->SetBoolField(TEXT("verifiedMovableNativeCrowd"),false);Blockers.Add(MakeShared<FJsonValueObject>(B));AllVerifiedCrowd=false;continue;}
        if(!C->IsPhysicsCollisionEnabled()||C->GetCollisionResponseToChannel(ECC_PhysicsBody)!=ECR_Block)continue;
        AActor* A=C->GetOwner();const bool Verified=IsValid(A)&&ClassKnown&&A->IsA(CrowdClass)&&C->Mobility==EComponentMobility::Movable;
        auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("componentKnown"),true);B->SetStringField(TEXT("componentPath"),C->GetPathName());B->SetStringField(TEXT("componentClass"),C->GetClass()->GetPathName());B->SetStringField(TEXT("ownerPath"),GetPathNameSafe(A));B->SetStringField(TEXT("ownerClass"),A?A->GetClass()->GetPathName():FString());B->SetNumberField(TEXT("actualComponentMobility"),int32(C->Mobility));B->SetBoolField(TEXT("verifiedMovableNativeCrowd"),Verified);B->SetBoolField(TEXT("physicsCollisionEnabled"),C->IsPhysicsCollisionEnabled());B->SetNumberField(TEXT("responsePhysicsBody"),int32(C->GetCollisionResponseToChannel(ECC_PhysicsBody)));B->SetNumberField(TEXT("overlapItemIndex"),Hit.ItemIndex);
        const FTransform Pose=C->GetComponentTransform();const FVector Velocity=C->GetComponentVelocity();B->SetBoolField(TEXT("finiteObservedComponentPose"),!Pose.ContainsNaN());B->SetBoolField(TEXT("finiteReportedComponentVelocity"),!Velocity.ContainsNaN());if(!Pose.ContainsNaN())B->SetArrayField(TEXT("observedComponentPositionCm"),MayaBoostTest::Vec(Pose.GetLocation()));if(!Velocity.ContainsNaN())B->SetArrayField(TEXT("reportedComponentVelocityCmps"),MayaBoostTest::Vec(Velocity));
        const FBodyInstance* NativeBody=C->GetBodyInstance(NAME_None,true,Hit.ItemIndex);const FBox Bounds=NativeBody&&NativeBody->IsValidBodyInstance()?NativeBody->GetBodyBounds():FBox(ForceInit);const bool BoundsKnown=Bounds.IsValid&&!Bounds.Min.ContainsNaN()&&!Bounds.Max.ContainsNaN();B->SetBoolField(TEXT("actualNativeBoundsKnown"),BoundsKnown);if(BoundsKnown){B->SetArrayField(TEXT("actualNativeBoundsMinCm"),MayaBoostTest::Vec(Bounds.Min));B->SetArrayField(TEXT("actualNativeBoundsMaxCm"),MayaBoostTest::Vec(Bounds.Max));}
        B->SetStringField(TEXT("motionObservationScope"),TEXT("Synchronized component pose/reported velocity and actual overlap-body bounds when available. Observations only, no clearance prediction or actor motion setter."));Blockers.Add(MakeShared<FJsonValueObject>(B));AllVerifiedCrowd=AllVerifiedCrowd&&Verified;
    }
    J->SetArrayField(TEXT("actualBlockingComponents"),Blockers);J->SetNumberField(TEXT("actualBlockingComponentCount"),Blockers.Num());J->SetBoolField(TEXT("allBlockersVerifiedMovableNativeCrowd"),AllVerifiedCrowd);
    if(!AllVerifiedCrowd){CurbSetupClearWorld=-1;Retain(TEXT("noncrowd_static_or_unknown_blocker"));Finish(false,TEXT("Original curb preflight contains a static/noncrowd/unknown blocker"));return false;}
    if(Blockers.Num()>0||!Quiet(O)){CurbSetupClearWorld=-1;Retain(Blockers.Num()>0?TEXT("verified_pedestrian_occupied"):TEXT("quiet_lost"));return false;}
    if(CurbSetupClearWorld<0)CurbSetupClearWorld=Now;const bool Admitted=Now-CurbSetupClearWorld>=.5;Retain(Admitted?TEXT("clear_quiet_dwell_admitted"):TEXT("clear_quiet_dwell"));return Admitted;
}
void AMAYABoostPowerNativeTest::Tick(float Dt)
{
    Super::Tick(Dt);if(bFinished)return;FMAYADeliveryRobotObservation O;if(!Robot||!Director||!Robot->ReadDeliveryState(O)){Finish(false,TEXT("Actual authority/state unavailable"));return;}
    const double Now=GetWorld()->GetTimeSeconds();const auto S=Director->Snapshot();
    if((bCurbOnly||bStaticBenchmark||bServiceOnly)&&!VerifySpawnDensity())
    {Check(false,TEXT("Actual effective ambient spawn density retains selected scope"));Save(O,true);Finish(false,TEXT("Actual ambient spawn density changed or became unavailable"));return;}
    if(bStaticBenchmark&&(LastPopulationWorld<0||Now-LastPopulationWorld>=.5))ObserveAmbientPopulation(TEXT("throughout_diagnostic"));
    const double OuterGuardRealS=FPlatformTime::Seconds();const bool OuterGuardWorldPaused=GetWorld()->IsPaused();
    if(Now-StartedWorld>600||OuterGuardRealS-StartedReal>720||OuterGuardWorldPaused||!O.Fault.IsEmpty()||S.Phase==EMAYADeliveryPhase::Failed||S.BatteryPercent<=0||S.bShiftWon)
    {
        auto G=MakeShared<FJsonObject>();G->SetStringField(TEXT("schema"),TEXT("MayaRobotics.BoostOuterGuardFailure.v1"));
        G->SetStringField(TEXT("scope"),TEXT("Existing Tick local robot observation/snapshot and guard clock operands captured before Save/Finish brake/release. Individual original predicates only; no inferred failure cause or changed acceptance. Game-thread receipt, not an identified solver step."));
        G->SetNumberField(TEXT("worldSeconds"),Now);G->SetNumberField(TEXT("realSeconds"),OuterGuardRealS);G->SetNumberField(TEXT("frame"),double(GFrameCounter));G->SetNumberField(TEXT("step"),Step);G->SetNumberField(TEXT("pass"),Pass);G->SetNumberField(TEXT("runMode"),RunMode);
        G->SetNumberField(TEXT("startedWorldSeconds"),StartedWorld);G->SetNumberField(TEXT("startedRealSeconds"),StartedReal);G->SetNumberField(TEXT("gameplayElapsedSeconds"),Now-StartedWorld);G->SetNumberField(TEXT("realElapsedSeconds"),OuterGuardRealS-StartedReal);
        G->SetBoolField(TEXT("worldTimeGuardTriggered"),Now-StartedWorld>600);G->SetBoolField(TEXT("realTimeGuardTriggered"),OuterGuardRealS-StartedReal>720);G->SetBoolField(TEXT("worldPausedGuardTriggered"),OuterGuardWorldPaused);
        G->SetBoolField(TEXT("robotFaultGuardTriggered"),!O.Fault.IsEmpty());G->SetBoolField(TEXT("failedPhaseGuardTriggered"),S.Phase==EMAYADeliveryPhase::Failed);G->SetBoolField(TEXT("batteryGuardTriggered"),S.BatteryPercent<=0);G->SetBoolField(TEXT("shiftWonGuardTriggered"),S.bShiftWon);
        const TCHAR* PhaseNames[]={TEXT("Available"),TEXT("Accepted"),TEXT("Loading"),TEXT("Carrying"),TEXT("Unloading"),TEXT("Succeeded"),TEXT("Failed")};const uint8 PhaseIndex=uint8(S.Phase);
        G->SetNumberField(TEXT("actualSnapshotPhaseEnum"),PhaseIndex);G->SetStringField(TEXT("actualSnapshotPhase"),PhaseIndex<UE_ARRAY_COUNT(PhaseNames)?PhaseNames[PhaseIndex]:TEXT("Unknown"));G->SetStringField(TEXT("actualSnapshotStatus"),S.Status);G->SetStringField(TEXT("actualRuntimeStatus"),Director->GetRuntimeStatus());
        G->SetBoolField(TEXT("actualWorldPaused"),OuterGuardWorldPaused);G->SetBoolField(TEXT("actualDirectorPaused"),Director->IsPaused());G->SetBoolField(TEXT("actualShiftWon"),S.bShiftWon);G->SetBoolField(TEXT("actualShiftStarted"),S.bShiftStarted);
        G->SetNumberField(TEXT("actualBatteryPercent"),S.BatteryPercent);G->SetNumberField(TEXT("actualCargoConditionPercent"),S.CargoConditionPercent);G->SetNumberField(TEXT("actualJobElapsedSeconds"),S.ElapsedSeconds);G->SetStringField(TEXT("actualJobInstanceId"),S.JobInstanceId);G->SetNumberField(TEXT("actualCompletedJobs"),S.CompletedJobs);
        G->SetStringField(TEXT("actualObservationFault"),O.Fault);G->SetNumberField(TEXT("actualObservationWorldSeconds"),O.ObservationWorldSeconds);G->SetStringField(TEXT("actualResetGeneration"),LexToString(O.ResetGeneration));G->SetBoolField(TEXT("actualKnown"),O.bKnown);G->SetBoolField(TEXT("actualReady"),O.bReady);G->SetBoolField(TEXT("actualGrounded"),O.bGrounded);G->SetBoolField(TEXT("actualUpright"),O.bUpright);G->SetNumberField(TEXT("actualSupportMask"),O.SupportMask);
        G->SetArrayField(TEXT("actualPositionCm"),MayaBoostTest::Vec(O.ChassisPose.GetLocation()));G->SetArrayField(TEXT("actualVelocityCmps"),MayaBoostTest::Vec(O.VelocityCmps));G->SetArrayField(TEXT("actualAngularVelocityRadps"),MayaBoostTest::Vec(O.AngularVelocityRadps));G->SetBoolField(TEXT("actualDoorsOpen"),O.bDoorsOpen);G->SetBoolField(TEXT("actualDoorsClosedLatched"),O.bDoorsClosedLatched);G->SetBoolField(TEXT("actualBoostDriveActive"),O.bBoostDriveActive);
        auto V=MakeShared<FJsonObject>();V->SetBoolField(TEXT("actualDroneTotalled"),S.bDroneTotalled);V->SetBoolField(TEXT("actualVehicleContactPending"),S.bVehicleContactPending);V->SetBoolField(TEXT("actualCrashFrozen"),S.VehicleCrash.bFrozen);V->SetStringField(TEXT("actualCrashReason"),S.VehicleCrash.Reason);
        const auto& C=S.VehicleCrash.Contact;V->SetStringField(TEXT("contactGeneration"),LexToString(C.Generation));V->SetNumberField(TEXT("contactWorldSeconds"),C.WorldSeconds);V->SetStringField(TEXT("vehicleActor"),C.VehicleActor);V->SetStringField(TEXT("vehicleComponent"),C.VehicleComponent);V->SetStringField(TEXT("robotComponent"),C.RobotComponent);V->SetStringField(TEXT("contactOutcome"),C.Outcome);
        V->SetBoolField(TEXT("nativeContact"),C.bNativeContact);V->SetBoolField(TEXT("blockingHit"),C.bBlockingHit);V->SetBoolField(TEXT("startPenetrating"),C.bStartPenetrating);V->SetBoolField(TEXT("vehicleIdentityKnown"),C.bVehicleIdentityKnown);V->SetBoolField(TEXT("sameNativeBody"),C.bSameNativeBody);V->SetBoolField(TEXT("historyKnown"),C.bHistoryKnown);G->SetObjectField(TEXT("actualVehicleCrashSnapshot"),V);
        G->SetStringField(TEXT("actualVehicleContactEvidenceBeforeTerminalActions"),Director->ReadVehicleContactEvidenceJson());
        OuterGuardEvidence=G;Save(O,true);Finish(false,TEXT("Actual boost trial600game/720real/phase/energy/fault guard refused"));return;
    }
    if(Step>0&&(!O.bKnown||!O.bReady||O.ResetGeneration!=Reset||O.ChassisPose.ContainsNaN()||O.VelocityCmps.ContainsNaN()||O.AngularVelocityRadps.ContainsNaN()))
    {Save(O,true);Finish(false,TEXT("Fresh native known/ready/reset/finite state changed"));return;}
    if(S.Phase==EMAYADeliveryPhase::Carrying&&(!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()||((Step==5||Step==6||Step==11)&&!Custody(S.Job.ParcelCount))))
    {Save(O,true);Finish(false,TEXT("Actual moving physical cargo lock/identity/restraint changed"));return;}
    PollCapture();if(bFinished)return;
    if(Step==1||Step==5)
    {if(!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()){Finish(false,TEXT("Flat moving physical lid lock changed"));return;}RunPass(O);}
    else if(Step==0)
    {
        Drive();if(Now-StepWorld>20){Finish(false,TEXT("Initial native stabilization exceeded20seconds"));return;}
        if(Quiet(O)&&S.Phase==EMAYADeliveryPhase::Available)
        {if(StableWorld<0)StableWorld=Now;if(Now-StableWorld>=2){Reset=O.ResetGeneration;if(!Check(Custody(0),TEXT("Actual empty baseline"))){Finish(false,(bCurbOnly||bServiceOnly)?TEXT("Actual empty initial custody refused"):TEXT("Actual original flat course admission failed"));return;}if(bCurbOnly){Enter(8);return;}if(bServiceOnly){Enter(3);return;}if(!Check(SurveyFlat(O),TEXT("Fresh original full-width level speed course and raised body corridor"))){Finish(false,TEXT("Actual original flat course admission failed"));return;}bSurveyed=true;Enter(1);}}
        else StableWorld=-1;
    }
    else if(Step==2||Step==6||Step==9)
    {
        if(Position(O,15,false))
        {if(Step==2)Enter(3);else if(Step==6){if(!Director->RequestInteract()){Finish(false,TEXT("Actual two-parcel dropoff request refused"));return;}Enter(7);}else{if(!Director->RequestInteract()){Finish(false,TEXT("Actual one-parcel pickup request refused"));return;}Enter(10);}}
    }
    else if(Step==3)
    {
        Drive();if(!Quiet(O)){if(Now-StepWorld>10)Finish(false,TEXT("Depot service readiness did not settle"));return;}
        if(!bRequested){if(!Director->RequestSelectOffer(1)||!Director->RequestSelectOffer(1)||Director->Snapshot().Job.ParcelCount!=2||!Director->RequestAccept()||!Director->RequestInteract()){Finish(false,TEXT("Ordinary two-parcel offer/accept/pickup refused"));return;}bRequested=true;Enter(4);}
    }
    else if(Step==4||Step==10)
    {
        Drive();if(Now-StepWorld>25){Finish(false,TEXT("Actual cargo loading/stopped endpoint exceeded25seconds"));return;}
        const int32 Count=Step==4?2:1;
        if(Step==10&&bCurbSetupStarted)
        {
            if(WaitForCurbSetupClearance(O))
            {auto* Arm=Robot->FindComponentByClass<UMAYAStableCameraArm>();if(!Arm||!Robot->SetPlayerCameraPov(false)){Finish(false,TEXT("Actual ordinary chase camera unavailable for curb three-quarter evidence"));return;}
                Arm->SetViewAngles(-20.f,65.f);bCurbConfigureCalled=true;bCurbConfigurePassed=Curb->Configure(Robot,Director,O);if(!bCurbConfigurePassed){Finish(false,Curb->FailureReason());return;}Enter(11);}
        }
        else if(S.Phase==EMAYADeliveryPhase::Carrying&&Quiet(O)&&Director->GetSecuredParcelCount()==Count)
        {if(!Check(Custody(Count,true),TEXT("Actual accepted parcel IDs/bodies/restraints bound after normal air pickup"))){Finish(false,TEXT("Physical custody bind refused"));return;}JobId=S.JobInstanceId;
            if(Step==4){if(bServiceOnly)Enter(6,Director->ObjectiveStation()->GetServicePoint());else Enter(5);}else
            {Curb=GetWorld()->SpawnActor<AMAYAV01CurbTrial>();if(!Curb){Finish(false,TEXT("Unconfigured curb actor unavailable"));return;}bCurbSetupStarted=true;CurbSetupStartedWorld=Now;CurbSetupStartedReal=FPlatformTime::Seconds();WaitForCurbSetupClearance(O);}}
    }
    else if(Step==7)
    {
        Drive();if(Now-StepWorld>25){Finish(false,TEXT("Actual two-parcel unloading did not complete"));return;}
        if(S.Phase==EMAYADeliveryPhase::Succeeded&&Quiet(O)&&Director->GetParcelCount()==0)
        {if(!Check(S.CompletedJobs>=1,TEXT("Real two-parcel ordinary delivery completed before one-parcel curb trial"))){Finish(false,TEXT("Actual completed delivery missing"));return;}SucceededBefore=S;Enter(13,Route[0]+Axis*1300);}
    }
    else if(Step==8)
    {
        Drive();if(Director->Snapshot().Job.ProfileId!=TEXT("city_1")){if(!Director->RequestSelectOffer(-1)){Finish(false,TEXT("Ordinary one-parcel offer selection refused"));return;}return;}
        if(!Director->RequestAccept()){Finish(false,TEXT("Actual one-parcel acceptance refused"));return;}Enter(9,Route[0]);
    }
    else if(Step==11)
    {
        if(!Custody(1)){Finish(false,TEXT("Original one-parcel ascent custody changed"));return;}
        if(bCurbComplete){Drive();if(PendingCapture.IsEmpty()){Check(Captures.Num()==3,TEXT("Three actual processed normal-blocked/braked, Boost-ascent and upper-hold PNGs"));Finish(true,bCurbOnly?TEXT("Ordinary-city actual one-parcel original-curb reverse ascent and three processed captures completed"):TEXT("Bounded actual matched boost-speed and original-curb reverse-ascent trial completed"));}return;}
        bCurbAcceptancePerformed=true;const auto Outcome=Curb->Advance(O);if(Outcome==AMAYAV01CurbTrial::EOutcome::Failed){Save(O,true);Finish(false,Curb->FailureReason());return;}
        const FString CurbStep=Curb->ReadCurrentStageName();
        if((CurbStep==TEXT("BrakeBeforeBoost")||CurbStep==TEXT("BoostAscent")||CurbStep==TEXT("UpperHold"))&&!CapturedSteps.Contains(CurbStep)&&PendingCapture.IsEmpty())
        {Capture(CurbStep);CapturedSteps.Add(CurbStep);}
        if(Outcome==AMAYAV01CurbTrial::EOutcome::Complete){Check(true,TEXT("Original curb normal-powered bounded no-progress then real brake/90-65 governed Boost reverse ascent with separate actual stalled-or-moving energy, six whole upper wheel contacts and stopped hold"));bCurbComplete=true;return;}
    }
    else if(Step==12||Step==13)
    {
        const auto& A=SucceededBefore.Result;const auto& B=S.Result;const double Age=Now-StepWorld;
        const bool Frozen=S.Phase==EMAYADeliveryPhase::Succeeded&&S.TotalCredits==SucceededBefore.TotalCredits&&S.CompletedJobs==SucceededBefore.CompletedJobs
            &&S.ElapsedSeconds==SucceededBefore.ElapsedSeconds&&S.TravelCm==SucceededBefore.TravelCm&&S.RechargeFeesPaidDollars==SucceededBefore.RechargeFeesPaidDollars
            &&A.JobInstanceId==B.JobInstanceId&&A.Title==B.Title&&A.Reason==B.Reason&&A.bSuccess==B.bSuccess&&A.Credits==B.Credits
            &&A.ElapsedSeconds==B.ElapsedSeconds&&A.BatteryPercent==B.BatteryPercent&&A.CargoConditionPercent==B.CargoConditionPercent&&A.Recoveries==B.Recoveries;
        if(!Frozen||(Step==12&&(Age>6||FVector::Dist2D(RunStart,O.ChassisPose.GetLocation())>500||O.SupportMask!=63))||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked())
        {Save(O,true);Finish(false,TEXT("Actual Succeeded free-drive result/cash/support/500cm/6s guard refused"));return;}
        if(Step==13){if(Position(O,15,true)){SucceededBefore=S;RunStart=O.ChassisPose.GetLocation();Enter(12);}return;}
        bSucceededBoostSeen|=O.bBoostDriveActive&&O.BoostMotorEffortFraction>0;
        if(Age<2){const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,Axis.Rotation().Yaw);if(Drive(-.5,FMath::Clamp(Error/40.,-.35,.35),false,true))bSucceededProbePerformed=true;}
        else
        {Drive();if(Quiet(O)){if(StableWorld<0)StableWorld=Now;if(Now-StableWorld>=.5){bSucceededProbePassed=Check(bSucceededBoostSeen&&SucceededBefore.BatteryPercent-S.BatteryPercent>.01*Age,TEXT("Actual Succeeded admitted Boost consumes live energy while result/cash/deadline remain frozen"));Save(O,true);if(bServiceOnly){Finish(bSucceededProbePassed,bSucceededProbePassed?TEXT("Ordinary-city actual two-parcel delivery/Succeeded Boost energy probe and supported stop completed"):TEXT("Succeeded energy probe or actual Next request failed"));return;}if(!bSucceededProbePassed||!Director->RequestNext(false)){Finish(false,TEXT("Succeeded energy probe or actual Next request failed"));return;}Enter(8);}}else StableWorld=-1;}
    }
    Save(O);
}
void AMAYABoostPowerNativeTest::ScreenshotProcessed()
{
    if(PendingCapture.IsEmpty())return;
    bCaptureProcessed=true;CaptureProcessedFrame=GFrameCounter;CaptureProcessedWorld=GetWorld()->GetTimeSeconds();
    ProcessedCaptureView=MakeShared<FJsonObject>();ProcessedCaptureView->SetNumberField(TEXT("readbackFrame"),double(GFrameCounter));ProcessedCaptureView->SetNumberField(TEXT("readbackWorldSeconds"),CaptureProcessedWorld);
    ProcessedCaptureView->SetStringField(TEXT("actualPawnCamera"),Robot->ReadPlayerCameraModeJson());
    if(auto* Arm=Robot->FindComponentByClass<UMAYAStableCameraArm>())ProcessedCaptureView->SetStringField(TEXT("actualStableArm"),Arm->ReadEvidenceJson());
    if(auto* PC=GetWorld()->GetFirstPlayerController()){FVector View;FRotator Rotation;PC->GetPlayerViewPoint(View,Rotation);ProcessedCaptureView->SetArrayField(TEXT("actualPlayerViewCm"),MayaBoostTest::Vec(View));ProcessedCaptureView->SetArrayField(TEXT("actualPlayerViewPitchYawRoll"),MayaBoostTest::Vec(FVector(Rotation.Pitch,Rotation.Yaw,Rotation.Roll)));ProcessedCaptureView->SetStringField(TEXT("actualViewTarget"),GetPathNameSafe(PC->GetViewTarget()));}
}
void AMAYABoostPowerNativeTest::Capture(const FString& Name)
{PendingCapture=FPaths::Combine(Output,Name+TEXT(".png"));CaptureRequestFrame=GFrameCounter;CaptureRequestWorld=GetWorld()->GetTimeSeconds();CaptureStartedReal=FPlatformTime::Seconds();bCaptureProcessed=false;ProcessedCaptureView.Reset();FScreenshotRequest::RequestScreenshot(PendingCapture,true,false);}
bool AMAYABoostPowerNativeTest::PollCapture()
{
    if(PendingCapture.IsEmpty())return true;
    if(FPlatformTime::Seconds()-CaptureStartedReal>8){Finish(false,TEXT("Actual curb screenshot did not process within8real seconds"));return false;}
    if(!bCaptureProcessed||IFileManager::Get().FileSize(*PendingCapture)<=64)return false;
    TArray<uint8> Bytes;const uint8 Signature[]={137,80,78,71,13,10,26,10};const bool Read=FFileHelper::LoadFileToArray(Bytes,*PendingCapture);
    if(!Check(Read&&Bytes.Num()>24&&FMemory::Memcmp(Bytes.GetData(),Signature,8)==0&&CaptureProcessedFrame>=CaptureRequestFrame,TEXT("Actual processed original-curb PNG: ")+FPaths::GetBaseFilename(PendingCapture))){Finish(false,TEXT("Actual curb PNG invalid"));return false;}
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("path"),PendingCapture);J->SetNumberField(TEXT("requestFrame"),double(CaptureRequestFrame));J->SetNumberField(TEXT("processedFrame"),double(CaptureProcessedFrame));J->SetNumberField(TEXT("requestWorldSeconds"),CaptureRequestWorld);J->SetNumberField(TEXT("processedWorldSeconds"),CaptureProcessedWorld);
    if(ProcessedCaptureView)J->SetObjectField(TEXT("actualScreenshotProcessedCamera"),ProcessedCaptureView);
    J->SetStringField(TEXT("cameraRequest"),TEXT("Existing stable chase arm pitch-20/yawoffset65 for actual three-quarter curb pixels; robot/cargo transforms unchanged."));Captures.Add(MakeShared<FJsonValueObject>(J));PendingCapture.Reset();return true;
}
void AMAYABoostPowerNativeTest::Finish(bool Success,const FString& Reason)
{
    if(bFinished)return;const bool NearCorrectionPendingBeforeTerminal=bFlatNearBearingCorrectionPending;bFinished=true;bFlatNearBearingCorrectionPending=false;Drive();SetActorTickEnabled(false);
    const FString TerminalReason=Success&&FailedWindowQualityCount>0?TEXT("Bounded trial observations completed; original speed-window acceptance failed"):(Success&&FailedRatioCount>0?TEXT("Bounded trial observations completed; doubled boosted road-speed ratio acceptance failed"):Reason);
    int32 QualifiedWindowCount=0;for(const bool Qualified:SpeedWindowQualified)if(Qualified)++QualifiedWindowCount;
    if(bCurbOnly)Success=Success&&bCurbComplete&&Captures.Num()==3&&Custody(1);
    else if(bServiceOnly)Success=Success&&bSucceededProbePerformed&&bSucceededProbePassed&&Director&&Director->Snapshot().Phase==EMAYADeliveryPhase::Succeeded&&Custody(0);
    else{Success=Success&&QualifiedWindowCount==8&&FailedWindowQualityCount==0&&IneligibleRatioPairs.Num()==0&&FailedRatioCount==0;if(bStaticBenchmark)Success=Success&&Pass==8&&ObservedRatioCount==4&&Custody(2);}
    if(bStaticBenchmark){VerifySpawnDensity();ObserveAmbientPopulation(TEXT("terminal"),true);bool DensityAccepted=false;LastSpawnDensityEvidence->TryGetBoolField(TEXT("selectedScopeDensityAccepted"),DensityAccepted);Success=Success&&DensityAccepted;}
    for(const auto& V:Checks){bool Passed=false;V->AsObject()->TryGetBoolField(TEXT("passed"),Passed);Success=Success&&Passed;}
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),bCurbOnly?TEXT("MayaRobotics.BoostCurbOnlyNativeTest.v4"):bStaticBenchmark?TEXT("MayaRobotics.BoostStaticBenchmarkNativeTest.v1"):bServiceOnly?TEXT("MayaRobotics.BoostServiceOnlyNativeTest.v1"):TEXT("MayaRobotics.BoostPowerNativeTest.v4"));J->SetStringField(TEXT("phase"),bCurbOnly?TEXT("NativeBoostCurbOnly"):bStaticBenchmark?TEXT("NativeBoostStaticBenchmark"):bServiceOnly?TEXT("NativeBoostServiceOnly"):TEXT("NativeBoostPowerTest"));J->SetBoolField(TEXT("terminal"),true);J->SetBoolField(TEXT("success"),Success);J->SetStringField(TEXT("reason"),TerminalReason);J->SetBoolField(TEXT("humanPhysicalInputVerified"),false);J->SetBoolField(TEXT("flatNearBearingCorrectionPendingBeforeTerminal"),NearCorrectionPendingBeforeTerminal);J->SetBoolField(TEXT("flatNearBearingCorrectionPendingAfterTerminal"),bFlatNearBearingCorrectionPending);J->SetStringField(TEXT("actualSettingsPath"),ActualSettingsPath);J->SetNumberField(TEXT("step"),Step);J->SetNumberField(TEXT("completedFlatPasses"),Pass);J->SetNumberField(TEXT("gameplayElapsedSeconds"),GetWorld()->GetTimeSeconds()-StartedWorld);J->SetNumberField(TEXT("realElapsedSeconds"),FPlatformTime::Seconds()-StartedReal);J->SetArrayField(TEXT("checks"),Checks);J->SetArrayField(TEXT("flatPasses"),Runs);J->SetArrayField(TEXT("freshOriginalFlatSurvey"),Survey);J->SetArrayField(TEXT("actualSamples"),Samples);
    J->SetStringField(TEXT("scope"),TEXT("Explicit same-build previous24radps6Nm motor recreation versus new42.4radps road12Nm/low-speed max18Nm; both use current battery law. Actual old/new forward/reverse empty/two-parcel8m passes require final1s steady-window and ratios1.90..2.10. Real two-parcel pickup/delivery and one-parcel original-curb reverse ascent after safe road descent with complete condition accounting (legacy landing-damage calibration reported separately, not required) with diagnostic90/65 speed hysteresis before permanent upper coast and separate actual stalled-or-moving Boost energy. Native automation callbacks/motor leases, not physical keyboard input. No resource/pose/velocity/event/geometry/physics setter. Applied torque cache and later velocity are separately sampled; no force-balance/power or universal curb guarantee."));
    if(bCurbOnly)J->SetStringField(TEXT("scope"),TEXT("Ordinary startup original-city one-parcel safe road descent with complete condition accounting and separately reported non-required legacy landing calibration, followed by normal-powered bounded no-progress/real-brake/current42.4radps road12Nm/low-speed max18Nm Boost reverse curb ascent with diagnostic90/65 speed hysteresis before permanent upper coast, separate actual stalled-or-moving Boost energy and three processed captures. Flat speed comparisons and Succeeded probe omitted, not accepted. Original physical driving/service/custody/support/collision/curb guards retained."));
    else if(bStaticBenchmark)J->SetStringField(TEXT("scope"),TEXT("Original Small City under early global requested Mass spawn density zero; paired old24radps6Nm/current42.4radps road12Nm/low-speed max18Nm empty/two-actual-parcel forward/reverse speed windows, four ratios and eight native brake/Quiet stops. Actual two-parcel pickup retained, delivery/Succeeded/curb omitted. Count-based crowd/traffic spawning affected; parked/intersection exceptions may remain collidable. Controlled CPU/frame conditions, not ordinary live-crowd gameplay or within-process restoration. No resource/pose/velocity/event/collision/geometry/physics setter."));
    else if(bServiceOnly)J->SetStringField(TEXT("scope"),TEXT("Ordinary startup original-city actual two-parcel offer/accept/pickup, physical objective approach/unload, genuine Succeeded, current Boost live-energy/frozen-result probe and native brake/Quiet endpoint. Flat speed comparisons and curb omitted, not accepted. No ambient population override or fabricated completion/resource/pose/velocity/event setter."));
    J->SetStringField(TEXT("diagnosticMode"),bCurbOnly?TEXT("CurbOnly"):bStaticBenchmark?TEXT("StaticBenchmark"):bServiceOnly?TEXT("ServiceOnly"):TEXT("Full"));J->SetStringField(TEXT("actualStartupCommandLine"),FCommandLine::Get());
    if(OuterGuardEvidence)J->SetObjectField(TEXT("actualOuterGuardFailure"),OuterGuardEvidence);
    J->SetBoolField(TEXT("curbSetupPreflightStarted"),bCurbSetupStarted);J->SetBoolField(TEXT("originalCurbConfigureCalled"),bCurbConfigureCalled);J->SetBoolField(TEXT("originalCurbConfigurePassed"),bCurbConfigurePassed);J->SetNumberField(TEXT("curbSetupSamplesSeen"),CurbSetupSamplesSeen);J->SetNumberField(TEXT("curbSetupSamplesRetained"),CurbSetupObservations.Num());J->SetBoolField(TEXT("curbSetupTraceTruncated"),bCurbSetupTraceTruncated);J->SetArrayField(TEXT("actualCurbSetupClearanceObservations"),CurbSetupObservations);
    J->SetStringField(TEXT("curbSetupPreflightScope"),TEXT("Step10 only after once-bound real one-parcel closed/braked Quiet pickup. Identical original raised box/query/selfignore; wait only for actualMovable verified native CitySample crowd blockers, never ignore/move them.10world AND10real bound inside original25s Step10/global clocks; actual clear+Quiet .5world dwell. Unknown/noncrowd/static blocker or identity change fails. One original fresh Configure then unchanged curb mechanics; preflight is not curb motion, climb or final-surface acceptance. Trace cap2048; component motion readback is not prediction."));
    J->SetBoolField(TEXT("speedAcceptancePerformed"),bSpeedAcceptancePerformed);J->SetBoolField(TEXT("SucceededProbePerformed"),bSucceededProbePerformed);J->SetBoolField(TEXT("curbAcceptancePerformed"),bCurbAcceptancePerformed);
    if(LastSpawnDensityEvidence)J->SetObjectField(TEXT("actualEffectiveSpawnDensity"),LastSpawnDensityEvidence);J->SetArrayField(TEXT("actualAmbientPopulationObservations"),AmbientPopulationObservations);J->SetNumberField(TEXT("ambientPopulationSamplesSeen"),AmbientPopulationSamplesSeen);J->SetNumberField(TEXT("ambientPopulationSamplesRetained"),AmbientPopulationObservations.Num());J->SetBoolField(TEXT("ambientPopulationTraceTruncated"),bAmbientPopulationTraceTruncated);
    J->SetStringField(TEXT("ambientPopulationObservationScope"),TEXT("Static benchmark counts actual CitySample crowd actors at initialization, each launch, at most0.5world-second intervals and terminal; read-only Mass crowd/vehicle/parked/intersection tag inventory at initialization, first launch/previous-profile comparison blocks and terminal. Cap2048 with explicit truncation. Sampled observations, not every solver frame or a universal absence claim."));
    J->SetBoolField(TEXT("actualSucceededBoostProbePassed"),bSucceededProbePassed);
    J->SetBoolField(TEXT("flatProfileSelectionAcknowledgementPending"),bFlatProfileSelectionPending);
    J->SetStringField(TEXT("nativeMotorOwnershipComparisonScope"),TEXT("Genuine previous24/6 external motor/bearing/brake route compared with permanent production solver-callback feedback. Selector transitions require actual braked Quiet and completed nonfuture zero-motor/native-brake acknowledgement with fenced retirement. Prelaunch pending ownership holds ordinary brake within original40s setup, then rechecks original position/clearance before RunWorld. Current-owner restoration after each measured stop is acknowledged before advancing Pass under original15s pass limit. Normal/current power laws, measured windows and ratio bounds unchanged; production feedback cadence changed."));
    J->SetNumberField(TEXT("observedRatioCount"),ObservedRatioCount);J->SetNumberField(TEXT("failedRatioCount"),FailedRatioCount);TArray<TSharedPtr<FJsonValue>> FailedPairs;for(const int32 Pair:FailedRatioPairs)FailedPairs.Add(MakeShared<FJsonValueNumber>(Pair));J->SetArrayField(TEXT("failedRatioPairs"),FailedPairs);J->SetBoolField(TEXT("flatSpeedRatioAcceptancePassed"),bSpeedAcceptancePerformed&&QualifiedWindowCount==8&&FailedWindowQualityCount==0&&IneligibleRatioPairs.Num()==0&&ObservedRatioCount==4&&FailedRatioCount==0);
    J->SetNumberField(TEXT("observedSpeedWindowCount"),Runs.Num());J->SetNumberField(TEXT("completedFlatStops"),Pass);J->SetNumberField(TEXT("qualifiedSpeedWindowCount"),QualifiedWindowCount);J->SetNumberField(TEXT("failedWindowQualityCount"),FailedWindowQualityCount);
    TArray<TSharedPtr<FJsonValue>> QualifiedWindows,FailedWindows,IneligiblePairs;for(const bool Qualified:SpeedWindowQualified)QualifiedWindows.Add(MakeShared<FJsonValueBoolean>(Qualified));for(const int32 FailedPass:FailedWindowQualityPasses)FailedWindows.Add(MakeShared<FJsonValueNumber>(FailedPass));for(const int32 Pair:IneligibleRatioPairs)IneligiblePairs.Add(MakeShared<FJsonValueNumber>(Pair));
    J->SetArrayField(TEXT("speedWindowQualifiedForRatioByPass"),QualifiedWindows);J->SetArrayField(TEXT("failedWindowQualityPasses"),FailedWindows);J->SetArrayField(TEXT("ineligibleRatioPairs"),IneligiblePairs);J->SetBoolField(TEXT("flatSpeedWindowAcceptancePassed"),bSpeedAcceptancePerformed&&QualifiedWindowCount==8&&FailedWindowQualityCount==0);
    J->SetStringField(TEXT("windowQualityFailureContinuationScope"),TEXT("Only finite CV>0.10 or half-mean relative difference>0.05 after unchanged full-throttle/1s/20-interval/positive-finite/admitted-Boost integrity and all physical guards may continue through the already requested native brake, original supported-stop envelopes and full0.5s Quiet dwell. Raw original800cm/final1s measurements and failed Checks remain. Each invalid window stays unqualified; either invalid partner makes its pair explicitly ineligible/unaccepted without evaluating a ratio PASS. Failed-window count/list is sticky and every failed Check permanently prevents terminal success. Later safety failures keep their immediate reason; later completed observations report failed-window acceptance. No retry, alternate window, reset, controller/profile/threshold or production change."));
    J->SetStringField(TEXT("ratioFailureContinuationScope"),TEXT("Only an eligible measured1.90..2.10 ratio miss is retained as a permanent failed Check and aggregated while the existing bounded flat/cargo/Succeeded/curb observations continue. Finite window-quality misses have separate sticky failure/eligibility accounting. All original safety/setup/custody/window-integrity/global guards still abort immediately; actual brake/Quiet dwell precedes every next pass. Any failed window or ratio forces terminalFAIL; no retry/reset/profile tuning/acceptance relaxation."));
    if(LastFlatGuard)J->SetObjectField(TEXT("lastFlatPassGuard"),LastFlatGuard);
    if(LastPositionGuard)J->SetObjectField(TEXT("lastPhysicalPositionGuard"),LastPositionGuard);
    J->SetArrayField(TEXT("actualFlatDriveAdmissionTrace"),FlatAdmissionTrace);
    J->SetArrayField(TEXT("actualDiagnosticYawFilterInitializations"),YawFilterInitializations);if(LastYawFilterUpdate)J->SetObjectField(TEXT("lastDiagnosticYawFilterUpdate"),LastYawFilterUpdate);
    J->SetStringField(TEXT("diagnosticYawFilterScope"),TEXT("Same all8: initialize from actual finite raw yaw at matched braked RunStart, update only RunMode1 powered controller ticks with alpha=1-exp(-positive finite worldDt/0.05), filtered=previous+alpha*(raw-previous); equal timestamp retains state with alpha0, backwards/nonfinite timestamp rejects. Only D-term measurement changes; P/lateral/gains/.65 clamp/.5s throttle ramp and every raw pose/support/body/window/ratio/stop guard remain. No bank cap. Per-tick filter/command receipts attach to the existing bounded active-drive trace and keep its explicit truncation; cached legacy PrePhysics or completed production solver-callback wheel operands and later PostUpdateWork body/filter/command clocks are distinct. Native outcome is unverified."));
    if(LastPrelaunchClearance)J->SetObjectField(TEXT("lastFlatPrelaunchClearance"),LastPrelaunchClearance);J->SetArrayField(TEXT("actualFlatPrelaunchClearanceTrace"),PrelaunchClearanceTrace);J->SetNumberField(TEXT("flatPrelaunchClearanceSamplesSeen"),PrelaunchSamplesSeen);J->SetNumberField(TEXT("flatPrelaunchClearanceSamplesRetained"),PrelaunchClearanceTrace.Num());J->SetBoolField(TEXT("flatPrelaunchClearanceTraceTruncated"),bPrelaunchTraceTruncated);
    J->SetStringField(TEXT("flatPrelaunchClearanceScope"),TEXT("Same all8 RunMode0 only: after actual Position/Quiet admission, hold ordinary zero throttle/steer native brake withBoostfalse while existing full raised corridor is occupied. Require fixed0.5s continuous observed clearance before profile selection/RunWorld start. Lost position/readiness or occupancy resets only separate clear dwell; original StepWorld40s deadline never resets. Per-wait-tick native support/stopped/custody/matched pose remains required. Blocker native/readback motion is evidence, not prediction or new threshold. All moving corridor/measurement/controller/ratio gates remain unchanged; no actor ignored, crowd disabled, active-pass restart or guarantee of future clearance."));
    J->SetBoolField(TEXT("flatSiteAdmitted"),bFlatSiteAdmitted);J->SetArrayField(TEXT("fixedFlatSiteAttempts"),FlatSiteAttempts);if(bFlatSiteAdmitted){J->SetArrayField(TEXT("selectedFlatCourseOriginCm"),MayaBoostTest::Vec(FlatCourseOrigin));J->SetNumberField(TEXT("selectedFlatLateralOffsetCm"),FlatSelectedLateralOffset);}J->SetStringField(TEXT("flatSiteSelectionScope"),TEXT("Fixed geometry-only candidates +15 then+30cm along existing across axis, same captured whole-body dimensions/query policy for each. Choose the first completely admitted original corridor before any pass, freeze its origin for all8. Retain all failed candidate hits/envelopes; they are site-selection observations, not speed acceptance Checks. All failed speed ratios remain permanent. Original service/Succeeded/curb Route0 targets unchanged. Historical offset0 native168 sign rejection stays preserved."));
    if(FlatSurveyEnvelope)J->SetObjectField(TEXT("freshWholeMotionSurveyEnvelope"),FlatSurveyEnvelope);if(LastFlatBounds)J->SetObjectField(TEXT("lastActualFlatBodyBounds"),LastFlatBounds);if(LastFlatContactEvidence)J->SetObjectField(TEXT("lastActualFlatSupportReceipts"),LastFlatContactEvidence);if(BrakeStartEvidence)J->SetObjectField(TEXT("latestActualBrakeStart"),BrakeStartEvidence);
    J->SetArrayField(TEXT("actualFlatBodyBoundsTrace"),FlatBoundsTrace);J->SetNumberField(TEXT("flatBodyBoundsSamplesSeen"),FlatBoundsSamplesSeen);J->SetNumberField(TEXT("flatBodyBoundsSamplesRetained"),FlatBoundsTrace.Num());J->SetBoolField(TEXT("flatBodyBoundsTraceTruncated"),bFlatBoundsTraceTruncated);J->SetArrayField(TEXT("actualCargoEnvelopeAdmissions"),CargoEnvelopeAdmissions);
    J->SetStringField(TEXT("phaseSpecificFlatStopScope"),TEXT("Addendum03: driving800cm trigger/900cm hardcap/3degree measured heading unchanged; only RunMode2 uses15degree/1000cm total/200cm additional from actual brake start. Same15s total pass clock/20cm lateral/backward-5/support63/original native contacts/physical locks/custody/Quietfull.5s. Only ordinary repeated zero throttle/steer Brake true Boost false; no corrective powered stop. Current native9+parcel collision bounds and all-mobility raised corridor are observed every driving/braking tick through stopped dwell. Body Z capturedunion+/-10 differs from raised-obstacle Ground+4..bodyMax+5."));
    J->SetNumberField(TEXT("flatDriveAdmissionTraceSamplesSeen"),FlatAdmissionTraceSamplesSeen);J->SetNumberField(TEXT("flatDriveAdmissionTraceSamplesRetained"),FlatAdmissionTrace.Num());J->SetBoolField(TEXT("flatDriveAdmissionTraceTruncated"),bFlatAdmissionTraceTruncated);
    J->SetStringField(TEXT("flatDriveAdmissionTraceScope"),TEXT("Every actual active flat-pass driving/stopping test tick before issuing another command, cap12000 rows with explicit counts/truncation. GT ObserveDoors/intent and actual completed production solver-callback or legacy ApplyPhysicalTorques clocks are separately retained; the later body observation is not simultaneous with cached motor operands. Measured acceptance and v12 steering are unchanged; v15 uses the same fixed0.5s time-only flat launch ramp for all8, with exact fullsignedraw throttle required throughoutfinal1s. v13 flat setup trim/receipts remain. Observed PostUpdateWork body yaw/rate/lateral/speed are separately stamped from cached PrePhysics motor decisions."));
    J->SetArrayField(TEXT("actualFlatSetupTranslationPulseObservations"),SetupPulseObservations);J->SetNumberField(TEXT("flatSetupTranslationPulseSamplesSeen"),SetupPulseSamplesSeen);J->SetNumberField(TEXT("flatSetupTranslationPulseSamplesRetained"),SetupPulseObservations.Num());J->SetBoolField(TEXT("flatSetupTranslationPulseTraceTruncated"),bSetupPulseTraceTruncated);
    J->SetStringField(TEXT("flatSetupTranslationPulseScope"),TEXT("Retired inactive compatibility evidence: no flat translation pulse, passive release, count4 exhaustion or0.80s power budget applies to continuous fine control. Pulse arrays/counters remain empty/zero. Actual continuous authority is bounded by original40s setup and native guards; finite yaw trim is separate and unchanged."));
    J->SetArrayField(TEXT("actualFlatSetupTrimObservations"),FlatTrimObservations);J->SetNumberField(TEXT("flatSetupTrimObservationsSeen"),FlatTrimObservationsSeen);J->SetNumberField(TEXT("flatSetupTrimObservationsRetained"),FlatTrimObservations.Num());J->SetBoolField(TEXT("flatSetupTrimObservationsTruncated"),bFlatTrimObservationsTruncated);
    J->SetStringField(TEXT("flatSetupTrimController"),TEXT("FlatStep1/5RunMode0 only: save actual initial trim sign, ordinary rawpureyaw+/-0.5 withoutBoost, motor phase bounded0.30s in world/real clocks with accepted command lease capped to remaining real budget minus1ms (all other leases0.25s), and ending sooner when fixedSign*targetMinusActualHeading<=-0.5degree or outerposition departs. No sign reversal midpulse. Realbrake, unchanged0.5s settle/Quiet and final2cm/1degree/full0.5s dwell; original40s clock. Service/Succeeded probe retains previous+/-0.35/0.12s/inner0.5degree trim. Native outcome, not transientheading, determines acceptance."));
    J->SetArrayField(TEXT("actualFlatSetupReapproachTransitions"),ReapproachTransitions);
    J->SetArrayField(TEXT("actualFlatContinuousFineObservations"),FlatFineObservations);J->SetNumberField(TEXT("flatContinuousFineSamplesSeen"),FlatFineObservationsSeen);J->SetNumberField(TEXT("flatContinuousFineSamplesRetained"),FlatFineObservations.Num());J->SetBoolField(TEXT("flatContinuousFineTraceTruncated"),bFlatFineObservationsTruncated);
    J->SetStringField(TEXT("flatContinuousFineObservationScope"),TEXT("Actual flat setup/entry20cm versus active phases1-4 retention30cm and latched phase5 cancellation, with separate recovery10cm eligibility, frozen/current bearing and signs, actual signed ray projection/miss/0.2Radius limit/feasibility/unit planar optical, required-bearing-correction pending before/after and consume/reset reason, projection/optical velocity, exact bounded PI/dt/reset/antiwindup/request operands and separate authority-qualified powered versus actual-braked residence. Cap4096 and explicit truncation; completed PT fields cached before later command, no hot-loop histories. Production and all final40s/position/heading/Quiet/window/ratio criteria unchanged."));
    J->SetStringField(TEXT("boundedFlatSetupReapproach"),TEXT("Near straight PI only within unchanged10cm recovery radius: unused recovery powered residence requires directional integral.30 or throttle.36 cap, actualspeed<=4 and fixed planar anchor<=1cm for1.5s under original guards. Clear integral before actual zero/native brake; unchanged>=.5s/Quiet then fresh independent1.5s/1cm actual-braked unresolved residence admits one original80cm waypoint/420..1300 clamp, normal-.35/steering/10s/100cm/support63/lateral35 and actual waypoint dwell. Only that admission suspends fine2Radius. Inactive entry20cm/active retention30cm controller remains reachable after recovery; no second attempt or setup clock reset. Active rescue outside10 cancellation retains original native brake/guards through.5s/Quiet then returns before later near control. Active phases1-4 outside30 enter latched phase5 actual-brake cancellation even if returning inside30/20 or entering fine; hard2Radius remains first, cancellation clears only original.5s/Quiet and returns before later inactive entry within20."));
    J->SetStringField(TEXT("physicalSetupController"),TEXT("Flat inactive entry20cm/active phases1-4 retention30cm control independent of recovery count, with unchanged separate recovery10cm eligibility: actual native brake/0.5s/Quiet, outer endpoint priority to original final heading/dwell; otherwise frozen nearest forward/reverse target bearing; straight admission/continuation uses actual positive signed projection and planar optical-ray miss<=0.2Radius, with finite/nonzero planar optical guard, else normal0/+/-.5 finite yaw bounded.30 world/real with signed-.5degree crossing and unchanged lease margin. Straight T=clamp(.012*(clamp((7/6)*actualOpticalProjection,+/-3)-actualOpticalVelocity)+I,+/-.36), S0; I integratesclamp(.02error,+/-.10)*actualdt capped+/-.30 with conditional antiwindup and explicit reset on exits/invalid dt. Fresh straight arrival requires0.3Radius and actualspeed<=4 before bearing/sign replan, then actual braked outer handoff. A valid straight ray loss requires a finite nonzero frozen-bearing yaw even when braking restores ray feasibility; pending survives yaw/phase3 brake and is consumed only after actual settlement, while exact-zero alignment invents no turn. New pass/target, actual outer handoff, admitted recovery, completed phase5 cancellation and terminal end clear it. First2Radius entry latches unchanged hard fine/native guards, ahead of recovery10cm and sticky active-exit30cm phase5 cancellation; only admitted original one80cm recovery suspends it. Existing final yaw retains Radius exit, final outer/1degree/full.5sQuiet/40s unchanged. Far flat PD and non-flat behavior unchanged; no pose/velocity/physics setter."));
    J->SetStringField(TEXT("flatPassController"),TEXT("All8passessame: diagnostic fixed launch raw throttle=direction*clamp(passAgeSeconds/0.5,0,1), thenfull+/-1. Explicit departure from prior immediatefullstep harness, time-only andnotoutcome-dependent; exact full signedraw throttle required/retained atbothendpoints ofeveryoverlappingfinal1s speedsegment with>=0.999s coverage. Curbrawcommands unchanged. Chassis-yaw steer=clamp((target-minus-actual optical heading degrees)/6 -direction*lateralCm*.01 -filteredWorldYawRateRadps*1.5,+/- .65 (fixed tau0.05worldseconds initialized actual matched-braked yaw eachpass)). Actual accepted requested steer and commandworld timestamp retained beside each final1s speed sample; applied native torque and later readback remain distinct observations. Driving3degree/20cm/fullsupport/nativeidentity/8m/9m/15s/window/ratio gates unchanged. Prospective supported braking uses separate addendum03 limits15degree/1000cm total/+200cm, with current body/corridor containment and original stopped dwell."));J->SetBoolField(TEXT("newNativeDepletionRepeatPerformed"),false);J->SetArrayField(TEXT("actualProcessedCaptures"),Captures);if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    if(Curb)J->SetStringField(TEXT("originalCurbEvidence"),Curb->ReadEvidenceJson(true));if(Robot)J->SetStringField(TEXT("finalActualNativeTelemetry"),Robot->ReadTelemetryJson());
    FString Text;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&Text));FFileHelper::SaveStringToFile(Text,*FPaths::Combine(Output,TEXT("result.json")));UE_LOG(LogTemp,Display,TEXT("MAYA BOOST POWER %s: %s"),Success?TEXT("PASS"):TEXT("FAIL"),*TerminalReason);FPlatformMisc::RequestExit(false,TEXT("MAYABoostPowerNativeTest"));
}
void AMAYABoostPowerNativeTest::EndPlay(EEndPlayReason::Type Reason)
{if(ScreenshotHandle.IsValid())FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);if(Robot)Robot->ReleaseDriveCommand();if(Curb)Curb->Cleanup();Super::EndPlay(Reason);}
