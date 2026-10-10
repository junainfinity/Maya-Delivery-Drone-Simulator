#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/PlayerInput.h"
#include "MAYADeliveryInputObserver.generated.h"

class APlayerController;
class FArchive;

// Opt-in observation only. This component never submits input, moves actors,
// changes focus, pauses the world, or decides gameplay acceptance.
UCLASS()
class DDSCITY_API UMAYADeliveryInputObserver : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYADeliveryInputObserver();
    bool Initialize(APlayerController* InController);
    void RecordInputCallback(const FInputKeyParams& Params, bool bControllerHandled);
    void RecordActionCallback(const FKey& Key, bool bActionAccepted);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void BeginDestroy() override;

private:
    struct FObservedKey
    {
        FKey Key;
        uint64 Pressed = 0, Released = 0, Repeated = 0, DoubleClick = 0, Axis = 0, Other = 0;
        uint64 ControllerHandled = 0, ActionAttempts = 0, ActionAccepted = 0;
        double LastCallbackElapsedS = -1, LastActionElapsedS = -1;
    };
    FObservedKey* FindKey(const FKey& Key);
    bool WithinBounds();
    bool WriteRecord(const TSharedRef<class FJsonObject>& Record, bool bTerminal = false);
    void WriteCallbackRecord(const FObservedKey& Entry, const TCHAR* Kind, EInputEvent Event, bool bAccepted);
    void Sample();
    void Stop(const FString& Reason);
    TWeakObjectPtr<APlayerController> ObservedController;
    TArray<FObservedKey> Keys;
    FArchive* TraceFile = nullptr;
    FString TracePath;
    double StartedMonotonicS = 0, LastSampleMonotonicS = -1;
    uint64 SampleCount = 0;
    uint64 CallbackEventCount = 0;
    static constexpr int64 MaxFileBytes = 64ll * 1024 * 1024;
    static constexpr double MaxWallSeconds = 900;
};
