#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MAYACityMapCaptureStateSubsystem.generated.h"

class USceneCaptureComponent2D;

// Preserve renderer history for the existing City Sample world-map capture.
// No publisher source/assets, main view, capture cadence or diagnostics change.
UCLASS()
class DDSCITY_API UMAYACityMapCaptureStateSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual void OnWorldComponentsUpdated(UWorld& InWorld) override;
    virtual void Deinitialize() override;
protected:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
private:
    void ConfigureCityMapCapture(UWorld& InWorld);
    TSet<TWeakObjectPtr<USceneCaptureComponent2D>> ObservedCaptures;
    TArray<TWeakObjectPtr<USceneCaptureComponent2D>> ChangedCaptures;
};
