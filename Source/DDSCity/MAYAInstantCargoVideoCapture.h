#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"

class FFrameGrabber;
class IImageWriteQueue;
class UWorld;
class FJsonValue;

// Explicit AutoTest presentation capture. Does not change simulation time or gameplay.
class FMAYAInstantCargoVideoCapture
{
public:
    FMAYAInstantCargoVideoCapture();
    ~FMAYAInstantCargoVideoCapture();
    bool Start(UWorld* World, const FString& RunDirectory, const FString& ClipLabel, const FString& MetadataJson);
    void Tick(double WorldSeconds, const FString& FrameEvidenceJson);
    void Finish(const FString& Reason);
    const FString& GetCaptureDirectory() const { return Directory; }
    int32 GetRequestedFrames() const { return Requested; }
    bool IsFinished() const { return bFinished; }
private:
    void Drain();
    TUniquePtr<FFrameGrabber> Grabber;
    IImageWriteQueue* Queue = nullptr;
    struct FWrite { TFuture<bool> Future; TSharedPtr<class FJsonObject> Receipt; };
    TArray<FWrite> Writes;
    TArray<TSharedPtr<FJsonValue>> Frames;
    FString Directory, ClipMetadataJson;
    int32 Requested = 0, Dropped = 0;
    double LastWorld = -1;
    bool bFinished = false;
};
