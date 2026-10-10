#include "MAYAPlayerNewShiftReloadTest.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYAV01NativeTestDriver.h"
#include "MAYANativeDemoPawn.h"
#include "GameFramework/HUD.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/ObjectKey.h"
#include "UnrealClient.h"
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

namespace
{
    bool FatalFixture()
    {
        FString Mode;return FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),Mode)
            && (Mode==TEXT("headon")||Mode==TEXT("rearend"));
    }
    struct FReloadQAState
    {
        TWeakObjectPtr<UGameInstance> Instance;
        TWeakObjectPtr<AMAYADeliveryPlayerController> OldPlayer;
        TWeakObjectPtr<AMAYADeliveryDirector> OldDirector;
        TWeakObjectPtr<AMAYANativeDemoPawn> OldRobot;
        FObjectKey OriginWorld, PlayerKey, DirectorKey, RobotKey;
        TSet<FObjectKey> OldNativeComponents;
        FString Output, SettingsFile, OldShift;
        TArray<uint8> SettingsBytes;
        TArray<TSharedPtr<FJsonValue>> Checks;
        TSet<FString> Captures;
        TSharedPtr<FJsonObject> OldReceipt, NewReceipt, OldPreferences, NewPreferences, FirstGameplay;
        int32 Step = 0, FixtureCount = 0, DriverSpawnCount = 0, InitialComponents = 0, SuccessorComponents = 0;
        double Started = 0, Due = 0, Deadline = 0, FailExitDue = 0;
        bool bSettingsExisted = false, bArmed = false, bOriginEndPlay = false;
        bool bFinished = false, bSuccess = false, bWriteFailed = false, bQuitRequested = false, bFirstGameplay = false;
    } GReload;

    bool SameInstance(const UWorld* World)
    { return World && GReload.Instance.IsValid() && GReload.Instance.Get() == World->GetGameInstance(); }
    TSharedPtr<FJsonObject> Identity(const UObject* Object)
    {
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("path"), GetPathNameSafe(Object));
        R->SetStringField(TEXT("class"), Object ? Object->GetClass()->GetPathName() : FString());
        R->SetNumberField(TEXT("objectSerialKeyHash"), GetTypeHash(FObjectKey(Object)));
        R->SetStringField(TEXT("comparisonBasis"), TEXT("Full FObjectKey object index/serial equality; hashes are diagnostic labels, not equality tests"));
        return R;
    }
    int32 NativeComponents(AMAYANativeDemoPawn* Robot, TSet<FObjectKey>* Keys,
        TArray<TSharedPtr<FJsonValue>>* Receipts, const TSet<FObjectKey>* Reject, bool& Unique)
    {
        Unique = true; int32 Count = 0; if (!Robot) return Count;
        TArray<UPrimitiveComponent*> Components; Robot->GetComponents<UPrimitiveComponent>(Components);
        for (UPrimitiveComponent* C : Components)
        {
            FBodyInstance* BI = C ? C->GetBodyInstance() : nullptr;
            if (!BI || !BI->IsValidBodyInstance() || !C->IsSimulatingPhysics()) continue;
            ++Count; const FObjectKey Key(C);
            if (Keys) Keys->Add(Key); if (Reject && Reject->Contains(Key)) Unique = false;
            if (Receipts)
            {
                auto J = Identity(C); J->SetBoolField(TEXT("nativeBodyValid"), true);
                J->SetBoolField(TEXT("simulating"), true);
                J->SetBoolField(TEXT("notAnOldComponentSerial"), !Reject || !Reject->Contains(Key));
                Receipts->Add(MakeShared<FJsonValueObject>(J));
            }
        }
        return Count; // welded children can share native bodies; this is a component count, not unique Chaos particles.
    }
}

UMAYAPlayerNewShiftReloadTest::UMAYAPlayerNewShiftReloadTest()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}
bool UMAYAPlayerNewShiftReloadTest::IsEnabled()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if(FatalFixture())return true;
#if WITH_EDITOR
    return FParse::Param(FCommandLine::Get(), TEXT("MayaV01NewShiftReloadTest"));
#else
    return false;
#endif
#else
    return false;
#endif
}
bool UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(const UObject* Context)
{
    const UWorld* World = Context ? Context->GetWorld() : nullptr;
    return IsEnabled() && GReload.bArmed && SameInstance(World) && FObjectKey(World) != GReload.OriginWorld;
}
void UMAYAPlayerNewShiftReloadTest::NoteInitialFixtureApplied(const UObject* Context)
{
    const UWorld* World = Context ? Context->GetWorld() : nullptr;
    if (IsEnabled() && SameInstance(World) && FObjectKey(World) == GReload.OriginWorld) ++GReload.FixtureCount;
}
void UMAYAPlayerNewShiftReloadTest::NoteAutomationDriverSpawned(const UObject* Context)
{
    const UWorld* World = Context ? Context->GetWorld() : nullptr;
    if (IsEnabled() && SameInstance(World) && FObjectKey(World) == GReload.OriginWorld) ++GReload.DriverSpawnCount;
}
AMAYADeliveryPlayerController* UMAYAPlayerNewShiftReloadTest::Player() const
{ return Cast<AMAYADeliveryPlayerController>(GetOwner()); }
AMAYADeliveryDirector* UMAYAPlayerNewShiftReloadTest::Director() const
{ for (TActorIterator<AMAYADeliveryDirector> It(GetWorld()); It; ++It) return *It; return nullptr; }
FIntPoint UMAYAPlayerNewShiftReloadTest::ViewportSize() const
{
    const auto* P = Player(); const auto* L = P ? P->GetLocalPlayer() : nullptr;
    UGameViewportClient* V = L ? L->ViewportClient.Get() : nullptr;
    return V && V->Viewport ? V->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
}
void UMAYAPlayerNewShiftReloadTest::BeginPlay()
{
    Super::BeginPlay(); if (!IsEnabled()) { SetComponentTickEnabled(false); return; }
    const double Now = FPlatformTime::Seconds();
    if (!GReload.Instance.IsValid())
    {
        GReload = FReloadQAState(); GReload.Instance = GetWorld()->GetGameInstance(); GReload.OriginWorld = FObjectKey(GetWorld());
        GReload.Started = GReload.Due = Now; GReload.Deadline = Now + 240;
        FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), GReload.Output);
        if (!GReload.Output.IsEmpty()) GReload.Output = FPaths::ConvertRelativePathToFull(GReload.Output);
    }
    if (GReload.Output.IsEmpty()) { SetComponentTickEnabled(false); return; }
    IFileManager::Get().MakeDirectory(*GReload.Output, true);
    if (!Check(SameInstance(GetWorld()), TEXT("Reload observer remains in the original GameInstance"))) return;
    const TCHAR* Cmd = FCommandLine::Get();
    double Initial = -1;
    if (!Check(FatalFixture() ? FParse::Param(Cmd,TEXT("MayaV01AutoTest")) && FParse::Param(Cmd,TEXT("MayaV01TrafficEncounterTest")) && FParse::Param(Cmd,TEXT("MayaV01CurbTest"))
        : FParse::Param(Cmd, TEXT("MayaV01AutoTest")) && FParse::Param(Cmd, TEXT("MayaV01OperatingTest"))
        && FParse::Value(Cmd, TEXT("MayaV01InitialBattery="), Initial) && FMath::IsFinite(Initial) && Initial >= .5 && Initial <= 1,
        TEXT("Explicit reload selector retains the declared original AutoTest/Operating0.5-1% sequence"))) return;
    for (const TCHAR* Flag : {TEXT("MayaV01CargoTest"), TEXT("MayaV01FullInput"), TEXT("MayaV01CashShiftTest"),
        TEXT("MayaV01CargoVideo"), TEXT("MayaV01UIScreenTest"), TEXT("MayaV01PackageUIScreenSmoke"),
        TEXT("MayaV01RingInteractionTest"), TEXT("MayaV01DamageTest"), TEXT("MayaV01CurbTest"),
        TEXT("MayaV01VisualCapture"), TEXT("MayaV01ObserveInput"), TEXT("NullRHI")})
        if (FParse::Param(Cmd, Flag) && !(FatalFixture()&&FCString::Strcmp(Flag,TEXT("MayaV01CurbTest"))==0))
        { Finish(false, TEXT("Incompatible reload selector: ") + FString(Flag)); return; }
    FString Override;
    FString Expected = FPaths::ConvertRelativePathToFull(FPaths::Combine(GReload.Output, TEXT("GameUserSettings.ini")));
    FString Destination = FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"), nullptr, *FPaths::GeneratedConfigDir()));
    const bool HasOverride = FParse::Value(Cmd, TEXT("GameUserSettingsINI="), Override);
    Override = FPaths::ConvertRelativePathToFull(Override);
    FPaths::NormalizeFilename(Expected); FPaths::NormalizeFilename(Override); FPaths::NormalizeFilename(Destination);
    if (!Check(HasOverride && GConfig && GConfig->FindConfigFile(GGameUserSettingsIni)
        && Override.Equals(Expected, ESearchCase::IgnoreCase) && Destination.Equals(Expected, ESearchCase::IgnoreCase),
        TEXT("Known settings cache resolves to this explicit isolated attempt file"))) return;
    if (GReload.SettingsFile.IsEmpty()) GReload.SettingsFile = Destination;
    if (!Check(GReload.SettingsFile.Equals(Destination, ESearchCase::IgnoreCase), TEXT("Same settings destination survives OpenLevel"))) return;
    RenderHandle = AHUD::OnHUDPostRender.AddUObject(this, &UMAYAPlayerNewShiftReloadTest::Rendered);
    ScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &UMAYAPlayerNewShiftReloadTest::ScreenshotProcessed);
    if (ShouldSuppressInitialAutomation(this))
    {
        if (!Check(GReload.Step == 5 && ++GReload.SuccessorComponents == 1, TEXT("Exactly one replacement controller observer resumes the armed travel"))) return;
        GReload.Step = 6; GReload.Due = Now; GReload.Deadline = Now + 120;
        Event(TEXT("successor_controller_begin"));
    }
    else
    {
        if (!Check(FObjectKey(GetWorld()) == GReload.OriginWorld && GReload.Step == 0 && ++GReload.InitialComponents == 1,
            TEXT("Exactly one original observer waits for actual Operating completion"))) return;
        Event(TEXT("origin_controller_begin"));
    }
}
bool UMAYAPlayerNewShiftReloadTest::BeginFatalHandoff(UWorld* World,const FString& Output,const FMAYADeliveryRobotObservation& O)
{
    if(!IsEnabled()||!FatalFixture()||!SameInstance(World)||GReload.bFinished||GReload.Step!=0||GReload.bArmed
        ||FObjectKey(World)!=GReload.OriginWorld||Output!=GReload.Output||GReload.FixtureCount!=0||GReload.DriverSpawnCount!=1)return false;
    AMAYADeliveryDirector* D=nullptr;for(TActorIterator<AMAYADeliveryDirector> It(World);It;++It)D=*It;
    auto* P=Cast<AMAYADeliveryPlayerController>(World->GetFirstPlayerController());
    auto* Robot=P?Cast<AMAYANativeDemoPawn>(P->GetPawn()):nullptr;
    if(!D||!P||!Robot||!D->Snapshot().bDroneTotalled||D->Snapshot().bVehicleContactPending
        ||!D->Snapshot().VehicleCrash.Contact.IsFatal()||!D->GetPlayerPresentation().bCanStartNewShift
        ||D->GetPlayerPresentation().bCanRecover||D->GetPlayerPresentation().bCanReplay)return false;
    GReload.OldPlayer=P;GReload.OldDirector=D;GReload.OldRobot=Robot;
    GReload.PlayerKey=FObjectKey(P);GReload.DirectorKey=FObjectKey(D);GReload.RobotKey=FObjectKey(Robot);
    GReload.OldShift=D->Snapshot().ShiftInstanceId;
    bool Unique;if(NativeComponents(Robot,&GReload.OldNativeComponents,nullptr,nullptr,Unique)<9)return false;
    GReload.Step=1;GReload.Due=FPlatformTime::Seconds()+.25;GReload.Deadline=GReload.Due+20;
    return true;
}
bool UMAYAPlayerNewShiftReloadTest::BeginOperatingHandoff(UWorld* World, const FString& Output, const FMAYADeliveryRobotObservation& O)
{
    if (!IsEnabled() || !SameInstance(World) || GReload.bFinished || GReload.Step != 0 || GReload.bArmed
        || FObjectKey(World) != GReload.OriginWorld || Output != GReload.Output || GReload.FixtureCount != 1 || GReload.DriverSpawnCount != 1) return false;
    AMAYADeliveryDirector* D = nullptr;
    for (TActorIterator<AMAYADeliveryDirector> It(World); It; ++It) D = *It;
    auto* P = Cast<AMAYADeliveryPlayerController>(World->GetFirstPlayerController());
    auto* Robot = P ? Cast<AMAYANativeDemoPawn>(P->GetPawn()) : nullptr;
    int32 Drivers = 0; for (TActorIterator<AMAYAV01NativeTestDriver> It(World); It; ++It) ++Drivers;
    if (!D || !P || !Robot || Drivers != 1 || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched
        || D->Snapshot().BatteryPercent != 0 || D->Snapshot().TotalCredits != 0 || D->Snapshot().bCharging
        || D->Snapshot().LastRecharge.Generation != 0 || D->GetParcelCount() != 0 || D->GetSecuredParcelCount() != 0
        || !D->GetPlayerPresentation().bCanStartNewShift || D->GetPlayerPresentation().bCanAccept
        || !D->IsNativeOperatingBatteryFixtureApplied() || UGameplayStatics::IsGamePaused(World)) return false;
    GReload.OldPlayer = P; GReload.OldDirector = D; GReload.OldRobot = Robot;
    GReload.PlayerKey = FObjectKey(P); GReload.DirectorKey = FObjectKey(D); GReload.RobotKey = FObjectKey(Robot);
    GReload.OldShift = D->Snapshot().ShiftInstanceId;
    bool Unique; if (NativeComponents(Robot, &GReload.OldNativeComponents, nullptr, nullptr, Unique) < 9) return false;
    GReload.Step = 1; GReload.Due = FPlatformTime::Seconds() + .25; GReload.Deadline = GReload.Due + 20;
    return true;
}
TSharedPtr<FJsonObject> UMAYAPlayerNewShiftReloadTest::PreferencesReceipt() const
{
    auto R = MakeShared<FJsonObject>(); const auto* P = Player(); const auto* Prefs = P ? P->GetPresentationSettings() : nullptr;
    const auto* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
    const auto* L = P ? P->GetLocalPlayer() : nullptr; UGameViewportClient* V = L ? L->ViewportClient.Get() : nullptr;
    const TSharedPtr<SWindow> Window = V ? V->GetWindow() : nullptr;
    R->SetBoolField(TEXT("known"), Prefs && S && Window.IsValid());
    R->SetNumberField(TEXT("viewportX"), ViewportSize().X); R->SetNumberField(TEXT("viewportY"), ViewportSize().Y);
    R->SetNumberField(TEXT("windowMode"), Window.IsValid() ? int32(Window->GetWindowMode()) : -1);
    R->SetStringField(TEXT("settingsDestination"), GReload.SettingsFile);
    if (S)
    {
        R->SetNumberField(TEXT("configuredX"), S->GetScreenResolution().X); R->SetNumberField(TEXT("configuredY"), S->GetScreenResolution().Y);
        R->SetNumberField(TEXT("confirmedX"), S->GetLastConfirmedScreenResolution().X); R->SetNumberField(TEXT("confirmedY"), S->GetLastConfirmedScreenResolution().Y);
        R->SetNumberField(TEXT("configuredMode"), int32(S->GetFullscreenMode())); R->SetNumberField(TEXT("confirmedMode"), int32(S->GetLastConfirmedFullscreenMode()));
        R->SetBoolField(TEXT("vsync"), S->IsVSyncEnabled());
    }
    if (Prefs)
    {
        R->SetNumberField(TEXT("masterVolume"), Prefs->MasterVolume); R->SetNumberField(TEXT("interfaceScale"), Prefs->InterfaceScale);
        R->SetBoolField(TEXT("largeText"), Prefs->bLargeText); R->SetBoolField(TEXT("highContrast"), Prefs->bHighContrast);
    }
    return R;
}
bool UMAYAPlayerNewShiftReloadTest::PreferencesMatch() const
{
    if (!GReload.OldPreferences.IsValid()) return false;
    const auto Current = PreferencesReceipt();
    if (!Current->GetBoolField(TEXT("known")) || !GReload.OldPreferences->GetBoolField(TEXT("known"))) return false;
    for (const TCHAR* Name : {TEXT("viewportX"), TEXT("viewportY"), TEXT("windowMode"), TEXT("configuredX"), TEXT("configuredY"),
        TEXT("confirmedX"), TEXT("confirmedY"), TEXT("configuredMode"), TEXT("confirmedMode"), TEXT("masterVolume"), TEXT("interfaceScale")})
        if (Current->GetNumberField(Name) != GReload.OldPreferences->GetNumberField(Name)) return false;
    for (const TCHAR* Name : {TEXT("vsync"), TEXT("largeText"), TEXT("highContrast")})
        if (Current->GetBoolField(Name) != GReload.OldPreferences->GetBoolField(Name)) return false;
    const bool Exists = IFileManager::Get().FileExists(*GReload.SettingsFile); TArray<uint8> Bytes;
    return Exists == GReload.bSettingsExisted && (!Exists || (FFileHelper::LoadFileToArray(Bytes, *GReload.SettingsFile) && Bytes == GReload.SettingsBytes));
}
TSharedPtr<FJsonObject> UMAYAPlayerNewShiftReloadTest::WorldReceipt() const
{
    auto R = MakeShared<FJsonObject>(); auto* P = Player(); auto* D = Director(); auto* Robot = P ? Cast<AMAYANativeDemoPawn>(P->GetPawn()) : nullptr;
    R->SetObjectField(TEXT("world"), Identity(GetWorld())); R->SetObjectField(TEXT("gameInstance"), Identity(GetWorld()->GetGameInstance()));
    R->SetObjectField(TEXT("controller"), Identity(P)); R->SetObjectField(TEXT("director"), Identity(D)); R->SetObjectField(TEXT("robot"), Identity(Robot));
    R->SetNumberField(TEXT("frame"), GFrameCounter); R->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    R->SetBoolField(TEXT("paused"), UGameplayStatics::IsGamePaused(this));
    if (P) { R->SetBoolField(TEXT("menuBypass"), P->IsStartupMenuBypassedForAutomation()); R->SetBoolField(TEXT("moveIgnored"), P->IsMoveInputIgnored()); R->SetBoolField(TEXT("lookIgnored"), P->IsLookInputIgnored()); }
    if (D)
    {
        const auto& S = D->Snapshot(); R->SetStringField(TEXT("shiftId"), S.ShiftInstanceId); R->SetStringField(TEXT("jobId"), S.JobInstanceId);
        R->SetNumberField(TEXT("cash"), S.TotalCredits); R->SetNumberField(TEXT("battery"), S.BatteryPercent);
        R->SetNumberField(TEXT("condition"), S.CargoConditionPercent); R->SetNumberField(TEXT("phase"), int32(S.Phase));
        R->SetNumberField(TEXT("completedJobs"), S.CompletedJobs); R->SetNumberField(TEXT("feesPaid"), S.RechargeFeesPaidDollars);
        R->SetBoolField(TEXT("droneTotalled"),S.bDroneTotalled);R->SetBoolField(TEXT("vehicleContactPending"),S.bVehicleContactPending);
        R->SetBoolField(TEXT("crashFrozen"),S.VehicleCrash.bFrozen);R->SetNumberField(TEXT("impactHistoryCount"),S.ImpactHistory.Num());
        if(FatalFixture())R->SetStringField(TEXT("actualVehicleContactEvidence"),D->ReadVehicleContactEvidenceJson());
        R->SetBoolField(TEXT("shiftStarted"), S.bShiftStarted); R->SetBoolField(TEXT("shiftWon"), S.bShiftWon);
        R->SetBoolField(TEXT("operatingFixtureApplied"), D->IsNativeOperatingBatteryFixtureApplied());
        R->SetNumberField(TEXT("parcelCount"), D->GetParcelCount()); R->SetNumberField(TEXT("securedCount"), D->GetSecuredParcelCount());
    }
    if (Robot)
    {
        R->SetStringField(TEXT("nativeRobotTelemetry"), Robot->ReadTelemetryJson());
        FMAYADeliveryRobotObservation O; R->SetBoolField(TEXT("nativeObservationKnown"), Robot->ReadDeliveryState(O));
        R->SetNumberField(TEXT("resetGeneration"), O.ResetGeneration); R->SetNumberField(TEXT("supportMask"), O.SupportMask);
        R->SetBoolField(TEXT("closedLatched"), O.bDoorsClosedLatched); R->SetBoolField(TEXT("freshMovementIntent"), Robot->HasFreshMovementIntent());
        TArray<TSharedPtr<FJsonValue>> Bodies; bool Unique;
        const int32 Count = NativeComponents(Robot, nullptr, &Bodies, GReload.bArmed ? &GReload.OldNativeComponents : nullptr, Unique);
        R->SetArrayField(TEXT("nativeBodyComponents"), Bodies); R->SetNumberField(TEXT("nativeBodyComponentCount"), Count);
        R->SetBoolField(TEXT("allBodyComponentSerialsReplaced"), Unique);
    }
    return R;
}
void UMAYAPlayerNewShiftReloadTest::Event(const FString& Name, const FString& Detail)
{
    if (GReload.bWriteFailed) return;
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("event"), Name); R->SetStringField(TEXT("detail"), Detail);
    R->SetNumberField(TEXT("step"), GReload.Step); R->SetNumberField(TEXT("frame"), GFrameCounter);
    R->SetNumberField(TEXT("realSeconds"), FPlatformTime::Seconds() - GReload.Started); R->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    R->SetBoolField(TEXT("replacementWorld"), ShouldSuppressInitialAutomation(this));
    if (auto* P = Player()) R->SetNumberField(TEXT("screen"), int32(P->GetPlayerScreen()));
    FString Text; FJsonSerializer::Serialize(R, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text + TEXT("\n"), *FPaths::Combine(GReload.Output, TEXT("new-shift-events.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
    { GReload.bWriteFailed = true; Finish(false, TEXT("NewShift evidence write failed")); }
}
bool UMAYAPlayerNewShiftReloadTest::Check(bool Condition, const FString& Description)
{
    auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("check"), Description); R->SetBoolField(TEXT("passed"), Condition);
    GReload.Checks.Add(MakeShared<FJsonValueObject>(R)); Event(Condition ? TEXT("check_pass") : TEXT("check_fail"), Description);
    if (!Condition) Finish(false, Description);
    return Condition && !GReload.bFinished && !GReload.bWriteFailed;
}
void UMAYAPlayerNewShiftReloadTest::Advance(double Delay, double Bound)
{ ++GReload.Step; GReload.Due = FPlatformTime::Seconds() + Delay; GReload.Deadline = FPlatformTime::Seconds() + Bound; }
bool UMAYAPlayerNewShiftReloadTest::Click(FName Action, bool Arm)
{
    if (GReload.bFinished || GReload.bWriteFailed) return false;
    auto* P = Player(); AHUD* H = P ? P->GetHUD() : nullptr; const FIntPoint Size = ViewportSize();
    if (!Check(P && LastRenderedScreen == P->GetPlayerScreen() && LastRenderedSize == Size && LastRenderedFrame > 0,
        TEXT("Actual current rendered screen/viewport supplies the hitbox"))) return false;
    if (!Check(H && H->GetHitBoxWithName(Action), TEXT("Rendered enabled hitbox: ") + Action.ToString())) return false;
    const FHUDHitBox* Named = H->GetHitBoxWithName(Action);
    for (int32 Y = 8; Y < Size.Y - 8; Y += 8) for (int32 X = 8; X < Size.X - 8; X += 8)
    {
        const FVector2D At(X, Y); const auto* B = H->GetHitBoxAtCoordinates(At);
        if (!Named->Contains(At) || !B || B->GetName() != Action) continue;
        bool Interior = true; for (const FVector2D Offset : {FVector2D(-3, 0), FVector2D(3, 0), FVector2D(0, -3), FVector2D(0, 3)})
        { const auto* N = H->GetHitBoxAtCoordinates(At + Offset); Interior &= N && N->GetName() == Action; }
        if (!Interior) continue;
        if (Arm)
        {
            if (!Check(Action == TEXT("ConfirmNewShift") && GReload.Step == 4 && !GReload.bArmed && PendingCapture.IsEmpty()
                && P->GetPlayerScreen() == EMAYAPlayerScreen::NewShiftConfirmation && FObjectKey(GetWorld()) == GReload.OriginWorld,
                TEXT("Arm once immediately before the real ConfirmNewShift dispatch, with no pending capture"))) return false;
            GReload.bArmed = true;
        }
        Event(TEXT("actual_hitbox_dispatch"), FString::Printf(TEXT("name=%s point=%d,%d viewport=%d,%d priority=%d"), *Action.ToString(), X, Y, Size.X, Size.Y, B->GetPriority()));
        if (GReload.bFinished || GReload.bWriteFailed) return false;
        const bool Hit = H->UpdateAndDispatchHitBoxClickEvents(At, IE_Pressed); H->UpdateAndDispatchHitBoxClickEvents(At, IE_Released);
        return Check(Hit, TEXT("Real HUD click dispatched: ") + Action.ToString());
    }
    return Check(false, TEXT("No interior point resolves to enabled hitbox: ") + Action.ToString());
}
bool UMAYAPlayerNewShiftReloadTest::Capture(const FString& Name)
{
    if (GReload.Captures.Contains(Name)) return true;
    if (!PendingCapture.IsEmpty()) return false;
    PendingCapture = FPaths::Combine(GReload.Output, Name + TEXT(".png")); ScreenshotSize = ViewportSize();
    ScreenshotRequestFrame = GFrameCounter; ScreenshotProcessedFrame = 0; bScreenshotProcessed = false;
    FScreenshotRequest::RequestScreenshot(PendingCapture, true, false); Event(TEXT("screenshot_requested"), PendingCapture); return false;
}
void UMAYAPlayerNewShiftReloadTest::Rendered(AHUD* H, UCanvas*)
{
    auto* P = Player(); if (!P || H != P->GetHUD() || GReload.bFinished) return;
    LastRenderedFrame = GFrameCounter; LastRenderedScreen = P->GetPlayerScreen(); LastRenderedSize = ViewportSize();
    if (ShouldSuppressInitialAutomation(this) && GReload.Step >= 8 && !GReload.bFirstGameplay
        && P->HasStartedShift() && !P->HasPlayerMenu())
    {
        GReload.bFirstGameplay = true; auto* D = Director(); auto* Robot = Cast<AMAYANativeDemoPawn>(P->GetPawn());
        GReload.FirstGameplay = WorldReceipt(); Event(TEXT("first_successor_gameplay_draw"));
        Check(D && Robot && D->Snapshot().TotalCredits == 0 && D->Snapshot().BatteryPercent == 100.0
            && !D->Snapshot().bDroneTotalled && !D->Snapshot().bVehicleContactPending && !D->Snapshot().VehicleCrash.bFrozen && D->Snapshot().CargoConditionPercent==100 && D->Snapshot().bShiftStarted && D->Snapshot().Phase == EMAYADeliveryPhase::Available && D->Snapshot().JobInstanceId.IsEmpty()
            && !P->IsStartupMenuBypassedForAutomation() && !P->IsMoveInputIgnored() && !P->IsLookInputIgnored()
            && !UGameplayStatics::IsGamePaused(this) && !Robot->HasFreshMovementIntent(),
            TEXT("First actual new Gameplay draw is exactly $0/100%, Available, ordinary input ownership and no leaked movement"));
    }
}
void UMAYAPlayerNewShiftReloadTest::ScreenshotProcessed()
{
    if (PendingCapture.IsEmpty()) return; bScreenshotProcessed = true; ScreenshotProcessedFrame = GFrameCounter;
    Event(TEXT("screenshot_engine_processed"), FString::Printf(TEXT("path=%s requestFrame=%llu processedFrame=%llu dimensions=%dx%d"),
        *PendingCapture, ScreenshotRequestFrame, ScreenshotProcessedFrame, ScreenshotSize.X, ScreenshotSize.Y));
}
void UMAYAPlayerNewShiftReloadTest::Finish(bool Success, const FString& Reason)
{
    if (GReload.bFinished) return; GReload.bFinished = true;
    GReload.bSuccess = Success && !GReload.bWriteFailed && GReload.bQuitRequested;
    auto R = MakeShared<FJsonObject>(); R->SetBoolField(TEXT("terminal"), true); R->SetBoolField(TEXT("success"), GReload.bSuccess);
    R->SetStringField(TEXT("phase"), TEXT("NativeNewShiftReload")); R->SetStringField(TEXT("reason"), Reason);
    R->SetStringField(TEXT("scope"), TEXT("Explicit editor Development Operating continuation: actual depleted NewShift/confirmation/OpenLevel, same GameInstance and replaced world/native component serial identities, ordinary menu/first Gameplay and product Quit. Not packaged restart, winning PlayAgain button or human physical input acceptance."));
    if(FatalFixture())R->SetStringField(TEXT("scope"),TEXT("Explicit Development controlled original-car genuine fatal contact continuation: actual Start Over/confirmation/OpenLevel, same GameInstance and replaced native world/body identities, exact fresh-shift resources, preserved settings, ordinary menu/first Gameplay and product Quit. Controlled car actuation is separately recorded; no natural-driver/human input claim."));
    R->SetBoolField(TEXT("productQuitRequested"), GReload.bQuitRequested); R->SetBoolField(TEXT("humanPhysicalInputVerified"), false);
    R->SetStringField(TEXT("nativePixelReview"), TEXT("pending independent inspection")); R->SetArrayField(TEXT("checks"), GReload.Checks);
    R->SetNumberField(TEXT("fixtureApplicationCount"), GReload.FixtureCount); R->SetNumberField(TEXT("automationDriverSpawnCount"), GReload.DriverSpawnCount);
    R->SetBoolField(TEXT("originalObserverEndPlayObserved"), GReload.bOriginEndPlay);
    R->SetNumberField(TEXT("initialObserverCount"), GReload.InitialComponents); R->SetNumberField(TEXT("replacementObserverCount"), GReload.SuccessorComponents);
    R->SetStringField(TEXT("settingsDestination"), GReload.SettingsFile); R->SetBoolField(TEXT("settingsFileExistedBeforeTravel"), GReload.bSettingsExisted);
    R->SetStringField(TEXT("checkpoint"), FPaths::Combine(GReload.Output, FatalFixture()?TEXT("crash-before-reload.json"):TEXT("operating-before-reload.json")));
    R->SetNumberField(TEXT("endWorldSeconds"), GetWorld()->GetTimeSeconds());
    if (GReload.OldReceipt.IsValid()) R->SetObjectField(TEXT("beforeReload"), GReload.OldReceipt);
    if (GReload.NewReceipt.IsValid()) R->SetObjectField(TEXT("replacementMainMenu"), GReload.NewReceipt);
    if (GReload.FirstGameplay.IsValid()) R->SetObjectField(TEXT("firstGameplayDraw"), GReload.FirstGameplay);
    if (GReload.OldPreferences.IsValid()) R->SetObjectField(TEXT("preferencesBefore"), GReload.OldPreferences);
    if (GReload.NewPreferences.IsValid()) R->SetObjectField(TEXT("preferencesAfter"), GReload.NewPreferences);
    TArray<TSharedPtr<FJsonValue>> Captures; for (const FString& Name : GReload.Captures) Captures.Add(MakeShared<FJsonValueString>(FPaths::Combine(GReload.Output, Name + TEXT(".png"))));
    R->SetArrayField(TEXT("captures"), Captures);
    FString Text; FJsonSerializer::Serialize(R, TJsonWriterFactory<>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text, *FPaths::Combine(GReload.Output, TEXT("result.json"))))
    { GReload.bSuccess = false; UE_LOG(LogTemp, Error, TEXT("MAYA NewShift terminal evidence write failed")); }
    GReload.FailExitDue = FPlatformTime::Seconds() + 1;
}
void UMAYAPlayerNewShiftReloadTest::TickComponent(float Dt, ELevelTick Tick, FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt, Tick, Function); const double Now = FPlatformTime::Seconds();
    if (GReload.bFinished) { if (!GReload.bSuccess && Now >= GReload.FailExitDue) FPlatformMisc::RequestExit(false, TEXT("MAYA bounded NewShift QA failure")); return; }
    if (Now - GReload.Started > 600 || Now > GReload.Deadline) { Finish(false, TEXT("Bounded NewShift continuation stage/loading deadline exceeded")); return; }
    if (GReload.Step == 0) return; // Driver owns the unchanged physical Operating proof.
    auto* P = Player(); auto* D = Director(); if (!P) return;
    if (!PendingCapture.IsEmpty())
    {
        if (!bScreenshotProcessed || IFileManager::Get().FileSize(*PendingCapture) <= 64) return;
        TArray<uint8> Bytes; const bool Read = FFileHelper::LoadFileToArray(Bytes, *PendingCapture);
        const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
        auto UInt = [&](int32 I) { return (uint32(Bytes[I]) << 24) | (uint32(Bytes[I + 1]) << 16) | (uint32(Bytes[I + 2]) << 8) | uint32(Bytes[I + 3]); };
        if (!Check(Read && Bytes.Num() > 24 && FMemory::Memcmp(Bytes.GetData(), Signature, 8) == 0
            && UInt(16) == uint32(ScreenshotSize.X) && UInt(20) == uint32(ScreenshotSize.Y) && ScreenshotProcessedFrame >= ScreenshotRequestFrame,
            TEXT("Engine-processed PNG signature/dimensions/frame are valid"))) return;
        Event(TEXT("screenshot_saved"), PendingCapture); GReload.Captures.Add(FPaths::GetBaseFilename(PendingCapture)); PendingCapture.Reset();
    }
    if (Now < GReload.Due || LastRenderedFrame == 0 || LastRenderedScreen != P->GetPlayerScreen() || LastRenderedSize != ViewportSize()) return;
    switch (GReload.Step)
    {
    case 1:
        if (!Check(FatalFixture() ? D&&D->Snapshot().bDroneTotalled&&D->Snapshot().VehicleCrash.Contact.IsFatal()
            &&D->GetPlayerPresentation().bCanStartNewShift&&!D->GetPlayerPresentation().bCanRecover&&!D->GetPlayerPresentation().bCanReplay
            &&P->HasStartedShift()&&P->HasPlayerMenu()&&P->GetPlayerScreen()==EMAYAPlayerScreen::ShiftLost
            : D && D->GetPlayerPresentation().bCanStartNewShift && D->Snapshot().BatteryPercent == 0 && D->Snapshot().TotalCredits == 0
            && !D->Snapshot().bCharging && D->Snapshot().LastRecharge.Generation == 0 && D->GetParcelCount() == 0
            && P->HasStartedShift() && !P->HasPlayerMenu() && P->GetPlayerScreen() == EMAYAPlayerScreen::Gameplay,
            FatalFixture()?TEXT("Actual totalled HUD exposes Start Over and rejects recovery/replay regardless of wreck settlement"):TEXT("Actual depleted HUD remains eligible with zero cash/energy, no free recharge and empty inventory"))) return;
        GReload.OldReceipt = WorldReceipt(); GReload.OldPreferences = PreferencesReceipt();
        if (!Check(GReload.OldPreferences->GetBoolField(TEXT("known")), TEXT("Pretravel actual display and loaded presentation preferences are known"))) return;
        GReload.bSettingsExisted = IFileManager::Get().FileExists(*GReload.SettingsFile);
        if (GReload.bSettingsExisted && !Check(FFileHelper::LoadFileToArray(GReload.SettingsBytes, *GReload.SettingsFile), TEXT("Read isolated pretravel settings bytes"))) return;
        Capture(FatalFixture()?TEXT("01-totalled-start-over"):TEXT("01-depleted-new-shift")); Advance(); break;
    case 2:
        if (!Click(TEXT("NewShift"))) return; Advance(); break;
    case 3:
        if (!Check(P->GetPlayerScreen() == EMAYAPlayerScreen::NewShiftConfirmation && UGameplayStatics::IsGamePaused(this)
            && P->IsMoveInputIgnored() && P->IsLookInputIgnored(), TEXT("Real NewShift click opens paused confirmation with menu input ownership"))) return;
        Capture(TEXT("02-new-shift-confirmation")); Advance(); break;
    case 4:
        if (!Check(PreferencesMatch(), TEXT("Preferences/settings bytes preserved before ConfirmNewShift")) || !Click(TEXT("ConfirmNewShift"), true)) return;
        Advance(.1, 30); break;
    case 5:
        // Await actual travel; the replacement component resumes step6. No old-world suppression.
        break;
    case 6:
    {
        if (P->GetPlayerScreen() == EMAYAPlayerScreen::StartupError) { Finish(false, TEXT("Actual replacement-world bootstrap failed")); return; }
        if (!D || P->GetPlayerScreen() != EMAYAPlayerScreen::MainMenu || !D->GetPlayerPresentation().bReadyForStart) return;
        auto* Robot = Cast<AMAYANativeDemoPawn>(P->GetPawn()); FMAYADeliveryRobotObservation O; const auto& S = D->Snapshot();
        int32 Drivers = 0; for (TActorIterator<AMAYAV01NativeTestDriver> It(GetWorld()); It; ++It) ++Drivers;
        bool ComponentsReplaced; const int32 Count = NativeComponents(Robot, nullptr, nullptr, &GReload.OldNativeComponents, ComponentsReplaced);
        if (!Check(ShouldSuppressInitialAutomation(this) && GReload.bOriginEndPlay && !GReload.OldPlayer.IsValid()
            && !GReload.OldDirector.IsValid() && !GReload.OldRobot.IsValid() && FObjectKey(P) != GReload.PlayerKey
            && FObjectKey(D) != GReload.DirectorKey && FObjectKey(Robot) != GReload.RobotKey && ComponentsReplaced && Count == GReload.OldNativeComponents.Num() && Count >= 9,
            TEXT("Same GameInstance replaced the old world/controller/core/robot/native component serial identities through actual OpenLevel"))) return;
        const bool bNativeObservationRead = Robot && Robot->ReadDeliveryState(O);
        const bool bFreshMenuCondition = bNativeObservationRead && O.IsStoppedAndSupported() && O.bDoorsClosedLatched && O.SupportMask == 63
            && O.ResetGeneration == 1 && !Robot->HasFreshMovementIntent() && GetWorld()->GetMapName().Contains(TEXT("Small_City"))
            && GetWorld()->GetAuthGameMode() && GetWorld()->GetAuthGameMode()->IsA<AMAYADeliveryGameMode>()
            && !P->HasStartedShift() && !P->IsStartupMenuBypassedForAutomation() && P->IsMoveInputIgnored() && P->IsLookInputIgnored()
            && UGameplayStatics::IsGamePaused(this) && !S.bShiftStarted && S.BatteryPercent == 100.0 && S.TotalCredits == 0
            && S.Phase == EMAYADeliveryPhase::Available && S.JobInstanceId.IsEmpty() && !S.ShiftInstanceId.IsEmpty() && S.ShiftInstanceId != GReload.OldShift
            && S.Service.Generation == 0 && S.CompletedJobs == 0 && S.Recoveries == 0 && S.RechargeFeesPaidDollars == 0 && S.LastRecharge.Generation == 0
            && !S.bShiftWon && !S.bDroneTotalled && !S.bVehicleContactPending && !S.VehicleCrash.bFrozen && S.CargoConditionPercent==100 && S.ImpactHistory.IsEmpty() && !S.bCharging && D->GetParcelCount() == 0 && D->GetSecuredParcelCount() == 0;
        // Preserve the exact admission result and its inputs before Check can seal a failure.
        GReload.NewReceipt = WorldReceipt();
        auto Guard = MakeShared<FJsonObject>();
        Guard->SetBoolField(TEXT("compoundPassed"), bFreshMenuCondition);
        Guard->SetStringField(TEXT("observationBasis"), TEXT("Exact native observation used by the unchanged fresh Main Menu compound predicate; no guard relaxation"));
        Guard->SetBoolField(TEXT("robotPresent"), Robot != nullptr);
        Guard->SetBoolField(TEXT("nativeObservationRead"), bNativeObservationRead);
        Guard->SetBoolField(TEXT("stoppedAndSupported"), O.IsStoppedAndSupported());
        Guard->SetBoolField(TEXT("doorsClosedLatched"), O.bDoorsClosedLatched);
        Guard->SetBoolField(TEXT("allSixSupport"), O.SupportMask == 63);
        Guard->SetBoolField(TEXT("initialResetGeneration"), O.ResetGeneration == 1);
        Guard->SetBoolField(TEXT("noFreshMovementIntent"), Robot && !Robot->HasFreshMovementIntent());
        Guard->SetBoolField(TEXT("smallCityMap"), GetWorld()->GetMapName().Contains(TEXT("Small_City")));
        Guard->SetBoolField(TEXT("authGameModePresent"), GetWorld()->GetAuthGameMode() != nullptr);
        Guard->SetBoolField(TEXT("deliveryGameMode"), GetWorld()->GetAuthGameMode() && GetWorld()->GetAuthGameMode()->IsA<AMAYADeliveryGameMode>());
        Guard->SetBoolField(TEXT("controllerShiftNotStarted"), !P->HasStartedShift());
        Guard->SetBoolField(TEXT("noAutomationMenuBypass"), !P->IsStartupMenuBypassedForAutomation());
        Guard->SetBoolField(TEXT("moveInputIgnored"), P->IsMoveInputIgnored());
        Guard->SetBoolField(TEXT("lookInputIgnored"), P->IsLookInputIgnored());
        Guard->SetBoolField(TEXT("worldPaused"), UGameplayStatics::IsGamePaused(this));
        Guard->SetBoolField(TEXT("coreShiftNotStarted"), !S.bShiftStarted);
        Guard->SetBoolField(TEXT("batteryExactly100"), S.BatteryPercent == 100.0);
        Guard->SetBoolField(TEXT("cashExactlyZero"), S.TotalCredits == 0);
        Guard->SetBoolField(TEXT("phaseAvailable"), S.Phase == EMAYADeliveryPhase::Available);
        Guard->SetBoolField(TEXT("jobIdEmpty"), S.JobInstanceId.IsEmpty());
        Guard->SetBoolField(TEXT("shiftIdNonempty"), !S.ShiftInstanceId.IsEmpty());
        Guard->SetBoolField(TEXT("shiftIdReplaced"), S.ShiftInstanceId != GReload.OldShift);
        Guard->SetBoolField(TEXT("noServiceGeneration"), S.Service.Generation == 0);
        Guard->SetBoolField(TEXT("noCompletedJobs"), S.CompletedJobs == 0);
        Guard->SetBoolField(TEXT("noRecoveries"), S.Recoveries == 0);
        Guard->SetBoolField(TEXT("noPaidFees"), S.RechargeFeesPaidDollars == 0);
        Guard->SetBoolField(TEXT("noRechargeGeneration"), S.LastRecharge.Generation == 0);
        Guard->SetBoolField(TEXT("notWon"), !S.bShiftWon);
        Guard->SetBoolField(TEXT("notCharging"), !S.bCharging);
        Guard->SetBoolField(TEXT("noParcelActors"), D->GetParcelCount() == 0);
        Guard->SetBoolField(TEXT("noSecuredParcels"), D->GetSecuredParcelCount() == 0);
        Guard->SetBoolField(TEXT("nativeKnown"), O.bKnown);
        Guard->SetBoolField(TEXT("nativeReady"), O.bReady);
        Guard->SetBoolField(TEXT("nativeGrounded"), O.bGrounded);
        Guard->SetBoolField(TEXT("nativeUpright"), O.bUpright);
        Guard->SetStringField(TEXT("nativeFault"), O.Fault);
        Guard->SetNumberField(TEXT("nativeSpeedCmps"), O.VelocityCmps.Size());
        Guard->SetNumberField(TEXT("nativeAngularSpeedRadps"), O.AngularVelocityRadps.Size());
        Guard->SetNumberField(TEXT("nativeObservationWorldSeconds"), O.ObservationWorldSeconds);
        Guard->SetNumberField(TEXT("serviceGeneration"), S.Service.Generation);
        Guard->SetNumberField(TEXT("recoveries"), S.Recoveries);
        Guard->SetNumberField(TEXT("rechargeGeneration"), S.LastRecharge.Generation);
        Guard->SetStringField(TEXT("mapName"), GetWorld()->GetMapName());
        Guard->SetStringField(TEXT("authGameModeClass"), GetPathNameSafe(GetWorld()->GetAuthGameMode() ? GetWorld()->GetAuthGameMode()->GetClass() : nullptr));
        GReload.NewReceipt->SetObjectField(TEXT("freshMenuGuard"), Guard);
        if (!Check(bFreshMenuCondition,
            TEXT("Actual fresh ordinary Main Menu is settled/closed/empty/Available at exact $0/100%, new shift and no leaked job/input/resources"))) return;
        if (!Check(Drivers == 0 && !D->IsNativeOperatingBatteryFixtureApplied() && GReload.FixtureCount == (FatalFixture()?0:1) && GReload.DriverSpawnCount == 1,
            TEXT("Initial fixture and automation driver each occurred once; neither restarted after reload"))) return;
        if (!Check(PreferencesMatch(), TEXT("Actual display/audio/accessibility and identical isolated settings bytes survive real reload"))) return;
        GReload.NewPreferences = PreferencesReceipt(); Capture(TEXT("03-reloaded-main-menu")); Advance(); break;
    }
    case 7:
        if (!Check(D && !D->Snapshot().bShiftStarted && D->Snapshot().BatteryPercent == 100.0 && D->Snapshot().TotalCredits == 0
            && PreferencesMatch(), TEXT("Fresh menu remains pristine through processed capture without preference mutation")) || !Click(TEXT("StartShift"))) return;
        Advance(.1); break;
    case 8:
        if (!Check(GReload.bFirstGameplay && P->HasStartedShift() && !P->HasPlayerMenu() && D && D->Snapshot().Phase == EMAYADeliveryPhase::Available,
            TEXT("Real StartShift produced the recorded first Gameplay draw without accepting a job"))) return;
        Capture(TEXT("04-first-new-gameplay")); Advance(); break;
    case 9:
        Event(TEXT("engine_input_callback"), TEXT("P pressed/released through the existing ordinary Pause binding; not physical human input"));
        P->InputKey(FInputKeyParams(EKeys::P, IE_Pressed, 1.0));
        Advance(.1); break;
    case 10:
        P->InputKey(FInputKeyParams(EKeys::P, IE_Released, 0.0));
        if (!Check(P->GetPlayerScreen() == EMAYAPlayerScreen::Pause && UGameplayStatics::IsGamePaused(this), TEXT("Ordinary Pause binding owns the fresh shift menu")) || !Click(TEXT("Quit"))) return;
        Advance(); break;
    case 11:
        if (!Check(P->GetPlayerScreen() == EMAYAPlayerScreen::QuitConfirmation, TEXT("Actual product Quit confirmation rendered")) || !Click(TEXT("ConfirmQuit"))) return;
        GReload.bQuitRequested = IsEngineExitRequested();
        if (!Check(GReload.bQuitRequested, TEXT("Actual product ConfirmQuit requests engine exit without helper fallback"))) return;
        Finish(true, FatalFixture()?TEXT("Actual totalled Start Over confirmation reloaded a fresh ordinary shift with exact resources and replaced native identities, retained preferences, rendered first Gameplay and product Quit"):TEXT("Actual depleted NewShift confirmation reloaded the same GameInstance into a new ordinary $0/100% shift, retained preferences, rendered first Gameplay and product Quit")); break;
    default: Finish(false, TEXT("Unknown NewShift continuation stage")); break;
    }
}
void UMAYAPlayerNewShiftReloadTest::EndPlay(const EEndPlayReason::Type Reason)
{
    if (RenderHandle.IsValid()) AHUD::OnHUDPostRender.Remove(RenderHandle);
    if (ScreenshotHandle.IsValid()) FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    if (IsEnabled() && SameInstance(GetWorld()) && FObjectKey(GetWorld()) == GReload.OriginWorld && GReload.bArmed)
        GReload.bOriginEndPlay = true;
    Super::EndPlay(Reason);
}
