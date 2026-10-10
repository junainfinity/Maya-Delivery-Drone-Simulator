#include "DDSN09ServiceBridge.h"

#include "DDSCityRollingPawn.h"
#include "Components/SceneComponent.h"

FDDSN08Observation DDSN09ServiceBridge::BuildCurrentObservation(
    const FDDSN08StableSnapshot& State, const FString& BayStationId,
    EDDSN08Leg BayLeg,
    const FDDSN09GeometryObservation& Geometry,
    const FDDSN09ExternalCargoDoorObservation& External)
{
    FDDSN08Observation O;
    O.MissionInstanceId = State.MissionInstanceId;
    O.StationId = BayStationId;
    // The actor's authored type and ID form one identity. The same fail-closed
    // observation reaches both prospective prompt and the actual E edge.
    const bool bExpectedPickup = State.Phase == EDDSN08Phase::AwaitingPickup
        && BayLeg == EDDSN08Leg::Pickup
        && BayStationId == State.PickupStationId;
    const bool bExpectedDelivery = State.Phase == EDDSN08Phase::Carrying
        && BayLeg == EDDSN08Leg::Delivery
        && BayStationId == State.DeliveryStationId;
    O.bKnown = Geometry.bKnown && !State.MissionInstanceId.IsEmpty()
        && !BayStationId.IsEmpty() && (bExpectedPickup || bExpectedDelivery);
    O.RobotSocketCm = Geometry.RobotSocketCm;
    O.StationPointCm = Geometry.StationPointCm;
    O.ActualVelocityCmPerSecond = Geometry.ActualVelocityCmPerSecond;
    O.bGrounded = Geometry.bGrounded;
    O.ObservationWorldSeconds = Geometry.ObservationWorldSeconds;
    O.bServicePathObserved = Geometry.bServicePathObserved;
    O.bServicePathClear = Geometry.bServicePathClear;
    O.bCargoObserved = External.bCargoObserved;
    O.ObservedCargoId = External.ObservedCargoId;
    O.bActiveCargoAtExpectedSource = External.bActiveCargoAtExpectedSource;
    O.bFullLidSweepObserved = External.bFullActualLidSweepObserved;
    O.bFullLidSweepClear = External.bFullActualLidSweepClear;
    O.bClosedOperableDoorObserved = External.bClosedOperableDoorObserved;
    O.bClosedOperableDoor = External.bClosedOperableDoor;
    // Geometry.bDeclaredProxyEnvelopeClear is intentionally not promoted to
    // actual N12 lid-sweep evidence. It only guides bay-space diagnostics.
    return O;
}

FDDSN08Eligibility DDSN09ServiceBridge::RefreshPrompt(
    FDDSN08Authority& Authority, ADDSN09ServiceBay& Bay,
    const ADDSCityRollingPawn& Pawn, const USceneComponent& RobotServiceSocket,
    const FDDSN09ExternalCargoDoorObservation& External)
{
    const FDDSN09GeometryObservation G = Bay.ObserveGeometry(Pawn,RobotServiceSocket);
    const FDDSN08StableSnapshot State = Authority.GetStableSnapshot();
    const FDDSN08Observation O = BuildCurrentObservation(
        State,Bay.GetStationId(),Bay.GetLeg(),G,External);
    const FDDSN08Eligibility E = Authority.EvaluateProspectivePrompt(O);
    Bay.ApplyReadOnly(State,Authority.GetPresentation(),E);
    return E;
}

FDDSN08Eligibility DDSN09ServiceBridge::DispatchInteractEdge(
    FDDSN08Authority& Authority, const ADDSN09ServiceBay& Bay,
    const ADDSCityRollingPawn& Pawn, const USceneComponent& RobotServiceSocket,
    const FDDSN09ExternalCargoDoorObservation& External, bool bPressed)
{
    const FDDSN09GeometryObservation G = Bay.ObserveGeometry(Pawn,RobotServiceSocket);
    const FDDSN08StableSnapshot State = Authority.GetStableSnapshot();
    const FDDSN08Observation O = BuildCurrentObservation(
        State,Bay.GetStationId(),Bay.GetLeg(),G,External);
    return Authority.OnInteractButton(bPressed,O);
}
