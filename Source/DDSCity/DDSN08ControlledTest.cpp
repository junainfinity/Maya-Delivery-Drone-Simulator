#include "DDSN08Authority.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

// Controlled adapter test only. It exercises the real N08 state machine after
// integration; it is not physical N12 lid or durable N11 disk evidence.
namespace DDSN08Controlled
{
    struct FMotion final : IDDSN08MotionPort
    {
        bool bLocked = false;
        int32 Calls = 0;
        void SetDriveLocked(bool bInLocked) override { bLocked = bInLocked; ++Calls; }
    };

    struct FMechanics final : IDDSN08MechanicsPort
    {
        TArray<FDDSN08Token> Begun;
        TArray<FDDSN08Token> Cancelled;
        void BeginPhysicalCycle(const FDDSN08Token& Token) override { Begun.Add(Token); }
        void CancelAndClose(const FDDSN08Token& Token) override { Cancelled.Add(Token); }
    };

    struct FSave final : IDDSN08SavePort
    {
        int32 PrepareCount = 0;
        TArray<FDDSN08SaveCandidate> Submissions;
        bool bQueueSucceeds = true;
        bool bPrepareInvalid = false;
        bool bSynchronousValidatedAck = false;
        bool bSynchronousFailedAck = false;
        bool bRestoreBarrierReady = false;
        int32 RestoreBarrierCalls = 0;
        uint64 FencedThroughRevision = 0;
        bool bHasControlledDurableHead = false;
        FDDSN08StableSnapshot ControlledDurableHead;
        FDDSN08Authority* Listener = nullptr;
        FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& Snapshot,
            const FDDSN08Token& Correlation) override
        {
            FDDSN08SaveCandidate C;
            C.Snapshot = Snapshot;
            C.Correlation = Correlation;
            C.Revision = static_cast<uint64>(++PrepareCount);
            C.Digest = FString::Printf(TEXT("CONTROLLED_ONLY_%d_%s"), PrepareCount,
                *Correlation.OperationId);
            if (bPrepareInvalid) C.Revision = 0;
            return C;
        }
        bool Submit(const FDDSN08SaveCandidate& C) override
        {
            Submissions.Add(C);
            if (Listener && (bSynchronousValidatedAck || bSynchronousFailedAck))
            {
                FDDSN08SaveAck Ack;
                Ack.Correlation = C.Correlation;
                Ack.Revision = C.Revision;
                Ack.Digest = C.Digest;
                Ack.bDurablyValidated = bSynchronousValidatedAck;
                Listener->OnSaveAck(Ack);
            }
            return bQueueSucceeds;
        }
        FDDSN08RestoreBarrier EstablishRestoreBarrier(
            const FDDSN08StableSnapshot& Checkpoint,
            const FDDSN08SaveCandidate* InFlight) override
        {
            ++RestoreBarrierCalls;
            FDDSN08RestoreBarrier Proof;
            Proof.MissionInstanceId = Checkpoint.MissionInstanceId;
            Proof.bCheckpointDurablyCurrent = bRestoreBarrierReady;
            Proof.bOlderWritesCannotOvertake = bRestoreBarrierReady;
            Proof.FencedThroughRevision = InFlight ? InFlight->Revision : 0;
            if (bRestoreBarrierReady)
            {
                FencedThroughRevision = FMath::Max(FencedThroughRevision,
                    Proof.FencedThroughRevision);
                ControlledDurableHead = Checkpoint;
                bHasControlledDurableHead = true;
            }
            return Proof;
        }
        bool CompleteControlledDelayedWrite(const FDDSN08SaveCandidate& OldWrite)
        {
            if (OldWrite.Revision <= FencedThroughRevision) return false;
            ControlledDurableHead = OldWrite.Snapshot;
            bHasControlledDurableHead = true;
            return true;
        }
    };

    struct FFixture
    {
        FMotion Motion;
        FMechanics Mechanics;
        FSave Save;
        FDDSN08Authority Authority;
        FFixture() : Authority(Motion, Mechanics, Save)
        {
            Save.Listener = &Authority;
            Authority.StartFreshMission(TEXT("mission-A"), TEXT("cargo-A"),
                TEXT("pickup-A"), TEXT("delivery-A"),
                {TEXT("star-A"), TEXT("star-B"), TEXT("star-C")});
        }
        FDDSN08Observation Good(const FString& Station = TEXT("pickup-A")) const
        {
            FDDSN08Observation O;
            O.MissionInstanceId = TEXT("mission-A");
            O.StationId = Station;
            O.bKnown = true;
            O.bCargoObserved = true;
            O.ObservedCargoId = TEXT("cargo-A");
            O.bActiveCargoAtExpectedSource = true;
            O.bGrounded = true;
            O.RobotSocketCm = FVector(0, 0, 0);
            O.StationPointCm = FVector(100, 0, 0);
            O.ActualVelocityCmPerSecond = FVector::ZeroVector;
            O.bServicePathObserved = O.bServicePathClear = true;
            O.bFullLidSweepObserved = O.bFullLidSweepClear = true;
            O.bClosedOperableDoorObserved = O.bClosedOperableDoor = true;
            O.ObservationWorldSeconds = 12.0;
            return O;
        }
        static FDDSN08LatchEvidence GoodLatch(const FDDSN08Token& T)
        {
            FDDSN08LatchEvidence E;
            E.Token = T;
            E.bMechanicalObservationKnown = true;
            E.bParcelStagedAtCorrectDestination = true;
            E.bFullOpeningAndClosingSweepClear = true;
            E.bLeftLidAtClosedPose = E.bRightLidAtClosedPose = true;
            E.bLeftLatchVerified = E.bRightLatchVerified = true;
            return E;
        }
        static FDDSN08SaveAck Ack(const FDDSN08SaveCandidate& C, bool bValidated = true)
        {
            FDDSN08SaveAck A;
            A.Correlation = C.Correlation;
            A.Revision = C.Revision;
            A.Digest = C.Digest;
            A.bDurablyValidated = bValidated;
            return A;
        }
        bool BeginAndLatch(const FString& Station)
        {
            const FDDSN08Observation O = Good(Station);
            const bool bStarted = Authority.OnInteractButton(true, O).bAllowed;
            Authority.OnInteractButton(false, O);
            if (!bStarted) return false;
            return Authority.OnVerifiedLatch(GoodLatch(Authority.GetActiveToken()));
        }
        bool SavedPickup()
        {
            if (!BeginAndLatch(TEXT("pickup-A"))) return false;
            return Authority.OnSaveAck(Ack(Authority.GetPinnedCandidate()));
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN08ControlledAuthorityTest,
    "DDSCity.N08.ControlledAuthority", EAutomationTestFlags::EditorContext
    | EAutomationTestFlags::EngineFilter)

bool FDDSN08ControlledAuthorityTest::RunTest(const FString& Parameters)
{
    using namespace DDSN08Controlled;
    (void)Parameters;
    // Every test below compares expected state against the actual public state
    // and callback return. An independent judge must still run and assess it.
    {
        FFixture F;
        FDDSN08Observation O = F.Good();
        O.StationPointCm = FVector(125, 0, 0);
        O.ActualVelocityCmPerSecond = FVector(-20, 0, 0);
        TestTrue(TEXT("AC02 inclusive 125cm and reverse 20cm/s"),
            F.Authority.EvaluateProspectivePrompt(O).bAllowed);
        O.StationPointCm.X = 125.01;
        TestEqual(TEXT("AC02 outside 125cm"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongStationOrBay);
        O = F.Good();
        O.StationPointCm = FVector(0, 0, 50);
        TestTrue(TEXT("AC02 inclusive 50cm height"), F.Authority.Evaluate(O, true).bAllowed);
        O.StationPointCm.Z = 50.01;
        TestEqual(TEXT("AC02 outside 50cm height"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongStationOrBay);
        O = F.Good();
        O.ActualVelocityCmPerSecond = FVector(0, -20.01, 0);
        TestEqual(TEXT("AC02 sideways actual velocity"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::Moving);
        O.ActualVelocityCmPerSecond = FVector(0, 0, -20.01);
        TestEqual(TEXT("AC02 falling actual velocity"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::Moving);
        O.ActualVelocityCmPerSecond = FVector(-20.01, 0, 0);
        TestEqual(TEXT("AC02 reverse actual velocity"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::Moving);
        O = F.Good(TEXT("wrong-station"));
        TestEqual(TEXT("AC03 wrong station"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongStationOrBay);
        O = F.Good(); O.MissionInstanceId = TEXT("other-mission");
        TestEqual(TEXT("AC03 wrong mission instance"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongPhaseOrCargo);
        O = F.Good(); O.bGrounded = false;
        TestEqual(TEXT("AC03 unsupported"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongStationOrBay);
        O = F.Good(); O.ObservedCargoId = TEXT("other-cargo");
        TestEqual(TEXT("AC03 wrong cargo"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongPhaseOrCargo);
        O = F.Good(); O.bActiveCargoAtExpectedSource = false;
        TestEqual(TEXT("AC03 expected source empty"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::WrongPhaseOrCargo);
        O = F.Good(); O.bServicePathClear = false;
        TestEqual(TEXT("AC03 wall in service path"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::ObstructedOrDoorUnavailable);
        O = F.Good(); O.bFullLidSweepObserved = false;
        TestEqual(TEXT("AC03 unobserved full sweep fails closed"),
            F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::ObstructedOrDoorUnavailable);
        O = F.Good(); O.bFullLidSweepClear = false;
        TestEqual(TEXT("AC03 blocked full sweep"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::ObstructedOrDoorUnavailable);
        O = F.Good(); O.bClosedOperableDoor = false;
        TestEqual(TEXT("AC03 door unavailable"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::ObstructedOrDoorUnavailable);
        O = F.Good(); O.bKnown = false;
        TestEqual(TEXT("AC02 unknown observation fails closed"), F.Authority.Evaluate(O, true).Reason,
            EDDSN08Denial::Unavailable);
        TestEqual(TEXT("AC03 invalid guards made zero receipts"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 0);
    }
    {
        FFixture F;
        const FDDSN08Observation O = F.Good();
        const FDDSN08Eligibility Prompt = F.Authority.EvaluateProspectivePrompt(O);
        const FDDSN08Eligibility Press = F.Authority.OnInteractButton(true, O);
        TestEqual(TEXT("AC02 prompt/E use same guard"), Prompt.Reason, Press.Reason);
        TestTrue(TEXT("AC03 first physical edge reserves"), Press.bAllowed);
        TestTrue(TEXT("AC04 green remains during PickingUp"),
            F.Authority.GetPresentation().bPickupBeacon);
        TestEqual(TEXT("AC03 one mechanics cycle"), F.Mechanics.Begun.Num(), 1);
        for (int32 I = 0; I < 10; ++I)
            TestFalse(TEXT("AC03 held/spammed E cannot reserve"),
                F.Authority.OnInteractButton(true, O).bAllowed);
        TestEqual(TEXT("AC03 still one mechanics cycle"), F.Mechanics.Begun.Num(), 1);
        TestEqual(TEXT("AC04 pre-latch owner retained"), F.Authority.GetStableSnapshot().Owner,
            EDDSN08Owner::Station);
        const FDDSN08Token Old = F.Authority.GetActiveToken();
        F.Authority.OnMechanicalProgress(Old, EDDSN08MechanicStep::Staged);
        TestEqual(TEXT("AC04 staged animation cannot change owner"),
            F.Authority.GetStableSnapshot().Owner, EDDSN08Owner::Station);
        TestEqual(TEXT("AC04 staged animation cannot create receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 0);
        FDDSN08LatchEvidence MissingLid = FFixture::GoodLatch(Old);
        MissingLid.bRightLatchVerified = false;
        TestFalse(TEXT("AC04 one lid cannot commit"), F.Authority.OnVerifiedLatch(MissingLid));
        FDDSN08LatchEvidence WrongGeneration = FFixture::GoodLatch(Old);
        ++WrongGeneration.Token.Generation;
        TestFalse(TEXT("AC03 wrong generation latch rejected"),
            F.Authority.OnVerifiedLatch(WrongGeneration));
        FDDSN08LatchEvidence WrongLeg = FFixture::GoodLatch(Old);
        WrongLeg.Token.Leg = EDDSN08Leg::Delivery;
        TestFalse(TEXT("AC03 wrong leg latch rejected"),
            F.Authority.OnVerifiedLatch(WrongLeg));
        TestEqual(TEXT("AC04 no receipt before both-lid latch"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 0);
        TestTrue(TEXT("AC04 verified latch commits"),
            F.Authority.OnVerifiedLatch(FFixture::GoodLatch(Old)));
        TestFalse(TEXT("AC03 duplicate latch rejected"),
            F.Authority.OnVerifiedLatch(FFixture::GoodLatch(Old)));
        TestEqual(TEXT("AC04 one receipt"), F.Authority.GetStableSnapshot().Receipts.Num(), 1);
        TestEqual(TEXT("AC04 carrying owner"), F.Authority.GetStableSnapshot().Owner,
            EDDSN08Owner::Robot);
        TestTrue(TEXT("AC04 save gate holds drive"), F.Motion.bLocked);
        TestTrue(TEXT("AC04 green remains through pickup save gate"),
            F.Authority.GetPresentation().bPickupBeacon);
        TestFalse(TEXT("AC04 red beacon waits for ack"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestEqual(TEXT("AC04 one prepared save"), F.Save.PrepareCount, 1);
        TestEqual(TEXT("AC04 one submission"), F.Save.Submissions.Num(), 1);
        FDDSN08SaveAck Wrong = FFixture::Ack(F.Authority.GetPinnedCandidate());
        ++Wrong.Revision;
        TestFalse(TEXT("AC05 wrong revision rejected"), F.Authority.OnSaveAck(Wrong));
        Wrong = FFixture::Ack(F.Authority.GetPinnedCandidate());
        --Wrong.Revision;
        TestFalse(TEXT("AC05 older revision rejected"), F.Authority.OnSaveAck(Wrong));
        Wrong = FFixture::Ack(F.Authority.GetPinnedCandidate());
        Wrong.Digest += TEXT("-wrong");
        TestFalse(TEXT("AC05 wrong digest rejected"), F.Authority.OnSaveAck(Wrong));
        Wrong = FFixture::Ack(F.Authority.GetPinnedCandidate());
        ++Wrong.Correlation.Generation;
        TestFalse(TEXT("AC05 wrong generation rejected"), F.Authority.OnSaveAck(Wrong));
        TestTrue(TEXT("AC05 failed matching ack accepted as failure"),
            F.Authority.OnSaveAck(FFixture::Ack(F.Authority.GetPinnedCandidate(), false)));
        TestEqual(TEXT("AC05 failure gate retained"), F.Authority.GetSaveGate(),
            EDDSN08Gate::FailedRetryable);
        TestTrue(TEXT("AC05 pickup save failure retains green"),
            F.Authority.GetPresentation().bPickupBeacon);
        TestEqual(TEXT("AC05 recovery blocked by save failure"),
            F.Authority.RequestRecoveryPolicy(true), EDDSN08Recovery::PersistenceLocked);
        TestTrue(TEXT("AC05 retry same candidate"), F.Authority.RetryPinnedSave());
        TestEqual(TEXT("AC05 no re-prepare"), F.Save.PrepareCount, 1);
        TestEqual(TEXT("AC05 retry submission count"), F.Save.Submissions.Num(), 2);
        TestEqual(TEXT("AC05 retry revision identical"),
            F.Save.Submissions[0].Revision, F.Save.Submissions[1].Revision);
        TestEqual(TEXT("AC05 retry digest identical"),
            F.Save.Submissions[0].Digest, F.Save.Submissions[1].Digest);
        TestTrue(TEXT("AC04 validated matching ack unlocks"),
            F.Authority.OnSaveAck(FFixture::Ack(F.Authority.GetPinnedCandidate())));
        TestFalse(TEXT("AC04 drive unlocked"), F.Motion.bLocked);
        TestTrue(TEXT("AC04 red delivery beacon after ack"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestFalse(TEXT("AC04 green pickup beacon retires after ack"),
            F.Authority.GetPresentation().bPickupBeacon);
        TestFalse(TEXT("AC03 duplicate ack rejected"), F.Authority.OnSaveAck(
            FFixture::Ack(F.Save.Submissions[0])));
        TestEqual(TEXT("AC03 duplicate cannot add receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
    }
    {
        FFixture F;
        TestTrue(TEXT("AC01 setup saved pickup"), F.SavedPickup());
        TestTrue(TEXT("AC01 N10 records exact scoped star"),
            F.Authority.RecordN10Star(TEXT("mission-A"), TEXT("star-B")));
        TestFalse(TEXT("AC01 duplicate star ignored"),
            F.Authority.RecordN10Star(TEXT("mission-A"), TEXT("star-B")));
        F.Authority.TickGameplay(9.25, false);
        F.Authority.TickGameplay(40.0, true);
        const FDDSN08Observation DeliveryO = F.Good(TEXT("delivery-A"));
        TestTrue(TEXT("AC04 delivery E begins"),
            F.Authority.OnInteractButton(true, DeliveryO).bAllowed);
        TestTrue(TEXT("AC04 red remains during Delivering"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        const FDDSN08Token DeliveryToken = F.Authority.GetActiveToken();
        F.Authority.OnInteractButton(false, DeliveryO);
        TestTrue(TEXT("AC01 delivery latch"),
            F.Authority.OnVerifiedLatch(FFixture::GoodLatch(DeliveryToken)));
        const FDDSN08StableSnapshot Frozen = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("AC01 recipient owner"), Frozen.Owner, EDDSN08Owner::Recipient);
        TestEqual(TEXT("AC01 exact frozen count"), Frozen.FrozenResult.Count, 1);
        TestEqual(TEXT("AC01 exact frozen ID"), Frozen.FrozenResult.ExactStarIds[0],
            FString(TEXT("star-B")));
        TestEqual(TEXT("AC01 paused time excluded"),
            Frozen.FrozenResult.FrozenElapsedSeconds, 9.25);
        TestFalse(TEXT("AC01 result hidden before durable ack"),
            F.Authority.GetPresentation().bShowDelivered);
        TestTrue(TEXT("AC04 red persists through delivery save gate"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestFalse(TEXT("AC01 star after completion rejected"),
            F.Authority.RecordN10Star(TEXT("mission-A"), TEXT("star-A")));
        F.Authority.TickGameplay(8.0, false);
        TestEqual(TEXT("AC01 result time stays frozen"),
            F.Authority.GetStableSnapshot().FrozenResult.FrozenElapsedSeconds, 9.25);
        TestTrue(TEXT("AC01 completion ack"), F.Authority.OnSaveAck(
            FFixture::Ack(F.Authority.GetPinnedCandidate())));
        TestTrue(TEXT("AC01 result visible after ack"),
            F.Authority.GetPresentation().bShowDelivered);
        TestFalse(TEXT("AC04 delivery neutral after validated ack"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestFalse(TEXT("AC01 no automatic next order"),
            F.Authority.HasActiveOperation());
        TestFalse(TEXT("AC01 replay rejects reused star IDs"), F.Authority.ReplayExplicit(
            TEXT("mission-B"), TEXT("cargo-B"),
            {TEXT("star-A"), TEXT("new-B"), TEXT("new-C")}));
        TestTrue(TEXT("AC01 explicit replay"), F.Authority.ReplayExplicit(
            TEXT("mission-B"), TEXT("cargo-B"),
            {TEXT("new-A"), TEXT("new-B"), TEXT("new-C")}));
        const FDDSN08StableSnapshot Replay = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("AC01 replay empty owner"), Replay.Owner, EDDSN08Owner::Station);
        TestEqual(TEXT("AC01 replay zero stars"), Replay.CollectedStarIds.Num(), 0);
        TestEqual(TEXT("AC01 replay zero receipts"), Replay.Receipts.Num(), 0);
        TestEqual(TEXT("AC01 replay timer reset"), Replay.ElapsedGameplaySeconds, 0.0);
    }
    {
        FFixture F;
        const FDDSN08Observation O = F.Good();
        TestTrue(TEXT("AC05 pre-latch setup"), F.Authority.OnInteractButton(true, O).bAllowed);
        const FDDSN08Token Old = F.Authority.GetActiveToken();
        TestTrue(TEXT("AC05 star before cancellation"),
            F.Authority.RecordN10Star(TEXT("mission-A"), TEXT("star-A")));
        F.Authority.TickGameplay(2.0, false);
        F.Authority.TickGameplay(100.0, true);
        TestTrue(TEXT("AC05 pause does not timeout"), F.Authority.HasActiveOperation());
        TestEqual(TEXT("AC05 active recovery requests closure"),
            F.Authority.RequestRecoveryPolicy(false), EDDSN08Recovery::WaitForClosedLids);
        TestFalse(TEXT("AC03 old latch rejected after generation invalidation"),
            F.Authority.OnVerifiedLatch(FFixture::GoodLatch(Old)));
        TestEqual(TEXT("AC05 prior owner retained"), F.Authority.GetStableSnapshot().Owner,
            EDDSN08Owner::Station);
        TestEqual(TEXT("AC05 current time retained"),
            F.Authority.GetStableSnapshot().ElapsedGameplaySeconds, 2.0);
        TestEqual(TEXT("AC05 collected star retained"),
            F.Authority.GetStableSnapshot().CollectedStarIds.Num(), 1);
        TestTrue(TEXT("AC05 drive remains locked until closure"), F.Motion.bLocked);
        TestFalse(TEXT("AC05 incomplete closure cannot unlock"),
            F.Authority.OnCancelledClosureVerified(Old, false));
        TestTrue(TEXT("AC05 tagged both-lid closure unlocks"),
            F.Authority.OnCancelledClosureVerified(Old, true));
        TestFalse(TEXT("AC05 drive unlocked after closure"), F.Motion.bLocked);
        TestEqual(TEXT("AC05 closed-lid recovery may use N06 primitive"),
            F.Authority.RequestRecoveryPolicy(true), EDDSN08Recovery::Allowed);
        TestEqual(TEXT("AC05 no receipt on cancelled leg"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 0);
    }
    {
        FFixture F;
        const FDDSN08Observation O = F.Good();
        F.Authority.OnInteractButton(true, O);
        const FDDSN08Token Old = F.Authority.GetActiveToken();
        F.Authority.TickGameplay(4.9, false);
        TestTrue(TEXT("AC05 still active before 5 gameplay seconds"),
            F.Authority.HasActiveOperation());
        F.Authority.TickGameplay(0.11, false);
        TestFalse(TEXT("AC05 finite 5-second operation timeout"),
            F.Authority.HasActiveOperation());
        TestEqual(TEXT("AC05 timeout requests cleanup"), F.Mechanics.Cancelled.Num(), 1);
        TestFalse(TEXT("AC03 late timeout callback cannot commit"),
            F.Authority.OnVerifiedLatch(FFixture::GoodLatch(Old)));
    }
    {
        FFixture F;
        const FDDSN08StableSnapshot LastValidated = F.Authority.GetStableSnapshot();
        TestTrue(TEXT("AC05 staged unsaved transfer"), F.BeginAndLatch(TEXT("pickup-A")));
        TestFalse(TEXT("AC05 unvalidated restore denied"),
            F.Authority.RestoreValidatedCheckpoint(LastValidated, false, true, true));
        TestFalse(TEXT("AC05 open lid restore denied"),
            F.Authority.RestoreValidatedCheckpoint(LastValidated, true, false, true));
        TestFalse(TEXT("AC05 invalid pose restore denied"),
            F.Authority.RestoreValidatedCheckpoint(LastValidated, true, true, false));
        TestFalse(TEXT("AC05 pending old write prevents restore without barrier"),
            F.Authority.RestoreValidatedCheckpoint(LastValidated, true, true, true));
        TestTrue(TEXT("AC05 old gate stays pending after denied restore"),
            F.Authority.GetSaveGate() == EDDSN08Gate::Pending && F.Motion.bLocked);
        const FDDSN08SaveCandidate OldWrite = F.Authority.GetPinnedCandidate();
        F.Save.bRestoreBarrierReady = true;
        TestTrue(TEXT("AC05 explicit prior validated checkpoint rollback"),
            F.Authority.RestoreValidatedCheckpoint(LastValidated, true, true, true));
        TestTrue(TEXT("AC05 N11 barrier was requested"), F.Save.RestoreBarrierCalls >= 2);
        TestFalse(TEXT("AC05 controlled delayed old write cannot become disk head"),
            F.Save.CompleteControlledDelayedWrite(OldWrite));
        TestTrue(TEXT("AC05 controlled head is present after barrier"),
            F.Save.bHasControlledDurableHead);
        TestEqual(TEXT("AC05 controlled head remains prior owner"),
            F.Save.ControlledDurableHead.Owner, EDDSN08Owner::Station);
        TestFalse(TEXT("AC05 delayed old ack cannot restore unsaved commit"),
            F.Authority.OnSaveAck(FFixture::Ack(OldWrite)));
        TestEqual(TEXT("AC05 unsaved owner rolls back"),
            F.Authority.GetStableSnapshot().Owner, EDDSN08Owner::Station);
        TestEqual(TEXT("AC05 unsaved receipt does not survive restore"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 0);
    }
    {
        FFixture F;
        F.Save.bPrepareInvalid = true;
        TestTrue(TEXT("AC05 committed latch with bad provider preparation"),
            F.BeginAndLatch(TEXT("pickup-A")));
        TestEqual(TEXT("AC05 provider contract fault is distinct"),
            F.Authority.GetSaveGate(), EDDSN08Gate::ProviderFault);
        TestTrue(TEXT("AC05 provider fault retains pickup green"),
            F.Authority.GetPresentation().bPickupBeacon);
        TestEqual(TEXT("AC05 receipt retained through provider fault"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
        TestTrue(TEXT("AC05 provider fault keeps drive locked"), F.Motion.bLocked);
        TestFalse(TEXT("AC05 cannot retry nonexistent ticket"), F.Authority.RetryPinnedSave());
        F.Authority.TickGameplay(3.0, false);
        F.Save.bPrepareInvalid = false;
        TestTrue(TEXT("AC05 repaired provider prepares same payload/key"),
            F.Authority.RetryProviderPreparation());
        TestEqual(TEXT("AC05 no second receipt on provider repair"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
        TestEqual(TEXT("AC05 prepared snapshot has original commit time"),
            F.Authority.GetPinnedCandidate().Snapshot.ElapsedGameplaySeconds, 0.0);
        TestEqual(TEXT("AC05 committed operation ID reused"),
            F.Save.Submissions[0].Correlation.OperationId,
            F.Authority.GetPinnedCandidate().Correlation.OperationId);
        TestTrue(TEXT("AC05 repaired provider validated ack"),
            F.Authority.OnSaveAck(FFixture::Ack(F.Authority.GetPinnedCandidate())));
        TestFalse(TEXT("AC05 repaired provider unlocks"), F.Motion.bLocked);
    }
    {
        FFixture F;
        F.Save.bQueueSucceeds = false;
        TestTrue(TEXT("AC05 latch survives queue rejection without callback"),
            F.BeginAndLatch(TEXT("pickup-A")));
        TestEqual(TEXT("AC05 no-callback queue rejection is retryable"),
            F.Authority.GetSaveGate(), EDDSN08Gate::FailedRetryable);
        TestTrue(TEXT("AC05 queue rejection retains drive lock"), F.Motion.bLocked);
        TestEqual(TEXT("AC05 queue rejection retains one receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
        TestEqual(TEXT("AC05 queue rejection attempted one submission"),
            F.Save.Submissions.Num(), 1);
        const FDDSN08SaveCandidate Original = F.Authority.GetPinnedCandidate();
        F.Save.bQueueSucceeds = true;
        TestTrue(TEXT("AC05 queue rejection retries pinned candidate"),
            F.Authority.RetryPinnedSave());
        TestEqual(TEXT("AC05 retry does not re-prepare"), F.Save.PrepareCount, 1);
        TestEqual(TEXT("AC05 retry makes exactly second submission"),
            F.Save.Submissions.Num(), 2);
        TestEqual(TEXT("AC05 retry keeps operation token"),
            F.Save.Submissions[0].Correlation.OperationId,
            F.Save.Submissions[1].Correlation.OperationId);
        TestEqual(TEXT("AC05 retry keeps generation"),
            F.Save.Submissions[0].Correlation.Generation,
            F.Save.Submissions[1].Correlation.Generation);
        TestEqual(TEXT("AC05 retry keeps revision"),
            F.Save.Submissions[0].Revision, F.Save.Submissions[1].Revision);
        TestEqual(TEXT("AC05 retry keeps digest"),
            F.Save.Submissions[0].Digest, F.Save.Submissions[1].Digest);
        TestEqual(TEXT("AC05 retry still has one receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
        TestTrue(TEXT("AC05 matching durable ack after retry"),
            F.Authority.OnSaveAck(FFixture::Ack(Original)));
        TestEqual(TEXT("AC05 matching ack clears queue-failure gate"),
            F.Authority.GetSaveGate(), EDDSN08Gate::None);
        TestFalse(TEXT("AC05 matching ack unlocks after queue failure"), F.Motion.bLocked);
        TestTrue(TEXT("AC05 matching ack advances destination"),
            F.Authority.GetPresentation().bDeliveryBeacon);
    }
    {
        FFixture F;
        F.Save.bSynchronousValidatedAck = true;
        F.Save.bQueueSucceeds = false; // Deliberately contradictory adapter return.
        TestTrue(TEXT("AC05 sync validated callback commits before Submit returns"),
            F.BeginAndLatch(TEXT("pickup-A")));
        TestEqual(TEXT("AC05 sync validated gate stays cleared"),
            F.Authority.GetSaveGate(), EDDSN08Gate::None);
        TestFalse(TEXT("AC05 sync validated drive stays unlocked"), F.Motion.bLocked);
        TestEqual(TEXT("AC05 sync validated still one receipt"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 1);
    }
    {
        FFixture F;
        F.Save.bSynchronousFailedAck = true;
        TestTrue(TEXT("AC05 sync failed callback after latch"),
            F.BeginAndLatch(TEXT("pickup-A")));
        TestEqual(TEXT("AC05 sync failure gate remains retryable"),
            F.Authority.GetSaveGate(), EDDSN08Gate::FailedRetryable);
        TestTrue(TEXT("AC05 sync failure remains drive locked"), F.Motion.bLocked);
        F.Save.bSynchronousFailedAck = false;
        TestTrue(TEXT("AC05 sync failure retries original candidate"),
            F.Authority.RetryPinnedSave());
    }
    for (int32 StarCount = 0; StarCount <= 3; ++StarCount)
    {
        FFixture F;
        TestTrue(TEXT("AC01 saved pickup for each result count"), F.SavedPickup());
        const TCHAR* IDs[3] = { TEXT("star-C"), TEXT("star-A"), TEXT("star-B") };
        for (int32 I = 0; I < StarCount; ++I)
            TestTrue(TEXT("AC01 accepted scoped star"),
                F.Authority.RecordN10Star(TEXT("mission-A"), IDs[I]));
        TestTrue(TEXT("AC01 delivery can commit at every star count"),
            F.BeginAndLatch(TEXT("delivery-A")));
        const FDDSN08StableSnapshot S = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("AC01 frozen count is not a completion gate"),
            S.FrozenResult.Count, StarCount);
        TestEqual(TEXT("AC01 exact frozen set size"),
            S.FrozenResult.ExactStarIds.Num(), StarCount);
        TestEqual(TEXT("AC04 two receipts after delivery"), S.Receipts.Num(), 2);
        TestTrue(TEXT("AC04 delivery remains locked before ack"), F.Motion.bLocked);
        TestFalse(TEXT("AC04 completed presentation waits for save"),
            F.Authority.GetPresentation().bShowDelivered);
        TestTrue(TEXT("AC04 red remains through completion pending"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        const FDDSN08SaveCandidate Original = F.Authority.GetPinnedCandidate();
        FDDSN08SaveAck Wrong = FFixture::Ack(Original);
        Wrong.Correlation.MissionInstanceId = TEXT("other-instance");
        TestFalse(TEXT("AC03 wrong-instance ack rejected"), F.Authority.OnSaveAck(Wrong));
        Wrong = FFixture::Ack(Original);
        Wrong.Correlation.OperationId = TEXT("old-operation");
        TestFalse(TEXT("AC03 wrong-operation ack rejected"), F.Authority.OnSaveAck(Wrong));
        TestTrue(TEXT("AC05 delivery save failure retains commit"),
            F.Authority.OnSaveAck(FFixture::Ack(Original, false)));
        TestEqual(TEXT("AC05 retained result after failure"),
            F.Authority.GetStableSnapshot().FrozenResult.Count, StarCount);
        TestTrue(TEXT("AC05 red remains through delivery save failure"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestTrue(TEXT("AC05 delivery retry same candidate"), F.Authority.RetryPinnedSave());
        TestEqual(TEXT("AC05 delivery retry does not re-prepare"), F.Save.PrepareCount, 2);
        TestEqual(TEXT("AC05 delivery retry keeps revision"),
            F.Save.Submissions[1].Revision, F.Save.Submissions[2].Revision);
        TestEqual(TEXT("AC05 delivery retry keeps digest"),
            F.Save.Submissions[1].Digest, F.Save.Submissions[2].Digest);
        TestTrue(TEXT("AC04 delivery validated ack"), F.Authority.OnSaveAck(
            FFixture::Ack(Original)));
        TestTrue(TEXT("AC04 saved result visible"), F.Authority.GetPresentation().bShowDelivered);
        TestFalse(TEXT("AC04 red retires only after completion ack"),
            F.Authority.GetPresentation().bDeliveryBeacon);
        TestEqual(TEXT("AC04 delivery still exactly two receipts"),
            F.Authority.GetStableSnapshot().Receipts.Num(), 2);
    }
    return true; // Automation framework records failures from every Test* above.
}
#endif
