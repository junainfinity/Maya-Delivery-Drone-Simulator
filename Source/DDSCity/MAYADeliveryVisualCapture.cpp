#include "MAYADeliveryVisualCapture.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryTypes.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

namespace MAYAVisualCapture
{
    const TCHAR* Names[] = { TEXT("front"), TEXT("rear"), TEXT("left"), TEXT("right"), TEXT("elevated") };
    TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
    {
        return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
    }
    TSharedRef<FJsonObject> Pose(const FTransform& T)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetArrayField(TEXT("positionCm"), Vector(T.GetLocation()));
        const FQuat Q = T.GetRotation();
        O->SetArrayField(TEXT("quaternionXYZW"), { MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
            MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W) });
        return O;
    }
}

AMAYADeliveryVisualCapture::AMAYADeliveryVisualCapture()
{
    PrimaryActorTick.bCanEverTick = !UE_BUILD_SHIPPING;
    PrimaryActorTick.bStartWithTickEnabled = false;
    PrimaryActorTick.bTickEvenWhenPaused = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

bool AMAYADeliveryVisualCapture::StartCapture(AMAYANativeDemoPawn* InRobot, APlayerController* InController, const FString& Label)
{
    if (bStarted || bComplete) return false;
    bStarted = true;
    StartedMonotonicS = FPlatformTime::Seconds();
    CaptureLabel = Label.Left(120);
    CaptureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
#if UE_BUILD_SHIPPING
    FinishCapture(TEXT("shipping_disabled")); return false;
#endif
    FString RequestedOutput;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), RequestedOutput) || RequestedOutput.IsEmpty())
    { FinishCapture(TEXT("explicit_output_directory_required")); return false; }
    OutputDirectory = FPaths::Combine(FPaths::ConvertRelativePathToFull(RequestedOutput), TEXT("visual-qa"), CaptureId);
    if (IFileManager::Get().DirectoryExists(*OutputDirectory) || !IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    { FinishCapture(TEXT("unique_output_directory_unavailable")); return false; }
    ManifestPath = FPaths::Combine(OutputDirectory, TEXT("capture.json"));
    UWorld* World = GetWorld();
    UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
    if (!FApp::CanEverRender() || FParse::Param(FCommandLine::Get(), TEXT("NullRHI")) || !Viewport || !Viewport->Viewport
        || Viewport->Viewport->GetSizeXY().X <= 0 || Viewport->Viewport->GetSizeXY().Y <= 0)
    { FinishCapture(TEXT("rendered_game_viewport_unavailable")); return false; }
    if (!IsValid(InRobot) || !IsValid(InController) || InRobot->GetWorld() != World || InController->GetWorld() != World
        || !InController->IsLocalController() || !IsValid(InController->GetViewTarget()))
    { FinishCapture(TEXT("owned_robot_local_controller_or_prior_view_unavailable")); return false; }
    if (FScreenshotRequest::IsScreenshotRequested() || GIsHighResScreenshot || GIsDumpingMovie
        || UGameViewportClient::OnScreenshotCaptured().IsBound())
    { FinishCapture(TEXT("another_screenshot_or_pixel_delegate_active")); return false; }
    Robot = InRobot; Controller = InController; PriorViewTarget = InController->GetViewTarget();
    PriorViewTargetPath = PriorViewTarget->GetPathName();
    FActorSpawnParameters Parameters; Parameters.Owner = this;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    OwnedCamera = World->SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, Parameters);
    if (!OwnedCamera) { FinishCapture(TEXT("owned_camera_spawn_failed")); return false; }
    OwnedCamera->GetCameraComponent()->SetFieldOfView(50.0f);
    OwnedCamera->GetCameraComponent()->bConstrainAspectRatio = false;
    if (!FollowMeasuredRobot()) { FinishCapture(TEXT("measured_robot_pose_unavailable")); return false; }
    ProcessedHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &AMAYADeliveryVisualCapture::OnScreenshotProcessed);
    InController->SetViewTarget(OwnedCamera);
    bViewTaken = true;
    if (InController->GetViewTarget() != OwnedCamera) { FinishCapture(TEXT("camera_view_target_refused")); return false; }
    if (AHUD* Hud = InController->GetHUD())
    {
        CapturedHud = Hud; bPriorHudVisible = Hud->bShowHUD;
        bHudTaken = true; bHudRestored = false;
        Hud->bShowHUD = false;
    }
    ViewStartedMonotonicS = FPlatformTime::Seconds();
    CaptureStatus = TEXT("capturing");
    SetActorTickEnabled(true);
    return true;
}

bool AMAYADeliveryVisualCapture::FollowMeasuredRobot()
{
    if (!Robot.IsValid() || !OwnedCamera || ViewIndex < 0 || ViewIndex >= UE_ARRAY_COUNT(MAYAVisualCapture::Names)) return false;
    FMAYADeliveryRobotObservation O; Robot->ReadDeliveryState(O);
    if (!O.bKnown || O.ChassisPose.ContainsNaN()) return false;
    const FVector Forward = -O.ChassisPose.GetUnitAxis(EAxis::X); // robot optical front is -X
    const FVector Right = -O.ChassisPose.GetUnitAxis(EAxis::Y);
    const FVector Origin = O.ChassisPose.GetLocation();
    const FVector LookAt = Origin + FVector::UpVector * 30.0;
    FVector Offset;
    switch (ViewIndex)
    {
    case 0: Offset = Forward * 250.0 + FVector::UpVector * 100.0; break;
    case 1: Offset = -Forward * 250.0 + FVector::UpVector * 100.0; break;
    case 2: Offset = -Right * 220.0 + FVector::UpVector * 80.0; break;
    case 3: Offset = Right * 220.0 + FVector::UpVector * 80.0; break;
    default: Offset = Forward * 150.0 + FVector::UpVector * 230.0; break;
    }
    const FVector CameraLocation = Origin + Offset;
    OwnedCamera->SetActorLocationAndRotation(CameraLocation, (LookAt - CameraLocation).Rotation());
    return true;
}

void AMAYADeliveryVisualCapture::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bStarted || bComplete) return;
    const double Now = FPlatformTime::Seconds();
    if (Now - StartedMonotonicS > 45.0) { FinishCapture(TEXT("sequence_45_second_timeout")); return; }
    if (!Controller.IsValid() || !PriorViewTarget.IsValid() || !OwnedCamera || Controller->GetViewTarget() != OwnedCamera)
    { FinishCapture(TEXT("controller_prior_target_or_owned_view_lost")); return; }
    if (!FollowMeasuredRobot()) { FinishCapture(TEXT("measured_robot_pose_lost")); return; }
    if (!PendingFilename.IsEmpty())
    {
        if (FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename() != PendingFilename)
        { FinishCapture(TEXT("screenshot_request_replaced_by_another_caller")); return; }
        if (bScreenshotProcessed)
        {
            int64 Bytes = 0; int32 Width = 0; int32 Height = 0;
            if (!ReadSavedPng(Bytes, Width, Height)) { FinishCapture(TEXT("processed_screenshot_png_missing_or_invalid")); return; }
            PendingView->SetNumberField(TEXT("pngBytes"), static_cast<double>(Bytes));
            PendingView->SetNumberField(TEXT("widthPixels"), Width); PendingView->SetNumberField(TEXT("heightPixels"), Height);
            Views.Add(MakeShared<FJsonValueObject>(PendingView.ToSharedRef()));
            PendingView.Reset(); PendingFilename.Empty(); bScreenshotProcessed = false;
            ++ViewIndex;
            if (ViewIndex == UE_ARRAY_COUNT(MAYAVisualCapture::Names)) { FinishCapture(TEXT("five_pngs_saved"), true); return; }
            SettledTickCount = 0; ViewStartedMonotonicS = Now;
            if (!FollowMeasuredRobot()) { FinishCapture(TEXT("next_measured_robot_pose_unavailable")); return; }
        }
        else if (Now - RequestedMonotonicS > 5.0) FinishCapture(TEXT("screenshot_processing_5_second_timeout"));
        return;
    }
    if (FScreenshotRequest::IsScreenshotRequested() || GIsHighResScreenshot || GIsDumpingMovie
        || UGameViewportClient::OnScreenshotCaptured().IsBound())
    { FinishCapture(TEXT("another_screenshot_started_during_settle")); return; }
    ++SettledTickCount;
    if (Now - ViewStartedMonotonicS < 1.0 || SettledTickCount < 3) return;
    FMAYADeliveryRobotObservation O; Robot->ReadDeliveryState(O);
    PendingFilename = FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%02d-%s.png"), ViewIndex + 1, MAYAVisualCapture::Names[ViewIndex]));
    if (IFileManager::Get().FileExists(*PendingFilename)) { FinishCapture(TEXT("screenshot_destination_already_exists")); return; }
    PendingView = MakeShared<FJsonObject>();
    PendingView->SetStringField(TEXT("view"), MAYAVisualCapture::Names[ViewIndex]);
    PendingView->SetStringField(TEXT("pngPath"), PendingFilename);
    PendingView->SetNumberField(TEXT("requestElapsedMonotonicSeconds"), Now - StartedMonotonicS);
    PendingView->SetNumberField(TEXT("renderSettleSeconds"), Now - ViewStartedMonotonicS);
    PendingView->SetNumberField(TEXT("worldSeconds"), O.ObservationWorldSeconds);
    PendingView->SetObjectField(TEXT("chassisPose"), MAYAVisualCapture::Pose(O.ChassisPose));
    PendingView->SetObjectField(TEXT("ownedCameraPose"), MAYAVisualCapture::Pose(OwnedCamera->GetActorTransform()));
    PendingView->SetArrayField(TEXT("doorAnglesDegrees"), { MakeShared<FJsonValueNumber>(O.DoorAngleDegrees[0]), MakeShared<FJsonValueNumber>(O.DoorAngleDegrees[1]) });
    PendingView->SetBoolField(TEXT("doorsOpen"), O.bDoorsOpen);
    PendingView->SetBoolField(TEXT("doorsClosedLatched"), O.bDoorsClosedLatched);
    PendingView->SetStringField(TEXT("robotFault"), O.Fault);
    if (Controller->PlayerCameraManager)
    {
        PendingView->SetObjectField(TEXT("playerCameraPoseAtRequest"), MAYAVisualCapture::Pose(FTransform(
            Controller->PlayerCameraManager->GetCameraRotation(), Controller->PlayerCameraManager->GetCameraLocation())));
        PendingView->SetNumberField(TEXT("playerCameraFovDegreesAtRequest"), Controller->PlayerCameraManager->GetFOVAngle());
    }
    RequestedMonotonicS = Now; bScreenshotProcessed = false;
    // Existing HUD and engine screen messages remain enabled. This is the real
    // game viewport screenshot pipeline; no scene capture or synthetic image.
    FScreenshotRequest::RequestScreenshot(PendingFilename, true, false, false);
    if (FScreenshotRequest::GetFilename() != PendingFilename)
    {
        // This request was just created by us, before any other caller can run.
        FScreenshotRequest::Reset(); FinishCapture(TEXT("engine_changed_unique_screenshot_path"));
    }
}

void AMAYADeliveryVisualCapture::OnScreenshotProcessed()
{
    if (!bComplete && !PendingFilename.IsEmpty()) bScreenshotProcessed = true;
}

bool AMAYADeliveryVisualCapture::ReadSavedPng(int64& OutBytes, int32& OutWidth, int32& OutHeight) const
{
    OutBytes = IFileManager::Get().FileSize(*PendingFilename);
    if (OutBytes < 24) return false;
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*PendingFilename));
    if (!Reader) return false;
    uint8 Header[24] = {}; Reader->Serialize(Header, sizeof(Header));
    const uint8 Signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (Reader->IsError() || FMemory::Memcmp(Header, Signature, 8) != 0 || FMemory::Memcmp(Header + 12, "IHDR", 4) != 0) return false;
    auto Dimension = [&Header](int32 Offset) -> uint32
    { return (uint32(Header[Offset]) << 24) | (uint32(Header[Offset + 1]) << 16) | (uint32(Header[Offset + 2]) << 8) | uint32(Header[Offset + 3]); };
    const uint32 W = Dimension(16), H = Dimension(20);
    if (W == 0 || H == 0 || W > uint32(MAX_int32) || H > uint32(MAX_int32)) return false;
    OutWidth = static_cast<int32>(W); OutHeight = static_cast<int32>(H); return true;
}

void AMAYADeliveryVisualCapture::FinishCapture(const FString& Reason, bool bAllFilesSaved)
{
    if (bComplete) return;
    bComplete = true; bCaptured = bAllFilesSaved; CaptureStatus = Reason;
    SetActorTickEnabled(false);
    if (ProcessedHandle.IsValid())
    { FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ProcessedHandle); ProcessedHandle.Reset(); }
    // Never cancel a request belonging to another actor.
    if (!PendingFilename.IsEmpty() && FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename() == PendingFilename)
        FScreenshotRequest::Reset();
    if (bViewTaken && Controller.IsValid() && PriorViewTarget.IsValid())
    {
        Controller->SetViewTarget(PriorViewTarget.Get());
        bViewRestored = Controller->GetViewTarget() == PriorViewTarget.Get();
    }
    else bViewRestored = !bViewTaken;
    if (bHudTaken && CapturedHud.IsValid())
    {
        CapturedHud->bShowHUD = bPriorHudVisible;
        bHudRestored = CapturedHud->bShowHUD == bPriorHudVisible;
    }
    else bHudRestored = !bHudTaken;
    if (OwnedCamera) { OwnedCamera->Destroy(); OwnedCamera = nullptr; }
    if (bViewTaken && !bViewRestored) CaptureStatus += TEXT("; exact_prior_view_target_could_not_be_restored");
    if (!bHudRestored) CaptureStatus += TEXT("; prior_hud_visibility_could_not_be_restored");
    WriteManifest();
}

void AMAYADeliveryVisualCapture::WriteManifest()
{
    if (ManifestPath.IsEmpty()) return;
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("schema"), TEXT("MayaRobotics.V01.VisualCapture.v1"));
    O->SetStringField(TEXT("captureId"), CaptureId); O->SetStringField(TEXT("label"), CaptureLabel);
    O->SetStringField(TEXT("finishedUtc"), FDateTime::UtcNow().ToIso8601());
    O->SetStringField(TEXT("captureStatus"), CaptureStatus);
    O->SetStringField(TEXT("visualAcceptance"), TEXT("not_assessed"));
    O->SetStringField(TEXT("inputAcceptance"), TEXT("not_assessed"));
    O->SetBoolField(TEXT("allFivePngsSaved"), bCaptured);
    O->SetBoolField(TEXT("ownedViewTargetTaken"), bViewTaken);
    O->SetBoolField(TEXT("priorViewTargetRestored"), bViewRestored);
    O->SetBoolField(TEXT("hudTemporarilyHidden"), bHudTaken);
    O->SetBoolField(TEXT("priorHudVisible"), bPriorHudVisible);
    O->SetBoolField(TEXT("priorHudVisibilityRestored"), bHudRestored);
    O->SetStringField(TEXT("priorViewTargetPath"), PriorViewTargetPath);
    O->SetNumberField(TEXT("elapsedMonotonicSeconds"), FPlatformTime::Seconds() - StartedMonotonicS);
    O->SetNumberField(TEXT("sequenceTimeoutSeconds"), 45.0);
    O->SetNumberField(TEXT("perScreenshotTimeoutSeconds"), 5.0);
    O->SetNumberField(TEXT("minimumViewSettleSeconds"), 1.0);
    O->SetArrayField(TEXT("views"), Views);
    if (PendingView) O->SetObjectField(TEXT("unfinishedView"), PendingView.ToSharedRef());
    FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(O, Writer)) return;
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*ManifestPath, FILEWRITE_NoReplaceExisting));
    if (!File) return;
    FTCHARToUTF8 Utf8(*Text); File->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length()); File->Flush();
    bManifestWritten = !File->IsError();
}

void AMAYADeliveryVisualCapture::CancelCapture() { FinishCapture(TEXT("caller_cancelled")); }
void AMAYADeliveryVisualCapture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{ FinishCapture(TEXT("world_or_helper_end_play")); Super::EndPlay(EndPlayReason); }
void AMAYADeliveryVisualCapture::BeginDestroy()
{ if (bStarted) FinishCapture(TEXT("helper_destroyed")); Super::BeginDestroy(); }
