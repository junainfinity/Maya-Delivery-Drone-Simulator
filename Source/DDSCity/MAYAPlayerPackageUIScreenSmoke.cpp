#include "MAYAPlayerPackageUIScreenSmoke.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYANativeDemoPawn.h"
#include "GameFramework/HUD.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "UnrealClient.h"
#include "EngineUtils.h"
#include "Widgets/SWindow.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "CoreGlobals.h"
#include "InputCoreTypes.h"

UMAYAPlayerPackageUIScreenSmoke::UMAYAPlayerPackageUIScreenSmoke()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}
AMAYADeliveryPlayerController* UMAYAPlayerPackageUIScreenSmoke::Player() const
{ return Cast<AMAYADeliveryPlayerController>(GetOwner()); }
AMAYADeliveryDirector* UMAYAPlayerPackageUIScreenSmoke::Director() const
{ for (TActorIterator<AMAYADeliveryDirector> It(GetWorld()); It; ++It) return *It; return nullptr; }
FIntPoint UMAYAPlayerPackageUIScreenSmoke::ViewportSize() const
{
    const auto* P = Player(); const auto* L = P ? P->GetLocalPlayer() : nullptr;
    UGameViewportClient* V = L ? L->ViewportClient.Get() : nullptr;
    return V && V->Viewport ? V->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
}
int32 UMAYAPlayerPackageUIScreenSmoke::WindowMode() const
{
    const auto* P = Player(); const auto* L = P ? P->GetLocalPlayer() : nullptr;
    UGameViewportClient* V = L ? L->ViewportClient.Get() : nullptr;
    const TSharedPtr<SWindow> W = V ? V->GetWindow() : nullptr;
    return W.IsValid() ? int32(W->GetWindowMode()) : -1;
}
void UMAYAPlayerPackageUIScreenSmoke::BeginPlay()
{
    Super::BeginPlay();
#if WITH_EDITOR || !UE_BUILD_DEVELOPMENT
    SetComponentTickEnabled(false);
#else
    Started = Due = StepStarted = FPlatformTime::Seconds();
    const TCHAR* Cmd = FCommandLine::Get();
    if (!FParse::Param(Cmd, TEXT("MayaV01PackageUIScreenSmoke"))
        || !FParse::Value(Cmd, TEXT("MayaV01Output="), Output) || Output.IsEmpty())
    { SetComponentTickEnabled(false); return; }
    IFileManager::Get().MakeDirectory(*Output, true);
    for (const TCHAR* Excluded : {TEXT("MayaV01AutoTest"), TEXT("MayaV01ObserveInput"), TEXT("MayaV01UIScreenTest"),
        TEXT("MayaV01CargoTest"), TEXT("MayaV01FullInput"), TEXT("MayaV01OperatingTest"), TEXT("MayaV01CashShiftTest"),
        TEXT("MayaV01DamageTest"), TEXT("MayaV01CurbTest"), TEXT("MayaV01RingInteractionTest"), TEXT("MayaV01CargoVideo"),
        TEXT("MayaV01AirLoweringTest"), TEXT("MayaV01LoadingScreenCapture"), TEXT("MayaHUDCaptureLayouts"),
        TEXT("NullRHI"), TEXT("NoSound"), TEXT("windowed"), TEXT("fullscreen")})
        if (FParse::Param(Cmd, Excluded)) { Finish(false, TEXT("Incompatible raw diagnostic/display selector: ") + FString(Excluded)); return; }
    FString Value;
    for (const TCHAR* Excluded : {TEXT("ResX="), TEXT("ResY="), TEXT("ExecCmds="), TEXT("MayaV01InitialBattery="), TEXT("ForceTextureEncodeSpeed=")})
        if (FParse::Value(Cmd, Excluded, Value)) { Finish(false, TEXT("Incompatible raw command override: ") + FString(Excluded)); return; }
    if (!Check(FParse::Value(Cmd, TEXT("MayaV01PackageUIPhase="), Phase)
        && (Phase == TEXT("write") || Phase == TEXT("verify")), TEXT("Explicit write or verify package smoke phase"))) return;
    bVerify = Phase == TEXT("verify");
    FString Expected = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(Output), TEXT("GameUserSettings.ini")));
    FString Override;
    ResolvedSettingsFile = FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"), nullptr, *FPaths::GeneratedConfigDir()));
    FPaths::NormalizeFilename(Expected); FPaths::NormalizeFilename(ResolvedSettingsFile);
    const bool HasOverride = FParse::Value(Cmd, TEXT("GameUserSettingsINI="), Override);
    Override = FPaths::ConvertRelativePathToFull(Override); FPaths::NormalizeFilename(Override);
    if (!Check(FPaths::GetCleanFilename(Output) == (bVerify ? TEXT("second") : TEXT("first"))
        && HasOverride && Expected.Equals(Override, ESearchCase::IgnoreCase)
        && GConfig && GConfig->FindConfigFile(GGameUserSettingsIni)
        && Expected.Equals(ResolvedSettingsFile, ESearchCase::IgnoreCase),
        TEXT("Known settings cache resolves to the same explicit shared isolated attempt-root file"))) return;
    if (bVerify)
    {
        FString Text; TSharedPtr<FJsonObject> Previous;
        const TSharedPtr<FJsonObject>* K = nullptr; const TSharedPtr<FJsonObject>* B = nullptr;
        bool Success = false, Terminal = false, Quit = false; FString Marker, PreviousPhase, PreviousSettings;
        double X = 0, Y = 0, Mode = -1, BX = 0, BY = 0;
        if (!Check(FParse::Value(Cmd, TEXT("MayaV01PackageUIExpected="), ExpectedResultPath)
            && FFileHelper::LoadFileToString(Text, *ExpectedResultPath)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Previous) && Previous.IsValid()
            && Previous->TryGetBoolField(TEXT("terminal"), Terminal) && Terminal
            && Previous->TryGetBoolField(TEXT("success"), Success) && Success
            && Previous->TryGetBoolField(TEXT("productQuitRequested"), Quit) && Quit
            && Previous->TryGetStringField(TEXT("phase"), Marker) && Marker == TEXT("PackagedDisplaySmoke")
            && Previous->TryGetStringField(TEXT("uiPhase"), PreviousPhase) && PreviousPhase == TEXT("write")
            && Previous->TryGetStringField(TEXT("settingsDestination"), PreviousSettings)
            && Previous->TryGetObjectField(TEXT("keptDisplay"), K) && K && K->IsValid()
            && (*K)->TryGetNumberField(TEXT("viewportX"), X) && (*K)->TryGetNumberField(TEXT("viewportY"), Y)
            && (*K)->TryGetNumberField(TEXT("windowMode"), Mode)
            && Previous->TryGetObjectField(TEXT("startupDisplay"), B) && B && B->IsValid()
            && (*B)->TryGetNumberField(TEXT("viewportX"), BX) && (*B)->TryGetNumberField(TEXT("viewportY"), BY)
            && BX == 1280 && BY == 720 && Mode == 2 && X >= 960 && Y >= 540 && X <= 8192 && Y <= 8192
            && FMath::FloorToDouble(X) == X && FMath::FloorToDouble(Y) == Y && (X != BX || Y != BY),
            TEXT("Verify reads the successful first-phase actual kept-display receipt"))) return;
        FString ExpectedReceipt = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(Output), TEXT("first/result.json")));
        FString ActualReceipt = FPaths::ConvertRelativePathToFull(ExpectedResultPath);
        FPaths::NormalizeFilename(ExpectedReceipt); FPaths::NormalizeFilename(ActualReceipt); FPaths::NormalizeFilename(PreviousSettings);
        if (!Check(ExpectedReceipt.Equals(ActualReceipt, ESearchCase::IgnoreCase)
            && PreviousSettings.Equals(ResolvedSettingsFile, ESearchCase::IgnoreCase),
            TEXT("Expected first receipt belongs to this attempt and the identical shared settings destination"))) return;
        Wanted = FIntPoint(int32(X), int32(Y));
    }
    else Wanted = Baseline;
    RenderHandle = AHUD::OnHUDPostRender.AddUObject(this, &UMAYAPlayerPackageUIScreenSmoke::Rendered);
    ScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &UMAYAPlayerPackageUIScreenSmoke::ScreenshotProcessed);
    Event(TEXT("initialized"), FString::Printf(TEXT("phase=%s map=%s gameMode=%s settingsCacheKey=%s resolvedSettingsDestination=%s expectedReceipt=%s"),
        *Phase, *GetWorld()->GetMapName(), *GetNameSafe(GetWorld()->GetAuthGameMode()), *GGameUserSettingsIni, *ResolvedSettingsFile, *ExpectedResultPath));
#endif
}
TSharedPtr<FJsonObject> UMAYAPlayerPackageUIScreenSmoke::DisplayReceipt() const
{
    auto R = MakeShared<FJsonObject>(); const FIntPoint Size = ViewportSize();
    R->SetNumberField(TEXT("viewportX"), Size.X); R->SetNumberField(TEXT("viewportY"), Size.Y);
    R->SetNumberField(TEXT("windowMode"), WindowMode()); R->SetNumberField(TEXT("frame"), GFrameCounter);
    R->SetNumberField(TEXT("lastRenderedFrame"), LastRenderedFrame); R->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    R->SetBoolField(TEXT("beforeAnyUIAction"), !bAnyUIAction); R->SetStringField(TEXT("settingsDestination"), ResolvedSettingsFile);
    if (const auto* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
    {
        R->SetNumberField(TEXT("configuredX"), S->GetScreenResolution().X); R->SetNumberField(TEXT("configuredY"), S->GetScreenResolution().Y);
        R->SetNumberField(TEXT("configuredMode"), int32(S->GetFullscreenMode()));
        R->SetNumberField(TEXT("confirmedX"), S->GetLastConfirmedScreenResolution().X); R->SetNumberField(TEXT("confirmedY"), S->GetLastConfirmedScreenResolution().Y);
        R->SetNumberField(TEXT("confirmedMode"), int32(S->GetLastConfirmedFullscreenMode()));
    }
    return R;
}
void UMAYAPlayerPackageUIScreenSmoke::Event(const FString& Name, const FString& Detail)
{
    if (bEvidenceWriteFailed) return;
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("event"), Name); R->SetStringField(TEXT("detail"), Detail);
    R->SetStringField(TEXT("uiPhase"), Phase); R->SetNumberField(TEXT("step"), Step); R->SetNumberField(TEXT("frame"), GFrameCounter);
    R->SetNumberField(TEXT("realSeconds"), FPlatformTime::Seconds() - Started); R->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    if (auto* P = Player()) { R->SetNumberField(TEXT("screen"), int32(P->GetPlayerScreen())); R->SetBoolField(TEXT("menuBypass"), P->IsStartupMenuBypassedForAutomation()); }
    if (auto* D = Director()) { R->SetNumberField(TEXT("cash"), D->Snapshot().TotalCredits); R->SetNumberField(TEXT("battery"), D->Snapshot().BatteryPercent); R->SetBoolField(TEXT("shiftStarted"), D->Snapshot().bShiftStarted); }
    FString Text; FJsonSerializer::Serialize(R, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text + TEXT("\n"), *FPaths::Combine(Output, TEXT("ui-events.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
    { bEvidenceWriteFailed = true; Finish(false, TEXT("Package UI evidence write failed")); }
}
bool UMAYAPlayerPackageUIScreenSmoke::Check(bool Condition, const FString& Description)
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("check"), Description); R->SetBoolField(TEXT("passed"), Condition);
    Checks.Add(MakeShared<FJsonValueObject>(R)); Event(Condition ? TEXT("check_pass") : TEXT("check_fail"), Description);
    if (!Condition) Finish(false, Description);
    return Condition && !bFinished && !bEvidenceWriteFailed;
}
void UMAYAPlayerPackageUIScreenSmoke::Advance(double Delay)
{ if (!bFinished) { ++Step; StepStarted = FPlatformTime::Seconds(); Due = StepStarted + Delay; } }
void UMAYAPlayerPackageUIScreenSmoke::Finish(bool Success, const FString& Reason)
{
    if (bFinished) return; bFinished = true; bSuccessfulFinish = Success && !bEvidenceWriteFailed && bQuitRequested;
    auto R = MakeShared<FJsonObject>(); R->SetBoolField(TEXT("terminal"), true); R->SetBoolField(TEXT("success"), bSuccessfulFinish);
    R->SetStringField(TEXT("phase"), TEXT("PackagedDisplaySmoke")); R->SetStringField(TEXT("uiPhase"), Phase);
    R->SetStringField(TEXT("reason"), Reason); R->SetBoolField(TEXT("productQuitRequested"), bQuitRequested);
    R->SetStringField(TEXT("scope"), TEXT("Two-phase packaged ordinary-startup settings persistence, explicit rendered HUD-hitbox engine dispatch. Not human input, clean launcher invocation or independent pixel acceptance."));
    R->SetBoolField(TEXT("humanPhysicalInputVerified"), false); R->SetStringField(TEXT("nativePixelReview"), TEXT("pending independent inspection"));
    R->SetArrayField(TEXT("checks"), Checks); R->SetStringField(TEXT("settingsDestination"), ResolvedSettingsFile);
    R->SetStringField(TEXT("expectedFirstReceipt"), ExpectedResultPath); R->SetNumberField(TEXT("endWorldSeconds"), GetWorld()->GetTimeSeconds());
    if (StartupDisplay.IsValid()) R->SetObjectField(TEXT("startupDisplay"), StartupDisplay);
    if (KeptDisplay.IsValid()) R->SetObjectField(TEXT("keptDisplay"), KeptDisplay);
    if (RestoredDisplay.IsValid()) R->SetObjectField(TEXT("restoredDisplay"), RestoredDisplay);
    TArray<TSharedPtr<FJsonValue>> Captures; for (const FString& Name : CompletedCaptures) Captures.Add(MakeShared<FJsonValueString>(FPaths::Combine(Output, Name + TEXT(".png"))));
    R->SetArrayField(TEXT("captures"), Captures);
    FString Text; FJsonSerializer::Serialize(R, TJsonWriterFactory<>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Output, TEXT("result.json"))))
    { bSuccessfulFinish = false; UE_LOG(LogTemp, Error, TEXT("Package UI terminal evidence could not be saved")); }
    Due = FPlatformTime::Seconds() + 1;
}
bool UMAYAPlayerPackageUIScreenSmoke::ReadResolutionRow(FIntPoint& Out) const
{
    const auto* P = Player(); if (!P) return false;
    for (const auto& A : P->GetMenuActions()) if (A.Id == TEXT("Resolution") && A.bEnabled)
    {
        FString V = A.Value, X, Y; V.ReplaceInline(TEXT(" "), TEXT(""));
        if (!V.Split(TEXT("x"), &X, &Y) || !X.IsNumeric() || !Y.IsNumeric()) return false;
        Out = FIntPoint(FCString::Atoi(*X), FCString::Atoi(*Y)); return Out.X >= 960 && Out.Y >= 540 && Out.X <= 8192 && Out.Y <= 8192;
    }
    return false;
}
bool UMAYAPlayerPackageUIScreenSmoke::Click(FName Action)
{
    if (bFinished || bEvidenceWriteFailed) return false;
    auto* P = Player(); AHUD* H = P ? P->GetHUD() : nullptr; const FIntPoint Size = ViewportSize();
    if (!Check(P && LastRenderedScreen == P->GetPlayerScreen() && LastRenderedSize == Size && LastRenderedFrame > 0,
        TEXT("Hitbox dispatch uses an actually rendered current screen and viewport"))) return false;
    if (!Check(H && H->GetHitBoxWithName(Action), TEXT("Rendered enabled hitbox exists: ") + Action.ToString())) return false;
    const FHUDHitBox* Named = H->GetHitBoxWithName(Action);
    for (int32 Y = 8; Y < Size.Y - 8; Y += 8) for (int32 X = 8; X < Size.X - 8; X += 8)
    {
        const FVector2D At(X, Y); if (!Named->Contains(At)) continue;
        const auto* B = H->GetHitBoxAtCoordinates(At); if (!B || B->GetName() != Action) continue;
        bool Interior = true;
        for (const FVector2D Offset : {FVector2D(-3, 0), FVector2D(3, 0), FVector2D(0, -3), FVector2D(0, 3)})
        { const auto* N = H->GetHitBoxAtCoordinates(At + Offset); Interior &= N && N->GetName() == Action; }
        if (!Interior) continue;
        Event(TEXT("actual_hitbox_dispatch"), FString::Printf(TEXT("name=%s point=(%d,%d) viewport=(%d,%d) priority=%d"), *Action.ToString(), X, Y, Size.X, Size.Y, B->GetPriority()));
        if (bFinished || bEvidenceWriteFailed) return false;
        bAnyUIAction = true;
        const bool Hit = H->UpdateAndDispatchHitBoxClickEvents(At, IE_Pressed);
        H->UpdateAndDispatchHitBoxClickEvents(At, IE_Released);
        return Check(Hit, TEXT("Real HUD click dispatched: ") + Action.ToString());
    }
    return Check(false, TEXT("No interior viewport point resolves to hitbox: ") + Action.ToString());
}
bool UMAYAPlayerPackageUIScreenSmoke::Capture(const FString& Name)
{
    if (bFinished || bEvidenceWriteFailed) return false;
    if (CompletedCaptures.Contains(Name)) return true;
    if (!PendingCapture.IsEmpty()) return false;
    PendingCapture = FPaths::Combine(Output, Name + TEXT(".png")); ScreenshotSize = ViewportSize();
    ScreenshotRequestFrame = GFrameCounter; ScreenshotProcessedFrame = 0; bScreenshotProcessed = false;
    FScreenshotRequest::RequestScreenshot(PendingCapture, true, false);
    Event(TEXT("screenshot_requested"), PendingCapture); return false;
}
void UMAYAPlayerPackageUIScreenSmoke::Rendered(AHUD* H, UCanvas*)
{
    const auto* P = Player(); if (!P || H != P->GetHUD() || bFinished) return;
    LastRenderedFrame = GFrameCounter; LastRenderedScreen = P->GetPlayerScreen(); LastRenderedSize = ViewportSize();
}
void UMAYAPlayerPackageUIScreenSmoke::ScreenshotProcessed()
{
    if (PendingCapture.IsEmpty()) return;
    bScreenshotProcessed = true; ScreenshotProcessedFrame = GFrameCounter;
    Event(TEXT("screenshot_engine_processed"), FString::Printf(TEXT("file=%s requestFrame=%llu processedFrame=%llu dimensions=%dx%d"),
        *PendingCapture, ScreenshotRequestFrame, ScreenshotProcessedFrame, ScreenshotSize.X, ScreenshotSize.Y));
}
void UMAYAPlayerPackageUIScreenSmoke::TickComponent(float Dt, ELevelTick Tick, FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt, Tick, Function); const double Now = FPlatformTime::Seconds();
    if (bFinished)
    {
        if (!bSuccessfulFinish && Now >= Due) FPlatformMisc::RequestExit(false, TEXT("MAYA package UI failed bounded smoke"));
        return;
    }
    if (Now - Started > 360 || (Step > 0 && Now - StepStarted > 20))
    { Finish(false, TEXT("Bounded package UI loading/step deadline exceeded")); return; }
    auto* P = Player(); auto* D = Director(); if (!P) return;
    if (P->IsStartupMenuBypassedForAutomation() || P->HasStartedShift())
    { Finish(false, TEXT("Package smoke left ordinary unstarted menu scope")); return; }
    if (MenuWorld >= 0 && (GetWorld()->GetTimeSeconds() != MenuWorld || !D
        || D->Snapshot().TotalCredits != 0 || D->Snapshot().BatteryPercent != 100.0))
    { Finish(false, TEXT("Ordinary menu resources/gameplay clock changed during display smoke")); return; }
    if (!PendingCapture.IsEmpty())
    {
        if (bScreenshotProcessed && IFileManager::Get().FileSize(*PendingCapture) > 64)
        {
            TArray<uint8> Bytes; const bool Read = FFileHelper::LoadFileToArray(Bytes, *PendingCapture);
            const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
            auto UInt = [&](int32 I) { return (uint32(Bytes[I]) << 24) | (uint32(Bytes[I + 1]) << 16) | (uint32(Bytes[I + 2]) << 8) | uint32(Bytes[I + 3]); };
            if (!Check(Read && Bytes.Num() > 24 && FMemory::Memcmp(Bytes.GetData(), Signature, 8) == 0
                && UInt(16) == uint32(ScreenshotSize.X) && UInt(20) == uint32(ScreenshotSize.Y) && ScreenshotProcessedFrame >= ScreenshotRequestFrame,
                TEXT("Engine-processed PNG signature, dimensions and capture frame are valid"))) return;
            Event(TEXT("screenshot_saved"), PendingCapture); CompletedCaptures.Add(FPaths::GetBaseFilename(PendingCapture)); PendingCapture.Reset();
        }
        else return;
    }
    if (Now < Due || LastRenderedFrame == 0 || LastRenderedScreen != P->GetPlayerScreen() || LastRenderedSize != ViewportSize()) return;
    const auto* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
    const auto DisplayMatches = [&](const FIntPoint& Size, bool Confirmed)
    {
        return S && ViewportSize() == Size && WindowMode() == 2 && S->GetScreenResolution() == Size && int32(S->GetFullscreenMode()) == 2
            && (!Confirmed || (S->GetLastConfirmedScreenResolution() == Size && int32(S->GetLastConfirmedFullscreenMode()) == 2));
    };
    const auto Screen = [&](EMAYAPlayerScreen Value) { return Check(P->GetPlayerScreen() == Value, TEXT("Observed screen ") + FString::FromInt(int32(Value))); };
    switch (Step)
    {
    case 0:
    {
        if (P->GetPlayerScreen() == EMAYAPlayerScreen::StartupError) { Finish(false, TEXT("Actual packaged ordinary startup failed")); return; }
        if (P->GetPlayerScreen() != EMAYAPlayerScreen::MainMenu || !D || !D->GetPlayerPresentation().bReadyForStart) return;
        FMAYADeliveryRobotObservation O; auto* Robot = Cast<AMAYANativeDemoPawn>(P->GetPawn());
        if (!Check(Robot && Robot->ReadDeliveryState(O) && O.IsStoppedAndSupported() && O.bDoorsClosedLatched
            && GetWorld()->GetAuthGameMode() && GetWorld()->GetAuthGameMode()->IsA<AMAYADeliveryGameMode>()
            && GetWorld()->GetMapName().Contains(TEXT("Small_City")) && UGameplayStatics::IsGamePaused(this)
            && !P->HasStartedShift() && !D->Snapshot().bShiftStarted && D->Snapshot().TotalCredits == 0
            && D->Snapshot().BatteryPercent == 100.0 && D->Snapshot().Phase == EMAYADeliveryPhase::Available,
            TEXT("Configured ordinary package starts ready/closed/paused at exact $0/100% with no bypass"))) return;
        if (!Check(!bAnyUIAction && DisplayMatches(Wanted, true), bVerify
            ? TEXT("Second process loaded actual kept viewport/window/current and confirmed settings before any UI action")
            : TEXT("Fresh isolated first process uses actual1280x720 Windowed packaged defaults"))) return;
        MenuWorld = GetWorld()->GetTimeSeconds(); StartupDisplay = DisplayReceipt();
        Event(bVerify ? TEXT("persisted_display_observed_before_any_ui") : TEXT("first_run_defaults_observed"));
        Capture(bVerify ? TEXT("01-second-startup-kept") : TEXT("01-first-startup-defaults")); Advance(); break;
    }
    case 1:
        if (!Screen(EMAYAPlayerScreen::MainMenu) || !Click(TEXT("Settings"))) return;
        if (bVerify) { KeptDisplay = StartupDisplay; Wanted = Baseline; }
        Advance(); break;
    case 2:
    {
        if (!Screen(EMAYAPlayerScreen::Settings)) return;
        FIntPoint Row;
        if (!Check(ReadResolutionRow(Row), TEXT("Resolution is observed from the actual enabled supported settings row"))) return;
        if ((!bVerify && Row == Baseline) || (bVerify && Row != Baseline))
        {
            if (!Check(++ResolutionSelections <= 8, TEXT("A distinct supported resolution/baseline is available within8 real row selections"))) return;
            if (!Click(TEXT("Resolution.Next"))) return; Due = Now + .3; break;
        }
        if (!bVerify) Wanted = Row;
        if (!Check(bVerify || Wanted != Baseline, TEXT("First kept resolution differs from the fresh baseline"))) return;
        Advance(.1); break;
    }
    case 3:
        if (!Screen(EMAYAPlayerScreen::Settings) || !Click(TEXT("Apply"))) return;
        Advance(.1); break;
    case 4:
        if (P->GetPlayerScreen() != EMAYAPlayerScreen::VideoConfirmation)
        {
            if (Now - StepStarted > 10) Finish(false, TEXT("Actual changed display confirmation never became available"));
            return;
        }
        if (!DisplayMatches(Wanted, false)) return;
        if (!Check(P->GetVideoConfirmationSeconds() > 1.0, TEXT("Actual resized display is observed before the product confirmation deadline"))) return;
        Event(TEXT("actual_resized_display_before_keep"), FString::Printf(TEXT("viewport=%dx%d mode=%d"), ViewportSize().X, ViewportSize().Y, WindowMode()));
        if (!Click(TEXT("KeepDisplay"))) return; Advance(); break;
    case 5:
        if (P->GetPlayerScreen() != EMAYAPlayerScreen::Settings || !DisplayMatches(Wanted, true)) return;
        if (bVerify) RestoredDisplay = DisplayReceipt(); else KeptDisplay = DisplayReceipt();
        Event(bVerify ? TEXT("actual_baseline_restored_after_keep") : TEXT("actual_changed_display_kept"));
        Capture(bVerify ? TEXT("02-baseline-restored") : TEXT("02-kept-display")); Advance(); break;
    case 6:
        if (!Screen(EMAYAPlayerScreen::Settings) || !Click(TEXT("Back"))) return; Advance(); break;
    case 7:
        if (!Screen(EMAYAPlayerScreen::MainMenu) || !Click(TEXT("Quit"))) return; Advance(); break;
    case 8:
        if (!Screen(EMAYAPlayerScreen::QuitConfirmation)) return;
        Capture(TEXT("03-quit-confirmation")); Advance(); break;
    case 9:
        if (!Screen(EMAYAPlayerScreen::QuitConfirmation) || !Click(TEXT("ConfirmQuit"))) return;
        bQuitRequested = IsEngineExitRequested();
        if (!Check(bQuitRequested, TEXT("Actual product ConfirmQuit requested engine exit without helper fallback"))) return;
        Finish(true, bVerify ? TEXT("Second ordinary package loaded kept display before UI, restored baseline through Apply/Keep and quit")
            : TEXT("Fresh ordinary package defaults observed, different supported display kept through real UI and product Quit")); break;
    default: Finish(false, TEXT("Unknown packaged display smoke stage")); break;
    }
}
void UMAYAPlayerPackageUIScreenSmoke::EndPlay(const EEndPlayReason::Type Reason)
{
    if (RenderHandle.IsValid()) AHUD::OnHUDPostRender.Remove(RenderHandle);
    if (ScreenshotHandle.IsValid()) FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    Super::EndPlay(Reason);
}
