#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MAYAPresentationSettings.generated.h"

// Only presentation preferences persist. There is no saved shift or cargo state.
UCLASS(Config=GameUserSettings)
class DDSCITY_API UMAYAPresentationSettings : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Config) float MasterVolume = .8f;
    UPROPERTY(Config) float InterfaceScale = 1.f;
    UPROPERTY(Config) bool bLargeText = false;
    UPROPERTY(Config) bool bHighContrast = false;
    void LoadPreferences();
    void SavePreferences();
    void ApplyAudio(UWorld* World) const;
};

struct FMAYAPresentationWorkingSettings
{
    FIntPoint Resolution = FIntPoint(1280, 720);
    int32 WindowMode = 2;
    bool bVSync = true;
    float MasterVolume = .8f, InterfaceScale = 1.f;
    bool bLargeText = false, bHighContrast = false;
};
