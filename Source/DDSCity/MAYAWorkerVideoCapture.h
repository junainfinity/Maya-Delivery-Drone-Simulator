#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"

class FFrameGrabber;
class IImageWriteQueue;
class UWorld;
class FJsonValue;

// Explicit AutoTest presentation capture. Does not change simulation time or gameplay.
class FMAYAWorkerVideoCapture
{
public:
    FMAYAWorkerVideoCapture();
    ~FMAYAWorkerVideoCapture();
    bool Start(UWorld* World, const FString& RunDirectory);
    void Tick(double WorldSeconds);
    void Finish(const FString& Reason);
private:
    void Drain();
    TUniquePtr<FFrameGrabber> Grabber;
    IImageWriteQueue* Queue = nullptr;
    struct FWrite { TFuture<bool> Future; TSharedPtr<class FJsonObject> Receipt; };
    TArray<FWrite> Writes;
    TArray<TSharedPtr<FJsonValue>> Frames;
    FString Directory;
    int32 Requested = 0, Dropped = 0;
    double LastWorld = -1;
    bool bFinished = false;
};
