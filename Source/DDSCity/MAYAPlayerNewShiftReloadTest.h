#pragma once
#include "CoreMinimal.h"
#include "MAYAPlayerScreenState.h"
#include "Components/ActorComponent.h"
#include "MAYAPlayerNewShiftReloadTest.generated.h"

class AMAYADeliveryPlayerController;
class AMAYADeliveryDirector;
class AHUD;
class UCanvas;
class FJsonObject;
struct FMAYADeliveryRobotObservation;

// Explicit Development QA continuation only. Product actions perform the reload.
// Process-local state has weak/serial object identities, never retained worlds.
UCLASS()
class DDSCITY_API UMAYAPlayerNewShiftReloadTest : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYAPlayerNewShiftReloadTest();
    virtual void BeginPlay() override;
    virtual void TickComponent(float Dt, ELevelTick Tick, FActorComponentTickFunction* Function) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    static bool IsEnabled();
    static bool ShouldSuppressInitialAutomation(const UObject* WorldContext);
    static void NoteInitialFixtureApplied(const UObject* WorldContext);
    static void NoteAutomationDriverSpawned(const UObject* WorldContext);
    static bool BeginFatalHandoff(UWorld* World, const FString& Output, const FMAYADeliveryRobotObservation& O);
    static bool BeginOperatingHandoff(UWorld* World, const FString& Output, const FMAYADeliveryRobotObservation& O);
private:
    AMAYADeliveryPlayerController* Player() const;
    AMAYADeliveryDirector* Director() const;
    FIntPoint ViewportSize() const;
    TSharedPtr<FJsonObject> PreferencesReceipt() const;
    TSharedPtr<FJsonObject> WorldReceipt() const;
    bool PreferencesMatch() const;
    void Rendered(AHUD* HUD, UCanvas* Canvas);
    void ScreenshotProcessed();
    void Event(const FString& Name, const FString& Detail = FString());
    bool Check(bool Condition, const FString& Description);
    bool Click(FName Action, bool bArmReload = false);
    bool Capture(const FString& Name);
    void Advance(double Delay = .25, double Bound = 20);
    void Finish(bool Success, const FString& Reason);
    FIntPoint ScreenshotSize, LastRenderedSize;
    EMAYAPlayerScreen LastRenderedScreen = EMAYAPlayerScreen::Gameplay;
    FString PendingCapture;
    uint64 LastRenderedFrame = 0, ScreenshotRequestFrame = 0, ScreenshotProcessedFrame = 0;
    bool bScreenshotProcessed = false;
    FDelegateHandle RenderHandle, ScreenshotHandle;
};
