#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Util/ICitySampleInputInterface.h"
#include "MAYADeliveryTypes.h"
#include "MAYAWheelFeedbackCallback.h"
#include "MAYANativeDemoPawn.generated.h"

class UStaticMesh;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UPhysicsConstraintComponent;
class UPhysicalMaterial;
class UInputMappingContext;
class UInputAction;
class UMAYAStableCameraArm;
class UCameraComponent;
class UBoxComponent;
class FJsonValue;
struct FInputActionValue;
namespace Chaos { class FPBDRigidsSolver; }

enum class EMAYADriveCommandOrigin : uint8 { None, LocalPlayer, NativeAutomation };

// GAME physics profile. Publisher assets remain read-only; delivery mode uses
// separately reviewed owned bay/seam colliders. Historical DDS certificates remain separate.
UCLASS()
class DDSCITY_API AMAYANativeDemoPawn : public APawn, public ICitySampleInputInterface
{
    GENERATED_BODY()
public:
    AMAYANativeDemoPawn();
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void UnPossessed() override;
    virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void AddInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& Subsystem) override;
    virtual void RemoveInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& Subsystem) override;

    // Default OFF. A source-reviewed, explicitly named GAME profile must be selected.
    UPROPERTY(EditAnywhere, Category="MAYA Demo") bool bReviewedCandidateProfile = false;
    UPROPERTY(EditAnywhere, Category="MAYA Delivery") bool bDeliveryOwnsInteraction = false;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") TMap<FName, UStaticMesh*> CosmeticMeshes;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") float Door1OpenDegrees = -78.f;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") float Door2OpenDegrees = 78.f;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") float LidKpNmPerRad = 2.f;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") float LidKdNmPerRadps = 1.f;
    UPROPERTY(EditAnywhere, Category="MAYA Demo") float LidPeakNm = 12.f;
    UPROPERTY(BlueprintReadOnly, Category="MAYA Demo") FString Fault;
    UFUNCTION(BlueprintCallable, Category="MAYA Demo") bool ResetDemo();
    UFUNCTION(BlueprintPure, Category="MAYA Demo") FString ReadTelemetryJson() const;
    UFUNCTION(BlueprintCallable, Category="MAYA Demo") FString CaptureCargoClearanceGeometryJson(FVector EnvelopeMinWorldCm, FVector EnvelopeMaxWorldCm, FString ReviewedSpecSha256);
    bool ReadDeliveryState(FMAYADeliveryRobotObservation& Out) const;
    bool HasFreshMovementIntent() const; // observation only; never bypasses native motor locks
    // Moving containment: measured closed pose and actual native locks. Service
    // uses ReadDeliveryState's additional stationary angular-rate settling guard.
    bool AreDeliveryDoorsClosedAndPhysicallyLocked() const;
    // Immutable cached ObserveDoors/ApplyPhysicalTorques decisions, before a later command reset.
    FString ReadDriveAdmissionEvidenceJson(bool bIncludeOwnershipHistory = true) const;
    void SetDeliveryDriveLocked(bool bLocked);
    bool IsDeliveryDriveLocked() const { return bDeliveryDriveLocked; }
    bool CanRequestDeliveryDoorState(bool bOpen) const;
    bool RequestDeliveryDoorState(bool bOpen);
    UPrimitiveComponent* GetDeliveryCargoMount(int32 Slot = 0) const;
    bool GetDeliveryCargoPlacementTarget(UPrimitiveComponent* Parcel, int32 Slot, FTransform& Out) const;
    bool AdmitDeliveryCargoPlacement(UPrimitiveComponent* Parcel, int32 Slot) const;
    bool AdmitDeliveryCargoTransfer(UPrimitiveComponent* Parcel, const FTransform& Destination) const;
    bool GetDeliveryCargoAirRemovalPoses(UPrimitiveComponent* Parcel, int32 Slot,
        FTransform& OutStart, FTransform& OutUpper, double& OutOpenAssemblyTopCm) const;
    bool AdmitDeliveryCargoAirRaising(UPrimitiveComponent* Parcel, int32 Slot,
        const FTransform& Lower, const FTransform& Upper) const;
    bool ReadDeliveryCargoAirRaisingFrame(UPrimitiveComponent* Parcel, int32 Slot,
        FVector& OutNativeBayCm, double& OutOpenAssemblyTopCm) const;
    bool AdmitDeliveryCargoAirRaisingRemaining(UPrimitiveComponent* Parcel, int32 Slot,
        const FTransform& Upper) const;
    bool GetDeliveryCargoAirObservationBounds(FBox& OutBounds) const;
    bool GetDeliveryCargoAirPlacementPoses(UPrimitiveComponent* Parcel, int32 Slot,
        FTransform& OutStart, FTransform& OutTarget, double& OutOpenAssemblyTopCm) const;
    bool AdmitDeliveryCargoAirLowering(UPrimitiveComponent* Parcel, int32 Slot,
        const FTransform& Start, const FTransform& Destination) const;
    bool GetDeliveryTransferWaypoints(UPrimitiveComponent* Parcel, const FTransform& Destination, TArray<FVector>& OutPoints) const;
    FString ReadDeliveryCargoTransferStatus() const { return CargoTransferStatus; }
    bool CanResetDeliveryRobot() const;
    const FString& ReadDeliveryResetClearanceQueryJson() const { return LastResetClearanceQueryJson; }
    bool ResetDeliveryRobot();
    bool SubmitDriveCommand(float InThrottle, float InSteer, bool bInBrake,
        EMAYADriveCommandOrigin Origin, float LeaseSeconds = 0.25f, bool bInBoost = false);
    void ReleaseDriveCommand();
    // Development-only matched native recreation of the prior boost envelope;
    // explicit AutoTest+BoostPowerTest and actual braked full support required.
    bool SelectNativeBoostComparisonProfile(bool bPreviousProfile);
    bool IsNativeBoostComparisonProfileReady(bool bPreviousProfile) const;

    // Presentation only. Chase continues ticking while POV is selected; mode
    // changes never reset/move the robot or alter a drive/service command.
    UFUNCTION(BlueprintCallable, Category="MAYA Camera") bool TogglePlayerCameraMode();
    UFUNCTION(BlueprintCallable, Category="MAYA Camera") bool SetPlayerCameraPov(bool bEnabled);
    UFUNCTION(BlueprintPure, Category="MAYA Camera") bool IsPlayerCameraPov() const { return bPlayerCameraPov; }
    UFUNCTION(BlueprintPure, Category="MAYA Camera") FString ReadPlayerCameraModeJson() const;

private:
    enum class EDoorState : uint8 { ClosedLatched, Opening, Open, Closing, Fault };
    struct FContactReceipt
    {
        TWeakObjectPtr<UPrimitiveComponent> Support;
        FTransform SupportPose;
        FTransform WheelPose;
        FVector Point = FVector::ZeroVector;
        FVector Normal = FVector::ZeroVector;
        double IssuedRealS = -1;
        // Actual loaded-contact age excludes world-paused intervals. Wall stamp remains evidence.
        double IssuedActiveRealS = -1;
        double ImpulseKgCmps = 0;
    };

    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Cosmetics;
    UPROPERTY() TArray<TObjectPtr<UPhysicsConstraintComponent>> Joints;
    UPROPERTY() TObjectPtr<UPhysicalMaterial> ContactMaterial;
    UPROPERTY() TObjectPtr<UMAYAStableCameraArm> CameraArm;
    UPROPERTY() TObjectPtr<UCameraComponent> Camera;
    UPROPERTY() TObjectPtr<UBoxComponent> CargoMount;
    UPROPERTY() TObjectPtr<UBoxComponent> RearCargoMount;
    UPROPERTY() TObjectPtr<UInputMappingContext> InputContext;
    UPROPERTY() TArray<TObjectPtr<UInputAction>> InputActions;
    TWeakObjectPtr<UObject> InputSubsystem;

    FTransform StartChassisPose;
    FTransform RestInChassis[15];
    FTransform JointInChassis[8];
    FTransform JointInPart[8];
    FContactReceipt Contacts[6];
    double MotorNm[6] = {}, BrakeNm[6] = {}, DoorAngleRad[2] = {}, DoorOmegaRadps[2] = {};
    double DoorGravityNm[2] = {}, DoorMotorNm[2] = {};
    double DoorPhysicalSampleWorldS = -1, DoorPhysicalSampleRealS = -1;
    uint64 DoorPhysicalSampleFrame = 0;
    struct FDriveAdmissionEvidence
    {
        bool bDoorDecisionKnown = false, bMotorDecisionKnown = false;
        uint64 DoorSampleFrame = 0, DoorDecisionFrame = 0, MotorFrame = 0, CommandSequence = 0;
        double DoorSampleWorldS = -1, DoorSampleRealS = -1, DoorDecisionWorldS = -1;
        double DoorEvaluationStartedRealS = -1, DoorDecisionRealS = -1, MotorWorldS = -1, MotorRealS = -1;
        double FullDoorAngleRad[2] = {-1, -1}, CachedDoorRateRadps[2] = {};
        bool DoorKnown[2] = {}, NativeLockReadKnown[2] = {}, ActualNativeClosedLock[2] = {};
        bool bClosedPose = false, bClosedSettled = false, bClosedLatched = false;
        bool bResetStabilizing = true, bDoorDriveInhibited = true;
        bool bFresh = false, bBrakeRequested = true, bDoorInhibitedAtMotorDecision = true, bDeliveryLocked = false, bInputsNeedRelease = true;
        bool bBoostRequested = false, bPreviousBoostProfile = false, bDriveAdmitted = false, bBoostMotorApplied = false;
        double CommandIssuedRealS = -1, RequestedThrottle = 0, RequestedSteer = 0;
        double MotorPeakNm = 0, MotorNoLoadRadps = 0, AppliedMotorNm[6] = {}, PassiveBearingNm[6] = {};
        bool VerifiedNativeBrakeEnabled[6] = {};
        double MotorErrorNmPerRadps = 0;
        double RelativeWheelOmegaRadps[6] = {}, UnclampedBankRequest[6] = {}, ClampedBankRequest[6] = {}, WheelInputSampleRealS[6] = {};
        bool WheelInputSampled[6] = {};
        bool bCurrentBoostPolicyEligible = false, bBoostDriveActiveAtMotorDecision = false;
        bool WheelDriveAdmitted[6] = {}, CounterTorqueSuppressed[6] = {};
        double OriginalLawMotorNm[6] = {}, MotorNmAfterPolicy[6] = {}, ExpectedAppliedMotorNm[6] = {};
        double ActualBoostEffortFractionAtMotorDecision = 0;
        bool bReserveSpeedSampleAttempted = false, bReserveNativeBodyAvailable = false, bReserveSpeedKnown = false;
        FVector ReserveChassisVelocityCmps = FVector::ZeroVector;
        double ReserveChassisSpeedCmps = 0, ReserveSpeedSampleRealS = -1, LowSpeedReserveFactor = 0;
        double ConfiguredCurrentBoostRoadPeakNm = 0, ConfiguredCurrentBoostMaximumReservePeakNm = 0;
    };
    FDriveAdmissionEvidence DriveAdmissionEvidence;
    FMAYAWheelFeedbackCallback* WheelFeedbackCallback = nullptr;
    Chaos::FPBDRigidsSolver* WheelFeedbackSolver = nullptr;
    TSharedPtr<FMAYAWheelFeedbackControl, ESPMode::ThreadSafe> WheelFeedbackControl;
    TWeakObjectPtr<UWorld> WheelFeedbackWorld;
    FDelegateHandle WheelFeedbackWorldTearDownHandle;
    FMAYAWheelFeedbackCommand LastPublishedWheelCommand;
    FMAYAWheelFeedbackSnapshot ActualWheelFeedback;
    bool bActualWheelFeedbackKnown = false, bWheelFeedbackActivationPending = true;
    bool bWheelComparisonSelectionPending = false, bWheelComparisonDesiredPrevious = false;
    bool bWheelExternalWritesStopped = false, bWheelFeedbackRetirementFailed = false;
    uint64 LastWheelFeedbackStep = 0;
    uint64 WheelFeedbackOwnershipTransitionsSeen = 0;
    TArray<TSharedPtr<FJsonValue>> WheelFeedbackOwnershipTransitions;
    bool RegisterWheelFeedback();
    bool RetireWheelFeedback();
    void PublishWheelFeedbackCommand();
    void DrainWheelFeedback();
    void InvalidateWheelFeedbackEvidence();
    bool HasCurrentWheelFeedback() const;
    bool HasCompletedBlockedWheelFeedback() const;
    void ProgressWheelFeedbackSelection();
    void OnWheelFeedbackWorldTearDown(UWorld* World);
    FString ReadWheelFeedbackEvidenceJson(bool bIncludeOwnershipHistory = true) const;
    void RecordWheelFeedbackOwnership(const TCHAR* Event);
    void ApplyPhysicalDoorTorques(bool Fresh);
    bool bDoorPhysicalStateKnown[2] = {};
    double InputIssuedRealS = -1, SupportStableSinceActiveRealS = -1, DoorPhaseStartRealS = -1;
    // UE5.3 world audio clock is unpaused real time, not dilated or clamped.
    double DoorPhaseStartActiveRealS = -1;
    double InputLeaseS = 0;
    EMAYADriveCommandOrigin InputOrigin = EMAYADriveCommandOrigin::None;
    bool bDeliveryDriveLocked = false;
    bool bBoostRequested = false, bBoostMotorApplied = false;
    bool bNativePreviousBoostProfile = false; // ordinary player always uses new boost
    mutable FString CargoTransferStatus;
    mutable FString LastResetClearanceQueryJson;
    mutable FString CargoTransferPathSummary;
    float CameraYawOffset = 0, CameraPitch = -20;
    bool bPlayerCameraPov = false, bPlayerCameraPovMountKnown = false;
    bool bPlayerCameraViewKnown = false, bLastPlayerCameraViewPov = false;
    FVector PlayerCameraPovEyeChassisCm = FVector::ZeroVector;
    FVector LastPlayerCameraViewCm = FVector::ZeroVector;
    FRotator LastPlayerCameraViewDegrees = FRotator::ZeroRotator;
    double PovResidualZCm = 0, PovClearanceCorrectionCm = 0, PovFrontClearanceCm = 0;
    float PovResidualPitchDegrees = 0, PovResidualRollDegrees = 0, PovResidualYawDegrees = 0;
    float LastPlayerCameraFov = 75, LastPlayerCameraNearClipCm = -1;
    double LastPlayerCameraViewWorldS = -1;
    uint64 PlayerCameraModeSequence = 0, PlayerCameraViewSamples = 0, PovFollowResetGeneration = 0;
    float Throttle = 0, Steer = 0;
    uint64 InputSequence = 0, ResetGeneration = 1;
    uint8 SupportMask = 0;
    FMAYADeliveryImpactEvent LastNativeImpact;
    uint64 NativeImpactGeneration = 0;
    bool bLandingSupportInitialized = false, bLandingSupportLost = false, bLandingEpisodeEmitted = false;
    double LandingLossStartWorldS = -1, LandingPeakChassisZ = 0, LandingPeakDownCmps = 0;
    double LandingCurrentLossStartWorldS = -1, LandingSupportRegainedWorldS = -1, LandingObservedLossWorldS = 0;
    double LastLandingEventWorldS = -1;
    bool bBrake = true, bReady = false, bPhysicalAssembly = false, bEnding = false, bContextActive = false;
    bool bInputsNeedRelease = true, bDriveInhibited = true;
    bool bResetStabilizing = true;
    bool bDeliveryCollisionProfileLoaded = false;
    bool bCargoClearanceGeometryCaptureConsumed = false; // Failure consumes the sole observation too.
    EDoorState DoorState = EDoorState::ClosedLatched;

    void EnsureInput();
    void ClearInput();
    void RemoveInput();
    bool CanReadLocalInput() const;
    bool HasFreshDriveCommand() const;
    bool IsBoostDriveActive() const;
    double ReadBoostMotorEffortFraction() const;
    void PollInput();
    bool ConfigurePlayerCameraPovMount();
    void ConsumeAxis(const FInputActionValue& Value); // Consumes keys; current raw state is sampled in PollInput.
    void ToggleDoors();
    void OnResetKey();
    bool PreparePhysics();
    bool PrepareDeliveryCollisionProfile();
    bool CreateJoints();
    bool CurrentStartPoseClear() const;
    void DestroyJoints();
    void ConfigureDoorJoint(int32 Lid, bool bClosedLock);
    bool ConfigureWheelBrake(int32 Wheel, bool bEnabled);
    void ApplyPhysicalTorques();
    bool HasObservedWheelSupport(int32 Wheel) const;
    void ObserveSupport();
    void ObserveNativeCargoLanding();
    void SampleDoorPhysicalState(); // Also runs while faulted; never changes readiness or door phase.
    void ObserveDoors();
    void Fail(const TCHAR* Why);
    UStaticMeshComponent* Body(int32 Index) const;
    UFUNCTION() void OnWheelHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
};
