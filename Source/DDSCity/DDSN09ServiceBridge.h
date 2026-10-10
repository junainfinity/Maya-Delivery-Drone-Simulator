#pragma once

#include "CoreMinimal.h"
#include "DDSN08Authority.h"
#include "DDSN09ServiceBay.h"

// The future sole game-thread coordinator supplies these N12/N10 cargo/lid
// facts. Defaults are unavailable, so N09 can never invent physical proof.
struct FDDSN09ExternalCargoDoorObservation
{
    bool bCargoObserved = false;
    FString ObservedCargoId;
    bool bActiveCargoAtExpectedSource = false;
    bool bFullActualLidSweepObserved = false;
    bool bFullActualLidSweepClear = false;
    bool bClosedOperableDoorObserved = false;
    bool bClosedOperableDoor = false;
};

// One builder for prospective prompt and E. N09 does not own E bindings,
// mission state, mechanical callbacks or the save gate.
namespace DDSN09ServiceBridge
{
    FDDSN08Observation BuildCurrentObservation(
        const FDDSN08StableSnapshot& State,
        const FString& BayStationId,
        EDDSN08Leg BayLeg,
        const FDDSN09GeometryObservation& Geometry,
        const FDDSN09ExternalCargoDoorObservation& External);

    FDDSN08Eligibility RefreshPrompt(
        FDDSN08Authority& Authority,
        ADDSN09ServiceBay& Bay,
        const ADDSCityRollingPawn& Pawn,
        const USceneComponent& RobotServiceSocket,
        const FDDSN09ExternalCargoDoorObservation& External);

    FDDSN08Eligibility DispatchInteractEdge(
        FDDSN08Authority& Authority,
        const ADDSN09ServiceBay& Bay,
        const ADDSCityRollingPawn& Pawn,
        const USceneComponent& RobotServiceSocket,
        const FDDSN09ExternalCargoDoorObservation& External,
        bool bPressed);
}
