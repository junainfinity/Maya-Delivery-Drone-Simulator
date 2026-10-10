#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "MassEntityTypes.h"
#include "MAYACrowdObstacleComponent.generated.h"

class AMAYANativeDemoPawn;
class UMassSimulationSubsystem;
class UMAYACrowdObstacleComponent;
class ACharacter;
class UCapsuleComponent;
class UPrimitiveComponent;
class UCharacterMovementComponent;
class UMassAgentComponent;
class UMassAgentSubsystem;
struct FMassEntityManager;

USTRUCT()
struct DDSCITY_API FMAYACrowdRobotObstacleOwner : public FMassFragment
{
    GENERATED_BODY()
    UPROPERTY() TWeakObjectPtr<UMAYACrowdObstacleComponent> Owner;
};

// Publishes the actual robot to original crowd navigation and traffic obstacle queries.
// Service-body sensors remain crowd-only. It never moves the robot, changes cargo,
// spawns people or edits publisher assets.
UCLASS(ClassGroup=(MAYA))
class DDSCITY_API UMAYACrowdObstacleComponent final : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYACrowdObstacleComponent();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void OnUnregister() override;
    const FString& ReadObstacleEvidenceJson() const { return LastEvidenceJson; }
    void StopObserving();

private:
    bool StartObserving();
    bool RefreshSensor();
    bool ReadNativeFootprint(FTransform& Pose, FVector& Velocity, FBox& Bounds, float& Radius) const;
    void RemoveSensor();
    struct FServiceObstacle
    {
        TWeakObjectPtr<UPrimitiveComponent> Body;
        FMassEntityHandle Entity;
    };
    void RefreshServiceObstacles();
    void RemoveServiceObstacles();
    void DestroyOwnedSensor(FMassEntityHandle Entity);
    TArray<FServiceObstacle> ServiceObstacles;
    FString LastServiceObstaclesJson = TEXT("[]");
    struct FScopedContactPolicy
    {
        TWeakObjectPtr<ACharacter> Actor;
        TWeakObjectPtr<UMassAgentComponent> Agent;
        TWeakObjectPtr<UCapsuleComponent> Capsule;
        TWeakObjectPtr<UCharacterMovementComponent> Movement;
        FMassEntityHandle Entity;
        FName Profile;
        ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
        ECollisionResponse OriginalResponse = ECR_Ignore;
        bool bOriginalNavSweep = false;
        bool bChangedResponse = false, bChangedNavSweep = false, bExternalEdit = false;
        FString BeforeSweepJson, AppliedSweepJson;
    };
    void ConsiderContactPolicy(ACharacter* Actor, UMassAgentComponent* Agent, UCapsuleComponent* Capsule, FMassEntityHandle Entity);
    void MaintainContactPolicies();
    void RestoreContactPolicy(FScopedContactPolicy& Policy, const TCHAR* Reason);
    void AgentDetaching(const UMassAgentComponent& Agent);
    FString CaptureContactSweep(UCapsuleComponent* Capsule) const;
    UFUNCTION() void OnCrowdCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
    TArray<FString> ContactHitEvents;
    FString FirstContactHitJson;
    uint64 ContactHitCount = 0;
    void RecordContactPolicyEvent(const FScopedContactPolicy& Policy, const TCHAR* Action);
    TArray<FScopedContactPolicy> ContactPolicies;
    TArray<FString> ContactPolicyEvents;
    TWeakObjectPtr<UMassAgentSubsystem> AgentSubsystem;
    FDelegateHandle AgentDetachingHandle;
    FVector LastNativeChassisPosition = FVector::ZeroVector;
    bool bHaveNativeChassisPosition = false;
    uint64 ContactApplyCount = 0, ContactRestoreCount = 0;
    void PhaseStarting(float DeltaSeconds);
    void WorldEnding(UWorld* World);
    void UpdateEvidence(const FTransform& Pose, const FVector& Velocity, const FBox& Bounds, float Radius, bool bInGrid);
    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TWeakObjectPtr<UWorld> ObservedWorld;
    TWeakObjectPtr<UMassSimulationSubsystem> Simulation;
    TWeakPtr<FMassEntityManager> Manager;
    FMassEntityHandle Sensor;
    FDelegateHandle PrePhase, PostPhase, WorldEndingHandle;
    bool bFollowing = false, bStopping = false, bRefreshing = false, bRemovalPending = false;
    uint64 PublishGeneration = 0;
    double LastCrowdSampleWorldSeconds = -1;
    FString LastNearbyCrowdJson = TEXT("[]");
    FString LastEvidenceJson = TEXT("{\"schema\":\"MayaRobotics.NativeCrowdObstacle.v1\",\"published\":false}");
    FString Refusal;
};
