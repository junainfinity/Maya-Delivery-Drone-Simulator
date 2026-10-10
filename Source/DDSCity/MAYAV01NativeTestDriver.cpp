#include "MAYAV01NativeTestDriver.h"
#include "MAYAPlayerNewShiftReloadTest.h"
#include "MAYANativeDemoPawn.h"
#include "MAYAPlayerDriveInput.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryPlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryParcel.h"
#include "MAYADeliveryChargeEvidence.h"
#include "MAYADeliveryEconomyEvidence.h"
#include "MAYADeliveryImpactEvidence.h"
#include "MAYAV01CargoNativeEvidence.h"
#include "MAYADeliveryVisualCapture.h"
#include "MAYAInstantCargoVideoCapture.h"
#include "MAYAV01DamageFixture.h"
#include "MAYAV01CurbTrial.h"
#include "MAYAV01TrafficEncounterTrial.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Chaos/Convex.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Containers/UnrealString.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"

namespace
{
constexpr double MayaV01TelemetryCadenceS = 0.5; // Full post-physics rows at 2 Hz, plus boundaries.
constexpr uint64 MayaV01TelemetryByteLimit = 64ull * 1024 * 1024;
constexpr int32 MayaV01TelemetryRecordLimit = 5000;
TArray<TSharedPtr<FJsonValue>> VJson(const FVector& V)
{
    return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
}
TSharedRef<FJsonObject> MayaV01RingObservationJson(const FMAYADeliveryRingObservation& R)
{
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("activeStationId"), R.ActiveStationId);
    J->SetBoolField(TEXT("inside"), R.bInside); J->SetBoolField(TEXT("eligible"), R.bEligible);
    J->SetBoolField(TEXT("visitConsumed"), R.bVisitConsumed);
    J->SetStringField(TEXT("entryGeneration"), LexToString(R.EntryGeneration));
    J->SetNumberField(TEXT("elapsedGameplaySeconds"), R.ElapsedGameplaySeconds);
    J->SetNumberField(TEXT("remainingGameplaySeconds"), R.RemainingGameplaySeconds);
    J->SetStringField(TEXT("requestGeneration"), LexToString(R.RequestGeneration));
    J->SetStringField(TEXT("automaticTriggerGeneration"), LexToString(R.AutomaticTriggerGeneration));
    J->SetStringField(TEXT("lastRequestOrigin"), R.LastRequestOrigin);
    J->SetBoolField(TEXT("lastRequestAccepted"), R.LastRequestAccepted);
    J->SetStringField(TEXT("lastRequestServiceGeneration"), LexToString(R.LastRequestServiceGeneration));
    J->SetNumberField(TEXT("lastRequestWorldSeconds"), R.LastRequestWorldSeconds);
    J->SetNumberField(TEXT("lastRequestElapsedGameplaySeconds"), R.LastRequestElapsedGameplaySeconds);
    J->SetStringField(TEXT("lastRequestEntryGeneration"), LexToString(R.LastRequestEntryGeneration));
    J->SetBoolField(TEXT("lastAutomaticRequestAccepted"), R.LastAutomaticRequestAccepted);
    J->SetStringField(TEXT("lastAutomaticServiceGeneration"), LexToString(R.LastAutomaticServiceGeneration));
    J->SetStringField(TEXT("lastAutomaticEntryGeneration"), LexToString(R.LastAutomaticEntryGeneration));
    J->SetNumberField(TEXT("lastAutomaticRequestWorldSeconds"), R.LastAutomaticRequestWorldSeconds);
    J->SetNumberField(TEXT("lastAutomaticElapsedGameplaySeconds"), R.LastAutomaticElapsedGameplaySeconds);
    J->SetStringField(TEXT("lastAutomaticStationId"), R.LastAutomaticStationId);
    return J;
}
FString SerializeMayaV01Json(const TSharedRef<FJsonObject>& O)
{
    FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
    FJsonSerializer::Serialize(O, Writer); return Text;
}
TSharedPtr<FJsonObject> MayaV01ReadJson(const FString& Text)
{
    TSharedPtr<FJsonObject> J;
    const auto Reader = TJsonReaderFactory<>::Create(Text);
    return FJsonSerializer::Deserialize(Reader, J) ? J : nullptr;
}
bool MayaV01ReadGeneration(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, uint64& Out)
{
    FString Value;
    return J.IsValid() && J->TryGetStringField(Key, Value) && LexTryParseString(Out, *Value);
}
TMap<FString, FString> MayaV01FailureFields(const FString& Text)
{
    TMap<FString, FString> Result; TArray<FString> Parts;
    Text.ParseIntoArray(Parts, TEXT(";"), true);
    for (const FString& Part : Parts)
    {
        FString Key, Value;
        if (Part.Split(TEXT("="), &Key, &Value))
        {
            Key.TrimStartAndEndInline(); Value.TrimStartAndEndInline();
            if (!Result.Contains(Key)) Result.Add(Key, Value);
        }
    }
    return Result;
}
bool MayaV01FailureNumber(const TMap<FString, FString>& Fields, const TCHAR* Key, double& Out)
{
    const FString* Value = Fields.Find(Key);
    return Value && LexTryParseString(Out, **Value) && FMath::IsFinite(Out);
}

}

AMAYAV01NativeTestDriver::AMAYAV01NativeTestDriver()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    PrimaryActorTick.bTickEvenWhenPaused = true; // Only the explicitly opted-in pause regression uses this.
}

void AMAYAV01NativeTestDriver::Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector)
{ Robot = InRobot; Director = InDirector; }

const TCHAR* AMAYAV01NativeTestDriver::PhaseName(EPhase Value)
{
    switch (Value)
    {
#define MAYAPHASE(Name) case EPhase::Name: return TEXT(#Name)
        MAYAPHASE(Settle); MAYAPHASE(Forward); MAYAPHASE(ForwardBrake); MAYAPHASE(Reverse); MAYAPHASE(ReverseBrake);
        MAYAPHASE(LeftTurn); MAYAPHASE(LeftBrake); MAYAPHASE(RightTurn); MAYAPHASE(RightBrake);
        MAYAPHASE(FullEmptyPosition); MAYAPHASE(FullEmptyAlign); MAYAPHASE(FullEmptyForward); MAYAPHASE(FullEmptyForwardBrake);
        MAYAPHASE(FullEmptyReverse); MAYAPHASE(FullEmptyReverseBrake); MAYAPHASE(FullEmptyLeftCenter); MAYAPHASE(FullEmptyLeftSettle); MAYAPHASE(FullEmptyLeft); MAYAPHASE(FullEmptyLeftBrake);
        MAYAPHASE(FullEmptyRightCenter); MAYAPHASE(FullEmptyRightSettle); MAYAPHASE(FullEmptyRight); MAYAPHASE(FullEmptyRightBrake);
        MAYAPHASE(FullLoadedPosition); MAYAPHASE(FullLoadedAlign); MAYAPHASE(FullLoadedForward); MAYAPHASE(FullLoadedForwardBrake);
        MAYAPHASE(FullLoadedReverse); MAYAPHASE(FullLoadedReverseBrake); MAYAPHASE(FullLoadedLeftCenter); MAYAPHASE(FullLoadedLeftSettle); MAYAPHASE(FullLoadedLeft); MAYAPHASE(FullLoadedLeftBrake);
        MAYAPHASE(FullLoadedRightCenter); MAYAPHASE(FullLoadedRightSettle); MAYAPHASE(FullLoadedRight); MAYAPHASE(FullLoadedRightBrake);
        MAYAPHASE(DoorOpen); MAYAPHASE(DoorDriveProbe); MAYAPHASE(DoorClose); MAYAPHASE(VisualOpen); MAYAPHASE(VisualClosed); MAYAPHASE(Recover); MAYAPHASE(RecoverSettle);
        MAYAPHASE(ChargeSettle); MAYAPHASE(Charge); MAYAPHASE(PaidChargeReturn); MAYAPHASE(PaidChargeWait); MAYAPHASE(PaidChargeDeparture); MAYAPHASE(PaidChargeStop); MAYAPHASE(Accept); MAYAPHASE(ToPickup); MAYAPHASE(Loading);
        MAYAPHASE(LoadedDrive); MAYAPHASE(LoadedBrake); MAYAPHASE(CarriedRecover); MAYAPHASE(CarriedRecoverSettle); MAYAPHASE(ToDropoff);
        MAYAPHASE(Unloading); MAYAPHASE(NextJob); MAYAPHASE(CashOpeningRetry); MAYAPHASE(CancelOpeningAwaitClosure); MAYAPHASE(CancelOpeningRetry);
        MAYAPHASE(RingAccept); MAYAPHASE(RingEarlyMovingInteraction); MAYAPHASE(RingExit); MAYAPHASE(RingOutsideSettle);
        MAYAPHASE(RingReenter); MAYAPHASE(RingInsideSettle); MAYAPHASE(RingAwaitAutomatic);
        MAYAPHASE(OperatingAccept); MAYAPHASE(OperatingDrain); MAYAPHASE(OperatingDepletedHold);
        MAYAPHASE(OperatingRecover); MAYAPHASE(OperatingRecoverSettle); MAYAPHASE(OperatingCharge);
        MAYAPHASE(OperatingUrgentAccept); MAYAPHASE(OperatingUrgentWait); MAYAPHASE(OperatingDeadlineHold);
        MAYAPHASE(OperatingRetryAccept); MAYAPHASE(DamageEmptyTrial); MAYAPHASE(DamageEmptyHard); MAYAPHASE(DamageEmptyReturn); MAYAPHASE(DamageLoadedSettle); MAYAPHASE(DamageLoadedSmall); MAYAPHASE(DamageLoadedHard); MAYAPHASE(CurbTrial); MAYAPHASE(TrafficEncounter); MAYAPHASE(ReverseSteering); MAYAPHASE(Complete); MAYAPHASE(Failed);
#undef MAYAPHASE
    }
    return TEXT("Unknown");
}

void AMAYAV01NativeTestDriver::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING
    SetActorTickEnabled(false); return;
#else
    if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))) { SetActorTickEnabled(false); return; }
#endif
    bReverseSteerTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01ReverseSteerTest"));
    bCurbTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01CurbTest"));
    bTrafficEncounterTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01TrafficEncounterTest"));
    bDamageTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageTest"));
    bDamageCalibration = FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageCalibration"));
    bFullInputTrials = FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"));
    bBoostTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01BoostTest"));
    bRingInteractionTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"));
    bVisualCaptureRequested = FParse::Param(FCommandLine::Get(), TEXT("MayaV01VisualCapture"));
    FString RequestedOutput;
    if (FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), RequestedOutput) && !RequestedOutput.IsEmpty())
        RunDirectory = FPaths::ConvertRelativePathToFull(RequestedOutput);
    else RunDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MAYA/V01"), FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")));
    IFileManager::Get().MakeDirectory(*RunDirectory, true);
    const FString TelemetryPath = FPaths::Combine(RunDirectory, TEXT("telemetry.jsonl"));
    if (!IFileManager::Get().FileExists(*TelemetryPath))
        TelemetryWriter.Reset(IFileManager::Get().CreateFileWriter(*TelemetryPath, FILEWRITE_AllowRead));
    if (!TelemetryWriter)
    {
        bTelemetryWriteFailed = true; TelemetryStatus = TEXT("file_open_failed_or_existing_trace");
        Stop(false, TEXT("Native shared-read telemetry writer could not create a fresh trace")); return;
    }
    RunStartWorldS = PhaseStartWorldS = GetWorld()->GetTimeSeconds();
    if (!Robot || !Director) { Stop(false, TEXT("Owned robot/director absent in actual game world")); return; }
    bCargoDiagnosticTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"));
    bCargoVideoRequested = FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoVideo"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ServiceVideo"));
    if (bCurbTest && (!FApp::CanEverRender() || bDamageTest || bDamageCalibration || bFullInputTrials
        || bBoostTest || bRingInteractionTest || bVisualCaptureRequested || bCargoDiagnosticTest || bCargoVideoRequested
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Original city curb diagnostic requires rendered exclusive AutoTest+CurbTest")); return; }
    if (bTrafficEncounterTest && (!bCurbTest || !FApp::CanEverRender()
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01UIScreenTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01NewShiftReloadTest"))))
    { Stop(false, TEXT("Real traffic encounter requires exclusive rendered AutoTest+CurbTest+TrafficEncounterTest")); return; }
    // Installed Mass PostPhysics completes before PostUpdateWork; FrameEnd starts later.
    // Only this explicit diagnostic moves; all read/refusal and native command guards remain.
    if (bTrafficEncounterTest) SetTickGroup(TG_PostUpdateWork);
    if (bDamageCalibration && !bDamageTest)
    { Stop(false, TEXT("DamageCalibration requires explicit AutoTest+DamageTest")); return; }
    if (bDamageTest && (bFullInputTrials || bBoostTest || bRingInteractionTest || bVisualCaptureRequested
        || bCargoDiagnosticTest || bCargoVideoRequested || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Physical damage diagnostic requires exclusive AutoTest+DamageTest; other native modes are excluded")); return; }
    if (bCargoVideoRequested && (!FApp::CanEverRender()
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01BoostTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Instant loading/unloading video requires rendered explicit AutoTest standard/cargo; mechanics/boost, operating, ring, observer and headless are excluded")); return; }
    if (bCargoDiagnosticTest && (FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Cargo diagnostic mode requires exclusive AutoTest opt-in; other optional modes were also requested")); return; }
    bCancelOpeningTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"));
    if (bCancelOpeningTest && (!bCargoDiagnosticTest || bFullInputTrials || bBoostTest || bRingInteractionTest
        || bVisualCaptureRequested || bCargoVideoRequested
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Opening cancellation regression requires exclusive AutoTest plus CargoTest and CancelOpeningTest")); return; }
    bAirLoweringTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01AirLoweringTest"));
    if (bAirLoweringTest && (!bCargoDiagnosticTest || !bCargoVideoRequested || !FApp::CanEverRender()
        || bCancelOpeningTest || bDamageTest || bDamageCalibration || bCurbTest || bFullInputTrials || bBoostTest
        || bRingInteractionTest || bVisualCaptureRequested
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Air lowering regression requires exclusive rendered AutoTest+CargoTest+CargoVideo+AirLoweringTest")); return; }
    bCashShiftTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01CashShiftTest"));
    if (bCashShiftTest && (!bCargoDiagnosticTest || !FApp::CanEverRender() || bCargoVideoRequested
        || bAirLoweringTest || bCancelOpeningTest || bDamageTest || bDamageCalibration || bCurbTest
        || bFullInputTrials || bBoostTest || bRingInteractionTest || bVisualCaptureRequested
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01UIScreenTest"))))
    { Stop(false, TEXT("CashShift requires exclusive rendered AutoTest+CargoTest+CashShiftTest")); return; }
    if (bCashShiftTest) CashShiftInstanceId = Director->Snapshot().ShiftInstanceId;
    bOperatingTest = FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"));
    if (bBoostTest && (!bFullInputTrials || bCargoDiagnosticTest || bOperatingTest || bRingInteractionTest
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Boost test requires explicit AutoTest plus FullInput and excludes cargo, operating, ring and observer modes")); return; }
    if (bRingInteractionTest && (bOperatingTest || bFullInputTrials || bCargoDiagnosticTest
        || bVisualCaptureRequested || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Ring interaction test requires exclusive AutoTest opt-in; other native modes are excluded")); return; }
    if (bOperatingTest)
    {
        if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
            || !Director->IsNativeOperatingBatteryFixtureApplied()
            || Director->GetNativeOperatingBatteryFixtureId() != TEXT("MayaRobotics.V01.Operating.InitialBattery.v1")
            || Director->GetNativeOperatingInitialBatteryPercent() < 0.5
            || Director->GetNativeOperatingInitialBatteryPercent() > 1.0)
        { Stop(false, TEXT("Operating regression requires exclusive native opt-in and the reviewed pristine 0.5-1 percent startup fixture")); return; }
        bCarriedRecoveryChecked = true; // Carried reconstruction is covered by the separate ordinary three-profile suite.
    }
    if (bVisualCaptureRequested && (bOperatingTest || bCargoDiagnosticTest || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    { Stop(false, TEXT("Visual capture requires rendered standard/full-input native mode, exclusive of cargo, operating and input-observer modes")); return; }
    float ReverseOptionalBattery = 0;
    if (bReverseSteerTest && (!FApp::CanEverRender() || bCurbTest || bTrafficEncounterTest
        || bDamageTest || bDamageCalibration || bFullInputTrials || bBoostTest || bRingInteractionTest
        || bVisualCaptureRequested || bCargoDiagnosticTest || bCargoVideoRequested || bCancelOpeningTest
        || bAirLoweringTest || bCashShiftTest || bOperatingTest
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01UIScreenTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01NewShiftReloadTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01PackageUIScreenSmoke"))
        || FParse::Value(FCommandLine::Get(), TEXT("MayaV01InitialBattery="), ReverseOptionalBattery)))
    { Stop(false, TEXT("Player reverse steering diagnostic requires exclusive rendered AutoTest+ReverseSteerTest with ordinary startup resources")); return; }
    AddTickPrerequisiteActor(Director);
    CaptureCollisionGeometry();
    WriteReport(false, false, TEXT("Native automation started; result pending"));
}

bool AMAYAV01NativeTestDriver::Drive(float Throttle, float Steer, bool bBrake)
{
    const bool TranslationTrial = Phase == EPhase::FullEmptyForward || Phase == EPhase::FullEmptyReverse
        || Phase == EPhase::FullLoadedForward || Phase == EPhase::FullLoadedReverse;
    const bool InhibitedProbe = Phase == EPhase::DoorDriveProbe || Phase == EPhase::DoorClose;
    bLastBoostRequested = bBoostTest && !bBrake && Throttle != 0 && (TranslationTrial || InhibitedProbe);
    bLastCommandAccepted = Robot && Robot->SubmitDriveCommand(Throttle, Steer, bBrake,
        EMAYADriveCommandOrigin::NativeAutomation, 0.25f, bLastBoostRequested);
    return bLastCommandAccepted;
}


TArray<TSharedPtr<FJsonValue>> AMAYAV01NativeTestDriver::ReadParcelInventoryJson() const
{
    TArray<TSharedPtr<FJsonValue>> Items;
    if (!Director) return Items;
    for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
    {
        auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("slot"), Slot);
        AMAYADeliveryParcel* Parcel = Director->GetParcel(Slot);
        J->SetBoolField(TEXT("actorValid"), IsValid(Parcel));
        if (IsValid(Parcel))
        {
            J->SetStringField(TEXT("parcelId"), Parcel->GetParcelId());
            J->SetStringField(TEXT("actor"), Parcel->GetPathName());
            J->SetBoolField(TEXT("dormantPendingRemoval"), Parcel->IsDormantForDeliveryRemoval());
            J->SetNumberField(TEXT("actualDeliverySlot"), Parcel->GetDeliverySlot());
            double Kg = 0; const bool MassKnown = Parcel->ObserveMass(Kg);
            J->SetBoolField(TEXT("nativeMassKnown"), MassKnown); J->SetNumberField(TEXT("nativeMassKg"), Kg);
            FTransform NativePose; const bool NativeKnown = Parcel->ObserveNativePose(NativePose);
            J->SetBoolField(TEXT("nativePoseKnown"), NativeKnown);
            if (NativeKnown)
            {
                const FQuat Q = NativePose.GetRotation();
                J->SetArrayField(TEXT("nativePositionCm"), VJson(NativePose.GetLocation()));
                J->SetArrayField(TEXT("nativeQuaternion"), {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
                    MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W)});
            }
            auto* Body = Parcel->GetParcelBody();
            J->SetBoolField(TEXT("simulating"), Body && Body->IsSimulatingPhysics());
            J->SetNumberField(TEXT("collisionEnabled"), Body ? int32(Body->GetCollisionEnabled()) : -1);
            if (Body) J->SetStringField(TEXT("body"), Body->GetPathName());
            FTransform Target;
            const bool TargetKnown = Robot && Robot->GetDeliveryCargoPlacementTarget(Body, Slot, Target);
            J->SetBoolField(TEXT("slotTargetKnown"), TargetKnown);
            if (TargetKnown) J->SetArrayField(TEXT("slotTargetCm"), VJson(Target.GetLocation()));
            if (bCashShiftTest && TargetKnown)
            {
                const FQuat Q=Target.GetRotation();
                J->SetArrayField(TEXT("slotTargetQuaternion"), {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y), MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W)});
                if (NativeKnown) { J->SetNumberField(TEXT("nativeSlotPositionErrorCm"), FVector::Dist(NativePose.GetLocation(),Target.GetLocation()));
                    J->SetNumberField(TEXT("nativeSlotAngularErrorDegrees"), FMath::RadiansToDegrees(NativePose.GetRotation().AngularDistance(Q))); }
            }
            auto* Mount = Robot ? Robot->GetDeliveryCargoMount(Slot) : nullptr;
            auto* Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
            J->SetBoolField(TEXT("actualSlotRestraintValid"), TargetKnown && Parcel->IsSecuredTo(Carrier, Target));
            J->SetStringField(TEXT("nativeCurrent"), MAYAV01CaptureCargoPhysicsJson(Parcel, Carrier, TEXT("inventory")));
            J->SetStringField(TEXT("tieDownInitialization"), Parcel->GetTieDownInitializationEvidenceJson());
        }
        Items.Add(MakeShared<FJsonValueObject>(J));
    }
    return Items;
}

FString AMAYAV01NativeTestDriver::ReadParcelIdentityPoseJson() const
{
    auto O = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Items;
    if (Director) for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
    {
        auto J = MakeShared<FJsonObject>(); auto* P = Director->GetParcel(Slot);
        J->SetNumberField(TEXT("slot"), Slot); J->SetBoolField(TEXT("valid"), IsValid(P));
        if (P)
        {
            J->SetStringField(TEXT("id"), P->GetParcelId());
            J->SetStringField(TEXT("actor"), P->GetPathName());
            J->SetBoolField(TEXT("dormant"), P->IsDormantForDeliveryRemoval());
            if (P->GetParcelBody()) J->SetStringField(TEXT("pose"), P->GetParcelBody()->GetComponentTransform().ToString());
            double Kg = 0; J->SetBoolField(TEXT("massKnown"), P->ObserveMass(Kg)); J->SetNumberField(TEXT("kg"), Kg);
        }
        Items.Add(MakeShared<FJsonValueObject>(J));
    }
    O->SetArrayField(TEXT("items"), Items); return SerializeMayaV01Json(O);
}

bool AMAYAV01NativeTestDriver::VerifyLoadedInventory(const FMAYADeliveryRobotObservation& O, bool bCapture)
{
    InventoryFailure.Empty(); const auto S = Director->Snapshot(); const int32 Count = Director->GetParcelCount();
    if (S.Job.ParcelCount < 1 || S.Job.ParcelCount > 2 || Count != S.Job.ParcelCount
        || Director->GetSecuredParcelCount() != Count || S.JobInstanceId.IsEmpty())
    { InventoryFailure = TEXT("Actual parcel count/secured count does not match the one/two-parcel job"); return false; }
    TSet<FString> Ids; TSet<UPrimitiveComponent*> Bodies; double SumKg = 0;
    TArray<FTransform> NewRelatives; TArray<FString> NewIds;
    TArray<TWeakObjectPtr<AMAYADeliveryParcel>> NewActors; TArray<TWeakObjectPtr<UPrimitiveComponent>> NewBodies;
    if (!bCapture && (LoadedParcelIds.Num() != Count || LoadedParcelRelatives.Num() != Count
        || LoadedParcelActors.Num() != Count || LoadedParcelBodies.Num() != Count))
    { InventoryFailure = TEXT("Captured individual cargo custody is absent"); return false; }
    for (int32 Slot = 0; Slot < Count; ++Slot)
    {
        auto* P = Director->GetParcel(Slot); auto* B = P ? P->GetParcelBody() : nullptr;
        const FString ExpectedId = Slot == 0 ? S.JobInstanceId : S.JobInstanceId + TEXT("/parcel_2");
        FTransform NativePose, Target; double Kg = 0;
        auto* Mount = Robot->GetDeliveryCargoMount(Slot);
        auto* Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
        if (!IsValid(P) || !B || P->GetParcelId() != ExpectedId || Ids.Contains(ExpectedId) || Bodies.Contains(B)
            || P->IsDormantForDeliveryRemoval() || P->GetDeliverySlot() != Slot || !B->IsSimulatingPhysics()
            || B->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
            || !P->ObserveMass(Kg) || !FMath::IsFinite(Kg) || Kg <= 0
            || FMath::Abs(Kg - S.Job.PayloadKg / Count) > 0.05 || !P->ObserveNativePose(NativePose)
            || !Robot->GetDeliveryCargoPlacementTarget(B, Slot, Target)
            || FVector::Dist(NativePose.GetLocation(), Target.GetLocation()) > 2.0
            || NativePose.GetRotation().AngularDistance(Target.GetRotation()) > FMath::DegreesToRadians(3.0)
            || !P->IsSecuredTo(Carrier, Target)
            || (!bCapture && (LoadedParcelIds[Slot] != ExpectedId || LoadedParcelActors[Slot].Get() != P
                || LoadedParcelBodies[Slot].Get() != B
                || !P->IsSecuredTo(Carrier, LoadedParcelRelatives[Slot] * O.ChassisPose))))
        { InventoryFailure = FString::Printf(TEXT("Individual parcel slot %d lost native identity, mass, pose, collision or independent restraint"), Slot); return false; }
        SumKg += Kg; Ids.Add(ExpectedId); Bodies.Add(B);
        NewIds.Add(ExpectedId); NewRelatives.Add(NativePose.GetRelativeTransform(O.ChassisPose)); NewActors.Add(P); NewBodies.Add(B);
    }
    if (FMath::Abs(SumKg - S.Job.PayloadKg) > 0.05)
    { InventoryFailure = TEXT("Summed actual native parcel mass does not match the job"); return false; }
    if (bCapture)
    {
        LoadedParcelIds = MoveTemp(NewIds); LoadedParcelRelatives = MoveTemp(NewRelatives);
        LoadedParcelActors = MoveTemp(NewActors); LoadedParcelBodies = MoveTemp(NewBodies);
        LoadedParcelRelativeToChassis = LoadedParcelRelatives[0];
    }
    return true;
}

bool AMAYAV01NativeTestDriver::CapturePausedInventory()
{
    PausedParcelIds.Reset(); PausedParcelPoses.Reset(); PausedParcelMasses.Reset();
    PausedParcelDormant.Reset(); PausedParcelSimulating.Reset(); PausedParcelCollision.Reset();
    for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
    {
        auto* P = Director->GetParcel(Slot); double Kg = 0;
        if (!P || !P->GetParcelBody() || !P->ObserveMass(Kg)) return false;
        auto* B = P->GetParcelBody(); PausedParcelIds.Add(P->GetParcelId());
        PausedParcelPoses.Add(B->GetComponentTransform()); PausedParcelMasses.Add(Kg);
        PausedParcelDormant.Add(P->IsDormantForDeliveryRemoval());
        PausedParcelSimulating.Add(B->IsSimulatingPhysics()); PausedParcelCollision.Add(int32(B->GetCollisionEnabled()));
    }
    return true; // Zero actors before instant insertion is a real, valid empty inventory.
}

bool AMAYAV01NativeTestDriver::PausedInventoryUnchanged() const
{
    if (Director->GetParcelCount() != PausedParcelIds.Num()) return false;
    for (int32 Slot = 0; Slot < PausedParcelIds.Num(); ++Slot)
    {
        auto* P = Director->GetParcel(Slot); auto* B = P ? P->GetParcelBody() : nullptr; double Kg = 0;
        if (!P || !B || P->GetParcelId() != PausedParcelIds[Slot] || !P->ObserveMass(Kg)
            || FMath::Abs(Kg - PausedParcelMasses[Slot]) > 1.e-5
            || FVector::Dist(B->GetComponentLocation(), PausedParcelPoses[Slot].GetLocation()) > 0.1
            || B->GetComponentQuat().AngularDistance(PausedParcelPoses[Slot].GetRotation()) > FMath::DegreesToRadians(0.1)
            || P->IsDormantForDeliveryRemoval() != PausedParcelDormant[Slot]
            || B->IsSimulatingPhysics() != PausedParcelSimulating[Slot]
            || int32(B->GetCollisionEnabled()) != PausedParcelCollision[Slot]) return false;
    }
    return true;
}

void AMAYAV01NativeTestDriver::Enter(EPhase Next, const FMAYADeliveryRobotObservation& O)
{
    Record(O, true, TEXT("phase_exit"));
    if (Phase == EPhase::Loading || Phase == EPhase::Unloading) RestoreInstantServiceObservationView();
    auto Receipt = MakeShared<FJsonObject>();
    Receipt->SetStringField(TEXT("phase"), PhaseName(Phase)); Receipt->SetBoolField(TEXT("pass"), true);
    if (Phase == EPhase::Loading && Next == EPhase::CashOpeningRetry)
    {
        Receipt->SetBoolField(TEXT("pass"), false);
        Receipt->SetStringField(TEXT("outcome"), TEXT("natural empty Opening cancelled safely; original failure retained; one normal retry pending"));
        Receipt->SetStringField(TEXT("originalFailureDetail"), CashNaturalOriginalFailure);
    }
    Receipt->SetNumberField(TEXT("startWorldSeconds"), PhaseStartWorldS);
    Receipt->SetNumberField(TEXT("endWorldSeconds"), O.ObservationWorldSeconds);
    Receipt->SetNumberField(TEXT("distanceCm"), FVector::Distance(PhaseStartPose.GetLocation(), O.ChassisPose.GetLocation()));
    Receipt->SetNumberField(TEXT("yawDeltaDegrees"), FMath::FindDeltaAngleDegrees(PhaseStartPose.Rotator().Yaw, O.ChassisPose.Rotator().Yaw));
    Receipt->SetNumberField(TEXT("speedCmps"), O.VelocityCmps.Size());
    Receipt->SetNumberField(TEXT("initialLinearSpeedCmps"), BrakeStartSpeed);
    Receipt->SetNumberField(TEXT("initialAngularSpeedRadps"), PhaseStartAngularSpeedRadps);
    if (Phase == EPhase::FullEmptyPosition || Phase == EPhase::FullLoadedPosition)
    {
        Receipt->SetNumberField(TEXT("setupAnchorReachCm"), 20.0);
        Receipt->SetNumberField(TEXT("actualSetupAnchorDistanceCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), Director->GetMeasuredRoute()[10]));
        Receipt->SetArrayField(TEXT("actualStoppedSetupPositionCm"), VJson(O.ChassisPose.GetLocation()));
    }
    if (Phase == EPhase::FullEmptyLeftCenter || Phase == EPhase::FullLoadedLeftCenter
        || Phase == EPhase::FullEmptyRightCenter || Phase == EPhase::FullLoadedRightCenter)
    {
        const auto& Route = Director->GetMeasuredRoute();
        const FVector Axis = (Route.Last() - Route[0]).GetSafeNormal2D();
        const FVector Lateral(-Axis.Y, Axis.X, 0);
        Receipt->SetNumberField(TEXT("actualCenterLateralCm"), FVector::DotProduct(O.ChassisPose.GetLocation() - Route[0], Lateral));
        Receipt->SetNumberField(TEXT("actualCenterHeadingErrorDegrees"), FMath::FindDeltaAngleDegrees(Axis.Rotation().Yaw, (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw));
        Receipt->SetBoolField(TEXT("setupUsedReverseDrive"), bFullCenterReverse);
        Receipt->SetNumberField(TEXT("continuousCenteredStopWorldSeconds"), StableSinceWorldS >= 0 ? O.ObservationWorldSeconds - StableSinceWorldS : -1);
    }
    if (Phase == EPhase::FullEmptyLeftSettle || Phase == EPhase::FullLoadedLeftSettle
        || Phase == EPhase::FullEmptyRightSettle || Phase == EPhase::FullLoadedRightSettle)
    {
        Receipt->SetNumberField(TEXT("boundedRestWindowStartWorldSeconds"), StableSinceWorldS);
        Receipt->SetNumberField(TEXT("boundedRestWindowEndWorldSeconds"), O.ObservationWorldSeconds);
        Receipt->SetArrayField(TEXT("boundedRestWindowStartPositionCm"), VJson(FullRestWindowPose.GetLocation()));
        Receipt->SetNumberField(TEXT("boundedRestWindowMaxTranslationCm"), FullRestMaxTranslationCm);
        Receipt->SetNumberField(TEXT("boundedRestWindowMaxYawDegrees"), FullRestMaxYawDegrees);
        Receipt->SetStringField(TEXT("boundedRestScope"), TEXT("Every postphysics observation satisfies production stopped/support predicates with brake held and <=1cm/1degree excursion for two gameplay seconds; not exact zero motion"));
    }
    if (Phase == EPhase::NextJob)
    {
        Receipt->SetNumberField(TEXT("continuousNextJobSetupWorldSeconds"), StableSinceWorldS >= 0 ? O.ObservationWorldSeconds - StableSinceWorldS : -1);
    }
    Receipt->SetNumberField(TEXT("finalAngularSpeedRadps"), O.AngularVelocityRadps.Size());
    Receipt->SetNumberField(TEXT("peakSpeedCmps"), PhasePeakSpeedCmps);
    Receipt->SetNumberField(TEXT("peakTiltDegrees"), PhasePeakTiltDegrees);
    Receipt->SetNumberField(TEXT("peakSampledSuspensionTravelCm"), PhasePeakSampledSuspensionCm);
    Receipt->SetNumberField(TEXT("unsupportedObservationFrames"), PhaseUnsupportedFrames);
    Receipt->SetNumberField(TEXT("supportedWheelMotionMask"), SupportedWheelMotionMask);
    Receipt->SetNumberField(TEXT("brakeTravelCm"), BrakeTravelCm); Receipt->SetNumberField(TEXT("brakeStartSpeedCmps"), BrakeStartSpeed);
    double ParcelKg = 0;
    Receipt->SetBoolField(TEXT("parcelMassKnown"), Director->GetParcel() && Director->GetParcel()->ObserveMass(ParcelKg));
    Receipt->SetNumberField(TEXT("parcelMassKg"), ParcelKg); // Legacy slot-zero field.
    Receipt->SetArrayField(TEXT("parcelInventory"), ReadParcelInventoryJson());
    Receipt->SetNumberField(TEXT("jobParcelCount"), Director->Snapshot().Job.ParcelCount);
    Receipt->SetNumberField(TEXT("actualParcelActorCount"), Director->GetParcelCount());
    Receipt->SetNumberField(TEXT("actualSecuredParcelCount"), Director->GetSecuredParcelCount());
    Receipt->SetBoolField(TEXT("boostTestRequested"), bBoostTest);
    Receipt->SetNumberField(TEXT("actualBoostDriveFrames"), ActualBoostDriveFrames);
    Receipt->SetNumberField(TEXT("phaseActualBoostDriveFrames"), PhaseBoostDriveFrames);
    Receipt->SetNumberField(TEXT("batteryPercent"), Director->Snapshot().BatteryPercent);
    Receipt->SetNumberField(TEXT("totalCredits"), Director->Snapshot().TotalCredits);
    Receipt->SetStringField(TEXT("jobInstanceId"), Director->Snapshot().JobInstanceId);
    if (Phase == EPhase::ToPickup || Phase == EPhase::ToDropoff)
    {
        Receipt->SetStringField(TEXT("serviceStationId"), ServiceStationId);
        Receipt->SetArrayField(TEXT("routeApproachCm"), VJson(ServiceApproachCm));
        Receipt->SetArrayField(TEXT("serviceStationCm"), VJson(ServiceStationCm));
        Receipt->SetNumberField(TEXT("actualStationDistanceCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm));
        Receipt->SetBoolField(TEXT("actualDirectorServiceAdmitted"), true);
        Receipt->SetNumberField(TEXT("continuousServiceSetupWorldSeconds"), StableSinceWorldS >= 0 ? O.ObservationWorldSeconds - StableSinceWorldS : -1);
    }
    PhaseResults.Add(MakeShared<FJsonValueObject>(Receipt));
    FScreenshotRequest::RequestScreenshot(FPaths::Combine(RunDirectory,
        FString::Printf(TEXT("%02d-%s.png"), PhaseResults.Num(), PhaseName(Phase))), true, false);
    Phase = Next; PhaseStartWorldS = GetWorld()->GetTimeSeconds(); PhaseStartPose = O.ChassisPose;
    if (Next == EPhase::CarriedRecover)
    {
        bCarriedPreflightInitialized = false; CarriedPreflightQueryCount = 0;
        LastCarriedPreflightQueryWorldS = -1; CarriedParcelIds.Reset();
    }
    if (Next == EPhase::DoorClose) { bDoorCloseRequested = false; bDoorClosingProbePending = false; DoorClosingProbeSamples = 0; }
    StableSinceWorldS = -1; BrakeTravelCm = 0; BrakeStartSpeed = O.VelocityCmps.Size(); LastPosition = O.ChassisPose.GetLocation();
    SupportedWheelMotionMask = 0;
    bStationArrivalLatched = false; ArrivalLatchWorldS = -1; bRingViewRequested = false;
    bFullSetupArrivalLatched = false;
    bDamageTrialInitialized = false;
    if (Next == EPhase::FullEmptyLeftCenter || Next == EPhase::FullLoadedLeftCenter
        || Next == EPhase::FullEmptyRightCenter || Next == EPhase::FullLoadedRightCenter)
    {
        const auto& Route = Director->GetMeasuredRoute();
        const FVector Axis = (Route.Last() - Route[0]).GetSafeNormal2D();
        const FVector Lateral(-Axis.Y, Axis.X, 0);
        // Choose the real direction that initially moves toward the center line.
        // Reverse avoids continuing an outward heading after the previous turn.
        bFullCenterReverse = FVector::DotProduct(O.ChassisPose.GetLocation() - Route[0], Lateral)
            * FVector::DotProduct(-O.ChassisPose.GetUnitAxis(EAxis::X), Lateral) > 0;
    }
    bAwaitAutomaticRingPickup = Next == EPhase::ToPickup && !bAutomaticRingPickupObserved;
    AutomaticWaitFrames = 0; AutomaticWaitEntryInteractionCalls = StationInteractionCalls;
    PhaseStartAngularSpeedRadps = O.AngularVelocityRadps.Size();
    PhasePeakSpeedCmps = PhasePeakTiltDegrees = PhasePeakSampledSuspensionCm = 0;
    PhaseBoostDriveFrames = 0;
    PhaseUnsupportedFrames = 0;
    Robot->ReleaseDriveCommand(); Drive(0, 0, true);
    if (Next == EPhase::Charge || Next == EPhase::OperatingCharge)
    {
        ChargeStartPercent = Director->Snapshot().BatteryPercent;
        // Observation only: connection is automatic and never requested by this driver.
    }
    if (Next == EPhase::PaidChargeReturn)
    {
        AMAYADeliveryStation* Depot = Director->FindStation(Director->Snapshot().Job.PickupStationId);
        if (!Depot || !Depot->IsChargingStation()) { Stop(false, TEXT("Paid-charge setup has no actual surveyed depot charger")); return; }
        PaidChargeStationCm = Depot->GetServicePoint(); PaidChargeStationId = Depot->StationId;
        PaidChargeShiftId = Director->Snapshot().ShiftInstanceId;
        PaidChargeCashBefore = Director->Snapshot().TotalCredits;
        PaidChargeReceiptBefore = Director->Snapshot().LastRecharge.Generation;
    }
    WriteReport(false, false, TEXT("Native test in progress"));
}

bool AMAYAV01NativeTestDriver::NavigateTo(const FVector& Destination, const FMAYADeliveryRobotObservation& O)
{
    const FVector Delta = Destination - O.ChassisPose.GetLocation();
    const double Distance = FVector(Delta.X, Delta.Y, 0).Size();
    if (Distance <= 65) { Drive(0, 0, true); return O.IsStoppedAndSupported(); }
    const double TargetYaw = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
    const double CurrentYaw = (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
    // A return along the measured corridor uses the already tested reverse command.
    // Differential steering yaw keeps its sign when longitudinal throttle reverses.
    const double ForwardError = FMath::FindDeltaAngleDegrees(CurrentYaw, TargetYaw);
    const bool bReverse = FMath::Abs(ForwardError) > 90;
    const double TravelYaw = CurrentYaw + (bReverse ? 180.0 : 0.0);
    const double Error = FMath::FindDeltaAngleDegrees(TravelYaw, TargetYaw);
    const float Steer = float(FMath::Clamp(Error / 40.0, -1.0, 1.0));
    const float Magnitude = FMath::Abs(Error) > 35 ? 0.f : float(FMath::Clamp(Distance / 600.0, 0.15, 0.55));
    const float Throttle = bReverse ? -Magnitude : Magnitude;
    Drive(Throttle, Steer, false); return false;
}

void AMAYAV01NativeTestDriver::SaveRingServiceReceipt(const FMAYADeliveryRobotObservation& O,
    bool bDriverRequested)
{
    const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("phase"), PhaseName(Phase));
    J->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId);
    J->SetStringField(TEXT("stationId"), S.Service.StationId);
    J->SetStringField(TEXT("serviceGeneration"), LexToString(S.Service.Generation));
    J->SetNumberField(TEXT("observedWorldSeconds"), O.ObservationWorldSeconds);
    J->SetBoolField(TEXT("driverSubmittedInteraction"), bDriverRequested);
    J->SetBoolField(TEXT("actualServiceAdmitted"), true);
    J->SetNumberField(TEXT("stationInteractionCalls"), StationInteractionCalls);
    J->SetNumberField(TEXT("automaticWaitObservedPostPhysicsFrames"), AutomaticWaitFrames);
    J->SetNumberField(TEXT("automaticWaitInteractionCalls"),
        StationInteractionCalls - AutomaticWaitEntryInteractionCalls);
    J->SetNumberField(TEXT("arrivalLatchWorldSeconds"), ArrivalLatchWorldS);
    J->SetNumberField(TEXT("actualStationDistanceCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm));
    J->SetObjectField(TEXT("ring"), MayaV01RingObservationJson(R));
    RingServiceReceipts.Add(MakeShared<FJsonValueObject>(J));
}

bool AMAYAV01NativeTestDriver::FindServiceApproach(const FVector& StationPoint, FVector& OutApproach) const
{
    if (!Director || StationPoint.ContainsNaN()) return false;
    const auto& Route = Director->GetMeasuredRoute();
    if (Route.Num() < 2) return false;
    double BestDistance = TNumericLimits<double>::Max();
    for (int32 I = 1; I < Route.Num(); ++I)
    {
        if (Route[I - 1].ContainsNaN() || Route[I].ContainsNaN()) return false;
        const FVector2D XY = FMath::ClosestPointOnSegment2D(FVector2D(StationPoint), FVector2D(Route[I - 1]), FVector2D(Route[I]));
        const FVector Candidate(XY.X, XY.Y, Route[I - 1].Z);
        const double Distance = FVector::Dist2D(StationPoint, Candidate);
        if (Distance < BestDistance) { BestDistance = Distance; OutApproach = Candidate; }
    }
    // This selects an approach only. Actual Core::BeginService remains authoritative
    // for current range, stopped support, custody and mission state at RequestInteract.
    return BestDistance <= FMAYADeliveryBalance().ServiceRadiusCm;
}

bool AMAYAV01NativeTestDriver::ReadZeroWheelMotorEvidence() const
{
    TSharedPtr<FJsonObject> Native;
    if (!Robot || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()), Native) || !Native) return false;
    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    FString Origin; double Throttle = 0, Steer = 0; bool Brake = true, Fresh = false;
    if (!Native->TryGetStringField(TEXT("inputOrigin"), Origin) || Origin != TEXT("NativeAutomation")
        || !Native->TryGetNumberField(TEXT("inputThrottle"), Throttle) || Throttle != 1
        || !Native->TryGetNumberField(TEXT("inputSteer"), Steer) || Steer != 0
        || !Native->TryGetBoolField(TEXT("inputBrakeRequested"), Brake) || Brake
        || !Native->TryGetBoolField(TEXT("inputCommandFresh"), Fresh) || !Fresh) return false;
    if (bBoostTest)
    {
        bool Requested = false, Active = true;
        if (!Native->TryGetBoolField(TEXT("inputBoostRequested"), Requested) || !Requested
            || !Native->TryGetBoolField(TEXT("boostDriveActive"), Active) || Active) return false;
    }
    if (!Native->TryGetArrayField(TEXT("parts"), Parts) || !Parts || Parts->Num() < 7) return false;
    for (int32 W = 0; W < 6; ++W)
    {
        const auto Part = (*Parts)[W + 1]->AsObject(); double Nm = 0;
        if (!Part || !Part->TryGetNumberField(TEXT("motorNm"), Nm) || !FMath::IsFinite(Nm) || FMath::Abs(Nm) > 1.e-6) return false;
    }
    return true;
}

bool AMAYAV01NativeTestDriver::ReadOperatingZeroMotorEvidence() const
{
    TSharedPtr<FJsonObject> J;
    if (!Robot || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()), J) || !J) return false;
    FString Origin; double Throttle = 0, Steer = 0; bool Brake = true, Fresh = false;
    if (!J->TryGetStringField(TEXT("inputOrigin"), Origin) || Origin != TEXT("NativeAutomation")
        || !J->TryGetNumberField(TEXT("inputThrottle"), Throttle) || Throttle != 1
        || !J->TryGetNumberField(TEXT("inputSteer"), Steer) || Steer != 0
        || !J->TryGetBoolField(TEXT("inputBrakeRequested"), Brake) || Brake
        || !J->TryGetBoolField(TEXT("inputCommandFresh"), Fresh) || !Fresh) return false;
    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    if (!J->TryGetArrayField(TEXT("parts"), Parts) || !Parts || Parts->Num() < 7) return false;
    for (int32 W = 0; W < 6; ++W)
    {
        const auto P = (*Parts)[W + 1]->AsObject(); double Nm = 0;
        if (!P || !P->TryGetNumberField(TEXT("motorNm"), Nm) || !FMath::IsFinite(Nm) || FMath::Abs(Nm) > 1.e-6) return false;
    }
    return true;
}

bool AMAYAV01NativeTestDriver::TryStartNativeLidPause(const FMAYADeliveryRobotObservation& Observation)
{
    if (!bOperatingTest || bNativePauseActive || !Observation.IsStoppedAndSupported()) return false;
    TSharedPtr<FJsonObject> J; double DoorState = -1;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()), J) || !J
        || !J->TryGetNumberField(TEXT("doorState"), DoorState)) return false;
    const bool Opening = DoorState == 1 && !bOpeningPauseComplete;
    const bool Closing = DoorState == 3 && !bClosingPauseComplete;
    // Pause a genuinely partial, moving lid state, not an already achieved endpoint.
    const double Angle = FMath::Max(FMath::Abs(Observation.DoorAngleDegrees[0]), FMath::Abs(Observation.DoorAngleDegrees[1]));
    const double Rate = FMath::Max(FMath::Abs(Observation.DoorOmegaRadps[0]), FMath::Abs(Observation.DoorOmegaRadps[1]));
    if ((!Opening && !Closing) || Angle <= 2 || Angle >= 75 || Rate <= 0.01) return false;
    if (Director->RequestCharging()) { Stop(false, TEXT("Charging was admitted during actual mechanical service")); return true; }
    bPausedDuringRingCountdown = false;
    bPausedDuringOpening = Opening;
    PausedMission = Director->Snapshot(); PausedRobot = Observation;
    if (!CapturePausedInventory()) { Stop(false, TEXT("Actual parcel inventory could not be captured before native pause")); return true; }
    PausedParcelId = PausedParcelIds.Num() ? PausedParcelIds[0] : FString();
    PauseWorldS = GetWorld()->GetTimeSeconds(); PauseAudioS = GetWorld()->GetAudioTimeSeconds();
    PauseStartedRealS = FPlatformTime::Seconds(); LastPauseSampleRealS = -1;
    Director->SetPaused(true);
    if (!UGameplayStatics::SetGamePaused(this, true) || !UGameplayStatics::IsGamePaused(this))
    { Director->SetPaused(false); Stop(false, TEXT("Native SetGamePaused request did not produce a real paused world")); return true; }
    bNativePauseActive = true;
    Record(Observation, true, Opening ? TEXT("native_opening_pause_start") : TEXT("native_closing_pause_start"));
    return true;
}

bool AMAYAV01NativeTestDriver::TryStartRingCountdownPause(const FMAYADeliveryRobotObservation& O)
{
    const auto& R = Director->GetRingObservation(); const auto S = Director->Snapshot();
    if (!bRingInteractionTest || bRingCountdownPauseComplete || bNativePauseActive
        || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched
        || S.Phase != EMAYADeliveryPhase::Accepted || !R.bInside || !R.bEligible || R.bVisitConsumed
        || R.ElapsedGameplaySeconds < 2.0 || R.ElapsedGameplaySeconds > 4.0) return false;
    bPausedDuringRingCountdown = true;
    PausedMission = S; PausedRobot = O;
    if (!CapturePausedInventory()) { Stop(false, TEXT("Actual parcel inventory could not be captured before native pause")); return true; }
    PausedParcelId = PausedParcelIds.Num() ? PausedParcelIds[0] : FString();
    PausedRingEvidenceJson = SerializeMayaV01Json(MayaV01RingObservationJson(R));
    PauseWorldS = GetWorld()->GetTimeSeconds(); PauseAudioS = GetWorld()->GetAudioTimeSeconds();
    PauseStartedRealS = FPlatformTime::Seconds(); LastPauseSampleRealS = -1;
    Director->SetPaused(true);
    if (!UGameplayStatics::SetGamePaused(this, true) || !UGameplayStatics::IsGamePaused(this))
    { Director->SetPaused(false); Stop(false, TEXT("Ring countdown pause did not produce an actual paused world")); return true; }
    bNativePauseActive = true;
    Record(O, true, TEXT("native_ring_countdown_pause_start")); return true;
}

bool AMAYAV01NativeTestDriver::TryStartAirLoweringPause(const FMAYADeliveryRobotObservation& O)
{
    if (!bAirLoweringTest || bAirLoweringPauseComplete || bNativePauseActive || !Director->IsCargoAirLowering()
        || Director->GetCargoAirPhaseElapsedSeconds() < 0.45 || Director->GetCargoAirPhaseElapsedSeconds() > 1.2
        || !O.IsStoppedAndSupported() || !O.bDoorsOpen) return false;
    bPausedDuringAirLowering = true; bPausedDuringRingCountdown = false; bPausedDuringOpening = false;
    PausedMission = Director->Snapshot(); PausedRobot = O;
    if (!CapturePausedInventory()) { Stop(false, TEXT("Actual mid-descent inventory could not be captured before pause")); return true; }
    PausedAirEvidenceJson = Director->ReadCargoInventoryEvidenceJson();
    PausedParcelId = PausedParcelIds.Num() ? PausedParcelIds[0] : FString();
    PauseWorldS = GetWorld()->GetTimeSeconds(); PauseAudioS = GetWorld()->GetAudioTimeSeconds();
    PauseStartedRealS = FPlatformTime::Seconds(); LastPauseSampleRealS = -1;
    Director->SetPaused(true);
    if (!UGameplayStatics::SetGamePaused(this, true) || !UGameplayStatics::IsGamePaused(this))
    { Director->SetPaused(false); Stop(false, TEXT("Air lowering regression did not produce an actual paused world")); return true; }
    bNativePauseActive = true; Record(O, true, TEXT("native_air_lowering_pause_start")); return true;
}

bool AMAYAV01NativeTestDriver::TryStartAirUnloadingPause(const FMAYADeliveryRobotObservation& O)
{
    if (!bAirLoweringTest || bAirUnloadPauseComplete || bNativePauseActive || !Director->IsCargoAirFadingOut()
        || Director->GetCargoAirPhaseElapsedSeconds() < 0.3 || Director->GetCargoAirPhaseElapsedSeconds() > 0.7
        || !O.IsStoppedAndSupported() || !O.bDoorsOpen) return false;
    bPausedDuringAirUnloading = true; bPausedDuringAirLowering = false;
    bPausedDuringRingCountdown = false; bPausedDuringOpening = false;
    PausedMission = Director->Snapshot(); PausedRobot = O;
    if (!CapturePausedInventory()) { Stop(false, TEXT("Actual raised inventory could not be captured before fade pause")); return true; }
    PausedAirEvidenceJson = Director->ReadCargoInventoryEvidenceJson();
    PausedParcelId = PausedParcelIds.Num() ? PausedParcelIds[0] : FString();
    PauseWorldS = GetWorld()->GetTimeSeconds(); PauseAudioS = GetWorld()->GetAudioTimeSeconds();
    PauseStartedRealS = FPlatformTime::Seconds(); LastPauseSampleRealS = -1;
    Director->SetPaused(true);
    if (!UGameplayStatics::SetGamePaused(this, true) || !UGameplayStatics::IsGamePaused(this))
    { Director->SetPaused(false); Stop(false, TEXT("Reverse unload fade did not enter the actual paused world")); return true; }
    bNativePauseActive = true; Record(O, true, TEXT("native_air_unloading_fade_pause_start")); return true;
}

void AMAYAV01NativeTestDriver::TickNativeLidPause()
{
    FMAYADeliveryRobotObservation O; Robot->ReadDeliveryState(O);
    const auto S = Director->Snapshot();
    const double RealNow = FPlatformTime::Seconds(), WallAge = RealNow - PauseStartedRealS;
    // Contact freshness is based on real time and can expire in a paused world.
    // The pause claim is unchanged measured poses/angles and frozen gameplay clocks.
    if (!UGameplayStatics::IsGamePaused(this) || !Director->IsPaused() || !O.bKnown || !O.Fault.IsEmpty()
        || FMath::Abs(GetWorld()->GetTimeSeconds() - PauseWorldS) > 0.001
        || FMath::Abs(GetWorld()->GetAudioTimeSeconds() - PauseAudioS) > 0.001
        || S.Phase != PausedMission.Phase || S.JobInstanceId != PausedMission.JobInstanceId
        || (bPausedDuringRingCountdown
            ? S.Service.Generation != PausedMission.Service.Generation
                || (S.Service.Generation != 0 && !S.Service.Equals(PausedMission.Service))
            : !S.Service.Equals(PausedMission.Service))
        || (bPausedDuringRingCountdown && SerializeMayaV01Json(MayaV01RingObservationJson(Director->GetRingObservation())) != PausedRingEvidenceJson)
        || S.TotalCredits != PausedMission.TotalCredits
        || FMath::Abs(S.ElapsedSeconds - PausedMission.ElapsedSeconds) > 1.e-6
        || FMath::Abs(S.BatteryPercent - PausedMission.BatteryPercent) > 1.e-6
        || O.ResetGeneration != PausedRobot.ResetGeneration
        || FVector::Dist(O.ChassisPose.GetLocation(), PausedRobot.ChassisPose.GetLocation()) > 0.1
        || O.ChassisPose.GetRotation().AngularDistance(PausedRobot.ChassisPose.GetRotation()) > FMath::DegreesToRadians(0.1)
        || FMath::Abs(O.DoorAngleDegrees[0] - PausedRobot.DoorAngleDegrees[0]) > 0.1
        || FMath::Abs(O.DoorAngleDegrees[1] - PausedRobot.DoorAngleDegrees[1]) > 0.1
        || !PausedInventoryUnchanged()
        || ((bPausedDuringAirLowering || bPausedDuringAirUnloading) && Director->ReadCargoInventoryEvidenceJson() != PausedAirEvidenceJson))
    { Stop(false, TEXT("Actual world pause changed mission/battery/service/cargo/lid pose or faulted the physical door watchdog")); return; }
    if (LastPauseSampleRealS < 0 || RealNow - LastPauseSampleRealS >= MayaV01TelemetryCadenceS)
    { LastPauseSampleRealS = RealNow; Record(O, true, TEXT("native_pause_observation")); }
    if (bTelemetryWriteFailed) { Stop(false, TEXT("Native pause telemetry append failed")); return; }
    if (WallAge < 6.0) return; // Deliberately exceeds the unchanged five-active-real-second door watchdog.
    auto P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("phase"), bPausedDuringAirUnloading ? TEXT("ActualAirUnloadingFadeWorldPause") : bPausedDuringAirLowering ? TEXT("ActualAirLoweringWorldPause") : bPausedDuringRingCountdown ? TEXT("ActualRingCountdownWorldPause")
        : bPausedDuringOpening ? TEXT("ActualOpeningWorldPause") : TEXT("ActualClosingWorldPause"));
    if (bPausedDuringRingCountdown)
    {
        P->SetStringField(TEXT("ringBefore"), PausedRingEvidenceJson);
        P->SetStringField(TEXT("ringAfter"), SerializeMayaV01Json(MayaV01RingObservationJson(Director->GetRingObservation())));
    }
    P->SetStringField(TEXT("pauseOrigin"), TEXT("Native SetGamePaused API; no manual key/focus acceptance"));
    P->SetBoolField(TEXT("pass"), true); P->SetNumberField(TEXT("wallSeconds"), WallAge);
    P->SetNumberField(TEXT("worldSecondsFrozenAt"), PauseWorldS); P->SetNumberField(TEXT("audioSecondsFrozenAt"), PauseAudioS);
    P->SetNumberField(TEXT("missionElapsedSecondsFrozenAt"), S.ElapsedSeconds);
    P->SetNumberField(TEXT("batteryPercentFrozenAt"), S.BatteryPercent);
    P->SetNumberField(TEXT("door1DegreesBefore"), PausedRobot.DoorAngleDegrees[0]); P->SetNumberField(TEXT("door1DegreesAfter"), O.DoorAngleDegrees[0]);
    P->SetNumberField(TEXT("door2DegreesBefore"), PausedRobot.DoorAngleDegrees[1]); P->SetNumberField(TEXT("door2DegreesAfter"), O.DoorAngleDegrees[1]);
    P->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId); P->SetStringField(TEXT("parcelId"), PausedParcelId);
    P->SetNumberField(TEXT("serviceGeneration"), double(S.Service.Generation));
    P->SetArrayField(TEXT("actualPausedParcelInventory"), ReadParcelInventoryJson());
    PhaseResults.Add(MakeShared<FJsonValueObject>(P));
    if (!UGameplayStatics::SetGamePaused(this, false) || UGameplayStatics::IsGamePaused(this))
    { Stop(false, TEXT("Native pause regression could not resume the actual game world")); return; }
    Director->SetPaused(false); bNativePauseActive = false;
    if (bPausedDuringAirUnloading) bAirUnloadPauseComplete = true;
    else if (bPausedDuringAirLowering) bAirLoweringPauseComplete = true;
    else if (bPausedDuringRingCountdown) bRingCountdownPauseComplete = true;
    else if (bPausedDuringOpening) bOpeningPauseComplete = true; else bClosingPauseComplete = true;
    bPausedDuringRingCountdown = false; bPausedDuringAirLowering = false; bPausedDuringAirUnloading = false;
    Drive(0, 0, true); // Renew a real neutral lease; phase clocks/poses remain unchanged.
    Record(O, true, TEXT("native_pause_resumed"));
}


void AMAYAV01NativeTestDriver::ObserveCashOpening(const FMAYADeliveryRobotObservation& O)
{
    if (!(bCashShiftTest || bTrafficEncounterTest) || bCashNaturalRetryUsed) return;
    const auto S = Director->Snapshot();
    if (S.Phase != EMAYADeliveryPhase::Loading || !S.Service.bPickup || S.Service.Generation == 0
        || (bCashOpeningObserved && S.Service.Generation == CashOpeningBefore.Service.Generation)
        || !Director->GetLastServiceFailureDetail().IsEmpty() || Director->GetParcelCount() != 0
        || Director->GetSecuredParcelCount() != 0 || S.bCharging || !O.IsStoppedAndSupported()) return;
    const auto Cancellation = MayaV01ReadJson(Director->ReadServiceCancellationEvidenceJson());
    uint64 Generation = 0;
    if (!MayaV01ReadGeneration(Cancellation, TEXT("cancellationGeneration"), Generation)
        || Cancellation->GetStringField(TEXT("serviceStep")) != TEXT("Opening")) return;
    // Read the actual native state once per service; missing endpoints alone do
    // not prove Opening. EDoorState::Opening is 1 in the installed pawn.
    const FString NativeText = Robot->ReadTelemetryJson(); const auto Native = MayaV01ReadJson(NativeText);
    double DoorState = -1;
    if (!Native.IsValid() || !Native->TryGetNumberField(TEXT("doorState"), DoorState) || DoorState != 1) return;
    int32 Live = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++Live;
    if (Live != 0) return;
    CashOpeningBefore = S; CashOpeningResetGeneration = O.ResetGeneration;
    CashOpeningCancellationBefore = Generation; CashOpeningNativeEvidence = NativeText; bCashOpeningObserved = true;
}

bool AMAYAV01NativeTestDriver::CashOpeningIdentityUnchanged(const FMAYADeliveryRobotObservation& O) const
{
    const auto S = Director->Snapshot();
    return S.ShiftInstanceId == CashOpeningBefore.ShiftInstanceId && S.JobInstanceId == CashOpeningBefore.JobInstanceId
        && S.Job.ParcelCount == CashOpeningBefore.Job.ParcelCount && S.Job.ProfileId == CashOpeningBefore.Job.ProfileId
        && O.ResetGeneration == CashOpeningResetGeneration && S.Recoveries == CashOpeningBefore.Recoveries
        && S.TotalCredits == CashOpeningBefore.TotalCredits && S.CompletedJobs == CashOpeningBefore.CompletedJobs
        && S.RechargeFeesPaidDollars == CashOpeningBefore.RechargeFeesPaidDollars
        && S.LastRecharge.Generation == CashOpeningBefore.LastRecharge.Generation
        && S.LastRecharge.VisitGeneration == CashOpeningBefore.LastRecharge.VisitGeneration
        && S.CargoConditionPercent == CashOpeningBefore.CargoConditionPercent && !S.bCharging && !S.bShiftWon;
}

bool AMAYAV01NativeTestDriver::BeginCashOpeningRetry(const FMAYADeliveryRobotObservation& O)
{
    if (!(bCashShiftTest || bTrafficEncounterTest) || bCashNaturalRetryUsed || !bCashOpeningObserved || !CashOpeningIdentityUnchanged(O)) return false;
    const auto S = Director->Snapshot(); const FString Failure = Director->GetLastServiceFailureDetail();
    const auto F = MayaV01FailureFields(Failure);
    double Speed = -1, Angular = -1, Drift = -1, Mask = -1;
    uint64 FailureService = 0, FailureReset = 0, ExpectedReset = 0;
    if (F.FindRef(TEXT("stage")) != TEXT("service_interrupt") || F.FindRef(TEXT("pickup")) != TEXT("true")
        || F.FindRef(TEXT("nativeEndpointAttempted")) != TEXT("false") || F.FindRef(TEXT("actorCount")) != TEXT("0")
        || F.FindRef(TEXT("securedCount")) != TEXT("0")
        || F.FindRef(TEXT("cancellationReason")) != TEXT("current stopped/support predicate lost")
        || F.FindRef(TEXT("known")) != TEXT("true") || F.FindRef(TEXT("ready")) != TEXT("true")
        || F.FindRef(TEXT("grounded")) != TEXT("true") || F.FindRef(TEXT("upright")) != TEXT("true")
        || F.FindRef(TEXT("stoppedSupported")) != TEXT("false") || F.FindRef(TEXT("doorsOpen")) != TEXT("false")
        || F.FindRef(TEXT("doorsClosedLatched")) != TEXT("false") || !F.Contains(TEXT("fault")) || !F.FindRef(TEXT("fault")).IsEmpty()
        || !MayaV01FailureNumber(F, TEXT("speedCmps"), Speed) || Speed < 0 || Speed > 5
        || !MayaV01FailureNumber(F, TEXT("angularSpeedRadps"), Angular) || Angular <= 0.05
        || !MayaV01FailureNumber(F, TEXT("servicePositionDriftCm"), Drift) || Drift < 0 || Drift > 5
        || !MayaV01FailureNumber(F, TEXT("supportMask"), Mask) || Mask != 63
        || !LexTryParseString(FailureService, *F.FindRef(TEXT("serviceGeneration")))
        || !LexTryParseString(FailureReset, *F.FindRef(TEXT("resetGeneration")))
        || !LexTryParseString(ExpectedReset, *F.FindRef(TEXT("expectedResetGeneration")))
        || FailureService != CashOpeningBefore.Service.Generation || FailureReset != CashOpeningResetGeneration
        || ExpectedReset != CashOpeningResetGeneration) return false;
    const FString CancellationText = Director->ReadServiceCancellationEvidenceJson();
    const auto C = MayaV01ReadJson(CancellationText); uint64 Generation = 0, Service = 0, OpenService = 0;
    if (!MayaV01ReadGeneration(C, TEXT("cancellationGeneration"), Generation)
        || !MayaV01ReadGeneration(C, TEXT("serviceGeneration"), Service)
        || !MayaV01ReadGeneration(C, TEXT("cancelledOpenServiceGeneration"), OpenService)
        || Generation != CashOpeningCancellationBefore + 1 || Service != FailureService || OpenService != FailureService
        || C->GetStringField(TEXT("serviceStep")) != TEXT("None") || C->GetBoolField(TEXT("cancellationPending"))
        || C->GetBoolField(TEXT("explicitOpeningRegressionUsed")) || !C->GetBoolField(TEXT("originalResetGenerationMatches"))
        || !C->GetBoolField(TEXT("thisServiceCancelledOpenEndpointsObserved"))
        || C->GetStringField(TEXT("lastCancellationReason")) != TEXT("current stopped/support predicate lost")
        || !Director->WasCancelledOpenObserved(FailureService) || S.Phase != EMAYADeliveryPhase::Accepted
        || !O.bDoorsClosedLatched || Director->GetParcelCount() != 0 || Director->GetSecuredParcelCount() != 0
        || !FMath::IsFinite(S.BatteryPercent) || S.BatteryPercent > CashOpeningBefore.BatteryPercent + 1e-6) return false;
    int32 Live = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++Live;
    const auto& Ring = Director->GetRingObservation();
    if (Live != 0 || !Ring.bInside || !Ring.bVisitConsumed || Ring.ActiveStationId != S.Job.PickupStationId) return false;
    bCashNaturalRetryUsed = true; CashNaturalOriginalFailure = Failure; CashNaturalCancellationEvidence = CancellationText;
    CashNaturalClosureBatteryPercent = S.BatteryPercent;
    auto J = MAYADeliveryEconomyToJson(S); J->SetStringField(TEXT("event"), TEXT("natural_empty_opening_cancel_closed_retry_setup"));
    J->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId); J->SetStringField(TEXT("resetGeneration"), LexToString(O.ResetGeneration));
    J->SetStringField(TEXT("originalServiceGeneration"), LexToString(FailureService));
    J->SetStringField(TEXT("openingNativeBefore"), CashOpeningNativeEvidence);
    J->SetStringField(TEXT("originalFailureDetail"), Failure); J->SetStringField(TEXT("actualCancellation"), CancellationText);
    J->SetStringField(TEXT("nativeAtClosedCancellation"), Robot->ReadTelemetryJson());
    J->SetNumberField(TEXT("worldSeconds"), O.ObservationWorldSeconds); J->SetNumberField(TEXT("batteryPercent"), S.BatteryPercent);
    J->SetNumberField(TEXT("conditionPercent"), S.CargoConditionPercent); J->SetNumberField(TEXT("actualParcelActors"), Live);
    J->SetStringField(TEXT("scope"), TEXT("One total CashShift or explicit TrafficEncounter diagnostic retry after observed natural empty Opening angular stop violation; original failed service retained; no production exception or resource writes"));
    CashNaturalOpeningRetryReceipts.Add(MakeShared<FJsonValueObject>(J)); Enter(EPhase::CashOpeningRetry, O); return true;
}

void AMAYAV01NativeTestDriver::TickCashOpeningRetry(const FMAYADeliveryRobotObservation& O, double Age, double Now)
{
    Drive(0, 0, true); const auto S = Director->Snapshot();
    if (Age > 20) { Stop(false, TEXT("Natural empty Opening diagnostic retry settlement exceeded twenty gameplay seconds")); return; }
    int32 Live = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++Live;
    const auto& Ring = Director->GetRingObservation();
    if (!(bCashShiftTest || bTrafficEncounterTest) || !bCashNaturalRetryUsed || bCashNaturalRetryAdmitted || !CashOpeningIdentityUnchanged(O)
        || S.Phase != EMAYADeliveryPhase::Accepted || Director->GetParcelCount() != 0 || Director->GetSecuredParcelCount() != 0
        || Live != 0 || !Ring.bInside || !Ring.bVisitConsumed || Ring.ActiveStationId != S.Job.PickupStationId
        || Director->GetLastServiceFailureDetail() != CashNaturalOriginalFailure
        || !FMath::IsFinite(S.BatteryPercent) || S.BatteryPercent > CashNaturalClosureBatteryPercent + 1e-6)
    { Stop(false, TEXT("Natural Opening retry changed the original identity, empty custody, cash, fee, condition, energy or visit")); return; }
    if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched || O.SupportMask != 63
        || !Robot->CanRequestDeliveryDoorState(true) || O.ObservationWorldSeconds > Now
        || Now - O.ObservationWorldSeconds > 0.25)
    { StableSinceWorldS = -1; return; }
    if (StableSinceWorldS < 0)
    {
        StableSinceWorldS = Now; CashNaturalRestWindowPose = O.ChassisPose;
        CashNaturalMaxWindowCm = CashNaturalMaxWindowDegrees = 0;
    }
    const double Distance = FVector::Dist(CashNaturalRestWindowPose.GetLocation(), O.ChassisPose.GetLocation());
    const double Angle = FMath::RadiansToDegrees(CashNaturalRestWindowPose.GetRotation().AngularDistance(O.ChassisPose.GetRotation()));
    CashNaturalMaxWindowCm = FMath::Max(CashNaturalMaxWindowCm, Distance);
    CashNaturalMaxWindowDegrees = FMath::Max(CashNaturalMaxWindowDegrees, Angle);
    if (Distance > 1 || Angle > 1) { StableSinceWorldS = -1; return; }
    if (Now - StableSinceWorldS < 2) return;
    const uint64 BeforeRequest = Ring.RequestGeneration;
    auto J = MAYADeliveryEconomyToJson(S); J->SetStringField(TEXT("event"), TEXT("one_normal_interaction_after_natural_cancel"));
    J->SetNumberField(TEXT("worldSeconds"), Now); J->SetNumberField(TEXT("continuousQuietGameplaySeconds"), Now - StableSinceWorldS);
    J->SetNumberField(TEXT("quietMaxTranslationCm"), CashNaturalMaxWindowCm); J->SetNumberField(TEXT("quietMaxAngularDegrees"), CashNaturalMaxWindowDegrees);
    J->SetNumberField(TEXT("batteryBeforeRequest"), S.BatteryPercent); J->SetStringField(TEXT("actualCancellationBeforeRetry"), Director->ReadServiceCancellationEvidenceJson());
    ++StationInteractionCalls;
    const bool Accepted = Director->RequestInteract(); const auto After = Director->Snapshot(); const auto& Request = Director->GetRingObservation();
    J->SetBoolField(TEXT("requestAccepted"), Accepted); J->SetObjectField(TEXT("ringAfterRequest"), MayaV01RingObservationJson(Request));
    J->SetStringField(TEXT("retryServiceGeneration"), LexToString(After.Service.Generation));
    CashNaturalOpeningRetryReceipts.Add(MakeShared<FJsonValueObject>(J));
    if (!Accepted || After.Phase != EMAYADeliveryPhase::Loading || !After.Service.bPickup
        || After.Service.Generation <= CashOpeningBefore.Service.Generation || !CashOpeningIdentityUnchanged(O)
        || !Request.LastRequestAccepted || Request.LastRequestOrigin != TEXT("ExplicitInteraction")
        || Request.RequestGeneration != BeforeRequest + 1 || Request.LastRequestServiceGeneration != After.Service.Generation)
    { Stop(false, TEXT("Single normal empty Opening diagnostic retry was refused or lacks matching new-token explicit provenance")); return; }
    bCashNaturalRetryAdmitted = true; CashNaturalRetryServiceGeneration = After.Service.Generation;
    ++ExplicitRingAdmissionCount; SaveRingServiceReceipt(O, true); Enter(EPhase::Loading, O);
}

bool AMAYAV01NativeTestDriver::ObserveCashShift(const FMAYADeliveryRobotObservation& O)
{
    const auto S = Director->Snapshot();
    if (GetWorld()->GetTimeSeconds() - RunStartWorldS > 1200 || S.CompletedJobs > 12)
    { Stop(false, TEXT("CashShift exceeded twelve genuine deliveries or1200 gameplay seconds")); return false; }
    if (S.ShiftInstanceId != CashShiftInstanceId || S.TotalCredits < 0 || S.RechargeFeesPaidDollars < 0)
    { Stop(false, TEXT("CashShift identity or nonnegative wallet authority changed")); return false; }
    if (S.CompletedJobs != CashShiftCompletedJobsObserved)
    {
        const int32 Earnings = S.Job.ParcelCount == 2 ? 16 : 10;
        int32 Live = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++Live;
        if (S.CompletedJobs != CashShiftCompletedJobsObserved + 1 || S.Phase != EMAYADeliveryPhase::Succeeded
            || !S.Result.bSuccess || S.Result.JobInstanceId != S.JobInstanceId || S.Result.Credits != Earnings
            || CashShiftCompletedIds.Contains(S.JobInstanceId) || Director->GetParcelCount() != 0 || Live != 0
            || !O.bKnown || !O.bReady || !O.Fault.IsEmpty() || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched)
        { Stop(false, TEXT("CashShift delivery ledger lacks unique verified closed-custody exact10/16 earnings")); return false; }
        CashShiftDeliveryEarnings += Earnings; ++CashShiftCompletedJobsObserved;
        CashShiftCompletedIds.Add(S.JobInstanceId);
        if (S.TotalCredits != CashShiftDeliveryEarnings - S.RechargeFeesPaidDollars)
        { Stop(false, TEXT("CashShift current net does not equal actual delivery earnings less paid fees")); return false; }
        auto J = MAYADeliveryEconomyToJson(S);
        J->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId); J->SetNumberField(TEXT("parcelCount"), S.Job.ParcelCount);
        J->SetNumberField(TEXT("actualCompletedJobs"), S.CompletedJobs); J->SetNumberField(TEXT("actualDeliveryEarningsDollars"), Earnings);
        J->SetNumberField(TEXT("summedVerifiedDeliveryEarningsDollars"), CashShiftDeliveryEarnings);
        J->SetNumberField(TEXT("worldSeconds"), O.ObservationWorldSeconds); J->SetNumberField(TEXT("remainingActualParcelActors"), Live);
        J->SetBoolField(TEXT("actualClosedLatched"), O.bDoorsClosedLatched);
        CashShiftDeliveryReceipts.Add(MakeShared<FJsonValueObject>(J));
    }
    if (!S.bShiftWon) return true;
    const auto& W = S.ShiftWin;
    const bool Complete = W.bFrozen && W.ShiftInstanceId == CashShiftInstanceId && W.FinalJobInstanceId == S.JobInstanceId
        && W.FinalCashDollars == S.TotalCredits && W.FinalCashDollars >=100 && W.CashBefore <100
        && W.CashBefore + W.DeliveryEarningsDollars == W.FinalCashDollars && W.CompletedJobs == S.CompletedJobs
        && S.CompletedJobs == CashShiftDeliveryReceipts.Num() && S.TotalCredits == CashShiftDeliveryEarnings - S.RechargeFeesPaidDollars
        && bPaidRingChargeObserved && bPaidMovementStopObserved && bBlockedPathChecked && bCarriedRecoveryChecked
        && bAutomaticRingPickupObserved && !bUnexpectedAutomaticServiceObserved && ExplicitRingAdmissionCount > 0
        && !S.bCharging && O.IsStoppedAndSupported() && O.bDoorsClosedLatched && Director->GetParcelCount()==0
        && Director->GetPlayerPresentation().bCanStartNewShift && !bTelemetryCapped && !bTelemetryWriteFailed
        && (!bCashNaturalRetryUsed || (bCashNaturalRetryAdmitted && bCashNaturalRetrySecured));
    MissionCount = S.CompletedJobs; bActualCashShiftWinObserved = Complete;
    Stop(Complete, Complete ? TEXT("Actual genuine deliveries, exact10/16 payouts, one funded ring fee and first net100 crossing completed; frozen ShiftWin and actual terminal viewport saved; keyboard/settings/package scope is separate")
        : TEXT("Actual ShiftWon lacked exact frozen cash/custody/paid-ring/recovery/uncapped native coverage"));
    return false;
}

void AMAYAV01NativeTestDriver::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds); if (bStopped || RunStartWorldS < 0 || !Robot || !Director) return;
    if (bNativePauseActive) { TickNativeLidPause(); return; }
    FMAYADeliveryRobotObservation O; Robot->ReadDeliveryState(O);
    if (bCashShiftTest && !ObserveCashShift(O)) return;
    PhasePeakSpeedCmps = FMath::Max(PhasePeakSpeedCmps, double(O.VelocityCmps.Size()));
    if (O.bBoostDriveActive) { ++ActualBoostDriveFrames; ++PhaseBoostDriveFrames; }
    const double Tilt = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(double(O.ChassisPose.GetUnitAxis(EAxis::Z).Z), -1.0, 1.0)));
    PhasePeakTiltDegrees = FMath::Max(PhasePeakTiltDegrees, Tilt);
    if (O.bKnown && O.SupportMask != 0x3f) ++PhaseUnsupportedFrames;
    UpdateInstantServiceObservationView(O);
    const bool ServiceVideoPhase = Phase == EPhase::Loading || Phase == EPhase::Unloading;
    if (bCargoVideoRequested && ServiceVideoPhase && ServiceObservationCamera && !bServiceVideoAttempted)
    {
        bServiceVideoAttempted = true;
        if (ServiceVideoClipCount >= 16) { Stop(false, TEXT("Instant-service video exceeded the declared sixteen-clip bound")); return; }
        ActiveServiceVideoParcelCount = Director->Snapshot().Job.ParcelCount; bActiveServiceVideoLoading = Phase == EPhase::Loading;
        ActiveServiceVideoLabel = FString::Printf(TEXT("service-%02d-%s-count%d"), ++ServiceVideoClipCount,
            PhaseName(Phase), Director->Snapshot().Job.ParcelCount);
        ServiceVideo = MakeShared<FMAYAInstantCargoVideoCapture>();
        if (!ServiceVideo->Start(GetWorld(), RunDirectory, ActiveServiceVideoLabel, ReadServiceVideoFrameEvidenceJson(O)))
        { ServiceVideo.Reset(); Stop(false, TEXT("Requested actual instant-service viewport capture could not start")); return; }
    }
    // Rendered camera warmup only: simulation/service proceeds without any delay.
    if (ServiceVideo && ServiceVideoPhase && GFrameCounter >= ServiceVideoStartFrame + 4
        && GetWorld()->GetTimeSeconds() - ServiceViewStartWorldS >= 0.25)
        ServiceVideo->Tick(GetWorld()->GetTimeSeconds(), ReadServiceVideoFrameEvidenceJson(O));
    Record(O);
    if (bCashShiftTest && bTelemetryCapped) { Stop(false, TEXT("CashShift telemetry reached the unchanged evidence limit before native completion")); return; }
    if (bTelemetryWriteFailed) { Stop(false, TEXT("Native telemetry file append failed; evidence recording unavailable")); return; }
    const double Now = GetWorld()->GetTimeSeconds(), Age = Now - PhaseStartWorldS;
    if (Phase == EPhase::ReverseSteering) { TickReverseSteering(O); return; }
    if (Phase == EPhase::TrafficEncounter)
    {
        // Preserve actual tumble/fault/contact reaction for the bounded observer;
        // its pre-contact guards remain strict and its only recovery uses the UI gate.
        if (!TrafficEncounterTrial)
        {
            TrafficEncounterTrial = GetWorld()->SpawnActor<AMAYAV01TrafficEncounterTrial>();
            if (!TrafficEncounterTrial || !TrafficEncounterTrial->Configure(Robot, Director, O, RunDirectory))
            { Stop(false, TrafficEncounterTrial ? TrafficEncounterTrial->FailureReason() : TEXT("Real traffic observer could not spawn")); return; }
        }
        const auto Outcome = TrafficEncounterTrial->Advance(O);
        bLastCommandAccepted = TrafficEncounterTrial->LastCommandAccepted();
        if (Outcome == AMAYAV01TrafficEncounterTrial::EOutcome::Failed) Stop(false, TrafficEncounterTrial->FailureReason());
        else if (Outcome == AMAYAV01TrafficEncounterTrial::EOutcome::Complete)
        {
            if (TrafficEncounterTrial->IsCrashFixture())
            {
                Record(O,true,TEXT("actual_fatal_crash_before_new_shift"));
                WriteReport(false,false,TEXT("Nonterminal genuine controlled original-car fatal contact; real Start Over confirmation/reload remains pending"));
                FString Checkpoint;
                if (!TrafficEncounterTrial->CrashFixturePassed()
                    || !FFileHelper::LoadFileToString(Checkpoint,*FPaths::Combine(RunDirectory,TEXT("result.json")))
                    || !FFileHelper::SaveStringToFile(Checkpoint,*FPaths::Combine(RunDirectory,TEXT("crash-before-reload.json")))
                    || !UMAYAPlayerNewShiftReloadTest::BeginFatalHandoff(GetWorld(),RunDirectory,O))
                { Stop(false,TEXT("Actual fatal crash could not hand off to the real Start Over observer"));return; }
                CloseTelemetryWriter();Robot->ReleaseDriveCommand();bStopped=true;SetActorTickEnabled(false);
            }
            else Stop(true, TEXT("One actual original-car native contact, bounded physical/camera reaction and ordinary same-body cargo recovery observed; no universal traffic/camera or human-key acceptance"));
        }
        return;
    }
    if (!O.Fault.IsEmpty()) { Stop(false, FString::Printf(TEXT("Native fault in %s: %s"), PhaseName(Phase), *O.Fault)); return; }
    if (!O.bKnown) { if (Age > 20) Stop(false, TEXT("Native state remained unknown for twenty seconds")); return; }
    if (Phase == EPhase::Settle && PhaseStartPose.Equals(FTransform::Identity)) { PhaseStartPose = O.ChassisPose; LastPosition = O.ChassisPose.GetLocation(); }
    const double SignedDistance = FVector::DotProduct(O.ChassisPose.GetLocation() - PhaseStartPose.GetLocation(), -PhaseStartPose.GetUnitAxis(EAxis::X));
    const double YawDelta = FMath::FindDeltaAngleDegrees(PhaseStartPose.Rotator().Yaw, O.ChassisPose.Rotator().Yaw);
    const bool FullEmpty = Phase >= EPhase::FullEmptyPosition && Phase <= EPhase::FullEmptyRightBrake;
    const bool FullLoaded = Phase >= EPhase::FullLoadedPosition && Phase <= EPhase::FullLoadedRightBrake;
    if (FullEmpty || FullLoaded)
    {
        const auto& Route = Director->GetMeasuredRoute();
        if (Route.Num() < 21) { Stop(false, TEXT("Full-input trials require the recorded straight corridor to be at least 1000 cm")); return; }
        const FVector Axis = (Route.Last() - Route[0]).GetSafeNormal2D();
        const FVector Delta = O.ChassisPose.GetLocation() - Route[0];
        const double Along = FVector::DotProduct(Delta, Axis);
        const double Lateral = FVector(Delta.X - Axis.X * Along, Delta.Y - Axis.Y * Along, 0).Size();
        if (!O.bUpright || !Robot->AreDeliveryDoorsClosedAndPhysicallyLocked() || Along < -60 || Along > FVector::Dist2D(Route[0], Route.Last()) - 60 || Lateral > 20)
        { Stop(false, TEXT("Full-input trial left its predeclared corridor/tilt/closed-lock containment")); return; }
        if (FullLoaded && (Director->Snapshot().Job.ParcelCount != 2
            || FMath::Abs(Director->Snapshot().Job.PayloadKg - 2.0) > 1.e-6 || !VerifyLoadedInventory(O)))
        { Stop(false, TEXT("Full-input loaded trial lost its two distinct secured one-kilogram parcel bodies: ") + InventoryFailure); return; }
    }
    switch (Phase)
    {
    case EPhase::FullEmptyPosition:
    case EPhase::FullLoadedPosition:
    {
        if (Age > 25) { Stop(false, TEXT("Full-input start pose was not physically reached and supported within 25 seconds")); break; }
        const FVector Target = Director->GetMeasuredRoute()[10];
        const FVector Delta = Target - O.ChassisPose.GetLocation();
        const double Distance = FVector(Delta.X, Delta.Y, 0).Size();
        // Setup anchor tolerance is declared separately from full-command travel.
        // Once reached, hold the brake instead of taper/steer cancellation or chatter.
        if (Distance <= 20.0) bFullSetupArrivalLatched = true;
        if (bFullSetupArrivalLatched)
        {
            Drive(0, 0, true);
            if (O.IsStoppedAndSupported())
            {
                if (Distance > 20.0) { Stop(false, TEXT("Full-input setup brake settled outside the declared twenty-centimetre anchor envelope")); break; }
                Enter(FullLoaded ? EPhase::FullLoadedAlign : EPhase::FullEmptyAlign, O);
            }
        }
        else
        {
            const double Error = FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw, Delta.Rotation().Yaw);
            const float Steer = Distance <= 60.0 && FMath::Abs(Error) <= 3.0 ? 0.f
                : float(FMath::Clamp(Error / 30.0, -1.0, 1.0));
            Drive(FMath::Abs(Error) > 20 ? 0 : float(FMath::Clamp(Distance / 200.0, 0.15, 0.35)), Steer);
        }
        break;
    }
    case EPhase::FullEmptyAlign:
    case EPhase::FullLoadedAlign:
    {
        if (Age > 10) { Stop(false, TEXT("Full-input corridor heading not reached physically within 10 seconds")); break; }
        const auto& Route = Director->GetMeasuredRoute();
        const double Error = FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw, (Route.Last() - Route[0]).Rotation().Yaw);
        if (FMath::Abs(Error) <= 1) bFullSetupArrivalLatched = true;
        if (bFullSetupArrivalLatched)
        {
            Drive(0, 0, true);
            if (O.IsStoppedAndSupported())
            {
                if (FMath::Abs(Error) > 1) { Stop(false, TEXT("Full-input heading brake settled outside the unchanged one-degree setup target")); break; }
                Enter(FullLoaded ? EPhase::FullLoadedForward : (bReverseSteerTest ? EPhase::ReverseSteering : EPhase::FullEmptyForward), O);
            }
        }
        else
        {
            // The standard turn suite already uses this command magnitude. This
            // bounded setup does not alter the player's motor law or full trials.
            const double Magnitude = FMath::Clamp(FMath::Max(FMath::Abs(Error / 30.0), 0.65), 0.0, 1.0);
            Drive(0, float(Error < 0 ? -Magnitude : Magnitude));
        }
        break;
    }
    case EPhase::FullEmptyForward:
    case EPhase::FullLoadedForward:
    case EPhase::FullEmptyReverse:
    case EPhase::FullLoadedReverse:
    {
        const bool Forward = Phase == EPhase::FullEmptyForward || Phase == EPhase::FullLoadedForward;
        Drive(Forward ? 1.f : -1.f, 0);
        if (Age > 10 || FVector::Dist2D(PhaseStartPose.GetLocation(), O.ChassisPose.GetLocation()) > 250)
            Stop(false, TEXT("Full-input 200 cm signed travel/50 cmps/six supported rotating wheels failed its 10 second/250 cm bound"));
        else if ((Forward ? SignedDistance : -SignedDistance) >= 200 && O.VelocityCmps.Size() >= 50 && SupportedWheelMotionMask == 0x3f)
        {
            if (bBoostTest && PhaseBoostDriveFrames == 0)
                Stop(false, TEXT("This individual full-translation phase observed no actual boosted native wheel drive"));
            else Enter(FullLoaded ? (Forward ? EPhase::FullLoadedForwardBrake : EPhase::FullLoadedReverseBrake)
                : (Forward ? EPhase::FullEmptyForwardBrake : EPhase::FullEmptyReverseBrake), O);
        }
        break;
    }
    case EPhase::FullEmptyLeftCenter:
    case EPhase::FullLoadedLeftCenter:
    case EPhase::FullEmptyRightCenter:
    case EPhase::FullLoadedRightCenter:
    {
        if (Age > 25 || FVector::Dist2D(PhaseStartPose.GetLocation(), O.ChassisPose.GetLocation()) > 250)
        { Stop(false, TEXT("Full-steer actual center/heading setup exceeded its twenty-five-second/two-hundred-fifty-centimetre bound")); break; }
        const auto& Route = Director->GetMeasuredRoute();
        const FVector Axis = (Route.Last() - Route[0]).GetSafeNormal2D();
        const FVector Lateral(-Axis.Y, Axis.X, 0);
        const FVector Delta = O.ChassisPose.GetLocation() - Route[0];
        const double Along = FVector::DotProduct(Delta, Axis);
        const double Across = FVector::DotProduct(Delta, Lateral);
        const double Heading = (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
        const double HeadingError = FMath::FindDeltaAngleDegrees(Axis.Rotation().Yaw, Heading);
        if (FMath::Abs(Across) <= 1.0 && FMath::Abs(HeadingError) <= 0.5) bFullSetupArrivalLatched = true;
        if (bFullSetupArrivalLatched)
        {
            Drive(0, 0, true);
            if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
            {
                if (FMath::Abs(Across) > 2.0 || FMath::Abs(HeadingError) > 1.0)
                { bFullSetupArrivalLatched = false; StableSinceWorldS = -1; break; }
                if (StableSinceWorldS < 0) StableSinceWorldS = Now;
                if (Now - StableSinceWorldS >= 0.5)
                {
                    const bool Left = Phase == EPhase::FullEmptyLeftCenter || Phase == EPhase::FullLoadedLeftCenter;
                    Enter(FullLoaded ? (Left ? EPhase::FullLoadedLeftSettle : EPhase::FullLoadedRightSettle)
                        : (Left ? EPhase::FullEmptyLeftSettle : EPhase::FullEmptyRightSettle), O);
                }
            }
            else StableSinceWorldS = -1;
        }
        else
        {
            const double TargetAlong = Along + (bFullCenterReverse ? -80.0 : 80.0);
            if (TargetAlong < 0 || TargetAlong > FVector::Dist2D(Route[0], Route.Last()))
            { Stop(false, TEXT("Full-steer centering lookahead left the measured physical route")); break; }
            const FVector Target = Route[0] + Axis * TargetAlong;
            const double TravelHeading = Heading + (bFullCenterReverse ? 180.0 : 0.0);
            const double Error = FMath::FindDeltaAngleDegrees(TravelHeading, (Target - O.ChassisPose.GetLocation()).Rotation().Yaw);
            Drive(bFullCenterReverse ? -0.35f : 0.35f, float(FMath::Clamp(Error / 15.0, -0.65, 0.65)), false);
        }
        break;
    }
    case EPhase::FullEmptyLeftSettle:
    case EPhase::FullLoadedLeftSettle:
    case EPhase::FullEmptyRightSettle:
    case EPhase::FullLoadedRightSettle:
    {
        Drive(0, 0, true);
        if (Age > 20) { Stop(false, TEXT("Full-steer trial lacked two seconds of supported bounded rest within twenty seconds")); break; }
        if (O.IsStoppedAndSupported())
        {
            if (StableSinceWorldS < 0)
            {
                StableSinceWorldS = Now; FullRestWindowPose = O.ChassisPose;
                FullRestMaxTranslationCm = FullRestMaxYawDegrees = 0;
            }
            FullRestMaxTranslationCm = FMath::Max(FullRestMaxTranslationCm,
                FVector::Distance(FullRestWindowPose.GetLocation(), O.ChassisPose.GetLocation()));
            FullRestMaxYawDegrees = FMath::Max(FullRestMaxYawDegrees,
                FMath::Abs(FMath::FindDeltaAngleDegrees(FullRestWindowPose.Rotator().Yaw, O.ChassisPose.Rotator().Yaw)));
            if (FullRestMaxTranslationCm > 1.0 || FullRestMaxYawDegrees > 1.0)
            {
                StableSinceWorldS = -1;
                break;
            }
            if (Now - StableSinceWorldS >= 2)
            {
                const bool Left = Phase == EPhase::FullEmptyLeftSettle || Phase == EPhase::FullLoadedLeftSettle;
                Enter(FullLoaded ? (Left ? EPhase::FullLoadedLeft : EPhase::FullLoadedRight)
                    : (Left ? EPhase::FullEmptyLeft : EPhase::FullEmptyRight), O);
            }
        }
        else StableSinceWorldS = -1;
        break;
    }
    case EPhase::FullEmptyLeft:
    case EPhase::FullLoadedLeft:
    case EPhase::FullEmptyRight:
    case EPhase::FullLoadedRight:
    {
        const bool Left = Phase == EPhase::FullEmptyLeft || Phase == EPhase::FullLoadedLeft;
        Drive(0, Left ? -1.f : 1.f);
        if (Age > 10) Stop(false, TEXT("Full-steer signed 30 degree turn not observed within 10 seconds"));
        else if ((Left ? -YawDelta : YawDelta) >= 30)
            Enter(FullLoaded ? (Left ? EPhase::FullLoadedLeftBrake : EPhase::FullLoadedRightBrake)
                : (Left ? EPhase::FullEmptyLeftBrake : EPhase::FullEmptyRightBrake), O);
        break;
    }
    case EPhase::FullEmptyForwardBrake:
    case EPhase::FullLoadedForwardBrake:
    case EPhase::FullEmptyReverseBrake:
    case EPhase::FullLoadedReverseBrake:
    case EPhase::FullEmptyLeftBrake:
    case EPhase::FullLoadedLeftBrake:
    case EPhase::FullEmptyRightBrake:
    case EPhase::FullLoadedRightBrake:
        Drive(0, 0, true); BrakeTravelCm += FVector::Distance(LastPosition, O.ChassisPose.GetLocation()); LastPosition = O.ChassisPose.GetLocation();
        if (BrakeTravelCm > 100 || Age > 2) { Stop(false, TEXT("Full-input native brake exceeded unchanged 2 second/100 cm stop bound")); break; }
        if (O.IsStoppedAndSupported())
        {
            if (Phase == EPhase::FullLoadedRightBrake) bFullLoadedTrialsComplete = true;
            const EPhase Next = Phase == EPhase::FullEmptyForwardBrake ? EPhase::FullEmptyReverse
                : Phase == EPhase::FullLoadedForwardBrake ? EPhase::FullLoadedReverse
                : Phase == EPhase::FullEmptyReverseBrake ? EPhase::FullEmptyLeftCenter
                : Phase == EPhase::FullLoadedReverseBrake ? EPhase::FullLoadedLeftCenter
                : Phase == EPhase::FullEmptyLeftBrake ? EPhase::FullEmptyRightCenter
                : Phase == EPhase::FullLoadedLeftBrake ? EPhase::FullLoadedRightCenter
                : FullLoaded ? (!bOperatingTest && !(CarriedRecoveryCountMask & (1 << Director->Snapshot().Job.ParcelCount))
                    ? EPhase::CarriedRecover : EPhase::ToDropoff) : EPhase::Forward;
            Enter(Next, O);
        }
        break;
    case EPhase::Settle:
    case EPhase::RecoverSettle:
        Drive(0, 0, true);
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS >= 2) Enter(Phase == EPhase::Settle ? (bCurbTest ? EPhase::Accept : bDamageTest ? EPhase::DamageEmptyTrial : bRingInteractionTest ? EPhase::RingAccept : (bCargoVideoRequested || bCargoDiagnosticTest) ? EPhase::Accept : (bOperatingTest ? EPhase::OperatingAccept : ((bFullInputTrials || bReverseSteerTest) ? EPhase::FullEmptyPosition : EPhase::Forward))) : EPhase::DoorOpen, O);
        }
        else StableSinceWorldS = -1;
        if (Age > 20) Stop(false, TEXT("Two-second six-wheel stable supported baseline not observed within twenty seconds"));
        break;
    case EPhase::Forward:
    case EPhase::Reverse:
        Drive(Phase == EPhase::Forward ? 0.5f : -0.5f, 0);
        if ((Phase == EPhase::Forward ? SignedDistance : -SignedDistance) >= 100 && O.VelocityCmps.Size() >= 50
            && SupportedWheelMotionMask == 0x3f)
            Enter(Phase == EPhase::Forward ? EPhase::ForwardBrake : EPhase::ReverseBrake, O);
        else if (Age > 12) Stop(false, TEXT("100 cm travel, 50 cm/s speed and all six wheels rotating under actual support not observed"));
        break;
    case EPhase::ForwardBrake:
    case EPhase::ReverseBrake:
    case EPhase::LeftBrake:
    case EPhase::RightBrake:
        Drive(0, 0, true); BrakeTravelCm += FVector::Distance(LastPosition, O.ChassisPose.GetLocation()); LastPosition = O.ChassisPose.GetLocation();
        if (BrakeTravelCm > 100 || Age > 2) { Stop(false, TEXT("Native brake exceeded 2 seconds or 100 cm before supported stop")); break; }
        if (O.IsStoppedAndSupported())
        {
            const EPhase Next = Phase == EPhase::ForwardBrake ? EPhase::Reverse : Phase == EPhase::ReverseBrake ? EPhase::LeftTurn
                : Phase == EPhase::LeftBrake ? EPhase::RightTurn : EPhase::Recover;
            Enter(Next, O);
        }
        break;
    case EPhase::LeftTurn:
    case EPhase::RightTurn:
        Drive(0, Phase == EPhase::LeftTurn ? -0.65f : 0.65f);
        if ((Phase == EPhase::LeftTurn ? -YawDelta : YawDelta) >= 30)
            Enter(Phase == EPhase::LeftTurn ? EPhase::LeftBrake : EPhase::RightBrake, O);
        else if (Age > 10) Stop(false, TEXT("Requested signed 30 degree physical turn not observed"));
        break;
    case EPhase::DoorOpen:
        Drive(0, 0, true); Robot->RequestDeliveryDoorState(true);
        if (O.bDoorsOpen && O.IsStoppedAndSupported()) Enter(bFullInputTrials ? EPhase::DoorDriveProbe : bVisualCaptureRequested ? EPhase::VisualOpen : EPhase::DoorClose, O);
        else if (Age > 7) Stop(false, TEXT("Measured physical open endpoints absent after bounded request"));
        break;
    case EPhase::DoorDriveProbe:
        Drive(1, 0, false);
        if (!O.bDoorsOpen || !O.IsStoppedAndSupported() || FVector::Dist2D(PhaseStartPose.GetLocation(), O.ChassisPose.GetLocation()) > 2
            || !ReadZeroWheelMotorEvidence()) Stop(false, TEXT("Open-lid full-throttle probe produced wheel motor or lost stopped/open containment"));
        else if (Age >= 0.75) Enter(bVisualCaptureRequested ? EPhase::VisualOpen : EPhase::DoorClose, O);
        break;
    case EPhase::DoorClose:
        if (Age > 7) { Stop(false, TEXT("Measured physical closed locks absent after bounded request")); break; }
        if (!bDoorCloseRequested)
        {
            Drive(0, 0, true);
            if (Robot->RequestDeliveryDoorState(false))
            {
                bDoorCloseRequested = true;
                // The real door transition clears held input. Perform the
                // production neutral-release handshake AFTER that transition.
                Drive(0, 0, true);
            }
            break;
        }
        if (O.bDoorsClosedLatched && O.IsStoppedAndSupported())
        {
            Drive(0, 0, true);
            if (bFullInputTrials && DoorClosingProbeSamples == 0) Stop(false, TEXT("Closing interlock probe lacked an actual nonlatched sample"));
            else Enter(bVisualCaptureRequested ? EPhase::VisualClosed : EPhase::ChargeSettle, O);
            break;
        }
        if (bFullInputTrials)
        {
            // Judge an accepted full-throttle command only after it has lived
            // across a native frame while the actual lids remain nonlatched.
            if (bDoorClosingProbePending && GFrameCounter > DoorClosingProbeIssuedFrame)
            {
                if (!O.IsStoppedAndSupported() || !ReadZeroWheelMotorEvidence())
                { Stop(false, TEXT("Closing-lid accepted full-throttle probe produced motor or lost stopped containment")); break; }
                ++DoorClosingProbeSamples;
            }
            Drive(1, 0, false);
            if (!bLastCommandAccepted) { Stop(false, TEXT("Closing-lid full command refused after neutral-release setup")); break; }
            bDoorClosingProbePending = true; DoorClosingProbeIssuedFrame = GFrameCounter;
        }
        else Drive(0, 0, true);
        break;
    case EPhase::Recover:
        if (!Robot->ResetDeliveryRobot()) Stop(false, FString::Printf(TEXT("Native clearance-checked reconstruction failed: %s"), *Robot->Fault));
        else Enter(EPhase::RecoverSettle, O);
        break;
    case EPhase::VisualOpen:
    case EPhase::VisualClosed:
    {
        Drive(0, 0, true);
        const bool bOpenView = Phase == EPhase::VisualOpen;
        const bool Eligible = O.IsStoppedAndSupported() && (bOpenView ? O.bDoorsOpen : O.bDoorsClosedLatched);
        if (Age > 47) { Stop(false, TEXT("Rendered five-view capture exceeded its bounded native phase")); break; }
        if (!Eligible)
        {
            StableSinceWorldS = -1;
            if (VisualCapture) Stop(false, TEXT("Native support or physical lid endpoint was lost during rendered capture"));
            else if (Age > 5) Stop(false, TEXT("Stable native lid/support setup for rendered capture absent within five seconds"));
            break;
        }
        if (!VisualCapture)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS < 0.5 || FScreenshotRequest::IsScreenshotRequested()) break;
            VisualCapture = GetWorld()->SpawnActor<AMAYADeliveryVisualCapture>();
            if (!VisualCapture || !VisualCapture->StartCapture(Robot, Cast<APlayerController>(Robot->GetController()),
                bOpenView ? TEXT("native-open-lids") : TEXT("native-closed-lids")))
            { Stop(false, TEXT("Explicit rendered all-angle capture could not start in the actual viewport")); break; }
        }
        if (VisualCapture->IsComplete())
        {
            auto Receipt = MakeShared<FJsonObject>();
            Receipt->SetStringField(TEXT("label"), bOpenView ? TEXT("native-open-lids") : TEXT("native-closed-lids"));
            Receipt->SetStringField(TEXT("manifestPath"), VisualCapture->GetManifestPath());
            Receipt->SetStringField(TEXT("captureStatus"), VisualCapture->GetCaptureStatus());
            const bool Saved = VisualCapture->DidSaveAllViews();
            Receipt->SetBoolField(TEXT("allViewsSavedAndViewRestored"), Saved);
            Receipt->SetStringField(TEXT("visualAcceptance"), TEXT("not_assessed"));
            VisualCaptureReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
            VisualCapture->Destroy(); VisualCapture = nullptr;
            if (!Saved) Stop(false, TEXT("Rendered all-angle capture did not save every view and restore its original view target"));
            else Enter(bOpenView ? EPhase::DoorClose : EPhase::ChargeSettle, O);
        }
        break;
    }
    case EPhase::ChargeSettle:
        Drive(0, 0, true);
        if (Age > 5) { Stop(false, TEXT("Continuous half-second supported closed-latch charging setup absent within five seconds")); break; }
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS >= 0.5) Enter(EPhase::Charge, O);
        }
        else StableSinceWorldS = -1;
        break;
    case EPhase::Charge:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 5) { Stop(false, TEXT("Unfunded startup observation did not remain safe within five seconds")); break; }
        if (S.TotalCredits != 0 || S.LastRecharge.Generation != 0 || S.bCharging || S.BatteryPercent > ChargeStartPercent)
        { Stop(false, TEXT("Initial zero-cash stationary ring granted energy or created a fee")); break; }
        if (Age >= 1 && O.IsStoppedAndSupported() && O.bDoorsClosedLatched && S.ChargeVisit.bInside)
        { bInitialUnfundedNoGainObserved = true; Enter(EPhase::Accept, O); }
        break;
    }
    case EPhase::PaidChargeReturn:
    {
        const auto S = Director->Snapshot();
        if (Age > 120) { Stop(false, TEXT("Paid-ring return exceeded existing bounded corridor travel setup")); break; }
        if (S.Phase != EMAYADeliveryPhase::Available || S.TotalCredits != PaidChargeCashBefore
            || S.CompletedJobs != 1 || S.ShiftInstanceId != PaidChargeShiftId || Director->GetParcelCount() != 0)
        { Stop(false, TEXT("Actual paid-charge return changed job/cash/shift/cargo before ring admission")); break; }
        const double Distance = FVector::Dist2D(O.ChassisPose.GetLocation(), PaidChargeStationCm);
        const bool InActualChargingRing = S.ChargeVisit.bInside && S.ChargeVisit.StationId == PaidChargeStationId
            && FMath::IsFinite(Distance) && Distance <= AMAYADeliveryStation::ChargingRadiusCm;
        if (bStationArrivalLatched && !InActualChargingRing)
        { bStationArrivalLatched = false; StableSinceWorldS = -1; ArrivalLatchWorldS = -1; ++ArrivalLatchExitCount; }
        const bool JustLatched = !bStationArrivalLatched && InActualChargingRing;
        if (JustLatched) { bStationArrivalLatched = true; ArrivalLatchWorldS = Now; }
        if (!bStationArrivalLatched) { NavigateTo(PaidChargeStationCm, O); break; }
        Drive(0, 0, true);
        if (JustLatched) Record(O, true, *FString::Printf(TEXT("paid_charge_parking_brake_requested: actual same-station observed charging ring entry; station=%s distance=%.9fcm radius=%.9fcm; half-second native closed/support setup and full production ten-second dwell still pending"),
            *PaidChargeStationId, Distance, AMAYADeliveryStation::ChargingRadiusCm));
        if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched) { StableSinceWorldS = -1; break; }
        if (StableSinceWorldS < 0) StableSinceWorldS = Now;
        if (Now - StableSinceWorldS >= .5) Enter(EPhase::PaidChargeWait, O);
        break;
    }
    case EPhase::PaidChargeWait:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 20) { Stop(false, TEXT("Ten-second automatic paid ring did not produce bounded actual energy gain")); break; }
        if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched || S.Phase != EMAYADeliveryPhase::Available
            || S.ShiftInstanceId != PaidChargeShiftId || Director->GetParcelCount() != 0
            || S.LastRecharge.Generation > PaidChargeReceiptBefore + 1)
        { Stop(false, TEXT("Paid automatic ring lost actual safety/identity or billed more than once")); break; }
        if (S.LastRecharge.Generation == PaidChargeReceiptBefore) break;
        const auto& Fee = S.LastRecharge;
        if (Fee.Generation != PaidChargeReceiptBefore + 1 || Fee.StationId != PaidChargeStationId
            || Fee.ShiftInstanceId != PaidChargeShiftId || Fee.FeeDollars != 2
            || Fee.CashBefore != PaidChargeCashBefore || Fee.CashAfter != PaidChargeCashBefore - 2
            || S.TotalCredits != PaidChargeCashBefore - 2 || Fee.BatteryAfter <= Fee.BatteryBefore
            || !S.ChargeVisit.bPaid || S.ChargeVisit.ElapsedSeconds != 10)
        { Stop(false, TEXT("Actual first-positive gain did not atomically match one2-dollar fee after ten-second dwell")); break; }
        const double ElapsedPaid = Now - Fee.WorldSeconds;
        if (ElapsedPaid < .25) break;
        const double Gain = S.BatteryPercent - Fee.BatteryAfter;
        const double Rate = ElapsedPaid > 0 ? Gain / ElapsedPaid : 0;
        if (Rate < 4.75 || Rate > 5.25 || !S.bCharging)
        { Stop(false, TEXT("Observed paid native energy gain did not match five percentage points per gameplay second")); break; }
        auto Receipt = MAYADeliveryEconomyToJson(S);
        Receipt->SetStringField(TEXT("nativeScope"), TEXT("Actual completed first mission earned funds; physical return to surveyed90cm ring; no connect/C call; one fee and measured post-fee gain"));
        Receipt->SetNumberField(TEXT("measuredPaidRatePercentPerSecond"), Rate);
        Receipt->SetNumberField(TEXT("actualStationDistanceCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), PaidChargeStationCm));
        PaidChargeReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
        bPaidRingChargeObserved = true; Enter(EPhase::PaidChargeDeparture, O); break;
    }
    case EPhase::PaidChargeDeparture:
    {
        const auto S = Director->Snapshot();
        if (Age > 5) { Stop(false, TEXT("Fresh accepted native movement intent did not end paid charging within five seconds")); break; }
        Drive(.35f, 0, false);
        if (!S.bCharging && S.ChargeVisit.bPaid && S.ChargeVisit.bConsumed && O.bFreshMovementIntent)
        {
            if (S.TotalCredits != PaidChargeCashBefore - 2 || S.LastRecharge.Generation != PaidChargeReceiptBefore + 1)
            { Stop(false, TEXT("Fresh movement departure changed fee/cash again")); break; }
            auto Receipt = MAYADeliveryEconomyToJson(S);
            Receipt->SetBoolField(TEXT("actualFreshMovementIntent"), O.bFreshMovementIntent);
            Receipt->SetStringField(TEXT("nativeScope"), TEXT("Actual accepted automation movement intent ended charger authority before normal subsequent drive; no keyboard/focus claim"));
            PaidChargeReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
            bPaidMovementStopObserved = true; Enter(EPhase::PaidChargeStop, O);
        }
        break;
    }
    case EPhase::PaidChargeStop:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 10) { Stop(false, TEXT("Actual post-charge neutral supported latch setup did not settle within ten seconds")); break; }
        if (S.bCharging || S.TotalCredits != PaidChargeCashBefore - 2 || S.LastRecharge.Generation != PaidChargeReceiptBefore + 1)
        { Stop(false, TEXT("Same paid visit restarted/billed while neutral controls were held")); break; }
        if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched) { StableSinceWorldS = -1; break; }
        if (StableSinceWorldS < 0) StableSinceWorldS = Now;
        if (Now - StableSinceWorldS >= .5) Enter(EPhase::Accept, O);
        break;
    }
    case EPhase::Accept:
        Drive(0, 0, true);
        if (!Director->RequestAccept()) Stop(false, TEXT("Delivery accept request refused"));
        else if (Director->RequestAccept()) Stop(false, TEXT("Duplicate accept unexpectedly admitted an existing active job"));
        else if (bOperatingTest && (!bOperatingRechargeObserved || Director->Snapshot().JobInstanceId == OperatingFailedJobId))
            Stop(false, TEXT("Fresh post-charge accept lacked a new active identity after retained-zero recovery"));
        else Enter(EPhase::ToPickup, O);
        break;
    case EPhase::ToPickup:
    case EPhase::ToDropoff:
    {
        const bool bPickup = Phase == EPhase::ToPickup;
        const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
        AMAYADeliveryStation* Station = Director->FindStation(bPickup ? S.Job.PickupStationId : S.Job.DropoffStationId);
        if (!Station) { Stop(false, TEXT("Actual active delivery station absent")); break; }
        if (Age > 120) { Stop(false, TEXT("Physical route navigation or ring service setup exceeded 120 second bound")); break; }
        ServiceStationCm = Station->GetServicePoint(); ServiceStationId = Station->StationId;
        if (!FindServiceApproach(ServiceStationCm, ServiceApproachCm))
        { Stop(false, TEXT("No measured route approach exists within the unchanged delivery service radius")); break; }

        // Director ticks first. An automatic service may already be admitted on this
        // post-physics frame; consume that actual transition without a second request.
        const EMAYADeliveryPhase ServicePhase = bPickup ? EMAYADeliveryPhase::Loading : EMAYADeliveryPhase::Unloading;
        if (S.Phase == ServicePhase)
        {
            if (!R.LastAutomaticRequestAccepted || R.AutomaticTriggerGeneration == 0
                || R.LastAutomaticServiceGeneration != S.Service.Generation
                || R.LastAutomaticStationId != ServiceStationId
                || S.Service.StationId != ServiceStationId || S.Service.JobInstanceId != S.JobInstanceId)
            { Stop(false, TEXT("Observed service transition lacks matching actual automatic ring admission provenance")); break; }
            if (bAwaitAutomaticRingPickup)
            {
                if (StationInteractionCalls != AutomaticWaitEntryInteractionCalls || AutomaticWaitFrames == 0
                    || !R.LastRequestAccepted || R.LastRequestOrigin != TEXT("AutomaticRing")
                    || R.LastRequestServiceGeneration != S.Service.Generation
                    || R.LastRequestElapsedGameplaySeconds < 10.0)
                { Stop(false, TEXT("First automatic pickup did not preserve the ten-second no-interaction countdown")); break; }
                bAutomaticRingPickupObserved = true;
            }
            else bUnexpectedAutomaticServiceObserved = true;
            ++AutomaticRingAdmissionCount;
            SaveRingServiceReceipt(O, false);
            Enter(bPickup ? EPhase::Loading : EPhase::Unloading, O); break;
        }
        if (S.Phase != (bPickup ? EMAYADeliveryPhase::Accepted : EMAYADeliveryPhase::Carrying))
        { Stop(false, TEXT("Station setup observed an unexpected actual mission phase")); break; }

        const double StationDistance = FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm);
        const double ApproachDistance = FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceApproachCm);
        // Reach at the original 65 cm threshold, then hold the brake. Tiny motion
        // across 65 cm cannot reintroduce motor pulses while native support settles.
        if (bStationArrivalLatched && StationDistance > AMAYADeliveryStation::ObjectiveRingRadiusCm)
        { bStationArrivalLatched = false; StableSinceWorldS = -1; ArrivalLatchWorldS = -1; ++ArrivalLatchExitCount; }
        if (!bStationArrivalLatched && ApproachDistance <= 65.0
            && StationDistance <= AMAYADeliveryStation::ObjectiveRingRadiusCm)
        { bStationArrivalLatched = true; ArrivalLatchWorldS = Now; }
        bool bSettledAtStation = false;
        if (bStationArrivalLatched)
        {
            Drive(0, 0, true);
            bSettledAtStation = O.IsStoppedAndSupported() && O.bDoorsClosedLatched
                && StationDistance <= FMAYADeliveryBalance().ServiceRadiusCm;
        }
        else { NavigateTo(ServiceApproachCm, O); StableSinceWorldS = -1; }

        if (bAwaitAutomaticRingPickup)
        {
            ++AutomaticWaitFrames; // No RequestInteract call exists on this branch.
        }
        if (!bRingViewRequested && R.bInside && !FScreenshotRequest::IsScreenshotRequested()
            && (bAwaitAutomaticRingPickup ? R.ElapsedGameplaySeconds >= 2.0 : bStationArrivalLatched))
        {
            const FString Path = FPaths::Combine(RunDirectory, FString::Printf(TEXT("ring-%s-%02d-before-service.png"),
                bPickup ? TEXT("pickup-countdown") : TEXT("dropoff"), RingViewReceipts.Num()));
            FScreenshotRequest::RequestScreenshot(Path, true, false);
            auto View = MakeShared<FJsonObject>();
            View->SetStringField(TEXT("path"), Path); View->SetStringField(TEXT("stationId"), ServiceStationId);
            View->SetNumberField(TEXT("requestedWorldSeconds"), Now);
            View->SetStringField(TEXT("view"), TEXT("Actual current gameplay camera and HUD before service; no camera reposition"));
            View->SetObjectField(TEXT("ringAtRequest"), MayaV01RingObservationJson(R));
            RingViewReceipts.Add(MakeShared<FJsonValueObject>(View)); bRingViewRequested = true;
        }
        if (bSettledAtStation)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (bAwaitAutomaticRingPickup) break;
            if (Now - StableSinceWorldS < 0.5 || !Robot->CanRequestDeliveryDoorState(true)) break;
            if (!R.bInside || R.ActiveStationId != ServiceStationId || R.ElapsedGameplaySeconds >= 10.0)
            { Stop(false, TEXT("Explicit native station interaction did not occur inside the ten-second ring window")); break; }
            const uint64 RequestBefore = R.RequestGeneration;
            ++StationInteractionCalls;
            // One explicit native API interaction. Actual keyboard/focus is separate QA.
            if (!Director->RequestInteract())
            { Stop(false, TEXT("Single explicit native service request was refused after measured station settlement")); break; }
            const auto After = Director->Snapshot(); const auto& Request = Director->GetRingObservation();
            if (After.Phase != ServicePhase || !Request.LastRequestAccepted
                || Request.LastRequestOrigin != TEXT("ExplicitInteraction")
                || Request.RequestGeneration <= RequestBefore
                || Request.LastRequestServiceGeneration != After.Service.Generation
                || Request.LastRequestElapsedGameplaySeconds >= 10.0)
            { Stop(false, TEXT("Explicit interaction lacked matching accepted pre-countdown service provenance")); break; }
            ++ExplicitRingAdmissionCount;
            SaveRingServiceReceipt(O, true);
            Enter(bPickup ? EPhase::Loading : EPhase::Unloading, O);
        }
        else StableSinceWorldS = -1;
        break;
    }
    case EPhase::Loading:
    case EPhase::Unloading:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if ((bCashShiftTest || bTrafficEncounterTest) && Phase == EPhase::Loading) ObserveCashOpening(O);
        if (bCancelOpeningTest && Phase == EPhase::Loading)
        {
            if (Age > 10 || S.Phase == EMAYADeliveryPhase::Failed || !Director->GetLastServiceFailureDetail().IsEmpty())
            { Stop(false, TEXT("Opening cancellation regression failed before its expected native boundary")); break; }
            if (bOpeningCancelRetried && S.Phase == EMAYADeliveryPhase::Carrying && O.bDoorsClosedLatched)
            {
                const bool Valid = bOpeningCancelTriggered && bCancelledOpenObserved && O.IsStoppedAndSupported()
                    && S.JobInstanceId == OpeningCancelJobId && S.TotalCredits == OpeningCancelCredits && !S.bCharging
                    && S.CargoConditionPercent == OpeningCancelConditionPercent && VerifyLoadedInventory(O, true);
                Stop(Valid, Valid ? TEXT("Explicit native Opening cancellation reached real open and closed endpoints, preserved the unrewarded job/empty custody, and one real retry secured the actual parcel")
                    : TEXT("Opening cancellation retry did not preserve the original job/reward/condition and actual native cargo custody"));
                break;
            }
            if (!bOpeningCancelTriggered && !O.bDoorsOpen && !O.bDoorsClosedLatched
                && FMath::Abs(O.DoorAngleDegrees[0]) >= 3.0 && FMath::Abs(O.DoorAngleDegrees[1]) >= 3.0
                && FMath::Abs(O.DoorAngleDegrees[0]) < 60.0 && FMath::Abs(O.DoorAngleDegrees[1]) < 60.0)
            {
                if (!O.IsStoppedAndSupported() || S.Phase != EMAYADeliveryPhase::Loading || Director->GetParcelCount() != 0)
                { Stop(false, TEXT("Explicit Opening cancellation trigger lost actual stopped support or empty loading inventory")); break; }
                OpeningCancelJobId = S.JobInstanceId; OpeningCancelServiceGeneration = S.Service.Generation;
                OpeningCancelResetGeneration = O.ResetGeneration;
                OpeningCancelCredits = S.TotalCredits; OpeningCancelBatteryPercent = S.BatteryPercent;
                OpeningCancelConditionPercent = S.CargoConditionPercent;
                auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), TEXT("explicit_test_only_opening_cancellation"));
                J->SetNumberField(TEXT("worldSeconds"), Now); J->SetStringField(TEXT("jobInstanceId"), OpeningCancelJobId);
                J->SetStringField(TEXT("serviceGeneration"), LexToString(OpeningCancelServiceGeneration));
                J->SetStringField(TEXT("nativeBefore"), Robot->ReadTelemetryJson());
                if (!Director->RequestNativeOpeningCancellationForTest())
                { Stop(false, TEXT("Strict actual native Opening cancellation regression entry refused")); break; }
                J->SetStringField(TEXT("actualCancellation"), Director->ReadServiceCancellationEvidenceJson());
                J->SetStringField(TEXT("originalFailureDetail"), Director->GetLastServiceFailureDetail());
                J->SetStringField(TEXT("scope"), TEXT("Explicit test interruption; no fabricated movement/support/fault and no claim of reproducing the original physical unsafe excursion"));
                OpeningCancellationReceipts.Add(MakeShared<FJsonValueObject>(J)); bOpeningCancelTriggered = true;
                Enter(EPhase::CancelOpeningAwaitClosure, O); break;
            }
            break;
        }
        if (bRingInteractionTest)
        {
            if (Age > 20 || S.Phase == EMAYADeliveryPhase::Failed
                || S.Phase == EMAYADeliveryPhase::Accepted || !Director->GetLastServiceFailureDetail().IsEmpty())
            { Stop(false, TEXT("Ring test actual automatic cargo service failed, cancelled or exceeded twenty seconds")); break; }
            if (S.Phase == EMAYADeliveryPhase::Carrying && O.bDoorsClosedLatched)
            {
                if (!bRingEarlyInteractionRejected || !bRingExitResetObserved || !bRingReentryResetObserved
                    || !bRingCountdownPauseComplete || !bAutomaticRingPickupObserved || StationInteractionCalls != 1
                    || !O.IsStoppedAndSupported() || S.TotalCredits != RingTestCreditsBefore || !VerifyLoadedInventory(O, true))
                { Stop(false, TEXT("Ring coverage or individual native instant-cargo restraint is incomplete: ") + InventoryFailure); break; }
                Stop(true, TEXT("Actual unsafe interaction refusal, physical ring exit/reentry reset, paused gameplay countdown and automatic ten-second pickup completed; keyboard E and full delivery loop are separate QA"));
            }
            break;
        }
if (bAirLoweringTest && Phase == EPhase::Loading && !bAirLoweringCancelTriggered)
        {
            if (TryStartAirLoweringPause(O)) break;
            if (bAirLoweringPauseComplete && Director->IsCargoAirLowering()
                && Director->GetCargoAirPhaseElapsedSeconds() >= 0.8 && O.IsStoppedAndSupported() && O.bDoorsOpen)
            {
                OpeningCancelJobId = S.JobInstanceId; OpeningCancelServiceGeneration = S.Service.Generation;
                OpeningCancelResetGeneration = O.ResetGeneration; OpeningCancelCredits = S.TotalCredits;
                OpeningCancelBatteryPercent = S.BatteryPercent; OpeningCancelConditionPercent = S.CargoConditionPercent;
                auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), TEXT("explicit_test_only_mid_descent_cancellation"));
                J->SetNumberField(TEXT("worldSeconds"), Now); J->SetStringField(TEXT("inventoryBefore"), Director->ReadCargoInventoryEvidenceJson());
                if (!Director->CancelAirLoweringForNativeTest())
                { Stop(false, TEXT("Actual mid-descent test cancellation entry refused")); break; }
                J->SetStringField(TEXT("actualCancellation"), Director->ReadServiceCancellationEvidenceJson());
                J->SetStringField(TEXT("scope"), TEXT("Explicit test interruption after real world pause/resume; no invented unsafe contact, custody, condition or reward"));
                AirLoweringRegressionReceipts.Add(MakeShared<FJsonValueObject>(J));
                bAirLoweringCancelTriggered = true; bOpeningCancelTriggered = true;
                Enter(EPhase::CancelOpeningAwaitClosure, O); break;
            }
        }

        if (bAirLoweringTest && Phase == EPhase::Unloading && !bAirUnloadCancelTriggered)
        {
            if (TryStartAirUnloadingPause(O)) break;
            if (bAirUnloadPauseComplete && Director->IsCargoAirFadingOut()
                && Director->GetCargoAirPhaseElapsedSeconds() >= 0.6 && O.IsStoppedAndSupported() && O.bDoorsOpen)
            {
                OpeningCancelJobId = S.JobInstanceId; OpeningCancelServiceGeneration = S.Service.Generation;
                OpeningCancelResetGeneration = O.ResetGeneration; OpeningCancelCredits = S.TotalCredits;
                OpeningCancelBatteryPercent = S.BatteryPercent; OpeningCancelConditionPercent = S.CargoConditionPercent;
                AirUnloadActors.Reset(); AirUnloadBodies.Reset(); AirUnloadIds.Reset(); AirUnloadMasses.Reset();
                for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
                {
                    auto* P = Director->GetParcel(Slot); double Kg = 0;
                    if (!P || !P->ObserveMass(Kg)) { Stop(false, TEXT("Raised original parcel identity/mass unavailable before cancellation")); break; }
                    AirUnloadActors.Add(P); AirUnloadBodies.Add(P->GetParcelBody()); AirUnloadIds.Add(P->GetParcelId()); AirUnloadMasses.Add(Kg);
                }
                if (bStopped) break;
                auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), TEXT("explicit_test_only_upper_endpoint_partial_fade_cancellation"));
                J->SetNumberField(TEXT("worldSeconds"), Now); J->SetStringField(TEXT("inventoryBefore"), Director->ReadCargoInventoryEvidenceJson());
                if (!Director->CancelAirUnloadingForNativeTest())
                { Stop(false, TEXT("Actual raised-parcel fade cancellation entry refused")); break; }
                J->SetStringField(TEXT("actualCancellation"), Director->ReadServiceCancellationEvidenceJson());
                J->SetStringField(TEXT("scope"), TEXT("Real upward motion/native endpoint and actual world fade pause; explicit cancellation requires same-body opaque Carrying restoration before one retry; no synthetic unsafe contact/reward"));
                AirLoweringRegressionReceipts.Add(MakeShared<FJsonValueObject>(J));
                bAirUnloadCancelTriggered = true; bAirUnloadCancellationInProgress = true;
                Enter(EPhase::CancelOpeningAwaitClosure, O); break;
            }
        }

        if (bOperatingTest && Phase == EPhase::Loading && TryStartNativeLidPause(O)) break;
        if (((Phase == EPhase::Loading && S.Phase == EMAYADeliveryPhase::Accepted)
            || (Phase == EPhase::Unloading && S.Phase == EMAYADeliveryPhase::Carrying)) && O.bDoorsClosedLatched)
        {
            if (Phase == EPhase::Loading && BeginCashOpeningRetry(O)) break;
            Stop(false, FString::Printf(TEXT("Actual mechanical service cancelled safely: %s"), *Director->GetLastServiceFailureDetail())); break;
        }
        if (Phase == EPhase::Loading && O.bDoorsOpen && O.IsStoppedAndSupported() && !bBlockedPathChecked
            && Director->GetLastServiceFailureDetail().IsEmpty() && Director->GetParcelCount() > 0)
        {
            auto* P = Director->GetParcel(0); FTransform InvalidTarget;
            const FString Before = ReadParcelIdentityPoseJson();
            const auto BeforeState = Director->Snapshot();
            if (!P || !P->GetParcelBody() || Robot->GetDeliveryCargoMount(2)
                || Robot->GetDeliveryCargoPlacementTarget(P->GetParcelBody(), 2, InvalidTarget)
                || Robot->AdmitDeliveryCargoPlacement(P->GetParcelBody(), 2)
                || ReadParcelIdentityPoseJson() != Before
                || Director->Snapshot().Phase != BeforeState.Phase
                || Director->Snapshot().Service.Generation != BeforeState.Service.Generation
                || Director->Snapshot().TotalCredits != BeforeState.TotalCredits)
            { Stop(false, TEXT("Invalid physical slot endpoint query mutated actual cargo/custody/reward or was admitted")); break; }
            auto Negative = MakeShared<FJsonObject>(); Negative->SetStringField(TEXT("phase"), TEXT("InvalidPhysicalCargoSlot"));
            Negative->SetBoolField(TEXT("pass"), true); Negative->SetNumberField(TEXT("worldSeconds"), Now);
            Negative->SetNumberField(TEXT("invalidSlot"), 2);
            Negative->SetStringField(TEXT("scope"), TEXT("No third physical mount/target; actual stopped open one/two-parcel inventory unchanged. Historical ground-to-bay floor-transfer test is superseded by air pickup and reversible unload service."));
            Negative->SetStringField(TEXT("nativeReason"), Robot->ReadDeliveryCargoTransferStatus());
            Negative->SetArrayField(TEXT("parcelInventory"), ReadParcelInventoryJson());
            PhaseResults.Add(MakeShared<FJsonValueObject>(Negative)); bBlockedPathChecked = true;
        }
        if (S.Phase == EMAYADeliveryPhase::Failed) { Stop(false, FString::Printf(TEXT("Delivery service failed: %s; native: %s"), *S.Status, *Robot->ReadDeliveryCargoTransferStatus())); break; }
        if (Phase == EPhase::Loading && S.Phase == EMAYADeliveryPhase::Carrying && O.bDoorsClosedLatched)
        {
            const int32 CreditsBefore = S.TotalCredits; const FString InventoryBefore = ReadParcelIdentityPoseJson();
            if (Director->RequestInteract() || Director->RequestNext() || Director->Snapshot().TotalCredits != CreditsBefore
                || ReadParcelIdentityPoseJson() != InventoryBefore)
                Stop(false, TEXT("Wrong-station service/next-job request altered actual individual cargo or reward"));
            else if (!VerifyLoadedInventory(O, true)) Stop(false, InventoryFailure);
            else
            {
                if ((bCashShiftTest || bTrafficEncounterTest) && bCashNaturalRetryUsed && !bCashNaturalRetrySecured)
                {
                    if (!bCashNaturalRetryAdmitted || !CashOpeningIdentityUnchanged(O)
                        || !FMath::IsFinite(S.BatteryPercent) || S.BatteryPercent > CashNaturalClosureBatteryPercent + 1e-6)
                    { Stop(false, TEXT("Natural Opening retry secured cargo without preserving the original authority")); break; }
                    bCashNaturalRetrySecured = true;
                    auto J = MAYADeliveryEconomyToJson(S); J->SetStringField(TEXT("event"), TEXT("normal_retry_same_job_native_cargo_secured"));
                    J->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId); J->SetStringField(TEXT("retryServiceGeneration"), LexToString(CashNaturalRetryServiceGeneration));
                    J->SetStringField(TEXT("actualInventory"), Director->ReadCargoInventoryEvidenceJson()); J->SetNumberField(TEXT("worldSeconds"), Now);
                    CashNaturalOpeningRetryReceipts.Add(MakeShared<FJsonValueObject>(J));
                }
                Enter(bCurbTest ? EPhase::CurbTrial : bDamageTest ? EPhase::DamageLoadedSettle : bCargoVideoRequested ? EPhase::ToDropoff : EPhase::LoadedDrive, O);
            }
        }
        else if (Phase == EPhase::Unloading && S.Phase == EMAYADeliveryPhase::Succeeded && O.bDoorsClosedLatched)
        {
            int32 LiveParcels = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++LiveParcels;
            if (Director->GetParcelCount() != 0 || Director->GetSecuredParcelCount() != 0 || LiveParcels != 0)
                Stop(false, TEXT("Successful closed-lid delivery retained actual removed parcel actors or secured mass"));
            else { ++MissionCount; Enter(EPhase::NextJob, O); }
        }
        else if (Age > 20) Stop(false, TEXT("Native transactional service exceeded 20 second bound"));
        break;
    }
    case EPhase::CashOpeningRetry:
        TickCashOpeningRetry(O, Age, Now); break;
    case EPhase::CancelOpeningAwaitClosure:
    case EPhase::CancelOpeningRetry:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 20) { Stop(false, TEXT("Opening cancellation measured closure/retry setup exceeded twenty gameplay seconds")); break; }
        int32 LiveParcelActors = 0;
        for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++LiveParcelActors;
        const bool AirCleanupPending = bAirLoweringTest && bAirLoweringCancelTriggered
            && Phase == EPhase::CancelOpeningAwaitClosure && S.Phase == EMAYADeliveryPhase::Loading;
        bool CargoPendingValid = AirCleanupPending
            ? Director->GetParcelCount() <= S.Job.ParcelCount && LiveParcelActors == Director->GetParcelCount()
            : Director->GetParcelCount() == 0 && LiveParcelActors == 0;
        if (bAirUnloadCancellationInProgress)
        {
            CargoPendingValid = Director->GetParcelCount() == S.Job.ParcelCount
                && LiveParcelActors == S.Job.ParcelCount && AirUnloadActors.Num() == S.Job.ParcelCount;
            for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
            {
                auto* P = Director->GetParcel(Slot); double Kg = 0;
                CargoPendingValid &= P && AirUnloadActors.IsValidIndex(Slot) && AirUnloadBodies.IsValidIndex(Slot)
                    && AirUnloadIds.IsValidIndex(Slot) && AirUnloadMasses.IsValidIndex(Slot)
                    && P == AirUnloadActors[Slot].Get() && P->GetParcelBody() == AirUnloadBodies[Slot].Get()
                    && P->GetParcelId() == AirUnloadIds[Slot] && P->ObserveMass(Kg)
                    && FMath::Abs(Kg - AirUnloadMasses[Slot]) <= 0.05;
            }
        }
        if (S.JobInstanceId != OpeningCancelJobId || O.ResetGeneration != OpeningCancelResetGeneration
            || S.TotalCredits != OpeningCancelCredits || S.bCharging || !CargoPendingValid
            || S.CargoConditionPercent != OpeningCancelConditionPercent || S.BatteryPercent > OpeningCancelBatteryPercent + 1e-6
            || (!bAirUnloadCancellationInProgress && Director->GetSecuredParcelCount() != 0) || S.Phase == EMAYADeliveryPhase::Failed)
        { Stop(false, TEXT("Cancellation changed original uncommitted inventory, reward, energy or condition")); break; }
        if (O.bDoorsOpen || Director->WasCancelledOpenObserved(OpeningCancelServiceGeneration)) bCancelledOpenObserved = true;
        if (Phase == EPhase::CancelOpeningAwaitClosure)
        {
            const auto RestoredPhase = bAirUnloadCancellationInProgress ? EMAYADeliveryPhase::Carrying : EMAYADeliveryPhase::Accepted;
            if (S.Phase == RestoredPhase && O.bDoorsClosedLatched && O.IsStoppedAndSupported())
            {
                if (bAirUnloadCancellationInProgress && (!VerifyLoadedInventory(O)
                    || Director->GetSecuredParcelCount() != S.Job.ParcelCount))
                { Stop(false, TEXT("Reverse unload cancellation did not restore every same-body native restraint: ") + InventoryFailure); break; }
                if (bAirUnloadCancellationInProgress)
                {
                    for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
                    {
                        auto* P = Director->GetParcel(Slot); double Alpha = 0; bool Restored = false;
                        if (!P || !P->ObserveAirAppearance(Alpha, Restored) || Alpha != 1 || !Restored)
                        { Stop(false, TEXT("Reverse unload cancellation did not restore actual original opaque materials")); break; }
                    }
                    if (bStopped) break;
                }
                if (!bCancelledOpenObserved) { Stop(false, TEXT("Cancellation closed without observing the required real open endpoints")); break; }
                auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), bAirUnloadCancellationInProgress ? TEXT("actual_reverse_unload_cancelled_same_body_carrying_closed") : TEXT("actual_open_then_cancelled_closed"));
                J->SetNumberField(TEXT("worldSeconds"), Now); J->SetStringField(TEXT("nativeAtClosure"), Robot->ReadTelemetryJson());
                J->SetStringField(TEXT("actualCancellation"), Director->ReadServiceCancellationEvidenceJson());
                J->SetStringField(TEXT("inventoryAtClosedRestoration"), Director->ReadCargoInventoryEvidenceJson());
                OpeningCancellationReceipts.Add(MakeShared<FJsonValueObject>(J)); Enter(EPhase::CancelOpeningRetry, O);
            }
            break;
        }
        if (S.Phase != (bAirUnloadCancellationInProgress ? EMAYADeliveryPhase::Carrying : EMAYADeliveryPhase::Accepted)) { Stop(false, TEXT("Cancelled job was not Accepted before its one retry")); break; }
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched && Robot->CanRequestDeliveryDoorState(true))
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS < 0.5) break;
            ++StationInteractionCalls;
            if (!Director->RequestInteract()) { Stop(false, TEXT("One actual safe retry refused after the cancelled lids settled")); break; }
            const auto After = Director->Snapshot();
            if (After.Phase != (bAirUnloadCancellationInProgress ? EMAYADeliveryPhase::Unloading : EMAYADeliveryPhase::Loading) || After.Service.Generation <= OpeningCancelServiceGeneration
                || After.JobInstanceId != OpeningCancelJobId)
            { Stop(false, TEXT("One retry did not admit a fresh service token for the same original job")); break; }
            const bool WasUnloading = bAirUnloadCancellationInProgress;
            if (WasUnloading) { bAirUnloadRetried = true; bAirUnloadCancellationInProgress = false; }
            else bOpeningCancelRetried = true;
            Enter(WasUnloading ? EPhase::Unloading : EPhase::Loading, O);
        }
        else StableSinceWorldS = -1;
        break;
    }
    case EPhase::LoadedDrive:
    {
        const auto& Route = Director->GetMeasuredRoute();
        if (Route.Num() < 13) { Stop(false, TEXT("Measured city corridor lacks the 600 cm approach needed for the bounded loaded driving trial")); break; }
        if (Age > 15) { Stop(false, TEXT("Loaded physical 100 cm travel and 50 cm/s speed not observed within fifteen seconds")); break; }
        if (!VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
        // Travel is through the existing surveyed corridor, using the same command/brake law.
        // Record differing start conditions; no unsupported greater/less-than mass-effect claim.
        // Keep acceleration room before the existing navigation taper/brake region.
        NavigateTo(Route[12], O);
        if (FVector::Dist2D(PhaseStartPose.GetLocation(), O.ChassisPose.GetLocation()) >= 100 && O.VelocityCmps.Size() >= 50)
            Enter(EPhase::LoadedBrake, O);

        break;
    }
    case EPhase::LoadedBrake:
    {
        if (!VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
        Drive(0, 0, true); BrakeTravelCm += FVector::Distance(LastPosition, O.ChassisPose.GetLocation()); LastPosition = O.ChassisPose.GetLocation();
        if (BrakeTravelCm > 100 || Age > 2) Stop(false, TEXT("Loaded native brake exceeded 2 seconds or 100 cm before supported stop"));
        else if (O.IsStoppedAndSupported()) Enter(bFullInputTrials && !bFullLoadedTrialsComplete && Director->Snapshot().Job.ParcelCount == 2 && Director->Snapshot().Job.PayloadKg == 2
            ? EPhase::FullLoadedPosition : !bOperatingTest && !(CarriedRecoveryCountMask & (1 << Director->Snapshot().Job.ParcelCount))
                ? EPhase::CarriedRecover : EPhase::ToDropoff, O);
        break;
    }
    case EPhase::CarriedRecover:
    {
        Drive(0, 0, true);
        if (Age > 20) { Stop(false, TEXT("Carried reset preflight remained obstructed or unsettled for twenty gameplay seconds")); break; }
        const auto S = Director->Snapshot();
        if (!bCarriedPreflightInitialized)
        {
            if (!VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
            CarriedJobId = S.JobInstanceId; CarriedResetGeneration = O.ResetGeneration;
            CarriedPreflightCredits = S.TotalCredits; CarriedParcelIds = LoadedParcelIds;
            CarriedParcelActors = LoadedParcelActors; bCarriedPreflightInitialized = true;
        }
        if (S.Phase != EMAYADeliveryPhase::Carrying || S.JobInstanceId != CarriedJobId
            || O.ResetGeneration != CarriedResetGeneration || S.TotalCredits != CarriedPreflightCredits
            || !VerifyLoadedInventory(O) || CarriedParcelIds != LoadedParcelIds)
        { Stop(false, TEXT("Carried reset preflight lost original individual cargo custody: ") + InventoryFailure); break; }
        if (!bLastCommandAccepted || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched) break;
        if ((LastCarriedPreflightQueryWorldS >= 0 && Now - LastCarriedPreflightQueryWorldS < 2) || CarriedPreflightQueryCount >= 10) break;
        LastCarriedPreflightQueryWorldS = Now; ++CarriedPreflightQueryCount;
        const bool Clear = Robot->CanResetDeliveryRobot(); auto Receipt = MakeShared<FJsonObject>();
        Receipt->SetStringField(TEXT("scope"), TEXT("Read-only original full proposed assembly query including every actual parcel/world blocker; no recovery or fault mutation"));
        Receipt->SetNumberField(TEXT("worldSeconds"), Now); Receipt->SetNumberField(TEXT("queryIndex"), CarriedPreflightQueryCount);
        Receipt->SetBoolField(TEXT("clear"), Clear); Receipt->SetStringField(TEXT("jobInstanceId"), CarriedJobId);
        Receipt->SetStringField(TEXT("nativeQuery"), Robot->ReadDeliveryResetClearanceQueryJson());
        Receipt->SetArrayField(TEXT("actualParcelInventory"), ReadParcelInventoryJson());
        CarriedRecoveryPreflightReceipts.Add(MakeShared<FJsonValueObject>(Receipt)); Record(O, true, TEXT("carried_recovery_preflight"));
        if (!Clear) break;
        if (!Director->RequestRecover()) Stop(false, TEXT("Authoritative carried recovery refused after clear preflight; actual race/transaction failure"));
        else Enter(EPhase::CarriedRecoverSettle, O);
        break;
    }
    case EPhase::CarriedRecoverSettle:
    {
        Drive(0, 0, true);
        if (Age > 20) { Stop(false, TEXT("Carried recovery did not observe rebuilt support and same individual depot custody within twenty seconds")); break; }
        const auto S = Director->Snapshot();
        if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched || O.ResetGeneration <= CarriedResetGeneration
            || S.Phase != EMAYADeliveryPhase::Accepted) break;
        int32 LiveParcels = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++LiveParcels;
        bool Valid = S.JobInstanceId == CarriedJobId && S.TotalCredits == CarriedPreflightCredits
            && Director->GetParcelCount() == CarriedParcelIds.Num() && LiveParcels == CarriedParcelIds.Num()
            && CarriedParcelActors.Num() == CarriedParcelIds.Num() && Director->GetSecuredParcelCount() == 0;
        double SumKg = 0;
        for (int32 Slot = 0; Valid && Slot < CarriedParcelIds.Num(); ++Slot)
        {
            auto* P = Director->GetParcel(Slot); FTransform StockPose, NativePose; double Kg = 0;
            Valid = IsValid(P) && P == CarriedParcelActors[Slot].Get() && P->GetParcelId() == CarriedParcelIds[Slot]
                && P->GetParcelBody() == LoadedParcelBodies[Slot].Get() && !P->IsDormantForDeliveryRemoval()
                && P->GetParcelBody()->IsSimulatingPhysics()
                && P->GetParcelBody()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics
                && P->ObserveMass(Kg) && FMath::Abs(Kg - S.Job.PayloadKg / S.Job.ParcelCount) <= 0.05
                && P->ObserveNativePose(NativePose) && Director->GetRecoveryParcelStockPose(Slot, StockPose)
                && P->IsAt(StockPose) && FVector::Dist(NativePose.GetLocation(), StockPose.GetLocation()) <= 2
                && NativePose.GetRotation().AngularDistance(StockPose.GetRotation()) <= FMath::DegreesToRadians(3.0);
            SumKg += Kg;
        }
        if (!Valid || FMath::Abs(SumKg - S.Job.PayloadKg) > 0.05)
            Stop(false, TEXT("Carried reconstruction did not restore the same individual actors/IDs/bodies/native masses at validated depot stock poses without secured custody or credit"));
        else
        {
            CarriedRecoveryCountMask |= 1 << S.Job.ParcelCount;
            bCarriedRecoveryChecked = (CarriedRecoveryCountMask & 6) == 6;
            if (bCurbTest)
            {
                const bool Complete = CurbTrial && bBlockedPathChecked && (CarriedRecoveryCountMask & 2);
                Stop(Complete, Complete
                    ? TEXT("Original city curb diagnostic completed actual one-point landing, neutral supported road hold and same-body one-parcel reconstruction; ordinary delivery/full controls are separate QA")
                    : TEXT("Original city curb diagnostic lacks required invalid-slot refusal or actual one-body reconstruction evidence"));
            }
            else Enter(EPhase::ToPickup, O);
        }
        break;
    }
    case EPhase::NextJob:
    {
        if (!bAutomaticRingPickupObserved || bUnexpectedAutomaticServiceObserved || ExplicitRingAdmissionCount < 1)
        { Stop(false, TEXT("Ring coverage requires one no-interaction automatic pickup and subsequent explicit native services before ten seconds")); break; }
        Drive(0, 0, true);
        if (bDamageTest)
        {
            const bool Complete = MissionCount == 1 && bBlockedPathChecked && DamageFixture && DamageFixture->GetCompletedTrialCount() == 3;
            Stop(Complete, Complete
                ? TEXT("Named physical empty/small-loaded/hard-loaded impact fixture and real one-parcel delivery completed; original city-curb landing, two-parcel damage, full controls and recovery remain separate QA")
                : TEXT("Named physical damage diagnostic did not complete all three actual contact trials, invalid-slot refusal and real delivery coverage"));
            break;
        }
        if (bAirLoweringTest && MissionCount >= 3
            && (!bAirLoweringPauseComplete || !bAirLoweringCancelTriggered || !bOpeningCancelRetried
                || !bAirUnloadPauseComplete || !bAirUnloadCancelTriggered || !bAirUnloadRetried))
        { Stop(false, TEXT("Completed deliveries lack the requested actual mid-descent pause/cancellation/one retry coverage")); break; }
        const bool bNeedsNextJobRequest = bCashShiftTest || bOperatingTest || !(MissionCount >= 3 && bBlockedPathChecked && (bCargoVideoRequested || bCarriedRecoveryChecked));
        if (bNeedsNextJobRequest)
        {
            if (Age > 20) { Stop(false, TEXT("Next-job setup did not retain actual safe admission for half a gameplay second within twenty seconds")); break; }
            if (!O.IsStoppedAndSupported() || !O.bDoorsClosedLatched || !Director->CanRequestNext())
            { StableSinceWorldS = -1; break; }
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS < 0.5) break;
        }
        // One normal request follows continuously observed production eligibility.
        // A refusal after this boundary remains a failed transaction, not a retry.
        if (bOperatingTest)
        {
            if (MissionCount != 1 || !bOpeningPauseComplete || !bClosingPauseComplete)
            { Stop(false, TEXT("Operating suite did not complete one real delivery and both partial-lid pause regressions")); break; }
            OperatingCreditsBefore = Director->Snapshot().TotalCredits;
            if (Director->RequestCharging()) { Stop(false, TEXT("Actual away-from-depot charge request was admitted")); break; }
            if (!Director->RequestNext()) { Stop(false, TEXT("Normal next job did not offer urgent profile")); break; }
            Enter(EPhase::OperatingUrgentAccept, O); break;
        }
        if (MissionCount >= 3 && (!bPaidRingChargeObserved || !bPaidMovementStopObserved))
        { Stop(false, TEXT("Completed native deliveries lack one actual funded automatic ring and fresh-movement disconnect")); break; }
        if (!bCashShiftTest && MissionCount >= 3 && bBlockedPathChecked && (bCargoVideoRequested || bCarriedRecoveryChecked))
            Stop(!bCargoVideoRequested || ServiceVideoRequestCoverageMask == 15, bCargoVideoRequested
                ? (ServiceVideoRequestCoverageMask == 15 ? TEXT("Actual one/two-parcel loading/unloading sequences and post-delivery funded automatic ring/fee/movement-disconnect completed; initial standalone mechanics and carried-recovery diagnostics omitted; actual pixels require independent review")
                    : TEXT("Normal deliveries completed but requested one/two-parcel loading/unloading viewport coverage is incomplete"))
                : bCargoDiagnosticTest
                ? TEXT("Cargo diagnostic completed all three actual payload profiles, mission lid service, invalid-slot endpoint refusal, carried recovery and one post-delivery funded automatic ring/fee/movement-disconnect; initial motion suite and standalone lid cycle were not assessed")
                : TEXT("Native mechanics, all three actual payload profiles, post-delivery paid automatic charging/movement-disconnect, invalid-slot endpoint refusal and carried recovery completed"));
        else if (!Director->RequestNext()) Stop(false, TEXT("Fresh next job request refused"));
        else Enter(MissionCount == 1 && !bPaidRingChargeObserved ? EPhase::PaidChargeReturn : EPhase::Accept, O);
        break;
    }
    case EPhase::RingAccept:
    {
        Drive(0, 0, true);
        if (!Director->RequestAccept()) { Stop(false, TEXT("Ring test could not normally accept the depot pickup job")); break; }
        const auto S = Director->Snapshot();
        AMAYADeliveryStation* Station = Director->FindStation(S.Job.PickupStationId);
        const auto& Route = Director->GetMeasuredRoute();
        if (!Station || Route.Num() < 4 || FVector::Dist2D(Station->GetServicePoint(), Route[0]) > 1
            || FMath::Abs(FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,
                (Route[3] - Route[0]).Rotation().Yaw)) > 2)
        { Stop(false, TEXT("Ring test lacks the measured depot parking point and aligned physical exit corridor")); break; }
        ServiceStationId = Station->StationId; ServiceStationCm = Station->GetServicePoint();
        ServiceApproachCm = Route[0]; RingTestJobId = S.JobInstanceId; RingTestCreditsBefore = S.TotalCredits;
        Enter(EPhase::RingEarlyMovingInteraction, O); break;
    }
    case EPhase::RingEarlyMovingInteraction:
    {
        if (Age > 5) { Stop(false, TEXT("Actual moving in-ring setup did not reach the unsafe interaction condition within five seconds")); break; }
        const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
        Drive(0.35f, 0, false);
        if (S.Phase != EMAYADeliveryPhase::Accepted || S.JobInstanceId != RingTestJobId
            || !R.bInside || R.ElapsedGameplaySeconds >= 5 || R.bVisitConsumed)
        { Stop(false, TEXT("Moving early-interaction setup left the actual unconsumed pickup ring")); break; }
        if (O.VelocityCmps.Size() <= 10 || Robot->CanRequestDeliveryDoorState(true)) break;
        RingFirstEntryGeneration = R.EntryGeneration;
        const double ElapsedBefore = R.ElapsedGameplaySeconds;
        const uint64 RequestBefore = R.RequestGeneration;
        const FString InventoryBefore = ReadParcelIdentityPoseJson();
        ++StationInteractionCalls;
        const bool Accepted = Director->RequestInteract();
        const auto After = Director->Snapshot(); const auto& Request = Director->GetRingObservation();
        if (Accepted || After.Phase != S.Phase || After.JobInstanceId != S.JobInstanceId
            || After.Service.Generation != S.Service.Generation || After.TotalCredits != S.TotalCredits
            || ReadParcelIdentityPoseJson() != InventoryBefore
            || Request.RequestGeneration <= RequestBefore || Request.LastRequestAccepted
            || Request.LastRequestOrigin != TEXT("ExplicitInteraction") || Request.bVisitConsumed
            || !Request.bInside || Request.EntryGeneration != RingFirstEntryGeneration
            || FMath::Abs(Request.ElapsedGameplaySeconds - ElapsedBefore) > 1.e-6)
        { Stop(false, TEXT("Unsafe explicit native interaction altered actual mission/custody or consumed/reset the countdown")); break; }
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("test"), TEXT("MovingEarlyExplicitInteractionRejected"));
        J->SetBoolField(TEXT("pass"), true); J->SetNumberField(TEXT("worldSeconds"), Now);
        J->SetNumberField(TEXT("actualSpeedCmps"), O.VelocityCmps.Size());
        J->SetStringField(TEXT("inputScope"), TEXT("One actual native RequestInteract API call; physical keyboard E not assessed"));
        J->SetObjectField(TEXT("ringAfter"), MayaV01RingObservationJson(Request));
        RingInteractionReceipts.Add(MakeShared<FJsonValueObject>(J)); bRingEarlyInteractionRejected = true;
        Enter(EPhase::RingExit, O); break;
    }
    case EPhase::RingExit:
    {
        if (Age > 15) { Stop(false, TEXT("Physical ring exit did not cross its measured boundary within fifteen seconds")); break; }
        const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
        if (S.Phase != EMAYADeliveryPhase::Accepted || S.JobInstanceId != RingTestJobId)
        { Stop(false, TEXT("Ring exit changed the actual accepted mission")); break; }
        if (FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm) >= 105.0 && !R.bInside)
        {
            if (R.ElapsedGameplaySeconds != 0 || R.RemainingGameplaySeconds != 10 || R.bVisitConsumed)
            { Stop(false, TEXT("Actual ring exit did not reset the continuous gameplay countdown")); break; }
            auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("test"), TEXT("PhysicalRingExitReset"));
            J->SetBoolField(TEXT("pass"), true); J->SetNumberField(TEXT("worldSeconds"), Now);
            J->SetNumberField(TEXT("distanceFromParkingCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm));
            J->SetObjectField(TEXT("ring"), MayaV01RingObservationJson(R));
            RingInteractionReceipts.Add(MakeShared<FJsonValueObject>(J)); bRingExitResetObserved = true;
            Enter(EPhase::RingOutsideSettle, O);
        }
        else Drive(0.35f, 0, false);
        break;
    }
    case EPhase::RingOutsideSettle:
    {
        Drive(0, 0, true); const auto& R = Director->GetRingObservation();
        if (Age > 5 || FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm) > 160)
        { Stop(false, TEXT("Outside-ring physical brake/settlement exceeded five seconds or160 cm from the depot")); break; }
        if (R.bInside || R.ElapsedGameplaySeconds != 0 || R.RemainingGameplaySeconds != 10)
        { Stop(false, TEXT("Actual outside-ring settlement advanced the countdown")); break; }
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS >= 0.5) Enter(EPhase::RingReenter, O);
        }
        else StableSinceWorldS = -1;
        break;
    }
    case EPhase::RingReenter:
    {
        if (Age > 15) { Stop(false, TEXT("Physical depot-ring reentry exceeded fifteen seconds")); break; }
        const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
        if (S.Phase != EMAYADeliveryPhase::Accepted || S.JobInstanceId != RingTestJobId)
        { Stop(false, TEXT("Physical reentry changed the accepted job")); break; }
        if (R.bInside && !bRingReentryResetObserved)
        {
            if (R.EntryGeneration <= RingFirstEntryGeneration || R.ElapsedGameplaySeconds > 0.1
                || R.RemainingGameplaySeconds < 9.9 || R.bVisitConsumed)
            { Stop(false, TEXT("Actual reentry did not create a fresh reset ten-second visit")); break; }
            auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("test"), TEXT("PhysicalRingReentryReset"));
            J->SetBoolField(TEXT("pass"), true); J->SetNumberField(TEXT("worldSeconds"), Now);
            J->SetArrayField(TEXT("actualPositionCm"), VJson(O.ChassisPose.GetLocation()));
            J->SetObjectField(TEXT("ring"), MayaV01RingObservationJson(R));
            RingInteractionReceipts.Add(MakeShared<FJsonValueObject>(J)); bRingReentryResetObserved = true;
        }
        // The fresh visit is observed at the true 90 cm boundary above. Continue
        // to the ordinary 65 cm parking distance before braking: stopping at the
        // boundary let small physical settling drift exit the ring in game33.
        // The original countdown still runs continuously from actual entry.
        if (bRingReentryResetObserved && R.bInside
            && FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm) <= 65.0)
        {
            Enter(EPhase::RingInsideSettle, O);
            AutomaticWaitEntryInteractionCalls = StationInteractionCalls; AutomaticWaitFrames = 0;
        }
        else NavigateTo(ServiceStationCm, O);
        break;
    }
    case EPhase::RingInsideSettle:
    case EPhase::RingAwaitAutomatic:
    {
        Drive(0, 0, true); ++AutomaticWaitFrames;
        if (Age > (Phase == EPhase::RingInsideSettle ? 5.0 : 15.0))
        { Stop(false, TEXT("Actual ring settlement/countdown admission exceeded its declared bound")); break; }
        const auto S = Director->Snapshot(); const auto& R = Director->GetRingObservation();
        if (S.Phase == EMAYADeliveryPhase::Loading)
        {
            if (!bRingCountdownPauseComplete || !R.LastAutomaticRequestAccepted
                || R.LastAutomaticServiceGeneration != S.Service.Generation || R.LastAutomaticStationId != ServiceStationId
                || R.LastAutomaticElapsedGameplaySeconds < 10 || !R.LastRequestAccepted
                || R.LastRequestOrigin != TEXT("AutomaticRing") || StationInteractionCalls != AutomaticWaitEntryInteractionCalls)
            { Stop(false, TEXT("Reentered ring automatic pickup lacks pause, ten-second duration or no-interaction provenance")); break; }
            bAutomaticRingPickupObserved = true; ++AutomaticRingAdmissionCount;
            SaveRingServiceReceipt(O, false); Enter(EPhase::Loading, O); break;
        }
        if (S.Phase != EMAYADeliveryPhase::Accepted || S.JobInstanceId != RingTestJobId || !R.bInside || R.bVisitConsumed)
        { Stop(false, TEXT("Reentered ring countdown lost its actual accepted in-ring visit")); break; }
        if (Phase == EPhase::RingInsideSettle)
        {
            if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched && Robot->CanRequestDeliveryDoorState(true))
            {
                if (StableSinceWorldS < 0) StableSinceWorldS = Now;
                if (Now - StableSinceWorldS >= 0.5) Enter(EPhase::RingAwaitAutomatic, O);
            }
            else StableSinceWorldS = -1;
        }
        else if (!bRingCountdownPauseComplete && TryStartRingCountdownPause(O)) return;
        break;
    }
    case EPhase::OperatingAccept:
        Drive(0, 0, true);
        if (!Director->RequestAccept()) { Stop(false, TEXT("Low-charge actual job accept was refused")); break; }
        OperatingFailedJobId = Director->Snapshot().JobInstanceId;
        OperatingCreditsBefore = Director->Snapshot().TotalCredits;
        Enter(EPhase::OperatingDrain, O);
        break;
    case EPhase::OperatingDrain:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        const auto& Ring = Director->GetRingObservation();
        if (S.Phase == EMAYADeliveryPhase::Loading && S.Service.Generation != 0
            && S.Service.Generation != OperatingInitialAutomaticServiceGeneration)
        {
            AMAYADeliveryStation* Station = Director->FindStation(S.Service.StationId);
            if (!Station || !Ring.LastAutomaticRequestAccepted
                || Ring.LastAutomaticServiceGeneration != S.Service.Generation
                || Ring.LastAutomaticStationId != S.Service.StationId
                || Ring.LastAutomaticElapsedGameplaySeconds < 10.0)
            { Stop(false, TEXT("Automatic service during actual low-charge depletion lacks matching ring provenance")); break; }
            ServiceStationId = Station->StationId; ServiceStationCm = Station->GetServicePoint();
            if (!FindServiceApproach(ServiceStationCm, ServiceApproachCm))
            { Stop(false, TEXT("Low-charge automatic service station is outside the measured corridor")); break; }
            OperatingInitialAutomaticServiceGeneration = S.Service.Generation;
            SaveRingServiceReceipt(O, false);
            // This normal automatic service is recorded separately. Post-recharge
            // pickup still requires its own ten-second no-interaction test and lid pauses.
        }
        if (Age > 120) { Stop(false, TEXT("Normal low-charge idle drain did not deplete within120 gameplay seconds")); break; }
        if (!O.IsStoppedAndSupported()) { Stop(false, TEXT("Idle depletion trial lost measured stopped support")); break; }
        if (S.Phase == EMAYADeliveryPhase::Failed)
        {
            if (S.BatteryPercent != 0 || !S.Result.Reason.Contains(TEXT("Battery depleted")) || S.Result.bSuccess
                || S.Result.JobInstanceId != OperatingFailedJobId || S.TotalCredits != OperatingCreditsBefore)
            { Stop(false, TEXT("Actual depletion did not freeze an unrewarded failed job at zero charge")); break; }
            OperatingFrozenResult = S.Result; OperatingZeroMotorSamples = 0;
            Enter(EPhase::OperatingDepletedHold, O);
        }
        break;
    }
    case EPhase::OperatingDepletedHold:
    case EPhase::OperatingDeadlineHold:
    {
        Drive(1, 0, false); const auto S = Director->Snapshot();
        if (Age > 10) { Stop(false, TEXT("Failed-state native drive inhibition hold exceeded10 seconds")); break; }
        if (S.Phase != EMAYADeliveryPhase::Failed || S.TotalCredits != OperatingCreditsBefore
            || S.Result.JobInstanceId != OperatingFrozenResult.JobInstanceId
            || S.Result.Reason != OperatingFrozenResult.Reason || S.Result.bSuccess || S.Result.Credits != 0
            || S.Result.ElapsedSeconds != OperatingFrozenResult.ElapsedSeconds
            || S.Result.BatteryPercent != OperatingFrozenResult.BatteryPercent
            || !O.IsStoppedAndSupported() || FVector::Dist(O.ChassisPose.GetLocation(), PhaseStartPose.GetLocation()) > 1)
        { Stop(false, TEXT("Failed result/reward changed or inhibited full-throttle moved the robot")); break; }
        if (Age > 0.1)
        {
            if (!ReadOperatingZeroMotorEvidence()) { Stop(false, TEXT("Failed-state full-throttle native lease/motor evidence did not prove zero wheel torque")); break; }
            ++OperatingZeroMotorSamples;
        }
        if (Age >= 3 && OperatingZeroMotorSamples > 0)
            Enter(Phase == EPhase::OperatingDepletedHold ? EPhase::OperatingRecover : EPhase::OperatingRetryAccept, O);
        break;
    }
    case EPhase::OperatingRecover:
        Drive(0, 0, true); OperatingPreResetGeneration = O.ResetGeneration;
        if (!Director->RequestRecover()) Stop(false, TEXT("Actual depleted native reconstruction request was refused"));
        else Enter(EPhase::OperatingRecoverSettle, O);
        break;
    case EPhase::OperatingRecoverSettle:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 20) { Stop(false, TEXT("Depleted actual reconstruction did not settle within20 seconds")); break; }
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched && O.ResetGeneration > OperatingPreResetGeneration
            && S.Phase == EMAYADeliveryPhase::Available)
        {
            int32 ActualParcels = 0; for (TActorIterator<AMAYADeliveryParcel> It(GetWorld()); It; ++It) if (IsValid(*It)) ++ActualParcels;
            if (S.BatteryPercent != 0 || !S.JobInstanceId.IsEmpty() || S.TotalCredits != OperatingCreditsBefore
                || Director->GetParcel() || ActualParcels != 0 || Director->RequestAccept())
            { Stop(false, TEXT("Depleted recovery minted energy/reward/cargo or permitted zero-charge accept")); break; }
            Enter(EPhase::OperatingCharge, O);
        }
        break;
    }
    case EPhase::OperatingCharge:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 15) { Stop(false, TEXT("Unfunded depleted-ring/NewShift presentation did not resolve within fifteen seconds")); break; }
        if (S.BatteryPercent != 0 || S.TotalCredits != 0 || S.bCharging || S.LastRecharge.Generation != 0)
        { Stop(false, TEXT("Depleted zero-cash recovery minted energy/cash or admitted a free recharge")); break; }
        if (Age >= 10 && O.IsStoppedAndSupported() && O.bDoorsClosedLatched && S.ChargeVisit.bInside)
        {
            const auto View = Director->GetPlayerPresentation();
            if (UMAYAPlayerNewShiftReloadTest::IsEnabled() && View.bCanStartNewShift && !View.bCanAccept)
            {
                Record(O, true, TEXT("operating_before_actual_new_shift_reload"));
                WriteReport(false, false, TEXT("Nonterminal actual Operating checkpoint; real NewShift confirmation/reload QA remains pending"));
                FString Checkpoint;
                if (!FFileHelper::LoadFileToString(Checkpoint, *FPaths::Combine(RunDirectory, TEXT("result.json")))
                    || !FFileHelper::SaveStringToFile(Checkpoint, *FPaths::Combine(RunDirectory, TEXT("operating-before-reload.json")))
                    || !UMAYAPlayerNewShiftReloadTest::BeginOperatingHandoff(GetWorld(), RunDirectory, O))
                { Stop(false, TEXT("Actual Operating proof could not seal its nonterminal NewShift handoff")); break; }
                CloseTelemetryWriter(); Robot->ReleaseDriveCommand();
                bStopped = true; SetActorTickEnabled(false); // No Stop(), Director pause, resource setter or exit timer.
            }
            else Stop(View.bCanStartNewShift && !View.bCanAccept,
                View.bCanStartNewShift && !View.bCanAccept
                ? TEXT("Actual depletion/zero motor hold/same native recovery/zero-cash no-free-charge and safe explicit NewShift eligibility completed; old free-recharge/urgent-retry scenario is superseded")
                : TEXT("Unfunded depleted state did not expose safe explicit NewShift while rejecting accept"));
        }
        break;
    }
    case EPhase::OperatingUrgentAccept:
    {
        Drive(0, 0, true); const auto Offered = Director->Snapshot();
        if (Offered.Job.ProfileId != TEXT("city_2") || Offered.Job.DeadlineSeconds != 150 || !Director->RequestAccept())
        { Stop(false, TEXT("Normal next/accept did not produce the unchanged150-second urgent job")); break; }
        OperatingFailedJobId = Director->Snapshot().JobInstanceId;
        Enter(EPhase::OperatingUrgentWait, O);
        break;
    }
    case EPhase::OperatingUrgentWait:
    {
        Drive(0, 0, true); const auto S = Director->Snapshot();
        if (Age > 155) { Stop(false, TEXT("Actual urgent deadline did not fail within155 gameplay seconds")); break; }
        if (!O.IsStoppedAndSupported()) { Stop(false, TEXT("Urgent deadline wait lost stopped support")); break; }
        if (S.Phase == EMAYADeliveryPhase::Failed)
        {
            if (!S.Result.Reason.Contains(TEXT("deadline expired")) || S.Result.bSuccess || S.Result.ElapsedSeconds < 150
                || S.Result.ElapsedSeconds > 151 || S.Result.JobInstanceId != OperatingFailedJobId || S.BatteryPercent <= 0
                || S.TotalCredits != OperatingCreditsBefore)
            { Stop(false, TEXT("Actual urgent deadline produced the wrong failure/clock/identity/reward")); break; }
            OperatingFrozenResult = S.Result; OperatingZeroMotorSamples = 0;
            Enter(EPhase::OperatingDeadlineHold, O);
        }
        break;
    }
    case EPhase::CurbTrial:
    {
        if (!VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
        if (!CurbTrial)
        {
            Drive(0, 0, true);
            if (Age > 10) { Stop(false, TEXT("Curb setup did not retain actual stopped/support/closed-latch readiness for half a second within ten seconds")); break; }
            if (!bLastCommandAccepted || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched)
            { StableSinceWorldS = -1; break; }
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS < 0.5) break;
            CurbTrial = GetWorld()->SpawnActor<AMAYAV01CurbTrial>();
            if (!CurbTrial || !CurbTrial->Configure(Robot, Director, O))
            { Stop(false, CurbTrial ? CurbTrial->FailureReason() : TEXT("Original city curb observer could not spawn")); break; }
        }
        const auto Outcome = CurbTrial->Advance(O);
        bLastCommandAccepted = CurbTrial->LastCommandAccepted();
        if (Outcome == AMAYAV01CurbTrial::EOutcome::Failed) Stop(false, CurbTrial->FailureReason());
        else if (Outcome == AMAYAV01CurbTrial::EOutcome::Complete)
        { CurbTrial->Cleanup(); Enter(bTrafficEncounterTest ? EPhase::TrafficEncounter : EPhase::CarriedRecover, O); }
        break;
    }
    case EPhase::DamageEmptyTrial:
    case EPhase::DamageEmptyHard:
    case EPhase::DamageLoadedSmall:
    case EPhase::DamageLoadedHard:
    {
        if (Age > 50) { Stop(false, TEXT("Named actual physical damage trial exceeded fifty gameplay seconds")); break; }
        const bool Loaded = Phase != EPhase::DamageEmptyTrial && Phase != EPhase::DamageEmptyHard;
        if (Loaded && !VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
        if (!DamageFixture)
        {
            DamageFixture = GetWorld()->SpawnActor<AMAYAV01DamageFixture>();
            if (!DamageFixture || !DamageFixture->Configure(Robot, Director))
            { Stop(false, DamageFixture ? DamageFixture->GetFailureReason() : TEXT("Named physical fixture could not spawn")); break; }
        }
        if (!bDamageTrialInitialized)
        {
            if (!DamageFixture->BeginTrial(Loaded, Phase == EPhase::DamageLoadedHard || Phase == EPhase::DamageEmptyHard, O))
            { Stop(false, DamageFixture->GetFailureReason()); break; }
            bDamageTrialInitialized = true;
        }
        const auto Outcome = DamageFixture->AdvanceTrial(O);
        bLastCommandAccepted = DamageFixture->WasLastCommandAccepted();
        if (Outcome == AMAYAV01DamageFixture::EOutcome::Failed)
        { Stop(false, DamageFixture->GetFailureReason()); break; }
        if (Outcome == AMAYAV01DamageFixture::EOutcome::Complete)
        {
            if (bDamageCalibration)
            {
                if (Phase == EPhase::DamageEmptyTrial) Enter(EPhase::DamageEmptyHard, O);
                else
                {
                    const bool Complete = Phase == EPhase::DamageEmptyHard && DamageFixture->GetCompletedTrialCount() == 2
                        && Director->GetParcelCount() == 0 && Director->Snapshot().CargoConditionPercent == 100
                        && Director->Snapshot().ImpactHistory.Num() == 0;
                    Stop(Complete, Complete ? TEXT("Actual empty small/hard contact calibration measurements completed; production cargo damage and city curb behavior are not assessed")
                        : TEXT("Actual empty contact calibration coverage or unchanged condition/history failed"));
                }
            }
            else if (Phase == EPhase::DamageLoadedHard)
            { DamageFixture->Cleanup(); Enter(EPhase::ToDropoff, O); }
            else Enter(Phase == EPhase::DamageEmptyTrial ? EPhase::DamageEmptyReturn : EPhase::DamageLoadedHard, O);
        }
        break;
    }
    case EPhase::DamageEmptyReturn:
    {
        if (Age > 25) { Stop(false, TEXT("Empty physical impact return did not reach the original depot supported within twenty-five seconds")); break; }
        const FVector Depot = Director->GetMeasuredRoute()[0];
        if (FVector::Dist2D(Depot, O.ChassisPose.GetLocation()) <= 65) bStationArrivalLatched = true;
        if (!bStationArrivalLatched) NavigateTo(Depot, O);
        else
        {
            Drive(0, 0, true);
            if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched && Robot->CanRequestDeliveryDoorState(true))
            {
                if (StableSinceWorldS < 0) StableSinceWorldS = Now;
                if (Now - StableSinceWorldS >= 2) Enter(EPhase::Accept, O);
            }
            else StableSinceWorldS = -1;
        }
        break;
    }
    case EPhase::DamageLoadedSettle:
        Drive(0, 0, true);
        if (Age > 10) { Stop(false, TEXT("Actual one-body cargo did not settle for the bounded physical impact fixture")); break; }
        if (!VerifyLoadedInventory(O)) { Stop(false, InventoryFailure); break; }
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Now - StableSinceWorldS >= 1) Enter(EPhase::DamageLoadedSmall, O);
        }
        else StableSinceWorldS = -1;
        break;
    case EPhase::OperatingRetryAccept:
        Drive(0, 0, true);
        if (!Director->RequestNext(true) || !Director->RequestAccept() || Director->Snapshot().JobInstanceId == OperatingFailedJobId
            || Director->Snapshot().TotalCredits != OperatingCreditsBefore || Director->Snapshot().Job.ProfileId != TEXT("city_2"))
            Stop(false, TEXT("Urgent failed retry did not create a fresh unrewarded active instance"));
        else Stop(true, TEXT("Actual pause/resume, low-charge depletion, failed drive inhibition, retained-zero recovery, depot recharge, real delivery and150-second urgent deadline/retry completed"));
        break;
    default: break;
    }
    if (!bStopped && O.bReady && !bLastCommandAccepted) Stop(false, TEXT("Explicit native automation command was refused"));
    if (!bStopped && Now - RunStartWorldS > (bCashShiftTest ? 1200 : 600)) Stop(false, bCashShiftTest ? TEXT("CashShift overall1200 second gameplay watchdog exceeded") : TEXT("Native suite overall 600 second gameplay watchdog exceeded"));
}

void AMAYAV01NativeTestDriver::Record(const FMAYADeliveryRobotObservation& O, bool bForce, const TCHAR* Event)
{
    const double Now = GetWorld()->GetTimeSeconds();
    const bool bSampleDue = bForce || LastSampleWorldS < 0 || Now - LastSampleWorldS >= MayaV01TelemetryCadenceS;
    if (!bSampleDue && SupportedWheelMotionMask == 0x3f) return;
    // Acceptance accumulation remains at the post-physics Tick rate. Only file
    // output is sampled; slowing the logger cannot hide a supported wheel rotation.
    const FString NativeTelemetry = Robot->ReadTelemetryJson();
    TSharedPtr<FJsonObject> NativeObject;
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(NativeTelemetry), NativeObject) && NativeObject.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
        if (NativeObject->TryGetArrayField(TEXT("parts"), Parts) && Parts && Parts->Num() >= 7)
            for (int32 W = 0; W < 6; ++W)
            {
                const auto Part = (*Parts)[W + 1]->AsObject(); double Omega = 0;
                if (Part.IsValid() && Part->TryGetNumberField(TEXT("axleOmegaRadps"), Omega)
                    && FMath::IsFinite(Omega) && FMath::Abs(Omega) >= 0.5 && (O.SupportMask & (1 << W)))
                    SupportedWheelMotionMask |= 1 << W;
                double Travel = 0;
                if (Part.IsValid() && Part->TryGetNumberField(TEXT("suspensionTravelCm"), Travel) && FMath::IsFinite(Travel))
                    PhasePeakSampledSuspensionCm = FMath::Max(PhasePeakSampledSuspensionCm, FMath::Abs(Travel));
            }
    }
    if (!bSampleDue) return;
    LastSampleWorldS = Now;
    const bool HeavyReceiptDue = !bCashShiftTest || bForce || LastCashShiftHeavyReceiptWorldS < 0 || Now - LastCashShiftHeavyReceiptWorldS >=5;
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("phase"), PhaseName(Phase));
    J->SetStringField(TEXT("event"), Event);
    if (DamageFixture) J->SetStringField(TEXT("actualDamageFixtureEvidence"), DamageFixture->ReadEvidenceJson(bForce));
    if (CurbTrial) J->SetStringField(TEXT("actualOriginalCityCurbEvidence"), CurbTrial->ReadEvidenceJson(bForce));
    if (TrafficEncounterTrial) J->SetStringField(TEXT("actualOriginalTrafficEncounter"), TrafficEncounterTrial->ReadEvidenceJson(bForce));
    J->SetBoolField(TEXT("nativeWorldPauseActive"), bNativePauseActive);
    J->SetBoolField(TEXT("directorPaused"), Director->IsPaused());
    if (Phase == EPhase::NextJob)
    {
        J->SetBoolField(TEXT("actualNextJobAdmissionPredicate"), Director->CanRequestNext());
        J->SetNumberField(TEXT("continuousNextJobSetupWorldSeconds"), StableSinceWorldS >= 0 ? Now - StableSinceWorldS : -1);
    }
    J->SetStringField(TEXT("serviceCancellationEvidence"), Director->ReadServiceCancellationEvidenceJson());
    if (bCashShiftTest || bTrafficEncounterTest)
    {
        J->SetBoolField(TEXT("naturalOpeningRetryUsed"), bCashNaturalRetryUsed);
        J->SetBoolField(TEXT("naturalOpeningRetryAdmitted"), bCashNaturalRetryAdmitted);
        J->SetBoolField(TEXT("naturalOpeningRetrySecured"), bCashNaturalRetrySecured);
        if (Phase == EPhase::CashOpeningRetry) J->SetNumberField(TEXT("continuousNaturalRetryQuietSeconds"), StableSinceWorldS >= 0 ? Now - StableSinceWorldS : -1);
    }
    J->SetNumberField(TEXT("missionElapsedSeconds"), Director->Snapshot().ElapsedSeconds);
    J->SetNumberField(TEXT("serviceGeneration"), double(Director->Snapshot().Service.Generation));
    J->SetStringField(TEXT("jobInstanceId"), Director->Snapshot().JobInstanceId);
    J->SetStringField(TEXT("frozenResultReason"), Director->Snapshot().Result.Reason);
    J->SetNumberField(TEXT("frozenResultElapsedSeconds"), Director->Snapshot().Result.ElapsedSeconds);
    J->SetStringField(TEXT("requestedCommandOrigin"), TEXT("NativeAutomation"));
    J->SetBoolField(TEXT("boostTestRequested"), bBoostTest);
    J->SetBoolField(TEXT("lastBoostRequested"), bLastBoostRequested);
    J->SetBoolField(TEXT("actualBoostDriveActive"), O.bBoostDriveActive);
    J->SetNumberField(TEXT("phaseActualBoostDriveFrames"), PhaseBoostDriveFrames);
    auto Inventory = ReadParcelInventoryJson();
    if (bCashShiftTest && !HeavyReceiptDue) for (const auto& Value : Inventory)
    { auto Item = Value->AsObject(); Item->RemoveField(TEXT("nativeCurrent")); Item->RemoveField(TEXT("tieDownInitialization")); }
    J->SetArrayField(TEXT("parcelInventory"), Inventory);
    J->SetNumberField(TEXT("jobParcelCount"), Director->Snapshot().Job.ParcelCount);
    J->SetNumberField(TEXT("actualParcelActorCount"), Director->GetParcelCount());
    J->SetNumberField(TEXT("actualSecuredParcelCount"), Director->GetSecuredParcelCount());
    J->SetBoolField(TEXT("lastSubmissionAccepted"), bLastCommandAccepted);
    J->SetNumberField(TEXT("worldSeconds"), Now); J->SetNumberField(TEXT("realSeconds"), FPlatformTime::Seconds());
    J->SetBoolField(TEXT("known"), O.bKnown); J->SetBoolField(TEXT("ready"), O.bReady);
    J->SetNumberField(TEXT("supportMask"), O.SupportMask); J->SetBoolField(TEXT("upright"), O.bUpright);
    J->SetArrayField(TEXT("positionCm"), VJson(O.ChassisPose.GetLocation())); J->SetArrayField(TEXT("velocityCmps"), VJson(O.VelocityCmps));
    J->SetArrayField(TEXT("angularVelocityRadps"), VJson(O.AngularVelocityRadps));
    J->SetNumberField(TEXT("yawDegrees"), O.ChassisPose.Rotator().Yaw); J->SetNumberField(TEXT("opticalForwardCmps"), O.OpticalForwardCmps);
    J->SetBoolField(TEXT("doorsOpen"), O.bDoorsOpen); J->SetBoolField(TEXT("doorsClosedLatched"), O.bDoorsClosedLatched);
    J->SetNumberField(TEXT("door1Degrees"), O.DoorAngleDegrees[0]); J->SetNumberField(TEXT("door2Degrees"), O.DoorAngleDegrees[1]);
    J->SetNumberField(TEXT("door1CurrentOmegaRadps"), O.DoorOmegaRadps[0]);
    J->SetNumberField(TEXT("door2CurrentOmegaRadps"), O.DoorOmegaRadps[1]);
    J->SetBoolField(TEXT("movingClosedPosePhysicallyLocked"), Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());
    J->SetBoolField(TEXT("fullNativeHeavyReceiptSaved"), HeavyReceiptDue);
    if (HeavyReceiptDue) J->SetStringField(TEXT("nativeTelemetry"), NativeTelemetry);
    J->SetNumberField(TEXT("supportedWheelMotionMask"), SupportedWheelMotionMask);
    J->SetNumberField(TEXT("missionPhase"), int32(Director->Snapshot().Phase)); J->SetStringField(TEXT("missionStatus"), Director->Snapshot().Status);
    J->SetNumberField(TEXT("batteryPercent"), Director->Snapshot().BatteryPercent);
    J->SetNumberField(TEXT("cargoConditionPercent"), Director->Snapshot().CargoConditionPercent);
    J->SetObjectField(TEXT("lastNativeImpact"), MAYADeliveryImpactEventToJson(O.LastNativeImpact));
    const auto AppliedImpact = MAYADeliveryImpactReceiptToJson(Director->Snapshot().LastImpact);
    const FString AppliedImpactEvidence = SerializeMayaV01Json(AppliedImpact);
    const bool bAppliedImpactChanged = AppliedImpactEvidence != LastRecordedAppliedImpactEvidence;
    if (bAppliedImpactChanged || bForce) J->SetObjectField(TEXT("lastAppliedImpact"), AppliedImpact);
    J->SetNumberField(TEXT("appliedImpactHistoryCount"), Director->Snapshot().ImpactHistory.Num());
    J->SetBoolField(TEXT("charging"), Director->Snapshot().bCharging);
    J->SetObjectField(TEXT("shiftEconomy"), MAYADeliveryEconomyToJson(Director->Snapshot()));
    J->SetBoolField(TEXT("actualFreshMovementIntent"), O.bFreshMovementIntent);
    J->SetObjectField(TEXT("lastChargeDisconnect"), MAYADeliveryChargeDisconnectToJson(Director->Snapshot().LastChargeDisconnect));
    J->SetBoolField(TEXT("chargeStoppedAndSupported"), O.IsStoppedAndSupported());
    J->SetBoolField(TEXT("chargeDoorsClosedLatched"), O.bDoorsClosedLatched);
    J->SetNumberField(TEXT("totalCredits"), Director->Snapshot().TotalCredits);
    J->SetStringField(TEXT("directorRuntimeStatus"), Director->GetRuntimeStatus());
    J->SetStringField(TEXT("recoveryDetail"), Director->GetLastRecoveryDetail());
    J->SetStringField(TEXT("cargoWorkerEvidence"), Director->ReadCargoWorkerEvidenceJson());
    if (HeavyReceiptDue) J->SetStringField(TEXT("crowdObstacleEvidence"), Director->ReadCrowdObstacleEvidenceJson());
    J->SetStringField(TEXT("instantServiceViewStatus"), ServiceObservationViewStatus);
    J->SetStringField(TEXT("activeInstantServiceVideoLabel"), ActiveServiceVideoLabel);
    J->SetBoolField(TEXT("instantServiceObservationViewActive"), ServiceObservationCamera != nullptr);
    if (ServiceObservationCamera)
    {
        J->SetArrayField(TEXT("instantServiceCameraWorldCm"), VJson(ServiceObservationCamera->GetActorLocation()));
        J->SetArrayField(TEXT("instantServiceCameraRotationDegrees"), VJson(FVector(
            ServiceObservationCamera->GetActorRotation().Pitch, ServiceObservationCamera->GetActorRotation().Yaw,
            ServiceObservationCamera->GetActorRotation().Roll)));
    }
    // Screenshots of actual instant cargo service from the owned test observation view.
    // No service timing, parcel or input is changed.
    if (!bCargoVideoRequested && (Phase == EPhase::Loading || Phase == EPhase::Unloading) && ServiceCaptureCount < 60
        && (LastServiceCaptureWorldS < 0 || Now - LastServiceCaptureWorldS >= 0.75)
        && !FScreenshotRequest::IsScreenshotRequested())
    {
        const FString CapturePath = FPaths::Combine(RunDirectory,
            FString::Printf(TEXT("instant-service-%02d-%s.png"), ++ServiceCaptureCount, PhaseName(Phase)));
        FScreenshotRequest::RequestScreenshot(CapturePath, true, false);
        LastServiceCaptureWorldS = Now;
        J->SetStringField(TEXT("instantServiceViewportCaptureRequested"), CapturePath);
    }
    J->SetStringField(TEXT("actualServiceFailureDetail"), Director->GetLastServiceFailureDetail());
    J->SetObjectField(TEXT("serviceRing"), MayaV01RingObservationJson(Director->GetRingObservation()));
    J->SetBoolField(TEXT("stationArrivalLatched"), bStationArrivalLatched);
    J->SetNumberField(TEXT("arrivalLatchWorldSeconds"), ArrivalLatchWorldS);
    J->SetNumberField(TEXT("arrivalLatchExitCount"), ArrivalLatchExitCount);
    J->SetBoolField(TEXT("awaitingAutomaticRingPickup"), bAwaitAutomaticRingPickup);
    J->SetNumberField(TEXT("stationInteractionCalls"), StationInteractionCalls);
    J->SetNumberField(TEXT("automaticRingAdmissionCount"), AutomaticRingAdmissionCount);
    J->SetNumberField(TEXT("explicitRingAdmissionCount"), ExplicitRingAdmissionCount);
    AMAYADeliveryParcel* Parcel = Director->GetParcel();
    UPrimitiveComponent* CargoMount = Robot->GetDeliveryCargoMount();
    UPrimitiveComponent* Carrier = CargoMount ? Cast<UPrimitiveComponent>(CargoMount->GetAttachParent()) : nullptr;
    if (HeavyReceiptDue) J->SetStringField(TEXT("cargoNativeTelemetry"), MAYAV01CaptureCargoPhysicsJson(Parcel, Carrier, Event));
    const FString InitializationEvidence = Parcel ? Parcel->GetTieDownInitializationEvidenceJson() : FString();
    const FString CommitEvidence = Director->GetLastServiceCommitEvidenceJson();
    const bool bInitializationChanged = InitializationEvidence != LastRecordedCargoInitializationEvidence;
    const bool bCommitChanged = CommitEvidence != LastRecordedServiceCommitEvidence;
    // Durable envelopes are saved on change, including explicit clearing, so repeated
    // 2 Hz rows do not consume the evidence budget with the same initialization.
    if (bInitializationChanged) J->SetStringField(TEXT("cargoTieDownInitializationEvidence"), InitializationEvidence);
    if (bCommitChanged) J->SetStringField(TEXT("serviceCommitEvidence"), CommitEvidence);
    double ParcelKg = 0;
    J->SetBoolField(TEXT("parcelMassKnown"), Director->GetParcel() && Director->GetParcel()->ObserveMass(ParcelKg));
    J->SetNumberField(TEXT("parcelMassKg"), ParcelKg);
    J->SetStringField(TEXT("parcelId"), Director->GetParcel() ? Director->GetParcel()->GetParcelId() : FString());
    if (Director->GetParcel() && Director->GetParcel()->GetParcelBody())
    {
        const auto* P = Director->GetParcel()->GetParcelBody();
        const FTransform Pose = P->GetComponentTransform();
        const FQuat Q = Pose.GetRotation();
        J->SetArrayField(TEXT("actualParcelPositionCm"), VJson(Pose.GetLocation()));
        J->SetArrayField(TEXT("actualParcelScale"), VJson(Pose.GetScale3D()));
        J->SetArrayField(TEXT("actualParcelQuaternion"), { MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
            MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W) });
        J->SetBoolField(TEXT("actualParcelSimulating"), P->IsSimulatingPhysics());
    }
    if (Phase == EPhase::ToPickup || Phase == EPhase::ToDropoff)
    {
        J->SetStringField(TEXT("serviceStationId"), ServiceStationId);
        J->SetArrayField(TEXT("routeApproachCm"), VJson(ServiceApproachCm));
        J->SetArrayField(TEXT("serviceStationCm"), VJson(ServiceStationCm));
        J->SetNumberField(TEXT("actualStationDistanceCm"), FVector::Dist2D(O.ChassisPose.GetLocation(), ServiceStationCm));
    }
    const FString Line = SerializeMayaV01Json(J).Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT("")) + TEXT("\n");
    const FTCHARToUTF8 UTF8(*Line, Line.Len());
    const uint64 Bytes = uint64(UTF8.Length()); // Same encoder as ForceUTF8WithoutBOM; includes newline.
    if (SampleCount >= MayaV01TelemetryRecordLimit || TelemetryBytes + Bytes > MayaV01TelemetryByteLimit)
    {
        bTelemetryCapped = true;
        TelemetryStatus = SampleCount >= MayaV01TelemetryRecordLimit ? TEXT("record_limit_reached") : TEXT("byte_limit_reached");
        return;
    }
    if (!TelemetryWriter || TelemetryWriter->IsError())
    { bTelemetryWriteFailed = true; TelemetryStatus = TEXT("file_writer_unavailable"); return; }
    TelemetryWriter->Serialize(const_cast<ANSICHAR*>(UTF8.Get()), UTF8.Length());
    TelemetryWriter->Flush(); // Complete rows become visible to FileShare.ReadWrite readers.
    if (!TelemetryWriter->IsError())
    {
        ++SampleCount; TelemetryBytes += Bytes; LastWrittenSampleWorldS = Now;
        if (bCashShiftTest && HeavyReceiptDue) LastCashShiftHeavyReceiptWorldS = Now;
        if (bAppliedImpactChanged) LastRecordedAppliedImpactEvidence = AppliedImpactEvidence;
        if (bInitializationChanged) LastRecordedCargoInitializationEvidence = InitializationEvidence;
        if (bCommitChanged) LastRecordedServiceCommitEvidence = CommitEvidence;
    }
    else { bTelemetryWriteFailed = true; TelemetryStatus = TEXT("file_write_or_flush_failed"); }
}

void AMAYAV01NativeTestDriver::CloseTelemetryWriter()
{
    if (!TelemetryWriter) return;
    TelemetryWriter->Flush();
    const bool bClosed = TelemetryWriter->Close();
    bTelemetryWriterClosed = bClosed && !TelemetryWriter->IsError();
    if (!bTelemetryWriterClosed)
    { bTelemetryWriteFailed = true; TelemetryStatus = TEXT("file_close_failed"); }
    TelemetryWriter.Reset();
}

void AMAYAV01NativeTestDriver::WriteReport(bool bTerminal, bool bSuccess, const FString& Reason)
{
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("schema"), TEXT("MayaRobotics.V01.NativeGame.v1"));
    O->SetStringField(TEXT("scope"), TEXT("Explicit native automation GAME integration; no manual focus/input or historical clearance certificate"));
    O->SetStringField(TEXT("runMode"), bReverseSteerTest ? TEXT("NativeReverseSteering") : bCashShiftTest ? TEXT("NativeCashShift") : bAirLoweringTest ? TEXT("NativeAirLoweringVideoRegression") : bCancelOpeningTest ? TEXT("NativeOpeningCancellation") : bRingInteractionTest ? TEXT("NativeRingInteraction") : bOperatingTest ? TEXT("NativeOperating") : bFullInputTrials ? TEXT("NativeFullInput")
        : bCargoVideoRequested ? TEXT("NativeAirPickupAndUnloadVideo") : bCargoDiagnosticTest ? TEXT("NativeCargoDiagnostic") : TEXT("NativeStandard"));
    O->SetBoolField(TEXT("airUnloadingActualFadePauseComplete"), bAirUnloadPauseComplete);
    O->SetBoolField(TEXT("airUnloadingExplicitCancellationTriggered"), bAirUnloadCancelTriggered);
    O->SetBoolField(TEXT("airUnloadingSameBodyRetryAdmitted"), bAirUnloadRetried);
    O->SetBoolField(TEXT("airLoweringTestRequested"), bAirLoweringTest);
    O->SetBoolField(TEXT("airLoweringActualPauseComplete"), bAirLoweringPauseComplete);
    O->SetBoolField(TEXT("airLoweringExplicitCancellationTriggered"), bAirLoweringCancelTriggered);
    O->SetBoolField(TEXT("airLoweringOneRetryAdmitted"), bAirLoweringTest && bOpeningCancelRetried);
    O->SetArrayField(TEXT("airLoweringRegressionReceipts"), AirLoweringRegressionReceipts);
    O->SetStringField(TEXT("airLoweringRegressionScope"), bAirLoweringTest
        ? TEXT("One real pickup mid-descent pause/cancel/empty closure/retry plus one actual unload upper-endpoint fade pause/cancel/same-body closed Carrying restoration/retry; ordinary one/two-parcel pickup and true reverse unloading video follows. Interruption does not simulate unsafe contact; actual pixels require independent review") : TEXT("not_requested"));
    O->SetBoolField(TEXT("openingCancellationTestRequested"), bCancelOpeningTest);
    O->SetBoolField(TEXT("openingCancellationTriggered"), bOpeningCancelTriggered);
    O->SetBoolField(TEXT("cancelledActualOpenEndpointsObserved"), bCancelledOpenObserved);
    O->SetBoolField(TEXT("oneActualCancelledServiceRetryAdmitted"), bOpeningCancelRetried);
    O->SetArrayField(TEXT("openingCancellationReceipts"), OpeningCancellationReceipts);
    O->SetStringField(TEXT("serviceCancellationEvidence"), Director ? Director->ReadServiceCancellationEvidenceJson() : FString());
    O->SetStringField(TEXT("openingCancellationScope"), bCancelOpeningTest
        ? TEXT("Exclusive explicit test-only interruption of actual first Loading/native Opening with zero bodies; real opening/restoration/closed-latch and one same-job retry are measured. No synthetic unsafe physics and no full-delivery/control/damage/video acceptance") : TEXT("not_requested"));
    O->SetBoolField(TEXT("reverseSteeringTestRequested"), bReverseSteerTest);
    O->SetArrayField(TEXT("reverseSteeringReceipts"), ReverseSteeringReceipts);
    if (auto G = MayaV01ReadJson(ReverseGroundEvidence)) O->SetObjectField(TEXT("reverseGroundAtReport"), G);
    O->SetStringField(TEXT("reverseSteeringScope"), TEXT("Exclusive rendered NativeAutomation diagnostic. One existing physical500cm corridor positioning/alignment precedes three short corrected shared-player-mapper segments: S+A, S+D, straight S. Legacy opposite paths were measured separately in game99/game100 on the same production mapper, whose failed diagnostic outcomes remain preserved. Drive-to-brake target is15degrees, final acceptance remains12degrees. No return/recenter stage is required. Each starts from its own continuous2s actual supported closed stop; no per-segment recenter, teleport, OS input or sensitivity/motor change. Forward and zero-throttle mapper identity are controlled source tests; old full-input evidence remains separate."));
    O->SetStringField(TEXT("reverseSteeringDeclaredBounds"), TEXT("Existing initial positioning25s/20cm corridor and heading10s/1degree remain; then120s total three-segment phase,250cm from actual staged entry,80cm route strip and four current nine-body-AABB corner physical static flat first-hit rays each frame. Each drive8s/150cm/30degree, curves>=5cm backward/>=0.5cm expected lateral/>=12degree expected nose yaw; straight>=40cm backward/<=5cm lateral/<=5degree yaw. Actual brake first supported stop within2s/100cm, then0.5s continuous support. Actual physical closed locks and closed poses remain mandatory during driving/braking. Stationary ClosedLatched plus stopped/support is required for2s segment setup and0.5s final settlement, including original2s first-stop bound. Original readiness/collision/reset/resources remain."));
    O->SetStringField(TEXT("reverseSteeringCaptureScope"), TEXT("Ordinary unchanged player camera reverse-before.png requested before; existing Stop requests terminal.png after. Requests are not processed-image or human input evidence."));
    O->SetBoolField(TEXT("cargoDiagnosticTestRequested"), bCargoDiagnosticTest);
    O->SetStringField(TEXT("initialMotionScope"), bReverseSteerTest
        ? TEXT("Dedicated three short corrected reverse steering segments after existing physical corridor positioning and alignment; no full-input/cargo/economy suite claim") : bCargoVideoRequested
        ? TEXT("Omitted in explicit presentation-only service video; normal depot stable baseline and real mission driving/service remain required. Full-control and recovery QA use separate nonvideo runs") : bRingInteractionTest
        ? TEXT("Dedicated depot-ring API regression: original motion/three-delivery/standalone lid/charge/full-input suites are omitted. Actual bounded moving unsafe request, physical exit/reentry, real six-wall-second world pause at active countdown and native automatic cargo pickup are required")
        : bOperatingTest
        ? TEXT("Operating regression begins from the measured supported depot; initial standard motion/reconstruction and standalone lid/ChargeSettle phases are skipped. Actual depletion, native depleted recovery and zero-cash no-free-charge/NewShift readiness are required; old free recharge and urgent-retry fixture is superseded")
        : bFullInputTrials
        ? TEXT("Full-input mode requires the original standard motion suite plus explicit full-command empty and two distinct secured one-kilogram parcel trials; manual keyboard/focus is not assessed")
        : bCargoDiagnosticTest
        ? TEXT("Diagnostic only: initial forward/reverse/turn/brake phases, post-motion reconstruction, standalone lid cycle and standalone charging are skipped; their readiness is not assessed by this run")
        : TEXT("Standard suite: initial forward/reverse/turn/brake phases and clearance-checked post-motion reconstruction are required"));
    O->SetBoolField(TEXT("standaloneChargingRequired"), false);
    O->SetBoolField(TEXT("initialUnfundedRingSetupRequired"), !bReverseSteerTest && !bCargoVideoRequested && !bCargoDiagnosticTest && !bOperatingTest && !bRingInteractionTest);
    O->SetBoolField(TEXT("postDeliveryPaidChargingRequired"), !bReverseSteerTest && !bOperatingTest && !bRingInteractionTest && !bDamageTest && !bCurbTest && !bCancelOpeningTest);
    O->SetBoolField(TEXT("operatingDepotRechargeRequired"), false);
    O->SetBoolField(TEXT("operatingUnfundedNewShiftRequired"), bOperatingTest);
    O->SetStringField(TEXT("nextJobSetupScope"), TEXT("Brake held with uninterrupted actual stopped/support, closed-latched and CanRequestNext production eligibility for 0.5 gameplay seconds within a twenty-second setup bound, then one request; ordinary and Operating NextJob branches share this setup. Production guards and race-refusal failure remain unchanged."));
    O->SetStringField(TEXT("stationServiceSetup"), bRingInteractionTest
        ? TEXT("Dedicated ring test performs one unsafe moving explicit API refusal, physical exit/reentry, measured stationary readiness, real active-countdown pause and no-interaction automatic pickup; the normal station-arrival setup is not assessed")
        : TEXT("Arrival latches at original 65 cm approach reach and holds continuous brake until actual native readiness; clears on phase or actual 90 cm parking-ring exit. Explicit service uses 0.5 s continuous measured stopped/support/latch plus unchanged native predicate within ten-second ring window; first pickup waits actual automatic service with no interaction calls. Original 120 cm service guard and 120 s phase bound retained"));
    O->SetStringField(TEXT("ringInteractionInputScope"), TEXT("ExplicitInteraction receipts in this driver are native API calls, not physical E keyboard/focus acceptance"));
    O->SetBoolField(TEXT("automaticRingPickupObserved"), bAutomaticRingPickupObserved);
    O->SetBoolField(TEXT("unexpectedAutomaticServiceObserved"), bUnexpectedAutomaticServiceObserved);
    O->SetNumberField(TEXT("automaticRingAdmissionCount"), AutomaticRingAdmissionCount);
    O->SetNumberField(TEXT("explicitRingAdmissionCount"), ExplicitRingAdmissionCount);
    O->SetNumberField(TEXT("stationInteractionCalls"), StationInteractionCalls);
    O->SetNumberField(TEXT("arrivalLatchExitCount"), ArrivalLatchExitCount);
    O->SetArrayField(TEXT("ringServiceReceipts"), RingServiceReceipts);
    for (const auto& Value : RingViewReceipts)
    {
        const auto View = Value->AsObject();
        View->SetBoolField(TEXT("fileExistsAtReport"), IFileManager::Get().FileExists(*View->GetStringField(TEXT("path"))));
        View->SetStringField(TEXT("visualAcceptance"), TEXT("not_assessed; actual PNG pixel review required"));
    }
    O->SetArrayField(TEXT("ringGameplayViewReceipts"), RingViewReceipts);
    if (Director) O->SetObjectField(TEXT("serviceRingAtReport"), MayaV01RingObservationJson(Director->GetRingObservation()));
    O->SetStringField(TEXT("standaloneChargingSetup"), bReverseSteerTest
        ? TEXT("Charging is not assessed; ordinary startup cash/fees/condition and zero cargo remain unchanged") : bCargoVideoRequested
        ? TEXT("Initial standalone charge is omitted; after one actual delivery the measured depot return verifies one funded automatic ten-second ring/$2 gain and fresh movement disconnect") : bRingInteractionTest
        ? TEXT("Omitted in dedicated ring interaction mode; charging is not assessed")
        : bOperatingTest
        ? TEXT("Standard standalone charge is omitted; actual depleted zero-cash recovery must refuse free energy and expose safe explicit NewShift after ten observed seconds")
        : TEXT("Named ChargeSettle holds existing actual support/latch for0.5 seconds, then proves zero-cash no-gain; after first verified delivery, physical depot return verifies automatic ten-second dwell, atomic$2 first gain, five-percent-per-second rate, fresh accepted movement disconnect and consumed-visit no-repeat"));
    O->SetBoolField(TEXT("standaloneLidCycleRequired"), !bReverseSteerTest && !bCargoVideoRequested && !bCargoDiagnosticTest && !bOperatingTest && !bRingInteractionTest);
    O->SetStringField(TEXT("lidAndCargoSetupScope"), bReverseSteerTest
        ? TEXT("Cargo transfer and standalone lid cycle are not assessed; actual empty support/closed physical locks remain required throughout reverse steering") : bCargoVideoRequested
        ? TEXT("Normal depot stable baseline then ordinary ring/manual service, native one/two-parcel smooth air reveal, swept descent and restraints, closed driving, actual opening/removal/closure and reward-once. No worker animation, recovery reconstruction or standalone mechanics; one actual post-delivery paid-ring observation is required") : bRingInteractionTest
        ? TEXT("Actual automatic pickup from the normally constructed depot after physical ring exit/reentry and countdown pause; native air reveal/descent, measured native settlement, closed latch, individual payloads and restraints are required. Recovered-depot fixture, standalone lid cycle and complete delivery loop are not assessed")
        : bOperatingTest
        ? TEXT("Operating mode ends after actual depletion/recovery plus zero-cash refusal and safe NewShift eligibility; superseded free-recharge/mission/urgent-retry phases are not claimed")
        : bCargoDiagnosticTest
        ? TEXT("Cargo diagnostic begins with mission acceptance from the normally constructed depot after two seconds of measured six-wheel stable support; actual mission lid/cargo service and driven pickup/dropoff retain all normal admission checks")
        : TEXT("Lid and subsequent cargo tests begin from the clearance-checked recovered depot fixture after two seconds of measured six-wheel stable support; post-turn service support is not accepted by this setup"));
    O->SetStringField(TEXT("engineVersion"), FApp::GetBuildVersion());
    O->SetStringField(TEXT("instantServiceViewStatus"), ServiceObservationViewStatus);
    O->SetBoolField(TEXT("instantServiceVideoRequested"), bCargoVideoRequested);
    O->SetNumberField(TEXT("instantServiceVideoClipCount"), ServiceVideoClipCount);
    O->SetNumberField(TEXT("instantServiceVideoRequestCoverageMask"), ServiceVideoRequestCoverageMask);
    O->SetStringField(TEXT("instantServiceVideoScope"), TEXT("Actual timestamped viewport requests for front-only and both-compartment loading and unloading; mask1/2=one-parcel loading/unloading,4/8=two-parcel loading/unloading. Request coverage does not establish JPEG write/pixel acceptance; inspect capture manifests and actual files before encoding"));
    O->SetArrayField(TEXT("instantServiceVideoReceipts"), ServiceVideoReceipts);
    O->SetNumberField(TEXT("instantServiceObservationViewCount"), ServiceObservationViewCount);
    O->SetStringField(TEXT("instantServiceObservationViewScope"), TEXT("Explicit AutoTest only: a fixed temporary elevated compartment camera and hidden HUD during real service, restored at phase exit/Stop/EndPlay; no player-input or regular-game camera acceptance"));
    O->SetBoolField(TEXT("visualCaptureRequested"), bVisualCaptureRequested);
    O->SetArrayField(TEXT("visualCaptureReceipts"), VisualCaptureReceipts);
    O->SetStringField(TEXT("visualAcceptance"), TEXT("not_assessed; independent actual PNG review required"));
    O->SetBoolField(TEXT("fullInputTrialsRequested"), bFullInputTrials);
    O->SetBoolField(TEXT("boostTestRequested"), bBoostTest);
    O->SetNumberField(TEXT("actualBoostDriveFrames"), ActualBoostDriveFrames);
    O->SetStringField(TEXT("boostTestScope"), TEXT("Boost requested only during existing full translation and open/closing zero-motor probes; actual motor usage/peak measured for every empty/loaded forward/reverse phase. Paired nonboost run required for achieved-speed comparison; no keyboard Shift acceptance."));
    if (Director)
    {
        O->SetObjectField(TEXT("lastAppliedImpact"), MAYADeliveryImpactReceiptToJson(Director->Snapshot().LastImpact));
        O->SetArrayField(TEXT("appliedImpactHistory"), MAYADeliveryImpactHistoryToJson(Director->Snapshot().ImpactHistory));
    }
    O->SetStringField(TEXT("appliedImpactEvidenceScope"), TEXT("Read-only typed actual applied GAME receipts; latest/changed successful rows and bounded latest64 final receipts include source generation, episode, world time, impulse/drop/approach and actual condition delta. No synthetic event or calibrated curb claim."));
    O->SetStringField(TEXT("cargoServiceScope"), TEXT("Pickup reveals both boxes simultaneously with actual translucent opacity for one gameplay second, box bottoms60.96cm above current open native vehicle bounds, then two-second eased swept vertical descent. Unloading reverses the animation with two-second swept rise to the same captured open-vehicle clearance, actual native upper-pose wait, one-second smooth fade to zero, then reversible physical removal and closed-latch commit. Actual separate IDs/native bodies/masses/independent restraints, all-slot closed custody and reward-once removal required. Worker animation and old continuous transfer-floor negative are superseded."));
    O->SetArrayField(TEXT("parcelInventoryAtReport"), ReadParcelInventoryJson());
    O->SetBoolField(TEXT("invalidSlotEndpointNegativeObserved"), bBlockedPathChecked);
    O->SetNumberField(TEXT("carriedRecoveryVerifiedCountMask"), CarriedRecoveryCountMask);
    O->SetStringField(TEXT("carriedRecoveryCountScope"), bCargoVideoRequested ? TEXT("Not assessed; explicit presentation video omits carried recovery so pickup proceeds to actual unloading. Separate ordinary/full-input count QA retains both recovery counts") : TEXT("Ordinary three-profile suite requires actual same-actor/native-mass reconstruction and reload for counts one and two; separate operating failed-job recovery retains its own scope."));
    O->SetArrayField(TEXT("carriedRecoveryPreflightReceipts"), CarriedRecoveryPreflightReceipts);
    O->SetStringField(TEXT("independentFullSteerCenteringScope"), TEXT("Before each full left/right trial, actual leased forward/reverse commands follow the measured route center with80cm lookahead; no teleport. Twenty-five-second/250cm setup bound, original global20cm corridor/physical locks/custody retained. Brake latch targets<=1cm/0.5degree, then requires measured supported closed stop within2cm lateral/1degree heading continuously0.5s before original two-second bounded rest. Actual center/direction/pose recorded; game44 original failure preserved."));
    O->SetStringField(TEXT("fullInputSetupEnvelope"), TEXT("Setup reaches the measured 500 cm corridor anchor within twenty centimetres, latches brake, and records the actual stopped starting pose; approach floor 0.15, cap 0.35 and near-anchor three-degree steering deadband. Heading retains one-degree target and ten-second bound with latched brake and bounded 0.65-1 steering outside target. Original twenty-five-second positioning, moving containment, actual signed full travel/speed, full steering, brake and custody criteria are unchanged."));
    O->SetStringField(TEXT("fullInputMovingDoorContainment"), TEXT("During moving trials: actual ClosedLatched state, full hinge pose within unchanged one-degree closed tolerance, exact native valid/unbroken closed-lock settings, finite current rates; rates are reported without applying the stationary 0.1 rad/s service-settling ceiling. Service and charge retain the original stationary qualifier."));
    O->SetStringField(TEXT("fullInputEnvelope"), TEXT("Opt-in +/-1 throttle and steering; 200 cm signed movement, 250 cm displacement bound, unchanged 2 second/100 cm brake; each signed full-steer trial begins after two continuous gameplay seconds with production stopped/support predicates, brake held and maximum 1cm translation/1degree yaw excursion at every postphysics observation (bounded rest, not exact zero motion); matched measured straight city corridor, actual empty vs two distinct secured one-kilogram parcels; measured peak is not terminal maximum or an enforced speed cap"));
    O->SetBoolField(TEXT("operatingTestsRequested"), bOperatingTest);
    O->SetStringField(TEXT("operatingScope"), TEXT("Actual normal low-charge drain, possible ordinary automatic pickup, failed zero-motor hold, same native recovery, zero-cash no-free-charge and safe explicit NewShift eligibility. Superseded free depot recharge, post-recharge service and urgent deadline/retry are not assessed; paid charge/deadline/pause rules have separate controlled tests. No manual key/focus or forced runtime energy/tick changes"));
    O->SetStringField(TEXT("operatingInitialAutomaticServiceGeneration"), LexToString(OperatingInitialAutomaticServiceGeneration));
    O->SetStringField(TEXT("ringCountdownPauseAndExitRuntimeCoverage"), bRingInteractionTest
        ? TEXT("Explicit real depot-ring physical exit/reentry and active-countdown actual world pause; scope follows typed receipts, no manual keyboard acceptance")
        : TEXT("not_assessed by this run; ring state unit tests and partial-lid real world pauses are distinct coverage"));
    O->SetBoolField(TEXT("ringInteractionTestRequested"), bRingInteractionTest);
    O->SetBoolField(TEXT("ringEarlyMovingInteractionRejected"), bRingEarlyInteractionRejected);
    O->SetBoolField(TEXT("ringPhysicalExitResetObserved"), bRingExitResetObserved);
    O->SetBoolField(TEXT("ringPhysicalReentryResetObserved"), bRingReentryResetObserved);
    O->SetBoolField(TEXT("ringCountdownPauseCompleted"), bRingCountdownPauseComplete);
    O->SetArrayField(TEXT("ringInteractionTestReceipts"), RingInteractionReceipts);
    O->SetBoolField(TEXT("startupBatteryFixtureApplied"), Director && Director->IsNativeOperatingBatteryFixtureApplied());
    O->SetNumberField(TEXT("startupBatteryPercent"), Director ? Director->GetNativeOperatingInitialBatteryPercent() : -1);
    O->SetStringField(TEXT("startupBatteryFixtureId"), Director ? Director->GetNativeOperatingBatteryFixtureId() : FString());
    O->SetBoolField(TEXT("actualOpeningPauseCompleted"), bOpeningPauseComplete);
    O->SetBoolField(TEXT("actualClosingPauseCompleted"), bClosingPauseComplete);
    O->SetNumberField(TEXT("lastFailedHoldZeroMotorSamples"), OperatingZeroMotorSamples);
    O->SetBoolField(TEXT("terminal"), bTerminal); O->SetBoolField(TEXT("success"), bSuccess);
    O->SetStringField(TEXT("phase"), PhaseName(Phase)); O->SetStringField(TEXT("reason"), Reason);
    O->SetNumberField(TEXT("missionCount"), MissionCount); O->SetNumberField(TEXT("sampleCount"), SampleCount);
    O->SetNumberField(TEXT("telemetryCadenceSeconds"), MayaV01TelemetryCadenceS);
    O->SetStringField(TEXT("telemetryBoundaryPolicy"), TEXT("Full phase_exit and terminal_before_quiesce rows in addition to periodic 2 Hz"));
    O->SetNumberField(TEXT("telemetryByteLimit"), double(MayaV01TelemetryByteLimit));
    O->SetNumberField(TEXT("telemetryRecordLimit"), MayaV01TelemetryRecordLimit);
    O->SetNumberField(TEXT("telemetryBytesWritten"), double(TelemetryBytes));
    O->SetBoolField(TEXT("telemetryCapped"), bTelemetryCapped);
    O->SetBoolField(TEXT("telemetryWriteFailed"), bTelemetryWriteFailed);
    O->SetStringField(TEXT("telemetryStatus"), TelemetryStatus);
    O->SetStringField(TEXT("telemetryWriterPolicy"), TEXT("One native writer opened with FILEWRITE_AllowRead, full UTF-8 row flush; live readers require FileShare.ReadWrite"));
    O->SetBoolField(TEXT("telemetryWriterClosed"), bTelemetryWriterClosed);
    O->SetStringField(TEXT("actualServiceFailureDetail"), Director ? Director->GetLastServiceFailureDetail() : FString());
    O->SetStringField(TEXT("recoveryDetail"), Director ? Director->GetLastRecoveryDetail() : FString());
    O->SetStringField(TEXT("cargoWorkerEvidence"), Director ? Director->ReadCargoWorkerEvidenceJson() : FString());
    O->SetStringField(TEXT("crowdObstacleEvidence"), Director ? Director->ReadCrowdObstacleEvidenceJson() : FString());
    O->SetBoolField(TEXT("chargingAtReport"), Director && Director->Snapshot().bCharging);
    if (Director) O->SetObjectField(TEXT("shiftEconomy"), MAYADeliveryEconomyToJson(Director->Snapshot()));
    O->SetBoolField(TEXT("initialUnfundedNoGainObserved"), bInitialUnfundedNoGainObserved);
    O->SetBoolField(TEXT("cashShiftTestRequested"), bCashShiftTest);
    O->SetBoolField(TEXT("actualCashShiftWinObserved"), bActualCashShiftWinObserved);
    O->SetArrayField(TEXT("cashShiftDeliveryReceipts"), CashShiftDeliveryReceipts);
    O->SetBoolField(TEXT("naturalOpeningRetryRequested"), bCashShiftTest || bTrafficEncounterTest);
    O->SetBoolField(TEXT("naturalOpeningRetryUsed"), bCashNaturalRetryUsed);
    O->SetBoolField(TEXT("naturalOpeningRetryAdmitted"), bCashNaturalRetryAdmitted);
    O->SetBoolField(TEXT("naturalOpeningRetrySecured"), bCashNaturalRetrySecured);
    O->SetStringField(TEXT("naturalOpeningOriginalFailure"), CashNaturalOriginalFailure);
    O->SetStringField(TEXT("naturalOpeningOriginalCancellation"), CashNaturalCancellationEvidence);
    O->SetArrayField(TEXT("naturalOpeningRetryReceipts"), CashNaturalOpeningRetryReceipts);
    O->SetStringField(TEXT("naturalOpeningRetryScope"), (bCashShiftTest || bTrafficEncounterTest) ? TEXT("At most one total normal RequestInteract after actual empty Opening angular stop breach, same-token real open/closed cancellation, unchanged identity/cash/fee/condition, twenty-second brake setup and two continuous stopped/native-ready seconds with <=1cm/1degree observed pose excursion. Original cancellation stays failed in phase receipt. Other/second cancellation or refused retry fails. Idle drain/deadline continue; no resource or production state writes.") : TEXT("not_requested"));
    O->SetNumberField(TEXT("verifiedDeliveryEarningsDollars"), CashShiftDeliveryEarnings);
    O->SetStringField(TEXT("cashShiftScope"), bCashShiftTest ? TEXT("Real1/1/2 job cycle until first current net>=100, actual closed custody and exact payouts, funded automatic ring/fee/movement stop, original count1/2 recovery, no resource setters. Twelve-job/1200 gameplay-second bounds; original64MiB retained. Full heavy physics/current-init receipts at phase boundaries and5-second cadence, typed cash/energy/per-slot native pose/ID/mass/restraint every2Hz row and original acceptance every tick. Terminal viewport must be independently inspected for ShiftWin presentation.") : TEXT("not_requested"));
    O->SetBoolField(TEXT("actualPaidRingChargeObserved"), bPaidRingChargeObserved);
    O->SetBoolField(TEXT("actualFreshMovementStoppedCharging"), bPaidMovementStopObserved);
    O->SetArrayField(TEXT("paidChargingReceipts"), PaidChargeReceipts);
    if (Director) O->SetObjectField(TEXT("lastChargeDisconnect"), MAYADeliveryChargeDisconnectToJson(Director->Snapshot().LastChargeDisconnect));
    AMAYADeliveryParcel* Parcel = Director ? Director->GetParcel() : nullptr;
    UPrimitiveComponent* CargoMount = Robot ? Robot->GetDeliveryCargoMount() : nullptr;
    UPrimitiveComponent* Carrier = CargoMount ? Cast<UPrimitiveComponent>(CargoMount->GetAttachParent()) : nullptr;
    O->SetStringField(TEXT("cargoNativeTelemetryAtReport"), MAYAV01CaptureCargoPhysicsJson(Parcel, Carrier, TEXT("report")));
    O->SetStringField(TEXT("cargoTieDownInitializationEvidence"), Parcel ? Parcel->GetTieDownInitializationEvidenceJson() : FString());
    O->SetStringField(TEXT("serviceCommitEvidence"), Director ? Director->GetLastServiceCommitEvidenceJson() : FString());
    O->SetStringField(TEXT("durableCargoEvidencePolicy"), TEXT("Initialization and commit envelopes on changed successful telemetry rows and in every report; current native cargo readback on every sampled row"));
    O->SetBoolField(TEXT("physicalDamageDiagnosticRequested"), bDamageTest);
    if (DamageFixture) O->SetStringField(TEXT("actualDamageFixtureEvidence"), DamageFixture->ReadEvidenceJson(true));
    if (bDamageTest)
    {
        O->SetStringField(TEXT("runMode"), TEXT("NativePhysicalDamageDiagnostic"));
        O->SetStringField(TEXT("initialMotionScope"), TEXT("Named impact-fixture approach/backoff and real depot return; standard initial motion/turn/reconstruction suite omitted"));
        O->SetBoolField(TEXT("standaloneChargingRequired"), false); O->SetBoolField(TEXT("standaloneLidCycleRequired"), false);
        O->SetStringField(TEXT("standaloneChargingSetup"), TEXT("Not assessed in physical damage diagnostic"));
        O->SetStringField(TEXT("lidAndCargoSetupScope"), TEXT("Actual ordinary one-parcel pickup and delivery service; no standalone lid cycle or carried recovery"));
        O->SetStringField(TEXT("carriedRecoveryCountScope"), TEXT("Not assessed in named physical damage diagnostic"));
        O->SetStringField(TEXT("physicalDamageDiagnosticScope"), TEXT("Exclusive AutoTest+DamageTest; actual named static UBox object on nine sampled pavement points. Empty contact control, measured small/hard loaded source events, neutral contact holds and actual delivery. No synthetic damage/event/impulse/time setters or production geometry changes. City static cross-section is observation only; no1percent curb-landing acceptance."));
    }
    if (bDamageCalibration)
    {
        O->SetStringField(TEXT("runMode"), TEXT("NativeEmptyContactCalibration"));
        O->SetBoolField(TEXT("physicalDamageCalibrationRequested"), true);
        O->SetBoolField(TEXT("cargoDamageBehaviorAccepted"), false);
        O->SetBoolField(TEXT("nativeContactCalibrationMeasurementsComplete"), bSuccess && DamageFixture && DamageFixture->GetCompletedTrialCount() == 2);
        O->SetStringField(TEXT("lidAndCargoSetupScope"), TEXT("Empty robot only; cargo service, loaded damage and delivery are not assessed"));
        O->SetStringField(TEXT("physicalDamageDiagnosticScope"), TEXT("Explicit AutoTest+DamageTest+DamageCalibration: two real empty-robot small/hard approaches to the same blocking object, actual per-body contact callbacks, neutral hold and backoff with100percent condition and unchanged empty applied history. Positive moving lateral contact ends the measurement; production damage rules are unchanged by calibration mode. This is calibration evidence, not cargo-damage or city-curb acceptance."));
    }
    O->SetBoolField(TEXT("originalCityCurbDiagnosticRequested"), bCurbTest);
    if (CurbTrial) O->SetStringField(TEXT("actualOriginalCityCurbEvidence"), CurbTrial->ReadEvidenceJson(true));
    if (bCurbTest)
    {
        O->SetStringField(TEXT("runMode"), TEXT("NativeOriginalCityCurbDiagnostic"));
        O->SetStringField(TEXT("initialMotionScope"), TEXT("Original route approach and actual ninety-degree upper-sidewalk alignment; standard full-input/motion phases omitted"));
        O->SetBoolField(TEXT("standaloneChargingRequired"), false); O->SetBoolField(TEXT("standaloneLidCycleRequired"), false);
        O->SetStringField(TEXT("standaloneChargingSetup"), TEXT("Not assessed in original-city curb diagnostic"));
        O->SetStringField(TEXT("lidAndCargoSetupScope"), TEXT("Actual ordinary automatic pickup of one independent1kg parcel, all endpoint/settlement/restraint/lid gates retained"));
        O->SetStringField(TEXT("stationServiceSetup"), TEXT("Original measured depot ring automatic pickup; no curb service or timer override"));
        O->SetStringField(TEXT("carriedRecoveryCountScope"), TEXT("Actual same-ID/body one-parcel reconstruction after supported original-road stop; two-parcel reconstruction covered separately"));
        O->SetStringField(TEXT("originalCityCurbScope"), TEXT("Exclusive rendered AutoTest+CurbTest; fresh physical first-hit survey of original sidewalk/curb/road, actual native leased crossing and exactly1total condition point with one measured landing episode, supported neutral road hold and original authoritative reconstruction. No manufactured surfaces, teleports, force/event setters, ordinary delivery/full-control or packaged-release claims."));
    }
    O->SetBoolField(TEXT("realTrafficEncounterRequested"), bTrafficEncounterTest);
    if (bTrafficEncounterTest) O->SetNumberField(TEXT("actualNativeDriverTickGroup"), int32(PrimaryActorTick.GetActualTickGroup()));
    if (TrafficEncounterTrial) O->SetStringField(TEXT("actualOriginalTrafficEncounter"), TrafficEncounterTrial->ReadEvidenceJson(true));
    if (bTrafficEncounterTest)
    {
        O->SetStringField(TEXT("runMode"), TEXT("NativeTrafficEncounter"));
        O->SetStringField(TEXT("carriedRecoveryCountScope"), TEXT("One ordinary UI recovery after matched original-car native contact; same actual one-parcel body/ID/mass and native stock endpoint. No contact means no collision PASS."));
        O->SetStringField(TEXT("realTrafficEncounterScope"), TEXT("One genuine pickup and unchanged accepted original-city curb entry; bounded leased native oncoming drive toward an actual entity/actor, passive physical/camera receipts and one production-gated recovery. No spawned car, traffic/controller/physics/resource mutation or human input claim; source sensor publication is not avoidance/contact acceptance."));
    }
    O->SetNumberField(TEXT("lastAttemptedSampleWorldSeconds"), LastSampleWorldS);
    O->SetNumberField(TEXT("lastWrittenSampleWorldSeconds"), LastWrittenSampleWorldS);
    O->SetNumberField(TEXT("runStartWorldSeconds"), RunStartWorldS);
    O->SetNumberField(TEXT("endWorldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
    O->SetArrayField(TEXT("phases"), PhaseResults); O->SetBoolField(TEXT("domainKnown"), false);
    O->SetStringField(TEXT("collisionSurvey"), TEXT("asset-shape-survey.json"));
    FFileHelper::SaveStringToFile(SerializeMayaV01Json(O), *FPaths::Combine(RunDirectory, TEXT("result.json")));
}

void AMAYAV01NativeTestDriver::Stop(bool bSuccess, const FString& Reason)
{
    if (bStopped) return;
    RestoreInstantServiceObservationView();
    CancelOwnedVisualCapture();
    if (Robot && Director)
    {
        FMAYADeliveryRobotObservation TerminalObservation;
        Robot->ReadDeliveryState(TerminalObservation);
        Record(TerminalObservation, true, TEXT("terminal_before_quiesce"));
    }
    bStopped = true;
    if (DamageFixture) DamageFixture->Cleanup();
    if (CurbTrial) CurbTrial->Cleanup();
    if (TrafficEncounterTrial) TrafficEncounterTrial->Cleanup();
    if (UGameplayStatics::IsGamePaused(this)) UGameplayStatics::SetGamePaused(this, false);
    bNativePauseActive = false; // Exit timer needs the actual world resumed, Director remains quiesced below.
    CloseTelemetryWriter();
    FString TerminalReason = Reason;
    if (bSuccess && bBoostTest && ActualBoostDriveFrames == 0)
    { bSuccess = false; TerminalReason += TEXT("; requested boost suite observed no actual boosted wheel drive"); }
    if (bTelemetryWriteFailed)
    {
        bSuccess = false;
        TerminalReason += TEXT("; native telemetry append failed, so evidence completion was not accepted");
    }
    if (Director) Director->SetPaused(true);
    if (Robot) { Robot->ReleaseDriveCommand(); Robot->SetDeliveryDriveLocked(true); }
    auto Failure = MakeShared<FJsonObject>(); Failure->SetStringField(TEXT("phase"), PhaseName(Phase));
    Failure->SetBoolField(TEXT("pass"), bSuccess); Failure->SetStringField(TEXT("reason"), TerminalReason);
    Failure->SetNumberField(TEXT("endWorldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
    PhaseResults.Add(MakeShared<FJsonValueObject>(Failure));
    if (bCashShiftTest && bSuccess)
    {
        // The real Win menu pauses the world. Observe its actual rendered frame
        // and saved PNG on the core ticker, which continues during that pause.
        const auto S = Director->Snapshot();
        CashWinFrozenCash = S.TotalCredits; CashWinFrozenFees = S.RechargeFeesPaidDollars;
        CashWinFrozenJobs = S.CompletedJobs; CashWinFrozenBattery = S.BatteryPercent;
        CashWinCompletionReason = TerminalReason;
        CashWinCapturePath = FPaths::Combine(RunDirectory, TEXT("terminal.png"));
        CashWinFinalizerStartedRealS = FPlatformTime::Seconds();
        CashWinRenderHandle = AHUD::OnHUDPostRender.AddUObject(this, &AMAYAV01NativeTestDriver::CashWinRendered);
        CashWinScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &AMAYAV01NativeTestDriver::CashWinScreenshotProcessed);
        CashWinTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &AMAYAV01NativeTestDriver::TickCashWinFinalizer));
        WriteReport(false, false, TEXT("Cash ledger complete; awaiting actual frozen ShiftWin HUD and processed PNG"));
        return;
    }
    WriteReport(true, bSuccess, TerminalReason);
    FScreenshotRequest::RequestScreenshot(FPaths::Combine(RunDirectory, TEXT("terminal.png")), true, false);
    // Normal exit is delayed one second so the requested frame and end-play cleanup can occur.
    FTimerHandle ExitTimer;
    GetWorld()->GetTimerManager().SetTimer(ExitTimer, []() { FPlatformMisc::RequestExit(false, TEXT("MayaV01NativeTestDriver")); }, 1.f, false);
}



void AMAYAV01NativeTestDriver::CashWinRendered(AHUD* HUD, UCanvas*)
{
    auto* P = GetWorld() ? Cast<AMAYADeliveryPlayerController>(GetWorld()->GetFirstPlayerController()) : nullptr;
    if (!P || HUD != P->GetHUD() || P->GetPlayerScreen() != EMAYAPlayerScreen::ShiftWin) return;
    UGameViewportClient* V = P->GetLocalPlayer() ? P->GetLocalPlayer()->ViewportClient.Get() : nullptr;
    if (!V || !V->Viewport) return;
    CashWinDrawFrame = GFrameCounter;
    CashWinViewportSize = V->Viewport->GetSizeXY();
}

void AMAYAV01NativeTestDriver::CashWinScreenshotProcessed()
{
    if (!bCashWinCaptureRequested) return;
    bCashWinCaptureProcessed = true; CashWinProcessedFrame = GFrameCounter;
}

bool AMAYAV01NativeTestDriver::TickCashWinFinalizer(float)
{
    const double Now = FPlatformTime::Seconds();
    if (Now - CashWinFinalizerStartedRealS > 15)
    { FinishCashWinFinalizer(false, TEXT("Actual ShiftWin HUD/capture exceeded fifteen real seconds")); return false; }
    auto* P = GetWorld() ? Cast<AMAYADeliveryPlayerController>(GetWorld()->GetFirstPlayerController()) : nullptr;
    if (!P || !Director || !Robot)
    { FinishCashWinFinalizer(false, TEXT("Cash win finalizer lost actual world/controller/robot")); return false; }
    const auto S = Director->Snapshot();
    if (!S.bShiftWon || !S.ShiftWin.bFrozen || S.ShiftInstanceId != CashShiftInstanceId
        || S.TotalCredits != CashWinFrozenCash || S.RechargeFeesPaidDollars != CashWinFrozenFees
        || S.CompletedJobs != CashWinFrozenJobs || S.BatteryPercent != CashWinFrozenBattery
        || S.bCharging || Director->GetParcelCount() != 0)
    { FinishCashWinFinalizer(false, TEXT("Actual win resources/custody changed while awaiting rendered menu")); return false; }
    if (P->GetPlayerScreen() != EMAYAPlayerScreen::ShiftWin || !UGameplayStatics::IsGamePaused(this)
        || CashWinDrawFrame == 0 || GFrameCounter - CashWinDrawFrame > 2
        || CashWinViewportSize.X < 960 || CashWinViewportSize.Y < 540) return true;
    if (!bCashWinCaptureRequested)
    {
        bCashWinCaptureRequested = true; CashWinRequestFrame = GFrameCounter;
        FScreenshotRequest::RequestScreenshot(CashWinCapturePath, true, false);
        return true;
    }
    if (!bCashWinCaptureProcessed) return true;
    TArray<uint8> Bytes;
    const bool Read = FFileHelper::LoadFileToArray(Bytes, *CashWinCapturePath);
    const auto UInt = [&](int32 O) { return (uint32(Bytes[O]) << 24) | (uint32(Bytes[O+1]) << 16) | (uint32(Bytes[O+2]) << 8) | uint32(Bytes[O+3]); };
    const uint8 Signature[] = {137,80,78,71,13,10,26,10};
    const bool Valid = Read && Bytes.Num() > 24 && FMemory::Memcmp(Bytes.GetData(), Signature, 8) == 0
        && UInt(16) == uint32(CashWinViewportSize.X) && UInt(20) == uint32(CashWinViewportSize.Y)
        && CashWinProcessedFrame >= CashWinRequestFrame;
    FinishCashWinFinalizer(Valid, Valid ? TEXT("Actual frozen ShiftWin HUD and engine-processed PNG verified; paused-world diagnostic exit uses real time")
        : TEXT("Processed ShiftWin capture lacks valid PNG/frame/viewport evidence"));
    return false;
}

void AMAYAV01NativeTestDriver::FinishCashWinFinalizer(bool bSuccess, const FString& Reason)
{
    if (CashWinRenderHandle.IsValid()) { AHUD::OnHUDPostRender.Remove(CashWinRenderHandle); CashWinRenderHandle.Reset(); }
    if (CashWinScreenshotHandle.IsValid()) { FScreenshotRequest::OnScreenshotRequestProcessed().Remove(CashWinScreenshotHandle); CashWinScreenshotHandle.Reset(); }
    auto J = MakeShared<FJsonObject>();
    J->SetBoolField(TEXT("success"), bSuccess); J->SetStringField(TEXT("reason"), Reason);
    J->SetBoolField(TEXT("humanPhysicalInputVerified"), false);
    J->SetNumberField(TEXT("cashDollars"), CashWinFrozenCash); J->SetNumberField(TEXT("feesDollars"), CashWinFrozenFees);
    J->SetNumberField(TEXT("completedJobs"), CashWinFrozenJobs); J->SetNumberField(TEXT("batteryPercent"), CashWinFrozenBattery);
    J->SetNumberField(TEXT("actualShiftWinDrawFrame"), CashWinDrawFrame); J->SetNumberField(TEXT("requestFrame"), CashWinRequestFrame);
    J->SetNumberField(TEXT("processedFrame"), CashWinProcessedFrame); J->SetNumberField(TEXT("viewportX"), CashWinViewportSize.X);
    J->SetNumberField(TEXT("viewportY"), CashWinViewportSize.Y); J->SetStringField(TEXT("capturePath"), CashWinCapturePath);
    J->SetNumberField(TEXT("elapsedRealSeconds"), FPlatformTime::Seconds() - CashWinFinalizerStartedRealS);
    J->SetStringField(TEXT("exitScope"), TEXT("Owned diagnostic RequestExit after actual processed win screenshot; not product Quit button"));
    const bool Saved = FFileHelper::SaveStringToFile(SerializeMayaV01Json(J), *FPaths::Combine(RunDirectory, TEXT("cash-win-ui-result.json")));
    WriteReport(true, bSuccess && Saved, CashWinCompletionReason + TEXT("; ") + Reason + (Saved ? TEXT("") : TEXT("; UI receipt write failed")));
    FPlatformMisc::RequestExit(false, TEXT("MayaCashWinRenderFinalizer"));
}

FString AMAYAV01NativeTestDriver::ReadServiceVideoFrameEvidenceJson(const FMAYADeliveryRobotObservation& O) const
{
    auto J = MakeShared<FJsonObject>(); const auto S = Director->Snapshot();
    J->SetStringField(TEXT("phaseAtRequest"), PhaseName(Phase)); J->SetNumberField(TEXT("missionPhaseAtRequest"), int32(S.Phase));
    J->SetStringField(TEXT("jobInstanceId"), S.JobInstanceId); J->SetNumberField(TEXT("jobParcelCount"), S.Job.ParcelCount);
    J->SetNumberField(TEXT("actualActorCount"), Director->GetParcelCount());
    J->SetNumberField(TEXT("actualSecuredCount"), Director->GetSecuredParcelCount());
    J->SetBoolField(TEXT("doorsOpen"), O.bDoorsOpen); J->SetBoolField(TEXT("doorsClosedLatched"), O.bDoorsClosedLatched);
    J->SetNumberField(TEXT("door1Degrees"), O.DoorAngleDegrees[0]); J->SetNumberField(TEXT("door2Degrees"), O.DoorAngleDegrees[1]);
    J->SetNumberField(TEXT("nativeObservationWorldSeconds"), O.ObservationWorldSeconds);
    J->SetBoolField(TEXT("airLowering"), Director->IsCargoAirLowering());
    J->SetBoolField(TEXT("airFadingOut"), Director->IsCargoAirFadingOut());
    J->SetBoolField(TEXT("airAnimationActive"), Director->IsCargoAirAnimationActive());
    J->SetNumberField(TEXT("airPhaseElapsedGameplaySeconds"), Director->GetCargoAirPhaseElapsedSeconds());
    J->SetNumberField(TEXT("credits"), S.TotalCredits); J->SetNumberField(TEXT("conditionPercent"), S.CargoConditionPercent);
    if (ServiceObservationCamera)
    {
        J->SetArrayField(TEXT("fixedCameraPositionCm"), VJson(ServiceObservationCamera->GetActorLocation()));
        J->SetStringField(TEXT("fixedCameraRotation"), ServiceObservationCamera->GetActorRotation().ToString());
    }
    TArray<TSharedPtr<FJsonValue>> Inventory;
    for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
    {
        auto P = MakeShared<FJsonObject>(); auto* Parcel = Director->GetParcel(Slot); FTransform Native; double Kg = 0;
        P->SetNumberField(TEXT("slot"), Slot); P->SetStringField(TEXT("compartment"), Slot == 0 ? TEXT("front_optical_minus_X") : TEXT("rear_plus_X"));
        P->SetBoolField(TEXT("actorValid"), IsValid(Parcel));
        if (Parcel)
        {
            P->SetStringField(TEXT("parcelId"), Parcel->GetParcelId()); P->SetStringField(TEXT("actor"), Parcel->GetPathName());
            P->SetBoolField(TEXT("dormantPendingRemoval"), Parcel->IsDormantForDeliveryRemoval());
            double Alpha = 0; bool Restored = false;
            const bool AppearanceKnown = Parcel->ObserveAirAppearance(Alpha, Restored);
            P->SetBoolField(TEXT("airLoweringActive"), Parcel->IsAirLoweringActive());
            P->SetBoolField(TEXT("airRaisingActive"), Parcel->IsAirRaisingActive());
            if (Parcel->IsAirRaisingActive())
            {
                P->SetStringField(TEXT("airRaisingTrajectoryBasis"), TEXT("Measured native bay tracking during eased rise; fixed parcel orientation; final upper pose/top latched before native wait/fade"));
                P->SetBoolField(TEXT("airRaisingUpperLatched"), Parcel->IsAirRaisingUpperLatched());
                P->SetNumberField(TEXT("airRaisingEasedProgress"), Parcel->GetAirRaisingEasedProgress());
                P->SetArrayField(TEXT("airRaisingReleasePositionCm"), VJson(Parcel->GetAirRaisingReleasePose().GetLocation()));
            }
            P->SetBoolField(TEXT("appearanceAlphaKnown"), AppearanceKnown);
            if (AppearanceKnown) P->SetNumberField(TEXT("actualAppearanceAlpha"), Alpha);
            P->SetBoolField(TEXT("originalMaterialsRestored"), Restored);
            if (Parcel->IsAirLoweringActive() || Parcel->IsAirRaisingActive())
            {
                P->SetArrayField(TEXT("airStartPositionCm"), VJson(Parcel->GetAirLoweringStartPose().GetLocation()));
                P->SetArrayField(TEXT("airTargetPositionCm"), VJson(Parcel->GetAirLoweringTargetPose().GetLocation()));
                P->SetNumberField(TEXT("actualOpenAssemblyTopCm"), Parcel->GetAirOpenAssemblyTopCm());
                P->SetNumberField(TEXT("stagedBottomClearanceCm"), 60.96);
            }
            const bool MassKnown = Parcel->ObserveMass(Kg); P->SetBoolField(TEXT("nativeMassKnown"), MassKnown);
            if (MassKnown) P->SetNumberField(TEXT("nativeMassKg"), Kg);
            const bool NativeKnown = Parcel->ObserveNativePose(Native); P->SetBoolField(TEXT("nativePoseKnown"), NativeKnown);
            if (NativeKnown) { P->SetArrayField(TEXT("nativePositionCm"), VJson(Native.GetLocation())); P->SetStringField(TEXT("nativeQuaternion"), Native.GetRotation().ToString()); }
        }
        Inventory.Add(MakeShared<FJsonValueObject>(P));
    }
    J->SetArrayField(TEXT("actualParcelsAtRequest"), Inventory);
    return SerializeMayaV01Json(J);
}

void AMAYAV01NativeTestDriver::UpdateInstantServiceObservationView(const FMAYADeliveryRobotObservation& O)
{
#if !UE_BUILD_SHIPPING
    if (Phase != EPhase::Loading && Phase != EPhase::Unloading)
    { RestoreInstantServiceObservationView(); return; }
    if (ServiceObservationCamera || !O.bKnown || O.ChassisPose.ContainsNaN()
        || VisualCapture || FScreenshotRequest::IsScreenshotRequested()) return;
    APlayerController* PC = Robot ? Cast<APlayerController>(Robot->GetController()) : nullptr;
    if (!PC || !PC->IsLocalController() || !IsValid(PC->GetViewTarget()) || !FApp::CanEverRender()) return;
    // Fixed world camera from actual chassis pose: both optical-minus-X front
    // and plus-X rear compartments are visible from above. No worker dependency.
    FVector LookAt = O.ChassisPose.TransformPosition(FVector(4.75, 0, 26.5));
    double HorizontalFov = 45;
    double HorizontalDistance = 145, VerticalDistance = 220;
    if (Phase == EPhase::Loading || Phase == EPhase::Unloading)
    {
        FBox Framing;
        if (!Robot->GetDeliveryCargoAirObservationBounds(Framing))
        { ServiceObservationViewStatus = TEXT("actual_air_and_lid_framing_bounds_unavailable"); return; }
        int32 ViewX = 0, ViewY = 0; PC->GetViewportSize(ViewX, ViewY);
        if (ViewX <= 0 || ViewY <= 0) return;
        HorizontalFov = 55;
        const double HalfVerticalFov = FMath::Atan(FMath::Tan(FMath::DegreesToRadians(HorizontalFov * 0.5)) / (double(ViewX) / ViewY));
        const double Distance = FMath::Max(300.0, 1.15 * Framing.GetExtent().Size() / FMath::Sin(HalfVerticalFov));
        LookAt = Framing.GetCenter(); HorizontalDistance = Distance * 0.82; VerticalDistance = Distance * 0.58;
    }
    const FVector Offsets[] = { FVector(10, -HorizontalDistance, VerticalDistance), FVector(10, HorizontalDistance, VerticalDistance),
        FVector(-HorizontalDistance, 10, VerticalDistance), FVector(HorizontalDistance, 10, VerticalDistance) };
    FVector Location = FVector::ZeroVector; bool FoundView = false;
    FCollisionQueryParams ViewQuery(SCENE_QUERY_STAT(MAYAInstantCargoView), false, Robot);
    for (int32 Slot = 0; Slot < Director->GetParcelCount(); ++Slot)
        if (auto* Parcel = Director->GetParcel(Slot)) ViewQuery.AddIgnoredActor(Parcel);
    for (const FVector& Offset : Offsets)
    {
        const FVector Candidate = LookAt + O.ChassisPose.TransformVectorNoScale(Offset);
        FHitResult ViewHit;
        if (!GetWorld()->SweepSingleByChannel(ViewHit, LookAt, Candidate, FQuat::Identity,
            ECC_Visibility, FCollisionShape::MakeSphere(8), ViewQuery))
        { Location = Candidate; FoundView = true; break; }
    }
    if (!FoundView) { ServiceObservationViewStatus = TEXT("actual_scene_blocks_all_declared_compartment_views"); return; }
    FActorSpawnParameters P; P.Owner = this;
    P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ServiceObservationCamera = GetWorld()->SpawnActor<ACameraActor>(Location, (LookAt - Location).Rotation(), P);
    if (!ServiceObservationCamera) { ServiceObservationViewStatus = TEXT("camera_spawn_failed"); return; }
    ServiceObservationCamera->GetCameraComponent()->SetFieldOfView(float(HorizontalFov));
    ServiceObservationCamera->GetCameraComponent()->bConstrainAspectRatio = false;
    ServiceViewController = PC; ServicePriorView = PC->GetViewTarget();
    PC->SetViewTarget(ServiceObservationCamera);
    if (PC->GetViewTarget() != ServiceObservationCamera)
    { ServiceObservationViewStatus = TEXT("camera_take_failed"); RestoreInstantServiceObservationView(); return; }
    if (AHUD* Hud = PC->GetHUD())
    {
        ServiceViewHud = Hud; bServicePriorHudVisible = Hud->bShowHUD;
        bServiceHudTaken = true; Hud->bShowHUD = false;
    }
    ServiceObservationViewStatus = TEXT("owned_fixed_compartment_service_view_active");
    bServiceVideoAttempted = false; ServiceVideoStartFrame = GFrameCounter;
    ServiceViewStartWorldS = GetWorld()->GetTimeSeconds();
    ++ServiceObservationViewCount;
#endif
}

void AMAYAV01NativeTestDriver::RestoreInstantServiceObservationView()
{
    if (!ServiceObservationCamera) return;
    if (ServiceVideo)
    {
        ServiceVideo->Finish(TEXT("actual_service_view_finished"));
        auto Receipt = MakeShared<FJsonObject>();
        Receipt->SetStringField(TEXT("label"), ActiveServiceVideoLabel);
        Receipt->SetStringField(TEXT("framesDirectory"), ServiceVideo->GetCaptureDirectory());
        Receipt->SetStringField(TEXT("captureManifest"), FPaths::Combine(ServiceVideo->GetCaptureDirectory(), TEXT("capture.json")));
        Receipt->SetNumberField(TEXT("requestedFrames"), ServiceVideo->GetRequestedFrames());
        Receipt->SetNumberField(TEXT("jobParcelCount"), ActiveServiceVideoParcelCount);
        Receipt->SetStringField(TEXT("serviceRole"), bActiveServiceVideoLoading ? TEXT("Loading") : TEXT("Unloading"));
        if (ServiceVideo->GetRequestedFrames() > 0 && (ActiveServiceVideoParcelCount == 1 || ActiveServiceVideoParcelCount == 2))
            ServiceVideoRequestCoverageMask |= 1 << ((ActiveServiceVideoParcelCount - 1) * 2 + (bActiveServiceVideoLoading ? 0 : 1));
        Receipt->SetNumberField(TEXT("viewStartWorldSeconds"), ServiceViewStartWorldS);
        Receipt->SetNumberField(TEXT("viewEndWorldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
        Receipt->SetStringField(TEXT("pixelAcceptance"), TEXT("not_assessed; inspect actual frame bytes/pixels before encoding"));
        ServiceVideoReceipts.Add(MakeShared<FJsonValueObject>(Receipt)); ServiceVideo.Reset();
    }
    bServiceVideoAttempted = false;
    bool Restored = false;
    if (ServiceViewController.IsValid() && ServicePriorView.IsValid()
        && ServiceViewController->GetViewTarget() == ServiceObservationCamera)
    {
        ServiceViewController->SetViewTarget(ServicePriorView.Get());
        Restored = ServiceViewController->GetViewTarget() == ServicePriorView.Get();
    }
    bool HudRestored = !bServiceHudTaken;
    if (bServiceHudTaken && ServiceViewHud.IsValid())
    { ServiceViewHud->bShowHUD = bServicePriorHudVisible; HudRestored = ServiceViewHud->bShowHUD == bServicePriorHudVisible; }
    ServiceObservationViewStatus = Restored && HudRestored ? TEXT("prior_camera_and_hud_restored") : TEXT("camera_or_hud_restore_not_confirmed");
    ServiceObservationCamera->Destroy(); ServiceObservationCamera = nullptr;
    ServiceViewController.Reset(); ServicePriorView.Reset(); ServiceViewHud.Reset(); bServiceHudTaken = false;
}

void AMAYAV01NativeTestDriver::CancelOwnedVisualCapture()
{
    if (!VisualCapture) return;
    VisualCapture->CancelCapture();
    auto Receipt = MakeShared<FJsonObject>();
    Receipt->SetStringField(TEXT("manifestPath"), VisualCapture->GetManifestPath());
    Receipt->SetStringField(TEXT("captureStatus"), VisualCapture->GetCaptureStatus());
    Receipt->SetBoolField(TEXT("allViewsSavedAndViewRestored"), VisualCapture->DidSaveAllViews());
    Receipt->SetStringField(TEXT("visualAcceptance"), TEXT("not_assessed"));
    VisualCaptureReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
    VisualCapture->Destroy(); VisualCapture = nullptr;
}

void AMAYAV01NativeTestDriver::EndPlay(EEndPlayReason::Type Reason)
{
    if (CashWinRenderHandle.IsValid()) AHUD::OnHUDPostRender.Remove(CashWinRenderHandle);
    if (CashWinScreenshotHandle.IsValid()) FScreenshotRequest::OnScreenshotRequestProcessed().Remove(CashWinScreenshotHandle);
    if (CashWinTickerHandle.IsValid()) FTSTicker::RemoveTicker(CashWinTickerHandle);
    RestoreInstantServiceObservationView();
    CancelOwnedVisualCapture();
    if (bNativePauseActive && GetWorld()) UGameplayStatics::SetGamePaused(this, false);
    bNativePauseActive = false;
    CloseTelemetryWriter();
    if (DamageFixture) { DamageFixture->Cleanup(); DamageFixture->Destroy(); DamageFixture = nullptr; }
    if (CurbTrial) { CurbTrial->Cleanup(); CurbTrial->Destroy(); CurbTrial = nullptr; }
    if (TrafficEncounterTrial) { TrafficEncounterTrial->Cleanup(); TrafficEncounterTrial->Destroy(); TrafficEncounterTrial = nullptr; }
    if (Robot) { Robot->ReleaseDriveCommand(); Robot->SetDeliveryDriveLocked(true); }
    Super::EndPlay(Reason);
}

bool AMAYAV01NativeTestDriver::CheckReverseSteeringGround(const FMAYADeliveryRobotObservation& O)
{
    FBox Bounds(ForceInit); int32 Count=0;
    TArray<UPrimitiveComponent*> Components; Robot->GetComponents<UPrimitiveComponent>(Components);
    for (UPrimitiveComponent* C:Components)
    {
        if (!C || !C->IsPhysicsCollisionEnabled()) continue;
        FBodyInstance* B=C->GetBodyInstance();
        if (B && B==C->GetBodyInstance(NAME_None,false) && B->IsValidBodyInstance())
        { Bounds+=B->GetBodyBounds(); ++Count; }
    }
    auto J=MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Rays;
    bool Clear=Count==9 && Bounds.IsValid!=0;
    if (Clear) for (double X:{Bounds.Min.X,Bounds.Max.X}) for (double Y:{Bounds.Min.Y,Bounds.Max.Y})
    {
        FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYAReverseSteeringGround),false,Robot);
        Q.MobilityType=EQueryMobilityType::Static; Q.AddIgnoredActor(this);
        FHitResult H;
        const bool Hit=GetWorld()->LineTraceSingleByChannel(H,FVector(X,Y,O.ChassisPose.GetLocation().Z+100),
            FVector(X,Y,O.ChassisPose.GetLocation().Z-300),ECC_Visibility,Q);
        UPrimitiveComponent* C=H.GetComponent(); auto* I=Cast<UInstancedStaticMeshComponent>(C);
        FBodyInstance* B=C && (!I || I->IsValidInstance(H.Item))?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
        const bool Physical=Hit && H.bBlockingHit && !H.bStartPenetrating && C
            && C->Mobility==EComponentMobility::Static && C->IsPhysicsCollisionEnabled()
            && C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block && B && B->IsValidBodyInstance()
            && H.ImpactNormal.Z>=.98;
        Clear=Clear && Physical; auto R=MakeShared<FJsonObject>();
        R->SetStringField(TEXT("component"),GetPathNameSafe(C)); R->SetNumberField(TEXT("instanceIndex"),H.Item);
        R->SetBoolField(TEXT("physicalFlatFirstHit"),Physical); R->SetBoolField(TEXT("startPenetrating"),H.bStartPenetrating);
        R->SetArrayField(TEXT("sampleXYCm"),VJson(FVector(X,Y,0)));
        if (Hit) { R->SetArrayField(TEXT("hitCm"),VJson(H.ImpactPoint)); R->SetArrayField(TEXT("normal"),VJson(H.ImpactNormal)); }
        Rays.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetNumberField(TEXT("independentNativeBodies"),Count); J->SetBoolField(TEXT("physicalGroundClear"),Clear);
    J->SetArrayField(TEXT("rays"),Rays); ReverseGroundEvidence=SerializeMayaV01Json(J);
    return Clear;
}

void AMAYAV01NativeTestDriver::TickReverseSteering(const FMAYADeliveryRobotObservation& O)
{
    const double Now=GetWorld()->GetTimeSeconds(); const auto S=Director->Snapshot();
    if (ReverseStageWorldS<0)
    {
        ReverseEntryCm=O.ChassisPose.GetLocation(); ReverseResetGeneration=O.ResetGeneration;
        ReverseStageWorldS=Now; ReverseStep=0;
        FScreenshotRequest::RequestScreenshot(FPaths::Combine(RunDirectory,TEXT("reverse-before.png")),true,false,false);
    }
    const auto& Route=Director->GetMeasuredRoute();
    if (Route.Num()<21 || Now-PhaseStartWorldS>120 || !O.bKnown || !O.bReady || !O.Fault.IsEmpty()
        || !O.bUpright || !Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()
        || O.SupportMask!=0x3f || O.ResetGeneration!=ReverseResetGeneration
        || S.Phase!=EMAYADeliveryPhase::Available || Director->GetParcelCount()!=0 || S.CompletedJobs!=0
        || S.TotalCredits!=0 || S.RechargeFeesPaidDollars!=0 || S.CargoConditionPercent!=100)
    { Stop(false,TEXT("Reverse steering diagnostic lost its unchanged empty native support/closed/identity/resource baseline or120second bound")); return; }
    const FVector Axis=(Route.Last()-Route[0]).GetSafeNormal2D(); const FVector Delta=O.ChassisPose.GetLocation()-Route[0];
    const double Along=FVector::DotProduct(Delta,Axis);
    const double Lateral=FVector(Delta.X-Axis.X*Along,Delta.Y-Axis.Y*Along,0).Size();
    if (FVector::Dist2D(ReverseEntryCm,O.ChassisPose.GetLocation())>250 || Lateral>80
        || Along < -60 || Along>FVector::Dist2D(Route[0],Route.Last())-60 || O.VelocityCmps.Size()>250
        || !CheckReverseSteeringGround(O))
    { Stop(false,TEXT("Reverse steering left its250cm original-entry/80cm route-strip/physical-flat-nine-body-footprint/speed envelope")); return; }

    static const TCHAR* Labels[]={TEXT("corrected_S_A_shared_mapper"),
        TEXT("corrected_S_D_shared_mapper"),TEXT("straight_S_shared_mapper")};
    const float Directional=ReverseSegmentIndex==0?-1.f:ReverseSegmentIndex==1?1.f:0.f;
    const float Mapped=MAYAPlayerDriveInput::BodyYawSteer(-1.f,Directional);
    if (ReverseStep==0)
    {
        Drive(0,0,true);
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (ReverseStableSinceWorldS<0) ReverseStableSinceWorldS=Now;
            if (Now-ReverseStableSinceWorldS>=2)
            {
                ReverseStartPose=O.ChassisPose; ReverseStageWorldS=Now; ReverseStep=1;
                ReverseStableSinceWorldS=-1; ReverseLastDriveEvidence.Reset(); Record(O,true,TEXT("reverse_segment_start"));
            }
        }
        else ReverseStableSinceWorldS=-1;
        if (Now-ReverseStageWorldS>20) Stop(false,TEXT("Reverse segment did not reach continuous2second unchanged native supported stop within20seconds"));
        if (ReverseStep==0 || bStopped) return;
    }
    const FVector Move=O.ChassisPose.GetLocation()-ReverseStartPose.GetLocation();
    const double Forward=FVector::DotProduct(Move,-ReverseStartPose.GetUnitAxis(EAxis::X));
    const double Right=FVector::DotProduct(Move,-ReverseStartPose.GetUnitAxis(EAxis::Y));
    const double Yaw=FMath::FindDeltaAngleDegrees(ReverseStartPose.Rotator().Yaw,O.ChassisPose.Rotator().Yaw);
    if (FMath::Abs(Yaw)>30)
    { Stop(false,TEXT("Actual reverse arc or brake exceeded the declared30degree segment yaw envelope")); return; }
    if (ReverseStep==1)
    {
        bLastBoostRequested=false;
        bLastCommandAccepted=Robot->SubmitDriveCommand(-1.f,Mapped,false,EMAYADriveCommandOrigin::NativeAutomation,.25f,false);
        if (!bLastCommandAccepted) { Stop(false,TEXT("Shared player-direction physical diagnostic command was refused by the normal native lease/input guards")); return; }
        ReverseLastDriveEvidence=Robot->ReadTelemetryJson();
        const bool Straight=ReverseSegmentIndex==2;
        const double ExpectedYaw=Mapped;
        const double ExpectedRight=-Mapped;
        const bool Reached=Straight?Forward<=-40 && FMath::Abs(Yaw)<=5 && FMath::Abs(Right)<=5:
            Forward<=-5 && ExpectedYaw*Yaw>=15 && ExpectedRight*Right>=.5;
        if (FMath::Abs(Yaw)>30 || Forward>5 || FVector::Dist2D(ReverseStartPose.GetLocation(),O.ChassisPose.GetLocation())>150
            || Now-ReverseStageWorldS>8)
        { Stop(false,TEXT("Reverse arc insufficient or unsafe within8seconds/150cm/30degree bounds; no requested bounds were relaxed")); return; }
        if (Reached)
        {
            ReverseStep=2; ReverseStageWorldS=Now; ReverseBrakeTravelCm=0;
            ReverseLastBrakeCm=O.ChassisPose.GetLocation(); ReverseStableSinceWorldS=-1;
            Drive(0,0,true); Record(O,true,TEXT("reverse_segment_brake"));
        }
        return;
    }
    Drive(0,0,true);
    ReverseBrakeTravelCm+=FVector::Distance(ReverseLastBrakeCm,O.ChassisPose.GetLocation()); ReverseLastBrakeCm=O.ChassisPose.GetLocation();
    if (ReverseBrakeTravelCm>100 || (Now-ReverseStageWorldS>2 && (ReverseStableSinceWorldS<0 || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched)))
    { Stop(false,TEXT("Reverse segment native brake exceeded the unchanged2second/100cm supported-stop bound")); return; }
    if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
    {
        if (ReverseStableSinceWorldS<0) ReverseStableSinceWorldS=Now;
        if (Now-ReverseStableSinceWorldS>=.5)
        {
            auto J=MakeShared<FJsonObject>(); J->SetStringField(TEXT("segment"),Labels[ReverseSegmentIndex]);
            J->SetNumberField(TEXT("playerThrottle"),-1); J->SetNumberField(TEXT("playerDirectionalSteer"),Directional);
            J->SetNumberField(TEXT("submittedBodyYawSteer"),Mapped); J->SetBoolField(TEXT("sharedProductionMapperUsed"),true);
            J->SetStringField(TEXT("inputScope"),TEXT("Actual NativeAutomation lease through shared production player mapper, never OS/human key evidence; legacy comparison is separate game99/game100 evidence"));
            J->SetArrayField(TEXT("startPositionCm"),VJson(ReverseStartPose.GetLocation())); J->SetArrayField(TEXT("stoppedEndPositionCm"),VJson(O.ChassisPose.GetLocation()));
            J->SetArrayField(TEXT("initialOpticalForward"),VJson(-ReverseStartPose.GetUnitAxis(EAxis::X)));
            J->SetArrayField(TEXT("initialOpticalRight"),VJson(-ReverseStartPose.GetUnitAxis(EAxis::Y)));
            J->SetNumberField(TEXT("signedInitialOpticalForwardCm"),Forward); J->SetNumberField(TEXT("signedInitialOpticalRightCm"),Right);
            J->SetNumberField(TEXT("signedNoseYawDegrees"),Yaw); J->SetNumberField(TEXT("nativeBrakeTravelCm"),ReverseBrakeTravelCm);
            J->SetBoolField(TEXT("actualStoppedSupportedClosed"),O.IsStoppedAndSupported() && O.bDoorsClosedLatched);
            J->SetNumberField(TEXT("finalSupportMask"),O.SupportMask); J->SetStringField(TEXT("resetGeneration"),LexToString(O.ResetGeneration));
            if (auto N=MayaV01ReadJson(ReverseLastDriveEvidence)) J->SetObjectField(TEXT("lastActualDriveNativeTelemetry"),N);
            if (auto N=MayaV01ReadJson(Robot->ReadTelemetryJson())) J->SetObjectField(TEXT("stoppedNativeTelemetry"),N);
            if (auto N=MayaV01ReadJson(ReverseGroundEvidence)) J->SetObjectField(TEXT("physicalGroundAtStoppedEnd"),N);
            if (const auto* PC=GetWorld()->GetFirstPlayerController())
            { FVector P; FRotator R; PC->GetPlayerViewPoint(P,R); J->SetArrayField(TEXT("unchangedPlayerCameraPositionCm"),VJson(P)); J->SetArrayField(TEXT("unchangedPlayerCameraPitchYawRoll"),VJson(FVector(R.Pitch,R.Yaw,R.Roll))); }
            const bool Signs=ReverseSegmentIndex==2?Forward<=-40 && FMath::Abs(Yaw)<=5 && FMath::Abs(Right)<=5:
                Forward<=-5 && Mapped*Yaw>=12 && -Mapped*Right>=.5;
            J->SetBoolField(TEXT("actualMotionSignsAndSupportedStopPassed"),Signs); ReverseSteeringReceipts.Add(MakeShared<FJsonValueObject>(J));
            Record(O,true,TEXT("reverse_segment_stopped"));
            if (!Signs) { Stop(false,TEXT("Actual stopped reverse path/yaw did not retain its declared directional signs")); return; }
            if (++ReverseSegmentIndex>=3)
            {
                Stop(true,TEXT("Three short actual reverse segments establish corrected shared player-direction left/right rear paths, straight reverse and native supported stops; legacy comparison remains separately preserved game99/game100 evidence; no human input or broader handling claim")); return;
            }
            ReverseStep=0; ReverseStageWorldS=Now; ReverseStableSinceWorldS=-1;
        }
    }
    else ReverseStableSinceWorldS=-1;
}

void AMAYAV01NativeTestDriver::CaptureCollisionGeometry()
{
    if (bGeometryCaptured || !Robot) return; bGeometryCaptured = true;
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("scope"), TEXT("Actual loaded reviewed BodySetup and cooked Chaos convex data; native transfer sweeps remain authoritative"));
    TArray<TSharedPtr<FJsonValue>> Bodies;
    TArray<UStaticMeshComponent*> Components; Robot->GetComponents<UStaticMeshComponent>(Components);
    for (auto* Component : Components)
    {
        auto* Setup = Component->GetBodySetup();
        if (!Setup || Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision) continue;
        auto B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("component"), Component->GetPathName());
        B->SetStringField(TEXT("mesh"), GetPathNameSafe(Component->GetStaticMesh()));
        B->SetArrayField(TEXT("worldPositionCm"), VJson(Component->GetComponentLocation()));
        B->SetNumberField(TEXT("nativeMassKg"), Component->GetMass());
        B->SetArrayField(TEXT("nativeCOMWorldCm"), VJson(Component->GetCenterOfMass()));
        const auto* BI = Component->GetBodyInstance();
        const FVector Inertia = BI ? BI->GetBodyInertiaTensor() : FVector::ZeroVector;
        const bool InertiaKnown = BI && !Inertia.ContainsNaN() && Inertia.GetMin() > 0;
        B->SetBoolField(TEXT("nativeInertiaMassSpaceKnown"), InertiaKnown);
        if (InertiaKnown) B->SetArrayField(TEXT("nativeInertiaMassSpaceKgCm2"), VJson(Inertia));
        TArray<TSharedPtr<FJsonValue>> Convexes;
        for (const auto& Elem : Setup->AggGeom.ConvexElems)
        {
            auto C = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Vertices;
            FBox Bounds(ForceInit);
            for (const FVector& Vertex : Elem.VertexData)
            {
                const FVector Local = Elem.GetTransform().TransformPosition(Vertex);
                Bounds += Local;
                auto V = MakeShared<FJsonObject>(); V->SetArrayField(TEXT("meshLocalCm"), VJson(Local));
                V->SetArrayField(TEXT("worldCm"), VJson(Component->GetComponentTransform().TransformPosition(Local)));
                Vertices.Add(MakeShared<FJsonValueObject>(V));
            }
            C->SetArrayField(TEXT("vertices"), Vertices); C->SetArrayField(TEXT("localBoundsMinCm"), VJson(Bounds.Min));
            C->SetArrayField(TEXT("localBoundsMaxCm"), VJson(Bounds.Max));
            const auto& Cooked = Elem.GetChaosConvexMesh();
            C->SetBoolField(TEXT("cookedChaosConvexKnown"), Cooked.IsValid() && Cooked->NumVertices() >= 4
                && FMath::IsFinite(Cooked->GetVolume()) && Cooked->GetVolume() > 0);
            if (Cooked.IsValid())
            {
                TArray<TSharedPtr<FJsonValue>> CookedVertices;
                for (int32 V = 0; V < Cooked->NumVertices(); ++V)
                    CookedVertices.Add(MakeShared<FJsonValueArray>(VJson(Elem.GetTransform().TransformPosition(FVector(Cooked->GetVertex(V))))));
                C->SetArrayField(TEXT("cookedMeshLocalVerticesCm"), CookedVertices);
                C->SetNumberField(TEXT("cookedVolumeCm3"), Cooked->GetVolume());
                C->SetNumberField(TEXT("cookedMarginCm"), Cooked->GetMargin());
            }
            Convexes.Add(MakeShared<FJsonValueObject>(C));
        }
        B->SetArrayField(TEXT("convexes"), Convexes); Bodies.Add(MakeShared<FJsonValueObject>(B));
    }
    O->SetArrayField(TEXT("bodies"), Bodies);
    FFileHelper::SaveStringToFile(SerializeMayaV01Json(O), *FPaths::Combine(RunDirectory, TEXT("asset-shape-survey.json")));
}
