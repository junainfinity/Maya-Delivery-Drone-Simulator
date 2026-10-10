#include "MAYADeliveryCore.h"
#include "MAYADeliveryEconomyTestFixture.h"
#include "MAYADeliveryBoostProfile.h"
#include "MAYALatchedDoorDrive.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace MAYABoostRuleTests
{
    FMAYADeliveryRobotObservation Robot(double SpeedCmps = 0, bool bBoostMotorUse = false, double Effort = 0.5)
    {
        FMAYADeliveryRobotObservation R;
        R.bKnown = R.bReady = R.bGrounded = R.bUpright = R.bDoorsClosedLatched = true;
        R.bBoostDriveActive = bBoostMotorUse;
        R.BoostMotorEffortFraction = bBoostMotorUse ? Effort : 0.0;
        R.ObservationWorldSeconds = 10;
        R.ResetGeneration = 1;
        R.VelocityCmps = FVector(SpeedCmps, 0, 0);
        return R;
    }
    FMAYADeliveryJob Job(double Payload = 1, double Deadline = 240)
    {
        FMAYADeliveryJob J;
        J.ProfileId = TEXT("boost_rule_fixture"); J.Title = TEXT("Boost rule fixture");
        J.PickupStationId = TEXT("depot"); J.DropoffStationId = TEXT("clinic");
        J.PayloadKg = Payload; J.DeadlineSeconds = Deadline;
        return J;
    }
    bool Start(FMAYADeliveryCore& Core, const FString& Id, double Payload = 1, double Deadline = 240)
    { return Core.Offer(Job(Payload, Deadline)) && Core.Accept(Id); }
    bool Near(double Actual, double Expected) { return FMath::IsNearlyEqual(Actual, Expected, 1.e-8); }
    FMAYADeliveryServiceEvidence Pickup(const FMAYADeliveryServiceToken& Token, double Kg)
    {
        FMAYADeliveryServiceEvidence E;
        E.Token = Token; E.Robot = Robot(); E.ObservedParcelId = Token.JobInstanceId;
        E.ObservedParcelIds = { Token.JobInstanceId };
        E.ObservedParcelCount = 1;
        E.bAuthorizedInstantTransferObserved = E.bActualPlacementEndpointsClear = true;
        E.bOpenEndpointsObserved = E.bParcelAtCorrectDestination
            = E.bActualCustodyObserved = E.bParcelMassObserved = true;
        E.ObservedParcelKg = Kg;
        return E;
    }
}

// This fixture supplies controlled GAME observations. It does not claim physical
// speed gain, keyboard delivery, native motor use, charger location or range.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryBoostEnergyTest, "Maya.Delivery.BoostEnergy",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryBoostEnergyTest::RunTest(const FString&)
{
    using namespace MAYABoostRuleTests;
    FMAYADeliveryCore Normal, Boost, Coasting, IdleShift;
    TestTrue(TEXT("Normal job admitted"), Start(Normal, TEXT("normal")));
    TestTrue(TEXT("Boost job admitted"), Start(Boost, TEXT("boost")));
    Start(Coasting, TEXT("coasting")); Start(IdleShift, TEXT("idle_shift"));
    Normal.Tick(1, false, Robot(100), false);
    Boost.Tick(1, false, Robot(100, true), false);
    Coasting.Tick(1, false, Robot(100, false), false);
    IdleShift.Tick(1, false, Robot(0, false), false);
    TestTrue(TEXT("Normal100cm/s drain uses declared .24 movement coefficient"), Near(Normal.Get().BatteryPercent, 99.75));
    TestTrue(TEXT("New100cm/s boost equals120% of previous50cm/s total boost drain"), Near(Boost.Get().BatteryPercent, 99.664));
    TestTrue(TEXT("Motion without actual boost motor use has no extra burn"), Near(Coasting.Get().BatteryPercent, Normal.Get().BatteryPercent));
    TestTrue(TEXT("Idle Shift incurs only unchanged idle drain"), Near(IdleShift.Get().BatteryPercent, 99.99));
    const double BeforePause = Boost.Get().BatteryPercent;
    Boost.Tick(30, true, Robot(100, true), false);
    TestEqual(TEXT("Pause freezes boosted battery drain"), Boost.Get().BatteryPercent, BeforePause);
    TestEqual(TEXT("Pause freezes promised delivery time"), Boost.Get().ElapsedSeconds, 1.0);

    FMAYADeliveryCore MovingLock, NotReady, Service, Loaded;
    Start(MovingLock, TEXT("moving_lock")); Start(NotReady, TEXT("not_ready"));
    Start(Service, TEXT("service")); Start(Loaded, TEXT("loaded"), 2);
    auto Moving = Robot(100, true); Moving.bDoorsClosedLatched = false;
    MovingLock.Tick(1, false, Moving, false);
    TestTrue(TEXT("Stationary lid-rate predicate cannot undercharge actual native boost motor use"), Near(MovingLock.Get().BatteryPercent, 99.664));
    auto UnknownDrive = Robot(100, true); UnknownDrive.bReady = false;
    NotReady.Tick(1, false, UnknownDrive, false);
    TestTrue(TEXT("Nonready robot cannot grant boost energy mode"), Near(NotReady.Get().BatteryPercent, 99.75));
    FMAYADeliveryServiceToken Token;
    TestTrue(TEXT("Loading fixture admitted normally"), Service.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Token));
    Service.Tick(1, false, Robot(100, true), false);
    TestTrue(TEXT("Drive-locked service cannot grant boost energy mode"), Near(Service.Get().BatteryPercent, 99.75));
    TestTrue(TEXT("Loaded fixture begins normal pickup transaction"), Loaded.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(), Token));
    TestTrue(TEXT("Controlled custody evidence commits2kg"), Loaded.CommitService(Pickup(Token, 2)));
    auto OneHeavyParcel = Robot(100, true); OneHeavyParcel.bSecuredParcelCountKnown = true; OneHeavyParcel.SecuredParcelCount = 1;
    Loaded.Tick(1, false, OneHeavyParcel, false);
    TestTrue(TEXT("A single two-kg parcel does not trigger the two-parcel energy penalty"), Near(Loaded.Get().BatteryPercent, 99.664));
    TestEqual(TEXT("No partial energy fixture creates a delivery reward"), Loaded.Get().TotalCredits, 0);
    const double BeforeFailure = Loaded.Get().BatteryPercent;
    Loaded.Fail(TEXT("Explicit rule fixture failure"));
    Loaded.Tick(30, false, Robot(100, true), false);
    TestEqual(TEXT("Terminal result battery remains unchanged"), Loaded.Get().BatteryPercent, BeforeFailure);
    TestEqual(TEXT("Terminal fixture cannot earn credits"), Loaded.Get().TotalCredits, 0);

    FMAYADeliveryCore CorrespondingSpeed, FullStall, HalfStall, NearStall, FadeEnd, TwoStall, InvalidEffort;
    Start(CorrespondingSpeed, TEXT("corresponding_speed")); Start(FullStall, TEXT("full_stall"));
    Start(HalfStall, TEXT("half_stall")); Start(NearStall, TEXT("near_stall"));
    Start(FadeEnd, TEXT("fade_end")); Start(TwoStall, TEXT("two_stall")); Start(InvalidEffort, TEXT("invalid_effort"));
    CorrespondingSpeed.Tick(1, false, Robot(200, true), false);
    const double PreviousBoost100Drain = .01 + .24 * 2.25;
    TestTrue(TEXT("New200cm/s total drain is exactly120% of previous100cm/s boost cost"), Near(100 - CorrespondingSpeed.Get().BatteryPercent, PreviousBoost100Drain * 1.2));
    FullStall.Tick(1, false, Robot(0, true, 1), false);
    HalfStall.Tick(1, false, Robot(0, true, .5), false);
    NearStall.Tick(1, false, Robot(10, true, 1), false);
    FadeEnd.Tick(1, false, Robot(20, true, 1), false);
    TestTrue(TEXT("Powered full-torque stall consumes.66percent/s"), Near(FullStall.Get().BatteryPercent, 99.34));
    TestTrue(TEXT("Half-torque stall consumes.336percent/s"), Near(HalfStall.Get().BatteryPercent, 99.664));
    TestTrue(TEXT("Near-stall effort floor fades smoothly at10cm/s"), Near(NearStall.Get().BatteryPercent, 99.664));
    TestTrue(TEXT("Effort floor is gone at20cm/s, retaining normalized motion cost"), Near(FadeEnd.Get().BatteryPercent, 99.9232));
    auto TwoPowered = Robot(0, true, 1); TwoPowered.bSecuredParcelCountKnown = true; TwoPowered.SecuredParcelCount = 2;
    TwoStall.Tick(1, false, TwoPowered, false);
    TestTrue(TEXT("Two-parcel powered-stall matched endurance remains exactly70%"), Near((100 - FullStall.Get().BatteryPercent) / (100 - TwoStall.Get().BatteryPercent), .7));
    auto BadEffort = Robot(100, true); BadEffort.BoostMotorEffortFraction = std::numeric_limits<double>::quiet_NaN();
    InvalidEffort.Tick(1, false, BadEffort, false);
    TestTrue(TEXT("Nonfinite effort cannot admit boost power cost"), Near(InvalidEffort.Get().BatteryPercent, 99.75));
    const double StallBeforePause = FullStall.Get().BatteryPercent;
    FullStall.Tick(20, true, Robot(0, true, 1), false);
    TestEqual(TEXT("Pause freezes powered-stall drain too"), FullStall.Get().BatteryPercent, StallBeforePause);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryBoostMotorEnvelopeTest, "Maya.Delivery.BoostMotorEnvelope",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryBoostMotorEnvelopeTest::RunTest(const FString&)
{
    FDDSRobotPhysicsProfile Base;
    Base.MotorNoLoadRadps = 20; Base.MotorPeakNm = 5; Base.MotorErrorNmPerRadps = .5;
    Base.BrakePeakNm = 8; Base.BearingNmPerRadps = .02;
    const auto Normal = MAYADeliveryBoostProfile::Apply(Base, false);
    const auto Boost = MAYADeliveryBoostProfile::Apply(Base, true);
    const auto Previous = MAYADeliveryBoostProfile::Apply(Base, true, true);
    TestEqual(TEXT("Normal no-load wheel target remains20rad/s"), Normal.MotorNoLoadRadps, 20.0);
    TestEqual(TEXT("Normal native motor cap remains5Nm"), Normal.MotorPeakNm, 5.0);
    TestTrue(TEXT("Calibrated Boost no-load target is nominal42.4 rad/s"), FMath::IsNearlyEqual(Boost.MotorNoLoadRadps, 42.4, 1.e-9));
    TestEqual(TEXT("New boost native motor cap is12Nm"), Boost.MotorPeakNm, 12.0);
    TestEqual(TEXT("Active native brake cap remains8Nm"), Boost.BrakePeakNm, Normal.BrakePeakNm);
    TestEqual(TEXT("Passive bearing remains unchanged"), Boost.BearingNmPerRadps, Normal.BearingNmPerRadps);
    TestEqual(TEXT("Motor error gain remains unchanged"), Boost.MotorErrorNmPerRadps, Normal.MotorErrorNmPerRadps);
    TestEqual(TEXT("Previous comparator no-load target remains 24 rad/s"), Previous.MotorNoLoadRadps, 24.0);
    TestEqual(TEXT("Previous comparator motor cap remains 6 Nm"), Previous.MotorPeakNm, 6.0);
    TestEqual(TEXT("Previous brake cap remains 8 Nm"), Previous.BrakePeakNm, 8.0);
    TestEqual(TEXT("Previous bearing remains unchanged"), Previous.BearingNmPerRadps, Base.BearingNmPerRadps);
    TestEqual(TEXT("Previous motor error gain remains unchanged"), Previous.MotorErrorNmPerRadps, Base.MotorErrorNmPerRadps);
    TestEqual(TEXT("Applying Boost leaves the base no-load target unchanged"), Base.MotorNoLoadRadps, 20.0);
    TestEqual(TEXT("Applying Boost leaves the base peak unchanged"), Base.MotorPeakNm, 5.0);
    const auto NormalWithComparator = MAYADeliveryBoostProfile::Apply(Base, false, true);
    TestEqual(TEXT("Comparator selection cannot change normal no-load target"), NormalWithComparator.MotorNoLoadRadps, 20.0);
    TestEqual(TEXT("Comparator selection cannot change normal motor cap"), NormalWithComparator.MotorPeakNm, 5.0);
    TestEqual(TEXT("Energy road-speed target remains independently declared 2.0"), MAYADeliveryBoostProfile::EnergyRoadSpeedTargetRatioToPreviousBoost, 2.0);
    TestEqual(TEXT("Actual battery normalization retains that road target, not42.4/24"), FMAYADeliveryBalance().BoostSpeedNormalization, MAYADeliveryBoostProfile::EnergyRoadSpeedTargetRatioToPreviousBoost);
    TestTrue(TEXT("Calibrated motor no-load ratio is42.4/24, independently of road target"), MAYABoostRuleTests::Near(MAYADeliveryBoostProfile::MotorNoLoadRatioToPreviousBoost, 42.4 / 24.0));
    // The old exact 2x torque-curve property no longer applies after no-load
    // calibration. These checkpoints exercise the current production motor policy.
    for (const double Sign : {-1.0, 1.0})
    {
        TestEqual(TEXT("Both signs have zero motor torque at the actual selected nominal42.4 rad/s no-load target"), MAYADeliveryBoostProfile::EvaluateMotorTorque(Sign, Sign * Boost.MotorNoLoadRadps, Boost, true, false, true).AppliedNm, 0.0);
        TestTrue(TEXT("Both signs retain 12 Nm full-command stalled torque"), MAYABoostRuleTests::Near(MAYADeliveryBoostProfile::EvaluateMotorTorque(Sign, 0, Boost, true, false, true).AppliedNm, Sign * 12.0));
        TestTrue(TEXT("Both signs use calibrated7.42 Nm at35 percent stalled command"), MAYABoostRuleTests::Near(MAYADeliveryBoostProfile::EvaluateMotorTorque(Sign * .35, 0, Boost, true, false, true).AppliedNm, Sign * 7.42));
        TestTrue(TEXT("Both signs produce961/530 Nm at30 rad/s under full command"), MAYABoostRuleTests::Near(MAYADeliveryBoostProfile::EvaluateMotorTorque(Sign, Sign * 30.0, Boost, true, false, true).AppliedNm, Sign * (961.0 / 530.0)));
        TestTrue(TEXT("Original counterfactual has restoring torque 1 rad/s above target"), MAYABoostRuleTests::Near(DDSMotorTorqueNm(Sign, Sign * (Boost.MotorNoLoadRadps + 1.0), Boost), -Sign * .5));
        TestEqual(TEXT("Current Boost coasts 1 rad/s above full target"), MAYADeliveryBoostProfile::EvaluateMotorTorque(Sign, Sign * (Boost.MotorNoLoadRadps + 1.0), Boost, true, false, true).AppliedNm, 0.0);
        TestTrue(TEXT("Normal full-command stall remains 5 Nm across both signs"), MAYABoostRuleTests::Near(DDSMotorTorqueNm(Sign, 0, Normal), Sign * 5.0));
        TestTrue(TEXT("Previous full-command stall remains 6 Nm across both signs"), MAYABoostRuleTests::Near(DDSMotorTorqueNm(Sign, 0, Previous), Sign * 6.0));
        TestEqual(TEXT("Previous no-load target remains zero torque at 24 rad/s"), DDSMotorTorqueNm(Sign, Sign * 24.0, Previous), 0.0);
        TestEqual(TEXT("Normal no-load target remains zero torque at 20 rad/s"), DDSMotorTorqueNm(Sign, Sign * 20.0, Normal), 0.0);
        TestTrue(TEXT("Normal motor curve retains 2.5 Nm at 10 rad/s across both signs"), MAYABoostRuleTests::Near(DDSMotorTorqueNm(Sign, Sign * 10.0, Normal), Sign * 2.5));
        TestTrue(TEXT("Previous motor curve retains 3 Nm at 12 rad/s across both signs"), MAYABoostRuleTests::Near(DDSMotorTorqueNm(Sign, Sign * 12.0, Previous), Sign * 3.0));
    }
    TestEqual(TEXT("Zero request is passive coast even with boost envelope"), MAYADeliveryBoostProfile::EvaluateMotorTorque(0, 0, Boost, true, false, true).AppliedNm, 0.0);
    TestEqual(TEXT("Full forward boost stall applies capped12Nm in optical-forward axle sign"), MAYADeliveryBoostProfile::EvaluateMotorTorque(-1, 0, Boost, true, false, true).AppliedNm, -12.0);
    TestEqual(TEXT("Full reverse boost stall applies capped12Nm in opposite axle sign"), MAYADeliveryBoostProfile::EvaluateMotorTorque(1, 0, Boost, true, false, true).AppliedNm, 12.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryBoostOverspeedCoastTest, "Maya.Delivery.BoostOverspeedCoast",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryBoostOverspeedCoastTest::RunTest(const FString&)
{
    using namespace MAYADeliveryBoostProfile;
    using MAYABoostRuleTests::Near;
    FDDSRobotPhysicsProfile Base;
    Base.MotorNoLoadRadps=20; Base.MotorPeakNm=5; Base.MotorErrorNmPerRadps=.5;
    Base.BrakePeakNm=8; Base.BearingNmPerRadps=.02;
    const auto Boost=Apply(Base,true), Previous=Apply(Base,true,true);
    for(const double Sign : {-1.0,1.0})
    {
        const auto HalfOverspeed=EvaluateMotorTorque(Sign*.5,Sign*30,Boost,true,false,true);
        TestTrue(TEXT("Partial-bank overspeed records original opposite-request4.4 Nm"),Near(HalfOverspeed.OriginalLawNm,-Sign*4.4));
        TestTrue(TEXT("Only current Boost policy is eligible and suppresses partial-bank countertorque"),HalfOverspeed.bCurrentBoostPolicyEligible&&HalfOverspeed.bCounterTorqueSuppressed);
        TestEqual(TEXT("Partial-bank overspeed has zero policy and actual motor torque"),HalfOverspeed.AppliedNm,0.0);
        TestEqual(TEXT("Partial-bank suppressed policy output is zero"),HalfOverspeed.AfterPolicyNm,0.0);
        const auto Motoring=EvaluateMotorTorque(Sign*.5,Sign*10,Boost,true,false,true);
        TestTrue(TEXT("Partial motoring retains original1134/265 Nm curve"),Near(Motoring.AppliedNm,Sign*(1134.0/265.0))&&!Motoring.bCounterTorqueSuppressed);
        const auto Reverse=EvaluateMotorTorque(Sign,-Sign*10,Boost,true,false,true);
        TestTrue(TEXT("Intentional request reversal against existing spin retains requested-sign 12 Nm"),Near(Reverse.AppliedNm,Sign*12)&&!Reverse.bCounterTorqueSuppressed);
        const auto AtTarget=EvaluateMotorTorque(Sign*.5,Sign*(Boost.MotorNoLoadRadps*.5),Boost,true,false,true);
        TestEqual(TEXT("Zero original torque at partial target remains zero"),AtTarget.AppliedNm,0.0);
        TestFalse(TEXT("Zero original torque is not labelled suppression"),AtTarget.bCounterTorqueSuppressed);
        const auto Coast=EvaluateMotorTorque(0,Sign*60,Boost,true,false,true);
        TestEqual(TEXT("Zero request coasts despite existing spin"),Coast.AppliedNm,0.0); TestFalse(TEXT("Zero request is not labelled suppression"),Coast.bCounterTorqueSuppressed);
        TestEqual(TEXT("Clamped full overspeed request coasts beyond42.4"),EvaluateMotorTorque(Sign*1.25,Sign*(Boost.MotorNoLoadRadps+1.0),Boost,true,false,true).AppliedNm,0.0);
        TestTrue(TEXT("Normal overspeed retains original requested-opposite brake torque"),Near(EvaluateMotorTorque(Sign,Sign*25,Base,false,false,true).AppliedNm,-Sign*2.5));
        TestTrue(TEXT("Legacy overspeed retains original requested-opposite brake torque"),Near(EvaluateMotorTorque(Sign,Sign*30,Previous,true,true,true).AppliedNm,-Sign*3));
        for(const double Request : {0.0,.2,.5,1.25})for(const double Omega : {-60.0,-30.0,-12.0,0.0,10.0,30.0,60.0})
        {
            const auto Normal=EvaluateMotorTorque(Sign*Request,Sign*Omega,Base,false,false,true);
            const auto Legacy=EvaluateMotorTorque(Sign*Request,Sign*Omega,Previous,true,true,true);
            TestEqual(TEXT("Normal representative signed curve is exactly original DDS law"),Normal.AppliedNm,DDSMotorTorqueNm(Sign*Request,Sign*Omega,Base));
            TestEqual(TEXT("Legacy representative signed curve is exactly original DDS law"),Legacy.AppliedNm,DDSMotorTorqueNm(Sign*Request,Sign*Omega,Previous));
            TestFalse(TEXT("Normal policy never suppresses"),Normal.bCurrentBoostPolicyEligible||Normal.bCounterTorqueSuppressed);
            TestFalse(TEXT("Legacy policy never suppresses"),Legacy.bCurrentBoostPolicyEligible||Legacy.bCounterTorqueSuppressed);
        }
    }
    // Exact native173 new-forward frame2152, wheel1; retained decision operands.
    auto Historical44 = Boost; Historical44.MotorNoLoadRadps = 44.0;
    const auto Captured=EvaluateMotorTorque(-.4094421863555908,-27.83315686197071,Historical44,true,false,true);
    TestTrue(TEXT("Captured native173 original 4.908850331 Nm is independently reproduced"),Near(Captured.OriginalLawNm,4.908850331162357));
    TestTrue(TEXT("Captured wrong-request-direction servo torque now coasts"),Captured.bCounterTorqueSuppressed&&Captured.AppliedNm==0);
    const auto CurrentCaptured=EvaluateMotorTorque(-.4094421863555908,-27.83315686197071,Boost,true,false,true);
    TestTrue(TEXT("Current42.4 law also coasts for the captured signed overspeed operands"),CurrentCaptured.bCounterTorqueSuppressed&&CurrentCaptured.AppliedNm==0);
    const double NaN=std::numeric_limits<double>::quiet_NaN(),Infinity=std::numeric_limits<double>::infinity();
    TestEqual(TEXT("Nonfinite request retains original finite zero result"),EvaluateMotorTorque(NaN,0,Boost,true,false,true).AppliedNm,0.0);
    TestEqual(TEXT("Nonfinite omega retains original finite zero result"),EvaluateMotorTorque(1,Infinity,Boost,true,false,true).AppliedNm,0.0);
    for(int32 Guard=0;Guard<5;++Guard)
    {
        const bool Admitted=MAYALatchedDoorDrive::MotorDriveAdmitted(Guard!=0,Guard==1,Guard==2,Guard==3,Guard==4);
        const auto Refused=EvaluateMotorTorque(-1,0,Apply(Base,Admitted),Admitted,false,Admitted);
        TestFalse(TEXT("Each original freshness/brake/door/delivery/release guard denies drive"),Admitted);
        TestEqual(TEXT("Shared actual motor decision cannot bypass admission"),Refused.AppliedNm,0.0);
    }
    TestEqual(TEXT("Native brake plus passive bearing law is unchanged"),DDSBrakeBearingTorqueNm(true,10,Boost),DDSBrakeBearingTorqueNm(true,10,Base));
    TestEqual(TEXT("Passive bearing law is unchanged"),DDSBrakeBearingTorqueNm(false,10,Boost),-.2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryBoostOverspeedCoastEnergyTest, "Maya.Delivery.BoostOverspeedCoastEnergy",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryBoostOverspeedCoastEnergyTest::RunTest(const FString&)
{
    using namespace MAYADeliveryBoostProfile;
    using namespace MAYABoostRuleTests;
    FDDSRobotPhysicsProfile Base; Base.MotorNoLoadRadps=20; Base.MotorPeakNm=5; Base.MotorErrorNmPerRadps=.5;
    const auto Boost=Apply(Base,true); double Motors[6]={};
    for(double& Nm : Motors)Nm=EvaluateMotorTorque(-.5,-30,Boost,true,false,true).AppliedNm;
    const bool CoastActive=HasActualMotorTorque(Motors);
    const double CoastEffort=ActualMotorEffortFraction(Motors,Boost.MotorPeakNm,CoastActive);
    TestFalse(TEXT("All six suppressed motors have inactive actual powered Boost"),CoastActive);
    TestEqual(TEXT("All six suppressed motors have zero actual normalized effort"),CoastEffort,0.0);
    FMAYADeliveryCore Coasting; TestTrue(TEXT("Coast energy job admitted"),Start(Coasting,TEXT("policy_coast_energy")));
    Coasting.Tick(1,false,Robot(100,CoastActive,CoastEffort),false);
    TestTrue(TEXT("All-six coast pays ordinary idle plus actual movement, with no fake active-Boost burn"),Near(Coasting.Get().BatteryPercent,99.75));
    Motors[0]=EvaluateMotorTorque(-1,0,Boost,true,false,true).AppliedNm;
    const bool PartialActive=HasActualMotorTorque(Motors);
    const double PartialEffort=ActualMotorEffortFraction(Motors,Boost.MotorPeakNm,PartialActive);
    TestTrue(TEXT("One actual stalled motor restores powered Boost activity"),PartialActive);
    TestTrue(TEXT("One 12 Nm motor among six uses actual one-sixth effort"),Near(PartialEffort,1.0/6.0));
    FMAYADeliveryCore PartialStall; TestTrue(TEXT("Partial stall energy job admitted"),Start(PartialStall,TEXT("policy_partial_stall")));
    PartialStall.Tick(1,false,Robot(0,PartialActive,PartialEffort),false);
    TestTrue(TEXT("One actual stalled motor pays shared production 0.12 percent per second"),Near(PartialStall.Get().BatteryPercent,99.88));
    for(double& Nm : Motors)Nm=EvaluateMotorTorque(-1,0,Boost,true,false,true).AppliedNm;
    const double FullEffort=ActualMotorEffortFraction(Motors,Boost.MotorPeakNm,HasActualMotorTorque(Motors));
    FMAYADeliveryCore FullStall; Start(FullStall,TEXT("policy_full_stall"));FullStall.Tick(1,false,Robot(0,true,FullEffort),false);
    TestEqual(TEXT("All six actual stall motors retain unit effort"),FullEffort,1.0);
    TestTrue(TEXT("Actual full powered stall retains 0.66 percent per second"),Near(FullStall.Get().BatteryPercent,99.34));
    TestEqual(TEXT("Energy road-speed target remains fixed2.0"),FMAYADeliveryBalance().BoostSpeedNormalization,2.0);
    Motors[0]=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Nonfinite final applied contribution cannot grant activity"),HasActualMotorTorque(Motors));
    TestEqual(TEXT("Inactive classification gives zero effort despite rejected nonfinite motor"),ActualMotorEffortFraction(Motors,Boost.MotorPeakNm,false),0.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliverySucceededDriveEnergyTest, "Maya.Delivery.SucceededDriveEnergy",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliverySucceededDriveEnergyTest::RunTest(const FString&)
{
    FMAYADeliveryCore Core;
    TestTrue(TEXT("Free-roam energy fixture earns an ordinary completed delivery"), MAYAEconomyFixture::Complete(Core, TEXT("completed_energy")));
    const auto FrozenResult = Core.Get().Result;
    const int32 Cash = Core.Get().TotalCredits, Completed = Core.Get().CompletedJobs;
    const double CompletedElapsed = Core.Get().ElapsedSeconds, CompletedTravel = Core.Get().TravelCm;
    double Before = Core.Get().BatteryPercent;
    Core.Tick(1, false, MAYABoostRuleTests::Robot(), false);
    TestTrue(TEXT("Quiet Succeeded phase retains ordinary.01percent/s idle cost"), MAYABoostRuleTests::Near(Core.Get().BatteryPercent, Before - .01));
    Before = Core.Get().BatteryPercent;
    Core.Tick(1, false, MAYABoostRuleTests::Robot(100), false);
    TestTrue(TEXT("Actual motion after success pays ordinary.25percent/s"), MAYABoostRuleTests::Near(Core.Get().BatteryPercent, Before - .25));
    Before = Core.Get().BatteryPercent;
    Core.Tick(1, false, MAYABoostRuleTests::Robot(200, true), false);
    TestTrue(TEXT("Actual boosted motion after success pays.66percent/s"), MAYABoostRuleTests::Near(Core.Get().BatteryPercent, Before - .66));
    Before = Core.Get().BatteryPercent;
    Core.Tick(1, false, MAYABoostRuleTests::Robot(0, true, 1), false);
    TestTrue(TEXT("Powered boost stall after success also costs.66percent/s"), MAYABoostRuleTests::Near(Core.Get().BatteryPercent, Before - .66));
    Before = Core.Get().BatteryPercent;
    Core.Tick(20, true, MAYABoostRuleTests::Robot(200, true), false);
    TestEqual(TEXT("Pause freezes free-roam boost battery"), Core.Get().BatteryPercent, Before);
    Core.Tick(1000, false, MAYABoostRuleTests::Robot(200, true), false);
    TestEqual(TEXT("Post-delivery driving depletes live battery at zero"), Core.Get().BatteryPercent, 0.0);
    TestTrue(TEXT("Depleted post-delivery robot uses the unchanged authority drive lock"), Core.IsDriveLocked());
    TestTrue(TEXT("Completed job remains Succeeded when live free-roam battery depletes"), Core.Get().Phase == EMAYADeliveryPhase::Succeeded);
    TestEqual(TEXT("No inactive job deadline is advanced"), Core.Get().ElapsedSeconds, CompletedElapsed);
    TestEqual(TEXT("No inactive job travel is advanced"), Core.Get().TravelCm, CompletedTravel);
    TestEqual(TEXT("No driving or depletion awards cash again"), Core.Get().TotalCredits, Cash);
    TestEqual(TEXT("No duplicate completion is added"), Core.Get().CompletedJobs, Completed);
    TestEqual(TEXT("Frozen result battery is retained independently of live battery"), Core.Get().Result.BatteryPercent, FrozenResult.BatteryPercent);
    TestEqual(TEXT("Frozen result elapsed time remains exact"), Core.Get().Result.ElapsedSeconds, FrozenResult.ElapsedSeconds);
    TestEqual(TEXT("Frozen result credits remain exact"), Core.Get().Result.Credits, FrozenResult.Credits);
    TestEqual(TEXT("Frozen result condition remains exact"), Core.Get().Result.CargoConditionPercent, FrozenResult.CargoConditionPercent);
    TestEqual(TEXT("Frozen result job identity remains exact"), Core.Get().Result.JobInstanceId, FrozenResult.JobInstanceId);
    TestEqual(TEXT("Frozen result reason remains exact"), Core.Get().Result.Reason, FrozenResult.Reason);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryChargingDeadlineTest, "Maya.Delivery.ChargingDeadline",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryChargingDeadlineTest::RunTest(const FString&)
{
    using namespace MAYAEconomyFixture;
    FMAYADeliveryCore Core; double Time = 10;
    TestTrue(TEXT("Charging funds come from completed rule delivery"), Complete(Core, TEXT("charge_earn"), 1, Time));
    Core.Offer(Job());
    for (int32 I = 0; I < 100; ++I)
    {
        Time += 1; auto R = Robot(Time); R.VelocityCmps = FVector(100, 0, 0);
        Core.Tick(1, false, R, Charging(Time, false));
    }
    TestTrue(TEXT("Deadline job accepts with earned funds"), Core.Offer(Job(1, 15)) && Core.Accept(TEXT("charge_clock")));
    Tick(Core, Time, 1, false);
    QualifyAndGain(Core, Time);
    TestTrue(TEXT("Ten-second automatic dwell precedes charging"), Core.Get().bCharging);
    TestEqual(TEXT("First actual gain debits2 from earned10"), Core.Get().TotalCredits, 8);
    const double Before = Core.Get().BatteryPercent;
    Tick(Core, Time, .02, true);
    TestTrue(TEXT("Paid charge adds5percent per second"), FMath::IsNearlyEqual(Core.Get().BatteryPercent, Before + .1, 1.e-8));
    const double PausedBattery = Core.Get().BatteryPercent, PausedTime = Core.Get().ElapsedSeconds;
    Core.Tick(30, true, Robot(Time), Charging(Time));
    TestEqual(TEXT("Pause excludes charging progress"), Core.Get().BatteryPercent, PausedBattery);
    TestEqual(TEXT("Pause excludes delivery time"), Core.Get().ElapsedSeconds, PausedTime);
    TestTrue(TEXT("Actual qualification/charging time continues deadline"), PausedTime > 11);
    const double UntilExpired = 15 - Core.Get().ElapsedSeconds + .01;
    Tick(Core, Time, UntilExpired, true);
    TestTrue(TEXT("Charging cannot postpone rules deadline"), Core.Get().Phase == EMAYADeliveryPhase::Failed);
    TestFalse(TEXT("Deadline failure stops charging"), Core.Get().bCharging);
    TestTrue(TEXT("Failure consumes paid visit instead of free restart"), Core.Get().ChargeVisit.bConsumed);
    TestTrue(TEXT("Expiry identifies deadline"), Core.Get().Result.Reason.Contains(TEXT("deadline")));
    TestEqual(TEXT("Failure earns0 while retaining actual net cash"), Core.Get().TotalCredits, 8);
    TestEqual(TEXT("Failure frozen earnings are0"), Core.Get().Result.Credits, 0);
    return true;
}

// Captured game162 values seed controlled predicates, not a simultaneous solver
// reconstruction. The production ObserveDoors and torque path call these helpers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYALatchedMovingDriveRegression, "Maya.Delivery.LatchedMovingDriveRegression",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYALatchedMovingDriveRegression::RunTest(const FString&)
{
    using namespace MAYALatchedDoorDrive;
    const double FullAngles[2] = {.00002049245048305619, .0016478823955305352};
    const double CachedRates[2] = {-.22383701312157975, .018655222380719056};
    const bool Pose = HasClosedPose(true, FullAngles[0], CachedRates[0]) && HasClosedPose(true, FullAngles[1], CachedRates[1]);
    const bool Settled = HasSettledClosure(true, FullAngles[0], CachedRates[0]) && HasSettledClosure(true, FullAngles[1], CachedRates[1]);
    TestTrue(TEXT("Captured full closed poses and finite moving rates remain eligible"), Pose);
    TestFalse(TEXT("Captured lid0 rate remains ineligible for closing confirmation"), Settled);
    const bool Inhibited = DoorDriveInhibited(false, true, Pose, true);
    TestFalse(TEXT("Already latched actual closed locks do not brake for rate alone"), Inhibited);
    FDDSRobotPhysicsProfile Base; Base.MotorNoLoadRadps = 20; Base.MotorPeakNm = 5; Base.MotorErrorNmPerRadps = .5;
    for (int32 Profile = 0; Profile < 3; ++Profile)
    {
        const auto P = MAYADeliveryBoostProfile::Apply(Base, Profile != 0, Profile == 1);
        const bool Drive = MotorDriveAdmitted(true, false, Inhibited, false, false);
        TestTrue(TEXT("Normal, previous Boost and new Boost share repaired motor admission"), Drive);
        TestTrue(TEXT("Each admitted envelope retains ordinary finite applied motor torque"), Drive && FMath::Abs(DDSMotorTorqueNm(-1, 0, P)) > 0);
    }
    FMAYADeliveryCore Core; FMAYADeliveryServiceToken Token;
    TestTrue(TEXT("Captured closure fixture starts an ordinary accepted job"), MAYABoostRuleTests::Start(Core, TEXT("latched_moving_service")));
    auto R = MAYABoostRuleTests::Robot(); R.bDoorsClosedLatched = Settled;
    TestFalse(TEXT("Moving-rate closure cannot admit stationary pickup service"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, R, Token));
    R.bDoorsClosedLatched = HasSettledClosure(true, FullAngles[0], 0) && HasSettledClosure(true, FullAngles[1], 0);
    TestTrue(TEXT("Actually settled closure retains ordinary service admission"), Core.BeginService(TEXT("depot"), FVector::ZeroVector, R, Token));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYALatchedClosureBoundaryRegression, "Maya.Delivery.LatchedClosureBoundaryRegression",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYALatchedClosureBoundaryRegression::RunTest(const FString&)
{
    using namespace MAYALatchedDoorDrive;
    const double Nan = std::numeric_limits<double>::quiet_NaN(), Inf = std::numeric_limits<double>::infinity();
    TestTrue(TEXT("Exact full one-degree pose boundary remains accepted"), HasClosedPose(true, ClosedAngleRad, .223837));
    TestFalse(TEXT("Full 3D connector angle beyond one degree remains rejected, regardless twist"), HasClosedPose(true, ClosedAngleRad + 1.e-8, 0));
    TestTrue(TEXT("Both exact settled rate boundaries remain inclusive"), HasSettledClosure(true, ClosedAngleRad, .1) && HasSettledClosure(true, ClosedAngleRad, -.1));
    TestFalse(TEXT("Rate above original closure boundary cannot confirm closing"), HasSettledClosure(true, 0, .1 + 1.e-8));
    TestFalse(TEXT("Unknown physical state cannot grant moving closed pose"), HasClosedPose(false, 0, 0));
    TestFalse(TEXT("Nonfinite full angle cannot grant moving closed pose"), HasClosedPose(true, Nan, 0) || HasClosedPose(true, Inf, 0));
    TestFalse(TEXT("Nonfinite relative rate cannot grant moving closed pose"), HasClosedPose(true, 0, Nan) || HasClosedPose(true, 0, Inf));
    TestFalse(TEXT("Invalid negative angular-distance evidence cannot grant moving pose"), HasClosedPose(true, -.01, 0));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYALatchedDriveGuardRegression, "Maya.Delivery.LatchedDriveGuardRegression",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYALatchedDriveGuardRegression::RunTest(const FString&)
{
    using namespace MAYALatchedDoorDrive;
    TestTrue(TEXT("Reset stabilization still inhibits latched motors"), DoorDriveInhibited(true, true, true, true));
    TestTrue(TEXT("Opening/open/closing/fault phase cannot use latched moving admission"), DoorDriveInhibited(false, false, true, true));
    TestTrue(TEXT("Unknown, nonfinite or excessive full pose still inhibits motors"), DoorDriveInhibited(false, true, false, true));
    TestTrue(TEXT("Missing, broken or unmatched native locks still inhibit motors"), DoorDriveInhibited(false, true, true, false));
    TestFalse(TEXT("Stale or absent command cannot apply motor torque"), MotorDriveAdmitted(false, false, false, false, false));
    TestFalse(TEXT("Requested brake retains priority"), MotorDriveAdmitted(true, true, false, false, false));
    TestFalse(TEXT("Door inhibition retains motor and brake boundary"), MotorDriveAdmitted(true, false, true, false, false));
    TestFalse(TEXT("Delivery authority lock retains motor and brake boundary"), MotorDriveAdmitted(true, false, false, true, false));
    TestFalse(TEXT("Input release interlock retains motor and brake boundary"), MotorDriveAdmitted(true, false, false, false, true));
    FMAYADeliveryCore Core;
    TestTrue(TEXT("Real completed rule job funds free-roam depletion fixture"), MAYAEconomyFixture::Complete(Core, TEXT("latched_depletion")));
    Core.Tick(1000, false, MAYABoostRuleTests::Robot(200, true, 1), false);
    TestEqual(TEXT("Ordinary production Core drain reaches zero"), Core.Get().BatteryPercent, 0.0);
    TestTrue(TEXT("Actual Core depletion continues to own delivery drive lock"), Core.IsDriveLocked());
    TestFalse(TEXT("Repaired closed latch cannot bypass depleted Core motor lock"), MotorDriveAdmitted(true, false, DoorDriveInhibited(false, true, true, true), Core.IsDriveLocked(), false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYADeliveryBoostLowSpeedReserveTest, "Maya.Delivery.BoostLowSpeedReserve",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYADeliveryBoostLowSpeedReserveTest::RunTest(const FString&)
{
    using namespace MAYADeliveryBoostProfile;
    using MAYABoostRuleTests::Near;
    FDDSRobotPhysicsProfile Base;
    Base.MotorNoLoadRadps=20;Base.MotorPeakNm=5;Base.MotorErrorNmPerRadps=.5;
    Base.BrakePeakNm=8;Base.BearingNmPerRadps=.02;
    const auto Road=Apply(Base,true), Previous=Apply(Base,true,true);
    const auto Rest=ApplyAtChassisSpeed(Base,true,false,true,0);
    const auto Half=ApplyAtChassisSpeed(Base,true,false,true,25);
    TestEqual(TEXT("Rest actual current Boost cap is18Nm"),Rest.MotorPeakNm,18.0);
    TestEqual(TEXT("25cm/s actual current Boost cap is15Nm"),Half.MotorPeakNm,15.0);
    TestTrue(TEXT("Low-speed selection keeps nominal42.4rad/s"),FMath::IsNearlyEqual(Rest.MotorNoLoadRadps,42.4,1.e-9));
    TestEqual(TEXT("Low-speed selection keeps error gain.5"),Rest.MotorErrorNmPerRadps,.5);
    TestEqual(TEXT("Low-speed selection keeps native brake8Nm"),Rest.BrakePeakNm,8.0);
    TestEqual(TEXT("Low-speed selection keeps passive bearing"),Rest.BearingNmPerRadps,Base.BearingNmPerRadps);
    for(const double Speed:{50.0,100.0})
    {
        const auto Selected=ApplyAtChassisSpeed(Base,true,false,true,Speed);
        TestEqual(TEXT("At/above50cm/s the current road cap is exactly12Nm"),Selected.MotorPeakNm,Road.MotorPeakNm);
        for(const double Request:{-1.25,-1.0,-.35,0.0,.35,1.0,1.25})for(const double Omega:{-60.0,-43.0,-30.0,0.0,30.0,43.0,60.0})
            TestEqual(TEXT("Road-speed selected motor outputs exactly retain current42.4/12 law"),EvaluateMotorTorque(Request,Omega,Selected,true,false,true).AppliedNm,EvaluateMotorTorque(Request,Omega,Road,true,false,true).AppliedNm);
    }
    for(const double BadSpeed:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        TestEqual(TEXT("Invalid speed grants no reserve"),ApplyAtChassisSpeed(Base,true,false,true,BadSpeed).MotorPeakNm,12.0);
    TestEqual(TEXT("Unavailable native speed grants no reserve"),ApplyAtChassisSpeed(Base,true,false,false,0).MotorPeakNm,12.0);
    for(const double Sign:{-1.0,1.0})
    {
        TestTrue(TEXT("Both drive signs can apply18Nm at zero omega/rest"),Near(EvaluateMotorTorque(Sign,0,Rest,true,false,true).AppliedNm,Sign*18));
        TestTrue(TEXT("Both signs apply15Nm at25cm/s and zero omega"),Near(EvaluateMotorTorque(Sign,0,Half,true,false,true).AppliedNm,Sign*15));
        TestTrue(TEXT("Partial request still obeys error gain:7.42Nm below reserve cap"),Near(EvaluateMotorTorque(Sign*.35,0,Rest,true,false,true).AppliedNm,Sign*7.42));
        TestEqual(TEXT("Low-speed full overspeed still coasts"),EvaluateMotorTorque(Sign,Sign*44,Rest,true,false,true).AppliedNm,0.0);
        TestEqual(TEXT("Low-speed partial overspeed still coasts"),EvaluateMotorTorque(Sign*.5,Sign*30,Rest,true,false,true).AppliedNm,0.0);
        TestEqual(TEXT("Zero request remains coast"),EvaluateMotorTorque(0,Sign*60,Rest,true,false,true).AppliedNm,0.0);
        TestEqual(TEXT("Reserve cannot bypass rejected brake/release/lock drive admission"),EvaluateMotorTorque(Sign,0,Rest,true,false,false).AppliedNm,0.0);
        TestEqual(TEXT("Normal curve remains original at a sampled zero chassis speed"),EvaluateMotorTorque(Sign,Sign*10,ApplyAtChassisSpeed(Base,false,false,true,0),false,false,true).AppliedNm,DDSMotorTorqueNm(Sign,Sign*10,Base));
        TestEqual(TEXT("Previous comparator remains original at a sampled zero chassis speed"),EvaluateMotorTorque(Sign,Sign*12,ApplyAtChassisSpeed(Base,true,true,true,0),true,true,true).AppliedNm,DDSMotorTorqueNm(Sign,Sign*12,Previous));
    }
    double Motors[6]={18,9,3,0,0,0};
    TestTrue(TEXT("Final effort normalizes by same actual18Nm cap, not static12"),Near(ActualMotorEffortFraction(Motors,Rest.MotorPeakNm,HasActualMotorTorque(Motors)),30.0/108.0));
    for(double& Nm:Motors)Nm=18;
    const double Effort=ActualMotorEffortFraction(Motors,Rest.MotorPeakNm,HasActualMotorTorque(Motors));
    TestEqual(TEXT("Six actual selected-cap motors have unit effort"),Effort,1.0);
    FMAYADeliveryCore Stall;Stall.Tick(1,false,MAYABoostRuleTests::Robot(0,true,Effort),false);
    TestTrue(TEXT("Core current law remains.66pp/s for actual full powered stall"),Near(Stall.Get().BatteryPercent,99.34));
    for(double& Nm:Motors)Nm=0;
    TestFalse(TEXT("All-six reserve-profile coast is inactive powered Boost"),HasActualMotorTorque(Motors));
    TestEqual(TEXT("All-six coast has zero actual effort"),ActualMotorEffortFraction(Motors,Rest.MotorPeakNm,false),0.0);
    TestEqual(TEXT("Nonpositive actual cached peak cannot normalize power"),ActualMotorEffortFraction(Motors,0,true),0.0);
    TestEqual(TEXT("Nonfinite actual cached peak cannot normalize power"),ActualMotorEffortFraction(Motors,std::numeric_limits<double>::quiet_NaN(),true),0.0);
    return true;
}

#endif
