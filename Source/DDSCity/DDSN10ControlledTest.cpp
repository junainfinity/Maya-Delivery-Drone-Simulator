#include "DDSN10StarBridge.h"
#include "DDSN10AccessSweep.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

// Controlled N08 mechanics/save adapters only. No physical star/world or N11
// disk result is asserted by this source until later native fixture execution.
namespace DDSN10Controlled
{
    struct FMotion final : IDDSN08MotionPort
    {
        void SetDriveLocked(bool bLocked) override { bDriveLocked = bLocked; }
        bool bDriveLocked = false;
    };
    struct FMechanics final : IDDSN08MechanicsPort
    {
        void BeginPhysicalCycle(const FDDSN08Token& Token) override { Last = Token; }
        void CancelAndClose(const FDDSN08Token& Token) override { Last = Token; }
        FDDSN08Token Last;
    };
    struct FSave final : IDDSN08SavePort
    {
        uint64 NextRevision = 1;
        FDDSN08SaveCandidate LastSubmitted;
        FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& Snapshot,
            const FDDSN08Token& Token) override
        {
            FDDSN08SaveCandidate C;
            C.Snapshot = Snapshot;
            C.Correlation = Token;
            C.Revision = NextRevision++;
            C.Digest = FString::Printf(TEXT("CONTROLLED_ONLY_%llu"),C.Revision);
            return C;
        }
        bool Submit(const FDDSN08SaveCandidate& Candidate) override
        {
            LastSubmitted = Candidate;
            return true;
        }
        FDDSN08RestoreBarrier EstablishRestoreBarrier(
            const FDDSN08StableSnapshot& Checkpoint,
            const FDDSN08SaveCandidate* InFlight) override
        {
            FDDSN08RestoreBarrier B;
            B.MissionInstanceId = Checkpoint.MissionInstanceId;
            B.bCheckpointDurablyCurrent = true; // Controlled fixture only.
            B.bOlderWritesCannotOvertake = true;
            B.FencedThroughRevision = InFlight ? InFlight->Revision : NextRevision;
            return B;
        }
    };
    struct FFixture
    {
        FMotion Motion;
        FMechanics Mechanics;
        FSave Save;
        FDDSN08Authority Authority;
        FString Instance;
        TArray<FString> ScopedIds;
        bool bStarted = false;
        explicit FFixture(int32 Case) : Authority(Motion,Mechanics,Save)
        {
            Instance = FString::Printf(TEXT("M-%d"),Case);
            const TArray<FString> Slots = {TEXT("Main"),TEXT("Passage"),TEXT("Detour")};
            bStarted = DDSN10StarBridge::MakeThreeScopedStarIds(Instance,Slots,ScopedIds)
                && Authority.StartFreshMission(Instance,TEXT("Cargo"),TEXT("Cafe"),
                    TEXT("Office"),ScopedIds);
        }
        FDDSN08Observation At(const FString& Station) const
        {
            FDDSN08Observation O;
            O.MissionInstanceId = Instance;
            O.StationId = Station;
            O.bKnown = O.bCargoObserved = O.bActiveCargoAtExpectedSource = true;
            O.ObservedCargoId = TEXT("Cargo");
            O.bGrounded = true;
            O.RobotSocketCm = FVector::ZeroVector;
            O.StationPointCm = FVector(100,0,0);
            O.ActualVelocityCmPerSecond = FVector::ZeroVector;
            O.bServicePathObserved = O.bServicePathClear = true;
            O.bFullLidSweepObserved = O.bFullLidSweepClear = true;
            O.bClosedOperableDoorObserved = O.bClosedOperableDoor = true;
            O.ObservationWorldSeconds = 12.0;
            return O;
        }
        static FDDSN08LatchEvidence Latch(const FDDSN08Token& Token)
        {
            FDDSN08LatchEvidence E;
            E.Token = Token;
            E.bMechanicalObservationKnown = E.bParcelStagedAtCorrectDestination = true;
            E.bFullOpeningAndClosingSweepClear = true;
            E.bLeftLidAtClosedPose = E.bRightLidAtClosedPose = true;
            E.bLeftLatchVerified = E.bRightLatchVerified = true;
            return E;
        }
        bool BeginAndLatch(const FString& Station)
        {
            const FDDSN08Observation O = At(Station);
            const bool bStartedTransfer = Authority.OnInteractButton(true,O).bAllowed;
            Authority.OnInteractButton(false,O);
            return bStartedTransfer && Authority.OnVerifiedLatch(Latch(Authority.GetActiveToken()));
        }
        bool AckCurrent()
        {
            const FDDSN08SaveCandidate& C = Authority.GetPinnedCandidate();
            FDDSN08SaveAck Ack;
            Ack.Correlation = C.Correlation;
            Ack.Revision = C.Revision;
            Ack.Digest = C.Digest;
            Ack.bDurablyValidated = true; // Simulated validated ack, not N11 disk.
            return Authority.OnSaveAck(Ack);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN10ControlledStarsTest,
    "DDSCity.N10.ControlledStars", EAutomationTestFlags::EditorContext
    | EAutomationTestFlags::EngineFilter)

bool FDDSN10ControlledStarsTest::RunTest(const FString& Parameters)
{
    using namespace DDSN10Controlled;
    (void)Parameters;
    // Reviewer N10-S01 centimeter fixture: a 55 cm trigger overlaps across
    // a 0..10 cm X wall rising to Z=60, while the old ray at Z=73..74 clears.
    // The shared access sweep must instead carry the N06 chassis (bottom Z=22)
    // across that wall. This is geometry setup only; native world-query and
    // actual overlap/award assertions remain required after integration.
    FDDSN10AccessSweep LowWallAccess;
    TestTrue(TEXT("low-wall access geometry is constructible"),
        FDDSN10AccessSweep::TryBuild(FVector(-50,0,46),
            FVector(45,32.5,24), FQuat::Identity, FVector(45,0,0),
            LowWallAccess));
    const double TriggerGapCm = FMath::Sqrt(50.0*50.0 + 10.0*10.0);
    const double OldRayAtWallCm = 65.0 + (50.0/95.0)*15.0;
    TestTrue(TEXT("trigger really overlaps across wall"), TriggerGapCm < 55.0);
    TestTrue(TEXT("old center ray falsely clears 60 cm wall"),
             OldRayAtWallCm > 60.0);
    TestTrue(TEXT("new chassis sweep crosses low wall in X and Z"),
        LowWallAccess.Start.X + LowWallAccess.HalfExtent.X < 0.0
        && LowWallAccess.End.X + LowWallAccess.HalfExtent.X > 10.0
        && LowWallAccess.Start.Z - LowWallAccess.HalfExtent.Z < 60.0
        && LowWallAccess.Start.Z + LowWallAccess.HalfExtent.Z > 0.0);
    TArray<FString> InvalidOutput = {TEXT("old")};
    TestFalse(TEXT("duplicate stable slot IDs rejected"),
        DDSN10StarBridge::MakeThreeScopedStarIds(TEXT("M"),
            {TEXT("A"),TEXT("a"),TEXT("C")},InvalidOutput));
    TestEqual(TEXT("invalid ID generation clears output"),InvalidOutput.Num(),0);
    for (int32 TargetCount=0; TargetCount<=3; ++TargetCount)
    {
        FFixture F(TargetCount);
        if (!TestTrue(FString::Printf(TEXT("%d-star fixture initialized"),TargetCount),F.bStarted))
            continue;
        F.Authority.TickGameplay(17.0,false);
        const FDDSN08StableSnapshot BeforeCollect = F.Authority.GetStableSnapshot();
        for (int32 Index=0; Index<TargetCount; ++Index)
        {
            TestTrue(FString::Printf(TEXT("%d-star first collection %d"),TargetCount,Index),
                F.Authority.RecordN10Star(F.Instance,F.ScopedIds[Index]));
            TestFalse(TEXT("duplicate overlap does not add ID"),
                F.Authority.RecordN10Star(F.Instance,F.ScopedIds[Index]));
        }
        TestFalse(TEXT("wrong instance cannot collect"),
            F.Authority.RecordN10Star(TEXT("other"),F.ScopedIds[0]));
        TestFalse(TEXT("unknown ID cannot collect"),
            F.Authority.RecordN10Star(F.Instance,TEXT("invalid-star")));
        TestFalse(TEXT("null/unowned contact cannot record"),
            DDSN10StarBridge::TryRecordValidatedContact(F.Authority,F.Instance,
                F.ScopedIds[0],nullptr,nullptr));
        const FDDSN08StableSnapshot AfterCollect = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("count equals unique collected IDs"),AfterCollect.CollectedStarIds.Num(),TargetCount);
        TestEqual(TEXT("collection does not alter owner"),AfterCollect.Owner,BeforeCollect.Owner);
        TestEqual(TEXT("collection does not alter phase"),AfterCollect.Phase,BeforeCollect.Phase);
        TestEqual(TEXT("collection does not alter cargo"),AfterCollect.CargoId,BeforeCollect.CargoId);
        TestEqual(TEXT("collection does not alter timer"),AfterCollect.ElapsedGameplaySeconds,
            BeforeCollect.ElapsedGameplaySeconds);
        TestEqual(TEXT("recovery policy permits closed-lid primitive"),
            F.Authority.RequestRecoveryPolicy(true),EDDSN08Recovery::Allowed);
        TestEqual(TEXT("recovery policy preserves star IDs"),
            F.Authority.GetStableSnapshot().CollectedStarIds,AfterCollect.CollectedStarIds);

        if (!TestTrue(TEXT("controlled pickup latch"),F.BeginAndLatch(TEXT("Cafe")))) continue;
        if (!TestTrue(TEXT("controlled pickup save ack"),F.AckCurrent())) continue;
        F.Authority.TickGameplay(5.0,false);
        if (!TestTrue(TEXT("controlled delivery latch"),F.BeginAndLatch(TEXT("Office")))) continue;
        const FDDSN08StableSnapshot Pending = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("delivery freezes exact count before ack"),Pending.FrozenResult.Count,TargetCount);
        TestEqual(TEXT("delivery freezes exact IDs"),Pending.FrozenResult.ExactStarIds,
            Pending.CollectedStarIds);
        TestEqual(TEXT("delivery freezes unchanged elapsed time"),
            Pending.FrozenResult.FrozenElapsedSeconds,22.0);
        TestFalse(TEXT("completed pending-save rejects remaining star"),
            F.Authority.RecordN10Star(F.Instance,F.ScopedIds[2]));
        TestFalse(TEXT("result stays hidden until ack"),
            DDSN10StarBridge::ProjectSavedResult(Pending,F.Authority.GetPresentation())
                .bSavedResultAvailable);
        if (!TestTrue(TEXT("controlled delivery save ack"),F.AckCurrent())) continue;
        const FDDSN08StableSnapshot Saved = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("two station receipts, no star receipt"),Saved.Receipts.Num(),2);
        const FDDSN10ResultView Result = DDSN10StarBridge::ProjectSavedResult(
            Saved,F.Authority.GetPresentation());
        TestTrue(TEXT("saved result visible"),Result.bSavedResultAvailable);
        TestEqual(TEXT("saved result n/3"),Result.Count,TargetCount);
        TestEqual(TEXT("saved result exact IDs"),Result.ExactStarIds,Saved.CollectedStarIds);
        TestEqual(TEXT("saved result frozen time"),Result.FrozenElapsedSeconds,22.0);
        // Dismiss/touch/reopen is a read-only projection, not a result owner.
        TestFalse(TEXT("former star location cannot collect after completion"),
            F.Authority.RecordN10Star(F.Instance,F.ScopedIds[0]));
        const FDDSN10ResultView Reopened = DDSN10StarBridge::ProjectSavedResult(
            F.Authority.GetStableSnapshot(),F.Authority.GetPresentation());
        TestEqual(TEXT("dismiss/reopen preserves exact IDs"),Reopened.ExactStarIds,Result.ExactStarIds);
        TestEqual(TEXT("dismiss/reopen preserves time"),Reopened.FrozenElapsedSeconds,
            Result.FrozenElapsedSeconds);
        TestEqual(TEXT("dismiss/touch creates no extra receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(),2);
        if (!TestTrue(TEXT("controlled restore barrier and state"),
            F.Authority.RestoreValidatedCheckpoint(Saved,true,true,true))) continue;
        const FDDSN10ResultView Restored = DDSN10StarBridge::ProjectSavedResult(
            F.Authority.GetStableSnapshot(),F.Authority.GetPresentation());
        TestEqual(TEXT("controlled restore preserves frozen IDs"),Restored.ExactStarIds,Result.ExactStarIds);
        TestEqual(TEXT("controlled restore preserves frozen time"),Restored.FrozenElapsedSeconds,
            Result.FrozenElapsedSeconds);
        TestEqual(TEXT("controlled restore creates no receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(),2);

        const FString NewInstance = FString::Printf(TEXT("Replay-%d"),TargetCount);
        TArray<FString> NewIds;
        TestTrue(TEXT("new replay IDs generated"),DDSN10StarBridge::MakeThreeScopedStarIds(
            NewInstance,{TEXT("Main"),TEXT("Passage"),TEXT("Detour")},NewIds));
        TestTrue(TEXT("explicit replay accepted"),F.Authority.ReplayExplicit(
            NewInstance,TEXT("NewCargo"),NewIds));
        const FDDSN08StableSnapshot Fresh = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("replay has new mission instance"),Fresh.MissionInstanceId,NewInstance);
        TestEqual(TEXT("replay has three active scoped IDs"),Fresh.ScopedStarIds.Num(),3);
        TestEqual(TEXT("replay empty collected-ID set"),Fresh.CollectedStarIds.Num(),0);
        TestEqual(TEXT("replay starts without old receipts"),Fresh.Receipts.Num(),0);
        TestEqual(TEXT("replay parcel remains at station"),Fresh.Owner,EDDSN08Owner::Station);
        TestEqual(TEXT("replay timer resets"),Fresh.ElapsedGameplaySeconds,0.0);
        TestFalse(TEXT("replay has no frozen result"),Fresh.bHasFrozenResult);
        TestTrue(TEXT("old saved result remains identifiable by old instance"),
            Result.MissionInstanceId == F.Instance && Result.Count == TargetCount);
        for (const FString& OldId : F.ScopedIds)
            TestFalse(TEXT("new scoped IDs exclude old instance"),Fresh.ScopedStarIds.Contains(OldId));
    }
    return true;
}
#endif
