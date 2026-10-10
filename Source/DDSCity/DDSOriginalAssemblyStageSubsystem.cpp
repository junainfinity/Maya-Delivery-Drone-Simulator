#include "DDSOriginalAssemblyStageSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/CitySamplePlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogDDSOriginalAssemblyStage, Log, All);

namespace DDSOriginalAssemblyStage
{
    constexpr double StartupSeconds = 600.0;
    constexpr double ReadyWaitSeconds = 120.0;
    constexpr double ScreenshotWaitSeconds = 60.0;
    constexpr int32 MaxPendingTransitionLogs = 64;
    constexpr int32 ScreenshotDraws = 3;
    constexpr int64 MaxScreenshotBytes = 16 * 1024 * 1024;
    constexpr double RecordingSeconds = 30.0, RecordingTailSeconds = 20.0;
    constexpr double RecordingSettlingSeconds = 10.0;
    constexpr double RecordingSampleInterval = 1.0 / 30.0;
    constexpr int32 MaxRecordingFrames = 900;
    constexpr int64 MaxRecordingFrameBytes = 224LL * 1024 * 1024;
    constexpr int64 MaxRecordingMetadataBytes = 2LL * 1024 * 1024;
    constexpr int32 MaxSpawnAttempts = 4;
    const TCHAR* MapPackage = TEXT("/Game/Map/Small_City_LVL");
    const TCHAR* VehicleClass = TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C");
    const TCHAR* Label = TEXT("Original asset assembly preview; driving not enabled");
    const TCHAR* OriginalMode = TEXT("/Game/Gameplay/Framework/BP_CitySampleGameMode.BP_CitySampleGameMode_C");

    const TCHAR* PendingName(EDDSOriginalStagePendingReason Reason)
    {
        switch (Reason)
        {
#define DDS_STAGE_REASON(Name) case EDDSOriginalStagePendingReason::Name: return TEXT(#Name)
            DDS_STAGE_REASON(None); DDS_STAGE_REASON(WorldUnavailable); DDS_STAGE_REASON(WorldNotBegunPlay);
            DDS_STAGE_REASON(PersistentLevelUnavailable); DDS_STAGE_REASON(ControllerUnavailable);
            DDS_STAGE_REASON(ControllerNotLocal); DDS_STAGE_REASON(ControllerNotBegunPlay);
            DDS_STAGE_REASON(PawnUnavailable); DDS_STAGE_REASON(PawnNotBegunPlay);
            DDS_STAGE_REASON(ViewTargetUnavailable); DDS_STAGE_REASON(CameraManagerUnavailable);
            DDS_STAGE_REASON(AlternateCameraActive); DDS_STAGE_REASON(CentreSupportMissing);
            DDS_STAGE_REASON(SuppliedSpawnFailed); DDS_STAGE_REASON(MeshBoundsUnavailable);
            DDS_STAGE_REASON(CornerSupportMissing); DDS_STAGE_REASON(SupportNonPlanar);
            DDS_STAGE_REASON(GeometryMoveFailed); DDS_STAGE_REASON(ComponentPoseMismatch);
            DDS_STAGE_REASON(PlacedBoundsMismatch); DDS_STAGE_REASON(ViewObstructed);
            DDS_STAGE_REASON(CameraSpawnFailed); DDS_STAGE_REASON(ContextChanged); DDS_STAGE_REASON(Ready);
#undef DDS_STAGE_REASON
        }
        return TEXT("Unknown");
    }

    bool Ground(UWorld* World, const FVector& XY, AActor* Ignored, FHitResult& Hit)
    {
        FCollisionQueryParams Params(SCENE_QUERY_STAT(DDSOriginalStageGround), false);
        if (Ignored) Params.AddIgnoredActor(Ignored);
        const bool bHit = World->LineTraceSingleByObjectType(Hit, XY + FVector(0,0,1000),
            XY - FVector(0,0,2000), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
        return bHit && Hit.bBlockingHit && !Hit.bStartPenetrating && Hit.ImpactNormal.Z >= 0.95;
    }
}

bool UDDSOriginalAssemblyStageSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
    return false;
#else
    const UWorld* World = Cast<UWorld>(Outer);
    return World && World->WorldType == EWorldType::Game
        && FParse::Param(FCommandLine::Get(), TEXT("DDSOriginalAssemblyStage"))
        && Super::ShouldCreateSubsystem(Outer);
#endif
}

void UDDSOriginalAssemblyStageSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    StartedSeconds = FPlatformTime::Seconds();
    NextAttemptSeconds = StartedSeconds;
    ConfigureScreenshot();
    ConfigureRecording();
    UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("Opt-in pending: %s; startup<=600s, ready support/camera wait<=120s after actual world/controller/pawn readiness; supplied actor spawn attempts<=4."), DDSOriginalAssemblyStage::Label);
}

bool UDDSOriginalAssemblyStageSubsystem::IsTickable() const
{
    return !IsTemplate() && IsInitialized() && (!bFinished || bScreenshotPending || bRecordingPending);
}

TStatId UDDSOriginalAssemblyStageSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UDDSOriginalAssemblyStageSubsystem, STATGROUP_Tickables);
}

void UDDSOriginalAssemblyStageSubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (bFinished)
    {
        if (bScreenshotPending) TickScreenshot();
        if (bRecordingPending) TickRecording();
        return;
    }
    const double Now = FPlatformTime::Seconds();
    if (!StageBudgetLive())
    {
        bFinished = true;
        ReleaseOwnedPreview();
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("REFUSED: bounded stage wait elapsed; lastPending=%s startupElapsed=%.3fs readyElapsed=%.3fs attempts=%d; startup<=600s/ready<=120s. No stage/render claim."),
            DDSOriginalAssemblyStage::PendingName(LastPendingReason), Now - StartedSeconds,
            ReadyStartedSeconds < 0.0 ? -1.0 : Now - ReadyStartedSeconds, SpawnAttempts);
        return;
    }
    if (Now < NextAttemptSeconds) return;
    NextAttemptSeconds = Now + 1.0;
    if (TryStage()) bFinished = true;
}

bool UDDSOriginalAssemblyStageSubsystem::StageBudgetLive() const
{
    const double Now = FPlatformTime::Seconds();
    return Now - StartedSeconds < DDSOriginalAssemblyStage::StartupSeconds
        && (ReadyStartedSeconds < 0.0 || Now - ReadyStartedSeconds < DDSOriginalAssemblyStage::ReadyWaitSeconds);
}

void UDDSOriginalAssemblyStageSubsystem::SetPendingReason(EDDSOriginalStagePendingReason Reason)
{
    if (LastPendingReason == Reason) return;
    LastPendingReason = Reason;
    if (PendingTransitionLogs < DDSOriginalAssemblyStage::MaxPendingTransitionLogs)
    {
        ++PendingTransitionLogs;
        UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("STAGE_PENDING reason=%s startupElapsed=%.3fs readyElapsed=%.3fs transition=%d/64 attempts=%d"),
            DDSOriginalAssemblyStage::PendingName(Reason), FPlatformTime::Seconds() - StartedSeconds,
            ReadyStartedSeconds < 0.0 ? -1.0 : FPlatformTime::Seconds() - ReadyStartedSeconds, PendingTransitionLogs, SpawnAttempts);
    }
}

bool UDDSOriginalAssemblyStageSubsystem::TryStage()
{
    UWorld* World = GetWorld();
    if (!World) { SetPendingReason(EDDSOriginalStagePendingReason::WorldUnavailable); return false; }
    if (!World->HasBegunPlay()) { SetPendingReason(EDDSOriginalStagePendingReason::WorldNotBegunPlay); return false; }
    if (!World->PersistentLevel) { SetPendingReason(EDDSOriginalStagePendingReason::PersistentLevelUnavailable); return false; }
    if (World->PersistentLevel->GetOutermost()->GetName() != DDSOriginalAssemblyStage::MapPackage)
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("REFUSED: persistent map is %s, required %s."),
            *World->PersistentLevel->GetOutermost()->GetName(), DDSOriginalAssemblyStage::MapPackage);
        return true;
    }
    APlayerController* PC = World->GetFirstPlayerController();
    APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (!PC) { SetPendingReason(EDDSOriginalStagePendingReason::ControllerUnavailable); return false; }
    if (!PC->IsLocalController()) { SetPendingReason(EDDSOriginalStagePendingReason::ControllerNotLocal); return false; }
    if (!PC->HasActorBegunPlay()) { SetPendingReason(EDDSOriginalStagePendingReason::ControllerNotBegunPlay); return false; }
    if (!Pawn) { SetPendingReason(EDDSOriginalStagePendingReason::PawnUnavailable); return false; }
    if (!Pawn->HasActorBegunPlay()) { SetPendingReason(EDDSOriginalStagePendingReason::PawnNotBegunPlay); return false; }
    if (ReadyStartedSeconds < 0.0)
    {
        ReadyStartedSeconds = FPlatformTime::Seconds(); // Latch once; loss of context never renews this budget.
        UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("STAGE_CONTEXT_READY startupElapsed=%.3fs originalController=%s originalPawn=%s; support/camera budget<=120s, overall startup<=600s."),
            ReadyStartedSeconds - StartedSeconds, *PC->GetPathName(), *Pawn->GetPathName());
    }
    if (!PC->GetViewTarget()) { SetPendingReason(EDDSOriginalStagePendingReason::ViewTargetUnavailable); return false; }
    const ACitySamplePlayerCameraManager* CityCamera = Cast<ACitySamplePlayerCameraManager>(PC->PlayerCameraManager);
    if (!CityCamera) { SetPendingReason(EDDSOriginalStagePendingReason::CameraManagerUnavailable); return false; }
    if (CityCamera->IsUsingAlternateCamera()) { SetPendingReason(EDDSOriginalStagePendingReason::AlternateCameraActive); return false; }
    if (SpawnAttempts >= DDSOriginalAssemblyStage::MaxSpawnAttempts)
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("REFUSED: all four original support/camera candidates failed. No fallback geometry."));
        return true;
    }

    const TWeakObjectPtr<AActor> CandidateViewTarget(PC->GetViewTarget());
    const TWeakObjectPtr<UWorld> ContextWorld(World);
    const TWeakObjectPtr<APlayerController> ContextPC(PC);
    const TWeakObjectPtr<APawn> ContextPawn(Pawn);
    const TWeakObjectPtr<ACitySamplePlayerCameraManager> ContextCamera(
        Cast<ACitySamplePlayerCameraManager>(PC->PlayerCameraManager));
    const auto ContextLive = [this, ContextWorld, ContextPC, ContextPawn, ContextCamera]()
    {
        UWorld* LiveWorld = ContextWorld.Get();
        APlayerController* LivePC = ContextPC.Get();
        APawn* LivePawn = ContextPawn.Get();
        ACitySamplePlayerCameraManager* LiveCamera = ContextCamera.Get();
        return StageBudgetLive() && LiveWorld && LivePC && LivePawn && LiveCamera
            && GetWorld() == LiveWorld && LiveWorld->HasBegunPlay()
            && LiveWorld->PersistentLevel
            && LiveWorld->PersistentLevel->GetOutermost()->GetName() == DDSOriginalAssemblyStage::MapPackage
            && LiveWorld->GetFirstPlayerController() == LivePC
            && LivePC->GetWorld() == LiveWorld && LivePawn->GetWorld() == LiveWorld
            && LiveCamera->GetWorld() == LiveWorld && LivePC->IsLocalController()
            && LivePC->HasActorBegunPlay() && LivePawn->HasActorBegunPlay()
            && LivePC->GetPawn() == LivePawn && LivePawn->GetController() == LivePC
            && LivePC->PlayerCameraManager == LiveCamera && !LiveCamera->IsUsingAlternateCamera();
    };
    // Seed only from the actual original pawn; never possess/replace it or change its input.
    const int32 Direction = CandidateCursor;
    CandidateCursor = (CandidateCursor + 1) % 4; // Advance even if centre support is absent.
    const FRotator Heading(0.0, Pawn->GetActorRotation().Yaw + 90.0 * Direction, 0.0);
    const FVector Seed = Pawn->GetActorLocation() + Heading.Vector() * 500.0;
    FHitResult CentreGround;
    if (!DDSOriginalAssemblyStage::Ground(World, Seed, Pawn, CentreGround)) { SetPendingReason(EDDSOriginalStagePendingReason::CentreSupportMissing); return false; }
    UClass* SuppliedClass = LoadClass<AActor>(nullptr, DDSOriginalAssemblyStage::VehicleClass);
    if (!SuppliedClass)
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Error, TEXT("REFUSED: exact supplied class unavailable: %s"), DDSOriginalAssemblyStage::VehicleClass);
        return true;
    }
    ++SpawnAttempts;
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Spawn.ObjectFlags |= RF_Transient;
    PreviewActor = World->SpawnActor<AActor>(SuppliedClass, CentreGround.ImpactPoint + FVector(0,0,200), Heading, Spawn);
    if (!PreviewActor) { SetPendingReason(EDDSOriginalStagePendingReason::SuppliedSpawnFailed); return false; }

    // Use provided runtime authored mesh transforms, including base and wheels, as-is.
    FBox VisualBounds(ForceInit);
    TArray<UStaticMeshComponent*> Meshes;
    TMap<UStaticMeshComponent*, FTransform> BeforeTransforms;
    PreviewActor->GetComponents<UStaticMeshComponent>(Meshes);
    bool bBase = false, bWheel = false;
    int32 MeshCount = 0;
    for (UStaticMeshComponent* Mesh : Meshes)
    {
        if (!Mesh || !Mesh->IsRegistered() || !Mesh->GetStaticMesh()) continue;
        ++MeshCount;
        BeforeTransforms.Add(Mesh, Mesh->GetComponentTransform());
        const FString Path = Mesh->GetStaticMesh()->GetPathName();
        bBase |= Path.Contains(TEXT("/SM_DeliveryRobot_Base."));
        bWheel |= Path.Contains(TEXT("/SM_DeliveryRobot_Wheel."));
        VisualBounds += Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox();
    }
    if (!VisualBounds.IsValid || !bBase || !bWheel || MeshCount < 15
        || VisualBounds.GetSize().GetMax() <= 0.0 || VisualBounds.GetSize().GetMax() > 1000.0)
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("REFUSED candidate: supplied base/wheel/full mesh bounds unavailable; meshes=%d."), MeshCount);
        SetPendingReason(EDDSOriginalStagePendingReason::MeshBoundsUnavailable);
        ReleaseOwnedPreview();
        return false;
    }
    double MaxGroundZ = CentreGround.ImpactPoint.Z;
    double MinGroundZ = MaxGroundZ;
    for (int32 X : {-1, 1}) for (int32 Y : {-1, 1})
    {
        FVector Corner = VisualBounds.GetCenter();
        Corner.X += X * VisualBounds.GetExtent().X;
        Corner.Y += Y * VisualBounds.GetExtent().Y;
        FHitResult Support;
        if (!DDSOriginalAssemblyStage::Ground(World, Corner, PreviewActor, Support))
        {
            SetPendingReason(EDDSOriginalStagePendingReason::CornerSupportMissing);
            ReleaseOwnedPreview();
            return false;
        }
        MaxGroundZ = FMath::Max(MaxGroundZ, Support.ImpactPoint.Z);
        MinGroundZ = FMath::Min(MinGroundZ, Support.ImpactPoint.Z);
    }
    if (MaxGroundZ - MinGroundZ > 5.0f)
    {
        SetPendingReason(EDDSOriginalStagePendingReason::SupportNonPlanar);
        ReleaseOwnedPreview();
        return false;
    }
    const FVector Lift(0,0,MaxGroundZ + 2.0 - VisualBounds.Min.Z);
    const FBox IntendedBounds = VisualBounds.ShiftBy(Lift);
    const FTransform IntendedPose(PreviewActor->GetActorRotation(),
        PreviewActor->GetActorLocation() + Lift, PreviewActor->GetActorScale3D());
    if (Lift.ContainsNaN() || IntendedPose.ContainsNaN() || !ContextLive()
        || !PreviewActor->SetActorLocation(IntendedPose.GetLocation(), false, nullptr, ETeleportType::TeleportPhysics)
        || !IsValid(PreviewActor) || !PreviewActor->GetActorTransform().Equals(IntendedPose, 0.1))
    {
        SetPendingReason(EDDSOriginalStagePendingReason::GeometryMoveFailed);
        ReleaseOwnedPreview();
        return false;
    }
    // A successful API return alone does not prove that the supplied component geometry moved.
    FBox ActualBounds(ForceInit);
    TArray<UStaticMeshComponent*> PlacedMeshes;
    PreviewActor->GetComponents<UStaticMeshComponent>(PlacedMeshes);
    int32 PlacedCount = 0;
    bool bPlacedBase = false, bPlacedWheel = false;
    for (UStaticMeshComponent* Mesh : PlacedMeshes)
    {
        if (!IsValid(Mesh) || !Mesh->IsRegistered() || !Mesh->GetStaticMesh()
            || Mesh->GetWorld() != World) continue;
        const FTransform ActualTransform = Mesh->GetComponentTransform();
        if (ActualTransform.ContainsNaN())
        {
            SetPendingReason(EDDSOriginalStagePendingReason::ComponentPoseMismatch);
            ReleaseOwnedPreview();
            return false;
        }
        const FTransform* Before = BeforeTransforms.Find(Mesh);
        if (!Before)
        {
            SetPendingReason(EDDSOriginalStagePendingReason::ComponentPoseMismatch);
            ReleaseOwnedPreview();
            return false;
        }
        FTransform ExpectedComponentPose = *Before;
        ExpectedComponentPose.AddToTranslation(Lift);
        if (!ActualTransform.Equals(ExpectedComponentPose, 0.1))
        {
            SetPendingReason(EDDSOriginalStagePendingReason::ComponentPoseMismatch);
            ReleaseOwnedPreview();
            return false;
        }
        ++PlacedCount;
        const FString Path = Mesh->GetStaticMesh()->GetPathName();
        bPlacedBase |= Path.Contains(TEXT("/SM_DeliveryRobot_Base."));
        bPlacedWheel |= Path.Contains(TEXT("/SM_DeliveryRobot_Wheel."));
        ActualBounds += Mesh->CalcBounds(ActualTransform).GetBox();
    }
    if (!ActualBounds.IsValid || ActualBounds.Min.ContainsNaN() || ActualBounds.Max.ContainsNaN()
        || PlacedCount != MeshCount || !bPlacedBase || !bPlacedWheel
        || !ActualBounds.Min.Equals(IntendedBounds.Min, 0.1)
        || !ActualBounds.Max.Equals(IntendedBounds.Max, 0.1) || !ContextLive())
    {
        SetPendingReason(EDDSOriginalStagePendingReason::PlacedBoundsMismatch);
        ReleaseOwnedPreview();
        return false;
    }
    VisualBounds = ActualBounds; // Only actual observed component bounds drive the camera and log.
    const FVector Aim = VisualBounds.GetCenter();
    const double Distance = FMath::Clamp(VisualBounds.GetSize().GetMax() * 4.0, 500.0, 900.0);
    const FVector CameraLocation = Aim - Heading.Vector() * Distance + FVector(0,0,Distance * 0.6);
    FCollisionQueryParams ViewParams(SCENE_QUERY_STAT(DDSOriginalStageView), true);
    ViewParams.AddIgnoredActor(PreviewActor);
    ViewParams.AddIgnoredActor(Pawn);
    FHitResult Obstruction;
    if (World->LineTraceSingleByObjectType(Obstruction, CameraLocation, Aim,
        FCollisionObjectQueryParams(ECC_WorldStatic), ViewParams))
    {
        SetPendingReason(EDDSOriginalStagePendingReason::ViewObstructed);
        ReleaseOwnedPreview();
        return false;
    }
    PreviewCamera = World->SpawnActor<ACameraActor>(CameraLocation, (Aim-CameraLocation).Rotation(), Spawn);
    if (!PreviewCamera)
    {
        SetPendingReason(EDDSOriginalStagePendingReason::CameraSpawnFailed);
        ReleaseOwnedPreview();
        return false;
    }
    PreviewCamera->GetCameraComponent()->FieldOfView = 60.0f;
    if (!ContextLive() || !IsValid(PreviewActor) || !IsValid(PreviewCamera)
        || PreviewCamera->GetWorld() != World || !CandidateViewTarget.IsValid()
        || PC->GetViewTarget() != CandidateViewTarget.Get())
    {
        SetPendingReason(EDDSOriginalStagePendingReason::ContextChanged);
        ReleaseOwnedPreview();
        return false;
    }
    PreviewController = PC;
    PreviousViewTarget = CandidateViewTarget;
    PC->SetViewTarget(PreviewCamera);
    // View callbacks may change target, manager, pawn or world: never publish from an intended write.
    if (!ContextLive() || !IsValid(PreviewActor) || !IsValid(PreviewCamera)
        || PreviewCamera->GetWorld() != World || PC->GetViewTarget() != PreviewCamera)
    {
        SetPendingReason(EDDSOriginalStagePendingReason::ContextChanged);
        ReleaseOwnedPreview();
        return false;
    }
    if (GEngine && !bRecordingOptIn) GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), 120.0f, FColor::Yellow, DDSOriginalAssemblyStage::Label);
    UE_LOG(LogDDSOriginalAssemblyStage, Display,
        TEXT("STAGE_ASSEMBLED map=%s mode=%s originalPawn=%s suppliedClass=%s actor=%s meshes=%d boundsMin=%s boundsMax=%s camera=%s view=%s; %s; rendered-frame/capture UNOBSERVED, native physics/driving readiness UNPROVED."),
        *World->PersistentLevel->GetOutermost()->GetName(), *GetNameSafe(World->GetAuthGameMode()),
        *Pawn->GetClass()->GetPathName(), DDSOriginalAssemblyStage::VehicleClass, *PreviewActor->GetPathName(), MeshCount,
        *VisualBounds.Min.ToString(), *VisualBounds.Max.ToString(), *CameraLocation.ToString(), *GetNameSafe(PC->GetViewTarget()), DDSOriginalAssemblyStage::Label);
    SetPendingReason(EDDSOriginalStagePendingReason::Ready);
    OriginalPawn = Pawn;
    OriginalCameraManager = PC->PlayerCameraManager ? Cast<ACitySamplePlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
    BeginScreenshot();
    BeginRecording();
    return true;
}

void UDDSOriginalAssemblyStageSubsystem::ConfigureScreenshot()
{
    const bool bHasPath = FParse::Value(FCommandLine::Get(), TEXT("DDSOriginalAssemblyScreenshot="), ScreenshotPath);
    const bool bHasNonce = FParse::Value(FCommandLine::Get(), TEXT("DDSOriginalAssemblyScreenshotNonce="), ScreenshotNonce);
    if (!bHasPath && !bHasNonce) return; // The preview remains usable with no capture opt-in.
    const auto Invalid = [this](const TCHAR* Reason)
    {
        bScreenshotOptIn = false;
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("SCREENSHOT_REFUSED configuration=%s; no screenshot request or output."), Reason);
    };
    if (!bHasPath || !bHasNonce || ScreenshotNonce.Len() != 32)
    {
        Invalid(TEXT("Exact root absolute path and 32-hex nonce both required"));
        return;
    }
    for (TCHAR C : ScreenshotNonce) if (!FChar::IsHexDigit(C))
    {
        Invalid(TEXT("Nonce is not 32 hex characters"));
        return;
    }
    FPaths::NormalizeFilename(ScreenshotPath);
    const FString Root(TEXT("E:/Projects/DDS City/ddscity/Evidence/LocalDevelopment/"));
    const FString ExpectedName = TEXT("engine-owned-first-frame-") + ScreenshotNonce + TEXT(".png");
    if (FPaths::IsRelative(ScreenshotPath) || !ScreenshotPath.StartsWith(Root, ESearchCase::CaseSensitive)
        || ScreenshotPath.Contains(TEXT("/../")) || ScreenshotPath.Contains(TEXT("/./"))
        || ScreenshotPath.Contains(TEXT("//")) || !FPaths::ValidatePath(ScreenshotPath)
        || FPaths::GetCleanFilename(ScreenshotPath) != ExpectedName)
    {
        Invalid(TEXT("Exact root run-child/engine-owned-first-frame-NONCE.png path required"));
        return;
    }
    const FString ChildAndName = ScreenshotPath.Mid(Root.Len());
    FString RunChild, FileName;
    if (!ChildAndName.Split(TEXT("/"), &RunChild, &FileName) || FileName != ExpectedName
        || RunChild.Len() < 8 || RunChild.Len() > 80)
    {
        Invalid(TEXT("One bounded literal existing log child required"));
        return;
    }
    for (TCHAR C : RunChild)
    {
        const bool bAscii = (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z'))
            || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('-');
        if (!bAscii) { Invalid(TEXT("Nonliteral log child")); return; }
    }
    if (!IFileManager::Get().DirectoryExists(*FPaths::GetPath(ScreenshotPath))
        || IFileManager::Get().FileExists(*ScreenshotPath) || IFileManager::Get().DirectoryExists(*ScreenshotPath))
    {
        Invalid(TEXT("Parent must exist and exact output must be absent"));
        return;
    }
    bScreenshotOptIn = true;
}

bool UDDSOriginalAssemblyStageSubsystem::ScreenshotContextLive() const
{
    const UWorld* World = GetWorld();
    const APlayerController* PC = PreviewController.Get();
    const APawn* Pawn = OriginalPawn.Get();
    const ACitySamplePlayerCameraManager* Camera = OriginalCameraManager.Get();
    const UGameViewportClient* ViewportClient = ScreenshotViewport.Get();
    return IsInGameThread() && World && World->WorldType == EWorldType::Game && World->HasBegunPlay()
        && World->PersistentLevel && World->PersistentLevel->GetOutermost()->GetName() == DDSOriginalAssemblyStage::MapPackage
        && FParse::Param(FCommandLine::Get(), TEXT("DDSOriginalAssemblyStage"))
        && World->GetAuthGameMode() && World->GetAuthGameMode()->GetClass()->GetPathName() == DDSOriginalAssemblyStage::OriginalMode
        && PC && Pawn && Camera && PC == World->GetFirstPlayerController() && PC->IsLocalController()
        && PC->GetWorld() == World && Pawn->GetWorld() == World && Camera->GetWorld() == World
        && PC->HasActorBegunPlay() && Pawn->HasActorBegunPlay() && PC->GetPawn() == Pawn && Pawn->GetController() == PC
        && PC->PlayerCameraManager == Camera && !Camera->IsUsingAlternateCamera()
        && IsValid(PreviewActor) && PreviewActor->GetWorld() == World
        && PreviewActor->GetClass()->GetPathName() == DDSOriginalAssemblyStage::VehicleClass
        && IsValid(PreviewCamera) && PreviewCamera->GetWorld() == World && PC->GetViewTarget() == PreviewCamera
        && ViewportClient && GEngine && GEngine->GameViewport == ViewportClient
        && World->GetGameViewport() == ViewportClient && ViewportClient->GetWorld() == World && ViewportClient->Viewport
        && ViewportClient->Viewport == ScreenshotRenderViewport
        && ViewportClient->Viewport->GetSizeXY() == FIntPoint(1920, 1080)
        && ViewportClient->Viewport->GetRenderTargetTextureSizeXY() == FIntPoint(1920, 1080)
        && !ViewportClient->Viewport->GetSceneHDREnabled(); // This one PNG is explicitly LDR; no implicit EXR conversion.
}

bool UDDSOriginalAssemblyStageSubsystem::ScreenshotBudgetLive() const
{
    const double Elapsed = FPlatformTime::Seconds() - ScreenshotStartedSeconds;
    return Elapsed >= 0.0 && Elapsed < DDSOriginalAssemblyStage::ScreenshotWaitSeconds;
}

void UDDSOriginalAssemblyStageSubsystem::BeginScreenshot()
{
    if (!bScreenshotOptIn) return;
    ScreenshotStartedSeconds = FPlatformTime::Seconds();
    bScreenshotPending = true;
    ScreenshotViewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    ScreenshotRenderViewport = ScreenshotViewport.IsValid() ? ScreenshotViewport->Viewport : nullptr;
    if (!ScreenshotBudgetLive() || !ScreenshotContextLive())
    {
        FinishScreenshot(TEXT("Owned original-world/controller/camera/LDR1920x1080 viewport context unavailable after stage acknowledgement"));
        return;
    }
    EndDrawHandle = ScreenshotViewport->OnEndDraw().AddUObject(this, &UDDSOriginalAssemblyStageSubsystem::OnOwnedViewportEndDraw);
    ViewportRenderedHandle = UGameViewportClient::OnViewportRendered().AddUObject(this, &UDDSOriginalAssemblyStageSubsystem::OnOwnedViewportRendered);
    UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("SCREENSHOT_PENDING nonce=%s path=%s; require three owned viewport draws after stage acknowledgement; wait<=60s; one RHI frame, SlateUI excluded, driving disabled."),
        *ScreenshotNonce, *ScreenshotPath);
}

void UDDSOriginalAssemblyStageSubsystem::OnOwnedViewportEndDraw()
{
    if (!bScreenshotPending || bReadbackInProgress) return;
    if (!ScreenshotBudgetLive()) { FinishScreenshot(TEXT("Owned end-draw callback arrived after60sec deadline")); return; }
    if (!ScreenshotContextLive()) { FinishScreenshot(TEXT("Owned context changed during post-stage render draws")); return; }
    OwnedDrawsAfterStage = FMath::Min(OwnedDrawsAfterStage + 1, DDSOriginalAssemblyStage::ScreenshotDraws);
}

void UDDSOriginalAssemblyStageSubsystem::TickScreenshot()
{
    if (!ScreenshotBudgetLive())
    {
        FinishScreenshot(TEXT("Bounded60sec owned render/screenshot callback wait elapsed; no frame pass"));
        return;
    }
    if (!ScreenshotContextLive()) { FinishScreenshot(TEXT("Owned original context changed before screenshot processing")); return; }
    // Readback starts only from the sender-bearing OnViewportRendered callback, never from Tick or a global screenshot request.
}

void UDDSOriginalAssemblyStageSubsystem::OnOwnedViewportRendered(FViewport* RenderedViewport)
{
    if (!bScreenshotPending || bReadbackInProgress || bScreenshotWritten) return;
    // This global event has an actual producer. An unrelated viewport can never authorize readback or output.
    if (!RenderedViewport || RenderedViewport != ScreenshotRenderViewport) return;
    if (!ScreenshotBudgetLive()) { FinishScreenshot(TEXT("Owned render callback arrived after60sec deadline")); return; }
    if (!ScreenshotContextLive()) { FinishScreenshot(TEXT("Actual rendered sender no longer has the exact owned original context")); return; }
    if (OwnedDrawsAfterStage < DDSOriginalAssemblyStage::ScreenshotDraws) return;
    if (GIsHighResScreenshot || GIsDumpingMovie || FScreenshotRequest::IsScreenshotRequested()
        || IFileManager::Get().FileExists(*ScreenshotPath) || IFileManager::Get().DirectoryExists(*ScreenshotPath))
    {
        FinishScreenshot(TEXT("Existing movie/screenshot/output state conflicts; shared state is not replaced or reset"));
        return;
    }
    if (!ScreenshotBudgetLive() || !ScreenshotContextLive())
    {
        FinishScreenshot(TEXT("Deadline or owned context expired before RHI readback"));
        return;
    }
    bReadbackInProgress = true; // Reentrant events cannot consume another frame.
    TArray<FColor> Pixels;
    // UE5.3 broadcasts this sender after Canvas.Flush_GameThread and before ProcessScreenShots(this).
    // Read only that verified render target; do not request Slate/window/desktop pixels or mutate screenshot globals.
    const bool bReadback = GetViewportScreenShot(RenderedViewport, Pixels, FIntRect(0, 0, 1920, 1080));
    if (!ScreenshotBudgetLive() || !ScreenshotContextLive() || RenderedViewport != ScreenshotRenderViewport
        || !bReadback || Pixels.Num() != 1920 * 1080)
    {
        FinishScreenshot(TEXT("Owned RHI readback/context/pixel count failed or returned after deadline; no output"));
        return;
    }
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    if (!ScreenshotBudgetLive()) { FinishScreenshot(TEXT("Deadline elapsed before PNG compression")); return; }
    TArray64<uint8> Png;
    const TArrayView64<const FColor> PixelView(Pixels.GetData(), static_cast<int64>(Pixels.Num()));
    FImageUtils::PNGCompressImageArray(1920, 1080, PixelView, Png);
    if (!ScreenshotBudgetLive() || !ScreenshotContextLive()
        || Png.Num() <= 0 || Png.Num() > DDSOriginalAssemblyStage::MaxScreenshotBytes)
    {
        FinishScreenshot(TEXT("PNG compression/bounded16MiB/context verification failed or completed after deadline; no output"));
        return;
    }
    if (!ScreenshotBudgetLive() || !ScreenshotContextLive())
    {
        FinishScreenshot(TEXT("Deadline or owned context expired before exclusive PNG creation"));
        return;
    }
    TUniquePtr<FArchive> Output(IFileManager::Get().CreateFileWriter(*ScreenshotPath, FILEWRITE_NoReplaceExisting | FILEWRITE_Silent));
    if (!Output) { FinishScreenshot(TEXT("Exact nonce-bound output could not be created without overwrite")); return; }
    bool bWriteWithinBudget = ScreenshotBudgetLive() && ScreenshotContextLive();
    for (int64 Offset = 0; bWriteWithinBudget && Offset < Png.Num(); Offset += 65536)
    {
        bWriteWithinBudget = ScreenshotBudgetLive() && ScreenshotContextLive();
        if (!bWriteWithinBudget) break;
        Output->Serialize(Png.GetData() + Offset, FMath::Min<int64>(65536, Png.Num() - Offset));
        bWriteWithinBudget = !Output->IsError() && ScreenshotBudgetLive() && ScreenshotContextLive();
    }
    if (bWriteWithinBudget && ScreenshotBudgetLive()) Output->Flush();
    bWriteWithinBudget = bWriteWithinBudget && ScreenshotBudgetLive() && ScreenshotContextLive();
    const bool bWriteError = Output->IsError();
    const bool bClosed = Output->Close(); // Cleanup may finish late; it never grants a successful output.
    Output.Reset();
    if (!bWriteWithinBudget || bWriteError || !bClosed || !ScreenshotBudgetLive() || !ScreenshotContextLive()
        || IFileManager::Get().FileSize(*ScreenshotPath) != Png.Num())
    {
        FinishScreenshot(TEXT("PNG create/write/close/byte-count/context/deadline check failed; partial output preserved, no frame pass"));
        return;
    }
    if (!ScreenshotBudgetLive()) { FinishScreenshot(TEXT("Deadline elapsed before screenshot completion/publication")); return; }
    bScreenshotWritten = true;
    const bool bCompleted = FinishScreenshot(); // Rechecks the same deadline and context before removing owned handles.
    if (!bCompleted) return;
    if (!ScreenshotBudgetLive()) { FinishScreenshot(TEXT("Deadline elapsed before FIRST_GPU_FRAME_WRITTEN publication")); return; }
    UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("FIRST_GPU_FRAME_WRITTEN nonce=%s path=%s width=1920 height=1080 bytes=%lld source=verified-owned-rendered-sender-RHI SlateUI=excluded; original asset assembly preview, driving not enabled; root pixel verdict still REQUIRED."),
        *ScreenshotNonce, *ScreenshotPath, Png.Num());
}

bool UDDSOriginalAssemblyStageSubsystem::FinishScreenshot(const TCHAR* Refusal)
{
    if (!Refusal && bScreenshotPending && (!ScreenshotBudgetLive() || !ScreenshotContextLive()))
    {
        Refusal = TEXT("Screenshot completion exceeded deadline or lost owned context; no frame pass");
    }
    if (Refusal)
    {
        bScreenshotWritten = false;
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("SCREENSHOT_REFUSED nonce=%s reason=%s; no captured-frame acceptance; preserved any partial output."), *ScreenshotNonce, Refusal);
    }
    if (ScreenshotViewport.IsValid() && EndDrawHandle.IsValid()) ScreenshotViewport->OnEndDraw().Remove(EndDrawHandle);
    if (ViewportRenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(ViewportRenderedHandle);
    EndDrawHandle.Reset();
    ViewportRenderedHandle.Reset();
    ScreenshotViewport.Reset();
    ScreenshotRenderViewport = nullptr;
    bScreenshotPending = false;
    bReadbackInProgress = false;
    return Refusal == nullptr;
}

void UDDSOriginalAssemblyStageSubsystem::ConfigureRecording()
{
    const bool bRequested = FParse::Param(FCommandLine::Get(), TEXT("DDSOriginalAssemblyRecord"));
    const bool bNonce = FParse::Value(FCommandLine::Get(), TEXT("DDSOriginalAssemblyRecordNonce="), RecordingNonce);
    const bool bDir = FParse::Value(FCommandLine::Get(), TEXT("RootRecordDir="), RecordingDir);
    FString Mode;
    const bool bMode = FParse::Value(FCommandLine::Get(), TEXT("DDSOriginalAssemblyRecordMode="), Mode);
    if (!bRequested && !bNonce && !bDir && !bMode) return;
    const auto Invalid = [](const TCHAR* Why) {
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("RECORDING_REFUSED configuration=%s; no recording output."), Why);
    };
    if (!bRequested || !bNonce || !bDir || !bMode || Mode != TEXT("RootApprovedRenderer30s") || RecordingNonce.Len() != 32)
    { Invalid(TEXT("Four root-approved recording flags and exact mode/32hex nonce required")); return; }
    for (TCHAR C : RecordingNonce) if (!FChar::IsHexDigit(C))
    { Invalid(TEXT("Recording nonce is not32hex")); return; }
    FPaths::NormalizeFilename(RecordingDir);
    const FString Root(TEXT("E:/Projects/DDS City/ddscity/Evidence/LocalDevelopment/"));
    if (FPaths::IsRelative(RecordingDir) || !RecordingDir.StartsWith(Root, ESearchCase::CaseSensitive)
        || RecordingDir.Contains(TEXT("/../")) || RecordingDir.Contains(TEXT("/./"))
        || RecordingDir.Contains(TEXT("//")) || !FPaths::ValidatePath(RecordingDir))
    { Invalid(TEXT("Exact existing absolute root recording child required")); return; }
    const FString Child = RecordingDir.Mid(Root.Len());
    if (Child.Len() < 8 || Child.Len() > 80)
    { Invalid(TEXT("One bounded literal recording child required")); return; }
    for (TCHAR C : Child)
    {
        const bool bLiteral = (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z'))
            || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('-');
        if (!bLiteral) { Invalid(TEXT("Nonliteral or nested recording child")); return; }
    }
    const FString Manifest = FPaths::Combine(RecordingDir, TEXT("engine-owned-record-") + RecordingNonce + TEXT(".json"));
    if (!IFileManager::Get().DirectoryExists(*RecordingDir) || IFileManager::Get().FileExists(*Manifest)
        || IFileManager::Get().DirectoryExists(*Manifest))
    { Invalid(TEXT("Approved parent must exist and nonce metadata must be absent")); return; }
    // The sole outer driver certifies all ancestors are non-reparse and keeps the
    // same run child under its256MiB guard. This flag is not a pixel verdict.
    bRecordingOptIn = true;
}

bool UDDSOriginalAssemblyStageSubsystem::RecordingContextLive() const
{
    const UWorld* World = GetWorld();
    const APlayerController* PC = PreviewController.Get();
    const APawn* Pawn = OriginalPawn.Get();
    const ACitySamplePlayerCameraManager* Camera = OriginalCameraManager.Get();
    const UGameViewportClient* ViewportClient = RecordingViewport.Get();
    return IsInGameThread() && bRecordingPending && IsValid(World) && !World->bIsTearingDown
        && World->WorldType == EWorldType::Game && World->HasBegunPlay()
        && World->PersistentLevel && World->PersistentLevel->GetOutermost()->GetName() == DDSOriginalAssemblyStage::MapPackage
        && FParse::Param(FCommandLine::Get(), TEXT("DDSOriginalAssemblyStage"))
        && RecordingOriginalMode.IsValid() && World->GetAuthGameMode() == RecordingOriginalMode.Get()
        && World->GetAuthGameMode()->GetClass()->GetPathName() == DDSOriginalAssemblyStage::OriginalMode
        && PC && Pawn && Camera && PC == World->GetFirstPlayerController() && PC->IsLocalController()
        && PC->GetWorld() == World && Pawn->GetWorld() == World && Camera->GetWorld() == World
        && PC->HasActorBegunPlay() && Pawn->HasActorBegunPlay() && PC->GetPawn() == Pawn && Pawn->GetController() == PC
        && PC->PlayerCameraManager == Camera && !Camera->IsUsingAlternateCamera()
        && IsValid(PreviewActor) && PreviewActor->GetWorld() == World
        && PreviewActor->GetClass()->GetPathName() == DDSOriginalAssemblyStage::VehicleClass
        && IsValid(PreviewCamera) && PreviewCamera->GetWorld() == World && PC->GetViewTarget() == PreviewCamera
        && ViewportClient && GEngine && GEngine->GameViewport == ViewportClient
        && World->GetGameViewport() == ViewportClient && ViewportClient->GetWorld() == World && ViewportClient->Viewport
        && ViewportClient->Viewport == RecordingRenderViewport
        && ViewportClient->Viewport->GetSizeXY() == FIntPoint(1920, 1080)
        && ViewportClient->Viewport->GetRenderTargetTextureSizeXY() == FIntPoint(1920, 1080)
        && !ViewportClient->Viewport->GetSceneHDREnabled()
        && !GIsHighResScreenshot && !GIsDumpingMovie && !FScreenshotRequest::IsScreenshotRequested();
}

bool UDDSOriginalAssemblyStageSubsystem::RecordingBudgetLive() const
{
    const double Elapsed = FPlatformTime::Seconds() - RecordingArmedSeconds;
    // Warmup and final synchronous I/O share the20s allowance; the actual
    // recording interval starts only at an owned rendered sender after3draws.
    return FMath::IsFinite(Elapsed) && Elapsed >= 0.0
        && Elapsed < (RecordingStartedSeconds < 0.0 ? DDSOriginalAssemblyStage::RecordingTailSeconds
            : DDSOriginalAssemblyStage::RecordingSeconds + DDSOriginalAssemblyStage::RecordingTailSeconds);
}

void UDDSOriginalAssemblyStageSubsystem::BeginRecording()
{
    if (!bRecordingOptIn) return;
    RecordingArmedSeconds = FPlatformTime::Seconds(); bRecordingPending = true;
    RecordingViewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    RecordingRenderViewport = RecordingViewport.IsValid() ? RecordingViewport->Viewport : nullptr;
    RecordingOriginalMode = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr;
    RecordingFrames.Reserve(DDSOriginalAssemblyStage::MaxRecordingFrames);
    if (!RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Owned original stage1920x1080 LDR viewport context unavailable")); return; }
    RecordingEndDrawHandle = RecordingViewport->OnEndDraw().AddUObject(this, &UDDSOriginalAssemblyStageSubsystem::OnRecordingViewportEndDraw);
    RecordingRenderedHandle = UGameViewportClient::OnViewportRendered().AddUObject(this, &UDDSOriginalAssemblyStageSubsystem::OnRecordingViewportRendered);
    UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("RECORDING_PENDING nonce=%s dir=%s settle>=10realSeconds+3ownedDraws interval=30realSeconds sample<=30Hz max900JPEGs quality75 frameBudget224MiB singleFile16MiB warmup+tail<=20s; outerRootNoReparse+256MiB contract REQUIRED."), *RecordingNonce, *RecordingDir);
}

void UDDSOriginalAssemblyStageSubsystem::OnRecordingViewportEndDraw()
{
    if (!bRecordingPending || bRecordingFinalizing) return;
    if (!RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Owned recording context/dimensions/deadline changed during draw")); return; }
    if (!bRecordingReadback)
        RecordingOwnedDraws = FMath::Min(RecordingOwnedDraws + 1, DDSOriginalAssemblyStage::ScreenshotDraws);
}

void UDDSOriginalAssemblyStageSubsystem::TickRecording()
{
    if (!bRecordingPending || bRecordingFinalizing) return;
    if (!RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Owned recording context/dimensions/deadline lost")); return; }
    if (!bRecordingReadback && RecordingStartedSeconds >= 0.0
        && FPlatformTime::Seconds() - RecordingStartedSeconds >= DDSOriginalAssemblyStage::RecordingSeconds)
        FinishRecording();
}

bool UDDSOriginalAssemblyStageSubsystem::WriteRecordingBytes(const FString& Path, const TArray64<uint8>& Bytes)
{
    if (!RecordingBudgetLive() || !RecordingContextLive() || Bytes.Num() <= 0
        || Bytes.Num() > DDSOriginalAssemblyStage::MaxScreenshotBytes || FPaths::GetPath(Path) != RecordingDir
        || !IFileManager::Get().DirectoryExists(*RecordingDir)) return false;
    TUniquePtr<FArchive> Output(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_NoReplaceExisting | FILEWRITE_Silent));
    if (!Output) return false;
    bool bWithinBudget = RecordingBudgetLive() && RecordingContextLive();
    for (int64 Offset = 0; bWithinBudget && Offset < Bytes.Num(); Offset += 65536)
    {
        bWithinBudget = RecordingBudgetLive() && RecordingContextLive();
        if (!bWithinBudget) break;
        Output->Serialize(const_cast<uint8*>(Bytes.GetData()) + Offset, FMath::Min<int64>(65536, Bytes.Num() - Offset));
        bWithinBudget = !Output->IsError() && RecordingBudgetLive() && RecordingContextLive();
    }
    if (bWithinBudget && RecordingBudgetLive()) Output->Flush();
    bWithinBudget = bWithinBudget && RecordingBudgetLive() && RecordingContextLive();
    const bool bError = Output->IsError(); const bool bClosed = Output->Close(); Output.Reset();
    return bWithinBudget && !bError && bClosed && RecordingBudgetLive() && RecordingContextLive()
        && IFileManager::Get().FileSize(*Path) == Bytes.Num();
}

void UDDSOriginalAssemblyStageSubsystem::OnRecordingViewportRendered(FViewport* RenderedViewport)
{
    if (!bRecordingPending || bRecordingFinalizing || !RenderedViewport || RenderedViewport != RecordingRenderViewport) return;
    const double Now = FPlatformTime::Seconds();
    if (!RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Actual rendered recording sender lost owned context/dimensions/deadline")); return; }
    if (RecordingOwnedDraws < DDSOriginalAssemblyStage::ScreenshotDraws) return;
    if (RecordingStartedSeconds < 0.0 && Now - RecordingArmedSeconds < DDSOriginalAssemblyStage::RecordingSettlingSeconds) return;
    if (RecordingStartedSeconds < 0.0 && (bRecordingReadback || bReadbackInProgress)) return;
    if (RecordingStartedSeconds < 0.0)
    {
        RecordingStartedSeconds = Now; RecordingUtcStart = FDateTime::UtcNow().ToIso8601();
        UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("RECORDING_STARTED nonce=%s monotonic=%.9f realWindow=30seconds; rendered-callback rate and sampled-frame rate will be reported separately."), *RecordingNonce, Now);
    }
    const double Elapsed = Now - RecordingStartedSeconds;
    if (!FMath::IsFinite(Elapsed) || Elapsed < 0.0 || (LastRecordingRenderSeconds >= 0.0 && Now < LastRecordingRenderSeconds))
    { FinishRecording(TEXT("Nonmonotonic owned renderer timestamp")); return; }
    if (Elapsed >= DDSOriginalAssemblyStage::RecordingSeconds)
    { if (!bRecordingReadback) FinishRecording(); return; }
    ++RecordingRenderedCallbacks; LastRecordingRenderSeconds = Now;
    if (bRecordingReadback || bReadbackInProgress || RecordingFrames.Num() >= DDSOriginalAssemblyStage::MaxRecordingFrames
        || (LastRecordingSampleSeconds >= 0.0 && Now - LastRecordingSampleSeconds < DDSOriginalAssemblyStage::RecordingSampleInterval)) return;
    if (GIsHighResScreenshot || GIsDumpingMovie || FScreenshotRequest::IsScreenshotRequested())
    { FinishRecording(TEXT("Existing screenshot/movie request conflicts with owned renderer recording")); return; }
    bRecordingReadback = true;
    FRecordingFrame Frame; Frame.RenderCallbackSeconds = Now; Frame.RenderCallbackIndex = RecordingRenderedCallbacks;
    Frame.GameFrameCounter = GFrameCounter; Frame.ReadbackStartedSeconds = FPlatformTime::Seconds();
    TArray<FColor> Pixels;
    const bool bRead = GetViewportScreenShot(RenderedViewport, Pixels, FIntRect(0, 0, 1920, 1080));
    Frame.ReadbackFinishedSeconds = FPlatformTime::Seconds();
    if (!bRead || Pixels.Num() != 1920 * 1080 || !RecordingBudgetLive() || !RecordingContextLive()
        || RenderedViewport != RecordingRenderViewport)
    { FinishRecording(TEXT("Owned rendered frame readback/pixel count/context/deadline failed")); return; }
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    TArray64<uint8> Jpeg;
    // This FColor constructor is inline BGRA8/sRGB in installed ImageCore.h.
    // CompressImage itself is an Engine export; no direct ImageCore export is called.
    const FImageView Image(Pixels.GetData(), 1920, 1080, EGammaSpace::sRGB);
    Frame.EncodeStartedSeconds = FPlatformTime::Seconds();
    if (!RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Owned recording context/deadline changed before JPEG encoding")); return; }
    const bool bEncoded = FImageUtils::CompressImage(Jpeg, TEXT("jpg"), Image, 75);
    Frame.EncodeFinishedSeconds = FPlatformTime::Seconds();
    if (!bEncoded || Jpeg.Num() <= 0 || Jpeg.Num() > DDSOriginalAssemblyStage::MaxScreenshotBytes
        || RecordingFrameBytes > DDSOriginalAssemblyStage::MaxRecordingFrameBytes - Jpeg.Num()
        || !RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("JPEG compression/context/deadline/single16MiB/total224MiB budget failed")); return; }
    Frame.FileName = FString::Printf(TEXT("engine-owned-record-%s-frame-%06d.jpg"), *RecordingNonce, RecordingFrames.Num() + 1);
    const FString FramePath = FPaths::Combine(RecordingDir, Frame.FileName);
    Frame.Bytes = Jpeg.Num(); RecordingFrameBytes += Frame.Bytes; // Reserve even if an exclusive write later leaves partial evidence.
    Frame.WriteStartedSeconds = FPlatformTime::Seconds();
    const bool bWritten = WriteRecordingBytes(FramePath, Jpeg);
    Frame.WriteFinishedSeconds = FPlatformTime::Seconds();
    if (!bWritten || !RecordingBudgetLive() || !RecordingContextLive())
    { FinishRecording(TEXT("Exclusive JPEG create/write/close/byte-count/context/deadline failed; partial evidence retained")); return; }
    RecordingFrames.Add(MoveTemp(Frame)); LastRecordingSampleSeconds = Now; bRecordingReadback = false;
    if (FPlatformTime::Seconds() - RecordingStartedSeconds >= DDSOriginalAssemblyStage::RecordingSeconds) FinishRecording();
}

bool UDDSOriginalAssemblyStageSubsystem::FinishRecording(const TCHAR* Refusal)
{
    if (!bRecordingPending) return !Refusal && bRecordingWritten;
    if (bRecordingFinalizing && !Refusal) return false;
    if (!Refusal)
    {
        bRecordingFinalizing = true;
        const double CompletedAt = FPlatformTime::Seconds();
        if (!RecordingBudgetLive() || !RecordingContextLive() || bRecordingReadback || RecordingStartedSeconds < 0.0
            || CompletedAt - RecordingStartedSeconds < DDSOriginalAssemblyStage::RecordingSeconds
            || RecordingFrames.Num() < 2 || RecordingRenderedCallbacks < 2)
            Refusal = TEXT("Full30realSeconds with multiple actual owned frames/context/deadline not observed; no successful partial clip");
        else
        {
            auto Metadata = MakeShared<FJsonObject>();
            Metadata->SetStringField(TEXT("schema"), TEXT("DDSCity.OwnedRendererFrames.v1"));
            Metadata->SetStringField(TEXT("status"), TEXT("candidate-complete; matching RENDERER_RECORDING_WRITTEN log required"));
            Metadata->SetBoolField(TEXT("acceptedAsVideo"), false);
            Metadata->SetStringField(TEXT("nonce"), RecordingNonce);
            Metadata->SetStringField(TEXT("mode"), TEXT("RootApprovedRenderer30s"));
            Metadata->SetStringField(TEXT("label"), DDSOriginalAssemblyStage::Label);
            Metadata->SetStringField(TEXT("source"), TEXT("actual owned OnViewportRendered sender; GetViewportScreenShot RHI; LDR BGRA8/sRGB; SlateUI excluded; canvas/HUD may be present"));
            Metadata->SetStringField(TEXT("timestampMeaning"), TEXT("FPlatformTime::Seconds monotonic process clock; renderCallback is game-thread delivery time, not GPU execution time"));
            Metadata->SetStringField(TEXT("outerContract"), TEXT("RootNoReparse ancestors and exact run child under256MiB; prior GPU PNG pixel verdict accepted; no other writer"));
            Metadata->SetStringField(TEXT("startUtc"), RecordingUtcStart);
            Metadata->SetNumberField(TEXT("armedMonotonicSeconds"), RecordingArmedSeconds);
            Metadata->SetNumberField(TEXT("startMonotonicSeconds"), RecordingStartedSeconds);
            Metadata->SetNumberField(TEXT("stableContextSettlingSeconds"), RecordingStartedSeconds - RecordingArmedSeconds);
            Metadata->SetNumberField(TEXT("endMonotonicSeconds"), RecordingStartedSeconds + DDSOriginalAssemblyStage::RecordingSeconds);
            Metadata->SetNumberField(TEXT("realCaptureWindowSeconds"), DDSOriginalAssemblyStage::RecordingSeconds);
            Metadata->SetNumberField(TEXT("completionElapsedSeconds"), CompletedAt - RecordingStartedSeconds);
            Metadata->SetNumberField(TEXT("ownedRenderedCallbacks"), double(RecordingRenderedCallbacks));
            Metadata->SetNumberField(TEXT("observedRenderedCallbackFps"), double(RecordingRenderedCallbacks) / DDSOriginalAssemblyStage::RecordingSeconds);
            Metadata->SetNumberField(TEXT("sampledFrames"), RecordingFrames.Num());
            Metadata->SetNumberField(TEXT("observedCapturedSampleHz"), double(RecordingFrames.Num()) / DDSOriginalAssemblyStage::RecordingSeconds);
            Metadata->SetNumberField(TEXT("lastRenderedOffsetSeconds"), LastRecordingRenderSeconds - RecordingStartedSeconds);
            Metadata->SetNumberField(TEXT("lastSampleOffsetSeconds"), LastRecordingSampleSeconds - RecordingStartedSeconds);
            Metadata->SetNumberField(TEXT("width"), 1920); Metadata->SetNumberField(TEXT("height"), 1080);
            Metadata->SetNumberField(TEXT("jpegQuality"), 75); Metadata->SetNumberField(TEXT("frameBytes"), double(RecordingFrameBytes));
            Metadata->SetNumberField(TEXT("intendedOfflineCfr"), 30);
            Metadata->SetStringField(TEXT("offlineVideo"), TEXT("not produced here; timestamp durations then offline CFR30 may duplicate actual temporal samples; no native30FPS claim"));
            Metadata->SetStringField(TEXT("audio"), TEXT("not captured; offline output must be silent"));
            Metadata->SetStringField(TEXT("world"), GetWorld()->GetPathName());
            Metadata->SetStringField(TEXT("originalPawn"), OriginalPawn->GetPathName());
            Metadata->SetStringField(TEXT("originalController"), PreviewController->GetPathName());
            Metadata->SetStringField(TEXT("originalCameraManager"), OriginalCameraManager->GetPathName());
            Metadata->SetStringField(TEXT("previewCamera"), PreviewCamera->GetPathName());
            TArray<TSharedPtr<FJsonValue>> Rows;
            for (const FRecordingFrame& Frame : RecordingFrames)
            {
                auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("file"), Frame.FileName); Row->SetNumberField(TEXT("bytes"), double(Frame.Bytes));
                Row->SetNumberField(TEXT("renderCallbackIndex"), double(Frame.RenderCallbackIndex));
                Row->SetNumberField(TEXT("gameFrameCounter"), double(Frame.GameFrameCounter));
                Row->SetNumberField(TEXT("renderCallbackSeconds"), Frame.RenderCallbackSeconds);
                Row->SetNumberField(TEXT("offsetSeconds"), Frame.RenderCallbackSeconds - RecordingStartedSeconds);
                Row->SetNumberField(TEXT("readbackStartedSeconds"), Frame.ReadbackStartedSeconds);
                Row->SetNumberField(TEXT("readbackFinishedSeconds"), Frame.ReadbackFinishedSeconds);
                Row->SetNumberField(TEXT("encodeStartedSeconds"), Frame.EncodeStartedSeconds);
                Row->SetNumberField(TEXT("encodeFinishedSeconds"), Frame.EncodeFinishedSeconds);
                Row->SetNumberField(TEXT("writeStartedSeconds"), Frame.WriteStartedSeconds);
                Row->SetNumberField(TEXT("writeFinishedSeconds"), Frame.WriteFinishedSeconds);
                Rows.Add(MakeShared<FJsonValueObject>(Row));
            }
            Metadata->SetArrayField(TEXT("frames"), Rows);
            FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
            const bool bSerialized = FJsonSerializer::Serialize(Metadata, Writer);
            FTCHARToUTF8 Utf8(*Text); TArray64<uint8> Bytes;
            if (bSerialized && Utf8.Length() > 0 && Utf8.Length() <= DDSOriginalAssemblyStage::MaxRecordingMetadataBytes)
                Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            const FString Path = FPaths::Combine(RecordingDir, TEXT("engine-owned-record-") + RecordingNonce + TEXT(".json"));
            if (!bSerialized || Bytes.Num() <= 0 || !WriteRecordingBytes(Path, Bytes)
                || !RecordingBudgetLive() || !RecordingContextLive())
                Refusal = TEXT("Bounded exclusive timestamp metadata/context/deadline write failed; candidate metadata alone is never acceptance");
        }
    }
    if (RecordingViewport.IsValid() && RecordingEndDrawHandle.IsValid()) RecordingViewport->OnEndDraw().Remove(RecordingEndDrawHandle);
    if (RecordingRenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(RecordingRenderedHandle);
    RecordingEndDrawHandle.Reset(); RecordingRenderedHandle.Reset();
    if (!Refusal && (!RecordingBudgetLive() || !RecordingContextLive()))
        Refusal = TEXT("Owned recording completion/context/deadline expired before publication");
    bRecordingWritten = Refusal == nullptr; bRecordingPending = false; bRecordingReadback = false; bRecordingFinalizing = false;
    RecordingViewport.Reset(); RecordingRenderViewport = nullptr; RecordingOriginalMode.Reset();
    if (Refusal)
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Warning, TEXT("RECORDING_REFUSED nonce=%s reason=%s samples=%d; preserved partial/candidate files, no successful recording or video acceptance."), *RecordingNonce, Refusal, RecordingFrames.Num());
    }
    else
    {
        UE_LOG(LogDDSOriginalAssemblyStage, Display, TEXT("RENDERER_RECORDING_WRITTEN nonce=%s dir=%s realWindow=30seconds samples=%d renderedCallbacks=%lld callbackFPS=%.3f captureHz=%.3f frameBytes=%lld; actual timestamps retained; offline CFR30/silent encoding and root video verdict still REQUIRED, driving not enabled."),
            *RecordingNonce, *RecordingDir, RecordingFrames.Num(), RecordingRenderedCallbacks,
            double(RecordingRenderedCallbacks) / DDSOriginalAssemblyStage::RecordingSeconds,
            double(RecordingFrames.Num()) / DDSOriginalAssemblyStage::RecordingSeconds, RecordingFrameBytes);
    }
    return Refusal == nullptr;
}

void UDDSOriginalAssemblyStageSubsystem::ReleaseOwnedPreview()
{
    if (PreviewController.IsValid() && IsValid(PreviewCamera) && PreviewController->GetViewTarget() == PreviewCamera)
    {
        if (PreviousViewTarget.IsValid()
            && PreviousViewTarget->GetWorld() == PreviewController->GetWorld())
            PreviewController->SetViewTarget(PreviousViewTarget.Get());
    }
    if (IsValid(PreviewCamera)) PreviewCamera->Destroy();
    if (IsValid(PreviewActor)) PreviewActor->Destroy();
    PreviewCamera = nullptr;
    PreviewActor = nullptr;
    PreviousViewTarget.Reset();
    PreviewController.Reset();
}

void UDDSOriginalAssemblyStageSubsystem::Deinitialize()
{
    bFinished = true;
    FinishRecording(bRecordingPending ? TEXT("Owned world deinitialized before recording completion") : nullptr);
    FinishScreenshot(bScreenshotPending ? TEXT("Owned world deinitialized before screenshot completion") : nullptr);
    ReleaseOwnedPreview();
    if (GEngine) GEngine->RemoveOnScreenDebugMessage(reinterpret_cast<uint64>(this));
    Super::Deinitialize();
}
