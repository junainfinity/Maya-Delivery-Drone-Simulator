#include "DDSN07RuntimeProbe.h"

#include "DDSCityRollingPawn.h"
#include "Camera/CameraTypes.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

ADDSN07RuntimeProbe::ADDSN07RuntimeProbe()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ADDSN07RuntimeProbe::BeginPlay()
{
    Super::BeginPlay();
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    Pawn = Controller ? Cast<ADDSCityRollingPawn>(Controller->GetPawn()) : nullptr;
    if (!Pawn)
    {
        UE_LOG(LogTemp, Error, TEXT("N07_PROBE no possessed rolling pawn"));
        FPlatformMisc::RequestExit(false);
        return;
    }
    FString Name = TEXT("N07-probe.csv");
    FParse::Value(FCommandLine::Get(), TEXT("N07ProbeName="), Name);
    Name = FPaths::GetCleanFilename(Name);
    OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Diagnostics"), Name);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
    Csv = TEXT("t,scenario,mode,switches,pawnXcm,pawnYcm,pawnZcm,speedMps,referenceMps,referenceValid,grounded,clearanceValid,beyond8,clearanceM,ray0M,ray1M,ray2M,ray3M,ray4M,ray5M,ray6M,ray7M,ray8M,cameraXcm,cameraYcm,cameraZcm,adapterPhase\n");
    Pawn->ResetForProbe(FVector(0, 0, 535), 0.0f);
    Pawn->SetTestControls(0.0f, 0.0f, false);
    UE_LOG(LogTemp, Display, TEXT("N07_PROBE_START output=%s pawn=%p controller=%p"), *OutputPath, Pawn.Get(), Controller);
}

void ADDSN07RuntimeProbe::AdvanceScenario()
{
    if (!Pawn) { return; }
    switch (++Step)
    {
    case 1:
        Scenario = TEXT("SensorForwardWall");
        Pawn->ResetForProbe(FVector(555, 0, 535), 0.0f);
        Pawn->ToggleCameraForProbe();
        break;
    case 2:
        Scenario = TEXT("SensorHeldNoRepeat");
        break;
    case 3:
        Scenario = TEXT("ChaseReturn");
        Pawn->ToggleCameraForProbe();
        break;
    case 4:
        Scenario = TEXT("SensorOpenClear");
        Pawn->ResetForProbe(FVector(0, 0, 535), 180.0f);
        Pawn->ToggleCameraForProbe();
        break;
    case 5:
        Scenario = TEXT("SensorUnavailableFixture");
        Pawn->SetClearanceInvalidForProbe(true);
        break;
    case 6:
        Scenario = TEXT("DriveActualSpeed");
        Pawn->SetClearanceInvalidForProbe(false);
        Pawn->ResetForProbe(FVector(0, 0, 535), 0.0f);
        Pawn->SetTestControls(1.0f, 0.0f, false);
        break;
    case 7:
        Scenario = TEXT("BrakeCurrentFrame");
        Pawn->SetTestControls(0.0f, 0.0f, true);
        break;
    case 8:
        Scenario = TEXT("AdapterAndC");
        Pawn->SetTestControls(0.0f, 0.0f, false);
        Pawn->AdvanceControlledAdapterForProbe();
        Pawn->ToggleCameraForProbe();
        break;
    case 9:
        Scenario = TEXT("AdapterPhaseContinues");
        Pawn->AdvanceControlledAdapterForProbe();
        break;
    case 10:
        Scenario = TEXT("RecoveryResetReference");
        Pawn->RequestRecovery(false);
        break;
    default:
        FFileHelper::SaveStringToFile(Csv, *OutputPath);
        UE_LOG(LogTemp, Display, TEXT("N07_PROBE_DONE rows=%d output=%s"), RowCount, *OutputPath);
        Pawn->ClearTestControls();
        SetActorTickEnabled(false);
        FPlatformMisc::RequestExit(false);
        break;
    }
    UE_LOG(LogTemp, Display, TEXT("N07_PROBE_SCENARIO step=%d name=%s view=%s switches=%d adapter=%d"),
        Step, *Scenario, Pawn->GetCameraMode() == EDDSN07CameraMode::Sensor ? TEXT("Sensor") : TEXT("Chase"),
        Pawn->GetCameraSwitchCount(), Pawn->GetControlledAdapterPhase());
}

void ADDSN07RuntimeProbe::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Pawn) { return; }
    Elapsed += DeltaSeconds;
    if (Elapsed >= (Step + 1) * 1.0f)
    {
        AdvanceScenario();
        if (!IsActorTickEnabled()) { return; }
    }
    FMinimalViewInfo CameraView;
    Pawn->CalcCamera(DeltaSeconds, CameraView);
    const FDDSN07TelemetrySample& Sample = Pawn->GetTelemetrySample();
    const FVector At = Pawn->GetActorLocation();
    Csv += FString::Printf(TEXT("%.6f,%s,%s,%d,%.4f,%.4f,%.4f,%.6f,%.6f,%d,%d,%d,%d,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n"),
        Elapsed, *Scenario, Pawn->GetCameraMode() == EDDSN07CameraMode::Sensor ? TEXT("Sensor") : TEXT("Chase"),
        Pawn->GetCameraSwitchCount(), At.X, At.Y, At.Z,
        Sample.SignedSpeedMetersPerSecond, Sample.ReferenceSpeedMetersPerSecond,
        Sample.bReferenceValid, Sample.bGrounded, Sample.bClearanceValid, Sample.bClearBeyondRange,
        Sample.FrontClearanceMeters,
        Sample.RayDistancesMeters[0], Sample.RayDistancesMeters[1], Sample.RayDistancesMeters[2],
        Sample.RayDistancesMeters[3], Sample.RayDistancesMeters[4], Sample.RayDistancesMeters[5],
        Sample.RayDistancesMeters[6], Sample.RayDistancesMeters[7], Sample.RayDistancesMeters[8],
        CameraView.Location.X, CameraView.Location.Y, CameraView.Location.Z,
        Pawn->GetControlledAdapterPhase());
    ++RowCount;
}
