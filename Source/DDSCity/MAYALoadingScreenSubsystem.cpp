#include "MAYALoadingScreenSubsystem.h"
#include "MAYADeliveryGameMode.h"
#include "MAYADeliveryTypes.h"
#include "MAYANativeDemoPawn.h"
#include "Game/CitySampleGameInstanceBase.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "MoviePlayer.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SLeafWidget.h"

namespace MAYALoading
{
    // CSS animation's default ease, cubic-bezier(.25,.1,.25,1), duration 1s.
    double Ease(double X)
    {
        double Low = 0, High = 1;
        for (int32 I = 0; I < 16; ++I)
        {
            const double T = (Low + High) * .5, U = 1 - T;
            const double BX = 3 * U * U * T * .25 + 3 * U * T * T * .25 + T * T * T;
            if (BX < X) Low = T; else High = T;
        }
        const double T = (Low + High) * .5, U = 1 - T;
        return 3 * U * U * T * .1 + 3 * U * T * T + T * T * T;
    }

    class SScreen final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SScreen) {} SLATE_ARGUMENT(UMaterialInterface*, Logo) SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {
            LogoBrush.SetResourceObject(Args._Logo);
            LogoBrush.DrawAs = ESlateBrushDrawType::Image;
            LogoBrush.ImageType = ESlateBrushImageType::FullColor;
            LogoBrush.ImageSize = FVector2D(420, 420 * 372.0 / 1813.0);
            Origin = FPlatformTime::Seconds();
            SetVisibility(EVisibility::HitTestInvisible); // does not intercept game controls/focus
            SetCanTick(true);
            ForceVolatile(true);
        }
        virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1280, 720); }
        virtual void Tick(const FGeometry&, double, float) override { Invalidate(EInvalidateWidgetReason::Paint); }
        virtual int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&,
            FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const override
        {
            const FVector2D Size = G.GetLocalSize();
            const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
            auto Box = [&](double X, double Y, double W, double H, const FLinearColor& Color, const FSlateBrush* Brush)
            {
                FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(FVector2D(W, H),
                    FSlateLayoutTransform(FVector2D(X, Y))), Brush, ESlateDrawEffect::None, Color);
            };
            Box(0, 0, Size.X, Size.Y, FLinearColor::Black, White);
            ++Layer;
            const double Width = FMath::Clamp(Size.X * .33, 240.0, 420.0);
            const double LogoHeight = Width * 372.0 / 1813.0;
            const double Top = (Size.Y - (LogoHeight + 196)) * .5;
            Box((Size.X - Width) * .5, Top, Width, LogoHeight, FLinearColor::White, &LogoBrush);
            const auto Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
            const FSlateFontInfo BrandFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 30);
            const FSlateFontInfo SubtitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18);
            const FString Brand(TEXT("ROBOTICS"));
            double GlyphWidth = 0;
            for (int32 I = 0; I < Brand.Len(); ++I) GlyphWidth += Measure->Measure(Brand.Mid(I, 1), BrandFont).X;
            const double Tracking = (Width - GlyphWidth) / (Brand.Len() - 1);
            double X = (Size.X - Width) * .5;
            const double BrandY = Top + LogoHeight + 14;
            for (int32 I = 0; I < Brand.Len(); ++I)
            {
                const FString Glyph = Brand.Mid(I, 1);
                const FVector2D GlyphSize = Measure->Measure(Glyph, BrandFont);
                FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(GlyphSize,
                    FSlateLayoutTransform(FVector2D(X, BrandY))), Glyph, BrandFont,
                    ESlateDrawEffect::None, FLinearColor(.91f, .86f, .76f));
                X += GlyphSize.X + Tracking;
            }
            const FString Subtitle(TEXT("Delivery Drone Simulator"));
            const FVector2D SubtitleSize = Measure->Measure(Subtitle, SubtitleFont);
            FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(SubtitleSize,
                FSlateLayoutTransform(FVector2D((Size.X - SubtitleSize.X) * .5, BrandY + 48))),
                Subtitle, SubtitleFont, ESlateDrawEffect::None, FLinearColor::White);
            // CSS content 100x50, black 10px border => 120x70 total. Three fixed
            // 20px disks at left/centre/right; the fourth travels left->right.
            // Blur the UNION (not a sum) with sigma4px, then contrast10 on black.
            // 1px samples are grouped into horizontal runs to bound draw calls.
            constexpr int32 W = 120, H = 70, Radius = 12;
            TArray<float> Mask, Horizontal;
            Mask.SetNumZeroed(W * H); Horizontal.SetNumZeroed(W * H);
            const double Phase = FMath::Frac(FPlatformTime::Seconds() - Origin);
            const double Centers[4] = { 20, 60, 100, 20 + 80 * Ease(Phase) };
            for (int32 Y = 0; Y < H; ++Y) for (int32 PixelX = 0; PixelX < W; ++PixelX)
                for (double Center : Centers)
                    if (FMath::Square(PixelX + .5 - Center) + FMath::Square(Y + .5 - 35) <= 100)
                    { Mask[Y * W + PixelX] = 1; break; }
            float Kernel[2 * Radius + 1]; double Sum = 0;
            for (int32 K = -Radius; K <= Radius; ++K)
            { Kernel[K + Radius] = float(FMath::Exp(-double(K * K) / 32.0)); Sum += Kernel[K + Radius]; }
            for (float& K : Kernel) K /= float(Sum);
            for (int32 Y = 0; Y < H; ++Y) for (int32 PixelX = 0; PixelX < W; ++PixelX)
                for (int32 K = -Radius; K <= Radius; ++K)
                    if (PixelX + K >= 0 && PixelX + K < W)
                        Horizontal[Y * W + PixelX] += Mask[Y * W + PixelX + K] * Kernel[K + Radius];
            const double Left = (Size.X - W) * .5, LoaderTop = BrandY + 103;
            for (int32 Y = 0; Y < H; ++Y)
            {
                int32 RunStart = 0, Prior = 0;
                for (int32 PixelX = 0; PixelX <= W; ++PixelX)
                {
                    int32 Shade = 0;
                    if (PixelX < W)
                    {
                        double Blur = 0;
                        for (int32 K = -Radius; K <= Radius; ++K)
                            if (Y + K >= 0 && Y + K < H)
                                Blur += Horizontal[(Y + K) * W + PixelX] * Kernel[K + Radius];
                        Shade = FMath::RoundToInt(FMath::Clamp((Blur - .5) * 10 + .5, 0.0, 1.0) * 32);
                    }
                    if (Shade != Prior)
                    {
                        if (Prior > 0) Box(Left + RunStart, LoaderTop + Y, PixelX - RunStart, 1,
                            FLinearColor(1, 1, 1, Prior / 32.0f), White);
                        RunStart = PixelX; Prior = Shade;
                    }
                }
            }
            return Layer;
        }
    private:
        FSlateBrush LogoBrush;
        double Origin = 0;
    };
}

bool UMAYALoadingScreenSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    return Super::ShouldCreateSubsystem(Outer) && Cast<UCitySampleGameInstanceBase>(Outer)
        && !IsRunningDedicatedServer() && !IsRunningCommandlet() && FApp::CanEverRender();
}

void UMAYALoadingScreenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (!FSlateApplication::IsInitialized()) return;
    LogoMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/MAYA/LoadingPresentation/M_MAYA_LoadingLogo_V2.M_MAYA_LoadingLogo_V2"));
    if (!LogoMaterial)
    { UE_LOG(LogTemp, Error, TEXT("MAYA LOADING logo material missing; startup presentation not installed")); return; }
    PreLoadHandle = FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &UMAYALoadingScreenSubsystem::OnPreLoadMap);
    PostLoadHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UMAYALoadingScreenSubsystem::OnPostLoadMap);
    WorldTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UMAYALoadingScreenSubsystem::OnWorldTick);
    TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UMAYALoadingScreenSubsystem::OnWorldTearDown);
    PrepareMovie(); // before original CitySample::Init performs its real preload
}

void UMAYALoadingScreenSubsystem::PrepareMovie()
{
    bActive = true;
    bCaptureRequested = false;
    if (!IsMoviePlayerEnabled() || !GetMoviePlayer() || GetMoviePlayer()->IsMovieCurrentlyPlaying()) return;
    MovieWidget = SNew(MAYALoading::SScreen).Logo(LogoMaterial);
    FLoadingScreenAttributes Screen;
    Screen.WidgetLoadingScreen = MovieWidget;
    Screen.MinimumLoadingScreenDisplayTime = -1; // no invented delay
    Screen.bAutoCompleteWhenLoadingCompletes = true;
    Screen.bWaitForManualStop = false;
    Screen.bMoviesAreSkippable = false;
    Screen.bAllowInEarlyStartup = false;
    Screen.bAllowEngineTick = false;
    GetMoviePlayer()->SetupLoadingScreen(Screen);
    bMovieOwned = GetMoviePlayer()->PlayMovie();
    UE_LOG(LogTemp, Display, TEXT("MAYA LOADING blocking-screen prepared movieStarted=%d realSeconds=%.6f logo=%s"),
        bMovieOwned, FPlatformTime::Seconds(), *GetPathNameSafe(LogoMaterial));
}

void UMAYALoadingScreenSubsystem::OnPreLoadMap(const FString& MapName)
{
    if (!MapName.Contains(TEXT("Small_City_LVL"))) { Finish(TEXT("other_map")); return; }
    PrepareMovie();
}

void UMAYALoadingScreenSubsystem::OnPostLoadMap(UWorld* World)
{
    if (!World) { Finish(TEXT("map_load_failed")); return; }
    if (World->GetGameInstance() != GetGameInstance() || !World->IsGameWorld()) return;
    if (!Cast<AMAYADeliveryGameMode>(World->GetAuthGameMode())) { Finish(TEXT("other_game_mode")); return; }
    LoadingWorld = World;
    LoadingWorldStartSeconds = World->GetTimeSeconds();
    AddOverlay(World);
}

void UMAYALoadingScreenSubsystem::AddOverlay(UWorld* World)
{
    if (!bActive || bOverlayAdded || !LogoMaterial || !World) return;
    UGameViewportClient* Viewport = GetGameInstance()->GetGameViewportClient();
    if (!Viewport) return;
    // A distinct widget avoids sharing mutable Slate state between movie thread
    // and normal viewport rendering during the actual map-load handoff.
    OverlayWidget = SNew(MAYALoading::SScreen).Logo(LogoMaterial);
    OverlayViewport = Viewport;
    Viewport->AddViewportWidgetContent(OverlayWidget.ToSharedRef(), 10000);
    bOverlayAdded = true;
    UE_LOG(LogTemp, Display, TEXT("MAYA LOADING bootstrap-overlay added worldSeconds=%.6f realSeconds=%.6f"),
        World->GetTimeSeconds(), FPlatformTime::Seconds());
}

void UMAYALoadingScreenSubsystem::OnWorldTick(UWorld* World, ELevelTick, float)
{
    if (!bActive || !World || World->GetGameInstance() != GetGameInstance() || World != LoadingWorld.Get()) return;
    AddOverlay(World);
    CaptureIfRequested();
    const AMAYADeliveryGameMode* Mode = Cast<AMAYADeliveryGameMode>(World->GetAuthGameMode());
    if (!Mode) { Finish(TEXT("game_mode_changed")); return; }
    if (Mode->HasStoppedDeliveryBootstrap()) { Finish(TEXT("bootstrap_failed")); return; }
    if (LoadingWorldStartSeconds >= 0 && World->GetTimeSeconds() - LoadingWorldStartSeconds > 60)
    { Finish(TEXT("native_ready_timeout")); return; }
    if (!Mode->HasCompletedDeliveryBootstrap()) return;
    APlayerController* Player = GetGameInstance()->GetFirstLocalPlayerController(World);
    const AMAYANativeDemoPawn* Robot = Player ? Cast<AMAYANativeDemoPawn>(Player->GetPawn()) : nullptr;
    if (!Robot) return;
    FMAYADeliveryRobotObservation Observation;
    if (!Robot->ReadDeliveryState(Observation)) return;
    if (!Observation.Fault.IsEmpty()) { Finish(TEXT("robot_startup_fault")); return; }
    if (Observation.IsStoppedAndSupported() && Observation.SupportMask == 63 && Observation.bDoorsClosedLatched
        && World->GetTimeSeconds() - Observation.ObservationWorldSeconds <= .25)
        Finish(TEXT("actual_robot_ready"));
}

void UMAYALoadingScreenSubsystem::CaptureIfRequested()
{
#if !UE_BUILD_SHIPPING
    if (bCaptureRequested || !bOverlayAdded || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01LoadingScreenCapture"))
        || FScreenshotRequest::IsScreenshotRequested()) return;
    FString Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), Output) || Output.IsEmpty())
        Output = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MayaLoadingScreen"));
    IFileManager::Get().MakeDirectory(*Output, true);
    const FString Path = FPaths::Combine(Output, TEXT("loading-screen-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png"));
    FScreenshotRequest::RequestScreenshot(Path, true, false);
    bCaptureRequested = true;
    UE_LOG(LogTemp, Display, TEXT("MAYA LOADING actual viewport screenshot REQUESTED: %s; pixels require independent review"), *Path);
#endif
}

void UMAYALoadingScreenSubsystem::Finish(const TCHAR* Reason)
{
    if (!bActive && !bOverlayAdded && !bMovieOwned) return;
    if (bOverlayAdded && OverlayViewport.IsValid() && OverlayWidget.IsValid())
        OverlayViewport->RemoveViewportWidgetContent(OverlayWidget.ToSharedRef());
    if (bMovieOwned && GetMoviePlayer() && GetMoviePlayer()->IsMovieCurrentlyPlaying()) GetMoviePlayer()->StopMovie();
    UE_LOG(LogTemp, Display, TEXT("MAYA LOADING presentation removed reason=%s worldSeconds=%.6f realSeconds=%.6f overlayWasAdded=%d"),
        Reason, LoadingWorld.IsValid() ? LoadingWorld->GetTimeSeconds() : -1.0, FPlatformTime::Seconds(), bOverlayAdded);
    bOverlayAdded = false; bMovieOwned = false; bActive = false;
    OverlayWidget.Reset(); MovieWidget.Reset(); OverlayViewport.Reset(); LoadingWorld.Reset();
    LoadingWorldStartSeconds = -1;
}

void UMAYALoadingScreenSubsystem::OnWorldTearDown(UWorld* World)
{
    if (World == LoadingWorld.Get()) Finish(TEXT("world_teardown"));
}

void UMAYALoadingScreenSubsystem::Deinitialize()
{
    Finish(TEXT("game_instance_shutdown"));
    FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadHandle);
    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadHandle);
    FWorldDelegates::OnWorldPostActorTick.Remove(WorldTickHandle);
    FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);
    LogoMaterial = nullptr;
    Super::Deinitialize();
}
