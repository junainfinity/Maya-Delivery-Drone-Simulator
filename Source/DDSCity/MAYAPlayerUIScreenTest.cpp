#include "MAYAPlayerUIScreenTest.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryDirector.h"
#include "MAYANativeDemoPawn.h"
#include "MAYAPresentationSettings.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
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
#include "AudioDevice.h"
#include "AudioThread.h"
#include "CoreGlobals.h"

UMAYAPlayerUIScreenTest::UMAYAPlayerUIScreenTest()
{
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.bTickEvenWhenPaused=true;
    PrimaryComponentTick.TickGroup=TG_PostUpdateWork;
}

AMAYADeliveryPlayerController* UMAYAPlayerUIScreenTest::Player() const { return Cast<AMAYADeliveryPlayerController>(GetOwner()); }
AMAYADeliveryDirector* UMAYAPlayerUIScreenTest::Director() const
{ for(TActorIterator<AMAYADeliveryDirector> It(GetWorld());It;++It) return *It; return nullptr; }

void UMAYAPlayerUIScreenTest::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    SetComponentTickEnabled(false);
#else
    Started=FPlatformTime::Seconds(); Due=Started;
    if (!FParse::Param(FCommandLine::Get(),TEXT("MayaV01UIScreenTest"))
        || FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))
        || FParse::Param(FCommandLine::Get(),TEXT("MayaV01ObserveInput"))
        || !FParse::Value(FCommandLine::Get(),TEXT("MayaV01Output="),Output) || Output.IsEmpty())
    { SetComponentTickEnabled(false); return; }
    IFileManager::Get().MakeDirectory(*Output,true);
    FString Expected=FPaths::ConvertRelativePathToFull(FPaths::Combine(Output,TEXT("GameUserSettings.ini")));
    // UE5.3 stores a known cache key in GGameUserSettingsIni. Resolve the
    // same disk destination used by ConfigContext/known-file initialization.
    FString Actual=FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"),nullptr,*FPaths::GeneratedConfigDir()));
    FPaths::NormalizeFilename(Expected); FPaths::NormalizeFilename(Actual);
    if(!Check(GConfig && GConfig->FindConfigFile(GGameUserSettingsIni) && Expected.Equals(Actual,ESearchCase::IgnoreCase),TEXT("Known settings cache exists and its resolved disk destination is the isolated attempt file"))) return;
    RenderHandle=AHUD::OnHUDPostRender.AddUObject(this,&UMAYAPlayerUIScreenTest::Rendered);
    ScreenshotHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this,&UMAYAPlayerUIScreenTest::ScreenshotProcessed);
    Event(TEXT("initialized"),FString::Printf(TEXT("map=%s; gameMode=%s; controller=%s; settingsCacheKey=%s; resolvedSettingsDestination=%s; no AutoTest/menu bypass"),
        *GetWorld()->GetMapName(),*GetNameSafe(GetWorld()->GetAuthGameMode()),*GetNameSafe(Player()),*GGameUserSettingsIni,*Actual));
#endif
}

FIntPoint UMAYAPlayerUIScreenTest::ViewportSize() const
{
    const auto* P=Player(); const auto* L=P?P->GetLocalPlayer():nullptr;
    UGameViewportClient* V=L?L->ViewportClient.Get():nullptr;
    return V && V->Viewport ? V->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
}
int32 UMAYAPlayerUIScreenTest::WindowMode() const
{
    const auto* P=Player(); const auto* L=P?P->GetLocalPlayer():nullptr; UGameViewportClient* V=L?L->ViewportClient.Get():nullptr;
    const TSharedPtr<SWindow> W=V?V->GetWindow():nullptr; return W.IsValid()?int32(W->GetWindowMode()):-1;
}
void UMAYAPlayerUIScreenTest::Event(const FString& Name,const FString& Detail)
{
    auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("event"),Name); O->SetStringField(TEXT("detail"),Detail);
    O->SetNumberField(TEXT("step"),Step); O->SetNumberField(TEXT("frame"),GFrameCounter);
    O->SetNumberField(TEXT("realSeconds"),FPlatformTime::Seconds()-Started); O->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());
    if(auto* P=Player()) {
        O->SetNumberField(TEXT("screen"),int32(P->GetPlayerScreen())); O->SetBoolField(TEXT("shiftStarted"),P->HasStartedShift());
        O->SetNumberField(TEXT("menuFocusIndex"),P->GetMenuFocus());
        const auto Actions=P->GetMenuActions();
        if(Actions.IsValidIndex(P->GetMenuFocus())) O->SetStringField(TEXT("focusedAction"),Actions[P->GetMenuFocus()].Id.ToString());
    }
    if(auto* D=Director()) { O->SetNumberField(TEXT("cash"),D->Snapshot().TotalCredits); O->SetNumberField(TEXT("battery"),D->Snapshot().BatteryPercent); O->SetNumberField(TEXT("phase"),int32(D->Snapshot().Phase)); O->SetNumberField(TEXT("jobSeconds"),D->Snapshot().ElapsedSeconds); }
    FString Text; FJsonSerializer::Serialize(O,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    if(!FFileHelper::SaveStringToFile(Text+TEXT("\n"),*FPaths::Combine(Output,TEXT("ui-events.jsonl")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_Append))
    {
        bEvidenceWriteFailed=true;
        UE_LOG(LogTemp,Error,TEXT("Maya UI evidence write failed"));
        Finish(false,TEXT("UI event evidence could not be saved"));
    }
}
bool UMAYAPlayerUIScreenTest::Check(bool Condition,const FString& Description)
{
    auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("check"),Description); O->SetBoolField(TEXT("passed"),Condition);
    Checks.Add(MakeShared<FJsonValueObject>(O)); Event(Condition?TEXT("check_pass"):TEXT("check_fail"),Description);
    if(!Condition) Finish(false,Description); return Condition && !bEvidenceWriteFailed && !bFinished;
}
void UMAYAPlayerUIScreenTest::Finish(bool Success,const FString& Reason)
{
    if(bFinished) return; bFinished=true; bSuccessfulFinish=Success && !bEvidenceWriteFailed;
    auto* P=Player(); if(P) for(const auto& K:InjectedHeldKeys) P->InputKey(FInputKeyParams(K,IE_Released,0.0));
    InjectedHeldKeys.Reset();
    auto O=MakeShared<FJsonObject>(); O->SetBoolField(TEXT("terminal"),true); O->SetBoolField(TEXT("success"),bSuccessfulFinish);
    O->SetStringField(TEXT("phase"),TEXT("NativeUIScreenQA")); O->SetStringField(TEXT("reason"),Reason);
    O->SetStringField(TEXT("scope"),TEXT("Explicit engine keyboard-callback and actual generated HUD-hitbox dispatch; ordinary startup. Not human input or independent pixel acceptance."));
    O->SetBoolField(TEXT("humanPhysicalInputVerified"),false); O->SetStringField(TEXT("nativePixelReview"),TEXT("pending independent inspection"));
    O->SetArrayField(TEXT("checks"),Checks); O->SetNumberField(TEXT("endWorldSeconds"),GetWorld()->GetTimeSeconds());
    FString Text; FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));
    if(!FFileHelper::SaveStringToFile(Text,*FPaths::Combine(Output,TEXT("result.json"))))
    { bSuccessfulFinish=false; UE_LOG(LogTemp,Error,TEXT("Maya UI terminal evidence could not be saved")); }
    Due=FPlatformTime::Seconds()+1;
}
void UMAYAPlayerUIScreenTest::Advance(double Delay) { if(!bFinished) { ++Step; Due=FPlatformTime::Seconds()+Delay; } }
bool UMAYAPlayerUIScreenTest::Key(FKey Value,EInputEvent Type)
{
    auto* P=Player(); if(!P || bFinished || bEvidenceWriteFailed) return false;
    Event(TEXT("input_callback_requested"),Value.ToString());
    if(bFinished || bEvidenceWriteFailed) return false;
    if(Type==IE_Pressed) InjectedHeldKeys.Add(Value); else if(Type==IE_Released) InjectedHeldKeys.Remove(Value);
    const bool Handled=P->InputKey(FInputKeyParams(Value,Type,Type==IE_Released?0.0:1.0));
    Event(TEXT("injected_input_callback"),FString::Printf(TEXT("key=%s event=%d handled=%d"),*Value.ToString(),int32(Type),Handled)); return Handled;
}
bool UMAYAPlayerUIScreenTest::Click(FName Action)
{
    if(bFinished || bEvidenceWriteFailed) return false;
    auto* P=Player(); AHUD* H=P?P->GetHUD():nullptr; const FIntPoint Size=ViewportSize();
    if(!Check(P && LastRenderedScreen==P->GetPlayerScreen() && LastRenderedSize==Size && LastRenderedFrame>0,TEXT("Hitbox dispatch uses an actually rendered current screen and viewport"))) return false;
    if(!Check(H && H->GetHitBoxWithName(Action),TEXT("Rendered enabled hitbox exists: ")+Action.ToString())) return false;
    const FHUDHitBox* Named=H->GetHitBoxWithName(Action);
    // Query actual top-priority hitboxes. No duplicated layout positions or direct action fallback.
    for(int32 Y=8;Y<Size.Y-8;Y+=8) for(int32 X=8;X<Size.X-8;X+=8)
    {
        const FVector2D At(X,Y); if(!Named->Contains(At)) continue;
        const auto* B=H->GetHitBoxAtCoordinates(At);
        if(!B || B->GetName()!=Action) continue;
        bool Interior=true; for(const FVector2D Offset:{FVector2D(-3,0),FVector2D(3,0),FVector2D(0,-3),FVector2D(0,3)})
        { const auto* N=H->GetHitBoxAtCoordinates(At+Offset); Interior &= N && N->GetName()==Action; }
        if(!Interior) continue;
        Event(TEXT("actual_hitbox_dispatch"),FString::Printf(TEXT("name=%s point=(%d,%d) viewport=(%d,%d) priority=%d"),*Action.ToString(),X,Y,Size.X,Size.Y,B->GetPriority()));
        if(bFinished || bEvidenceWriteFailed) return false;
        const bool Hit=H->UpdateAndDispatchHitBoxClickEvents(At,IE_Pressed);
        H->UpdateAndDispatchHitBoxClickEvents(At,IE_Released);
        return Check(Hit,TEXT("Real HUD click dispatched: ")+Action.ToString());
    }
    return Check(false,TEXT("No interior viewport point resolves to hitbox: ")+Action.ToString());
}
bool UMAYAPlayerUIScreenTest::Capture(const FString& Name)
{
    if(bFinished || bEvidenceWriteFailed) return false;
    if(CompletedCaptures.Contains(Name)) return true;
    if(!PendingCapture.IsEmpty()) return false;
    PendingCapture=FPaths::Combine(Output,Name+TEXT(".png"));
    ScreenshotSize=ViewportSize(); ScreenshotRequestFrame=GFrameCounter; ScreenshotProcessedFrame=0; bScreenshotProcessed=false;
    FScreenshotRequest::RequestScreenshot(PendingCapture,true,false);
    Event(TEXT("screenshot_requested"),PendingCapture); return false;
}
void UMAYAPlayerUIScreenTest::Rendered(AHUD* H,UCanvas*)
{
    auto* P=Player(); if(!P || H!=P->GetHUD() || bFinished) return;
    LastRenderedFrame=GFrameCounter; LastRenderedScreen=P->GetPlayerScreen(); LastRenderedSize=ViewportSize();
    if(!bFirstGameplayObserved && P->HasStartedShift() && !P->HasPlayerMenu())
    {
        bFirstGameplayObserved=true; FirstGameplayWorldSeconds=GetWorld()->GetTimeSeconds(); auto* D=Director(); Event(TEXT("first_gameplay_draw"));
        Check(D && D->Snapshot().TotalCredits==0 && D->Snapshot().BatteryPercent==100.0
            && D->Snapshot().Phase==EMAYADeliveryPhase::Available && !P->IsStartupMenuBypassedForAutomation(),
            TEXT("First actual Gameplay HUD draw has exactly $0/100%, Available and no automation menu bypass"));
    }
}
void UMAYAPlayerUIScreenTest::ScreenshotProcessed()
{
    if(PendingCapture.IsEmpty()) return;
    bScreenshotProcessed=true; ScreenshotProcessedFrame=GFrameCounter;
    Event(TEXT("screenshot_engine_processed"),FString::Printf(TEXT("file=%s requestFrame=%llu processedFrame=%llu dimensions=%dx%d"),
        *PendingCapture,ScreenshotRequestFrame,ScreenshotProcessedFrame,ScreenshotSize.X,ScreenshotSize.Y));
}

void UMAYAPlayerUIScreenTest::TickComponent(float Dt,ELevelTick Tick,FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt,Tick,Function);
    const double Now=FPlatformTime::Seconds();
    if(bFinished)
    {
        // Success must exit through the actual product Quit action, never this fallback.
        if(!bSuccessfulFinish && Now>=Due) FPlatformMisc::RequestExit(false,TEXT("MAYA UI failed bounded QA"));
        return;
    }
    if(Now-Started>480) { Finish(false,TEXT("Bounded UI QA timed out")); return; }
    auto* P=Player(); auto* D=Director(); if(!P) return;
    if(Step==13 || Step==14)
    {
        if(P->GetPlayerScreen()==EMAYAPlayerScreen::VideoConfirmation)
        {
            PreviousVideoObserved=LastVideoObserved; LastVideoObserved=Now;
            if(ObservedVideoDeadline==0) ObservedVideoDeadline=Now+P->GetVideoConfirmationSeconds();
            if(Now-RevertStarted>=14 && !bNearVideoDeadlineObserved)
            {
                if(!Check(ViewportSize()!=OriginalSize && GetWorld()->GetTimeSeconds()==MenuWorld,TEXT("Confirmation and changed display remain active at fourteen real seconds with gameplay frozen"))) return;
                bNearVideoDeadlineObserved=true; Event(TEXT("video_confirmation_near_deadline"));
            }
        }
        else if(P->GetPlayerScreen()==EMAYAPlayerScreen::Settings && ObservedRevert==0)
        {
            ObservedRevert=Now; Event(TEXT("video_revert_transition"),FString::Printf(TEXT("observedDeadline=%.9f lastActive=%.9f previousActive=%.9f revertedAt=%.9f"),ObservedVideoDeadline,LastVideoObserved,PreviousVideoObserved,ObservedRevert));
            if(!Check(bNearVideoDeadlineObserved && Now+1e-6>=ObservedVideoDeadline,TEXT("Video revert occurs only at or after its observed fifteen-second deadline"))) return;
        }
    }
    if(!PendingCapture.IsEmpty())
    {
        if(bScreenshotProcessed && IFileManager::Get().FileSize(*PendingCapture)>64)
        {
            TArray<uint8> Bytes; const bool Read=FFileHelper::LoadFileToArray(Bytes,*PendingCapture);
            const uint8 Signature[]={137,80,78,71,13,10,26,10};
            auto UInt=[&](int32 I){return (uint32(Bytes[I])<<24)|(uint32(Bytes[I+1])<<16)|(uint32(Bytes[I+2])<<8)|uint32(Bytes[I+3]);};
            if(!Check(Read && Bytes.Num()>24 && FMemory::Memcmp(Bytes.GetData(),Signature,8)==0
                && UInt(16)==uint32(ScreenshotSize.X) && UInt(20)==uint32(ScreenshotSize.Y) && ScreenshotProcessedFrame>=ScreenshotRequestFrame,
                TEXT("Engine-processed PNG signature, dimensions and capture frame are valid"))) return;
            Event(TEXT("screenshot_saved"),PendingCapture); CompletedCaptures.Add(FPaths::GetBaseFilename(PendingCapture)); PendingCapture.Reset();
        }
        else return;
    }
    if(Now<Due) return;
    if(LastRenderedFrame==0 || LastRenderedScreen!=P->GetPlayerScreen() || LastRenderedSize!=ViewportSize()) return;
    if(P->IsStartupMenuBypassedForAutomation()) { Finish(false,TEXT("UI test incorrectly bypassed ordinary startup")); return; }
    auto Screen=[&](EMAYAPlayerScreen Wanted){return Check(P->GetPlayerScreen()==Wanted,TEXT("Observed screen ")+FString::FromInt(int32(Wanted)));};
    auto Release=[&](FKey K){Key(K,IE_Released);};
    switch(Step)
    {
    case 0:
        if(P->GetPlayerScreen()==EMAYAPlayerScreen::StartupError) { Finish(false,TEXT("Actual ordinary city startup failed")); return; }
        if(P->GetPlayerScreen()!=EMAYAPlayerScreen::MainMenu || !D || LastRenderedScreen!=EMAYAPlayerScreen::MainMenu) return;
        OriginalSize=ViewportSize(); OriginalMode=WindowMode(); MenuWorld=GetWorld()->GetTimeSeconds();
        if(auto* Robot=Cast<AMAYANativeDemoPawn>(P->GetPawn())) Robot->ReadDeliveryState(PausedRobot);
        if(!Check(!P->HasStartedShift() && UGameplayStatics::IsGamePaused(this) && D->Snapshot().TotalCredits==0 && D->Snapshot().BatteryPercent==100.0,TEXT("Ordinary main menu starts paused at exact $0/100%"))) return;
        for(const TCHAR* Id:{TEXT("StartShift"),TEXT("Controls"),TEXT("Settings"),TEXT("JoinProject"),TEXT("Quit")})
            if(!Check(P->GetHUD()->GetHitBoxWithName(FName(Id))!=nullptr,FString(TEXT("Main menu button rendered: "))+Id)) return;
        if(auto* Robot=Cast<AMAYANativeDemoPawn>(P->GetPawn())) Event(TEXT("initial_menu_native_telemetry"),Robot->ReadTelemetryJson());
        Capture(TEXT("01-main-menu")); Advance(3); break;
    case 1:
        if(!Check(D && GetWorld()->GetTimeSeconds()==MenuWorld && D->Snapshot().BatteryPercent==100.0 && D->Snapshot().TotalCredits==0,TEXT("Main menu resources and world remain frozen for three real seconds"))) return;
        {
            FMAYADeliveryRobotObservation Fresh; auto* Robot=Cast<AMAYANativeDemoPawn>(P->GetPawn());
            if(!Check(Robot && Robot->ReadDeliveryState(Fresh) && Fresh.ChassisPose.Equals(PausedRobot.ChassisPose,1e-7)
                && Fresh.VelocityCmps.Equals(PausedRobot.VelocityCmps,1e-7),TEXT("Fresh native chassis pose and velocity stay frozen while menu is paused"))) return;
        }
        Advance(.1); break;
    case 2:
        // The actual pointer may legitimately hover a different compact button.
        // Establish the test's start through its real rendered HUD hover callback,
        // then exercise Tab and Enter in one callback cycle. No OS input claim.
        if(!Check(P->GetHUD() && P->GetHUD()->GetHitBoxWithName(TEXT("StartShift")),TEXT("Rendered StartShift hitbox exists for explicit native hover setup"))) return;
        Event(TEXT("native_hover_callback_requested"),TEXT("StartShift"));
        P->GetHUD()->NotifyHitBoxBeginCursorOver(TEXT("StartShift"));
        if(!Check(P->GetMenuFocus()==0,TEXT("Real StartShift hover callback established initial menu focus"))) return;
        Key(EKeys::Tab,IE_Pressed); Release(EKeys::Tab);
        if(!Check(P->GetMenuActions().IsValidIndex(P->GetMenuFocus()) && P->GetMenuActions()[P->GetMenuFocus()].Id==TEXT("Controls"),TEXT("Real Tab callback advances StartShift focus to Controls"))) return;
        Key(EKeys::Enter,IE_Pressed); Advance(.1); break;
    case 3: Release(EKeys::Enter); if(!Screen(EMAYAPlayerScreen::Controls)) return; Capture(TEXT("02-controls")); Key(EKeys::Enter,IE_Repeat); Advance(.2); break;
    case 4: if(!Screen(EMAYAPlayerScreen::Controls)) return; Key(EKeys::W,IE_Pressed); Key(EKeys::E,IE_Pressed); Advance(.1); break;
    case 5: Release(EKeys::E); Key(EKeys::Escape,IE_Pressed); Advance(.1); break;
    case 6: Release(EKeys::Escape); if(!Screen(EMAYAPlayerScreen::MainMenu)) return; if(!Click(TEXT("Settings"))) return; Advance(); break;
    case 7:
        if(!Screen(EMAYAPlayerScreen::Settings)) return;
        for(const auto& A:P->GetMenuActions()) if(A.Id==TEXT("Volume")) InitialVolumeValue=A.Value;
        if(!Click(InitialVolumeValue==TEXT("0%")?FName(TEXT("Volume.Next")):FName(TEXT("Volume.Previous")))) return;
        Advance(); break;
    case 8: if(!Click(TEXT("Back"))) return; Advance(); break;
    case 9: if(!Screen(EMAYAPlayerScreen::MainMenu) || !Click(TEXT("Settings"))) return; Advance(); break;
    case 10:
        if(!Screen(EMAYAPlayerScreen::Settings)) return;
        for(const auto& A:P->GetMenuActions()) if(A.Id==TEXT("Volume"))
            if(!Check(A.Value==InitialVolumeValue,TEXT("Back discarded the unapplied volume change"))) return;
        // Defaults are working values only; applying tests genuine serialization and audio.
        if(!Click(TEXT("Defaults"))) return; Advance(); break;
    case 11:
        if(!Click(TEXT("Volume.Previous"))) return;
        if(!Click(TEXT("UIScale.Next")) || !Click(TEXT("TextSize.Next")) || !Click(TEXT("Contrast.Next"))) return;
        if(!Click(TEXT("Resolution.Next"))) return; Advance(); break;
    case 12: if(!Capture(TEXT("03-settings-pending"))) return; if(!Click(TEXT("Apply"))) return; RevertStarted=Now; Advance(); break;
    case 13:
        if(!Screen(EMAYAPlayerScreen::VideoConfirmation)) return;
        ChangedSize=ViewportSize(); bChangedViewportObserved=ChangedSize!=OriginalSize;
        if(!Check(bChangedViewportObserved && WindowMode()==2 && P->GetVideoConfirmationSeconds()>12,TEXT("Actual display changed and real 15-second confirmation is running"))) return;
        Capture(TEXT("04-display-confirmation")); Advance(15.5-(Now-RevertStarted)); break;
    case 14:
        if(!Screen(EMAYAPlayerScreen::Settings)) return;
        if(!Check(bNearVideoDeadlineObserved && ObservedRevert>=ObservedVideoDeadline && Now-RevertStarted>=15 && ViewportSize()==OriginalSize && WindowMode()==OriginalMode && GetWorld()->GetTimeSeconds()==MenuWorld,TEXT("Automatic 15-real-second revert restored actual viewport while gameplay stayed frozen"))) return;
        {
            const auto* Pref=P->GetPresentationSettings(); AppliedVolume=Pref?Pref->MasterVolume:-1;
            auto* Reloaded=NewObject<UMAYAPresentationSettings>(this); Reloaded->LoadPreferences();
            if(!Check(Pref && FMath::IsNearlyEqual(AppliedVolume,.7f) && Reloaded->MasterVolume==Pref->MasterVolume && Reloaded->bLargeText && Reloaded->bHighContrast && FMath::IsNearlyEqual(Reloaded->InterfaceScale,1.1f),TEXT("Applied non-display preferences persist in the isolated settings file after video revert"))) return;
            AudioValue=MakeShared<TAtomic<float>,ESPMode::ThreadSafe>(-1.f); const auto Result=AudioValue; const auto Device=GetWorld()->GetAudioDevice();
            if(!Check(Device.IsValid(),TEXT("Actual audio device exists"))) return;
            FAudioThread::RunCommandOnAudioThread([Device,Result](){Result->Store(Device->GetTransientPrimaryVolume());});
        }
        if(!Capture(TEXT("05-settings-restored"))) return; if(!Click(TEXT("Resolution.Next")) || !Click(TEXT("Apply"))) return; Advance(); break;
    case 15:
        if(!Screen(EMAYAPlayerScreen::VideoConfirmation) || !Check(AudioValue.IsValid() && FMath::IsNearlyEqual(AudioValue->Load(),AppliedVolume),TEXT("Actual audio-thread device scalar matches applied master volume"))) return;
        if(!Click(TEXT("KeepDisplay"))) return; Advance(); break;
    case 16:
        if(!Screen(EMAYAPlayerScreen::Settings) || !Check(ViewportSize()==ChangedSize,TEXT("Keep retains the actual changed display"))) return;
        if(!Click(TEXT("Resolution.Previous")) || !Click(TEXT("Apply"))) return; Advance(); break;
    case 17: if(!Screen(EMAYAPlayerScreen::VideoConfirmation) || !Click(TEXT("KeepDisplay"))) return; Advance(); break;
    case 18:
        if(!Check(ViewportSize()==OriginalSize && WindowMode()==OriginalMode,TEXT("Original actual display restored through Apply and Keep"))) return;
        if(!Click(TEXT("Back"))) return; Advance(); break;
    case 19: if(!Screen(EMAYAPlayerScreen::MainMenu) || !Click(TEXT("Quit"))) return; Advance(); break;
    case 20: if(!Screen(EMAYAPlayerScreen::QuitConfirmation)) return; if(!Capture(TEXT("06-quit-confirmation"))) return; if(!Click(TEXT("Back"))) return; Advance(); break;
    case 21:
        if(!Screen(EMAYAPlayerScreen::MainMenu) || !Click(TEXT("JoinProject"))) return;
        Event(TEXT("join_project_action_dispatched"),TEXT("Product route uses exact https://quantellics.com/maya and OS default browser; browser/page observation separate; product status=")+P->GetMenuStatus());
        if(!Check(P->GetMenuStatus().StartsWith(TEXT("Opening quantellics.com/maya")),TEXT("Actual default-browser LaunchURL dispatch reports no platform error"))) return;
        Advance(2); break;
    case 22:
        if(auto* Robot=Cast<AMAYANativeDemoPawn>(P->GetPawn()))
        {
            FMAYADeliveryRobotObservation O; const bool Known=Robot->ReadDeliveryState(O);
            Event(TEXT("pre_start_robot_observation"),FString::Printf(TEXT("read=%d known=%d ready=%d grounded=%d supportMask=%u upright=%d closed=%d speed=%.9f omega=%.9f fault=%s presentationReady=%d"),
                Known,O.bKnown,O.bReady,O.bGrounded,uint32(O.SupportMask),O.bUpright,O.bDoorsClosedLatched,O.VelocityCmps.Size(),O.AngularVelocityRadps.Size(),*O.Fault,D && D->GetPlayerPresentation().bReadyForStart));
            Event(TEXT("pre_start_native_telemetry"),Robot->ReadTelemetryJson());
        }
        if(!Screen(EMAYAPlayerScreen::MainMenu) || !Check(!P->HasStartedShift() && D && D->Snapshot().BatteryPercent==100.0 && D->Snapshot().TotalCredits==0,TEXT("Join action preserves main menu and unstarted resources"))) return;
        if(!Click(TEXT("StartShift"))) return;
        if(!Check(P->HasStartedShift() && D->Snapshot().BatteryPercent==100.0 && D->Snapshot().TotalCredits==0,TEXT("Start Shift commits exact initial resources"))) return;
        Key(EKeys::W,IE_Repeat); Advance(.3); break;
    case 23:
        if(!bGameplayStartChecksDone)
        {
            if(!Check(bFirstGameplayObserved && P->GetPlayerScreen()==EMAYAPlayerScreen::Gameplay && D->Snapshot().Phase==EMAYADeliveryPhase::Available,TEXT("First gameplay was rendered and no menu service/accept input leaked"))) return;
            if(!Check(!P->IsInputKeyDown(EKeys::W),TEXT("Held-through-menu W repeat remains suppressed until its real release"))) return;
            Release(EKeys::W); bGameplayStartChecksDone=true;
        }
        if(!Capture(TEXT("07-gameplay-offer"))) return;
        // Observe natural gameplay time; never set hint opacity or its timer.
        if(GetWorld()->GetTimeSeconds()-FirstGameplayWorldSeconds<6.25) return;
        if(!Capture(TEXT("09-control-hint-fading"))) return;
        if(GetWorld()->GetTimeSeconds()-FirstGameplayWorldSeconds<8.0) return;
        if(!Capture(TEXT("10-control-hint-hidden"))) return;
        Key(EKeys::P,IE_Pressed); Advance(.1); break;
    case 24: Release(EKeys::P); if(!Screen(EMAYAPlayerScreen::Pause)) return; if(!Capture(TEXT("08-pause"))) return; if(!Click(TEXT("Controls"))) return; Advance(); break;
    case 25: if(!Screen(EMAYAPlayerScreen::Controls) || !Click(TEXT("Back"))) return; Advance(); break;
    case 26: if(!Screen(EMAYAPlayerScreen::Pause) || !Click(TEXT("Settings"))) return; Advance(); break;
    case 27: if(!Screen(EMAYAPlayerScreen::Settings) || !Click(TEXT("Back"))) return; Advance(); break;
    case 28: if(!Screen(EMAYAPlayerScreen::Pause)) return; Key(EKeys::P,IE_Pressed); Advance(.1); break;
    case 29: Release(EKeys::P); if(!Screen(EMAYAPlayerScreen::Gameplay)) return; if(!Capture(TEXT("11-control-hint-resumed"))) return; Key(EKeys::P,IE_Pressed); Advance(.1); break;
    case 30: Release(EKeys::P); if(!Screen(EMAYAPlayerScreen::Pause) || !Click(TEXT("Quit"))) return; Advance(); break;
    case 31:
        if(!Screen(EMAYAPlayerScreen::QuitConfirmation)) return;
        if(Click(TEXT("ConfirmQuit")) && Check(IsEngineExitRequested(),TEXT("Actual product ConfirmQuit requested engine exit without harness fallback")))
            Finish(true,TEXT("Ordinary startup and bounded native UI callback/hitbox/settings routes passed; independent pixels and browser observation remain separate"));
        break;
    default: Finish(false,TEXT("Unknown UI test stage")); break;
    }
}
void UMAYAPlayerUIScreenTest::EndPlay(const EEndPlayReason::Type Reason)
{
    if(RenderHandle.IsValid()) AHUD::OnHUDPostRender.Remove(RenderHandle);
    if(ScreenshotHandle.IsValid()) FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
    Super::EndPlay(Reason);
}
