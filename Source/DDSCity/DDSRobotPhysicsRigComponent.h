#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSRobotPhysicsRigComponent.generated.h"

class UPhysicsConstraintComponent;
class UPhysicalMaterial;
class FDDSRobotSimCallback;
struct FDDSRobotRetirementFence;
struct FDDSRobotControlBarrier;
class FDDSRobotLifecycleLease;
namespace Chaos { class FPBDRigidsSolver; }
class UDDSRobotPhysicsRigComponent;

// Copyable compare-and-set receipt; only the rig can issue or refresh its fields.
// This is collision-data ownership, never measured binding or a drive/service permit.
struct FDDSCollisionReadinessCapability
{
    uint64 GetGeneration() const { return Generation; }
    uint64 GetWorldEpoch() const { return WorldEpoch; }
    uint64 GetRevision() const { return Revision; }
    uint64 GetSerial() const { return Serial; }
    bool IsReady() const { return bReady; }
private:
    friend class UDDSRobotPhysicsRigComponent;
    TWeakObjectPtr<UActorComponent> Owner;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> Rig;
    uint64 Generation = 0, WorldEpoch = 0, Revision = 0, Serial = 0;
    bool bReady = false;
};

// Provider must supply measured original parts. Empty binding is a hard refusal.
UCLASS(ClassGroup=(DDSCity), meta=(BlueprintSpawnableComponent))
class DDSCITY_API UDDSRobotPhysicsRigComponent final : public UActorComponent
{
    GENERATED_BODY()
public:
    UDDSRobotPhysicsRigComponent();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    bool CreateMeasuredRig(const FDDSRobotMeasuredBinding& Binding,
        const FDDSRobotPhysicsProfile& Profile, FString& Error);
    bool SubmitCommand(const FDDSRobotCommand& Command);
    bool ConsumeSnapshot(FDDSRobotSnapshot& Out) const;
    UStaticMeshComponent* GetActualChassis() const;
    const FDDSRobotMeasuredBinding& GetMeasuredBinding() const { return Measured; }
    uint64 GetGeneration() const { return Generation; }
    uint64 GetReadinessRevision() const { return ReadinessRevision; }
    uint64 GetCollisionWorldEpoch() const { return WorldEpoch; }
    // Completed state before collision-data admission. Never substitute for ConsumeSnapshot.
    bool ReadPreReadinessCompletedState(FDDSRobotSnapshot& Out) const;
    bool AcquireCollisionReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Out);
    bool CollisionReadinessMatches(UActorComponent* Owner, const FDDSCollisionReadinessCapability& Receipt) const;
    bool CompareAndSetCollisionReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Receipt, bool bReady);
    // False + a new collision-data epoch. Release removes only this matched owner's authority.
    bool WithdrawCollisionReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Receipt, bool bRelease);
    uint64 GetLastSubmittedSequence() const { return LastSequence; }
    EDDSRobotRigState GetRigState() const { return State; }
    bool IsServiceEvidenceFresh(double RealS, FDDSRobotSnapshot& Out) const;
    void InvalidateCollisionWorld(uint64 NewEpoch);
    // I01/A02 supplies collision readiness; no down-ray promotes unknown to ready.
    bool SetCollisionWorldReady(uint64 Epoch, bool bReady);
    bool ValidateFullAssemblyRecoveryPose(const FTransform& WorldChassisCm) const;
    bool ResetAssemblyAtApprovedRecoveryPose(const FDDSRobotRecoveryPermit& Permit);
    void BeginSafeUnregister();
    bool CancelPendingRecovery(uint64 RequestId);
    // A02 body owner MUST wait for this true result before releasing/rebinding.
    bool ReleaseMeasuredBindingsAfterFence();
    bool SubmitN12Command(FDDSN12StepCommand Command);
    uint64 RevokeN12Operation();
    uint64 GetN12OperationEpoch() const { return N12Epoch; } // Game-thread owner state, never a PT shortcut.
    bool ReleaseN12DriveLock(uint64 Epoch);
    FString GetFault() const { return Fault; }
private:
    friend class FDDSRobotLifecycleLease;
    void RetireWithLiveLease(FDDSRobotLifecycleLease& Lease);
    void RevokeObservation(bool bPreserveReadinessOwner = false);
    void ClearCollisionReadinessOwner();
    void RefreshCollisionReadinessReceipt(FDDSCollisionReadinessCapability& Receipt) const;
    void RevokeExpiredCollisionReadinessOwner();
    TWeakObjectPtr<UActorComponent> CollisionReadinessOwner;
    uint64 CollisionReadinessSerial = 0, NextCollisionReadinessSerial = 1;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> BoundParcel;
    uint64 N12Epoch = 1, N12Sequence = 0;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> BoundBodies;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> BoundSuspensionParts;
    UPROPERTY() TArray<TObjectPtr<UPhysicsConstraintComponent>> Joints;
    UPROPERTY() TObjectPtr<UPhysicalMaterial> RobotContactMaterial;
    FDDSRobotMeasuredBinding Measured;
    FDDSRobotPhysicsProfile Profile;
    FDDSRobotSnapshot Latest;
    FDDSRobotCommand LastCommand;
    FDDSRobotSimCallback* Callback = nullptr; // GT registration owner only.
    Chaos::FPBDRigidsSolver* Solver = nullptr;
    TSharedPtr<FDDSRobotRetirementFence, ESPMode::ThreadSafe> Retirement;
    TSharedPtr<FDDSRobotLifecycleLease, ESPMode::ThreadSafe> Lifecycle;
    TSharedPtr<FDDSRobotControlBarrier, ESPMode::ThreadSafe> Control;
    uint64 ReadinessRevision = 1;
    uint64 PendingResetOldGeneration = 0, PendingResetNewGeneration = 0;
    double BindingStartedRealS = 0;
    EDDSRobotRigState State = EDDSRobotRigState::Unbound;
    uint64 Generation = 1;
    uint64 WorldEpoch = 1;
    uint64 LastSequence = 0;
    uint64 LastResetRequest = 0;
    uint64 CoalescedOutputs = 0;
    bool bWorldReady = false;
    bool bSetupSubmitted = false;
    bool bResetPending = false;
    // Construction stack ownership survives synchronous cancellation/Retired.
    // Only the CreateMeasuredRig scope guard changes this private state.
    bool bConstructionInProgress = false;
    FString Fault;
    bool WriteSetupInput();
};
