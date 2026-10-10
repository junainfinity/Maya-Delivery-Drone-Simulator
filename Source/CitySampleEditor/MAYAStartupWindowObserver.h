#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MAYAStartupWindowObserver.generated.h"

// Inactive editor diagnostic. No registration, startup hook, actor or input dependency.
UCLASS()
class CITYSAMPLEEDITOR_API UMAYAStartupWindowObserver : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category="MAYA|WindowDiagnostic")
    static FString ReadNativeFrameJson();

    UFUNCTION(BlueprintPure, Category="MAYA|WindowDiagnostic")
    static FString ReadStartupConfigurationJson();

    UFUNCTION(BlueprintPure, Category="MAYA|WindowDiagnostic")
    static FString ReadEditorWindowSnapshotJson();

    // Exactly one target-derived void engine request per process; no arbitrary HWND argument.
    UFUNCTION(BlueprintCallable, Category="MAYA|WindowDiagnostic")
    static FString RequestEditorForegroundOnceJson();
};
