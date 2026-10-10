#include "MAYAStartupWindowObserver.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "Windows/WindowsHWrapper.h"

namespace MAYAWindowDiagnostic
{
using FObject = TSharedPtr<FJsonObject>;
bool RequestUsed = false;
int32 SnapshotCount = 0;
TWeakPtr<IAssetViewport> BoundViewport;
TWeakPtr<FSceneViewport> BoundScene;
TWeakPtr<SViewport> BoundWidget;
TWeakPtr<SWindow> BoundWindow;
TWeakPtr<FGenericWindow> BoundNative;
HWND BoundHandle = nullptr;

FString Hex(UPTRINT Value) { return FString::Printf(TEXT("0x%0*llX"), int32(sizeof(void*)*2), (unsigned long long)Value); }
FString Handle(HWND Value) { return Hex(reinterpret_cast<UPTRINT>(Value)); }
FString Pointer(const void* Value) { return Hex(reinterpret_cast<UPTRINT>(Value)); }
FString Json(const FObject& Object)
{
    FString Result; const auto Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Object.ToSharedRef(), Writer); return Result;
}
FObject Stamp()
{
    auto R=MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("monotonicSeconds"),FPlatformTime::Seconds());
    R->SetStringField(TEXT("engineFrame"),LexToString(GFrameCounter));
    R->SetNumberField(TEXT("ownedPid"),GetCurrentProcessId());
    R->SetStringField(TEXT("targetScope"),TEXT("active_editor_level_sceneviewport"));
    R->SetBoolField(TEXT("PIE"),false); R->SetBoolField(TEXT("robotSpawn"),false);
    R->SetBoolField(TEXT("failedPIETargetObserved"),false); return R;
}
void Session(const FObject& R, DWORD Pid)
{
    DWORD Id=0; SetLastError(0); const bool Ok=Pid && ProcessIdToSessionId(Pid,&Id);
    R->SetStringField(TEXT("sessionState"),Ok?TEXT("OBSERVED"):TEXT("UNAVAILABLE"));
    if(Ok) R->SetNumberField(TEXT("sessionId"),Id);
    else R->SetNumberField(TEXT("sessionError"),GetLastError());
}
FObject Window(HWND H)
{
    auto R=MakeShared<FJsonObject>(); R->SetStringField(TEXT("hwnd"),Handle(H));
    if(!H) { R->SetStringField(TEXT("state"),TEXT("NULL")); return R; }
    if(!IsWindow(H)) { R->SetStringField(TEXT("state"),TEXT("UNAVAILABLE_INVALID_WINDOW")); return R; }
    DWORD Pid=0; const DWORD Tid=GetWindowThreadProcessId(H,&Pid);
    R->SetStringField(TEXT("state"),Tid?TEXT("OBSERVED"):TEXT("UNAVAILABLE"));
    R->SetNumberField(TEXT("pid"),Pid); R->SetNumberField(TEXT("tid"),Tid);
    R->SetBoolField(TEXT("sameProcess"),Pid==GetCurrentProcessId()); Session(R,Pid);
    wchar_t Class[256]={}; SetLastError(0); const int32 N=GetClassNameW(H,Class,UE_ARRAY_COUNT(Class));
    R->SetStringField(TEXT("classState"),N?TEXT("OBSERVED"):TEXT("UNAVAILABLE"));
    if(N) R->SetStringField(TEXT("class"),FString(Class)); else R->SetNumberField(TEXT("classError"),GetLastError());
    // Never query an external application/document title.
    if(Pid==GetCurrentProcessId())
    {
        wchar_t Title[512]={}; SetLastError(0); const int32 Size=GetWindowTextW(H,Title,UE_ARRAY_COUNT(Title));
        const DWORD Error=GetLastError(); R->SetStringField(TEXT("title"),FString(Title));
        R->SetStringField(TEXT("titleState"),(Size || !Error)?TEXT("OBSERVED"):TEXT("UNAVAILABLE"));
        R->SetBoolField(TEXT("titleMayBeTruncated"),Size==UE_ARRAY_COUNT(Title)-1);
        R->SetNumberField(TEXT("titleError"),Error);
    }
    else R->SetStringField(TEXT("titleState"),TEXT("REDACTED_EXTERNAL_NOT_QUERIED"));
    SetLastError(0); const LONG_PTR Style=GetWindowLongPtrW(H,GWL_STYLE); const DWORD StyleError=GetLastError();
    SetLastError(0); const LONG_PTR ExStyle=GetWindowLongPtrW(H,GWL_EXSTYLE); const DWORD ExError=GetLastError();
    R->SetStringField(TEXT("styleState"),StyleError?TEXT("UNAVAILABLE"):TEXT("OBSERVED"));
    R->SetStringField(TEXT("exStyleState"),ExError?TEXT("UNAVAILABLE"):TEXT("OBSERVED"));
    R->SetNumberField(TEXT("styleError"),StyleError); R->SetNumberField(TEXT("exStyleError"),ExError);
    if(!StyleError)
    {
        R->SetStringField(TEXT("style"),Hex(UPTRINT(Style)));
        R->SetBoolField(TEXT("child"),(Style&WS_CHILD)!=0); R->SetBoolField(TEXT("popup"),(Style&WS_POPUP)!=0);
        R->SetBoolField(TEXT("disabled"),(Style&WS_DISABLED)!=0);
    }
    if(!ExError)
    {
        R->SetStringField(TEXT("exStyle"),Hex(UPTRINT(ExStyle)));
        R->SetBoolField(TEXT("toolWindow"),(ExStyle&WS_EX_TOOLWINDOW)!=0);
        R->SetBoolField(TEXT("topmost"),(ExStyle&WS_EX_TOPMOST)!=0);
    }
    R->SetBoolField(TEXT("visible"),IsWindowVisible(H)!=0); R->SetBoolField(TEXT("enabled"),IsWindowEnabled(H)!=0);
    R->SetBoolField(TEXT("minimized"),IsIconic(H)!=0);
    return R;
}
FObject Hierarchy(HWND H)
{
    auto R=Window(H); if(!H || !IsWindow(H)) return R;
    R->SetObjectField(TEXT("parent"),Window(GetParent(H)));
    R->SetObjectField(TEXT("owner"),Window(GetWindow(H,GW_OWNER)));
    R->SetObjectField(TEXT("root"),Window(GetAncestor(H,GA_ROOT)));
    R->SetObjectField(TEXT("rootOwner"),Window(GetAncestor(H,GA_ROOTOWNER)));
    for(int32 Kind=0;Kind<2;++Kind)
    {
        TArray<TSharedPtr<FJsonValue>> Rows; TSet<UPTRINT> Seen; HWND Current=H;
        bool Cycle=false;
        for(int32 Hop=0;Current && Hop<8;++Hop)
        {
            if(Seen.Contains(reinterpret_cast<UPTRINT>(Current))) { Cycle=true; break; }
            Seen.Add(reinterpret_cast<UPTRINT>(Current)); Rows.Add(MakeShared<FJsonValueObject>(Window(Current)));
            Current=Kind?GetWindow(Current,GW_OWNER):GetParent(Current);
        }
        const TCHAR* Prefix=Kind?TEXT("ownerChain"):TEXT("parentChain");
        R->SetArrayField(Prefix,Rows); R->SetBoolField(FString(Prefix)+TEXT("Cycle"),Cycle);
        R->SetBoolField(FString(Prefix)+TEXT("Truncated"),Current && !Cycle);
    }
    return R;
}
struct FEnumeration
{
    TArray<TSharedPtr<FJsonValue>> Rows; TSet<UPTRINT> Seen;
    int32 Visited=0; bool Truncated=false, InChildEnumeration=false;
};
BOOL CALLBACK Enumerate(HWND H, LPARAM Parameter)
{
    auto& E=*reinterpret_cast<FEnumeration*>(Parameter);
    if(++E.Visited>4096) { E.Truncated=true; return 0; }
    DWORD Pid=0; GetWindowThreadProcessId(H,&Pid);
    if(Pid==GetCurrentProcessId() && !E.Seen.Contains(reinterpret_cast<UPTRINT>(H)))
    {
        if(E.Rows.Num()>=64) { E.Truncated=true; return 0; }
        E.Seen.Add(reinterpret_cast<UPTRINT>(H)); E.Rows.Add(MakeShared<FJsonValueObject>(Window(H)));
        if(!E.InChildEnumeration)
        {
            E.InChildEnumeration=true; EnumChildWindows(H,Enumerate,Parameter); E.InChildEnumeration=false;
        }
    }
    return E.Truncated?0:1;
}
FObject Snapshot()
{
    auto R=Stamp(); R->SetNumberField(TEXT("snapshotOrdinal"),++SnapshotCount);
    if(SnapshotCount>64) { R->SetStringField(TEXT("failure"),TEXT("64 snapshot cap")); return R; }
    if(!IsInGameThread() || !GEditor || GEditor->PlayWorld || !FSlateApplication::IsInitialized())
    { R->SetStringField(TEXT("failure"),TEXT("Normal editor game-thread/Slate identity required; PIE prohibited")); return R; }
    auto* Module=FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
    auto Viewport=Module?Module->GetFirstActiveViewport():nullptr;
    auto Scene=Viewport?Viewport->GetSharedActiveViewport():nullptr;
    auto Widget=Scene?Scene->GetViewportWidget().Pin():nullptr;
    auto W=Widget?FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()):nullptr;
    auto N=W?W->GetNativeWindow():nullptr;
    HWND H=N?static_cast<HWND>(N->GetOSWindowHandle()):nullptr;
    DWORD Pid=0; const DWORD Tid=H?GetWindowThreadProcessId(H,&Pid):0;
    if(!Viewport || !Scene || !Widget || !W || !N || !H || !Tid || Pid!=GetCurrentProcessId())
    { R->SetBoolField(TEXT("editorReady"),false); R->SetStringField(TEXT("failure"),TEXT("Actual owned active level SceneViewport/window/HWND absent")); return R; }
    if(BoundHandle && (BoundViewport.Pin()!=Viewport || BoundScene.Pin()!=Scene || BoundWidget.Pin()!=Widget || BoundWindow.Pin()!=W || BoundNative.Pin()!=N || BoundHandle!=H))
    { R->SetStringField(TEXT("failure"),TEXT("Bound viewport/window/native HWND identity changed")); return R; }
    BoundViewport=Viewport; BoundScene=Scene; BoundWidget=Widget; BoundWindow=W; BoundNative=N; BoundHandle=H;
    R->SetBoolField(TEXT("editorReady"),true);
    R->SetStringField(TEXT("selectionPath"),TEXT("LevelEditor.GetFirstActiveViewport -> GetSharedActiveViewport -> GetViewportWidget -> Slate.FindWidgetWindow -> GetNativeWindow -> GetOSWindowHandle"));
    R->SetStringField(TEXT("assetViewport"),Pointer(Viewport.Get())); R->SetStringField(TEXT("sceneViewport"),Pointer(Scene.Get()));
    R->SetStringField(TEXT("viewportWidget"),Pointer(Widget.Get())); R->SetStringField(TEXT("slateWindow"),Pointer(W.Get()));
    R->SetStringField(TEXT("nativeWindow"),Pointer(N.Get())); R->SetStringField(TEXT("targetHwnd"),Handle(H));
    R->SetBoolField(TEXT("slateWindowVisible"),W->IsVisible()); R->SetBoolField(TEXT("slateRegularWindow"),W->IsRegularWindow());
    R->SetNumberField(TEXT("slateWindowType"),int32(W->GetType())); R->SetBoolField(TEXT("nativeVisible"),N->IsVisible());
    R->SetBoolField(TEXT("nativeForeground"),N->IsForegroundWindow()); R->SetBoolField(TEXT("viewportHasFocus"),Scene->HasFocus());
    R->SetBoolField(TEXT("slateActive"),FSlateApplication::Get().IsActive());
    UWorld* World=Viewport->GetAssetViewportClient().GetWorld();
    R->SetStringField(TEXT("world"),World?World->GetPathName():TEXT("UNAVAILABLE"));
    if(World) { R->SetNumberField(TEXT("worldType"),int32(World->WorldType)); R->SetNumberField(TEXT("worldTimeSeconds"),World->GetTimeSeconds()); }
    const HWND ForegroundBefore=GetForegroundWindow();
    R->SetObjectField(TEXT("target"),Hierarchy(H)); R->SetObjectField(TEXT("foreground"),Hierarchy(ForegroundBefore));
    R->SetBoolField(TEXT("targetEqualsForeground"),H==ForegroundBefore);
    GUITHREADINFO Gui={}; Gui.cbSize=sizeof(Gui); SetLastError(0);
    auto G=MakeShared<FJsonObject>(); const bool GuiOk=GetGUIThreadInfo(Tid,&Gui)!=0;
    G->SetStringField(TEXT("state"),GuiOk?TEXT("OBSERVED"):TEXT("UNAVAILABLE")); G->SetNumberField(TEXT("error"),GetLastError());
    if(GuiOk) { G->SetStringField(TEXT("activeHwnd"),Handle(Gui.hwndActive)); G->SetStringField(TEXT("focusHwnd"),Handle(Gui.hwndFocus)); }
    R->SetObjectField(TEXT("targetGuiThread"),G);
    wchar_t DesktopName[256]={}; DWORD Needed=0; SetLastError(0);
    const HDESK Desktop=GetThreadDesktop(Tid); const bool DesktopOk=Desktop && GetUserObjectInformationW(Desktop,UOI_NAME,DesktopName,sizeof(DesktopName),&Needed);
    R->SetStringField(TEXT("targetDesktopState"),DesktopOk?TEXT("OBSERVED"):TEXT("UNAVAILABLE"));
    if(DesktopOk) R->SetStringField(TEXT("targetDesktopName"),FString(DesktopName)); else R->SetNumberField(TEXT("targetDesktopError"),GetLastError());
    FEnumeration E; SetLastError(0); const bool EnumOk=EnumWindows(Enumerate,reinterpret_cast<LPARAM>(&E))!=0;
    const DWORD EnumError=EnumOk?0:GetLastError(); R->SetArrayField(TEXT("ownedWindows"),E.Rows);
    R->SetBoolField(TEXT("ownedEnumerationComplete"),EnumOk && !E.Truncated && !EnumError);
    R->SetBoolField(TEXT("ownedEnumerationTruncated"),E.Truncated); R->SetNumberField(TEXT("ownedEnumerationError"),EnumError);
    const HWND ForegroundAfter=GetForegroundWindow(); R->SetStringField(TEXT("foregroundHwndAtSnapshotEnd"),Handle(ForegroundAfter));
    R->SetBoolField(TEXT("foregroundChangedDuringSnapshot"),ForegroundAfter!=ForegroundBefore);
    R->SetStringField(TEXT("consistencyScope"),TEXT("Read-only non-atomic native queries; foreground before/end recorded"));
    return R;
}
}

FString UMAYAStartupWindowObserver::ReadStartupConfigurationJson()
{
    using namespace MAYAWindowDiagnostic; auto R=Stamp();
    R->SetNumberField(TEXT("snapshotOrdinal"),++SnapshotCount);
    if(SnapshotCount>64) { R->SetStringField(TEXT("failure"),TEXT("64 snapshot cap")); return Json(R); }
    TArray<FString> Values;
    if(!IsInGameThread() || !GConfig) { R->SetStringField(TEXT("failure"),TEXT("Config game-thread identity unavailable")); return Json(R); }
    GConfig->GetArray(TEXT("/Script/PythonScriptPlugin.PythonScriptPluginSettings"),TEXT("StartupScripts"),Values,GEngineIni);
    TArray<TSharedPtr<FJsonValue>> Rows; for(const auto& V:Values) Rows.Add(MakeShared<FJsonValueString>(V));
    R->SetArrayField(TEXT("cachedEngineStartupScripts"),Rows);
    UClass* Class=FindObject<UClass>(nullptr,TEXT("/Script/PythonScriptPlugin.PythonScriptPluginSettings"));
    auto* Property=Class?FindFProperty<FArrayProperty>(Class,TEXT("StartupScripts")):nullptr;
    auto* Inner=Property?CastField<FStrProperty>(Property->Inner):nullptr;
    UObject* Default=Class?Class->GetDefaultObject(false):nullptr;
    if(!Property || !Inner || !Default) R->SetStringField(TEXT("failure"),TEXT("Exact loaded Python settings array/CDO unavailable"));
    else
    {
        FScriptArrayHelper Array(Property,Property->ContainerPtrToValuePtr<void>(Default)); Rows.Empty();
        for(int32 Index=0;Index<Array.Num();++Index) Rows.Add(MakeShared<FJsonValueString>(Inner->GetPropertyValue(Array.GetRawPtr(Index))));
        R->SetArrayField(TEXT("effectiveDefaultStartupScripts"),Rows);
        R->SetStringField(TEXT("settingsDefault"),Default->GetPathName());
    }
    return Json(R);
}
FString UMAYAStartupWindowObserver::ReadNativeFrameJson()
{
    using namespace MAYAWindowDiagnostic; auto R=Stamp(); R->SetNumberField(TEXT("snapshotOrdinal"),++SnapshotCount);
    if(SnapshotCount>64) R->SetStringField(TEXT("failure"),TEXT("64 snapshot cap")); return Json(R);
}
FString UMAYAStartupWindowObserver::ReadEditorWindowSnapshotJson() { return MAYAWindowDiagnostic::Json(MAYAWindowDiagnostic::Snapshot()); }
FString UMAYAStartupWindowObserver::RequestEditorForegroundOnceJson()
{
    using namespace MAYAWindowDiagnostic; auto R=Stamp();
    if(RequestUsed || !BoundHandle || SnapshotCount>60) { R->SetStringField(TEXT("failure"),TEXT("One request requires existing bound target and four reserved snapshots")); return Json(R); }
    RequestUsed=true; auto Before=Snapshot(); R->SetObjectField(TEXT("before"),Before);
    if(Before->HasField(TEXT("failure"))) { R->SetStringField(TEXT("failure"),TEXT("Pre-request bound identity failed")); return Json(R); }
    auto W=BoundWindow.Pin(); if(!W) { R->SetStringField(TEXT("failure"),TEXT("Bound SWindow expired")); return Json(R); }
    // Only stimulus; no direct Win32 foreground call, focus chain, input or arbitrary handle.
    W->HACK_ForceToFront();
    R->SetStringField(TEXT("requestResult"),TEXT("UNOBSERVED_VOID_ENGINE_REQUEST"));
    R->SetObjectField(TEXT("immediateAfter"),Snapshot()); R->SetNumberField(TEXT("requestCount"),1);
    return Json(R);
}
