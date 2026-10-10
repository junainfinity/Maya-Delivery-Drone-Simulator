#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MAYANativeTestBridge.generated.h"

class APawn;

/** Editor-only controlled native input. No pawn callback, transform or physics mutation. */
UCLASS()
class CITYSAMPLEEDITOR_API UMAYANativeTestBridge : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static bool RequestRealPIE();

    // One instance; fresh frame directory under Saved/MAYA/NativeBridge only.
    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static bool BeginControlledInput(APawn* ExpectedPawn, bool bRequestNormalPossession,
        const FString& FreshFrameDirectory, float PhaseWallSeconds=60.f, int32 MaximumFrames=120,
        float RequestedSampleHz=6.f);

    // W/S/A/D/Space/E/R only. Press watchdog <=2 seconds; release is always permitted.
    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static bool SubmitKey(FName Key, bool bPressed, float HoldWallSeconds=1.f);

    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static bool RequestNativeFrame();

    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static FString ReadReceiptJson();

    UFUNCTION(BlueprintCallable, Category="MAYA|Native Test")
    static void EndControlledInput();
};
