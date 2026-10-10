#include "MAYADeliveryCore.h"
#include "MAYADeliveryChargeEvidence.h"
#include "MAYADeliveryEconomyTestFixture.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryChargeDisconnectTest, "Maya.Delivery.ChargeDisconnect",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryChargeDisconnectTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Core; double Time = 10;
    TestTrue(TEXT("Recharge money is earned through delivery commits"), EarnAndPrepare(Core, Time));
    QualifyAndGain(Core, Time);
    TestTrue(TEXT("Fresh paid automatic visit is charging"), Core.Get().bCharging);
    Time += .1; auto Unsupported = Robot(Time); Unsupported.bGrounded = false; Unsupported.SupportMask = 31;
    const double Before = Core.Get().BatteryPercent;
    Core.Tick(.1, false, Unsupported, Charging(Time));
    TestFalse(TEXT("Loss of existing support gate disables charging"), Core.Get().bCharging);
    TestTrue(TEXT("Status no longer claims active charging"), Core.Get().Status.StartsWith(TEXT("Charging stopped.")));
    TestTrue(TEXT("Ordinary idle drain continues on disconnect tick"), Core.Get().BatteryPercent < Before);
    const auto Receipt = Core.Get().LastChargeDisconnect;
    TestEqual(TEXT("One actual eligibility-loss receipt"), Receipt.Generation, uint64(1));
    TestEqual(TEXT("Only support predicate failed"), Receipt.FailedConditions.Num(), 1);
    TestTrue(TEXT("Exact failed condition retained"), Receipt.FailedConditions.Contains(TEXT("six_wheel_support")));
    TestEqual(TEXT("Exact support mask retained"), Receipt.Robot.SupportMask, uint8(31));
    TestEqual(TEXT("Exact observation time retained"), Receipt.Robot.ObservationWorldSeconds, Time);
    TestEqual(TEXT("Pre-drain battery retained"), Receipt.BatteryPercentBeforeTick, Before);
    const auto Evidence = MAYADeliveryChargeDisconnectToJson(Receipt);
    TestTrue(TEXT("Durable receipt serializes observed event"), Evidence->GetBoolField(TEXT("observed")));
    TestEqual(TEXT("JSON preserves actual observation time"),
        Evidence->GetObjectField(TEXT("robotObservationAtDisconnect"))->GetNumberField(TEXT("observationWorldSeconds")), Time);
    Tick(Core, Time, 1, true);
    TestTrue(TEXT("Paid interrupted visit remains consumed"), Core.Get().ChargeVisit.bConsumed);
    TestFalse(TEXT("Manual reconnect cannot bypass consumed visit"), Core.SetCharging(true, Robot(Time), true));
    TestEqual(TEXT("Later calls do not overwrite first receipt"), Core.Get().LastChargeDisconnect.Generation, uint64(1));
    Tick(Core, Time, .1, false); QualifyAndGain(Core, Time);
    TestTrue(TEXT("Real exit/new funded visit can charge"), Core.Get().bCharging);
    const double BeforeManual = Core.Get().BatteryPercent;
    TestTrue(TEXT("Compatibility stop ends active visit"), Core.SetCharging(false, Robot(Time), true));
    TestFalse(TEXT("Stop disables energy progress"), Core.Get().bCharging);
    TestEqual(TEXT("Stop itself neither drains nor charges"), Core.Get().BatteryPercent, BeforeManual);
    TestEqual(TEXT("Explicit stop is not an eligibility-loss receipt"), Core.Get().LastChargeDisconnect.Generation, uint64(1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryChargePredicatesTest, "Maya.Delivery.ChargePredicates",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryChargePredicatesTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Core; double Time = 10; EarnAndPrepare(Core, Time); QualifyAndGain(Core, Time);
    auto Unsafe = Robot(Time); Unsafe.VelocityCmps = FVector(6, 0, 0);
    Unsafe.AngularVelocityRadps = FVector(0, 0, .06); Unsafe.bDoorsClosedLatched = false;
    const double BeforePause = Core.Get().BatteryPercent;
    Core.Tick(3, true, Unsafe, Charging(Time, false));
    TestTrue(TEXT("Pause retains active paid visit"), Core.Get().bCharging);
    TestEqual(TEXT("Pause excludes energy progress"), Core.Get().BatteryPercent, BeforePause);
    TestEqual(TEXT("Pause alone has no eligibility-loss receipt"), Core.Get().LastChargeDisconnect.Generation, uint64(0));
    Time += .1; Unsafe.ObservationWorldSeconds = Time;
    Core.Tick(.1, false, Unsafe, Charging(Time, false));
    const auto& Failed = Core.Get().LastChargeDisconnect.FailedConditions;
    TestEqual(TEXT("All four failed physical inputs retained"), Failed.Num(), 4);
    TestTrue(TEXT("Actual station departure retained"), Failed.Contains(TEXT("at_charger")));
    TestTrue(TEXT("Linear stop gate retained"), Failed.Contains(TEXT("speed_at_most_5_cmps")));
    TestTrue(TEXT("Angular stop gate retained"), Failed.Contains(TEXT("angular_speed_at_most_0_05_radps")));
    TestTrue(TEXT("Native settled-latch gate retained"), Failed.Contains(TEXT("doors_closed_latched")));
    TestTrue(TEXT("Exact angular velocity retained"), Core.Get().LastChargeDisconnect.Robot.AngularVelocityRadps.Equals(Unsafe.AngularVelocityRadps));
    TestFalse(TEXT("Invalid state cannot explicitly reconnect"), Core.SetCharging(true, Unsafe, false));
    TestEqual(TEXT("Rejected request does not overwrite receipt"), Core.Get().LastChargeDisconnect.Generation, uint64(1));
    auto Invalid = Core.Get().LastChargeDisconnect;
    Invalid.Robot.ObservationWorldSeconds = std::numeric_limits<double>::quiet_NaN();
    FString Text; const auto Json = MAYADeliveryChargeDisconnectToJson(Invalid);
    TestTrue(TEXT("Nonfinite evidence serializes valid JSON"), FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text)));
    TSharedPtr<FJsonObject> Parsed;
    TestTrue(TEXT("Diagnostic JSON parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed) && Parsed.IsValid());
    if (Parsed) TestTrue(TEXT("Nonfinite raw time remains explicit"),
        Parsed->GetObjectField(TEXT("robotObservationAtDisconnect"))->GetStringField(TEXT("observationWorldSeconds")).Contains(TEXT("nan"), ESearchCase::IgnoreCase));
    FMAYADeliveryCore Full; double FullTime = 10; EarnAndPrepare(Full, FullTime); QualifyAndGain(Full, FullTime);
    Tick(Full, FullTime, 1, true);
    TestEqual(TEXT("Actual paid charge caps at100"), Full.Get().BatteryPercent, 100.0);
    TestTrue(TEXT("Full visit reports completion and consumption"), Full.Get().ChargeVisit.Stage == EMAYADeliveryChargeStage::Full && Full.Get().ChargeVisit.bConsumed);
    TestEqual(TEXT("Completion is not eligibility loss"), Full.Get().LastChargeDisconnect.Generation, uint64(0));
    const auto Absent = MAYADeliveryChargeDisconnectToJson(Full.Get().LastChargeDisconnect);
    TestFalse(TEXT("Absent event cannot invent observation"), Absent->GetBoolField(TEXT("observed")));
    TestFalse(TEXT("Absent event omits default robot sample"), Absent->HasField(TEXT("robotObservationAtDisconnect")));
    return true;
}
#endif
