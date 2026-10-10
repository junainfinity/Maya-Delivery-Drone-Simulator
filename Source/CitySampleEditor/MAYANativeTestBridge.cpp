#include "MAYANativeTestBridge.h"

#include "Editor.h"
#include "LevelEditor.h"
#include "PlayInEditorDataTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedPlayerInput.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/GenericWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace MAYABridge
{
struct FAllowedKey { const TCHAR* Label; FKey Key; };
const FAllowedKey Keys[]={{TEXT("W"),EKeys::W},{TEXT("S"),EKeys::S},{TEXT("A"),EKeys::A},
    {TEXT("D"),EKeys::D},{TEXT("Space"),EKeys::SpaceBar},{TEXT("E"),EKeys::E},{TEXT("R"),EKeys::R}};
struct FState
{
    bool Active=false, Armed=false, InTicker=false, Pending=false, Captured=false;
    int32 NeutralTicks=0, MaxFrames=0, CompletedFrames=0, KeySubmissions=0;
    uint64 NativeTickCount=0, LastNeutralFrame=MAX_uint64;
    int64 PngBytes=0;
    double Started=0, Deadline=0, PendingStarted=0, LastFrameRequest=0, SampleInterval=1./6.;
    FString Directory, PendingPath, Status=TEXT("UNRUN"), Failure;
    TWeakObjectPtr<APawn> Pawn;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<ULocalPlayer> Player;
    TWeakObjectPtr<UGameViewportClient> Viewport;
    FInputDeviceId Device=INPUTDEVICEID_NONE;
    TMap<FKey,double> Holds;
    TSharedPtr<FJsonObject> Receipt=MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> PendingRecord;
    TArray<TSharedPtr<FJsonValue>> Events, Frames;
    FDelegateHandle WorldTick, WorldCleanup, EndPIE, Screenshot, Processed;
    FTSTicker::FDelegateHandle Ticker;
};
FState S;
bool ControlledInstanceUsed=false;
double Now() { return FPlatformTime::Seconds(); }
FString Path(const UObject* Object) { return Object ? Object->GetPathName() : TEXT("NULL"); }
TSharedPtr<FJsonObject> Stamp(const TCHAR* Kind)
{
    auto Row=MakeShared<FJsonObject>(); Row->SetStringField(TEXT("kind"),Kind);
    Row->SetNumberField(TEXT("monotonicSeconds"),Now());
    Row->SetStringField(TEXT("engineFrame"),LexToString(GFrameCounter));
    Row->SetStringField(TEXT("worldPostActorTicks"),LexToString(S.NativeTickCount));
    if(S.World.IsValid()) Row->SetNumberField(TEXT("gameTimeSeconds"),S.World->GetTimeSeconds());
    return Row;
}
FString Telemetry()
{
    APawn* Pawn=S.Pawn.Get(); if(!Pawn) return TEXT("UNAVAILABLE_NO_PAWN");
    UFunction* Function=Pawn->FindFunction(TEXT("ReadTelemetryJson"));
    FStrProperty* Return=Function ? CastField<FStrProperty>(Function->GetReturnProperty()) : nullptr;
    if(!Function || !Function->HasAllFunctionFlags(FUNC_BlueprintPure) || Function->NumParms!=1 || !Return)
        return TEXT("UNAVAILABLE_PURE_TELEMETRY_SIGNATURE_MISMATCH");
    // Only the exact public BlueprintPure no-argument receipt function; no generic command dispatch.
    FStructOnScope Params(Function); Pawn->ProcessEvent(Function,Params.GetStructMemory());
    return Return->GetPropertyValue_InContainer(Params.GetStructMemory());
}
TSharedPtr<FJsonObject> Observe()
{
    auto Row=Stamp(TEXT("actual_context_focus_input"));
    auto PC=S.PC.Get(); auto LP=S.Player.Get(); auto V=S.Viewport.Get(); auto Pawn=S.Pawn.Get();
    Row->SetStringField(TEXT("pawn"),Path(Pawn)); Row->SetStringField(TEXT("controller"),Path(PC));
    Row->SetStringField(TEXT("localPlayer"),Path(LP)); Row->SetStringField(TEXT("viewportClient"),Path(V));
    Row->SetStringField(TEXT("pawnClass"),Pawn ? Path(Pawn->GetClass()) : TEXT("NULL"));
    Row->SetStringField(TEXT("controllerClass"),PC ? Path(PC->GetClass()) : TEXT("NULL"));
    Row->SetStringField(TEXT("localPlayerClass"),LP ? Path(LP->GetClass()) : TEXT("NULL"));
    Row->SetStringField(TEXT("playerInputClass"),PC && PC->PlayerInput ? Path(PC->PlayerInput->GetClass()) : TEXT("NULL"));
    Row->SetStringField(TEXT("inputComponentClass"),Pawn && Pawn->InputComponent ? Path(Pawn->InputComponent->GetClass()) : TEXT("NULL"));
    Row->SetBoolField(TEXT("actualPossession"),PC && Pawn && PC->GetPawn()==Pawn && Pawn->GetController()==PC);
    Row->SetBoolField(TEXT("enhancedPlayerInput"),PC && Cast<UEnhancedPlayerInput>(PC->PlayerInput));
    Row->SetBoolField(TEXT("enhancedInputComponent"),Pawn && Cast<UEnhancedInputComponent>(Pawn->InputComponent));
    Row->SetBoolField(TEXT("slateActive"),FSlateApplication::IsInitialized() && FSlateApplication::Get().IsActive());
    TSharedPtr<SWindow> Window=V ? V->GetWindow() : nullptr;
    TSharedPtr<FGenericWindow> Native=Window ? Window->GetNativeWindow() : nullptr;
    Row->SetBoolField(TEXT("nativeWindowVisible"),Native && Native->IsVisible());
    Row->SetBoolField(TEXT("nativeWindowForeground"),Native && Native->IsForegroundWindow());
    Row->SetBoolField(TEXT("viewportHasFocus"),V && V->Viewport && V->Viewport->HasFocus());
    Row->SetBoolField(TEXT("viewportIgnoresInput"),!V || V->IgnoreInput());
    Row->SetBoolField(TEXT("worldPaused"),!S.World.IsValid() || S.World->IsPaused());
    Row->SetBoolField(TEXT("movementInputIgnored"),!PC || PC->IsMoveInputIgnored());
    Row->SetNumberField(TEXT("inputDeviceId"),S.Device.GetId());
    Row->SetBoolField(TEXT("deviceMapsToLocalPlayer"),V && LP && GEngine && GEngine->GetLocalPlayerFromInputDevice(V,S.Device)==LP);
    TArray<TSharedPtr<FJsonValue>> Raw;
    for(const auto& K:Keys)
    {
        auto R=MakeShared<FJsonObject>(); R->SetStringField(TEXT("key"),K.Label);
        R->SetStringField(TEXT("nativeKey"),K.Key.ToString()); R->SetBoolField(TEXT("down"),PC && PC->IsInputKeyDown(K.Key));
        R->SetStringField(TEXT("observationState"),PC?TEXT("OBSERVED"):TEXT("UNAVAILABLE_CONTROLLER_DESTROYED"));
        Raw.Add(MakeShared<FJsonValueObject>(R));
    }
    Row->SetArrayField(TEXT("rawKeyState"),Raw); Row->SetStringField(TEXT("telemetryJson"),Telemetry());
    return Row;
}
bool Contexts(FString& Why)
{
    auto Pawn=S.Pawn.Get(); auto PC=S.PC.Get(); auto LP=S.Player.Get();
    auto Input=PC ? Cast<UEnhancedPlayerInput>(PC->PlayerInput) : nullptr;
    auto Subsystem=LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
    if(!Pawn || !Input || !Subsystem || Subsystem->GetPlayerInput()!=Input || !Cast<UEnhancedInputComponent>(Pawn->InputComponent))
    { Why=TEXT("Actual local-player Enhanced input stack absent or mismatched"); return false; }
    const UInputMappingContext* Owned=FindObject<UInputMappingContext>(Pawn,TEXT("MAYADemoInput"));
    int32 Priority=INDEX_NONE;
    if(!Owned || Owned->GetOuter()!=Pawn || Owned->GetFName()!=TEXT("MAYADemoInput") || !Subsystem->HasMappingContext(Owned,Priority) || Owned->GetMappings().Num()!=UE_ARRAY_COUNT(Keys))
    { Why=TEXT("Exact owned applied seven-key context absent"); return false; }
    // Public subsystem queries observe active context/key membership. QueryKeysMappedToAction
    // returns unique keys, so this deliberately makes no global inventory or duplicate-count claim.
    S.Receipt->SetStringField(TEXT("inputSubsystem"),Path(Subsystem));
    S.Receipt->SetBoolField(TEXT("subsystemPlayerInputMatches"),true);
    S.Receipt->SetStringField(TEXT("ownedContext"),Path(Owned));
    S.Receipt->SetNumberField(TEXT("ownedContextPriority"),Priority);
    S.Receipt->SetStringField(TEXT("contextObservationScope"),TEXT("Exact pawn-owned context is applied; active action-key membership via public subsystem query; no global context inventory or native duplicate multiplicity proof"));
    TSet<FKey> Seen; TArray<TSharedPtr<FJsonValue>> Mappings;
    for(const auto& M:Owned->GetMappings())
    {
        bool Allowed=false; for(const auto& K:Keys) Allowed|=(M.Key==K.Key);
        if(!Allowed || !M.Action || M.Action->GetOuter()!=Pawn || Seen.Contains(M.Key))
        { Why=TEXT("Owned key/action relationship mismatch"); return false; }
        const TArray<FKey> ActualKeys=Subsystem->QueryKeysMappedToAction(M.Action);
        if(!ActualKeys.Contains(M.Key)) { Why=TEXT("Actual active action-key membership absent"); return false; }
        Seen.Add(M.Key); auto R=MakeShared<FJsonObject>(); R->SetStringField(TEXT("key"),M.Key.ToString());
        R->SetStringField(TEXT("action"),Path(M.Action)); R->SetBoolField(TEXT("activeKeyMembershipObserved"),true);
        TArray<TSharedPtr<FJsonValue>> Queried;
        for(const auto& Key:ActualKeys) Queried.Add(MakeShared<FJsonValueString>(Key.ToString()));
        R->SetArrayField(TEXT("activeUniqueKeysForAction"),Queried);
        R->SetStringField(TEXT("queryScope"),TEXT("Public QueryKeysMappedToAction uses AddUnique; duplicate mapping count unobserved"));
        Mappings.Add(MakeShared<FJsonValueObject>(R));
    }
    S.Receipt->SetArrayField(TEXT("sevenActualMappings"),Mappings);
    S.Receipt->SetBoolField(TEXT("sourceSevenUniqueKeys"),Seen.Num()==UE_ARRAY_COUNT(Keys));
    return Seen.Num()==UE_ARRAY_COUNT(Keys);
}
bool Guard(FString& Why, bool CheckFocus=true)
{
    if(Now()>S.Deadline) { Why=TEXT("Declared phase wall cap reached before request/completion"); return false; }
    auto P=S.Pawn.Get(); auto PC=S.PC.Get(); auto LP=S.Player.Get(); auto V=S.Viewport.Get();
    UClass* Exact=FindObject<UClass>(nullptr,TEXT("/Script/DDSCity.MAYANativeDemoPawn"));
    if(!S.Active || !GEditor || GEditor->bIsSimulatingInEditor || !GEditor->PlayWorld || !P || !Exact || P->GetClass()!=Exact || P->GetWorld()!=GEditor->PlayWorld || P->GetWorld()->WorldType!=EWorldType::PIE)
    { Why=TEXT("Actual exact-pawn PIE relationship absent"); return false; }
    if(!PC || !LP || !V || PC->GetPawn()!=P || P->GetController()!=PC || PC->GetLocalPlayer()!=LP || LP->ViewportClient!=V || !V->Viewport || V->IgnoreInput() || !S.Device.IsValid() || GEngine->GetLocalPlayerFromInputDevice(V,S.Device)!=LP)
    { Why=TEXT("Actual local-player possession/device/viewport relationship absent"); return false; }
    if(P->GetWorld()->IsPaused() || PC->IsMoveInputIgnored()) { Why=TEXT("World paused or movement input disabled"); return false; }
    if(!Contexts(Why)) return false;
    if(CheckFocus)
    {
        auto Window=V->GetWindow(); auto Native=Window ? Window->GetNativeWindow() : nullptr;
        if(!FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsActive() || !Native || !Native->IsVisible() || !Native->IsForegroundWindow() || !V->Viewport->HasFocus())
        { Why=TEXT("Actual visible foreground game focus absent"); return false; }
    }
    return true;
}
bool Send(const FKey& Key, bool Pressed)
{
    // Releases bypass focus/possession/arming, never the still-owned PC/world/device identity.
    auto V=S.Viewport.Get(); auto PC=S.PC.Get(); auto LP=S.Player.Get(); auto World=S.World.Get();
    if(!V || !V->Viewport || !PC || !LP || !World || PC->GetWorld()!=World || PC->GetLocalPlayer()!=LP || LP->GetPlayerController(World)!=PC || !S.Device.IsValid() || !GEngine || GEngine->GetLocalPlayerFromInputDevice(V,S.Device)!=LP) return false;
    return V->InputKey(FInputKeyEventArgs(V->Viewport,S.Device,Key,Pressed?IE_Pressed:IE_Released,Pressed?1.f:0.f,false));
}
void ReleaseAll()
{
    auto Row=Stamp(TEXT("release_all_whitelisted_keys")); TArray<TSharedPtr<FJsonValue>> Sent;
    for(const auto& K:Keys)
    {
        auto R=MakeShared<FJsonObject>(); R->SetStringField(TEXT("key"),K.Label); R->SetBoolField(TEXT("viewportReturn"),Send(K.Key,false));
        Sent.Add(MakeShared<FJsonValueObject>(R));
    }
    Row->SetArrayField(TEXT("submissions"),Sent); S.Events.Add(MakeShared<FJsonValueObject>(Row)); S.Holds.Empty();
    auto PC=S.PC.Get(); bool Neutral=PC!=nullptr;
    if(PC) for(const auto& K:Keys) Neutral&=!PC->IsInputKeyDown(K.Key);
    Row->SetStringField(TEXT("neutralReadbackState"),PC?TEXT("OBSERVED_IMMEDIATELY_AFTER_RELEASE"):TEXT("UNAVAILABLE_CONTROLLER_TEARDOWN"));
    Row->SetBoolField(TEXT("observedNeutral"),Neutral); Row->SetObjectField(TEXT("afterRelease"),Observe());
}
void Stop(const FString& Failure)
{
    if(S.Active) ReleaseAll();
    S.Active=false; S.Armed=false;
    if(!Failure.IsEmpty()) { S.Failure=Failure; S.Status=TEXT("INCOMPLETE"); }
    else if(S.Status!=TEXT("INCOMPLETE")) S.Status=TEXT("STOPPED_RECEIPT_ONLY");
    FWorldDelegates::OnWorldPostActorTick.Remove(S.WorldTick); FWorldDelegates::OnWorldCleanup.Remove(S.WorldCleanup);
    FEditorDelegates::EndPIE.Remove(S.EndPIE); UGameViewportClient::OnScreenshotCaptured().Remove(S.Screenshot);
    FScreenshotRequest::OnScreenshotRequestProcessed().Remove(S.Processed);
    if(!S.InTicker && S.Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(S.Ticker);
    S.Ticker.Reset();
    if(S.Pending) { S.Receipt->SetStringField(TEXT("unfinishedFramePath"),S.PendingPath); S.Receipt->SetBoolField(TEXT("frameRequestIncomplete"),true); }
    if(S.Pending && FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename()==S.PendingPath)
    { FScreenshotRequest::Reset(); S.Receipt->SetBoolField(TEXT("ownedPendingScreenshotCancelled"),true); }
    S.Pending=false;
}
void PostTick(UWorld* World,ELevelTick,float)
{
    if(!S.Active || World!=GEditor->PlayWorld) return;
    ++S.NativeTickCount; FString Why;
    if(!Guard(Why)) { Stop(Why); return; }
    if(!S.Armed)
    {
        bool Neutral=true; for(const auto& K:Keys) Neutral&=!S.PC->IsInputKeyDown(K.Key);
        if(!Neutral) { Stop(TEXT("Key neutrality failed before arming")); return; }
        if(S.LastNeutralFrame!=GFrameCounter) { ++S.NeutralTicks; S.LastNeutralFrame=GFrameCounter; }
        if(S.NeutralTicks>=2) { S.Armed=true; S.Status=TEXT("ARMED_ACTUAL_NEUTRAL_TWO_TICKS"); S.Events.Add(MakeShared<FJsonValueObject>(Observe())); }
    }
}
bool Tick(float)
{
    S.InTicker=true;
    if(S.Active)
    {
        FString Why;
        if(Now()>S.Deadline) Stop(TEXT("Declared phase wall cap reached"));
        else if(!Guard(Why)) Stop(Why);
        else if(S.Pending && Now()-S.PendingStarted>2.) Stop(TEXT("Native screenshot completion exceeded two seconds"));
        else
        {
            TArray<FKey> Due; for(const auto& H:S.Holds) if(Now()>=H.Value) Due.Add(H.Key);
            for(const auto& Key:Due) { const double Deadline=S.Holds.FindChecked(Key); Send(Key,false); S.Holds.Remove(Key); auto R=Stamp(TEXT("hold_watchdog_release")); R->SetStringField(TEXT("key"),Key.ToString()); R->SetNumberField(TEXT("scheduledReleaseMonotonicSeconds"),Deadline); R->SetNumberField(TEXT("lateReleaseSeconds"),FMath::Max(0.,Now()-Deadline)); S.Events.Add(MakeShared<FJsonValueObject>(R)); }
        }
    }
    S.InTicker=false; return S.Active;
}
void Captured(int32 Width,int32 Height,const TArray<FColor>& Pixels)
{
    if(!S.Active || !S.Pending) return;
    FString Why;
    if(!Guard(Why) || Now()-S.PendingStarted>2. || FScreenshotRequest::GetFilename()!=S.PendingPath || Width<=0 || Height<=0 || Width>4096 || Height>4096 || Pixels.Num()!=int64(Width)*Height)
    { Stop(TEXT("Screenshot context, identity, dimensions or focus invalid: ")+Why); return; }
    TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Width,Height,TArrayView64<const FColor>(Pixels.GetData(),Pixels.Num()),Png);
    if(Png.IsEmpty() || Png.Num()>16*1024*1024 || S.PngBytes+Png.Num()>128*1024*1024 || FPaths::FileExists(S.PendingPath) || !FFileHelper::SaveArrayToFile(Png,*S.PendingPath))
    { Stop(TEXT("Native PNG output failure/cap/overwrite")); return; }
    if(Now()-S.PendingStarted>2. || Now()>S.Deadline) { Stop(TEXT("Native PNG completion exceeded declared capture/phase wall cap; file retained incomplete")); return; }
    S.PngBytes+=Png.Num(); S.Captured=true;
    S.PendingRecord->SetObjectField(TEXT("completion"),Observe()); S.PendingRecord->SetNumberField(TEXT("width"),Width); S.PendingRecord->SetNumberField(TEXT("height"),Height);
    S.PendingRecord->SetNumberField(TEXT("pngBytes"),static_cast<double>(Png.Num())); S.PendingRecord->SetNumberField(TEXT("requestToCompletionSeconds"),Now()-S.PendingStarted);
    S.PendingRecord->SetStringField(TEXT("completionEngineFrame"),LexToString(GFrameCounter));
    S.PendingRecord->SetBoolField(TEXT("lateBeyondRequestedSampleInterval"),Now()-S.PendingStarted>S.SampleInterval);
}
void Processed()
{
    if(!S.Active || !S.Pending) return;
    if(Now()-S.PendingStarted>2. || Now()>S.Deadline) { Stop(TEXT("Processed screenshot exceeded capture/phase wall cap; retained incomplete")); return; }
    if(!S.Captured) { Stop(TEXT("Native screenshot processed without captured pixels")); return; }
    S.PendingRecord->SetObjectField(TEXT("requestProcessed"),Stamp(TEXT("native_request_processed")));
    S.Frames.Add(MakeShared<FJsonValueObject>(S.PendingRecord)); ++S.CompletedFrames;
    S.Pending=false; S.Captured=false; S.PendingRecord.Reset();
    UGameViewportClient::OnScreenshotCaptured().Remove(S.Screenshot);
    FScreenshotRequest::OnScreenshotRequestProcessed().Remove(S.Processed);
}
}

bool UMAYANativeTestBridge::RequestRealPIE()
{
    using namespace MAYABridge;
    if(!IsInGameThread() || !GEditor || GEditor->PlayWorld || S.Active || S.Status==TEXT("REAL_PIE_REQUESTED_NOT_YET_OBSERVED")) return false;
    FLevelEditorModule& Level=FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    auto View=Level.GetFirstActiveViewport(); if(!View.IsValid()) return false;
    FRequestPlaySessionParams Params; Params.DestinationSlateViewport=View;
    GEditor->RequestPlaySession(Params); S.Status=TEXT("REAL_PIE_REQUESTED_NOT_YET_OBSERVED");
    S.Receipt=MakeShared<FJsonObject>(); S.Receipt->SetObjectField(TEXT("pieRequest"),Stamp(TEXT("default_real_PIE_request")));
    return true;
}
bool UMAYANativeTestBridge::BeginControlledInput(APawn* ExpectedPawn,bool RequestPossession,const FString& FrameDirectory,float PhaseSeconds,int32 MaxFrames,float RequestedHz)
{
    using namespace MAYABridge;
    if(!IsInGameThread() || S.Active || ControlledInstanceUsed || !GEditor || !GEditor->PlayWorld || GEditor->bIsSimulatingInEditor || !ExpectedPawn || ExpectedPawn->GetWorld()!=GEditor->PlayWorld || PhaseSeconds<=0 || PhaseSeconds>120 || !FMath::IsFinite(PhaseSeconds) || MaxFrames<1 || MaxFrames>120 || !FMath::IsFinite(RequestedHz) || RequestedHz<1 || RequestedHz>30) return false;
    UClass* Exact=FindObject<UClass>(nullptr,TEXT("/Script/DDSCity.MAYANativeDemoPawn"));
    if(!Exact || ExpectedPawn->GetClass()!=Exact) return false;
    auto GI=ExpectedPawn->GetWorld()->GetGameInstance(); if(!GI || GI->GetLocalPlayers().Num()!=1) return false;
    ULocalPlayer* LP=GI->GetLocalPlayers()[0]; APlayerController* PC=LP ? LP->GetPlayerController(ExpectedPawn->GetWorld()) : nullptr;
    if(!PC || !PC->IsLocalController() || (PC->GetPawn() && PC->GetPawn()!=ExpectedPawn) || (ExpectedPawn->GetController() && ExpectedPawn->GetController()!=PC)) return false;
    if(PC->GetPawn()!=ExpectedPawn) { if(!RequestPossession) return false; PC->Possess(ExpectedPawn); }
    if(PC->GetPawn()!=ExpectedPawn || ExpectedPawn->GetController()!=PC || !LP->ViewportClient) return false;
    FString Directory=FPaths::ConvertRelativePathToFull(FrameDirectory),Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("MAYA/NativeBridge/"));
    FPaths::NormalizeDirectoryName(Directory); FPaths::NormalizeDirectoryName(Root);
    if(!FPaths::CollapseRelativeDirectories(Directory) || !Directory.StartsWith(Root+TEXT("/")) || IFileManager::Get().DirectoryExists(*Directory)) return false;
    if(!IFileManager::Get().MakeDirectory(*Directory,true)) return false;
    ControlledInstanceUsed=true;
    TSharedPtr<FJsonObject> PreviousReceipt=S.Receipt;
    S=FState(); S.Active=true; S.Pawn=ExpectedPawn; S.World=ExpectedPawn->GetWorld(); S.PC=PC; S.Player=LP; S.Viewport=LP->ViewportClient;
    S.Receipt->SetObjectField(TEXT("precedingPIERequestReceipt"),PreviousReceipt);
    S.Device=IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(LP->GetPlatformUserId());
    S.Started=Now(); S.Deadline=S.Started+PhaseSeconds; S.Directory=Directory; S.MaxFrames=MaxFrames; S.SampleInterval=1./RequestedHz; S.Status=TEXT("WAITING_TWO_ACTUAL_NEUTRAL_TICKS");
    auto Window=S.Viewport->GetWindow(); auto Native=Window ? Window->GetNativeWindow() : nullptr;
    if(Window && Native) { Window->BringToFront(true); Window->HACK_ForceToFront(); Native->SetWindowFocus(); UWidgetBlueprintLibrary::SetFocusToGameViewport(); }
    FString Why; if(!Guard(Why)) { Stop(Why); return false; }
    ReleaseAll(); S.Events.Add(MakeShared<FJsonValueObject>(Observe()));
    S.WorldTick=FWorldDelegates::OnWorldPostActorTick.AddStatic(&PostTick);
    S.WorldCleanup=FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* World,bool,bool){if(S.Active && S.Pawn.IsValid() && S.Pawn->GetWorld()==World) Stop(TEXT("Owned PIE world cleanup"));});
    S.EndPIE=FEditorDelegates::EndPIE.AddLambda([](bool){if(S.Active) Stop(TEXT("PIE ended; all keys released"));});
    S.Ticker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
    S.Receipt->SetStringField(TEXT("schema"),TEXT("MAYA.NativeInputBridgeReceipt.v1"));
    S.Receipt->SetNumberField(TEXT("phaseWallSeconds"),PhaseSeconds); S.Receipt->SetNumberField(TEXT("maximumFrames"),MaxFrames);
    S.Receipt->SetNumberField(TEXT("requestedSampleHz"),RequestedHz); S.Receipt->SetBoolField(TEXT("constantRateVideoProven"),false);
    S.Receipt->SetStringField(TEXT("freshFrameDirectory"),Directory); S.Receipt->SetBoolField(TEXT("physicsOrTransformWrites"),false);
    return true;
}
bool UMAYANativeTestBridge::SubmitKey(FName Name,bool Pressed,float HoldSeconds)
{
    using namespace MAYABridge;
    if(!IsInGameThread() || !S.Active) return false;
    if(++S.KeySubmissions>256) { Stop(TEXT("Input submission count cap exceeded")); return false; }
    const FAllowedKey* Allowed=nullptr; for(const auto& K:Keys) if(Name==FName(K.Label)) Allowed=&K;
    if(!Allowed) { Stop(TEXT("Nonwhitelisted key request")); return false; }
    if(Pressed)
    {
        FString Why; if(!S.Armed || !Guard(Why) || !FMath::IsFinite(HoldSeconds) || HoldSeconds<=0 || HoldSeconds>2 || S.Holds.Contains(Allowed->Key)) { Stop(TEXT("Press rejected by arming/focus/context/hold guard: ")+Why); return false; }
    }
    const bool Returned=Send(Allowed->Key,Pressed);
    if(Pressed) S.Holds.Add(Allowed->Key,FMath::Min(Now()+HoldSeconds,S.Deadline)); else S.Holds.Remove(Allowed->Key);
    auto Row=Stamp(Pressed?TEXT("viewport_key_press"):TEXT("viewport_key_release")); Row->SetStringField(TEXT("key"),Allowed->Label);
    Row->SetBoolField(TEXT("viewportReturn"),Returned); Row->SetNumberField(TEXT("declaredHoldCapSeconds"),Pressed?HoldSeconds:0);
    Row->SetObjectField(TEXT("actualReadback"),Observe()); S.Events.Add(MakeShared<FJsonValueObject>(Row));
    if(!Returned) Stop(TEXT("Viewport key route returned false; release all"));
    return Returned; // Caller must inspect raw state/telemetry; return value is not physical success.
}
bool UMAYANativeTestBridge::RequestNativeFrame()
{
    using namespace MAYABridge;
    FString Why; IConsoleVariable* Delegate=IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenshotDelegate"));
    if(!IsInGameThread() || !S.Active) return false;
    if(!S.Armed || !Guard(Why)) { Stop(TEXT("Frame rejected by actual readiness/focus guard: ")+Why); return false; }
    if(S.Pending || S.CompletedFrames>=S.MaxFrames) return false; // Explicit busy/cap receipt, never another pending request.
    if(S.LastFrameRequest>0 && Now()-S.LastFrameRequest<S.SampleInterval) return false;
    if(FScreenshotRequest::IsScreenshotRequested() || !Delegate || Delegate->GetInt()==0 || UGameViewportClient::OnScreenshotCaptured().IsBound()) { Stop(TEXT("Native screenshot channel unavailable/conflicting; no override")); return false; }
    S.PendingPath=S.Directory/FString::Printf(TEXT("frame-%04d.png"),S.CompletedFrames);
    if(FPaths::FileExists(S.PendingPath)) return false;
    S.Pending=true; S.Captured=false; S.PendingStarted=Now(); S.PendingRecord=Stamp(TEXT("real_viewport_frame_request"));
    if(S.LastFrameRequest>0) { const double Gap=Now()-S.LastFrameRequest; S.PendingRecord->SetNumberField(TEXT("actualRequestGapSeconds"),Gap); S.PendingRecord->SetNumberField(TEXT("missedPlannedRequestSlots"),FMath::Max(0,FMath::FloorToInt(Gap/S.SampleInterval)-1)); }
    S.LastFrameRequest=S.PendingStarted;
    S.PendingRecord->SetStringField(TEXT("path"),S.PendingPath); S.PendingRecord->SetObjectField(TEXT("request"),Observe());
    S.Screenshot=UGameViewportClient::OnScreenshotCaptured().AddStatic(&Captured);
    S.Processed=FScreenshotRequest::OnScreenshotRequestProcessed().AddStatic(&Processed);
    FScreenshotRequest::RequestScreenshot(S.PendingPath,false,false,false);
    if(FScreenshotRequest::GetFilename()!=S.PendingPath || !FScreenshotRequest::IsScreenshotRequested()) { Stop(TEXT("Native screenshot request identity mismatch")); return false; }
    return true;
}
FString UMAYANativeTestBridge::ReadReceiptJson()
{
    using namespace MAYABridge;
    S.Receipt->SetStringField(TEXT("state"),S.Status); S.Receipt->SetStringField(TEXT("failure"),S.Failure);
    S.Receipt->SetBoolField(TEXT("active"),S.Active); S.Receipt->SetBoolField(TEXT("armed"),S.Armed);
    S.Receipt->SetNumberField(TEXT("neutralActualTicks"),S.NeutralTicks); S.Receipt->SetNumberField(TEXT("completedFrames"),S.CompletedFrames);
    S.Receipt->SetNumberField(TEXT("totalPngBytes"),static_cast<double>(S.PngBytes)); S.Receipt->SetBoolField(TEXT("pendingFrame"),S.Pending);
    S.Receipt->SetNumberField(TEXT("keySubmissions"),S.KeySubmissions);
    S.Receipt->SetArrayField(TEXT("events"),S.Events); S.Receipt->SetArrayField(TEXT("frames"),S.Frames);
    if(S.Active) S.Receipt->SetObjectField(TEXT("currentActualReadback"),Observe());
    S.Receipt->SetBoolField(TEXT("nodePass"),false); S.Receipt->SetBoolField(TEXT("productPass"),false);
    S.Receipt->SetStringField(TEXT("sampling"),TEXT("Actual request/completion engine-frame and monotonic times; no fixed timestep, no implied constant-rate video or dropped-frame concealment"));
    FString Json; FJsonSerializer::Serialize(S.Receipt.ToSharedRef(),TJsonWriterFactory<>::Create(&Json)); return Json;
}
void UMAYANativeTestBridge::EndControlledInput() { MAYABridge::Stop(FString()); }
