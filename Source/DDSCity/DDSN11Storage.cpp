#include "DDSN11Storage.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Hash/Blake3.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace DDSN11
{
constexpr uint32 Magic = 0x31315344; // DS11, little endian on the Win64 target.
constexpr int32 MaxIdBytes = 512;

struct FWriter
{
    TArray<uint8> Bytes;
    bool bGood = true;
    void U8(uint8 V) { Bytes.Add(V); }
    void U32(uint32 V) { for (int32 I = 0; I != 4; ++I) U8(uint8(V >> (8 * I))); }
    void U64(uint64 V) { for (int32 I = 0; I != 8; ++I) U8(uint8(V >> (8 * I))); }
    void F64(double V)
    {
        uint64 Bits = 0;
        FMemory::Memcpy(&Bits, &V, sizeof(Bits));
        U64(Bits);
    }
    void String(const FString& S)
    {
        FTCHARToUTF8 U(*S);
        if (U.Length() < 0 || U.Length() > MaxIdBytes) { bGood = false; return; }
        U32(uint32(U.Length()));
        Bytes.Append(reinterpret_cast<const uint8*>(U.Get()), U.Length());
    }
};

struct FReader
{
    const TArray<uint8>& Bytes;
    int32 Offset = 0;
    bool bGood = true;
    explicit FReader(const TArray<uint8>& In) : Bytes(In) {}
    bool Has(int32 Count) const { return bGood && Count >= 0 && Offset <= Bytes.Num() - Count; }
    uint8 U8()
    {
        if (!Has(1)) { bGood = false; return 0; }
        return Bytes[Offset++];
    }
    uint32 U32()
    {
        uint32 V = 0;
        for (int32 I = 0; I != 4; ++I) V |= uint32(U8()) << (8 * I);
        return V;
    }
    uint64 U64()
    {
        uint64 V = 0;
        for (int32 I = 0; I != 8; ++I) V |= uint64(U8()) << (8 * I);
        return V;
    }
    double F64()
    {
        const uint64 Bits = U64();
        double V = 0;
        FMemory::Memcpy(&V, &Bits, sizeof(V));
        return V;
    }
    FString String()
    {
        const uint32 Length = U32();
        if (Length > uint32(MaxIdBytes) || !Has(int32(Length)))
        {
            bGood = false;
            return FString();
        }
        const uint8* Begin = Bytes.GetData() + Offset;
        FUTF8ToTCHAR Conversion(reinterpret_cast<const ANSICHAR*>(Begin), int32(Length));
        FString Result(Conversion.Length(), Conversion.Get());
        FTCHARToUTF8 RoundTrip(*Result);
        if (RoundTrip.Length() != int32(Length)
            || (Length && FMemory::Memcmp(RoundTrip.Get(), Begin, Length) != 0))
            bGood = false;
        Offset += int32(Length);
        return Result;
    }
};

void WriteStrings(FWriter& W, const TArray<FString>& Values)
{
    W.U8(uint8(Values.Num()));
    for (const FString& V : Values) W.String(V);
}

void ReadStrings(FReader& R, TArray<FString>& Values, uint8 Limit)
{
    const uint8 Count = R.U8();
    if (Count > Limit) { R.bGood = false; return; }
    for (uint8 I = 0; I < Count && R.bGood; ++I) Values.Add(R.String());
}

bool WritePayload(const FDDSN08StableSnapshot& S, const FDDSN11Pose& P,
                  TArray<uint8>& Out)
{
    FWriter W;
    W.String(S.MissionInstanceId);
    W.String(S.CargoId);
    W.String(S.PickupStationId);
    W.String(S.DeliveryStationId);
    W.U8(uint8(S.Phase));
    W.U8(uint8(S.Owner));
    W.F64(S.ElapsedGameplaySeconds);
    WriteStrings(W, S.ScopedStarIds);
    WriteStrings(W, S.CollectedStarIds);
    W.U8(uint8(S.Receipts.Num()));
    for (const FDDSN08Receipt& Receipt : S.Receipts)
    {
        W.String(Receipt.MissionInstanceId);
        W.U8(uint8(Receipt.PrePhase));
        W.String(Receipt.StationId);
        W.String(Receipt.OperationId);
        W.U64(Receipt.Generation);
        W.String(Receipt.CargoId);
    }
    W.U8(S.bHasFrozenResult ? 1 : 0);
    WriteStrings(W, S.FrozenResult.ExactStarIds);
    W.U32(uint32(S.FrozenResult.Count));
    W.F64(S.FrozenResult.FrozenElapsedSeconds);
    W.F64(P.LocationCm.X); W.F64(P.LocationCm.Y); W.F64(P.LocationCm.Z);
    W.F64(P.Rotation.Pitch); W.F64(P.Rotation.Yaw); W.F64(P.Rotation.Roll);
    // Only the validated closed reconstruction is stored. Live door state is
    // intentionally absent, including when a pre-latch request occurs mid-open.
    W.U8(1);
    if (!W.bGood || W.Bytes.Num() > FDDSN11Codec::MaximumRecordBytes) return false;
    Out = MoveTemp(W.Bytes);
    return true;
}

bool ReadPayload(const TArray<uint8>& Bytes, FDDSN08StableSnapshot& S, FDDSN11Pose& P)
{
    FReader R(Bytes);
    S.MissionInstanceId = R.String();
    S.CargoId = R.String();
    S.PickupStationId = R.String();
    S.DeliveryStationId = R.String();
    S.Phase = EDDSN08Phase(R.U8());
    S.Owner = EDDSN08Owner(R.U8());
    S.ElapsedGameplaySeconds = R.F64();
    ReadStrings(R, S.ScopedStarIds, 3);
    ReadStrings(R, S.CollectedStarIds, 3);
    const uint8 ReceiptCount = R.U8();
    if (ReceiptCount > 2) R.bGood = false;
    for (uint8 I = 0; I < ReceiptCount && R.bGood; ++I)
    {
        FDDSN08Receipt Receipt;
        Receipt.MissionInstanceId = R.String();
        Receipt.PrePhase = EDDSN08Phase(R.U8());
        Receipt.StationId = R.String();
        Receipt.OperationId = R.String();
        Receipt.Generation = R.U64();
        Receipt.CargoId = R.String();
        S.Receipts.Add(MoveTemp(Receipt));
    }
    const uint8 Frozen = R.U8();
    S.bHasFrozenResult = Frozen == 1;
    if (Frozen > 1) R.bGood = false;
    ReadStrings(R, S.FrozenResult.ExactStarIds, 3);
    S.FrozenResult.Count = int32(R.U32());
    S.FrozenResult.FrozenElapsedSeconds = R.F64();
    P.LocationCm.X = R.F64(); P.LocationCm.Y = R.F64(); P.LocationCm.Z = R.F64();
    P.Rotation.Pitch = R.F64(); P.Rotation.Yaw = R.F64(); P.Rotation.Roll = R.F64();
    const uint8 ClosedMarker = R.U8();
    P.bClosedLatchedReconstructionValidated = ClosedMarker == 1;
    // Ground/occupancy are fresh world queries, never self-certified by bytes.
    P.bGroundedSidewalkValidated = false;
    P.bOccupancyValidated = false;
    return R.bGood && R.Offset == Bytes.Num() && ClosedMarker == 1;
}

bool ValidSnapshot(const FDDSN08StableSnapshot& S)
{
    if (S.MissionInstanceId.IsEmpty() || S.CargoId.IsEmpty()
        || S.PickupStationId.IsEmpty() || S.DeliveryStationId.IsEmpty()
        || S.PickupStationId == S.DeliveryStationId
        || S.ScopedStarIds.Num() != 3 || S.CollectedStarIds.Num() > 3
        || S.Receipts.Num() > 2 || !FMath::IsFinite(S.ElapsedGameplaySeconds)
        || S.ElapsedGameplaySeconds < 0.0) return false;
    TArray<FString> Scoped = S.ScopedStarIds;
    Scoped.Sort();
    if (Scoped[0].IsEmpty() || Scoped[0] == Scoped[1] || Scoped[1] == Scoped[2]) return false;
    TArray<FString> Collected = S.CollectedStarIds;
    Collected.Sort();
    for (int32 I = 0; I < Collected.Num(); ++I)
        if (!Scoped.Contains(Collected[I]) || (I && Collected[I] == Collected[I-1])) return false;
    const int32 Phase = int32(S.Phase);
    if (Phase < 0 || Phase > 2 || int32(S.Owner) != Phase) return false;
    if (S.Receipts.Num() != Phase) return false;
    for (int32 I = 0; I < S.Receipts.Num(); ++I)
    {
        const FDDSN08Receipt& R = S.Receipts[I];
        if (R.MissionInstanceId != S.MissionInstanceId || R.CargoId != S.CargoId
            || R.OperationId.IsEmpty() || R.Generation == 0
            || int32(R.PrePhase) != I
            || R.StationId != (I == 0 ? S.PickupStationId : S.DeliveryStationId))
            return false;
    }
    if (S.bHasFrozenResult != (S.Phase == EDDSN08Phase::Completed)) return false;
    if (S.bHasFrozenResult)
    {
        TArray<FString> Exact = S.FrozenResult.ExactStarIds;
        Exact.Sort();
        if (Exact != Collected || S.FrozenResult.Count != Collected.Num()
            || S.FrozenResult.FrozenElapsedSeconds != S.ElapsedGameplaySeconds)
            return false;
    }
    else if (S.FrozenResult.Count != 0 || S.FrozenResult.ExactStarIds.Num() != 0
             || S.FrozenResult.FrozenElapsedSeconds != 0.0) return false;
    return true;
}

bool ValidCommittedCorrelation(const FDDSN08StableSnapshot& S, const FDDSN08Token& T)
{
    if (!ValidSnapshot(S) || T.MissionInstanceId != S.MissionInstanceId
        || T.OperationId.IsEmpty() || T.Generation == 0) return false;
    const bool bPickup = T.PrePhase == EDDSN08Phase::AwaitingPickup
        && T.Leg == EDDSN08Leg::Pickup && S.Phase == EDDSN08Phase::Carrying
        && T.StationId == S.PickupStationId;
    const bool bDelivery = T.PrePhase == EDDSN08Phase::Carrying
        && T.Leg == EDDSN08Leg::Delivery && S.Phase == EDDSN08Phase::Completed
        && T.StationId == S.DeliveryStationId;
    if (!bPickup && !bDelivery) return false;
    for (const FDDSN08Receipt& R : S.Receipts)
    {
        if (R.MissionInstanceId == T.MissionInstanceId
            && R.PrePhase == T.PrePhase && R.StationId == T.StationId
            && R.OperationId == T.OperationId && R.Generation == T.Generation
            && R.CargoId == S.CargoId) return true;
    }
    return false;
}

bool ValidPoseForWrite(const FDDSN11Pose& P)
{
    return P.bGroundedSidewalkValidated && P.bClosedLatchedReconstructionValidated
        && P.bOccupancyValidated && FMath::IsFinite(P.LocationCm.X)
        && FMath::IsFinite(P.LocationCm.Y) && FMath::IsFinite(P.LocationCm.Z)
        && FMath::IsFinite(P.Rotation.Pitch) && FMath::IsFinite(P.Rotation.Yaw)
        && FMath::IsFinite(P.Rotation.Roll);
}

bool DigestFor(uint64 Revision, const TArray<uint8>& Payload, FString& Out)
{
    FWriter W;
    W.U32(FDDSN11Codec::SchemaVersion);
    W.U64(Revision);
    W.Bytes.Append(Payload);
    // UE5.3.2 Core has a real BLAKE3 implementation. GenericPlatformMisc's
    // GetSHA256Signature is an unimplemented check(false) fallback here.
    Out = LexToString(FBlake3::HashBuffer(W.Bytes.GetData(), uint64(W.Bytes.Num())));
    return Out.Len() == 64;
}
} // namespace DDSN11

bool FDDSN11Codec::Validate(const FDDSN11Record& R)
{
    return R.Revision > 0 && DDSN11::ValidSnapshot(R.Snapshot)
        && DDSN11::ValidPoseForWrite(R.Pose);
}

bool FDDSN11Codec::Encode(FDDSN11Record& R, TArray<uint8>& OutBytes)
{
    if (!Validate(R)) return false;
    TArray<uint8> Payload;
    if (!DDSN11::WritePayload(R.Snapshot, R.Pose, Payload)
        || !DDSN11::DigestFor(R.Revision, Payload, R.Digest)) return false;
    DDSN11::FWriter W;
    W.U32(DDSN11::Magic);
    W.U32(SchemaVersion);
    W.U64(R.Revision);
    W.U32(uint32(Payload.Num()));
    for (int32 I = 0; I < 32; ++I)
    {
        const TCHAR* Hex = *R.Digest + 2*I;
        const int32 High = FParse::HexDigit(Hex[0]);
        const int32 Low = FParse::HexDigit(Hex[1]);
        W.U8(uint8((High << 4) | Low));
    }
    W.Bytes.Append(Payload);
    if (W.Bytes.Num() > MaximumRecordBytes) return false;
    OutBytes = MoveTemp(W.Bytes);
    return true;
}

bool FDDSN11Codec::Decode(const TArray<uint8>& Bytes, FDDSN11Record& OutRecord)
{
    if (Bytes.Num() < 52 || Bytes.Num() > MaximumRecordBytes) return false;
    DDSN11::FReader R(Bytes);
    if (R.U32() != DDSN11::Magic || R.U32() != SchemaVersion) return false;
    FDDSN11Record Decoded;
    Decoded.Revision = R.U64();
    const uint32 Length = R.U32();
    if (Length != uint32(Bytes.Num() - 52)) return false;
    uint8 ExpectedDigest[32];
    for (uint8& B : ExpectedDigest) B = R.U8();
    if (!R.bGood || !R.Has(int32(Length))) return false;
    TArray<uint8> Payload;
    Payload.Append(Bytes.GetData() + R.Offset, int32(Length));
    if (!DDSN11::ReadPayload(Payload, Decoded.Snapshot, Decoded.Pose)
        || !DDSN11::ValidSnapshot(Decoded.Snapshot) || Decoded.Revision == 0) return false;
    // Ground/occupancy remain false on a loaded record until an actual world
    // provider revalidates them. Only a temporary copy is used for canonical
    // byte comparison; that copy is never returned as live pose evidence.
    FDDSN11Record Canonical = Decoded;
    Canonical.Pose.bGroundedSidewalkValidated = true;
    Canonical.Pose.bOccupancyValidated = true;
    if (!Validate(Canonical)) return false;
    FString Digest;
    if (!DDSN11::DigestFor(Decoded.Revision, Payload, Digest)) return false;
    for (int32 I = 0; I < 32; ++I)
    {
        const TCHAR* Hex = *Digest + 2*I;
        const uint8 B = uint8((FParse::HexDigit(Hex[0]) << 4) | FParse::HexDigit(Hex[1]));
        if (ExpectedDigest[I] != B) return false;
    }
    TArray<uint8> RoundTrip;
    if (!Encode(Canonical, RoundTrip) || RoundTrip != Bytes) return false;
    Decoded.Digest = Digest;
    OutRecord = MoveTemp(Decoded);
    return true;
}

bool FDDSN11Codec::SameLogicalSnapshot(const FDDSN08StableSnapshot& A,
                                       const FDDSN08StableSnapshot& B)
{
    // Use the versioned field order rather than relying on struct padding.
    FDDSN11Pose Dummy;
    TArray<uint8> AB, BB;
    return DDSN11::WritePayload(A, Dummy, AB)
        && DDSN11::WritePayload(B, Dummy, BB) && AB == BB;
}

bool FDDSN11UnrealFileIO::Exists(const FString& Path)
{
    return IFileManager::Get().FileExists(*Path);
}
bool FDDSN11UnrealFileIO::Read(const FString& Path, TArray<uint8>& OutBytes)
{
    return FFileHelper::LoadFileToArray(OutBytes, *Path);
}
bool FDDSN11UnrealFileIO::Write(const FString& Path, const TArray<uint8>& Bytes)
{
    return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), Bytes.Num()), *Path);
}
bool FDDSN11UnrealFileIO::MoveReplace(const FString& From, const FString& To)
{
    return IFileManager::Get().Move(*To, *From, true, true);
}
bool FDDSN11UnrealFileIO::EnsureDirectory(const FString& Directory)
{
    return IFileManager::Get().DirectoryExists(*Directory)
        || IFileManager::Get().MakeDirectory(*Directory, true);
}

FDDSN11SerializedStore::FDDSN11SerializedStore(IDDSN11FileIO& InIO,
                                               const FString& InDirectory)
    : IO(InIO), Directory(InDirectory) {}

FString FDDSN11SerializedStore::SlotPath(int32 Slot) const
{
    return FPaths::Combine(Directory, Slot == 0 ? TEXT("checkpoint-a.dds11")
                                                : TEXT("checkpoint-b.dds11"));
}
FString FDDSN11SerializedStore::TempPath() const
{
    return FPaths::Combine(Directory, TEXT("checkpoint-pending.tmp"));
}
FDDSN11SerializedStore::ESlotRead FDDSN11SerializedStore::ReadSlot(
    int32 Slot, FDDSN11Record& OutRecord)
{
    const FString Path = SlotPath(Slot);
    if (!IO.Exists(Path)) return ESlotRead::Missing;
    TArray<uint8> Bytes;
    if (!IO.Read(Path, Bytes)) return ESlotRead::IOError;
    return FDDSN11Codec::Decode(Bytes, OutRecord) ? ESlotRead::Valid : ESlotRead::Corrupt;
}
EDDSN11Load FDDSN11SerializedStore::LoadLatest(FDDSN11Record& OutRecord, int32* OutSlot)
{
    FDDSN11Record A, B;
    const ESlotRead AStatus = ReadSlot(0, A);
    const ESlotRead BStatus = ReadSlot(1, B);
    if (AStatus == ESlotRead::IOError || BStatus == ESlotRead::IOError)
        return EDDSN11Load::StorageReadError;
    const bool bA = AStatus == ESlotRead::Valid;
    const bool bB = BStatus == ESlotRead::Valid;
    if (!bA && !bB)
        return AStatus != ESlotRead::Missing || BStatus != ESlotRead::Missing
            ? EDDSN11Load::ExplicitFreshRequired : EDDSN11Load::NoRecord;
    if (bA && bB && A.Revision == B.Revision && A.Digest != B.Digest)
        return EDDSN11Load::ConflictingSameRevision;
    const int32 Slot = !bB || (bA && A.Revision >= B.Revision) ? 0 : 1;
    OutRecord = Slot == 0 ? A : B;
    if (OutSlot) *OutSlot = Slot;
    return EDDSN11Load::Found;
}
bool FDDSN11SerializedStore::NextRevision(uint64& OutRevision)
{
    FDDSN11Record Head;
    const EDDSN11Load State = LoadLatest(Head);
    if ((State == EDDSN11Load::NoRecord || State == EDDSN11Load::ExplicitFreshRequired)
        && bFreshMissionAuthorized && FencedThroughRevision < MAX_uint64)
    {
        OutRevision = FencedThroughRevision + 1;
        return true;
    }
    if (State != EDDSN11Load::Found) return false;
    const uint64 Floor = FMath::Max(Head.Revision, FencedThroughRevision);
    if (Floor == MAX_uint64) return false;
    OutRevision = Floor + 1;
    return true;
}
bool FDDSN11SerializedStore::Publish(FDDSN11Record& Record, int32 TargetSlot)
{
    TArray<uint8> Bytes;
    if (!FDDSN11Codec::Encode(Record, Bytes) || !IO.EnsureDirectory(Directory)) return false;
    const FString Pending = TempPath();
    if (!IO.Write(Pending, Bytes)) return false;
    TArray<uint8> VerifyTemp;
    FDDSN11Record TempRecord;
    if (!IO.Read(Pending, VerifyTemp) || VerifyTemp != Bytes
        || !FDDSN11Codec::Decode(VerifyTemp, TempRecord)
        || TempRecord.Revision != Record.Revision || TempRecord.Digest != Record.Digest)
        return false;
#if WITH_DEV_AUTOMATION_TESTS
    if (FaultPointHook) FaultPointHook(TEXT("after-temp-validated"));
#endif
    if (!IO.MoveReplace(Pending, SlotPath(TargetSlot))) return false;
#if WITH_DEV_AUTOMATION_TESTS
    if (FaultPointHook) FaultPointHook(TEXT("after-move-before-readback"));
#endif
    FDDSN11Record Readback;
    if (ReadSlot(TargetSlot, Readback) != ESlotRead::Valid
        || Readback.Revision != Record.Revision
        || Readback.Digest != Record.Digest
        || !FDDSN11Codec::SameLogicalSnapshot(Readback.Snapshot, Record.Snapshot)) return false;
    FencedThroughRevision = FMath::Max(FencedThroughRevision, Record.Revision);
    bFreshMissionAuthorized = false;
    return true;
}
bool FDDSN11SerializedStore::Save(FDDSN11Record& Record)
{
    FDDSN11Record Head;
    int32 HeadSlot = -1;
    const EDDSN11Load State = LoadLatest(Head, &HeadSlot);
    if (State == EDDSN11Load::ConflictingSameRevision
        || State == EDDSN11Load::StorageReadError) return false;
    if (State == EDDSN11Load::Found && Record.Revision == Head.Revision)
    {
        // Exact retry after a lost callback must validate the caller's full
        // pose/record, derive its digest independently, and compare the exact
        // encoded bytes with the already validated selected disk head.
        FDDSN11Record Candidate = Record;
        TArray<uint8> CandidateBytes, HeadBytes;
        if (!FDDSN11Codec::Encode(Candidate, CandidateBytes)
            || Candidate.Digest != Record.Digest
            || Candidate.Digest != Head.Digest
            || !IO.Read(SlotPath(HeadSlot), HeadBytes)) return false;
        return CandidateBytes == HeadBytes;
    }
    if (Record.Revision <= FencedThroughRevision
        || (State == EDDSN11Load::Found && Record.Revision <= Head.Revision)) return false;
    if (State != EDDSN11Load::Found && !bFreshMissionAuthorized) return false;
    const uint64 Floor = FMath::Max(FencedThroughRevision,
        State == EDDSN11Load::Found ? Head.Revision : uint64(0));
    if (Floor == MAX_uint64 || Record.Revision != Floor + 1) return false;
    return Publish(Record, HeadSlot == 0 ? 1 : 0);
}
bool FDDSN11SerializedStore::EstablishRestoreBarrier(
    const FDDSN08StableSnapshot& Checkpoint, const FDDSN08SaveCandidate* InFlight,
    const FDDSN11Pose& FreshlyValidatedRestorePose,
    FDDSN11Record& OutHead, uint64& OutFence)
{
    if (!DDSN11::ValidPoseForWrite(FreshlyValidatedRestorePose)) return false;
    FDDSN11Record Head, A, B;
    int32 HeadSlot = -1;
    if (LoadLatest(Head, &HeadSlot) != EDDSN11Load::Found) return false;
    const ESlotRead AStatus = ReadSlot(0, A);
    const ESlotRead BStatus = ReadSlot(1, B);
    if (AStatus == ESlotRead::IOError || BStatus == ESlotRead::IOError) return false;
    const bool bA = AStatus == ESlotRead::Valid
        && FDDSN11Codec::SameLogicalSnapshot(A.Snapshot, Checkpoint);
    const bool bB = BStatus == ESlotRead::Valid
        && FDDSN11Codec::SameLogicalSnapshot(B.Snapshot, Checkpoint);
    if (!bA && !bB) return false;
    int32 ChosenSlot = !bB || (bA && A.Revision >= B.Revision) ? 0 : 1;
    FDDSN11Record Chosen = ChosenSlot == 0 ? A : B;
    const bool bPoseChanged = Chosen.Pose.LocationCm != FreshlyValidatedRestorePose.LocationCm
        || Chosen.Pose.Rotation != FreshlyValidatedRestorePose.Rotation;
    Chosen.Pose = FreshlyValidatedRestorePose;
    uint64 Floor = FMath::Max(FencedThroughRevision, Head.Revision);
    if (InFlight) Floor = FMath::Max(Floor, InFlight->Revision);
    if (Floor == MAX_uint64) return false;
    if (ChosenSlot != HeadSlot || bPoseChanged)
    {
        Chosen.Revision = Floor + 1;
        // Overwrite the *other* (newer) slot, preserving the chosen valid
        // checkpoint if temp/move/readback fails.
        if (!Publish(Chosen, 1 - ChosenSlot)) return false;
        if (LoadLatest(Head, &HeadSlot) != EDDSN11Load::Found
            || Head.Revision != Chosen.Revision
            || !FDDSN11Codec::SameLogicalSnapshot(Head.Snapshot, Checkpoint)) return false;
    }
    else if (!FDDSN11Codec::SameLogicalSnapshot(Head.Snapshot, Checkpoint)) return false;
    // No async writer exists in this provider. Later Submit with the old
    // revision is rejected by this fence; a restart has no queued operation.
    FencedThroughRevision = FMath::Max(FencedThroughRevision, Floor);
    OutFence = FencedThroughRevision;
    OutHead = Head;
    return true;
}

FDDSN11SaveAdapter::FDDSN11SaveAdapter(FDDSN11SerializedStore& InStore) : Store(InStore) {}
bool FDDSN11SaveAdapter::SetPoseContext(const FDDSN11Pose& Pose)
{
    if (!IsInGameThread() || bPinned) return false;
    if (!DDSN11::ValidPoseForWrite(Pose)) return false;
    PoseContext = Pose;
    bPoseContextSet = true;
    return true;
}
bool FDDSN11SaveAdapter::SetRestorePoseContext(const FDDSN11Pose& Pose)
{
    // No cached prior validation can survive a rejected setter. The next
    // barrier must receive a fresh valid context from the current world.
    bRestorePoseContextSet = false;
    if (!IsInGameThread() || !DDSN11::ValidPoseForWrite(Pose)) return false;
    RestorePoseContext = Pose;
    bRestorePoseContextSet = true;
    return true;
}
void FDDSN11SaveAdapter::SetAckSink(TFunction<bool(const FDDSN08SaveAck&)> InSink)
{
    AckSink = MoveTemp(InSink);
}
void FDDSN11SaveAdapter::ClearAckSink() { AckSink = nullptr; }
FDDSN08SaveCandidate FDDSN11SaveAdapter::Prepare(
    const FDDSN08StableSnapshot& Snapshot, const FDDSN08Token& Correlation)
{
    FDDSN08SaveCandidate Candidate;
    if (!IsInGameThread() || bPinned || !bPoseContextSet
        || !DDSN11::ValidCommittedCorrelation(Snapshot, Correlation)) return Candidate;
    uint64 Revision = 0;
    if (!Store.NextRevision(Revision)) return Candidate;
    FDDSN11Record Record;
    Record.Revision = Revision;
    Record.Snapshot = Snapshot;
    Record.Pose = PoseContext;
    TArray<uint8> Encoded;
    if (!FDDSN11Codec::Encode(Record, Encoded)) return Candidate;
    Candidate.Correlation = Correlation;
    Candidate.Snapshot = Snapshot;
    Candidate.Revision = Revision;
    Candidate.Digest = Record.Digest;
    return Candidate;
}
bool FDDSN11SaveAdapter::SameCandidate(const FDDSN08SaveCandidate& A,
                                      const FDDSN08SaveCandidate& B) const
{
    return A.Correlation.Equals(B.Correlation) && A.Revision == B.Revision
        && A.Digest == B.Digest
        && FDDSN11Codec::SameLogicalSnapshot(A.Snapshot, B.Snapshot);
}
bool FDDSN11SaveAdapter::Submit(const FDDSN08SaveCandidate& Candidate)
{
    if (!IsInGameThread() || !AckSink || Candidate.Revision == 0
        || Candidate.Digest.IsEmpty()
        || !DDSN11::ValidCommittedCorrelation(Candidate.Snapshot,
                                              Candidate.Correlation)) return false;
    if (!bPinned)
    {
        if (!bPoseContextSet || Candidate.Revision <= Store.GetFence()) return false;
        FDDSN11Record Proposed;
        Proposed.Revision = Candidate.Revision;
        Proposed.Snapshot = Candidate.Snapshot;
        Proposed.Pose = PoseContext;
        TArray<uint8> Encoded;
        if (!FDDSN11Codec::Encode(Proposed, Encoded)
            || Proposed.Digest != Candidate.Digest) return false;
        PinnedCandidate = Candidate;
        PinnedRecord = MoveTemp(Proposed);
        bPinned = true;
    }
    if (!SameCandidate(Candidate, PinnedCandidate)) return false;
    const bool bValidated = Store.Save(PinnedRecord);
    FDDSN08SaveAck Ack;
    Ack.Correlation = Candidate.Correlation;
    Ack.Revision = Candidate.Revision;
    Ack.Digest = Candidate.Digest;
    Ack.bDurablyValidated = bValidated; // Readback-validated; not a power-loss guarantee.
    const bool bReceived = AckSink(Ack);
    if (bValidated && bReceived)
    {
        bPinned = false;
        bPoseContextSet = false;
        bRestorePoseContextSet = false;
    }
    return true; // Accepted synchronous attempt, including a reported failure.
}
FDDSN08RestoreBarrier FDDSN11SaveAdapter::EstablishRestoreBarrier(
    const FDDSN08StableSnapshot& ValidatedCheckpoint,
    const FDDSN08SaveCandidate* PossiblyInFlightCandidate)
{
    // Preserve the original provider contract for its existing callers.
    return EstablishRestoreBarrierWithCurrentAdmission(ValidatedCheckpoint,
        PossiblyInFlightCandidate, []() { return true; });
}
FDDSN08RestoreBarrier FDDSN11SaveAdapter::EstablishRestoreBarrierWithCurrentAdmission(
    const FDDSN08StableSnapshot& ValidatedCheckpoint,
    const FDDSN08SaveCandidate* PossiblyInFlightCandidate,
    TFunctionRef<bool()> ReadmitCurrentPhysicalState)
{
    FDDSN08RestoreBarrier Result;
    if (!IsInGameThread())
    {
        bRestorePoseContextSet = false;
        return Result;
    }
    if (!bRestorePoseContextSet) return Result;
    const FDDSN11Pose OneAttemptPose = RestorePoseContext;
    bRestorePoseContextSet = false; // Consume on both success and failure.
    FDDSN11Record Head;
    uint64 Fence = 0;
    if (!Store.EstablishRestoreBarrier(ValidatedCheckpoint,
        PossiblyInFlightCandidate, OneAttemptPose, Head, Fence)) return Result;
    // No success fields, pin release or ACK precedes the consumer's CURRENT
    // post-I/O admission. Durable head/fence facts remain truthful on refusal.
    if (!ReadmitCurrentPhysicalState()) return Result;
    Result.MissionInstanceId = ValidatedCheckpoint.MissionInstanceId;
    Result.bCheckpointDurablyCurrent = true; // Readback-valid provider head only.
    Result.bOlderWritesCannotOvertake = true; // Single writer, revision fence.
    Result.FencedThroughRevision = Fence;
    bPinned = false;
    bPoseContextSet = false;
    return Result;
}
bool FDDSN11SaveAdapter::SaveStableProgress(
    const FDDSN08StableSnapshot& Snapshot, const FDDSN11Pose& ClosedReconstructionPose,
    FDDSN11Record& OutValidatedRecord)
{
    if (!IsInGameThread() || bPinned || !DDSN11::ValidPoseForWrite(ClosedReconstructionPose))
        return false;
    uint64 Revision = 0;
    if (!Store.NextRevision(Revision)) return false;
    FDDSN11Record Record;
    Record.Revision = Revision;
    Record.Snapshot = Snapshot;
    Record.Pose = ClosedReconstructionPose;
    if (!Store.Save(Record)) return false;
    OutValidatedRecord = MoveTemp(Record);
    return true;
}
