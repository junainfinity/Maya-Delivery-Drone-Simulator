#include "MAYAWorkerVideoCapture.h"
#include "FrameGrabber.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "ImagePixelData.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
struct FWorkerVideoFrame : IFramePayload
{
    int32 Index = 0;
    double WorldSeconds = 0, RealSeconds = 0;
    uint64 EngineFrame = 0;
};
}

FMAYAWorkerVideoCapture::FMAYAWorkerVideoCapture() = default;
FMAYAWorkerVideoCapture::~FMAYAWorkerVideoCapture() { Finish(TEXT("capture_destroyed")); }

bool FMAYAWorkerVideoCapture::Start(UWorld* World, const FString& RunDirectory)
{
    if (!IsInGameThread() || !World || !World->GetGameViewport() || Grabber || bFinished) return false;
    const IConsoleVariable* Latency = IConsoleManager::Get().FindConsoleVariable(TEXT("framegrabber.framelatency"));
    if (!Latency || Latency->GetInt() != 0) return false;
    UGameViewportClient* Client = World->GetGameViewport();
    const auto Widget = Client->GetGameViewportWidget();
    const auto Viewport = Widget.IsValid() ? StaticCastSharedPtr<FSceneViewport>(Widget->GetViewportInterface().Pin()) : nullptr;
    if (!Viewport.IsValid() || Viewport.Get() != Client->GetGameViewport()) return false;
    Directory = FPaths::Combine(RunDirectory, TEXT("pickup-video-frames"));
    if (IFileManager::Get().DirectoryExists(*Directory) || !IFileManager::Get().MakeDirectory(*Directory, true))
    { Directory.Empty(); return false; }
    Queue = &FModuleManager::LoadModuleChecked<IImageWriteQueueModule>(TEXT("ImageWriteQueue")).GetWriteQueue();
    Grabber = MakeUnique<FFrameGrabber>(Viewport.ToSharedRef(), FIntPoint(1280, 720), PF_B8G8R8A8, 3);
    Grabber->StartCapturingFrames();
    return true;
}

void FMAYAWorkerVideoCapture::Drain()
{
    if (!Grabber || !Queue) return;
    for (FCapturedFrameData& Frame : Grabber->GetCapturedFrames())
    {
        const FWorkerVideoFrame* Payload = Frame.GetPayload<FWorkerVideoFrame>();
        if (!Payload || Frame.ColorBuffer.Num() != Frame.BufferSize.X * Frame.BufferSize.Y)
        { ++Dropped; continue; }
        const FString Name = FString::Printf(TEXT("frame_%06d.jpg"), Payload->Index);
        auto Receipt = MakeShared<FJsonObject>();
        Receipt->SetStringField(TEXT("file"), Name);
        Receipt->SetNumberField(TEXT("index"), Payload->Index);
        Receipt->SetNumberField(TEXT("worldSecondsAtRequest"), Payload->WorldSeconds);
        Receipt->SetNumberField(TEXT("realSecondsAtRequest"), Payload->RealSeconds);
        Receipt->SetStringField(TEXT("engineFrameAtRequest"), LexToString(Payload->EngineFrame));
        Receipt->SetNumberField(TEXT("width"), Frame.BufferSize.X);
        Receipt->SetNumberField(TEXT("height"), Frame.BufferSize.Y);
        Receipt->SetBoolField(TEXT("writeCompleted"), false);
        Receipt->SetBoolField(TEXT("writeSucceeded"), false);
        auto Task = MakeUnique<FImageWriteTask>();
        Task->Filename = FPaths::Combine(Directory, Name);
        Task->Format = EImageFormat::JPEG; Task->CompressionQuality = 92; Task->bOverwriteFile = false;
        Task->PixelData = MakeUnique<TImagePixelData<FColor>>(Frame.BufferSize, TArray64<FColor>(MoveTemp(Frame.ColorBuffer)));
        FWrite Write; Write.Receipt = Receipt;
        Write.Future = Queue->Enqueue(MoveTemp(Task), false);
        Frames.Add(MakeShared<FJsonValueObject>(Receipt));
        if (Write.Future.IsValid()) Writes.Add(MoveTemp(Write)); else ++Dropped;
    }
    for (int32 I = Writes.Num() - 1; I >= 0; --I)
        if (Writes[I].Future.IsReady())
        {
            Writes[I].Receipt->SetBoolField(TEXT("writeCompleted"), true);
            Writes[I].Receipt->SetBoolField(TEXT("writeSucceeded"), Writes[I].Future.Get());
            Writes.RemoveAt(I);
        }
}

void FMAYAWorkerVideoCapture::Tick(double WorldSeconds)
{
    if (bFinished || !Grabber || !FMath::IsFinite(WorldSeconds)) return;
    Drain();
    if (Requested >= 600) { Finish(TEXT("bounded_600_frame_limit")); return; }
    if (LastWorld >= 0 && WorldSeconds - LastWorld < 1.0 / 30.0) return;
    if (Writes.Num() >= 8 || Queue->GetNumPendingTasks() >= 16) { ++Dropped; return; }
    auto Payload = MakeShared<FWorkerVideoFrame, ESPMode::ThreadSafe>();
    Payload->Index = ++Requested; Payload->WorldSeconds = WorldSeconds;
    Payload->RealSeconds = FPlatformTime::Seconds(); Payload->EngineFrame = GFrameCounter;
    Grabber->CaptureThisFrame(Payload); LastWorld = WorldSeconds;
}

void FMAYAWorkerVideoCapture::Finish(const FString& Reason)
{
    if (bFinished) return;
    bFinished = true;
    if (Grabber)
    {
        Grabber->StopCapturingFrames(); Grabber->Shutdown(); Drain();
        if (Queue) { auto Fence = Queue->CreateFence(); Fence.WaitFor(FTimespan::FromSeconds(5)); }
        Drain(); Grabber.Reset();
    }
    if (Directory.IsEmpty()) return;
    auto Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeWorkerVideoFrames.v1"));
    Report->SetStringField(TEXT("scope"), TEXT("Actual rendered game viewport; unmodified gameplay time; up to30 requests/gameplay-second; async JPEG92. No invented/interpolated poses."));
    Report->SetStringField(TEXT("finishedReason"), Reason);
    Report->SetNumberField(TEXT("observedFrameGrabberLatencyAtStart"), 0);
    Report->SetNumberField(TEXT("requestedFrames"), Requested);
    Report->SetNumberField(TEXT("droppedRequestsOrFrames"), Dropped);
    Report->SetNumberField(TEXT("pendingWrites"), Writes.Num());
    Report->SetArrayField(TEXT("frames"), Frames);
    FString Json; FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
    FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Directory, TEXT("capture.json")));
}
