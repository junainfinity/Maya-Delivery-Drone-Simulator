#include "DDSN11Storage.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DDSN11Test
{
struct FMemoryFiles final : IDDSN11FileIO
{
    TMap<FString, TArray<uint8>> Files;
    bool bFailNextWrite = false;
    bool bFailNextMove = false;
    bool bCorruptAfterMove = false;
    FString FailReadPath;

    virtual bool Exists(const FString& Path) override { return Files.Contains(Path); }
    virtual bool Read(const FString& Path, TArray<uint8>& Out) override
    {
        if (Path == FailReadPath) return false;
        if (const TArray<uint8>* Bytes = Files.Find(Path)) { Out = *Bytes; return true; }
        return false;
    }
    virtual bool Write(const FString& Path, const TArray<uint8>& Bytes) override
    {
        if (bFailNextWrite) { bFailNextWrite = false; return false; }
        Files.Add(Path, Bytes);
        return true;
    }
    virtual bool MoveReplace(const FString& From, const FString& To) override
    {
        if (bFailNextMove) { bFailNextMove = false; return false; }
        TArray<uint8>* Bytes = Files.Find(From);
        if (!Bytes) return false;
        Files.Add(To, *Bytes);
        Files.Remove(From);
        if (bCorruptAfterMove)
        {
            bCorruptAfterMove = false;
            Files[To].Last() ^= 0x5a;
        }
        return true;
    }
    virtual bool EnsureDirectory(const FString&) override { return true; }
};

FDDSN11Pose Pose()
{
    FDDSN11Pose P;
    P.LocationCm = FVector(140.0, 70.0, 20.0);
    P.Rotation = FRotator(0.0, 90.0, 0.0);
    P.bGroundedSidewalkValidated = true;
    P.bClosedLatchedReconstructionValidated = true;
    P.bOccupancyValidated = true;
    return P;
}

FDDSN08StableSnapshot Snapshot(int32 Leg, int32 Stars)
{
    FDDSN08StableSnapshot S;
    S.MissionInstanceId = TEXT("instance-1");
    S.CargoId = TEXT("parcel-1");
    S.PickupStationId = TEXT("cafe");
    S.DeliveryStationId = TEXT("office");
    S.ScopedStarIds = { TEXT("star-a"), TEXT("star-b"), TEXT("star-c") };
    for (int32 I = 0; I < Stars; ++I) S.CollectedStarIds.Add(S.ScopedStarIds[I]);
    S.ElapsedGameplaySeconds = 10.0 + Stars;
    if (Leg >= 1)
    {
        FDDSN08Receipt R;
        R.MissionInstanceId = S.MissionInstanceId;
        R.PrePhase = EDDSN08Phase::AwaitingPickup;
        R.StationId = S.PickupStationId;
        R.OperationId = TEXT("pickup-op");
        R.Generation = 1;
        R.CargoId = S.CargoId;
        S.Receipts.Add(R);
        S.Phase = EDDSN08Phase::Carrying;
        S.Owner = EDDSN08Owner::Robot;
    }
    if (Leg >= 2)
    {
        FDDSN08Receipt R;
        R.MissionInstanceId = S.MissionInstanceId;
        R.PrePhase = EDDSN08Phase::Carrying;
        R.StationId = S.DeliveryStationId;
        R.OperationId = TEXT("delivery-op");
        R.Generation = 2;
        R.CargoId = S.CargoId;
        S.Receipts.Add(R);
        S.Phase = EDDSN08Phase::Completed;
        S.Owner = EDDSN08Owner::Recipient;
        S.bHasFrozenResult = true;
        S.FrozenResult.ExactStarIds = S.CollectedStarIds;
        S.FrozenResult.Count = S.CollectedStarIds.Num();
        S.FrozenResult.FrozenElapsedSeconds = S.ElapsedGameplaySeconds;
    }
    return S;
}

FDDSN08Token Token()
{
    FDDSN08Token T;
    T.MissionInstanceId = TEXT("instance-1");
    T.OperationId = TEXT("delivery-op");
    T.Generation = 2;
    T.Leg = EDDSN08Leg::Delivery;
    T.PrePhase = EDDSN08Phase::Carrying;
    T.StationId = TEXT("office");
    return T;
}
FDDSN08Token PickupToken()
{
    FDDSN08Token T;
    T.MissionInstanceId = TEXT("instance-1");
    T.OperationId = TEXT("pickup-op");
    T.Generation = 1;
    T.Leg = EDDSN08Leg::Pickup;
    T.PrePhase = EDDSN08Phase::AwaitingPickup;
    T.StationId = TEXT("cafe");
    return T;
}
} // namespace DDSN11Test

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN11ControlledStorageTest,
    "DDSCity.N11.ControlledStorage", EAutomationTestFlags::EditorContext |
    EAutomationTestFlags::EngineFilter)

bool FDDSN11ControlledStorageTest::RunTest(const FString& Parameters)
{
    using namespace DDSN11Test;
    // Codec rejects a changed byte, incoherent receipts and a pose that was
    // never validated. Decode never grants fresh world-validation flags.
    FDDSN11Record Source;
    Source.Revision = 1;
    Source.Snapshot = Snapshot(2, 3);
    Source.Pose = Pose();
    TArray<uint8> Encoded;
    TestTrue(TEXT("encode stable completed 3/3"), FDDSN11Codec::Encode(Source, Encoded));
    FDDSN11Record Decoded;
    TestTrue(TEXT("decode exact bytes"), FDDSN11Codec::Decode(Encoded, Decoded));
    TestEqual(TEXT("exact frozen count"), Decoded.Snapshot.FrozenResult.Count, 3);
    TestFalse(TEXT("loaded pose still needs ground check"), Decoded.Pose.bGroundedSidewalkValidated);
    TestFalse(TEXT("loaded pose still needs occupancy check"), Decoded.Pose.bOccupancyValidated);
    for (int32 Count = 0; Count <= 3; ++Count)
    {
        FDDSN11Record Result = Source;
        Result.Snapshot = Snapshot(2, Count);
        TArray<uint8> ResultBytes;
        FDDSN11Record RoundTrip;
        TestTrue(*FString::Printf(TEXT("%d-star stable encode"), Count),
                 FDDSN11Codec::Encode(Result, ResultBytes));
        TestTrue(*FString::Printf(TEXT("%d-star stable decode"), Count),
                 FDDSN11Codec::Decode(ResultBytes, RoundTrip));
        TestEqual(*FString::Printf(TEXT("%d-star immutable count"), Count),
                  RoundTrip.Snapshot.FrozenResult.Count, Count);
        TestTrue(*FString::Printf(TEXT("%d-star exact IDs"), Count),
                 RoundTrip.Snapshot.FrozenResult.ExactStarIds
                     == Result.Snapshot.CollectedStarIds);
    }
    TArray<uint8> Damaged = Encoded;
    Damaged.Last() ^= 1;
    TestFalse(TEXT("digest catches changed payload"), FDDSN11Codec::Decode(Damaged, Decoded));
    FDDSN11Record Invalid = Source;
    Invalid.Snapshot.Receipts.RemoveAt(0);
    TestFalse(TEXT("receipt/phase incoherence rejected"), FDDSN11Codec::Encode(Invalid, Encoded));
    Invalid = Source;
    Invalid.Pose.bOccupancyValidated = false;
    TestFalse(TEXT("unvalidated pose cannot be written"), FDDSN11Codec::Encode(Invalid, Encoded));

    FMemoryFiles Files;
    FDDSN11SerializedStore Store(Files, TEXT("MEMORY"));
    FDDSN11Record Head;
    TestEqual(TEXT("no checkpoint initially"), int32(Store.LoadLatest(Head)),
              int32(EDDSN11Load::NoRecord));
    uint64 Revision = 0;
    TestFalse(TEXT("fresh mission requires explicit decision"), Store.NextRevision(Revision));
    Store.AuthorizeExplicitFreshMission();
    TestTrue(TEXT("initial revision available"), Store.NextRevision(Revision));
    Head.Revision = Revision;
    Head.Snapshot = Snapshot(0, 0);
    Head.Pose = Pose();
    TestTrue(TEXT("save initial stable record"), Store.Save(Head));
    const FDDSN08StableSnapshot Initial = Head.Snapshot;

    FDDSN11Record Pickup;
    TestTrue(TEXT("next revision"), Store.NextRevision(Pickup.Revision));
    Pickup.Snapshot = Snapshot(1, 1);
    Pickup.Pose = Pose();
    Files.bFailNextWrite = true;
    TestFalse(TEXT("injected temp-write failure"), Store.Save(Pickup));
    FDDSN11Record Loaded;
    TestEqual(TEXT("previous head remains after write failure"),
              int32(Store.LoadLatest(Loaded)), int32(EDDSN11Load::Found));
    TestEqual(TEXT("previous revision preserved"), Loaded.Revision, Head.Revision);
    TestTrue(TEXT("same candidate retry"), Store.Save(Pickup));
    TestEqual(TEXT("retry did not change revision"), Pickup.Revision, uint64(2));

    FDDSN11Record Delivery;
    TestTrue(TEXT("delivery revision"), Store.NextRevision(Delivery.Revision));
    Delivery.Snapshot = Snapshot(2, 2);
    Delivery.Pose = Pose();
    Files.bFailNextMove = true;
    TestFalse(TEXT("interrupted before slot replacement"), Store.Save(Delivery));
    TestEqual(TEXT("old head survives temp-only interruption"),
              int32(Store.LoadLatest(Loaded)), int32(EDDSN11Load::Found));
    TestEqual(TEXT("still pickup"), Loaded.Revision, Pickup.Revision);
    TestTrue(TEXT("delivery retry publishes once"), Store.Save(Delivery));
    TestTrue(TEXT("idempotent exact retry after missing callback"), Store.Save(Delivery));
    FDDSN11Record WrongPose = Delivery;
    WrongPose.Pose.LocationCm.X += 10.0;
    TestFalse(TEXT("same revision/digest cannot acknowledge changed pose"),
              Store.Save(WrongPose));
    FDDSN11Record InvalidRetryPose = Delivery;
    InvalidRetryPose.Pose.bOccupancyValidated = false;
    TestFalse(TEXT("same revision rejects invalid pose proof"),
              Store.Save(InvalidRetryPose));
    FDDSN11Record WrongDigest = Delivery;
    WrongDigest.Digest = FString::ChrN(64, TCHAR('0'));
    TestFalse(TEXT("same revision rejects stale caller digest"),
              Store.Save(WrongDigest));
    TestEqual(TEXT("frozen two-star result"), Delivery.Snapshot.FrozenResult.Count, 2);

    // Simulate a later process loading the same file image, then corruption of
    // the latest slot: validated previous checkpoint is selected, not guessed.
    FDDSN11SerializedStore Restart(Files, TEXT("MEMORY"));
    TestEqual(TEXT("restart reads newest"), int32(Restart.LoadLatest(Loaded)),
              int32(EDDSN11Load::Found));
    TestEqual(TEXT("restart revision"), Loaded.Revision, Delivery.Revision);
    Files.Files[TEXT("MEMORY/checkpoint-a.dds11")].Last() ^= 3;
    TestEqual(TEXT("corrupt latest falls back"), int32(Restart.LoadLatest(Loaded)),
              int32(EDDSN11Load::Found));
    TestEqual(TEXT("fallback is pickup"), Loaded.Revision, Pickup.Revision);
    Files.FailReadPath = TEXT("MEMORY/checkpoint-a.dds11");
    TestEqual(TEXT("I/O error does not masquerade as corruption"),
              int32(Restart.LoadLatest(Loaded)), int32(EDDSN11Load::StorageReadError));
    Files.FailReadPath.Empty();
    Files.Files[TEXT("MEMORY/checkpoint-b.dds11")].Last() ^= 4;
    TestEqual(TEXT("no valid slot requires explicit fresh mission"),
              int32(Restart.LoadLatest(Loaded)),
              int32(EDDSN11Load::ExplicitFreshRequired));
    TestFalse(TEXT("corruption never silently starts fresh"), Restart.NextRevision(Revision));
    // Rebuild a controlled two-slot image for the separate barrier test.
    Files.Files.Empty();
    FDDSN11SerializedStore BarrierBase(Files, TEXT("MEMORY"));
    BarrierBase.AuthorizeExplicitFreshMission();
    Head.Revision = 1; Head.Snapshot = Initial; Head.Pose = Pose();
    TestTrue(TEXT("barrier fixture initial"), BarrierBase.Save(Head));
    Pickup.Revision = 2; Pickup.Snapshot = Snapshot(1, 1); Pickup.Pose = Pose();
    TestTrue(TEXT("barrier fixture pickup"), BarrierBase.Save(Pickup));

    // A stale unacknowledged delivery candidate cannot overtake a deliberate
    // restore. Barrier republishes the older selected record above its revision.
    FDDSN08SaveCandidate InFlight;
    InFlight.Correlation = Token();
    InFlight.Snapshot = Snapshot(2, 2);
    InFlight.Revision = 4;
    InFlight.Digest = TEXT("stale-digest");
    FDDSN11Record Restored;
    uint64 Fence = 0;
    TestTrue(TEXT("restore barrier validates and selects checkpoint"),
             Restart.EstablishRestoreBarrier(Initial, &InFlight,
                                             Pose(), Restored, Fence));
    TestTrue(TEXT("restore revision fences old candidate"), Fence >= InFlight.Revision);
    Delivery.Revision = 3; Delivery.Snapshot = Snapshot(2, 2); Delivery.Pose = Pose();
    TestTrue(TEXT("old candidate cannot publish after barrier"), !Restart.Save(Delivery));
    TestEqual(TEXT("restore head is selected pickup"),
              int32(Restart.LoadLatest(Loaded)), int32(EDDSN11Load::Found));
    TestTrue(TEXT("selected owner/receipt unchanged"),
             FDDSN11Codec::SameLogicalSnapshot(Loaded.Snapshot, Initial));

    // Controlled port-level test: a reported write failure keeps the exact
    // N08 candidate; retry emits one matching readback-validated acknowledgment.
    FMemoryFiles PortFiles;
    FDDSN11SerializedStore PortStore(PortFiles, TEXT("PORT"));
    PortStore.AuthorizeExplicitFreshMission();
    FDDSN11SaveAdapter Adapter(PortStore);
    TestTrue(TEXT("validated pose context"), Adapter.SetPoseContext(Pose()));
    int32 FailedAcks = 0, GoodAcks = 0;
    Adapter.SetAckSink([&](const FDDSN08SaveAck& Ack)
    {
        if (Ack.bDurablyValidated) ++GoodAcks; else ++FailedAcks;
        return true;
    });
    FDDSN08SaveCandidate Candidate = Adapter.Prepare(Snapshot(1, 0), PickupToken());
    TestTrue(TEXT("candidate has ordered revision and digest"),
             Candidate.Revision == 1 && Candidate.Digest.Len() == 64);
    PortFiles.bFailNextWrite = true;
    TestTrue(TEXT("attempt accepted; failure callback issued"), Adapter.Submit(Candidate));
    TestEqual(TEXT("one failure callback"), FailedAcks, 1);
    TestTrue(TEXT("identical retry succeeds"), Adapter.Submit(Candidate));
    TestEqual(TEXT("one validated matching callback"), GoodAcks, 1);
    TestTrue(TEXT("same result loaded after retry"),
             PortStore.LoadLatest(Loaded) == EDDSN11Load::Found
             && FDDSN11Codec::SameLogicalSnapshot(Loaded.Snapshot, Candidate.Snapshot));

    // Adapter-level failed commit: the pinned delivery candidate keeps P1,
    // while explicit restore can independently choose freshly checked P2.
    FMemoryFiles RestoreFiles;
    FDDSN11SerializedStore RestoreStore(RestoreFiles, TEXT("RESTORE"));
    RestoreStore.AuthorizeExplicitFreshMission();
    FDDSN11Record Prior;
    Prior.Revision = 1;
    Prior.Snapshot = Snapshot(1, 1);
    Prior.Pose = Pose();
    TestTrue(TEXT("prior validated carrying checkpoint"), RestoreStore.Save(Prior));
    FDDSN11SaveAdapter RestoreAdapter(RestoreStore);
    const FDDSN11Pose CommitPose = Pose();
    TestTrue(TEXT("commit pose P1"), RestoreAdapter.SetPoseContext(CommitPose));
    int32 RestoreFailedAcks = 0, RestoreGoodAcks = 0;
    RestoreAdapter.SetAckSink([&](const FDDSN08SaveAck& Ack)
    {
        if (Ack.bDurablyValidated) ++RestoreGoodAcks; else ++RestoreFailedAcks;
        return true;
    });
    FDDSN08SaveCandidate OldCommit = RestoreAdapter.Prepare(Snapshot(2, 2), Token());
    TestTrue(TEXT("delivery candidate revision 2"), OldCommit.Revision == 2);
    RestoreFiles.bFailNextWrite = true;
    TestTrue(TEXT("failed commit attempt reports callback"), RestoreAdapter.Submit(OldCommit));
    TestEqual(TEXT("commit failure does not acknowledge"), RestoreFailedAcks, 1);
    TestEqual(TEXT("no success ack through barrier setup"), RestoreGoodAcks, 0);
    FDDSN11Pose SafeFallback = Pose();
    SafeFallback.LocationCm.X += 35.0;
    TestFalse(TEXT("pinned commit pose cannot be changed"),
              RestoreAdapter.SetPoseContext(SafeFallback));
    TestTrue(TEXT("initial restore proof accepted while commit remains pinned"),
             RestoreAdapter.SetRestorePoseContext(SafeFallback));
    FDDSN11Pose RejectedFallback = SafeFallback;
    RejectedFallback.bOccupancyValidated = false;
    TestFalse(TEXT("rejected new pose invalidates earlier accepted proof"),
              RestoreAdapter.SetRestorePoseContext(RejectedFallback));
    FDDSN08RestoreBarrier Barrier = RestoreAdapter.EstablishRestoreBarrier(
        Prior.Snapshot, &OldCommit);
    TestFalse(TEXT("no cached restore pose after rejection"),
              Barrier.bCheckpointDurablyCurrent);
    TestTrue(TEXT("fresh N06/N12 restore pose allowed while pinned"),
             RestoreAdapter.SetRestorePoseContext(SafeFallback));
    RestoreFiles.bFailNextMove = true;
    Barrier = RestoreAdapter.EstablishRestoreBarrier(Prior.Snapshot, &OldCommit);
    TestFalse(TEXT("failed barrier cannot release save gate"),
              Barrier.bCheckpointDurablyCurrent);
    Barrier = RestoreAdapter.EstablishRestoreBarrier(Prior.Snapshot, &OldCommit);
    TestFalse(TEXT("failed attempt consumed restore validation"),
              Barrier.bCheckpointDurablyCurrent);
    TestEqual(TEXT("no success ack from failed barrier"), RestoreGoodAcks, 0);
    TestTrue(TEXT("new current-world proof for retry"),
             RestoreAdapter.SetRestorePoseContext(SafeFallback));
    Barrier = RestoreAdapter.EstablishRestoreBarrier(Prior.Snapshot, &OldCommit);
    TestTrue(TEXT("selected prior checkpoint becomes durable head"),
             Barrier.bCheckpointDurablyCurrent && Barrier.bOlderWritesCannotOvertake);
    TestTrue(TEXT("fence covers abandoned commit"),
             Barrier.FencedThroughRevision >= OldCommit.Revision);
    TestEqual(TEXT("read back restored head"),
              int32(RestoreStore.LoadLatest(Loaded)), int32(EDDSN11Load::Found));
    TestTrue(TEXT("restored safe P2, not old P1"),
             Loaded.Pose.LocationCm == SafeFallback.LocationCm);
    TestTrue(TEXT("same prior mission/receipt"),
             FDDSN11Codec::SameLogicalSnapshot(Loaded.Snapshot, Prior.Snapshot));
    TestFalse(TEXT("abandoned old Submit rejected after fence"),
              RestoreAdapter.Submit(OldCommit));
    TestEqual(TEXT("still no late success ack"), RestoreGoodAcks, 0);

    // N10 integration must add separate 0/1/2/3, replay and ten-cycle cases;
    // this controlled source is deliberately not a node gauntlet or certificate.
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
