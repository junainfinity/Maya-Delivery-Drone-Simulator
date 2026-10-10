#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DDSN07TelemetryWidget.generated.h"

class UTextBlock;
struct FDDSN15ViewModel;

UCLASS()
class DDSCITY_API UDDSN07TelemetryWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual TSharedRef<SWidget> RebuildWidget() override;

    void SetReadouts(const FString& View, const FString& Speed, const FString& Clearance,
        const FString& Prompt, const FString& Recovery,
        bool bInstantComfort, int32 ControlledAdapterPhase);
    void SetViewModel(const FDDSN15ViewModel& Model, bool bInstantComfort,
        const FString& Recovery);

private:
    UPROPERTY()
    TObjectPtr<UTextBlock> ViewText;
    UPROPERTY()
    TObjectPtr<UTextBlock> SpeedText;
    UPROPERTY()
    TObjectPtr<UTextBlock> ClearanceText;
    UPROPERTY()
    TObjectPtr<UTextBlock> ObjectiveText;
    UPROPERTY()
    TObjectPtr<UTextBlock> CargoText;
    UPROPERTY()
    TObjectPtr<UTextBlock> AdapterText;
    UPROPERTY()
    TObjectPtr<UTextBlock> PromptText;
    UPROPERTY()
    TObjectPtr<UTextBlock> ControlsText;
    UPROPERTY()
    TObjectPtr<UTextBlock> RecoveryText;
    UPROPERTY()
    TObjectPtr<UTextBlock> MissionStatusText;
    UPROPERTY()
    TObjectPtr<UTextBlock> TimerStarsText;
    UPROPERTY()
    TObjectPtr<UTextBlock> ResultText;
};
