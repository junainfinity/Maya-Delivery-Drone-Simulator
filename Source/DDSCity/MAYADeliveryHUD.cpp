#include "MAYADeliveryHUD.h"
#include "MAYAUITheme.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYAPresentationSettings.h"
#include "MAYAPlayerPresentation.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryGameMode.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryParcel.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Font.h"
#include "CanvasItem.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "GameFramework/PlayerController.h"
#if !UE_BUILD_SHIPPING
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

// Explicit native QA only. This session never changes game, input, pause or camera state.
struct FMAYAHUDCaptureSession
{
    FString Directory, Manifest, PendingFilename, Reason, StartReason;
    TArray<TSharedPtr<FJsonValue>> Views;
    TSharedPtr<FJsonObject> PendingView;
    FDelegateHandle ProcessedHandle;
    FIntPoint OriginalSize = FIntPoint::ZeroValue;
    EWindowMode::Type OriginalMode = EWindowMode::Windowed;
    double Initialized = 0, Ready = 0, Started = 0, Resized = 0, Requested = 0, Restoring = 0;
    int32 Index = 0, ResizeIndex = -1, SettledFrames = 0, ReadyFrames = 0;
    bool bProcessed = false, bFinished = false, bSaved = false, bRestored = false, bManifestWritten = false;
};
#endif

namespace MAYAUI
{
    using namespace MAYAHUDGeometry;
    const FLinearColor White=MAYAUITheme::Text, Muted=MAYAUITheme::Muted;
    const FLinearColor Emerald=MAYAUITheme::Accent, Amber=MAYAUITheme::Accent; // Ordinary route/bar selection.
    const FLinearColor Red=MAYAUITheme::DangerRed, ServiceGreen=MAYAUITheme::ServiceGreen;
    const FLinearColor RadarBackground=MAYAUITheme::Background; // The minimap stays solid.
    const FLinearColor Border=MAYAUITheme::Border;
    constexpr float MinimumTextScale = 1.05f; // same engine-font metrics for draw, fit and wrapping
    float LegibleTextScale(float Scale) { return FMath::Max(MinimumTextScale, Scale); }
    constexpr int32 MaxChevrons = 18; // At most three camera visibility traces per chevron.
    float RouteArrowOpacity(double GameplaySeconds)
    {
        // Gentle opacity breathing only: never off, no scale/color jump; pause freezes it.
        return float(.65+.20*FMath::Cos(GameplaySeconds*(2.0*PI/2.4)));
    }

    struct FNavigation : FGuidance
    {
        FPolyline Survey;
        AMAYADeliveryStation* Target = nullptr;
    };

    FNavigation Navigation(AMAYADeliveryDirector& D)
    {
        FNavigation N;
        const auto& S = D.Snapshot(); const auto& R = D.Observation();
        N.Survey.Build(D.GetActiveGuidanceRoute());
        if (ObjectivePhase(S.Phase)) N.Target = D.ObjectiveStation();
        if (N.Target) static_cast<FGuidance&>(N) = Guidance(N.Survey, S, R, D.IsPaused(),
            N.Target->GetServicePoint(), AMAYADeliveryStation::ObjectiveRingRadiusCm);
        if (D.UsesCityBlockGuidance())
        {
            if (!D.IsCityBlockRouteReady()) { N.bMoving=false; N.bGuidance=false; N.CueText=TEXT("Route unavailable"); }
            const auto Advice=D.GetRouteAdvice();
            if (Advice.Kind!=EMAYARouteAdvice::None && (Advice.Kind==EMAYARouteAdvice::CrossingExit
                || Advice.DistanceCm<=N.CueDistanceCm+50))
            {
                N.CuePoint=Advice.Point; N.CueDistanceCm=Advice.DistanceCm; N.CueText=Advice.Text;
            }
        }
        return N;
    }

    FString PhaseLabel(const FMAYADeliverySnapshot& S)
    {
        if (S.bDroneTotalled) return TEXT("DRONE TOTALLED");
        switch (S.Phase)
        {
        case EMAYADeliveryPhase::Available: return TEXT("DELIVERY OFFER");
        case EMAYADeliveryPhase::Accepted: return TEXT("COLLECT");
        case EMAYADeliveryPhase::Loading: return TEXT("COLLECTING");
        case EMAYADeliveryPhase::Carrying: return TEXT("DELIVER");
        case EMAYADeliveryPhase::Unloading: return TEXT("DELIVERING");
        case EMAYADeliveryPhase::Succeeded: return TEXT("DELIVERY COMPLETE");
        default: return TEXT("DELIVERY ENDED");
        }
    }
}

void AMAYADeliveryHUD::OnQAScreenshotProcessed()
{
#if !UE_BUILD_SHIPPING
    if (QACapture.IsValid() && !QACapture->bFinished && !QACapture->PendingFilename.IsEmpty()) QACapture->bProcessed = true;
#endif
}

void AMAYADeliveryHUD::WriteQACaptureManifest()
{
#if !UE_BUILD_SHIPPING
    if (!QACapture.IsValid() || QACapture->bManifestWritten || QACapture->Manifest.IsEmpty()) return;
    auto& Q = *QACapture;
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("schema"), TEXT("MayaRobotics.HUDCapture.v1"));
    O->SetStringField(TEXT("scope"), TEXT("Actual HUD pixels and request-frame telemetry only; no forced phase, input, camera or mechanical state."));
    O->SetStringField(TEXT("finishedUtc"), FDateTime::UtcNow().ToIso8601());
    O->SetStringField(TEXT("status"), Q.Reason);
    O->SetStringField(TEXT("sequenceStartReason"), Q.StartReason);
    O->SetBoolField(TEXT("fourPngsSaved"), Q.bSaved);
    O->SetBoolField(TEXT("originalResolutionAndWindowModeObservedRestored"), Q.bRestored);
    O->SetBoolField(TEXT("complete"), Q.bSaved && Q.bRestored);
    O->SetNumberField(TEXT("originalWidth"), Q.OriginalSize.X); O->SetNumberField(TEXT("originalHeight"), Q.OriginalSize.Y);
    O->SetNumberField(TEXT("originalWindowMode"), int32(Q.OriginalMode));
    O->SetNumberField(TEXT("elapsedMonotonicSeconds"), FPlatformTime::Seconds() - Q.Initialized);
    O->SetArrayField(TEXT("captures"), Q.Views);
    FString Json; const auto Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(O, Writer) || IFileManager::Get().FileExists(*Q.Manifest))
    { UE_LOG(LogTemp, Warning, TEXT("MAYA HUD QA manifest refused: %s"), *Q.Manifest); Q.bManifestWritten = true; return; }
    // A unique directory and no-replace writer protect earlier evidence.
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*Q.Manifest, FILEWRITE_NoReplaceExisting));
    if (File)
    {
        const FTCHARToUTF8 Utf8(*Json); File->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length()); File->Close();
        if (File->IsError()) UE_LOG(LogTemp, Warning, TEXT("MAYA HUD QA manifest write failed: %s"), *Q.Manifest);
    }
    Q.bManifestWritten = true;
    UE_LOG(LogTemp, Display, TEXT("MAYA HUD QA %s; saved=%d restored=%d manifest=%s"), *Q.Reason, Q.bSaved, Q.bRestored, *Q.Manifest);
#endif
}

void AMAYADeliveryHUD::FinishQACapture(const FString& Reason, bool bSaved)
{
#if !UE_BUILD_SHIPPING
    if (!QACapture.IsValid() || QACapture->bFinished) return;
    auto& Q = *QACapture; Q.bFinished = true; Q.bSaved = bSaved; Q.Reason = Reason;
    if (Q.ProcessedHandle.IsValid()) { FScreenshotRequest::OnScreenshotRequestProcessed().Remove(Q.ProcessedHandle); Q.ProcessedHandle.Reset(); }
    if (!Q.PendingFilename.IsEmpty() && FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename() == Q.PendingFilename)
        FScreenshotRequest::Reset(); // Never reset another caller's request.
    UGameViewportClient* V = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    const TSharedPtr<SWindow> Window = V ? V->GetWindow() : nullptr;
    Q.bRestored = Canvas && Canvas->SizeX == Q.OriginalSize.X && Canvas->SizeY == Q.OriginalSize.Y
        && Window.IsValid() && Window->GetWindowMode() == Q.OriginalMode;
    if (Q.bRestored && Q.ResizeIndex < 0) { WriteQACaptureManifest(); return; }
    // Even an unobserved resize may be queued. Always supersede our last request
    // with the original size/mode before claiming restoration.
    Q.bRestored = false;
    if (PlayerOwner && Q.OriginalSize.X > 0 && Q.OriginalSize.Y > 0)
    {
        const TCHAR* Mode = Q.OriginalMode == EWindowMode::Fullscreen ? TEXT("f")
            : Q.OriginalMode == EWindowMode::WindowedFullscreen ? TEXT("wf") : TEXT("w");
        Q.Restoring = FPlatformTime::Seconds();
        PlayerOwner->ConsoleCommand(FString::Printf(TEXT("r.SetRes %dx%d%s"), Q.OriginalSize.X, Q.OriginalSize.Y, Mode), false);
    }
    else { Q.Reason += TEXT("; original_viewport_restore_unavailable"); WriteQACaptureManifest(); }
#endif
}

void AMAYADeliveryHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
#if !UE_BUILD_SHIPPING
    if (QACapture.IsValid())
    {
        FinishQACapture(TEXT("hud_endplay_before_sequence_complete"));
        if (!QACapture->bManifestWritten)
        { QACapture->Reason += TEXT("; restoration_requested_but_not_observed_before_endplay"); WriteQACaptureManifest(); }
    }
#endif
    Super::EndPlay(EndPlayReason);
}

FString AMAYADeliveryHUD::ReadMinimapEvidenceJson() const
{
#if !UE_BUILD_SHIPPING
    auto Json=MakeShared<FJsonObject>(); auto& O=*Json;
    O.SetStringField(TEXT("minimapEvidenceSchema"),TEXT("MayaRobotics.MinimapSurroundings.v1"));
    using namespace MAYAHUDGeometry;
    int32 RadarClassCounts[4] = {0,0,0,0}, RadarStale = 0, RadarSampled = 0;
    for (const auto& Cell : RadarSurfaceCache.Slots) if (Cell.bOccupied)
    { ++RadarSampled; ++RadarClassCounts[int32(Cell.EffectiveSurface(RadarSampleNow))]; RadarStale += Cell.IsStale(RadarSampleNow); }
    O.SetNumberField(TEXT("radarSampledCells"), RadarSampled);
    O.SetNumberField(TEXT("radarTotalCells"), FSurroundingsCache::Capacity);
    O.SetNumberField(TEXT("radarVisibleCells"), RadarVisibleCells);
    O.SetNumberField(TEXT("radarVisibleUnknownCells"), RadarVisibleClassCounts[0]); O.SetNumberField(TEXT("radarVisibleRoadCells"), RadarVisibleClassCounts[1]);
    O.SetNumberField(TEXT("radarVisibleSidewalkCells"), RadarVisibleClassCounts[2]); O.SetNumberField(TEXT("radarVisibleStructureCells"), RadarVisibleClassCounts[3]);
    O.SetStringField(TEXT("radarClassCountsScope"),TEXT("unprefixed class counts are retained cache; visible-prefixed counts are current local view"));
    O.SetNumberField(TEXT("radarCacheCapacity"), FSurroundingsCache::Capacity);
    O.SetNumberField(TEXT("radarWorldCellSizeCm"), MinimapCellCm);
    O.SetNumberField(TEXT("radarUnknownCells"), RadarClassCounts[0]); O.SetNumberField(TEXT("radarRoadCells"), RadarClassCounts[1]);
    O.SetNumberField(TEXT("radarSidewalkCells"), RadarClassCounts[2]); O.SetNumberField(TEXT("radarStructureCells"), RadarClassCounts[3]);
    O.SetNumberField(TEXT("radarStaleRetainedCells"), RadarStale);
    O.SetNumberField(TEXT("radarActualTracesLastFrame"), RadarLastFrameTraces);
    O.SetNumberField(TEXT("radarNewTracesLastFrame"), RadarNewTraces); O.SetNumberField(TEXT("radarRefreshTracesLastFrame"), RadarRefreshTraces);
    O.SetNumberField(TEXT("radarDrawRunsLastFrame"), RadarDrawRuns);
    O.SetBoolField(TEXT("radarNorthUp"),true);
    O.SetStringField(TEXT("radarPolicy"),TEXT("All offers actual-centred40m view;50cm world cells/96x96 retained window;24 actual static traces/frame; unknown1s/known10s refresh, uncertain known retention<=10s; measured mesh classes, merged opaque runs and thin route overlays"));
    O.SetNumberField(TEXT("radarWorldCentreX"), RadarWorldCentre.X);
    O.SetNumberField(TEXT("radarWorldCentreY"), RadarWorldCentre.Y);
    O.SetNumberField(TEXT("radarWorldSpanCm"), RadarWorldSpan);
    TArray<TSharedPtr<FJsonValue>> RadarReceipts;
    for (const FIntPoint& Key : RadarLastSampleKeys)
    {
        const FSurroundingsCell* Cell = RadarSurfaceCache.Find(Key); if (!Cell) continue;
        auto Receipt=MakeShared<FJsonObject>(); const FVector2D WorldCentre=MinimapCellCentre(Key);
        Receipt->SetNumberField(TEXT("worldCellX"),Key.X); Receipt->SetNumberField(TEXT("worldCellY"),Key.Y);
        Receipt->SetNumberField(TEXT("worldCentreX"),WorldCentre.X); Receipt->SetNumberField(TEXT("worldCentreY"),WorldCentre.Y);
        Receipt->SetNumberField(TEXT("lastAttemptMonotonicSeconds"),Cell->LastAttemptSeconds);
        Receipt->SetNumberField(TEXT("effectiveSurfaceEnum"),int32(Cell->EffectiveSurface(RadarSampleNow)));
        Receipt->SetBoolField(TEXT("latestQueryKnown"),Cell->bLastQueryKnown); Receipt->SetBoolField(TEXT("staleRetained"),Cell->IsStale(RadarSampleNow));
        Receipt->SetStringField(TEXT("knownMesh"),Cell->KnownMesh.ToString()); Receipt->SetStringField(TEXT("knownComponent"),Cell->KnownComponent.ToString());
        Receipt->SetStringField(TEXT("latestQueryMesh"),Cell->LastQueryMesh.ToString());
        Receipt->SetNumberField(TEXT("latestQueryWorldZ"),Cell->LastQueryWorldZ); Receipt->SetNumberField(TEXT("latestQueryNormalZ"),Cell->LastQueryNormalZ);
        RadarReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
    }
    O.SetArrayField(TEXT("radarLastFrameSampleReceipts"),RadarReceipts);
    FString Text; auto Writer=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Json,Writer); return Text;
#else
    return TEXT("{}");
#endif
}

void AMAYADeliveryHUD::UpdateQACapture(AMAYADeliveryDirector* D)
{
#if !UE_BUILD_SHIPPING
    const double Now = FPlatformTime::Seconds();
    UGameViewportClient* V = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    const TSharedPtr<SWindow> Window = V ? V->GetWindow() : nullptr;
    if (!bQACaptureChecked)
    {
        bQACaptureChecked = true;
        if (!FParse::Param(FCommandLine::Get(), TEXT("MayaHUDCaptureLayouts"))) return;
        QACapture = MakeShared<FMAYAHUDCaptureSession>(); auto& Q = *QACapture; Q.Initialized = Now;
        Q.OriginalSize = FIntPoint(Canvas->SizeX, Canvas->SizeY);
        if (Window.IsValid()) Q.OriginalMode = Window->GetWindowMode();
        FString Output;
        if (!FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), Output) || Output.IsEmpty())
        { FinishQACapture(TEXT("explicit_MayaV01Output_required")); return; }
        Q.Directory = FPaths::Combine(FPaths::ConvertRelativePathToFull(Output), TEXT("hud-qa"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
        if (IFileManager::Get().DirectoryExists(*Q.Directory) || !IFileManager::Get().MakeDirectory(*Q.Directory, true))
        { FinishQACapture(TEXT("unique_output_directory_unavailable")); return; }
        Q.Manifest = FPaths::Combine(Q.Directory, TEXT("capture.json"));
        if (GetWorld()->WorldType != EWorldType::Game || !PlayerOwner || !PlayerOwner->IsLocalController()
            || !V || !V->Viewport || !Window.IsValid() || FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
        { FinishQACapture(TEXT("rendered_local_game_viewport_required")); return; }
        Q.ProcessedHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &AMAYADeliveryHUD::OnQAScreenshotProcessed);
    }
    if (!QACapture.IsValid()) return;
    auto& Q = *QACapture;
    if (Q.bFinished)
    {
        if (Q.bManifestWritten) return;
        Q.bRestored = Canvas->SizeX == Q.OriginalSize.X && Canvas->SizeY == Q.OriginalSize.Y
            && Window.IsValid() && Window->GetWindowMode() == Q.OriginalMode;
        if ((Q.bRestored && Now - Q.Restoring >= .1) || Now - Q.Restoring > 5.0)
        { if (!Q.bRestored) Q.Reason += TEXT("; original_viewport_restore_5_second_timeout"); WriteQACaptureManifest(); }
        return;
    }
    if (!PlayerOwner || !V || !V->Viewport || !Window.IsValid()) { FinishQACapture(TEXT("game_viewport_lost")); return; }
    if (Q.Started == 0)
    {
        const bool bReady = D && D->Observation().bKnown && D->Observation().bReady
            && !D->Observation().ChassisPose.ContainsNaN() && MAYAHUDGeometry::Finite(D->Observation().VelocityCmps);
        Q.ReadyFrames = bReady ? Q.ReadyFrames + 1 : 0;
        if (bReady && Q.Ready == 0) Q.Ready = Now;
        if (Now - (Q.Ready > 0 ? Q.Ready : Q.Initialized) > 300.0)
        { FinishQACapture(TEXT("no_stable_ready_HUD_within_300_seconds")); return; }
        if (!bReady || Q.ReadyFrames < 3) return;
        const bool bCarrying = D->Snapshot().Phase == EMAYADeliveryPhase::Carrying
            && MAYAHUDGeometry::NavigationAllowed(D->Snapshot(), D->Observation(), D->IsPaused());
        if (!bCarrying && Now - Q.Ready < 5.0) return;
        Q.StartReason = bCarrying ? TEXT("eligible_actual_Carrying") : TEXT("stable_ready_layout_fallback_after_5_seconds_without_eligible_Carrying");
        Q.Started = Now;
    }
    if (Now - Q.Started > 40.0) { FinishQACapture(TEXT("capture_cycle_40_second_timeout")); return; }
    const FIntPoint Sizes[] = { FIntPoint(1280, 720), FIntPoint(1920, 1080), FIntPoint(2560, 1080), FIntPoint(1024, 768) };
    const FIntPoint Target = Sizes[Q.Index];
    if (!Q.PendingFilename.IsEmpty())
    {
        if (FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename() != Q.PendingFilename)
        { FinishQACapture(TEXT("owned_screenshot_request_replaced")); return; }
        if (Q.bProcessed)
        {
            TUniquePtr<FArchive> Png(IFileManager::Get().CreateFileReader(*Q.PendingFilename));
            uint8 Header[24] = {}; const uint8 Signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
            if (!Png || Png->TotalSize() < 24) { FinishQACapture(TEXT("processed_png_missing")); return; }
            Png->Serialize(Header, sizeof(Header));
            auto Dimension = [&Header](int32 Offset) -> uint32
            { return (uint32(Header[Offset]) << 24) | (uint32(Header[Offset + 1]) << 16) | (uint32(Header[Offset + 2]) << 8) | uint32(Header[Offset + 3]); };
            if (Png->IsError() || FMemory::Memcmp(Header, Signature, 8) || FMemory::Memcmp(Header + 12, "IHDR", 4)
                || Dimension(16) != uint32(Target.X) || Dimension(20) != uint32(Target.Y))
            { FinishQACapture(TEXT("processed_png_invalid_or_dimensions_differ")); return; }
            Q.PendingView->SetNumberField(TEXT("pngWidth"), Dimension(16)); Q.PendingView->SetNumberField(TEXT("pngHeight"), Dimension(20));
            Q.PendingView->SetNumberField(TEXT("pngBytes"), double(Png->TotalSize()));
            Q.PendingView->SetBoolField(TEXT("ownedRequestProcessedAndPngValidated"), true);
            Q.Views.Add(MakeShared<FJsonValueObject>(Q.PendingView.ToSharedRef()));
            Q.PendingView.Reset(); Q.PendingFilename.Empty(); Q.bProcessed = false;
            if (++Q.Index == UE_ARRAY_COUNT(Sizes)) FinishQACapture(TEXT("four_native_HUD_pngs_saved"), true);
        }
        else if (Now - Q.Requested > 5.0) FinishQACapture(TEXT("screenshot_processing_5_second_timeout"));
        return;
    }
    if (FScreenshotRequest::IsScreenshotRequested() || GIsHighResScreenshot || GIsDumpingMovie
        || UGameViewportClient::OnScreenshotCaptured().IsBound() || FScreenshotRequest::OnScreenshotCaptured().IsBound())
    { FinishQACapture(TEXT("another_screenshot_owner_active")); return; }
    if (Q.ResizeIndex != Q.Index)
    {
        Q.ResizeIndex = Q.Index; Q.Resized = Now; Q.SettledFrames = 0;
        PlayerOwner->ConsoleCommand(FString::Printf(TEXT("r.SetRes %dx%dw"), Target.X, Target.Y), false); return;
    }
    if (Now - Q.Resized > 8.0) { FinishQACapture(TEXT("actual_viewport_resize_8_second_timeout")); return; }
    if (Canvas->SizeX != Target.X || Canvas->SizeY != Target.Y || Window->GetWindowMode() != EWindowMode::Windowed)
    { Q.SettledFrames = 0; return; }
    if (++Q.SettledFrames < 3 || Now - Q.Resized < .35) return;
    if (!D || !D->Observation().bKnown || !D->Observation().bReady || D->Observation().ChassisPose.ContainsNaN()) return;
    Q.PendingFilename = FPaths::Combine(Q.Directory, FString::Printf(TEXT("%02d-%dx%d.png"), Q.Index + 1, Target.X, Target.Y));
    if (IFileManager::Get().FileExists(*Q.PendingFilename)) { FinishQACapture(TEXT("existing_screenshot_file_refused")); return; }
    const auto& S = D->Snapshot(); const auto& R = D->Observation();
    Q.PendingView = MakeShared<FJsonObject>(); auto& O = *Q.PendingView;
    O.SetStringField(TEXT("pngPath"), Q.PendingFilename); O.SetNumberField(TEXT("canvasWidth"), Canvas->SizeX); O.SetNumberField(TEXT("canvasHeight"), Canvas->SizeY);
    O.SetStringField(TEXT("phase"), MAYAUI::PhaseLabel(S)); O.SetNumberField(TEXT("phaseEnum"), int32(S.Phase));
    O.SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds()); O.SetNumberField(TEXT("observationWorldSeconds"), R.ObservationWorldSeconds);
    if (FMath::IsFinite(S.BatteryPercent)) O.SetNumberField(TEXT("batteryChargePercent"), S.BatteryPercent);
    O.SetBoolField(TEXT("charging"), S.bCharging); O.SetBoolField(TEXT("paused"), D->IsPaused()); O.SetStringField(TEXT("robotFault"), R.Fault);
    O.SetBoolField(TEXT("hudVisible"), bShowHUD); O.SetBoolField(TEXT("known"), R.bKnown); O.SetBoolField(TEXT("ready"), R.bReady);
    const auto N = MAYAUI::Navigation(*D); O.SetBoolField(TEXT("movingChevronsAllowed"), N.bMoving); O.SetBoolField(TEXT("destinationBearingAllowed"), N.bGuidance);
    O.SetStringField(TEXT("routeArrowStylePolicy"),TEXT("Procedural hollow six-sided emerald ribbon, thin mint bevel and restrained line halo; whole-glyph protected-region admission, three visibility traces per world glyph"));
    O.SetNumberField(TEXT("routeArrowOpacityPolicyAtCapture"),MAYAUI::RouteArrowOpacity(GetWorld()->GetTimeSeconds()));
    O.SetNumberField(TEXT("routeArrowOpacityPeriodGameplaySeconds"),2.4);
    TSharedPtr<FJsonObject> RadarEvidence;
    const auto RadarReader=TJsonReaderFactory<TCHAR>::Create(ReadMinimapEvidenceJson());
    if(FJsonSerializer::Deserialize(RadarReader,RadarEvidence) && RadarEvidence.IsValid())
        for(const auto& Field : RadarEvidence->Values) O.SetField(Field.Key,Field.Value);
    O.SetBoolField(TEXT("boostDriveActive"), R.bBoostDriveActive);
    O.SetNumberField(TEXT("actualSecuredParcelCount"), D->GetSecuredParcelCount());
    O.SetNumberField(TEXT("actualSecuredCargoKg"), D->GetSecuredCargoKg());
    O.SetBoolField(TEXT("resultActionReady"), D->CanRequestNext());
    O.SetBoolField(TEXT("offRoute"), N.bOffRoute); O.SetBoolField(TEXT("atArrivalRing"), N.bArrival);
    O.SetStringField(TEXT("objective"), N.Target ? N.Target->DisplayName : TEXT(""));
    auto Vector = [](const FVector& P) -> TArray<TSharedPtr<FJsonValue>>
    { return { MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y), MakeShared<FJsonValueNumber>(P.Z) }; };
    FVector Camera; FRotator Rotation; PlayerOwner->GetPlayerViewPoint(Camera, Rotation);
    if (!MAYAHUDGeometry::Finite(Camera) || Rotation.ContainsNaN()) { FinishQACapture(TEXT("actual_camera_pose_unavailable")); return; }
    O.SetArrayField(TEXT("cameraPositionCm"), Vector(Camera)); O.SetArrayField(TEXT("cameraPitchYawRollDegrees"), Vector(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
    O.SetArrayField(TEXT("chassisPositionCm"), Vector(R.ChassisPose.GetLocation()));
    if (N.Target) O.SetArrayField(TEXT("objectiveServicePointCm"), Vector(N.Target->GetServicePoint()));
    O.SetNumberField(TEXT("sequenceElapsedMonotonicSeconds"), Now - Q.Started); O.SetNumberField(TEXT("settledFrames"), Q.SettledFrames);
    Q.Requested = Now; Q.bProcessed = false;
    FScreenshotRequest::RequestScreenshot(Q.PendingFilename, true, false, false);
    if (FScreenshotRequest::GetFilename() != Q.PendingFilename) FinishQACapture(TEXT("engine_changed_owned_screenshot_path"));
#endif
}

FSlateFontInfo AMAYADeliveryHUD::TextFontInfo(float Scale)
{
    if (!RuntimeUIFont)
    {
        RuntimeUIFont = NewObject<UFont>(this, NAME_None, RF_Transient);
        RuntimeUIFont->FontCacheType = EFontCacheType::Runtime;
        RuntimeUIFont->LegacyFontName = TEXT("Regular");
        RuntimeUIFont->LegacyFontSize = 12;
        RuntimeUIFont->CompositeFont = *FCoreStyle::GetDefaultFont();
    }
    const int32 Size = FMath::Max(FMath::RoundToInt(12.f * TextSizeFactor),
        FMath::RoundToInt(12.f * MAYAUI::LegibleTextScale(Scale) * TextSizeFactor * UIScale));
    return FSlateFontInfo(RuntimeUIFont, Size, TEXT("Regular"));
}

FVector2D AMAYADeliveryHUD::MeasureUIText(const FString& Text, float Scale)
{
    if (!FSlateApplication::IsInitialized()) return FVector2D::ZeroVector;
    return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, TextFontInfo(Scale)) / UIScale;
}

void AMAYADeliveryHUD::UIText(const FString& Text, const FLinearColor& Color, float X, float Y, float Scale)
{
    if (bSuppressUI || UIGroupOpacity<=0 || !Canvas || Text.IsEmpty() || !FSlateApplication::IsInitialized()) return;
    FLinearColor ActualColor = Color;
    if (bHighContrast && Color.A > .99f && Color.GetLuminance() > .1f && Color.GetLuminance() < .9f
        && FMath::Max3(Color.R,Color.G,Color.B)-FMath::Min3(Color.R,Color.G,Color.B) < .32f)
        ActualColor=FLinearColor(.98f,1.f,1.f,Color.A);
    ActualColor.A *= UIGroupOpacity;
    FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(X * UIScale),FMath::RoundToFloat(Y * UIScale)),
        FText::FromString(Text), TextFontInfo(Scale), ActualColor);
    Item.Scale = FVector2D(1,1); // Rasterize at the final size; never stretch a small glyph atlas.
    Canvas->DrawItem(Item);
}

void AMAYADeliveryHUD::UITextFit(const FString& Text,const FLinearColor& Color,float X,float Y,float Width,float Scale)
{
    FString Fitted=Text.Replace(TEXT("\n"),TEXT(" ")).Replace(TEXT("\r"),TEXT(" "));
    if (MeasureUIText(Fitted,Scale).X > Width)
    {
        while (!Fitted.IsEmpty() && MeasureUIText(Fitted+TEXT("..."),Scale).X > Width) Fitted.LeftChopInline(1,false);
        Fitted += TEXT("...");
    }
    UIText(Fitted,Color,X,Y,Scale);
}

void AMAYADeliveryHUD::UITextWrap(const FString& Text,const FLinearColor& Color,float X,float Y,float Width,int32 MaxLines,float Scale)
{
    if (MaxLines <= 0) return;
    TArray<FString> Words; Text.ParseIntoArrayWS(Words);
    FString Line; int32 Row=0;
    const float Step=MeasureUIText(TEXT("Ag"),Scale).Y+4.f/UIScale;
    for (const FString& Word : Words)
    {
        const FString Candidate=Line.IsEmpty()?Word:Line+TEXT(" ")+Word;
        if (!Line.IsEmpty() && MeasureUIText(Candidate,Scale).X>Width && Row<MaxLines-1)
        { UITextFit(Line,Color,X,Y+Step*Row++,Width,Scale); Line=Word; }
        else Line=Candidate;
    }
    if (!Line.IsEmpty()) UITextFit(Line,Color,X,Y+Step*Row,Width,Scale);
}

void AMAYADeliveryHUD::UIRect(const FLinearColor& Color,float X,float Y,float Width,float Height)
{
    if (bSuppressUI || UIGroupOpacity<=0) return;
    const float PX=FMath::RoundToFloat(X*UIScale),PY=FMath::RoundToFloat(Y*UIScale);
    FLinearColor ActualColor=Color; ActualColor.A*=UIGroupOpacity;
    AHUD::DrawRect(ActualColor,PX,PY,FMath::Max(0.f,FMath::RoundToFloat((X+Width)*UIScale)-PX),
        FMath::Max(0.f,FMath::RoundToFloat((Y+Height)*UIScale)-PY));
}
void AMAYADeliveryHUD::UILine(float X1,float Y1,float X2,float Y2,const FLinearColor& Color,float Thickness)
{
    if (bSuppressUI || UIGroupOpacity<=0) return;
    FLinearColor ActualColor=Color; ActualColor.A*=UIGroupOpacity;
    AHUD::DrawLine(FMath::RoundToFloat(X1*UIScale),FMath::RoundToFloat(Y1*UIScale),
        FMath::RoundToFloat(X2*UIScale),FMath::RoundToFloat(Y2*UIScale),ActualColor,
        FMath::Max(1.f,FMath::RoundToFloat(Thickness*UIScale)));
}
void AMAYADeliveryHUD::UIRoundedPanel(float X,float Y,float Width,float Height,float Radius,float FillOpacity)
{
    if (bSuppressUI || UIGroupOpacity<=0 || !Canvas || Width<=0 || Height<=0) return;
    const FTextureResource* Resource=Canvas->DefaultTexture?Canvas->DefaultTexture->GetResource():nullptr;
    const FTexture* Texture=Resource&&!Resource->IsProxy()?Resource:nullptr;
    if (!Texture) return;
    const float Stroke=1.f/UIScale;
    Radius=FMath::Clamp(Radius,Stroke,FMath::Min(Width,Height)*.5f);
    TArray<FVector2D> Outer,Inner;
    const FVector2D Centres[]={FVector2D(X+Width-Radius,Y+Radius),FVector2D(X+Width-Radius,Y+Height-Radius),
        FVector2D(X+Radius,Y+Height-Radius),FVector2D(X+Radius,Y+Radius)};
    for(int32 Corner=0;Corner<4;++Corner) for(int32 Segment=0;Segment<=5;++Segment)
    {
        const double Angle=(-.5+Corner*.5+Segment*.1)*PI;
        const FVector2D Direction(FMath::Cos(Angle),FMath::Sin(Angle));
        Outer.Add((Centres[Corner]+Direction*Radius)*UIScale);
        Inner.Add((Centres[Corner]+Direction*(Radius-Stroke))*UIScale);
    }
    auto Triangle=[](TArray<FCanvasUVTri>& List,const FVector2D& A,const FVector2D& B,const FVector2D& C)
    {
        FCanvasUVTri Tri; Tri.V0_Pos=A;Tri.V1_Pos=B;Tri.V2_Pos=C;
        Tri.V0_UV=Tri.V1_UV=Tri.V2_UV=FVector2D::ZeroVector; List.Add(Tri);
    };
    TArray<FCanvasUVTri> Fill,Edge;
    const FVector2D Centre((X+Width*.5f)*UIScale,(Y+Height*.5f)*UIScale);
    for(int32 I=0;I<Outer.Num();++I)
    {
        const int32 Next=(I+1)%Outer.Num();
        Triangle(Fill,Centre,Inner[I],Inner[Next]);
        Triangle(Edge,Outer[I],Outer[Next],Inner[Next]); Triangle(Edge,Outer[I],Inner[Next],Inner[I]);
    }
    // Separate inner fill and one-physical-pixel edge: alpha is never compounded.
    FCanvasTriangleItem FillItem(Fill,Texture),EdgeItem(Edge,Texture);
    FillItem.SetColor(MAYAUITheme::Alpha(MAYAUITheme::Panel,FillOpacity*UIGroupOpacity));
    EdgeItem.SetColor(MAYAUITheme::Alpha(MAYAUITheme::Border,(bHighContrast?.70f:.50f)*UIGroupOpacity));
    FillItem.BlendMode=EdgeItem.BlendMode=SE_BLEND_Translucent;
    Canvas->DrawItem(FillItem); Canvas->DrawItem(EdgeItem);
}

void AMAYADeliveryHUD::UIPanel(float X,float Y,float Width,float Height,const FLinearColor& Accent,float FillOpacity)
{
    if (Width<=0 || Height<=0) return;
    const float DefaultOpacity=bDrawingPlayerMenus?(bHighContrast?1.f:.94f):(bHighContrast?.70f:.58f);
    const float Alpha=FillOpacity<0?DefaultOpacity:FMath::Clamp(FillOpacity,0.f,1.f);
    if (!bDrawingPlayerMenus) { UIRoundedPanel(X,Y,Width,Height,8.f,Alpha); return; }
    UIRect(MAYAUITheme::Alpha(MAYAUITheme::Panel,Alpha),X,Y,Width,Height);
    const float StrokeAlpha=bDrawingPlayerMenus?1.f:(bHighContrast?.70f:.55f);
    const FLinearColor Edge=MAYAUITheme::Alpha(MAYAUITheme::Border,StrokeAlpha);
    FLinearColor AccentEdge=Accent; AccentEdge.A=StrokeAlpha;
    const float Pixel=1.f/UIScale;
    UIRect(Edge,X,Y,Width,Pixel); UIRect(Edge,X,Y+Height-Pixel,Width,Pixel);
    UIRect(Edge,X,Y,Pixel,Height); UIRect(Edge,X+Width-Pixel,Y,Pixel,Height);
    UIRect(AccentEdge,X,Y,FMath::Min(32.f,Width),2.f/UIScale);
}

void AMAYADeliveryHUD::UIArc(float X, float Y, float Radius, double Fraction, const FLinearColor& Color, float Thickness)
{
    const double End = 2 * PI * FMath::Clamp(Fraction, 0.0, 1.0);
    constexpr int32 Segments = 48;
    for (int32 I = 0; I < Segments; ++I)
    {
        const double A = 2 * PI * I / Segments, B = FMath::Min(End, 2 * PI * (I + 1) / Segments);
        if (A >= End) break;
        UILine(X + Radius * float(FMath::Sin(A)), Y - Radius * float(FMath::Cos(A)),
            X + Radius * float(FMath::Sin(B)), Y - Radius * float(FMath::Cos(B)), Color, Thickness);
    }
}

void AMAYADeliveryHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas || !GetWorld() || Canvas->SizeX <= 0 || Canvas->SizeY <= 0) return;
    using namespace MAYAUI;
    const auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    const auto* Preferences=PC?PC->GetPresentationSettings():nullptr;
    bSuppressUI=false; UIGroupOpacity=1.f;
    FLayout L = Layout(float(Canvas->SizeX), float(Canvas->SizeY), Preferences?Preferences->InterfaceScale:1.f); UIScale = L.Scale;
    TextSizeFactor=Preferences&&Preferences->bLargeText?1.18f:1.f;
    bHighContrast=Preferences&&Preferences->bHighContrast;
    if (!CachedDirector.IsValid() || CachedDirector->GetWorld() != GetWorld())
    {
        CachedDirector.Reset();
        for (TActorIterator<AMAYADeliveryDirector> It(GetWorld()); It; ++It) { CachedDirector = *It; break; }
    }
    AMAYADeliveryDirector* D = CachedDirector.Get();
    if(PC && (PC->GetPlayerScreen()==EMAYAPlayerScreen::MainMenu || PC->GetPlayerScreen()==EMAYAPlayerScreen::StartupError))
    { DrawPlayerMenus(D); UpdateQACapture(D); return; }
    if (PC && (PC->GetPlayerScreen()==EMAYAPlayerScreen::ShiftWin || PC->GetPlayerScreen()==EMAYAPlayerScreen::ShiftLost)) L.Modal.H=284;
    ContextX=L.Modal.X; ContextY=L.Modal.Y; ContextWidth=L.Modal.W; ContextHeight=L.Modal.H;
    FRect ProjectedRobot;
    bool bProjectedRobot=false;
    if (D && D->GetRobot() && PlayerOwner)
    {
        // Editor camera frusta and invisible physics/source proxies are not the
        // displayed robot. Protect the actual visible static-mesh render bounds.
        FBox Bounds(ForceInit);
        TArray<UStaticMeshComponent*> VisibleMeshes;
        D->GetRobot()->GetComponents<UStaticMeshComponent>(VisibleMeshes);
        for (const UStaticMeshComponent* Mesh : VisibleMeshes)
            if (Mesh && Mesh->IsRegistered() && Mesh->IsVisible() && !Mesh->bHiddenInGame && Mesh->GetStaticMesh())
                Bounds += Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox();
        FBox2D ScreenBounds(ForceInit);
        for (int32 I=0; I<8; ++I)
        {
            const FVector Corner((I&1)?Bounds.Max.X:Bounds.Min.X,(I&2)?Bounds.Max.Y:Bounds.Min.Y,(I&4)?Bounds.Max.Z:Bounds.Min.Z);
            FVector2D Screen;
            if (Bounds.IsValid && PlayerOwner->ProjectWorldLocationToScreen(Corner,Screen)) ScreenBounds+=Screen/UIScale;
        }
        if (ScreenBounds.bIsValid)
        {
            const float Pad=12.f/UIScale;
            ProjectedRobot={float(ScreenBounds.Min.X)-Pad,float(ScreenBounds.Min.Y)-Pad,
                float(ScreenBounds.GetSize().X)+2*Pad,float(ScreenBounds.GetSize().Y)+2*Pad};
            bProjectedRobot=true;
            if (L.Modal.Intersects(ProjectedRobot))
            {
                const FRect Right{L.Width-16-L.Modal.W,L.Modal.Y,L.Modal.W,L.Modal.H};
                if (!Right.Intersects(ProjectedRobot)) { ContextX=Right.X; L.Modal.X=Right.X; L.Prompt.X=Right.X; }
            }
        }
    }
    auto PassiveGroup=[&](const FRect& Rect)
    { bSuppressUI=bProjectedRobot && Rect.Intersects(ProjectedRobot); };
    if (!D)
    {
        bSuppressUI=false;
        UIPanel(L.Navigation.X, L.Navigation.Y, L.Navigation.W, 108, Emerald);
        UIText(TEXT("PREPARING YOUR DELIVERY"), White, L.Navigation.X + 16, L.Navigation.Y + 16, 1.f);
        const AMAYADeliveryGameMode* Mode = Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode());
        UITextWrap(Mode ? Mode->GetBootstrapStatus() : TEXT("Preparing the city delivery route..."), Muted,
            L.Navigation.X + 16, L.Navigation.Y + 46, L.Navigation.W - 32, 2, .88f);
        UIPanel(L.Battery.X,L.Battery.Y,L.Battery.W,L.Battery.H+L.Identity.H,Muted);
        UITextFit(TEXT("--:--"),White,L.Battery.X+16,L.Battery.Y+14,L.Battery.W-32,1.6f);
        UITextFit(TEXT("$-- / $100"),White,L.Identity.X+16,L.Identity.Y+14,L.Identity.W-32,1.3f);
        const FVector2D Centre(L.Map.X+L.Map.W*.5f,L.Map.Y+L.Map.W*.5f);
        const float Radius=L.Map.W*.5f-4.f;
        for(int32 Row=-FMath::FloorToInt(Radius);Row<=FMath::FloorToInt(Radius);++Row)
        { const float Half=FMath::Sqrt(FMath::Max(0.f,Radius*Radius-Row*Row)); UIRect(RadarBackground,Centre.X-Half,Centre.Y+Row,2*Half,1); }
        UIArc(Centre.X,Centre.Y,Radius,1,Emerald,1);
        UITextFit(TEXT("Locating..."),White,L.Map.X+20,Centre.Y-8,L.Map.W-40,.9f);
        UpdateQACapture(nullptr);
        return;
    }
    const auto& S = D->Snapshot(); const auto& R = D->Observation();
    const double HintNow=GetWorld()->GetTimeSeconds();
    const FString CurrentHintContext=FString::Printf(TEXT("%s:%d"),*S.JobInstanceId,int32(S.Phase));
    if(HintContext!=CurrentHintContext || HintStartedWorldSeconds<0 || (PC && PC->HasPlayerMenu()))
    { HintContext=CurrentHintContext; HintStartedWorldSeconds=HintNow; }
    // Brief help on entry/resume/stage change: 5.5 seconds solid, 1.5 seconds fade.
    HintOpacity=FMath::Clamp(float((7.0-(HintNow-HintStartedWorldSeconds))/1.5),0.f,1.f);
    const bool bFault = !R.Fault.IsEmpty();
    const bool bPaused = D->IsPaused();
    const bool bTerminal = S.Phase == EMAYADeliveryPhase::Succeeded || S.Phase == EMAYADeliveryPhase::Failed;
    const auto Presentation=D->GetPlayerPresentation();
    const bool bActiveCharge=Presentation.Charge.bInside && (Presentation.Charge.Stage==EMAYAChargeVisitStage::Qualifying || Presentation.Charge.Stage==EMAYAChargeVisitStage::Charging);
    const bool bContextOverridesResult=Presentation.bDroneTotalled || Presentation.bVehicleContactPending || Presentation.bRecoveryPending || !Presentation.bKnown || bFault || S.BatteryPercent<=0 || bActiveCharge;
    if (!bTerminal) PresentedResultId.Reset();
    else if (PresentedResultId.IsEmpty())
    { PresentedResultId=TEXT("CurrentResult"); ResultPresentedWorldSeconds=GetWorld()->GetTimeSeconds(); }
    const bool bTelemetry = R.bKnown && !R.ChassisPose.ContainsNaN() && Finite(R.VelocityCmps);
    const bool bBattery = bTelemetry && FMath::IsFinite(S.BatteryPercent);
    const double Charge = bBattery ? FMath::Clamp(S.BatteryPercent, 0.0, 100.0) : 0;
    const FLinearColor ChargeColor = !bBattery ? Muted : Charge <= 10 ? Red : S.bCharging ? ServiceGreen : Amber;
    const FNavigation N = Navigation(*D);
    const double TimeLeft = FMath::Max(0.0, S.Job.DeadlineSeconds - S.ElapsedSeconds);
    const bool bTime = FMath::IsFinite(TimeLeft) && ObjectivePhase(S.Phase);
    const FRect ClockCash{L.Battery.X,L.Battery.Y,L.Battery.W,L.Battery.H+L.Identity.H};
    PassiveGroup(ClockCash);
    const bool bTimerCashSubmitted=!bSuppressUI;
    UIPanel(ClockCash.X,ClockCash.Y,ClockCash.W,ClockCash.H,Muted);
    const float IconX=ClockCash.X+18,TimerY=ClockCash.Y+23;
    UIArc(IconX,TimerY,7,1,White,1); UILine(IconX,TimerY,IconX,TimerY-4,White,1);
    UILine(IconX,TimerY,IconX+3,TimerY+2,White,1); UIRect(White,IconX-3,TimerY-11,6,2);
    UITextFit(bTime?FString::Printf(TEXT("%02d:%02d"),int32(TimeLeft)/60,int32(TimeLeft)%60):TEXT("--:--"),
        bTime&&TimeLeft<30?Amber:White,ClockCash.X+36,ClockCash.Y+12,ClockCash.W-46,1.7f);
    UIRect(MAYAUITheme::Alpha(Border,.30f),ClockCash.X+14,ClockCash.Y+49,ClockCash.W-28,1.f/UIScale);
    UIArc(IconX,ClockCash.Y+74,6,1,White,1);
    UILine(IconX-5,ClockCash.Y+81,IconX+5,ClockCash.Y+81,White,1);
    UITextFit(FString::Printf(TEXT("$%d / $100"),S.TotalCredits),White,ClockCash.X+36,ClockCash.Y+64,ClockCash.W-46,1.2f);

    const bool bPickupPhase=S.Phase==EMAYADeliveryPhase::Accepted || S.Phase==EMAYADeliveryPhase::Loading;
    const bool bDeliveryPhase=S.Phase==EMAYADeliveryPhase::Carrying || S.Phase==EMAYADeliveryPhase::Unloading;
    const FLinearColor MissionAccent=bFault?Red:S.bCharging?ServiceGreen:bPickupPhase?ServiceGreen:bDeliveryPhase?Red:Amber;
    PassiveGroup(L.Navigation);
    const bool bMissionSubmitted=!bSuppressUI;
    UIPanel(L.Navigation.X,L.Navigation.Y,L.Navigation.W,L.Navigation.H,MissionAccent);
    UIRect(MAYAUITheme::Alpha(MissionAccent,.70f),L.Navigation.X+2,L.Navigation.Y+12,2,L.Navigation.H-24);
    const FString MainTitle=bTerminal?PhaseLabel(S):bFault?TEXT("ROBOT STOPPED"):bPaused?TEXT("DELIVERY PAUSED")
        :S.bCharging?TEXT("CHARGING"):PhaseLabel(S);
    UITextFit(MainTitle,MissionAccent,L.Navigation.X+16,L.Navigation.Y+12,L.Navigation.W-32,1.15f);
    FString TargetText=N.Target?N.Target->DisplayName:S.Job.Title;
    if(S.bCharging) TargetText=D->GetChargingStationName();
    const float PinX=L.Navigation.X+23,PinY=L.Navigation.Y+51;
    UIArc(PinX,PinY-4,5,1,White,1);
    UILine(PinX-4,PinY,PinX,PinY+7,White,1); UILine(PinX,PinY+7,PinX+4,PinY,White,1);
    UITextFit(TargetText,White,L.Navigation.X+40,L.Navigation.Y+42,L.Navigation.W-54,1.3f);
    FString Cue=N.bArrival?TEXT("At service ring"):N.bRoute?FString::Printf(TEXT("%.0f m"),N.bOffRoute?N.DirectM:N.RemainingCm/100.0):TEXT("Choose a delivery");
    if(!N.CueText.IsEmpty() && (D->UsesCityBlockGuidance()||N.bOffRoute))
        Cue=FString::Printf(TEXT("%s  /  %.0f m"),*N.CueText,N.CueDistanceCm/100.0);
    UITextFit(Cue,Presentation.RouteAdvice.Kind==EMAYARouteAdvice::Yield?Amber:Muted,
        L.Navigation.X+40,L.Navigation.Y+76,L.Navigation.W-54,.9f);
    // Continuous local north-up surroundings on every offer. Each pavement cell
    // has a fixed world address; unknown and expired-stream evidence stay honest.
    PassiveGroup(L.Map);
    if (PC && (PC->GetPlayerScreen()==EMAYAPlayerScreen::ShiftWin || PC->GetPlayerScreen()==EMAYAPlayerScreen::ShiftLost)) bSuppressUI=true;
    if (bTerminal && ContextHeight<218.f) bSuppressUI=true; // Compact result actions take priority over the passive circle.
    const bool bMinimapSubmitted=!bSuppressUI;
    const FVector2D RadarCentre(L.Map.X + L.Map.W * .5, L.Map.Y + L.Map.W * .5f);
    const double RadarRadius = L.Map.W * .5 - 4;
    for (int32 Y = -FMath::FloorToInt(RadarRadius); Y <= FMath::FloorToInt(RadarRadius); ++Y)
    {
        const double Half = FMath::Sqrt(FMath::Max(0.0, RadarRadius * RadarRadius - Y * Y));
        // UIRect rounds shared row edges, so fractional UI scales cannot leave scanline gaps.
        UIRect(RadarBackground, float(RadarCentre.X - Half), float(RadarCentre.Y + Y), float(2 * Half), 1.f);
    }
    if (bTelemetry && Finite(R.ChassisPose.GetLocation()))
    {
        TArray<AMAYADeliveryStation*> Stations;
        for (TActorIterator<AMAYADeliveryStation> It(GetWorld()); It; ++It)
        {
            if (D->FindStation(It->StationId) != *It || !Finite(It->GetServicePoint())
                || (!D->UsesCityBlockGuidance() && D->IsCityBlockStation(*It))) continue;
            Stations.Add(*It);
        }
        const FRect Inner { float(RadarCentre.X - RadarRadius), float(RadarCentre.Y - RadarRadius),
            float(RadarRadius * 2), float(RadarRadius * 2) };
        const FMapTransform Map = LocalMapTransform(Inner, R.ChassisPose.GetLocation());
        RadarWorldCentre = Map.WorldCentre; RadarWorldSpan = MinimapSpanCm;
        RadarSampleNow = FPlatformTime::Seconds(); // retries work while the world is paused
        const FIntPoint PlayerCell = MinimapCellKey(RadarWorldCentre);
        RadarSurfaceCache.MoveWindow(PlayerCell);
        if (RadarSampleOrder.IsEmpty())
        {
            for (int32 Y = -41; Y <= 41; ++Y) for (int32 X = -41; X <= 41; ++X)
                RadarSampleOrder.Add(FIntPoint(X, Y));
            RadarSampleOrder.Sort([](const FIntPoint& A, const FIntPoint& B)
            {
                const int32 DA = A.X * A.X + A.Y * A.Y, DB = B.X * B.X + B.Y * B.Y;
                return DA == DB ? (A.Y == B.Y ? A.X < B.X : A.Y < B.Y) : DA < DB;
            }); // one-time order; no per-frame scene discovery or sort
        }
        auto InView = [&](const FIntPoint& Key)
        {
            const FVector2D Delta = MinimapCellCentre(Key) - RadarWorldCentre;
            const double DX = FMath::Max(0.0, FMath::Abs(Delta.X) - MinimapCellCm * .5);
            const double DY = FMath::Max(0.0, FMath::Abs(Delta.Y) - MinimapCellCm * .5);
            return RadarSurfaceCache.Contains(Key) && DX * DX + DY * DY <= FMath::Square(MinimapSpanCm * .5);
        };
        FCollisionQueryParams SurfaceQuery(SCENE_QUERY_STAT(MAYARadarStaticSurvey), false);
        SurfaceQuery.MobilityType = EQueryMobilityType::Static;
        SurfaceQuery.AddIgnoredActor(D->GetRobot());
        for (int32 I = 0; I < D->GetParcelCount(); ++I) SurfaceQuery.AddIgnoredActor(D->GetParcel(I));
        RadarLastFrameTraces = RadarNewTraces = RadarRefreshTraces = 0; RadarLastSampleKeys.Reset();
        auto SampleCell = [&](const FIntPoint& Key, bool bRefresh)
        {
            if (!TakeMinimapTrace(RadarLastFrameTraces,RadarNewTraces,RadarRefreshTraces,bRefresh)) return;
            const FVector2D XY = MinimapCellCentre(Key); const double Z = R.ChassisPose.GetLocation().Z;
            FHitResult SurfaceHit;
            const bool bHit = GetWorld()->LineTraceSingleByChannel(SurfaceHit,
                FVector(XY.X, XY.Y, Z + 10000), FVector(XY.X, XY.Y, Z - 500), ECC_Visibility, SurfaceQuery);
            RadarLastSampleKeys.Add(Key);
            UPrimitiveComponent* Component = bHit ? SurfaceHit.GetComponent() : nullptr;
            const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component);
            const FString MeshPath = MeshComponent ? GetPathNameSafe(MeshComponent->GetStaticMesh()) : FString();
            const bool bActualStatic = Component && Component->Mobility == EComponentMobility::Static;
            const bool bFiniteHit = bHit && !SurfaceHit.bStartPenetrating && Finite(SurfaceHit.ImpactPoint)
                && Finite(SurfaceHit.ImpactNormal);
            const ESurroundingSurface Surface = bFiniteHit
                ? ClassifyMinimapSurface(MeshPath, bActualStatic, SurfaceHit.ImpactPoint.Z - Z, SurfaceHit.ImpactNormal.Z)
                : ESurroundingSurface::Unknown;
            RadarSurfaceCache.Observe(Key, Surface, RadarSampleNow, Component, FName(*MeshPath),
                Component ? FName(*Component->GetPathName()) : NAME_None,
                bFiniteHit ? SurfaceHit.ImpactPoint.Z : 0, bFiniteHit ? SurfaceHit.ImpactNormal.Z : 0);
        };
        auto SampleNew = [&](int32 Quota)
        {
            int32 Used = 0;
            for (const FIntPoint& Offset : RadarSampleOrder)
            {
                if (Used >= Quota || RadarLastFrameTraces >= 24) break;
                const FIntPoint Key = PlayerCell + Offset;
                if (InView(Key) && RadarSurfaceCache.NeverSampled(Key)) { SampleCell(Key, false); ++Used; }
            }
        };
        auto SampleRefresh = [&](int32 Quota)
        {
            int32 Used = 0;
            for (int32 Scan = 0; Scan < RadarSampleOrder.Num() && Used < Quota && RadarLastFrameTraces < 24; ++Scan)
            {
                const FIntPoint Key = PlayerCell + RadarSampleOrder[RadarRefreshCursor];
                RadarRefreshCursor = (RadarRefreshCursor + 1) % RadarSampleOrder.Num();
                const FSurroundingsCell* Cell = RadarSurfaceCache.Find(Key);
                if (InView(Key) && Cell && Cell->RefreshDue(RadarSampleNow)) { SampleCell(Key, true); ++Used; }
            }
        };
        // At most24 ACTUAL traces/frame:18 new near-player cells and6 retry/refresh;
        // unused quota is redistributed without erasing any overlapping sample.
        SampleNew(18); SampleRefresh(6); SampleNew(24 - RadarLastFrameTraces); SampleRefresh(24 - RadarLastFrameTraces);
        auto SurfaceColour = [&](ESurroundingSurface Surface, bool bStale)
        {
            FLinearColor Colour = RadarBackground;
            if (Surface == ESurroundingSurface::Road) Colour = MAYAUITheme::Road;
            if (Surface == ESurroundingSurface::Sidewalk) Colour = MAYAUITheme::Sidewalk;
            if (Surface == ESurroundingSurface::Structure) Colour = MAYAUITheme::Structure;
            if (bStale) { Colour.R *= .72f; Colour.G *= .72f; Colour.B *= .72f; }
            return Colour; // solid surface/backing; opacity is never compounded
        };
        const FIntPoint MinKey = MinimapCellKey(RadarWorldCentre - FVector2D(MinimapSpanCm * .5));
        const FIntPoint MaxKey = MinimapCellKey(RadarWorldCentre + FVector2D(MinimapSpanCm * .5));
        RadarDrawRuns = RadarVisibleCells = 0;
        for (int32& Count : RadarVisibleClassCounts) Count = 0;
        for (int32 Y = MaxKey.Y; Y >= MinKey.Y; --Y)
        {
            const double Top = Map.Project(FVector(0, (double(Y) + 1) * MinimapCellCm, 0)).Y;
            const double Bottom = Map.Project(FVector(0, double(Y) * MinimapCellCm, 0)).Y;
            const double RowTop = FMath::Max(Top, RadarCentre.Y - RadarRadius + 2);
            const double RowBottom = FMath::Min(Bottom, RadarCentre.Y + RadarRadius - 2);
            const double FarY = FMath::Max(FMath::Abs(RowTop - RadarCentre.Y), FMath::Abs(RowBottom - RadarCentre.Y));
            if (RowBottom <= RowTop || FarY >= RadarRadius - 2) continue;
            const double HalfRow = FMath::Sqrt(FMath::Square(RadarRadius - 2) - FarY * FarY);
            int32 RunStart = MinKey.X;
            ESurroundingSurface RunSurface = ESurroundingSurface::Unknown; bool bRunStale = false;
            auto FlushRun = [&](int32 EndX)
            {
                if (RunSurface == ESurroundingSurface::Unknown) return; // opaque unknown backing already drawn
                const double Left = FMath::Max(Map.Project(FVector(double(RunStart) * MinimapCellCm, 0, 0)).X, RadarCentre.X - HalfRow);
                const double Right = FMath::Min(Map.Project(FVector(double(EndX) * MinimapCellCm, 0, 0)).X, RadarCentre.X + HalfRow);
                if (Right > Left) { UIRect(SurfaceColour(RunSurface, bRunStale),float(Left),float(RowTop),float(Right-Left),float(RowBottom-RowTop)); ++RadarDrawRuns; }
            };
            for (int32 X = MinKey.X; X <= MaxKey.X + 1; ++X)
            {
                const FIntPoint Key(X,Y); const FSurroundingsCell* Cell = X <= MaxKey.X ? RadarSurfaceCache.Find(Key) : nullptr;
                const ESurroundingSurface Surface = Cell ? Cell->EffectiveSurface(RadarSampleNow) : ESurroundingSurface::Unknown;
                const bool bStale = Cell && Cell->IsStale(RadarSampleNow);
                if (X <= MaxKey.X && InView(Key)) { ++RadarVisibleCells; ++RadarVisibleClassCounts[int32(Surface)]; }
                if (X == MinKey.X) { RunSurface = Surface; bRunStale = bStale; }
                else if (X > MaxKey.X || Surface != RunSurface || bStale != bRunStale)
                { FlushRun(X); RunStart=X; RunSurface=Surface; bRunStale=bStale; }
            }
        }
        auto MapPoint = [&](const FVector& P) { return Map.Project(P); };
        auto MapLine = [&](FVector2D A, FVector2D B, const FLinearColor& C, float Thickness)
        { if (ClipCircle(RadarCentre, RadarRadius - 4, A, B)) UILine(A.X, A.Y, B.X, B.Y, C, Thickness); };
        for (int32 I = 1; I < N.Survey.Points.Num(); ++I)
            MapLine(MapPoint(N.Survey.Points[I - 1]), MapPoint(N.Survey.Points[I]), MAYAUITheme::Track, .75f);
        for (int32 I = 1; I < N.ActivePath.Num(); ++I)
            MapLine(MapPoint(N.ActivePath[I - 1]), MapPoint(N.ActivePath[I]), Emerald, 1.5f);
        for (AMAYADeliveryStation* Station : Stations)
        {
            const FVector2D P = MapPoint(Station->GetServicePoint());
            if ((P - RadarCentre).Size() > RadarRadius - 13) continue;
            const bool bActive = Station == N.Target && !S.bCharging;
            const FLinearColor MarkerColor=bPickupPhase?ServiceGreen:bDeliveryPhase?Red:Amber;
            if (bActive) UIArc(P.X, P.Y, 8, 1, Amber, 2);
            if (Station->IsChargingStation())
            {
                // Lightning bolt remains visible even while a delivery objective is red in-world.
                MapLine(P + FVector2D(2,-6), P + FVector2D(-3,1), ServiceGreen, 2);
                MapLine(P + FVector2D(-3,1), P + FVector2D(3,-1), ServiceGreen, 2);
                MapLine(P + FVector2D(3,-1), P + FVector2D(-2,6), ServiceGreen, 2);
                // A charger can also be the actual delivery/pickup objective.
                // Keep its green bolt and a separate semantic objective core.
                if(bActive)UIRect(MarkerColor,P.X+3.5f,P.Y+3.5f,5,5);
            }
            else UIRect(bActive ? MarkerColor : Muted, P.X - 3, P.Y - 3, 6, 6);
        }
        if (bTelemetry)
        {
            const FVector Actual = R.ChassisPose.GetLocation();
            FVector2D P = MapPoint(Actual);
            const bool bEdge = (P - RadarCentre).Size() > RadarRadius - 11;
            if (bEdge) P = RadarCentre + (P - RadarCentre).GetSafeNormal() * (RadarRadius - 11);
            const FVector2D Heading = Map.OpticalHeading(R.ChassisPose), Side(-Heading.Y, Heading.X);
            const FVector2D Tip = P + Heading * 9, Left = P - Heading * 5 + Side * 5, Right = P - Heading * 5 - Side * 5;
            MapLine(Left, Tip, White, 3); MapLine(Tip, Right, White, 3); MapLine(Right, Left, White, 2);
        }
    }
    UIArc(RadarCentre.X, RadarCentre.Y, RadarRadius + 2, 1, FLinearColor(Border.R,Border.G,Border.B,1.f), 1.5f);
    UIArc(RadarCentre.X, RadarCentre.Y, RadarRadius + 6, 1, MAYAUITheme::Alpha(MAYAUITheme::Background,.8f), 3);
    UIText(TEXT("N"), White, RadarCentre.X - 5, RadarCentre.Y - RadarRadius - 7, .9f);
    // Detailed address stays in the full map; the driving circle has no prose footer.

    bSuppressUI=false;
    // Route graphics use the current camera, not the robot heading, so orbit stays honest.
    if (PlayerOwner && N.bGuidance)
    {
        FVector Camera; FRotator CameraRotation; PlayerOwner->GetPlayerViewPoint(Camera, CameraRotation);
        const FVector CameraForward = CameraRotation.Vector();
        const FRect RenderRect { 10, 10, L.Width - 20, L.Height - 20 };
        TArray<FRect> Reserved = { L.Identity,L.Battery,L.Navigation,L.Map,L.Telemetry,L.Modal,L.Controls };
        // Known visible robot bounds already include the12px render reserve.
        // The broad camera band is only a fallback for unavailable projection.
        if (bProjectedRobot) Reserved.Add(ProjectedRobot);
        else Reserved.Add(FRect{L.Width*.30f,L.Height*.30f,L.Width*.40f,L.Height*.60f});
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYAPlayerRouteVisibility), false);
        Query.AddIgnoredActor(this); Query.AddIgnoredActor(D);
        if (D->GetRobot()) Query.AddIgnoredActor(D->GetRobot());
        if (D->GetParcel()) Query.AddIgnoredActor(D->GetParcel());
        auto Visible = [&](const FVector& P)
        {
            if (!Finite(P) || !Finite(Camera) || FVector::DotProduct(P - Camera, CameraForward) <= 5.0) return false;
            FHitResult Hit;
            return !GetWorld()->LineTraceSingleByChannel(Hit, Camera, P, ECC_Visibility, Query)
                || FVector::DistSquared(Hit.ImpactPoint, P) <= 9.0;
        };
        auto Project = [&](const FVector& P, FVector2D& Out)
        {
            if (!Finite(P) || FVector::DotProduct(P - Camera, CameraForward) <= 5.0
                || !PlayerOwner->ProjectWorldLocationToScreen(P, Out) || !Finite(Out)) return false;
            Out /= UIScale; return true;
        };
        const float ArrowOpacity=RouteArrowOpacity(GetWorld()->GetTimeSeconds());
        const FTextureResource* ArrowTextureResource=Canvas->DefaultTexture?Canvas->DefaultTexture->GetResource():nullptr;
        const FTexture* ArrowTexture=ArrowTextureResource&&!ArrowTextureResource->IsProxy()?ArrowTextureResource:nullptr;
        // UE5.3 canvas lines are opaque. Arrow-only edge quads retain vertex alpha.
        auto DrawArrowEdge = [&](const FVector2D& From,const FVector2D& To,const FLinearColor& Color,float Thickness)
        {
            if(bSuppressUI||UIGroupOpacity<=0||!ArrowTexture)return;
            const FVector2D Start(FMath::RoundToFloat(From.X*UIScale),FMath::RoundToFloat(From.Y*UIScale));
            const FVector2D End(FMath::RoundToFloat(To.X*UIScale),FMath::RoundToFloat(To.Y*UIScale));
            const FVector2D Along=(End-Start).GetSafeNormal();
            if(Along.IsNearlyZero())return;
            const FVector2D HalfWidth=FVector2D(-Along.Y,Along.X)*(FMath::Max(1.f,FMath::RoundToFloat(Thickness*UIScale))*.5f);
            const FVector2D A=Start+HalfWidth,B=End+HalfWidth,C=End-HalfWidth,E=Start-HalfWidth;
            FCanvasTriangleItem Stroke(A,B,C,ArrowTexture);
            FCanvasUVTri Second=Stroke.TriangleList[0];
            Second.V0_Pos=A;Second.V1_Pos=C;Second.V2_Pos=E;
            Stroke.TriangleList.Add(Second);
            FLinearColor ActualColor=Color;ActualColor.A*=UIGroupOpacity;
            Stroke.SetColor(ActualColor);
            Stroke.BlendMode=SE_BLEND_Translucent;
            Canvas->DrawItem(Stroke);
        };
        auto RenderHollowChevron = [&](const FVector2D& LeftPoint,const FVector2D& TipPoint,const FVector2D& RightPoint)
        {
            const FVector2D BaseCentre=(LeftPoint+RightPoint)*.5;
            // Small projections disappear as whole glyphs instead of closing the hollow notch.
            if((RightPoint-LeftPoint).Size()*UIScale<10||(TipPoint-BaseCentre).Size()*UIScale<10)return;
            const FVector2D Ribbon[]={LeftPoint,TipPoint,RightPoint,
                BaseCentre+(RightPoint-BaseCentre)*.72,
                BaseCentre+(TipPoint-BaseCentre)*.65,
                BaseCentre+(LeftPoint-BaseCentre)*.72};
            const FRect GlyphBounds{float(FMath::Min3(LeftPoint.X,TipPoint.X,RightPoint.X)-6),
                float(FMath::Min3(LeftPoint.Y,TipPoint.Y,RightPoint.Y)-6),
                float(FMath::Max3(LeftPoint.X,TipPoint.X,RightPoint.X)-FMath::Min3(LeftPoint.X,TipPoint.X,RightPoint.X)+12),
                float(FMath::Max3(LeftPoint.Y,TipPoint.Y,RightPoint.Y)-FMath::Min3(LeftPoint.Y,TipPoint.Y,RightPoint.Y)+12)};
            // Halo and every edge share one admission, so no fragment can cross a card or drone.
            if(!RenderRect.Contains(FVector2D(GlyphBounds.X,GlyphBounds.Y))
                ||!RenderRect.Contains(FVector2D(GlyphBounds.X+GlyphBounds.W,GlyphBounds.Y+GlyphBounds.H)))return;
            for(const FRect& Block:Reserved)if(GlyphBounds.Intersects(Block))return;
            const FLinearColor Halo=MAYAUITheme::Alpha(MAYAUITheme::Accent,ArrowOpacity*.14f);
            const FLinearColor Mint=MAYAUITheme::Alpha(MAYAUITheme::Text,ArrowOpacity);
            const FLinearColor Rim(Emerald.R,Emerald.G,Emerald.B,ArrowOpacity);
            for(int32 EdgeIndex=0;EdgeIndex<UE_ARRAY_COUNT(Ribbon);++EdgeIndex)
            {
                const auto& From=Ribbon[EdgeIndex];const auto& To=Ribbon[(EdgeIndex+1)%UE_ARRAY_COUNT(Ribbon)];
                DrawArrowEdge(From,To,Halo,4.f/UIScale);
                DrawArrowEdge(From,To,EdgeIndex<2?Mint:Rim,1.f/UIScale);
            }
        };
        for (int32 I = 0; N.bMoving && I < MaxChevrons; ++I)
        {
            const double Ahead = 105.0 + I * 145.0;
            // Keep the entire arrow before the goal and outside its 90 cm service circle.
            if (Ahead + 25 >= N.RemainingCm - AMAYADeliveryStation::ObjectiveRingRadiusCm) break;
            const double Arc = N.Robot.ArcCm + Ahead * N.Direction;
            const FVector Back = N.Survey.Sample(Arc - 20 * N.Direction), Tip = N.Survey.Sample(Arc + 25 * N.Direction);
            const FVector Tangent = (Tip - Back).GetSafeNormal2D();
            if (Tangent.IsNearlyZero()) continue;
            const FVector Side(-Tangent.Y, Tangent.X, 0), Lift(0, 0, 6);
            const FVector Left = Back + Side * 22 + Lift, Right = Back - Side * 22 + Lift, Point = Tip + Lift;
            FVector2D A, B, C;
            if (!Project(Left, A) || !Project(Point, B) || !Project(Right, C)) continue;
            if (!Visible(Left) || !Visible(Point) || !Visible(Right)) continue;
            RenderHollowChevron(A,B,C);
        }

        // Destination tag is a single rigid unit. Off-screen/behind targets get a camera-relative pointer.
        const FVector Marker = N.CuePoint + FVector(0, 0, 90);
        FVector2D MarkerScreen;
        const bool bProjected = Project(Marker, MarkerScreen);
        const FRect Tag { bProjected ? float(MarkerScreen.X - 82) : 0, bProjected ? float(MarkerScreen.Y - 17) : 0, 164, 34 };
        bool bTagClear = bProjected && RenderRect.Contains(FVector2D(Tag.X, Tag.Y), 6)
            && RenderRect.Contains(FVector2D(Tag.X + Tag.W, Tag.Y + Tag.H), 6);
        for (const FRect& Block : Reserved) if (Tag.Intersects(Block)) bTagClear = false;
        if (bTagClear && Visible(Marker))
        {
            UIPanel(Tag.X, Tag.Y, Tag.W, Tag.H, Emerald);
            // The mission card retains the cue; this tag preserves its actual distance.
            UIText(FString::Printf(TEXT("%.0f m"),N.CueDistanceCm/100.0),
                Emerald,Tag.X+12,Tag.Y+10,.9f);
        }
        else if (!bProjected || !RenderRect.Contains(MarkerScreen))
        {
            const FVector To = Marker - Camera;
            const double CameraRight = FVector::DotProduct(To, CameraRotation.RotateVector(FVector::RightVector));
            const double CameraAhead = FVector::DotProduct(To, CameraForward);
            const double CameraUp = FVector::DotProduct(To, CameraRotation.RotateVector(FVector::UpVector));
            FVector2D Direction(CameraRight, -CameraUp);
            if (CameraAhead <= 0) Direction = FVector2D(CameraRight >= 0 ? 1.0 : -1.0, .25);
            if (!Direction.Normalize()) Direction = FVector2D(0, -1);
            // Free central band keeps edge guidance clear of all corner cards and the robot.
            const FRect Edge { 298, 174, L.Width - 596, L.Height - 390 };
            const FVector2D Centre(Edge.X + Edge.W * .5, Edge.Y + Edge.H * .5);
            const double Radius = FMath::Min((Edge.W * .5 - 14) / FMath::Max(.001, FMath::Abs(Direction.X)),
                (Edge.H * .5 - 14) / FMath::Max(.001, FMath::Abs(Direction.Y)));
            const FVector2D P = Centre + Direction * Radius, Side(-Direction.Y, Direction.X);
            const FRect PointerBounds{float(P.X)-20,float(P.Y)-20,40,40};
            bool bPointerClear=true;
            for (const FRect& Block:Reserved) if(PointerBounds.Intersects(Block)) bPointerClear=false;
            if (bPointerClear) {
            RenderHollowChevron(P-Direction*16+Side*10,P,P-Direction*16-Side*10);
            }
        }
    }

    // Speed, energy and custody are essential. A conservative aggregate projected
    // render box must not erase the entire strip (actual game153 exposed this).
    const bool bTelemetryProjectedRobotOverlap=bProjectedRobot && L.Telemetry.Intersects(ProjectedRobot);
    bSuppressUI=false;
    const bool bTelemetrySubmitted=true;
    UIPanel(L.Telemetry.X,L.Telemetry.Y,L.Telemetry.W,L.Telemetry.H,Emerald);
    const float TX=L.Telemetry.X,TY=L.Telemetry.Y,TW=L.Telemetry.W;
    const float SpeedW=TW*.19f,BatteryW=TW*.29f,CargoW=TW*.29f;
    const float BatteryX=TX+SpeedW,CargoX=BatteryX+BatteryW,ParcelX=CargoX+CargoW;
    const FLinearColor Divider=MAYAUITheme::Alpha(Border,.28f);
    for(float ColumnX:{BatteryX,CargoX,ParcelX}) UIRect(Divider,ColumnX,TY+16,1.f/UIScale,L.Telemetry.H-32);
    const FString SpeedText=bTelemetry?FString::Printf(TEXT("%.1f"),R.VelocityCmps.Size()*.036):TEXT("--");
    const float SpeedTextWidth=SpeedW-22.f;
    float SpeedTextScale=2.6f;
    if(MeasureUIText(SpeedText,SpeedTextScale).X>SpeedTextWidth)
    {
        // Actual native font metrics; keep the existing12px/large14px floor.
        // Six bounded probes choose the largest fitting scale, never an ellipsis.
        float Lower=MinimumTextScale,Upper=SpeedTextScale;
        for(int32 FitStep=0;FitStep<6;++FitStep)
        {const float Mid=(Lower+Upper)*.5f;if(MeasureUIText(SpeedText,Mid).X<=SpeedTextWidth)Lower=Mid;else Upper=Mid;}
        SpeedTextScale=Lower;
    }
    const float SpeedMeasuredWidth=MeasureUIText(SpeedText,SpeedTextScale).X;
    const bool bSpeedValueFits=SpeedMeasuredWidth<=SpeedTextWidth;
    UIText(SpeedText,White,TX+13,TY+13,SpeedTextScale);
    UITextFit(R.bBoostDriveActive?TEXT("BOOST"):TEXT("km/h"),R.bBoostDriveActive?Emerald:Muted,TX+14,TY+57,SpeedW-22,.9f);
    const int32 SecuredCount=D->GetSecuredParcelCount();
    const bool bCondition=SecuredCount>0 && bTelemetry && FMath::IsFinite(S.CargoConditionPercent);
    const double Condition=bCondition?FMath::Clamp(S.CargoConditionPercent,0.0,100.0):0;
    const FLinearColor ConditionColor=!bCondition?Muted:Condition<80?Red:Amber;
    auto Meter=[&](float X,float Width,const FString& Label,const FString& Value,double Percent,const FLinearColor& Color)
    {
        UITextFit(Label,Muted,X+12,TY+13,Width-24,.9f);
        UITextFit(Value,White,X+12,TY+34,Width-24,1.45f);
        const float BarWidth=Width-24,Gap=2.f,SegmentWidth=(BarWidth-9*Gap)/10;
        for(int32 Segment=0;Segment<10;++Segment)
        {
            const float BX=X+12+Segment*(SegmentWidth+Gap);
            UIRect(MAYAUITheme::Alpha(MAYAUITheme::Track,.75f),BX,TY+65,SegmentWidth,6);
            const float Fraction=FMath::Clamp(float(Percent/10.0-Segment),0.f,1.f);
            if(Fraction>0) UIRect(Color,BX,TY+65,SegmentWidth*Fraction,6);
        }
    };
    Meter(BatteryX,BatteryW,S.bCharging?TEXT("Charging"):Charge<=10&&bBattery?TEXT("Critical"):Charge<20&&bBattery?TEXT("Low battery"):TEXT("Battery"),
        bBattery?FString::Printf(TEXT("%.0f%%"),Charge):TEXT("--"),Charge,ChargeColor);
    Meter(CargoX,CargoW,bCondition&&Condition<80?TEXT("Critical cargo"):bCondition&&Condition<=85?TEXT("Low cargo"):TEXT("Cargo"),bCondition?FString::Printf(TEXT("%.1f%%"),Condition):TEXT("--"),Condition,ConditionColor);
    const float BoxX=ParcelX+14,BoxY=TY+23;
    UILine(BoxX,BoxY,BoxX+9,BoxY-5,White,1); UILine(BoxX+9,BoxY-5,BoxX+18,BoxY,White,1);
    UILine(BoxX,BoxY,BoxX+9,BoxY+5,White,1); UILine(BoxX+9,BoxY+5,BoxX+18,BoxY,White,1);
    UILine(BoxX,BoxY,BoxX,BoxY+11,White,1); UILine(BoxX+18,BoxY,BoxX+18,BoxY+11,White,1);
    UILine(BoxX,BoxY+11,BoxX+9,BoxY+16,White,1); UILine(BoxX+9,BoxY+16,BoxX+18,BoxY+11,White,1);
    UILine(BoxX+9,BoxY+5,BoxX+9,BoxY+16,White,1);
    const FString ParcelText=FString::Printf(TEXT("%d parcel%s"),SecuredCount,SecuredCount==1?TEXT(""):TEXT("s"));
    const float ParcelWidth=TW-(ParcelX-TX)-24;
    // Keep the real count intact at small/large-text sizes; the cube supplies its meaning.
    if(MeasureUIText(ParcelText,.9f).X<=ParcelWidth) UIText(ParcelText,White,ParcelX+12,TY+57,.9f);
    else UIText(FString::Printf(TEXT("x%d"),SecuredCount),White,ParcelX+12,TY+57,1.05f);

    // Full key reference remains in Pause -> Controls; no persistent instruction slab.
    PassiveGroup(L.Controls);
    FLinearColor HelpColor=Muted; HelpColor.A=HintOpacity;
    UITextFit(TEXT("M  Map    C  Camera    P  Pause"),HelpColor,L.Controls.X,L.Controls.Y,L.Controls.W,.85f);
    // Essential state/actions remain reachable; passive corner groups alone yield to the drone.
    bSuppressUI=false;
    bContextOccluded=PC && PC->GetPlayerScreen()==EMAYAPlayerScreen::ShiftLost && bProjectedRobot && L.Modal.Intersects(ProjectedRobot);
    if (bTerminal && !bContextOverridesResult && !(PC && PC->HasPlayerMenu()))
    {
        const bool Success=S.Result.bSuccess;
        const FLinearColor Accent=Success?Emerald:Red;
        const float X=ContextX,Y=ContextY,W=ContextWidth;
        const float ResultH=FMath::Max(218.f,ContextHeight);
        UIPanel(X,Y,W,ResultH,Accent);
        UITextFit(Success?TEXT("DELIVERED"):TEXT("MISSION FAILED"),Accent,X+12,Y+12,W-24,1.25f);
        UITextWrap(S.Result.Reason,White,X+12,Y+43,W-24,2,.95f);
        UITextFit(FString::Printf(TEXT("+$%d   Cash $%d"),S.Result.Credits,S.TotalCredits),White,X+12,Y+99,W-24,1.f);
        UITextFit(FString::Printf(TEXT("Cargo %.1f%%  /  %02d:%02d"),S.Result.CargoConditionPercent,
            int32(S.Result.ElapsedSeconds)/60,int32(S.Result.ElapsedSeconds)%60),Muted,X+12,Y+126,W-24,.9f);
        const auto View=D->GetPlayerPresentation();
        if (bPaused) UIAction(TEXT("Resume"),TEXT("[P] RESUME"),X+12,Y+163,W-24,42,true,true);
        else
        {
            UIAction(Success?TEXT("Next"):TEXT("Replay"),Success?TEXT("[N] NEXT DELIVERY"):TEXT("[Enter] REPLAY"),
                X+12,Y+ResultH-76,W-24,32,Success?View.bCanNext:View.bCanReplay,true);
            UIAction(Success?TEXT("Replay"):TEXT("Recover"),Success?TEXT("[Enter] REPLAY"):TEXT("[R] RECOVER"),
                X+12,Y+ResultH-38,W-24,30,Success?View.bCanReplay:View.bCanRecover);
        }
    }
    if (!bTerminal || bContextOverridesResult) DrawContextPanels(D);
    bSuppressUI=false;
#if !UE_BUILD_SHIPPING
    auto Evidence=MakeShared<FJsonObject>();
    Evidence->SetStringField(TEXT("schema"),TEXT("MayaRobotics.DrivingHUD.ConceptC.v1"));
    Evidence->SetStringField(TEXT("theme"),TEXT("AtlasNoir"));
    if(D->GetRobot())Evidence->SetStringField(TEXT("actualSelectedCameraMode"),D->GetRobot()->IsPlayerCameraPov()?TEXT("POV"):TEXT("Chase"));
    Evidence->SetNumberField(TEXT("lastDrawGameFrame"),double(GFrameCounter));
    Evidence->SetNumberField(TEXT("lastDrawWorldSeconds"),GetWorld()->GetTimeSeconds());
    Evidence->SetNumberField(TEXT("width"),Canvas->SizeX);Evidence->SetNumberField(TEXT("height"),Canvas->SizeY);Evidence->SetNumberField(TEXT("uiScale"),UIScale);
    Evidence->SetStringField(TEXT("jobInstanceId"),S.JobInstanceId);Evidence->SetNumberField(TEXT("phase"),int32(S.Phase));
    Evidence->SetStringField(TEXT("missionLabel"),MainTitle);Evidence->SetStringField(TEXT("destination"),TargetText);Evidence->SetStringField(TEXT("serviceStage"),Presentation.ServiceStage);
    Evidence->SetNumberField(TEXT("resetGeneration"),double(R.ResetGeneration));Evidence->SetNumberField(TEXT("securedParcelCount"),SecuredCount);
    Evidence->SetNumberField(TEXT("cashDollars"),S.TotalCredits);Evidence->SetBoolField(TEXT("deadlineKnown"),bTime);
    if(bTime)Evidence->SetNumberField(TEXT("deadlineRemainingSeconds"),TimeLeft);
    if(bTelemetry)Evidence->SetNumberField(TEXT("speedKmph"),R.VelocityCmps.Size()*.036);
    Evidence->SetStringField(TEXT("speedDisplayText"),SpeedText);Evidence->SetNumberField(TEXT("speedDisplayScale"),SpeedTextScale);
    Evidence->SetNumberField(TEXT("speedTextMeasuredWidthPixels"),SpeedMeasuredWidth*UIScale);Evidence->SetNumberField(TEXT("speedTextAvailableWidthPixels"),SpeedTextWidth*UIScale);
    Evidence->SetBoolField(TEXT("speedValueFits"),bSpeedValueFits);Evidence->SetStringField(TEXT("speedTextDrawMode"),TEXT("full_numeric_no_ellipsis"));
    if(bBattery)Evidence->SetNumberField(TEXT("batteryPercent"),Charge);
    Evidence->SetBoolField(TEXT("cargoConditionKnown"),bCondition);if(bCondition)Evidence->SetNumberField(TEXT("cargoConditionPercent"),Condition);
    Evidence->SetNumberField(TEXT("hintOpacity"),HintOpacity);Evidence->SetBoolField(TEXT("canAccept"),Presentation.bCanAccept);Evidence->SetBoolField(TEXT("canInteract"),Presentation.bCanInteract);
    auto Box=[&](const TCHAR* Name,const FRect& Rect)
    {auto B=MakeShared<FJsonObject>();B->SetNumberField(TEXT("x"),Rect.X*UIScale);B->SetNumberField(TEXT("y"),Rect.Y*UIScale);B->SetNumberField(TEXT("w"),Rect.W*UIScale);B->SetNumberField(TEXT("h"),Rect.H*UIScale);Evidence->SetObjectField(Name,B);};
    Evidence->SetBoolField(TEXT("missionSubmitted"),bMissionSubmitted);Evidence->SetBoolField(TEXT("timerCashSubmitted"),bTimerCashSubmitted);
    Evidence->SetBoolField(TEXT("minimapSubmitted"),bMinimapSubmitted);Evidence->SetBoolField(TEXT("telemetrySubmitted"),bTelemetrySubmitted);
    Evidence->SetBoolField(TEXT("telemetrySuppressedByRobot"),false);Evidence->SetBoolField(TEXT("telemetryProjectedRobotOverlap"),bTelemetryProjectedRobotOverlap);
    Evidence->SetBoolField(TEXT("projectedRobotKnown"),bProjectedRobot);if(bProjectedRobot)Box(TEXT("projectedRobotBox"),ProjectedRobot);
    Evidence->SetStringField(TEXT("visibilityScope"),TEXT("Actual draw submission gates; screenshot pixels decide composited visibility/overlap."));
    Box(TEXT("missionBox"),L.Navigation);Box(TEXT("timerCashBox"),ClockCash);Box(TEXT("minimapBox"),L.Map);Box(TEXT("telemetryBox"),L.Telemetry);Box(TEXT("contextBox"),L.Modal);
    Evidence->SetNumberField(TEXT("visibleMinimapDiameterPixels"),2*RadarRadius*UIScale);
    FJsonSerializer::Serialize(Evidence,TJsonWriterFactory<>::Create(&DrivingHUDEvidence));
#endif
    DrawPlayerMenus(D);
    UpdateQACapture(D);
}
