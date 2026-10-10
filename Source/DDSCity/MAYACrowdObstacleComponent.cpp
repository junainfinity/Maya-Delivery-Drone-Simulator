#include "MAYACrowdObstacleComponent.h"
#include "MAYANativeDemoPawn.h"
#include "MAYACargoWorker.h"
#include "MAYADeliveryParcel.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassEntityView.h"
#include "MassSimulationSubsystem.h"
#include "MassCommands.h"
#include "MassCommandBuffer.h"
#include "MassCommonFragments.h"
#include "MassMovementFragments.h"
#include "MassNavigationFragments.h"
#include "MassNavigationSubsystem.h"
#include "MassTrafficFragments.h"
#include "MassTrafficSubsystem.h"
#include "MassAgentComponent.h"
#include "MassAgentSubsystem.h"
#include "Translators/MassCapsuleComponentTranslators.h"
#include "Translators/MassSceneComponentLocationTranslator.h"
#include "Translators/MassCharacterMovementTranslators.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PBDRigidsSolver.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <cmath>

namespace MayaNativeCrowdObstacle
{
    bool Owned(FMassEntityManager& M, FMassEntityHandle E, TWeakObjectPtr<UMAYACrowdObstacleComponent> Owner)
    {
        const auto* Data = M.IsEntityActive(E) ? M.GetFragmentDataPtr<FMAYACrowdRobotObstacleOwner>(E) : nullptr;
        return Data && Data->Owner.HasSameIndexAndSerialNumber(Owner);
    }
    TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (double C : {V.X, V.Y, V.Z})
            if (FMath::IsFinite(C)) Values.Add(MakeShared<FJsonValueNumber>(C));
            else Values.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%g"), C)));
        return Values;
    }
}

UMAYACrowdObstacleComponent::UMAYACrowdObstacleComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
    PrimaryComponentTick.TickInterval = 0.1f; // startup retry only; phase callbacks publish while following
}

void UMAYACrowdObstacleComponent::BeginPlay()
{
    Super::BeginPlay();
    StartObserving();
}

void UMAYACrowdObstacleComponent::TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(Dt, TickType, TickFunction);
    if (!bFollowing && !bStopping && !bRemovalPending) StartObserving();
    if (bFollowing && !bStopping) MaintainContactPolicies();
}

bool UMAYACrowdObstacleComponent::StartObserving()
{
    AMAYANativeDemoPawn* Pawn = Cast<AMAYANativeDemoPawn>(GetOwner());
    UWorld* World = GetWorld();
    UMassEntitySubsystem* Entities = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
    UMassSimulationSubsystem* Sim = World ? World->GetSubsystem<UMassSimulationSubsystem>() : nullptr;
    if (!IsInGameThread() || bFollowing || bRemovalPending || !IsRegistered() || !IsValid(Pawn)
        || !World || World->bIsTearingDown || (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE)
        || !Entities || !Sim || !Sim->IsSimulationStarted() || !World->GetSubsystem<UMassNavigationSubsystem>()
        || Entities->GetMutableEntityManager().IsProcessing()) return false;
    Robot = Pawn; ObservedWorld = World; Simulation = Sim;
    Manager = Entities->GetMutableEntityManager().AsShared();
    bFollowing = true;
    SetComponentTickInterval(0.0f); // apply before dependent CharacterMovement ticks
    AgentSubsystem = World->GetSubsystem<UMassAgentSubsystem>();
    if (UMassAgentSubsystem* Agents = AgentSubsystem.Get())
        AgentDetachingHandle = Agents->GetOnMassAgentComponentEntityDetaching().AddUObject(this, &UMAYACrowdObstacleComponent::AgentDetaching);
    PrePhase = Sim->GetOnProcessingPhaseStarted(EMassProcessingPhase::PrePhysics).AddUObject(this, &UMAYACrowdObstacleComponent::PhaseStarting);
    PostPhase = Sim->GetOnProcessingPhaseStarted(EMassProcessingPhase::PostPhysics).AddUObject(this, &UMAYACrowdObstacleComponent::PhaseStarting);
    WorldEndingHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UMAYACrowdObstacleComponent::WorldEnding);
    return RefreshSensor();
}

bool UMAYACrowdObstacleComponent::ReadNativeFootprint(FTransform& Pose, FVector& Velocity, FBox& Bounds, float& Radius) const
{
    AMAYANativeDemoPawn* Pawn = Robot.Get();
    UWorld* World = ObservedWorld.Get();
    if (!IsValid(Pawn) || !World || Pawn->GetWorld() != World || !World->GetPhysicsScene()) return false;
    UPrimitiveComponent* Mount = Pawn->GetDeliveryCargoMount();
    UPrimitiveComponent* Chassis = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    if (!Chassis || !Chassis->GetBodyInstance() || !Chassis->GetBodyInstance()->IsValidBodyInstance()) return false;
    TArray<UPrimitiveComponent*> Parts;
    Pawn->GetComponents<UPrimitiveComponent>(Parts);
    TArray<FPhysicsActorHandle> Actors;
    int32 CollisionComponents = 0, WeldedComponents = 0;
    const FBodyInstance* ChassisBody = Chassis->GetBodyInstance();
    for (UPrimitiveComponent* Part : Parts)
        if (Part && Part->IsPhysicsCollisionEnabled())
        {
            // Default GetBodyInstance resolves a welded child to the actual compound
            // actor. Keep all physical component membership, count that actor once.
            const FBodyInstance* Body = Part->GetBodyInstance();
            const FBodyInstance* OwnBody = Part->GetBodyInstance(NAME_None, false);
            if (!Body || !OwnBody || !Body->IsValidBodyInstance()) return false;
            ++CollisionComponents;
            if (OwnBody != Body)
            {
                if (OwnBody->WeldParent != ChassisBody || Body != ChassisBody) return false;
                ++WeldedComponents;
            }
            Actors.AddUnique(Body->GetPhysicsActorHandle());
        }
    // Native PreparePhysics explicitly makes 15 components: 9 independent actors
    // (chassis, six wheels, two lids), with all six suspension shapes in chassis.
    if (CollisionComponents != 15 || WeldedComponents != 6 || Actors.Num() != 9) return false;
    const FPhysicsActorHandle ChassisActor = Chassis->GetBodyInstance()->GetPhysicsActorHandle();
    if (!ChassisActor || !Actors.Contains(ChassisActor)) return false;
    Bounds = FBox(ForceInit);
    bool bValid = true;
    // One actual scene read lock covers every body pose/geometry and velocity.
    // No visible-mesh AABB, readiness, input or joint-state gate is substituted.
    const bool bRead = FPhysicsCommand::ExecuteRead(World->GetPhysicsScene(), [&]()
    {
        for (const FPhysicsActorHandle& Actor : Actors)
        {
            if (!Actor || Actor->GetSolverBase() != World->GetPhysicsScene()->GetSolver()) { bValid = false; return; }
            const FBox B = FPhysicsInterface::GetBounds_AssumesLocked(Actor);
            if (!B.IsValid || B.Min.ContainsNaN() || B.Max.ContainsNaN() || B.GetSize().GetMin() <= 0)
            { bValid = false; return; }
            Bounds += B;
        }
        Pose = FPhysicsInterface::GetGlobalPose_AssumesLocked(ChassisActor);
        const FVector COM = FPhysicsInterface::GetComTransform_AssumesLocked(ChassisActor).GetLocation();
        Velocity = FPhysicsInterface::GetLinearVelocity_AssumesLocked(ChassisActor)
            + FVector::CrossProduct(FPhysicsInterface::GetAngularVelocity_AssumesLocked(ChassisActor), Pose.GetLocation() - COM);
    });
    if (!bRead || !bValid || Pose.ContainsNaN() || !Pose.GetRotation().IsNormalized() || Velocity.ContainsNaN()) return false;
    const FVector Low = Bounds.Min - Pose.GetLocation(), High = Bounds.Max - Pose.GetLocation();
    const double X = FMath::Max(FMath::Abs(Low.X), FMath::Abs(High.X));
    const double Y = FMath::Max(FMath::Abs(Low.Y), FMath::Abs(High.Y));
    const double R = FMath::Sqrt(X*X + Y*Y);
    if (!FMath::IsFinite(R) || R <= 0 || R >= MAX_flt) return false;
    Radius = float(R);
    if (double(Radius) < R) Radius = std::nextafter(Radius, MAX_flt);
    return true;
}

bool UMAYACrowdObstacleComponent::RefreshSensor()
{
    const auto M = Manager.Pin();
    UWorld* World = ObservedWorld.Get();
    if (bRefreshing || bStopping || bRemovalPending || !bFollowing || !M.IsValid() || !World || World->bIsTearingDown
        || M->GetWorld() != World || M->IsProcessing()) return false;
    TGuardValue<bool> RefreshGuard(bRefreshing, true);
    FMassArchetypeHandle Archetype;
    if (!Sensor.IsSet())
    {
        const UScriptStruct* Types[] = {FMAYACrowdRobotObstacleOwner::StaticStruct(), FTransformFragment::StaticStruct(),
            FMassVelocityFragment::StaticStruct(), FAgentRadiusFragment::StaticStruct(), FMassNavigationObstacleGridCellLocationFragment::StaticStruct(),
            FMassTrafficObstacleTag::StaticStruct()};
        Archetype = M->CreateArchetype(MakeArrayView(Types), TEXT("MAYANativeRobotCrowdObstacle"));
        // Archetype creation can broadcast external callbacks. Finish them before
        // reading native physics, then revalidate this exact owned context.
        if (!Archetype.IsValid() || !bFollowing || bStopping || !Robot.IsValid() || World->bIsTearingDown
            || Manager.Pin() != M || Robot->GetWorld() != World) return false;
    }
    FTransform Pose; FVector Velocity; FBox Bounds; float Radius = 0;
    if (!ReadNativeFootprint(Pose, Velocity, Bounds, Radius))
    {
        Refusal = TEXT("Actual robot collision footprint unavailable"); RemoveSensor();
        LastEvidenceJson = TEXT("{\"schema\":\"MayaRobotics.NativeCrowdObstacle.v1\",\"published\":false,\"refusal\":\"Actual robot collision footprint unavailable\"}");
        return false;
    }
    if (!Sensor.IsSet())
    {
        Sensor = M->CreateEntity(Archetype);
        M->GetFragmentDataChecked<FMAYACrowdRobotObstacleOwner>(Sensor).Owner = this;
    }
    if (!MayaNativeCrowdObstacle::Owned(*M, Sensor, this)) { Refusal = TEXT("Owned Mass sensor identity unavailable"); return false; }
    M->GetFragmentDataChecked<FTransformFragment>(Sensor).SetTransform(Pose);
    M->GetFragmentDataChecked<FMassVelocityFragment>(Sensor).Value = Velocity;
    M->GetFragmentDataChecked<FAgentRadiusFragment>(Sensor).Radius = Radius;
    // No MoveTarget: original avoidance treats this robot as nonreciprocal.
    const bool bInGrid = FMassEntityView(*M, Sensor).HasTag<FMassInNavigationObstacleGridTag>();
    ++PublishGeneration; Refusal.Empty();
    RefreshServiceObstacles();
    UpdateEvidence(Pose, Velocity, Bounds, Radius, bInGrid);
    return true;
}


void UMAYACrowdObstacleComponent::DestroyOwnedSensor(FMassEntityHandle Entity)
{
    const auto M = Manager.Pin();
    if (!Entity.IsSet() || !M.IsValid()) return;
    const TWeakObjectPtr<UMAYACrowdObstacleComponent> Owner(this);
    if (!M->IsProcessing())
    { if (MayaNativeCrowdObstacle::Owned(*M, Entity, Owner)) M->DestroyEntity(Entity); return; }
    const auto ExpectedManager = Manager;
    M->Defer().PushCommand<FMassDeferredDestroyCommand>([Owner, ExpectedManager, Entity](FMassEntityManager& System)
    {
        if (ExpectedManager.Pin().Get() == &System && MayaNativeCrowdObstacle::Owned(System, Entity, Owner))
            System.DestroyEntity(Entity);
    });
}

void UMAYACrowdObstacleComponent::RemoveServiceObstacles()
{
    TArray<FServiceObstacle> Previous = MoveTemp(ServiceObstacles);
    LastServiceObstaclesJson = TEXT("[]");
    for (const FServiceObstacle& Entry : Previous) DestroyOwnedSensor(Entry.Entity);
}

void UMAYACrowdObstacleComponent::RefreshServiceObstacles()
{
    UWorld* World = ObservedWorld.Get(); const auto M = Manager.Pin();
    if (!World || World->bIsTearingDown || !World->GetPhysicsScene() || !M.IsValid()
        || M->IsProcessing() || M->GetWorld() != World || bStopping || !bFollowing) return;
    const auto ContextValid = [&]()
    { return IsValid(World) && !World->bIsTearingDown && bFollowing && !bStopping
        && Manager.Pin() == M && M->GetWorld() == World && !M->IsProcessing(); };
    TArray<UPrimitiveComponent*> ActualBodies;
    for (TActorIterator<AMAYACargoWorker> It(World); It; ++It)
        if (!It->IsActorBeingDestroyed())
            if (auto* Capsule = Cast<UCapsuleComponent>(It->GetRootComponent())) ActualBodies.AddUnique(Capsule);
    for (TActorIterator<AMAYADeliveryParcel> It(World); It; ++It)
        if (!It->IsActorBeingDestroyed())
            if (UPrimitiveComponent* Body = It->GetParcelBody()) ActualBodies.AddUnique(Body);
    for (int32 I = ServiceObstacles.Num() - 1; I >= 0; --I)
        if (!ActualBodies.Contains(ServiceObstacles[I].Body.Get()))
        {
            const FMassEntityHandle Previous = ServiceObstacles[I].Entity;
            ServiceObstacles.RemoveAt(I); // destruction can invoke external teardown callbacks
            DestroyOwnedSensor(Previous);
            if (!ContextValid()) return;
        }
    TArray<TSharedPtr<FJsonValue>> Evidence;
    for (UPrimitiveComponent* Body : ActualBodies)
    {
        FTransform Pose; FVector Velocity; FBox Bounds(ForceInit); bool bKnown = false;
        FBodyInstance* Instance = IsValid(Body) && Body->IsPhysicsCollisionEnabled() ? Body->GetBodyInstance() : nullptr;
        if (Instance && Instance->IsValidBodyInstance())
        {
            const FPhysicsActorHandle Actor = Instance->GetPhysicsActorHandle();
            FPhysicsCommand::ExecuteRead(World->GetPhysicsScene(), [&]()
            {
                if (!Actor || Actor->GetSolverBase() != World->GetPhysicsScene()->GetSolver()) return;
                Pose = FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor);
                Bounds = FPhysicsInterface::GetBounds_AssumesLocked(Actor);
                const FVector COM = FPhysicsInterface::GetComTransform_AssumesLocked(Actor).GetLocation();
                Velocity = FPhysicsInterface::GetLinearVelocity_AssumesLocked(Actor)
                    + FVector::CrossProduct(FPhysicsInterface::GetAngularVelocity_AssumesLocked(Actor), Pose.GetLocation() - COM);
                bKnown = !Pose.ContainsNaN() && Pose.GetRotation().IsNormalized() && !Velocity.ContainsNaN()
                    && Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN() && Bounds.GetSize().GetMin() > 0;
            });
        }
        FServiceObstacle* Entry = ServiceObstacles.FindByPredicate([&](const FServiceObstacle& E) { return E.Body == Body; });
        if (!bKnown)
        {
            if (Entry)
            {
                const FMassEntityHandle Previous = Entry->Entity;
                Entry->Entity.Reset(); // never retain an array pointer across destruction observers
                DestroyOwnedSensor(Previous);
                if (!ContextValid()) return;
            }
            continue;
        }
        const FVector Low = Bounds.Min - Pose.GetLocation(), High = Bounds.Max - Pose.GetLocation();
        const double X = FMath::Max(FMath::Abs(Low.X), FMath::Abs(High.X));
        const double Y = FMath::Max(FMath::Abs(Low.Y), FMath::Abs(High.Y));
        const double R = FMath::Sqrt(X * X + Y * Y);
        if (!FMath::IsFinite(R) || R <= 0 || R >= MAX_flt)
        {
            if (Entry)
            {
                const FMassEntityHandle Previous = Entry->Entity;
                Entry->Entity.Reset(); // never retain an array pointer across destruction observers
                DestroyOwnedSensor(Previous);
                if (!ContextValid()) return;
            }
            continue;
        }
        float Radius = float(R); if (double(Radius) < R) Radius = std::nextafter(Radius, MAX_flt);
        if (!Entry) { FServiceObstacle New; New.Body = Body; ServiceObstacles.Add(New); Entry = &ServiceObstacles.Last(); }
        if (!Entry->Entity.IsSet())
        {
            const UScriptStruct* Types[] = {FMAYACrowdRobotObstacleOwner::StaticStruct(), FTransformFragment::StaticStruct(),
                FMassVelocityFragment::StaticStruct(), FAgentRadiusFragment::StaticStruct(), FMassNavigationObstacleGridCellLocationFragment::StaticStruct()};
            const FMassArchetypeHandle A = M->CreateArchetype(MakeArrayView(Types), TEXT("MAYANativeServiceCrowdObstacle"));
            // Creation may invoke external observers. Do not retain an array pointer across it.
            if (!A.IsValid() || bStopping || !bFollowing || World->bIsTearingDown || Manager.Pin() != M || !IsValid(Body)) return;
            Entry = ServiceObstacles.FindByPredicate([&](const FServiceObstacle& E) { return E.Body == Body; });
            if (!Entry) return;
            const FMassEntityHandle Entity = M->CreateEntity(A);
            if (!M->IsEntityActive(Entity)) return;
            M->GetFragmentDataChecked<FMAYACrowdRobotObstacleOwner>(Entity).Owner = this;
            Entry = ServiceObstacles.FindByPredicate([&](const FServiceObstacle& E) { return E.Body == Body; });
            if (bStopping || !bFollowing || !Entry || World->bIsTearingDown || Manager.Pin() != M)
            { if (MayaNativeCrowdObstacle::Owned(*M, Entity, this)) M->DestroyEntity(Entity); return; }
            Entry->Entity = Entity;
        }
        if (!MayaNativeCrowdObstacle::Owned(*M, Entry->Entity, this)) continue;
        M->GetFragmentDataChecked<FTransformFragment>(Entry->Entity).SetTransform(Pose);
        M->GetFragmentDataChecked<FMassVelocityFragment>(Entry->Entity).Value = Velocity;
        M->GetFragmentDataChecked<FAgentRadiusFragment>(Entry->Entity).Radius = Radius;
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("component"), Body->GetPathName());
        Item->SetStringField(TEXT("entity"), Entry->Entity.DebugGetDescription());
        Item->SetArrayField(TEXT("actualNativeWorldCm"), MayaNativeCrowdObstacle::Vec(Pose.GetLocation()));
        Item->SetArrayField(TEXT("actualNativeVelocityCmps"), MayaNativeCrowdObstacle::Vec(Velocity));
        Item->SetNumberField(TEXT("actualNativeEnclosingRadiusCm"), Radius);
        Item->SetBoolField(TEXT("inOriginalAvoidanceGrid"), FMassEntityView(*M, Entry->Entity).HasTag<FMassInNavigationObstacleGridTag>());
        Evidence.Add(MakeShared<FJsonValueObject>(Item));
    }
    FString Json; const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(Evidence, Writer); LastServiceObstaclesJson = Json;
}

void UMAYACrowdObstacleComponent::UpdateEvidence(const FTransform& Pose, const FVector& Velocity, const FBox& Bounds, float Radius, bool bInGrid)
{
    UWorld* World = ObservedWorld.Get(); const auto M = Manager.Pin();
    if (!World || !M.IsValid()) return;
    const double Now = World->GetTimeSeconds();
    LastNativeChassisPosition = Pose.GetLocation(); bHaveNativeChassisPosition = true;
    if (Now - LastCrowdSampleWorldSeconds >= 0.1 || LastCrowdSampleWorldSeconds < 0)
    {
        LastCrowdSampleWorldSeconds = Now;
        TArray<TSharedPtr<FJsonValue>> Nearby;
        for (TActorIterator<ACharacter> It(World); It; ++It)
        {
            ACharacter* Person = *It;
            UMassAgentComponent* Agent = Person->FindComponentByClass<UMassAgentComponent>();
            UCapsuleComponent* Capsule = Person->GetCapsuleComponent();
            if (!Agent || !Capsule || Capsule->GetComponentTransform().ContainsNaN()
                || !FMath::IsFinite(Capsule->GetScaledCapsuleRadius()) || !FMath::IsFinite(Capsule->GetScaledCapsuleHalfHeight())
                || Person->IsHidden() || Person->IsActorBeingDestroyed()
                || FVector::Dist2D(Capsule->GetComponentLocation(), Pose.GetLocation()) > 600.0) continue;
            const FMassEntityHandle Entity = Agent->GetEntityHandle();
            if (!M->IsEntityActive(Entity)) continue;
            ConsiderContactPolicy(Person, Agent, Capsule, Entity);
            if (Nearby.Num() >= 16) continue; // policy covers every nearby eligible actor; only evidence rows are capped
            const auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("actor"), Person->GetPathName());
            Item->SetStringField(TEXT("actorClass"), Person->GetClass()->GetPathName());
            Item->SetStringField(TEXT("entity"), Entity.DebugGetDescription());
            Item->SetArrayField(TEXT("capsuleWorldCm"), MayaNativeCrowdObstacle::Vec(Capsule->GetComponentLocation()));
            Item->SetNumberField(TEXT("capsuleRadiusCm"), Capsule->GetScaledCapsuleRadius());
            Item->SetNumberField(TEXT("capsuleHalfHeightCm"), Capsule->GetScaledCapsuleHalfHeight());
            Item->SetNumberField(TEXT("collisionEnabled"), uint8(Capsule->GetCollisionEnabled()));
            Item->SetNumberField(TEXT("capsuleObjectType"), uint8(Capsule->GetCollisionObjectType()));
            if (const FScopedContactPolicy* Policy = ContactPolicies.FindByPredicate([&](const FScopedContactPolicy& P) { return P.Capsule == Capsule && P.Entity == Entity; }))
            {
                Item->SetBoolField(TEXT("scopedContactPolicyExternalEditObserved"), Policy->bExternalEdit);
                Item->SetNumberField(TEXT("originalResponseToRobotPhysicsBody"), uint8(Policy->OriginalResponse));
                Item->SetBoolField(TEXT("originalNavWalkingSweep"), Policy->bOriginalNavSweep);
                Item->SetStringField(TEXT("contactQueryBeforePolicy"), Policy->BeforeSweepJson);
                Item->SetStringField(TEXT("contactQueryAfterPolicy"), Policy->AppliedSweepJson);
            }
            Item->SetStringField(TEXT("collisionProfile"), Capsule->GetCollisionProfileName().ToString());
            Item->SetNumberField(TEXT("responseToRobotPhysicsBody"), uint8(Capsule->GetCollisionResponseToChannel(ECC_PhysicsBody)));
            Item->SetBoolField(TEXT("capsuleMassCopyToActor"), FMassEntityView(*M, Entity).HasTag<FMassCapsuleTransformCopyToActorTag>());
            Item->SetBoolField(TEXT("feetSceneLocationMassCopyToActor"), FMassEntityView(*M, Entity).HasTag<FMassSceneComponentLocationCopyToActorTag>());
            Item->SetBoolField(TEXT("feetSceneLocationMassCopyToMass"), FMassEntityView(*M, Entity).HasTag<FMassSceneComponentLocationCopyToMassTag>());
            Item->SetBoolField(TEXT("characterMovementMassCopyToActor"), FMassEntityView(*M, Entity).HasTag<FMassCharacterMovementCopyToActorTag>());
            Item->SetBoolField(TEXT("characterMovementMassCopyToMass"), FMassEntityView(*M, Entity).HasTag<FMassCharacterMovementCopyToMassTag>());
            Item->SetNumberField(TEXT("enclosingCircleSeparationCm"), FVector::Dist2D(Capsule->GetComponentLocation(), Pose.GetLocation()) - Radius - Capsule->GetScaledCapsuleRadius());
            int32 ActualRobotOverlaps = 0;
            TArray<UPrimitiveComponent*> RobotParts; Robot->GetComponents<UPrimitiveComponent>(RobotParts);
            TArray<FPhysicsActorHandle> CheckedActors;
            for (UPrimitiveComponent* Part : RobotParts)
                if (Part && Part->IsPhysicsCollisionEnabled() && Part->GetBodyInstance() && Part->GetBodyInstance()->IsValidBodyInstance())
                {
                    const FPhysicsActorHandle Actor = Part->GetBodyInstance()->GetPhysicsActorHandle();
                    if (CheckedActors.Contains(Actor)) continue;
                    CheckedActors.Add(Actor);
                    ActualRobotOverlaps += Part->GetBodyInstance()->OverlapTest(Capsule->GetComponentLocation(),
                        Capsule->GetComponentQuat(), Capsule->GetCollisionShape(), nullptr, false) ? 1 : 0;
                }
            Item->SetNumberField(TEXT("actualNativeIndependentBodyOwnShapeOverlapCount"), ActualRobotOverlaps);
            // Original world object query includes every actual welded shape. Ignore
            // only this capsule actor, then filter results to this exact robot.
            TArray<FOverlapResult> WorldOverlaps;
            FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
            FCollisionQueryParams OverlapParams(SCENE_QUERY_STAT(MAYACrowdCurrentCapsuleOverlap), false, Person);
            World->OverlapMultiByObjectType(WorldOverlaps, Capsule->GetComponentLocation(), Capsule->GetComponentQuat(),
                Objects, Capsule->GetCollisionShape(), OverlapParams);
            TArray<TSharedPtr<FJsonValue>> RobotOverlapComponents;
            TArray<FString> UniquePaths;
            for (const FOverlapResult& Overlap : WorldOverlaps)
                if (Overlap.GetActor() == Robot.Get())
                {
                    const FString Path = GetPathNameSafe(Overlap.GetComponent());
                    if (!UniquePaths.Contains(Path))
                    { UniquePaths.Add(Path); RobotOverlapComponents.Add(MakeShared<FJsonValueString>(Path)); }
                }
            Item->SetBoolField(TEXT("currentFullCompoundRobotOverlapObserved"), !UniquePaths.IsEmpty());
            Item->SetArrayField(TEXT("currentFullCompoundRobotOverlapComponents"), RobotOverlapComponents);
            Item->SetStringField(TEXT("fullCompoundOverlapScope"), TEXT("Read-only current actual capsule world overlap against PhysicsBody, filtered to the exact robot actor; includes welded shapes, no actor movement or clearance prediction."));
            Item->SetStringField(TEXT("overlapObservationCoverage"), TEXT("Read-locked original independent-body own simple shapes; welded suspension-owned shapes are excluded by the engine body filter. Not a complete compound clearance assertion."));
            if (UCharacterMovementComponent* Move = Person->GetCharacterMovement())
            {
                Item->SetBoolField(TEXT("movementActive"), Move->IsActive());
                Item->SetNumberField(TEXT("actualMovementMode"), uint8(Move->MovementMode));
                Item->SetBoolField(TEXT("actualPhysicsInteractionEnabled"), Move->bEnablePhysicsInteraction);
                if (FMath::IsFinite(Move->InitialPushForceFactor)) Item->SetNumberField(TEXT("actualInitialPushForceFactor"), Move->InitialPushForceFactor);
                if (FMath::IsFinite(Move->PushForceFactor)) Item->SetNumberField(TEXT("actualPushForceFactor"), Move->PushForceFactor);
                Item->SetBoolField(TEXT("actualNavWalkingSweep"), Move->bSweepWhileNavWalking);
                Item->SetBoolField(TEXT("capsuleGeneratesOverlapEvents"), Capsule->GetGenerateOverlapEvents());
                Item->SetBoolField(TEXT("movementTickEnabled"), Move->IsComponentTickEnabled());
                Item->SetArrayField(TEXT("actualCharacterVelocityCmps"), MayaNativeCrowdObstacle::Vec(Move->Velocity));
                Item->SetArrayField(TEXT("lastRequestedCharacterVelocityCmps"), MayaNativeCrowdObstacle::Vec(Move->GetLastUpdateRequestedVelocity()));
            }
            Nearby.Add(MakeShared<FJsonValueObject>(Item));

        }
        const auto ArrayWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&LastNearbyCrowdJson);
        FJsonSerializer::Serialize(Nearby, ArrayWriter);
    }
    const auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeCrowdObstacle.v1"));
    J->SetStringField(TEXT("scope"), TEXT("Original crowd navigation and traffic obstacle queries receive the same actual 15-component/9-actor scene-locked footprint/velocity; tag publication is not proof of vehicle braking or collision-free travel"));
    J->SetBoolField(TEXT("published"), true); J->SetBoolField(TEXT("inOriginalNavigationGrid"), bInGrid);
    J->SetBoolField(TEXT("trafficObstacleTagPresent"), M->IsEntityActive(Sensor) && FMassEntityView(*M, Sensor).HasTag<FMassTrafficObstacleTag>());
    J->SetBoolField(TEXT("trafficSubsystemAvailable"), World->GetSubsystem<UMassTrafficSubsystem>() != nullptr);
    J->SetStringField(TEXT("trafficAvoidanceScope"), TEXT("Only the owned actual robot sensor is tagged for the original Traffic FindObstacles query; existing service sensors remain crowd-only; no traffic actors, lane settings, collision, forces or input changed"));
    J->SetStringField(TEXT("generation"), LexToString(PublishGeneration)); J->SetStringField(TEXT("ownedEntity"), Sensor.DebugGetDescription());
    J->SetBoolField(TEXT("nonreciprocal"), true); J->SetNumberField(TEXT("nativeCollisionComponentCount"), 15);
    J->SetNumberField(TEXT("nativeIndependentCollisionActorCount"), 9); J->SetNumberField(TEXT("nativeWeldedSuspensionComponentCount"), 6);
    J->SetNumberField(TEXT("worldSeconds"), Now); J->SetNumberField(TEXT("nearbyContactSampleWorldSeconds"), LastCrowdSampleWorldSeconds);
    J->SetArrayField(TEXT("chassisWorldCm"), MayaNativeCrowdObstacle::Vec(Pose.GetLocation()));
    J->SetArrayField(TEXT("originVelocityCmps"), MayaNativeCrowdObstacle::Vec(Velocity));
    J->SetArrayField(TEXT("nativeBoundsMinWorldCm"), MayaNativeCrowdObstacle::Vec(Bounds.Min));
    J->SetArrayField(TEXT("nativeBoundsMaxWorldCm"), MayaNativeCrowdObstacle::Vec(Bounds.Max));
    J->SetNumberField(TEXT("enclosingRadiusCm"), Radius); J->SetStringField(TEXT("refusal"), Refusal);
    J->SetStringField(TEXT("contactPolicyScope"), TEXT("Nearby active original Mass CharacterMovement Custom-profile capsules only: block PhysicsBody and enable NavWalking sweeps. Query receipts do not establish a moving encounter PASS."));
    J->SetNumberField(TEXT("contactPolicyTrackedCount"), ContactPolicies.Num());
    J->SetStringField(TEXT("contactPolicyApplyCount"), LexToString(ContactApplyCount));
    J->SetStringField(TEXT("contactPolicyRestoreCount"), LexToString(ContactRestoreCount));
    TArray<TSharedPtr<FJsonValue>> PolicyEvents;
    for (const FString& Event : ContactPolicyEvents) PolicyEvents.Add(MakeShared<FJsonValueString>(Event));
    J->SetArrayField(TEXT("contactPolicyEvents"), PolicyEvents);
    J->SetStringField(TEXT("contactHitCount"), LexToString(ContactHitCount));
    J->SetStringField(TEXT("firstActualCapsuleRobotHit"), FirstContactHitJson);
    TArray<TSharedPtr<FJsonValue>> Hits;
    for (const FString& Event : ContactHitEvents) Hits.Add(MakeShared<FJsonValueString>(Event));
    J->SetArrayField(TEXT("lastActualCapsuleRobotHits"), Hits);
    FString Json;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(J, Writer);
    Json.LeftChopInline(1); // append the already-bounded captured array without resampling it
    LastEvidenceJson = Json + TEXT(",\"nearbyActualMassCharacters\":") + LastNearbyCrowdJson
        + TEXT(",\"actualNativeServiceObstacles\":") + LastServiceObstaclesJson + TEXT("}");
}


FString UMAYACrowdObstacleComponent::CaptureContactSweep(UCapsuleComponent* Capsule) const
{
    const auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("scope"), TEXT("Read-only actual capsule sweep with its current channel responses; actor is not moved and no moving encounter PASS is asserted"));
    UWorld* World = ObservedWorld.Get();
    if (!World || !Capsule || !Robot.IsValid() || !bHaveNativeChassisPosition)
    { J->SetBoolField(TEXT("queryKnown"), false); }
    else
    {
        const FVector Start = Capsule->GetComponentLocation();
        const FVector End(LastNativeChassisPosition.X, LastNativeChassisPosition.Y, Start.Z);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MAYACrowdContactReadback), false, Capsule->GetOwner());
        FCollisionResponseParams Responses(Capsule->GetCollisionResponseToChannels());
        FHitResult Hit;
        const bool bHit = World->SweepSingleByChannel(Hit, Start, End, Capsule->GetComponentQuat(),
            Capsule->GetCollisionObjectType(), Capsule->GetCollisionShape(), Params, Responses);
        J->SetBoolField(TEXT("queryKnown"), true); J->SetNumberField(TEXT("worldSeconds"), World->GetTimeSeconds());
        J->SetArrayField(TEXT("startCapsuleWorldCm"), MayaNativeCrowdObstacle::Vec(Start));
        J->SetArrayField(TEXT("endCapsuleWorldCm"), MayaNativeCrowdObstacle::Vec(End));
        J->SetNumberField(TEXT("capsuleObjectType"), uint8(Capsule->GetCollisionObjectType()));
        J->SetNumberField(TEXT("responseToPhysicsBody"), uint8(Capsule->GetCollisionResponseToChannel(ECC_PhysicsBody)));
        J->SetBoolField(TEXT("blockingHit"), bHit && Hit.bBlockingHit);
        J->SetBoolField(TEXT("robotBlockingHit"), bHit && Hit.bBlockingHit && Hit.GetActor() == Robot.Get());
        J->SetBoolField(TEXT("startPenetrating"), Hit.bStartPenetrating);
        J->SetNumberField(TEXT("hitTime"), Hit.Time); J->SetNumberField(TEXT("penetrationDepthCm"), Hit.PenetrationDepth);
        J->SetStringField(TEXT("hitActor"), GetPathNameSafe(Hit.GetActor()));
        J->SetStringField(TEXT("hitComponent"), GetPathNameSafe(Hit.GetComponent()));
        J->SetArrayField(TEXT("impactWorldCm"), MayaNativeCrowdObstacle::Vec(Hit.ImpactPoint));
        J->SetArrayField(TEXT("impactNormal"), MayaNativeCrowdObstacle::Vec(Hit.ImpactNormal));
    }
    FString Json; const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(J, Writer); return Json;
}

void UMAYACrowdObstacleComponent::RecordContactPolicyEvent(const FScopedContactPolicy& Policy, const TCHAR* Action)
{
    const auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("action"), Action); J->SetStringField(TEXT("entity"), Policy.Entity.DebugGetDescription());
    J->SetStringField(TEXT("actor"), GetPathNameSafe(Policy.Actor.Get()));
    J->SetStringField(TEXT("capsule"), GetPathNameSafe(Policy.Capsule.Get()));
    J->SetNumberField(TEXT("worldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1.0);
    J->SetStringField(TEXT("profile"), Policy.Profile.ToString());
    J->SetNumberField(TEXT("originalPhysicsBodyResponse"), uint8(Policy.OriginalResponse));
    J->SetBoolField(TEXT("originalNavWalkingSweep"), Policy.bOriginalNavSweep);
    J->SetBoolField(TEXT("changedPhysicsBodyResponse"), Policy.bChangedResponse);
    J->SetBoolField(TEXT("changedNavWalkingSweep"), Policy.bChangedNavSweep);
    J->SetBoolField(TEXT("externalEditObserved"), Policy.bExternalEdit);
    FString Json; const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(J, Writer); ContactPolicyEvents.Add(Json);
    if (ContactPolicyEvents.Num() > 64) ContactPolicyEvents.RemoveAt(0, ContactPolicyEvents.Num() - 64);
}

void UMAYACrowdObstacleComponent::ConsiderContactPolicy(ACharacter* Actor, UMassAgentComponent* Agent, UCapsuleComponent* Capsule, FMassEntityHandle Entity)
{
    const auto M = Manager.Pin();
    UCharacterMovementComponent* Move = Actor ? Actor->GetCharacterMovement() : nullptr;
    if (!IsInGameThread() || !M.IsValid() || M->IsProcessing() || !Actor || !Agent || !Capsule || !Move
        || !M->IsEntityActive(Entity) || Agent->GetEntityHandle() != Entity || Move->UpdatedComponent != Capsule
        || !Actor->GetActorEnableCollision() || !Move->IsActive() || !Move->IsComponentTickEnabled()
        || Capsule->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
        || Capsule->GetCollisionProfileName() != FName(TEXT("Custom"))
        || !FMassEntityView(*M, Entity).HasTag<FMassCharacterMovementCopyToActorTag>()
        || ContactPolicies.ContainsByPredicate([&](const FScopedContactPolicy& P) { return P.Capsule == Capsule && P.Entity == Entity; })) return;
    TArray<UPrimitiveComponent*> Parts; Robot->GetComponents<UPrimitiveComponent>(Parts);
    int32 BlockingParts = 0;
    for (UPrimitiveComponent* Part : Parts) if (Part && Part->IsPhysicsCollisionEnabled())
    {
        if (Part->GetCollisionObjectType() != ECC_PhysicsBody
            || Part->GetCollisionResponseToChannel(Capsule->GetCollisionObjectType()) != ECR_Block) return;
        ++BlockingParts;
    }
    if (BlockingParts != 15) return;
    FScopedContactPolicy Policy;
    Policy.Actor = Actor; Policy.Agent = Agent; Policy.Capsule = Capsule; Policy.Movement = Move; Policy.Entity = Entity;
    Policy.Profile = Capsule->GetCollisionProfileName(); Policy.CollisionEnabled = Capsule->GetCollisionEnabled();
    Policy.OriginalResponse = Capsule->GetCollisionResponseToChannel(ECC_PhysicsBody);
    Policy.bOriginalNavSweep = Move->bSweepWhileNavWalking;
    Policy.bChangedResponse = Policy.OriginalResponse != ECR_Block;
    Policy.bChangedNavSweep = !Policy.bOriginalNavSweep;
    Policy.BeforeSweepJson = CaptureContactSweep(Capsule);
    ContactPolicies.Add(Policy); // detaching callback can now restore before the old entity handle is cleared
    Move->AddTickPrerequisiteComponent(this);
    Capsule->OnComponentHit.AddUniqueDynamic(this, &UMAYACrowdObstacleComponent::OnCrowdCapsuleHit);
    if (Policy.bChangedNavSweep) Move->bSweepWhileNavWalking = true;
    if (Policy.bChangedResponse) Capsule->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
    if (FScopedContactPolicy* Installed = ContactPolicies.FindByPredicate([&](const FScopedContactPolicy& P) { return P.Capsule == Capsule && P.Entity == Entity; }))
    {
        Installed->AppliedSweepJson = CaptureContactSweep(Capsule);
        ++ContactApplyCount; RecordContactPolicyEvent(*Installed, TEXT("applied"));
    }
}

void UMAYACrowdObstacleComponent::RestoreContactPolicy(FScopedContactPolicy& Policy, const TCHAR* Reason)
{
    ACharacter* Actor = Policy.Actor.Get(); UMassAgentComponent* Agent = Policy.Agent.Get();
    UCapsuleComponent* Capsule = Policy.Capsule.Get(); UCharacterMovementComponent* Move = Policy.Movement.Get();
    if (Move) Move->RemoveTickPrerequisiteComponent(this);
    if (Capsule) Capsule->OnComponentHit.RemoveDynamic(this, &UMAYACrowdObstacleComponent::OnCrowdCapsuleHit);
    // The Mass pre-detach delegate runs before clearing the exact actor/entity handle.
    // Other paths restore only that same actor, component, entity and unchanged installed value.
    if (!Actor || !Agent || Agent->GetOwner() != Actor || Agent->GetEntityHandle() != Policy.Entity
        || !Capsule || Capsule->GetOwner() != Actor) { RecordContactPolicyEvent(Policy, TEXT("identity_changed_no_property_restore")); return; }
    if (Policy.bChangedResponse && Capsule->GetCollisionProfileName() == Policy.Profile
        && Capsule->GetCollisionResponseToChannel(ECC_PhysicsBody) == ECR_Block)
        Capsule->SetCollisionResponseToChannel(ECC_PhysicsBody, Policy.OriginalResponse);
    if (Policy.bChangedNavSweep && Move && Move->GetOwner() == Actor && Move->UpdatedComponent == Capsule && Move->bSweepWhileNavWalking)
        Move->bSweepWhileNavWalking = Policy.bOriginalNavSweep;
    ++ContactRestoreCount; RecordContactPolicyEvent(Policy, Reason);
}

void UMAYACrowdObstacleComponent::MaintainContactPolicies()
{
    const auto M = Manager.Pin();
    if (!M.IsValid() || M->IsProcessing()) return;
    for (int32 I = ContactPolicies.Num() - 1; I >= 0; --I)
    {
        if (!ContactPolicies.IsValidIndex(I)) continue;
        FScopedContactPolicy& Policy = ContactPolicies[I];
        ACharacter* Actor = Policy.Actor.Get(); UCapsuleComponent* Capsule = Policy.Capsule.Get();
        UCharacterMovementComponent* Move = Policy.Movement.Get(); UMassAgentComponent* Agent = Policy.Agent.Get();
        const bool bSame = Actor && Capsule && Move && Agent && Agent->GetEntityHandle() == Policy.Entity
            && Capsule->GetOwner() == Actor && Move->GetOwner() == Actor;
        if (!bSame || !M->IsEntityActive(Policy.Entity)
            || !FMassEntityView(*M, Policy.Entity).HasTag<FMassCharacterMovementCopyToActorTag>()
            || Move->UpdatedComponent != Capsule || Actor->IsHidden() || !Actor->GetActorEnableCollision() || !Move->IsActive() || !Move->IsComponentTickEnabled()
            || !bHaveNativeChassisPosition || Capsule->GetComponentLocation().ContainsNaN()
            || FVector::Dist2D(Capsule->GetComponentLocation(), LastNativeChassisPosition) > 800.0)
        {
            FScopedContactPolicy Prior = Policy; ContactPolicies.RemoveAtSwap(I);
            RestoreContactPolicy(Prior, TEXT("left_nearby_scope")); continue;
        }
        if (Policy.bExternalEdit) continue;
        if (Capsule->GetCollisionProfileName() != Policy.Profile || Capsule->GetCollisionEnabled() != Policy.CollisionEnabled
            || Capsule->GetCollisionResponseToChannel(ECC_PhysicsBody) != ECR_Block)
        {
            Policy.bExternalEdit = true; RecordContactPolicyEvent(Policy, TEXT("external_collision_edit_preserved")); continue;
        }
        // The sweep flag was set once when admitted. Preserve a foreign change,
        // including an engine reconfiguration; never reassert it every frame.
        if (Policy.bChangedNavSweep && !Move->bSweepWhileNavWalking)
        {
            Policy.bExternalEdit = true;
            RecordContactPolicyEvent(Policy, TEXT("external_nav_sweep_edit_preserved"));
        }
    }
}

void UMAYACrowdObstacleComponent::AgentDetaching(const UMassAgentComponent& Agent)
{
    if (!IsInGameThread()) return;
    for (int32 I = ContactPolicies.Num() - 1; I >= 0; --I)
        if (ContactPolicies.IsValidIndex(I) && ContactPolicies[I].Agent.Get() == &Agent
            && ContactPolicies[I].Entity == Agent.GetEntityHandle())
        {
            FScopedContactPolicy Prior = ContactPolicies[I]; ContactPolicies.RemoveAtSwap(I);
            RestoreContactPolicy(Prior, TEXT("mass_entity_pre_detach"));
        }
}


void UMAYACrowdObstacleComponent::OnCrowdCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
    if (!IsInGameThread() || bStopping || !bFollowing || OtherActor != Robot.Get() || !HitComponent || !Hit.bBlockingHit) return;
    const FScopedContactPolicy* Policy = ContactPolicies.FindByPredicate([&](const FScopedContactPolicy& P)
    { return P.Capsule.Get() == HitComponent && P.Agent.IsValid() && P.Agent->GetEntityHandle() == P.Entity; });
    if (!Policy) return;
    const auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("scope"), TEXT("Actual original capsule OnComponentHit callback; passive listener only, no notify flags enabled. Swept kinematic hits may supply zero NormalImpulse, which is not a solver-impulse assertion."));
    J->SetStringField(TEXT("generation"), LexToString(++ContactHitCount));
    J->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    J->SetNumberField(TEXT("realSeconds"), FPlatformTime::Seconds());
    J->SetStringField(TEXT("actor"), GetPathNameSafe(Policy->Actor.Get()));
    J->SetStringField(TEXT("entity"), Policy->Entity.DebugGetDescription());
    J->SetStringField(TEXT("capsule"), GetPathNameSafe(HitComponent));
    J->SetStringField(TEXT("robotComponent"), GetPathNameSafe(OtherComponent));
    J->SetBoolField(TEXT("blockingHit"), Hit.bBlockingHit);
    J->SetBoolField(TEXT("startPenetrating"), Hit.bStartPenetrating);
    J->SetNumberField(TEXT("hitTime"), Hit.Time); J->SetNumberField(TEXT("penetrationDepthCm"), Hit.PenetrationDepth);
    J->SetArrayField(TEXT("capsuleComponentWorldCm"), MayaNativeCrowdObstacle::Vec(HitComponent->GetComponentLocation()));
    J->SetArrayField(TEXT("normalImpulseAsProvided"), MayaNativeCrowdObstacle::Vec(NormalImpulse));
    J->SetArrayField(TEXT("traceStartWorldCm"), MayaNativeCrowdObstacle::Vec(Hit.TraceStart));
    J->SetArrayField(TEXT("traceEndWorldCm"), MayaNativeCrowdObstacle::Vec(Hit.TraceEnd));
    J->SetArrayField(TEXT("impactWorldCm"), MayaNativeCrowdObstacle::Vec(Hit.ImpactPoint));
    J->SetArrayField(TEXT("impactNormal"), MayaNativeCrowdObstacle::Vec(Hit.ImpactNormal));
    J->SetNumberField(TEXT("currentPhysicsBodyResponse"), uint8(HitComponent->GetCollisionResponseToChannel(ECC_PhysicsBody)));
    FString Json; const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(J, Writer);
    if (FirstContactHitJson.IsEmpty()) FirstContactHitJson = Json;
    ContactHitEvents.Add(Json);
    if (ContactHitEvents.Num() > 64) ContactHitEvents.RemoveAt(0, ContactHitEvents.Num() - 64);
}

void UMAYACrowdObstacleComponent::RemoveSensor()
{
    if (!Sensor.IsSet()) return;
    const auto M = Manager.Pin(); const FMassEntityHandle Entity = Sensor;
    const TWeakObjectPtr<UMAYACrowdObstacleComponent> Owner(this); const auto ExpectedManager = Manager;
    Sensor.Reset();
    LastEvidenceJson = TEXT("{\"schema\":\"MayaRobotics.NativeCrowdObstacle.v1\",\"published\":false}");
    if (!M.IsValid()) return;
    if (!M->IsProcessing())
    { if (MayaNativeCrowdObstacle::Owned(*M, Entity, Owner)) M->DestroyEntity(Entity); return; }
    bRemovalPending = true;
    M->Defer().PushCommand<FMassDeferredDestroyCommand>([Owner, ExpectedManager, Entity](FMassEntityManager& System)
    {
        if (ExpectedManager.Pin().Get() != &System) return;
        if (MayaNativeCrowdObstacle::Owned(System, Entity, Owner)) System.DestroyEntity(Entity);
        if (UMAYACrowdObstacleComponent* Component = Owner.Get()) Component->bRemovalPending = false;
    });
}

void UMAYACrowdObstacleComponent::PhaseStarting(float)
{
    if (!bFollowing || bStopping || !Robot.IsValid() || !ObservedWorld.IsValid()) return;
    RefreshSensor();
}

void UMAYACrowdObstacleComponent::StopObserving()
{
    if (bStopping) return;
    TGuardValue<bool> StopGuard(bStopping, true);
    bFollowing = false;
    if (UMassSimulationSubsystem* Sim = Simulation.Get())
    {
        Sim->GetOnProcessingPhaseStarted(EMassProcessingPhase::PrePhysics).Remove(PrePhase);
        Sim->GetOnProcessingPhaseStarted(EMassProcessingPhase::PostPhysics).Remove(PostPhase);
    }
    if (UMassAgentSubsystem* Agents = AgentSubsystem.Get())
        Agents->GetOnMassAgentComponentEntityDetaching().Remove(AgentDetachingHandle);
    AgentDetachingHandle.Reset(); AgentSubsystem.Reset();
    while (!ContactPolicies.IsEmpty())
    {
        FScopedContactPolicy Prior = ContactPolicies.Pop(false);
        RestoreContactPolicy(Prior, TEXT("bridge_stopped"));
    }
    bHaveNativeChassisPosition = false;
    SetComponentTickInterval(0.1f);
    FWorldDelegates::OnWorldBeginTearDown.Remove(WorldEndingHandle);
    PrePhase.Reset(); PostPhase.Reset(); WorldEndingHandle.Reset();
    RemoveServiceObstacles(); RemoveSensor(); Robot.Reset(); ObservedWorld.Reset(); Simulation.Reset(); Manager.Reset();
}

void UMAYACrowdObstacleComponent::WorldEnding(UWorld* World) { if (World == ObservedWorld.Get()) StopObserving(); }
void UMAYACrowdObstacleComponent::EndPlay(EEndPlayReason::Type Reason) { StopObserving(); Super::EndPlay(Reason); }
void UMAYACrowdObstacleComponent::OnUnregister() { StopObserving(); Super::OnUnregister(); }
