#include "DDSN08Authority.h"

#include "Misc/Guid.h"

bool FDDSN08Token::Equals(const FDDSN08Token& Other) const
{
    return MissionInstanceId == Other.MissionInstanceId && OperationId == Other.OperationId
        && Generation == Other.Generation && Leg == Other.Leg && PrePhase == Other.PrePhase
        && StationId == Other.StationId;
}

FDDSN08Authority::FDDSN08Authority(IDDSN08MotionPort& InMotion,
    IDDSN08MechanicsPort& InMechanics, IDDSN08SavePort& InSave)
    : Motion(InMotion), Mechanics(InMechanics), Save(InSave)
{
}

void FDDSN08Authority::ApplyDriveLock(bool bLocked)
{
    if (bDriveLocked != bLocked)
    {
        bDriveLocked = bLocked;
        Motion.SetDriveLocked(bLocked);
    }
}

bool FDDSN08Authority::ValidStableState(const FDDSN08StableSnapshot& S) const
{
    if (S.MissionInstanceId.IsEmpty() || S.CargoId.IsEmpty() || S.PickupStationId.IsEmpty()
        || S.DeliveryStationId.IsEmpty() || S.PickupStationId == S.DeliveryStationId
        || S.ScopedStarIds.Num() != 3 || !FMath::IsFinite(S.ElapsedGameplaySeconds)
        || S.ElapsedGameplaySeconds < 0.0)
    {
        return false;
    }
    TArray<FString> Unique = S.ScopedStarIds;
    Unique.Sort();
    if (Unique[0].IsEmpty() || Unique[0] == Unique[1] || Unique[1] == Unique[2])
    {
        return false;
    }
    if ((S.Phase == EDDSN08Phase::AwaitingPickup && S.Owner != EDDSN08Owner::Station)
        || (S.Phase == EDDSN08Phase::Carrying && S.Owner != EDDSN08Owner::Robot)
        || (S.Phase == EDDSN08Phase::Completed && S.Owner != EDDSN08Owner::Recipient))
    {
        return false;
    }
    if (S.Receipts.Num() > 2 || S.CollectedStarIds.Num() > 3)
    {
        return false;
    }
    for (int32 I = 0; I < S.CollectedStarIds.Num(); ++I)
    {
        if (!S.ScopedStarIds.Contains(S.CollectedStarIds[I])) return false;
        for (int32 J = I + 1; J < S.CollectedStarIds.Num(); ++J)
        {
            if (S.CollectedStarIds[I] == S.CollectedStarIds[J]) return false;
        }
    }
    for (int32 I = 0; I < S.Receipts.Num(); ++I)
    {
        const FDDSN08Receipt& R = S.Receipts[I];
        if (R.MissionInstanceId != S.MissionInstanceId || R.CargoId != S.CargoId
            || R.OperationId.IsEmpty() || R.Generation == 0
            || (R.PrePhase != EDDSN08Phase::AwaitingPickup && R.PrePhase != EDDSN08Phase::Carrying)
            || R.StationId != (R.PrePhase == EDDSN08Phase::AwaitingPickup
                ? S.PickupStationId : S.DeliveryStationId)) return false;
        for (int32 J = I + 1; J < S.Receipts.Num(); ++J)
        {
            if (R.PrePhase == S.Receipts[J].PrePhase && R.StationId == S.Receipts[J].StationId)
                return false;
        }
    }
    const bool bPickupReceipt = S.Receipts.ContainsByPredicate([](const FDDSN08Receipt& R)
        { return R.PrePhase == EDDSN08Phase::AwaitingPickup; });
    const bool bDeliveryReceipt = S.Receipts.ContainsByPredicate([](const FDDSN08Receipt& R)
        { return R.PrePhase == EDDSN08Phase::Carrying; });
    if ((S.Phase == EDDSN08Phase::AwaitingPickup && (bPickupReceipt || bDeliveryReceipt))
        || (S.Phase == EDDSN08Phase::Carrying && (!bPickupReceipt || bDeliveryReceipt))
        || (S.Phase == EDDSN08Phase::Completed && (!bPickupReceipt || !bDeliveryReceipt)))
        return false;
    if (S.bHasFrozenResult != (S.Phase == EDDSN08Phase::Completed)) return false;
    if (S.bHasFrozenResult)
    {
        TArray<FString> Sorted = S.CollectedStarIds;
        Sorted.Sort();
        if (S.FrozenResult.ExactStarIds != Sorted || S.FrozenResult.Count != Sorted.Num()
            || S.FrozenResult.FrozenElapsedSeconds != S.ElapsedGameplaySeconds)
            return false;
    }
    return true;
}

bool FDDSN08Authority::SameStableSnapshot(const FDDSN08StableSnapshot& A,
    const FDDSN08StableSnapshot& B) const
{
    if (A.MissionInstanceId != B.MissionInstanceId || A.CargoId != B.CargoId
        || A.PickupStationId != B.PickupStationId || A.DeliveryStationId != B.DeliveryStationId
        || A.ScopedStarIds != B.ScopedStarIds || A.CollectedStarIds != B.CollectedStarIds
        || A.Phase != B.Phase || A.Owner != B.Owner
        || A.ElapsedGameplaySeconds != B.ElapsedGameplaySeconds
        || A.bHasFrozenResult != B.bHasFrozenResult || A.Receipts.Num() != B.Receipts.Num())
        return false;
    for (int32 I = 0; I < A.Receipts.Num(); ++I)
    {
        const FDDSN08Receipt& X = A.Receipts[I];
        const FDDSN08Receipt& Y = B.Receipts[I];
        if (X.MissionInstanceId != Y.MissionInstanceId || X.PrePhase != Y.PrePhase
            || X.StationId != Y.StationId || X.OperationId != Y.OperationId
            || X.Generation != Y.Generation || X.CargoId != Y.CargoId) return false;
    }
    return !A.bHasFrozenResult || (A.FrozenResult.ExactStarIds == B.FrozenResult.ExactStarIds
        && A.FrozenResult.Count == B.FrozenResult.Count
        && A.FrozenResult.FrozenElapsedSeconds == B.FrozenResult.FrozenElapsedSeconds);
}

bool FDDSN08Authority::StartFreshMission(const FString& InstanceId, const FString& CargoId,
    const FString& PickupStationId, const FString& DeliveryStationId,
    const TArray<FString>& ThreeScopedStarIds)
{
    if (bInitialized || bHasOperation || Gate != EDDSN08Gate::None || bWaitingForClosedLids)
        return false;
    FDDSN08StableSnapshot Fresh;
    Fresh.MissionInstanceId = InstanceId;
    Fresh.CargoId = CargoId;
    Fresh.PickupStationId = PickupStationId;
    Fresh.DeliveryStationId = DeliveryStationId;
    Fresh.ScopedStarIds = ThreeScopedStarIds;
    if (!ValidStableState(Fresh)) return false;
    State = MoveTemp(Fresh);
    bInitialized = true;
    bHasCommittedPayload = false;
    bHasCandidate = false;
    bStablePhaseAcknowledged = true; // Explicit fresh mission, no previous transfer.
    bInteractDown = false;
    ApplyDriveLock(false);
    return true;
}

bool FDDSN08Authority::ReplayExplicit(const FString& NewInstanceId,
    const FString& NewCargoId, const TArray<FString>& ThreeNewScopedStarIds)
{
    if (!bInitialized || State.Phase != EDDSN08Phase::Completed
        || !bStablePhaseAcknowledged || Gate != EDDSN08Gate::None || bHasOperation
        || bWaitingForClosedLids || NewInstanceId == State.MissionInstanceId)
        return false;
    for (const FString& StarId : ThreeNewScopedStarIds)
    {
        if (State.ScopedStarIds.Contains(StarId)) return false;
    }
    const FString Pickup = State.PickupStationId;
    const FString Delivery = State.DeliveryStationId;
    bInitialized = false;
    if (StartFreshMission(NewInstanceId, NewCargoId, Pickup, Delivery, ThreeNewScopedStarIds))
        return true;
    bInitialized = true; // Preserve the completed result if replay input was invalid.
    return false;
}

bool FDDSN08Authority::RestoreValidatedCheckpoint(const FDDSN08StableSnapshot& Snapshot,
    bool bN11Validated, bool bLidsClosedLatched, bool bFinitePoseValidated)
{
    if (!bN11Validated || !bLidsClosedLatched || !bFinitePoseValidated
        || !ValidStableState(Snapshot)) return false;
    // The write barrier must complete before the in-memory gate is discarded.
    // It also checks that the requested validated checkpoint is the durable
    // head, not merely some older valid snapshot. N11 implements/proves this.
    const FDDSN08SaveCandidate* InFlight = bHasCandidate ? &Candidate : nullptr;
    const FDDSN08RestoreBarrier Barrier = Save.EstablishRestoreBarrier(Snapshot, InFlight);
    if (Barrier.MissionInstanceId != Snapshot.MissionInstanceId
        || !Barrier.bCheckpointDurablyCurrent || !Barrier.bOlderWritesCannotOvertake
        || (InFlight && Barrier.FencedThroughRevision < InFlight->Revision))
        return false;
    // Explicit restore now discards an unsaved transaction timeline. The disk
    // read, corruption fallback and finite pose check belong to N11/N06.
    ++NextGeneration;
    bHasOperation = false;
    bWaitingForClosedLids = false;
    Transient = EDDSN08Transient::None;
    MechanicStep = EDDSN08MechanicStep::None;
    Gate = EDDSN08Gate::None;
    bHasCommittedPayload = false;
    bHasCandidate = false;
    State = Snapshot;
    bInitialized = true;
    bStablePhaseAcknowledged = true;
    bInteractDown = false;
    OperationGameplaySeconds = 0.0;
    ApplyDriveLock(false);
    return true;
}

void FDDSN08Authority::TickGameplay(double DeltaSeconds, bool bPaused)
{
    if (!bInitialized || bPaused || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0)
        return;
    if (State.Phase != EDDSN08Phase::Completed)
        State.ElapsedGameplaySeconds += DeltaSeconds;
    if (bHasOperation)
    {
        OperationGameplaySeconds += DeltaSeconds;
        if (OperationGameplaySeconds >= 5.0)
        {
            const FDDSN08Token TimedOut = Operation;
            CancelPreLatch(TimedOut);
        }
    }
}

FString FDDSN08Authority::ActiveStationId() const
{
    return State.Phase == EDDSN08Phase::AwaitingPickup ? State.PickupStationId
        : State.Phase == EDDSN08Phase::Carrying ? State.DeliveryStationId : FString();
}

EDDSN08Leg FDDSN08Authority::ActiveLeg() const
{
    return State.Phase == EDDSN08Phase::Carrying ? EDDSN08Leg::Delivery : EDDSN08Leg::Pickup;
}

FDDSN08Eligibility FDDSN08Authority::Evaluate(const FDDSN08Observation& O,
    bool bNewPressEdge) const
{
    FDDSN08Eligibility E;
    E.MissionInstanceId = O.MissionInstanceId;
    E.StationId = O.StationId;
    E.Leg = ActiveLeg();
    E.ObservationWorldSeconds = O.ObservationWorldSeconds;
    if (!bInitialized || !O.bKnown || !O.bCargoObserved
        || !FMath::IsFinite(O.ObservationWorldSeconds)
        || O.RobotSocketCm.ContainsNaN() || O.StationPointCm.ContainsNaN()
        || O.ActualVelocityCmPerSecond.ContainsNaN())
    {
        E.Reason = EDDSN08Denial::Unavailable;
        return E;
    }
    if (O.MissionInstanceId != State.MissionInstanceId
        || State.Phase == EDDSN08Phase::Completed
        || (State.Phase == EDDSN08Phase::AwaitingPickup && State.Owner != EDDSN08Owner::Station)
        || (State.Phase == EDDSN08Phase::Carrying && State.Owner != EDDSN08Owner::Robot)
        || O.ObservedCargoId != State.CargoId || !O.bActiveCargoAtExpectedSource)
    {
        E.Reason = EDDSN08Denial::WrongPhaseOrCargo;
        return E;
    }
    if (bHasOperation || bWaitingForClosedLids || Gate != EDDSN08Gate::None
        || !bStablePhaseAcknowledged)
    {
        E.Reason = EDDSN08Denial::BusyOrSaving;
        return E;
    }
    if (O.StationId != ActiveStationId() || !O.bGrounded
        || FVector::Dist(O.RobotSocketCm, O.StationPointCm) > 125.0
        || FMath::Abs(O.RobotSocketCm.Z - O.StationPointCm.Z) > 50.0)
    {
        E.Reason = EDDSN08Denial::WrongStationOrBay;
        return E;
    }
    if (!O.bServicePathObserved || !O.bServicePathClear
        || !O.bFullLidSweepObserved || !O.bFullLidSweepClear
        || !O.bClosedOperableDoorObserved || !O.bClosedOperableDoor)
    {
        E.Reason = EDDSN08Denial::ObstructedOrDoorUnavailable;
        return E;
    }
    // Actual 3-D displacement velocity from N06, including reverse, sideways
    // and vertical motion. Signed forward speed/throttle would undercount these.
    if (O.ActualVelocityCmPerSecond.Size() > 20.0)
    {
        E.Reason = EDDSN08Denial::Moving;
        return E;
    }
    if (!bNewPressEdge)
    {
        E.Reason = EDDSN08Denial::InputHeld;
        return E;
    }
    E.bAllowed = true;
    E.Reason = EDDSN08Denial::None;
    return E;
}

FDDSN08Eligibility FDDSN08Authority::EvaluateProspectivePrompt(
    const FDDSN08Observation& O) const
{
    return Evaluate(O, !bInteractDown);
}

FDDSN08Eligibility FDDSN08Authority::OnInteractButton(bool bPressed,
    const FDDSN08Observation& O)
{
    const bool bNewEdge = bPressed && !bInteractDown;
    bInteractDown = bPressed;
    FDDSN08Eligibility E = Evaluate(O, bNewEdge);
    if (!E.bAllowed) return E;

    Operation.MissionInstanceId = State.MissionInstanceId;
    Operation.OperationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Operation.Generation = NextGeneration++;
    Operation.Leg = ActiveLeg();
    Operation.PrePhase = State.Phase;
    Operation.StationId = ActiveStationId();
    bHasOperation = true; // Reserve before either external port can callback.
    Transient = Operation.Leg == EDDSN08Leg::Pickup
        ? EDDSN08Transient::PickingUp : EDDSN08Transient::Delivering;
    MechanicStep = EDDSN08MechanicStep::None;
    OperationGameplaySeconds = 0.0;
    ApplyDriveLock(true);
    Mechanics.BeginPhysicalCycle(Operation);
    return E;
}

bool FDDSN08Authority::ValidCurrentToken(const FDDSN08Token& Token) const
{
    return bHasOperation && Operation.Equals(Token)
        && Token.MissionInstanceId == State.MissionInstanceId
        && Token.PrePhase == State.Phase && Token.StationId == ActiveStationId()
        && Token.Leg == ActiveLeg();
}

void FDDSN08Authority::OnMechanicalProgress(const FDDSN08Token& Token,
    EDDSN08MechanicStep Step)
{
    // Progress is display-only. An animation callback cannot mutate cargo.
    if (!ValidCurrentToken(Token) || Step == EDDSN08MechanicStep::None) return;
    if (static_cast<uint8>(Step) >= static_cast<uint8>(MechanicStep))
        MechanicStep = Step;
}

bool FDDSN08Authority::HasTransferReceipt(const FDDSN08Token& Token) const
{
    return State.Receipts.ContainsByPredicate([&Token](const FDDSN08Receipt& R)
    {
        return R.MissionInstanceId == Token.MissionInstanceId && R.PrePhase == Token.PrePhase
            && R.StationId == Token.StationId;
    });
}

bool FDDSN08Authority::OnVerifiedLatch(const FDDSN08LatchEvidence& Evidence)
{
    const FDDSN08Token& T = Evidence.Token;
    if (!ValidCurrentToken(T) || HasTransferReceipt(T)
        || !Evidence.bMechanicalObservationKnown
        || !Evidence.bParcelStagedAtCorrectDestination
        || !Evidence.bFullOpeningAndClosingSweepClear
        || !Evidence.bLeftLidAtClosedPose || !Evidence.bRightLidAtClosedPose
        || !Evidence.bLeftLatchVerified || !Evidence.bRightLatchVerified)
        return false;

    // One game-thread action: latch check, receipt, owner/phase and frozen result.
    // No external callback runs until the coherent commit and gate are installed.
    FDDSN08Receipt Receipt;
    Receipt.MissionInstanceId = T.MissionInstanceId;
    Receipt.PrePhase = T.PrePhase;
    Receipt.StationId = T.StationId;
    Receipt.OperationId = T.OperationId;
    Receipt.Generation = T.Generation;
    Receipt.CargoId = State.CargoId;
    State.Receipts.Add(MoveTemp(Receipt));
    if (T.Leg == EDDSN08Leg::Pickup)
    {
        State.Phase = EDDSN08Phase::Carrying;
        State.Owner = EDDSN08Owner::Robot;
    }
    else
    {
        State.Phase = EDDSN08Phase::Completed;
        State.Owner = EDDSN08Owner::Recipient;
        State.CollectedStarIds.Sort();
        State.bHasFrozenResult = true;
        State.FrozenResult.ExactStarIds = State.CollectedStarIds;
        State.FrozenResult.Count = State.CollectedStarIds.Num();
        State.FrozenResult.FrozenElapsedSeconds = State.ElapsedGameplaySeconds;
    }
    bHasOperation = false;
    Transient = EDDSN08Transient::None;
    MechanicStep = EDDSN08MechanicStep::None;
    bStablePhaseAcknowledged = false;
    Gate = EDDSN08Gate::Pending;
    CommittedPayload = State;
    CommittedCorrelation = T;
    bHasCommittedPayload = true;
    bHasCandidate = false;
    ApplyDriveLock(true);
    PreparePinnedCandidate();
    return true;
}

bool FDDSN08Authority::PreparePinnedCandidate()
{
    if (!bHasCommittedPayload || bHasCandidate) return false;
    // Prepare must be side-effect-free. An unsolicited ack here cannot match a
    // candidate and is rejected. N11 must only callback from Submit or later.
    Candidate = Save.Prepare(CommittedPayload, CommittedCorrelation);
    bHasCandidate = Candidate.Correlation.Equals(CommittedCorrelation)
        && Candidate.Revision > 0 && !Candidate.Digest.IsEmpty()
        && SameStableSnapshot(Candidate.Snapshot, CommittedPayload);
    if (!bHasCandidate)
    {
        Gate = EDDSN08Gate::ProviderFault;
        return false;
    }
    return SubmitPinnedCandidate();
}

bool FDDSN08Authority::SubmitPinnedCandidate()
{
    if (!bHasCandidate) return false;
    Gate = EDDSN08Gate::Pending;
    const bool bQueued = Save.Submit(Candidate);
    // A permitted synchronous callback may have already cleared/failed Gate.
    if (!bQueued && Gate == EDDSN08Gate::Pending)
    {
        Gate = EDDSN08Gate::FailedRetryable;
    }
    return Gate == EDDSN08Gate::Pending || Gate == EDDSN08Gate::None;
}

void FDDSN08Authority::CancelPreLatch(const FDDSN08Token& Token)
{
    if (!ValidCurrentToken(Token)) return;
    ++NextGeneration; // Invalidate commit callbacks before calling mechanics.
    bHasOperation = false;
    Transient = EDDSN08Transient::None;
    MechanicStep = EDDSN08MechanicStep::None;
    bWaitingForClosedLids = true;
    CancelledToken = Token;
    OperationGameplaySeconds = 0.0;
    ApplyDriveLock(true);
    Mechanics.CancelAndClose(Token);
}

void FDDSN08Authority::OnMechanicalFailure(const FDDSN08Token& Token)
{
    CancelPreLatch(Token);
}

bool FDDSN08Authority::OnCancelledClosureVerified(const FDDSN08Token& Token,
    bool bBothLidsClosedAndLatched)
{
    if (!bWaitingForClosedLids || !CancelledToken.Equals(Token)
        || !bBothLidsClosedAndLatched) return false;
    bWaitingForClosedLids = false;
    ApplyDriveLock(false);
    return true;
}

bool FDDSN08Authority::OnSaveAck(const FDDSN08SaveAck& Ack)
{
    if (!bHasCandidate || (Gate != EDDSN08Gate::Pending
        && Gate != EDDSN08Gate::FailedRetryable)
        || !Ack.Correlation.Equals(Candidate.Correlation)
        || Ack.Revision != Candidate.Revision || Ack.Digest != Candidate.Digest)
        return false;
    if (!Ack.bDurablyValidated)
    {
        Gate = EDDSN08Gate::FailedRetryable;
        return true;
    }
    Gate = EDDSN08Gate::None;
    bHasCommittedPayload = false;
    bHasCandidate = false;
    bStablePhaseAcknowledged = true;
    ApplyDriveLock(false);
    return true;
}

bool FDDSN08Authority::RetryPinnedSave()
{
    if (Gate != EDDSN08Gate::FailedRetryable || !bHasCandidate) return false;
    return SubmitPinnedCandidate();
}

bool FDDSN08Authority::RetryProviderPreparation()
{
    if (Gate != EDDSN08Gate::ProviderFault || !bHasCommittedPayload || bHasCandidate)
        return false;
    return PreparePinnedCandidate();
}

EDDSN08Recovery FDDSN08Authority::RequestRecoveryPolicy(bool bBothLidsClosedLatched)
{
    if (Gate != EDDSN08Gate::None) return EDDSN08Recovery::PersistenceLocked;
    if (bHasOperation)
    {
        const FDDSN08Token ToCancel = Operation;
        CancelPreLatch(ToCancel);
        return EDDSN08Recovery::WaitForClosedLids;
    }
    if (bWaitingForClosedLids || !bBothLidsClosedLatched)
        return EDDSN08Recovery::WaitForClosedLids;
    // Caller may now invoke N06's bounded RequestRecovery with a real ledger.
    return EDDSN08Recovery::Allowed;
}

bool FDDSN08Authority::RecordN10Star(const FString& InstanceId, const FString& StarId)
{
    if (!bInitialized || InstanceId != State.MissionInstanceId
        || State.Phase == EDDSN08Phase::Completed || Gate != EDDSN08Gate::None
        || !State.ScopedStarIds.Contains(StarId)
        || State.CollectedStarIds.Contains(StarId)) return false;
    State.CollectedStarIds.Add(StarId);
    State.CollectedStarIds.Sort();
    return true;
}

FDDSN08Presentation FDDSN08Authority::GetPresentation() const
{
    FDDSN08Presentation P;
    P.StablePhase = State.Phase;
    P.Transient = Transient;
    P.SaveGate = Gate;
    P.bDriveLocked = bDriveLocked;
    if (!bInitialized) { P.Status = TEXT("Mission unavailable"); return P; }
    if (Gate == EDDSN08Gate::FailedRetryable)
        P.Status = TEXT("Committed | Save failed | Retry save");
    else if (Gate == EDDSN08Gate::ProviderFault)
        P.Status = TEXT("Committed | Save provider invalid | Repair and retry preparation");
    else if (Gate == EDDSN08Gate::Pending)
        P.Status = TEXT("Saving transfer");
    else if (bWaitingForClosedLids)
        P.Status = TEXT("Closing lids before drive/recovery");
    else if (bHasOperation)
        P.Status = TEXT("Transfer in progress");
    else if (State.Phase == EDDSN08Phase::Completed && bStablePhaseAcknowledged)
        P.Status = TEXT("Delivered");
    else
        P.Status = TEXT("Ready");
    // Keep the active leg's color through its physical transaction. Pickup
    // stays green during its save gate; red becomes active only after pickup
    // acknowledgment. Delivery stays red through its save gate and becomes
    // neutral only after the validated completion acknowledgment.
    P.bPickupBeacon = State.Phase == EDDSN08Phase::AwaitingPickup
        || (State.Phase == EDDSN08Phase::Carrying && !bStablePhaseAcknowledged
            && bHasCommittedPayload && CommittedCorrelation.Leg == EDDSN08Leg::Pickup);
    P.bDeliveryBeacon = (State.Phase == EDDSN08Phase::Carrying
            && bStablePhaseAcknowledged)
        || (State.Phase == EDDSN08Phase::Completed && !bStablePhaseAcknowledged
            && bHasCommittedPayload && CommittedCorrelation.Leg == EDDSN08Leg::Delivery);
    P.bShowDelivered = State.Phase == EDDSN08Phase::Completed
        && bStablePhaseAcknowledged && Gate == EDDSN08Gate::None;
    return P;
}
