#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryVisualCapture.generated.h"

class AMAYANativeDemoPawn;
class APlayerController;
class ACameraActor;
class AHUD;
class FJsonObject;
class FJsonValue;

// Explicitly invoked diagnostic camera. It never drives the robot, advances a
// job, requests doors, pauses the world, changes input, or decides visual QA.
UCLASS(NotBlueprintable, Transient)
class DDSCITY_API AMAYADeliveryVisualCapture : public AActor
{
    GENERATED_BODY()
public:
    AMAYADeliveryVisualCapture();
    bool StartCapture(AMAYANativeDemoPawn* InRobot, APlayerController* InController, const FString& Label);
    void CancelCapture();
    bool IsComplete() const { return bComplete; }
    // Only file capture plus exact view-target restoration, never visual PASS.
    bool DidSaveAllViews() const { return bComplete && bCaptured && bViewRestored && bHudRestored && bManifestWritten; }
    const FString& GetManifestPath() const { return ManifestPath; }
    const FString& GetCaptureStatus() const { return CaptureStatus; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void BeginDestroy() override;

private:
    bool FollowMeasuredRobot();
    void OnScreenshotProcessed();
    void FinishCapture(const FString& Reason, bool bAllFilesSaved = false);
    void WriteManifest();
    bool ReadSavedPng(int64& OutBytes, int32& OutWidth, int32& OutHeight) const;

    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TWeakObjectPtr<APlayerController> Controller;
    TWeakObjectPtr<AActor> PriorViewTarget;
    TWeakObjectPtr<AHUD> CapturedHud;
    UPROPERTY(Transient) TObjectPtr<ACameraActor> OwnedCamera;
    TArray<TSharedPtr<FJsonValue>> Views;
    TSharedPtr<FJsonObject> PendingView;
    FDelegateHandle ProcessedHandle;
    FString OutputDirectory;
    FString CaptureId;
    FString CaptureLabel;
    FString ManifestPath;
    FString PendingFilename;
    FString CaptureStatus = TEXT("not_started");
    FString PriorViewTargetPath;
    double StartedMonotonicS = 0;
    double ViewStartedMonotonicS = 0;
    double RequestedMonotonicS = 0;
    int32 ViewIndex = 0;
    int32 SettledTickCount = 0;
    bool bStarted = false;
    bool bComplete = false;
    bool bScreenshotProcessed = false;
    bool bViewTaken = false;
    bool bViewRestored = false;
    bool bCaptured = false;
    bool bManifestWritten = false;
    bool bHudTaken = false;
    bool bPriorHudVisible = false;
    bool bHudRestored = true;
};
