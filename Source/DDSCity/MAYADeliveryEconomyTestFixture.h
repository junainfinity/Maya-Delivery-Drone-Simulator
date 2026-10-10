#pragma once
#include "MAYADeliveryCore.h"

#if WITH_DEV_AUTOMATION_TESTS
// Controlled GAME-rule observations only. These helpers neither operate native
// bodies nor certify service physics. Cash is earned through ordinary commits;
// battery is spent through ordinary ticks. No wallet/energy setter exists here.
namespace MAYAEconomyFixture
{
    inline FMAYADeliveryJob Job(int32 Count = 1, double Deadline = 240)
    {
        FMAYADeliveryJob J;
        J.ProfileId = TEXT("economy_rule_fixture"); J.Title = TEXT("Controlled cash rules");
        J.PickupStationId = TEXT("depot"); J.DropoffStationId = TEXT("recipient");
        J.ParcelCount = Count; J.PayloadKg = Count; J.DeadlineSeconds = Deadline;
        return J;
    }
    inline FMAYADeliveryRobotObservation Robot(double Time, int32 Count = 0)
    {
        FMAYADeliveryRobotObservation R;
        R.bKnown = R.bReady = R.bGrounded = R.bUpright = R.bDoorsClosedLatched = true;
        R.SupportMask = 63; R.ObservationWorldSeconds = Time; R.ResetGeneration = 1;
        R.bSecuredParcelCountKnown = true; R.SecuredParcelCount = Count;
        return R;
    }
    inline FMAYADeliveryServiceEvidence Evidence(const FMAYADeliveryServiceToken& T, double Time, int32 Count)
    {
        FMAYADeliveryServiceEvidence E;
        E.Token = T; E.Robot = Robot(Time, Count); E.ObservedParcelId = T.JobInstanceId;
        E.ObservedParcelIds.Add(T.JobInstanceId);
        if (Count == 2) E.ObservedParcelIds.Add(T.JobInstanceId + TEXT("/parcel_2"));
        E.ObservedParcelCount = Count; E.ObservedParcelKg = Count;
        E.bAuthorizedInstantTransferObserved = E.bActualPlacementEndpointsClear = true;
        E.bOpenEndpointsObserved = E.bParcelAtCorrectDestination = E.bActualCustodyObserved = E.bParcelMassObserved = true;
        return E;
    }
    inline bool Complete(FMAYADeliveryCore& Core, const FString& Id, int32 Count = 1, double Time = 10)
    {
        FMAYADeliveryServiceToken T;
        return Core.Offer(Job(Count)) && Core.Accept(Id)
            && Core.BeginService(TEXT("depot"), FVector::ZeroVector, Robot(Time), T)
            && Core.CommitService(Evidence(T, Time, Count))
            && Core.BeginService(TEXT("recipient"), FVector::ZeroVector, Robot(Time, Count), T)
            && Core.CommitService(Evidence(T, Time, Count));
    }
    inline FMAYADeliveryChargingObservation Charging(double Time, bool bInside = true)
    {
        FMAYADeliveryChargingObservation C;
        C.WorldSeconds = Time; C.bInside = bInside;
        if (bInside) C.StationId = TEXT("controlled_remote_charger");
        return C;
    }
    inline void Tick(FMAYADeliveryCore& Core, double& Time, double Dt, bool bInside = true, int32 Count = 0)
    {
        Time += Dt; Core.Tick(Dt, false, Robot(Time, Count), Charging(Time, bInside));
    }
    inline bool EarnAndPrepare(FMAYADeliveryCore& Core, double& Time)
    {
        if (!Complete(Core, TEXT("earned_initial_ten"), 1, Time) || !Core.Offer(Job())) return false;
        Tick(Core, Time, 1, false); // ordinary partial depletion; no resource injection
        return Core.Get().TotalCredits == 10 && Core.Get().BatteryPercent < 100;
    }
    inline void QualifyAndGain(FMAYADeliveryCore& Core, double& Time)
    {
        Tick(Core, Time, .001, true);
        for (int32 I = 0; I < 10; ++I) Tick(Core, Time, 1, true);
        Tick(Core, Time, .01, true);
    }
}
#endif
