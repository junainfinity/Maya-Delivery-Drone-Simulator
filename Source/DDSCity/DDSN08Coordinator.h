#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DDSN08Authority.h"
#include "DDSN09ServiceBridge.h"
#include "DDSN11Storage.h"
#include "DDSN15Presentation.h"
#include "DDSN15AudioIntents.h"
#include "DDSRobotOwnerHandoffProposal.h"
#include "DDSN08Coordinator.generated.h"

class ADDSCityRollingPawn;
class ADDSN10RouteStar;
class UPrimitiveComponent;

// Native owner reports a CURRENT reconstruction of this exact logical snapshot.
// These observations are never created or inferred by the coordinator.
struct FDDSN08NativeMissionReconstruction
{
    FDDSN08StableSnapshot ObservedSnapshot;
    FDDSN08PhysicalSafetyWitness PhysicalWitness;
    FDDSN11Pose ClosedPose;
    UPrimitiveComponent* ActualParcel = nullptr;
    FString ObservedParcelId;
    EDDSN08Owner ObservedParcelOwner = EDDSN08Owner::Station;
    double ObservedParcelMassKg = 0.0;
};

enum class EDDSN08MissionEntryResult : uint8
{
    Refused, InitializedProgressUnsaved, InitializedProgressDurable, CheckpointRestored
};
struct FDDSRecoveryLedgerView;

struct FDDSN08RecoveryOutcome
{
    EDDSN08Recovery Policy = EDDSN08Recovery::WaitForClosedLids;
    bool bRecovered = false;
    FString Status;
};

// Source-only assembly of the accepted consumers. No physical provider is
// implemented here. The original-asset N12 owner must supply and review the
// actual observation/actuation binding before any mission is initialized.
UCLASS(ClassGroup=(DDSCity))
class DDSCITY_API UDDSN08Coordinator final : public UActorComponent,
    public IDDSN08MotionPort, public IDDSN08MechanicsPort, public IDDSN08SavePort
{
    GENERATED_BODY()
public:
    UDDSN08Coordinator();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    FDDSN08Eligibility OnInteractInput(bool bPressed);
    void ReleaseInteractAtInputBoundary();
    FDDSN08RecoveryOutcome TryRecovery(bool bFromPausedMenu);
    FString BuildPrompt() const;
    FString DescribeRecoveryReady() const;
    bool IsControlledFixture() const { return false; }
    FDDSN15ViewModel BuildViewModel() const;
    void StopAudioAtPauseOrRecovery();

    // Owner-bound preparation only: no implementation or call in this bundle
    // installs this provider. Observer must sample real N12 facts NOW on the
    // game thread, at the supplied bay, and may return false for any unknown.
    using FNativeObservationReader = TFunction<bool(const ADDSN09ServiceBay&,
        FDDSN09ExternalCargoDoorObservation&, FDDSN15ProjectionInputs&)>;
    bool BindNativeMechanics(UActorComponent& LifetimeOwner,
        IDDSN08MechanicsPort& Port, FNativeObservationReader Reader);
    bool BindExistingWorldActors(const TArray<ADDSN09ServiceBay*>& Bays,
        const TArray<ADDSN10RouteStar*>& Stars);
    void UnbindNativeMechanics();

    // The future N12 owner routes its real completed observations here. Latch
    // and cancellation require the exact accepted physical witness; they
    // cannot be substituted by a timer, phase, visual alpha or boolean ACK.
    bool OnNativeVerifiedLatch(const FDDSN08LatchEvidence& Evidence,
        const FDDSN08PhysicalSafetyWitness& Witness, const FDDSN11Pose& Pose);
    bool OnNativeCancelledClosure(const FDDSN08Token& Token,
        const FDDSN08PhysicalSafetyWitness& Witness);
    void OnNativeMechanicalProgress(const FDDSN08Token& Token,
        EDDSN08MechanicStep Step);
    void OnNativeMechanicalFailure(const FDDSN08Token& Token);
    bool RetrySave();

    // Same lifetime as the bound mechanics; callback must observe/reconstruct
    // actual original parcel, closed lids, current rig and validated pose.
    using FNativeMissionReader = TFunction<bool(const FDDSN08StableSnapshot&,
        FDDSN08NativeMissionReconstruction&)>;
    bool BindNativeMissionReader(UActorComponent& LifetimeOwner, FNativeMissionReader Reader);
    static bool ReplayStationIdentityMatches(const FDDSN08StableSnapshot& Prospective,
        const FDDSN08StableSnapshot& Retained);
    EDDSN08MissionEntryResult StartFreshMissionExplicit(const FString& InstanceId,
        const FString& ActualCargoId, FString& Error);
    EDDSN08MissionEntryResult ReplayMissionExplicit(const FString& NewInstanceId,
        const FString& ActualNewCargoId, FString& Error);
    // Explicit bytes only; decode validates N11 schema/digest/pose. The existing
    // barrier still requires the logical checkpoint in durable storage.
    EDDSN08MissionEntryResult LoadCheckpointExplicit(const TArray<uint8>& Bytes, FString& Error);
    bool SaveCurrentStableProgress(FDDSN11Record& OutReadbackValidatedRecord, FString& Error);
    const FString& GetStableProgressStatus() const { return StableProgressStatus; }

    // This remains the only authority; no entry point restores disk head automatically.
    const FDDSN08Authority* GetAuthority() const { return Authority.Get(); }

private:
    // Preserve N08/N11 while a synchronous provider callback is on the stack.
    // EndPlay withdraws use immediately and releases only at outer call exit.
    struct FAuthorityCallScope
    {
        explicit FAuthorityCallScope(UDDSN08Coordinator& InOwner);
        ~FAuthorityCallScope();
        UDDSN08Coordinator& Owner;
    };
    void ReleaseOwnedState();
    bool BuildMissionIdentity(const FString& InstanceId, const FString& CargoId,
        FDDSN08StableSnapshot& Out, FString& Error) const;
    bool ReadCurrentMissionReconstruction(const FDDSN08StableSnapshot& Requested,
        FDDSN08NativeMissionReconstruction& Out, FString& Error);
    EDDSN08MissionEntryResult InitializeMissionExplicit(const FString& InstanceId,
        const FString& CargoId, bool bReplay, FString& Error);

    virtual void SetDriveLocked(bool bLocked) override;
    virtual void BeginPhysicalCycle(const FDDSN08Token& Token) override;
    virtual void CancelAndClose(const FDDSN08Token& Token) override;
    virtual FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot& Snapshot,
        const FDDSN08Token& Correlation) override;
    virtual bool Submit(const FDDSN08SaveCandidate& Candidate) override;
    virtual FDDSN08RestoreBarrier EstablishRestoreBarrier(
        const FDDSN08StableSnapshot& Checkpoint,
        const FDDSN08SaveCandidate* InFlight) override;

    bool HasNativeMechanics() const;
    ADDSN09ServiceBay* FindCurrentBay() const;
    FDDSN08Observation Observe(FDDSN15ProjectionInputs* ViewInputs = nullptr) const;
    void RefreshWorldProjection();
    void OnValidatedStar(const FString& Instance, const FString& StarId,
        ADDSCityRollingPawn* ContactPawn);
    bool OnStorageAck(const FDDSN08SaveAck& Ack);
    FDDSRecoveryLedgerView BuildRecoveryLedger() const;

    TUniquePtr<FDDSN11UnrealFileIO> FileIO;
    TUniquePtr<FDDSN11SerializedStore> Store;
    TUniquePtr<FDDSN11SaveAdapter> SaveAdapter;
    TUniquePtr<FDDSN08Authority> Authority;
    TWeakObjectPtr<ADDSCityRollingPawn> Pawn;
    TWeakObjectPtr<UActorComponent> NativeMechanicsLifetime;
    IDDSN08MechanicsPort* NativeMechanics = nullptr;
    FNativeObservationReader ReadNativeObservation;
    FNativeMissionReader ReadNativeMission;
    // Explicit load's one-attempt callback is consumed before durable barrier I/O.
    TFunction<bool()> ReadmitRestoreAfterDurableIO;
    bool bMissionEntryBusy = false;
    FString StableProgressStatus = TEXT("Stable progress not saved");
    TArray<TWeakObjectPtr<ADDSN09ServiceBay>> ServiceBays;
    TArray<TWeakObjectPtr<ADDSN10RouteStar>> RouteStars;
    FDDSN15AudioRouter AudioRouter;
    FDDSN15AudioIntent LastAudioIntent; // Intent only; no shipped playback exists.
    FDDSN15AudioIntent LastDurableAudioIntent; // Preserve synchronous ACK intent separately from latch click.
    FDDSN08PhysicalSafetyWitness AcceptedLatchWitness;
    uint64 NativeBindingRevision = 0;
    bool bInteractDown = false;
    bool bEndingPlay = false;
    int32 AuthorityCallDepth = 0;
};
