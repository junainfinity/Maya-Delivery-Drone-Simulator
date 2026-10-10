#include "DDSOriginalCityDriveReadinessProvider.h"
#include "DDSCityRollingPawn.h"
#include "Game/CitySamplePlayerController.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/GameModeBase.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PBDRigidsSolver.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
constexpr double PollBudgetS = 0.020;
constexpr double ActualStepS = 1.0 / 120.0;
constexpr double ActualGravityZ = -980.665;
constexpr int32 MaxNearbyResults = 256;
bool FinitePose(const FTransform& Pose)
{
    return !Pose.ContainsNaN() && Pose.GetRotation().IsNormalized()
        && !Pose.GetLocation().ContainsNaN() && !Pose.GetScale3D().ContainsNaN();
}
}

UDDSOriginalCityDriveReadinessProvider::UDDSOriginalCityDriveReadinessProvider()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

bool UDDSOriginalCityDriveReadinessProvider::CurrentContext(bool bRequireRegistration) const
{
    if (!IsInGameThread() || !IsValid(this) || !IsRegistered() || !bFollowing || bStopping) return false;
    auto* Pawn = FollowedPawn.Get(); auto* Rig = FollowedRig.Get(); auto* Chassis = FollowedChassis.Get();
    auto* World = OriginalWorld.Get(); auto* WP = Partition.Get(); auto* Subsystem = StreamingSubsystem.Get();
    auto* Controller = OriginalController.Get(); auto* Mode = OriginalMode.Get();
    if (!IsValid(Pawn) || !IsValid(Rig) || !IsValid(Chassis) || !IsValid(World) || !IsValid(WP)
        || !IsValid(Subsystem) || !IsValid(Controller) || !IsValid(Mode) || World->bIsTearingDown
        || World->WorldType != EWorldType::Game || World->GetOutermost()->GetName() != TEXT("/Game/Map/Small_City_LVL")
        || GetOwner() != Pawn || Pawn->IsActorBeingDestroyed() || GetWorld() != World || Pawn->GetWorld() != World
        || Pawn->GetBoundPhysicalRig() != Rig || Rig->GetOwner() != Pawn || Rig->GetWorld() != World
        || !Rig->IsRegistered() || Rig->GetActualChassis() != Chassis || Chassis->GetOwner() != Pawn
        || Chassis->GetWorld() != World || !Chassis->IsRegistered() || !Chassis->IsPhysicsStateCreated()
        || Rig->GetRigState() != EDDSRobotRigState::Active || World->GetWorldPartition() != WP
        || !WP->IsInitialized() || !WP->IsStreamingEnabled() || World->GetSubsystem<UWorldPartitionSubsystem>() != Subsystem
        || Pawn->GetController() != Controller || Controller->GetPawn() != Pawn || Controller->GetWorld() != World
        || World->GetAuthGameMode() != Mode
        || Mode->GetClass()->GetPathName() != TEXT("/Game/Gameplay/Framework/BP_CitySampleGameMode.BP_CitySampleGameMode_C")
        || !FinitePose(Chassis->GetComponentTransform()) || !Rig->CollisionReadinessMatches(const_cast<UDDSOriginalCityDriveReadinessProvider*>(this), Capability)) return false;
    return !bRequireRegistration || Subsystem->IsStreamingSourceProviderRegistered(const_cast<UDDSOriginalCityDriveReadinessProvider*>(this));
}

bool UDDSOriginalCityDriveReadinessProvider::GetStreamingSource(FWorldPartitionStreamingSource& Out) const
{
    if (!CurrentContext(true)) return false;
    const FTransform Pose = FollowedChassis->GetComponentTransform();
    Out = FWorldPartitionStreamingSource(SourceName, Pose.GetLocation(), Pose.Rotator(),
        EStreamingSourceTargetState::Activated, false, EStreamingSourcePriority::Normal, false);
    return true; // Actual chassis pose; default grid loading ranges, no physical transform write.
}

bool UDDSOriginalCityDriveReadinessProvider::StartFollowingBoundOriginalChassis(ADDSCityRollingPawn* Pawn, FString& Error)
{
    check(IsInGameThread());
    if (bFollowing || bStopping || bPolling || !IsValid(this) || !IsRegistered() || !IsValid(Pawn)
        || GetOwner() != Pawn || Pawn->IsActorBeingDestroyed()) { Error = TEXT("READINESS_OWNER_NOT_STARTABLE"); return false; }
    auto* Rig = Pawn->GetBoundPhysicalRig(); auto* Chassis = Rig ? Rig->GetActualChassis() : nullptr;
    auto* World = Pawn->GetWorld(); auto* Controller = Cast<ACitySamplePlayerController>(Pawn->GetController());
    auto* WP = World ? World->GetWorldPartition() : nullptr;
    auto* Subsystem = World ? World->GetSubsystem<UWorldPartitionSubsystem>() : nullptr;
    FDDSRobotSnapshot Physical;
    if (!IsValid(World) || World->WorldType != EWorldType::Game || World->bIsTearingDown
        || World->GetOutermost()->GetName() != TEXT("/Game/Map/Small_City_LVL") || GetWorld() != World
        || !IsValid(Rig) || !Rig->IsRegistered() || Rig->GetOwner() != Pawn || Rig->GetWorld() != World
        || !IsValid(Chassis) || !Chassis->IsRegistered() || Chassis->GetOwner() != Pawn || Chassis->GetWorld() != World
        || !IsValid(Controller) || Controller->GetPawn() != Pawn || !IsValid(WP) || !WP->IsInitialized()
        || !WP->IsStreamingEnabled() || !IsValid(Subsystem) || !World->GetAuthGameMode()
        || World->GetAuthGameMode()->GetClass()->GetPathName() != TEXT("/Game/Gameplay/Framework/BP_CitySampleGameMode.BP_CitySampleGameMode_C")
        || !Pawn->GetPreReadinessPhysicalState(Physical) || !Pawn->AcquireOwnedPhysicalReadiness(this, Capability))
    { Error = TEXT("ACTUAL_BOUND_ORIGINAL_CITY_CONTEXT_REQUIRED"); return false; }
    FollowedPawn = Pawn; FollowedRig = Rig; FollowedChassis = Chassis; OriginalWorld = World;
    Partition = WP; StreamingSubsystem = Subsystem; OriginalController = Controller; OriginalMode = World->GetAuthGameMode();
    SourceName = FName(*FString::Printf(TEXT("DDSOriginalChassis_%u_%llu"), GetUniqueID(), Capability.GetSerial()));
    bFollowing = true; LastPollRealS = 0; LastObservation = FDDSOriginalCityCollisionObservation();
    Subsystem->RegisterStreamingSourceProvider(this);
    if (!CurrentContext(true)) { StopFollowingBoundOriginalChassis(); Error = TEXT("STREAMING_REGISTRATION_CONTEXT_LOST"); return false; }
    AddTickPrerequisiteComponent(Rig);
    WorldEndingHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UDDSOriginalCityDriveReadinessProvider::WorldEnding);
    SetComponentTickEnabled(true); Error.Reset(); return true;
}

bool UDDSOriginalCityDriveReadinessProvider::CaptureSupport(UPrimitiveComponent* Primitive, FSupportReceipt& Out) const
{
    // Narrow source-provenance contract: ordinary supplied City road mesh components.
    // Unsupported subclasses/instances are unknown and cannot promote readiness.
    auto* C = Cast<UStaticMeshComponent>(Primitive); auto* World = OriginalWorld.Get();
    if (!IsValid(C) || C->GetClass() != UStaticMeshComponent::StaticClass() || !IsValid(World)
        || !C->IsRegistered() || C->GetWorld() != World || C->HasAnyFlags(RF_Transient)
        || !C->IsPhysicsStateCreated() || !C->IsQueryCollisionEnabled() || !C->IsPhysicsCollisionEnabled()
        || C->GetCollisionObjectType() != ECC_WorldStatic || !FinitePose(C->GetComponentTransform())) return false;
    auto* A = C->GetOwner(); auto* Mesh = C->GetStaticMesh().Get(); auto* Level = A ? A->GetLevel() : nullptr;
    auto* BI = C->UPrimitiveComponent::GetBodyInstance();
    auto* Handle = BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
    if (!IsValid(A) || A == FollowedPawn.Get() || A->IsActorBeingDestroyed() || A->HasAnyFlags(RF_Transient)
        || !A->HasAnyFlags(RF_WasLoaded)
        || A->GetWorld() != World || !IsValid(Level) || !World->GetLevels().Contains(Level)
        || !IsValid(Mesh) || Mesh->HasAnyFlags(RF_Transient)
        || !Mesh->GetOutermost()->GetName().StartsWith(TEXT("/Game/Road/"))
        || !Handle || !World->GetPhysicsScene() || Handle->GetSolverBase() != World->GetPhysicsScene()->GetSolver()) return false;
    Out.Component = C; Out.Actor = A; Out.Mesh = Mesh; Out.Level = Level;
    Out.ActorPackage = A->GetOutermost(); Out.MeshPackage = Mesh->GetOutermost(); Out.LevelPackage = Level->GetOutermost();
    Out.ExternalPackage = A->GetExternalPackage(); Out.bHadExternalPackage = A->GetExternalPackage() != nullptr;
    Out.Pose = C->GetComponentTransform(); Out.ActorHandle = Handle;
    return true;
}

bool UDDSOriginalCityDriveReadinessProvider::SupportUnchanged(const FSupportReceipt& R) const
{
    auto* C = R.Component.Get(); auto* A = R.Actor.Get(); auto* Mesh = R.Mesh.Get(); auto* Level = R.Level.Get();
    auto* World = OriginalWorld.Get();
    if (!IsValid(C) || !IsValid(A) || !IsValid(Mesh) || !IsValid(Level) || !IsValid(World)
        || C->GetClass() != UStaticMeshComponent::StaticClass() || !C->IsRegistered() || C->GetWorld() != World
        || C->GetOwner() != A || C->GetStaticMesh() != Mesh || A->GetWorld() != World || A->GetLevel() != Level
        || A->IsActorBeingDestroyed() || !A->HasAnyFlags(RF_WasLoaded) || C->HasAnyFlags(RF_Transient)
        || A->HasAnyFlags(RF_Transient) || Mesh->HasAnyFlags(RF_Transient)
        || !World->GetLevels().Contains(Level) || A->GetOutermost() != R.ActorPackage.Get()
        || Mesh->GetOutermost() != R.MeshPackage.Get() || Level->GetOutermost() != R.LevelPackage.Get()
        || (R.bHadExternalPackage && !R.ExternalPackage.IsValid())
        || A->GetExternalPackage() != R.ExternalPackage.Get() || !C->GetComponentTransform().Equals(R.Pose, 1.e-6)
        || !C->IsPhysicsStateCreated() || !C->USceneComponent::IsQueryCollisionEnabled()
        || !C->USceneComponent::IsPhysicsCollisionEnabled() || C->UPrimitiveComponent::GetCollisionObjectType() != ECC_WorldStatic) return false;
    auto* BI = C->UPrimitiveComponent::GetBodyInstance();
    auto* Handle = BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
    return Handle && Handle == R.ActorHandle && World->GetPhysicsScene()
        && Handle->GetSolverBase() == World->GetPhysicsScene()->GetSolver();
}

bool UDDSOriginalCityDriveReadinessProvider::FailObservation(const TCHAR* Reason, FString& Error)
{
    Error = Reason; LastObservation.Refusal = Reason; LastObservation.FinishedRealS = FPlatformTime::Seconds();
    LastObservation.bPublishedReady = false;
    // One loss advances the owned epoch; initial acquisition already did so.
    // Repeated unknown polls must allow a fresh false-state observation to arrive.
    if (Capability.IsReady())
    {
        auto* Pawn = FollowedPawn.Get();
        if (!IsValid(Pawn) || !Pawn->WithdrawOwnedPhysicalReadiness(this, Capability, false))
            StopFollowingBoundOriginalChassis();
    }
    if (bFollowing && !CurrentContext(true)) StopFollowingBoundOriginalChassis();
    return false;
}

bool UDDSOriginalCityDriveReadinessProvider::PollCollisionData(FString& Error)
{
    check(IsInGameThread());
    if (bPolling || !bFollowing) { Error = TEXT("READINESS_POLL_NOT_ACTIVE"); return false; }
    TGuardValue<bool> PollGuard(bPolling, true);
    LastObservation = FDDSOriginalCityCollisionObservation();
    const double Started = FPlatformTime::Seconds(); LastObservation.StartedRealS = Started;
    if (!FMath::IsFinite(Started) || !CurrentContext(true)) return FailObservation(TEXT("READINESS_CONTEXT_LOST"), Error);
    TStrongObjectPtr<ADDSCityRollingPawn> PawnPin(FollowedPawn.Get());
    TStrongObjectPtr<UWorld> WorldPin(OriginalWorld.Get());
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(FollowedRig.Get());
    TStrongObjectPtr<UStaticMeshComponent> ChassisPin(FollowedChassis.Get());
    auto* World = WorldPin.Get(); auto* Rig = RigPin.Get(); auto* Subsystem = StreamingSubsystem.Get();
    FDDSRobotSnapshot Physical;
    if (!PawnPin->GetPreReadinessPhysicalState(Physical)) return FailObservation(TEXT("FRESH_PRE_READINESS_COMPLETED_STATE_REQUIRED"), Error);
    auto* Scene = World->GetPhysicsScene(); auto* Solver = Scene ? Scene->GetSolver() : nullptr;
    const double SolverStep = Solver ? Solver->GetAsyncDeltaTime() : 0;
    const double GravityZ = World->GetGravityZ(); const FTransform ChassisPose = ChassisPin->GetComponentTransform();
    LastObservation.SolverStepS = SolverStep; LastObservation.WorldGravityCmps2 = GravityZ;
    LastObservation.CapturedPhysicsRealS = Physical.CapturedRealS; LastObservation.PhysicsStepSequence = Physical.StepSequence;
    LastObservation.RigGeneration = Physical.Generation; LastObservation.CollisionEpoch = Physical.CollisionWorldGeneration;
    LastObservation.ReadinessRevision = Physical.ReadinessRevision; LastObservation.SourceLocationCm = ChassisPose.GetLocation();
    if (!Solver || !FMath::IsFinite(SolverStep) || !FMath::IsFinite(GravityZ) || !FinitePose(ChassisPose)
        || FMath::Abs(SolverStep-ActualStepS) > 1.e-7 || FMath::Abs(GravityZ-ActualGravityZ) > 1.e-4
        || Physical.ActualGravityCmps2.ContainsNaN() || !Physical.ActualGravityCmps2.Equals(FVector(0,0,ActualGravityZ),1.e-4)
        || !FMath::IsFinite(Physical.ActualCallbackDtS) || !FMath::IsFinite(Physical.PreviousEvolutionDtS)
        || FMath::Abs(Physical.ActualCallbackDtS-ActualStepS) > 1.e-7
        || FMath::Abs(Physical.PreviousEvolutionDtS-ActualStepS) > 1.e-7)
        return FailObservation(TEXT("ACTUAL_120HZ_GRAVITY_READBACK_REQUIRED"), Error);
    const uint64 Generation = Capability.GetGeneration(), Epoch = Capability.GetWorldEpoch(), Revision = Capability.GetRevision();
    auto TransactionCurrent = [&]()
    {
        return CurrentContext(true) && Capability.GetGeneration() == Generation && Capability.GetWorldEpoch() == Epoch
            && Capability.GetRevision() == Revision && World->GetPhysicsScene() == Scene && Scene->GetSolver() == Solver
            && FMath::Abs(Solver->GetAsyncDeltaTime()-ActualStepS) <= 1.e-7
            && FMath::Abs(World->GetGravityZ()-ActualGravityZ) <= 1.e-4
            && ChassisPin->GetComponentTransform().Equals(ChassisPose, 1.e-6)
            && FPlatformTime::Seconds()-Started <= PollBudgetS;
    };
    FWorldPartitionStreamingSource Source;
    if (!GetStreamingSource(Source) || !TransactionCurrent()) return FailObservation(TEXT("CURRENT_CHASSIS_SOURCE_REQUIRED"), Error);
    // Filter delegates may execute here; revalidate the whole transaction afterwards.
    const auto EligibleProviders = Subsystem->GetStreamingSourceProviders();
    if (!TransactionCurrent() || !EligibleProviders.Contains(this)) return FailObservation(TEXT("STREAMING_PROVIDER_FILTERED_OR_REVOKED"), Error);
    // Engine policy must have processed this registered source at least once.
    // Its last tick's pose is reported separately; the completion query below uses the current pose.
    for (const auto& EngineSource : Partition->GetStreamingSources())
    {
        if (EngineSource.Name == SourceName && EngineSource.TargetState == EStreamingSourceTargetState::Activated
            && !EngineSource.Location.ContainsNaN())
        { LastObservation.bEngineSourceObserved = true; LastObservation.EngineSourceLocationCm = EngineSource.Location; break; }
    }
    if (!LastObservation.bEngineSourceObserved || !TransactionCurrent())
        return FailObservation(TEXT("ENGINE_PROCESSED_CHASSIS_STREAMING_SOURCE_REQUIRED"), Error);
    const bool Completed = Subsystem->IsStreamingCompleted(this);
    if (!TransactionCurrent() || !Completed) return FailObservation(TEXT("STREAMING_NOT_CURRENTLY_COMPLETED"), Error);
    TArray<FWorldPartitionStreamingSource> CurrentSources; CurrentSources.Add(Source);
    if (!Partition->IsStreamingCompleted(&CurrentSources) || !TransactionCurrent())
        return FailObservation(TEXT("ORIGINAL_PARTITION_CURRENT_SOURCE_NOT_COMPLETED"), Error);
    LastObservation.bStreamingCompleted = Completed;
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(DDSOriginalCityReadiness), false, PawnPin.Get());
    TArray<FSupportReceipt> Receipts; TArray<TStrongObjectPtr<UObject>> SupportPins;
    FVector FirstSupportPoint = FVector::ZeroVector;
    const FVector Offsets[5] = { FVector::ZeroVector, FVector(200,0,0), FVector(-200,0,0), FVector(0,200,0), FVector(0,-200,0) };
    for (const FVector& Offset : Offsets)
    {
        if (!TransactionCurrent()) return FailObservation(TEXT("SUPPORT_QUERY_CONTEXT_OR_BUDGET_LOST"), Error);
        const FVector Center = ChassisPose.GetLocation()+Offset;
        FHitResult Hit; ++LastObservation.SupportQueries;
        const bool bHit = World->LineTraceSingleByObjectType(Hit, Center+FVector(0,0,200), Center-FVector(0,0,3000), Objects, Params);
        if (!TransactionCurrent()) return FailObservation(TEXT("SUPPORT_QUERY_CONTEXT_CHANGED"), Error);
        for (const auto& R : Receipts) if (!SupportUnchanged(R)) return FailObservation(TEXT("EARLIER_SUPPORT_REVOKED"), Error);
        if (!bHit) continue;
        auto* QueryComponent = Hit.GetComponent();
        if (Hit.bBlockingHit) ++LastObservation.RawBlockingHits;
        LastObservation.QueryHitComponentPaths.Add(IsValid(QueryComponent) ? QueryComponent->GetPathName() : TEXT("<expired>"));
        LastObservation.QueryHitComponentClasses.Add(IsValid(QueryComponent) ? QueryComponent->GetClass()->GetPathName() : TEXT("<expired>"));
        FSupportReceipt Receipt;
        if (!Hit.bBlockingHit || Hit.ImpactPoint.ContainsNaN() || Hit.ImpactNormal.ContainsNaN()
            || Hit.ImpactNormal.Z < 0.25 || !CaptureSupport(Hit.GetComponent(), Receipt) || !TransactionCurrent())
            return FailObservation(TEXT("UNKNOWN_ORIGINAL_ROAD_COLLISION_PROVENANCE"), Error);
        if (!LastObservation.SupportHits) FirstSupportPoint = Hit.ImpactPoint;
        ++LastObservation.SupportHits;
        SupportPins.Emplace(Receipt.Component.Get()); SupportPins.Emplace(Receipt.Actor.Get()); SupportPins.Emplace(Receipt.Mesh.Get());
        LastObservation.SupportComponentPaths.Add(Receipt.Component->GetPathName());
        LastObservation.SupportMeshPackages.Add(Receipt.MeshPackage->GetName());
        LastObservation.SupportLevelPackages.Add(Receipt.LevelPackage->GetName());
        LastObservation.SupportActorPackages.Add(Receipt.ActorPackage->GetName());
        Receipts.Add(Receipt);
    }
    if (!LastObservation.SupportHits) return FailObservation(TEXT("NO_ORIGINAL_ROAD_COLLISION_DATA_OBSERVED"), Error);
    TArray<FOverlapResult> Nearby; ++LastObservation.NearbyQueries;
    // A broad data probe around observed road support, not a wheel contact test.
    World->OverlapMultiByObjectType(Nearby, FirstSupportPoint+FVector(0,0,20), FQuat::Identity,
        Objects, FCollisionShape::MakeSphere(300), Params);
    LastObservation.NearbyResults = Nearby.Num();
    if (!TransactionCurrent() || Nearby.Num() > MaxNearbyResults) return FailObservation(TEXT("NEARBY_QUERY_CONTEXT_OR_RESULT_BUDGET_LOST"), Error);
    bool bObservedSupportNearby = false;
    for (const auto& R : Receipts)
    {
        if (!SupportUnchanged(R)) return FailObservation(TEXT("SUPPORT_COLLISION_DATA_CHANGED"), Error);
        for (const auto& O : Nearby) if (O.GetComponent() == R.Component.Get()) bObservedSupportNearby = true;
    }
    if (!bObservedSupportNearby) return FailObservation(TEXT("NEARBY_REGISTERED_SUPPORT_COLLISION_REQUIRED"), Error);
    const bool StillCompleted = Subsystem->IsStreamingCompleted(this);
    if (!StillCompleted || !TransactionCurrent()) return FailObservation(TEXT("FINAL_STREAMING_OR_CONTEXT_CHANGED"), Error);
    if (!Partition->IsStreamingCompleted(&CurrentSources) || !TransactionCurrent())
        return FailObservation(TEXT("FINAL_ORIGINAL_PARTITION_STREAMING_CHANGED"), Error);
    for (const auto& R : Receipts) if (!SupportUnchanged(R)) return FailObservation(TEXT("FINAL_SUPPORT_RECEIPT_CHANGED"), Error);
    FDDSRobotSnapshot FinalPhysical;
    if (!PawnPin->GetPreReadinessPhysicalState(FinalPhysical) || FinalPhysical.StepSequence != Physical.StepSequence
        || FinalPhysical.Generation != Generation || FinalPhysical.CollisionWorldGeneration != Epoch
        || FinalPhysical.ReadinessRevision != Revision || !TransactionCurrent())
        return FailObservation(TEXT("FINAL_CURRENT_PHYSICAL_STATE_CHANGED"), Error);
    if (!PawnPin->CommitOwnedPhysicalReadiness(this, Capability, true)) return FailObservation(TEXT("OWNED_READINESS_COMPARE_AND_SET_REFUSED"), Error);
    if (!CurrentContext(true) || FPlatformTime::Seconds()-Started > PollBudgetS)
        return FailObservation(TEXT("PUBLICATION_CONTEXT_OR_BUDGET_LOST"), Error);
    LastObservation.PublishedReadinessRevision = Capability.GetRevision();
    LastObservation.FinishedRealS = FPlatformTime::Seconds(); LastObservation.bPublishedReady = true;
    Error.Reset(); return true;
}

void UDDSOriginalCityDriveReadinessProvider::StopFollowingBoundOriginalChassis()
{
    check(IsInGameThread());
    if (bStopping) return;
    TGuardValue<bool> StopGuard(bStopping, true);
    SetComponentTickEnabled(false);
    if (auto* Pawn = FollowedPawn.Get()) Pawn->WithdrawOwnedPhysicalReadiness(this, Capability, true);
    if (auto* Subsystem = StreamingSubsystem.Get())
        if (Subsystem->IsStreamingSourceProviderRegistered(this)) Subsystem->UnregisterStreamingSourceProvider(this);
    if (auto* Rig = FollowedRig.Get()) RemoveTickPrerequisiteComponent(Rig);
    if (WorldEndingHandle.IsValid()) FWorldDelegates::OnWorldBeginTearDown.Remove(WorldEndingHandle);
    WorldEndingHandle.Reset(); bFollowing = false; Capability = FDDSCollisionReadinessCapability();
    FollowedPawn.Reset(); FollowedRig.Reset(); FollowedChassis.Reset(); OriginalController.Reset(); OriginalMode.Reset();
    OriginalWorld.Reset(); Partition.Reset(); StreamingSubsystem.Reset(); LastObservation.bPublishedReady = false;
}

void UDDSOriginalCityDriveReadinessProvider::WorldEnding(UWorld* World)
{ if (World == OriginalWorld.Get()) StopFollowingBoundOriginalChassis(); }

void UDDSOriginalCityDriveReadinessProvider::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const double Now = FPlatformTime::Seconds();
    // Identity loss is checked every GT tick; bounded data polling is at most 10Hz.
    if (bFollowing && !CurrentContext(true)) { FString Error; FailObservation(TEXT("TICK_CONTEXT_LOST"), Error); return; }
    if (bFollowing && (!FMath::IsFinite(Now) || Now < LastPollRealS || Now-LastPollRealS >= 0.1))
    { LastPollRealS = Now; FString Error; PollCollisionData(Error); }
}

void UDDSOriginalCityDriveReadinessProvider::OnUnregister()
{ StopFollowingBoundOriginalChassis(); Super::OnUnregister(); }

void UDDSOriginalCityDriveReadinessProvider::EndPlay(const EEndPlayReason::Type Reason)
{ StopFollowingBoundOriginalChassis(); Super::EndPlay(Reason); }
