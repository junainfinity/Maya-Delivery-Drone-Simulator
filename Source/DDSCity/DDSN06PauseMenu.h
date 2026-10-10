#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DDSN06PauseMenu.generated.h"

class ADDSCityRollingPawn;
class UTextBlock;

UCLASS()
class DDSCITY_API UDDSN06PauseMenu : public UUserWidget
{
    GENERATED_BODY()

public:
    void SetPawn(ADDSCityRollingPawn* InPawn);
    void RefreshStatus();

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;

private:
    UFUNCTION()
    void OnRecoverClicked();

    UFUNCTION()
    void OnResumeClicked();

    UPROPERTY()
    TObjectPtr<UTextBlock> StatusText;

    TWeakObjectPtr<ADDSCityRollingPawn> Pawn;
};
