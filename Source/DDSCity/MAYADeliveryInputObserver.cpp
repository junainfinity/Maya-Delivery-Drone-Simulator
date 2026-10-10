#include "MAYADeliveryInputObserver.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryParcel.h"
#include "MAYANativeDemoPawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MAYAInputObservation
{
    const TCHAR* Schema = TEXT("MayaRobotics.V01.InputObservation.v1");
    TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
    {
        return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
    }
    TSharedRef<FJsonObject> Pose(const FTransform& T)
    {
        auto O = MakeShared<FJsonObject>();
        O->SetArrayField(TEXT("positionCm"), Vector(T.GetLocation()));
        const FQuat Q = T.GetRotation();
        O->SetArrayField(TEXT("quaternionXYZW"), {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
            MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W)});
        O->SetArrayField(TEXT("scale"), Vector(T.GetScale3D()));
        return O;
    }
    const TCHAR* Phase(EMAYADeliveryPhase Value)
    {
        switch (Value)
        {
        case EMAYADeliveryPhase::Available: return TEXT("Available");
        case EMAYADeliveryPhase::Accepted: return TEXT("Accepted");
        case EMAYADeliveryPhase::Loading: return TEXT("Loading");
        case EMAYADeliveryPhase::Carrying: return TEXT("Carrying");
        case EMAYADeliveryPhase::Unloading: return TEXT("Unloading");
        case EMAYADeliveryPhase::Succeeded: return TEXT("Succeeded");
        case EMAYADeliveryPhase::Failed: return TEXT("Failed");
        }
        return TEXT("Unknown");
    }
}

UMAYADeliveryInputObserver::UMAYADeliveryInputObserver()
{
    PrimaryComponentTick.bCanEverTick = !UE_BUILD_SHIPPING;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    // A zero engine interval allows paused ticks; monotonic time below limits
    // full observations to 2 Hz without accumulating catch-up samples.
    PrimaryComponentTick.TickInterval = 0;
    for (const FKey& Key : {EKeys::W, EKeys::S, EKeys::A, EKeys::D, EKeys::SpaceBar, EKeys::LeftShift,
        EKeys::J, EKeys::E, EKeys::R, EKeys::C, EKeys::N, EKeys::P, EKeys::Enter, EKeys::Home, EKeys::RightMouseButton})
    {
        FObservedKey Entry; Entry.Key = Key; Keys.Add(Entry);
    }
}

bool UMAYADeliveryInputObserver::Initialize(APlayerController* InController)
{
#if UE_BUILD_SHIPPING
    return false;
#else
    if (TraceFile || !IsValid(InController) || !InController->IsLocalController()
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))) return false;
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest")))
    {
        UE_LOG(LogTemp, Error, TEXT("MAYA input observation refuses AutoTest; use an interactive launch."));
        return false;
    }
    FString Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), Output) || Output.IsEmpty())
        Output = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MayaV01InputObservation"));
    Output = FPaths::ConvertRelativePathToFull(Output);
    if (!IFileManager::Get().MakeDirectory(*Output, true)) return false;
    TracePath = FPaths::Combine(Output, TEXT("input-observations.jsonl"));
    TraceFile = IFileManager::Get().CreateFileWriter(*TracePath, FILEWRITE_NoReplaceExisting | FILEWRITE_AllowRead);
    if (!TraceFile)
    {
        UE_LOG(LogTemp, Error, TEXT("MAYA input observation could not create a new trace; existing evidence is never replaced: %s"), *TracePath);
        return false;
    }
    ObservedController = InController;
    StartedMonotonicS = FPlatformTime::Seconds();
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("record"), TEXT("start"));
    O->SetStringField(TEXT("startedUtc"), FDateTime::UtcNow().ToIso8601());
    O->SetStringField(TEXT("scope"), TEXT("Actual controller callbacks, UPlayerInput states and gameplay observations; no input commands, focus assertion or acceptance result"));
    O->SetNumberField(TEXT("sampleMaxHz"), 2);
    O->SetStringField(TEXT("callbackEventPolicy"), TEXT("Immediate allowlisted pressed/released/doubleclick and delivery-action rows; repeat/axis/other counters remain in periodic samples"));
    O->SetNumberField(TEXT("maxWallSeconds"), MaxWallSeconds);
    O->SetNumberField(TEXT("maxFileBytes"), MaxFileBytes);
    TArray<TSharedPtr<FJsonValue>> Names;
    for (const auto& Entry : Keys) Names.Add(MakeShared<FJsonValueString>(Entry.Key.GetFName().ToString()));
    O->SetArrayField(TEXT("allowedKeys"), Names);
    if (!WriteRecord(O)) { Stop(TEXT("write_error")); return false; }
    UE_LOG(LogTemp, Display, TEXT("MAYA input observation started; <=2 Hz full samples plus compact callback events, 900 s, 64 MiB: %s"), *TracePath);
    return true;
#endif
}

UMAYADeliveryInputObserver::FObservedKey* UMAYADeliveryInputObserver::FindKey(const FKey& Key)
{
    for (auto& Entry : Keys) if (Entry.Key == Key) return &Entry;
    return nullptr;
}

bool UMAYADeliveryInputObserver::WithinBounds()
{
    if (!TraceFile) return false;
    if (FPlatformTime::Seconds() - StartedMonotonicS >= MaxWallSeconds) { Stop(TEXT("time_limit")); return false; }
    if (TraceFile->IsError()) { Stop(TEXT("write_error")); return false; }
    if (TraceFile->Tell() >= MaxFileBytes - 4096) { Stop(TEXT("file_limit")); return false; }
    return true;
}

void UMAYADeliveryInputObserver::RecordInputCallback(const FInputKeyParams& Params, bool bControllerHandled)
{
    if (!WithinBounds()) return;
    FObservedKey* Entry = FindKey(Params.Key);
    if (!Entry) return; // never capture text or any key outside the game allowlist
    switch (Params.Event)
    {
    case IE_Pressed: ++Entry->Pressed; break;
    case IE_Released: ++Entry->Released; break;
    case IE_Repeat: ++Entry->Repeated; break;
    case IE_DoubleClick: ++Entry->DoubleClick; break;
    case IE_Axis: ++Entry->Axis; break;
    default: ++Entry->Other; break;
    }
    if (bControllerHandled) ++Entry->ControllerHandled;
    Entry->LastCallbackElapsedS = FPlatformTime::Seconds() - StartedMonotonicS;
    if (Params.Event == IE_Pressed || Params.Event == IE_Released || Params.Event == IE_DoubleClick)
        WriteCallbackRecord(*Entry, TEXT("input_callback"), Params.Event, bControllerHandled);
}

void UMAYADeliveryInputObserver::RecordActionCallback(const FKey& Key, bool bActionAccepted)
{
    if (!WithinBounds()) return;
    if (FObservedKey* Entry = FindKey(Key))
    {
        ++Entry->ActionAttempts;
        if (bActionAccepted) ++Entry->ActionAccepted;
        Entry->LastActionElapsedS = FPlatformTime::Seconds() - StartedMonotonicS;
        WriteCallbackRecord(*Entry, TEXT("delivery_action_callback"), IE_Pressed, bActionAccepted);
    }
}

void UMAYADeliveryInputObserver::WriteCallbackRecord(const FObservedKey& Entry, const TCHAR* Kind, EInputEvent Event, bool bAccepted)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("record"), Kind);
    O->SetNumberField(TEXT("callbackEventIndex"), CallbackEventCount);
    O->SetStringField(TEXT("key"), Entry.Key.GetFName().ToString());
    if (FCString::Strcmp(Kind, TEXT("input_callback")) == 0)
    {
        O->SetStringField(TEXT("event"), Event == IE_Pressed ? TEXT("pressed") : Event == IE_Released ? TEXT("released") : TEXT("doubleclick"));
        O->SetBoolField(TEXT("controllerHandled"), bAccepted);
    }
    else O->SetBoolField(TEXT("actionAccepted"), bAccepted);
    O->SetNumberField(TEXT("pressedCallbacks"), Entry.Pressed);
    O->SetNumberField(TEXT("releasedCallbacks"), Entry.Released);
    O->SetNumberField(TEXT("deliveryActionAttempts"), Entry.ActionAttempts);
    O->SetNumberField(TEXT("deliveryActionAccepted"), Entry.ActionAccepted);
    if (GetWorld())
    {
        O->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
        O->SetBoolField(TEXT("worldPaused"), GetWorld()->IsPaused());
    }
    APlayerController* Controller = ObservedController.Get();
    const FKeyState* State = Controller && Controller->PlayerInput ? Controller->PlayerInput->GetKeyState(Entry.Key) : nullptr;
    O->SetBoolField(TEXT("keyStateKnown"), State != nullptr);
    if (State)
    {
        O->SetBoolField(TEXT("keyDown"), Controller->PlayerInput->IsPressed(Entry.Key));
        O->SetArrayField(TEXT("rawValue"), MAYAInputObservation::Vector(State->RawValue));
        O->SetArrayField(TEXT("processedValue"), MAYAInputObservation::Vector(State->Value));
    }
    if (WriteRecord(O)) ++CallbackEventCount;
    else Stop(TraceFile && TraceFile->IsError() ? TEXT("write_error") : TEXT("file_limit"));
}

bool UMAYADeliveryInputObserver::WriteRecord(const TSharedRef<FJsonObject>& Record, bool bTerminal)
{
    if (!TraceFile || TraceFile->IsError()) return false;
    Record->SetStringField(TEXT("schema"), MAYAInputObservation::Schema);
    Record->SetStringField(TEXT("gameplayAcceptance"), TEXT("not_assessed"));
    Record->SetNumberField(TEXT("elapsedMonotonicSeconds"), FPlatformTime::Seconds() - StartedMonotonicS);
    FString Line;
    if (!FJsonSerializer::Serialize(Record, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line))) return false;
    Line += TEXT("\n");
    FTCHARToUTF8 Utf8(*Line);
    const int64 Limit = bTerminal ? MaxFileBytes : MaxFileBytes - 4096;
    if (TraceFile->Tell() + Utf8.Length() > Limit) return false;
    TraceFile->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
    TraceFile->Flush();
    return !TraceFile->IsError();
}

void UMAYADeliveryInputObserver::Sample()
{
    using namespace MAYAInputObservation;
    APlayerController* Controller = ObservedController.Get();
    if (!IsValid(Controller) || !GetWorld()) { Stop(TEXT("controller_or_world_unavailable")); return; }
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("record"), TEXT("sample"));
    O->SetNumberField(TEXT("sampleIndex"), SampleCount);
    O->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    O->SetBoolField(TEXT("worldPaused"), GetWorld()->IsPaused());
    O->SetBoolField(TEXT("playerInputAvailable"), Controller->PlayerInput != nullptr);
    auto Input = MakeShared<FJsonObject>();
    for (const auto& Entry : Keys)
    {
        auto Key = MakeShared<FJsonObject>();
        const FKeyState* State = Controller->PlayerInput ? Controller->PlayerInput->GetKeyState(Entry.Key) : nullptr;
        Key->SetBoolField(TEXT("stateKnown"), State != nullptr);
        if (State)
        {
            Key->SetBoolField(TEXT("down"), Controller->PlayerInput->IsPressed(Entry.Key));
            Key->SetArrayField(TEXT("rawValue"), Vector(State->RawValue));
            Key->SetArrayField(TEXT("processedValue"), Vector(State->Value));
            Key->SetNumberField(TEXT("lastUpDownTransitionEngineSeconds"), State->LastUpDownTransitionTime);
        }
        Key->SetNumberField(TEXT("pressedCallbacks"), Entry.Pressed);
        Key->SetNumberField(TEXT("releasedCallbacks"), Entry.Released);
        Key->SetNumberField(TEXT("repeatCallbacks"), Entry.Repeated);
        Key->SetNumberField(TEXT("doubleClickCallbacks"), Entry.DoubleClick);
        Key->SetNumberField(TEXT("axisCallbacks"), Entry.Axis);
        Key->SetNumberField(TEXT("otherCallbacks"), Entry.Other);
        Key->SetNumberField(TEXT("controllerHandledCallbacks"), Entry.ControllerHandled);
        Key->SetNumberField(TEXT("lastCallbackElapsedSeconds"), Entry.LastCallbackElapsedS);
        Key->SetNumberField(TEXT("deliveryActionAttempts"), Entry.ActionAttempts);
        Key->SetNumberField(TEXT("deliveryActionAccepted"), Entry.ActionAccepted);
        Key->SetNumberField(TEXT("lastDeliveryActionElapsedSeconds"), Entry.LastActionElapsedS);
        Input->SetObjectField(Entry.Key.GetFName().ToString(), Key);
    }
    O->SetObjectField(TEXT("input"), Input);
    O->SetBoolField(TEXT("cameraAvailable"), IsValid(Controller->PlayerCameraManager));
    if (Controller->PlayerCameraManager)
    {
        auto Camera = MakeShared<FJsonObject>();
        Camera->SetArrayField(TEXT("positionCm"), Vector(Controller->PlayerCameraManager->GetCameraLocation()));
        const FRotator Rotation = Controller->PlayerCameraManager->GetCameraRotation();
        Camera->SetArrayField(TEXT("rotationPitchYawRollDegrees"), {MakeShared<FJsonValueNumber>(Rotation.Pitch),
            MakeShared<FJsonValueNumber>(Rotation.Yaw), MakeShared<FJsonValueNumber>(Rotation.Roll)});
        Camera->SetNumberField(TEXT("fovDegrees"), Controller->PlayerCameraManager->GetFOVAngle());
        O->SetObjectField(TEXT("camera"), Camera);
    }
    AMAYANativeDemoPawn* Robot = Cast<AMAYANativeDemoPawn>(Controller->GetPawn());
    O->SetBoolField(TEXT("nativePawnAvailable"), IsValid(Robot));
    if (Robot)
    {
        O->SetStringField(TEXT("pawnPath"), Robot->GetPathName());
        const FString NativeJson = Robot->ReadTelemetryJson();
        TSharedPtr<FJsonObject> Telemetry;
        const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(NativeJson), Telemetry) && Telemetry.IsValid();
        O->SetBoolField(TEXT("nativeTelemetryJsonParsed"), bParsed);
        if (bParsed) O->SetObjectField(TEXT("nativeTelemetry"), Telemetry);
        else O->SetStringField(TEXT("nativeTelemetryRaw"), NativeJson);
        FMAYADeliveryRobotObservation R;
        const bool bRead = Robot->ReadDeliveryState(R);
        auto Native = MakeShared<FJsonObject>();
        Native->SetBoolField(TEXT("readReturned"), bRead);
        Native->SetBoolField(TEXT("known"), R.bKnown); Native->SetBoolField(TEXT("ready"), R.bReady);
        Native->SetBoolField(TEXT("sixWheelSupported"), R.bGrounded); Native->SetBoolField(TEXT("upright"), R.bUpright);
        Native->SetBoolField(TEXT("doorsOpen"), R.bDoorsOpen); Native->SetBoolField(TEXT("doorsClosedLatched"), R.bDoorsClosedLatched);
        Native->SetBoolField(TEXT("boostDriveActive"), R.bBoostDriveActive);
        Native->SetNumberField(TEXT("supportMask"), R.SupportMask); Native->SetNumberField(TEXT("resetGeneration"), R.ResetGeneration);
        Native->SetNumberField(TEXT("observationWorldSeconds"), R.ObservationWorldSeconds);
        Native->SetNumberField(TEXT("opticalForwardCmps"), R.OpticalForwardCmps);
        Native->SetObjectField(TEXT("chassisPose"), Pose(R.ChassisPose));
        Native->SetArrayField(TEXT("velocityCmps"), Vector(R.VelocityCmps));
        Native->SetArrayField(TEXT("angularVelocityRadps"), Vector(R.AngularVelocityRadps));
        Native->SetArrayField(TEXT("doorDegrees"), {MakeShared<FJsonValueNumber>(R.DoorAngleDegrees[0]), MakeShared<FJsonValueNumber>(R.DoorAngleDegrees[1])});
        Native->SetArrayField(TEXT("doorRadps"), {MakeShared<FJsonValueNumber>(R.DoorOmegaRadps[0]), MakeShared<FJsonValueNumber>(R.DoorOmegaRadps[1])});
        Native->SetStringField(TEXT("fault"), R.Fault);
        O->SetObjectField(TEXT("nativeObservation"), Native);
    }
    AMAYADeliveryDirector* Director = nullptr;
    for (TActorIterator<AMAYADeliveryDirector> It(GetWorld()); It; ++It)
        if (It->GetRobot() == Robot && Robot) { Director = *It; break; }
    O->SetBoolField(TEXT("deliveryAvailable"), Director != nullptr);
    if (Director)
    {
        const FMAYADeliverySnapshot& S = Director->Snapshot();
        auto Delivery = MakeShared<FJsonObject>();
        Delivery->SetStringField(TEXT("phase"), Phase(S.Phase));
        Delivery->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId);
        Delivery->SetStringField(TEXT("profileId"), S.Job.ProfileId);
        Delivery->SetStringField(TEXT("title"), S.Job.Title);
        Delivery->SetStringField(TEXT("pickupStationId"), S.Job.PickupStationId);
        Delivery->SetStringField(TEXT("dropoffStationId"), S.Job.DropoffStationId);
        Delivery->SetNumberField(TEXT("payloadKg"), S.Job.PayloadKg);
        Delivery->SetNumberField(TEXT("manifestParcelCount"), S.Job.ParcelCount);
        Delivery->SetNumberField(TEXT("actualParcelActorCount"), Director->GetParcelCount());
        Delivery->SetNumberField(TEXT("actualSecuredParcelCount"), Director->GetSecuredParcelCount());
        Delivery->SetNumberField(TEXT("actualSecuredCargoKg"), Director->GetSecuredCargoKg());
        Delivery->SetBoolField(TEXT("rulesSecuredParcelCountKnown"), S.bSecuredParcelCountKnown);
        Delivery->SetNumberField(TEXT("rulesSecuredParcelCount"), S.SecuredParcelCount);
        TArray<TSharedPtr<FJsonValue>> Cargo;
        for (int32 Slot = 0; Slot < 2; ++Slot)
        {
            AMAYADeliveryParcel* Parcel = Director->GetParcel(Slot);
            auto Item = MakeShared<FJsonObject>();
            Item->SetNumberField(TEXT("slot"), Slot);
            Item->SetBoolField(TEXT("actorPresent"), IsValid(Parcel));
            if (IsValid(Parcel))
            {
                Item->SetStringField(TEXT("actorPath"), Parcel->GetPathName());
                Item->SetStringField(TEXT("parcelId"), Parcel->GetParcelId());
                Item->SetNumberField(TEXT("observedDeliverySlot"), Parcel->GetDeliverySlot());
                Item->SetBoolField(TEXT("dormantForRemoval"), Parcel->IsDormantForDeliveryRemoval());
                double Kg = 0;
                const bool bMassKnown = Parcel->ObserveMass(Kg);
                Item->SetBoolField(TEXT("massKnown"), bMassKnown);
                if (bMassKnown) Item->SetNumberField(TEXT("actualMassKg"), Kg);
                FTransform NativePose;
                const bool bPoseKnown = Parcel->ObserveNativePose(NativePose);
                Item->SetBoolField(TEXT("nativePoseKnown"), bPoseKnown);
                if (bPoseKnown) Item->SetObjectField(TEXT("nativePose"), Pose(NativePose));
            }
            Cargo.Add(MakeShared<FJsonValueObject>(Item));
        }
        Delivery->SetArrayField(TEXT("actualCargoSlots"), Cargo);
        const auto& Applied = S.LastImpact;
        auto Impact = MakeShared<FJsonObject>();
        Impact->SetNumberField(TEXT("episodeGeneration"), Applied.EpisodeGeneration);
        Impact->SetStringField(TEXT("eventKind"), Applied.Event.Kind);
        Impact->SetNumberField(TEXT("eventGeneration"), Applied.Event.Generation);
        Impact->SetNumberField(TEXT("eventWorldSeconds"), Applied.Event.WorldSeconds);
        Impact->SetNumberField(TEXT("impulseKgMps"), Applied.Event.ImpulseKgMps);
        Impact->SetNumberField(TEXT("dropCm"), Applied.Event.DropCm);
        Impact->SetNumberField(TEXT("preContactDownCmps"), Applied.Event.PreContactDownCmps);
        Impact->SetNumberField(TEXT("appliedConditionPoints"), Applied.AppliedConditionPoints);
        Impact->SetNumberField(TEXT("episodeConditionPoints"), Applied.EpisodeConditionPoints);
        Impact->SetNumberField(TEXT("conditionBefore"), Applied.ConditionBefore);
        Impact->SetNumberField(TEXT("conditionAfter"), Applied.ConditionAfter);
        Delivery->SetObjectField(TEXT("lastAppliedImpact"), Impact);
        Delivery->SetNumberField(TEXT("deadlineSeconds"), S.Job.DeadlineSeconds);
        Delivery->SetBoolField(TEXT("fragile"), S.Job.bFragile);
        Delivery->SetNumberField(TEXT("batteryPercent"), S.BatteryPercent);
        Delivery->SetNumberField(TEXT("cargoConditionPercent"), S.CargoConditionPercent);
        Delivery->SetNumberField(TEXT("elapsedSeconds"), S.ElapsedSeconds);
        Delivery->SetNumberField(TEXT("travelCm"), S.TravelCm);
        Delivery->SetNumberField(TEXT("recoveries"), S.Recoveries);
        Delivery->SetNumberField(TEXT("totalCredits"), S.TotalCredits);
        Delivery->SetNumberField(TEXT("completedJobs"), S.CompletedJobs);
        Delivery->SetBoolField(TEXT("charging"), S.bCharging);
        Delivery->SetStringField(TEXT("nearbyChargingStation"), Director->GetChargingStationName());
        Delivery->SetBoolField(TEXT("directorPaused"), Director->IsPaused());
        Delivery->SetStringField(TEXT("status"), S.Status);
        Delivery->SetStringField(TEXT("runtimeStatus"), Director->GetRuntimeStatus());
        auto Service = MakeShared<FJsonObject>();
        Service->SetStringField(TEXT("jobInstanceId"), S.Service.JobInstanceId);
        Service->SetNumberField(TEXT("generation"), S.Service.Generation);
        Service->SetStringField(TEXT("stationId"), S.Service.StationId);
        Service->SetBoolField(TEXT("pickup"), S.Service.bPickup);
        Service->SetNumberField(TEXT("robotResetGeneration"), S.Service.RobotResetGeneration);
        Service->SetNumberField(TEXT("startedWorldSeconds"), S.Service.StartedWorldSeconds);
        Service->SetArrayField(TEXT("robotServicePositionCm"), Vector(S.Service.RobotServicePosition));
        Delivery->SetObjectField(TEXT("serviceToken"), Service);
        auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("jobInstanceId"), S.Result.JobInstanceId);
        Result->SetStringField(TEXT("title"), S.Result.Title); Result->SetStringField(TEXT("reason"), S.Result.Reason);
        Result->SetBoolField(TEXT("deliverySucceeded"), S.Result.bSuccess);
        Result->SetNumberField(TEXT("credits"), S.Result.Credits);
        Result->SetNumberField(TEXT("elapsedSeconds"), S.Result.ElapsedSeconds);
        Result->SetNumberField(TEXT("batteryPercent"), S.Result.BatteryPercent);
        Result->SetNumberField(TEXT("cargoConditionPercent"), S.Result.CargoConditionPercent);
        Result->SetNumberField(TEXT("recoveries"), S.Result.Recoveries);
        Delivery->SetObjectField(TEXT("missionResult"), Result);
        O->SetObjectField(TEXT("delivery"), Delivery);
    }
    if (WriteRecord(O)) ++SampleCount;
    else Stop(TraceFile && TraceFile->IsError() ? TEXT("write_error") : TEXT("file_limit"));
}

void UMAYADeliveryInputObserver::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!WithinBounds()) return;
    const double Now = FPlatformTime::Seconds();
    if (LastSampleMonotonicS < 0 || Now - LastSampleMonotonicS >= 0.5)
    {
        LastSampleMonotonicS = Now;
        Sample();
    }
}

void UMAYADeliveryInputObserver::Stop(const FString& Reason)
{
    if (!TraceFile) return;
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("record"), TEXT("end"));
    O->SetStringField(TEXT("reason"), Reason);
    O->SetNumberField(TEXT("sampleCount"), SampleCount);
    O->SetNumberField(TEXT("callbackEventCount"), CallbackEventCount);
    WriteRecord(O, true);
    TraceFile->Close();
    delete TraceFile;
    TraceFile = nullptr;
    UE_LOG(LogTemp, Display, TEXT("MAYA input observation stopped: %s; %llu samples; gameplay acceptance not assessed"), *Reason, SampleCount);
}

void UMAYADeliveryInputObserver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Stop(TEXT("end_play"));
    Super::EndPlay(EndPlayReason);
}

void UMAYADeliveryInputObserver::BeginDestroy()
{
    Stop(TEXT("component_destroyed"));
    Super::BeginDestroy();
}
