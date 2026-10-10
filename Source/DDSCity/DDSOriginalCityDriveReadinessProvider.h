#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "DDSOriginalCityDriveReadinessProvider.generated.h"

class ADDSCityRollingPawn;
class ACitySamplePlayerController;
class UWorldPartition;
class UWorldPartitionSubsystem;
class UStaticMesh;
class UPackage;
class ULevel;

// Actual data observations, separate from physical wheel contacts and service admission.
struct FDDSOriginalCityCollisionObservation
{
    double StartedRealS = 0, FinishedRealS = 0, CapturedPhysicsRealS = 0;
    double SolverStepS = 0, WorldGravityCmps2 = 0;
    uint64 RigGeneration = 0, CollisionEpoch = 0, ReadinessRevision = 0, PhysicsStepSequence = 0;
    uint64 PublishedReadinessRevision = 0;
    int32 SupportQueries = 0, SupportHits = 0, NearbyQueries = 0, NearbyResults = 0;
    int32 RawBlockingHits = 0;
    bool bEngineSourceObserved = false, bStreamingCompleted = false, bPublishedReady = false;
    FVector SourceLocationCm = FVector::ZeroVector;
    FVector EngineSourceLocationCm = FVector::ZeroVector;
    TArray<FString> SupportComponentPaths, SupportMeshPackages, SupportLevelPackages, SupportActorPackages;
    TArray<FString> QueryHitComponentPaths, QueryHitComponentClasses;
    FString Refusal;
};

// Explicit opt-in helper. No bootstrap, possession, asset/config edit or measured certificate.
UCLASS()
class DDSCITY_API UDDSOriginalCityDriveReadinessProvider final : public UActorComponent,
    public IWorldPartitionStreamingSourceProvider
{
    GENERATED_BODY()
public:
    UDDSOriginalCityDriveReadinessProvider();
    bool StartFollowingBoundOriginalChassis(ADDSCityRollingPawn* Pawn, FString& Error);
    void StopFollowingBoundOriginalChassis();
    bool PollCollisionData(FString& Error);
    const FDDSOriginalCityCollisionObservation& GetLastObservation() const { return LastObservation; }
    virtual bool GetStreamingSource(FWorldPartitionStreamingSource& Out) const override;
    virtual const UObject* GetStreamingSourceOwner() const override { return this; }
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void OnUnregister() override;
private:
    struct FSupportReceipt
    {
        TWeakObjectPtr<UStaticMeshComponent> Component;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UStaticMesh> Mesh;
        TWeakObjectPtr<ULevel> Level;
        TWeakObjectPtr<UPackage> ActorPackage, MeshPackage, LevelPackage, ExternalPackage;
        FTransform Pose;
        const void* ActorHandle = nullptr;
        bool bHadExternalPackage = false;
    };
    bool CurrentContext(bool bRequireRegistration) const;
    bool CaptureSupport(UPrimitiveComponent* Component, FSupportReceipt& Out) const;
    bool SupportUnchanged(const FSupportReceipt& Receipt) const;
    bool FailObservation(const TCHAR* Reason, FString& Error);
    void WorldEnding(UWorld* World);
    TWeakObjectPtr<ADDSCityRollingPawn> FollowedPawn;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> FollowedRig;
    TWeakObjectPtr<UStaticMeshComponent> FollowedChassis;
    TWeakObjectPtr<ACitySamplePlayerController> OriginalController;
    TWeakObjectPtr<AActor> OriginalMode;
    TWeakObjectPtr<UWorld> OriginalWorld;
    TWeakObjectPtr<UWorldPartition> Partition;
    TWeakObjectPtr<UWorldPartitionSubsystem> StreamingSubsystem;
    FDDSCollisionReadinessCapability Capability;
    FDDSOriginalCityCollisionObservation LastObservation;
    FDelegateHandle WorldEndingHandle;
    FName SourceName;
    bool bFollowing = false, bStopping = false, bPolling = false;
    double LastPollRealS = 0;
};
