#pragma once

#include "CoreMinimal.h"
#include "MAYAVehicleContactTypes.h"

// GAME calibration from native game53 actual wheel-first impacts:
// small25.98cm/s,1.786kgm/s (follow-through2.390@21.80);
// hard53.98cm/s,2.756kgm/s (follow-through4.382@45.48).
// A hard episode needs both speed and impulse; stationary pushing cannot rearm.
struct FMAYADeliveryLateralImpactCalibration
{
    static constexpr double MinimumImpulseKgMps = 0.5;
    static constexpr double MinimumApproachCmps = 5.0;
    static constexpr double HardImpulseKgMps = 2.5;
    static constexpr double HardApproachCmps = 40.0;
};

struct FMAYADeliveryImpactEvent
{
    uint64 Generation = 0;
    FString Kind;
    double WorldSeconds = -1, ImpulseKgMps = 0, PreContactDownCmps = 0, DropCm = 0;
    double ApproachSpeedCmps = 0, QuietSecondsBeforeEvent = 0; // actual lateral recontact diagnostics
    FString OtherActor, OtherComponent;
    FString RobotComponent; // actual qualifying native body; legacy ChassisImpact kind includes wheel contacts
};

struct FMAYADeliveryImpactReceipt
{
    uint64 EpisodeGeneration = 0;
    FMAYADeliveryImpactEvent Event;
    double AppliedConditionPoints = 0, EpisodeConditionPoints = 0;
    double ConditionBefore = 100, ConditionAfter = 100;
};

// GAME observations sampled from the owned native robot. These are not a DDS
// assembly admission or the historical continuous-clearance certificate.
struct FMAYADeliveryRobotObservation
{
    bool bKnown = false;
    bool bReady = false;
    bool bGrounded = false; // all six wheels, from current native support
    bool bUpright = false;
    bool bDoorsOpen = false; // measured physical endpoints, not command targets
    bool bDoorsClosedLatched = false; // measured pose and native physical locks
    bool bFreshMovementIntent = false; // actual accepted, unpaused native command with focus/lease/release guards
    bool bBoostDriveActive = false; // fresh native boost drive with actual motor use; never key intent alone
    double BoostMotorEffortFraction = 0.0; // mean absolute applied six-wheel motor Nm / boosted peak; excludes bearing/brake
    bool bSecuredParcelCountKnown = false;
    int32 SecuredParcelCount = 0; // Director observes actual independent slot restraints, never infers from kg
    FMAYADeliveryImpactEvent LastNativeImpact;
    FTransform ChassisPose = FTransform::Identity;
    FVector VelocityCmps = FVector::ZeroVector;
    FVector AngularVelocityRadps = FVector::ZeroVector;
    double OpticalForwardCmps = 0.0;
    uint8 SupportMask = 0;
    double DoorAngleDegrees[2] = {};
    double DoorOmegaRadps[2] = {};
    uint64 ResetGeneration = 0;
    double ObservationWorldSeconds = -1.0;
    FString Fault;

    bool IsStoppedAndSupported() const
    {
        return bKnown && bReady && bGrounded && bUpright && Fault.IsEmpty()
            && !ChassisPose.ContainsNaN() && !VelocityCmps.ContainsNaN()
            && !AngularVelocityRadps.ContainsNaN() && AngularVelocityRadps.Size() <= 0.05
            && FMath::IsFinite(ObservationWorldSeconds) && ObservationWorldSeconds >= 0.0
            && VelocityCmps.Size() <= 5.0;
    }
};

enum class EMAYADeliveryPhase : uint8
{
    Available, Accepted, Loading, Carrying, Unloading, Succeeded, Failed
};

struct FMAYADeliveryJob
{
    FString ProfileId;
    FString Title;
    FString PickupStationId;
    FString DropoffStationId;
    double PayloadKg = 1.0; // aggregate actual mass; count is independent
    int32 ParcelCount = 1;
    double DeadlineSeconds = 240.0;
    bool bFragile = false;
};

struct FMAYADeliveryServiceToken
{
    FString JobInstanceId;
    uint64 Generation = 0;
    bool bPickup = true;
    FString StationId;
    uint64 RobotResetGeneration = 0;
    double StartedWorldSeconds = -1;
    FVector RobotServicePosition = FVector::ZeroVector;

    bool Equals(const FMAYADeliveryServiceToken& Other) const
    {
        return !JobInstanceId.IsEmpty() && Generation != 0
            && JobInstanceId == Other.JobInstanceId && Generation == Other.Generation
            && bPickup == Other.bPickup && StationId == Other.StationId
            && RobotResetGeneration == Other.RobotResetGeneration
            && StartedWorldSeconds == Other.StartedWorldSeconds
            && RobotServicePosition.Equals(Other.RobotServicePosition, 0.01);
    }
};

struct FMAYADeliveryServiceEvidence
{
    FMAYADeliveryServiceToken Token;
    FMAYADeliveryRobotObservation Robot;
    FString ObservedParcelId; // first parcel, retained for diagnostics
    TArray<FString> ObservedParcelIds;
    int32 ObservedParcelCount = 1;
    bool bAuthorizedInstantTransferObserved = false;
    bool bActualPlacementEndpointsClear = false;
    bool bOpenEndpointsObserved = false;
    bool bActualTransferPathClear = false; // historical field; instant service never asserts this proof
    bool bParcelAtCorrectDestination = false;
    bool bActualCustodyObserved = false;
    bool bParcelMassObserved = false;
    double ObservedParcelKg = 0.0;
};

struct FMAYADeliveryResult
{
    FString JobInstanceId;
    FString Title;
    FString Reason;
    bool bSuccess = false;
    int32 Credits = 0; // compatibility name: frozen exact delivery earnings in dollars
    double ElapsedSeconds = 0.0;
    double BatteryPercent = 100.0;
    double CargoConditionPercent = 100.0;
    int32 Recoveries = 0;
};

struct FMAYADeliveryBalance
{
    double CapacityKg = 3.0; // declared GAME limit, additionally checked natively
    double IdleDrainPercentPerSecond = 0.01;
    double MovingDrainPercentPerSecond = 0.24; // GAME trial coefficient per measured100cm/s
    double BoostMovingDrainMultiplier = 2.25; // previous boost movement coefficient, retained before normalization
    double BoostSpeedNormalization = 2.0; // new2x speed compares with previous1x boosted speed
    double BoostTotalDrainMultiplier = 1.2; // +20% TOTAL boost drain at corresponding speeds
    double BoostStallFadeCmps = 20.0; // declared GAME effort floor fades to zero by20cm/s
    double ChargePercentPerSecond = 5.0;
    double TwoParcelDrainMultiplier = 1.0 / 0.7; // exact matched-condition70% endurance
    double MinimumDeliveryConditionPercent = 80.0;
    double ServiceRadiusCm = 120.0;
    double ServiceTimeoutSeconds = 12.0;
};

// A diagnostic receipt for the exact tick that automatically disconnects an
// already-enabled charger. It does not alter or grant charge eligibility.
struct FMAYADeliveryChargeDisconnect
{
    uint64 Generation = 0; // zero means no automatic disconnection observed
    TArray<FString> FailedConditions;
    FMAYADeliveryRobotObservation Robot;
    EMAYADeliveryPhase Phase = EMAYADeliveryPhase::Available;
    bool bAtCharger = false;
    double BatteryPercentBeforeTick = 100;
};

struct FMAYADeliveryCashRules
{
    static constexpr int32 OneParcelDollars = 10, TwoParcelDollars = 16;
    static constexpr int32 RechargeFeeDollars = 2, GoalDollars = 100;
    static constexpr double ChargeDwellSeconds = 10;
    static constexpr bool CanAffordRecharge(int32 CashDollars) { return CashDollars >= RechargeFeeDollars; }
};

enum class EMAYADeliveryChargeStage : uint8 { None, Qualifying, Charging, Full, Blocked };

// Actual typed station/safety context; no energy or wallet setter.
struct FMAYADeliveryChargingObservation
{
    FString StationId;
    double WorldSeconds = -1;
    bool bInside = false, bObjectiveServicePriority = false, bRecoveryPending = false;
    bool bFreshMovementIntent = false;
};

struct FMAYADeliveryChargeVisit
{
    uint64 Generation = 0;
    FString StationId, Reason;
    EMAYADeliveryChargeStage Stage = EMAYADeliveryChargeStage::None;
    bool bInside = false, bEligible = false, bPaid = false, bConsumed = false;
    double ElapsedSeconds = 0, RemainingSeconds = FMAYADeliveryCashRules::ChargeDwellSeconds;
};

struct FMAYADeliveryRechargeReceipt
{
    uint64 Generation = 0, VisitGeneration = 0;
    FString ShiftInstanceId, StationId;
    double WorldSeconds = -1, BatteryBefore = 0, BatteryAfter = 0;
    int32 FeeDollars = 0, CashBefore = 0, CashAfter = 0;
};

struct FMAYADeliveryShiftWinReceipt
{
    bool bFrozen = false;
    FString ShiftInstanceId, FinalJobInstanceId;
    int32 CashBefore = 0, DeliveryEarningsDollars = 0, FinalCashDollars = 0, CompletedJobs = 0;
    double CargoConditionPercent = 100;
};

struct FMAYADeliverySnapshot
{
    EMAYADeliveryPhase Phase = EMAYADeliveryPhase::Available;
    FMAYADeliveryJob Job;
    FString JobInstanceId;
    FMAYADeliveryServiceToken Service;
    FMAYADeliveryResult Result;
    double BatteryPercent = 100.0;
    double CargoConditionPercent = 100.0;
    double ElapsedSeconds = 0.0;
    double TravelCm = 0.0;
    int32 Recoveries = 0;
    int32 TotalCredits = 0; // compatibility name: authoritative current NET dollars
    FString ShiftInstanceId;
    bool bShiftStarted = true, bShiftWon = false;
    bool bDroneTotalled = false, bVehicleContactPending = false;
    FMAYAVehicleCrashReceipt VehicleCrash;
    int32 RechargeFeesPaidDollars = 0;
    FMAYADeliveryChargeVisit ChargeVisit;
    FMAYADeliveryRechargeReceipt LastRecharge;
    FMAYADeliveryShiftWinReceipt ShiftWin;
    int32 CompletedJobs = 0;
    bool bCharging = false;
    bool bSecuredParcelCountKnown = false;
    int32 SecuredParcelCount = 0;
    FMAYADeliveryImpactReceipt LastImpact;
    TArray<FMAYADeliveryImpactReceipt> ImpactHistory; // bounded latest64 applied deltas
    FMAYADeliveryChargeDisconnect LastChargeDisconnect;
    FString Status = TEXT("Accept a delivery to begin.");
};
