#include "MAYADeliveryDirector.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryRingVisitTest, "Maya.Delivery.RingVisit",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMAYADeliveryRingVisitTest::RunTest(const FString&)
{
    FMAYADeliveryRingObservation V;
    V.Advance(TEXT("depot"), true, true, true, 3.0, false);
    TestEqual(TEXT("Entry does not count preceding outside time"), V.ElapsedGameplaySeconds, 0.0);
    TestEqual(TEXT("First observed visit identified"), V.EntryGeneration, uint64(1));
    V.Advance(TEXT("depot"), true, true, true, 4.0, false);
    TestEqual(TEXT("Actual eligible gameplay time accumulates"), V.RemainingGameplaySeconds, 6.0);
    V.ConsumeIfAdmitted(false);
    TestFalse(TEXT("Unsafe early E retains automatic eligibility"), V.bVisitConsumed);
    V.Advance(TEXT("depot"), true, true, true, 80.0, true);
    TestEqual(TEXT("Pause excludes elapsed time"), V.RemainingGameplaySeconds, 6.0);
    V.Advance(TEXT("depot"), true, true, true, 5.5, false);
    TestTrue(TEXT("Cannot reach trigger before ten seconds"), V.RemainingGameplaySeconds > 0);
    V.Advance(TEXT("depot"), true, true, true, 0.5, false);
    TestEqual(TEXT("Ten actual seconds reaches waiting boundary"), V.RemainingGameplaySeconds, 0.0);
    V.ConsumeIfAdmitted(true); // rule-state admission fixture, not native success
    V.Advance(TEXT("depot"), true, true, false, 15.0, false);
    TestTrue(TEXT("Loading preserves consumed visit"), V.bVisitConsumed);
    V.Advance(TEXT("depot"), true, true, true, 15.0, false);
    TestTrue(TEXT("Safe cancellation cannot retrigger in same visit"), V.bVisitConsumed);
    V.Advance(TEXT("depot"), true, false, true, 1.0, false);
    TestFalse(TEXT("Exit resets consumption"), V.bVisitConsumed);
    TestEqual(TEXT("Exit restarts full countdown"), V.RemainingGameplaySeconds, 10.0);
    V.Advance(TEXT("depot"), true, true, true, 1.0, false);
    TestEqual(TEXT("Reentry is separately identified"), V.EntryGeneration, uint64(2));
    V.Advance(TEXT("depot"), true, true, true, 2.0, false);
    V.ConsumeIfAdmitted(true); // actual manual admission suppresses automatic request
    V.Advance(TEXT("depot"), true, true, true, 20.0, false);
    TestEqual(TEXT("Manual attempt stops countdown progression"), V.ElapsedGameplaySeconds, 2.0);
    V.Advance(TEXT("clinic"), true, true, true, 2.0, false);
    TestEqual(TEXT("A changed objective begins a fresh visit"), V.EntryGeneration, uint64(3));
    TestFalse(TEXT("New destination clears prior attempt"), V.bVisitConsumed);
    TestEqual(TEXT("Destination changes preserve full entry window"), V.RemainingGameplaySeconds, 10.0);
    V.Advance(TEXT("clinic"), false, false, false, 1.0, false);
    TestFalse(TEXT("Inactive mission has no inside state"), V.bInside);
    return true;
}
#endif
