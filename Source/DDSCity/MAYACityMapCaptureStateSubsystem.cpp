#include "MAYACityMapCaptureStateSubsystem.h"
#include "MAYADeliveryGameMode.h"
#include "Game/CitySampleWorldInfo.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"

bool UMAYACityMapCaptureStateSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UMAYACityMapCaptureStateSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);
    // UE5.3 invokes this before GameMode::StartPlay and actor BeginPlay,
    // hence before CitySampleWorldInfo's initial CaptureScene request.
    ConfigureCityMapCapture(InWorld);
}

void UMAYACityMapCaptureStateSubsystem::OnWorldComponentsUpdated(UWorld& InWorld)
{
    Super::OnWorldComponentsUpdated(InWorld);
    ConfigureCityMapCapture(InWorld);
}

void UMAYACityMapCaptureStateSubsystem::ConfigureCityMapCapture(UWorld& InWorld)
{
    if (!Cast<AMAYADeliveryGameMode>(InWorld.GetAuthGameMode())) return;
    for (TActorIterator<ACitySampleWorldInfo> It(&InWorld); It; ++It)
    {
        USceneCaptureComponent2D* Capture = It->GetWorldMapSceneCapture();
        if (!IsValid(Capture) || ObservedCaptures.Contains(Capture)) continue;
        ObservedCaptures.Add(Capture);
        const bool bOldPersist = Capture->bAlwaysPersistRenderingState;
        // Only the existing on-demand map capture needs retained view state.
        // Preserve any continuous-capture policy supplied by the city itself.
        if (!Capture->bCaptureEveryFrame && !bOldPersist)
        {
            Capture->bAlwaysPersistRenderingState = true;
            ChangedCaptures.Add(Capture);
        }
        UE_LOG(LogTemp, Display, TEXT("MAYA city-map capture actor=%s component=%s captureEveryFrame=%s persistBefore=%s persistAfter=%s actorBegunPlay=%s"),
            *It->GetPathName(), *Capture->GetPathName(), Capture->bCaptureEveryFrame ? TEXT("true") : TEXT("false"),
            bOldPersist ? TEXT("true") : TEXT("false"), Capture->bAlwaysPersistRenderingState ? TEXT("true") : TEXT("false"),
            It->HasActorBegunPlay() ? TEXT("true") : TEXT("false"));
    }
}

void UMAYACityMapCaptureStateSubsystem::Deinitialize()
{
    for (const TWeakObjectPtr<USceneCaptureComponent2D>& Weak : ChangedCaptures)
        if (USceneCaptureComponent2D* Capture = Weak.Get())
            if (Capture->bAlwaysPersistRenderingState) Capture->bAlwaysPersistRenderingState = false;
    ChangedCaptures.Reset(); ObservedCaptures.Reset();
    // Component teardown owns destruction of its allocated native view state.
    Super::Deinitialize();
}
