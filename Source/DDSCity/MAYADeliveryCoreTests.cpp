#include "MAYADeliveryCore.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace MAYADeliveryTests
{
    FMAYADeliveryJob Job()
    {
        FMAYADeliveryJob J;
        J.ProfileId = TEXT("test_standard"); J.Title = TEXT("Controlled delivery");
        J.PickupStationId = TEXT("depot"); J.DropoffStationId = TEXT("recipient");
        return J;
    }
    FMAYADeliveryRobotObservation Robot()
    {
        FMAYADeliveryRobotObservation R;
        R.bKnown = R.bReady = R.bGrounded = R.bUpright = R.bDoorsClosedLatched = true;
        R.ObservationWorldSeconds = 10;
        R.ResetGeneration = 1;
        return R;
    }
    FMAYADeliveryServiceEvidence Evidence(const FMAYADeliveryServiceToken& Token)
    {
        FMAYADeliveryServiceEvidence E;
        E.Token = Token; E.Robot = Robot(); E.ObservedParcelId = Token.JobInstanceId;
        E.ObservedParcelIds = { Token.JobInstanceId };
        E.ObservedParcelCount = 1;
        E.bAuthorizedInstantTransferObserved = E.bActualPlacementEndpointsClear = true;
        E.bOpenEndpointsObserved = E.bParcelAtCorrectDestination
            = E.bActualCustodyObserved = E.bParcelMassObserved = true;
        E.ObservedParcelKg = 1;
        return E;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryTransactionsTest, "Maya.Delivery.Transactions",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryTransactionsTest::RunTest(const FString&)
{
    using namespace MAYADeliveryTests;
    FMAYADeliveryCore Core;
    TestTrue(TEXT("Offer valid job"), Core.Offer(Job()));
    TestTrue(TEXT("Accept unique instance"), Core.Accept(TEXT("run_1")));
    FMAYADeliveryServiceToken Pickup;
    TestFalse(TEXT("Cannot unload before pickup"), Core.BeginService(TEXT("recipient"), FVector::ZeroVector, Robot(), Pickup));
    auto Moving = Robot(); Moving.VelocityCmps = FVector(6, 0, 0);
    TestFalse(TEXT("Moving service denied"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, Moving, Pickup));
    TestTrue(TEXT("Stopped pickup begins"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Pickup));
    auto E = Evidence(Pickup); E.bOpenEndpointsObserved = false;
    TestFalse(TEXT("Timer/target cannot replace actual open"), Core.CommitService(E));
    E = Evidence(Pickup); E.Robot.ResetGeneration = 2;
    TestFalse(TEXT("Old reset receipt cannot commit"), Core.CommitService(E));
    E = Evidence(Pickup); E.Robot.ChassisPose.SetLocation(FVector(6, 0, 0));
    TestFalse(TEXT("Drift invalidates service"), Core.CommitService(E));
    E = Evidence(Pickup); E.ObservedParcelId = TEXT("wrong_cargo");
    TestFalse(TEXT("Wrong parcel denied"), Core.CommitService(E));
    TestTrue(TEXT("Observed pickup commits"), Core.CommitService(Evidence(Pickup)));
    TestFalse(TEXT("Duplicate pickup denied"), Core.CommitService(Evidence(Pickup)));
    TestEqual(TEXT("Pickup earns no reward"), Core.Get().TotalCredits, 0);
    FMAYADeliveryServiceToken Dropoff;
    TestTrue(TEXT("Correct dropoff begins"), Core.BeginService(TEXT("recipient"), FVector::ZeroVector, Robot(), Dropoff));
    E = Evidence(Dropoff); E.bActualCustodyObserved = false;
    TestFalse(TEXT("Missing recipient custody denied"), Core.CommitService(E));
    TestTrue(TEXT("Delivery commits"), Core.CommitService(Evidence(Dropoff)));
    const int32 Reward = Core.Get().TotalCredits;
    TestTrue(TEXT("Reward positive"), Reward > 0);
    TestFalse(TEXT("Duplicate dropoff denied"), Core.CommitService(Evidence(Dropoff)));
    TestEqual(TEXT("Duplicate cannot award again"), Core.Get().TotalCredits, Reward);
    TestEqual(TEXT("One completion"), Core.Get().CompletedJobs, 1);
    TestTrue(TEXT("Next job available"), Core.Offer(Job()));
    TestFalse(TEXT("Reused job identity denied"), Core.Accept(TEXT("run_1")));
    TestTrue(TEXT("Next unique job accepted"), Core.Accept(TEXT("run_2")));
    TestEqual(TEXT("Shift score persists"), Core.Get().TotalCredits, Reward);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryOperatingLimitsTest, "Maya.Delivery.OperatingLimits",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryOperatingLimitsTest::RunTest(const FString&)
{
    using namespace MAYADeliveryTests;
    FMAYADeliveryCore Core;
    auto Heavy = Job(); Heavy.PayloadKg = 4;
    TestFalse(TEXT("Overcapacity job refused"), Core.Offer(Heavy));
    Core.Offer(Job()); Core.Accept(TEXT("limits_1"));
    Core.ApplyImpactDamage(100);
    TestEqual(TEXT("Empty robot collision cannot damage depot parcel"), Core.Get().CargoConditionPercent, 100.0);
    const double BeforeBattery = Core.Get().BatteryPercent;
    Core.Tick(50, true, Robot(), false);
    TestEqual(TEXT("Pause excludes deadline time"), Core.Get().ElapsedSeconds, 0.0);
    TestEqual(TEXT("Pause excludes battery"), Core.Get().BatteryPercent, BeforeBattery);
    Core.Tick(10, false, Robot(), false);
    TestTrue(TEXT("Measured elapsed progresses"), Core.Get().ElapsedSeconds == 10);
    TestTrue(TEXT("Battery drains"), Core.Get().BatteryPercent < BeforeBattery);
    TestFalse(TEXT("Charge away from depot denied"), Core.SetCharging(true, Robot(), false));
    TestFalse(TEXT("Unfunded explicit connection cannot bypass automatic charging"), Core.SetCharging(true, Robot(), true));
    const double UnfundedBattery = Core.Get().BatteryPercent;
    Core.Tick(1, false, Robot(), true);
    TestTrue(TEXT("Zero cash grants no free energy"), Core.Get().BatteryPercent <= UnfundedBattery);
    Core.Tick(230, false, Robot(), false);
    TestTrue(TEXT("Expired job fails"), Core.Get().Phase == EMAYADeliveryPhase::Failed);
    TestEqual(TEXT("Failure awards no reward"), Core.Get().TotalCredits, 0);
    auto Unsupported = Robot(); Unsupported.bGrounded = false;
    TestFalse(TEXT("Unsafe recovery refused"), Core.RecoverAfterNativeReset(Unsupported));
    TestTrue(TEXT("Measured reset permits recovery"), Core.RecoverAfterNativeReset(Robot()));
    TestTrue(TEXT("Recovered job can be accepted"), Core.Accept(TEXT("limits_2")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryCancellationTest, "Maya.Delivery.Cancellation",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryCancellationTest::RunTest(const FString&)
{
    using namespace MAYADeliveryTests;
    FMAYADeliveryCore Core;
    Core.Offer(Job()); Core.Accept(TEXT("cancel_1"));
    FMAYADeliveryServiceToken Token;
    Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Token);
    Core.Tick(12, false, Robot(), false);
    TestTrue(TEXT("Service timeout needs rollback"), Core.IsServiceCancellationPending());
    TestTrue(TEXT("Drive locked through cancelled closure"), Core.IsDriveLocked());
    TestFalse(TEXT("Late completion after timeout denied"), Core.CommitService(Evidence(Token)));
    auto Open = Robot(); Open.bDoorsClosedLatched = false; Open.bDoorsOpen = true;
    TestFalse(TEXT("Open lids do not release drive"), Core.ConfirmCancelledClosure(Open));
    TestTrue(TEXT("Measured closure releases cancelled cycle"), Core.ConfirmCancelledClosure(Robot()));
    TestTrue(TEXT("Original pickup custody retained"), Core.Get().Phase == EMAYADeliveryPhase::Accepted);
    FMAYADeliveryServiceToken Retry;
    TestTrue(TEXT("Retry begins"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Retry));
    TestFalse(TEXT("Previous generation cannot commit retry"), Core.CommitService(Evidence(Token)));
    TestTrue(TEXT("Correct retry commits"), Core.CommitService(Evidence(Retry)));
    Core.ApplyImpactDamage(100);
    TestEqual(TEXT("One hard collision is bounded to one condition point"), Core.Get().CargoConditionPercent, 99.0);
    TestTrue(TEXT("One collision does not fail an otherwise healthy parcel"), Core.Get().Phase == EMAYADeliveryPhase::Carrying);
    TestEqual(TEXT("Cancelled pickup and impact create no reward"), Core.Get().TotalCredits, 0);
    return true;
}

// Controlled rule receipts exercise transaction and damage boundaries. They do
// not claim native collisions, box placement, or achieved driving endurance.
namespace MAYAInstantRuleTests
{
    FMAYADeliveryRobotObservation Robot(int32 Count = 1, double Time = 10)
    {
        auto R = MAYADeliveryTests::Robot();
        R.bSecuredParcelCountKnown = true; R.SecuredParcelCount = Count;
        R.ObservationWorldSeconds = Time;
        return R;
    }
    FMAYADeliveryServiceEvidence Evidence(const FMAYADeliveryServiceToken& Token, int32 Count)
    {
        auto E = MAYADeliveryTests::Evidence(Token);
        E.ObservedParcelCount = Count; E.ObservedParcelKg = Count;
        if (Count == 2) E.ObservedParcelIds.Add(Token.JobInstanceId + TEXT("/parcel_2"));
        E.Robot = Robot(Count);
        return E;
    }
    bool Load(FMAYADeliveryCore& Core, const FString& Id, int32 Count = 1)
    {
        auto J = MAYADeliveryTests::Job(); J.ParcelCount = Count; J.PayloadKg = Count;
        FMAYADeliveryServiceToken T;
        return Core.Offer(J) && Core.Accept(Id)
            && Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(0), T)
            && Core.CommitService(Evidence(T, Count));
    }
    FMAYADeliveryImpactEvent Impact(uint64 Generation, double Time, double Impulse = 5)
    {
        FMAYADeliveryImpactEvent E;
        E.Generation = Generation; E.Kind = TEXT("ChassisImpact");
        E.WorldSeconds = Time; E.ImpulseKgMps = Impulse; E.ApproachSpeedCmps = 5;
        E.OtherActor = TEXT("controlled_rule_obstacle"); E.OtherComponent = TEXT("fixture");
        return E;
    }
    bool Apply(FMAYADeliveryCore& Core, const FMAYADeliveryImpactEvent& Event)
    {
        auto R = Robot(Core.Get().SecuredParcelCount, FMath::Max(10.0, Event.WorldSeconds));
        R.LastNativeImpact = Event;
        const double Before = Core.Get().CargoConditionPercent;
        Core.Tick(.001, false, R, false);
        return Core.Get().CargoConditionPercent < Before;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryTwoParcelTransactionTest, "Maya.Delivery.TwoParcelTransaction",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryTwoParcelTransactionTest::RunTest(const FString&)
{
    using namespace MAYAInstantRuleTests;
    FMAYADeliveryCore Core;
    auto J = MAYADeliveryTests::Job(); J.ParcelCount = 2; J.PayloadKg = 2;
    TestTrue(TEXT("Two distinct parcels are an explicit valid manifest"), Core.Offer(J));
    Core.Accept(TEXT("two_parcels"));
    FMAYADeliveryServiceToken T;
    TestTrue(TEXT("Two-parcel pickup begins"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(0), T));
    auto E = Evidence(T, 2); E.ObservedParcelCount = 1;
    TestFalse(TEXT("A single two-kg box cannot satisfy a two-parcel manifest"), Core.CommitService(E));
    E = Evidence(T, 2); E.ObservedParcelIds[1] = E.ObservedParcelIds[0];
    TestFalse(TEXT("Duplicate parcel identity cannot satisfy the rear slot"), Core.CommitService(E));
    E = Evidence(T, 2); E.bActualPlacementEndpointsClear = false;
    TestFalse(TEXT("Instant service still requires actual endpoint clearance"), Core.CommitService(E));
    E = Evidence(T, 2); E.bAuthorizedInstantTransferObserved = false; E.bActualTransferPathClear = true;
    TestFalse(TEXT("Old path certificate cannot substitute for the new transaction"), Core.CommitService(E));
    TestTrue(TEXT("Complete measured inventory commits atomically"), Core.CommitService(Evidence(T, 2)));
    TestEqual(TEXT("Two actual secured parcels are represented"), Core.Get().SecuredParcelCount, 2);
    TestEqual(TEXT("Pickup awards zero"), Core.Get().TotalCredits, 0);
    TestTrue(TEXT("Dropoff begins for the complete manifest"), Core.BeginService(TEXT("recipient"), FVector::ZeroVector, Robot(2), T));
    E = Evidence(T, 2); E.ObservedParcelIds.RemoveAt(1);
    TestFalse(TEXT("Partial unloading cannot complete or reward"), Core.CommitService(E));
    TestEqual(TEXT("Partial unloading awards zero"), Core.Get().TotalCredits, 0);
    TestTrue(TEXT("Complete unloading succeeds"), Core.CommitService(Evidence(T, 2)));
    const int32 Credits = Core.Get().TotalCredits;
    TestTrue(TEXT("Successful complete manifest earns reward"), Credits > 0);
    TestFalse(TEXT("Duplicate manifest completion denied"), Core.CommitService(Evidence(T, 2)));
    TestEqual(TEXT("Whole job rewards exactly once"), Core.Get().TotalCredits, Credits);
    TestEqual(TEXT("Both slots are released after delivery"), Core.Get().SecuredParcelCount, 0);
    J.ParcelCount = 3;
    TestFalse(TEXT("More than two parcels cannot be offered"), Core.Offer(J));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryParcelEnduranceTest, "Maya.Delivery.ParcelEndurance",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryParcelEnduranceTest::RunTest(const FString&)
{
    using namespace MAYAInstantRuleTests;
    for (int32 Mode = 0; Mode < 3; ++Mode)
    {
        FMAYADeliveryCore One, Two;
        TestTrue(TEXT("One-parcel matched trial loads"), Load(One, TEXT("one"), 1));
        TestTrue(TEXT("Two-parcel matched trial loads"), Load(Two, TEXT("two"), 2));
        auto R1 = Robot(1), R2 = Robot(2);
        if (Mode > 0) R1.VelocityCmps = R2.VelocityCmps = FVector(100, 0, 0);
        R1.bBoostDriveActive = R2.bBoostDriveActive = Mode == 2;
        R1.BoostMotorEffortFraction = R2.BoostMotorEffortFraction = Mode == 2 ? .5 : 0.0;
        One.Tick(2, false, R1, false); Two.Tick(2, false, R2, false);
        const double Burn1 = 100 - One.Get().BatteryPercent, Burn2 = 100 - Two.Get().BatteryPercent;
        TestTrue(TEXT("Two-parcel endurance is70percent at matched idle, cruise or boost"),
            FMath::IsNearlyEqual(Burn1 / Burn2, 0.7, 1.e-8));
        const double B = Two.Get().BatteryPercent;
        Two.Tick(20, true, R2, false);
        TestEqual(TEXT("Pause freezes two-parcel battery drain"), Two.Get().BatteryPercent, B);
        TestFalse(TEXT("Two-parcel custody cannot bypass recharge funds/dwell"), Two.SetCharging(true, Robot(2), true));
        Two.Tick(.001, false, Robot(2), true);
        TestTrue(TEXT("Unfunded custody grants no energy"), Two.Get().BatteryPercent <= B);
    }
    FMAYADeliveryCore Pending;
    auto J = MAYADeliveryTests::Job(); J.ParcelCount = 2; J.PayloadKg = 2;
    Pending.Offer(J); Pending.Accept(TEXT("pending"));
    auto R = Robot(0); R.VelocityCmps = FVector(100, 0, 0);
    Pending.Tick(1, false, R, false);
    TestTrue(TEXT("Offered count does not charge for boxes not yet secured"),
        FMath::IsNearlyEqual(Pending.Get().BatteryPercent, 99.75, 1.e-8));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryImpactEpisodesTest, "Maya.Delivery.ImpactEpisodes",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryImpactEpisodesTest::RunTest(const FString&)
{
    using namespace MAYAInstantRuleTests;
    FMAYADeliveryCore Core;
    TestTrue(TEXT("Impact trial loads two boxes"), Load(Core, TEXT("impact"), 2));
    TestFalse(TEXT("Pre-custody contact cannot damage loaded cargo"), Apply(Core, Impact(1, 9)));
    auto Bump = Impact(2, 11);
    TestTrue(TEXT("One small bump applies"), Apply(Core, Bump));
    TestEqual(TEXT("Two parcels share one0.5-point damage event"), Core.Get().CargoConditionPercent, 99.5);
    for (int32 I = 0; I < 100; ++I) Apply(Core, Bump);
    TestEqual(TEXT("Repeated physics receipt does not accumulate damage"), Core.Get().CargoConditionPercent, 99.5);
    Bump.ImpulseKgMps = 20; Bump.ApproachSpeedCmps = 50;
    TestTrue(TEXT("Same episode can escalate to hard impact"), Apply(Core, Bump));
    TestEqual(TEXT("Escalation caps the whole event at1point"), Core.Get().CargoConditionPercent, 99.0);
    auto Landing = Impact(1, 11.1); Landing.Kind = TEXT("CurbLanding");
    Landing.DropCm = 10; Landing.PreContactDownCmps = 50;
    Apply(Core, Landing);
    TestEqual(TEXT("Wheel landing and chassis callback cannot double-charge one episode"), Core.Get().CargoConditionPercent, 99.0);
    Landing.Generation = 2; Landing.WorldSeconds = 12;
    TestTrue(TEXT("A distinct curb drop applies"), Apply(Core, Landing));
    TestEqual(TEXT("A distinct curb drop costs1point"), Core.Get().CargoConditionPercent, 98.0);
    auto R = Robot(2, 13); R.LastNativeImpact = Impact(3, 13);
    Core.Tick(1, true, R, false);
    TestEqual(TEXT("Paused observations cannot damage cargo"), Core.Get().CargoConditionPercent, 98.0);
    R.SecuredParcelCount = 0;
    Core.Tick(.01, false, R, false);
    TestEqual(TEXT("No actual secured boxes means no parcel impact damage"), Core.Get().CargoConditionPercent, 98.0);

    FMAYADeliveryCore Late;
    TestTrue(TEXT("Late escalation fixture loads"), Load(Late, TEXT("late")));
    auto SameEvent = Impact(1, 11);
    Apply(Late, SameEvent);
    SameEvent.WorldSeconds = 12; SameEvent.ImpulseKgMps = 20; SameEvent.ApproachSpeedCmps = 50;
    Apply(Late, SameEvent);
    TestEqual(TEXT("One retained contact generation cannot cost1.5points after a merge timeout"), Late.Get().CargoConditionPercent, 99.0);

    // Source-linked observed native samples, not threshold-mirroring values.
    FMAYADeliveryCore Calibrated;
    TestTrue(TEXT("Observed wheel-impact samples load an actual-count rule fixture"), Load(Calibrated, TEXT("native_samples")));
    auto SmallObserved = Impact(1, 11, 1.78585693359375); SmallObserved.ApproachSpeedCmps = 25.98057746887207;
    TestTrue(TEXT("Native game53 low-speed wheel impact qualifies"), Apply(Calibrated, SmallObserved));
    TestEqual(TEXT("Observed small wheel bump costs0.5point"), Calibrated.Get().CargoConditionPercent, 99.5);
    SmallObserved.WorldSeconds = 11.2; SmallObserved.ImpulseKgMps = 2.3901292419433595; SmallObserved.ApproachSpeedCmps = 21.80002784729004;
    TestFalse(TEXT("Actual small follow-through does not become hard solely from greater impulse"), Apply(Calibrated, SmallObserved));
    auto RestingPush = Impact(2, 12, .8); RestingPush.ApproachSpeedCmps = 0;
    TestFalse(TEXT("Observed sustained wheel push at rest does not damage cargo"), Apply(Calibrated, RestingPush));
    auto Noise = Impact(3, 13, .49); Noise.ApproachSpeedCmps = 30;
    TestFalse(TEXT("Below-calibrated impulse noise does not damage cargo"), Apply(Calibrated, Noise));
    auto HardObserved = Impact(4, 14, 2.7561279296875); HardObserved.ApproachSpeedCmps = 53.98066329956055;
    TestTrue(TEXT("Native game53 high-speed wheel impact qualifies as hard"), Apply(Calibrated, HardObserved));
    TestEqual(TEXT("Observed hard impact costs1point"), Calibrated.Get().CargoConditionPercent, 98.5);

    FMAYADeliveryCore Stale;
    Stale.Offer(MAYADeliveryTests::Job()); Stale.Accept(TEXT("stale"));
    auto Before = Robot(0, 10); Before.LastNativeImpact = Impact(100, 10);
    Stale.Tick(.01, false, Before, false);
    FMAYADeliveryServiceToken Token;
    Stale.BeginService(TEXT("depot"), FVector::ZeroVector, Before, Token);
    auto Receipt = Evidence(Token, 1); Receipt.Robot.LastNativeImpact = Before.LastNativeImpact;
    TestTrue(TEXT("Cargo secures at the same world timestamp as earlier empty contact"), Stale.CommitService(Receipt));
    auto After = Robot(1, 10); After.LastNativeImpact = Before.LastNativeImpact;
    Stale.Tick(.01, false, After, false);
    TestEqual(TEXT("Retained pre-custody event at identical timestamp cannot damage new cargo"), Stale.Get().CargoConditionPercent, 100.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryConditionBoundaryTest, "Maya.Delivery.ConditionBoundary",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryConditionBoundaryTest::RunTest(const FString&)
{
    using namespace MAYAInstantRuleTests;
    for (int32 Bumps : {40, 41})
    {
        FMAYADeliveryCore Core;
        TestTrue(TEXT("Condition boundary trial loads"), Load(Core, FString::Printf(TEXT("boundary%d"), Bumps)));
        for (int32 I = 0; I < Bumps; ++I) Apply(Core, Impact(I + 1, 11 + I));
        const double Expected = 100 - Bumps * .5;
        TestEqual(TEXT("Distinct small bumps reduce condition by exact half points"), Core.Get().CargoConditionPercent, Expected);
        FMAYADeliveryServiceToken T;
        TestTrue(TEXT("Damaged cargo can reach recipient service"), Core.BeginService(TEXT("recipient"), FVector::ZeroVector, Robot(1, 60), T));
        auto E = Evidence(T, 1); E.Robot.ObservationWorldSeconds = 60;
        const bool bExpectedSuccess = Bumps == 40;
        TestEqual(TEXT("80percent inclusive;79.5percent refused"), Core.CommitService(E), bExpectedSuccess);
        TestEqual(TEXT("Frozen result retains exact condition"), Core.Get().Result.CargoConditionPercent, Expected);
        TestEqual(TEXT("Frozen success agrees with boundary"), Core.Get().Result.bSuccess, bExpectedSuccess);
        const int32 Credits = Core.Get().TotalCredits;
        if (bExpectedSuccess) TestTrue(TEXT("80percent delivery earns a reward"), Credits > 0);
        else
        {
            TestTrue(TEXT("Below80 becomes a terminal failure"), Core.Get().Phase == EMAYADeliveryPhase::Failed);
            TestEqual(TEXT("Below80 earns zero credits"), Credits, 0);
            TestTrue(TEXT("Failure reason identifies condition"), Core.Get().Result.Reason.Contains(TEXT("80")));
        }
        TestFalse(TEXT("Boundary receipt cannot be replayed"), Core.CommitService(E));
        TestEqual(TEXT("Boundary replay never duplicates reward"), Core.Get().TotalCredits, Credits);
    }
    return true;
}

#endif
