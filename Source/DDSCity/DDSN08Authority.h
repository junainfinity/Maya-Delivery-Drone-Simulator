#pragma once

#include "CoreMinimal.h"

// Staged N08 authority. This is ordinary UE C++ (no UObject/UHT type) so a future
// coordinator can own one instance on the game thread. N09/N11/N12 own the real
// geometry, durable storage, and physical mechanisms behind these ports.

enum class EDDSN08Phase : uint8 { AwaitingPickup, Carrying, Completed };
enum class EDDSN08Owner : uint8 { Station, Robot, Recipient };
enum class EDDSN08Leg : uint8 { Pickup, Delivery };
enum class EDDSN08Transient : uint8 { None, PickingUp, Delivering };
enum class EDDSN08MechanicStep : uint8 { None, Opening, Open, Staged, Closing };
enum class EDDSN08Gate : uint8 { None, Pending, FailedRetryable, ProviderFault };
enum class EDDSN08Denial : uint8 {
    None, Unavailable, WrongPhaseOrCargo, WrongStationOrBay,
    ObstructedOrDoorUnavailable, Moving, InputHeld, BusyOrSaving
};
enum class EDDSN08Recovery : uint8 { Allowed, WaitForClosedLids, PersistenceLocked };

struct FDDSN08Token {
    FString MissionInstanceId;
    FString OperationId;
    uint64 Generation = 0;
    EDDSN08Leg Leg = EDDSN08Leg::Pickup;
    EDDSN08Phase PrePhase = EDDSN08Phase::AwaitingPickup;
    FString StationId;

    bool Equals(const FDDSN08Token& Other) const;
};

struct FDDSN08Observation {
    FString MissionInstanceId;
    FString StationId;
    bool bKnown = false;
    bool bCargoObserved = false;
    FString ObservedCargoId;
    // N12 reports station source for pickup or robot source for delivery.
    bool bActiveCargoAtExpectedSource = false;
    bool bGrounded = false;
    FVector RobotSocketCm = FVector::ZeroVector;
    FVector StationPointCm = FVector::ZeroVector;
    FVector ActualVelocityCmPerSecond = FVector::ZeroVector;
    bool bServicePathObserved = false;
    bool bServicePathClear = false;
    bool bFullLidSweepObserved = false;
    bool bFullLidSweepClear = false;
    bool bClosedOperableDoorObserved = false;
    bool bClosedOperableDoor = false;
    double ObservationWorldSeconds = 0.0;
};

struct FDDSN08Eligibility {
    bool bAllowed = false;
    EDDSN08Denial Reason = EDDSN08Denial::Unavailable;
    FString MissionInstanceId;
    FString StationId;
    EDDSN08Leg Leg = EDDSN08Leg::Pickup;
    double ObservationWorldSeconds = 0.0;
};

struct FDDSN08Receipt {
    FString MissionInstanceId;
    EDDSN08Phase PrePhase = EDDSN08Phase::AwaitingPickup;
    FString StationId;
    FString OperationId;
    uint64 Generation = 0;
    FString CargoId;
};

struct FDDSN08Result {
    TArray<FString> ExactStarIds; // Sorted, immutable after delivery latch.
    int32 Count = 0;
    double FrozenElapsedSeconds = 0.0;
};

// This is a logical stable payload only. N11 adds durable schema, ordered revision,
// digest, validation and pose. No transient door alpha, staged parcel or operation.
struct FDDSN08StableSnapshot {
    FString MissionInstanceId;
    FString CargoId;
    FString PickupStationId;
    FString DeliveryStationId;
    TArray<FString> ScopedStarIds;
    TArray<FString> CollectedStarIds;
    TArray<FDDSN08Receipt> Receipts;
    EDDSN08Phase Phase = EDDSN08Phase::AwaitingPickup;
    EDDSN08Owner Owner = EDDSN08Owner::Station;
    double ElapsedGameplaySeconds = 0.0;
    bool bHasFrozenResult = false;
    FDDSN08Result FrozenResult;
};

struct FDDSN08SaveCandidate {
    FDDSN08Token Correlation;
    FDDSN08StableSnapshot Snapshot;
    uint64 Revision = 0;
    FString Digest;
};

struct FDDSN08SaveAck {
    FDDSN08Token Correlation;
    uint64 Revision = 0;
    FString Digest;
    bool bDurablyValidated = false;
};

struct FDDSN08RestoreBarrier {
    FString MissionInstanceId;
    // N11 guarantees the requested checkpoint is now the coherent durable
    // head and every older queued write is cancelled, drained or superseded.
    bool bCheckpointDurablyCurrent = false;
    bool bOlderWritesCannotOvertake = false;
    uint64 FencedThroughRevision = 0;
};

struct FDDSN08LatchEvidence {
    FDDSN08Token Token;
    bool bMechanicalObservationKnown = false;
    bool bParcelStagedAtCorrectDestination = false;
    bool bFullOpeningAndClosingSweepClear = false;
    bool bLeftLidAtClosedPose = false;
    bool bRightLidAtClosedPose = false;
    bool bLeftLatchVerified = false;
    bool bRightLatchVerified = false;
};

struct FDDSN08Presentation {
    EDDSN08Phase StablePhase = EDDSN08Phase::AwaitingPickup;
    EDDSN08Transient Transient = EDDSN08Transient::None;
    EDDSN08Gate SaveGate = EDDSN08Gate::None;
    bool bPickupBeacon = false;
    bool bDeliveryBeacon = false;
    bool bShowDelivered = false;
    bool bDriveLocked = false;
    FString Status;
};

class IDDSN08MotionPort {
public:
    virtual ~IDDSN08MotionPort() = default;
    virtual void SetDriveLocked(bool bLocked) = 0;
};

class IDDSN08MechanicsPort {
public:
    virtual ~IDDSN08MechanicsPort() = default;
    virtual void BeginPhysicalCycle(const FDDSN08Token& Token) = 0;
    // Must clear staged visuals and close/restore lids; authority waits for a
    // separate tagged verified-closure callback before releasing drive/recovery.
    virtual void CancelAndClose(const FDDSN08Token& Token) = 0;
};

class IDDSN08SavePort {
public:
    virtual ~IDDSN08SavePort() = default;
    // N11 adapter assigns revision/digest and returns the same stable snapshot.
    virtual FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& Snapshot,
                                         const FDDSN08Token& Correlation) = 0;
    // Retry receives the exact original candidate; false means queue failure.
    virtual bool Submit(const FDDSN08SaveCandidate& Candidate) = 0;
    // Synchronous N11 barrier for explicit restore. It may cancel/drain older
    // writes or durably publish a newer superseding revision of the chosen
    // validated checkpoint. No old candidate may later become the disk head.
    virtual FDDSN08RestoreBarrier EstablishRestoreBarrier(
        const FDDSN08StableSnapshot& ValidatedCheckpoint,
        const FDDSN08SaveCandidate* PossiblyInFlightCandidate) = 0;
};

class FDDSN08Authority final {
public:
    FDDSN08Authority(IDDSN08MotionPort& InMotion, IDDSN08MechanicsPort& InMechanics,
                    IDDSN08SavePort& InSave);

    bool StartFreshMission(const FString& InstanceId, const FString& CargoId,
                           const FString& PickupStationId, const FString& DeliveryStationId,
                           const TArray<FString>& ThreeScopedStarIds);
    bool ReplayExplicit(const FString& NewInstanceId, const FString& NewCargoId,
                        const TArray<FString>& ThreeNewScopedStarIds);
    // N11 must validate checkpoint digest/schema/pose and closed lids before this.
    bool RestoreValidatedCheckpoint(const FDDSN08StableSnapshot& Snapshot,
                                    bool bN11Validated, bool bLidsClosedLatched,
                                    bool bFinitePoseValidated);

    void TickGameplay(double DeltaSeconds, bool bPaused);
    FDDSN08Eligibility Evaluate(const FDDSN08Observation& Observation,
                                bool bNewPressEdge) const;
    FDDSN08Eligibility EvaluateProspectivePrompt(const FDDSN08Observation& Observation) const;
    FDDSN08Eligibility OnInteractButton(bool bPressed, const FDDSN08Observation& Observation);
    void OnMechanicalProgress(const FDDSN08Token& Token, EDDSN08MechanicStep Step);
    bool OnVerifiedLatch(const FDDSN08LatchEvidence& Evidence);
    void OnMechanicalFailure(const FDDSN08Token& Token);
    bool OnCancelledClosureVerified(const FDDSN08Token& Token,
                                    bool bBothLidsClosedAndLatched);
    bool OnSaveAck(const FDDSN08SaveAck& Ack);
    bool RetryPinnedSave();
    // Only for a Prepare contract fault: no valid revision/digest was ever
    // created. Re-prepare the SAME immutable committed payload/key after repair.
    bool RetryProviderPreparation();
    EDDSN08Recovery RequestRecoveryPolicy(bool bBothLidsClosedLatched);
    bool RecordN10Star(const FString& InstanceId, const FString& StarId);

    FDDSN08StableSnapshot GetStableSnapshot() const { return State; }
    FDDSN08Presentation GetPresentation() const;
    EDDSN08Gate GetSaveGate() const { return Gate; }
    EDDSN08Transient GetTransient() const { return Transient; }
    EDDSN08MechanicStep GetMechanicStep() const { return MechanicStep; }
    bool IsDriveLocked() const { return bDriveLocked; }
    bool HasActiveOperation() const { return bHasOperation; }
    // Read-only cancellation wait; no physical closure claim or new transition.
    bool IsWaitingForClosedLids() const { return bWaitingForClosedLids; }
    bool HasPinnedCandidate() const { return bHasCandidate; }
    const FDDSN08Token& GetActiveToken() const { return Operation; }
    const FDDSN08SaveCandidate& GetPinnedCandidate() const { return Candidate; }

private:
    bool ValidCurrentToken(const FDDSN08Token& Token) const;
    bool ValidStableState(const FDDSN08StableSnapshot& Snapshot) const;
    bool SameStableSnapshot(const FDDSN08StableSnapshot& A,
                            const FDDSN08StableSnapshot& B) const;
    bool HasTransferReceipt(const FDDSN08Token& Token) const;
    void CancelPreLatch(const FDDSN08Token& Token);
    void ApplyDriveLock(bool bLocked);
    bool SubmitPinnedCandidate();
    bool PreparePinnedCandidate();
    FString ActiveStationId() const;
    EDDSN08Leg ActiveLeg() const;

    IDDSN08MotionPort& Motion;
    IDDSN08MechanicsPort& Mechanics;
    IDDSN08SavePort& Save;
    FDDSN08StableSnapshot State;
    bool bInitialized = false;
    bool bStablePhaseAcknowledged = false;
    bool bInteractDown = false;
    bool bDriveLocked = false;
    bool bHasOperation = false;
    FDDSN08Token Operation;
    EDDSN08Transient Transient = EDDSN08Transient::None;
    EDDSN08MechanicStep MechanicStep = EDDSN08MechanicStep::None;
    double OperationGameplaySeconds = 0.0;
    uint64 NextGeneration = 1;
    bool bWaitingForClosedLids = false;
    FDDSN08Token CancelledToken;
    EDDSN08Gate Gate = EDDSN08Gate::None;
    bool bHasCommittedPayload = false;
    FDDSN08StableSnapshot CommittedPayload;
    FDDSN08Token CommittedCorrelation;
    bool bHasCandidate = false;
    FDDSN08SaveCandidate Candidate;
};
