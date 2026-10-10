#include "MAYAV01TrafficEncounterTrial.h"
#include "MAYANativeDemoPawn.h"
#include "Vehicles/CitySampleVehicleBase.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryParcel.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYACrowdObstacleComponent.h"
#include "MAYAStableCameraArm.h"
#include "MAYADeliveryImpactEvidence.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassEntityView.h"
#include "MassExecutionContext.h"
#include "MassCommonFragments.h"
#include "MassTrafficFragments.h"
#include "MassActorSubsystem.h"
#include "MassRepresentationFragments.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"

namespace MayaTrafficEncounter
{
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{ return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)}; }
FString Json(const TSharedRef<FJsonObject>& J)
{ FString S;FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&S));return S; }
void Finite(const TSharedRef<FJsonObject>& J,const TCHAR* Key,double Value)
{ if(FMath::IsFinite(Value))J->SetNumberField(Key,Value);else J->SetStringField(Key,TEXT("nonfinite")); }
const TCHAR* Name(int32 I)
{ const TCHAR* N[]={TEXT("StageRoad"),TEXT("FindDirection"),TEXT("Align"),TEXT("FindOncoming"),TEXT("Intercept"),TEXT("PostContact"),TEXT("RecoveryWait"),TEXT("Complete"),TEXT("Failed")};return I>=0&&I<UE_ARRAY_COUNT(N)?N[I]:TEXT("Unknown"); }
TSharedRef<FJsonObject> Component(UPrimitiveComponent* C)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("component"),GetPathNameSafe(C));
    if(!C)return J;
    J->SetStringField(TEXT("class"),GetPathNameSafe(C->GetClass()));J->SetNumberField(TEXT("mobility"),int32(C->Mobility));
    J->SetNumberField(TEXT("collisionEnabled"),int32(C->GetCollisionEnabled()));J->SetBoolField(TEXT("simulating"),C->IsSimulatingPhysics());
    J->SetNumberField(TEXT("objectChannel"),int32(C->GetCollisionObjectType()));
    J->SetNumberField(TEXT("responsePhysicsBody"),int32(C->GetCollisionResponseToChannel(ECC_PhysicsBody)));
    J->SetNumberField(TEXT("responseCamera"),int32(C->GetCollisionResponseToChannel(ECC_Camera)));
    J->SetArrayField(TEXT("componentVelocityCmps"),Vec(C->GetComponentVelocity()));
    FBodyInstance* B=C->GetBodyInstance();J->SetBoolField(TEXT("nativeBodyValid"),B&&B->IsValidBodyInstance());
    if(B&&B->IsValidBodyInstance())
    {J->SetArrayField(TEXT("nativePositionCm"),Vec(B->GetUnrealWorldTransform().GetLocation()));J->SetArrayField(TEXT("nativeVelocityCmps"),Vec(B->GetUnrealWorldVelocity()));}
    return J;
}
bool PhysicalBlockingComponent(UPrimitiveComponent* C)
{
    if(!IsValid(C)||C->GetCollisionEnabled()!=ECollisionEnabled::QueryAndPhysics
        ||C->GetCollisionResponseToChannel(ECC_PhysicsBody)!=ECR_Block)return false;
    const FBodyInstance* B=C->GetBodyInstance();return B&&B->IsValidBodyInstance();
}
bool PhysicalBlockingActor(AActor* A)
{
    if(!IsValid(A))return false;TArray<UPrimitiveComponent*> Cs;A->GetComponents<UPrimitiveComponent>(Cs);
    for(auto* C:Cs)if(PhysicalBlockingComponent(C))return true;return false;
}
}

AMAYAV01TrafficEncounterTrial::AMAYAV01TrafficEncounterTrial()
{
    PrimaryActorTick.bCanEverTick=false;
    VehicleQuery.AddTagRequirement<FMassTrafficVehicleTag>(EMassFragmentPresence::All);
    VehicleQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
    VehicleQuery.AddRequirement<FMassTrafficVehicleControlFragment>(EMassFragmentAccess::ReadOnly);
    SensorQuery.AddRequirement<FMAYACrowdRobotObstacleOwner>(EMassFragmentAccess::ReadOnly);
    SensorQuery.AddTagRequirement<FMassTrafficObstacleTag>(EMassFragmentPresence::All);
}

bool AMAYAV01TrafficEncounterTrial::Configure(AMAYANativeDemoPawn* InRobot,AMAYADeliveryDirector* InDirector,const FMAYADeliveryRobotObservation& O,const FString& InOutputDirectory)
{
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    Failure=TEXT("Real traffic encounter diagnostic is unavailable in Shipping/Test");return false;
#endif
    if(bConfigured||!GetWorld()||!IsValid(InRobot)||!IsValid(InDirector)
        ||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))
        ||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01CurbTest"))
        ||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01TrafficEncounterTest")))
    {Failure=TEXT("Real traffic encounter requires exclusive rendered AutoTest+CurbTest+TrafficEncounterTest");return false;}
    FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),CrashFixtureMode);
    if(!CrashFixtureMode.IsEmpty()&&CrashFixtureMode!=TEXT("headon")&&CrashFixtureMode!=TEXT("rearend"))
    {Failure=TEXT("Unknown controlled original-car crash fixture mode");return false;}
    Robot=InRobot;Director=InDirector;const auto S=Director->Snapshot();
    OriginalParcel=Director->GetParcel();OriginalParcelBody=OriginalParcel.IsValid()?OriginalParcel->GetParcelBody():nullptr;
    if(S.Phase!=EMAYADeliveryPhase::Carrying||S.Job.ParcelCount!=1||Director->GetSecuredParcelCount()!=1
        ||!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched||!OriginalParcel.IsValid()
        ||!OriginalParcel->ObserveMass(OriginalMassKg)||!FMath::IsFinite(OriginalMassKg)||FMath::Abs(OriginalMassKg-1)>.05)
    {Failure=TEXT("Real traffic encounter requires actual one-parcel original-road supported/closed endpoint");return false;}
    EntryCm=O.ChassisPose.GetLocation();AlignmentAnchorCm=EntryCm;
    RoadStagingHeading=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();ResetBefore=O.ResetGeneration;JobId=S.JobInstanceId;ShiftId=S.ShiftInstanceId;
    ParcelId=OriginalParcel->GetParcelId();ConditionBefore=S.CargoConditionPercent;HistoryBefore=S.ImpactHistory.Num();CashBefore=S.TotalCredits;
    TArray<UPrimitiveComponent*> Cs;Robot->GetComponents<UPrimitiveComponent>(Cs);
    for(UPrimitiveComponent* C:Cs)
    {
        if(!C||!C->IsPhysicsCollisionEnabled())continue;
        FBodyInstance* B=C->GetBodyInstance();FBodyInstance* Own=C->GetBodyInstance(NAME_None,false);
        if(B&&Own&&B==Own&&B->IsValidBodyInstance())
        {C->OnComponentHit.AddDynamic(this,&AMAYAV01TrafficEncounterTrial::OnNativeHit);BoundBodies.Add(C);}
    }
    if(BoundBodies.Num()!=9){Cleanup();Failure=TEXT("Nine actual independent native robot collision bodies unavailable");return false;}
    OutputDirectory=FPaths::Combine(InOutputDirectory,TEXT("traffic-frames"));IFileManager::Get().MakeDirectory(*OutputDirectory,true);
    ScreenshotHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this,&AMAYAV01TrafficEncounterTrial::ScreenshotProcessed);
    StartWorldS=StepWorldS=GetWorld()->GetTimeSeconds();bConfigured=true;
    if(!ReadRoad(O,false)){Cleanup();Failure=TEXT("Actual traffic starting footprint lacks physical original-road support");return false;}
    Command(0,0,true);Save(O,true);return true;
}

bool AMAYAV01TrafficEncounterTrial::Command(float T,float S,bool Brake)
{bAccepted=Robot&&Robot->SubmitDriveCommand(T,S,Brake,EMAYADriveCommandOrigin::NativeAutomation,.25f,false);return bAccepted;}
void AMAYAV01TrafficEncounterTrial::Enter(EStep Next,double Now){Step=Next;StepWorldS=Now;StableWorldS=-1;}
AMAYAV01TrafficEncounterTrial::EOutcome AMAYAV01TrafficEncounterTrial::Fail(const FString& R)
{Failure=R;Step=EStep::Failed;Command(0,0,true);return EOutcome::Failed;}

bool AMAYAV01TrafficEncounterTrial::ReadRoad(const FMAYADeliveryRobotObservation& O,bool Ahead,double OpticalTravelSign)
{
    FBox Bounds(ForceInit);int32 Count=0;TArray<UPrimitiveComponent*> Cs;Robot->GetComponents<UPrimitiveComponent>(Cs);
    for(UPrimitiveComponent* C:Cs)
    {if(!C||!C->IsPhysicsCollisionEnabled())continue;FBodyInstance* B=C->GetBodyInstance();
        if(B&&B==C->GetBodyInstance(NAME_None,false)&&B->IsValidBodyInstance()){Bounds+=B->GetBodyBounds();++Count;}}
    if(Count!=9||!Bounds.IsValid)return false;
    const FVector Heading=OpticalTravelSign*(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();TArray<TSharedPtr<FJsonValue>> Rays;bool Clear=true;
    // Read the original road beneath the intentional contact target; this does not change physical collision.
    const AActor* RoadQueryIgnoredTarget=bFinalTargetSelected&&TargetActor.IsValid()?TargetActor.Get():nullptr;
    // The carried parcel is a separate actor, not covered by the robot actor ignore.
    // Exclude it only with exact current original inventory/body/ID and real restraint.
    const AMAYADeliveryParcel* RoadQueryIgnoredCargo=Director&&OriginalParcel.IsValid()&&OriginalParcelBody.IsValid()
        &&Director->GetParcel()==OriginalParcel.Get()&&Director->GetParcelCount()==1&&Director->GetSecuredParcelCount()==1
        &&Director->Snapshot().JobInstanceId==JobId&&OriginalParcel->GetParcelId()==ParcelId
        &&OriginalParcel->GetParcelBody()==OriginalParcelBody.Get()?OriginalParcel.Get():nullptr;
    for(double X:{Bounds.Min.X,Bounds.Max.X})for(double Y:{Bounds.Min.Y,Bounds.Max.Y})for(int32 AheadIndex=0;AheadIndex<(Ahead?2:1);++AheadIndex)
    {
        const FVector XY=FVector(X,Y,0)+Heading*(AheadIndex*50);FHitResult H;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYATrafficOriginalRoad),false,Robot);Q.MobilityType=EQueryMobilityType::Static;Q.AddIgnoredActor(this);
        if(RoadQueryIgnoredTarget)Q.AddIgnoredActor(RoadQueryIgnoredTarget);
        if(RoadQueryIgnoredCargo)Q.AddIgnoredActor(RoadQueryIgnoredCargo);
        const bool Hit=GetWorld()->LineTraceSingleByChannel(H,FVector(XY.X,XY.Y,O.ChassisPose.GetLocation().Z+100),FVector(XY.X,XY.Y,O.ChassisPose.GetLocation().Z-300),ECC_Visibility,Q);
        auto* C=H.GetComponent();auto* M=Cast<UStaticMeshComponent>(C);auto* I=Cast<UInstancedStaticMeshComponent>(C);
        FBodyInstance* B=C&&(!I||I->IsValidInstance(H.Item))?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
        const bool Physical=Hit&&H.bBlockingHit&&!H.bStartPenetrating&&C&&C->Mobility==EComponentMobility::Static
            &&C->IsPhysicsCollisionEnabled()&&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block
            &&B&&B->IsValidBodyInstance()&&H.ImpactNormal.Z>=.98&&M&&M->GetStaticMesh()
            &&M->GetStaticMesh()->GetPathName()==TEXT("/Game/Road/Kit_City_Road/SM_ROAD_19_20_0_0_road.SM_ROAD_19_20_0_0_road");
        Clear&=Physical;auto J=MayaTrafficEncounter::Component(C);J->SetBoolField(TEXT("physicalOriginalRoad"),Physical);
        J->SetArrayField(TEXT("sampleXYCm"),MayaTrafficEncounter::Vec(XY));J->SetNumberField(TEXT("instanceIndex"),H.Item);
        if(Hit){J->SetArrayField(TEXT("hitCm"),MayaTrafficEncounter::Vec(H.ImpactPoint));J->SetArrayField(TEXT("normal"),MayaTrafficEncounter::Vec(H.ImpactNormal));}
        Rays.Add(MakeShared<FJsonValueObject>(J));
    }
    auto J=MakeShared<FJsonObject>();J->SetBoolField(TEXT("actualFootprintAndLookaheadRoad"),Clear);J->SetArrayField(TEXT("rays"),Rays);
    J->SetBoolField(TEXT("selectedTargetIgnoredOnlyForRoadQuery"),RoadQueryIgnoredTarget!=nullptr);
    J->SetStringField(TEXT("roadQueryIgnoredSelectedActor"),GetPathNameSafe(RoadQueryIgnoredTarget));
    J->SetBoolField(TEXT("currentlySecuredOriginalParcelIgnoredOnlyForRoadQuery"),RoadQueryIgnoredCargo!=nullptr);
    J->SetStringField(TEXT("roadQueryIgnoredSecuredParcelActor"),GetPathNameSafe(RoadQueryIgnoredCargo));
    J->SetStringField(TEXT("roadQueryIgnoredSecuredParcelBody"),RoadQueryIgnoredCargo?GetPathNameSafe(RoadQueryIgnoredCargo->GetParcelBody()):FString());
    J->SetStringField(TEXT("roadQueryIgnoredSecuredParcelId"),RoadQueryIgnoredCargo?RoadQueryIgnoredCargo->GetParcelId():FString());
    J->SetNumberField(TEXT("opticalTravelSignForLookahead"),OpticalTravelSign);
    J->SetArrayField(TEXT("actualProposedTravelLookaheadHeading"),MayaTrafficEncounter::Vec(Heading));
    J->SetStringField(TEXT("scope"),TEXT("First static Visibility hits beneath actual nine-body AABB corners and50cm signed proposed-travel lookahead (default optical forward); the exact currently bound final target actor and exact currently secured original parcel (same inventory actor/body/ID and ObserveSecured-backed count) are additionally ignored only to inspect original physical road beneath the proposed robot/cargo footprint. Unsecured/replaced cargo and all other actors/street props remain query blockers. Physical contact and camera collision are unchanged; this is course admission, never load-bearing support substitution."));
    LastRoadJson=MayaTrafficEncounter::Json(J);return Clear;
}

bool AMAYAV01TrafficEncounterTrial::ReadTraffic(const FMAYADeliveryRobotObservation& O,bool Select,bool Oncoming)
{
    LastTrafficReadRefusal.Reset();bTrafficReadRetryable=false;
    // A false lookup is not itself proof that a selected actor was replaced.
    // Read optional current fragments only while the manager is idle; persist no fragment pointers.
    auto Refuse=[&](const TCHAR* Reason,const FMassEntityManager* Manager)
    {
        LastTrafficReadRefusal=Reason;
        bTrafficReadRetryable=bFinalTargetSelected&&!Select&&FCString::Strcmp(Reason,TEXT("mass_manager_processing"))==0;
        auto J=MakeShared<FJsonObject>();J->SetBoolField(TEXT("trafficReadAvailable"),false);
        J->SetStringField(TEXT("trafficReadRefusal"),Reason);J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());
        J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetBoolField(TEXT("selecting"),Select);J->SetBoolField(TEXT("oncomingSelection"),Oncoming);
        J->SetBoolField(TEXT("subsystemAvailable"),Manager!=nullptr);J->SetBoolField(TEXT("managerProcessing"),Manager&&Manager->IsProcessing());
        J->SetNumberField(TEXT("expectedEntityIndex"),TargetEntity.Index);J->SetNumberField(TEXT("expectedEntitySerial"),TargetEntity.SerialNumber);
        J->SetStringField(TEXT("expectedActor"),GetPathNameSafe(TargetActor.Get()));J->SetBoolField(TEXT("expectedActorValid"),TargetActor.IsValid());
        if(Manager&&!Manager->IsProcessing())
        {
            const bool Active=Manager->IsEntityActive(TargetEntity);J->SetBoolField(TEXT("expectedEntityActive"),Active);
            if(Active)
            {
                const FMassEntityView V(*Manager,TargetEntity);J->SetBoolField(TEXT("vehicleTagPresent"),V.HasTag<FMassTrafficVehicleTag>());
                auto* AF=V.GetFragmentDataPtr<FMassActorFragment>();AActor* Actual=AF?AF->GetMutable():nullptr;
                const auto* T=V.GetFragmentDataPtr<FTransformFragment>();const auto* Control=V.GetFragmentDataPtr<FMassTrafficVehicleControlFragment>();
                const auto* Rep=V.GetFragmentDataPtr<FMassRepresentationFragment>();const auto* LOD=V.GetFragmentDataPtr<FMassTrafficSimulationLODFragment>();
                J->SetBoolField(TEXT("actorFragmentPresent"),AF!=nullptr);J->SetStringField(TEXT("currentMappedActor"),GetPathNameSafe(Actual));
                J->SetBoolField(TEXT("currentMappedActorValid"),IsValid(Actual));J->SetBoolField(TEXT("currentMappingMatchesExpectedActor"),Actual==TargetActor.Get());
                J->SetBoolField(TEXT("currentActorWorldMatches"),IsValid(Actual)&&Actual->GetWorld()==GetWorld());
                J->SetBoolField(TEXT("transformFragmentPresent"),T!=nullptr);J->SetBoolField(TEXT("controlFragmentPresent"),Control!=nullptr);
                if(T){const FVector P=T->GetTransform().GetLocation(),F=T->GetTransform().GetUnitAxis(EAxis::X).GetSafeNormal2D();
                    J->SetBoolField(TEXT("positionContainsNaN"),P.ContainsNaN());J->SetBoolField(TEXT("forwardNearlyZero"),F.IsNearlyZero());
                    if(!P.ContainsNaN()){J->SetArrayField(TEXT("currentMassPositionCm"),MayaTrafficEncounter::Vec(P));MayaTrafficEncounter::Finite(J,TEXT("currentDistanceCm"),FVector::Dist2D(P,O.ChassisPose.GetLocation()));}}
                if(Control)MayaTrafficEncounter::Finite(J,TEXT("currentLaneSpeedCmps"),Control->Speed);
                J->SetBoolField(TEXT("representationKnown"),Rep!=nullptr);J->SetNumberField(TEXT("currentRepresentation"),Rep?int32(Rep->CurrentRepresentation):-1);
                J->SetBoolField(TEXT("simulationLODKnown"),LOD!=nullptr);J->SetNumberField(TEXT("simulationLOD"),LOD?int32(LOD->LOD):-1);
                J->SetNumberField(TEXT("previousRepresentation"),Rep?int32(Rep->PrevRepresentation):-1);
                TArray<TSharedPtr<FJsonValue>> Parts;
                if(IsValid(Actual)){TArray<UPrimitiveComponent*> Cs;Actual->GetComponents<UPrimitiveComponent>(Cs);
                    for(auto* C:Cs)if(C)Parts.Add(MakeShared<FJsonValueObject>(MayaTrafficEncounter::Component(C)));}
                J->SetArrayField(TEXT("currentMappedActorComponents"),Parts);
                J->SetBoolField(TEXT("currentMappedActorPhysicalBlockingBody"),MayaTrafficEncounter::PhysicalBlockingActor(Actual));
                const bool DataSafe=(!T||(!T->GetTransform().GetLocation().ContainsNaN()&&!T->GetTransform().GetUnitAxis(EAxis::X).GetSafeNormal2D().IsNearlyZero()
                    &&FVector::Dist2D(T->GetTransform().GetLocation(),O.ChassisPose.GetLocation())<=3000))&&(!Control||FMath::IsFinite(Control->Speed));
                const bool MappingAbsent=FCString::Strcmp(Reason,TEXT("selected_actor_mapping_absent"))==0&&Actual==nullptr;
                const bool RequiredReadAbsent=FCString::Strcmp(Reason,TEXT("selected_entity_not_in_vehicle_query"))==0&&(!T||!Control||!AF);
                const bool MappingSafe=Actual==nullptr||(IsValid(Actual)&&Actual!=Robot&&Actual->GetWorld()==GetWorld());
                bTrafficReadRetryable=bFinalTargetSelected&&!Select&&V.HasTag<FMassTrafficVehicleTag>()&&DataSafe&&MappingSafe&&(MappingAbsent||RequiredReadAbsent);
                if(bTrafficReadRetryable&&MappingAbsent)bObservedTargetActorGap=true;
            }
        }
        J->SetBoolField(TEXT("boundedReadRetryEligible"),bTrafficReadRetryable);
        J->SetNumberField(TEXT("maximumReadDistanceCm"),3000);J->SetStringField(TEXT("scope"),TEXT("Exact current read refusal; same selected entity serial only. Manager-processing or active-entity missing actor/read may wait at most1s with actual brakes. Invalid identity/world/data/distance remains refusal; no inferred actor replacement."));
        LastTargetJson=MayaTrafficEncounter::Json(J);return false;
    };
    auto* Sub=GetWorld()->GetSubsystem<UMassEntitySubsystem>();if(!Sub)return Refuse(TEXT("mass_subsystem_unavailable"),nullptr);
    auto& M=Sub->GetMutableEntityManager();if(M.IsProcessing())return Refuse(TEXT("mass_manager_processing"),&M);
    FMassExecutionContext SensorContext(M,0.f);
    SensorQuery.ForEachEntityChunk(M,SensorContext,[&](FMassExecutionContext& C)
    {const auto Owners=C.GetFragmentView<FMAYACrowdRobotObstacleOwner>();const auto Es=C.GetEntities();
        for(int32 I=0;I<C.GetNumEntities();++I)if(Owners[I].Owner.IsValid()&&Owners[I].Owner->GetOwner()==Robot)RobotSensor=Es[I];});
    struct FCandidate{FMassEntityHandle Entity;AActor* Actor=nullptr;FVector Position,Forward;double Speed=0,Distance=MAX_dbl;};
    FCandidate Best;bool Found=false;bool BestIsDirection=false;const FVector Optical=((CrashFixtureMode==TEXT("rearend")?-1:1)*-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
    const TCHAR* TargetReadRefusal=TEXT("selected_entity_not_in_vehicle_query");
    FMassExecutionContext Context(M,0.f);
    VehicleQuery.ForEachEntityChunk(M,Context,[&](FMassExecutionContext& C)
    {
        const auto Ts=C.GetFragmentView<FTransformFragment>();const auto Controls=C.GetFragmentView<FMassTrafficVehicleControlFragment>();const auto Es=C.GetEntities();
        for(int32 I=0;I<C.GetNumEntities();++I)
        {
            const auto E=Es[I];if(!Select&&E!=TargetEntity)continue;
            auto* AF=M.GetFragmentDataPtr<FMassActorFragment>(E);AActor* Actor=AF?AF->GetMutable():nullptr; // Pointer read only; no fragment/actor mutation.
            if(!Actor){if(!Select)TargetReadRefusal=TEXT("selected_actor_mapping_absent");continue;}
            if(!IsValid(Actor)){if(!Select)TargetReadRefusal=TEXT("selected_actor_invalid");continue;}
            if(Actor==Robot){if(!Select)TargetReadRefusal=TEXT("selected_actor_maps_to_robot");continue;}
            if(Actor->GetWorld()!=GetWorld()){if(!Select)TargetReadRefusal=TEXT("selected_actor_world_mismatch");continue;}
            const FVector P=Ts[I].GetTransform().GetLocation(),F=Ts[I].GetTransform().GetUnitAxis(EAxis::X).GetSafeNormal2D();
            const double Speed=Controls[I].Speed,D=FVector::Dist2D(P,O.ChassisPose.GetLocation());
            if(P.ContainsNaN()){if(!Select)TargetReadRefusal=TEXT("selected_position_nonfinite");continue;}
            if(F.IsNearlyZero()){if(!Select)TargetReadRefusal=TEXT("selected_forward_nearly_zero");continue;}
            if(!FMath::IsFinite(Speed)){if(!Select)TargetReadRefusal=TEXT("selected_lane_speed_nonfinite");continue;}
            if(D>3000){if(!Select)TargetReadRefusal=TEXT("selected_distance_exceeds_3000cm");continue;}
            if(Select)
            {
                // Final acquisition may use a genuine car that naturally stopped after braking.
                // Initial direction also requires nearby physical traffic; no claimed prior motion.
                if(D<200||D>1000)continue;
                const FVector Delta=P-O.ChassisPose.GetLocation();
                // Same ahead/lateral lane geometry expressed before the robot turns:
                // the actual car faces toward the robot, rather than an adjacent/passed car.
                if(!Oncoming&&(FVector::DotProduct(F,Delta)>=-200
                    ||FMath::Abs(FVector::CrossProduct(F,Delta).Z)>250))continue;
                if(Oncoming&&(FVector::DotProduct(F,Optical)>-.8||FVector::DotProduct(Delta,Optical)<200
                    ||FMath::Abs(FVector::CrossProduct(Optical,Delta).Z)>250))continue;
                if(!MayaTrafficEncounter::PhysicalBlockingActor(Actor))continue;
                if(IsCrashFixture())
                {
                    auto* Car=Cast<ACitySampleVehicleBase>(Actor);
                    auto* Body=Car?Cast<UPrimitiveComponent>(Car->GetRootComponent()):nullptr;
                    if(!Body||!Body->IsSimulatingPhysics()||!MayaTrafficEncounter::PhysicalBlockingComponent(Body))continue;
                }
            }
            const bool IsDirection=Select&&Oncoming&&E==DirectionEntity;
            if(!Found||(IsDirection&&!BestIsDirection)||(IsDirection==BestIsDirection&&D<Best.Distance))
            {Found=true;BestIsDirection=IsDirection;Best={E,Actor,P,F,Speed,D};}
        }
    });
    if(!Found)return Refuse(Select?TEXT("no_selection_candidate_passed_original_filters"):TargetReadRefusal,&M);
    const auto* CurrentRep=M.GetFragmentDataPtr<FMassRepresentationFragment>(Best.Entity);
    const bool PhysicalBlockingBody=MayaTrafficEncounter::PhysicalBlockingActor(Best.Actor);
    const bool ActorRepresentation=CurrentRep&&(CurrentRep->CurrentRepresentation==EMassRepresentationType::HighResSpawnedActor
        ||CurrentRep->CurrentRepresentation==EMassRepresentationType::LowResSpawnedActor);
    if(bFinalTargetSelected&&!Select&&!PhysicalBlockingBody&&!ActorRepresentation)
        return Refuse(TEXT("selected_nonphysical_mapping_has_no_active_actor_representation"),&M);
    if(Select){TargetEntity=Best.Entity;TargetActor=Best.Actor;TargetSpeedAtSelection=Best.Speed;MinimumTargetSpeed=Best.Speed;
        ++TargetBindingEpoch;if(Oncoming){bSensorRecognized=false;bBrakingObserved=false;}}
    else if(TargetActor.Get()!=Best.Actor)
    {
        // A representation actor may change, but the actual selected Mass identity may not.
        const auto* Rep=M.GetFragmentDataPtr<FMassRepresentationFragment>(Best.Entity);
        const bool ObservedTransition=Rep&&(int32(Rep->CurrentRepresentation)!=TargetRepresentationAtLastRead
            ||(int32(Rep->PrevRepresentation)==TargetRepresentationAtLastRead&&Rep->PrevRepresentation!=Rep->CurrentRepresentation)
            ||bObservedTargetActorGap);
        if(!bFinalTargetSelected||!Oncoming||Best.Entity!=TargetEntity||!M.IsEntityActive(TargetEntity)
            ||!ActorRepresentation||!ObservedTransition)
            return Refuse(TEXT("selected_entity_actor_mapping_changed_without_valid_representation_lineage"),&M);
        auto Lineage=MakeShared<FJsonObject>();Lineage->SetStringField(TEXT("event"),TEXT("same_entity_representation_actor_transition"));
        Lineage->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());Lineage->SetNumberField(TEXT("entityIndex"),TargetEntity.Index);
        Lineage->SetNumberField(TEXT("entitySerial"),TargetEntity.SerialNumber);Lineage->SetStringField(TEXT("previousActor"),GetPathNameSafe(TargetActor.Get()));
        Lineage->SetStringField(TEXT("currentActor"),Best.Actor->GetPathName());Lineage->SetNumberField(TEXT("lastReadRepresentation"),TargetRepresentationAtLastRead);
        Lineage->SetNumberField(TEXT("currentRepresentation"),int32(Rep->CurrentRepresentation));Lineage->SetNumberField(TEXT("previousRepresentation"),int32(Rep->PrevRepresentation));
        Lineage->SetBoolField(TEXT("sameEntityActorGapObserved"),bObservedTargetActorGap);Lineage->SetBoolField(TEXT("physicalBlockingBodyValid"),PhysicalBlockingBody);
        if(TrafficActorLineage.Num()>=32)return Refuse(TEXT("same_entity_actor_lineage_receipt_capacity_exceeded"),&M);
        TrafficActorLineage.Add(MakeShared<FJsonValueObject>(Lineage)); // Durable independently of temporary-read history.
        TargetActor=Best.Actor;++TargetBindingEpoch;
    }
    const bool Waiting=bFinalTargetSelected&&!Select&&!PhysicalBlockingBody;
    if(Waiting!=bWaitingForPhysicalRepresentation||(CurrentRep&&TargetRepresentationAtLastRead>=0
        &&int32(CurrentRep->CurrentRepresentation)!=TargetRepresentationAtLastRead))++TargetBindingEpoch;
    bWaitingForPhysicalRepresentation=Waiting;
    if(PhysicalBlockingBody)LastPhysicalBindingReadWorldS=GetWorld()->GetTimeSeconds();
    TargetPositionCm=Best.Position;TargetForward=Best.Forward;MinimumTargetSpeed=FMath::Min(MinimumTargetSpeed,Best.Speed);
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("entityIndex"),Best.Entity.Index);J->SetNumberField(TEXT("entitySerial"),Best.Entity.SerialNumber);
    J->SetStringField(TEXT("actor"),Best.Actor->GetPathName());J->SetStringField(TEXT("actorClass"),Best.Actor->GetClass()->GetPathName());
    J->SetBoolField(TEXT("trafficReadAvailable"),true);J->SetNumberField(TEXT("readWorldSeconds"),GetWorld()->GetTimeSeconds());
    J->SetStringField(TEXT("physicalEncounterState"),Waiting?TEXT("WaitingForPhysicalRepresentation"):TEXT("CurrentPhysicalBlockingActor"));
    J->SetBoolField(TEXT("physicalBlockingBodyValid"),PhysicalBlockingBody);
    J->SetNumberField(TEXT("bindingEpoch"),double(TargetBindingEpoch));
    J->SetNumberField(TEXT("maximumFinalAcquisitionDistanceCm"),1000);
    J->SetBoolField(TEXT("reusesOriginalObservedDirectionEntity"),Best.Entity==DirectionEntity);
    J->SetNumberField(TEXT("originalDirectionObservedSpeedCmps"),DirectionObservedSpeedCmps);
    J->SetBoolField(TEXT("originalDirectionWasMovingWhenObserved"),DirectionObservedSpeedCmps>5);
    J->SetBoolField(TEXT("currentlyMovingAtRead"),Best.Speed>5);
    J->SetStringField(TEXT("encounterMotionScope"),Best.Speed>5?TEXT("currently_moving_original_traffic"):TEXT("drive_into_actual_stopped_original_traffic"));
    J->SetArrayField(TEXT("massPositionCm"),MayaTrafficEncounter::Vec(Best.Position));J->SetArrayField(TEXT("massForward"),MayaTrafficEncounter::Vec(Best.Forward));
    J->SetArrayField(TEXT("actorPositionCm"),MayaTrafficEncounter::Vec(Best.Actor->GetActorLocation()));J->SetArrayField(TEXT("actorVelocityCmps"),MayaTrafficEncounter::Vec(Best.Actor->GetVelocity()));
    J->SetNumberField(TEXT("laneSpeedCmps"),Best.Speed);J->SetNumberField(TEXT("distanceCm"),Best.Distance);
    const auto* Rep=M.GetFragmentDataPtr<FMassRepresentationFragment>(Best.Entity);const auto* LOD=M.GetFragmentDataPtr<FMassTrafficSimulationLODFragment>(Best.Entity);
    J->SetBoolField(TEXT("representationKnown"),Rep!=nullptr);J->SetBoolField(TEXT("simulationLODKnown"),LOD!=nullptr);
    J->SetNumberField(TEXT("currentRepresentation"),Rep?int32(Rep->CurrentRepresentation):-1);J->SetNumberField(TEXT("simulationLOD"),LOD?int32(LOD->LOD):-1);
    J->SetNumberField(TEXT("previousRepresentation"),Rep?int32(Rep->PrevRepresentation):-1);
    TargetRepresentationAtLastRead=Rep?int32(Rep->CurrentRepresentation):-1;bObservedTargetActorGap=false;
    if(Select&&Oncoming)OriginalSelectedActorPath=Best.Actor->GetPathName();
    const auto* Obstacles=M.GetFragmentDataPtr<FMassTrafficObstacleListFragment>(Best.Entity);
    const bool Recognized=Obstacles&&RobotSensor.IsSet()&&M.IsEntityActive(RobotSensor)&&Obstacles->Obstacles.Contains(RobotSensor);
    bSensorRecognized|=Recognized;J->SetBoolField(TEXT("actualObstacleListContainsRobotSensor"),Recognized);
    J->SetNumberField(TEXT("robotSensorIndex"),RobotSensor.Index);J->SetNumberField(TEXT("robotSensorSerial"),RobotSensor.SerialNumber);
    TArray<TSharedPtr<FJsonValue>> ObstacleIds;if(Obstacles)for(const auto E:Obstacles->Obstacles)
    {auto A=MakeShared<FJsonObject>();A->SetNumberField(TEXT("index"),E.Index);A->SetNumberField(TEXT("serial"),E.SerialNumber);ObstacleIds.Add(MakeShared<FJsonValueObject>(A));}J->SetArrayField(TEXT("actualObstacleEntities"),ObstacleIds);
    const auto* Avoid=M.GetFragmentDataPtr<FMassTrafficObstacleAvoidanceFragment>(Best.Entity);
    J->SetBoolField(TEXT("avoidanceFragmentKnown"),Avoid!=nullptr);
    if(Avoid){MayaTrafficEncounter::Finite(J,TEXT("timeToCollidingObstacleSeconds"),Avoid->TimeToCollidingObstacle);MayaTrafficEncounter::Finite(J,TEXT("distanceToCollidingObstacleCm"),Avoid->DistanceToCollidingObstacle);}
    const auto* PID=M.GetFragmentDataPtr<FMassTrafficPIDVehicleControlFragment>(Best.Entity);
    J->SetBoolField(TEXT("PIDControlPresent"),PID!=nullptr);if(PID){J->SetNumberField(TEXT("actualThrottle"),PID->Throttle);J->SetNumberField(TEXT("actualBrake"),PID->Brake);J->SetNumberField(TEXT("actualSteering"),PID->Steering);}
    bBrakingObserved|=Recognized&&PID&&PID->Brake>.05&&Best.Speed<TargetSpeedAtSelection-50;
    TArray<TSharedPtr<FJsonValue>> Parts;TArray<UPrimitiveComponent*> Cs;Best.Actor->GetComponents<UPrimitiveComponent>(Cs);
    for(auto* C:Cs)if(C)Parts.Add(MakeShared<FJsonValueObject>(MayaTrafficEncounter::Component(C)));
    J->SetArrayField(TEXT("actualCollisionComponents"),Parts);LastTargetJson=MayaTrafficEncounter::Json(J);return true;
}

FString AMAYAV01TrafficEncounterTrial::ReadCamera(const FMAYADeliveryRobotObservation& O) const
{
    auto J=MakeShared<FJsonObject>();const auto* Camera=Robot->FindComponentByClass<UCameraComponent>();const auto* Arm=Robot->FindComponentByClass<UMAYAStableCameraArm>();
    J->SetBoolField(TEXT("cameraKnown"),Camera&&Arm);if(!Camera||!Arm)return MayaTrafficEncounter::Json(J);
    const FVector Location=Camera->GetComponentLocation(),Pivot=Arm->GetComponentLocation()+Arm->TargetOffset;
    J->SetArrayField(TEXT("actualCameraPositionCm"),MayaTrafficEncounter::Vec(Location));J->SetArrayField(TEXT("actualCameraRotationDegrees"),MayaTrafficEncounter::Vec(Camera->GetComponentRotation().Euler()));
    J->SetStringField(TEXT("actualArmMetadata"),Arm->ReadEvidenceJson());FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYATrafficDiagnosticCamera),false,Robot);Q.AddIgnoredActor(this);
    TArray<TSharedPtr<FJsonValue>> OverlapRows;
    for(int32 I=0;I<2;++I)
    {
        TArray<FOverlapResult> Hits;GetWorld()->OverlapMultiByChannel(Hits,I==0?Pivot:Location,FQuat::Identity,ECC_Camera,FCollisionShape::MakeSphere(Arm->ProbeSize),Q);
        for(const auto& H:Hits)if(H.bBlockingHit){auto R=MayaTrafficEncounter::Component(H.GetComponent());R->SetStringField(TEXT("sample"),I==0?TEXT("actualArmPivot"):TEXT("actualCamera"));R->SetNumberField(TEXT("instanceIndex"),H.ItemIndex);OverlapRows.Add(MakeShared<FJsonValueObject>(R));}
    }
    J->SetArrayField(TEXT("diagnosticCameraSphereBlockingOverlaps"),OverlapRows);
    FHitResult H;const bool Blocked=GetWorld()->LineTraceSingleByChannel(H,Location,O.ChassisPose.GetLocation(),ECC_Camera,Q);
    J->SetBoolField(TEXT("diagnosticRobotLineOfSightBlocked"),Blocked&&H.bBlockingHit);J->SetStringField(TEXT("lineOfSightBlocker"),GetPathNameSafe(H.GetComponent()));
    J->SetBoolField(TEXT("lineStartPenetrating"),H.bStartPenetrating);J->SetNumberField(TEXT("lineHitTime"),H.Time);
    auto* PC=Cast<APlayerController>(Robot->GetController());int32 Width=0,Height=0;FVector2D Minimum(MAX_dbl,MAX_dbl),Maximum(-MAX_dbl,-MAX_dbl);bool Projected=PC!=nullptr;
    if(PC)PC->GetViewportSize(Width,Height);TArray<UPrimitiveComponent*> Cs;Robot->GetComponents<UPrimitiveComponent>(Cs);FBox Bounds(ForceInit);
    for(auto* C:Cs)if(C&&C->IsPhysicsCollisionEnabled()){FBodyInstance* B=C->GetBodyInstance();if(B&&B==C->GetBodyInstance(NAME_None,false)&&B->IsValidBodyInstance())Bounds+=B->GetBodyBounds();}
    Projected=Projected&&(Bounds.IsValid!=0);
    if(Projected)for(double X:{Bounds.Min.X,Bounds.Max.X})for(double Y:{Bounds.Min.Y,Bounds.Max.Y})for(double Z:{Bounds.Min.Z,Bounds.Max.Z})
    {FVector2D Screen;const bool P=PC->ProjectWorldLocationToScreen(FVector(X,Y,Z),Screen);Projected&=P;if(P){Minimum.X=FMath::Min(Minimum.X,Screen.X);Minimum.Y=FMath::Min(Minimum.Y,Screen.Y);Maximum.X=FMath::Max(Maximum.X,Screen.X);Maximum.Y=FMath::Max(Maximum.Y,Screen.Y);}}
    const bool InViewport=Projected&&Width>0&&Height>0&&Minimum.X>=0&&Minimum.Y>=0&&Maximum.X<=Width&&Maximum.Y<=Height;
    J->SetBoolField(TEXT("nativeRobotBoundsProjected"),Projected);J->SetBoolField(TEXT("nativeRobotBoundsInsideViewport"),InViewport);
    if(Projected){J->SetArrayField(TEXT("nativeRobotProjectedMinimumPx"),{MakeShared<FJsonValueNumber>(Minimum.X),MakeShared<FJsonValueNumber>(Minimum.Y)});J->SetArrayField(TEXT("nativeRobotProjectedMaximumPx"),{MakeShared<FJsonValueNumber>(Maximum.X),MakeShared<FJsonValueNumber>(Maximum.Y)});}
    J->SetBoolField(TEXT("diagnosticCameraClearAndRobotFramed"),InViewport&&OverlapRows.IsEmpty()&&!(Blocked&&H.bBlockingHit));
    J->SetStringField(TEXT("cameraPixelVisibility"),TEXT("not_assessed_until_processed_pixels_reviewed"));
    J->SetStringField(TEXT("scope"),TEXT("Passive actual player camera/arm metadata plus separately named diagnostic12cm sphere overlap and camera-channel LOS queries. These are not the historical SpringArm trace result and do not mutate the view or prove pixel visibility."));
    return MayaTrafficEncounter::Json(J);
}

void AMAYAV01TrafficEncounterTrial::QualifyNativeContact(const TSharedRef<FJsonObject>& J,double ActualHitWorldS)
{
    J->SetBoolField(TEXT("matchesSelectedTrafficActor"),true);
    if(!bContact){bContact=true;FirstContactWorldS=ActualHitWorldS;FirstSelectedContactJson=MayaTrafficEncounter::Json(J);
        if(Contacts.Num()>=128)Contacts.Last()=MakeShared<FJsonValueObject>(J);}
}

void AMAYAV01TrafficEncounterTrial::ReconcileDeferredNativeHits(const FMassEntityManager& M)
{
    if(M.IsProcessing())return;const double Now=GetWorld()->GetTimeSeconds();
    for(const auto& P:DeferredNativeHits)
    {
        auto J=P.Receipt.ToSharedRef();FString Reason;
        if(Now-P.CallbackWorldS>.25||Now<P.CallbackWorldS||P.BeforeReadWorldS<0
            ||P.CallbackWorldS-P.BeforeReadWorldS>.25)Reason=TEXT("expired_or_missing_bounded_pre_event_read");
        else if(P.Entity!=TargetEntity||P.BindingEpoch!=TargetBindingEpoch||!M.IsEntityActive(P.Entity))Reason=TEXT("entity_or_binding_epoch_changed");
        else if(!P.Actor.IsValid()||!P.Component.IsValid()||P.Actor.Get()!=TargetActor.Get()
            ||P.Actor->GetWorld()!=GetWorld()||P.Component->GetOwner()!=P.Actor.Get())Reason=TEXT("actor_or_component_identity_unavailable");
        else
        {
            const auto* AF=M.GetFragmentDataPtr<FMassActorFragment>(P.Entity);
            const auto* Rep=M.GetFragmentDataPtr<FMassRepresentationFragment>(P.Entity);
            if(!AF||AF->Get()!=P.Actor.Get())Reason=TEXT("current_exact_entity_actor_mapping_does_not_match");
            else if(!Rep||int32(Rep->CurrentRepresentation)!=P.Representation
                ||(Rep->PrevRepresentation!=Rep->CurrentRepresentation&&int32(Rep->PrevRepresentation)!=P.Representation))
                Reason=TEXT("representation_transition_or_unknown");
            else if(bWaitingForPhysicalRepresentation||!MayaTrafficEncounter::PhysicalBlockingComponent(P.Component.Get()))
                Reason=TEXT("current_native_physical_blocking_component_unavailable");
        }
        J->SetNumberField(TEXT("reconciliationWorldSeconds"),Now);J->SetNumberField(TEXT("reconciliationFrame"),double(GFrameCounter));
        J->SetBoolField(TEXT("deferredMappingQualified"),Reason.IsEmpty());J->SetStringField(TEXT("deferredMappingRejection"),Reason);
        J->SetStringField(TEXT("identityEvidenceScope"),TEXT("Real original callback; <=.25s same-serial/actor/binding-epoch/representation mapping reads bracket the callback; actual callback component is revalidated physically at reconciliation, not mapping-known at callback"));
        if(Reason.IsEmpty())QualifyNativeContact(J,P.CallbackWorldS);
    }
    DeferredNativeHits.Reset();
}

void AMAYAV01TrafficEncounterTrial::OnNativeHit(UPrimitiveComponent* HitComponent,AActor* OtherActor,UPrimitiveComponent* OtherComponent,FVector Impulse,const FHitResult& Hit)
{
    if(!bConfigured||!GetWorld()||GetWorld()->IsPaused()||!IsValid(OtherActor)||OtherActor==Robot||OtherActor==OriginalParcel.Get()
        ||!IsValid(OtherComponent)||Impulse.ContainsNaN())return;
    auto* Sub=GetWorld()->GetSubsystem<UMassEntitySubsystem>();bool CurrentMapping=false,Processing=false;
    if(Sub){auto& M=Sub->GetMutableEntityManager();Processing=M.IsProcessing();if(!Processing&&M.IsEntityActive(TargetEntity))
        {const auto* A=M.GetFragmentDataPtr<FMassActorFragment>(TargetEntity);CurrentMapping=A&&A->Get()==OtherActor;}}
    ++ContactCount;const double Now=GetWorld()->GetTimeSeconds();
    const bool SelectedActor=bFinalTargetSelected&&OtherActor==TargetActor.Get()&&!bWaitingForPhysicalRepresentation
        &&MayaTrafficEncounter::PhysicalBlockingComponent(OtherComponent);
    const bool Matched=SelectedActor&&CurrentMapping;auto J=MayaTrafficEncounter::Component(OtherComponent);
    J->SetNumberField(TEXT("worldSeconds"),Now);J->SetStringField(TEXT("robotComponent"),GetPathNameSafe(HitComponent));
    J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetNumberField(TEXT("platformRealSeconds"),FPlatformTime::Seconds());
    J->SetBoolField(TEXT("currentEntitySerialActorMappingMatches"),CurrentMapping);J->SetBoolField(TEXT("blockingHit"),Hit.bBlockingHit);
    J->SetBoolField(TEXT("mappingKnownAtCallback"),!Processing&&Sub!=nullptr);
    J->SetBoolField(TEXT("mappingDeferredBecauseProcessing"),SelectedActor&&Processing);
    J->SetStringField(TEXT("otherActor"),GetPathNameSafe(OtherActor));J->SetBoolField(TEXT("matchesSelectedTrafficActor"),Matched);
    J->SetArrayField(TEXT("normalImpulseKgCmps"),MayaTrafficEncounter::Vec(Impulse));J->SetNumberField(TEXT("impulseKgMps"),Impulse.Size()/100);
    J->SetArrayField(TEXT("normal"),MayaTrafficEncounter::Vec(Hit.ImpactNormal));J->SetBoolField(TEXT("startPenetrating"),Hit.bStartPenetrating);J->SetNumberField(TEXT("penetrationDepthCm"),Hit.PenetrationDepth);
    J->SetArrayField(TEXT("impactPointCm"),MayaTrafficEncounter::Vec(Hit.ImpactPoint));
    J->SetNumberField(TEXT("selectedEntityIndex"),TargetEntity.Index);J->SetNumberField(TEXT("selectedEntitySerial"),TargetEntity.SerialNumber);
    if(Contacts.Num()<128)Contacts.Add(MakeShared<FJsonValueObject>(J));
    if(Matched)QualifyNativeContact(J,Now);
    else if(SelectedActor&&Processing)
    {
        J->SetNumberField(TEXT("bindingEpochAtCallback"),double(TargetBindingEpoch));
        J->SetNumberField(TEXT("lastPhysicalMappingReadWorldSeconds"),LastPhysicalBindingReadWorldS);
        if(DeferredNativeHits.Num()<8&&DeferredNativeHitReceipts.Num()<32)
        {
            FDeferredNativeHit P;P.Entity=TargetEntity;P.Actor=OtherActor;P.Component=OtherComponent;P.Receipt=J;
            P.BindingEpoch=TargetBindingEpoch;P.Representation=TargetRepresentationAtLastRead;P.CallbackWorldS=Now;P.BeforeReadWorldS=LastPhysicalBindingReadWorldS;
            DeferredNativeHits.Add(P);DeferredNativeHitReceipts.Add(MakeShared<FJsonValueObject>(J));
        }
        else
        {
            J->SetBoolField(TEXT("deferredMappingQualified"),false);J->SetStringField(TEXT("deferredMappingRejection"),TEXT("bounded_pending_or_receipt_capacity_exceeded"));
            if(Contacts.Num()>=128)Contacts.Last()=MakeShared<FJsonValueObject>(J);
        }
    }
}

void AMAYAV01TrafficEncounterTrial::Save(const FMAYADeliveryRobotObservation& O,bool Force)
{
    const double Now=GetWorld()->GetTimeSeconds();if(!Force&&LastSampleWorldS>=0&&Now-LastSampleWorldS<.1)return;LastSampleWorldS=Now;
    LastCameraJson=ReadCamera(O);if(Samples.Num()>=(IsCrashFixture()?1300:1000))return;auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),Now);J->SetStringField(TEXT("step"),MayaTrafficEncounter::Name(int32(Step)));
    J->SetArrayField(TEXT("chassisPositionCm"),MayaTrafficEncounter::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("velocityCmps"),MayaTrafficEncounter::Vec(O.VelocityCmps));
    J->SetArrayField(TEXT("angularVelocityRadps"),MayaTrafficEncounter::Vec(O.AngularVelocityRadps));J->SetArrayField(TEXT("rotationDegrees"),MayaTrafficEncounter::Vec(O.ChassisPose.Rotator().Euler()));
    J->SetNumberField(TEXT("supportMask"),O.SupportMask);J->SetBoolField(TEXT("ready"),O.bReady);J->SetStringField(TEXT("fault"),O.Fault);
    J->SetBoolField(TEXT("nativeDoorsPhysicallyLocked"),Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());J->SetBoolField(TEXT("commandAccepted"),bAccepted);
    J->SetStringField(TEXT("actualTraffic"),LastTargetJson);J->SetStringField(TEXT("actualCamera"),LastCameraJson);
    J->SetStringField(TEXT("cargoInventory"),Director->ReadCargoInventoryEvidenceJson());const auto S=Director->Snapshot();
    J->SetNumberField(TEXT("conditionPercent"),S.CargoConditionPercent);J->SetNumberField(TEXT("cash"),S.TotalCredits);J->SetNumberField(TEXT("batteryPercent"),S.BatteryPercent);
    if(S.LastImpact.EpisodeGeneration>0)J->SetObjectField(TEXT("lastAppliedImpact"),MAYADeliveryImpactReceiptToJson(S.LastImpact));
    if(IsCrashFixture()&&CrashLaneEvidence.IsValid())J->SetObjectField(TEXT("actualCrashLaneStaging"),CrashLaneEvidence);
    Samples.Add(MakeShared<FJsonValueObject>(J));
}

AMAYAV01TrafficEncounterTrial::EOutcome AMAYAV01TrafficEncounterTrial::Advance(const FMAYADeliveryRobotObservation& O)
{
    if(!bConfigured||!Robot||!Director||!GetWorld()||GetWorld()->IsPaused())return Fail(TEXT("Actual traffic diagnostic world/authority unavailable or paused"));
    const double Now=GetWorld()->GetTimeSeconds();double Age=Now-StepWorldS;const auto S=Director->Snapshot();
    // Reconcile a genuine processing-time callback before applying no-contact containment.
    // A real shove/tumble must be observed as postcontact reaction when exact identity is proved.
    if(auto* Sub=GetWorld()->GetSubsystem<UMassEntitySubsystem>())
    {const auto& M=Sub->GetMutableEntityManager();if(!M.IsProcessing())ReconcileDeferredNativeHits(M);}
    if(Now-StartWorldS>(IsCrashFixture()?120:95))return Fail(IsCrashFixture()
        ?TEXT("Controlled physical crash fixture exceeded120gameplay seconds from original entry")
        :TEXT("Real traffic encounter and production recovery exceeded95gameplay seconds"));
    if(!bContact&&Now-StartWorldS>(IsCrashFixture()?90:60))return Fail(IsCrashFixture()
        ?TEXT("CONTACT_NOT_OBSERVED: controlled original-car fixture exceeded90gameplay seconds from original entry")
        :TEXT("CONTACT_NOT_OBSERVED: no genuine selected-traffic native contact within60gameplay seconds; recognition/braking receipts are separate evidence"));
    if(S.JobInstanceId!=JobId||S.ShiftInstanceId!=ShiftId||S.TotalCredits!=CashBefore)
    {Save(O,true);return Fail(TEXT("Real traffic diagnostic lost original job/shift or changed cash"));}
    if(IsCrashFixture()&&(Step==EStep::Intercept||bContact))return AdvanceCrashFixture(O);
    if(bContact&&Step<EStep::PostContact){Enter(EStep::PostContact,FirstContactWorldS);Age=Now-StepWorldS;Capture(TEXT("first-contact-aftermath"));}
    if(Step<EStep::PostContact)
    {
        if(!O.bKnown||!O.bReady||!O.Fault.IsEmpty()||O.ResetGeneration!=ResetBefore||S.Phase!=EMAYADeliveryPhase::Carrying
            ||Director->GetSecuredParcelCount()!=1||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()
            ||FVector::Dist2D(O.ChassisPose.GetLocation(),EntryCm)>(IsCrashFixture()?800:600)||O.VelocityCmps.Size()>90
            ||FMath::Abs(O.ChassisPose.Rotator().Pitch)>20||FMath::Abs(O.ChassisPose.Rotator().Roll)>20)
        {Save(O,true);return Fail(IsCrashFixture()?TEXT("Actual fixture pre-contact native identity/custody/lock/8m/90cmps/20degree containment failed"):TEXT("Actual pre-contact native identity/custody/lock/6m/90cmps/20degree containment failed"));}
        if(Step==EStep::StageRoad||LastSampleWorldS<0||Now-LastSampleWorldS>=.1)
            if(!ReadRoad(O,Step==EStep::StageRoad||Step==EStep::Intercept)){Save(O,true);return Fail(TEXT("Actual robot footprint or50cm proposed heading no longer has physical original-road first-hit admission"));}
    }
    if(Step==EStep::StageRoad)
    {
        // Normal zero-steer motion farther onto the measured original road before turning.
        // EntryCm remains the original600cm ordinary/800cm explicit-fixture boundary; only the turn anchor moves.
        if(Age>20){Save(O,true);return Fail(TEXT("Physical100cm original-road staging did not settle within20seconds"));}
        const FVector Delta=O.ChassisPose.GetLocation()-EntryCm;
        const FVector Optical=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
        const double Along=FVector::DotProduct(Delta,RoadStagingHeading);
        const double Lateral=FMath::Abs(FVector::CrossProduct(RoadStagingHeading,Delta).Z);
        const double HeadingError=FMath::Abs(FMath::FindDeltaAngleDegrees(RoadStagingHeading.Rotation().Yaw,Optical.Rotation().Yaw));
        if(Along< -10||Along>150||Lateral>25||HeadingError>5)
        {Save(O,true);return Fail(TEXT("Physical road-staging150cm/25cm/5degree straight-motion envelope exceeded"));}
        bRoadStagingDistanceReached|=Along>=100;
        if(!bRoadStagingDistanceReached)Command(O.OpticalForwardCmps>65?0:.5f,0,O.OpticalForwardCmps>65);
        else
        {
            Command(0,0,true);
            if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
            {
                if(StableWorldS<0)StableWorldS=Now;
                if(Now-StableWorldS>=.5)
                {AlignmentAnchorCm=O.ChassisPose.GetLocation();bRoadStagingComplete=true;Enter(EStep::FindDirection,Now);Save(O,true);}
            }
            else StableWorldS=-1;
        }
    }
    else if(Step==EStep::FindDirection)
    {
        Command(0,0,true);
        if(ReadTraffic(O,true,false))
        {
            DirectionEntity=TargetEntity;DirectionObservedSpeedCmps=TargetSpeedAtSelection;
            if(IsCrashFixture())
            {
                // Freeze this exact physical car for the controlled fixture; no second selection.
                auto* Car=Cast<ACitySampleVehicleBase>(TargetActor.Get());
                auto* Body=Car?Cast<UPrimitiveComponent>(Car->GetRootComponent()):nullptr;
                auto* Native=Body?Body->GetBodyInstance():nullptr;
                if(!Body||!Body->IsSimulatingPhysics()||!MayaTrafficEncounter::PhysicalBlockingComponent(Body)
                    ||!Native||!Native->IsValidBodyInstance())return Fail(TEXT("Selected controlled car lacks an actual native simulating root"));
                CrashSelectedBody=Body;CrashSelectedNativeActor=uint64(reinterpret_cast<UPTRINT>(Native->GetPhysicsActorHandle()));
                CrashLaneStartedWorldS=Now;CrashTurnAnchorCm=O.ChassisPose.GetLocation();
                OriginalSelectedActorPath=Car->GetPathName();
                bFinalTargetSelected=true;Enter(EStep::Intercept,Now);Capture(TEXT("before-physical-car-lane-staging"));
            }
            else {IntendedHeading=-TargetForward;Enter(EStep::Align,Now);}
        }
    }
    else if(Step==EStep::Align)
    {
        if(Age>20){Save(O,true);return Fail(TEXT("Actual road heading could not align opposite an observed nearby physical car within20seconds"));}
        const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,IntendedHeading.Rotation().Yaw);
        if(FMath::Abs(Error)<=5)
        {
            Command(0,0,true);if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
            {if(StableWorldS<0)StableWorldS=Now;if(Now-StableWorldS>=.5)Enter(EStep::FindOncoming,Now);}else StableWorldS=-1;
        }
        else{StableWorldS=-1;const FVector Optical=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
            const float Correction=float(FMath::Clamp(.02*FVector::DotProduct(AlignmentAnchorCm-O.ChassisPose.GetLocation(),Optical)-.015*O.OpticalForwardCmps,-.35,.35));Command(Correction,Error>0?1.f:-1.f,false);}
        ReadTraffic(O,false,false);
    }
    else if(Step==EStep::FindOncoming)
    {
        Command(0,0,true);if(ReadTraffic(O,true,true)){bFinalTargetSelected=true;Enter(EStep::Intercept,Now);Capture(TEXT("before-actual-physical-car-drive"));}
    }
    else if(Step==EStep::Intercept)
    {
        if(TargetReadWaitStartedWorldS>=0&&Now-TargetReadWaitStartedWorldS>=1)
        {Save(O,true);return Fail(TEXT("Selected actual traffic read did not return within1gameplay second of its first refusal; exact retry receipts retained"));}
        if(!ReadTraffic(O,false,true))
        {
            Command(0,0,true);
            auto Event=MakeShared<FJsonObject>();Event->SetStringField(TEXT("event"),TEXT("same_selected_entity_read_refusal"));
            Event->SetNumberField(TEXT("worldSeconds"),Now);Event->SetStringField(TEXT("exactRefusal"),LastTrafficReadRefusal);
            Event->SetStringField(TEXT("actualRefusalReceipt"),LastTargetJson);Event->SetBoolField(TEXT("brakeCommandAccepted"),bAccepted);
            Event->SetBoolField(TEXT("retryEligible"),bTrafficReadRetryable);
            if(TargetReadWaitStartedWorldS<0&&bTrafficReadRetryable)TargetReadWaitStartedWorldS=Now;
            Event->SetNumberField(TEXT("firstRefusalWorldSeconds"),TargetReadWaitStartedWorldS);
            if(TrafficReadEvents.Num()<128)TrafficReadEvents.Add(MakeShared<FJsonValueObject>(Event));
            Save(O,true);
            if(!bAccepted)return Fail(TEXT("Neutral brake renewal refused while waiting for same selected traffic entity"));
            if(!bTrafficReadRetryable)return Fail(TEXT("Selected actual traffic lookup refused with nontransient identity/world/data/distance/lineage cause; exact receipt retained"));
            return EOutcome::Running;
        }
        if(TargetReadWaitStartedWorldS>=0)
        {
            auto Event=MakeShared<FJsonObject>();Event->SetStringField(TEXT("event"),TEXT("same_selected_entity_read_resumed"));
            Event->SetNumberField(TEXT("worldSeconds"),Now);Event->SetNumberField(TEXT("firstRefusalWorldSeconds"),TargetReadWaitStartedWorldS);
            Event->SetNumberField(TEXT("actualWaitSeconds"),Now-TargetReadWaitStartedWorldS);Event->SetStringField(TEXT("actualRestoredRead"),LastTargetJson);
            if(TrafficReadEvents.Num()<128)TrafficReadEvents.Add(MakeShared<FJsonValueObject>(Event));
            else TrafficReadEvents.Last()=MakeShared<FJsonValueObject>(Event); // Keep the actual resumed read even after a saturated retry history.
            TargetReadWaitStartedWorldS=-1;
        }
        if(bContact){Command(0,0,true);Enter(EStep::PostContact,FirstContactWorldS);Capture(TEXT("first-contact-aftermath"));Save(O,true);return EOutcome::Running;}
        if(bWaitingForPhysicalRepresentation)
        {
            Command(0,0,true);Save(O);
            if(!bAccepted)return Fail(TEXT("Neutral brake renewal refused while actual selected entity has nonphysical representation"));
            return EOutcome::Running;
        }
        const FVector Optical=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D(),Delta=TargetPositionCm-O.ChassisPose.GetLocation();
        if(FVector::DotProduct(Delta,Optical)<-100){Save(O,true);return Fail(TEXT("CONTACT_NOT_OBSERVED: genuine selected oncoming car passed without matched native contact"));}
        const double Error=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,Delta.Rotation().Yaw);
        if(FMath::Abs(Error)>60){Save(O,true);return Fail(TEXT("Selected original traffic path no longer permits a bounded forward oncoming approach"));}
        Command(O.OpticalForwardCmps>65?0:.5f,float(FMath::Clamp(Error/45,-.65,.65)),O.OpticalForwardCmps>65);
    }
    else if(Step==EStep::PostContact)
    {
        Command(0,0,true);ReadTraffic(O,false,false);
        bPostStable|=O.IsStoppedAndSupported()&&O.bDoorsClosedLatched;
        if(Age>=3)
        {
            if(!bPostCaptureRequested){Capture(TEXT("three-second-physical-reaction"));bPostCaptureRequested=true;}
            if(!PendingScreenshotPath.IsEmpty()){Save(O);if(Age>5)return Fail(TEXT("Postcontact real viewport capture was not processed before guarded recovery"));return EOutcome::Running;}
            BeforeRecoveryJson=Director->ReadCargoInventoryEvidenceJson();const auto View=Director->GetPlayerPresentation();
            auto* PC=Cast<AMAYADeliveryPlayerController>(Robot->GetController());
            if(!PC||!View.bCanRecover){Save(O,true);return Fail(TEXT("Genuine traffic contact observed; current production UI recovery is not admitted after3second physical observation (no forced reset)"));}
            bRecoveryRequested=true;RecoveryRequestWorldS=Now;bRecoveryAccepted=PC->ActivateUIAction(TEXT("Recover"));
            Save(O,true);if(!bRecoveryAccepted)return Fail(TEXT("Genuine traffic contact observed; ordinary recovery action refused; exact production receipt retained"));
            Enter(EStep::RecoveryWait,Now);
        }
    }
    else if(Step==EStep::RecoveryWait)
    {
        Command(0,0,true);AfterRecoveryJson=Director->ReadCargoInventoryEvidenceJson();
        if(Age>20){Save(O,true);return Fail(TEXT("Actual traffic recovery did not reach safe same-body stock endpoint within20seconds"));}
        if(O.ResetGeneration>ResetBefore&&S.Phase==EMAYADeliveryPhase::Accepted&&O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
        {
            auto* P=Director->GetParcel();FTransform Native,Stock;double Kg=0;
            if(P!=OriginalParcel.Get()||!IsValid(P)||P->GetParcelId()!=ParcelId||P->GetParcelBody()!=OriginalParcelBody.Get()
                ||Director->GetParcelCount()!=1||Director->GetSecuredParcelCount()!=0||!P->ObserveMass(Kg)||FMath::Abs(Kg-OriginalMassKg)>.05
                ||!P->ObserveNativePose(Native)||!Director->GetRecoveryParcelStockPose(0,Stock)||!P->IsAt(Stock)
                ||FVector::Dist(Native.GetLocation(),Stock.GetLocation())>2||Native.GetRotation().AngularDistance(Stock.GetRotation())>FMath::DegreesToRadians(3))
            {Save(O,true);return Fail(TEXT("Production traffic recovery changed actual parcel identity/body/mass or failed validated stock endpoint"));}
            if(!bRecoveryCompleted){bRecoveryCompleted=true;Save(O,true);Capture(TEXT("same-body-recovery-endpoint"));}
            if(!PendingScreenshotPath.IsEmpty())return EOutcome::Running;
            Enter(EStep::Complete,Now);return EOutcome::Complete;
        }
    }
    Save(O);if(O.bReady&&!bAccepted)return Fail(TEXT("Actual traffic diagnostic leased drive/brake command refused while robot ready"));return EOutcome::Running;
}

FString AMAYAV01TrafficEncounterTrial::ReadEvidenceJson(bool Full) const
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("MayaRobotics.NativeRealTrafficEncounter.v1"));J->SetStringField(TEXT("step"),MayaTrafficEncounter::Name(int32(Step)));J->SetStringField(TEXT("failure"),Failure);
    J->SetBoolField(TEXT("nativeContactObserved"),bContact);J->SetBoolField(TEXT("trafficSensorRecognized"),bSensorRecognized);J->SetBoolField(TEXT("actualRecognizedCarBrakingObserved"),bBrakingObserved);
    J->SetBoolField(TEXT("postContactSupportedStopObserved"),bPostStable);J->SetBoolField(TEXT("recoveryRequested"),bRecoveryRequested);J->SetBoolField(TEXT("recoveryAccepted"),bRecoveryAccepted);J->SetBoolField(TEXT("recoveryCompleted"),bRecoveryCompleted);
    J->SetNumberField(TEXT("firstContactWorldSeconds"),FirstContactWorldS);J->SetNumberField(TEXT("recoveryRequestWorldSeconds"),RecoveryRequestWorldS);J->SetNumberField(TEXT("rawNativeContactCount"),double(ContactCount));
    J->SetStringField(TEXT("firstSelectedNativeContact"),FirstSelectedContactJson);
    J->SetStringField(TEXT("originalSelectedActor"),OriginalSelectedActorPath);J->SetNumberField(TEXT("selectedEntityIndex"),TargetEntity.Index);
    J->SetNumberField(TEXT("selectedEntitySerial"),TargetEntity.SerialNumber);J->SetArrayField(TEXT("sameEntityReadAndRepresentationEvents"),TrafficReadEvents);
    J->SetArrayField(TEXT("actualSameEntityActorLineage"),TrafficActorLineage);
    J->SetArrayField(TEXT("actualDeferredNativeContactReceipts"),DeferredNativeHitReceipts);
    J->SetBoolField(TEXT("waitingForPhysicalRepresentation"),bWaitingForPhysicalRepresentation);
    J->SetBoolField(TEXT("physicalRoadStagingCompleted"),bRoadStagingComplete);
    J->SetArrayField(TEXT("originalRoadEntryCm"),MayaTrafficEncounter::Vec(EntryCm));
    J->SetNumberField(TEXT("originalEntryEnvelopeCm"),IsCrashFixture()?800:600);
    J->SetArrayField(TEXT("actualStoppedTurnAnchorCm"),MayaTrafficEncounter::Vec(AlignmentAnchorCm));
    J->SetArrayField(TEXT("capturedStraightStagingHeading"),MayaTrafficEncounter::Vec(RoadStagingHeading));
    J->SetStringField(TEXT("roadStagingScope"),TEXT("Actual100cm zero-steer leased motion along captured road-exit heading, then .5s stopped/support/closed latch before turn. Every staging tick checks the whole native footprint and50cm heading first-hit original-road admission retained;20s staging bound. Original EntryCm remains fixed: ordinary600cm/60s, explicit crashfixture800cm/90s contact and120s whole-trial bounds; actual settled endpoint is only the alignment feedback anchor, no teleport or world geometry/collision change."));
    J->SetNumberField(TEXT("maximumFinalTargetAcquisitionDistanceCm"),1000);
    J->SetNumberField(TEXT("originalDirectionEntityIndex"),DirectionEntity.Index);J->SetNumberField(TEXT("originalDirectionEntitySerial"),DirectionEntity.SerialNumber);
    J->SetNumberField(TEXT("originalDirectionObservedSpeedCmps"),DirectionObservedSpeedCmps);
    J->SetBoolField(TEXT("originalDirectionWasMovingWhenObserved"),DirectionObservedSpeedCmps>5);
    J->SetNumberField(TEXT("selectedReadWaitFirstRefusalWorldSeconds"),TargetReadWaitStartedWorldS);
    J->SetStringField(TEXT("selectedReadContinuationScope"),TEXT("Final target is actual native Q&P/blocking traffic within10m, preferring the original observed nearby direction entity, moving or stopped as actually measured. Same final serial never reselected. Known nonphysical same-entity actor representation waits under neutral brake inside60s/6m/deadline; unreadable state retains1s first-refusal limit. Mapping transitions require explicit lineage. Genuine callback during Mass processing may be qualified only by <=.25s bracketing same-serial/actor/epoch/representation mapping reads plus actual callback component physical revalidation; actual callback time is retained, inferred mapping labelled."));
    J->SetNumberField(TEXT("conditionBeforeTraffic"),ConditionBefore);J->SetNumberField(TEXT("appliedHistoryCountBeforeTraffic"),HistoryBefore);J->SetNumberField(TEXT("cashBeforeTraffic"),CashBefore);
    J->SetStringField(TEXT("originalJobId"),JobId);J->SetStringField(TEXT("originalShiftId"),ShiftId);J->SetStringField(TEXT("originalParcelId"),ParcelId);
    J->SetStringField(TEXT("actualTraffic"),LastTargetJson);J->SetStringField(TEXT("actualCamera"),LastCameraJson);J->SetStringField(TEXT("currentRoadAdmission"),LastRoadJson);
    J->SetStringField(TEXT("beforeRecoveryCargo"),BeforeRecoveryJson);J->SetStringField(TEXT("afterRecoveryCargo"),AfterRecoveryJson);J->SetStringField(TEXT("productionRecoveryDetail"),Director?Director->GetLastRecoveryDetail():FString());
    J->SetArrayField(TEXT("actualFrameCaptures"),Screenshots);
    J->SetStringField(TEXT("scope"),TEXT("Explicit Development AutoTest+CurbTest+TrafficEncounterTest. One real pickup, unchanged original city curb entry, actual leased motors toward one nearby physical original traffic entity/actor, possibly naturally stopped (motion scope recorded); at most60s contact search/align/drive and6m road travel, then3s passive reaction and one normally admitted UI recovery within20s. No car spawn/recycle/teleport, traffic/obstacle/control mutation, collision exemption, resource/state/event injection, view mutation or human-key claim. Sensor recognition/braking alone never proves physical contact or universal avoidance. Camera queries are diagnostic, not pixel acceptance."));
    J->SetStringField(TEXT("productionVehicleContactEvidence"),Director?Director->ReadVehicleContactEvidenceJson():FString());
    if(IsCrashFixture())
    {
        J->SetStringField(TEXT("crashFixtureMode"),CrashFixtureMode);
        J->SetBoolField(TEXT("crashFixturePassed"),bCrashFixturePassed);
        J->SetNumberField(TEXT("fixtureContactBoundGameplaySeconds"),90);
        J->SetNumberField(TEXT("fixtureWholeTrialBoundGameplaySeconds"),120);
        J->SetNumberField(TEXT("fixtureLaneStagingBoundGameplaySeconds"),40);
        J->SetNumberField(TEXT("fixtureLaneStagingStartedWorldSeconds"),CrashLaneStartedWorldS);
        J->SetNumberField(TEXT("fixtureLatestImpulseAgeFromEntrySeconds"),85);
        J->SetNumberField(TEXT("fixtureSampleCapacity"),1300);
        J->SetStringField(TEXT("fixtureLaneStagingScope"),TEXT("First eligible physical direction entity/actor/native root is final and immutable. Ordinary leased robot motors first align the final nose (-nativeForward headon, +nativeForward rearend), then join the native car lane while travelling away along +Forward with100cm cross-track lookahead. JOIN keeps that travel sign until lane<=5cm and final heading<=3degrees, followed by an actual .5s supported/latched stop before changing travel direction. The longitudinal approach freezes its signed travel toward nominal550cm and continuously corrects the lane/body heading rather than making a large final pivot. Nominal550 is only a steering target, not a final precision prerequisite. Current actual paired-body corridor and heading<=3degrees initiate braking and are rechecked at the actual settled endpoint. Raw native yaw is unchanged; the player reverse mapper is not used. Current whole native footprint/50cm signed proposed-travel original-road queries, original-entry8m explicit-fixture envelope and coarse native-COM setup distance200..1100cm remain; ordinary traffic retains600cm. Acquisition uses Mass transform position, so its coarse200..1000cm range is not an identical measurement. Native custody/locks/reset and stopped endpoint remain. After actual final facing/stop, impulse admission uses paired exact simple native-body longitudinal ray intersections at3 actual Base-bound heights; projected AABB overlap selects a common candidate line only. Both actual surfaces must face longitudinally with150..400cm positive surface gap, heading<=3degrees, .5s continuous supported/closed robot stop and car XYspeed<=5cm/s. The gap provides nominal prehistory opportunity only; genuine production history and callback remain mandatory. Lane setup40s, contact90s and whole120s all run from unrestarted clocks; impulse by entry85s reserves full5s contact wait. No body teleport, target reselection, geometry or production mutation."));
        if(CrashLaneEvidence.IsValid())J->SetObjectField(TEXT("actualCrashLaneStaging"),CrashLaneEvidence);
        J->SetStringField(TEXT("scope"),TEXT("Explicit controlled original-car physical fixture. Real pickup, curb and robot road alignment precede one bounded velocity-change impulse on the original mapped simulating car. Actual movement/history and genuine native contact, not the impulse request, establish collision. No native callback/fatal-state injection, teleport, mass/collision/LOD modification or natural-driver/human-input claim. The production totalled latch and denied bypasses are checked before real UI NewShift handoff."));
        if(CrashActuation.IsValid())J->SetObjectField(TEXT("controlledCarActuation"),CrashActuation);
        if(CrashOutcome.IsValid())J->SetObjectField(TEXT("actualCrashOutcome"),CrashOutcome);
    }
    if(Full){J->SetArrayField(TEXT("samples"),Samples);J->SetArrayField(TEXT("actualNativeContactReceipts"),Contacts);}
    return MayaTrafficEncounter::Json(J);
}
void AMAYAV01TrafficEncounterTrial::Capture(const TCHAR* Label)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("label"),Label);J->SetNumberField(TEXT("requestWorldSeconds"),GetWorld()->GetTimeSeconds());
    J->SetNumberField(TEXT("requestFrame"),double(GFrameCounter));
    if(!PendingScreenshotPath.IsEmpty()||FScreenshotRequest::IsScreenshotRequested())
    {J->SetBoolField(TEXT("requested"),false);J->SetStringField(TEXT("reason"),TEXT("existing_screenshot_request"));Screenshots.Add(MakeShared<FJsonValueObject>(J));return;}
    PendingScreenshotPath=FPaths::Combine(OutputDirectory,FString::Printf(TEXT("%02d-%s.png"),Screenshots.Num()+1,Label));
    ScreenshotRequestWorldS=GetWorld()->GetTimeSeconds();ScreenshotRequestFrame=GFrameCounter;
    J->SetBoolField(TEXT("requested"),true);J->SetStringField(TEXT("path"),PendingScreenshotPath);J->SetBoolField(TEXT("processed"),false);
    Screenshots.Add(MakeShared<FJsonValueObject>(J));FScreenshotRequest::RequestScreenshot(PendingScreenshotPath,true,false);
}
void AMAYAV01TrafficEncounterTrial::ScreenshotProcessed()
{
    if(PendingScreenshotPath.IsEmpty()||Screenshots.IsEmpty())return;TSharedPtr<FJsonObject> J;
    for(int32 I=Screenshots.Num()-1;I>=0;--I){const auto Row=Screenshots[I]->AsObject();FString Path;if(Row&&Row->TryGetStringField(TEXT("path"),Path)&&Path==PendingScreenshotPath){J=Row;break;}}
    if(J){J->SetBoolField(TEXT("processed"),true);J->SetNumberField(TEXT("processedFrame"),double(GFrameCounter));
        J->SetNumberField(TEXT("processedWorldSeconds"),GetWorld()?GetWorld()->GetTimeSeconds():-1);
        J->SetBoolField(TEXT("filePresent"),IFileManager::Get().FileSize(*PendingScreenshotPath)>0);
        J->SetStringField(TEXT("scope"),TEXT("Actual ordinary view request/processed frames after the named physics observation; no exact-hit-frame or human screenshot claim"));}
    PendingScreenshotPath.Empty();
}
void AMAYAV01TrafficEncounterTrial::Cleanup()
{
    if(ScreenshotHandle.IsValid()){FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);ScreenshotHandle.Reset();}
    for(auto C:BoundBodies)if(C.IsValid())C->OnComponentHit.RemoveDynamic(this,&AMAYAV01TrafficEncounterTrial::OnNativeHit);
    BoundBodies.Reset();if(Robot)Robot->ReleaseDriveCommand();
}
void AMAYAV01TrafficEncounterTrial::EndPlay(EEndPlayReason::Type Reason){Cleanup();Super::EndPlay(Reason);}


AMAYAV01TrafficEncounterTrial::EOutcome AMAYAV01TrafficEncounterTrial::AdvanceCrashFixture(const FMAYADeliveryRobotObservation& O)
{
    const double Now=GetWorld()->GetTimeSeconds();
    const auto S=Director->Snapshot();
    if(bContact)
    {
        Robot->ReleaseDriveCommand(); bAccepted=true;
        Save(O);
        if(!bCrashImpulseApplied)return Fail(TEXT("Contact occurred before controlled actuation; not accepted as this fixture"));
        if(!S.bDroneTotalled)
        {if(Now-FirstContactWorldS>1)return Fail(TEXT("Genuine controlled moving-car contact did not trigger production totalled state"));return EOutcome::Running;}
        const auto& E=S.VehicleCrash.Contact;
        if(!E.IsFatal()||E.VehicleActor!=GetPathNameSafe(TargetActor.Get())||E.WorldSeconds<CrashImpulseWorldS)
            return Fail(TEXT("Totalled receipt does not prove this actual original-car fixture contact"));
        const double NormalForward=FVector::DotProduct(E.Normal,E.RobotOpticalForward);
        if((CrashFixtureMode==TEXT("headon")&&NormalForward>=0)||(CrashFixtureMode==TEXT("rearend")&&NormalForward<=0))
            return Fail(TEXT("Real collision reached the wrong robot end for the declared fixture mode"));
        if(Now-FirstContactWorldS<.2)return EOutcome::Running;
        if(!CrashOutcome.IsValid())
        {
            CrashOutcome=MakeShared<FJsonObject>();
            CrashOutcome->SetNumberField(TEXT("worldSeconds"),Now);
            CrashOutcome->SetStringField(TEXT("actualVehicleEvidence"),Director->ReadVehicleContactEvidenceJson());
            CrashOutcome->SetBoolField(TEXT("actualSupportedAtResult"),O.IsStoppedAndSupported());
            CrashOutcome->SetBoolField(TEXT("actualClosedAtResult"),O.bDoorsClosedLatched);
            auto* PC=Cast<AMAYADeliveryPlayerController>(Robot->GetController());
            const bool Denied=PC&&!PC->ActivateUIAction(TEXT("Recover"))&&!PC->ActivateUIAction(TEXT("Replay"))
                &&!PC->ActivateUIAction(TEXT("Accept"))&&!PC->ActivateUIAction(TEXT("Next"))
                &&!Director->RequestRecover()&&!Director->RequestAccept();
            TSharedPtr<FJsonObject> Native;
            bool Locked=false;const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;
            bool ZeroMotors=FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()),Native)&&Native
                &&Native->TryGetBoolField(TEXT("deliveryDriveLocked"),Locked)&&Locked
                &&Native->TryGetArrayField(TEXT("parts"),Parts)&&Parts&&Parts->Num()>=7;
            for(int32 W=0;ZeroMotors&&W<6;++W)
            {double Nm=0;const auto Part=(*Parts)[W+1]->AsObject();ZeroMotors=Part&&Part->TryGetNumberField(TEXT("motorNm"),Nm)&&FMath::IsFinite(Nm)&&FMath::Abs(Nm)<1.e-6;}
            CrashOutcome->SetBoolField(TEXT("actualRecoveryReplayAcceptNextDenied"),Denied);
            CrashOutcome->SetBoolField(TEXT("actualNativeDriveLockedAndSixMotorsZero"),ZeroMotors);
            if(Native)CrashOutcome->SetObjectField(TEXT("actualNativeRobotAfterFatal"),Native);
            CrashOutcome->SetBoolField(TEXT("actualStartOverEligible"),Director->GetPlayerPresentation().bCanStartNewShift);
            if(!Denied||!ZeroMotors||!Director->GetPlayerPresentation().bCanStartNewShift)return Fail(TEXT("Fatal state permits a bypass or blocks Start Over"));
            Capture(TEXT("drone-totalled-controlled-physical-crash"));
        }
        if(Now-FirstContactWorldS>=1.5&&PendingScreenshotPath.IsEmpty())
        {bCrashFixturePassed=true;Save(O,true);Enter(EStep::Complete,Now);return EOutcome::Complete;}
        return EOutcome::Running;
    }
    if(S.bDroneTotalled)return Fail(TEXT("A different contact totalled the drone before selected fixture contact"));
    if(!ReadTraffic(O,false,true))
    {Command(0,0,true);Save(O,true);return Fail(TEXT("Controlled fixture lost its exact selected original car read"));}
    if(bWaitingForPhysicalRepresentation)return Fail(TEXT("Controlled fixture selected car is no longer physical"));
    auto* Car=Cast<ACitySampleVehicleBase>(TargetActor.Get());
    auto* Body=Car?Cast<UPrimitiveComponent>(Car->GetRootComponent()):nullptr;
    auto* Native=Body?Body->GetBodyInstance():nullptr;
    if(!Body||Body!=CrashSelectedBody.Get()||!Body->IsSimulatingPhysics()||!MayaTrafficEncounter::PhysicalBlockingComponent(Body)
        ||!Native||!Native->IsValidBodyInstance()
        ||uint64(reinterpret_cast<UPTRINT>(Native->GetPhysicsActorHandle()))!=CrashSelectedNativeActor)
        return Fail(TEXT("Controlled car no longer has the same selected component/native simulating body"));
    if(bCrashImpulseApplied)
    {
        Command(0,0,true);Save(O);
        if(Now-CrashImpulseWorldS>5)return Fail(TEXT("No genuine moving-car contact within five seconds of the single bounded fixture impulse"));
        return EOutcome::Running;
    }
    if(S.Phase!=EMAYADeliveryPhase::Carrying||!O.bKnown||!O.bReady||!O.Fault.IsEmpty()||!O.bUpright||O.SupportMask!=63
        ||O.ResetGeneration!=ResetBefore||Director->GetSecuredParcelCount()!=1
        ||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()||FVector::Dist2D(EntryCm,O.ChassisPose.GetLocation())>800
        ||O.VelocityCmps.Size()>90||FMath::Abs(O.ChassisPose.Rotator().Pitch)>20||FMath::Abs(O.ChassisPose.Rotator().Roll)>20||!ReadRoad(O,false))return Fail(TEXT("Controlled approach lost the existing native pre-contact road/identity/custody envelope"));
    if(CrashLaneStartedWorldS<0||Now-CrashLaneStartedWorldS>40)
        return Fail(TEXT("Actual car-lane robot staging did not settle within40gameplay seconds"));
    if(Now-StartWorldS>85)return Fail(TEXT("Controlled fixture setup cannot reserve the full five-second genuine-contact interval"));
    const FTransform CarNativePose=Native->GetUnrealWorldTransform();
    const FVector CarCOM=Body->GetCenterOfMass(),CarForward=CarNativePose.GetUnitAxis(EAxis::X).GetSafeNormal2D();
    const FVector Current=Native->GetUnrealWorldVelocity();
    const FVector RobotPosition=O.ChassisPose.GetLocation(),Optical=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
    if(CarNativePose.ContainsNaN()||CarCOM.ContainsNaN()||Current.ContainsNaN()||CarForward.IsNearlyZero())
        return Fail(TEXT("Selected car native lane pose/COM/velocity is invalid"));
    const bool Rear=CrashFixtureMode==TEXT("rearend");
    const FVector Waypoint=CarCOM+CarForward*550;
    const FVector CarRight(-CarForward.Y,CarForward.X,0);
    const FVector CarToRobot=RobotPosition-CarCOM;
    const double Longitudinal=FVector::DotProduct(CarToRobot,CarForward);
    const double SignedCrossTrack=FVector::DotProduct(CarToRobot,CarRight);
    const double Lateral=FMath::Abs(SignedCrossTrack);
    const double WaypointDistance=FVector::Dist2D(Waypoint,RobotPosition);
    const FVector DesiredFacing=(Rear?1:-1)*CarForward;
    const double FacingError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,DesiredFacing.Rotation().Yaw);
    const double Distance=CarToRobot.Size2D();
    CrashLaneEvidence=MakeShared<FJsonObject>();
    const TCHAR* LaneSteps[]={TEXT("FaceFirst"),TEXT("LaneJoin"),TEXT("LaneApproach"),TEXT("Ready")};
    CrashLaneEvidence->SetStringField(TEXT("step"),LaneSteps[int32(CrashLaneStep)]);
    CrashLaneEvidence->SetNumberField(TEXT("worldSeconds"),Now);
    CrashLaneEvidence->SetArrayField(TEXT("actualNativeCarCOMCm"),MayaTrafficEncounter::Vec(CarCOM));
    CrashLaneEvidence->SetArrayField(TEXT("actualNativeCarForward"),MayaTrafficEncounter::Vec(CarForward));
    CrashLaneEvidence->SetArrayField(TEXT("actualNativeCarVelocityCmps"),MayaTrafficEncounter::Vec(Current));
    CrashLaneEvidence->SetArrayField(TEXT("currentMeasuredLaneWaypointCm"),MayaTrafficEncounter::Vec(Waypoint));
    CrashLaneEvidence->SetArrayField(TEXT("currentRealTurnFeedbackAnchorCm"),MayaTrafficEncounter::Vec(CrashTurnAnchorCm));
    CrashLaneEvidence->SetNumberField(TEXT("waypointDistanceCm"),WaypointDistance);
    CrashLaneEvidence->SetNumberField(TEXT("laneLateralCm"),Lateral);
    CrashLaneEvidence->SetNumberField(TEXT("signedLaneCrossTrackCm"),SignedCrossTrack);
    CrashLaneEvidence->SetNumberField(TEXT("frozenApproachCarAxisTravelSign"),CrashApproachTravelAxisSign);
    CrashLaneEvidence->SetNumberField(TEXT("laneLookaheadCm"),100);
    CrashLaneEvidence->SetNumberField(TEXT("laneLongitudinalCm"),Longitudinal);
    CrashLaneEvidence->SetNumberField(TEXT("facingErrorDegrees"),FacingError);
    CrashLaneEvidence->SetStringField(TEXT("selectedNativeActorIdentity"),FString::Printf(TEXT("%llu"),CrashSelectedNativeActor));
    CrashLaneEvidence->SetNumberField(TEXT("laneSetupElapsedGameplaySeconds"),Now-CrashLaneStartedWorldS);
    if(FVector::Dist2D(EntryCm,Waypoint)>800||Distance<200||Distance>1100)
        return Fail(TEXT("Measured car-lane waypoint exceeds original-entry8m fixture or declared200..1100cm native-COM setup-distance envelope"));
    // A moving target is held under brake, not chased; the setup clock keeps running.
    if(Current.Size2D()>5){Command(0,0,true);CrashStoppedSinceWorldS=-1;Save(O);return EOutcome::Running;}
    // The loaded chassis walks during large pivots: align before correcting the lane,
    // never demand a large final turn at a point that the turn itself can displace.
    if(CrashLaneStep==ECrashLaneStep::FaceFirst)
    {
        if(FMath::Abs(FacingError)>3)
        {
            CrashStoppedSinceWorldS=-1;
            const float Correction=float(FMath::Clamp(.02*FVector::DotProduct(CrashTurnAnchorCm-RobotPosition,Optical)
                -.015*O.OpticalForwardCmps,-.35,.35));
            Command(Correction,FacingError>0?1.f:-1.f,false);Save(O);return EOutcome::Running;
        }
        Command(0,0,true);
        if(!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched){CrashStoppedSinceWorldS=-1;Save(O);return EOutcome::Running;}
        if(CrashStoppedSinceWorldS<0)CrashStoppedSinceWorldS=Now;
        if(Now-CrashStoppedSinceWorldS<.5){Save(O);return EOutcome::Running;}
        CrashLaneStep=ECrashLaneStep::LaneJoin;CrashStoppedSinceWorldS=-1;
        Save(O,true);return EOutcome::Running;
    }
    auto DriveLane=[&](double CarAxisTravelSign)->EOutcome
    {
        // Raw BODY yaw follows geometric nose error even with negative throttle.
        const double DriveSign=(Rear?1:-1)*CarAxisTravelSign;
        const FVector DesiredTravel=(CarForward*(CarAxisTravelSign*100)-CarRight*SignedCrossTrack).GetSafeNormal2D();
        const FVector DesiredBodyFacing=DriveSign*DesiredTravel;
        const double BodyYawError=FMath::FindDeltaAngleDegrees(Optical.Rotation().Yaw,DesiredBodyFacing.Rotation().Yaw);
        const float Steering=float(FMath::Clamp(BodyYawError/35.,-.65,.65));
        const bool SpeedBrake=DriveSign*O.OpticalForwardCmps>65;
        CrashLaneEvidence->SetNumberField(TEXT("commandCarAxisTravelSign"),CarAxisTravelSign);
        CrashLaneEvidence->SetNumberField(TEXT("commandRawOpticalThrottleSign"),DriveSign);
        CrashLaneEvidence->SetArrayField(TEXT("commandDesiredTravelHeading"),MayaTrafficEncounter::Vec(DesiredTravel));
        CrashLaneEvidence->SetArrayField(TEXT("commandDesiredBodyHeading"),MayaTrafficEncounter::Vec(DesiredBodyFacing));
        CrashLaneEvidence->SetNumberField(TEXT("commandBodyYawErrorDegrees"),BodyYawError);
        CrashLaneEvidence->SetNumberField(TEXT("commandRawSteer"),Steering);
        CrashLaneEvidence->SetBoolField(TEXT("signedSpeedBrake"),SpeedBrake);
        if(!ReadRoad(O,true,DriveSign))return Fail(TEXT("Signed car-lane translation lacks actual footprint/50cm proposed-travel original-road admission"));
        CrashStoppedSinceWorldS=-1;
        Command(SpeedBrake?0.f:float(DriveSign*.5),Steering,SpeedBrake);
        Save(O);return EOutcome::Running;
    };
    if(CrashLaneStep==ECrashLaneStep::LaneJoin)
    {
        // Keep travelling AWAY on +carForward until lane/nose alignment is real.
        // Do not flip at longitudinal550 while the lateral correction is unfinished.
        if(Lateral>5||FMath::Abs(FacingError)>3)return DriveLane(1);
        Command(0,0,true);
        if(!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched){CrashStoppedSinceWorldS=-1;Save(O);return EOutcome::Running;}
        if(CrashStoppedSinceWorldS<0)CrashStoppedSinceWorldS=Now;
        if(Now-CrashStoppedSinceWorldS<.5){Save(O);return EOutcome::Running;}
        CrashApproachTravelAxisSign=Longitudinal<550?1:-1;
        CrashLaneStep=ECrashLaneStep::LaneApproach;CrashStoppedSinceWorldS=-1;
        Save(O,true);return EOutcome::Running;
    }
    if(FMath::Abs(FacingError)>3)
        return DriveLane(CrashApproachTravelAxisSign);
    // During the aligned lane approach and again after actual braking, test the
    // native collision silhouettes; nominal center position is not proof of contact.
    // AABB projection only chooses a shared line; exact simple-body rays are decisive.
    auto Geometry=MakeShared<FJsonObject>();
    Geometry->SetStringField(TEXT("scope"),TEXT("Prospective current native-body longitudinal collision corridor queries, rechecked at the actual stationary impulse endpoint; not an actual contact or future path certificate"));
    Geometry->SetStringField(TEXT("directBodyRayHitSemantics"),TEXT("UE5.3 exact FBodyInstance::LineTrace uses a touch filter: successful ray return, not bBlockingHit, establishes shape intersection. Raw blocking flags remain reported. Separately current selected car and owned independent Base must be actual QueryAndPhysics/ECC_PhysicsBody-blocking native bodies; finite noninitial geometry, opposed longitudinal normals and150..400cm surface gap remain mandatory."));
    auto* Base=Cast<UPrimitiveComponent>(Robot->GetRootComponent());
    auto* BaseNative=Base?Base->GetBodyInstance():nullptr;
    bool OwnedBase=false;
    for(const auto& C:BoundBodies)OwnedBase|=C.Get()==Base;
    bool Corridor=false;
    TArray<TSharedPtr<FJsonValue>> RayRows;
    if(OwnedBase&&MayaTrafficEncounter::PhysicalBlockingComponent(Base)&&BaseNative
        &&BaseNative==Base->GetBodyInstance(NAME_None,false)&&BaseNative->IsValidBodyInstance())
    {
        const FBox CarBounds=Native->GetBodyBounds(),BaseBounds=BaseNative->GetBodyBounds();
        Geometry->SetStringField(TEXT("actualCarBody"),Body->GetPathName());
        Geometry->SetStringField(TEXT("actualRobotBaseBody"),Base->GetPathName());
        Geometry->SetArrayField(TEXT("nativeCarBoundsMinCm"),MayaTrafficEncounter::Vec(CarBounds.Min));
        Geometry->SetArrayField(TEXT("nativeCarBoundsMaxCm"),MayaTrafficEncounter::Vec(CarBounds.Max));
        Geometry->SetArrayField(TEXT("nativeRobotBaseBoundsMinCm"),MayaTrafficEncounter::Vec(BaseBounds.Min));
        Geometry->SetArrayField(TEXT("nativeRobotBaseBoundsMaxCm"),MayaTrafficEncounter::Vec(BaseBounds.Max));
        if(CarBounds.IsValid!=0&&BaseBounds.IsValid!=0&&!CarBounds.Min.ContainsNaN()&&!CarBounds.Max.ContainsNaN()
            &&!BaseBounds.Min.ContainsNaN()&&!BaseBounds.Max.ContainsNaN())
        {
            const FVector Right(-CarForward.Y,CarForward.X,0);
            auto LateralInterval=[&](const FBox& B)
            {
                FVector2D Interval(MAX_dbl,-MAX_dbl);
                for(double X:{B.Min.X,B.Max.X})for(double Y:{B.Min.Y,B.Max.Y})
                {
                    const double P=FVector::DotProduct(FVector(X,Y,CarCOM.Z)-CarCOM,Right);
                    Interval.X=FMath::Min(Interval.X,P);Interval.Y=FMath::Max(Interval.Y,P);
                }
                return Interval;
            };
            const FVector2D CarInterval=LateralInterval(CarBounds),BaseInterval=LateralInterval(BaseBounds);
            const double SharedMin=FMath::Max(CarInterval.X,BaseInterval.X),SharedMax=FMath::Min(CarInterval.Y,BaseInterval.Y);
            Geometry->SetNumberField(TEXT("projectedLateralOverlapMinCm"),SharedMin);
            Geometry->SetNumberField(TEXT("projectedLateralOverlapMaxCm"),SharedMax);
            Geometry->SetBoolField(TEXT("projectedBoundsPlausiblyOverlap"),SharedMax>SharedMin);
            if(SharedMax>SharedMin)
            {
                const double SharedLateral=(SharedMin+SharedMax)*.5;
                for(double HeightFraction:{.25,.5,.75})
                {
                    FVector LineCenter=CarCOM+Right*SharedLateral;
                    LineCenter.Z=FMath::Lerp(BaseBounds.Min.Z,BaseBounds.Max.Z,HeightFraction);
                    const FVector Behind=LineCenter-CarForward*1100,Ahead=LineCenter+CarForward*1100;
                    FHitResult CarHit,BaseHit;
                    const bool CarRay=Native->LineTrace(CarHit,Ahead,Behind,false,false);
                    const bool BaseRay=BaseNative->LineTrace(BaseHit,Behind,Ahead,false,false);
                    auto Ray=MakeShared<FJsonObject>();
                    Ray->SetNumberField(TEXT("robotBaseBoundHeightFraction"),HeightFraction);
                    Ray->SetArrayField(TEXT("sharedWorldLineCenterCm"),MayaTrafficEncounter::Vec(LineCenter));
                    Ray->SetBoolField(TEXT("actualSimpleCarBodyRayHit"),CarRay);
                    Ray->SetBoolField(TEXT("actualSimpleRobotBaseBodyRayHit"),BaseRay);
                    Ray->SetBoolField(TEXT("carRayBlocking"),CarHit.bBlockingHit);
                    Ray->SetBoolField(TEXT("robotRayBlocking"),BaseHit.bBlockingHit);
                    Ray->SetBoolField(TEXT("carRayInitialOverlap"),CarHit.bStartPenetrating);
                    Ray->SetBoolField(TEXT("robotRayInitialOverlap"),BaseHit.bStartPenetrating);
                    if(CarRay&&BaseRay&&!CarHit.bStartPenetrating&&!BaseHit.bStartPenetrating
                        &&!CarHit.ImpactPoint.ContainsNaN()&&!BaseHit.ImpactPoint.ContainsNaN()
                        &&!CarHit.ImpactNormal.ContainsNaN()&&!BaseHit.ImpactNormal.ContainsNaN())
                    {
                        const FVector CarNormal=CarHit.ImpactNormal.GetSafeNormal2D(),BaseNormal=BaseHit.ImpactNormal.GetSafeNormal2D();
                        const double CarLong=FVector::DotProduct(CarNormal,CarForward),BaseLong=FVector::DotProduct(BaseNormal,CarForward);
                        const bool bLongitudinalSurfaces=CarLong>0&&BaseLong<0
                            &&FMath::Abs(CarLong)>=FMath::Abs(FVector::DotProduct(CarNormal,Right))
                            &&FMath::Abs(BaseLong)>=FMath::Abs(FVector::DotProduct(BaseNormal,Right));
                        const double Gap=FVector::DotProduct(BaseHit.ImpactPoint-CarHit.ImpactPoint,CarForward);
                        Ray->SetArrayField(TEXT("actualCarSurfaceCm"),MayaTrafficEncounter::Vec(CarHit.ImpactPoint));
                        Ray->SetArrayField(TEXT("actualRobotBaseSurfaceCm"),MayaTrafficEncounter::Vec(BaseHit.ImpactPoint));
                        Ray->SetArrayField(TEXT("actualCarSurfaceNormal"),MayaTrafficEncounter::Vec(CarHit.ImpactNormal));
                        Ray->SetArrayField(TEXT("actualRobotSurfaceNormal"),MayaTrafficEncounter::Vec(BaseHit.ImpactNormal));
                        Ray->SetNumberField(TEXT("actualLongitudinalSurfaceGapCm"),Gap);
                        Ray->SetBoolField(TEXT("actualBothSurfacesLongitudinal"),bLongitudinalSurfaces);
                        const bool Accepted=bLongitudinalSurfaces&&FMath::IsFinite(Gap)&&Gap>=150&&Gap<=400;
                        Ray->SetBoolField(TEXT("actualNativeCollisionCorridorAdmitted"),Accepted);
                        Corridor|=Accepted;
                    }
                    RayRows.Add(MakeShared<FJsonValueObject>(Ray));
                }
            }
        }
    }
    Geometry->SetArrayField(TEXT("actualPairedNativeBodyRays"),RayRows);
    Geometry->SetBoolField(TEXT("actualNativeCollisionCorridorAdmitted"),Corridor);
    CrashLaneEvidence->SetObjectField(TEXT("actualNativeCollisionCorridor"),Geometry);
    if(!Corridor)
    {
        return DriveLane(CrashApproachTravelAxisSign);
    }
    Command(0,0,true);
    if(!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched){CrashStoppedSinceWorldS=-1;Save(O);return EOutcome::Running;}
    CrashLaneStep=ECrashLaneStep::Ready;
    if(CrashStoppedSinceWorldS<0)CrashStoppedSinceWorldS=Now;
    if(Now-CrashStoppedSinceWorldS<.5){Save(O);return EOutcome::Running;}
    // One declared physical velocity-change impulse; all mass/shape/collision settings remain untouched.
    const FVector DeltaV=CarForward*1000-FVector(Current.X,Current.Y,0);
    CrashActuation=MakeShared<FJsonObject>();
    CrashActuation->SetStringField(TEXT("kind"),TEXT("ONE_CONTROLLED_NATIVE_VELOCITY_CHANGE_IMPULSE_NOT_NATURAL_TRAFFIC"));
    CrashActuation->SetStringField(TEXT("actor"),Car->GetPathName());CrashActuation->SetStringField(TEXT("body"),Body->GetPathName());
    CrashActuation->SetNumberField(TEXT("worldSeconds"),Now);CrashActuation->SetNumberField(TEXT("nativeMassKgUnchanged"),Body->GetMass());
    CrashActuation->SetNumberField(TEXT("actualCarRobotDistanceCm"),Distance);
    CrashActuation->SetNumberField(TEXT("actualLaneLateralCm"),Lateral);
    CrashActuation->SetNumberField(TEXT("actualLaneLongitudinalCm"),Longitudinal);
    CrashActuation->SetNumberField(TEXT("actualRobotFacingErrorDegrees"),FacingError);
    CrashActuation->SetArrayField(TEXT("actualNativeCarLongitudinalForward"),MayaTrafficEncounter::Vec(CarForward));
    CrashActuation->SetObjectField(TEXT("actualLaneStagingAtImpulse"),CrashLaneEvidence);
    CrashActuation->SetArrayField(TEXT("actualVelocityBeforeCmps"),MayaTrafficEncounter::Vec(Current));
    CrashActuation->SetArrayField(TEXT("requestedDeltaVelocityCmps"),MayaTrafficEncounter::Vec(DeltaV));
    CrashActuation->SetStringField(TEXT("evidenceScope"),TEXT("Actuation only. Subsequent production pre-contact native history and genuine callback must independently establish moving contact."));
    Body->AddImpulse(DeltaV,NAME_None,true);bCrashImpulseApplied=true;CrashImpulseWorldS=Now;
    Capture(TEXT("controlled-car-impulse-before-contact"));Save(O,true);return EOutcome::Running;
}
