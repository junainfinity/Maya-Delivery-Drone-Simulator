#include "DDSCityRollingPawn.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "DDSN06PauseMenu.h"
#include "DDSN07TelemetryWidget.h"
#include "DDSN08Coordinator.h"
#include "Camera/CitySamplePlayerCameraManager.h"
#include "Game/CitySamplePlayerController.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "EnhancedInputComponent.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputCoreTypes.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "DDSN07SafeSpringArm.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"


ADDSCityRollingPawn::ADDSCityRollingPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    AutoPossessPlayer = EAutoReceiveInput::Player0;
    MotionRoot = CreateDefaultSubobject<USceneComponent>(TEXT("UnboundPresentationRoot"));
    RootComponent = MotionRoot;
    PhysicsRig = CreateDefaultSubobject<UDDSRobotPhysicsRigComponent>(TEXT("MeasuredPhysicalRig"));
    MissionCoordinator = CreateDefaultSubobject<UDDSN08Coordinator>(TEXT("N08MissionCoordinator"));
    ServiceSocket = CreateDefaultSubobject<USceneComponent>(TEXT("N08MeasuredServiceSocket"));
    ServiceSocket->SetupAttachment(MotionRoot);
    DiagnosticArm = CreateDefaultSubobject<UDDSN07SafeSpringArm>(TEXT("N06DiagnosticArm"));
    DiagnosticArm->SetupAttachment(MotionRoot);
    DiagnosticArm->TargetArmLength = 290.0f;
    DiagnosticArm->bDoCollisionTest = true;
    DiagnosticArm->ProbeSize = 12.0f; // Existing camera setting; requalification pending.
    DiagnosticCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("N06DiagnosticCamera"));
    DiagnosticCamera->SetupAttachment(DiagnosticArm);
    DiagnosticCamera->FieldOfView = 85.0f;
    SensorCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("N07SensorCamera"));
    SensorCamera->SetupAttachment(MotionRoot);
    SensorCamera->FieldOfView = 85.0f;
    SensorCamera->SetActive(false);
    WheelDegrees.Init(0.0f, 6);
}

void ADDSCityRollingPawn::BeginPlay()
{
    Super::BeginPlay();
    AddTickPrerequisiteComponent(PhysicsRig);
    StartPose = GetActorTransform();
    RecoveryStatus = TEXT("A02 original measured assembly binding required");
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        TelemetryWidget = CreateWidget<UDDSN07TelemetryWidget>(PC, UDDSN07TelemetryWidget::StaticClass());
        if (TelemetryWidget) TelemetryWidget->AddToViewport(10);
    }
    RefreshN07Hud();
}

void ADDSCityRollingPawn::EndPlay(const EEndPlayReason::Type Reason)
{
    bOriginalCityEndingPlay = true;
    RemoveOwnedRollingInputContext();
    ReleaseOriginalCityCamera();
    PresentationChassis.Reset(); // This follow root has no physical attachment to detach.
    if (MissionCoordinator) MissionCoordinator->StopAudioAtPauseOrRecovery();
    if (TelemetryWidget) { TelemetryWidget->RemoveFromParent(); TelemetryWidget = nullptr; }
    if (PauseMenu) { PauseMenu->RemoveFromParent(); PauseMenu = nullptr; }
    Super::EndPlay(Reason); // Accepted N06 rig still owns its teardown.
}

void ADDSCityRollingPawn::UnPossessed()
{
    RemoveOwnedRollingInputContext();
    ReleaseOriginalCityCamera();
    if (MissionCoordinator) MissionCoordinator->UnbindNativeMechanics();
    ClearTestControls();
    Super::UnPossessed();
}

void ADDSCityRollingPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateOriginalCityPresentation();
    ClearReleasedRollingKeys();
    if (bRecoverHeld && !bRecoverTriggered && GetWorld()->GetRealTimeSeconds() - HoldStartSeconds >= 1.0f)
    { bRecoverTriggered = true; RequestRecoveryViaAuthority(false); }
    FDDSRobotCommand Command;
    Command.Generation = PhysicsRig->GetGeneration();
    Command.CollisionWorldGeneration = PhysicalWorldEpoch;
    PhysicalCommandSequence = FMath::Max(PhysicalCommandSequence + 1, PhysicsRig->GetLastSubmittedSequence() + 1);
    Command.Sequence = PhysicalCommandSequence;
    Command.IssuedRealS = FPlatformTime::Seconds();
    Command.LeftRequest = FMath::Clamp(static_cast<double>(Throttle + Steer), -1.0, 1.0);
    Command.RightRequest = FMath::Clamp(static_cast<double>(Throttle - Steer), -1.0, 1.0);
    Command.bBrake = bBrakeHeld;
    Command.bDriveLocked = bMovementLocked || UGameplayStatics::IsGamePaused(this);
    PhysicsRig->SubmitCommand(Command);
    FDDSRobotSnapshot Snapshot;
    if (PhysicsRig->ConsumeSnapshot(Snapshot))
    {
        PhysicalSnapshot = Snapshot;
        const auto& Actual = Snapshot.Bodies[0];
        // World time timestamps this admitted fresh GT observation. Solver
        // time stays separate for completed-state differencing.
        MotionSample.WorldSeconds = GetWorld()->GetTimeSeconds();
        MotionSample.ActualVelocityCmPerSecond = Actual.VelocityCmps;
        MotionSample.ActualAngularVelocityRadPerSecond = Actual.AngularVelocityRadps;
        MotionSample.PhysicsGeneration = Snapshot.Generation;
        MotionSample.bPhysicalStateKnown = Snapshot.bCompleteState && Snapshot.bTimingEnvelopeValid && Snapshot.bWorldReady;
        MotionSample.bServiceStable = PhysicsRig->IsServiceEvidenceFresh(FPlatformTime::Seconds(), Snapshot);
        const FRotator R = Actual.WorldCm.Rotator();
        MotionSample.HeadingDegrees = R.Yaw; MotionSample.BodyPitchDegrees = R.Pitch; MotionSample.BodyRollDegrees = R.Roll;
        MotionSample.SignedSpeedMetersPerSecond = FVector::DotProduct(Actual.VelocityCmps,
            Actual.WorldCm.GetRotation().RotateVector(FVector::ForwardVector)) / 100.0;
        CachedSupportMask = Snapshot.SupportMask;
        MotionSample.SupportedWheels = FMath::CountBits(static_cast<uint64>(CachedSupportMask));
        MotionSample.bGrounded = MotionSample.bPhysicalStateKnown && MotionSample.SupportedWheels > 0;
        MotionSample.bBodyBlocked = false; // No fabricated obstruction diagnosis from a velocity request.
        for (int32 I = 0; I < 6; ++I)
        {
            WheelDegrees[I] = FMath::RadiansToDegrees(Snapshot.Wheels[I].AxleAngleRad);
            CachedGroundHeights[I] = Snapshot.Wheels[I].ContactPointCm.Z;
        }
    }
    else
    {
        MotionSample.bPhysicalStateKnown = MotionSample.bServiceStable = MotionSample.bGrounded = false;
        CachedSupportMask = 0; MotionSample.SupportedWheels = 0;
    }
    UpdateN07Telemetry(); RefreshN07Hud();
}

void ADDSCityRollingPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    UEnhancedInputComponent* Enhanced = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (Enhanced && Cast<ACitySamplePlayerController>(GetController()))
    {
        EnsureRollingInputObjects();
        // Match the original controller's Add/RemovePawnInputContext lifecycle.
        Enhanced->BindAction(RollingInputActions[0], ETriggerEvent::Triggered, this, &ThisClass::OnEnhancedThrottle);
        Enhanced->BindAction(RollingInputActions[0], ETriggerEvent::Completed, this, &ThisClass::OnEnhancedThrottle);
        Enhanced->BindAction(RollingInputActions[0], ETriggerEvent::Canceled, this, &ThisClass::OnEnhancedThrottle);
        Enhanced->BindAction(RollingInputActions[1], ETriggerEvent::Triggered, this, &ThisClass::OnEnhancedSteer);
        Enhanced->BindAction(RollingInputActions[1], ETriggerEvent::Completed, this, &ThisClass::OnEnhancedSteer);
        Enhanced->BindAction(RollingInputActions[1], ETriggerEvent::Canceled, this, &ThisClass::OnEnhancedSteer);
        Enhanced->BindAction(RollingInputActions[2], ETriggerEvent::Started, this, &ThisClass::OnEnhancedBrakeStarted);
        Enhanced->BindAction(RollingInputActions[2], ETriggerEvent::Completed, this, &ThisClass::OnBrakeReleased);
        Enhanced->BindAction(RollingInputActions[2], ETriggerEvent::Canceled, this, &ThisClass::OnBrakeReleased);
        Enhanced->BindAction(RollingInputActions[3], ETriggerEvent::Started, this, &ThisClass::OnEnhancedRecoverStarted);
        Enhanced->BindAction(RollingInputActions[3], ETriggerEvent::Completed, this, &ThisClass::OnRecoverReleased);
        Enhanced->BindAction(RollingInputActions[3], ETriggerEvent::Canceled, this, &ThisClass::OnRecoverReleased);
        Enhanced->BindAction(RollingInputActions[4], ETriggerEvent::Started, this, &ThisClass::OnEnhancedInteractStarted);
        Enhanced->BindAction(RollingInputActions[4], ETriggerEvent::Completed, this, &ThisClass::OnEnhancedInteractFinished);
        Enhanced->BindAction(RollingInputActions[4], ETriggerEvent::Canceled, this, &ThisClass::OnEnhancedInteractFinished);
        Enhanced->BindAction(RollingInputActions[5], ETriggerEvent::Started, this, &ThisClass::OnEnhancedCameraStarted);
        Enhanced->BindAction(RollingInputActions[6], ETriggerEvent::Started, this, &ThisClass::OnEnhancedComfortStarted);
        Enhanced->BindAction(RollingInputActions[7], ETriggerEvent::Triggered, this, &ThisClass::OnEnhancedLookYaw);
        Enhanced->BindAction(RollingInputActions[8], ETriggerEvent::Triggered, this, &ThisClass::OnEnhancedLookPitch);
        return; // Original CitySample options remain on its controller's Escape action.
    }
    // Retain the existing legacy input path for named standalone fixtures.
    PlayerInputComponent->BindAxis(TEXT("Drive"), this, &ADDSCityRollingPawn::SetThrottle);
    PlayerInputComponent->BindAxis(TEXT("Steer"), this, &ADDSCityRollingPawn::SetSteer);
    PlayerInputComponent->BindAction(TEXT("Brake"), IE_Pressed, this, &ADDSCityRollingPawn::OnBrakePressed);
    FInputActionBinding& BrakeRelease = PlayerInputComponent->BindAction(TEXT("Brake"), IE_Released, this, &ADDSCityRollingPawn::OnBrakeReleased);
    BrakeRelease.bExecuteWhenPaused = true;
    PlayerInputComponent->BindAction(TEXT("RecoverHold"), IE_Pressed, this, &ADDSCityRollingPawn::OnRecoverPressed);
    FInputActionBinding& RecoverRelease = PlayerInputComponent->BindAction(TEXT("RecoverHold"), IE_Released, this, &ADDSCityRollingPawn::OnRecoverReleased);
    RecoverRelease.bExecuteWhenPaused = true;
    PlayerInputComponent->BindAction(TEXT("Interact"), IE_Pressed, this, &ADDSCityRollingPawn::OnInteractPressed);
    FInputActionBinding& InteractRelease = PlayerInputComponent->BindAction(TEXT("Interact"), IE_Released, this, &ADDSCityRollingPawn::OnInteractReleased);
    InteractRelease.bExecuteWhenPaused = true;
    FInputActionBinding& PauseBinding = PlayerInputComponent->BindAction(TEXT("Pause"), IE_Pressed, this, &ADDSCityRollingPawn::TogglePause);
    PauseBinding.bExecuteWhenPaused = true;
    PlayerInputComponent->BindAction(TEXT("ToggleView"), IE_Pressed, this, &ADDSCityRollingPawn::OnToggleCameraPressed);
    PlayerInputComponent->BindAction(TEXT("InstantComfort"), IE_Pressed, this, &ADDSCityRollingPawn::OnToggleInstantComfortPressed);
    PlayerInputComponent->BindAxis(TEXT("LookYaw"), this, &ADDSCityRollingPawn::OnLookYaw);
    PlayerInputComponent->BindAxis(TEXT("LookPitch"), this, &ADDSCityRollingPawn::OnLookPitch);
}

void ADDSCityRollingPawn::EnsureRollingInputObjects()
{
    if (RollingInputContext) return;
    RollingInputContext = NewObject<UInputMappingContext>(this, TEXT("DDSRollingInputContext"), RF_Transient);
    const TCHAR* Names[] = { TEXT("DDSDrive"), TEXT("DDSSteer"), TEXT("DDSBrake"),
        TEXT("DDSRecover"), TEXT("DDSInteract"), TEXT("DDSView"), TEXT("DDSComfort"),
        TEXT("DDSLookYaw"), TEXT("DDSLookPitch") };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
    {
        UInputAction* Action = NewObject<UInputAction>(this, FName(Names[Index]), RF_Transient);
        Action->ValueType = (Index < 2 || Index >= 7) ? EInputActionValueType::Axis1D : EInputActionValueType::Boolean;
        Action->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
        // Let held-input releases run during pause; Started callbacks reject pause.
        Action->bTriggerWhenPaused = Index <= 4;
        RollingInputActions.Add(Action);
    }
    const auto Map = [this](int32 ActionIndex, FKey Key, bool bNegate = false)
    {
        FEnhancedActionKeyMapping& Mapping = RollingInputContext->MapKey(RollingInputActions[ActionIndex], Key);
        if (bNegate) Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(RollingInputContext, NAME_None, RF_Transient));
    };
    Map(0, EKeys::W); Map(0, EKeys::S, true);
    Map(1, EKeys::A, true); Map(1, EKeys::D);
    Map(2, EKeys::SpaceBar); Map(3, EKeys::R); Map(4, EKeys::E);
    Map(5, EKeys::C); Map(6, EKeys::V);
    Map(7, EKeys::MouseX); Map(8, EKeys::MouseY, true);
}

void ADDSCityRollingPawn::AddInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& SubsystemInterface)
{
    if (bOriginalCityEndingPlay || !SubsystemInterface.GetInterface()) return;
    EnsureRollingInputObjects();
    if (RollingInputSubsystem.Get() != SubsystemInterface.GetObject()) RemoveOwnedRollingInputContext();
    RollingInputSubsystem = SubsystemInterface.GetObject();
    bRollingContextActive = true;
    // Priority 2 consumes only DDS keys over the original controller's priority 1
    // interaction key. Its options, traversal, and debug mappings remain owned there.
    // Default options ignore already-held keys until release after context rebuild.
    SubsystemInterface->AddMappingContext(RollingInputContext, 2);
}

void ADDSCityRollingPawn::RemoveInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& SubsystemInterface)
{
    if (RollingInputSubsystem.Get() == SubsystemInterface.GetObject()) RemoveOwnedRollingInputContext();
}

void ADDSCityRollingPawn::RemoveOwnedRollingInputContext()
{
    bRollingContextActive = false;
    ClearRollingHeldInputs();
    UObject* PreviousSubsystem = RollingInputSubsystem.Get();
    RollingInputSubsystem.Reset();
    if (IEnhancedInputSubsystemInterface* Subsystem = Cast<IEnhancedInputSubsystemInterface>(PreviousSubsystem))
        if (RollingInputContext) Subsystem->RemoveMappingContext(RollingInputContext);
}

void ADDSCityRollingPawn::ClearRollingHeldInputs()
{
    ClearTestControls();
    if (bRecoverHeld) OnRecoverReleased();
    bRecoverTriggered = false;
    bRollingInteractHeld = false;
    if (MissionCoordinator) MissionCoordinator->ReleaseInteractAtInputBoundary();
}

bool ADDSCityRollingPawn::CanAcceptRollingInput() const
{
    const APlayerController* PC = Cast<APlayerController>(GetController());
    return bRollingContextActive && !bOriginalCityEndingPlay && PC && PC->IsLocalController()
        && PC->GetPawn() == this && !UGameplayStatics::IsGamePaused(this);
}

void ADDSCityRollingPawn::ClearReleasedRollingKeys()
{
    if (!bRollingContextActive) return;
    if (!CanAcceptRollingInput()) { ClearRollingHeldInputs(); return; }
    const APlayerController* PC = Cast<APlayerController>(GetController());
    // Release-only fallback: viewport focus loss flushes these raw key states.
    // This never creates drive input, physics evidence, or service permission.
    if (!bTestControls)
    {
        if (!PC->IsInputKeyDown(EKeys::W) && !PC->IsInputKeyDown(EKeys::S)) Throttle = 0.0f;
        if (!PC->IsInputKeyDown(EKeys::A) && !PC->IsInputKeyDown(EKeys::D)) Steer = 0.0f;
        if (!PC->IsInputKeyDown(EKeys::SpaceBar)) OnBrakeReleased();
    }
    if (bRecoverHeld && !PC->IsInputKeyDown(EKeys::R)) OnRecoverReleased();
    if (bRollingInteractHeld && !PC->IsInputKeyDown(EKeys::E)) OnEnhancedInteractFinished();
}

void ADDSCityRollingPawn::OnEnhancedThrottle(const FInputActionValue& Value)
{ SetThrottle(CanAcceptRollingInput() ? Value.Get<float>() : 0.0f); }
void ADDSCityRollingPawn::OnEnhancedSteer(const FInputActionValue& Value)
{ SetSteer(CanAcceptRollingInput() ? Value.Get<float>() : 0.0f); }
void ADDSCityRollingPawn::OnEnhancedLookYaw(const FInputActionValue& Value)
{ if (CanAcceptRollingInput()) OnLookYaw(Value.Get<float>()); }
void ADDSCityRollingPawn::OnEnhancedLookPitch(const FInputActionValue& Value)
{ if (CanAcceptRollingInput()) OnLookPitch(Value.Get<float>()); }
void ADDSCityRollingPawn::OnEnhancedBrakeStarted()
{ if (CanAcceptRollingInput()) OnBrakePressed(); }
void ADDSCityRollingPawn::OnEnhancedRecoverStarted()
{ if (CanAcceptRollingInput()) OnRecoverPressed(); }
void ADDSCityRollingPawn::OnEnhancedInteractStarted()
{
    if (CanAcceptRollingInput()) { bRollingInteractHeld = true; OnInteractPressed(); }
}
void ADDSCityRollingPawn::OnEnhancedInteractFinished()
{
    if (bRollingInteractHeld) { bRollingInteractHeld = false; OnInteractReleased(); }
}
void ADDSCityRollingPawn::OnEnhancedCameraStarted()
{ if (CanAcceptRollingInput()) OnToggleCameraPressed(); }
void ADDSCityRollingPawn::OnEnhancedComfortStarted()
{ if (CanAcceptRollingInput()) OnToggleInstantComfortPressed(); }

void ADDSCityRollingPawn::UpdateN07CameraMounts()
{
    if (!PhysicsRig->GetActualChassis()) return;
    const auto& B = PhysicsRig->GetMeasuredBinding();
    DiagnosticArm->SetRelativeRotation((B.ChaseMountInChassisMeters.GetRotation()
        * FRotator(LookPitchDegrees, LookYawDegrees, 0).Quaternion()).Rotator());
    SensorCamera->SetRelativeRotation((B.SensorMountInChassisMeters.GetRotation()
        * FRotator(LookPitchDegrees, LookYawDegrees, 0).Quaternion()).Rotator());
}

void ADDSCityRollingPawn::OnLookYaw(float Value)
{
    if (FMath::IsNearlyZero(Value) || UGameplayStatics::IsGamePaused(this)) { return; }
    LookYawDegrees = FMath::Clamp(LookYawDegrees + Value * 1.15f, -75.0f, 75.0f);
    UpdateN07CameraMounts();
}

void ADDSCityRollingPawn::OnLookPitch(float Value)
{
    if (FMath::IsNearlyZero(Value) || UGameplayStatics::IsGamePaused(this)) { return; }
    LookPitchDegrees = FMath::Clamp(LookPitchDegrees + Value * 1.15f, -25.0f, 25.0f);
    UpdateN07CameraMounts();
}

void ADDSCityRollingPawn::OnToggleCameraPressed()
{
    if (UGameplayStatics::IsGamePaused(this)) { return; }
    if (bHasLastRenderedView)
    {
        BlendStartView = LastRenderedView;
    }
    else
    {
        UCameraComponent* Previous = CameraMode == EDDSN07CameraMode::Chase ? DiagnosticCamera.Get() : SensorCamera.Get();
        Previous->GetCameraView(0.0f, BlendStartView);
    }
    CameraMode = CameraMode == EDDSN07CameraMode::Chase ? EDDSN07CameraMode::Sensor : EDDSN07CameraMode::Chase;
    ++CameraSwitchCount;
    bViewBlendActive = !bInstantCameraComfort;
    ViewBlendStartRealSeconds = GetWorld()->GetRealTimeSeconds();
    LastViewEdgePlatformSeconds = FPlatformTime::Seconds();
    bViewFirstFrameLogged = false;
    UE_LOG(LogTemp, Display, TEXT("N07_VIEW_EDGE switch=%d target=%s pawn=%p controller=%p xyz=%s heading=%.3f actualSpeed=%.4f adapter=%d instant=%d world=%.6f"),
        CameraSwitchCount, CameraMode == EDDSN07CameraMode::Sensor ? TEXT("Sensor") : TEXT("Chase"), this, GetController(),
        *GetActorLocation().ToString(), MotionSample.HeadingDegrees, MotionSample.SignedSpeedMetersPerSecond,
        ControlledAdapterPhase, bInstantCameraComfort, GetWorld()->GetTimeSeconds());
    RefreshN07Hud();
}

void ADDSCityRollingPawn::ToggleCameraForProbe()
{
    OnToggleCameraPressed();
}

void ADDSCityRollingPawn::OnToggleInstantComfortPressed()
{
    bInstantCameraComfort = !bInstantCameraComfort;
    if (bInstantCameraComfort) { bViewBlendActive = false; }
    UE_LOG(LogTemp, Display, TEXT("N07_COMFORT instant=%d"), bInstantCameraComfort);
    RefreshN07Hud();
}

void ADDSCityRollingPawn::AdvanceControlledAdapterForProbe()
{
    ControlledAdapterPhase = (ControlledAdapterPhase + 1) % 5;
    static const TCHAR* Names[] = { TEXT("Idle"), TEXT("Opening"), TEXT("Loading"), TEXT("Closing"), TEXT("Latched") };
    UE_LOG(LogTemp, Display, TEXT("N07_CONTROLLED_ADAPTER phase=%s audioMarker=%s source=N07_TEST_ONLY cargoCommit=0 save=0"),
        Names[ControlledAdapterPhase], Names[ControlledAdapterPhase]);
    RefreshN07Hud();
}

void ADDSCityRollingPawn::OnControlledAdapterPressed()
{
    if (!UGameplayStatics::IsGamePaused(this)) { AdvanceControlledAdapterForProbe(); }
}

void ADDSCityRollingPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    if (ACitySamplePlayerCameraManager* Manager = OriginalCityCameraManager.Get())
    {
        if (!HasOriginalCityPresentation())
        {
            // Unavailable presentation holds the last actual manager view for
            // this invocation; it is not fresh physical/camera admission.
            const FMinimalViewInfo PreviousView = Manager->GetCameraCachePOV();
            ReleaseOriginalCityCamera();
            if (!PreviousView.Location.ContainsNaN() && !PreviousView.Rotation.ContainsNaN()
                && FMath::IsFinite(PreviousView.FOV) && PreviousView.FOV > 0) OutResult = PreviousView;
            return;
        }
    }
    UCameraComponent* TargetCamera = CameraMode == EDDSN07CameraMode::Chase ? DiagnosticCamera.Get() : SensorCamera.Get();
    if (!TargetCamera)
    {
        Super::CalcCamera(DeltaTime, OutResult);
        return;
    }
    FMinimalViewInfo TargetView;
    TargetCamera->GetCameraView(DeltaTime, TargetView);
    float Alpha = 1.0f;
    if (bViewBlendActive)
    {
        Alpha = FMath::Clamp((GetWorld()->GetRealTimeSeconds() - ViewBlendStartRealSeconds) / 0.28f, 0.0f, 1.0f);
        if (Alpha >= 1.0f) { bViewBlendActive = false; }
    }
    if (bViewBlendActive)
    {
        const float Smooth = Alpha * Alpha * (3.0f - 2.0f * Alpha);
        OutResult = TargetView;
        const FVector Control = (BlendStartView.Location + TargetView.Location) * 0.5f + FVector(0, 0, 80.0f);
        const float Inverse = 1.0f - Smooth;
        FVector Candidate = Inverse * Inverse * BlendStartView.Location
            + 2.0f * Inverse * Smooth * Control + Smooth * Smooth * TargetView.Location;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(DDSN07CameraBlend), false, this);
        FHitResult Hit;
        if (GetWorld()->SweepSingleByChannel(Hit, BlendStartView.Location, Candidate, FQuat::Identity,
            ECC_Camera, FCollisionShape::MakeSphere(10.0f), Query) && Hit.bBlockingHit)
        {
            Candidate = Hit.Location;
        }
        OutResult.Location = Candidate;
        OutResult.Rotation = FQuat::Slerp(BlendStartView.Rotation.Quaternion(), TargetView.Rotation.Quaternion(), Smooth).Rotator();
        OutResult.FOV = FMath::Lerp(BlendStartView.FOV, TargetView.FOV, Smooth);
    }
    else
    {
        OutResult = TargetView;
    }

    LastRenderedView = OutResult;
    bHasLastRenderedView = true;
    if (!bViewFirstFrameLogged)
    {
        bViewFirstFrameLogged = true;
        UE_LOG(LogTemp, Display, TEXT("N07_VIEW_FIRST switch=%d responseMs=%.3f view=%s"), CameraSwitchCount,
            (FPlatformTime::Seconds() - LastViewEdgePlatformSeconds) * 1000.0,
            CameraMode == EDDSN07CameraMode::Sensor ? TEXT("Sensor") : TEXT("Chase"));
    }
    if (!bViewBlendActive && Alpha >= 1.0f && LastViewEdgePlatformSeconds > 0.0)
    {
        UE_LOG(LogTemp, Display, TEXT("N07_VIEW_SETTLED switch=%d elapsedMs=%.3f xyz=%s"), CameraSwitchCount,
            (FPlatformTime::Seconds() - LastViewEdgePlatformSeconds) * 1000.0, *OutResult.Location.ToString());
        LastViewEdgePlatformSeconds = 0.0;
    }
}

void ADDSCityRollingPawn::UpdateN07Telemetry()
{
    TelemetrySample.WorldSeconds = MotionSample.WorldSeconds;
    TelemetrySample.bGrounded = MotionSample.bGrounded;
    const FVector SurfaceNormal = FVector::UpVector;
    const FVector TravelForward = FVector::VectorPlaneProject(
        PhysicalSnapshot.Bodies[0].WorldCm.GetRotation().RotateVector(FVector::ForwardVector),
        SurfaceNormal).GetSafeNormal();
    const FVector ActualTravelVelocity = FVector::VectorPlaneProject(MotionSample.ActualVelocityCmPerSecond, SurfaceNormal);
    TelemetrySample.SignedSpeedMetersPerSecond = FVector::DotProduct(ActualTravelVelocity, TravelForward) / 100.0f;

    const double Now = PhysicalSnapshot.CompletedStateTimeS;
    const FVector At = PhysicalSnapshot.Bodies[0].WorldCm.GetLocation();
    const double ReferenceDelta = Now - LastTelemetryReferenceSeconds;
    TelemetrySample.bReferenceValid = MotionSample.bPhysicalStateKnown
        && bTelemetryReferenceValid && !bTelemetryReferenceResetNeeded
        && ReferenceDelta > 0.0001 && ReferenceDelta <= 0.2;
    if (TelemetrySample.bReferenceValid)
    {
        const FVector ReferenceVelocity = (At - LastTelemetryReferenceLocation) / ReferenceDelta;
        TelemetrySample.ReferenceSpeedMetersPerSecond = FVector::DotProduct(
            FVector::VectorPlaneProject(ReferenceVelocity, SurfaceNormal), TravelForward) / 100.0f;
    }
    else
    {
        TelemetrySample.ReferenceSpeedMetersPerSecond = 0.0f;
    }
    LastTelemetryReferenceLocation = At;
    LastTelemetryReferenceSeconds = Now;
    bTelemetryReferenceValid = MotionSample.bPhysicalStateKnown;
    bTelemetryReferenceResetNeeded = false;

    TelemetrySample.bClearanceValid = MotionSample.bPhysicalStateKnown && GetWorld() != nullptr && SensorCamera != nullptr && !bForceClearanceInvalidForProbe;
    TelemetrySample.bClearBeyondRange = false;
    TelemetrySample.FrontClearanceMeters = 8.0f;
    if (TelemetrySample.bClearanceValid)
    {
        const FVector Origin = SensorCamera->GetComponentLocation();
        FCollisionQueryParams Query(SCENE_QUERY_STAT(DDSN07Clearance), false, this);
        const bool bOriginBlocked = GetWorld()->OverlapBlockingTestByChannel(Origin, FQuat::Identity,
            ECC_Visibility, FCollisionShape::MakeSphere(1.0f), Query);
        if (bOriginBlocked)
        {
            TelemetrySample.bClearanceValid = false;
        }
        else
        {
            int32 HitCount = 0;
            for (int32 Index = 0; Index < 9; ++Index)
            {
                const FVector Direction = SensorCamera->GetComponentQuat().RotateVector(FRotator(0, (Index - 4) * 15.0f, 0).Vector());
                const FVector End = Origin + Direction * 800.0f;
                FHitResult Hit;
                const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Origin, End, ECC_Visibility, Query)
                    && Hit.IsValidBlockingHit();
                TelemetrySample.RayOrigins[Index] = Origin;
                TelemetrySample.RayEnds[Index] = bHit ? Hit.ImpactPoint : End;
                TelemetrySample.bRayHits[Index] = bHit;
                TelemetrySample.RayDistancesMeters[Index] = bHit ? FVector::Distance(Origin, Hit.ImpactPoint) / 100.0f : 8.0f;
                if (bHit)
                {
                    ++HitCount;
                    TelemetrySample.FrontClearanceMeters = FMath::Min(TelemetrySample.FrontClearanceMeters,
                        TelemetrySample.RayDistancesMeters[Index]);
                }
            }
            TelemetrySample.bClearBeyondRange = HitCount == 0;
        }
    }

    if (Now - LastTelemetryLogSeconds >= 0.1)
    {
        LastTelemetryLogSeconds = Now;
        UE_LOG(LogTemp, Display, TEXT("N07_SAMPLE t=%.6f view=%s speed=%.5f reference=%.5f refValid=%d grounded=%d clearanceValid=%d beyond8=%d clearance=%.5f rays=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f hits=%d%d%d%d%d%d%d%d%d origin=%s"),
            Now, CameraMode == EDDSN07CameraMode::Sensor ? TEXT("Sensor") : TEXT("Chase"),
            TelemetrySample.SignedSpeedMetersPerSecond, TelemetrySample.ReferenceSpeedMetersPerSecond,
            TelemetrySample.bReferenceValid, TelemetrySample.bGrounded, TelemetrySample.bClearanceValid,
            TelemetrySample.bClearBeyondRange, TelemetrySample.FrontClearanceMeters,
            TelemetrySample.RayDistancesMeters[0], TelemetrySample.RayDistancesMeters[1], TelemetrySample.RayDistancesMeters[2],
            TelemetrySample.RayDistancesMeters[3], TelemetrySample.RayDistancesMeters[4], TelemetrySample.RayDistancesMeters[5],
            TelemetrySample.RayDistancesMeters[6], TelemetrySample.RayDistancesMeters[7], TelemetrySample.RayDistancesMeters[8],
            TelemetrySample.bRayHits[0], TelemetrySample.bRayHits[1], TelemetrySample.bRayHits[2],
            TelemetrySample.bRayHits[3], TelemetrySample.bRayHits[4], TelemetrySample.bRayHits[5],
            TelemetrySample.bRayHits[6], TelemetrySample.bRayHits[7], TelemetrySample.bRayHits[8],
            *SensorCamera->GetComponentLocation().ToString());
    }
}

void ADDSCityRollingPawn::RefreshN07Hud()
{
    if (!TelemetryWidget) { return; }
    if (MissionCoordinator)
    {
        TelemetryWidget->SetViewModel(MissionCoordinator->BuildViewModel(),
            bInstantCameraComfort, RecoveryStatus);
        return;
    }
    const FString View = CameraMode == EDDSN07CameraMode::Sensor ? TEXT("SENSOR") : TEXT("CHASE");
    const FString Speed = !MotionSample.bPhysicalStateKnown ? TEXT("Speed/support: Unavailable")
        : FString::Printf(TEXT("Speed: %+.2f m/s | pitch %.1f roll %.1f deg"), TelemetrySample.SignedSpeedMetersPerSecond,
            MotionSample.BodyPitchDegrees, MotionSample.BodyRollDegrees);
    const FString Clearance = !TelemetrySample.bClearanceValid ? TEXT("Front clearance: Unavailable")
        : TelemetrySample.bClearBeyondRange ? TEXT("Front clearance: >8 m")
        : FString::Printf(TEXT("Front clearance: %.2f m"), TelemetrySample.FrontClearanceMeters);
    const FString Prompt = MissionCoordinator ? MissionCoordinator->BuildPrompt()
        : TEXT("E unavailable | mission coordinator missing");
    const FString Recovery = MissionCoordinator && RecoveryStatus == TEXT("Ready")
        ? MissionCoordinator->DescribeRecoveryReady() : RecoveryStatus;
    TelemetryWidget->SetReadouts(View, Speed, Clearance, Prompt, Recovery,
        bInstantCameraComfort, ControlledAdapterPhase);
}

void ADDSCityRollingPawn::SetThrottle(float Value)
{
    if (!bTestControls)
    {
        Throttle = FMath::Clamp(Value, -1.0f, 1.0f);
    }
}

void ADDSCityRollingPawn::SetSteer(float Value)
{
    if (!bTestControls)
    {
        Steer = FMath::Clamp(Value, -1.0f, 1.0f);
    }
}

void ADDSCityRollingPawn::SetTestControls(float InThrottle, float InSteer, bool bInBrake)
{
    bTestControls = true;
    Throttle = FMath::Clamp(InThrottle, -1.0f, 1.0f);
    Steer = FMath::Clamp(InSteer, -1.0f, 1.0f);
    bBrakeHeld = bInBrake;
}

void ADDSCityRollingPawn::ClearTestControls()
{
    bTestControls = false;
    Throttle = 0.0f;
    Steer = 0.0f;
    bBrakeHeld = false;
}

void ADDSCityRollingPawn::ResetForProbe(const FVector& Location, float YawDegrees)
{
    // Historical fixture cannot mint N08/N12/N11 full-assembly reset permission.
    RecoveryStatus = TEXT("Probe reset requires admitted physical assembly permit");
    UE_LOG(LogTemp, Error, TEXT("N06_HISTORICAL_PROBE_RESET_REFUSED requested=%s yaw=%.3f"),
        *Location.ToString(), YawDegrees);
}

void ADDSCityRollingPawn::SetDiagnosticSideViewForProbe(bool bShowNearSideWheel)
{
    // Test-fixture view only: the one-wheel pad is at +Y (wheel 1).
    DiagnosticArm->SetRelativeRotation(bShowNearSideWheel ? FRotator(-10.0f, -65.0f, 0.0f) : FRotator(-20.0f, 30.0f, 0.0f));
    DiagnosticArm->TargetArmLength = bShowNearSideWheel ? 180.0f : 220.0f;
}

void ADDSCityRollingPawn::OnBrakePressed() { bBrakeHeld = true; }
void ADDSCityRollingPawn::OnBrakeReleased() { bBrakeHeld = false; }
void ADDSCityRollingPawn::OnRecoverPressed()
{
    bRecoverHeld = true;
    bRecoverTriggered = false;
    HoldStartSeconds = GetWorld()->GetRealTimeSeconds();
    RecoveryStatus = TEXT("Hold R to recover — 0%");
    UE_LOG(LogTemp, Display, TEXT("N06_RECOVERY_HOLD started"));
}
void ADDSCityRollingPawn::OnRecoverReleased()
{
    if (bRecoverHeld && !bRecoverTriggered)
    {
        RecoveryStatus = TEXT("Recovery hold canceled");
    }
    bRecoverHeld = false;
    UE_LOG(LogTemp, Display, TEXT("N06_RECOVERY_HOLD released triggered=%d"), bRecoverTriggered);
}

void ADDSCityRollingPawn::OnInteractPressed()
{
    if (MissionCoordinator) { MissionCoordinator->OnInteractInput(true); }
}

void ADDSCityRollingPawn::OnInteractReleased()
{
    if (MissionCoordinator) { MissionCoordinator->OnInteractInput(false); }
}

bool ADDSCityRollingPawn::RequestRecoveryViaAuthority(bool bFromPausedMenu)
{
    if (!MissionCoordinator)
    {
        RecoveryStatus = TEXT("Recovery unavailable: mission coordinator missing");
        return false;
    }
    const FDDSN08RecoveryOutcome Outcome = MissionCoordinator->TryRecovery(bFromPausedMenu);
    RecoveryStatus = Outcome.Status;
    RefreshN07Hud();
    return Outcome.bRecovered;
}

bool ADDSCityRollingPawn::TryGetN08ServiceSocketWorldLocation(FVector& OutLocation) const
{
    if (!PhysicsRig || !PhysicsRig->GetActualChassis()) return false;
    FDDSRobotSnapshot S;
    if (!PhysicsRig->ConsumeSnapshot(S)) return false;
    const FTransform& Local = PhysicsRig->GetMeasuredBinding().ServiceSocketInChassisMeters;
    OutLocation = S.Bodies[0].WorldCm.TransformPosition(Local.GetLocation() * 100.0);
    return !OutLocation.ContainsNaN();
}

const USceneComponent* ADDSCityRollingPawn::GetN08MeasuredServiceSocket() const
{
    FVector Current;
    return ServiceSocket && ServiceSocket->IsRegistered()
        && ServiceSocket->GetOwner() == this
        && TryGetN08ServiceSocketWorldLocation(Current) ? ServiceSocket.Get() : nullptr;
}

void ADDSCityRollingPawn::SetMovementLocked(bool bLocked)
{
    // Locks drive request only. Finite physical brake is applied in callback.
    bMovementLocked = bLocked;
}

void ADDSCityRollingPawn::TogglePause()
{
    const bool bPause = !UGameplayStatics::IsGamePaused(this);
    if (MissionCoordinator) MissionCoordinator->StopAudioAtPauseOrRecovery();
    // Menu focus can consume key-up events. Discard every held gameplay input at
    // the boundary so an interrupted R hold or drive/brake press cannot resume.
    bRecoverHeld = false;
    bRecoverTriggered = false;
    if (MissionCoordinator) { MissionCoordinator->ReleaseInteractAtInputBoundary(); }
    Throttle = 0.0f;
    Steer = 0.0f;
    bBrakeHeld = false;
    APlayerController* PlayerController = Cast<APlayerController>(GetController());
    if (PlayerController)
    {
        PlayerController->FlushPressedKeys();
    }
    if (MissionCoordinator) { MissionCoordinator->ReleaseInteractAtInputBoundary(); }
    UGameplayStatics::SetGamePaused(this, bPause);
    UE_LOG(LogTemp, Display, TEXT("N06_PAUSE transition=%s recoveryHold=0 throttle=0 steer=0 brake=0 physicalVelocityPreserved=1"), bPause ? TEXT("paused") : TEXT("resumed"));
    if (bPause)
    {
        RecoveryStatus = TEXT("Paused — choose Recover robot or Resume");
        if (PlayerController)
        {
            PauseMenu = CreateWidget<UDDSN06PauseMenu>(PlayerController, UDDSN06PauseMenu::StaticClass());
            if (PauseMenu)
            {
                PauseMenu->SetPawn(this);
                PauseMenu->AddToViewport(100);
                FInputModeGameAndUI Mode;
                Mode.SetWidgetToFocus(PauseMenu->TakeWidget());
                PlayerController->SetInputMode(Mode);
                PlayerController->bShowMouseCursor = true;
            }
        }
    }
    else
    {
        if (PauseMenu)
        {
            PauseMenu->RemoveFromParent();
            PauseMenu = nullptr;
        }
        if (PlayerController)
        {
            PlayerController->SetInputMode(FInputModeGameOnly());
            PlayerController->bShowMouseCursor = false;
        }
        RecoveryStatus = TEXT("Ready");
    }
}

void ADDSCityRollingPawn::ResumeFromPauseMenu()
{
    if (UGameplayStatics::IsGamePaused(this))
    {
        TogglePause();
    }
}









void ADDSCityRollingPawn::StoreRecentPose()
{
    // Source provider cannot label original sidewalk support without I01 identity.
    // RecordValidatedPhysicalRecoveryPose is the explicit forward provider seam.
}

bool ADDSCityRollingPawn::ValidateRecoveryPose(const FTransform& Pose) const
{
    return PhysicsRig && PhysicsRig->ValidateFullAssemblyRecoveryPose(Pose);
}

bool ADDSCityRollingPawn::RequestRecovery(bool bFromPausedMenu, const FDDSRecoveryLedgerView* SuppliedLedger)
{
    if (bMovementLocked || !SuppliedLedger || SuppliedLedger->MissionInstanceId.IsEmpty())
    { RecoveryStatus = TEXT("Recovery waits for N08 closure and persistence permission"); return false; }
    const double Started = FPlatformTime::Seconds();
    int32 Checked = 0;
    bool Found = false;
    for (int32 I = RecentPoses.Num() - 1; I >= 0 && Checked < 16; --I)
    {
        if (FPlatformTime::Seconds() - Started > 1.0) break;
        ++Checked;
        if (ValidateRecoveryPose(RecentPoses[I])) { Found = true; break; }
    }
    if (!Found && bMeasuredStartBayValidated && FPlatformTime::Seconds() - Started <= 1.0)
    { ++Checked; Found = ValidateRecoveryPose(StartPose); }
    RecoveryStatus = Found ? TEXT("Clear assembly pose awaits tagged N08/N12/N11 reset permit")
        : TEXT("No clear recovery spot - Wait and retry");
    UE_LOG(LogTemp, Display, TEXT("N06_PHYSICAL_RECOVERY_SEARCH checked=%d elapsedMs=%.2f menu=%d found=%d reset=0"),
        Checked, (FPlatformTime::Seconds() - Started) * 1000, bFromPausedMenu, Found);
    return false; // Legacy ledger view is NOT a physical reset/latch/save permit.
}


bool ADDSCityRollingPawn::BindMeasuredOriginalRobot(const FDDSRobotMeasuredBinding& B,
    const FDDSRobotPhysicsProfile& P, FString& Error)
{
    if (!PhysicsRig->CreateMeasuredRig(B, P, Error)) return false;
    Chassis = PhysicsRig->GetActualChassis(); Body = Chassis;
    Wheels.Reset();
    for (int32 I = 1; I < 7; ++I) Wheels.Add(B.Bodies[I].ActualPart);
    DiagnosticArm->AttachToComponent(Chassis, FAttachmentTransformRules::KeepRelativeTransform);
    SensorCamera->AttachToComponent(Chassis, FAttachmentTransformRules::KeepRelativeTransform);
    ServiceSocket->AttachToComponent(Chassis, FAttachmentTransformRules::KeepRelativeTransform);
    DiagnosticArm->SetUsingAbsoluteScale(true);
    SensorCamera->SetUsingAbsoluteScale(true); ServiceSocket->SetUsingAbsoluteScale(true);
    DiagnosticArm->SetRelativeLocation(DDSVisualAttachmentCm(B.ChaseMountInChassisMeters, B.Bodies[0].SupplierScale).GetLocation());
    SensorCamera->SetRelativeLocation(DDSVisualAttachmentCm(B.SensorMountInChassisMeters, B.Bodies[0].SupplierScale).GetLocation());
    ServiceSocket->SetRelativeLocation(DDSVisualAttachmentCm(B.ServiceSocketInChassisMeters, B.Bodies[0].SupplierScale).GetLocation());
    ServiceSocket->SetRelativeRotation(B.ServiceSocketInChassisMeters.GetRotation());
    StartPose = DDSRigidComponentPose(Chassis->GetComponentTransform());
    PhysicalCommandSequence += 16; // Above initial setup/reserved provider sequences.
    UpdateN07CameraMounts();
    RecoveryStatus = TEXT("Physical binding submitted; original collision readiness required");
    UpdateOriginalCityPresentation();
    return true;
}

bool ADDSCityRollingPawn::HasOriginalCityPresentation() const
{
    const UStaticMeshComponent* Actual = PhysicsRig ? PhysicsRig->GetActualChassis() : nullptr;
    if (bOriginalCityEndingPlay || !IsValid(Actual) || !Actual->IsRegistered() || Actual->GetOwner() != this
        || PresentationChassis.Get() != Actual || !MotionRoot || MotionRoot->GetAttachParent()) return false;
    const FTransform CurrentChassisTransform = Actual->GetComponentTransform();
    if (!CurrentChassisTransform.IsValid()) return false; // Includes infinity/NaN and unnormalized rotation.
    TArray<USceneComponent*> PresentationDescendants;
    MotionRoot->GetChildrenComponents(true, PresentationDescendants);
    return PresentationDescendants.IsEmpty(); // No descendant, especially no physical part, may be transformed.
}

bool ADDSCityRollingPawn::UpdateOriginalCityPresentation()
{
    UStaticMeshComponent* Actual = PhysicsRig ? PhysicsRig->GetActualChassis() : nullptr;
    TArray<USceneComponent*> PresentationDescendants;
    if (MotionRoot) MotionRoot->GetChildrenComponents(true, PresentationDescendants);
    if (bOriginalCityEndingPlay || !IsValid(Actual) || !Actual->IsRegistered() || Actual->GetOwner() != this
        || !MotionRoot || MotionRoot->GetAttachParent() || !PresentationDescendants.IsEmpty())
    {
        PresentationChassis.Reset();
        ReleaseOriginalCityCamera();
        return false;
    }
    const FTransform CurrentChassisTransform = Actual->GetComponentTransform();
    if (!CurrentChassisTransform.IsValid())
    {
        PresentationChassis.Reset(); ReleaseOriginalCityCamera(); return false;
    }
    // UE5.3 rejects same-owner root -> chassis attachment. This empty scene
    // component follows the actual GT chassis pose; no physical body is written.
    const uint64 Revision = OriginalCityCameraRevision;
    MotionRoot->SetWorldLocationAndRotationNoPhysics(CurrentChassisTransform.GetLocation(), CurrentChassisTransform.Rotator());
    if (bOriginalCityEndingPlay || Revision != OriginalCityCameraRevision || !IsValid(Actual)
        || PhysicsRig->GetActualChassis() != Actual)
    { PresentationChassis.Reset(); ReleaseOriginalCityCamera(); return false; }
    PresentationChassis = Actual;
    if (ACitySamplePlayerCameraManager* Manager = OriginalCityCameraManager.Get())
    {
        const ACitySamplePlayerController* PC = Cast<ACitySamplePlayerController>(GetController());
        if (!PC || PC->PlayerCameraManager != Manager || Manager->PCOwner != PC
            || PC->GetViewTarget() != this || Manager->HasAlternateCameraAvailable()
            || Manager->IsUsingAlternateCamera() || Manager->CameraStyle != OriginalCityStyleToken)
            ReleaseOriginalCityCamera();
    }
    return true;
}

bool ADDSCityRollingPawn::ActivateOriginalCityCamera(FString& Error)
{
    check(IsInGameThread());
    if (!UpdateOriginalCityPresentation()) { Error = TEXT("Original chassis presentation binding unavailable"); return false; }
    ACitySamplePlayerController* PC = Cast<ACitySamplePlayerController>(GetController());
    ACitySamplePlayerCameraManager* Manager = PC ? Cast<ACitySamplePlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
    if (!Manager || Manager->PCOwner != PC || PC->GetViewTarget() != this || bOriginalCityEndingPlay)
    { Error = TEXT("Original City controller/manager must already view this bound pawn"); return false; }
    if (OriginalCityCameraManager.Get() == Manager && Manager->CameraStyle == OriginalCityStyleToken
        && !Manager->HasAlternateCameraAvailable() && !Manager->IsUsingAlternateCamera()) return true;
    if (Manager->HasAlternateCameraAvailable() || Manager->IsUsingAlternateCamera())
    { Error = TEXT("Original City alternate camera slot is occupied"); return false; }
    if (Manager->CameraStyle != NAME_Default)
    { Error = TEXT("Original City engine camera style is occupied"); return false; }
    ReleaseOriginalCityCamera();
    // Public CameraStyle assignment has no SetViewTarget/Blueprint callbacks.
    // Unknown engine styles use BlueprintUpdateCamera, then Pawn.CalcCamera
    // when that event declines. The original Blueprint camera override is honored.
    OriginalCityStyleToken = FName(*FString::Printf(TEXT("DDSOriginalPawnCamera_%u"), GetUniqueID()));
    SavedOriginalCityStyle = Manager->CameraStyle;
    OriginalCityCameraManager = Manager;
    Manager->CameraStyle = OriginalCityStyleToken;
    return true;
}

void ADDSCityRollingPawn::ReleaseOriginalCityCamera()
{
    ACitySamplePlayerCameraManager* Manager = OriginalCityCameraManager.Get();
    OriginalCityCameraManager.Reset();
    ++OriginalCityCameraRevision; // Withdraw before any continuation can use ownership.
    // Direct matching-token restore has no view callbacks and never writes the
    // publisher alternate slot, even when a foreign owner took that slot.
    if (Manager && Manager->CameraStyle == OriginalCityStyleToken)
        Manager->CameraStyle = SavedOriginalCityStyle;
}

bool ADDSCityRollingPawn::GetFreshPhysicalServiceEvidence(FDDSRobotSnapshot& Out) const
{
    return PhysicsRig && PhysicsRig->IsServiceEvidenceFresh(FPlatformTime::Seconds(), Out);
}

bool ADDSCityRollingPawn::GetFreshPhysicalStateEvidence(FDDSRobotSnapshot& Out) const
{
    return PhysicsRig && PhysicsRig->ConsumeSnapshot(Out);
}

bool ADDSCityRollingPawn::SetPhysicalCollisionReadiness(uint64 Epoch, bool bReady)
{
    if (!PhysicsRig) return false;
    if (Epoch > PhysicsRig->GetCollisionWorldEpoch()) PhysicsRig->InvalidateCollisionWorld(Epoch);
    if (!PhysicsRig->SetCollisionWorldReady(Epoch, bReady)) return false;
    PhysicalWorldEpoch = PhysicsRig->GetCollisionWorldEpoch(); return true;
}

bool ADDSCityRollingPawn::GetPreReadinessPhysicalState(FDDSRobotSnapshot& Out) const
{ return PhysicsRig && PhysicsRig->ReadPreReadinessCompletedState(Out); }

bool ADDSCityRollingPawn::AcquireOwnedPhysicalReadiness(UActorComponent* ReadinessOwner, FDDSCollisionReadinessCapability& Out)
{
    if (!PhysicsRig || !PhysicsRig->AcquireCollisionReadiness(ReadinessOwner, Out)
        || !PhysicsRig->CollisionReadinessMatches(ReadinessOwner, Out)) return false;
    PhysicalWorldEpoch = Out.GetWorldEpoch(); return true;
}

bool ADDSCityRollingPawn::CommitOwnedPhysicalReadiness(UActorComponent* ReadinessOwner,
    FDDSCollisionReadinessCapability& R, bool bReady)
{
    if (!PhysicsRig || !PhysicsRig->CompareAndSetCollisionReadiness(ReadinessOwner, R, bReady)
        || !PhysicsRig->CollisionReadinessMatches(ReadinessOwner, R)) return false;
    PhysicalWorldEpoch = R.GetWorldEpoch(); return true;
}

bool ADDSCityRollingPawn::WithdrawOwnedPhysicalReadiness(UActorComponent* ReadinessOwner,
    FDDSCollisionReadinessCapability& R, bool bRelease)
{
    if (!PhysicsRig || !PhysicsRig->WithdrawCollisionReadiness(ReadinessOwner, R, bRelease)) return false;
    PhysicalWorldEpoch = PhysicsRig->GetCollisionWorldEpoch(); return true;
}

bool ADDSCityRollingPawn::ApplyApprovedPhysicalRecovery(const FDDSRobotRecoveryPermit& Permit)
{
    if (MissionCoordinator) MissionCoordinator->StopAudioAtPauseOrRecovery();
    const bool Queued = PhysicsRig->ResetAssemblyAtApprovedRecoveryPose(Permit);
    RecoveryStatus = Queued ? TEXT("Assembly reset queued; waiting for physical boundary and settling")
        : TEXT("Physical recovery permission/pose rejected");
    bTelemetryReferenceResetNeeded = true;
    return Queued;
}

bool ADDSCityRollingPawn::RecordValidatedPhysicalRecoveryPose(const FTransform& Pose,
    uint64 PhysicsEpoch, uint64 CollisionEpoch, bool bOriginalSidewalkSupportValidated, bool bStartBay)
{
    FDDSRobotSnapshot S;
    if (!bOriginalSidewalkSupportValidated || !PhysicsRig->IsServiceEvidenceFresh(FPlatformTime::Seconds(), S)
        || PhysicsEpoch != S.Generation || CollisionEpoch != S.CollisionWorldGeneration
        || !PhysicsRig->ValidateFullAssemblyRecoveryPose(Pose)) return false;
    if (bStartBay) { StartPose = Pose; bMeasuredStartBayValidated = true; return true; }
    if (RecentPoses.Num() && FVector::Dist2D(RecentPoses.Last().GetLocation(), Pose.GetLocation()) < 40) return false;
    RecentPoses.Add(Pose);
    if (RecentPoses.Num() > 16) RecentPoses.RemoveAt(0);
    return true;
}
