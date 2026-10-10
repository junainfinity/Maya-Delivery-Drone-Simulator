#pragma once

#include "CoreMinimal.h"

// Read-only values from the Director's last normal observation. Unknown is the
// safe default; the UI never runs a gameplay mutation to ask whether it is safe.
enum class EMAYAChargeVisitStage : uint8 { None, Qualifying, Charging, Full, Blocked };

struct FMAYAChargePresentation
{
    EMAYAChargeVisitStage Stage = EMAYAChargeVisitStage::None;
    FString StationName, Reason;
    bool bInside = false, bEligible = false;
    double ElapsedSeconds = 0, RemainingSeconds = 10;
};

enum class EMAYARouteAdvice : uint8 { None, SignalUnavailable, Yield, CrossingOpen, CrossingExit };

struct FMAYARouteAdvicePresentation
{
    EMAYARouteAdvice Kind = EMAYARouteAdvice::None;
    FVector Point = FVector::ZeroVector;
    double DistanceCm = 0;
    FString Text;
};

struct FMAYAPlayerPresentation
{
    bool bKnown = false;
    bool bReadyForStart = false;
    bool bCanAccept = false, bCanInteract = false, bCanRecover = false;
    bool bCanNext = false, bCanReplay = false;
    bool bCanChooseOffer = false, bCityBlockOfferAvailable = false;
    int32 SelectedOfferIndex = 0, AvailableOfferCount = 3;
    bool bShiftWon = false, bCanStartNewShift = false;
    bool bDroneTotalled = false, bVehicleContactPending = false;
    bool bRecoveryPending = false, bRecoveryRobotReady = false, bRecoveryCargoReady = false;
    bool bServiceOpenObserved = false, bServiceTransferObserved = false, bServiceClosedObserved = false;
    FString ServiceStage, ActionReason;
    FMAYAChargePresentation Charge;
    FMAYARouteAdvicePresentation RouteAdvice;
};
