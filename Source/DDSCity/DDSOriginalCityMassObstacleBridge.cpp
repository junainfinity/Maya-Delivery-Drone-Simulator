#include "DDSOriginalCityMassObstacleBridge.h"
#include "DDSCityRollingPawn.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassSimulationSubsystem.h"
#include "MassCommands.h"
#include "MassCommandBuffer.h"
#include "MassCommonFragments.h"
#include "MassMovementFragments.h"
#include "MassNavigationFragments.h"
#include "MassNavigationSubsystem.h"
#include "MassTrafficFragments.h"
#include "MassTrafficSubsystem.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PBDRigidsSolver.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <cmath>

namespace
{
constexpr double BoundsReadBudgetS = 0.020;
bool ValidBounds(const FBox& Box)
{
    const FVector Size = Box.GetSize();
    return Box.IsValid && !Box.Min.ContainsNaN() && !Box.Max.ContainsNaN()
        && Size.X > 1.e-4 && Size.Y > 1.e-4 && Size.Z > 1.e-4;
}
bool OwnedEntityMatches(FMassEntityManager& Manager, FMassEntityHandle Entity,
    TWeakObjectPtr<UDDSOriginalCityMassObstacleBridge> Owner, uint64 Serial)
{
    if (!Manager.IsEntityActive(Entity)) return false;
    const auto* Data = Manager.GetFragmentDataPtr<FDDSOriginalCityObstacleOwnerFragment>(Entity);
    return Data && Data->Owner.HasSameIndexAndSerialNumber(Owner) && Data->OperationSerial == Serial;
}
}

UDDSOriginalCityMassObstacleBridge::UDDSOriginalCityMassObstacleBridge()
{
    PrimaryComponentTick.bCanEverTick = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    bAutoActivate = false;
}

bool UDDSOriginalCityMassObstacleBridge::CurrentContext() const
{
    auto* World = OriginalWorld.Get(); auto* Pawn = FollowedPawn.Get(); auto* Rig = FollowedRig.Get();
    auto* Entities = EntitySubsystem.Get(); auto* Simulation = SimulationSubsystem.Get(); auto* Mode = OriginalMode.Get();
    const auto Manager = OwnedManager.Pin();
    if (!IsInGameThread() || !IsValid(this) || !IsRegistered() || !bFollowing || bStopping
        || !IsValid(World) || World->bIsTearingDown || World->WorldType != EWorldType::Game
        || World->GetNetMode() != NM_Standalone || World->GetOutermost()->GetName() != TEXT("/Game/Map/Small_City_LVL")
        || GetWorld() != World || !IsValid(Pawn) || GetOwner() != Pawn || Pawn->GetWorld() != World
        || Pawn->IsActorBeingDestroyed() || !IsValid(Rig) || !Rig->IsRegistered() || Rig->GetOwner() != Pawn
        || Rig->GetWorld() != World || Pawn->GetBoundPhysicalRig() != Rig
        || Rig->GetRigState() != EDDSRobotRigState::Active || Rig->GetGeneration() != BoundGeneration
        || !World->GetPhysicsScene() || !World->GetPhysicsScene()->GetSolver()
        || !IsValid(Mode) || World->GetAuthGameMode() != Mode
        || Mode->GetClass()->GetPathName() != TEXT("/Game/Gameplay/Framework/BP_CitySampleGameMode.BP_CitySampleGameMode_C")
        || !IsValid(Entities) || Entities->GetWorld() != World || World->GetSubsystem<UMassEntitySubsystem>() != Entities
        || !IsValid(Simulation) || Simulation->GetWorld() != World || !Simulation->IsSimulationStarted()
        || World->GetSubsystem<UMassSimulationSubsystem>() != Simulation || !Manager.IsValid()
        || Manager->GetWorld() != World || &Entities->GetMutableEntityManager() != Manager.Get()
        || !World->GetSubsystem<UMassNavigationSubsystem>() || !World->GetSubsystem<UMassTrafficSubsystem>()) return false;
    const auto& Binding = Rig->GetMeasuredBinding();
    for (int32 I = 0; I < 9; ++I)
    {
        auto* Body = OriginalBodies[I].Get();
        if (!IsValid(Body) || Binding.Bodies[I].ActualPart != Body || !Body->IsRegistered()
            || Body->GetWorld() != World || Body->GetOwner() != Pawn || !Body->IsPhysicsStateCreated()
            || !Body->IsPhysicsCollisionEnabled()) return false;
    }
    // No controller, input, support mask or world-ready test: a stopped robot remains an obstacle.
    return Rig->GetActualChassis() == OriginalBodies[0].Get();
}

bool UDDSOriginalCityMassObstacleBridge::CaptureCoherentFootprint(
    FDDSOriginalCityObstacleObservation& Out, FString& Error) const
{
    Out = FDDSOriginalCityObstacleObservation();
    const double Started = FPlatformTime::Seconds();
    if (!FMath::IsFinite(Started) || !CurrentContext()) { Error = TEXT("OBSTACLE_CONTEXT_LOST"); return false; }
    TStrongObjectPtr<ADDSCityRollingPawn> PawnPin(FollowedPawn.Get());
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(FollowedRig.Get());
    TStrongObjectPtr<UWorld> WorldPin(OriginalWorld.Get());
    TStrongObjectPtr<UStaticMeshComponent> BodyPins[9];
    FPhysicsActorHandle Handles[9] = {};
    FDDSRobotSnapshot Snapshot;
    if (!PawnPin->GetPreReadinessPhysicalState(Snapshot)) { Error = TEXT("FRESH_COMPLETED_PRE_READINESS_STATE_REQUIRED"); return false; }
    auto* Rig = RigPin.Get(); auto* Scene = WorldPin->GetPhysicsScene(); auto* Solver = Scene->GetSolver();
    if (Snapshot.Generation != BoundGeneration || Snapshot.CollisionWorldGeneration != Rig->GetCollisionWorldEpoch()
        || Snapshot.ReadinessRevision != Rig->GetReadinessRevision()) { Error = TEXT("COMPLETED_STATE_CONTEXT_MISMATCH"); return false; }
    for (int32 I = 0; I < 9; ++I)
    {
        BodyPins[I].Reset(OriginalBodies[I].Get());
        auto* Body = BodyPins[I].Get(); const auto& State = Snapshot.Bodies[I];
        auto* BI = Body->GetBodyInstance();
        Handles[I] = BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
        if (!State.bLive || State.ParticleId == INDEX_NONE || State.WorldCm.ContainsNaN()
            || !State.WorldCm.GetRotation().IsNormalized() || !State.WorldCm.GetScale3D().Equals(FVector::OneVector, 1.e-6)
            || State.VelocityCmps.ContainsNaN() || State.AngularVelocityRadps.ContainsNaN()
            || State.COMLocalCm.ContainsNaN() || !Handles[I] || Handles[I]->GetSolverBase() != Solver)
        { Error = TEXT("ACTUAL_NINE_BODY_PHYSICS_REQUIRED"); return false; }
        bool bMatchedParticle = false;
        const bool bRead = FPhysicsCommand::ExecuteRead(Handles[I], [&](const FPhysicsActorHandle& Actor)
        {
            if (Actor == Handles[I] && Actor->GetSolverBase() == Solver
                && Actor->GetGameThreadAPI().UniqueIdx().Idx == State.ParticleId)
            {
                // Native Geometry()->BoundingBox(), transformed by this ONE completed snapshot.
                // Never union nine independently sampled live world poses or apply supplier scale twice.
                Out.BodyWorldBounds[I] = FPhysicsInterface::GetBounds_AssumesLocked(Actor, State.WorldCm);
                bMatchedParticle = true;
            }
        });
        ++Out.NativeBoundsReadAttempts;
        if (!bRead || !bMatchedParticle || !ValidBounds(Out.BodyWorldBounds[I]))
        { Error = TEXT("NATIVE_COLLISION_BOUNDS_UNKNOWN"); return false; }
        ++Out.NativeBoundsReadCount;
        Out.ParticleIds[I] = State.ParticleId;
        Out.AssemblyWorldBounds += Out.BodyWorldBounds[I];
        if (FPlatformTime::Seconds() - Started > BoundsReadBudgetS)
        { Error = TEXT("NATIVE_BOUNDS_READ_BUDGET_EXCEEDED"); return false; }
    }
    // Re-read all source identities, actor handles, scene and epoch/revision after all nine locks.
    FDDSRobotSnapshot Final;
    if (!CurrentContext() || WorldPin->GetPhysicsScene() != Scene || Scene->GetSolver() != Solver
        || !PawnPin->GetPreReadinessPhysicalState(Final) || Final.Generation != Snapshot.Generation
        || Final.CollisionWorldGeneration != Snapshot.CollisionWorldGeneration
        || Final.ReadinessRevision != Snapshot.ReadinessRevision || Final.StepSequence != Snapshot.StepSequence
        || Final.CapturedRealS != Snapshot.CapturedRealS)
    { Error = TEXT("NINE_BODY_READ_CONTEXT_CHANGED"); return false; }
    for (int32 I = 0; I < 9; ++I)
    {
        auto* Body = BodyPins[I].Get(); auto* BI = Body->GetBodyInstance();
        if (OriginalBodies[I].Get() != Body || Rig->GetMeasuredBinding().Bodies[I].ActualPart != Body
            || !BI || !BI->IsValidBodyInstance() || BI->GetPhysicsActorHandle() != Handles[I]
            || Handles[I]->GetSolverBase() != Solver || Handles[I]->GetGameThreadAPI().UniqueIdx().Idx != Snapshot.Bodies[I].ParticleId
            || Final.Bodies[I].ParticleId != Snapshot.Bodies[I].ParticleId
            || !Final.Bodies[I].WorldCm.Equals(Snapshot.Bodies[I].WorldCm, 1.e-6))
        { Error = TEXT("NATIVE_BODY_IDENTITY_CHANGED"); return false; }
    }
    Out.ChassisWorldCm = Snapshot.Bodies[0].WorldCm;
    const FVector Origin = Out.ChassisWorldCm.GetLocation();
    const FVector Low = Out.AssemblyWorldBounds.Min - Origin, High = Out.AssemblyWorldBounds.Max - Origin;
    const double X = FMath::Max(FMath::Abs(Low.X), FMath::Abs(High.X));
    const double Y = FMath::Max(FMath::Abs(Low.Y), FMath::Abs(High.Y));
    const double Radius = FMath::Sqrt(X * X + Y * Y);
    const auto& Chassis = Snapshot.Bodies[0];
    const FVector COM = Chassis.WorldCm.TransformPosition(Chassis.COMLocalCm);
    Out.ChassisOriginVelocityCmps = Chassis.VelocityCmps + FVector::CrossProduct(Chassis.AngularVelocityRadps, Origin - COM);
    Out.ReadFinishedRealS = FPlatformTime::Seconds();
    if (!FMath::IsFinite(Radius) || Radius <= 0 || Radius > MAX_flt || Out.ChassisOriginVelocityCmps.ContainsNaN()
        || !FMath::IsFinite(Out.ReadFinishedRealS) || Out.ReadFinishedRealS - Started > BoundsReadBudgetS)
    { Error = TEXT("FINITE_COHERENT_SENSING_ENVELOPE_REQUIRED"); return false; }
    Out.SensingRadiusCm = static_cast<float>(Radius);
    if (static_cast<double>(Out.SensingRadiusCm) < Radius)
        Out.SensingRadiusCm = std::nextafter(Out.SensingRadiusCm, MAX_flt); // Round outward, never shrink the envelope.
    Out.RigGeneration = Snapshot.Generation; Out.WorldEpoch = Snapshot.CollisionWorldGeneration;
    Out.ReadinessRevision = Snapshot.ReadinessRevision; Out.CompletedStep = Snapshot.StepSequence;
    Out.CompletedPhysicsTimeS = Snapshot.CompletedStateTimeS; Out.CapturedRealS = Snapshot.CapturedRealS;
    Error.Reset(); return true;
}

bool UDDSOriginalCityMassObstacleBridge::RefreshOwnedSensor(FString& Error)
{
    if (bRefreshing || bRemovalPending || !CurrentContext()) { Error = TEXT("OBSTACLE_REFRESH_NOT_AVAILABLE"); return false; }
    TGuardValue<bool> RefreshGuard(bRefreshing, true);
    const auto Manager = OwnedManager.Pin();
    if (Manager->IsProcessing()) { Error = TEXT("MASS_PHASE_BOUNDARY_REQUIRED"); RetireOwnedSensor(); return false; }
    FMassArchetypeHandle Archetype;
    if (!SensorEntity.IsSet())
    {
        const UScriptStruct* Types[] = { FDDSOriginalCityObstacleOwnerFragment::StaticStruct(),
            FTransformFragment::StaticStruct(), FMassVelocityFragment::StaticStruct(), FAgentRadiusFragment::StaticStruct(),
            FMassNavigationObstacleGridCellLocationFragment::StaticStruct(), FMassTrafficObstacleTag::StaticStruct() };
        Archetype = Manager->CreateArchetype(MakeArrayView(Types), TEXT("DDSOriginalRobotSensingOnly"));
        // OnNewArchetype may invoke external callbacks. Finish it BEFORE capturing
        // epoch/revision/geometry, then revalidate the complete owned context.
        if (!CurrentContext() || !Archetype.IsValid() || bRemovalPending)
        { Error = TEXT("SENSOR_ARCHETYPE_CONTEXT_CHANGED"); return false; }
    }
    FDDSOriginalCityObstacleObservation Observation;
    if (!CaptureCoherentFootprint(Observation, Error))
    {
        Observation.Refusal = Error; LastObservation = Observation;
        RetireOwnedSensor(); return false; // Unknown/stale pose is never presented as a current obstacle.
    }
    if (!SensorEntity.IsSet())
    {
        // Installed 5.3 InternalBuildEntity only adds entity data; it does not
        // broadcast creation observers. No external callback intervenes here.
        SensorEntity = Manager->CreateEntity(Archetype);
        auto& Owner = Manager->GetFragmentDataChecked<FDDSOriginalCityObstacleOwnerFragment>(SensorEntity);
        Owner.Owner = this; Owner.OperationSerial = OperationSerial;
    }
    if (!CurrentContext() || !OwnedEntityMatches(*Manager, SensorEntity,
        TWeakObjectPtr<UDDSOriginalCityMassObstacleBridge>(this), OperationSerial))
    { Error = TEXT("SENSOR_OWNERSHIP_CONTEXT_CHANGED"); RetireOwnedSensor(); return false; }
    auto& Owner = Manager->GetFragmentDataChecked<FDDSOriginalCityObstacleOwnerFragment>(SensorEntity);
    Owner.RigGeneration = Observation.RigGeneration; Owner.WorldEpoch = Observation.WorldEpoch;
    Owner.ReadinessRevision = Observation.ReadinessRevision; Owner.CompletedStep = Observation.CompletedStep;
    Manager->GetFragmentDataChecked<FTransformFragment>(SensorEntity).SetTransform(Observation.ChassisWorldCm);
    Manager->GetFragmentDataChecked<FMassVelocityFragment>(SensorEntity).Value = Observation.ChassisOriginVelocityCmps;
    Manager->GetFragmentDataChecked<FAgentRadiusFragment>(SensorEntity).Radius = Observation.SensingRadiusCm;
    // Original grid processor and traffic find/TTC processors consume these public fragments.
    // No movement target/force/vehicle/shared simulation data: no replacement AI or reciprocity claim.
    Observation.bPublishedSensor = true; LastObservation = Observation; Error.Reset(); return true;
}

bool UDDSOriginalCityMassObstacleBridge::StartObservingBoundOriginalAssembly(ADDSCityRollingPawn* Pawn, FString& Error)
{
    check(IsInGameThread());
    if (bFollowing || bRefreshing || bStopping || bRemovalPending || SensorEntity.IsSet()
        || !IsValid(this) || !IsRegistered() || !IsValid(Pawn) || GetOwner() != Pawn || OperationSerial == MAX_uint64)
    { Error = TEXT("OBSTACLE_BRIDGE_NOT_STARTABLE"); return false; }
    TInlineComponentArray<UDDSOriginalCityMassObstacleBridge*> Bridges(Pawn);
    for (auto* Bridge : Bridges)
        if (IsValid(Bridge) && Bridge != this && (Bridge->bFollowing || Bridge->bRemovalPending || Bridge->SensorEntity.IsSet()))
        { Error = TEXT("ONE_ORIGINAL_ASSEMBLY_SENSOR_OWNER_REQUIRED"); return false; }
    auto* World = Pawn->GetWorld(); auto* Rig = Pawn->GetBoundPhysicalRig();
    auto* Entities = IsValid(World) ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
    auto* Simulation = IsValid(World) ? World->GetSubsystem<UMassSimulationSubsystem>() : nullptr;
    if (!IsValid(Rig) || !IsValid(Entities) || !IsValid(Simulation) || !Simulation->IsSimulationStarted()
        || Entities->GetMutableEntityManager().IsProcessing())
    { Error = TEXT("BOUND_PHYSICS_AND_IDLE_ORIGINAL_MASS_REQUIRED"); return false; }
    FollowedPawn = Pawn; FollowedRig = Rig; OriginalWorld = World; OriginalMode = World->GetAuthGameMode();
    EntitySubsystem = Entities; SimulationSubsystem = Simulation;
    OwnedManager = Entities->GetMutableEntityManager().AsShared();
    BoundGeneration = Rig->GetGeneration(); ++OperationSerial;
    for (int32 I = 0; I < 9; ++I) OriginalBodies[I] = Rig->GetMeasuredBinding().Bodies[I].ActualPart;
    bFollowing = true;
    if (!RefreshOwnedSensor(Error)) { StopObservingBoundOriginalAssembly(); return false; }
    PrePhaseHandle = Simulation->GetOnProcessingPhaseStarted(EMassProcessingPhase::PrePhysics)
        .AddUObject(this, &UDDSOriginalCityMassObstacleBridge::PhaseStarting);
    PostPhaseHandle = Simulation->GetOnProcessingPhaseStarted(EMassProcessingPhase::PostPhysics)
        .AddUObject(this, &UDDSOriginalCityMassObstacleBridge::PhaseStarting);
    WorldEndingHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UDDSOriginalCityMassObstacleBridge::WorldEnding);
    Error.Reset(); return true;
}

void UDDSOriginalCityMassObstacleBridge::RetireOwnedSensor()
{
    LastObservation.bPublishedSensor = false;
    if (!SensorEntity.IsSet()) return;
    const auto Manager = OwnedManager.Pin(); const auto Entity = SensorEntity;
    const TWeakObjectPtr<UDDSOriginalCityMassObstacleBridge> Owner(this);
    const auto World = OriginalWorld; const uint64 Serial = OperationSerial;
    SensorEntity = FMassEntityHandle(); // Reset before original observer callbacks; no double retirement.
    if (!Manager.IsValid() || !World.IsValid() || Manager->GetWorld() != World.Get()) return;
    if (!Manager->IsProcessing())
    {
        if (OwnedEntityMatches(*Manager, Entity, Owner, Serial)) Manager->DestroyEntity(Entity);
        return; // DestroyEntity invokes original navigation grid removal observer.
    }
    bRemovalPending = true;
    const auto ManagerIdentity = OwnedManager;
    Manager->Defer().PushCommand<FMassDeferredDestroyCommand>([Owner, World, ManagerIdentity, Entity, Serial](FMassEntityManager& System)
    {
        const auto ExpectedManager = ManagerIdentity.Pin();
        if (!World.IsValid() || ExpectedManager.Get() != &System || System.GetWorld() != World.Get()) return;
        if (System.IsEntityActive(Entity))
        {
            if (!OwnedEntityMatches(System, Entity, Owner, Serial)) return;
            System.DestroyEntity(Entity);
        }
        if (auto* Bridge = Owner.Get(); IsValid(Bridge) && Bridge->OperationSerial == Serial) Bridge->bRemovalPending = false;
    });
}

void UDDSOriginalCityMassObstacleBridge::PhaseStarting(float DeltaSeconds)
{
    check(IsInGameThread());
    if (!bFollowing || bRefreshing || bStopping) return;
    if (!CurrentContext()) { LastObservation.Refusal = TEXT("ORIGINAL_MASS_OR_PHYSICS_LIFETIME_LOST"); StopObservingBoundOriginalAssembly(); return; }
    FString Error;
    if (!RefreshOwnedSensor(Error)) LastObservation.Refusal = Error;
    // A transient missing completed state retires the sensor; the next real fresh
    // observation may recreate it within this explicitly started same-generation session.
}

void UDDSOriginalCityMassObstacleBridge::StopObservingBoundOriginalAssembly()
{
    check(IsInGameThread());
    if (bStopping) return;
    TGuardValue<bool> StopGuard(bStopping, true);
    bFollowing = false;
    if (auto* Simulation = SimulationSubsystem.Get(); IsValid(Simulation))
    {
        Simulation->GetOnProcessingPhaseStarted(EMassProcessingPhase::PrePhysics).Remove(PrePhaseHandle);
        Simulation->GetOnProcessingPhaseStarted(EMassProcessingPhase::PostPhysics).Remove(PostPhaseHandle);
    }
    FWorldDelegates::OnWorldBeginTearDown.Remove(WorldEndingHandle);
    PrePhaseHandle.Reset(); PostPhaseHandle.Reset(); WorldEndingHandle.Reset();
    RetireOwnedSensor();
    FollowedPawn.Reset(); FollowedRig.Reset(); OriginalWorld.Reset(); OriginalMode.Reset();
    EntitySubsystem.Reset(); SimulationSubsystem.Reset(); OwnedManager.Reset(); BoundGeneration = 0;
    for (auto& Body : OriginalBodies) Body.Reset();
}

void UDDSOriginalCityMassObstacleBridge::WorldEnding(UWorld* World)
{ if (World == OriginalWorld.Get()) StopObservingBoundOriginalAssembly(); }
void UDDSOriginalCityMassObstacleBridge::EndPlay(const EEndPlayReason::Type Reason)
{ StopObservingBoundOriginalAssembly(); Super::EndPlay(Reason); }
void UDDSOriginalCityMassObstacleBridge::OnUnregister()
{ StopObservingBoundOriginalAssembly(); Super::OnUnregister(); }
