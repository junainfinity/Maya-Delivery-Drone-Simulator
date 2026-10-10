#include "DDSN15AudioIntents.h"
#include "DDSN15Presentation.h"
#include "DDSCityRollingPawn.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

// UNRUN source regressions. Declared mock ports use actual N08 transitions.
// No mock is real N09/N10/N11/N12, durable storage, pawn physics or playback.
namespace DDSN15Controlled {
struct FMotion final : IDDSN08MotionPort {
    bool bLocked = false;
    void SetDriveLocked(bool bValue) override { bLocked = bValue; }
};
struct FMechanics final : IDDSN08MechanicsPort {
    FDDSN08Token Begun, Cancelled;
    void BeginPhysicalCycle(const FDDSN08Token& T) override { Begun = T; }
    void CancelAndClose(const FDDSN08Token& T) override { Cancelled = T; }
};
struct FSave final : IDDSN08SavePort {
    FDDSN08Authority* Listener = nullptr;
    FDDSN15AudioRouter* Router = nullptr;
    TArray<FDDSN08SaveCandidate> Submitted;
    TArray<EDDSN15AudioCue> Effects;
    bool bSynchronous = false, bSyncValidated = true, bLastAccepted = false, bBarrierReady = true;
    int32 AckCalls = 0;
    uint64 Revision = 1;
    FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& S, const FDDSN08Token& T) override {
        FDDSN08SaveCandidate C; C.Snapshot = S; C.Correlation = T; C.Revision = Revision++;
        C.Digest = FString::Printf(TEXT("MOCK_NOT_DURABLE_%llu"), static_cast<unsigned long long>(C.Revision));
        return C;
    }
    static FDDSN08SaveAck Ack(const FDDSN08SaveCandidate& C, bool bValidated = true) {
        FDDSN08SaveAck A; A.Correlation = C.Correlation; A.Revision = C.Revision;
        A.Digest = C.Digest; A.bDurablyValidated = bValidated; return A;
    }
    EDDSN15AudioCue Dispatch(const FDDSN08SaveCandidate& C, const FDDSN08SaveAck& A) {
        ++AckCalls;
        const FDDSN08Presentation Before = Listener->GetPresentation();
        bLastAccepted = Listener->OnSaveAck(A); // Real accepted return, including matching failure.
        const EDDSN15AudioCue Cue = Router->OnAuthoritySaveAck(bLastAccepted, C, A, Before,
            Listener->GetStableSnapshot(), Listener->GetPresentation()).Cue;
        if (Cue != EDDSN15AudioCue::None) Effects.Add(Cue);
        return Cue;
    }
    bool Submit(const FDDSN08SaveCandidate& C) override {
        const FDDSN08SaveCandidate Exact = C; // Copy before synchronous callback clears gate.
        Submitted.Add(Exact);
        if (bSynchronous) Dispatch(Exact, Ack(Exact, bSyncValidated));
        return true;
    }
    FDDSN08RestoreBarrier EstablishRestoreBarrier(const FDDSN08StableSnapshot& S,
        const FDDSN08SaveCandidate* InFlight) override {
        FDDSN08RestoreBarrier B; B.MissionInstanceId = S.MissionInstanceId;
        B.bCheckpointDurablyCurrent = B.bOlderWritesCannotOvertake = bBarrierReady;
        B.FencedThroughRevision = InFlight ? InFlight->Revision : 0; return B;
    }
};
struct FFixture final {
    FMotion Motion; FMechanics Mechanics; FSave Save; FDDSN15AudioRouter Router;
    FDDSN08Authority Authority;
    FDDSN15PawnProjection Pawn; // Controlled read-only owned-pawn sample seam.
    FDDSN15ProjectionInputs Inputs;
    bool bStarted = false;
    FFixture(bool bStart = true) : Authority(Motion, Mechanics, Save) {
        Save.Listener = &Authority; Save.Router = &Router;
        if (bStart) bStarted = Authority.StartFreshMission(TEXT("mission-A"), TEXT("cargo-A"),
            TEXT("pickup-A"), TEXT("delivery-A"), {TEXT("A-1"), TEXT("A-2"), TEXT("A-3")});
        Inputs.bRealN12MechanicsAvailable = true; // Guard simulation ONLY; not real N12.
        Inputs.NowWorldSeconds = Inputs.DoorObservationWorldSeconds = 10.0;
    }
    FDDSN15ViewModel Build() const { return FDDSN15Presentation::Build(Authority, Pawn, Inputs); }
    void MockClosed() {
        Inputs.DoorObservationMissionId = Authority.GetStableSnapshot().MissionInstanceId;
        Inputs.bNativeDoorObservationKnown = true;
        Inputs.bLeftLidAtClosedPose = Inputs.bRightLidAtClosedPose = true;
        Inputs.bLeftLatchVerified = Inputs.bRightLatchVerified = true;
    }
    FDDSN08Observation Good() const {
        const FDDSN08StableSnapshot S = Authority.GetStableSnapshot(); FDDSN08Observation O;
        O.MissionInstanceId = S.MissionInstanceId; O.ObservedCargoId = S.CargoId;
        O.StationId = S.Phase == EDDSN08Phase::AwaitingPickup ? S.PickupStationId : S.DeliveryStationId;
        O.bKnown = O.bCargoObserved = O.bActiveCargoAtExpectedSource = O.bGrounded = true;
        O.StationPointCm = FVector(100,0,0);
        O.bServicePathObserved = O.bServicePathClear = true;
        O.bFullLidSweepObserved = O.bFullLidSweepClear = true;
        O.bClosedOperableDoorObserved = O.bClosedOperableDoor = true;
        O.ObservationWorldSeconds = 10.0; return O;
    }
    bool Begin() {
        const FDDSN08Observation O = Good();
        const bool bAllowed = Authority.OnInteractButton(true, O).bAllowed;
        Authority.OnInteractButton(false, O); return bAllowed;
    }
    static FDDSN08LatchEvidence Latch(const FDDSN08Token& T) {
        FDDSN08LatchEvidence E; E.Token = T; E.bMechanicalObservationKnown = true;
        E.bParcelStagedAtCorrectDestination = E.bFullOpeningAndClosingSweepClear = true;
        E.bLeftLidAtClosedPose = E.bRightLidAtClosedPose = true;
        E.bLeftLatchVerified = E.bRightLatchVerified = true; return E;
    }
    bool Commit() {
        if (!Begin()) return false;
        Authority.OnMechanicalProgress(Authority.GetActiveToken(), EDDSN08MechanicStep::Closing);
        return Authority.OnVerifiedLatch(Latch(Authority.GetActiveToken()));
    }
    bool SavedPickup() {
        if (!Commit()) return false;
        const FDDSN08SaveCandidate C = Save.Submitted.Last();
        return Save.Dispatch(C, FSave::Ack(C)) == EDDSN15AudioCue::PickupSaved;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN15IntentGateTest, "DDSCity.N15.IntentGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSN15IntentGateTest::RunTest(const FString& Parameters) {
    using namespace DDSN15Controlled; (void)Parameters;
    { FFixture F(false); const FDDSN15ViewModel M = F.Build();
      TestEqual(TEXT("no mission timer unavailable"), M.Timer, FString(TEXT("Time unavailable")));
      TestEqual(TEXT("no mission stars unavailable"), M.Stars, FString(TEXT("Route stars: Unavailable")));
      TestFalse(TEXT("no mission result hidden"), M.bShowResult);
      TestFalse(TEXT("no mission replay hidden"), M.bOfferReplay); }
    for (bool bDelivery : {false,true}) for (bool bRecovery : {false,true}) {
        FFixture F; TestTrue(TEXT("valid fresh mission"), F.bStarted);
        if (bDelivery) TestTrue(TEXT("setup saved pickup"), F.SavedPickup());
        TestTrue(TEXT("actual optional star acceptance"), F.Authority.RecordN10Star(TEXT("mission-A"), TEXT("A-1")));
        F.Authority.TickGameplay(12.5, false); const FDDSN08StableSnapshot Old = F.Authority.GetStableSnapshot();
        TestTrue(TEXT("authorized operation starts"), F.Begin()); const FDDSN08Token T = F.Authority.GetActiveToken();
        F.Authority.OnMechanicalProgress(T, EDDSN08MechanicStep::Opening);
        if (bRecovery) TestEqual(TEXT("recovery waits for closure"), F.Authority.RequestRecoveryPolicy(false), EDDSN08Recovery::WaitForClosedLids);
        else F.Authority.OnMechanicalFailure(T);
        TestFalse(TEXT("cancel clears operation"), F.Authority.HasActiveOperation());
        TestEqual(TEXT("cancel clears step"), F.Authority.GetMechanicStep(), EDDSN08MechanicStep::None);
        TestEqual(TEXT("cancel clears transient"), F.Authority.GetTransient(), EDDSN08Transient::None);
        TestTrue(TEXT("structured wait retained"), F.Authority.IsWaitingForClosedLids());
        TestTrue(TEXT("closure wait keeps drive lock"), F.Motion.bLocked);
        TestEqual(TEXT("actual N08 wait status"), F.Authority.GetPresentation().Status, FString(TEXT("Closing lids before drive/recovery")));
        TestEqual(TEXT("pending closure door"), F.Build().Door, FString(TEXT("Closing cargo door · Waiting for verification")));
        TestEqual(TEXT("pending closure not Ready"), F.Build().Status, FString(TEXT("Closing cargo door · Drive locked")));
        const FDDSN08StableSnapshot Now = F.Authority.GetStableSnapshot();
        TestEqual(TEXT("cancel preserves phase"), Now.Phase, Old.Phase);
        TestEqual(TEXT("cancel preserves cargo owner"), Now.Owner, Old.Owner);
        TestEqual(TEXT("cancel preserves stars"), FString::Join(Now.CollectedStarIds,TEXT("|")), FString::Join(Old.CollectedStarIds,TEXT("|")));
        TestEqual(TEXT("cancel preserves elapsed"), Now.ElapsedGameplaySeconds, Old.ElapsedGameplaySeconds);
        TestEqual(TEXT("cancel creates no receipt"), Now.Receipts.Num(), Old.Receipts.Num());
        for (int32 Repeat=0; Repeat<3; ++Repeat) {
            F.MockClosed(); // Even a supplied closed sample cannot bypass N08 wait.
            TestEqual(TEXT("stuck closure stays waiting"), F.Build().Status, FString(TEXT("Closing cargo door · Drive locked")));
            TestFalse(TEXT("unverified closure rejected"), F.Authority.OnCancelledClosureVerified(T,false));
        }
        FDDSN08Token Wrong=T; ++Wrong.Generation;
        TestFalse(TEXT("wrong closure token rejected"), F.Authority.OnCancelledClosureVerified(Wrong,true));
        TestTrue(TEXT("matching verified cancel closure accepted"), F.Authority.OnCancelledClosureVerified(T,true));
        TestFalse(TEXT("verified closure clears wait"), F.Authority.IsWaitingForClosedLids());
        TestFalse(TEXT("verified closure unlocks drive"), F.Motion.bLocked);
        F.Inputs.bNativeDoorObservationKnown=false;
        TestEqual(TEXT("ack without current physical fact unavailable"), F.Build().Door, FString(TEXT("Cargo door: Unavailable")));
        TestEqual(TEXT("availability does not prove Ready"), F.Build().Status, FString(TEXT("Cargo door verification unavailable")));
        F.MockClosed(); TestEqual(TEXT("current controlled closed facts"), F.Build().Door, FString(TEXT("Cargo door: Closed")));
        TestEqual(TEXT("verified current closed permits Ready"), F.Build().Status, FString(TEXT("Ready")));
        F.Inputs.DoorObservationWorldSeconds=9;
        TestEqual(TEXT("stale door sample unavailable"), F.Build().Door, FString(TEXT("Cargo door: Unavailable")));
        F.Inputs.DoorObservationWorldSeconds=11;
        TestEqual(TEXT("future door sample unavailable"), F.Build().Door, FString(TEXT("Cargo door: Unavailable")));
        F.Inputs.DoorObservationWorldSeconds=10; F.Inputs.DoorObservationMissionId=TEXT("old-mission");
        TestEqual(TEXT("wrong mission door sample unavailable"), F.Build().Door, FString(TEXT("Cargo door: Unavailable")));
    }
    for (bool bDelivery : {false,true}) {
        FFixture F; if (bDelivery) TestTrue(TEXT("setup pickup ack"), F.SavedPickup());
        TestTrue(TEXT("coherent latch commits"), F.Commit());
        const FDDSN08SaveCandidate C=F.Save.Submitted.Last(); const FDDSN08StableSnapshot S=F.Authority.GetStableSnapshot();
        TestTrue(TEXT("candidate exact full correlation"), C.Correlation.Equals(F.Mechanics.Begun));
        TestEqual(TEXT("candidate exact station"), C.Correlation.StationId, bDelivery ? S.DeliveryStationId : S.PickupStationId);
        TestEqual(TEXT("candidate exact phase"), C.Snapshot.Phase, S.Phase);
        TestEqual(TEXT("candidate exact cargo owner"), C.Snapshot.Owner, S.Owner);
        TestEqual(TEXT("coherent receipt count"), C.Snapshot.Receipts.Num(), bDelivery ? 2 : 1);
        const FDDSN08Receipt& R=C.Snapshot.Receipts.Last();
        TestEqual(TEXT("receipt exact mission"), R.MissionInstanceId,C.Correlation.MissionInstanceId);
        TestEqual(TEXT("receipt exact operation"), R.OperationId,C.Correlation.OperationId);
        TestEqual(TEXT("receipt exact generation"), R.Generation,C.Correlation.Generation);
        TestEqual(TEXT("receipt exact prephase"), R.PrePhase,C.Correlation.PrePhase);
        TestEqual(TEXT("receipt exact station"), R.StationId,C.Correlation.StationId);
        TestEqual(TEXT("receipt exact cargo"), R.CargoId,C.Snapshot.CargoId);
        TestEqual(TEXT("actual save pending"),F.Authority.GetSaveGate(),EDDSN08Gate::Pending);
        TestFalse(TEXT("pending result hidden"),F.Build().bShowResult);
        const int32 Effects=F.Save.Effects.Num();
        for (int32 I=0; I<4; ++I) {
            FDDSN08SaveAck Wrong=FSave::Ack(C);
            if(I==0) Wrong.Digest+=TEXT("wrong"); if(I==1) ++Wrong.Revision;
            if(I==2) Wrong.Correlation.StationId=TEXT("wrong"); if(I==3) ++Wrong.Correlation.Generation;
            TestEqual(TEXT("actual mismatch silent"),F.Save.Dispatch(C,Wrong),EDDSN15AudioCue::None);
            TestFalse(TEXT("actual authority rejects mismatch"),F.Save.bLastAccepted);
            TestEqual(TEXT("mismatch retains gate"),F.Authority.GetSaveGate(),EDDSN08Gate::Pending);
        }
        TestEqual(TEXT("matched failed ack silent"),F.Save.Dispatch(C,FSave::Ack(C,false)),EDDSN15AudioCue::None);
        TestTrue(TEXT("real authority accepts failed callback"),F.Save.bLastAccepted);
        TestEqual(TEXT("failed ack retry gate"),F.Authority.GetSaveGate(),EDDSN08Gate::FailedRetryable);
        TestTrue(TEXT("retry offered"),F.Build().bOfferRetrySave); TestTrue(TEXT("warned restore offered"),F.Build().bOfferRestore);
        TestFalse(TEXT("failed ack replay hidden"),F.Build().bOfferReplay);
        TestTrue(TEXT("retry pinned candidate"),F.Authority.RetryPinnedSave());
        const FDDSN08SaveCandidate Retry=F.Save.Submitted.Last();
        TestEqual(TEXT("immutable retry digest"),Retry.Digest,C.Digest);
        TestEqual(TEXT("immutable retry revision"),Retry.Revision,C.Revision);
        TestTrue(TEXT("immutable retry correlation"),Retry.Correlation.Equals(C.Correlation));
        TestEqual(TEXT("accepted live saved cue"),F.Save.Dispatch(C,FSave::Ack(C)), bDelivery ? EDDSN15AudioCue::DeliverySaved : EDDSN15AudioCue::PickupSaved);
        TestTrue(TEXT("actual success accepted"),F.Save.bLastAccepted);
        TestEqual(TEXT("ack clears gate"),F.Authority.GetSaveGate(),EDDSN08Gate::None);
        TestEqual(TEXT("exactly one new effect"),F.Save.Effects.Num(),Effects+1);
        TestEqual(TEXT("actual duplicate callback silent"),F.Save.Dispatch(C,FSave::Ack(C)),EDDSN15AudioCue::None);
        TestFalse(TEXT("actual duplicate rejected"),F.Save.bLastAccepted);
    }
    for(bool bValidated : {false,true}) {
        FFixture F; F.Save.bSynchronous=true; F.Save.bSyncValidated=bValidated;
        TestTrue(TEXT("synchronous pickup commit"),F.Commit()); TestTrue(TEXT("actual sync callback accepted"),F.Save.bLastAccepted);
        TestEqual(TEXT("sync pickup cue count"),F.Save.Effects.Num(),bValidated ? 1 : 0);
        TestEqual(TEXT("sync pickup gate"),F.Authority.GetSaveGate(),bValidated ? EDDSN08Gate::None : EDDSN08Gate::FailedRetryable);
        if(!bValidated) { const FDDSN08SaveCandidate C=F.Save.Submitted.Last(); F.Save.Dispatch(C,FSave::Ack(C)); }
        TestTrue(TEXT("synchronous delivery commit"),F.Commit());
        TestEqual(TEXT("sync delivery cue count"),F.Save.Effects.Num(),bValidated ? 2 : 1);
        TestEqual(TEXT("sync delivery result eligibility"),F.Build().bShowResult,bValidated);
    }
    for(int32 Count=0; Count<=3; ++Count) {
        FFixture F; TestTrue(TEXT("frozen setup pickup"),F.SavedPickup());
        for(int32 I=0; I<Count; ++I) {
            const FString Id=F.Authority.GetStableSnapshot().ScopedStarIds[I];
            const bool Accepted=F.Authority.RecordN10Star(TEXT("mission-A"),Id);
            TestTrue(TEXT("actual N08 optional star accepted"),Accepted);
            TestEqual(TEXT("star intent after accepted contact"),F.Router.OnAuthorityStarAccepted(Accepted,TEXT("mission-A"),Id,F.Authority.GetStableSnapshot()).Cue,EDDSN15AudioCue::StarCollected);
            TestFalse(TEXT("actual N08 duplicate star rejected"),F.Authority.RecordN10Star(TEXT("mission-A"),Id));
        }
        F.Authority.TickGameplay(72.75,false); TestTrue(TEXT("delivery freezes actual result"),F.Commit());
        const FDDSN08SaveCandidate C=F.Save.Submitted.Last();
        TestTrue(TEXT("candidate frozen result present"),C.Snapshot.bHasFrozenResult);
        TestEqual(TEXT("candidate exact frozen count"),C.Snapshot.FrozenResult.Count,Count);
        TestEqual(TEXT("candidate exact frozen IDs"),FString::Join(C.Snapshot.FrozenResult.ExactStarIds,TEXT("|")),FString::Join(C.Snapshot.CollectedStarIds,TEXT("|")));
        TestEqual(TEXT("candidate exact frozen elapsed"),C.Snapshot.FrozenResult.FrozenElapsedSeconds,72.75);
        TestFalse(TEXT("frozen result before ack hidden"),F.Build().bShowResult);
        TestFalse(TEXT("pre-ack replay rejected"),F.Authority.ReplayExplicit(TEXT("mission-B"),TEXT("cargo-B"),{TEXT("B-1"),TEXT("B-2"),TEXT("B-3")}));
        TestEqual(TEXT("delivery accepted saved effect"),F.Save.Dispatch(C,FSave::Ack(C)),EDDSN15AudioCue::DeliverySaved);
        const FDDSN08StableSnapshot Completed=F.Authority.GetStableSnapshot(); F.Authority.TickGameplay(300,false);
        TestEqual(TEXT("exploration keeps frozen timer"),F.Build().Timer,FString(TEXT("Time 01:12")));
        TestEqual(TEXT("frozen star display"),F.Build().Stars,FString::Printf(TEXT("Route stars %d/3"),Count));
        const int32 Effects=F.Save.Effects.Num();
        for(int32 I=0; I<3; ++I) TestTrue(TEXT("dismiss/reopen model preserves immutable result"),F.Build().bShowResult);
        TestEqual(TEXT("projection emits no saved effect"),F.Save.Effects.Num(),Effects);
        TestFalse(TEXT("same-instance replay rejected"),F.Authority.ReplayExplicit(TEXT("mission-A"),TEXT("cargo-B"),{TEXT("B-1"),TEXT("B-2"),TEXT("B-3")}));
        TestTrue(TEXT("explicit new-instance replay accepted"),F.Authority.ReplayExplicit(TEXT("mission-B"),TEXT("cargo-B"),{TEXT("B-1"),TEXT("B-2"),TEXT("B-3")}));
        TestEqual(TEXT("new mission instance"),F.Authority.GetStableSnapshot().MissionInstanceId,FString(TEXT("mission-B")));
        TestEqual(TEXT("replay timer reset"),F.Build().Timer,FString(TEXT("Time 00:00")));
        TestEqual(TEXT("replay stars reset"),F.Build().Stars,FString(TEXT("Route stars 0/3"))); TestFalse(TEXT("replay result removed"),F.Build().bShowResult);
        FFixture Reload(false); Reload.Save.bBarrierReady=false;
        TestFalse(TEXT("mock barrier failure blocks restore"),Reload.Authority.RestoreValidatedCheckpoint(Completed,true,true,true));
        Reload.Save.bBarrierReady=true;
        TestTrue(TEXT("controlled validated snapshot reload"),Reload.Authority.RestoreValidatedCheckpoint(Completed,true,true,true));
        TestTrue(TEXT("first load projection shows saved result"),Reload.Build().bShowResult);
        TestEqual(TEXT("reload frozen stars"),Reload.Build().Stars,FString::Printf(TEXT("Route stars %d/3"),Count));
        TestEqual(TEXT("reload has no ack audio boundary"),Reload.Save.AckCalls,0);
        TestEqual(TEXT("new router/load has no saved cue"),Reload.Save.Effects.Num(),0);
    }
    { FDDSN15AudioRouter R; FDDSMotionSample M; M.WorldSeconds=10; M.bGrounded=true; M.SupportedWheels=6;
      M.bCompletedWheelMotionMeasured=true; M.CompletedWheelSpeedCmPerSecond=30;
      TestEqual(TEXT("completed stationary pivot motor"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::MotorUpdate);
      M.bBodyBlocked=M.bBodyBlockedDuringCompletedWheelSample=true;
      TestEqual(TEXT("collision-rejected pivot stop"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::MotorStop);
      TestEqual(TEXT("blocked pivot cannot restart"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::None);
      M.bBodyBlocked=false;
      TestEqual(TEXT("blocked earlier substep silent"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::None);
      M.bBodyBlockedDuringCompletedWheelSample=false; M.ActualVelocityCmPerSecond=FVector(-50,0,0); M.CompletedWheelSpeedCmPerSecond=50;
      TestEqual(TEXT("actual reverse travel motor"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::MotorUpdate);
      TestEqual(TEXT("stale sample stops"),R.UpdateMeasuredMotor(M,11,false).Cue,EDDSN15AudioCue::MotorStop);
      TestEqual(TEXT("future sample silent"),R.UpdateMeasuredMotor(M,9,false).Cue,EDDSN15AudioCue::None);
      TestEqual(TEXT("fresh sample restarts"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::MotorUpdate);
      TestEqual(TEXT("paused sample stops"),R.UpdateMeasuredMotor(M,10,true).Cue,EDDSN15AudioCue::MotorStop);
      R.UpdateMeasuredMotor(M,10,false); TestEqual(TEXT("immediate pause/recovery event stop"),R.StopForPauseOrRecovery().Cue,EDDSN15AudioCue::MotorStop);
      R.UpdateMeasuredMotor(M,10,false); TestEqual(TEXT("teardown stops"),R.StopForTeardown().Cue,EDDSN15AudioCue::MotorStop);
      TestEqual(TEXT("teardown idempotent"),R.StopForTeardown().Cue,EDDSN15AudioCue::None);
      M.ActualVelocityCmPerSecond=FVector::ZeroVector; M.CompletedWheelSpeedCmPerSecond=0;
      TestEqual(TEXT("stationary sample silent"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::None);
      M.CompletedWheelSpeedCmPerSecond=30; M.bCompletedWheelMotionMeasured=false;
      TestEqual(TEXT("unmeasured wheel value cannot invent pivot"),R.UpdateMeasuredMotor(M,10,false).Cue,EDDSN15AudioCue::None); }
    { FFixture F; TestTrue(TEXT("step fixture begins"),F.Begin()); const FDDSN08Token T=F.Authority.GetActiveToken();
      F.Authority.OnMechanicalProgress(T,EDDSN08MechanicStep::Opening);
      TestEqual(TEXT("fake provenance cannot authorize step"),F.Router.OnNativeN12StepObserved(T,EDDSN08MechanicStep::Opening,false,F.Authority).Cue,EDDSN15AudioCue::None);
      TestEqual(TEXT("mock native flag exercises step gate only"),F.Router.OnNativeN12StepObserved(T,EDDSN08MechanicStep::Opening,true,F.Authority).Cue,EDDSN15AudioCue::NativeDoorOpening);
      const FDDSN08LatchEvidence E=FFixture::Latch(T);
      TestEqual(TEXT("fake provenance cannot authorize latch"),F.Router.OnNativeN12LatchObserved(E,false).Cue,EDDSN15AudioCue::None);
      TestEqual(TEXT("mock latch click separate from durable save"),F.Router.OnNativeN12LatchObserved(E,true).Cue,EDDSN15AudioCue::NativeLatchClick);
      TestEqual(TEXT("latch emits no saved cue"),F.Save.Effects.Num(),0);
      FDDSN08Eligibility Denied; Denied.Reason=EDDSN08Denial::Moving;
      TestEqual(TEXT("new denied press cue"),F.Router.OnAuthorityDeniedInteract(true,Denied).Cue,EDDSN15AudioCue::DeniedInteract);
      TestEqual(TEXT("held press cannot spam"),F.Router.OnAuthorityDeniedInteract(false,Denied).Cue,EDDSN15AudioCue::None); }
    return true;
}
#endif
