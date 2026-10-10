#include "MAYADeliveryCore.h"
#include "Misc/Guid.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

namespace
{
    // Describe each failed input to the existing charge predicate. The result
    // is diagnostic only; Tick and SetCharging retain their original gates.
    TArray<FString> FailedChargeConditions(const FMAYADeliveryRobotObservation& R,
        bool bAtCharger, EMAYADeliveryPhase Phase)
    {
        TArray<FString> Failed;
        if (!bAtCharger) Failed.Add(TEXT("at_charger"));
        if (!R.bKnown) Failed.Add(TEXT("observation_known"));
        if (!R.bReady) Failed.Add(TEXT("robot_ready"));
        if (!R.bGrounded) Failed.Add(TEXT("six_wheel_support"));
        if (!R.bUpright) Failed.Add(TEXT("upright"));
        if (!R.Fault.IsEmpty()) Failed.Add(TEXT("fault_clear"));
        if (R.ChassisPose.ContainsNaN()) Failed.Add(TEXT("chassis_pose_finite"));
        if (R.VelocityCmps.ContainsNaN()) Failed.Add(TEXT("velocity_finite"));
        else if (R.VelocityCmps.Size() > 5.0) Failed.Add(TEXT("speed_at_most_5_cmps"));
        if (R.AngularVelocityRadps.ContainsNaN()) Failed.Add(TEXT("angular_velocity_finite"));
        else if (R.AngularVelocityRadps.Size() > 0.05) Failed.Add(TEXT("angular_speed_at_most_0_05_radps"));
        if (!FMath::IsFinite(R.ObservationWorldSeconds) || R.ObservationWorldSeconds < 0)
            Failed.Add(TEXT("observation_time_valid"));
        if (!R.bDoorsClosedLatched) Failed.Add(TEXT("doors_closed_latched"));
        if (Phase == EMAYADeliveryPhase::Loading || Phase == EMAYADeliveryPhase::Unloading)
            Failed.Add(TEXT("outside_loading_and_unloading"));
        return Failed;
    }
}

FMAYADeliveryCore::FMAYADeliveryCore(const FMAYADeliveryBalance& InBalance) : Balance(InBalance)
{
    State.ShiftInstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
}

bool FMAYADeliveryCore::PrepareShiftForStart()
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (!State.bShiftStarted || State.bShiftWon || State.Phase != EMAYADeliveryPhase::Available
        || State.BatteryPercent != 100 || State.TotalCredits != 0 || State.CompletedJobs != 0
        || State.RechargeFeesPaidDollars != 0 || State.ElapsedSeconds != 0 || State.TravelCm != 0
        || !State.JobInstanceId.IsEmpty() || !UsedInstanceIds.IsEmpty() || !RewardedInstanceIds.IsEmpty()
        || State.bCharging || bCancellationPending || NextServiceGeneration != 1) return false;
    State.bShiftStarted = false;
    State.Status = TEXT("Ready for a new shift: $0 and 100% battery."); return true;
}

bool FMAYADeliveryCore::StartShift()
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (State.bShiftStarted || State.bShiftWon || State.Phase != EMAYADeliveryPhase::Available
        || !ValidJob(State.Job) || State.BatteryPercent != 100 || State.TotalCredits != 0
        || State.CompletedJobs != 0 || State.bCharging || bCancellationPending) return false;
    State.bShiftStarted = true;
    State.Status = TEXT("Accept a delivery. Park in a green charging ring for ten seconds; recharge costs $2."); return true;
}

#if !UE_BUILD_SHIPPING
bool FMAYADeliveryCore::InitializeNativeOperatingBattery(double InitialPercent)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01VisualCapture"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CurbTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || !FMath::IsFinite(InitialPercent) || InitialPercent < 0.5 || InitialPercent > 1.0
        || bNativeOperatingBatteryFixtureConsumed || State.Phase != EMAYADeliveryPhase::Available
        || !State.Job.ProfileId.IsEmpty() || !State.JobInstanceId.IsEmpty()
        || !State.Result.JobInstanceId.IsEmpty() || !State.Service.JobInstanceId.IsEmpty()
        || UsedInstanceIds.Num() || RewardedInstanceIds.Num() || State.TotalCredits || State.CompletedJobs
        || State.ElapsedSeconds != 0 || State.TravelCm != 0 || State.BatteryPercent != 100
        || State.bCharging || bCancellationPending || NextServiceGeneration != 1) return false;
    State.BatteryPercent = InitialPercent;
    bNativeOperatingBatteryFixtureConsumed = true;
    return true;
}
#endif

bool FMAYADeliveryCore::ValidJob(const FMAYADeliveryJob& J) const
{
    return !J.ProfileId.IsEmpty() && !J.Title.IsEmpty() && !J.PickupStationId.IsEmpty()
        && !J.DropoffStationId.IsEmpty() && J.PickupStationId != J.DropoffStationId
        && J.ParcelCount >= 1 && J.ParcelCount <= 2
        && FMath::IsFinite(J.PayloadKg) && J.PayloadKg > 0 && J.PayloadKg <= Balance.CapacityKg
        && FMath::IsFinite(J.DeadlineSeconds) && J.DeadlineSeconds > 0;
}

bool FMAYADeliveryCore::IsActive() const
{
    return !State.bDroneTotalled && (State.Phase == EMAYADeliveryPhase::Accepted || State.Phase == EMAYADeliveryPhase::Loading
        || State.Phase == EMAYADeliveryPhase::Carrying || State.Phase == EMAYADeliveryPhase::Unloading);
}

bool FMAYADeliveryCore::Offer(const FMAYADeliveryJob& J)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (State.bShiftWon || IsActive() || bCancellationPending || !ValidJob(J)) return false;
    State.Job = J;
    State.JobInstanceId.Reset();
    State.Service = FMAYADeliveryServiceToken();
    State.Result = FMAYADeliveryResult();
    State.Phase = EMAYADeliveryPhase::Available;
    State.ElapsedSeconds = State.TravelCm = 0;
    State.CargoConditionPercent = 100;
    State.Recoveries = 0;
    State.bSecuredParcelCountKnown = false; State.SecuredParcelCount = 0;
    State.LastImpact = FMAYADeliveryImpactReceipt(); State.ImpactHistory.Reset();
    AppliedImpactCeilings.Reset(); ImpactEpisodeWorldSeconds = CargoSecuredWorldSeconds = -1;
    ImpactEpisodeConditionPoints = 0;
    State.bCharging = false; State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.Status = TEXT("J: accept this delivery. Green bolt: charger. Hold Left Shift to boost.");
    return true;
}

bool FMAYADeliveryCore::Accept(const FString& Id)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (!State.bShiftStarted || State.bShiftWon || State.Phase != EMAYADeliveryPhase::Available || !ValidJob(State.Job)
        || Id.IsEmpty() || UsedInstanceIds.Contains(Id) || State.BatteryPercent <= 0
        || bCancellationPending) return false;
    UsedInstanceIds.Add(Id);
    State.JobInstanceId = Id;
    State.Phase = EMAYADeliveryPhase::Accepted;
    State.bCharging = false; State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.Status = TEXT("Collect the parcel at the depot. Brake before pressing E.");
    MissingObservationSeconds = 0;
    return true;
}

bool FMAYADeliveryCore::BeginService(const FString& StationId, const FVector& Point,
    const FMAYADeliveryRobotObservation& R, FMAYADeliveryServiceToken& Out)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    const bool bPickup = State.Phase == EMAYADeliveryPhase::Accepted;
    if (!State.bShiftStarted || State.bShiftWon || (!bPickup && State.Phase != EMAYADeliveryPhase::Carrying) || bCancellationPending
        || State.bCharging || !R.IsStoppedAndSupported() || !R.bDoorsClosedLatched
        || Point.ContainsNaN() || StationId != (bPickup ? State.Job.PickupStationId : State.Job.DropoffStationId)
        || FVector::Dist2D(R.ChassisPose.GetLocation(), Point) > Balance.ServiceRadiusCm
        || FMath::Abs(R.ChassisPose.GetLocation().Z - Point.Z) > 100) return false;
    State.Service.JobInstanceId = State.JobInstanceId;
    State.Service.Generation = NextServiceGeneration++;
    State.Service.bPickup = bPickup;
    State.Service.StationId = StationId;
    State.Service.RobotResetGeneration = R.ResetGeneration;
    State.Service.StartedWorldSeconds = R.ObservationWorldSeconds;
    State.Service.RobotServicePosition = R.ChassisPose.GetLocation();
    State.Phase = bPickup ? EMAYADeliveryPhase::Loading : EMAYADeliveryPhase::Unloading;
    ServiceElapsed = 0;
    State.Status = bPickup ? TEXT("Loading the parcel. Keep the robot stopped.")
        : TEXT("Unloading the parcel. Keep the robot stopped.");
    Out = State.Service;
    return true;
}

bool FMAYADeliveryCore::CommitService(const FMAYADeliveryServiceEvidence& E)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    const bool bPickup = State.Phase == EMAYADeliveryPhase::Loading;
    TArray<FString> ExpectedIds;
    for (int32 I = 0; I < State.Job.ParcelCount; ++I)
        ExpectedIds.Add(I == 0 ? State.JobInstanceId : State.JobInstanceId + TEXT("/parcel_2"));
    if ((!bPickup && State.Phase != EMAYADeliveryPhase::Unloading) || bCancellationPending
        || !State.Service.Equals(E.Token) || E.Token.bPickup != bPickup
        || !E.Robot.IsStoppedAndSupported() || !E.Robot.bDoorsClosedLatched
        || E.Robot.ResetGeneration != State.Service.RobotResetGeneration
        || E.Robot.ObservationWorldSeconds < State.Service.StartedWorldSeconds
        || FVector::Dist(E.Robot.ChassisPose.GetLocation(), State.Service.RobotServicePosition) > 5.0
        || E.ObservedParcelId != State.JobInstanceId || !E.bOpenEndpointsObserved
        || E.ObservedParcelCount != State.Job.ParcelCount || E.ObservedParcelIds != ExpectedIds
        || !E.bAuthorizedInstantTransferObserved || !E.bActualPlacementEndpointsClear
        || !E.bParcelAtCorrectDestination
        || !E.bActualCustodyObserved || !E.bParcelMassObserved
        || !FMath::IsFinite(E.ObservedParcelKg)
        || FMath::Abs(E.ObservedParcelKg - State.Job.PayloadKg) > 0.05) return false;
    if (!bPickup && State.CargoConditionPercent < Balance.MinimumDeliveryConditionPercent)
    { Fail(TEXT("Cargo condition is below 80%. Delivery cannot be completed.")); return false; }
    State.bSecuredParcelCountKnown = true;
    State.SecuredParcelCount = bPickup ? State.Job.ParcelCount : 0;
    if (bPickup) CargoSecuredWorldSeconds = E.Robot.ObservationWorldSeconds;
    State.Service = FMAYADeliveryServiceToken();
    State.Phase = bPickup ? EMAYADeliveryPhase::Carrying : EMAYADeliveryPhase::Succeeded;
    State.Status = bPickup ? TEXT("Parcel secured. Follow the destination marker.") : TEXT("Delivery complete.");
    if (!bPickup) FreezeResult(true, TEXT("Parcel delivered safely."));
    return true;
}

bool FMAYADeliveryCore::CancelService(const FMAYADeliveryServiceToken& Token, const FString& Reason)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if ((State.Phase != EMAYADeliveryPhase::Loading && State.Phase != EMAYADeliveryPhase::Unloading)
        || !State.Service.Equals(Token) || bCancellationPending) return false;
    bCancellationPending = true;
    State.Status = TEXT("Service blocked: ") + Reason + TEXT(". Closing the robot before retry.");
    return true;
}

bool FMAYADeliveryCore::ConfirmCancelledClosure(const FMAYADeliveryRobotObservation& R)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (!bCancellationPending || !R.IsStoppedAndSupported() || !R.bDoorsClosedLatched) return false;
    const bool bPickup = State.Service.bPickup;
    bCancellationPending = false;
    State.Service = FMAYADeliveryServiceToken();
    if (State.Phase != EMAYADeliveryPhase::Failed)
        State.Phase = bPickup ? EMAYADeliveryPhase::Accepted : EMAYADeliveryPhase::Carrying;
    State.Status = TEXT("Service cancelled safely. Cargo ownership is unchanged; E retries.");
    return true;
}

void FMAYADeliveryCore::StopChargingVisit(const FMAYADeliveryRobotObservation& R,
    bool bAtCharger, const FString& Reason, bool bAutomaticDisconnect)
{
    if (State.bCharging && bAutomaticDisconnect)
    {
        auto& Receipt = State.LastChargeDisconnect; ++Receipt.Generation;
        Receipt.FailedConditions = FailedChargeConditions(R, bAtCharger, State.Phase);
        if (Receipt.FailedConditions.IsEmpty()) Receipt.FailedConditions.Add(Reason);
        Receipt.Robot = R; Receipt.Phase = State.Phase; Receipt.bAtCharger = bAtCharger;
        Receipt.BatteryPercentBeforeTick = State.BatteryPercent;
        State.Status = TEXT("Charging stopped. ") + Reason;
    }
    State.bCharging = false; State.ChargeVisit.bEligible = false;
    State.ChargeVisit.ElapsedSeconds = 0;
    State.ChargeVisit.RemainingSeconds = FMAYADeliveryCashRules::ChargeDwellSeconds;
    State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.ChargeVisit.Reason = Reason;
    State.ChargeVisit.Stage = State.ChargeVisit.bInside ? EMAYADeliveryChargeStage::Blocked : EMAYADeliveryChargeStage::None;
}

bool FMAYADeliveryCore::TickChargingRing(double Dt, const FMAYADeliveryRobotObservation& R,
    const FMAYADeliveryChargingObservation& C)
{
    auto& Visit = State.ChargeVisit;
    const bool bFresh = FMath::IsFinite(C.WorldSeconds) && FMath::IsFinite(R.ObservationWorldSeconds)
        && C.WorldSeconds >= 0 && R.ObservationWorldSeconds >= 0
        && C.WorldSeconds - R.ObservationWorldSeconds >= 0
        && C.WorldSeconds - R.ObservationWorldSeconds <= 0.25
        && R.ObservationWorldSeconds > LastChargeObservationWorldSeconds;
    if (FMath::IsFinite(R.ObservationWorldSeconds) && R.ObservationWorldSeconds >= 0)
        LastChargeObservationWorldSeconds = FMath::Max(LastChargeObservationWorldSeconds, R.ObservationWorldSeconds);
    if (!bFresh || !R.bKnown || (C.bInside && C.StationId.IsEmpty()))
    {
        // Unknown/stale observations stop energy but do not invent a real exit
        // that could rearm a paid/consumed visit at the same physical station.
        StopChargingVisit(R, Visit.bInside, TEXT("Charging observation is stale or unknown."), true); return false;
    }
    const bool bInside = C.bInside && !C.StationId.IsEmpty();
    const bool bNewVisit = bInside && (!Visit.bInside || Visit.StationId != C.StationId);
    if (!bInside || bNewVisit)
    {
        StopChargingVisit(R, bInside, bInside ? TEXT("Entered a different charging ring.") : TEXT("Outside a fresh observed charging ring."), true);
        if (!bInside) { Visit.bInside = false; Visit.StationId.Empty(); Visit.Stage = EMAYADeliveryChargeStage::None; return false; }
        Visit = FMAYADeliveryChargeVisit(); Visit.Generation = ++NextChargeVisitGeneration;
        Visit.StationId = C.StationId; Visit.bInside = true;
    }
    // Full standing is consumed, so later tiny idle drain cannot bill a second
    // event without an actual exit. An already-paid $2->$0 event remains funded.
    if (State.BatteryPercent >= 100)
    {
        State.bCharging = false; Visit.bConsumed = true; Visit.bEligible = false;
        Visit.Stage = EMAYADeliveryChargeStage::Full; Visit.Reason = TEXT("Battery full."); return false;
    }
    if (Visit.bConsumed)
    {
        State.bCharging = false; Visit.bEligible = false;
        Visit.Stage = EMAYADeliveryChargeStage::Blocked;
        Visit.Reason = TEXT("This charging visit is complete. Leave the ring before another recharge."); return false;
    }
    const bool bPhysicalSafe = bFresh && R.IsStoppedAndSupported() && R.bDoorsClosedLatched
        && State.bShiftStarted && !State.bShiftWon && !bCancellationPending
        && State.Phase != EMAYADeliveryPhase::Loading && State.Phase != EMAYADeliveryPhase::Unloading
        && !C.bObjectiveServicePriority && !C.bRecoveryPending && !C.bFreshMovementIntent;
    if (!bPhysicalSafe)
    {
        const FString Reason = C.bFreshMovementIntent ? TEXT("Fresh movement intent ends charging; release controls before driving.")
            : C.bObjectiveServicePriority ? TEXT("Cargo service has priority at this station.")
            : C.bRecoveryPending ? TEXT("Wait for native recovery to settle.")
            : TEXT("Park with supported robot and closed latched lids.");
        StopChargingVisit(R, bInside, Reason, true); return false;
    }
    if (!Visit.bPaid && !FMAYADeliveryCashRules::CanAffordRecharge(State.TotalCredits))
    { StopChargingVisit(R, bInside, TEXT("Recharge costs $2. Insufficient cash; no fee or energy was applied."), true); return false; }
    Visit.bEligible = true; Visit.Reason.Empty();
    const double BeforeDwell = Visit.ElapsedSeconds;
    if (!Visit.bPaid)
    {
        // First observed entry never counts a preceding outside frame.
        if (!bNewVisit) Visit.ElapsedSeconds = FMath::Min(FMAYADeliveryCashRules::ChargeDwellSeconds, BeforeDwell + Dt);
        Visit.RemainingSeconds = FMath::Max(0.0, FMAYADeliveryCashRules::ChargeDwellSeconds - Visit.ElapsedSeconds);
        if (Visit.RemainingSeconds > 0) { Visit.Stage = EMAYADeliveryChargeStage::Qualifying; return false; }
    }
    const double EnergySeconds = Visit.bPaid ? Dt : FMath::Max(0.0, Dt - (FMAYADeliveryCashRules::ChargeDwellSeconds - BeforeDwell));
    if (!FMath::IsFinite(Balance.ChargePercentPerSecond) || Balance.ChargePercentPerSecond <= 0 || EnergySeconds <= 0)
    { Visit.Stage = EMAYADeliveryChargeStage::Qualifying; return false; }
    const double Gain = FMath::Min(100.0 - State.BatteryPercent, Balance.ChargePercentPerSecond * EnergySeconds);
    if (!FMath::IsFinite(Gain) || Gain <= 0) return false;
    if (!Visit.bPaid)
    {
        // Fee and first positive gain are one authority transaction. No UI call
        // or elapsed timer alone can buy or grant energy.
        auto& Receipt = State.LastRecharge;
        Receipt = FMAYADeliveryRechargeReceipt(); Receipt.Generation = ++NextRechargeGeneration;
        Receipt.VisitGeneration = Visit.Generation; Receipt.ShiftInstanceId = State.ShiftInstanceId;
        Receipt.StationId = Visit.StationId; Receipt.WorldSeconds = R.ObservationWorldSeconds;
        Receipt.FeeDollars = FMAYADeliveryCashRules::RechargeFeeDollars;
        Receipt.CashBefore = State.TotalCredits; Receipt.BatteryBefore = State.BatteryPercent;
        State.TotalCredits -= Receipt.FeeDollars; State.RechargeFeesPaidDollars += Receipt.FeeDollars;
        State.BatteryPercent += Gain; Receipt.CashAfter = State.TotalCredits; Receipt.BatteryAfter = State.BatteryPercent;
        Visit.bPaid = true;
    }
    else State.BatteryPercent += Gain;
    State.bCharging = State.BatteryPercent < 100;
    Visit.Stage = State.bCharging ? EMAYADeliveryChargeStage::Charging : EMAYADeliveryChargeStage::Full;
    Visit.ElapsedSeconds = FMAYADeliveryCashRules::ChargeDwellSeconds; Visit.RemainingSeconds = 0;
    if (!State.bCharging) { Visit.bConsumed = true; Visit.Reason = TEXT("Battery full."); }
    State.Status = State.bCharging ? TEXT("Charging in the green ring. Delivery deadline continues.") : TEXT("Battery full.");
    return true;
}

void FMAYADeliveryCore::Tick(double Dt, bool bPaused, const FMAYADeliveryRobotObservation& R, bool bAtCharger)
{
    FMAYADeliveryChargingObservation C;
    C.WorldSeconds = R.ObservationWorldSeconds; C.bInside = bAtCharger;
    // Legacy bool has no station identity and cannot admit a paid ring event.
    Tick(Dt, bPaused, R, C);
}

void FMAYADeliveryCore::Tick(double Dt, bool bPaused, const FMAYADeliveryRobotObservation& R,
    const FMAYADeliveryChargingObservation& Charging)
{
    if (bPaused || !State.bShiftStarted || State.bShiftWon || State.bDroneTotalled || !FMath::IsFinite(Dt) || Dt <= 0) return;
    if (State.bVehicleContactPending)
    {
        // Brief identity adjudication inhibits energy/custody/rewards while the
        // real job deadline keeps running. No world pause or resource grant.
        if (IsActive())
        {
            State.ElapsedSeconds += Dt;
            if (State.Phase == EMAYADeliveryPhase::Loading || State.Phase == EMAYADeliveryPhase::Unloading) ServiceElapsed += Dt;
            if (State.ElapsedSeconds >= State.Job.DeadlineSeconds) Fail(TEXT("Delivery deadline expired during vehicle-contact adjudication."));
        }
        return;
    }
    LastObservedWorldSeconds = R.ObservationWorldSeconds;
    State.bSecuredParcelCountKnown = R.bSecuredParcelCountKnown;
    State.SecuredParcelCount = R.bSecuredParcelCountKnown ? FMath::Clamp(R.SecuredParcelCount, 0, 2) : 0;
    ApplyImpactEvent(R.LastNativeImpact);
    const bool bEnergyAdded = TickChargingRing(Dt, R, Charging);
    // Succeeded is still a playable driving phase between offers. Live battery
    // uses ordinary idle/motion/boost rules; the frozen result and job clock do
    // not change, because the inactive-job return remains after energy below.
    if (!bEnergyAdded && R.bKnown && State.Phase != EMAYADeliveryPhase::Failed)
    {
        const double Movement = R.VelocityCmps.ContainsNaN() ? 0.0 : FMath::Clamp(R.VelocityCmps.Size() / 100.0, 0.0, 3.0);
        const double CountMultiplier = R.bSecuredParcelCountKnown && R.SecuredParcelCount == 2
            ? Balance.TwoParcelDrainMultiplier : 1.0;
        // Actual fresh motor use admits boost energy, including a powered curb
        // stall. Key intent alone remains ordinary idle. New2x boosted motion is
        // normalized to previous1x motion, then the entire old boost cost gains20%.
        const bool bBoostPower = R.bBoostDriveActive && R.bReady && R.Fault.IsEmpty()
            && !IsDriveLocked() && FMath::IsFinite(R.BoostMotorEffortFraction)
            && R.BoostMotorEffortFraction > 0.0
            && FMath::IsFinite(Balance.BoostSpeedNormalization) && Balance.BoostSpeedNormalization > 0.0
            && FMath::IsFinite(Balance.BoostStallFadeCmps) && Balance.BoostStallFadeCmps > 0.0
            && FMath::IsFinite(Balance.BoostTotalDrainMultiplier) && Balance.BoostTotalDrainMultiplier > 0.0;
        double Drain = Balance.IdleDrainPercentPerSecond + Balance.MovingDrainPercentPerSecond * Movement;
        if (bBoostPower)
        {
            const double Speed = R.VelocityCmps.ContainsNaN() ? 0.0 : R.VelocityCmps.Size();
            const double BoostMovement = FMath::Clamp(Speed / (100.0 * Balance.BoostSpeedNormalization), 0.0, 3.0);
            const double StallWeight = FMath::Clamp(1.0 - Speed / Balance.BoostStallFadeCmps, 0.0, 1.0);
            const double Effort = FMath::Clamp(R.BoostMotorEffortFraction, 0.0, 1.0);
            const double Basis = FMath::Max(BoostMovement, Effort * StallWeight);
            Drain = Balance.BoostTotalDrainMultiplier * (Balance.IdleDrainPercentPerSecond
                + Balance.MovingDrainPercentPerSecond * Balance.BoostMovingDrainMultiplier * Basis);
        }
        Drain *= CountMultiplier;
        State.BatteryPercent = FMath::Max(0.0, State.BatteryPercent - Drain * Dt);
    }
    if (!IsActive()) return;
    State.ElapsedSeconds += Dt;
    if (R.bKnown && !R.VelocityCmps.ContainsNaN()) State.TravelCm += R.VelocityCmps.Size2D() * Dt;
    MissingObservationSeconds = R.bKnown && R.bReady && R.Fault.IsEmpty() ? 0.0 : MissingObservationSeconds + Dt;
    if (MissingObservationSeconds >= 3) { Fail(TEXT("The robot stopped responding. Recover to the depot.")); return; }
    if (State.BatteryPercent <= 0) { Fail(TEXT("Battery depleted. Recharge costs $2; recover to the depot or start a new shift.")); return; }
    if (State.ElapsedSeconds >= State.Job.DeadlineSeconds) { Fail(TEXT("Delivery deadline expired.")); return; }
    if (State.Phase == EMAYADeliveryPhase::Loading || State.Phase == EMAYADeliveryPhase::Unloading)
    {
        ServiceElapsed += Dt;
        if (ServiceElapsed >= Balance.ServiceTimeoutSeconds && !bCancellationPending)
            CancelService(State.Service, TEXT("mechanical service timeout"));
    }
}

bool FMAYADeliveryCore::SetCharging(bool bRequested, const FMAYADeliveryRobotObservation& R, bool bAtCharger)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (bRequested)
    { State.Status = TEXT("Charging starts automatically after ten eligible seconds in the green ring; recharge costs $2."); return false; }
    StopChargingVisit(R, bAtCharger, TEXT("Charging stopped."), false); return true;
}

bool FMAYADeliveryCore::ApplyImpactEvent(const FMAYADeliveryImpactEvent& Event)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (Event.Generation == 0 || Event.Kind.IsEmpty()) return false;
    const FString Key = Event.Kind + TEXT("/") + LexToString(Event.Generation);
    const double* Prior = AppliedImpactCeilings.Find(Key);
    const uint64 SeenGeneration = LastObservedImpactGeneration.FindRef(Event.Kind);
    if (!Prior && Event.Generation <= SeenGeneration) return false;
    LastObservedImpactGeneration.Add(Event.Kind, FMath::Max(SeenGeneration, Event.Generation));
    // Consume pre-custody events, including the same world timestamp as pickup.
    // Source generations remain observed across reset/offer boundaries.
    if ((State.Phase != EMAYADeliveryPhase::Carrying && State.Phase != EMAYADeliveryPhase::Unloading)
        || !State.bSecuredParcelCountKnown || State.SecuredParcelCount < 1
        || !FMath::IsFinite(Event.WorldSeconds) || Event.WorldSeconds <= CargoSecuredWorldSeconds
        || (LastObservedWorldSeconds >= 0 && Event.WorldSeconds > LastObservedWorldSeconds + 0.001)
        || !FMath::IsFinite(Event.ImpulseKgMps) || Event.ImpulseKgMps < 0
        || !FMath::IsFinite(Event.PreContactDownCmps) || !FMath::IsFinite(Event.DropCm)
        || !FMath::IsFinite(Event.ApproachSpeedCmps))
    { AppliedImpactCeilings.Add(Key, 1.0); return false; }
    double Ceiling = 0;
    if (Event.Kind == TEXT("ChassisImpact")
        && Event.ImpulseKgMps >= FMAYADeliveryLateralImpactCalibration::MinimumImpulseKgMps
        && Event.ApproachSpeedCmps >= FMAYADeliveryLateralImpactCalibration::MinimumApproachCmps)
        Ceiling = Event.ImpulseKgMps >= FMAYADeliveryLateralImpactCalibration::HardImpulseKgMps
            && Event.ApproachSpeedCmps >= FMAYADeliveryLateralImpactCalibration::HardApproachCmps ? 1.0 : 0.5;
    else if (Event.Kind == TEXT("CurbLanding") && Event.DropCm >= 8 && Event.PreContactDownCmps >= 40)
        Ceiling = 1.0;
    const double PreviouslySeen = AppliedImpactCeilings.FindRef(Key);
    if (Ceiling <= PreviouslySeen) return false;
    if (ImpactEpisodeWorldSeconds < 0 || Event.WorldSeconds - ImpactEpisodeWorldSeconds > 0.5)
    { ++NextImpactEpisode; ImpactEpisodeWorldSeconds = Event.WorldSeconds; ImpactEpisodeConditionPoints = 0; }
    // Both ceilings apply: even late escalation of the SAME source generation
    // costs only its remaining0.5%, rather than starting another1% loss.
    const double Delta = FMath::Max(0.0, FMath::Min(Ceiling - PreviouslySeen, Ceiling - ImpactEpisodeConditionPoints));
    AppliedImpactCeilings.Add(Key, Ceiling);
    ImpactEpisodeConditionPoints = FMath::Max(ImpactEpisodeConditionPoints, Ceiling);
    if (Delta <= 0) return false;
    FMAYADeliveryImpactReceipt Receipt;
    Receipt.EpisodeGeneration = NextImpactEpisode; Receipt.Event = Event;
    Receipt.AppliedConditionPoints = Delta; Receipt.EpisodeConditionPoints = ImpactEpisodeConditionPoints;
    Receipt.ConditionBefore = State.CargoConditionPercent;
    State.CargoConditionPercent = FMath::Max(0.0, State.CargoConditionPercent - Delta);
    Receipt.ConditionAfter = State.CargoConditionPercent;
    State.LastImpact = Receipt; State.ImpactHistory.Add(Receipt);
    if (State.ImpactHistory.Num() > 64) State.ImpactHistory.RemoveAt(0);
    return true;
}

void FMAYADeliveryCore::ApplyImpactDamage(double Points)
{
    // Legacy controlled fixtures use the same bounded event rule. Production
    // supplies source-tagged measured events via ApplyImpactEvent instead.
    FMAYADeliveryImpactEvent Event;
    Event.Generation = uint64(AppliedImpactCeilings.Num() + 1); Event.Kind = TEXT("ChassisImpact");
    Event.WorldSeconds = FMath::Max(CargoSecuredWorldSeconds + 0.01, ImpactEpisodeWorldSeconds + 1);
    Event.ApproachSpeedCmps = Points >= 1 ? FMAYADeliveryLateralImpactCalibration::HardApproachCmps
        : FMAYADeliveryLateralImpactCalibration::MinimumApproachCmps;
    LastObservedWorldSeconds = FMath::Max(LastObservedWorldSeconds, Event.WorldSeconds);
    Event.ImpulseKgMps = Points >= 1 ? 20 : Points > 0 ? 5 : 0;
    ApplyImpactEvent(Event);
}

void FMAYADeliveryCore::FreezeResult(bool bSuccess, const FString& Reason)
{
    // Deadline failures are evidence, not a reward/custody commit. They must
    // still freeze while identity is pending; only success is held back.
    if (State.bDroneTotalled || (State.bVehicleContactPending && bSuccess) || !State.Result.JobInstanceId.IsEmpty()) return;
    State.Result.JobInstanceId = State.JobInstanceId;
    State.Result.Title = State.Job.Title;
    State.Result.bSuccess = bSuccess;
    State.Result.Reason = Reason;
    State.Result.ElapsedSeconds = State.ElapsedSeconds;
    State.Result.BatteryPercent = State.BatteryPercent;
    State.Result.CargoConditionPercent = State.CargoConditionPercent;
    State.Result.Recoveries = State.Recoveries;
    if (bSuccess && !RewardedInstanceIds.Contains(State.JobInstanceId))
    {
        State.Result.Credits = State.Job.ParcelCount == 2 ? FMAYADeliveryCashRules::TwoParcelDollars
            : FMAYADeliveryCashRules::OneParcelDollars;
        const int32 CashBefore = State.TotalCredits;
        RewardedInstanceIds.Add(State.JobInstanceId);
        State.TotalCredits += State.Result.Credits;
        ++State.CompletedJobs;
        if (!State.bShiftWon && CashBefore < FMAYADeliveryCashRules::GoalDollars
            && State.TotalCredits >= FMAYADeliveryCashRules::GoalDollars)
        {
            State.bShiftWon = true; State.bCharging = false;
            auto& Win = State.ShiftWin; Win.bFrozen = true; Win.ShiftInstanceId = State.ShiftInstanceId;
            Win.FinalJobInstanceId = State.JobInstanceId; Win.CashBefore = CashBefore;
            Win.DeliveryEarningsDollars = State.Result.Credits; Win.FinalCashDollars = State.TotalCredits;
            Win.CompletedJobs = State.CompletedJobs; Win.CargoConditionPercent = State.CargoConditionPercent;
        }
    }
}

void FMAYADeliveryCore::Fail(const FString& Reason)
{
    if (!IsActive()) return;
    if (State.Phase == EMAYADeliveryPhase::Loading || State.Phase == EMAYADeliveryPhase::Unloading)
        bCancellationPending = true;
    State.Phase = EMAYADeliveryPhase::Failed;
    State.bCharging = false; State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.Status = Reason;
    FreezeResult(false, Reason);
}

bool FMAYADeliveryCore::RecoverAfterNativeReset(const FMAYADeliveryRobotObservation& R)
{
    if (State.bDroneTotalled || State.bVehicleContactPending) return false;
    if (State.bShiftWon || !R.IsStoppedAndSupported() || !R.bDoorsClosedLatched
        || State.Phase == EMAYADeliveryPhase::Succeeded || State.Phase == EMAYADeliveryPhase::Available) return false;
    ++NextServiceGeneration; // Old callbacks can no longer own this mission.
    bCancellationPending = false;
    State.Service = FMAYADeliveryServiceToken();
    State.Phase = State.Phase == EMAYADeliveryPhase::Failed ? EMAYADeliveryPhase::Available : EMAYADeliveryPhase::Accepted;
    if (State.Phase == EMAYADeliveryPhase::Available)
    {
        State.JobInstanceId.Reset();
        State.Result = FMAYADeliveryResult();
        State.ElapsedSeconds = State.TravelCm = 0;
        State.CargoConditionPercent = 100;
    }
    ++State.Recoveries;
    State.bCharging = false; State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.Status = TEXT("Recovered to depot. Cargo returned to pickup. Park in the green ring for ten seconds; recharge costs $2.");
    MissingObservationSeconds = 0;
    return true;
}

bool FMAYADeliveryCore::IsDriveLocked() const
{
    return State.bDroneTotalled || State.bVehicleContactPending || !State.bShiftStarted || State.bShiftWon || State.bCharging || State.BatteryPercent <= 0 || bCancellationPending
        || State.Phase == EMAYADeliveryPhase::Loading || State.Phase == EMAYADeliveryPhase::Unloading
        || State.Phase == EMAYADeliveryPhase::Failed;
}

void FMAYADeliveryCore::SetVehicleContactAdjudicationPending(bool bPending)
{
    State.bVehicleContactPending = !State.bDroneTotalled && bPending;
    if (State.bVehicleContactPending)
    { State.bCharging = false; State.ChargeVisit.bEligible = false; State.ChargeVisit.Reason = TEXT("Checking actual vehicle contact."); }
}

bool FMAYADeliveryCore::ApplyVehicleContact(const FMAYAVehicleContactEvent& Event)
{
    if (!State.bShiftStarted || State.bShiftWon || State.bDroneTotalled || !Event.IsFatal()) return false;
    auto& Crash = State.VehicleCrash;
    Crash.bFrozen = true; Crash.ShiftInstanceId = State.ShiftInstanceId; Crash.JobInstanceId = State.JobInstanceId;
    Crash.Contact = Event; Crash.CashDollars = State.TotalCredits; Crash.CompletedJobs = State.CompletedJobs;
    Crash.RechargeFeesPaidDollars = State.RechargeFeesPaidDollars; Crash.BatteryPercent = State.BatteryPercent;
    Crash.CargoConditionPercent = State.CargoConditionPercent;
    Crash.Reason = TEXT("DRONE TOTALLED. A moving car struck the drone from the front or rear. Shift lost; Start Over begins a fresh game.");
    // Existing mission/result history is retained. No cargo deletion, credit,
    // energy refill or depot reconstruction is used to disguise a fatal crash.
    State.bDroneTotalled = true; State.bVehicleContactPending = false; State.bCharging = false;
    State.ChargeVisit.bEligible = false; State.ChargeVisit.bConsumed |= State.ChargeVisit.bPaid;
    State.Phase = EMAYADeliveryPhase::Failed; State.Status = Crash.Reason;
    State.Service = FMAYADeliveryServiceToken(); ++NextServiceGeneration; bCancellationPending = false;
    return true;
}
