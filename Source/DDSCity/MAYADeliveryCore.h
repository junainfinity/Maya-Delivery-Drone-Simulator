#pragma once

#include "MAYADeliveryTypes.h"

// Deterministic game-thread rules; Unreal actors supply measured observations.
// Physical transitions are never inferred from elapsed time or target angles.
class DDSCITY_API FMAYADeliveryCore final
{
public:
    explicit FMAYADeliveryCore(const FMAYADeliveryBalance& InBalance = FMAYADeliveryBalance());
#if !UE_BUILD_SHIPPING
    // One initial condition before the first offer; never a runtime setter.
    bool InitializeNativeOperatingBattery(double InitialPercent);
#endif
    bool PrepareShiftForStart(); // pristine ordinary bootstrap only; never a resource reset
    bool StartShift();
    bool Offer(const FMAYADeliveryJob& Job);
    bool Accept(const FString& UniqueJobInstanceId);
    bool BeginService(const FString& StationId, const FVector& ServicePoint,
        const FMAYADeliveryRobotObservation& Robot, FMAYADeliveryServiceToken& OutToken);
    bool CommitService(const FMAYADeliveryServiceEvidence& Evidence);
    bool CancelService(const FMAYADeliveryServiceToken& Token, const FString& Reason);
    bool ConfirmCancelledClosure(const FMAYADeliveryRobotObservation& Robot);
    void Tick(double DeltaSeconds, bool bPaused, const FMAYADeliveryRobotObservation& Robot,
        bool bAtCharger);
    void Tick(double DeltaSeconds, bool bPaused, const FMAYADeliveryRobotObservation& Robot,
        const FMAYADeliveryChargingObservation& Charging);
    // Legacy explicit connection cannot bypass automatic ring dwell/funds.
    // False may stop the current event; no player connect/C action exists.
    bool SetCharging(bool bRequested, const FMAYADeliveryRobotObservation& Robot, bool bAtCharger);
    void ApplyImpactDamage(double ConditionPoints); // controlled legacy rule fixture: bounded0.5/1 point event
    bool ApplyImpactEvent(const FMAYADeliveryImpactEvent& Event);
    bool ApplyVehicleContact(const FMAYAVehicleContactEvent& Event);
    void SetVehicleContactAdjudicationPending(bool bPending);
    void Fail(const FString& Reason);
    bool RecoverAfterNativeReset(const FMAYADeliveryRobotObservation& Robot);
    bool IsDriveLocked() const;
    bool IsActive() const;
    bool IsServiceCancellationPending() const { return bCancellationPending; }
    const FMAYADeliverySnapshot& Get() const { return State; }
    const FMAYADeliveryBalance& GetBalance() const { return Balance; }

private:
    bool ValidJob(const FMAYADeliveryJob& Job) const;
    bool TickChargingRing(double Dt, const FMAYADeliveryRobotObservation& Robot,
        const FMAYADeliveryChargingObservation& Charging);
    void StopChargingVisit(const FMAYADeliveryRobotObservation& Robot, bool bAtCharger,
        const FString& Reason, bool bAutomaticDisconnect);
    void FreezeResult(bool bSuccess, const FString& Reason);
    FMAYADeliveryBalance Balance;
    FMAYADeliverySnapshot State;
    TSet<FString> UsedInstanceIds;
    TSet<FString> RewardedInstanceIds;
    uint64 NextServiceGeneration = 1;
    uint64 NextChargeVisitGeneration = 0, NextRechargeGeneration = 0;
    double LastChargeObservationWorldSeconds = -1;
    double ServiceElapsed = 0.0;
    double MissingObservationSeconds = 0.0;
    bool bCancellationPending = false;
    TMap<FString, double> AppliedImpactCeilings;
    TMap<FString, uint64> LastObservedImpactGeneration; // persists across offers and resets
    double LastObservedWorldSeconds = -1;
    uint64 NextImpactEpisode = 0;
    double ImpactEpisodeWorldSeconds = -1, ImpactEpisodeConditionPoints = 0;
    double CargoSecuredWorldSeconds = -1;
#if !UE_BUILD_SHIPPING
    bool bNativeOperatingBatteryFixtureConsumed = false;
#endif
};
