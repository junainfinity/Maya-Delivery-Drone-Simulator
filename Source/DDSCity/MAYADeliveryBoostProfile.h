#pragma once

#include "DDSRobotPhysicsTypes.h"

// GAME motor calibration. Achieved road speed requires matched native trials;
// the declared road-speed energy target is independent of motor no-load ratio.
namespace MAYADeliveryBoostProfile
{
    constexpr double PreviousMultiplier = 1.2;
    constexpr double NoLoadMultiplier = 2.12;
    constexpr double PeakTorqueMultiplier = 2.4;
    constexpr double MotorNoLoadRatioToPreviousBoost = NoLoadMultiplier / PreviousMultiplier;
    constexpr double EnergyRoadSpeedTargetRatioToPreviousBoost = 2.0;

    inline FDDSRobotPhysicsProfile Apply(const FDDSRobotPhysicsProfile& Base, bool bBoost, bool bPreviousComparison = false)
    {
        auto Profile = Base;
        if (bBoost)
        {
            const double SelectedNoLoadMultiplier = bPreviousComparison ? PreviousMultiplier : NoLoadMultiplier;
            const double SelectedPeakMultiplier = bPreviousComparison ? PreviousMultiplier : PeakTorqueMultiplier;
            Profile.MotorNoLoadRadps *= SelectedNoLoadMultiplier;
            Profile.MotorPeakNm *= SelectedPeakMultiplier;
        }
        return Profile;
    }

    constexpr double LowSpeedReserveNm = 6.0;
    constexpr double LowSpeedReserveFadeCmps = 50.0;

    inline double CurrentBoostReserveFactor(bool bSpeedKnown, double ChassisSpeedCmps)
    {
        if (!bSpeedKnown || !FMath::IsFinite(ChassisSpeedCmps) || ChassisSpeedCmps < 0) return 0;
        return FMath::Clamp(1.0 - ChassisSpeedCmps / LowSpeedReserveFadeCmps, 0.0, 1.0);
    }

    inline FDDSRobotPhysicsProfile ApplyAtChassisSpeed(const FDDSRobotPhysicsProfile& Base,
        bool bBoost, bool bPreviousComparison, bool bSpeedKnown, double ChassisSpeedCmps)
    {
        auto Profile = Apply(Base, bBoost, bPreviousComparison);
        if (bBoost && !bPreviousComparison)
            Profile.MotorPeakNm += LowSpeedReserveNm * CurrentBoostReserveFactor(bSpeedKnown, ChassisSpeedCmps);
        return Profile;
    }

    struct FMotorTorqueDecision
    {
        double OriginalLawNm = 0, AfterPolicyNm = 0, AppliedNm = 0;
        bool bCurrentBoostPolicyEligible = false, bCounterTorqueSuppressed = false;
    };

    inline FMotorTorqueDecision EvaluateMotorTorque(double Request, double Omega,
        const FDDSRobotPhysicsProfile& Profile, bool bBoost, bool bPreviousComparison, bool bDriveAdmitted)
    {
        FMotorTorqueDecision Decision;
        Decision.OriginalLawNm = DDSMotorTorqueNm(Request, Omega, Profile);
        Decision.bCurrentBoostPolicyEligible = bBoost && !bPreviousComparison;
        const double ClampedRequest = FMath::IsFinite(Request) ? FMath::Clamp(Request, -1.0, 1.0) : 0;
        Decision.bCounterTorqueSuppressed = Decision.bCurrentBoostPolicyEligible
            && ClampedRequest != 0 && ClampedRequest * Decision.OriginalLawNm < 0;
        Decision.AfterPolicyNm = Decision.bCounterTorqueSuppressed ? 0 : Decision.OriginalLawNm;
        Decision.AppliedNm = bDriveAdmitted ? Decision.AfterPolicyNm : 0;
        return Decision;
    }

    // Shared readback arithmetic from the final six applied motor contributions.
    // Admission/freshness/door/release predicates remain at their original caller.
    inline bool HasActualMotorTorque(const double (&MotorNm)[6])
    {
        bool Any = false;
        for (const double Nm : MotorNm)
        {
            if (!FMath::IsFinite(Nm)) return false;
            Any |= FMath::Abs(Nm) > 1.e-12;
        }
        return Any;
    }

    inline double ActualMotorEffortFraction(const double (&MotorNm)[6], double PeakNm, bool bActive)
    {
        if (!bActive || !FMath::IsFinite(PeakNm) || PeakNm <= 0) return 0;
        double Sum = 0;
        for (const double Nm : MotorNm) Sum += FMath::Abs(Nm);
        return FMath::Clamp(Sum / (6.0 * PeakNm), 0.0, 1.0);
    }
}
