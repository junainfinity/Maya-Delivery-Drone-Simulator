#include "DDSN08Coordinator.h"

#include "DDSCityRollingPawn.h"
#include "DDSN10RouteStar.h"
#include "DDSN10StarBridge.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"

namespace
{
bool MatchesCurrentPhysicalWitness(const FDDSN08PhysicalSafetyWitness& W,
    const FDDSN08Token& Token, const ADDSCityRollingPawn& Pawn)
{
    FDDSRobotSnapshot S;
    // The accepted rig enforces its frozen service and sample age bounds.
    // Reuse that admission instead of guessing another clock/tolerance.
    return Pawn.GetFreshPhysicalServiceEvidence(S) && W.Operation.Equals(Token)
        && S.bCompleteState && S.bServiceStable && S.bWorldReady
        && W.PhysicsGeneration == S.Generation
        && W.CollisionWorldGeneration == S.CollisionWorldGeneration
        && W.ReadinessRevision == S.ReadinessRevision
        && W.CompletedStepSequence == S.StepSequence
        && W.CompletedStateTimeS == S.CompletedStateTimeS
        && W.CapturedRealS == S.CapturedRealS
        && W.bFreshPhysicalStableSupport && W.bCurrentLidSweepAndServicePathClear
        && W.bBothActualLidsClosedLatched && W.bExpectedParcelPhysicalOwnerAndMass;
}
}

UDDSN08Coordinator::UDDSN08Coordinator()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UDDSN08Coordinator::FAuthorityCallScope::FAuthorityCallScope(UDDSN08Coordinator& InOwner)
    : Owner(InOwner)
{
    ++Owner.AuthorityCallDepth;
}
UDDSN08Coordinator::FAuthorityCallScope::~FAuthorityCallScope()
{
    if (--Owner.AuthorityCallDepth == 0 && Owner.bEndingPlay) Owner.ReleaseOwnedState();
}
void UDDSN08Coordinator::ReleaseOwnedState()
{
    if (SaveAdapter) SaveAdapter->ClearAckSink();
    ReadNativeObservation = FNativeObservationReader();
    ReadNativeMission = FNativeMissionReader();
    ReadmitRestoreAfterDurableIO = TFunction<bool()>();
    NativeMechanics = nullptr; NativeMechanicsLifetime.Reset();
    Authority.Reset(); SaveAdapter.Reset(); Store.Reset(); FileIO.Reset();
}

void UDDSN08Coordinator::BeginPlay()
{
    Super::BeginPlay();
    Pawn = Cast<ADDSCityRollingPawn>(GetOwner());
    if (!Pawn.IsValid()) return;
    AddTickPrerequisiteActor(Pawn.Get());
    FileIO = MakeUnique<FDDSN11UnrealFileIO>();
    Store = MakeUnique<FDDSN11SerializedStore>(*FileIO,
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames/DDSCity")));
    SaveAdapter = MakeUnique<FDDSN11SaveAdapter>(*Store);
    Authority = MakeUnique<FDDSN08Authority>(*this, *this, *this);
    SaveAdapter->SetAckSink([this](const FDDSN08SaveAck& Ack) { return OnStorageAck(Ack); });
    // No automatic mission, disk restore, parcel creation or native proof.
    // Explicit entry points remain unavailable until the actual owner binds.
    UE_LOG(LogTemp, Display, TEXT("DDS_ASSEMBLY providersBound=0 missionInitialized=0 nativeEvidence=0"));
}

void UDDSN08Coordinator::EndPlay(const EEndPlayReason::Type Reason)
{
    bEndingPlay = true;
    LastAudioIntent = AudioRouter.StopForTeardown();
    for (const auto& WeakStar : RouteStars)
        if (ADDSN10RouteStar* Star = WeakStar.Get()) Star->OnValidatedContact().RemoveAll(this);
    if (Pawn.IsValid()) Pawn->SetMovementLocked(true);
    if (AuthorityCallDepth == 0) ReleaseOwnedState();
    Super::EndPlay(Reason);
}

bool UDDSN08Coordinator::HasNativeMechanics() const
{
    const UActorComponent* Provider = NativeMechanicsLifetime.Get();
    return !bEndingPlay && Pawn.IsValid() && Provider && NativeMechanics
        && ReadNativeObservation && Provider->GetOwner() == Pawn.Get()
        && Provider->IsRegistered() && Provider->GetWorld() == GetWorld();
}

bool UDDSN08Coordinator::BindNativeMechanics(UActorComponent& LifetimeOwner,
    IDDSN08MechanicsPort& Port, FNativeObservationReader Reader)
{
    check(IsInGameThread());
    if (bEndingPlay || !Authority || !Pawn.IsValid() || Authority->HasActiveOperation()
        || Authority->IsWaitingForClosedLids() || Authority->GetSaveGate() != EDDSN08Gate::None
        || &LifetimeOwner == this || &Port == static_cast<IDDSN08MechanicsPort*>(this)
        || LifetimeOwner.GetOwner() != Pawn.Get() || !LifetimeOwner.IsRegistered()
        || LifetimeOwner.GetWorld() != GetWorld() || !Reader) return false;
    // The actual owner must guarantee Port/Reader belong to LifetimeOwner.
    // No replacement provider or fabricated physical observations are built.
    NativeMechanicsLifetime = &LifetimeOwner;
    NativeMechanics = &Port;
    ReadNativeObservation = MoveTemp(Reader);
    ReadNativeMission = FNativeMissionReader();
    ReadmitRestoreAfterDurableIO = TFunction<bool()>();
    ++NativeBindingRevision;
    return true;
}

void UDDSN08Coordinator::UnbindNativeMechanics()
{
    check(IsInGameThread());
    FAuthorityCallScope Scope(*this);
    StopAudioAtPauseOrRecovery();
    if (Authority && Authority->HasActiveOperation())
        Authority->OnMechanicalFailure(Authority->GetActiveToken());
    ReadNativeObservation = FNativeObservationReader();
    ReadNativeMission = FNativeMissionReader();
    ReadmitRestoreAfterDurableIO = TFunction<bool()>();
    NativeMechanics = nullptr; NativeMechanicsLifetime.Reset();
    ++NativeBindingRevision;
    AcceptedLatchWitness = FDDSN08PhysicalSafetyWitness();
    // Provider loss is not an unlock. Only matching closure/save ACK can
    // release an existing authority lock. Full recovery remains unavailable.
    if (Pawn.IsValid()) Pawn->SetMovementLocked(true);
}

bool UDDSN08Coordinator::BindExistingWorldActors(
    const TArray<ADDSN09ServiceBay*>& Bays, const TArray<ADDSN10RouteStar*>& Stars)
{
    check(IsInGameThread());
    if (!Authority || Authority->HasActiveOperation() || bEndingPlay
        || Bays.Num() != 2 || Stars.Num() != 3) return false;
    TSet<FString> BayIds, StarSlots;
    int32 PickupCount = 0, DeliveryCount = 0;
    for (ADDSN09ServiceBay* Bay : Bays)
    {
        if (!IsValid(Bay) || Bay->GetWorld() != GetWorld() || Bay->GetStationId().TrimStartAndEnd().IsEmpty()
            || BayIds.Contains(Bay->GetStationId().ToLower())) return false;
        BayIds.Add(Bay->GetStationId().ToLower());
        Bay->GetLeg() == EDDSN08Leg::Pickup ? ++PickupCount : ++DeliveryCount;
    }
    for (ADDSN10RouteStar* Star : Stars)
    {
        if (!IsValid(Star) || Star->GetWorld() != GetWorld() || Star->GetStableSlotId().TrimStartAndEnd().IsEmpty()
            || StarSlots.Contains(Star->GetStableSlotId().ToLower())) return false;
        StarSlots.Add(Star->GetStableSlotId().ToLower());
    }
    if (PickupCount != 1 || DeliveryCount != 1) return false;
    for (const auto& WeakStar : RouteStars)
        if (ADDSN10RouteStar* Star = WeakStar.Get()) Star->OnValidatedContact().RemoveAll(this);
    ServiceBays.Reset(); RouteStars.Reset();
    for (ADDSN09ServiceBay* Bay : Bays) ServiceBays.Add(Bay);
    for (ADDSN10RouteStar* Star : Stars)
    {
        RouteStars.Add(Star);
        Star->OnValidatedContact().AddUObject(this, &UDDSN08Coordinator::OnValidatedStar);
    }
    RefreshWorldProjection();
    return true;
}

void UDDSN08Coordinator::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    FAuthorityCallScope Scope(*this);
    if (!Authority || bEndingPlay) return;
    Authority->TickGameplay(DeltaTime, UGameplayStatics::IsGamePaused(this));
    if (NativeMechanics && !HasNativeMechanics()) UnbindNativeMechanics();
    RefreshWorldProjection();
    // Physical motor/contact classification is not yet admitted. No proxy
    // displacement/yaw sample or requested input is an audio measurement.
}

ADDSN09ServiceBay* UDDSN08Coordinator::FindCurrentBay() const
{
    if (!Authority) return nullptr;
    const FDDSN08StableSnapshot State = Authority->GetStableSnapshot();
    const EDDSN08Leg Leg = State.Phase == EDDSN08Phase::AwaitingPickup
        ? EDDSN08Leg::Pickup : EDDSN08Leg::Delivery;
    const FString Id = Leg == EDDSN08Leg::Pickup ? State.PickupStationId : State.DeliveryStationId;
    ADDSN09ServiceBay* Found = nullptr;
    for (const auto& WeakBay : ServiceBays)
        if (ADDSN09ServiceBay* Bay = WeakBay.Get())
            if (Bay->GetStationId() == Id && Bay->GetLeg() == Leg)
            { if (Found) return nullptr; Found = Bay; }
    return Found;
}

FDDSN08Observation UDDSN08Coordinator::Observe(FDDSN15ProjectionInputs* ViewInputs) const
{
    FAuthorityCallScope Scope(*const_cast<UDDSN08Coordinator*>(this));
    FDDSN08Observation Unknown;
    if (!Authority || !Pawn.IsValid() || !GetWorld() || !HasNativeMechanics()) return Unknown;
    ADDSN09ServiceBay* Bay = FindCurrentBay();
    const USceneComponent* Socket = Pawn->GetN08MeasuredServiceSocket();
    if (!Bay || !Socket) return Unknown;
    const auto ReadPhysical=[&](FDDSRobotSnapshot& Out)
    { return Authority && Authority->HasActiveOperation()
        ? Pawn->GetFreshPhysicalStateEvidence(Out) && Out.bChassisSupportStable
        : Pawn->GetFreshPhysicalServiceEvidence(Out); };
    FDDSRobotSnapshot Physical;
    if (!ReadPhysical(Physical)) return Unknown;
    FDDSN09ExternalCargoDoorObservation Facts;
    FDDSN15ProjectionInputs NativeView;
    // A local function copy survives an observer's synchronous unbind.
    const uint64 Revision = NativeBindingRevision;
    const FNativeObservationReader Reader = ReadNativeObservation;
    if (!Reader(*Bay, Facts, NativeView) || bEndingPlay || !HasNativeMechanics()
        || Revision != NativeBindingRevision) return Unknown;
    // Reader may revoke/advance N06 without rebinding N12. The pre-callback
    // witness is usable only if the live rig still admits that exact state.
    FDDSRobotSnapshot Current;
    if (!ReadPhysical(Current)
        || Current.Generation != Physical.Generation
        || Current.CollisionWorldGeneration != Physical.CollisionWorldGeneration
        || Current.ReadinessRevision != Physical.ReadinessRevision
        || Current.StepSequence != Physical.StepSequence
        || Current.CapturedRealS != Physical.CapturedRealS) return Unknown;
    // Actual external source owns cargo/lid assertions and freshness. Bind
    // contract requires the same current owned-pawn/mission observation; this
    // consumer also rejects stale/future/mismatched mission view provenance.
    const auto State = Authority->GetStableSnapshot();
    const double Now = GetWorld()->GetTimeSeconds();
    if (!NativeView.bRealN12MechanicsAvailable || !NativeView.bNativeDoorObservationKnown
        || NativeView.DoorObservationMissionId != State.MissionInstanceId
        || !FMath::IsFinite(NativeView.DoorObservationWorldSeconds)
        || NativeView.DoorObservationWorldSeconds < 0 || NativeView.DoorObservationWorldSeconds > Now
        || Now - NativeView.DoorObservationWorldSeconds > 0.20) return Unknown;
    NativeView.NowWorldSeconds = Now;
    FDDSN09GeometryObservation Geometry = Bay->ObserveGeometry(*Pawn, *Socket);
    // Consume current authoritative motion/support, not the cache that N09's
    // diagnostic geometry reads between pawn ticks. Geometry never moves the body.
    Geometry.ActualVelocityCmPerSecond = Current.Bodies[0].VelocityCmps;
    Geometry.bGrounded = Current.SupportMask != 0;
    FDDSRobotSnapshot AtUse;
    if (!ReadPhysical(AtUse)
        || AtUse.Generation != Current.Generation
        || AtUse.CollisionWorldGeneration != Current.CollisionWorldGeneration
        || AtUse.ReadinessRevision != Current.ReadinessRevision
        || AtUse.StepSequence != Current.StepSequence
        || AtUse.CapturedRealS != Current.CapturedRealS) return Unknown;
    if (ViewInputs) *ViewInputs = NativeView;
    return DDSN09ServiceBridge::BuildCurrentObservation(State, Bay->GetStationId(), Bay->GetLeg(),
        Geometry, Facts);
}

FDDSN08Eligibility UDDSN08Coordinator::OnInteractInput(bool bPressed)
{
    FAuthorityCallScope Scope(*this);
    FDDSN08Eligibility Result;
    if (!Authority || bEndingPlay) return Result;
    const bool NewPress = bPressed && !bInteractDown;
    bInteractDown = bPressed;
    FDDSN08Observation O = Observe();
    if (UGameplayStatics::IsGamePaused(this)) O.bKnown = false;
    Result = Authority->OnInteractButton(bPressed, O);
    LastAudioIntent = AudioRouter.OnAuthorityDeniedInteract(NewPress, Result);
    RefreshWorldProjection();
    return Result;
}

void UDDSN08Coordinator::ReleaseInteractAtInputBoundary()
{
    bInteractDown = false;
    if (Authority) Authority->OnInteractButton(false, FDDSN08Observation());
}

FString UDDSN08Coordinator::BuildPrompt() const
{
    FAuthorityCallScope Scope(*const_cast<UDDSN08Coordinator*>(this));
    if (!Authority || !HasNativeMechanics()) return TEXT("Interact unavailable");
    const FDDSN08Eligibility E = Authority->EvaluateProspectivePrompt(Observe());
    ADDSN09ServiceBay* Bay = FindCurrentBay();
    if (!Bay) return TEXT("Interact unavailable");
    return ADDSN09ServiceBay::ProjectReadOnly(Bay->GetStationId(), Bay->GetLeg(),
        Authority->GetStableSnapshot(), Authority->GetPresentation(), E).Prompt;
}

FDDSN15ViewModel UDDSN08Coordinator::BuildViewModel() const
{
    FAuthorityCallScope Scope(*const_cast<UDDSN08Coordinator*>(this));
    if (!Authority || !Pawn.IsValid()) return FDDSN15ViewModel();
    FDDSN15ProjectionInputs Inputs;
    Observe(&Inputs);
    Inputs.N09InteractionPrompt = BuildPrompt();
    FDDSN15ViewModel Model = FDDSN15Presentation::Build(*Authority, *Pawn, Inputs);
    if (!Pawn->GetMotionSample().bPhysicalStateKnown) Model.Speed = TEXT("Speed: Unavailable");
    return Model;
}

void UDDSN08Coordinator::RefreshWorldProjection()
{
    FAuthorityCallScope Scope(*this);
    if (!Authority || !Pawn.IsValid()) return;
    const auto State = Authority->GetStableSnapshot();
    const auto P = Authority->GetPresentation();
    const auto Eligibility = Authority->EvaluateProspectivePrompt(Observe());
    for (const auto& WeakBay : ServiceBays)
        if (ADDSN09ServiceBay* Bay = WeakBay.Get()) Bay->ApplyReadOnly(State, P, Eligibility);
    for (const auto& WeakStar : RouteStars)
        if (ADDSN10RouteStar* Star = WeakStar.Get()) Star->ApplyAuthoritySnapshot(State, P.SaveGate, Pawn.Get());
}

void UDDSN08Coordinator::OnValidatedStar(const FString& Instance, const FString& StarId,
    ADDSCityRollingPawn* ContactPawn)
{
    FAuthorityCallScope Scope(*this);
    if (!Authority || !HasNativeMechanics()) return;
    FDDSRobotSnapshot Physical;
    if (!Pawn->GetFreshPhysicalStateEvidence(Physical)
        || FMath::CountBits(static_cast<uint64>(Physical.SupportMask)) < 3) return;
    const bool Accepted = DDSN10StarBridge::TryRecordValidatedContact(*Authority, Instance,
        StarId, ContactPawn, Pawn.Get());
    LastAudioIntent = AudioRouter.OnAuthorityStarAccepted(Accepted, Instance,
        StarId, Authority->GetStableSnapshot());
    RefreshWorldProjection();
    if (Accepted)
    {
        FDDSN11Record DurableProgress; FString SaveError;
        SaveCurrentStableProgress(DurableProgress, SaveError);
        // Failure leaves accepted in-memory star progress intact and explicit
        // retry available; this path never fabricates a transfer ACK or gate.
    }
}

bool UDDSN08Coordinator::OnNativeVerifiedLatch(const FDDSN08LatchEvidence& Evidence,
    const FDDSN08PhysicalSafetyWitness& Witness, const FDDSN11Pose& Pose)
{
    check(IsInGameThread());
    FAuthorityCallScope Scope(*this);
    if (!Authority || !SaveAdapter || !HasNativeMechanics()
        || !Authority->HasActiveOperation() || !Authority->GetActiveToken().Equals(Evidence.Token)
        || !MatchesCurrentPhysicalWitness(Witness, Evidence.Token, *Pawn)
        || !Pose.bGroundedSidewalkValidated || !Pose.bClosedLatchedReconstructionValidated
        || !Pose.bOccupancyValidated || !Pawn->ValidateRecoveryPose(FTransform(Pose.Rotation, Pose.LocationCm))) return false;
    FDDSRobotSnapshot Current;
    if (!Pawn->GetFreshPhysicalServiceEvidence(Current)
        || Pose.LocationCm != Current.Bodies[0].WorldCm.GetLocation()
        || Pose.Rotation != Current.Bodies[0].WorldCm.Rotator()
        || !SaveAdapter->SetPoseContext(Pose)) return false;
    AcceptedLatchWitness = Witness;
    const bool Accepted = Authority->OnVerifiedLatch(Evidence);
    if (Accepted) LastAudioIntent = AudioRouter.OnNativeN12LatchObserved(Evidence, true);
    RefreshWorldProjection();
    return Accepted;
}

bool UDDSN08Coordinator::OnNativeCancelledClosure(const FDDSN08Token& Token,
    const FDDSN08PhysicalSafetyWitness& Witness)
{
    check(IsInGameThread());
    FAuthorityCallScope Scope(*this);
    if (!Authority || !HasNativeMechanics()
        || !MatchesCurrentPhysicalWitness(Witness, Token, *Pawn)) return false;
    const bool Accepted = Authority->OnCancelledClosureVerified(Token, true);
    RefreshWorldProjection();
    return Accepted;
}

void UDDSN08Coordinator::OnNativeMechanicalProgress(const FDDSN08Token& Token, EDDSN08MechanicStep Step)
{
    check(IsInGameThread());
    if (!Authority || !HasNativeMechanics() || !Authority->HasActiveOperation()
        || !Authority->GetActiveToken().Equals(Token)) return;
    // Native N12 must call only after observing the actual step. This method
    // cannot commit ownership or release drive; accepted tagged latch still gates.
    Authority->OnMechanicalProgress(Token, Step);
    LastAudioIntent = AudioRouter.OnNativeN12StepObserved(Token, Step, true, *Authority);
}

void UDDSN08Coordinator::OnNativeMechanicalFailure(const FDDSN08Token& Token)
{
    check(IsInGameThread());
    FAuthorityCallScope Scope(*this);
    if (Authority && HasNativeMechanics()) Authority->OnMechanicalFailure(Token);
}

bool UDDSN08Coordinator::OnStorageAck(const FDDSN08SaveAck& Ack)
{
    FAuthorityCallScope Scope(*this);
    if (bEndingPlay || !Authority || !Authority->HasPinnedCandidate()) return false;
    // Copy BEFORE synchronous OnSaveAck can clear candidate/lifetime state.
    const FDDSN08SaveCandidate Candidate = Authority->GetPinnedCandidate();
    if (Ack.bDurablyValidated)
    {
        FDDSRobotSnapshot Physical;
        FDDSN15ProjectionInputs Doors;
        const FDDSN08Observation Observation = Observe(&Doors);
        // Actual disk readback is necessary but cannot substitute current
        // real mechanics. A withdrawn/stale closure retains the drive lock
        // and the adapter's exact pinned bytes for a later explicit retry.
        if (!HasNativeMechanics() || !Pawn->GetFreshPhysicalServiceEvidence(Physical)
            || !AcceptedLatchWitness.Operation.Equals(Candidate.Correlation)
            || Physical.Generation != AcceptedLatchWitness.PhysicsGeneration
            || Physical.CollisionWorldGeneration != AcceptedLatchWitness.CollisionWorldGeneration
            || Physical.ReadinessRevision != AcceptedLatchWitness.ReadinessRevision
            || !Physical.bClosedLidBaseline || !Observation.bClosedOperableDoorObserved
            || !Observation.bClosedOperableDoor || !Doors.bNativeDoorObservationKnown
            || !Doors.bLeftLidAtClosedPose || !Doors.bRightLidAtClosedPose
            || !Doors.bLeftLatchVerified || !Doors.bRightLatchVerified) return false;
    }
    const FDDSN08Presentation Before = Authority->GetPresentation();
    const bool Accepted = Authority->OnSaveAck(Ack);
    LastDurableAudioIntent = AudioRouter.OnAuthoritySaveAck(Accepted, Candidate, Ack, Before,
        Authority->GetStableSnapshot(), Authority->GetPresentation());
    RefreshWorldProjection();
    return Accepted;
}

void UDDSN08Coordinator::SetDriveLocked(bool bLocked)
{
    if (Pawn.IsValid()) Pawn->SetMovementLocked(bLocked || bEndingPlay);
}
void UDDSN08Coordinator::BeginPhysicalCycle(const FDDSN08Token& Token)
{
    if (HasNativeMechanics()) NativeMechanics->BeginPhysicalCycle(Token);
    // Missing provider cannot produce mechanical progress, closure or ACK.
}
void UDDSN08Coordinator::CancelAndClose(const FDDSN08Token& Token)
{
    if (HasNativeMechanics()) NativeMechanics->CancelAndClose(Token);
}
FDDSN08SaveCandidate UDDSN08Coordinator::Prepare(const FDDSN08StableSnapshot& Snapshot,
    const FDDSN08Token& Correlation)
{
    return SaveAdapter ? SaveAdapter->Prepare(Snapshot, Correlation) : FDDSN08SaveCandidate();
}
bool UDDSN08Coordinator::Submit(const FDDSN08SaveCandidate& Candidate)
{
    if (bEndingPlay || !Authority || !SaveAdapter) return false;
    const bool AttemptAccepted = SaveAdapter->Submit(Candidate);
    // N11 is synchronous: true means an attempt, even if our ACK sink rejected
    // stale physical closure. Pending after return means no accepted outcome.
    // Report that to unchanged N08 so it enters FailedRetryable with both exact
    // candidates pinned. Do not forge a failed/durable ACK or change disk facts.
    return AttemptAccepted && !bEndingPlay && Authority
        && Authority->GetSaveGate() != EDDSN08Gate::Pending;
}
FDDSN08RestoreBarrier UDDSN08Coordinator::EstablishRestoreBarrier(
    const FDDSN08StableSnapshot& Checkpoint, const FDDSN08SaveCandidate* InFlight)
{
    // Consume this explicit load's admission context before I/O. The adapter
    // retains every exact pin unless post-I/O physical admission also succeeds.
    TFunction<bool()> Admission = MoveTemp(ReadmitRestoreAfterDurableIO);
    ReadmitRestoreAfterDurableIO = TFunction<bool()>();
    if (!SaveAdapter || !Admission || bEndingPlay || !bMissionEntryBusy)
    {
        if (SaveAdapter) SaveAdapter->SetRestorePoseContext(FDDSN11Pose());
        return FDDSN08RestoreBarrier();
    }
    return SaveAdapter->EstablishRestoreBarrierWithCurrentAdmission(Checkpoint, InFlight,
        [&Admission]() { return Admission(); });
}
bool UDDSN08Coordinator::RetrySave()
{
    check(IsInGameThread());
    FAuthorityCallScope Scope(*this);
    if (bEndingPlay || !Authority) return false;
    return Authority->GetSaveGate() == EDDSN08Gate::ProviderFault
        ? Authority->RetryProviderPreparation() : Authority->RetryPinnedSave();
}
void UDDSN08Coordinator::StopAudioAtPauseOrRecovery()
{
    LastAudioIntent = AudioRouter.StopForPauseOrRecovery();
    // No shipped audio playback is implemented; future component must apply
    // this stop in this same event before tick/pause/recovery can suspend it.
}

FDDSN08RecoveryOutcome UDDSN08Coordinator::TryRecovery(bool bFromPausedMenu)
{
    FAuthorityCallScope Scope(*this);
    StopAudioAtPauseOrRecovery();
    FDDSN08RecoveryOutcome Out;
    if (!Authority || !Pawn.IsValid()) { Out.Status = TEXT("Recovery unavailable"); return Out; }
    const auto Observation = Observe();
    Out.Policy = Authority->RequestRecoveryPolicy(Observation.bKnown
        && Observation.bClosedOperableDoorObserved && Observation.bClosedOperableDoor);
    // A truthful approved reset needs actual closed lids/parcel ownership,
    // N11 fence and original-sidewalk full-assembly permission. Never mint it
    // from these UI observations or from the historical ledger alone.
    Out.Status = Out.Policy == EDDSN08Recovery::PersistenceLocked
        ? TEXT("Recovery waiting for save") : TEXT("Recovery unavailable");
    return Out;
}
FString UDDSN08Coordinator::DescribeRecoveryReady() const
{
    return Authority && Authority->GetSaveGate() != EDDSN08Gate::None
        ? TEXT("Recovery waiting for save") : TEXT("Recovery unavailable");
}
FDDSRecoveryLedgerView UDDSN08Coordinator::BuildRecoveryLedger() const
{
    FDDSRecoveryLedgerView Ledger;
    if (!Authority) return Ledger;
    const auto State = Authority->GetStableSnapshot();
    Ledger.MissionInstanceId = State.MissionInstanceId;
    Ledger.CargoOwner = State.Owner == EDDSN08Owner::Robot ? TEXT("Robot")
        : State.Owner == EDDSN08Owner::Recipient ? TEXT("Recipient") : TEXT("Station");
    Ledger.StarIds = State.CollectedStarIds; Ledger.ElapsedSeconds = State.ElapsedGameplaySeconds;
    Ledger.Result = State.bHasFrozenResult ? TEXT("Delivered") : TEXT("Pending");
    return Ledger;
}


bool UDDSN08Coordinator::BindNativeMissionReader(UActorComponent& LifetimeOwner, FNativeMissionReader Reader)
{
    if (!IsInGameThread() || bMissionEntryBusy || !HasNativeMechanics() || !Authority
        || NativeMechanicsLifetime.Get() != &LifetimeOwner || !Reader
        || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids()
        || Authority->GetSaveGate() != EDDSN08Gate::None) return false;
    ReadNativeMission = MoveTemp(Reader); ++NativeBindingRevision;
    return true;
}

bool UDDSN08Coordinator::BuildMissionIdentity(const FString& InstanceId, const FString& CargoId,
    FDDSN08StableSnapshot& Out, FString& Error) const
{
    Out = FDDSN08StableSnapshot();
    if (InstanceId.TrimStartAndEnd().IsEmpty() || CargoId.TrimStartAndEnd().IsEmpty()
        || ServiceBays.Num() != 2 || RouteStars.Num() != 3 || !GetWorld())
    { Error = TEXT("Explicit identity and two real bays/three real stars required"); return false; }
    Out.MissionInstanceId = InstanceId; Out.CargoId = CargoId;
    TArray<FString> Slots;
    for (const auto& WeakBay : ServiceBays)
    {
        const auto* Bay = WeakBay.Get();
        if (!IsValid(Bay) || Bay->GetWorld() != GetWorld() || !Bay->GetRootComponent()
            || !Bay->GetRootComponent()->IsRegistered() || Bay->GetStationId().TrimStartAndEnd().IsEmpty())
        { Error = TEXT("Mission bay is not live and registered"); return false; }
        FString& Id = Bay->GetLeg() == EDDSN08Leg::Pickup ? Out.PickupStationId : Out.DeliveryStationId;
        if (!Id.IsEmpty()) { Error = TEXT("Duplicate mission bay leg"); return false; }
        Id = Bay->GetStationId();
    }
    for (const auto& WeakStar : RouteStars)
    {
        const auto* Star = WeakStar.Get();
        if (!IsValid(Star) || Star->GetWorld() != GetWorld() || !Star->GetRootComponent()
            || !Star->GetRootComponent()->IsRegistered())
        { Error = TEXT("Mission star is not live and registered"); return false; }
        Slots.Add(Star->GetStableSlotId());
    }
    if (Out.PickupStationId.IsEmpty() || Out.DeliveryStationId.IsEmpty()
        || Out.PickupStationId.Equals(Out.DeliveryStationId, ESearchCase::IgnoreCase)
        || !DDSN10StarBridge::MakeThreeScopedStarIds(InstanceId, Slots, Out.ScopedStarIds))
    { Error = TEXT("Mission station or scoped-star identity invalid"); return false; }
    return true;
}

bool UDDSN08Coordinator::ReadCurrentMissionReconstruction(const FDDSN08StableSnapshot& RequestedInput,
    FDDSN08NativeMissionReconstruction& Out, FString& Error)
{
    const FDDSN08StableSnapshot Requested = RequestedInput;
    if (!Authority || !SaveAdapter || !Store || !HasNativeMechanics() || !ReadNativeMission
        || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids())
    { Error = TEXT("Current native mechanics/reconstruction unavailable or mechanically busy"); return false; }
    FDDSN08StableSnapshot Identity;
    if (!BuildMissionIdentity(Requested.MissionInstanceId, Requested.CargoId, Identity, Error)
        || Identity.PickupStationId != Requested.PickupStationId
        || Identity.DeliveryStationId != Requested.DeliveryStationId
        || Identity.ScopedStarIds != Requested.ScopedStarIds)
    { if (Error.IsEmpty()) Error = TEXT("Checkpoint identity does not match current registered world actors"); return false; }
    const FDDSN08StableSnapshot Before = Authority->GetStableSnapshot();
    const EDDSN08Gate GateBefore = Authority->GetSaveGate();
    const uint64 Revision = NativeBindingRevision;
    const FNativeMissionReader Reader = ReadNativeMission;
    FDDSRobotSnapshot BeforePhysical;
    if (!Pawn->GetFreshPhysicalServiceEvidence(BeforePhysical))
    { Error = TEXT("Actual N06 service readiness is unavailable"); return false; }
    FDDSN08NativeMissionReconstruction Read;
    if (!Reader(Requested, Read) || bEndingPlay || !HasNativeMechanics()
        || Revision != NativeBindingRevision || !Authority
        || !FDDSN11Codec::SameLogicalSnapshot(Before, Authority->GetStableSnapshot())
        || Authority->GetSaveGate() != GateBefore || Authority->HasActiveOperation()
        || Authority->IsWaitingForClosedLids())
    { Error = TEXT("Native reconstruction refused or changed live mission/provider context"); return false; }
    TStrongObjectPtr<UPrimitiveComponent> ParcelPin(Read.ActualParcel);
    FDDSRobotSnapshot Current;
    const auto& W = Read.PhysicalWitness;
    const auto& Pose = Read.ClosedPose;
    AActor* ExpectedParcelActor = Pawn.Get();
    if (Requested.Owner != EDDSN08Owner::Robot)
    {
        ExpectedParcelActor = nullptr;
        const FString Station = Requested.Owner == EDDSN08Owner::Station
            ? Requested.PickupStationId : Requested.DeliveryStationId;
        for (const auto& Bay : ServiceBays)
            if (Bay.IsValid() && Bay->GetStationId() == Station) ExpectedParcelActor = Bay.Get();
    }
    UPrimitiveComponent* Parcel = ParcelPin.Get();
    if (!Pawn->GetFreshPhysicalServiceEvidence(Current)
        || Current.Generation != BeforePhysical.Generation
        || Current.CollisionWorldGeneration != BeforePhysical.CollisionWorldGeneration
        || Current.ReadinessRevision != BeforePhysical.ReadinessRevision
        || Current.StepSequence != BeforePhysical.StepSequence || Current.CapturedRealS != BeforePhysical.CapturedRealS
        || !Current.bCompleteState || !Current.bServiceStable || !Current.bWorldReady || !Current.bClosedLidBaseline
        || W.Operation.MissionInstanceId != Requested.MissionInstanceId
        || W.PhysicsGeneration != Current.Generation || W.CollisionWorldGeneration != Current.CollisionWorldGeneration
        || W.ReadinessRevision != Current.ReadinessRevision || W.CompletedStepSequence != Current.StepSequence
        || W.CompletedStateTimeS != Current.CompletedStateTimeS || W.CapturedRealS != Current.CapturedRealS
        || !W.bFreshPhysicalStableSupport || !W.bCurrentLidSweepAndServicePathClear
        || !W.bBothActualLidsClosedLatched || !W.bExpectedParcelPhysicalOwnerAndMass
        || !FDDSN11Codec::SameLogicalSnapshot(Requested, Read.ObservedSnapshot)
        || Read.ObservedParcelId != Requested.CargoId || Read.ObservedParcelOwner != Requested.Owner
        || !IsValid(Parcel) || !IsValid(ExpectedParcelActor) || Parcel->GetOwner() != ExpectedParcelActor
        || !Parcel->IsRegistered() || Parcel->GetWorld() != GetWorld()
        || !Parcel->GetBodyInstance() || !Parcel->GetBodyInstance()->IsValidBodyInstance()
        || !FMath::IsFinite(Read.ObservedParcelMassKg) || Read.ObservedParcelMassKg <= 0
        || !FMath::IsNearlyEqual(double(Parcel->GetMass()), Read.ObservedParcelMassKg, 1.e-6)
        || !Pose.bGroundedSidewalkValidated || !Pose.bClosedLatchedReconstructionValidated || !Pose.bOccupancyValidated
        || Pose.LocationCm != Current.Bodies[0].WorldCm.GetLocation()
        || Pose.Rotation != Current.Bodies[0].WorldCm.Rotator()
        || !Pawn->ValidateRecoveryPose(FTransform(Pose.Rotation, Pose.LocationCm)))
    { Error = TEXT("Actual closed pose, parcel ownership/mass or current physical identity refused"); return false; }
    FDDSN11Record Check; Check.Revision = 1; Check.Snapshot = Requested; Check.Pose = Pose;
    TArray<uint8> Encoded;
    FDDSRobotSnapshot AtUse;
    FDDSN08StableSnapshot LiveIdentity;
    if (!FDDSN11Codec::Encode(Check, Encoded) || bEndingPlay || !HasNativeMechanics()
        || Revision != NativeBindingRevision || !Authority
        || !FDDSN11Codec::SameLogicalSnapshot(Before, Authority->GetStableSnapshot())
        || Authority->GetSaveGate() != GateBefore || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids()
        || !BuildMissionIdentity(Requested.MissionInstanceId, Requested.CargoId, LiveIdentity, Error)
        || LiveIdentity.PickupStationId != Requested.PickupStationId
        || LiveIdentity.DeliveryStationId != Requested.DeliveryStationId || LiveIdentity.ScopedStarIds != Requested.ScopedStarIds
        || !Pawn->GetFreshPhysicalServiceEvidence(AtUse)
        || AtUse.Generation != Current.Generation || AtUse.CollisionWorldGeneration != Current.CollisionWorldGeneration
        || AtUse.ReadinessRevision != Current.ReadinessRevision || AtUse.StepSequence != Current.StepSequence
        || AtUse.CapturedRealS != Current.CapturedRealS || !IsValid(Parcel)
        || Parcel->GetOwner() != ExpectedParcelActor || !Parcel->IsRegistered() || Parcel->GetWorld() != GetWorld())
    { if (Error.IsEmpty()) Error = TEXT("Native reconstruction changed before authority use"); return false; }
    Out = MoveTemp(Read); return true;
}

EDDSN08MissionEntryResult UDDSN08Coordinator::InitializeMissionExplicit(const FString& InstanceId,
    const FString& CargoId, bool bReplay, FString& Error)
{
    Error.Reset();
    if (!IsInGameThread() || bMissionEntryBusy || bEndingPlay || !Authority || !Store || !SaveAdapter
        || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids()
        || Authority->GetSaveGate() != EDDSN08Gate::None || Authority->HasPinnedCandidate())
    { Error = TEXT("Explicit mission entry unavailable or persistence/mechanics busy"); return EDDSN08MissionEntryResult::Refused; }
    FAuthorityCallScope Scope(*this); TGuardValue<bool> Guard(bMissionEntryBusy, true);
    const auto Before = Authority->GetStableSnapshot();
    if ((!bReplay && !Before.MissionInstanceId.IsEmpty()) || (bReplay && Before.Phase != EDDSN08Phase::Completed))
    { Error = TEXT("Fresh requires uninitialized authority; replay requires completed mission"); return EDDSN08MissionEntryResult::Refused; }
    FDDSN08StableSnapshot Fresh;
    if (!BuildMissionIdentity(InstanceId, CargoId, Fresh, Error)) return EDDSN08MissionEntryResult::Refused;
    // ReplayExplicit retains the completed station IDs. Validate that EXACT
    // prospective identity before any native reconstruction or state/save mutation.
    if (bReplay && !ReplayStationIdentityMatches(Fresh, Before))
    { Error = TEXT("Replay current stations differ from retained completed mission"); return EDDSN08MissionEntryResult::Refused; }
    FDDSN08NativeMissionReconstruction Native;
    if (!ReadCurrentMissionReconstruction(Fresh, Native, Error)) return EDDSN08MissionEntryResult::Refused;
    const bool Changed = bReplay ? Authority->ReplayExplicit(Fresh.MissionInstanceId, Fresh.CargoId, Fresh.ScopedStarIds)
        : Authority->StartFreshMission(Fresh.MissionInstanceId, Fresh.CargoId, Fresh.PickupStationId,
            Fresh.DeliveryStationId, Fresh.ScopedStarIds);
    if (!Changed) { Error = TEXT("Existing authority refused explicit fresh/replay"); return EDDSN08MissionEntryResult::Refused; }
    bInteractDown = false; AcceptedLatchWitness = FDDSN08PhysicalSafetyWitness();
    Store->AuthorizeExplicitFreshMission(); // only following explicit accepted initialization.
    FDDSN11Record Durable;
    const bool Saved = SaveAdapter->SaveStableProgress(Authority->GetStableSnapshot(), Native.ClosedPose, Durable);
    StableProgressStatus = Saved ? TEXT("Initial progress durably readback validated") : TEXT("Mission initialized; initial progress unsaved; explicit retry required");
    if (!Saved) Error = StableProgressStatus;
    RefreshWorldProjection();
    return Saved ? EDDSN08MissionEntryResult::InitializedProgressDurable : EDDSN08MissionEntryResult::InitializedProgressUnsaved;
}
EDDSN08MissionEntryResult UDDSN08Coordinator::StartFreshMissionExplicit(const FString& Id, const FString& Cargo, FString& Error)
{ return InitializeMissionExplicit(Id, Cargo, false, Error); }
EDDSN08MissionEntryResult UDDSN08Coordinator::ReplayMissionExplicit(const FString& Id, const FString& Cargo, FString& Error)
{ return InitializeMissionExplicit(Id, Cargo, true, Error); }

EDDSN08MissionEntryResult UDDSN08Coordinator::LoadCheckpointExplicit(const TArray<uint8>& InputBytes, FString& Error)
{
    Error.Reset();
    if (!IsInGameThread() || bMissionEntryBusy || bEndingPlay || !Authority || !Store || !SaveAdapter
        || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids())
    { Error = TEXT("Explicit load unavailable or mechanics busy"); return EDDSN08MissionEntryResult::Refused; }
    FAuthorityCallScope Scope(*this); TGuardValue<bool> Guard(bMissionEntryBusy, true);
    FDDSN11Record Checkpoint;
    if (InputBytes.Num() > FDDSN11Codec::MaximumRecordBytes || !FDDSN11Codec::Decode(InputBytes, Checkpoint))
    { Error = TEXT("Explicit checkpoint schema/digest/logical state/pose invalid"); return EDDSN08MissionEntryResult::Refused; }
    FDDSN08NativeMissionReconstruction Native;
    if (!ReadCurrentMissionReconstruction(Checkpoint.Snapshot, Native, Error)
        || !SaveAdapter->SetRestorePoseContext(Native.ClosedPose)) return EDDSN08MissionEntryResult::Refused;
    const FDDSN08StableSnapshot BeforeRestore = Authority->GetStableSnapshot();
    const EDDSN08Gate BeforeGate = Authority->GetSaveGate();
    const uint64 ProviderRevision = NativeBindingRevision;
    // Value captures bind this attempt to the exact checkpoint, physical epochs,
    // durable pose and original logical/save context, without retaining caller aliases.
    ReadmitRestoreAfterDurableIO = [this, Snapshot = Checkpoint.Snapshot,
        Initial = Native, BeforeRestore, BeforeGate, ProviderRevision]()
    {
        if (bEndingPlay || !bMissionEntryBusy || !Authority || !HasNativeMechanics()
            || NativeBindingRevision != ProviderRevision || Authority->GetSaveGate() != BeforeGate
            || !FDDSN11Codec::SameLogicalSnapshot(BeforeRestore, Authority->GetStableSnapshot())) return false;
        FDDSN08NativeMissionReconstruction Current; FString AdmissionError;
        if (!ReadCurrentMissionReconstruction(Snapshot, Current, AdmissionError)) return false;
        // The current sample may renew its step timestamp; replacement provider,
        // physics/world/readiness epoch or reconstructed saved pose cannot change.
        return !bEndingPlay && Authority && HasNativeMechanics()
            && NativeBindingRevision == ProviderRevision && Authority->GetSaveGate() == BeforeGate
            && FDDSN11Codec::SameLogicalSnapshot(BeforeRestore, Authority->GetStableSnapshot())
            && Current.PhysicalWitness.PhysicsGeneration == Initial.PhysicalWitness.PhysicsGeneration
            && Current.PhysicalWitness.CollisionWorldGeneration == Initial.PhysicalWitness.CollisionWorldGeneration
            && Current.PhysicalWitness.ReadinessRevision == Initial.PhysicalWitness.ReadinessRevision
            && Current.ClosedPose.LocationCm == Initial.ClosedPose.LocationCm
            && Current.ClosedPose.Rotation == Initial.ClosedPose.Rotation
            && Current.ActualParcel == Initial.ActualParcel;
    };
    const bool N11Validated = FDDSN11Codec::Validate(Checkpoint);
    const bool ActualLidsClosed = Native.PhysicalWitness.bBothActualLidsClosedLatched;
    const bool ActualFinitePose = Native.ClosedPose.bGroundedSidewalkValidated
        && Native.ClosedPose.bClosedLatchedReconstructionValidated && Native.ClosedPose.bOccupancyValidated;
    if (!Authority->RestoreValidatedCheckpoint(Checkpoint.Snapshot, N11Validated, ActualLidsClosed, ActualFinitePose))
    { ReadmitRestoreAfterDurableIO = TFunction<bool()>();
        Error = TEXT("Existing checkpoint durable barrier or current post-I/O physical admission refused; pins retained");
        return EDDSN08MissionEntryResult::Refused; }
    bInteractDown = false; AcceptedLatchWitness = FDDSN08PhysicalSafetyWitness();
    StableProgressStatus = TEXT("Explicit checkpoint restored through validated N11 barrier");
    RefreshWorldProjection(); return EDDSN08MissionEntryResult::CheckpointRestored;
}

bool UDDSN08Coordinator::SaveCurrentStableProgress(FDDSN11Record& OutRecord, FString& Error)
{
    Error.Reset(); OutRecord = FDDSN11Record();
    if (!IsInGameThread() || bMissionEntryBusy || bEndingPlay || !Authority || !SaveAdapter
        || Authority->HasActiveOperation() || Authority->IsWaitingForClosedLids()
        || Authority->GetSaveGate() != EDDSN08Gate::None || Authority->HasPinnedCandidate())
    { Error = TEXT("Stable progress blocked by mechanics/pinned transfer save"); StableProgressStatus = Error; return false; }
    FAuthorityCallScope Scope(*this); TGuardValue<bool> Guard(bMissionEntryBusy, true);
    FDDSN08NativeMissionReconstruction Native;
    const auto Snapshot = Authority->GetStableSnapshot();
    if (!ReadCurrentMissionReconstruction(Snapshot, Native, Error)
        || !SaveAdapter->SaveStableProgress(Snapshot, Native.ClosedPose, OutRecord))
    { if (Error.IsEmpty()) Error = TEXT("Stable progress storage readback failed; explicit retry required"); StableProgressStatus = Error; return false; }
    StableProgressStatus = TEXT("Stable initial/star progress durably readback validated"); return true;
}


bool UDDSN08Coordinator::ReplayStationIdentityMatches(const FDDSN08StableSnapshot& Prospective,
    const FDDSN08StableSnapshot& Retained)
{
    return Prospective.PickupStationId == Retained.PickupStationId
        && Prospective.DeliveryStationId == Retained.DeliveryStationId;
}
