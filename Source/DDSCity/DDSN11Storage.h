#pragma once

#include "CoreMinimal.h"
#include "DDSN08Authority.h"
#include "Templates/Function.h"

// Staged, uncompiled N11 source. Only the N08 authority owns mission state.
// This record reconstructs a closed/latching-safe checkpoint; a live pre-latch
// door may be open while the preceding stable owner/phase is saved.
struct FDDSN11Pose
{
    FVector LocationCm = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    bool bGroundedSidewalkValidated = false;
    bool bClosedLatchedReconstructionValidated = false;
    bool bOccupancyValidated = false;
};

struct FDDSN11Record
{
    uint64 Revision = 0;
    FString Digest;
    FDDSN08StableSnapshot Snapshot;
    FDDSN11Pose Pose;
};

enum class EDDSN11Load : uint8
{
    Found,
    NoRecord,
    ExplicitFreshRequired,
    ConflictingSameRevision,
    StorageReadError
};

class FDDSN11Codec final
{
public:
    static constexpr uint32 SchemaVersion = 1;
    static constexpr int32 MaximumRecordBytes = 1024 * 1024;

    static bool Validate(const FDDSN11Record& Record);
    static bool Encode(FDDSN11Record& Record, TArray<uint8>& OutBytes);
    static bool Decode(const TArray<uint8>& Bytes, FDDSN11Record& OutRecord);
    static bool SameLogicalSnapshot(const FDDSN08StableSnapshot& A,
                                    const FDDSN08StableSnapshot& B);
};

// The production implementation below is an ordinary UE file adapter. Tests
// inject an in-memory implementation, so staging never writes a real save.
class IDDSN11FileIO
{
public:
    virtual ~IDDSN11FileIO() = default;
    virtual bool Exists(const FString& Path) = 0;
    virtual bool Read(const FString& Path, TArray<uint8>& OutBytes) = 0;
    virtual bool Write(const FString& Path, const TArray<uint8>& Bytes) = 0;
    virtual bool MoveReplace(const FString& From, const FString& To) = 0;
    virtual bool EnsureDirectory(const FString& Directory) = 0;
};

class FDDSN11UnrealFileIO final : public IDDSN11FileIO
{
public:
    virtual bool Exists(const FString& Path) override;
    virtual bool Read(const FString& Path, TArray<uint8>& OutBytes) override;
    virtual bool Write(const FString& Path, const TArray<uint8>& Bytes) override;
    virtual bool MoveReplace(const FString& From, const FString& To) override;
    virtual bool EnsureDirectory(const FString& Directory) override;
};

// All operations are synchronous on one game-thread owner. There is no worker
// queue whose older write could race a restore barrier. Alternating slots keep
// the selected valid head until a replacement is read back and validated.
class FDDSN11SerializedStore final
{
public:
    FDDSN11SerializedStore(IDDSN11FileIO& InIO, const FString& InDirectory);
    void AuthorizeExplicitFreshMission() { bFreshMissionAuthorized = true; }
    EDDSN11Load LoadLatest(FDDSN11Record& OutRecord, int32* OutSlot = nullptr);
    bool NextRevision(uint64& OutRevision);
    bool Save(FDDSN11Record& Record);
    bool EstablishRestoreBarrier(const FDDSN08StableSnapshot& Checkpoint,
                                 const FDDSN08SaveCandidate* InFlight,
                                 const FDDSN11Pose& FreshlyValidatedRestorePose,
                                 FDDSN11Record& OutHead, uint64& OutFence);
    uint64 GetFence() const { return FencedThroughRevision; }
#if WITH_DEV_AUTOMATION_TESTS
    void SetFaultPointHook(TFunction<void(const TCHAR*)> InHook)
    {
        FaultPointHook = MoveTemp(InHook);
    }
#endif

private:
    enum class ESlotRead : uint8 { Missing, Valid, Corrupt, IOError };
    FString SlotPath(int32 Slot) const;
    FString TempPath() const;
    ESlotRead ReadSlot(int32 Slot, FDDSN11Record& OutRecord);
    bool Publish(FDDSN11Record& Record, int32 TargetSlot);
    IDDSN11FileIO& IO;
    FString Directory;
    uint64 FencedThroughRevision = 0;
    bool bFreshMissionAuthorized = false;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void(const TCHAR*)> FaultPointHook;
#endif
};

// N08 SavePort implementation. Set a validated immutable pose context before
// N08 calls OnVerifiedLatch; the robot stays braked until its save ack. Prepare
// reads but does not mutate storage or adapter state. Submit pins exact bytes
// for all retries. Bind the ack sink to the one live N08 authority on game
// thread; its lifetime and teardown are an integration obligation.
class FDDSN11SaveAdapter final : public IDDSN08SavePort
{
public:
    explicit FDDSN11SaveAdapter(FDDSN11SerializedStore& InStore);
    bool SetPoseContext(const FDDSN11Pose& Pose);
    // A separate current-world N06/N12 proof for one explicit restore attempt.
    // May be set while a failed commit remains pinned; never changes its bytes.
    // Every call first invalidates any older restore proof, including failure.
    bool SetRestorePoseContext(const FDDSN11Pose& Pose);
    void SetAckSink(TFunction<bool(const FDDSN08SaveAck&)> InSink);
    void ClearAckSink();
    virtual FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& Snapshot,
                                         const FDDSN08Token& Correlation) override;
    virtual bool Submit(const FDDSN08SaveCandidate& Candidate) override;
    virtual FDDSN08RestoreBarrier EstablishRestoreBarrier(
        const FDDSN08StableSnapshot& ValidatedCheckpoint,
        const FDDSN08SaveCandidate* PossiblyInFlightCandidate) override;

    // Explicit consumer restore: durable I/O first, then fresh current physical
    // admission. A rejection returns no barrier permission and retains exact
    // pinned candidate/record/pose; only the one-attempt restore context is consumed.
    FDDSN08RestoreBarrier EstablishRestoreBarrierWithCurrentAdmission(
        const FDDSN08StableSnapshot& ValidatedCheckpoint,
        const FDDSN08SaveCandidate* PossiblyInFlightCandidate,
        TFunctionRef<bool()> ReadmitCurrentPhysicalState);

    // Separate safe-point persistence entry for initial/collected-star progress
    // and pre-latch save requests. Caller supplies N08's stable snapshot plus a
    // separately validated CLOSED reconstruction pose; actual lids can be open.
    // Reject while a post-latch N08 candidate is pinned; never overtake it.
    bool SaveStableProgress(const FDDSN08StableSnapshot& Snapshot,
                            const FDDSN11Pose& ClosedReconstructionPose,
                            FDDSN11Record& OutValidatedRecord);

private:
    bool SameCandidate(const FDDSN08SaveCandidate& A,
                       const FDDSN08SaveCandidate& B) const;
    FDDSN11SerializedStore& Store;
    FDDSN11Pose PoseContext;
    bool bPoseContextSet = false;
    FDDSN11Pose RestorePoseContext;
    bool bRestorePoseContextSet = false;
    bool bPinned = false;
    FDDSN08SaveCandidate PinnedCandidate;
    FDDSN11Record PinnedRecord;
    TFunction<bool(const FDDSN08SaveAck&)> AckSink;
};
