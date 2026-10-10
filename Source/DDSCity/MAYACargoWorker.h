#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYACargoWorker.generated.h"

class UCapsuleComponent;
class USkeletalMeshComponent;
class UPoseableMeshComponent;
class USkeletalMesh;
class UAnimSequence;
class UStaticMeshComponent;
class UPrimitiveComponent;

// One real, animated skeletal person. Cargo movement/custody remains owned by
// the Director; this actor never writes a parcel pose or disables its collision.
UCLASS()
class DDSCITY_API AMAYACargoWorker : public AActor
{
    GENERATED_BODY()
public:
    AMAYACargoWorker();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    bool Configure(USkeletalMesh* Mesh, UAnimSequence* Idle, UAnimSequence* Walk, float MeshYawDegrees);
    bool IsConfigured() const { return bConfigured && Fault.IsEmpty(); }
    bool BeginCargoService(UStaticMeshComponent* ParcelBody, UStaticMeshComponent* ParcelVisual, UPrimitiveComponent* Chassis,
        const FTransform& Source, const FTransform& Destination, bool bPickup);
    void SetGrippingEnabled(bool bEnabled);
    bool IsReadyToTransfer() const { return bGripReady && Fault.IsEmpty(); }
    // Call before each already-admitted swept parcel movement. False means hold
    // the existing parcel pose; Fault distinguishes obstruction from animation.
    bool PrepareParcelMove(const FTransform& ProposedPose, float DeltaSeconds);
    void BeginRelease();
    bool IsClearForLidClosure() const;
    void CancelService();
    bool IsApproachBlocked() const { return bApproachBlocked; }
    const FString& GetFault() const { return Fault; }
    FString ReadWorkerEvidenceJson() const;

private:
    bool ReadGripTargets(const FTransform& ParcelPose, FVector& Left, FVector& Right) const;
    bool MoveFeetToCargo(const FVector& CargoPosition, float DeltaSeconds);
    bool ReadGroundedStance(const FVector& CargoPosition, const FVector& Direction, FVector& OutPosition) const;
    bool IsCapsuleStanceClear(const FVector& Position, const FVector& Direction) const;
    bool SelectClearCargoFacing(const FVector& CargoPosition);
    bool PoseForCargo(const FTransform& CargoPose, float DeltaSeconds);
    bool SolveLimb(FName Upper, FName Lower, FName End, const FVector& TargetWorld, const FVector& PoleWorld);
    bool SetComponentBone(FName Bone, const FTransform& Pose);
    bool GetComponentBone(FName Bone, FTransform& Pose) const;
    void RebuildComponentPose();
    FVector PalmWorld(bool bLeft) const;
    bool ObserveArmClearance() const;
    bool ReadGround(const FVector& XY, double& OutHeight) const;

    UPROPERTY() TObjectPtr<UCapsuleComponent> Capsule;
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> BaseAnimation;
    UPROPERTY() TObjectPtr<UPoseableMeshComponent> Person;
    UPROPERTY() TObjectPtr<USkeletalMesh> DefaultPersonMesh;
    UPROPERTY() TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY() TObjectPtr<UAnimSequence> WalkAnimation;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Cargo;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> CargoVisual;
    UPROPERTY() TObjectPtr<UPrimitiveComponent> Carrier;
    TArray<FTransform> ComponentPose;
    FVector Facing = FVector::ForwardVector;
    FVector OriginalFacing = FVector::ForwardVector, ServiceDestinationPosition = FVector::ZeroVector;
    FVector PalmOffset[2] = {};
    FVector FingerAxis[2] = {};
    FVector PalmNormal[2] = {};
    FVector GripTarget[2] = {};
    FTransform LastParcelPose = FTransform::Identity;
    double FootGroundCm = 0, LastWorldSeconds = -1;
    double ActualWalkCmps = 0, GripErrorCm[2] = {};
    uint64 MovementFrame = MAX_uint64, AlphaFrame = MAX_uint64;
    double MovementBudgetCm = 0;
    FVector FrameStartPosition = FVector::ZeroVector;
    float GripAlpha = 0;
    bool bConfigured = false, bServiceActive = false, bGrippingEnabled = false;
    bool bReleasing = false, bGripReady = false, bPlayingWalk = false;
    FString Fault;
    FString LastServiceFault;
    bool bApproachBlocked = false;
    uint64 ApproachBlockedSweepCount = 0;
    double FirstApproachBlockWorldSeconds = -1, LastApproachBlockWorldSeconds = -1;
    FString LastApproachBlockEvidence;
    bool bFacingSelected = false;
    double LastStanceSearchWorldSeconds = -1, SelectedStanceAngleDegrees = 0;
    uint64 StanceSelectionCount = 0, TangentSweepCount = 0;
    bool bSelectedSourcePathClear = false;
    FVector SelectedSourceStance = FVector::ZeroVector, SelectedDestinationStance = FVector::ZeroVector;
    FString StanceSelectionDetail, LastTangentSweepEvidence;
    mutable double ArmMinimumClearanceCm = -1;
    mutable FString ArmClearanceDetail;
};
