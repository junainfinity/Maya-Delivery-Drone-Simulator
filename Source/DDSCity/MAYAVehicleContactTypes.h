#pragma once

#include "CoreMinimal.h"

// Native callback and strictly pre-contact same-body evidence. This is separate
// from calibrated cargo damage: no minimum car speed or impact impulse applies.
struct FMAYAVehiclePrecontactSample
{
    double WorldSeconds = -1; uint64 GameFrame = 0;
    FVector CarCOMCm = FVector::ZeroVector, CarVelocityCmps = FVector::ZeroVector;
    FVector RobotCOMCm = FVector::ZeroVector, RobotVelocityCmps = FVector::ZeroVector;
};

struct FMAYAVehicleContactEvent
{
    uint64 Generation = 0, CallbackGameFrame = 0;
    double WorldSeconds = -1, LatestSampleWorldSeconds = -1;
    bool bNativeContact = false, bBlockingHit = false, bStartPenetrating = false;
    bool bVehicleIdentityKnown = false, bSameNativeBody = false;
    bool bHistoryKnown = false, bIdentityDeferred = false;
    bool bEpisodeContinuation = false;
    double ReconciledWorldSeconds = -1;
    FString VehicleActor, VehicleComponent, RobotComponent, IdentityBasis, Outcome;
    FVector Normal = FVector::ZeroVector, ContactPointCm = FVector::ZeroVector;
    FVector CarCOMCm = FVector::ZeroVector, RobotCOMCm = FVector::ZeroVector;
    FVector RobotContactCOMCm = FVector::ZeroVector, RobotOpticalForward = FVector::ZeroVector;
    FVector CarVelocityCmps = FVector::ZeroVector, RobotVelocityCmps = FVector::ZeroVector;
    FVector CoherentCarProgressCm = FVector::ZeroVector;
    int32 CoherentSamples = 0;
    double CoherentSpanSeconds = 0, ImpulseKgMps = 0;
    TArray<FMAYAVehiclePrecontactSample> PrecontactSamples; // bounded same-body source readback
    bool bMassKnown = false;
    int32 MassEntityIndex = INDEX_NONE, MassEntitySerial = 0, Representation = -1;
    double MassSpeedCmps = 0, MassThrottle = 0, MassBrake = 0;

    bool IsFatal() const
    {
        if (Generation == 0 || !bNativeContact || !bBlockingHit || bStartPenetrating || !bVehicleIdentityKnown || !bSameNativeBody || !bHistoryKnown
            || VehicleActor.IsEmpty() || VehicleComponent.IsEmpty() || RobotComponent.IsEmpty()
            || !FMath::IsFinite(WorldSeconds) || !FMath::IsFinite(LatestSampleWorldSeconds)
            || LatestSampleWorldSeconds < 0 || LatestSampleWorldSeconds >= WorldSeconds
            || WorldSeconds - LatestSampleWorldSeconds > .25 || CoherentSamples < 3
            || !FMath::IsFinite(CoherentSpanSeconds) || CoherentSpanSeconds < .1 || CoherentSpanSeconds > .5
            || Normal.ContainsNaN() || ContactPointCm.ContainsNaN() || RobotCOMCm.ContainsNaN() || CarCOMCm.ContainsNaN() || RobotContactCOMCm.ContainsNaN()
            || RobotOpticalForward.ContainsNaN() || CarVelocityCmps.ContainsNaN()
            || RobotVelocityCmps.ContainsNaN() || CoherentCarProgressCm.ContainsNaN()) return false;
        const FVector N = FVector(Normal.X, Normal.Y, 0).GetSafeNormal();
        const FVector F = FVector(RobotOpticalForward.X, RobotOpticalForward.Y, 0).GetSafeNormal();
        const FVector R(-F.Y, F.X, 0);
        const FVector Toward = (RobotCOMCm - CarCOMCm).GetSafeNormal2D();
        const FVector Point = ContactPointCm - RobotContactCOMCm;
        // Front/rear is a longitudinal contact on the corresponding end.
        // Side or geometrically ambiguous native contacts remain ordinary bumps.
        const double Longitudinal = FVector::DotProduct(N, F), PointLongitudinal = FVector::DotProduct(Point, F);
        return !N.IsNearlyZero() && !F.IsNearlyZero() && FMath::Abs(Normal.Z) < .5
            && FMath::Abs(Longitudinal) >= FMath::Abs(FVector::DotProduct(N, R))
            && PointLongitudinal * Longitudinal < 0
            && !Toward.IsNearlyZero() && FVector::DotProduct(CoherentCarProgressCm, Toward) > .02
            && FVector::DotProduct(CarVelocityCmps, Toward) > 0
            && FVector::DotProduct(CarVelocityCmps - RobotVelocityCmps, Toward) > 0;
    }
};

struct FMAYAVehicleCrashReceipt
{
    bool bFrozen = false;
    FString ShiftInstanceId, JobInstanceId, Reason;
    FMAYAVehicleContactEvent Contact;
    int32 CashDollars = 0, CompletedJobs = 0, RechargeFeesPaidDollars = 0;
    double BatteryPercent = 100, CargoConditionPercent = 100;
};
