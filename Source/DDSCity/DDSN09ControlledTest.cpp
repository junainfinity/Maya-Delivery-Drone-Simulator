#if WITH_DEV_AUTOMATION_TESTS
#include "DDSN09ServiceBay.h"
#include "DDSN09ServiceBridge.h"
#include "Misc/AutomationTest.h"

namespace DDSN09Controlled
{
    struct FMotion final : IDDSN08MotionPort
    {
        void SetDriveLocked(bool) override {}
    };
    struct FMechanics final : IDDSN08MechanicsPort
    {
        void BeginPhysicalCycle(const FDDSN08Token&) override {}
        void CancelAndClose(const FDDSN08Token&) override {}
    };
    struct FSave final : IDDSN08SavePort
    {
        FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot&,
            const FDDSN08Token&) override { return FDDSN08SaveCandidate(); }
        bool Submit(const FDDSN08SaveCandidate&) override { return false; }
        FDDSN08RestoreBarrier EstablishRestoreBarrier(
            const FDDSN08StableSnapshot&,
            const FDDSN08SaveCandidate*) override { return FDDSN08RestoreBarrier(); }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN09ProjectionTest,
    "DDSCity.N09.ControlledProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDDSN09ProjectionTest::RunTest(const FString& Parameters)
{
    FDDSN08StableSnapshot State;
    State.MissionInstanceId = TEXT("M-fixture");
    State.PickupStationId = TEXT("Cafe");
    State.DeliveryStationId = TEXT("Office");
    State.Phase = EDDSN08Phase::AwaitingPickup;
    FDDSN08Presentation P;
    P.bPickupBeacon = true;
    FDDSN08Eligibility E;
    E.bAllowed = true;
    E.Reason = EDDSN08Denial::None;
    E.MissionInstanceId = State.MissionInstanceId;
    E.StationId = TEXT("Cafe");
    E.Leg = EDDSN08Leg::Pickup;
    auto Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestTrue(TEXT("matching authoritative pickup allows E display"),Visual.bActive && Visual.bShowE);
    TestTrue(TEXT("pickup has inward parcel symbol and word"),
        Visual.WordAndIcon.Contains(TEXT("PICKUP")) && Visual.WordAndIcon.Contains(TEXT("<-")));
    TestEqual(TEXT("pickup uses rectangular outline"),Visual.OutlineShape,FString(TEXT("RECTANGLE")));

    auto Inactive = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestTrue(TEXT("inactive delivery cannot show E"),!Inactive.bActive && !Inactive.bShowE);
    E.bAllowed = false;
    E.Reason = EDDSN08Denial::Moving;
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestEqual(TEXT("moving in range receives stop reason"),Visual.Prompt,FString(TEXT("Stop to pick up")));
    TestFalse(TEXT("moving cannot display usable E"),Visual.bShowE);
    E.Reason = EDDSN08Denial::WrongStationOrBay;
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestFalse(TEXT("out of bay has no usable E"),Visual.bShowE);
    E.bAllowed = true;
    E.Reason = EDDSN08Denial::None;
    E.StationId = TEXT("Wrong");
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestFalse(TEXT("mismatched eligibility station cannot display E"),Visual.bShowE);

    // Pickup is already committed, so N08's prospective eligibility describes
    // delivery while the green beacon truthfully remains until save ack.
    State.Phase = EDDSN08Phase::Carrying;
    P.SaveGate = EDDSN08Gate::Pending;
    P.Status = TEXT("Saving transfer");
    E.bAllowed = false;
    E.Reason = EDDSN08Denial::WrongPhaseOrCargo;
    E.StationId = TEXT("Cafe");
    E.Leg = EDDSN08Leg::Delivery;
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestTrue(TEXT("pickup beacon stays green while committed save pending"),Visual.bActive);
    TestFalse(TEXT("pickup pending save never shows E"),Visual.bShowE);
    TestEqual(TEXT("pickup pending reports N08 status"),Visual.Prompt,P.Status);
    P.SaveGate = EDDSN08Gate::FailedRetryable;
    P.Status = TEXT("Committed | Save failed | Retry save");
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Cafe"),
        EDDSN08Leg::Pickup,State,P,E);
    TestTrue(TEXT("pickup save failure keeps green beacon"),Visual.bActive);
    TestFalse(TEXT("pickup save failure never shows E"),Visual.bShowE);
    TestEqual(TEXT("pickup failure reports N08 status"),Visual.Prompt,P.Status);

    State.Phase = EDDSN08Phase::Carrying;
    P.SaveGate = EDDSN08Gate::None;
    P.bPickupBeacon = false;
    P.bDeliveryBeacon = true;
    E.bAllowed = true;
    E.Reason = EDDSN08Denial::None;
    E.StationId = TEXT("Office");
    E.Leg = EDDSN08Leg::Delivery;
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestTrue(TEXT("delivery is active after authority pickup ack"),Visual.bActive && Visual.bShowE);
    TestTrue(TEXT("delivery has outward parcel symbol and word"),
        Visual.WordAndIcon.Contains(TEXT("DELIVER")) && Visual.WordAndIcon.Contains(TEXT("->")));
    TestEqual(TEXT("delivery uses chevron outline"),Visual.OutlineShape,FString(TEXT("CHEVRON")));
    P.SaveGate = EDDSN08Gate::Pending;
    P.Status = TEXT("Saving transfer");
    E.bAllowed = false;
    E.Reason = EDDSN08Denial::BusyOrSaving;
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestTrue(TEXT("red remains active during delivery save gate"),Visual.bActive);
    TestFalse(TEXT("save gate has no E"),Visual.bShowE);
    TestEqual(TEXT("save gate explains wait"),Visual.Prompt,P.Status);
    State.Phase = EDDSN08Phase::Completed;
    E.Leg = EDDSN08Leg::Pickup; // N08's post-commit default next-leg projection.
    E.Reason = EDDSN08Denial::WrongPhaseOrCargo;
    P.Status = TEXT("Saving transfer");
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestTrue(TEXT("delivery beacon stays red while completion save pending"),Visual.bActive);
    TestFalse(TEXT("delivery pending save never shows E"),Visual.bShowE);
    TestEqual(TEXT("delivery pending reports N08 status"),Visual.Prompt,P.Status);
    P.SaveGate = EDDSN08Gate::FailedRetryable;
    P.Status = TEXT("Committed | Save failed | Retry save");
    Visual = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestTrue(TEXT("delivery save failure keeps red beacon"),Visual.bActive);
    TestFalse(TEXT("delivery save failure never shows E"),Visual.bShowE);
    TestEqual(TEXT("delivery failure reports N08 status"),Visual.Prompt,P.Status);
    P.SaveGate = EDDSN08Gate::None;
    P.bDeliveryBeacon = false;
    Inactive = ADDSN09ServiceBay::ProjectReadOnly(TEXT("Office"),
        EDDSN08Leg::Delivery,State,P,E);
    TestFalse(TEXT("saved completion turns bay neutral"),Inactive.bActive);

    FDDSN09GeometryObservation G;
    G.bKnown = true;
    G.bGrounded = true;
    G.ActualVelocityCmPerSecond = FVector(-18.0,9.0,-7.0);
    G.RobotSocketCm = FVector(10,20,40);
    G.StationPointCm = FVector(20,30,40);
    G.bServicePathObserved = true;
    G.bServicePathClear = true;
    G.bDeclaredProxyEnvelopeObserved = true;
    G.bDeclaredProxyEnvelopeClear = true;
    FDDSN09ExternalCargoDoorObservation UnknownN12;
    State.Phase = EDDSN08Phase::Carrying;
    const FDDSN08Observation O = DDSN09ServiceBridge::BuildCurrentObservation(
        State,TEXT("Office"),EDDSN08Leg::Delivery,G,UnknownN12);
    TestTrue(TEXT("active typed service geometry is known"),O.bKnown);
    TestTrue(TEXT("bridge preserves full signed 3D velocity"),
        O.ActualVelocityCmPerSecond.Equals(G.ActualVelocityCmPerSecond,KINDA_SMALL_NUMBER));
    TestTrue(TEXT("proxy fit never invents N12 cargo or actual sweep"),
        !O.bCargoObserved && !O.bFullLidSweepObserved && !O.bClosedOperableDoorObserved);
    TestTrue(TEXT("service path is a real N09 world query field"),
        O.bServicePathObserved && O.bServicePathClear);

    // A unique red actor misconfigured with the active pickup ID must be
    // inactive and unable to start an operation through the shared builder.
    DDSN09Controlled::FMotion Motion;
    DDSN09Controlled::FMechanics Mechanics;
    DDSN09Controlled::FSave Save;
    FDDSN08Authority Authority(Motion,Mechanics,Save);
    const TArray<FString> StarIds = {TEXT("s1"),TEXT("s2"),TEXT("s3")};
    if (!TestTrue(TEXT("controlled identity mission starts"),
        Authority.StartFreshMission(TEXT("M-id"),TEXT("Cargo"),TEXT("Cafe"),
            TEXT("Office"),StarIds))) return false;
    const FDDSN08StableSnapshot IdentityState = Authority.GetStableSnapshot();
    FDDSN09GeometryObservation NearGrounded = G;
    NearGrounded.RobotSocketCm = FVector::ZeroVector;
    NearGrounded.StationPointCm = FVector(100,0,0);
    NearGrounded.ActualVelocityCmPerSecond = FVector::ZeroVector;
    FDDSN09ExternalCargoDoorObservation ControlledExternal;
    ControlledExternal.bCargoObserved = true;
    ControlledExternal.ObservedCargoId = TEXT("Cargo");
    ControlledExternal.bActiveCargoAtExpectedSource = true;
    ControlledExternal.bFullActualLidSweepObserved = true;
    ControlledExternal.bFullActualLidSweepClear = true;
    ControlledExternal.bClosedOperableDoorObserved = true;
    ControlledExternal.bClosedOperableDoor = true;
    const FDDSN08Observation WrongType = DDSN09ServiceBridge::BuildCurrentObservation(
        IdentityState,TEXT("Cafe"),EDDSN08Leg::Delivery,NearGrounded,ControlledExternal);
    TestFalse(TEXT("wrong type with correct ID fails closed"),WrongType.bKnown);
    const FDDSN08Eligibility WrongPrompt = Authority.EvaluateProspectivePrompt(WrongType);
    const FDDSN09VisualDecision WrongVisual = ADDSN09ServiceBay::ProjectReadOnly(
        TEXT("Cafe"),EDDSN08Leg::Delivery,IdentityState,Authority.GetPresentation(),WrongPrompt);
    TestFalse(TEXT("wrong-type bay is visually inactive"),WrongVisual.bActive);
    TestFalse(TEXT("wrong-type bay has no prompt E"),WrongVisual.bShowE);
    const FDDSN08Eligibility WrongPress = Authority.OnInteractButton(true,WrongType);
    TestFalse(TEXT("wrong-type correct-ID E is denied"),WrongPress.bAllowed);
    TestFalse(TEXT("wrong-type E starts no operation"),Authority.HasActiveOperation());
    Authority.OnInteractButton(false,WrongType);
    const FDDSN08Observation CorrectType = DDSN09ServiceBridge::BuildCurrentObservation(
        IdentityState,TEXT("Cafe"),EDDSN08Leg::Pickup,NearGrounded,ControlledExternal);
    TestTrue(TEXT("correct type and active ID remain known"),CorrectType.bKnown);
    TestTrue(TEXT("shared builder still permits valid prospective E"),
        Authority.EvaluateProspectivePrompt(CorrectType).bAllowed);
    return true;
}
#endif
