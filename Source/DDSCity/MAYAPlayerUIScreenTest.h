#pragma once
#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "MAYAPlayerScreenState.h"
#include "Components/ActorComponent.h"
#include "MAYADeliveryTypes.h"
#include "MAYAPlayerUIScreenTest.generated.h"
class AMAYADeliveryPlayerController;
class AMAYADeliveryDirector;
class AHUD;
class UCanvas;
class FJsonObject;
class FJsonValue;

// Explicit development-only engine callback/hitbox QA. No OS-input or pixel acceptance claim.
UCLASS()
class DDSCITY_API UMAYAPlayerUIScreenTest : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYAPlayerUIScreenTest();
    virtual void BeginPlay() override;
    virtual void TickComponent(float Dt,ELevelTick Tick,FActorComponentTickFunction* Function) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    AMAYADeliveryPlayerController* Player() const;
    AMAYADeliveryDirector* Director() const;
    void Rendered(AHUD* HUD,UCanvas* Canvas);
    void ScreenshotProcessed();
    void Event(const FString& Name,const FString& Detail=FString());
    bool Check(bool Condition,const FString& Description);
    bool Key(FKey Value,EInputEvent EventType);
    bool Click(FName Action);
    bool Capture(const FString& Name);
    void Advance(double Delay=.6);
    void Finish(bool Success,const FString& Reason);
    FIntPoint ViewportSize() const;
    int32 WindowMode() const;
    FString Output,PendingCapture,InitialVolumeValue;
    TSet<FString> CompletedCaptures;
    int32 Step=0;
    double Started=0,Due=0,MenuWorld=0,RevertStarted=0;
    double FirstGameplayWorldSeconds=-1;
    bool bGameplayStartChecksDone=false;
    double ObservedVideoDeadline=0,LastVideoObserved=0,PreviousVideoObserved=0,ObservedRevert=0;
    bool bFinished=false,bFirstGameplayObserved=false,bChangedViewportObserved=false;
    bool bSuccessfulFinish=false,bScreenshotProcessed=false,bNearVideoDeadlineObserved=false,bEvidenceWriteFailed=false;
    uint64 LastRenderedFrame=0;
    uint64 ScreenshotRequestFrame=0,ScreenshotProcessedFrame=0;
    EMAYAPlayerScreen LastRenderedScreen=EMAYAPlayerScreen::Gameplay;
    FIntPoint LastRenderedSize,ScreenshotSize;
    FDelegateHandle RenderHandle,ScreenshotHandle;
    FIntPoint OriginalSize,ChangedSize;
    int32 OriginalMode=2;
    FMAYADeliveryRobotObservation PausedRobot;
    TArray<TSharedPtr<FJsonValue>> Checks;
    TSet<FKey> InjectedHeldKeys;
    TSharedPtr<TAtomic<float>,ESPMode::ThreadSafe> AudioValue;
    float AppliedVolume=0;
};
