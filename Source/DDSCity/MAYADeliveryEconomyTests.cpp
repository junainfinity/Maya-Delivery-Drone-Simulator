#include "MAYADeliveryEconomyTestFixture.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryCashGoalTest, "Maya.Delivery.CashGoal",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryCashGoalTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Core; double Time = 10;
    const int32 Counts[] = {1, 2, 2, 2, 1, 1, 1};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Counts); ++I)
    {
        const int32 Before = Core.Get().TotalCredits;
        TestTrue(TEXT("Controlled delivery commits through existing custody rules"), Complete(Core, FString::Printf(TEXT("earn_%d"), I), Counts[I], Time));
        const int32 Exact = Counts[I] == 2 ? 16 : 10;
        TestEqual(TEXT("Whole manifest earns exact cash once"), Core.Get().TotalCredits - Before, Exact);
        TestEqual(TEXT("Frozen result records exact earnings"), Core.Get().Result.Credits, Exact);
    }
    TestEqual(TEXT("Seven deliveries earn88 before fee"), Core.Get().TotalCredits, 88);
    Core.Offer(Job()); Tick(Core, Time, 1, false); QualifyAndGain(Core, Time);
    TestEqual(TEXT("Actual first-positive recharge transaction debits2"), Core.Get().TotalCredits, 86);
    TestEqual(TEXT("One frozen fee receipt"), Core.Get().LastRecharge.FeeDollars, 2);
    Tick(Core, Time, .01, false);
    TestFalse(TEXT("86 current net does not win"), Core.Get().bShiftWon);
    TestTrue(TEXT("Final two-parcel job commits"), Complete(Core, TEXT("winning_two"), 2, Time));
    TestEqual(TEXT("86 plus16 remains102 without clamp"), Core.Get().TotalCredits, 102);
    TestTrue(TEXT("First net threshold crossing wins"), Core.Get().bShiftWon && Core.Get().ShiftWin.bFrozen);
    TestEqual(TEXT("Win records current cash before crossing"), Core.Get().ShiftWin.CashBefore, 86);
    TestEqual(TEXT("Win records exact final earnings"), Core.Get().ShiftWin.DeliveryEarningsDollars, 16);
    TestEqual(TEXT("Win receipt retains102"), Core.Get().ShiftWin.FinalCashDollars, 102);
    const auto Frozen = Core.Get().ShiftWin; const double Battery = Core.Get().BatteryPercent;
    TestFalse(TEXT("No next offer after win"), Core.Offer(Job()));
    TestFalse(TEXT("No job replay after win"), Core.Accept(TEXT("winning_two")));
    TestFalse(TEXT("Recovery cannot reset winning resources"), Core.RecoverAfterNativeReset(Robot(Time, 0)));
    Tick(Core, Time, 20, true);
    TestEqual(TEXT("Won shift does not drain/charge"), Core.Get().BatteryPercent, Battery);
    TestEqual(TEXT("Win remains frozen after later calls"), Core.Get().ShiftWin.FinalJobInstanceId, Frozen.FinalJobInstanceId);
    TestEqual(TEXT("Win cannot duplicate earnings"), Core.Get().TotalCredits, 102);
    FMAYADeliveryCore NetOnly; double NetTime = 10;
    for (int32 I = 0; I < 10; ++I)
    {
        TestTrue(TEXT("Controlled gross earnings scenario commits"), Complete(NetOnly, FString::Printf(TEXT("net_only_%d"), I), 1, NetTime));
        if (I < 2)
        {
            NetOnly.Offer(Job()); Tick(NetOnly, NetTime, 1, false);
            QualifyAndGain(NetOnly, NetTime); Tick(NetOnly, NetTime, .01, false);
        }
    }
    TestEqual(TEXT("Gross100 minus real4 in fees leaves96 net"), NetOnly.Get().TotalCredits, 96);
    TestFalse(TEXT("Gross earnings reaching100 does not win below net goal"), NetOnly.Get().bShiftWon);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryPaidChargingRingTest, "Maya.Delivery.PaidChargingRing",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryPaidChargingRingTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Empty; double EmptyTime = 10; Empty.Offer(Job()); Tick(Empty, EmptyTime, 1, false);
    const double Before = Empty.Get().BatteryPercent;
    QualifyAndGain(Empty, EmptyTime);
    TestEqual(TEXT("Unfunded ring cannot debit cash"), Empty.Get().TotalCredits, 0);
    TestEqual(TEXT("Unfunded ring never creates a fee receipt"), Empty.Get().LastRecharge.Generation, uint64(0));
    TestTrue(TEXT("Unfunded ring grants no energy"), Empty.Get().BatteryPercent <= Before);
    TestFalse(TEXT("Explicit C/API cannot bypass paid automatic dwell"), Empty.SetCharging(true, Robot(EmptyTime), true));
    TestFalse(TEXT("One dollar is insufficient"), FMAYADeliveryCashRules::CanAffordRecharge(1));
    TestTrue(TEXT("Exactly two dollars can fund one event"), FMAYADeliveryCashRules::CanAffordRecharge(2));

    FMAYADeliveryCore Core; double Time = 10; TestTrue(TEXT("Funds come from a real rule commit"), EarnAndPrepare(Core, Time));
    Tick(Core, Time, .001, true);
    const double EntryBattery = Core.Get().BatteryPercent;
    const uint64 Visit = Core.Get().ChargeVisit.Generation;
    for (int32 I = 0; I < 9; ++I) Tick(Core, Time, 1, true);
    TestEqual(TEXT("Nine seconds still has no fee"), Core.Get().LastRecharge.Generation, uint64(0));
    TestFalse(TEXT("Nine seconds has no charging"), Core.Get().bCharging);
    auto R = Robot(Time); auto C = Charging(Time);
    Core.Tick(20, true, R, C);
    TestEqual(TEXT("Pause freezes ring elapsed time"), Core.Get().ChargeVisit.ElapsedSeconds, 9.0);
    TestEqual(TEXT("Pause preserves no fee"), Core.Get().LastRecharge.Generation, uint64(0));
    Tick(Core, Time, 1, true);
    TestEqual(TEXT("Exact ten-second boundary grants no preceding-frame energy"), Core.Get().LastRecharge.Generation, uint64(0));
    const double BeforeGain = Core.Get().BatteryPercent;
    Tick(Core, Time, .01, true);
    TestEqual(TEXT("First positive gain costs exact2"), Core.Get().TotalCredits, 8);
    TestEqual(TEXT("Receipt belongs to observed visit"), Core.Get().LastRecharge.VisitGeneration, Visit);
    TestTrue(TEXT("First gain uses five percentage points per second"), FMath::IsNearlyEqual(Core.Get().BatteryPercent, BeforeGain + .05, 1.e-8));
    TestTrue(TEXT("Qualifying time used ordinary idle drain"), BeforeGain < EntryBattery);
    const double PaidBattery = Core.Get().BatteryPercent;
    Core.Tick(20, true, Robot(Time), Charging(Time));
    TestEqual(TEXT("Paid pause freezes energy"), Core.Get().BatteryPercent, PaidBattery);
    TestTrue(TEXT("Paid pause retains event"), Core.Get().ChargeVisit.bPaid && !Core.Get().ChargeVisit.bConsumed);
    Tick(Core, Time, .01, true);
    TestEqual(TEXT("Paid resume cannot bill again"), Core.Get().LastRecharge.Generation, uint64(1));
    auto Intent = Charging(Time + .01); Intent.bFreshMovementIntent = true;
    Time += .01; Core.Tick(.01, false, Robot(Time), Intent);
    TestFalse(TEXT("Fresh movement intent stops energy before native driving"), Core.Get().bCharging);
    TestTrue(TEXT("Paid interruption consumes this visit"), Core.Get().ChargeVisit.bConsumed);
    QualifyAndGain(Core, Time);
    TestEqual(TEXT("Standing at the same ring cannot rearm"), Core.Get().LastRecharge.Generation, uint64(1));
    Tick(Core, Time, .01, false);
    QualifyAndGain(Core, Time);
    TestEqual(TEXT("Real exit/new entry creates exactly one next event"), Core.Get().LastRecharge.Generation, uint64(2));
    TestEqual(TEXT("Second event costs2"), Core.Get().TotalCredits, 6);
    for (int32 I = 0; I < 3; ++I)
    { Tick(Core, Time, .01, false); QualifyAndGain(Core, Time); }
    TestEqual(TEXT("Five independently entered paid events spend exactly10"), Core.Get().TotalCredits, 0);
    TestEqual(TEXT("Final event paid from exactly2 to0"), Core.Get().LastRecharge.CashBefore, 2);
    TestEqual(TEXT("Final event records zero cash after fee"), Core.Get().LastRecharge.CashAfter, 0);
    const double ZeroCashPaidBattery = Core.Get().BatteryPercent;
    Tick(Core, Time, .001, true);
    TestTrue(TEXT("Already-paid zero-cash visit keeps gaining energy"), Core.Get().BatteryPercent > ZeroCashPaidBattery);
    TestEqual(TEXT("Zero-cash continuation has no second debit"), Core.Get().LastRecharge.Generation, uint64(5));
    for (int32 I = 0; I < 30; ++I) Tick(Core, Time, .1, true);
    TestTrue(TEXT("Completion consumes visit"), Core.Get().ChargeVisit.bConsumed);
    TestEqual(TEXT("Full/standing/tiny idle drain cannot recharge again"), Core.Get().LastRecharge.Generation, uint64(5));
    Tick(Core, Time, .01, false); QualifyAndGain(Core, Time);
    TestEqual(TEXT("Unfunded new visit cannot create debt"), Core.Get().TotalCredits, 0);
    TestEqual(TEXT("Unfunded new visit cannot bill"), Core.Get().LastRecharge.Generation, uint64(5));
    FMAYADeliveryCore One, Two; double T1 = 10, T2 = 10;
    EarnAndPrepare(One, T1); EarnAndPrepare(Two, T2);
    QualifyAndGain(One, T1); QualifyAndGain(Two, T2);
    const double OneBefore = One.Get().BatteryPercent, TwoBefore = Two.Get().BatteryPercent;
    Tick(One, T1, .001, true, 1); Tick(Two, T2, .001, true, 2);
    TestTrue(TEXT("Actual secured count does not alter paid recharge rate"),
        FMath::IsNearlyEqual(One.Get().BatteryPercent - OneBefore, .005, 1.e-8)
        && FMath::IsNearlyEqual(Two.Get().BatteryPercent - TwoBefore, .005, 1.e-8));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryShiftLifecycleTest, "Maya.Delivery.ShiftLifecycle",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryShiftLifecycleTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Core;
    const FString ShiftId = Core.Get().ShiftInstanceId;
    TestTrue(TEXT("Ordinary pristine startup can wait for Start"), Core.PrepareShiftForStart());
    TestTrue(TEXT("Offer does not spend initial resources"), Core.Offer(Job()));
    double Time = 10; Tick(Core, Time, 30, false);
    TestEqual(TEXT("Prestart physical settling/menu time preserves exact100"), Core.Get().BatteryPercent, 100.0);
    TestEqual(TEXT("Prestart cash remains exact0"), Core.Get().TotalCredits, 0);
    TestTrue(TEXT("Prestart drive is locked"), Core.IsDriveLocked());
    TestFalse(TEXT("Accept cannot bypass Start"), Core.Accept(TEXT("too_early")));
    TestTrue(TEXT("One authoritative Start succeeds"), Core.StartShift());
    TestFalse(TEXT("Start cannot be repeated to refill"), Core.StartShift());
    Tick(Core, Time, 1, false);
    TestTrue(TEXT("Full gameplay frames after Start use ordinary drain"), Core.Get().BatteryPercent < 100);
    TestFalse(TEXT("Prepare cannot reset spent battery"), Core.PrepareShiftForStart());
    TestTrue(TEXT("Controlled one-parcel delivery earns10"), Complete(Core, TEXT("earned_for_recovery"), 1, Time));
    const int32 Cash = Core.Get().TotalCredits; const double Energy = Core.Get().BatteryPercent;
    Core.Offer(Job()); Core.Accept(TEXT("active_to_recover"));
    TestTrue(TEXT("Normal native-reset observation recovers active job"), Core.RecoverAfterNativeReset(Robot(Time)));
    TestEqual(TEXT("Recovery preserves wallet"), Core.Get().TotalCredits, Cash);
    TestEqual(TEXT("Recovery preserves battery"), Core.Get().BatteryPercent, Energy);
    TestEqual(TEXT("Recovery preserves shift identity"), Core.Get().ShiftInstanceId, ShiftId);
    Core.Fail(TEXT("controlled failure"));
    TestEqual(TEXT("Failure earns exactzero"), Core.Get().Result.Credits, 0);
    TestEqual(TEXT("Failure preserves current wallet"), Core.Get().TotalCredits, Cash);
    TestTrue(TEXT("Failed job recovery restores availability"), Core.RecoverAfterNativeReset(Robot(Time)));
    TestTrue(TEXT("Replay uses fresh identity"), Core.Accept(TEXT("fresh_replay")));
    TestEqual(TEXT("Replay never resets wallet"), Core.Get().TotalCredits, Cash);
    TestEqual(TEXT("Replay never refills energy"), Core.Get().BatteryPercent, Energy);
    FMAYADeliveryCore NewShift;
    TestEqual(TEXT("Explicit new world starts0"), NewShift.Get().TotalCredits, 0);
    TestEqual(TEXT("Explicit new world starts100"), NewShift.Get().BatteryPercent, 100.0);
    TestTrue(TEXT("New shift has distinct identity"), NewShift.Get().ShiftInstanceId != ShiftId);
    return true;
}
#endif
