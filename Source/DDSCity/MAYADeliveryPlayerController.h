#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "MAYAPlayerScreenState.h"
#include "MAYAPresentationSettings.h"
#include "MAYADeliveryPlayerController.generated.h"

class AMAYADeliveryDirector;
class UMAYADeliveryInputObserver;

UCLASS()
class DDSCITY_API AMAYADeliveryPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    AMAYADeliveryPlayerController();
    virtual void BeginPlay() override;
    virtual bool InputKey(const FInputKeyParams& Params) override;
    virtual void SetupInputComponent() override;
    virtual void Tick(float Dt) override;
    virtual void OnPossess(APawn* InPawn) override;
    virtual void OnUnPossess() override;
    virtual void ClientRestart_Implementation(APawn* NewPawn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    EMAYAPlayerScreen GetPlayerScreen() const { return Screens.Current(); }
    bool IsMapRouteFocused() const { return bMapRouteFocused; }
    bool HasPlayerMenu() const { return Screens.IsOpen(); }
    bool HasStartedShift() const { return bShiftStarted; }
    bool IsStartupMenuBypassedForAutomation() const { return bAutomationMenuBypass; }
    int32 GetMenuFocus() const { return Screens.FocusIndex; }
    TArray<FMAYAPlayerMenuAction> GetMenuActions() const;
    bool ActivateUIAction(FName Action, int32 Direction = 1);
    void SetMenuFocus(FName Action);
    double GetVideoConfirmationSeconds() const;
    const FString& GetMenuStatus() const { return MenuStatus; }
    const UMAYAPresentationSettings* GetPresentationSettings() const { return PresentationSettings; }
private:
    bool ChangePause(bool bPause);
    void OpenScreen(EMAYAPlayerScreen Screen, bool bRoot = false);
    void BackScreen();
    void UpdateInputOwnership();
    void ReadWorkingSettings();
    bool ApplyWorkingSettings();
    void ConfirmDisplay(bool bKeep);
    TArray<FIntPoint> SupportedResolutions() const;
    FMAYAPlayerScreenState Screens;
    FMAYAPresentationWorkingSettings WorkingSettings, PreviousSettings;
    UPROPERTY() TObjectPtr<UMAYAPresentationSettings> PresentationSettings;
    bool bShiftStarted = false, bAutomationMenuBypass = false, bMenuInputOwned = false;
    bool bCursorModeActive = false, bShiftWinPresented = false, bShiftLostPresented = false, bInputModeInitialized = false;
    bool bMenuShiftHeld = false;
    TSet<FKey> HeldDriveKeys, SuppressedDriveKeys;
    bool bVideoConfirmationPending = false;
    double VideoConfirmationDeadline = 0, LastAudioApplyRealSeconds = -1;
    FString MenuStatus;
    void RefreshPawnContext();
    void RemovePawnContext();
    AMAYADeliveryDirector* Delivery() const;
    void Accept();
    void Interact();
    void Next();
    void Retry();
    void Recover();
    void TogglePause();
    void ToggleMap();
    void ToggleCamera();
    bool bMapRouteFocused = false;
    TWeakObjectPtr<APawn> ContextPawn;
    UPROPERTY() TObjectPtr<UMAYADeliveryInputObserver> InputObserver;
};
