#pragma once
#include "CoreMinimal.h"

// Production-shared GAME admission predicates. Full quaternion angular distance
// is supplied by native connector poses; closure/service still require settling.
namespace MAYALatchedDoorDrive
{
    constexpr double ClosedAngleRad = PI / 180.0;
    constexpr double ClosedOmegaRadps = 0.1;
    inline bool HasClosedPose(bool Known, double FullAngleRad, double RelativeRateRadps)
    {
        return Known && FMath::IsFinite(FullAngleRad) && FullAngleRad >= 0
            && FullAngleRad <= ClosedAngleRad && FMath::IsFinite(RelativeRateRadps);
    }
    inline bool HasSettledClosure(bool Known, double FullAngleRad, double RelativeRateRadps)
    {
        return HasClosedPose(Known, FullAngleRad, RelativeRateRadps)
            && FMath::Abs(RelativeRateRadps) <= ClosedOmegaRadps;
    }
    inline bool DoorDriveInhibited(bool ResetStabilizing, bool ClosedLatched,
        bool BothClosedPoses, bool BothActualNativeLocks)
    {
        return ResetStabilizing || !ClosedLatched || !BothClosedPoses || !BothActualNativeLocks;
    }
    inline bool MotorDriveAdmitted(bool Fresh, bool BrakeRequested, bool DoorInhibited,
        bool DeliveryDriveLocked, bool InputsNeedRelease)
    {
        return Fresh && !BrakeRequested && !DoorInhibited && !DeliveryDriveLocked && !InputsNeedRelease;
    }
}
