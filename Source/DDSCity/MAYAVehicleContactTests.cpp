#include "MAYADeliveryCore.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace
{
    FMAYAVehicleContactEvent Incoming(bool Rear = false)
    {
        FMAYAVehicleContactEvent E; E.Generation = 1; E.WorldSeconds = 1; E.LatestSampleWorldSeconds = .99;
        E.bNativeContact = E.bBlockingHit = E.bVehicleIdentityKnown = E.bSameNativeBody = E.bHistoryKnown = true;
        E.VehicleActor = TEXT("Controlled original car"); E.VehicleComponent = TEXT("NativeVehicleMesh"); E.RobotComponent = TEXT("NativeBase");
        E.RobotOpticalForward = FVector(-1,0,0); E.CoherentSamples = 12; E.CoherentSpanSeconds = .2;
        const double Sign = Rear ? -1 : 1;
        E.CarCOMCm = FVector(-100*Sign,0,0); E.Normal = FVector(Sign,0,0);
        E.ContactPointCm = FVector(-30*Sign,0,0); E.CarVelocityCmps = FVector(Sign,0,0);
        E.CoherentCarProgressCm = FVector(.2*Sign,0,0); // real1cm/s crawl, no fatal speed floor
        return E;
    }
    FMAYADeliveryJob Job()
    { FMAYADeliveryJob J; J.ProfileId=TEXT("fixture");J.Title=TEXT("Rule fixture");J.PickupStationId=TEXT("depot");J.DropoffStationId=TEXT("dropoff");return J; }
    FMAYADeliveryRobotObservation Robot()
    { FMAYADeliveryRobotObservation R;R.bKnown=R.bReady=R.bGrounded=R.bUpright=R.bDoorsClosedLatched=true;R.ObservationWorldSeconds=1;R.ResetGeneration=1;return R; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAVehicleContactClassificationTest,"Maya.Delivery.VehicleContactClassification",
    EAutomationTestFlags::ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FMAYAVehicleContactClassificationTest::RunTest(const FString&)
{
    TestTrue(TEXT("Actual coherent slow head-on strike qualifies without impulse threshold"),Incoming().IsFatal());
    TestTrue(TEXT("Actual coherent slow rear-end strike qualifies"),Incoming(true).IsFatal());
    auto E=Incoming();E.CoherentCarProgressCm=FVector(.000837,0,0);E.CarVelocityCmps=FVector(.008131,0,0);
    TestFalse(TEXT("Measured parked game96 noise cannot total drone"),E.IsFatal());
    E=Incoming();E.CarVelocityCmps=FVector::ZeroVector;
    TestFalse(TEXT("Approach then observed stop before drone bump invalidates older progress"),E.IsFatal());
    E=Incoming();E.CarVelocityCmps=FVector(-30,0,0);E.RobotVelocityCmps=FVector(-100,0,0);E.CoherentCarProgressCm=FVector(-6,0,0);
    TestFalse(TEXT("Faster drone rear bump into departing car survives"),E.IsFatal());
    E=Incoming();E.Normal=FVector(0,1,0);E.ContactPointCm=FVector(0,-20,0);
    TestFalse(TEXT("Side contact remains ordinary damage"),E.IsFatal());
    E=Incoming();E.bBlockingHit=false;TestFalse(TEXT("Nonblocking receipt cannot total"),E.IsFatal());
    E=Incoming();E.bStartPenetrating=true;TestFalse(TEXT("Initial overlap is not a proved incoming strike"),E.IsFatal());
    E=Incoming();E.bEpisodeContinuation=true;E.CoherentCarProgressCm=FVector(.000837,0,0);
    TestFalse(TEXT("Later geometry cannot promote frozen parked motion"),E.IsFatal());
    E=Incoming();E.bEpisodeContinuation=true;
    TestTrue(TEXT("Later longitudinal geometry can use fresh frozen incoming history"),E.IsFatal());
    E=Incoming();E.LatestSampleWorldSeconds=.7;TestFalse(TEXT("Stale prior car movement is rejected"),E.IsFatal());
    E=Incoming();E.CarCOMCm=FVector(100,0,0); // glancing normal points opposite center direction
    TestFalse(TEXT("Contact normal cannot fabricate car-to-drone approach"),E.IsFatal());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAVehicleTotalledAuthorityTest,"Maya.Delivery.VehicleTotalledAuthority",
    EAutomationTestFlags::ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FMAYAVehicleTotalledAuthorityTest::RunTest(const FString&)
{
    FMAYADeliveryCore C;C.Offer(Job());C.Accept(TEXT("actual_job_fixture"));
    FMAYADeliveryServiceToken Token;TestTrue(TEXT("Service initially admitted"),C.BeginService(TEXT("depot"),FVector::ZeroVector,Robot(),Token));
    const auto Before=C.Get(); C.SetVehicleContactAdjudicationPending(true);
    TestTrue(TEXT("Pending identity locks motors"),C.IsDriveLocked());
    TestFalse(TEXT("Pending identity prevents accept/replay authority"),C.Offer(Job()));
    C.Tick(.1,false,Robot(),true);
    TestEqual(TEXT("Pending identity still advances real deadline"),C.Get().ElapsedSeconds,.1);
    TestEqual(TEXT("Pending identity never grants energy"),C.Get().BatteryPercent,Before.BatteryPercent);
    TestEqual(TEXT("Pending identity never charges fee"),C.Get().RechargeFeesPaidDollars,0);
    C.SetVehicleContactAdjudicationPending(false);
    TestTrue(TEXT("Actual typed incoming strike totals shift"),C.ApplyVehicleContact(Incoming()));
    TestTrue(TEXT("Totalled outcome is immutable"),C.Get().bDroneTotalled&&C.Get().VehicleCrash.bFrozen);
    const auto Crash=C.Get().VehicleCrash;
    auto Other=Incoming(true);Other.Generation=2;TestFalse(TEXT("Further hits cannot rewrite crash"),C.ApplyVehicleContact(Other));
    TestEqual(TEXT("Frozen initial native event remains"),C.Get().VehicleCrash.Contact.Generation,Crash.Contact.Generation);
    TestFalse(TEXT("Totalled robot cannot recover"),C.RecoverAfterNativeReset(Robot()));
    TestFalse(TEXT("Totalled robot cannot offer/replay"),C.Offer(Job()));
    TestFalse(TEXT("Totalled robot cannot accept"),C.Accept(TEXT("forbidden")));
    TestFalse(TEXT("Totalled robot cannot begin service"),C.BeginService(TEXT("depot"),FVector::ZeroVector,Robot(),Token));
    FMAYADeliveryServiceEvidence Late;Late.Token=Before.Service;
    TestFalse(TEXT("Precrash custody completion cannot reward"),C.CommitService(Late));
    TestTrue(TEXT("Totalled motors stay locked"),C.IsDriveLocked());
    C.Tick(30,false,Robot(),true);
    TestEqual(TEXT("Totalled shift cannot refill battery"),C.Get().BatteryPercent,Crash.BatteryPercent);
    TestEqual(TEXT("Totalled shift cannot change frozen cash"),C.Get().TotalCredits,Crash.CashDollars);
    TestEqual(TEXT("Totalled shift cannot mint a reward"),C.Get().CompletedJobs,Crash.CompletedJobs);
    FMAYADeliveryCore Fresh;
    TestEqual(TEXT("Fresh world Core starts with zero cash"),Fresh.Get().TotalCredits,0);
    TestEqual(TEXT("Fresh world Core starts with full battery"),Fresh.Get().BatteryPercent,100.0);
    TestFalse(TEXT("Fresh world is not totalled"),Fresh.Get().bDroneTotalled);
    TestNotEqual(TEXT("Fresh world owns a distinct shift"),Fresh.Get().ShiftInstanceId,Crash.ShiftInstanceId);
    FMAYADeliveryCore Deadline;auto Short=Job();Short.DeadlineSeconds=.05;
    Deadline.Offer(Short);Deadline.Accept(TEXT("pending_deadline"));Deadline.SetVehicleContactAdjudicationPending(true);
    Deadline.Tick(.1,false,Robot(),true);Deadline.SetVehicleContactAdjudicationPending(false);
    TestTrue(TEXT("Real deadline failure survives nonfatal identity resolution"),Deadline.Get().Phase==EMAYADeliveryPhase::Failed);
    TestEqual(TEXT("Pending deadline preserves failed-result identity"),Deadline.Get().Result.JobInstanceId,FString(TEXT("pending_deadline")));
    TestFalse(TEXT("Pending deadline result is not a success"),Deadline.Get().Result.bSuccess);
    TestEqual(TEXT("Pending deadline failure grants no cash"),Deadline.Get().TotalCredits,0);
    return true;
}
#endif
