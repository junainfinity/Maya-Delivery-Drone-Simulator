#include "MAYADeliveryPlayerController.h"
#include "MAYAPlayerUIScreenTest.h"
#include "MAYAPlayerPackageUIScreenSmoke.h"
#include "MAYAPlayerNewShiftReloadTest.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYANativeDemoPawn.h"
#include "MAYAPlayerPresentation.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MAYADeliveryInputObserver.h"
#include "Util/ICitySampleInputInterface.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

AMAYADeliveryPlayerController::AMAYADeliveryPlayerController()
{
    bShouldPerformFullTickWhenPaused = true;
    bShowMouseCursor = false;
}

void AMAYADeliveryPlayerController::BeginPlay()
{
    Super::BeginPlay();
    PresentationSettings = NewObject<UMAYAPresentationSettings>(this);
    PresentationSettings->LoadPreferences();
    PresentationSettings->ApplyAudio(GetWorld());
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    bAutomationMenuBypass = FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        && !UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(this);
#endif
    bShiftStarted = bAutomationMenuBypass;
    UpdateInputOwnership();
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (IsLocalController() && UMAYAPlayerNewShiftReloadTest::IsEnabled())
    {
        auto* Reload = NewObject<UMAYAPlayerNewShiftReloadTest>(this, TEXT("MayaBoundedNewShiftReloadTest"));
        AddInstanceComponent(Reload); Reload->RegisterComponent();
    }
#endif
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01UIScreenTest")))
    {
        auto* UITest = NewObject<UMAYAPlayerUIScreenTest>(this,TEXT("MayaBoundedUIScreenTest"));
        AddInstanceComponent(UITest); UITest->RegisterComponent();
    }
#endif
#if !WITH_EDITOR && !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (IsLocalController() && FParse::Param(FCommandLine::Get(), TEXT("MayaV01PackageUIScreenSmoke")))
    {
        auto* Smoke = NewObject<UMAYAPlayerPackageUIScreenSmoke>(this,TEXT("MayaBoundedPackageUIScreenSmoke"));
        AddInstanceComponent(Smoke); Smoke->RegisterComponent();
    }
#endif
#if !UE_BUILD_SHIPPING
    if (IsLocalController() && FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput")))
    {
        InputObserver = NewObject<UMAYADeliveryInputObserver>(this, TEXT("MayaInputObservation"));
        if (InputObserver->Initialize(this))
        {
            AddInstanceComponent(InputObserver);
            InputObserver->RegisterComponent();
        }
        else InputObserver = nullptr;
    }
#endif
}

bool AMAYADeliveryPlayerController::InputKey(const FInputKeyParams& Params)
{
    bool bHandled = false;
    // FlushPressedKeys releases Unreal's cached input, not a physically held key.
    // Keep each held drive key suppressed across menus until its real callback releases it.
    const bool DriveKey=Params.Key==EKeys::W || Params.Key==EKeys::S || Params.Key==EKeys::A || Params.Key==EKeys::D
        || Params.Key==EKeys::SpaceBar || Params.Key==EKeys::LeftShift || Params.Key==EKeys::RightShift;
    if (DriveKey)
    {
        if (Params.Event==IE_Released) { HeldDriveKeys.Remove(Params.Key); SuppressedDriveKeys.Remove(Params.Key); }
        else if (Params.Event==IE_Pressed || Params.Event==IE_Repeat)
        {
            HeldDriveKeys.Add(Params.Key);
            if (Screens.IsOpen() || !bShiftStarted) SuppressedDriveKeys.Add(Params.Key);
        }
    }
    if (Screens.IsOpen() && Params.Key != EKeys::LeftMouseButton)
    {
        bHandled = true;
        if (Params.Key == EKeys::LeftShift || Params.Key == EKeys::RightShift)
            bMenuShiftHeld = Params.Event != IE_Released;
        if (Params.Event == IE_Pressed || Params.Event == IE_Repeat)
        {
            const auto Actions = GetMenuActions();
            if (Params.Key == EKeys::Escape && Params.Event==IE_Pressed) BackScreen();
            else if (Params.Key == EKeys::M && Params.Event==IE_Pressed && Screens.Current()==EMAYAPlayerScreen::CityMap) BackScreen();
            else if (Params.Key == EKeys::P && Params.Event==IE_Pressed && Screens.Current() == EMAYAPlayerScreen::Pause) ActivateUIAction(TEXT("Resume"));
            else if (Params.Key == EKeys::Tab || Params.Key == EKeys::Down || Params.Key == EKeys::Up)
                Screens.MoveFocus(Params.Key == EKeys::Up || (Params.Key == EKeys::Tab && bMenuShiftHeld) ? -1 : 1, Actions.Num());
            else if (Actions.IsValidIndex(Screens.FocusIndex))
            {
                const auto& A = Actions[Screens.FocusIndex];
                if ((Params.Key == EKeys::Enter || Params.Key == EKeys::SpaceBar) && Params.Event==IE_Pressed) ActivateUIAction(A.Id);
                else if (A.bAdjustable && (Params.Key == EKeys::Left || Params.Key == EKeys::Right))
                    ActivateUIAction(A.Id, Params.Key == EKeys::Left ? -1 : 1);
            }
        }
    }
    else if ((!bShiftStarted && !Screens.IsOpen()) || (DriveKey && SuppressedDriveKeys.Contains(Params.Key))) bHandled = true;
    else bHandled = Super::InputKey(Params);
    if (InputObserver) InputObserver->RecordInputCallback(Params, bHandled);
    return bHandled;
}

void AMAYADeliveryPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindKey(EKeys::M, IE_Pressed, this, &AMAYADeliveryPlayerController::ToggleMap).bExecuteWhenPaused = true;
    InputComponent->BindKey(EKeys::C, IE_Pressed, this, &AMAYADeliveryPlayerController::ToggleCamera);
    InputComponent->BindKey(EKeys::J, IE_Pressed, this, &AMAYADeliveryPlayerController::Accept);
    InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AMAYADeliveryPlayerController::Interact);
    InputComponent->BindKey(EKeys::N, IE_Pressed, this, &AMAYADeliveryPlayerController::Next);
    InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &AMAYADeliveryPlayerController::Retry);
    InputComponent->BindKey(EKeys::R, IE_Pressed, this, &AMAYADeliveryPlayerController::Recover);
    InputComponent->BindKey(EKeys::P, IE_Pressed, this, &AMAYADeliveryPlayerController::TogglePause).bExecuteWhenPaused = true;
    InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AMAYADeliveryPlayerController::TogglePause).bExecuteWhenPaused = true;
}

void AMAYADeliveryPlayerController::RefreshPawnContext()
{
    if (!IsLocalController() || !GetPawn() || ContextPawn.Get() == GetPawn()) return;
    ULocalPlayer* Local = GetLocalPlayer();
    UEnhancedInputLocalPlayerSubsystem* Subsystem = Local ? Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
    if (!Subsystem || !GetPawn()->GetClass()->ImplementsInterface(UCitySampleInputInterface::StaticClass())) return;
    RemovePawnContext();
    const TScriptInterface<IEnhancedInputSubsystemInterface> Interface = Subsystem;
    ICitySampleInputInterface::Execute_AddInputContext(GetPawn(), Interface);
    ContextPawn = GetPawn();
    UpdateInputOwnership();
}

void AMAYADeliveryPlayerController::RemovePawnContext()
{
    ULocalPlayer* Local = GetLocalPlayer();
    UEnhancedInputLocalPlayerSubsystem* Subsystem = Local ? Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
    if (ContextPawn.IsValid() && Subsystem)
    {
        const TScriptInterface<IEnhancedInputSubsystemInterface> Interface = Subsystem;
        ICitySampleInputInterface::Execute_RemoveInputContext(ContextPawn.Get(), Interface);
    }
    ContextPawn.Reset();
}

void AMAYADeliveryPlayerController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);
    RefreshPawnContext();
}

void AMAYADeliveryPlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
    // UE resets its stacked input-ignore flags at the start of ClientRestart.
    // Invalidate our matching ownership first so callbacks or the final refresh
    // reapply one menu claim after that reset, without accumulating claims.
    bMenuInputOwned = false;
    Super::ClientRestart_Implementation(NewPawn);
    UpdateInputOwnership();
}

void AMAYADeliveryPlayerController::OnUnPossess()
{
    RemovePawnContext();
    Super::OnUnPossess();
}

void AMAYADeliveryPlayerController::Tick(float Dt)
{
    Super::Tick(Dt);
    RefreshPawnContext();
    AMAYADeliveryDirector* D = Delivery();
    const auto* Mode = GetWorld() ? Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()) : nullptr;
    if (!bShiftStarted && !Screens.IsOpen())
    {
        if (Mode && Mode->HasStoppedDeliveryBootstrap()) OpenScreen(EMAYAPlayerScreen::StartupError, true);
        else if (Mode && Mode->HasCompletedDeliveryBootstrap() && D && D->GetPlayerPresentation().bReadyForStart)
            OpenScreen(EMAYAPlayerScreen::MainMenu, true);
    }
    if (bVideoConfirmationPending && FPlatformTime::Seconds() >= VideoConfirmationDeadline) ConfirmDisplay(false);
    if (bShiftStarted && !bShiftLostPresented && !Screens.IsOpen() && D && D->GetPlayerPresentation().bDroneTotalled)
    { OpenScreen(EMAYAPlayerScreen::ShiftLost,true); bShiftLostPresented=Screens.Current()==EMAYAPlayerScreen::ShiftLost; }
    if (bShiftStarted && !bShiftWinPresented && !Screens.IsOpen() && D
        && D->GetPlayerPresentation().bShiftWon && D->GetPlayerPresentation().bCanStartNewShift)
    { OpenScreen(EMAYAPlayerScreen::ShiftWin,true); bShiftWinPresented=Screens.Current()==EMAYAPlayerScreen::ShiftWin; }
    UpdateInputOwnership();
    const double Now = FPlatformTime::Seconds();
    if (PresentationSettings && Now - LastAudioApplyRealSeconds >= 1)
    { PresentationSettings->ApplyAudio(GetWorld()); LastAudioApplyRealSeconds = Now; }
}

AMAYADeliveryDirector* AMAYADeliveryPlayerController::Delivery() const
{
    for (TActorIterator<AMAYADeliveryDirector> It(GetWorld()); It; ++It) return *It;
    return nullptr;
}

void AMAYADeliveryPlayerController::Accept()
{
    if (!bShiftStarted || Screens.IsOpen()) return;
    auto* D = Delivery(); const bool bAccepted = D && D->RequestAccept();
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::J, bAccepted);
}
void AMAYADeliveryPlayerController::Interact()
{
    if (!bShiftStarted || Screens.IsOpen()) return;
    auto* D = Delivery(); const bool bAccepted = D && D->RequestInteract();
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::E, bAccepted);
}
void AMAYADeliveryPlayerController::Next()
{
    if (!bShiftStarted || Screens.IsOpen()) return;
    auto* D = Delivery(); const bool bAccepted = D && (D->Snapshot().Phase == EMAYADeliveryPhase::Available
        ? D->RequestSelectOffer(1) : D->RequestNext());
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::N, bAccepted);
}
void AMAYADeliveryPlayerController::Retry()
{
    if (!bShiftStarted || Screens.IsOpen()) return;
    auto* D = Delivery(); const bool bAccepted = D && D->RequestNext(true);
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::Enter, bAccepted);
}
void AMAYADeliveryPlayerController::Recover()
{
    if (!bShiftStarted || Screens.IsOpen()) return;
    auto* D = Delivery(); const bool bAccepted = D && D->RequestRecover();
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::R, bAccepted);
}
void AMAYADeliveryPlayerController::TogglePause()
{
    bool bAccepted = false;
    if (bShiftStarted)
    {
        if (Screens.Current() == EMAYAPlayerScreen::Pause) bAccepted = ActivateUIAction(TEXT("Resume"));
        else if (!Screens.IsOpen()) { OpenScreen(EMAYAPlayerScreen::Pause, true); bAccepted = Screens.IsOpen(); }
    }
    if (InputObserver) InputObserver->RecordActionCallback(EKeys::P, bAccepted);
}

bool AMAYADeliveryPlayerController::ChangePause(bool bPause)
{
    const bool bAccepted = UGameplayStatics::IsGamePaused(this) == bPause || UGameplayStatics::SetGamePaused(this, bPause);
    if (bAccepted) if (auto* D = Delivery()) D->SetPaused(bPause);
    return bAccepted;
}

void AMAYADeliveryPlayerController::UpdateInputOwnership()
{
    const bool bOwn = Screens.IsOpen() || !bShiftStarted;
    if (bOwn != bMenuInputOwned)
    {
        SetIgnoreMoveInput(bOwn);
        SetIgnoreLookInput(bOwn);
        bMenuInputOwned = bOwn;
        if (bOwn) SuppressedDriveKeys.Append(HeldDriveKeys);
        bMenuShiftHeld = false;
        if (PlayerInput) PlayerInput->FlushPressedKeys();
        if (auto* Robot = Cast<AMAYANativeDemoPawn>(GetPawn())) Robot->ReleaseDriveCommand();
    }
    auto* D=Delivery();
    const bool bContextCursor = bShiftStarted && !bAutomationMenuBypass && D && D->Observation().IsStoppedAndSupported()
        && (D->Snapshot().Phase==EMAYADeliveryPhase::Available || D->Snapshot().Phase==EMAYADeliveryPhase::Succeeded
            || D->Snapshot().Phase==EMAYADeliveryPhase::Failed || D->GetRingObservation().bInside);
    const bool bCursor=Screens.IsOpen() || bContextCursor;
    bShowMouseCursor=bEnableClickEvents=bEnableMouseOverEvents=bCursor;
    if (!bInputModeInitialized || bCursor!=bCursorModeActive)
    {
        bInputModeInitialized = true;
        bCursorModeActive=bCursor;
        if (bCursor) SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false).SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock));
        else SetInputMode(FInputModeGameOnly());
    }
}

void AMAYADeliveryPlayerController::OpenScreen(EMAYAPlayerScreen Screen, bool bRoot)
{
    if (Screen == EMAYAPlayerScreen::ShiftLost)
    {
        // Freeze gameplay authority/input, not the genuine physical aftermath.
        // Explicit settings/controls/quit keep their existing pause behavior.
        if (bRoot) Screens.SetRoot(Screen); else Screens.Push(Screen);
        MenuStatus.Reset(); UpdateInputOwnership(); return;
    }
    if (!ChangePause(true)) { MenuStatus = TEXT("Unable to pause the current game."); return; }
    if (bRoot) Screens.SetRoot(Screen); else Screens.Push(Screen);
    if (Screen == EMAYAPlayerScreen::Settings) ReadWorkingSettings();
    MenuStatus.Reset();
    UpdateInputOwnership();
}

void AMAYADeliveryPlayerController::BackScreen()
{
    switch (Screens.Current())
    {
    case EMAYAPlayerScreen::MainMenu:
    case EMAYAPlayerScreen::ShiftWin:
    case EMAYAPlayerScreen::ShiftLost:
    case EMAYAPlayerScreen::StartupError: return;
    case EMAYAPlayerScreen::VideoConfirmation: ConfirmDisplay(false); return;
    case EMAYAPlayerScreen::Pause: ActivateUIAction(TEXT("Resume")); return;
    case EMAYAPlayerScreen::Settings: ReadWorkingSettings(); break;
    default: break;
    }
    Screens.Back();
    if (!Screens.IsOpen()) ChangePause(false);
    MenuStatus.Reset();
    UpdateInputOwnership();
}

TArray<FIntPoint> AMAYADeliveryPlayerController::SupportedResolutions() const
{
    TArray<FIntPoint> Values;
    UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
    const FIntPoint Desktop = S ? S->GetDesktopResolution() : FIntPoint(1920, 1080);
    for (const auto Size : { FIntPoint(1280,720), FIntPoint(1600,900), FIntPoint(1920,1080), FIntPoint(2560,1440) })
        if (Size.X <= Desktop.X && Size.Y <= Desktop.Y) Values.Add(Size);
    if (Values.IsEmpty()) Values.Add(FIntPoint(1280,720));
    if (WorkingSettings.Resolution.X >= 960 && WorkingSettings.Resolution.Y >= 540) Values.AddUnique(WorkingSettings.Resolution);
    return Values;
}

void AMAYADeliveryPlayerController::ReadWorkingSettings()
{
    if (const UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
    {
        WorkingSettings.Resolution = S->GetScreenResolution();
        WorkingSettings.WindowMode = int32(S->GetFullscreenMode());
        WorkingSettings.bVSync = S->IsVSyncEnabled();
    }
    if (GetLocalPlayer() && GetLocalPlayer()->ViewportClient)
    {
        UGameViewportClient* V = GetLocalPlayer()->ViewportClient.Get();
        if (V->Viewport) WorkingSettings.Resolution = V->Viewport->GetSizeXY();
        if (const TSharedPtr<SWindow> Window = V->GetWindow()) WorkingSettings.WindowMode = int32(Window->GetWindowMode());
    }
    if (PresentationSettings)
    {
        WorkingSettings.MasterVolume = PresentationSettings->MasterVolume;
        WorkingSettings.InterfaceScale = PresentationSettings->InterfaceScale;
        WorkingSettings.bLargeText = PresentationSettings->bLargeText;
        WorkingSettings.bHighContrast = PresentationSettings->bHighContrast;
    }
}

bool AMAYADeliveryPlayerController::ApplyWorkingSettings()
{
    UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
    if (!S || !PresentationSettings || bVideoConfirmationPending) return false;
    PreviousSettings.Resolution = S->GetScreenResolution();
    PreviousSettings.WindowMode = int32(S->GetFullscreenMode());
    if (GetLocalPlayer() && GetLocalPlayer()->ViewportClient)
    {
        UGameViewportClient* V = GetLocalPlayer()->ViewportClient.Get();
        if (V->Viewport) PreviousSettings.Resolution = V->Viewport->GetSizeXY();
        if (const TSharedPtr<SWindow> Window = V->GetWindow()) PreviousSettings.WindowMode = int32(Window->GetWindowMode());
    }
    const bool bDisplayChanged = PreviousSettings.Resolution != WorkingSettings.Resolution || PreviousSettings.WindowMode != WorkingSettings.WindowMode;
    S->SetScreenResolution(WorkingSettings.Resolution);
    S->SetFullscreenMode(EWindowMode::Type(FMath::Clamp(WorkingSettings.WindowMode,0,2)));
    S->SetVSyncEnabled(WorkingSettings.bVSync);
    S->ApplySettings(false);
    PresentationSettings->MasterVolume = FMath::Clamp(WorkingSettings.MasterVolume,0.f,1.f);
    PresentationSettings->InterfaceScale = FMath::Clamp(WorkingSettings.InterfaceScale,.9f,1.1f);
    PresentationSettings->bLargeText = WorkingSettings.bLargeText;
    PresentationSettings->bHighContrast = WorkingSettings.bHighContrast;
    PresentationSettings->ApplyAudio(GetWorld());
    PresentationSettings->SavePreferences();
    if (bDisplayChanged)
    {
        bVideoConfirmationPending = true;
        VideoConfirmationDeadline = FPlatformTime::Seconds() + 15;
        Screens.Push(EMAYAPlayerScreen::VideoConfirmation);
        Screens.FocusIndex = 1; // Revert is the conservative default.
        MenuStatus = TEXT("Keep this display mode? It will revert automatically.");
    }
    else { S->ConfirmVideoMode(); S->SaveSettings(); MenuStatus = TEXT("Settings applied."); }
    return true;
}

void AMAYADeliveryPlayerController::ConfirmDisplay(bool bKeep)
{
    if (!bVideoConfirmationPending) return;
    if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
    {
        if (!bKeep)
        {
            S->SetScreenResolution(PreviousSettings.Resolution);
            S->SetFullscreenMode(EWindowMode::Type(PreviousSettings.WindowMode));
            S->ApplyResolutionSettings(false);
        }
        S->ConfirmVideoMode(); S->SaveSettings();
    }
    bVideoConfirmationPending = false;
    if (Screens.Current() == EMAYAPlayerScreen::VideoConfirmation) Screens.Back();
    ReadWorkingSettings();
    MenuStatus = bKeep ? TEXT("Display mode kept.") : TEXT("Previous display mode restored.");
    UpdateInputOwnership();
}

double AMAYADeliveryPlayerController::GetVideoConfirmationSeconds() const
{
    return bVideoConfirmationPending ? FMath::Max(0.0, VideoConfirmationDeadline - FPlatformTime::Seconds()) : 0;
}

TArray<FMAYAPlayerMenuAction> AMAYADeliveryPlayerController::GetMenuActions() const
{
    TArray<FMAYAPlayerMenuAction> A;
    auto Add = [&](const TCHAR* Id, const TCHAR* Label, bool Enabled = true, const FString& Value = FString(), bool Adjustable = false)
    { A.Add({ FName(Id), FString(Label), Value, Enabled, Adjustable }); };
    switch (Screens.Current())
    {
    case EMAYAPlayerScreen::MainMenu:
        if (Delivery() && (Delivery()->GetPlayerPresentation().bShiftWon || Delivery()->GetPlayerPresentation().bDroneTotalled))
            Add(TEXT("NewShift"),Delivery()->GetPlayerPresentation().bDroneTotalled?TEXT("START OVER"):TEXT("START NEW SHIFT"),Delivery()->GetPlayerPresentation().bCanStartNewShift);
        else Add(TEXT("StartShift"),TEXT("START SHIFT"),Delivery() && Delivery()->GetPlayerPresentation().bReadyForStart);
        Add(TEXT("Controls"),TEXT("CONTROLS")); Add(TEXT("Settings"),TEXT("SETTINGS"));
        Add(TEXT("JoinProject"),TEXT("JOIN THE PROJECT"),true,TEXT("quantellics.com/maya")); Add(TEXT("Quit"),TEXT("QUIT GAME")); break;
    case EMAYAPlayerScreen::Pause:
        Add(TEXT("Resume"),TEXT("RESUME DELIVERY")); Add(TEXT("Map"),TEXT("CITY MAP")); Add(TEXT("Controls"),TEXT("CONTROLS"));
        Add(TEXT("Settings"),TEXT("SETTINGS")); Add(TEXT("Quit"),TEXT("QUIT GAME")); break;
    case EMAYAPlayerScreen::CityMap:
        Add(TEXT("MapFocus"),bMapRouteFocused?TEXT("SHOW WHOLE CITY"):TEXT("ZOOM TO DELIVERY")); Add(TEXT("Back"),TEXT("BACK")); break;
    case EMAYAPlayerScreen::Controls: Add(TEXT("Back"),TEXT("BACK")); break;
    case EMAYAPlayerScreen::Settings:
        Add(TEXT("Resolution"),TEXT("Resolution"),true,FString::Printf(TEXT("%d x %d"),WorkingSettings.Resolution.X,WorkingSettings.Resolution.Y),true);
        Add(TEXT("WindowMode"),TEXT("Window mode"),true,WorkingSettings.WindowMode == 0 ? TEXT("Fullscreen") : WorkingSettings.WindowMode == 1 ? TEXT("Borderless") : TEXT("Windowed"),true);
        Add(TEXT("VSync"),TEXT("V-Sync"),true,WorkingSettings.bVSync ? TEXT("On") : TEXT("Off"),true);
        Add(TEXT("Volume"),TEXT("Master volume"),true,FString::Printf(TEXT("%.0f%%"),100*WorkingSettings.MasterVolume),true);
        Add(TEXT("UIScale"),TEXT("UI scale"),true,FString::Printf(TEXT("%.0f%%"),100*WorkingSettings.InterfaceScale),true);
        Add(TEXT("TextSize"),TEXT("Text size"),true,WorkingSettings.bLargeText ? TEXT("Large") : TEXT("Standard"),true);
        Add(TEXT("Contrast"),TEXT("High contrast"),true,WorkingSettings.bHighContrast ? TEXT("On") : TEXT("Off"),true);
        Add(TEXT("Apply"),TEXT("APPLY CHANGES")); Add(TEXT("Defaults"),TEXT("RESET DEFAULTS")); Add(TEXT("Back"),TEXT("BACK")); break;
    case EMAYAPlayerScreen::QuitConfirmation:
        Add(TEXT("Back"),TEXT("KEEP PLAYING")); Add(TEXT("ConfirmQuit"),TEXT("QUIT GAME")); break;
    case EMAYAPlayerScreen::NewShiftConfirmation:
        Add(TEXT("Back"),TEXT("KEEP CURRENT SHIFT"));
        Add(TEXT("ConfirmNewShift"),Delivery() && Delivery()->GetPlayerPresentation().bDroneTotalled?TEXT("START OVER"):TEXT("START NEW SHIFT"),Delivery() && Delivery()->GetPlayerPresentation().bCanStartNewShift); break;
    case EMAYAPlayerScreen::VideoConfirmation:
        Add(TEXT("KeepDisplay"),TEXT("KEEP CHANGES")); Add(TEXT("RevertDisplay"),TEXT("REVERT")); break;
    case EMAYAPlayerScreen::StartupError:
        Add(TEXT("RetryStartup"),TEXT("TRY AGAIN")); Add(TEXT("Quit"),TEXT("QUIT GAME")); break;
    case EMAYAPlayerScreen::ShiftLost:
        Add(TEXT("NewShift"),TEXT("START OVER"),Delivery() && Delivery()->GetPlayerPresentation().bCanStartNewShift);
        Add(TEXT("Controls"),TEXT("CONTROLS")); Add(TEXT("Settings"),TEXT("SETTINGS")); Add(TEXT("Quit"),TEXT("QUIT GAME")); break;
    case EMAYAPlayerScreen::ShiftWin:
        Add(TEXT("NewShift"),TEXT("PLAY AGAIN"),Delivery() && Delivery()->GetPlayerPresentation().bCanStartNewShift);
        Add(TEXT("MainMenu"),TEXT("MAIN MENU")); Add(TEXT("Controls"),TEXT("CONTROLS")); Add(TEXT("Quit"),TEXT("QUIT GAME")); break;
    default: break;
    }
    return A;
}

void AMAYADeliveryPlayerController::SetMenuFocus(FName Action)
{
    const auto Actions = GetMenuActions();
    for (int32 I=0; I<Actions.Num(); ++I) if (Actions[I].Id == Action) { Screens.FocusIndex=I; break; }
}

bool AMAYADeliveryPlayerController::ActivateUIAction(FName Action, int32 Direction)
{
    const auto Actions = GetMenuActions();
    const auto* MenuAction = Actions.FindByPredicate([&](const FMAYAPlayerMenuAction& A){return A.Id==Action;});
    if (Screens.IsOpen() && (!MenuAction || !MenuAction->bEnabled)) return false;
    AMAYADeliveryDirector* D = Delivery();
    if (Action == TEXT("StartShift"))
    {
        if (!D || !D->GetPlayerPresentation().bReadyForStart || !ChangePause(false)) return false;
        if (!D->StartShift()) { ChangePause(true); return false; }
        bShiftStarted=true; Screens.SetRoot(EMAYAPlayerScreen::Gameplay); UpdateInputOwnership(); return true;
    }
    if (Action == TEXT("Resume"))
    {
        if (!bShiftStarted || (D && D->GetPlayerPresentation().bDroneTotalled) || !ChangePause(false)) return false;
        Screens.SetRoot(EMAYAPlayerScreen::Gameplay); UpdateInputOwnership(); return true;
    }
    if (Action == TEXT("Map"))
    {
        if (!bShiftStarted || !D || D->GetPlayerPresentation().bDroneTotalled || (Screens.IsOpen() && Screens.Current()!=EMAYAPlayerScreen::Pause)) return false;
        bMapRouteFocused=false; OpenScreen(EMAYAPlayerScreen::CityMap); return Screens.Current()==EMAYAPlayerScreen::CityMap;
    }
    if (Action == TEXT("MapFocus"))
    { if(Screens.Current()!=EMAYAPlayerScreen::CityMap)return false; bMapRouteFocused=!bMapRouteFocused; return true; }
    if (Action == TEXT("Camera"))
    {
        if(!bShiftStarted || Screens.IsOpen() || !D || D->GetPlayerPresentation().bDroneTotalled)return false;
        if(auto* Robot=Cast<AMAYANativeDemoPawn>(GetPawn()))return Robot->TogglePlayerCameraMode();
        return false;
    }
    if (Action == TEXT("Controls")) { OpenScreen(EMAYAPlayerScreen::Controls); return true; }
    if (Action == TEXT("MainMenu"))
    {
        if (!D || !D->GetPlayerPresentation().bShiftWon || Screens.Current()!=EMAYAPlayerScreen::ShiftWin) return false;
        OpenScreen(EMAYAPlayerScreen::MainMenu,true); return Screens.Current()==EMAYAPlayerScreen::MainMenu;
    }
    if (Action == TEXT("JoinProject"))
    {
        if (Screens.Current()!=EMAYAPlayerScreen::MainMenu) return false;
        FString Error;
        FPlatformProcess::LaunchURL(TEXT("https://quantellics.com/maya"),nullptr,&Error);
        MenuStatus=Error.IsEmpty() ? TEXT("Opening quantellics.com/maya in your default browser. Your game remains here.")
            : TEXT("Could not open your browser. Visit https://quantellics.com/maya. ")+Error;
        return Error.IsEmpty();
    }
    if (Action == TEXT("Settings")) { OpenScreen(EMAYAPlayerScreen::Settings); return true; }
    if (Action == TEXT("Quit")) { OpenScreen(EMAYAPlayerScreen::QuitConfirmation); return true; }
    if (Action == TEXT("Back")) { BackScreen(); return true; }
    if (Action == TEXT("ConfirmQuit"))
    { UKismetSystemLibrary::QuitGame(this,this,EQuitPreference::Quit,false); return true; }
    if (Action == TEXT("RetryStartup"))
    {
        const auto* Mode=GetWorld()?Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()):nullptr;
        if (!Mode || !Mode->HasStoppedDeliveryBootstrap()) return false;
        ChangePause(false);
        UGameplayStatics::OpenLevel(this,FName(TEXT("/Game/Map/Small_City_LVL")),true,TEXT("game=/Script/DDSCity.MAYADeliveryGameMode"));
        return true;
    }
    if (Action == TEXT("NewShift"))
    {
        if (!bShiftStarted || !D || !D->GetPlayerPresentation().bCanStartNewShift) return false;
        if (!D->GetPlayerPresentation().bShiftWon)
        { OpenScreen(EMAYAPlayerScreen::NewShiftConfirmation, true); return Screens.Current()==EMAYAPlayerScreen::NewShiftConfirmation; }
        if (!ChangePause(false)) return false;
        UGameplayStatics::OpenLevel(this,FName(TEXT("/Game/Map/Small_City_LVL")),true,TEXT("game=/Script/DDSCity.MAYADeliveryGameMode"));
        return true;
    }
    if (Action == TEXT("ConfirmNewShift"))
    {
        if (Screens.Current()!=EMAYAPlayerScreen::NewShiftConfirmation || !D || !D->GetPlayerPresentation().bCanStartNewShift || !ChangePause(false)) return false;
        UGameplayStatics::OpenLevel(this,FName(TEXT("/Game/Map/Small_City_LVL")),true,TEXT("game=/Script/DDSCity.MAYADeliveryGameMode"));
        return true;
    }
    if (Action == TEXT("KeepDisplay") || Action == TEXT("RevertDisplay")) { ConfirmDisplay(Action==TEXT("KeepDisplay")); return true; }
    if (Screens.Current()==EMAYAPlayerScreen::Settings)
    {
        const int32 Sign=Direction<0?-1:1;
        if (Action==TEXT("Resolution"))
        { const auto Values=SupportedResolutions(); int32 I=Values.IndexOfByKey(WorkingSettings.Resolution); WorkingSettings.Resolution=Values[(FMath::Max(0,I)+Sign+Values.Num())%Values.Num()]; }
        else if (Action==TEXT("WindowMode")) WorkingSettings.WindowMode=(WorkingSettings.WindowMode+Sign+3)%3;
        else if (Action==TEXT("VSync")) WorkingSettings.bVSync=!WorkingSettings.bVSync;
        else if (Action==TEXT("Volume")) WorkingSettings.MasterVolume=FMath::Clamp(WorkingSettings.MasterVolume+Sign*.1f,0.f,1.f);
        else if (Action==TEXT("UIScale")) WorkingSettings.InterfaceScale=FMath::Clamp(WorkingSettings.InterfaceScale+Sign*.1f,.9f,1.1f);
        else if (Action==TEXT("TextSize")) WorkingSettings.bLargeText=!WorkingSettings.bLargeText;
        else if (Action==TEXT("Contrast")) WorkingSettings.bHighContrast=!WorkingSettings.bHighContrast;
        else if (Action==TEXT("Apply")) return ApplyWorkingSettings();
        else if (Action==TEXT("Defaults")) WorkingSettings=FMAYAPresentationWorkingSettings();
        else return false;
        MenuStatus=TEXT("Changes are pending. Select Apply Changes to save."); return true;
    }
    if (!bShiftStarted || Screens.IsOpen() || !D) return false;
    const auto View=D->GetPlayerPresentation();
    if (Action==TEXT("Accept")) return View.bCanAccept && D->RequestAccept();
    if (Action==TEXT("ChooseOffer")) return View.bCanChooseOffer && D->RequestSelectOffer(Direction);
    if (Action==TEXT("Interact")) return View.bCanInteract && D->RequestInteract();
    if (Action==TEXT("Recover")) return View.bCanRecover && D->RequestRecover();
    if (Action==TEXT("Next")) return View.bCanNext && D->RequestNext();
    if (Action==TEXT("Replay")) return View.bCanReplay && D->RequestNext(true);
    return false;
}

void AMAYADeliveryPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
    if (bVideoConfirmationPending) ConfirmDisplay(false);
    RemovePawnContext();
    Super::EndPlay(Reason);
}

void AMAYADeliveryPlayerController::ToggleMap()
{
    if(Screens.Current()==EMAYAPlayerScreen::CityMap)BackScreen();
    else ActivateUIAction(TEXT("Map"));
}

void AMAYADeliveryPlayerController::ToggleCamera()
{ ActivateUIAction(TEXT("Camera")); }
