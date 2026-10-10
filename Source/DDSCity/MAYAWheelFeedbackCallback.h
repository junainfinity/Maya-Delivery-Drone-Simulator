#pragma once

#include "MAYADeliveryBoostProfile.h"
#include "MAYALatchedDoorDrive.h"
#include "Chaos/SimCallbackObject.h"
#include "Chaos/ParticleHandle.h"
#include "HAL/CriticalSection.h"

struct FMAYAWheelFeedbackCommand
{
    uint64 Generation = 0, Revision = 0, Sequence = 0;
    double IssuedRealS = -1, LeaseSeconds = 0;
    float Throttle = 0, Steer = 0;
    bool bReady = false, bDriveInhibited = true, bDeliveryLocked = false;
    bool bInputsNeedRelease = true, bBrakeRequested = true, bBoostRequested = false;
    bool bClosedLatched = false, bFreshPermission = false;
};

struct FMAYAWheelFeedbackOperands
{
    FMAYAWheelFeedbackCommand Command;
    double SampleRealS = -1, DeltaSeconds = 0;
    bool bNativeStateFinite = false, bActualClosedLocks = false;
    bool bChassisSpeedKnown = false;
    double ChassisSpeedCmps = 0, RelativeWheelOmegaRadps[6] = {};
};

struct FMAYAWheelFeedbackDecision
{
    bool bFresh = false, bDriveAdmitted = false, bBoostActive = false, bBrakeEnabled = true;
    double SelectedPeakNm = 0, NoLoadRadps = 0, ReserveFactor = 0, EffortFraction = 0;
    double MotorNm[6] = {}, BearingNm[6] = {};
    MAYADeliveryBoostProfile::FMotorTorqueDecision MotorDecisions[6];
};

namespace MAYAWheelFeedback
{
    // Shared pure decision used by the PT adapter and focused tests. No state setters.
    inline FMAYAWheelFeedbackDecision Evaluate(const FMAYAWheelFeedbackOperands& O,
        const FDDSRobotPhysicsProfile& Base)
    {
        FMAYAWheelFeedbackDecision D;
        const auto& C = O.Command;
        const double Age = O.SampleRealS - C.IssuedRealS;
        bool Valid = O.bNativeStateFinite && FMath::IsFinite(O.DeltaSeconds) && O.DeltaSeconds > 0
            && FMath::IsFinite(C.Throttle) && FMath::IsFinite(C.Steer);
        for (const double Omega : O.RelativeWheelOmegaRadps) Valid &= FMath::IsFinite(Omega);
        D.bFresh = C.bFreshPermission && FMath::IsFinite(O.SampleRealS) && FMath::IsFinite(C.IssuedRealS)
            && FMath::IsFinite(C.LeaseSeconds) && C.LeaseSeconds > 0 && C.LeaseSeconds <= 0.25
            && FMath::IsFinite(Age) && Age >= 0 && Age <= C.LeaseSeconds && Age <= 0.25;
        D.bDriveAdmitted = Valid && C.bReady && C.bClosedLatched && O.bActualClosedLocks
            && MAYALatchedDoorDrive::MotorDriveAdmitted(D.bFresh, C.bBrakeRequested,
                C.bDriveInhibited, C.bDeliveryLocked, C.bInputsNeedRelease);
        const bool Boost = D.bDriveAdmitted && C.bBoostRequested;
        const auto Profile = MAYADeliveryBoostProfile::ApplyAtChassisSpeed(Base, Boost, false,
            O.bChassisSpeedKnown, O.ChassisSpeedCmps);
        D.SelectedPeakNm = Profile.MotorPeakNm; D.NoLoadRadps = Profile.MotorNoLoadRadps;
        D.ReserveFactor = Boost ? MAYADeliveryBoostProfile::CurrentBoostReserveFactor(
            O.bChassisSpeedKnown, O.ChassisSpeedCmps) : 0;
        D.bBrakeEnabled = C.bBrakeRequested || !D.bDriveAdmitted || !C.bReady;
        for (int32 W = 0; W < 6; ++W)
        {
            const double Omega = O.RelativeWheelOmegaRadps[W];
            const double Request = FMath::IsFinite(C.Throttle) && FMath::IsFinite(C.Steer)
                ? FMath::Clamp(double(-C.Throttle + (W < 3 ? -C.Steer : C.Steer)), -1.0, 1.0) : 0;
            if (FMath::IsFinite(Omega))
            {
                D.MotorDecisions[W] = MAYADeliveryBoostProfile::EvaluateMotorTorque(
                    Request, Omega, Profile, Boost, false, D.bDriveAdmitted);
                D.MotorNm[W] = D.MotorDecisions[W].AppliedNm;
                D.BearingNm[W] = DDSBrakeBearingTorqueNm(false, Omega, Base);
            }
        }
        D.bBoostActive = Boost && MAYADeliveryBoostProfile::HasActualMotorTorque(D.MotorNm);
        D.EffortFraction = MAYADeliveryBoostProfile::ActualMotorEffortFraction(
            D.MotorNm, D.SelectedPeakNm, D.bBoostActive);
        return D;
    }
}

struct FMAYAWheelFeedbackControl
{
    FCriticalSection Mutex;
    uint64 Generation = 0, Revision = 0;
    bool bRetiring = true;
};

struct FMAYAWheelFeedbackSetup
{
    uint64 Generation = 0;
    int32 ParticleIds[9] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE,
        INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
    FTransform JointInChassis[8], JointInPart[8];
    FDDSRobotPhysicsProfile BaseProfile;
    TSharedPtr<FMAYAWheelFeedbackControl, ESPMode::ThreadSafe> Control;
};

struct FMAYAWheelFeedbackInput : Chaos::FSimCallbackInput
{
    bool bHasSetup = false;
    FMAYAWheelFeedbackSetup Setup;
    FMAYAWheelFeedbackCommand Command;
    void Reset() { *this = FMAYAWheelFeedbackInput(); }
};

struct FMAYAWheelFeedbackSnapshot
{
    uint64 Generation = 0, Revision = 0, Sequence = 0, StepSequence = 0;
    double SimTimeS = -1, DeltaSeconds = 0, SampleRealS = -1;
    bool bOwnerCurrent = false, bNineBodiesBound = false, bEightJointIdentities = false;
    bool bNativeStateFinite = false, bActualClosedLocks = false, bWheelBrakesVerified = false;
    bool bChassisSpeedKnown = false, bChassisNativeBodyAvailable = false;
    FVector ChassisVelocityCmps = FVector::ZeroVector;
    FMAYAWheelFeedbackOperands Operands;
    FMAYAWheelFeedbackDecision CalculatedDecision;
    FMAYAWheelFeedbackDecision Decision;
    bool NativeBrakeEnabled[6] = {};
    bool AppliedPairKnown[6] = {};
    FVector WheelAxes[6];
};

struct FMAYAWheelFeedbackOutput : Chaos::FSimCallbackOutput
{
    FMAYAWheelFeedbackSnapshot Snapshot;
    void Reset() { Snapshot = FMAYAWheelFeedbackSnapshot(); }
};

class FMAYAWheelFeedbackCallback final : public Chaos::TSimCallbackObject<
    FMAYAWheelFeedbackInput, FMAYAWheelFeedbackOutput, Chaos::ESimCallbackOptions::Presimulate>
{
public:
    virtual FName GetFNameForStatId() const override
    { static const FName Name(TEXT("MAYAProductionWheelFeedback")); return Name; }
private:
    virtual void OnPreSimulate_Internal() override;
    bool Bind_Internal(const FMAYAWheelFeedbackSetup& InSetup);
    FMAYAWheelFeedbackSetup Setup;
    FMAYAWheelFeedbackCommand LastCommand;
    Chaos::FWeakParticleHandle Bodies[9];
    int32 JointIndices[8] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE,
        INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
    uint64 Generation = 0, StepSequence = 0;
};
