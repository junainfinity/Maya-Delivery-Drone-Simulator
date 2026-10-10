#pragma once
#include "DDSRobotPhysicsTypes.h"
#include "Chaos/GeometryParticles.h"

namespace Chaos { class FPBDCollisionConstraints; }

struct FDDSRobotContactLimits
{
    double SeparationCm = 0;
    double UpDot = 0;
    double MinLoadedImpulseKgCmps = 0;
    double SleepingAnchorCm = 0;
    double SleepingSpeedCmps = 0;
    double SleepingOmegaRadps = 0;
};

// Physics thread only. No raw constraint pointer survives CaptureCompleted.
class FDDSRobotContactObservation
{
public:
    void Reset(uint64 WorldEpoch);
    void CaptureCompleted(const Chaos::FPBDCollisionConstraints& Contacts,
        const Chaos::FWeakParticleHandle (&Bodies)[9], double CompletedS, double PreviousDt,
        uint64 WorldEpoch, const FDDSRobotContactLimits& Limits, FDDSRobotSnapshot& Out);
private:
    struct FCache
    {
        bool bLoaded = false;
        Chaos::FWeakParticleHandle Wheel;
        Chaos::FWeakParticleHandle Other;
        int32 WheelId = INDEX_NONE;
        int32 OtherId = INDEX_NONE;
        uint64 WheelShape = 0, OtherShape = 0;
        uint64 WheelGeometry = 0, OtherGeometry = 0;
        FVector AnchorInWheelCm = FVector::ZeroVector;
        FVector AnchorInOtherCm = FVector::ZeroVector;
        FVector NormalInOther = FVector::ZeroVector;
        double LastSolvedS = 0;
        FDDSRobotWheelState Evidence;
    };
    FCache Cache[6];
    uint64 Epoch = 0;
    bool ValidateSleeping(int32 I, double TimeS, const FDDSRobotContactLimits& Limits,
        FDDSRobotWheelState& Out) const;
};
