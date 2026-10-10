#include "DDSN15Presentation.h"

#include "DDSCityRollingPawn.h"

namespace
{
FString SafeName(const FString& Proposed, const TCHAR* Fallback)
{
    const FString Trimmed = Proposed.TrimStartAndEnd();
    return Trimmed.IsEmpty() ? FString(Fallback) : Trimmed;
}

FString TimeText(double Seconds)
{
    if (!FMath::IsFinite(Seconds) || Seconds < 0) return TEXT("Time unavailable");
    const int64 Whole = FMath::FloorToInt64(Seconds);
    return FString::Printf(TEXT("Time %02lld:%02lld"),
        static_cast<long long>(Whole / 60), static_cast<long long>(Whole % 60));
}
}

FDDSN15ViewModel FDDSN15Presentation::Build(const FDDSN08Authority& Authority,
    const ADDSCityRollingPawn& OwnedPawn, const FDDSN15ProjectionInputs& Inputs)
{
    FDDSN15PawnProjection Pawn;
    Pawn.CameraMode = OwnedPawn.GetCameraMode();
    Pawn.Telemetry = OwnedPawn.GetTelemetrySample();
    return Build(Authority, Pawn, Inputs);
}

FDDSN15ViewModel FDDSN15Presentation::Build(const FDDSN08Authority& Authority,
    const FDDSN15PawnProjection& Pawn, const FDDSN15ProjectionInputs& Inputs)
{
    const FDDSN08StableSnapshot State = Authority.GetStableSnapshot();
    const FDDSN08Presentation P = Authority.GetPresentation();
    const FDDSN07TelemetrySample& Telemetry = Pawn.Telemetry;
    FDDSN15ViewModel Out;
    const bool bHasMission = !State.MissionInstanceId.IsEmpty();
    const bool bAcknowledgedResult = bHasMission && P.bShowDelivered
        && P.SaveGate == EDDSN08Gate::None && State.bHasFrozenResult
        && State.Phase == EDDSN08Phase::Completed;
    const FString Pickup = SafeName(Inputs.PickupDisplayName, TEXT("Pickup point"));
    const FString Delivery = SafeName(Inputs.DeliveryDisplayName, TEXT("Delivery point"));
    Out.View = Pawn.CameraMode == EDDSN07CameraMode::Sensor
        ? TEXT("Sensor view") : TEXT("Chase view");
    Out.Cargo = !bHasMission ? TEXT("Cargo: Unavailable")
        : State.Owner == EDDSN08Owner::Robot ? TEXT("Cargo: Loaded")
        : TEXT("Cargo: Empty");
    const bool bFrozen = State.Phase == EDDSN08Phase::Completed
        && State.bHasFrozenResult;
    const int32 Count = bFrozen ? State.FrozenResult.Count
        : State.CollectedStarIds.Num();
    Out.Stars = !bHasMission ? TEXT("Route stars: Unavailable")
        : FString::Printf(TEXT("Route stars %d/3"), FMath::Clamp(Count, 0, 3));
    Out.Timer = !bHasMission ? TEXT("Time unavailable") : TimeText(bFrozen
        ? State.FrozenResult.FrozenElapsedSeconds : State.ElapsedGameplaySeconds);

    if (!bHasMission) Out.Objective = TEXT("Mission unavailable");
    else if (bAcknowledgedResult) Out.Objective = TEXT("Delivery complete");
    else if (State.Phase == EDDSN08Phase::Completed)
        Out.Objective = P.SaveGate == EDDSN08Gate::FailedRetryable
            ? TEXT("Delivery committed · Save failed")
            : P.SaveGate == EDDSN08Gate::ProviderFault
                ? TEXT("Delivery committed · Save unavailable")
            : TEXT("Delivery committed · Saving");
    else if (State.Phase == EDDSN08Phase::Carrying
        && P.SaveGate != EDDSN08Gate::None)
        Out.Objective = P.SaveGate == EDDSN08Gate::FailedRetryable
            ? TEXT("Pickup committed · Save failed")
            : P.SaveGate == EDDSN08Gate::ProviderFault
                ? TEXT("Pickup committed · Save unavailable")
            : TEXT("Pickup committed · Saving");
    else if (State.Phase == EDDSN08Phase::Carrying)
        Out.Objective = FString::Printf(TEXT("Deliver at %s"), *Delivery);
    else Out.Objective = FString::Printf(TEXT("Pick up at %s"), *Pickup);

    // Depends on the unapplied owner-bound N08 getter proposal. The getter
    // exposes existing cancellation state; it grants no new authority.
    const bool bWaitingForClosedLids = Authority.IsWaitingForClosedLids();
    const bool bCurrentDoorFacts = bHasMission && Inputs.bRealN12MechanicsAvailable
        && Inputs.bNativeDoorObservationKnown
        && Inputs.DoorObservationMissionId == State.MissionInstanceId
        && FMath::IsFinite(Inputs.NowWorldSeconds)
        && FMath::IsFinite(Inputs.DoorObservationWorldSeconds)
        && Inputs.DoorObservationWorldSeconds >= 0.0
        && Inputs.NowWorldSeconds >= Inputs.DoorObservationWorldSeconds
        && Inputs.NowWorldSeconds - Inputs.DoorObservationWorldSeconds <= 0.20;
    const bool bPhysicallyClosed = bCurrentDoorFacts
        && Inputs.bLeftLidAtClosedPose && Inputs.bRightLidAtClosedPose
        && Inputs.bLeftLatchVerified && Inputs.bRightLatchVerified;
    if (!bHasMission)
        Out.Door = TEXT("Cargo door: Unavailable");
    else if (bWaitingForClosedLids)
        Out.Door = TEXT("Closing cargo door · Waiting for verification");
    else if (!bCurrentDoorFacts)
        Out.Door = TEXT("Cargo door: Unavailable");
    else if (Authority.HasActiveOperation())
    {
        switch (Authority.GetMechanicStep())
        {
        case EDDSN08MechanicStep::Opening: Out.Door = TEXT("Opening cargo door…"); break;
        case EDDSN08MechanicStep::Open:
        case EDDSN08MechanicStep::Staged:
            Out.Door = P.Transient == EDDSN08Transient::Delivering
                ? TEXT("Unloading parcel…") : TEXT("Loading parcel…"); break;
        case EDDSN08MechanicStep::Closing: Out.Door = TEXT("Closing cargo door…"); break;
        default: Out.Door = TEXT("Cargo door: Waiting"); break;
        }
    }
    else Out.Door = bPhysicallyClosed ? TEXT("Cargo door: Closed")
        : TEXT("Cargo door: Not closed");

    if (!bHasMission) Out.Status = TEXT("Mission unavailable");
    else if (P.SaveGate == EDDSN08Gate::FailedRetryable)
        Out.Status = State.Phase == EDDSN08Phase::Completed
            ? TEXT("Delivery committed · Save failed · Retry save")
            : TEXT("Pickup committed · Save failed · Retry save");
    else if (P.SaveGate == EDDSN08Gate::ProviderFault)
        Out.Status = TEXT("Transfer committed · Save unavailable · Retry when ready");
    else if (P.SaveGate == EDDSN08Gate::Pending)
        Out.Status = State.Phase == EDDSN08Phase::Completed
            ? TEXT("Saving delivery…") : TEXT("Saving pickup…");
    else if (bWaitingForClosedLids)
        Out.Status = TEXT("Closing cargo door · Drive locked");
    else if (bAcknowledgedResult) Out.Status = TEXT("Delivered");
    else if (P.Transient != EDDSN08Transient::None)
        Out.Status = TEXT("Transfer in progress");
    else if (P.bDriveLocked) Out.Status = TEXT("Drive locked");
    else Out.Status = bPhysicallyClosed ? TEXT("Ready")
        : TEXT("Cargo door verification unavailable");

    Out.InteractionPrompt = Inputs.N09InteractionPrompt.IsEmpty()
        ? TEXT("Interact unavailable") : Inputs.N09InteractionPrompt;
    Out.bOfferRetrySave = P.SaveGate == EDDSN08Gate::FailedRetryable
        || P.SaveGate == EDDSN08Gate::ProviderFault;
    Out.bOfferRestore = bHasMission && P.SaveGate != EDDSN08Gate::None;
    Out.RestoreWarning = TEXT("Restore last saved checkpoint? Progress since that save will be lost.");
    Out.bShowResult = bAcknowledgedResult;
    Out.bOfferReplay = bAcknowledgedResult;
    if (bAcknowledgedResult)
    {
        Out.ResultTitle = TEXT("Delivered");
        Out.ResultDetail = FString::Printf(TEXT("%s · Route stars %d/3"),
            *TimeText(State.FrozenResult.FrozenElapsedSeconds),
            FMath::Clamp(State.FrozenResult.Count, 0, 3));
    }

    if (Pawn.CameraMode == EDDSN07CameraMode::Sensor)
    {
        Out.Speed = FMath::IsFinite(Telemetry.SignedSpeedMetersPerSecond)
            ? FString::Printf(TEXT("Speed: %+.2f m/s"),
                Telemetry.SignedSpeedMetersPerSecond)
            : TEXT("Speed: Unavailable");
        Out.FrontClearance = !Telemetry.bClearanceValid
            ? TEXT("Front clearance: Unavailable")
            : Telemetry.bClearBeyondRange ? TEXT("Front clearance: >8 m")
            : FMath::IsFinite(Telemetry.FrontClearanceMeters)
                ? FString::Printf(TEXT("Front clearance: %.2f m"),
                    Telemetry.FrontClearanceMeters)
                : TEXT("Front clearance: Unavailable");
    }
    // Chase keeps objective/count/cargo/timer at the same logical positions;
    // its detailed speed/clearance fields may be hidden by the later widget.
    return Out;
}

bool FDDSN15Presentation::IsIntentOffered(EDDSN15ActionIntent Intent,
    const FDDSN15ViewModel& Model)
{
    switch (Intent)
    {
    case EDDSN15ActionIntent::Pause:
    case EDDSN15ActionIntent::Resume:
    case EDDSN15ActionIntent::Quit: return true;
    case EDDSN15ActionIntent::RetrySave: return Model.bOfferRetrySave;
    case EDDSN15ActionIntent::RestoreCheckpoint: return Model.bOfferRestore;
    case EDDSN15ActionIntent::ReplayMission: return Model.bOfferReplay;
    }
    return false;
}
