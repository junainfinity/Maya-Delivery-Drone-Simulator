#include "DDSN08Coordinator.h"
#include "DDSN11Storage.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
// Real identity predicate and real N11 adapter/store; controlled memory I/O and
// admission. NOT actual physics sample aging, world readiness or disk proof.
namespace DDSMissionRestoreControl
{
struct FMemoryFiles final : IDDSN11FileIO
{
    TMap<FString, TArray<uint8>> Files;
    bool bFailNextWrite = false;
    bool bFailNextMove = false;
    bool bCorruptAfterMove = false;
    FString FailReadPath;
    TFunction<void()> ReadHook;

    virtual bool Exists(const FString& Path) override { return Files.Contains(Path); }
    virtual bool Read(const FString& Path, TArray<uint8>& Out) override
    {
        if (ReadHook) ReadHook();
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
} // namespace DDSMissionRestoreControl
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSMissionEntryRepairControls,
    "DDSCity.N08.MissionEntryE01E02", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSMissionEntryRepairControls::RunTest(const FString&)
{
    using namespace DDSMissionRestoreControl;
    const auto Completed = Snapshot(2, 2);
    auto Fresh = Snapshot(0, 0); Fresh.MissionInstanceId = TEXT("new-instance");
    TestTrue(TEXT("E01 same retained stations admitted"), UDDSN08Coordinator::ReplayStationIdentityMatches(Fresh, Completed));
    Fresh.PickupStationId = TEXT("other-pickup");
    TestFalse(TEXT("E01 changed pickup refused"), UDDSN08Coordinator::ReplayStationIdentityMatches(Fresh, Completed));
    Fresh = Snapshot(0, 0); Fresh.DeliveryStationId = TEXT("other-delivery");
    TestFalse(TEXT("E01 changed delivery refused"), UDDSN08Coordinator::ReplayStationIdentityMatches(Fresh, Completed));
    TestEqual(TEXT("E01 prior completed result retained"), Completed.FrozenResult.Count, 2);
    FMemoryFiles Files; FDDSN11SerializedStore Store(Files, TEXT("E02MEMORY"));
    Store.AuthorizeExplicitFreshMission();
    FDDSN11Record Prior; Prior.Revision = 1; Prior.Snapshot = Snapshot(1, 1); Prior.Pose = Pose();
    TestTrue(TEXT("E02 durable selected fixture"), Store.Save(Prior));
    FDDSN11SaveAdapter Adapter(Store); TestTrue(TEXT("E02 commit pose"), Adapter.SetPoseContext(Pose()));
    int32 Acks = 0;
    Adapter.SetAckSink([&](const FDDSN08SaveAck&) { ++Acks; return false; });
    const auto ExactCandidate = Adapter.Prepare(Snapshot(2, 2), Token());
    Files.bFailNextWrite = true;
    TestTrue(TEXT("E02 failed attempt pins exact candidate"), Adapter.Submit(ExactCandidate));
    const int32 AcksBeforeRestore = Acks;
    auto RestorePose = Pose(); RestorePose.LocationCm.X += 1;
    TestTrue(TEXT("E02 one attempt current restore pose"), Adapter.SetRestorePoseContext(RestorePose));
    bool CurrentAdmission = true;
    Files.ReadHook = [&]() { CurrentAdmission = false; }; // simulated I/O expiry/revocation.
    int32 Admissions = 0;
    auto Refused = Adapter.EstablishRestoreBarrierWithCurrentAdmission(Prior.Snapshot, &ExactCandidate,
        [&]() { ++Admissions; return CurrentAdmission; });
    Files.ReadHook = {};
    TestEqual(TEXT("E02 post-I/O admission ran once"), Admissions, 1);
    TestFalse(TEXT("E02 expired proof grants no barrier permission"), Refused.bCheckpointDurablyCurrent);
    TestFalse(TEXT("E02 no fence permission published on physical refusal"), Refused.bOlderWritesCannotOvertake);
    TestEqual(TEXT("E02 barrier did not forge an ACK"), Acks, AcksBeforeRestore);
    TestFalse(TEXT("E02 exact commit pose remains pinned"), Adapter.SetPoseContext(Pose()));
    auto ForeignCandidate = ExactCandidate; ForeignCandidate.Digest = TEXT("different");
    TestFalse(TEXT("E02 foreign candidate cannot replace retained pin"), Adapter.Submit(ForeignCandidate));
    TestTrue(TEXT("E02 exact candidate retry still accepted"), Adapter.Submit(ExactCandidate));
    TestFalse(TEXT("E02 restore context consumed on rejection"), Adapter.EstablishRestoreBarrierWithCurrentAdmission(
        Prior.Snapshot, &ExactCandidate, [](){ return true; }).bCheckpointDurablyCurrent);
    TestTrue(TEXT("E02 renewed one-attempt pose"), Adapter.SetRestorePoseContext(RestorePose));
    auto Accepted = Adapter.EstablishRestoreBarrierWithCurrentAdmission(Prior.Snapshot, &ExactCandidate, [](){ return true; });
    TestTrue(TEXT("E02 current proof after durable I/O grants barrier"), Accepted.bCheckpointDurablyCurrent && Accepted.bOlderWritesCannotOvertake);
    TestTrue(TEXT("E02 pin released only after accepted post-I/O admission"), Adapter.SetPoseContext(Pose()));
    return true;
}
#endif
