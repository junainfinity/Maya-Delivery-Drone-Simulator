#include "DDSN06RuntimeProbe.h"

#include "DDSCityRollingPawn.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

ADDSN06RuntimeProbe::ADDSN06RuntimeProbe()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void ADDSN06RuntimeProbe::BeginPlay()
{
    Super::BeginPlay();
    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    Pawn = PlayerController ? Cast<ADDSCityRollingPawn>(PlayerController->GetPawn()) : nullptr;
    FString Name = TEXT("N06-probe.csv");
    FParse::Value(FCommandLine::Get(), TEXT("N06ProbeName="), Name);
    bVisualProof = FParse::Param(FCommandLine::Get(), TEXT("N06VisualProof"));
    OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), Name);
    Csv = TEXT("scenario,scenario_s,frame_dt_s,x_cm,y_cm,z_cm,yaw_deg,body_pitch_deg,body_roll_deg,signed_speed_mps,support_count,support_mask,grounded,blocked,wheel0_deg,wheel1_deg,wheel2_deg,wheel3_deg,wheel4_deg,wheel5_deg,contact0_z_cm,contact1_z_cm,contact2_z_cm,contact3_z_cm,contact4_z_cm,contact5_z_cm,recovery_result,recovery_dx_cm,ledger_unchanged,recovery_status\n");
    SuppliedLedger.MissionInstanceId = TEXT("probe-481");
    SuppliedLedger.CargoOwner = TEXT("Robot");
    SuppliedLedger.StarIds = {TEXT("alpha"), TEXT("beta")};
    SuppliedLedger.ElapsedSeconds = 52.437;
    SuppliedLedger.Result = TEXT("Pending");
    LedgerBefore = SuppliedLedger.Fingerprint();
    UE_LOG(LogTemp, Display, TEXT("N06_PROBE_START pawn=%d output=%s"), Pawn.IsValid(), *OutputPath);
}

void ADDSN06RuntimeProbe::StartScenario()
{
    ++ScenarioIndex;
    ScenarioSeconds = 0.0f;
    if (!Pawn.IsValid() || ScenarioIndex >= 17)
    {
        Finish();
        return;
    }
    struct FCase { const TCHAR* Name; float Duration; FVector At; float Yaw; float Throttle; float Steer; bool Brake; bool Reset; };
    const FCase Cases[] = {
        {TEXT("flat_forward"), 2.4f, FVector(-600,-100,535), 0, 1, 0, false, true},
        {TEXT("flat_brake"), 0.9f, FVector::ZeroVector, 0, 1, 0, true, false},
        {TEXT("flat_reverse"), 1.8f, FVector::ZeroVector, 0, -1, 0, false, false},
        {TEXT("near_stop_pivot"), 1.4f, FVector(0,100,535), 0, 0, 1, false, true},
        {TEXT("rolling_turn"), 1.8f, FVector(-500,-200,535), 0, 1, 1, false, true},
        {TEXT("blocking_wall"), 2.0f, FVector(550,0,535), 0, 1, 0, false, true},
        {TEXT("three_cm_seam"), 2.5f, FVector(0,-320,535), 0, 1, 0, false, true},
        {TEXT("ten_deg_ramp"), 8.0f, FVector(0,390,535), 0, 1, 0, false, true},
        {TEXT("split_curb"), 1.5f, FVector(-380,230,535), 0, 1, 0, false, true},
        {TEXT("single_wheel_curb"), 1.5f, FVector(-580,296,535), 0, 1, 0, false, true},
        {TEXT("corner"), 1.5f, FVector(750,75,535), 90, 1, 0, false, true},
        {TEXT("low_overhang"), 2.0f, FVector(850,100,535), 0, 1, 0, false, true},
        {TEXT("side_obstacle"), 2.5f, FVector(850,-60,535), 0, 1, 0, false, true},
        {TEXT("ledge_fall"), 2.0f, FVector(1220,-200,535), 0, 1, 0, false, true},
        {TEXT("recovery_clear"), 0.2f, FVector(400,0,700), 0, 0, 0, false, true},
        {TEXT("recovery_start_fallback"), 0.2f, FVector::ZeroVector, 0, 0, 0, false, false},
        {TEXT("recovery_all_blocked"), 0.2f, FVector::ZeroVector, 0, 0, 0, false, false},
    };
    const FCase& Case = Cases[ScenarioIndex];
    ScenarioName = Case.Name;
    ScenarioDuration = Case.Duration;
    if (Case.Reset) { Pawn->ResetForProbe(Case.At, Case.Yaw); }
    if (bVisualProof) { Pawn->SetDiagnosticSideViewForProbe(ScenarioName == TEXT("single_wheel_curb")); }
    Pawn->SetTestControls(Case.Throttle, Case.Steer, Case.Brake);
    bRecoveryResult = false;
    if (ScenarioIndex == 14)
    {
        BeforeRecovery = Pawn->GetActorLocation();
        bRecoveryResult = Pawn->RequestRecovery(false, &SuppliedLedger);
    }
    else if (ScenarioIndex == 15)
    {
        SpawnPoseBlockers();
        BeforeRecovery = Pawn->GetActorLocation();
        bRecoveryResult = Pawn->RequestRecovery(false, &SuppliedLedger);
    }
    else if (ScenarioIndex == 16)
    {
        SpawnBlocker();
        BeforeRecovery = Pawn->GetActorLocation();
        bRecoveryResult = Pawn->RequestRecovery(false, &SuppliedLedger);
    }
    UE_LOG(LogTemp, Display, TEXT("N06_PROBE_CASE name=%s reset=%d throttle=%.1f steer=%.1f brake=%d recovery=%d ledgerUnchanged=%d"),
        *ScenarioName, Case.Reset, Case.Throttle, Case.Steer, Case.Brake, bRecoveryResult, SuppliedLedger.Fingerprint() == LedgerBefore);
}

void ADDSN06RuntimeProbe::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (ScenarioIndex < 0) { StartScenario(); }
    if (!Pawn.IsValid() || ScenarioIndex >= 17) { return; }
    const FDDSMotionSample& Sample = Pawn->GetMotionSample();
    const FVector At = Pawn->GetActorLocation();
    const TArray<float>& Wheels = Pawn->GetWheelDegrees();
    const float* Contacts = Pawn->GetWheelContactHeights();
    if (bVisualProof && ScenarioName == TEXT("ten_deg_ramp"))
    {
        if (!bCapturedUphill && At.X >= 250 && At.X < 300)
        {
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("N06-v11-ramp-uphill.png")), false, false);
            bCapturedUphill = true;
        }
        if (!bCapturedDownhill && At.X >= 850 && At.X < 900)
        {
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("N06-v11-ramp-downhill.png")), false, false);
            bCapturedDownhill = true;
        }
    }
    if (bVisualProof && ScenarioName == TEXT("single_wheel_curb") && !bCapturedSingleWheel)
    {
        int32 Raised = 0;
        for (int32 Index = 0; Index < 6; ++Index) { if (Contacts[Index] > 507.0f) { ++Raised; } }
        if (Raised == 1)
        {
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("N06-v11-single-wheel-contact-near-side.png")), false, false);
            bCapturedSingleWheel = true;
        }
    }
    Csv += FString::Printf(TEXT("%s,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%d,%s\n"),
        *ScenarioName, ScenarioSeconds, DeltaSeconds, At.X, At.Y, At.Z, Sample.HeadingDegrees, Sample.BodyPitchDegrees, Sample.BodyRollDegrees,
        Sample.SignedSpeedMetersPerSecond, Sample.SupportedWheels, Pawn->GetWheelSupportMask(), Sample.bGrounded, Sample.bBodyBlocked,
        Wheels[0], Wheels[1], Wheels[2], Wheels[3], Wheels[4], Wheels[5],
        Contacts[0], Contacts[1], Contacts[2], Contacts[3], Contacts[4], Contacts[5],
        bRecoveryResult, (At - BeforeRecovery).Size(), SuppliedLedger.Fingerprint() == LedgerBefore, *Pawn->GetRecoveryStatus());
    ScenarioSeconds += DeltaSeconds;
    if (ScenarioSeconds >= ScenarioDuration) { StartScenario(); }
}

void ADDSN06RuntimeProbe::SpawnBlocker()
{
    // One temporary occupancy fixture covers every recorded sidewalk pose and the start bay.
    RecoveryBlocker = NewObject<UBoxComponent>(this, TEXT("N06AllRecoveryPosesOccupied"));
    RecoveryBlocker->SetBoxExtent(FVector(1050, 500, 50));
    RecoveryBlocker->SetCollisionProfileName(TEXT("BlockAll"));
    RecoveryBlocker->SetWorldLocation(FVector(300,0,535));
    AddInstanceComponent(RecoveryBlocker);
    RecoveryBlocker->RegisterComponent();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(N06BlockerFixture), false, this);
    const bool bOccupied = GetWorld()->OverlapBlockingTestByChannel(Pawn->GetActorLocation(), Pawn->GetActorQuat(), ECC_Pawn,
        FCollisionShape::MakeBox(FVector(45,32.5,24)), Query);
    UE_LOG(LogTemp, Display, TEXT("N06_PROBE_BLOCKER origin=%s currentOccupied=%d"), *RecoveryBlocker->GetComponentLocation().ToString(), bOccupied);
}

void ADDSN06RuntimeProbe::SpawnPoseBlockers()
{
    int32 Index = 0;
    for (const FTransform& Pose : Pawn->GetRecentPoses())
    {
        UBoxComponent* Blocker = NewObject<UBoxComponent>(this, *FString::Printf(TEXT("N06RecentPoseOccupied%d"), Index++));
        Blocker->SetBoxExtent(FVector(5,5,5));
        Blocker->SetCollisionProfileName(TEXT("BlockAll"));
        Blocker->SetWorldLocation(Pose.GetLocation());
        AddInstanceComponent(Blocker);
        Blocker->RegisterComponent();
        RecoveryPoseBlockers.Add(Blocker);
    }
    UE_LOG(LogTemp, Display, TEXT("N06_PROBE_RECENT_OCCUPIED count=%d"), RecoveryPoseBlockers.Num());
}

void ADDSN06RuntimeProbe::Finish()
{
    if (Pawn.IsValid()) { Pawn->ClearTestControls(); }
    const bool bSaved = FFileHelper::SaveStringToFile(Csv, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TArray<FString> Lines;
    Csv.ParseIntoArrayLines(Lines);
    UE_LOG(LogTemp, Display, TEXT("N06_PROBE_END saved=%d rows=%d path=%s"), bSaved, Lines.Num() - 1, *OutputPath);
    FPlatformMisc::RequestExit(false);
}
