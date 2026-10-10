#include "MAYADeliveryGameMode.h"
#include "MAYACityBlockRouteContent.h"
#include "MAYAPlayerNewShiftReloadTest.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryHUD.h"
#include "MAYANativeDemoPawn.h"
#include "Camera/CameraActor.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "TimerManager.h"
#if !UE_BUILD_SHIPPING
#include "MAYAV01NativeTestDriver.h"
#include "MAYACityBlockRouteSurvey.h"
#include "MAYACityBlockMissionTrial.h"
#include "MAYANavigationNativeTest.h"
#include "MAYABoostPowerNativeTest.h"
#endif

AMAYADeliveryGameMode::AMAYADeliveryGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    DefaultPawnClass = nullptr; // wait for loaded measured city support
    PlayerControllerClass = AMAYADeliveryPlayerController::StaticClass();
    HUDClass = AMAYADeliveryHUD::StaticClass();
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("DeliveryStreamingAnchor")));
    CityStreaming = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("DeliveryCityStreaming"));
    FStreamingSourceShape CityArea;
    CityArea.bUseGridLoadingRange = false;
    CityArea.Radius = 6000;
    CityStreaming->Shapes.Add(CityArea);
    CityBlockBootstrapStreaming = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("DeliveryBlockStationBootstrapStreaming"));
    FStreamingSourceShape BlockStationArea;
    BlockStationArea.bUseGridLoadingRange = false; BlockStationArea.Radius = 1200;
    BlockStationArea.Location = MAYACityBlockRouteContent::ServicePoint() - HistoricalDepot;
    CityBlockBootstrapStreaming->Shapes.Add(BlockStationArea);
    CityBlockBootstrapStreaming->DisableStreamingSource();
}

void AMAYADeliveryGameMode::BeginPlay()
{
    Super::BeginPlay();
    GetRootComponent()->SetWorldLocation(HistoricalDepot);
    BootstrapStartWorldS = GetWorld()->GetTimeSeconds();
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        && (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01VisualCapture"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CurbTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))))
    {
        StopBootstrap(TEXT("Operating test requires AutoTest and excludes CargoTest, FullInput, RingInteractionTest, VisualCapture, DamageTest, CurbTest, CancelOpeningTest and ObserveInput."));
        return;
    }
#endif
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01BlockRouteSurvey")))
    {
        bool bExclusive = FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"));
        const TCHAR* Forbidden[] = {TEXT("MayaV01CargoTest"), TEXT("MayaV01CurbTest"), TEXT("MayaV01TrafficEncounterTest"),
            TEXT("MayaV01OperatingTest"), TEXT("MayaV01DamageTest"), TEXT("MayaV01FullInput"), TEXT("MayaV01ReverseSteerTest"),
            TEXT("MayaV01CashShiftTest"), TEXT("MayaV01UIScreenTest"), TEXT("MayaV01ObserveInput"), TEXT("MayaV01NewShiftReloadTest"),
            TEXT("MayaV01RingInteractionTest"), TEXT("MayaV01BoostTest"), TEXT("MayaV01VisualCapture"), TEXT("MayaV01CancelOpeningTest"),
            TEXT("MayaV01CargoVideo"), TEXT("MayaV01AirLoweringTest"), TEXT("MayaV01DamageCalibration"), TEXT("MayaV01PackageUIScreenSmoke"), TEXT("MayaV01CityBlockMissionTest"), TEXT("MayaV01NavigationTest"), TEXT("nullrhi")};
        for (const TCHAR* Flag : Forbidden) bExclusive = bExclusive && !FParse::Param(FCommandLine::Get(), Flag);
        FString Fixture;
        bExclusive = bExclusive && !FParse::Value(FCommandLine::Get(), TEXT("MayaV01InitialBattery="), Fixture)
            && !FParse::Value(FCommandLine::Get(), TEXT("MayaV01CrashFixture="), Fixture);
#if !WITH_EDITOR || UE_BUILD_SHIPPING || UE_BUILD_TEST
        bExclusive = false;
#endif
        if (!bExclusive) { StopBootstrap(TEXT("Block route survey requires exclusive rendered editor Development AutoTest, without gameplay/resource fixtures.")); return; }
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01CityBlockMissionTest")))
    {
        bool bExclusive = FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest")) && FApp::CanEverRender();
        const TCHAR* Forbidden[] = {TEXT("MayaV01BlockRouteSurvey"), TEXT("MayaV01CargoTest"), TEXT("MayaV01CurbTest"), TEXT("MayaV01TrafficEncounterTest"),
            TEXT("MayaV01OperatingTest"), TEXT("MayaV01DamageTest"), TEXT("MayaV01FullInput"), TEXT("MayaV01ReverseSteerTest"),
            TEXT("MayaV01CashShiftTest"), TEXT("MayaV01UIScreenTest"), TEXT("MayaV01ObserveInput"), TEXT("MayaV01NewShiftReloadTest"),
            TEXT("MayaV01RingInteractionTest"), TEXT("MayaV01BoostTest"), TEXT("MayaV01VisualCapture"), TEXT("MayaV01CancelOpeningTest"),
            TEXT("MayaV01CargoVideo"), TEXT("MayaV01AirLoweringTest"), TEXT("MayaV01DamageCalibration"), TEXT("MayaV01PackageUIScreenSmoke"), TEXT("MayaV01NavigationTest"), TEXT("nullrhi")};
        for (const TCHAR* Flag : Forbidden) bExclusive = bExclusive && !FParse::Param(FCommandLine::Get(), Flag);
        FString Fixture;
        bExclusive = bExclusive && !FParse::Value(FCommandLine::Get(), TEXT("MayaV01InitialBattery="), Fixture)
            && !FParse::Value(FCommandLine::Get(), TEXT("MayaV01CrashFixture="), Fixture);
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
        bExclusive = false;
#endif
        if (!bExclusive) { StopBootstrap(TEXT("City block mission requires exclusive rendered Development AutoTest without resource or traffic fixtures.")); return; }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("MayaV01HUDStyleTest"))&&!FParse::Param(FCommandLine::Get(),TEXT("MayaV01NavigationTest")))
    {StopBootstrap(TEXT("HUD-style QA requires the exclusive rendered NavigationTest selector."));return;}
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01NavigationTest")))
    {
        bool bExclusive=FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))&&FApp::CanEverRender();
        const TCHAR* Forbidden[]={TEXT("MayaV01BlockRouteSurvey"),TEXT("MayaV01CityBlockMissionTest"),TEXT("MayaV01CargoTest"),TEXT("MayaV01CurbTest"),
            TEXT("MayaV01TrafficEncounterTest"),TEXT("MayaV01OperatingTest"),TEXT("MayaV01DamageTest"),TEXT("MayaV01FullInput"),TEXT("MayaV01ReverseSteerTest"),
            TEXT("MayaV01CashShiftTest"),TEXT("MayaV01UIScreenTest"),TEXT("MayaV01ObserveInput"),TEXT("MayaV01NewShiftReloadTest"),TEXT("MayaV01RingInteractionTest"),
            TEXT("MayaV01BoostTest"),TEXT("MayaV01VisualCapture"),TEXT("MayaV01CancelOpeningTest"),TEXT("MayaV01CargoVideo"),TEXT("MayaV01AirLoweringTest"),
            TEXT("MayaV01DamageCalibration"),TEXT("MayaV01PackageUIScreenSmoke"),TEXT("MayaV01WheelAppearance"),TEXT("MayaV01LoadingScreenCapture"),TEXT("MayaHUDCaptureLayouts"),TEXT("nullrhi")};
        for(const TCHAR* Flag:Forbidden)bExclusive=bExclusive&&!FParse::Param(FCommandLine::Get(),Flag);
        FString Fixture;bExclusive=bExclusive&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01InitialBattery="),Fixture)
            &&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),Fixture);
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
        bExclusive=false;
#endif
        if(!bExclusive){StopBootstrap(TEXT("Navigation QA requires exclusive rendered Development AutoTest without resource/traffic/gameplay fixtures."));return;}
    }
    if((FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostCurbOnly"))||FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostStaticBenchmark"))||FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostServiceOnly")))&&!FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest")))
    {StopBootstrap(TEXT("Separated Boost diagnostic scope requires exclusive BoostPowerTest"));return;}
    if(FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostLegacyComparison"))&&!FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest")))
    {StopBootstrap(TEXT("Previous boost recreation requires the exclusive BoostPowerTest selector"));return;}
    if(FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest")))
    {
        const bool CurbOnly=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostCurbOnly")),StaticBenchmark=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostStaticBenchmark")),ServiceOnly=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostServiceOnly")),Legacy=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostLegacyComparison"));
        const int32 Modes=int32(CurbOnly)+int32(StaticBenchmark)+int32(ServiceOnly);
        bool bExclusive=FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))&&FApp::CanEverRender()&&Modes<=1&&((CurbOnly||ServiceOnly)?!Legacy:Legacy);
        const TCHAR* Forbidden[]={TEXT("MayaV01BlockRouteSurvey"),TEXT("MayaV01CityBlockMissionTest"),TEXT("MayaV01NavigationTest"),TEXT("MayaV01HUDStyleTest"),
            TEXT("MayaV01CargoTest"),TEXT("MayaV01CurbTest"),TEXT("MayaV01TrafficEncounterTest"),TEXT("MayaV01OperatingTest"),TEXT("MayaV01DamageTest"),TEXT("MayaV01FullInput"),
            TEXT("MayaV01ReverseSteerTest"),TEXT("MayaV01CashShiftTest"),TEXT("MayaV01UIScreenTest"),TEXT("MayaV01ObserveInput"),TEXT("MayaV01NewShiftReloadTest"),
            TEXT("MayaV01RingInteractionTest"),TEXT("MayaV01BoostTest"),TEXT("MayaV01VisualCapture"),TEXT("MayaV01CancelOpeningTest"),TEXT("MayaV01CargoVideo"),TEXT("MayaV01AirLoweringTest"),
            TEXT("MayaV01DamageCalibration"),TEXT("MayaV01PackageUIScreenSmoke"),TEXT("MayaV01WheelAppearance"),TEXT("MayaV01LoadingScreenCapture"),TEXT("MayaHUDCaptureLayouts"),TEXT("nullrhi")};
        for(const TCHAR* Flag:Forbidden)bExclusive=bExclusive&&!FParse::Param(FCommandLine::Get(),Flag);
        FString Fixture;bExclusive=bExclusive&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01InitialBattery="),Fixture)&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),Fixture);
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
        bExclusive=false;
#endif
        if(!bExclusive){StopBootstrap(TEXT("Boost power QA requires exclusive rendered Development AutoTest with valid full/static legacy or ordinary curb/service scope; no resource or other test fixtures"));return;}
    }
    if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01BlockRouteSurvey"))) CityBlockBootstrapStreaming->EnableStreamingSource();
    CityStreaming->EnableStreamingSource();
    LoadingCamera = GetWorld()->SpawnActor<ACameraActor>(HistoricalDepot + FVector(450, 0, 230), FRotator(-20, 180, 0));
    if (DeliveryPlayer.IsValid() && LoadingCamera) DeliveryPlayer->SetViewTarget(LoadingCamera);
    NextBootstrapWorldS = GetWorld()->GetTimeSeconds() + 2.0;
}

void AMAYADeliveryGameMode::HandleStartingNewPlayer_Implementation(APlayerController* Player)
{
    DeliveryPlayer = Player;
    if (LoadingCamera) Player->SetViewTarget(LoadingCamera);
    if (bBootstrapped && Robot) Player->Possess(Robot);
}

bool AMAYADeliveryGameMode::TraceSidewalk(const FVector& XY, FHitResult& Hit) const
{
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYADeliveryCitySurface), false);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, FVector(XY.X, XY.Y, HistoricalDepot.Z + 180),
        FVector(XY.X, XY.Y, HistoricalDepot.Z - 150), ECC_Visibility, Query)
        || Hit.ImpactNormal.Z < 0.98 || FMath::Abs(Hit.ImpactPoint.Z - HistoricalDepot.Z) > 20) return false;
    UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Hit.Component.Get());
    return Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName().Contains(TEXT("Sidewalk"));
}

bool AMAYADeliveryGameMode::SurveyRoute(TArray<FVector>& Out) const
{
    FHitResult Start;
    if (!TraceSidewalk(HistoricalDepot, Start)) return false;
    TArray<FVector> Best;
    const FVector Directions[] = { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) };
    for (const FVector& Direction : Directions)
    {
        TArray<FVector> Candidate;
        Candidate.Add(Start.ImpactPoint);
        const FVector Side(-Direction.Y, Direction.X, 0);
        for (int32 I = 1; I <= 60; ++I)
        {
            FHitResult Center, Left, Right;
            const FVector P = Start.ImpactPoint + Direction * (I * 50.0);
            if (!TraceSidewalk(P, Center) || !TraceSidewalk(P + Side * 40, Left) || !TraceSidewalk(P - Side * 40, Right)
                || FMath::Abs(Center.ImpactPoint.Z - Candidate.Last().Z) > 2
                || FMath::Abs(Left.ImpactPoint.Z - Right.ImpactPoint.Z) > 2) break;
            FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYADeliveryRouteEnvelope), false);
            FHitResult Obstruction;
            const FVector RaisedStart = Candidate.Last() + FVector(0, 0, 35);
            const FVector RaisedEnd = Center.ImpactPoint + FVector(0, 0, 35);
            if (GetWorld()->SweepSingleByChannel(Obstruction, RaisedStart, RaisedEnd, Direction.Rotation().Quaternion(),
                ECC_Visibility, FCollisionShape::MakeBox(FVector(45, 32.5, 20)), Q)) break;
            Candidate.Add(Center.ImpactPoint);
        }
        if (Candidate.Num() > Best.Num()) Best = MoveTemp(Candidate);
    }
    if (Best.Num() < 13) return false; // three distinct reachable approach candidates, >=6m sampled corridor
    Out = MoveTemp(Best);
    return true;
}

bool AMAYADeliveryGameMode::ApplyCosmetics(AMAYANativeDemoPawn& P) const
{
    auto Add = [&](const FString& SourceRole, const FString& Path)
    {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
        if (!Mesh) return false;
        P.CosmeticMeshes.Add(FName(*SourceRole), Mesh);
        return true;
    };
    if (!Add(TEXT("SM_DeliveryRobot_Base"), TEXT("/Game/MAYA/StructuralShadingProof/Meshes/SM_MAYA_Base.SM_MAYA_Base"))
        || !Add(TEXT("SM_DeliveryRobot_Door1"), TEXT("/Game/MAYA/StructuralShadingProof/Meshes/SM_MAYA_Door1.SM_MAYA_Door1"))
        || !Add(TEXT("SM_DeliveryRobot_Door2"), TEXT("/Game/MAYA/StructuralShadingProof/Meshes/SM_MAYA_Door2.SM_MAYA_Door2"))) return false;
    for (int32 I = 0; I < 6; ++I)
    {
        const FString Suffix = I == 0 ? TEXT("") : FString::FromInt(I);
        for (const FString& Part : { FString(TEXT("Suspension")), FString(TEXT("Wheel")) })
        {
            const FString Asset = TEXT("SM_MAYA_") + Part + Suffix;
            if (!Add(TEXT("SM_DeliveryRobot_") + Part + Suffix,
                TEXT("/Game/MAYA/AcceptedComponentsProof/Meshes/") + Asset + TEXT(".") + Asset)) return false;
        }
    }
    return P.CosmeticMeshes.Num() == 15;
}

bool AMAYADeliveryGameMode::Bootstrap()
{
    TArray<FVector> Route;
    if (!DeliveryPlayer.IsValid()) { BootstrapStatus = TEXT("Waiting for the player..."); return false; }
    if (!CityStreaming->IsStreamingCompleted()) { BootstrapStatus = TEXT("Loading the depot neighbourhood..."); return false; }
    if (!SurveyRoute(Route)) { BootstrapStatus = TEXT("A clear sidewalk route could not be found near the depot."); return false; }
    bool bInstallCityBlock = !FParse::Param(FCommandLine::Get(), TEXT("MayaV01BlockRouteSurvey")) && CityBlockUnavailableReason.IsEmpty();
    FMAYACityBlockRouteDefinition CityBlockDefinition;
    FHitResult BlockServiceSurface, BlockStationSurface;
    auto BlockUnavailable = [&](const FString& Reason)
    {
        CityBlockUnavailableReason = Reason; bInstallCityBlock = false;
        CityBlockBootstrapStreaming->DisableStreamingSource();
        UE_LOG(LogTemp, Warning, TEXT("MAYA CITY BLOCK unavailable: %s; original three delivery offers retained"), *Reason);
    };
    if (bInstallCityBlock && !CityBlockBootstrapStreaming->IsStreamingCompleted())
    {
        if (CityBlockPreparationStartedWorldS < 0) CityBlockPreparationStartedWorldS = GetWorld()->GetTimeSeconds();
        const bool bOptionalWaitFitsOriginalDeadline = GetWorld()->GetTimeSeconds() - BootstrapStartWorldS + 2 < 60;
        if (GetWorld()->GetTimeSeconds() - CityBlockPreparationStartedWorldS < 10 && bOptionalWaitFitsOriginalDeadline)
        { BootstrapStatus = TEXT("Loading the optional block delivery destination..."); return false; }
        BlockUnavailable(bOptionalWaitFitsOriginalDeadline
            ? TEXT("Optional destination streaming did not complete within10 gameplay seconds after actual depot admission")
            : TEXT("Optional destination wait omitted to preserve the original60-second depot bootstrap deadline"));
    }
    if (bInstallCityBlock)
    {
        FString ContentReason;
        if (!MAYACityBlockRouteContent::Build(Route, CityBlockDefinition, ContentReason))
            BlockUnavailable(TEXT("Frozen content refused: ") + ContentReason);
        else if (!TraceSidewalk(CityBlockDefinition.Points.Last(), BlockServiceSurface)
            || !TraceSidewalk(MAYACityBlockRouteContent::StationPose().GetLocation(), BlockStationSurface))
            BlockUnavailable(TEXT("No fresh original sidewalk surface at the appended service/station location"));
    }
    if (!bInstallCityBlock && FParse::Param(FCommandLine::Get(), TEXT("MayaV01CityBlockMissionTest")))
    { StopBootstrap(TEXT("Required fourth-mission extension unavailable: ") + CityBlockUnavailableReason); return false; }
    const FVector Direction = (Route.Last() - Route[0]).GetSafeNormal2D();
    const FVector Side(-Direction.Y, Direction.X, 0);
    TArray<AMAYADeliveryStation*> Stations;
    static const TCHAR* Names[] = { TEXT("Maya Depot"), TEXT("Corner Store"), TEXT("Community Clinic"), TEXT("Research Studio") };
    for (int32 I = 0; I < 4; ++I)
    {
        const int32 RouteIndex = I == 0 ? 0 : FMath::Clamp((Route.Num() - 1) * I / 3, 1, Route.Num() - 1);
        FHitResult Surface;
        if (!TraceSidewalk(Route[RouteIndex] + Side * 90, Surface))
        { BootstrapStatus = TEXT("A delivery station has no clear sidewalk approach."); for (AMAYADeliveryStation* S : Stations) S->Destroy(); return false; }
        const FTransform Pose(Direction.Rotation(), Surface.ImpactPoint);
        AMAYADeliveryStation* S = GetWorld()->SpawnActorDeferred<AMAYADeliveryStation>(AMAYADeliveryStation::StaticClass(), Pose, this);
        if (!S) { StopBootstrap(TEXT("A delivery station could not be created.")); for (AMAYADeliveryStation* Existing : Stations) Existing->Destroy(); return false; }
        S->StationId = FString::Printf(TEXT("maya_station_%d"), I);
        S->DisplayName = Names[I]; S->bDepot = I == 0;
        S->bChargingStation = I == 0 || I == 2; // depot + surveyed Community Clinic parking
        UGameplayStatics::FinishSpawningActor(S, Pose);
        S->SetServicePoint(Route[RouteIndex]); // measured robot parking; parcel remains at the original sidewalk pose
        Stations.Add(S);
    }
    // Historical reviewed start center was ground +23.546 cm, settling to +11.727.
    // The native preflight still checks all actual bodies and current start clearance.
    const FTransform SpawnPose((-Direction).Rotation(), Route[0] + FVector(0, 0, 23.546));
    Robot = GetWorld()->SpawnActorDeferred<AMAYANativeDemoPawn>(AMAYANativeDemoPawn::StaticClass(), SpawnPose, DeliveryPlayer.Get(), nullptr,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Robot || !ApplyCosmetics(*Robot))
    { StopBootstrap(TEXT("The robot or its accepted visual assets could not be loaded.")); if (Robot) Robot->Destroy(); Robot = nullptr; for (AMAYADeliveryStation* S : Stations) S->Destroy(); return false; }
    Robot->bReviewedCandidateProfile = true;
    Robot->bDeliveryOwnsInteraction = true;
    UGameplayStatics::FinishSpawningActor(Robot, SpawnPose);
    if (!Robot->Fault.IsEmpty())
    { StopBootstrap(TEXT("The robot could not initialize safely: ") + Robot->Fault); Robot->Destroy(); Robot = nullptr; for (AMAYADeliveryStation* S : Stations) S->Destroy(); return false; }
    Director = GetWorld()->SpawnActor<AMAYADeliveryDirector>();
    if (!Director || !Director->ConfigureStations(Stations, Route))
    { StopBootstrap(TEXT("Delivery jobs could not be initialized.")); if (Director) Director->Destroy(); Robot->Destroy(); Robot = nullptr; for (AMAYADeliveryStation* S : Stations) S->Destroy(); return false; }
    Director->BindRobot(Robot);
    if (bInstallCityBlock)
    {
        FTransform BlockPose = MAYACityBlockRouteContent::StationPose();
        BlockPose.SetLocation(BlockStationSurface.ImpactPoint);
        AMAYADeliveryStation* BlockStation = GetWorld()->SpawnActorDeferred<AMAYADeliveryStation>(AMAYADeliveryStation::StaticClass(), BlockPose, this);
        if (!BlockStation) BlockUnavailable(TEXT("Destination station could not be created"));
        else
        {
            BlockStation->StationId = TEXT("maya_station_city_block");
            BlockStation->DisplayName = TEXT("Across the Block");
            BlockStation->bDepot = false; BlockStation->bChargingStation = false;
            UGameplayStatics::FinishSpawningActor(BlockStation, BlockPose);
            BlockStation->SetServicePoint(BlockServiceSurface.ImpactPoint);
            const auto ActualGraph = MAYACityBlockRoute::Read(GetWorld(), CityBlockDefinition);
            if (!ActualGraph.bBound || !Director->InstallCityBlockMission(BlockStation, CityBlockDefinition))
            {
                const FString Refusal = !ActualGraph.bBound ? ActualGraph.Reason : TEXT("Frozen route/station installation predicates refused");
                BlockStation->Destroy(); BlockUnavailable(Refusal);
            }
            else
            {
                double PathLengthCm = 0;
                for (int32 I = 1; I < CityBlockDefinition.Points.Num(); ++I) PathLengthCm += FVector::Dist2D(CityBlockDefinition.Points[I-1], CityBlockDefinition.Points[I]);
                UE_LOG(LogTemp, Display, TEXT("MAYA CITY BLOCK installed: routeId=%s auditSha256=%s eastRampAuditSha256=%s approachAuditSha256=%s crestAuditSha256=%s stopTurnCount=%d profileCount=%d pointCount=%d pathLengthCm=%.3f crossingCount=%d stationId=%s stationActor=%s graphBound=%d; pedestrian signals are live advisory"),
                    *CityBlockDefinition.RouteId, *CityBlockDefinition.SurveyReceiptSha256, *CityBlockDefinition.EastRampSurfaceAuditSha256, *CityBlockDefinition.ApproachSurfaceAuditSha256, *CityBlockDefinition.CrestSurfaceAuditSha256, CityBlockDefinition.StopTurns.Num(), Director->GetAvailableProfileCount(), CityBlockDefinition.Points.Num(), PathLengthCm,
                    CityBlockDefinition.Crossings.Num(), *BlockStation->StationId, *BlockStation->GetPathName(), ActualGraph.bBound);
                CityBlockBootstrapStreaming->DisableStreamingSource();
            }
        }
        if (!bInstallCityBlock && FParse::Param(FCommandLine::Get(), TEXT("MayaV01CityBlockMissionTest")))
        { StopBootstrap(TEXT("Required fourth-mission extension unavailable: ") + CityBlockUnavailableReason); return false; }
    }
    DeliveryPlayer->Possess(Robot);
    DeliveryPlayer->SetViewTarget(Robot);
    BootstrapStatus = TEXT("City delivery ready.");
    UE_LOG(LogTemp, Display, TEXT("MAYA GAME bootstrap: %d sampled city sidewalk points; 4 stations; accepted 15 cosmetics; pawn %s"), Route.Num(), *Robot->GetPathName());
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01BlockRouteSurvey")))
    {
#if WITH_EDITOR && !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        const FTransform ProbePose;
        AMAYACityBlockRouteSurvey* Probe = GetWorld()->SpawnActorDeferred<AMAYACityBlockRouteSurvey>(AMAYACityBlockRouteSurvey::StaticClass(), ProbePose, this);
        if (!Probe) { StopBootstrap(TEXT("Read-only block route survey could not be created.")); return false; }
        Probe->Configure(Robot, Director);
        UGameplayStatics::FinishSpawningActor(Probe, ProbePose);
#endif
    }
    else if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01CityBlockMissionTest")))
    {
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        const FTransform TrialPose;
        AMAYACityBlockMissionTrial* Trial = GetWorld()->SpawnActorDeferred<AMAYACityBlockMissionTrial>(AMAYACityBlockMissionTrial::StaticClass(), TrialPose, this);
        if (!Trial) { StopBootstrap(TEXT("Actual city block mission trial could not be created.")); return false; }
        Trial->Configure(Robot, Director, CityBlockDefinition);
        UGameplayStatics::FinishSpawningActor(Trial, TrialPose);
#endif
    }
    else if (FParse::Param(FCommandLine::Get(),TEXT("MayaV01NavigationTest")))
    {
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        const FTransform TestPose;
        auto* Test=GetWorld()->SpawnActorDeferred<AMAYANavigationNativeTest>(AMAYANavigationNativeTest::StaticClass(),TestPose,this);
        if(!Test){StopBootstrap(TEXT("Native navigation QA could not be created."));return false;}
        Test->Configure(Robot,Director,Route);UGameplayStatics::FinishSpawningActor(Test,TestPose);
#endif
    }
    else if(FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest")))
    {
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        const FTransform TestPose;
        auto* Test=GetWorld()->SpawnActorDeferred<AMAYABoostPowerNativeTest>(AMAYABoostPowerNativeTest::StaticClass(),TestPose,this);
        if(!Test){StopBootstrap(TEXT("Native boost power trial could not be created"));return false;}
        Test->Configure(Robot,Director,Route);UGameplayStatics::FinishSpawningActor(Test,TestPose);
#endif
    }
    else if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        && !UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(this))
    {
        const FTransform DriverPose;
        AMAYAV01NativeTestDriver* Driver = GetWorld()->SpawnActorDeferred<AMAYAV01NativeTestDriver>(AMAYAV01NativeTestDriver::StaticClass(), DriverPose, this);
        if (Driver)
        {
            Driver->Configure(Robot, Director);
            UGameplayStatics::FinishSpawningActor(Driver, DriverPose);
            UMAYAPlayerNewShiftReloadTest::NoteAutomationDriverSpawned(this);
        }
    }
#endif
    return true;
}

void AMAYADeliveryGameMode::Tick(float Dt)
{
    Super::Tick(Dt);
    if (bBootstrapped || bBootstrapStopped || GetWorld()->GetTimeSeconds() < NextBootstrapWorldS) return;
    if (GetWorld()->GetTimeSeconds() - BootstrapStartWorldS > 60)
    {
        StopBootstrap(TEXT("City preparation timed out: ") + BootstrapStatus);
        return;
    }
    NextBootstrapWorldS = GetWorld()->GetTimeSeconds() + 2;
    bBootstrapped = Bootstrap();
}

void AMAYADeliveryGameMode::StopBootstrap(const FString& Reason)
{
    if (bBootstrapStopped) return;
    bBootstrapStopped = true;
    if (CityBlockBootstrapStreaming) CityBlockBootstrapStreaming->DisableStreamingSource();
    BootstrapStatus = Reason;
    UE_LOG(LogTemp, Error, TEXT("MAYA GAME bootstrap FAILED: %s; streaming anchor=%s completed=%d"),
        *Reason, *GetRootComponent()->GetComponentLocation().ToString(), CityStreaming->IsStreamingCompleted());
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        && !UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(this))
    {
        FString Output;
        if (!FParse::Value(FCommandLine::Get(), TEXT("MayaV01Output="), Output) || Output.IsEmpty())
            Output = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MayaV01Native"));
        IFileManager::Get().MakeDirectory(*Output, true);
        auto Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("schema"), TEXT("MayaRobotics.V01.NativeGame.v1"));
        Report->SetStringField(TEXT("scope"), TEXT("City preparation failure; native mechanics and delivery were not executed"));
        Report->SetStringField(TEXT("engineVersion"), FApp::GetBuildVersion());
        Report->SetBoolField(TEXT("terminal"), true); Report->SetBoolField(TEXT("success"), false);
        Report->SetBoolField(TEXT("domainKnown"), false);
        Report->SetStringField(TEXT("phase"), TEXT("Bootstrap")); Report->SetStringField(TEXT("reason"), Reason);
        Report->SetNumberField(TEXT("missionCount"), 0); Report->SetNumberField(TEXT("sampleCount"), 0);
        Report->SetNumberField(TEXT("endWorldSeconds"), GetWorld()->GetTimeSeconds());
        FString Json;
        FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
        FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, TEXT("result.json")));
        FTimerHandle ExitTimer;
        GetWorld()->GetTimerManager().SetTimer(ExitTimer, []()
            { FPlatformMisc::RequestExit(false, TEXT("MAYADeliveryGameMode")); }, 1.f, false);
    }
#endif
}
