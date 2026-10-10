#include "MAYADeliveryCore.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

// Controlled GAME authority tests. Native energy/cargo/reset evidence is a
// separate acceptance run; the fixture does not grant physical certificates.
namespace MAYADeliveryAdditionalTests
{
    FMAYADeliveryJob Job()
    {
        FMAYADeliveryJob J;
        J.ProfileId = TEXT("failure_recovery"); J.Title = TEXT("Controlled failure recovery");
        J.PickupStationId = TEXT("depot"); J.DropoffStationId = TEXT("recipient");
        return J;
    }
    FMAYADeliveryRobotObservation Robot(uint64 ResetGeneration = 1)
    {
        FMAYADeliveryRobotObservation R;
        R.bKnown = R.bReady = R.bGrounded = R.bUpright = R.bDoorsClosedLatched = true;
        R.ObservationWorldSeconds = 10;
        R.ResetGeneration = ResetGeneration;
        return R;
    }
    FMAYADeliveryServiceEvidence Evidence(const FMAYADeliveryServiceToken& Token, double ParcelKg = 1)
    {
        FMAYADeliveryServiceEvidence E;
        E.Token = Token; E.Robot = Robot(); E.ObservedParcelId = Token.JobInstanceId;
        E.ObservedParcelIds = { Token.JobInstanceId };
        E.ObservedParcelCount = 1;
        E.bAuthorizedInstantTransferObserved = E.bActualPlacementEndpointsClear = true;
        E.bOpenEndpointsObserved = E.bParcelAtCorrectDestination
            = E.bActualCustodyObserved = E.bParcelMassObserved = true;
        E.ObservedParcelKg = ParcelKg;
        return E;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryDepletionRecoveryTest, "Maya.Delivery.DepletionRecovery",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryDepletionRecoveryTest::RunTest(const FString&)
{
    using namespace MAYADeliveryAdditionalTests;
    FMAYADeliveryBalance FixtureBalance;
    FixtureBalance.IdleDrainPercentPerSecond = 100; // accelerate this explicit logic fixture only
    FixtureBalance.MovingDrainPercentPerSecond = 0;
    FMAYADeliveryCore Core(FixtureBalance);
    Core.Offer(Job()); Core.Accept(TEXT("depleted_run"));
    FMAYADeliveryServiceToken OldService;
    TestTrue(TEXT("Pickup service active before depletion"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), OldService));
    Core.Tick(1, false, Robot(), false);
    TestEqual(TEXT("Battery reaches exactly zero"), Core.Get().BatteryPercent, 0.0);
    TestTrue(TEXT("Depletion fails active mission"), Core.Get().Phase == EMAYADeliveryPhase::Failed);
    TestTrue(TEXT("Depletion failure is identified"), Core.Get().Result.Reason.Contains(TEXT("Battery depleted")));
    TestTrue(TEXT("Depletion keeps drive locked"), Core.IsDriveLocked());
    TestTrue(TEXT("Interrupted service needs cleanup"), Core.IsServiceCancellationPending());
    TestFalse(TEXT("Late depleted-service completion cannot reward"), Core.CommitService(Evidence(OldService)));
    TestEqual(TEXT("Depletion earns no credits"), Core.Get().TotalCredits, 0);
    auto Moving = Robot(2); Moving.VelocityCmps = FVector(6, 0, 0);
    TestFalse(TEXT("Moving reset observation cannot recover"), Core.RecoverAfterNativeReset(Moving));
    TestTrue(TEXT("Measured stopped reset restores job availability"), Core.RecoverAfterNativeReset(Robot(2)));
    TestFalse(TEXT("Empty battery cannot accept a new job"), Core.Accept(TEXT("retry_empty")));
    TestFalse(TEXT("Zero cash cannot purchase a depot recharge after recovery"), Core.SetCharging(true, Robot(2), true));
    Core.Tick(30, true, Robot(2), true);
    TestEqual(TEXT("Pause cannot charge empty battery"), Core.Get().BatteryPercent, 0.0);
    auto Fresh = Robot(2); Fresh.ObservationWorldSeconds = 11;
    FMAYADeliveryChargingObservation C; C.WorldSeconds = 11; C.bInside = true; C.StationId = TEXT("depot");
    Core.Tick(1, false, Fresh, C);
    TestEqual(TEXT("Insufficient funds grant no energy"), Core.Get().BatteryPercent, 0.0);
    TestEqual(TEXT("Recovery cannot reset wallet or charge fees"), Core.Get().TotalCredits, 0);
    TestFalse(TEXT("Failed job identity cannot be reused"), Core.Accept(TEXT("depleted_run")));
    TestFalse(TEXT("Fresh retry identity cannot bypass depletion"), Core.Accept(TEXT("retry_empty_again")));
    TestEqual(TEXT("Recovery remains tracked without a free refill"), Core.Get().Recoveries, 1);
    TestFalse(TEXT("Old service token remains invalid after reset/retry"), Core.CommitService(Evidence(OldService)));
    TestEqual(TEXT("Failure/recovery/retry still no phantom reward"), Core.Get().TotalCredits, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryPauseAndFragileTest, "Maya.Delivery.PauseAndFragile",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryPauseAndFragileTest::RunTest(const FString&)
{
    using namespace MAYADeliveryAdditionalTests;
    FMAYADeliveryCore Paused;
    Paused.Offer(Job()); Paused.Accept(TEXT("paused_service"));
    FMAYADeliveryServiceToken Token;
    Paused.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Token);
    const double BeforeBattery = Paused.Get().BatteryPercent;
    Paused.Tick(30, true, Robot(), false);
    TestFalse(TEXT("Paused service does not time out"), Paused.IsServiceCancellationPending());
    TestEqual(TEXT("Paused service excludes deadline elapsed"), Paused.Get().ElapsedSeconds, 0.0);
    TestEqual(TEXT("Paused service excludes battery drain"), Paused.Get().BatteryPercent, BeforeBattery);
    TestTrue(TEXT("Pause cannot complete cargo transfer"), Paused.Get().Phase == EMAYADeliveryPhase::Loading);
    Paused.Tick(12, false, Robot(), false);
    TestTrue(TEXT("Timeout resumes when gameplay resumes"), Paused.IsServiceCancellationPending());
    TestEqual(TEXT("Paused/cancelled service yields no credits"), Paused.Get().TotalCredits, 0);

    FMAYADeliveryCore Fragile;
    auto FragileJob = Job(); FragileJob.PayloadKg = 2; FragileJob.bFragile = true;
    TestTrue(TEXT("Two kg fragile profile allowed within capacity"), Fragile.Offer(FragileJob));
    Fragile.Accept(TEXT("fragile_run"));
    Fragile.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Token);
    TestFalse(TEXT("Wrong actual mass cannot secure fragile parcel"), Fragile.CommitService(Evidence(Token, 1)));
    TestTrue(TEXT("Correct two kg measured parcel can secure"), Fragile.CommitService(Evidence(Token, 2)));
    Fragile.ApplyImpactDamage(25);
    TestEqual(TEXT("A fragile label does not multiply the fixed impact amount"), Fragile.Get().CargoConditionPercent, 99.0);
    Fragile.ApplyImpactDamage(25);
    TestEqual(TEXT("Two distinct hard impacts cost two points"), Fragile.Get().CargoConditionPercent, 98.0);
    TestTrue(TEXT("Healthy fragile cargo remains deliverable"), Fragile.Get().Phase == EMAYADeliveryPhase::Carrying);
    TestEqual(TEXT("Impacts alone earn no reward"), Fragile.Get().TotalCredits, 0);
    return true;
}
#endif
