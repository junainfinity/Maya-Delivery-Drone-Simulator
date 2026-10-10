#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MassEntityTypes.h"
#include "DDSOriginalCityMassObstacleBridge.generated.h"

class ADDSCityRollingPawn;
class AGameModeBase;
class UStaticMeshComponent;
class UDDSRobotPhysicsRigComponent;
class UMassEntitySubsystem;
class UMassSimulationSubsystem;
class UDDSOriginalCityMassObstacleBridge;
struct FMassEntityManager;

// Sensor provenance only; never a physical body, measured certificate or permission.
USTRUCT()
struct DDSCITY_API FDDSOriginalCityObstacleOwnerFragment : public FMassFragment
{
    GENERATED_BODY()
    UPROPERTY() TWeakObjectPtr<UDDSOriginalCityMassObstacleBridge> Owner;
    uint64 OperationSerial = 0;
    uint64 RigGeneration = 0;
    uint64 WorldEpoch = 0;
    uint64 ReadinessRevision = 0;
    uint64 CompletedStep = 0;
};

struct FDDSOriginalCityObstacleObservation
{
    FDDSOriginalCityObstacleObservation()
    { for (int32 I = 0; I < 9; ++I) { ParticleIds[I] = INDEX_NONE; BodyWorldBounds[I] = FBox(ForceInit); } }
    uint64 RigGeneration = 0, WorldEpoch = 0, ReadinessRevision = 0, CompletedStep = 0;
    double CompletedPhysicsTimeS = 0, CapturedRealS = 0, ReadFinishedRealS = 0;
    int32 NativeBoundsReadCount = 0;
    int32 NativeBoundsReadAttempts = 0;
    int32 ParticleIds[9] = {};
    FBox BodyWorldBounds[9];
    FBox AssemblyWorldBounds = FBox(ForceInit);
    FTransform ChassisWorldCm = FTransform::Identity;
    FVector ChassisOriginVelocityCmps = FVector::ZeroVector;
    float SensingRadiusCm = 0;
    bool bPublishedSensor = false;
    FString Refusal;
};

// Explicitly started on an already measured/bound original robot. No automatic
// creation, possession, population, readiness, streaming or physical writes.
UCLASS(ClassGroup=(DDSCity))
class DDSCITY_API UDDSOriginalCityMassObstacleBridge final : public UActorComponent
{
    GENERATED_BODY()
public:
    UDDSOriginalCityMassObstacleBridge();
    bool StartObservingBoundOriginalAssembly(ADDSCityRollingPawn* Pawn, FString& Error);
    void StopObservingBoundOriginalAssembly();
    bool IsObserving() const { return bFollowing; }
    bool HasPendingRemoval() const { return bRemovalPending; }
    const FDDSOriginalCityObstacleObservation& GetLastObservation() const { return LastObservation; }
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void OnUnregister() override;
private:
    bool CurrentContext() const;
    bool CaptureCoherentFootprint(FDDSOriginalCityObstacleObservation& Out, FString& Error) const;
    bool RefreshOwnedSensor(FString& Error);
    void RetireOwnedSensor();
    void PhaseStarting(float DeltaSeconds);
    void WorldEnding(UWorld* World);
    TWeakObjectPtr<ADDSCityRollingPawn> FollowedPawn;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> FollowedRig;
    TWeakObjectPtr<UStaticMeshComponent> OriginalBodies[9];
    TWeakObjectPtr<UWorld> OriginalWorld;
    TWeakObjectPtr<AGameModeBase> OriginalMode;
    TWeakObjectPtr<UMassEntitySubsystem> EntitySubsystem;
    TWeakObjectPtr<UMassSimulationSubsystem> SimulationSubsystem;
    TWeakPtr<FMassEntityManager> OwnedManager;
    FMassEntityHandle SensorEntity;
    FDelegateHandle PrePhaseHandle, PostPhaseHandle, WorldEndingHandle;
    uint64 BoundGeneration = 0, OperationSerial = 0;
    bool bFollowing = false, bRefreshing = false, bStopping = false, bRemovalPending = false;
    FDDSOriginalCityObstacleObservation LastObservation;
};
