#pragma once
#include "DDSRobotPhysicsTypes.h"
#include "DDSRobotContactObservation.h"
#include "Chaos/SimCallbackObject.h"
#include "Templates/Atomic.h"
#include "DDSRobotControlBarrier.h"
namespace Chaos { class FPBDJointConstraintHandle; class FPBDJointSettings; }

struct FDDSRobotSimSetup
{
    FDDSN12Calibration Mechanics;
    int32 ParcelParticleId = INDEX_NONE;
    FVector ParcelCOMCm=FVector::ZeroVector, ParcelInertiaKgCm2=FVector::ZeroVector;
    FQuat ParcelPrincipalAxes = FQuat::Identity;
    double ParcelMassKg = 0;
    uint64 Generation = 0;
    int32 ParticleIds[9] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };
    FVector COMCm[9];
    FVector InertiaKgCm2[9];
    FQuat PrincipalAxes[9];
    FTransform RestInChassisCm[9];
    FTransform JointInChassisCm[8];
    FTransform JointInPartCm[8];
    int32 WheelForwardSign[6] = {};
    double JointTravelCm[8] = {}, JointTargetCm[8] = {};
    bool JointExcludeCollision[8] = {};
    TSharedPtr<FDDSRobotControlBarrier, ESPMode::ThreadSafe> Control;
    FDDSRobotPhysicsProfile Profile;
    FDDSRobotContactLimits Contact;
    double SlipDomainCmps = 0;
    double ClosedLidAngleToleranceRad = 0;
    double ClosedLidOmegaToleranceRadps = 0;
};

struct FDDSRobotSimInput : Chaos::FSimCallbackInput
{
    FDDSRobotCommand Command;
    FDDSN12StepCommand Mechanics;
    bool bHasSetup = false;
    FDDSRobotSimSetup Setup;
    bool bApprovedReset = false;
    uint64 ResetRequestId = 0;
    uint64 ResetNewGeneration = 0;
    uint64 ResetOldGeneration = 0, ResetWorldEpoch = 0, ResetReadinessRevision = 0;
    double ResetValidatedRealS = 0;
    FTransform ResetWorldCm[9];
    void Reset() { *this = FDDSRobotSimInput(); }
};
struct FDDSRobotSimOutput : Chaos::FSimCallbackOutput
{
    FDDSRobotSnapshot Snapshot;
    uint64 AppliedResetId = 0;
    uint64 CancelledResetId = 0, ResetOldGeneration = 0;
    void Reset() { Snapshot = FDDSRobotSnapshot(); AppliedResetId = CancelledResetId = ResetOldGeneration = 0; }
};
struct FDDSRobotRetirementFence
{
    TAtomic<bool> bRemovalCommandPassed{false};
};

class FDDSRobotSimCallback final : public Chaos::TSimCallbackObject<
    FDDSRobotSimInput, FDDSRobotSimOutput, Chaos::ESimCallbackOptions::Presimulate>
{
public:
    void ReleaseN12_Internal(); // Solver command before callback unregister, under live lease.
    virtual FName GetFNameForStatId() const override { static const FName Name(TEXT("DDSRobotPhysicalRig")); return Name; }
private:
    virtual void OnPreSimulate_Internal() override;
    void StepN12_Internal(FDDSRobotSnapshot& Out, const FDDSN12StepCommand* Input);
    bool N12JointMatches(const Chaos::FPBDJointSettings& S, int32 I) const;
    Chaos::FWeakParticleHandle Parcel;
    Chaos::FPBDJointConstraintHandle* ParcelJoint = nullptr;
    Chaos::FWeakParticleHandle ParcelJointEndpoints[2];
    bool bParcelEndpointRegistered[2] = {};
    bool bParcelIgnorePairOwned = false;
    bool CreateParcelJoint_Internal(const Chaos::FPBDJointSettings& Settings);
    void RemoveParcelJoint_Internal();
    FDDSN12StepCommand N12Command;
    FDDSN12StepState BoundaryWitness;
    uint64 N12BoundaryEpoch = 0;
    uint64 N12AppliedSequence = 0, N12BoundarySequence = 0, N12MutationStep = 0;
    bool bLidUnlocked[2] = {};
    double ChassisStableSinceS = -1;
    bool BindInternal(const FDDSRobotSimSetup& Setup);
    bool ValidateInternalJoints(bool bFind);
    int32 JointIndices[8] = {INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    uint64 PreviousReadinessRevision = 0;
    uint64 RecoveryInhibitGeneration = 0;
    uint64 RecoveryQualifiedRevision = 0;
    uint64 AppliedRecoveryGeneration = 0;
    Chaos::FWeakParticleHandle Bodies[9];
    FDDSRobotSimSetup Setup;
    FDDSRobotCommand LastCommand;
    FDDSRobotContactObservation Contacts;
    uint64 Generation = 0;
    uint64 StepSequence = 0;
    uint64 CoalescedCommands = 0;
    uint64 LastResetId = 0;
    double PreviousStartS = 0;
    double PreviousDtS = 0;
    double StableSinceS = -1;
    bool bPreviousStepAdmissible = false;
    double PreviousWrappedAngle[6] = {};
    double UnwrappedAngle[6] = {};
    bool bAngleInitialized[6] = {};
    FVector PreviousVelocityCmps[9];
};
