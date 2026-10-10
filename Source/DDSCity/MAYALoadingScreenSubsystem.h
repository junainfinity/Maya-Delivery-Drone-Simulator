#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/World.h"
#include "MAYALoadingScreenSubsystem.generated.h"

class UMaterialInterface;
class UGameViewportClient;
class SWidget;

// Owned runtime presentation. The original City Sample BP game instance and
// its Init/save/preload behavior stay intact; subsystems initialize in Super::Init.
UCLASS()
class DDSCITY_API UMAYALoadingScreenSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
private:
    void PrepareMovie();
    void OnPreLoadMap(const FString& MapName);
    void OnPostLoadMap(UWorld* World);
    void OnWorldTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
    void OnWorldTearDown(UWorld* World);
    void AddOverlay(UWorld* World);
    void Finish(const TCHAR* Reason);
    void CaptureIfRequested();
    UPROPERTY() TObjectPtr<UMaterialInterface> LogoMaterial;
    TSharedPtr<SWidget> MovieWidget;
    TSharedPtr<SWidget> OverlayWidget;
    TWeakObjectPtr<UGameViewportClient> OverlayViewport;
    TWeakObjectPtr<UWorld> LoadingWorld;
    FDelegateHandle PreLoadHandle, PostLoadHandle, WorldTickHandle, TearDownHandle;
    double LoadingWorldStartSeconds = -1;
    bool bMovieOwned = false;
    bool bOverlayAdded = false;
    bool bActive = false;
    bool bCaptureRequested = false;
};
