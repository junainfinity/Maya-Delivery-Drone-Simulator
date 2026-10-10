#pragma once

#include "CoreMinimal.h"
#include "DDSN08Authority.h"

struct FDDSMotionSample;

enum class EDDSN15AudioCue : uint8
{
    None,
    MotorUpdate,
    MotorStop,
    NativeDoorOpening,
    NativeParcelTransfer,
    NativeDoorClosing,
    NativeLatchClick,
    DeniedInteract,
    StarCollected,
    PickupSaved,
    DeliverySaved
};

struct FDDSN15AudioIntent final
{
    EDDSN15AudioCue Cue = EDDSN15AudioCue::None;
    float NormalizedMotorLevel = 0.0f;
    float NormalizedMotorPitch = 0.0f;
};

// Produces intents only. N15's future audio component must bind actual shipped
// cues and user volume; no sound asset or playback is claimed here.
class FDDSN15AudioRouter final
{
public:
    // Call only at the live N08.OnSaveAck boundary, after it returns. A state
    // projection/reload never calls this and therefore cannot replay a tone.
    FDDSN15AudioIntent OnAuthoritySaveAck(bool bN08Accepted,
        const FDDSN08SaveCandidate& Candidate, const FDDSN08SaveAck& Ack,
        const FDDSN08Presentation& Before,
        const FDDSN08StableSnapshot& AfterState,
        const FDDSN08Presentation& After);

    // Call only when N08.RecordN10Star returned true for a real N10 contact.
    FDDSN15AudioIntent OnAuthorityStarAccepted(bool bN08Accepted,
        const FString& EventInstanceId, const FString& ScopedStarId,
        const FDDSN08StableSnapshot& AfterState);

    FDDSN15AudioIntent OnAuthorityDeniedInteract(bool bNewPressEdge,
        const FDDSN08Eligibility& Eligibility) const;

    // N12's real step observation must also have reached the active N08
    // operation. A controlled phase adapter cannot authorize production cues.
    FDDSN15AudioIntent OnNativeN12StepObserved(
        const FDDSN08Token& Token, EDDSN08MechanicStep Step,
        bool bNativeN12Observation, const FDDSN08Authority& Authority);

    // Physical click is independent of save success. Only the real N12
    // observer may call this after verifying both lids at the closed pose.
    FDDSN15AudioIntent OnNativeN12LatchObserved(
        const FDDSN08LatchEvidence& Evidence, bool bNativeN12Observation);

    // Actual N06 completed wheel travel/velocity/contact is sampled freshly; throttle input and the
    // N07 controlled phase/audio adapter are never motor audio providers.
    FDDSN15AudioIntent UpdateMeasuredMotor(const FDDSMotionSample& Motion,
        double NowWorldSeconds, bool bPaused);

    FDDSN15AudioIntent StopForTeardown()
    {
        return StopForPauseOrRecovery();
    }

    // Must be called at the immediate pause/recovery event before pawn ticking
    // stops, and applied to the real playback component in that same event.
    FDDSN15AudioIntent StopForPauseOrRecovery()
    {
        FDDSN15AudioIntent Intent;
        if (bMotorWasPlaying) Intent.Cue = EDDSN15AudioCue::MotorStop;
        bMotorWasPlaying = false;
        return Intent;
    }

private:
    FString LastConfirmedAckKey; // ephemeral effect dedupe, not a receipt ledger
    FString LastStarEffectKey;
    FString LastNativeStepKey;
    FString LastLatchEffectKey;
    bool bMotorWasPlaying = false;
};
