#pragma once
#include "CoreMinimal.h"
#include "MAYAPlayerScreenState.h"
#include "Components/ActorComponent.h"
#include "MAYAPlayerPackageUIScreenSmoke.generated.h"

class AMAYADeliveryPlayerController;
class AMAYADeliveryDirector;
class AHUD;
class UCanvas;
class FJsonObject;
class FJsonValue;

// Exclusive packaged Development diagnostic. Ordinary menus; actual rendered
// HUD hitboxes only. No physical human-input or independent pixel claim.
UCLASS()
class DDSCITY_API UMAYAPlayerPackageUIScreenSmoke : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYAPlayerPackageUIScreenSmoke();
    virtual void BeginPlay() override;
    virtual void TickComponent(float Dt, ELevelTick Tick, FActorComponentTickFunction* Function) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    AMAYADeliveryPlayerController* Player() const;
    AMAYADeliveryDirector* Director() const;
    FIntPoint ViewportSize() const;
    int32 WindowMode() const;
    bool ReadResolutionRow(FIntPoint& Out) const;
    TSharedPtr<FJsonObject> DisplayReceipt() const;
    void Rendered(AHUD* HUD, UCanvas* Canvas);
    void ScreenshotProcessed();
    void Event(const FString& Name, const FString& Detail = FString());
    bool Check(bool Condition, const FString& Description);
    bool Click(FName Action);
    bool Capture(const FString& Name);
    void Advance(double Delay = 0.4);
    void Finish(bool Success, const FString& Reason);
    FString Output, Phase, ExpectedResultPath, ResolvedSettingsFile, PendingCapture;
    TSet<FString> CompletedCaptures;
    TArray<TSharedPtr<FJsonValue>> Checks;
    TSharedPtr<FJsonObject> StartupDisplay, KeptDisplay, RestoredDisplay;
    FIntPoint Baseline = FIntPoint(1280, 720), Wanted, ScreenshotSize, LastRenderedSize;
    EMAYAPlayerScreen LastRenderedScreen = EMAYAPlayerScreen::Gameplay;
    int32 Step = 0, ResolutionSelections = 0;
    double Started = 0, Due = 0, StepStarted = 0, MenuWorld = -1;
    uint64 LastRenderedFrame = 0, ScreenshotRequestFrame = 0, ScreenshotProcessedFrame = 0;
    bool bFinished = false, bSuccessfulFinish = false, bEvidenceWriteFailed = false;
    bool bScreenshotProcessed = false, bVerify = false, bAnyUIAction = false, bQuitRequested = false;
    FDelegateHandle RenderHandle, ScreenshotHandle;
};
