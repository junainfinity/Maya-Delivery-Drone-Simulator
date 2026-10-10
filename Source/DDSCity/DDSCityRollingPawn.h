#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Pawn.h"
#include "Util/ICitySampleInputInterface.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSCityRollingPawn.generated.h"

class UDDSRobotPhysicsRigComponent;
struct FDDSCollisionReadinessCapability;
class USceneComponent;
class UCameraComponent;
class USpringArmComponent;
class UDDSN07SafeSpringArm;
class UStaticMeshComponent;
class UTextRenderComponent;
class UDDSN06PauseMenu;
class UDDSN07TelemetryWidget;
class UDDSN08Coordinator;
class ADDSN06RuntimeProbe;
class ADDSN07RuntimeProbe;
class ACitySamplePlayerCameraManager;
class UInputMappingContext;
struct FInputActionValue;

enum class EDDSN07CameraMode : uint8
{
    Chase,
    Sensor
};

struct FDDSN07TelemetrySample
{
    double WorldSeconds = 0.0;
    float SignedSpeedMetersPerSecond = 0.0f;
    float ReferenceSpeedMetersPerSecond = 0.0f;
    bool bReferenceValid = false;
    bool bGrounded = false;
    bool bClearanceValid = false;
    bool bClearBeyondRange = false;
    float FrontClearanceMeters = 0.0f;
    FVector RayOrigins[9] = {};
    FVector RayEnds[9] = {};
    float RayDistancesMeters[9] = {};
    bool bRayHits[9] = {};
};

USTRUCT(BlueprintType)
struct FDDSMotionSample
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly)
    double WorldSeconds = 0;

    UPROPERTY(BlueprintReadOnly)
    FVector ActualVelocityCmPerSecond = FVector::ZeroVector;

    // N15 compatibility seam. The proxy travel/yaw patch cannot measure
    // articulated dynamics. These remain unavailable pending calibration.
    UPROPERTY(BlueprintReadOnly)
    float CompletedWheelSpeedCmPerSecond = 0;
    UPROPERTY(BlueprintReadOnly)
    bool bCompletedWheelMotionMeasured = false;
    UPROPERTY(BlueprintReadOnly)
    bool bBodyBlockedDuringCompletedWheelSample = false;

    UPROPERTY(BlueprintReadOnly)
    float SignedSpeedMetersPerSecond = 0;

    UPROPERTY(BlueprintReadOnly)
    float HeadingDegrees = 0;

    UPROPERTY(BlueprintReadOnly)
    float BodyPitchDegrees = 0;

    UPROPERTY(BlueprintReadOnly)
    float BodyRollDegrees = 0;

    UPROPERTY(BlueprintReadOnly)
    int32 SupportedWheels = 0;

    UPROPERTY(BlueprintReadOnly)
    bool bGrounded = false;

    UPROPERTY(BlueprintReadOnly)
    bool bBodyBlocked = false;

    UPROPERTY(BlueprintReadOnly)
    FVector ActualAngularVelocityRadPerSecond = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly)
    int64 PhysicsGeneration = 0;
    UPROPERTY(BlueprintReadOnly)
    bool bPhysicalStateKnown = false;
    UPROPERTY(BlueprintReadOnly)
    bool bServiceStable = false;
};

// Read-only handoff from the future mission owner. N06 may relocate the pawn,
// but cannot alter any authoritative mission value supplied by that owner.
USTRUCT(BlueprintType)
struct FDDSRecoveryLedgerView
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly)
    FString MissionInstanceId;

    UPROPERTY(BlueprintReadOnly)
    FString CargoOwner;

    UPROPERTY(BlueprintReadOnly)
    TArray<FString> StarIds;

    UPROPERTY(BlueprintReadOnly)
    double ElapsedSeconds = 0;

    UPROPERTY(BlueprintReadOnly)
    FString Result;

    FString Fingerprint() const
    {
        return MissionInstanceId + TEXT("|") + CargoOwner + TEXT("|") + FString::Join(StarIds, TEXT(","))
            + FString::Printf(TEXT("|%.9f|"), ElapsedSeconds) + Result;
    }
};

UCLASS()
class DDSCITY_API ADDSCityRollingPawn : public APawn, public ICitySampleInputInterface
{
    GENERATED_BODY()

public:
    ADDSCityRollingPawn();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void UnPossessed() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
    virtual void AddInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& SubsystemInterface) override;
    virtual void RemoveInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& SubsystemInterface) override;

    const FDDSMotionSample& GetMotionSample() const { return MotionSample; }
    // The original owned static-mesh chassis; no proxy collision box.
    const UStaticMeshComponent* GetN06ChassisCollision() const { return Chassis.Get(); }
    const USceneComponent* GetN08MeasuredServiceSocket() const;
    bool RequestRecoveryViaAuthority(bool bFromPausedMenu);
    bool TryGetN08ServiceSocketWorldLocation(FVector& OutLocation) const;
    void ResumeFromPauseMenu();
    void SetMovementLocked(bool bLocked);
    void SetTestControls(float InThrottle, float InSteer, bool bInBrake);
    void ClearTestControls();
    void ResetForProbe(const FVector& Location, float YawDegrees);
    void SetDiagnosticSideViewForProbe(bool bShowNearSideWheel);
    FString GetRecoveryStatus() const { return RecoveryStatus; }
    int32 GetRecentPoseCount() const { return RecentPoses.Num(); }
    const TArray<FTransform>& GetRecentPoses() const { return RecentPoses; }
    const TArray<float>& GetWheelDegrees() const { return WheelDegrees; }
    const float* GetWheelContactHeights() const { return CachedGroundHeights; }
    uint8 GetWheelSupportMask() const { return CachedSupportMask; }
    EDDSN07CameraMode GetCameraMode() const { return CameraMode; }
    int32 GetCameraSwitchCount() const { return CameraSwitchCount; }
    const FDDSN07TelemetrySample& GetTelemetrySample() const { return TelemetrySample; }
    bool IsInstantCameraComfortEnabled() const { return bInstantCameraComfort; }
    void ToggleCameraForProbe();
    void SetInstantCameraComfortForProbe(bool bEnabled) { bInstantCameraComfort = bEnabled; }
    void AdvanceControlledAdapterForProbe();
    int32 GetControlledAdapterPhase() const { return ControlledAdapterPhase; }
    void SetClearanceInvalidForProbe(bool bInvalid) { bForceClearanceInvalidForProbe = bInvalid; }

    bool BindMeasuredOriginalRobot(const FDDSRobotMeasuredBinding& B,
        const FDDSRobotPhysicsProfile& P, FString& Error);
    bool GetFreshPhysicalServiceEvidence(FDDSRobotSnapshot& Out) const;
    // Current general admission for moving contacts; no service-stop requirement.
    bool GetFreshPhysicalStateEvidence(FDDSRobotSnapshot& Out) const;
    // Explicit City bootstrap calls this only after original measured binding.
    bool ActivateOriginalCityCamera(FString& Error);
    void ReleaseOriginalCityCamera();
    bool HasOriginalCityPresentation() const;
    bool SetPhysicalCollisionReadiness(uint64 Epoch, bool bReady);
    UDDSRobotPhysicsRigComponent* GetBoundPhysicalRig() const { return PhysicsRig.Get(); }
    bool GetPreReadinessPhysicalState(FDDSRobotSnapshot& Out) const;
    bool AcquireOwnedPhysicalReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Out);
    bool CommitOwnedPhysicalReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Receipt, bool bReady);
    bool WithdrawOwnedPhysicalReadiness(UActorComponent* Owner, FDDSCollisionReadinessCapability& Receipt, bool bRelease);
    bool ApplyApprovedPhysicalRecovery(const FDDSRobotRecoveryPermit& Permit);
    bool RecordValidatedPhysicalRecoveryPose(const FTransform& Pose,
        uint64 PhysicsEpoch, uint64 CollisionEpoch, bool bOriginalSidewalkSupportValidated, bool bStartBay);

    // Historical fixture dimensions ONLY. Actual rig never consumes these.
    static constexpr float BodyLengthCm = 90.0f;
    static constexpr float BodyWidthCm = 65.0f;
    static constexpr float ProxyHeightCm = 70.0f;
    static constexpr float WheelRadiusCm = 11.2f;

private:
    // Original CitySample controller owns context activation; these are per-pawn,
    // transient input objects, with no changes to publisher input assets.
    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> RollingInputContext;
    UPROPERTY(Transient)
    TArray<TObjectPtr<UInputAction>> RollingInputActions;
    TWeakObjectPtr<UObject> RollingInputSubsystem;
    bool bRollingContextActive = false;
    bool bRollingInteractHeld = false;
    void EnsureRollingInputObjects();
    void RemoveOwnedRollingInputContext();
    void ClearRollingHeldInputs();
    void ClearReleasedRollingKeys();
    bool CanAcceptRollingInput() const;
    void OnEnhancedThrottle(const FInputActionValue& Value);
    void OnEnhancedSteer(const FInputActionValue& Value);
    void OnEnhancedLookYaw(const FInputActionValue& Value);
    void OnEnhancedLookPitch(const FInputActionValue& Value);
    void OnEnhancedBrakeStarted();
    void OnEnhancedRecoverStarted();
    void OnEnhancedInteractStarted();
    void OnEnhancedInteractFinished();
    void OnEnhancedCameraStarted();
    void OnEnhancedComfortStarted();
    bool UpdateOriginalCityPresentation();
    TWeakObjectPtr<UStaticMeshComponent> PresentationChassis;
    TWeakObjectPtr<ACitySamplePlayerCameraManager> OriginalCityCameraManager;
    FName OriginalCityStyleToken;
    FName SavedOriginalCityStyle;
    uint64 OriginalCityCameraRevision = 0;
    bool bOriginalCityEndingPlay = false;
    friend class UDDSN08Coordinator;
    friend class ADDSN06RuntimeProbe; // Existing named native fixture only.
    friend class ADDSN07RuntimeProbe; // Existing named native fixture only.
    bool RequestRecovery(bool bFromPausedMenu, const FDDSRecoveryLedgerView* SuppliedLedger = nullptr);

    UPROPERTY(VisibleAnywhere, Category = "N06|Physical")
    TObjectPtr<UDDSRobotPhysicsRigComponent> PhysicsRig;

    UPROPERTY(VisibleAnywhere, Category = "N06|Physical")
    TObjectPtr<USceneComponent> MotionRoot;

    UPROPERTY(VisibleAnywhere, Category = "N08|Service")
    TObjectPtr<USceneComponent> ServiceSocket;

    UPROPERTY(VisibleAnywhere, Category = "N08|Authority")
    TObjectPtr<UDDSN08Coordinator> MissionCoordinator;

    UPROPERTY(VisibleAnywhere, Category = "N06|Physical")
    TObjectPtr<UStaticMeshComponent> Chassis;

    UPROPERTY(VisibleAnywhere, Category = "N06|Physical")
    TObjectPtr<UStaticMeshComponent> Body;

    UPROPERTY(VisibleAnywhere, Category = "N06|Physical")
    TArray<TObjectPtr<UStaticMeshComponent>> Wheels;

    UPROPERTY(VisibleAnywhere, Category = "N06|Diagnostic")
    TObjectPtr<UDDSN07SafeSpringArm> DiagnosticArm;

    UPROPERTY(VisibleAnywhere, Category = "N06|Diagnostic")
    TObjectPtr<UCameraComponent> DiagnosticCamera;

    UPROPERTY()
    TObjectPtr<UDDSN06PauseMenu> PauseMenu;

    UPROPERTY(VisibleAnywhere, Category = "N07|Camera")
    TObjectPtr<UCameraComponent> SensorCamera;

    UPROPERTY()
    TObjectPtr<UDDSN07TelemetryWidget> TelemetryWidget;

    void SetThrottle(float Value);
    void SetSteer(float Value);
    void OnBrakePressed();
    void OnBrakeReleased();
    void OnRecoverPressed();
    void OnRecoverReleased();
    void OnInteractPressed();
    void OnInteractReleased();
    void TogglePause();
    void OnToggleCameraPressed();
    void OnToggleInstantComfortPressed();
    void OnControlledAdapterPressed();
    void OnLookYaw(float Value);
    void OnLookPitch(float Value);
    void UpdateN07CameraMounts();
    void UpdateN07Telemetry();
    void RefreshN07Hud();
    bool ValidateRecoveryPose(const FTransform& Pose) const;
    void StoreRecentPose();

    float Throttle = 0.0f;
    float Steer = 0.0f;
    float HoldStartSeconds = 0.0f;
    float LastPoseSaveSeconds = -100.0f;
    float CachedGroundHeights[6] = {};
    uint64 PhysicalCommandSequence = 0;
    uint64 PhysicalWorldEpoch = 1;
    bool bMeasuredStartBayValidated = false;
    FDDSRobotSnapshot PhysicalSnapshot;
    bool bBrakeHeld = false;
    bool bMovementLocked = false;
    bool bRecoverHeld = false;
    bool bRecoverTriggered = false;
    bool bTestControls = false;
    uint8 CachedSupportMask = 0;
    TArray<float> WheelDegrees;
    TArray<FTransform> RecentPoses;
    FTransform StartPose;
    FDDSMotionSample MotionSample;
    FString RecoveryStatus = TEXT("Ready");

    EDDSN07CameraMode CameraMode = EDDSN07CameraMode::Chase;
    int32 CameraSwitchCount = 0;
    bool bInstantCameraComfort = false;
    bool bViewBlendActive = false;
    bool bHasLastRenderedView = false;
    float ViewBlendStartRealSeconds = 0.0f;
    float LookYawDegrees = 0.0f;
    float LookPitchDegrees = 0.0f;
    FMinimalViewInfo BlendStartView;
    FMinimalViewInfo LastRenderedView;
    FDDSN07TelemetrySample TelemetrySample;
    FVector LastTelemetryReferenceLocation = FVector::ZeroVector;
    double LastTelemetryReferenceSeconds = 0.0;
    bool bTelemetryReferenceValid = false;
    bool bTelemetryReferenceResetNeeded = true;
    int32 ControlledAdapterPhase = 0;
    bool bForceClearanceInvalidForProbe = false;
    double LastTelemetryLogSeconds = -100.0;
    double LastViewEdgePlatformSeconds = 0.0;
    bool bViewFirstFrameLogged = true;
};
