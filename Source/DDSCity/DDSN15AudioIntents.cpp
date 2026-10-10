#include "DDSN15AudioIntents.h"

#include "DDSCityRollingPawn.h"

FDDSN15AudioIntent FDDSN15AudioRouter::OnAuthoritySaveAck(bool bN08Accepted,
    const FDDSN08SaveCandidate& Candidate, const FDDSN08SaveAck& Ack,
    const FDDSN08Presentation& Before,
    const FDDSN08StableSnapshot& AfterState,
    const FDDSN08Presentation& After)
{
    FDDSN15AudioIntent Intent;
    if (!bN08Accepted || !Ack.bDurablyValidated
        || (Before.SaveGate != EDDSN08Gate::Pending
            && Before.SaveGate != EDDSN08Gate::FailedRetryable)
        || After.SaveGate != EDDSN08Gate::None
        || !Ack.Correlation.Equals(Candidate.Correlation)
        || Ack.Revision == 0 || Ack.Revision != Candidate.Revision
        || Ack.Digest.IsEmpty() || Ack.Digest != Candidate.Digest
        || Candidate.Snapshot.MissionInstanceId.IsEmpty()
        || Candidate.Snapshot.MissionInstanceId != AfterState.MissionInstanceId
        || Candidate.Correlation.MissionInstanceId != AfterState.MissionInstanceId
        || Candidate.Snapshot.Phase != AfterState.Phase)
        return Intent;
    if (Candidate.Correlation.Leg == EDDSN08Leg::Pickup)
    {
        if (AfterState.Phase != EDDSN08Phase::Carrying
            || !After.bDeliveryBeacon || After.bShowDelivered) return Intent;
        Intent.Cue = EDDSN15AudioCue::PickupSaved;
    }
    else
    {
        if (AfterState.Phase != EDDSN08Phase::Completed
            || !After.bShowDelivered || !AfterState.bHasFrozenResult)
            return Intent;
        Intent.Cue = EDDSN15AudioCue::DeliverySaved;
    }
    const FString Key = Candidate.Correlation.MissionInstanceId + TEXT("|")
        + Candidate.Correlation.OperationId + TEXT("|")
        + FString::Printf(TEXT("%llu|%llu|"),
            static_cast<unsigned long long>(Candidate.Correlation.Generation),
            static_cast<unsigned long long>(Candidate.Revision)) + Candidate.Digest;
    if (Key == LastConfirmedAckKey) return FDDSN15AudioIntent();
    LastConfirmedAckKey = Key;
    return Intent;
}

FDDSN15AudioIntent FDDSN15AudioRouter::OnAuthorityStarAccepted(
    bool bN08Accepted, const FString& EventInstanceId,
    const FString& ScopedStarId, const FDDSN08StableSnapshot& AfterState)
{
    FDDSN15AudioIntent Intent;
    if (!bN08Accepted || EventInstanceId.IsEmpty() || ScopedStarId.IsEmpty()
        || EventInstanceId != AfterState.MissionInstanceId
        || AfterState.Phase == EDDSN08Phase::Completed
        || !AfterState.CollectedStarIds.Contains(ScopedStarId)) return Intent;
    const FString Key = EventInstanceId + TEXT("|") + ScopedStarId;
    if (Key == LastStarEffectKey) return Intent;
    LastStarEffectKey = Key;
    Intent.Cue = EDDSN15AudioCue::StarCollected;
    return Intent;
}

FDDSN15AudioIntent FDDSN15AudioRouter::OnAuthorityDeniedInteract(
    bool bNewPressEdge, const FDDSN08Eligibility& Eligibility) const
{
    FDDSN15AudioIntent Intent;
    if (bNewPressEdge && !Eligibility.bAllowed
        && Eligibility.Reason != EDDSN08Denial::None)
        Intent.Cue = EDDSN15AudioCue::DeniedInteract;
    return Intent;
}

FDDSN15AudioIntent FDDSN15AudioRouter::OnNativeN12StepObserved(
    const FDDSN08Token& Token, EDDSN08MechanicStep Step,
    bool bNativeN12Observation, const FDDSN08Authority& Authority)
{
    FDDSN15AudioIntent Intent;
    if (!bNativeN12Observation || !Authority.HasActiveOperation()
        || !Authority.GetActiveToken().Equals(Token)
        || Authority.GetMechanicStep() != Step) return Intent;
    switch (Step)
    {
    case EDDSN08MechanicStep::Opening:
        Intent.Cue = EDDSN15AudioCue::NativeDoorOpening; break;
    case EDDSN08MechanicStep::Staged:
        Intent.Cue = EDDSN15AudioCue::NativeParcelTransfer; break;
    case EDDSN08MechanicStep::Closing:
        Intent.Cue = EDDSN15AudioCue::NativeDoorClosing; break;
    default: return Intent;
    }
    const FString Key = Token.MissionInstanceId + TEXT("|") + Token.OperationId
        + FString::Printf(TEXT("|%llu|%d"),
            static_cast<unsigned long long>(Token.Generation), int32(Step));
    if (Key == LastNativeStepKey) return FDDSN15AudioIntent();
    LastNativeStepKey = Key;
    return Intent;
}

FDDSN15AudioIntent FDDSN15AudioRouter::OnNativeN12LatchObserved(
    const FDDSN08LatchEvidence& Evidence, bool bNativeN12Observation)
{
    FDDSN15AudioIntent Intent;
    if (!bNativeN12Observation || !Evidence.bMechanicalObservationKnown
        || !Evidence.bLeftLidAtClosedPose || !Evidence.bRightLidAtClosedPose
        || !Evidence.bLeftLatchVerified || !Evidence.bRightLatchVerified
        || Evidence.Token.MissionInstanceId.IsEmpty()
        || Evidence.Token.OperationId.IsEmpty()) return Intent;
    const FString Key = Evidence.Token.MissionInstanceId + TEXT("|")
        + Evidence.Token.OperationId + TEXT("|")
        + FString::Printf(TEXT("%llu"),
            static_cast<unsigned long long>(Evidence.Token.Generation));
    if (Key == LastLatchEffectKey) return Intent;
    LastLatchEffectKey = Key;
    Intent.Cue = EDDSN15AudioCue::NativeLatchClick;
    return Intent;
}

FDDSN15AudioIntent FDDSN15AudioRouter::UpdateMeasuredMotor(
    const FDDSMotionSample& Motion, double NowWorldSeconds, bool bPaused)
{
    FDDSN15AudioIntent Intent;
    const bool bFresh = FMath::IsFinite(NowWorldSeconds)
        && FMath::IsFinite(Motion.WorldSeconds)
        && Motion.WorldSeconds >= 0.0 && NowWorldSeconds >= Motion.WorldSeconds
        && NowWorldSeconds - Motion.WorldSeconds <= 0.20;
    const float SpeedCmPerSecond = Motion.ActualVelocityCmPerSecond.Size();
    // N06-owned, unapplied read-only sample proposal: completed absolute wheel
    // travel includes actual yaw at a stationary center, never requested steer.
    const float WheelSpeed = Motion.CompletedWheelSpeedCmPerSecond;
    const bool bWheelFacts = Motion.bCompletedWheelMotionMeasured
        && FMath::IsFinite(WheelSpeed) && WheelSpeed >= 0.0f;
    const float MeasuredSpeed = bWheelFacts
        ? FMath::Max(SpeedCmPerSecond, WheelSpeed) : SpeedCmPerSecond;
    const bool bMoving = !bPaused && bFresh && Motion.bGrounded
        && Motion.SupportedWheels >= 3 && !Motion.bBodyBlocked
        && !Motion.ActualVelocityCmPerSecond.ContainsNaN()
        && FMath::IsFinite(SpeedCmPerSecond)
        && !Motion.bBodyBlockedDuringCompletedWheelSample
        && FMath::IsFinite(MeasuredSpeed) && MeasuredSpeed > 3.0f;
    if (!bMoving)
    {
        if (bMotorWasPlaying) Intent.Cue = EDDSN15AudioCue::MotorStop;
        bMotorWasPlaying = false;
        return Intent;
    }
    bMotorWasPlaying = true;
    const float Level = FMath::Clamp(MeasuredSpeed / 180.0f, 0.0f, 1.0f);
    Intent.Cue = EDDSN15AudioCue::MotorUpdate;
    Intent.NormalizedMotorLevel = Level;
    Intent.NormalizedMotorPitch = 0.8f + 0.4f * Level;
    return Intent;
}
