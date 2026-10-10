#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DDSOriginalAssemblyStageSubsystem.generated.h"

class ACameraActor;
class AGameModeBase;
class APlayerController;
class APawn;
class ACitySamplePlayerCameraManager;
class UGameViewportClient;
class FViewport;

enum class EDDSOriginalStagePendingReason : uint8
{
    None, WorldUnavailable, WorldNotBegunPlay, PersistentLevelUnavailable,
    ControllerUnavailable, ControllerNotLocal, ControllerNotBegunPlay,
    PawnUnavailable, PawnNotBegunPlay, ViewTargetUnavailable, CameraManagerUnavailable,
    AlternateCameraActive, CentreSupportMissing, SuppliedSpawnFailed, MeshBoundsUnavailable,
    CornerSupportMissing, SupportNonPlanar, GeometryMoveFailed, ComponentPoseMismatch,
    PlacedBoundsMismatch, ViewObstructed, CameraSpawnFailed, ContextChanged, Ready
};

// Explicit local-development visual preview, never a driving/readiness producer.
UCLASS()
class DDSCITY_API UDDSOriginalAssemblyStageSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override;
    virtual TStatId GetStatId() const override;
    virtual void Deinitialize() override;
private:
    bool TryStage();
    void ReleaseOwnedPreview();
    void SetPendingReason(EDDSOriginalStagePendingReason Reason);
    bool StageBudgetLive() const;
    void ConfigureScreenshot();
    void BeginScreenshot();
    void TickScreenshot();
    bool ScreenshotContextLive() const;
    bool ScreenshotBudgetLive() const;
    void OnOwnedViewportEndDraw();
    void OnOwnedViewportRendered(FViewport* RenderedViewport);
    bool FinishScreenshot(const TCHAR* Refusal = nullptr);
    void ConfigureRecording();
    void BeginRecording();
    void TickRecording();
    bool RecordingContextLive() const;
    bool RecordingBudgetLive() const;
    void OnRecordingViewportEndDraw();
    void OnRecordingViewportRendered(FViewport* RenderedViewport);
    bool WriteRecordingBytes(const FString& Path, const TArray64<uint8>& Bytes);
    bool FinishRecording(const TCHAR* Refusal = nullptr);
    struct FRecordingFrame
    {
        FString FileName;
        double RenderCallbackSeconds = 0.0, ReadbackStartedSeconds = 0.0, ReadbackFinishedSeconds = 0.0;
        double EncodeStartedSeconds = 0.0, EncodeFinishedSeconds = 0.0;
        double WriteStartedSeconds = 0.0, WriteFinishedSeconds = 0.0;
        int64 RenderCallbackIndex = 0, Bytes = 0;
        uint64 GameFrameCounter = 0;
    };
    bool bRecordingOptIn = false, bRecordingPending = false, bRecordingReadback = false;
    bool bRecordingFinalizing = false, bRecordingWritten = false;
    double RecordingArmedSeconds = 0.0, RecordingStartedSeconds = -1.0;
    double LastRecordingRenderSeconds = -1.0, LastRecordingSampleSeconds = -1.0;
    int32 RecordingOwnedDraws = 0;
    int64 RecordingRenderedCallbacks = 0, RecordingFrameBytes = 0;
    FString RecordingDir, RecordingNonce, RecordingUtcStart;
    TArray<FRecordingFrame> RecordingFrames;
    FDelegateHandle RecordingEndDrawHandle, RecordingRenderedHandle;
    FViewport* RecordingRenderViewport = nullptr;
    UPROPERTY(Transient) TWeakObjectPtr<UGameViewportClient> RecordingViewport;
    UPROPERTY(Transient) TWeakObjectPtr<AGameModeBase> RecordingOriginalMode;
    bool bFinished = false;
    double StartedSeconds = 0.0;
    double ReadyStartedSeconds = -1.0;
    double NextAttemptSeconds = 0.0;
    int32 SpawnAttempts = 0;
    int32 CandidateCursor = 0;
    EDDSOriginalStagePendingReason LastPendingReason = EDDSOriginalStagePendingReason::None;
    int32 PendingTransitionLogs = 0;
    bool bScreenshotOptIn = false;
    bool bScreenshotPending = false;
    bool bReadbackInProgress = false;
    bool bScreenshotWritten = false;
    double ScreenshotStartedSeconds = 0.0;
    int32 OwnedDrawsAfterStage = 0;
    FString ScreenshotPath;
    FString ScreenshotNonce;
    FDelegateHandle EndDrawHandle;
    FDelegateHandle ViewportRenderedHandle;
    FViewport* ScreenshotRenderViewport = nullptr; // Borrowed only while the bound owning viewport client remains exact.
    UPROPERTY(Transient) TWeakObjectPtr<UGameViewportClient> ScreenshotViewport;
    UPROPERTY(Transient) TWeakObjectPtr<APawn> OriginalPawn;
    UPROPERTY(Transient) TWeakObjectPtr<ACitySamplePlayerCameraManager> OriginalCameraManager;
    UPROPERTY(Transient) TObjectPtr<AActor> PreviewActor;
    UPROPERTY(Transient) TObjectPtr<ACameraActor> PreviewCamera;
    UPROPERTY(Transient) TWeakObjectPtr<APlayerController> PreviewController;
    UPROPERTY(Transient) TWeakObjectPtr<AActor> PreviousViewTarget;
};
