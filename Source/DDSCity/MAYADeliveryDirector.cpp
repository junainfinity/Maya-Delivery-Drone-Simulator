#include "MAYADeliveryDirector.h"
#include "MAYAPlayerNewShiftReloadTest.h"
#include "MAYADeliveryStation.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYADeliveryParcel.h"
#include "MAYACrowdObstacleComponent.h"
#include "MAYAVehicleContactObserver.h"
#include "MAYANativeDemoPawn.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/World.h"
#include "CoreGlobals.h"
#include "Misc/Guid.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#endif

namespace
{
    FString ServicePoseTrace(const FTransform& Pose)
    {
        const FVector P = Pose.GetLocation(), S = Pose.GetScale3D(); const FQuat Q = Pose.GetRotation();
        return FString::Printf(TEXT("position=(%.6f,%.6f,%.6f) quaternion=(%.12f,%.12f,%.12f,%.12f) scale=(%.9f,%.9f,%.9f)"),
            P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
    }

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    void SetServiceReceiptNumber(const TSharedRef<FJsonObject>& Json, const TCHAR* Key, double Value)
    {
        if (FMath::IsFinite(Value)) Json->SetNumberField(Key, Value);
        else Json->SetStringField(Key, FMath::IsNaN(Value) ? TEXT("NaN") : Value > 0 ? TEXT("+Infinity") : TEXT("-Infinity"));
    }

    TSharedRef<FJsonObject> MakeServiceCommitReceipt(const FMAYADeliveryServiceEvidence& E,
        const FMAYADeliverySnapshot& S, bool bCancellationPending, const FTransform& Target, const FTransform& Current)
    {
        const bool bPickup = S.Phase == EMAYADeliveryPhase::Loading;
        const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.ServiceCommitEvidence.v1"));
        J->SetStringField(TEXT("scope"), TEXT("Evidence passed to Core::CommitService; game-thread observations, solver-step scope unknown"));
        J->SetNumberField(TEXT("phaseBeforeCommit"), static_cast<uint8>(S.Phase));
        J->SetBoolField(TEXT("pickupFromPhase"), bPickup);
        J->SetBoolField(TEXT("expectedServicePhase"), bPickup || S.Phase == EMAYADeliveryPhase::Unloading);
        J->SetBoolField(TEXT("cancellationPending"), bCancellationPending);
        J->SetBoolField(TEXT("serviceTokenMatches"), S.Service.Equals(E.Token));
        J->SetBoolField(TEXT("tokenPickupMatchesPhase"), E.Token.bPickup == bPickup);
        J->SetStringField(TEXT("serviceGeneration"), LexToString(S.Service.Generation));
        J->SetStringField(TEXT("evidenceServiceGeneration"), LexToString(E.Token.Generation));
        J->SetStringField(TEXT("expectedParcelId"), S.JobInstanceId);
        J->SetStringField(TEXT("observedParcelId"), E.ObservedParcelId);
        J->SetBoolField(TEXT("parcelIdMatches"), E.ObservedParcelId == S.JobInstanceId);
        J->SetBoolField(TEXT("openEndpointsObserved"), E.bOpenEndpointsObserved);
        J->SetBoolField(TEXT("actualTransferPathClear"), E.bActualTransferPathClear);
        J->SetBoolField(TEXT("parcelAtCorrectDestination"), E.bParcelAtCorrectDestination);
        J->SetBoolField(TEXT("actualCustodyObserved"), E.bActualCustodyObserved);
        J->SetBoolField(TEXT("parcelMassObserved"), E.bParcelMassObserved);
        J->SetBoolField(TEXT("parcelMassFinite"), FMath::IsFinite(E.ObservedParcelKg));
        J->SetBoolField(TEXT("parcelMassWithinTolerance"), FMath::Abs(E.ObservedParcelKg - S.Job.PayloadKg) <= 0.05);
        SetServiceReceiptNumber(J, TEXT("observedParcelKg"), E.ObservedParcelKg);
        SetServiceReceiptNumber(J, TEXT("expectedParcelKg"), S.Job.PayloadKg);
        J->SetBoolField(TEXT("stoppedAndSupported"), E.Robot.IsStoppedAndSupported());
        J->SetBoolField(TEXT("known"), E.Robot.bKnown);
        J->SetBoolField(TEXT("ready"), E.Robot.bReady);
        J->SetBoolField(TEXT("grounded"), E.Robot.bGrounded);
        J->SetBoolField(TEXT("upright"), E.Robot.bUpright);
        J->SetNumberField(TEXT("supportMask"), E.Robot.SupportMask);
        J->SetStringField(TEXT("fault"), E.Robot.Fault);
        J->SetBoolField(TEXT("doorsClosedLatched"), E.Robot.bDoorsClosedLatched);
        J->SetStringField(TEXT("expectedResetGeneration"), LexToString(S.Service.RobotResetGeneration));
        J->SetStringField(TEXT("observedResetGeneration"), LexToString(E.Robot.ResetGeneration));
        J->SetBoolField(TEXT("resetGenerationMatches"), E.Robot.ResetGeneration == S.Service.RobotResetGeneration);
        J->SetBoolField(TEXT("observationNotBeforeService"), E.Robot.ObservationWorldSeconds >= S.Service.StartedWorldSeconds);
        SetServiceReceiptNumber(J, TEXT("worldSeconds"), E.Robot.ObservationWorldSeconds);
        SetServiceReceiptNumber(J, TEXT("serviceStartedWorldSeconds"), S.Service.StartedWorldSeconds);
        const double Drift = FVector::Dist(E.Robot.ChassisPose.GetLocation(), S.Service.RobotServicePosition);
        J->SetBoolField(TEXT("servicePositionWithinTolerance"), Drift <= 5.0);
        SetServiceReceiptNumber(J, TEXT("servicePositionDriftCm"), Drift);
        SetServiceReceiptNumber(J, TEXT("speedCmps"), E.Robot.VelocityCmps.Size());
        SetServiceReceiptNumber(J, TEXT("angularSpeedRadps"), E.Robot.AngularVelocityRadps.Size());
        SetServiceReceiptNumber(J, TEXT("parcelTargetDistanceCm"), FVector::Dist(Current.GetLocation(), Target.GetLocation()));
        SetServiceReceiptNumber(J, TEXT("parcelTargetAngularDeltaDegrees"), FMath::RadiansToDegrees(Current.GetRotation().AngularDistance(Target.GetRotation())));
        J->SetStringField(TEXT("actualTargetPose"), ServicePoseTrace(Target));
        J->SetStringField(TEXT("actualParcelPose"), ServicePoseTrace(Current));
        J->SetStringField(TEXT("chassisPose"), ServicePoseTrace(E.Robot.ChassisPose));
        J->SetStringField(TEXT("servicePositionCm"), S.Service.RobotServicePosition.ToString());
        J->SetStringField(TEXT("velocityCmps"), E.Robot.VelocityCmps.ToString());
        J->SetStringField(TEXT("angularVelocityRadps"), E.Robot.AngularVelocityRadps.ToString());
        return J;
    }
#endif
}

AMAYADeliveryDirector::AMAYADeliveryDirector()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
}

void FMAYADeliveryRingObservation::Advance(const FString& StationId, bool bActive,
    bool bObservedInside, bool bPhaseEligible, double DeltaSeconds, bool bPaused)
{
    if (bPaused) return;
    const bool bStationChanged = ActiveStationId != StationId;
    ActiveStationId = bActive ? StationId : FString();
    bEligible = bActive && bPhaseEligible;
    if (!bActive || !bObservedInside || bStationChanged)
    {
        bInside = false; bVisitConsumed = false;
        ElapsedGameplaySeconds = 0; RemainingGameplaySeconds = 10;
    }
    if (!bActive || !bObservedInside) return;
    const bool bNewVisit = !bInside;
    if (bNewVisit) { ++EntryGeneration; bInside = true; }
    // First observed entry does not count the preceding outside frame.
    if (!bNewVisit && bEligible && !bVisitConsumed && FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0)
        ElapsedGameplaySeconds = FMath::Min(10.0, ElapsedGameplaySeconds + DeltaSeconds);
    RemainingGameplaySeconds = FMath::Max(0.0, 10.0 - ElapsedGameplaySeconds);
}

void AMAYADeliveryDirector::UpdateRingVisit(double DeltaSeconds)
{
    AMAYADeliveryStation* Station = ObjectiveStation();
    const auto Phase = Rules.Get().Phase;
    const bool bActive = Station && Rules.IsActive();
    const bool bInside = bActive && RobotState.bKnown
        && FVector::Dist2D(Station->GetServicePoint(), RobotState.ChassisPose.GetLocation()) <= AMAYADeliveryStation::ObjectiveRingRadiusCm;
    const bool bEligible = !bRecoveryPending && (Phase == EMAYADeliveryPhase::Accepted || Phase == EMAYADeliveryPhase::Carrying);
    RingObservation.Advance(Station ? Station->StationId : FString(), bActive, bInside, bEligible, DeltaSeconds, bPaused);
}

void AMAYADeliveryDirector::TickAutomaticService(double DeltaSeconds)
{
    UpdateRingVisit(DeltaSeconds);
    if (!Rules.Get().bShiftStarted || Rules.Get().bShiftWon || !RingObservation.bInside || !RingObservation.bEligible || RingObservation.bVisitConsumed
        || RingObservation.RemainingGameplaySeconds > 0 || Rules.Get().bCharging || bPaused
        || ServiceStep != EServiceStep::None || !RobotState.IsStoppedAndSupported()
        || !Robot || !Robot->CanRequestDeliveryDoorState(true)) return;
    RequestService(true);
}

void AMAYADeliveryDirector::SetPaused(bool bInPaused)
{
    bPaused = bInPaused;
    // Physics/game ticks stop immediately at pause. Neutralize input now;
    // normal post-physics Tick evaluates the current mission lock on resume.
    if (Robot)
    {
        Robot->ReleaseDriveCommand();
        Robot->SetDeliveryDriveLocked(true);
    }
}

bool AMAYADeliveryDirector::ConfigureStations(const TArray<AMAYADeliveryStation*>& In, const TArray<FVector>& Measured)
{
    if (In.Num() != 4 || Measured.Num() < 2 || Rules.IsActive()) return false;
    TSet<FString> Ids;
    for (AMAYADeliveryStation* S : In)
    {
        if (!IsValid(S) || S->StationId.IsEmpty() || Ids.Contains(S->StationId)) return false;
        Ids.Add(S->StationId);
    }
    if (!In[0]->bDepot) return false;
    Stations.Reset();
    for (AMAYADeliveryStation* S : In) Stations.Add(S);
    Route = Measured;
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        && !UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(this))
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01VisualCapture"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CurbTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
            || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput")))
        {
            UE_LOG(LogTemp, Error, TEXT("MAYA operating fixture refused incompatible test flags."));
            return false;
        }
        if (!bNativeOperatingBatteryFixtureApplied)
        {
            double InitialPercent = 0.5;
            FParse::Value(FCommandLine::Get(), TEXT("MayaV01InitialBattery="), InitialPercent);
            if (!Rules.InitializeNativeOperatingBattery(InitialPercent))
            {
                UE_LOG(LogTemp, Error, TEXT("MAYA operating initial-battery fixture refused: require pristine pre-offer state and finite 0.5-1 percent."));
                return false;
            }
            NativeOperatingInitialBatteryPercent = InitialPercent;
            NativeOperatingBatteryFixtureId = TEXT("MayaRobotics.V01.Operating.InitialBattery.v1");
            bNativeOperatingBatteryFixtureApplied = true;
            UMAYAPlayerNewShiftReloadTest::NoteInitialFixtureApplied(this);
            UE_LOG(LogTemp, Display, TEXT("MAYA GAME TEST FIXTURE %s: initial battery %.6f percent before first offer; normal drain/charge/ticks unchanged"),
                *NativeOperatingBatteryFixtureId, NativeOperatingInitialBatteryPercent);
        }
    }
#endif
    // Ordinary menus can wait for real physics without spending the pristine
    // shift's battery. Native automation is explicit active GAME scope.
    bool bNativeAutomation = false;
#if !UE_BUILD_SHIPPING
    bNativeAutomation = FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        && !UMAYAPlayerNewShiftReloadTest::ShouldSuppressInitialAutomation(this);
#endif
    if (!bNativeAutomation && !Rules.PrepareShiftForStart()) return false;
    return OfferProfile(0);
}

FString AMAYADeliveryDirector::ReadCargoWorkerEvidenceJson() const
{
    return TEXT("{\"schema\":\"MayaRobotics.CargoWorkerDisabled.v1\",\"enabled\":false,\"reason\":\"User-selected instant cargo service\"}");
}

void AMAYADeliveryDirector::UnbindImpactBodies()
{
    for (const auto& Body : ImpactBodies) if (Body.IsValid())
        Body->OnComponentHit.RemoveDynamic(this, &AMAYADeliveryDirector::OnRobotHit);
    ImpactBodies.Reset();
}

void AMAYADeliveryDirector::BindRobot(AMAYANativeDemoPawn* In)
{
    if (IsValid(CrowdObstacle) && CrowdObstacle->GetOwner() != In)
    { CrowdObstacle->StopObserving(); CrowdObstacle->DestroyComponent(); CrowdObstacle = nullptr; }
    UnbindImpactBodies();
    Robot = In;
    if (Robot)
    {
        AddTickPrerequisiteActor(Robot);
        if (!IsValid(CrowdObstacle))
        {
            CrowdObstacle = Robot->FindComponentByClass<UMAYACrowdObstacleComponent>();
            if (!CrowdObstacle)
            {
                CrowdObstacle = NewObject<UMAYACrowdObstacleComponent>(Robot);
                Robot->AddInstanceComponent(CrowdObstacle);
                CrowdObstacle->RegisterComponent();
            }
        }
        // Nine independent native bodies: compound chassis, six wheels, two lids.
        // Welded suspension children resolve to the chassis actor and are not
        // subscribed twice. Notification only; no collision/force settings change.
        TArray<UPrimitiveComponent*> Components; Robot->GetComponents<UPrimitiveComponent>(Components);
        for (UPrimitiveComponent* Component : Components)
        {
            if (!Component || !Component->IsPhysicsCollisionEnabled()) continue;
            FBodyInstance* Native = Component->GetBodyInstance();
            FBodyInstance* Own = Component->GetBodyInstance(NAME_None, false);
            if (!Native || !Own || Own != Native || !Native->IsValidBodyInstance()) continue;
            Component->SetNotifyRigidBodyCollision(true);
            Component->OnComponentHit.AddUniqueDynamic(this, &AMAYADeliveryDirector::OnRobotHit);
            ImpactBodies.Add(Component);
        }
    }
    if (Robot)
    {
        if (!VehicleContactObserver)
        {
            VehicleContactObserver = NewObject<UMAYAVehicleContactObserver>(this);
            AddInstanceComponent(VehicleContactObserver); VehicleContactObserver->RegisterComponent();
        }
        VehicleContactObserver->BindRobot(Robot);
    }
    ReadRobot();
}

UPrimitiveComponent* AMAYADeliveryDirector::CarrierBody() const
{
    UPrimitiveComponent* Mount = Robot ? Robot->GetDeliveryCargoMount() : nullptr;
    return Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
}

FString AMAYADeliveryDirector::ReadCrowdObstacleEvidenceJson() const
{
    return IsValid(CrowdObstacle) ? CrowdObstacle->ReadObstacleEvidenceJson()
        : TEXT("{\"schema\":\"MayaRobotics.NativeCrowdObstacle.v1\",\"published\":false}");
}

AMAYADeliveryStation* AMAYADeliveryDirector::FindStation(const FString& Id) const
{
    for (AMAYADeliveryStation* S : Stations) if (IsValid(S) && S->StationId == Id) return S;
    return nullptr;
}

bool AMAYADeliveryDirector::OfferProfile(int32 Index)
{
    if (Stations.Num() < 4 || Index < 0 || Index >= GetAvailableProfileCount()) return false;
    FMAYADeliveryJob Job;
    static const TCHAR* Titles[] = { TEXT("Neighbourhood essentials"), TEXT("Urgent medical supplies"), TEXT("Two-parcel laboratory delivery") };
    static const double Deadlines[] = {240, 150, 300};
    Job.ProfileId = Index == 3 ? TEXT("city_block") : FString::Printf(TEXT("city_%d"), Index + 1);
    Job.Title = Index == 3 ? TEXT("Across the block") : Titles[Index];
    Job.PickupStationId = Stations[0]->StationId;
    Job.DropoffStationId = Stations[Index + 1]->StationId;
    Job.PayloadKg = Index == 2 ? 2 : 1;
    Job.ParcelCount = Index == 2 ? 2 : 1;
    Job.DeadlineSeconds = Index == 3 ? CityBlockRoute.DeadlineSeconds : Deadlines[Index];
    Job.bFragile = Index == 2;
    if (!Rules.Offer(Job)) return false;
    ProfileIndex = Index;
    RuntimeStatus = TEXT("J accepts this delivery. N chooses another offer while safely stopped.");
    return true;
}

bool AMAYADeliveryDirector::InstallCityBlockMission(AMAYADeliveryStation* Station, const FMAYACityBlockRouteDefinition& Definition)
{
    using namespace MAYAHUDGeometry;
    if (!GetWorld() || !IsValid(Station) || Station->GetWorld() != GetWorld() || Station->bDepot
        || Station->StationId.IsEmpty() || FindStation(Station->StationId) || Stations.Num() != 4
        || !CityBlockDestinationId.IsEmpty() || Rules.IsActive() || Rules.Get().bDroneTotalled
        || Rules.Get().bVehicleContactPending || Rules.Get().bShiftWon || !Parcels.IsEmpty()
        || ServiceStep != EServiceStep::None || Rules.IsServiceCancellationPending() || bRecoveryPending
        || Definition.RouteId.IsEmpty() || Definition.SurveyReceiptSha256.Len() != 64
        || Definition.Points.Num() <= Route.Num() || Definition.Points.Num() > 512
        || Definition.PointLaneIndices.Num() != Definition.Points.Num()
        || Definition.Crossings.Num() != 2 || Definition.Lanes.IsEmpty()
        || !FMath::IsFinite(Definition.DeadlineSeconds) || Definition.DeadlineSeconds <= 0) return false;
    for (const TCHAR Character : Definition.SurveyReceiptSha256)
        if (!FChar::IsHexDigit(Character)) return false;
    FPolyline Path;
    if (!Path.Build(Definition.Points) || Path.Points.Num() != Definition.Points.Num()) return false;
    for (int32 I = 0; I < Route.Num(); ++I)
        if (!Definition.Points[I].Equals(Route[I], .01) || Definition.PointLaneIndices[I] != INDEX_NONE) return false;
    if (!Definition.EastRampSurfaceAuditSha256.IsEmpty()
        && Definition.EastRampSurfaceAuditSha256 != TEXT("E76CF442C718A13A474F1A4DF2E5F990751913F4CC83CF234F7FE3E3E18D7DAE")) return false;
    if (!Definition.ApproachSurfaceAuditSha256.IsEmpty()
        && Definition.ApproachSurfaceAuditSha256 != TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86")) return false;
    for (const auto& Lane : Definition.Lanes)
    {
        // Actual141 binds only these two reference lanes inside its measured rectangle.
        const double CrestLimitCm=Lane.LaneIndex==2071?180.:(Lane.LaneIndex==9793?160.:0.);
        if (!FMath::IsFinite(Lane.CrestAuditedMaxWaypointOffsetCm)
            || Lane.CrestAuditedMaxWaypointOffsetCm < 0 || Lane.CrestAuditedMaxWaypointOffsetCm > CrestLimitCm) return false;
        if (Lane.CrestAuditedMaxWaypointOffsetCm > 0
            && (Lane.CrestAuditedMaxWaypointOffsetCm < Lane.WidthCm*.5
                || Definition.CrestSurfaceAuditSha256 != TEXT("B58D0006D12E6C2C00D000E0A02B3BFEE56A1398D8C9E863F9AC5A84C8F3D3B0"))) return false;
        if (!FMath::IsFinite(Lane.StagedApproachAuditedMaxWaypointOffsetCm)
            || Lane.StagedApproachAuditedMaxWaypointOffsetCm < 0
            || Lane.StagedApproachAuditedMaxWaypointOffsetCm > 250) return false;
        if (Lane.StagedApproachAuditedMaxWaypointOffsetCm > 0
            && ((Lane.LaneIndex != 2836 && Lane.LaneIndex != 7422)
                || Lane.StagedApproachAuditedMaxWaypointOffsetCm < Lane.WidthCm * .5
                || Definition.ApproachSurfaceAuditSha256 != TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86"))) return false;
        if (!FMath::IsFinite(Lane.NorthRampAuditedMaxWaypointOffsetCm) || Lane.NorthRampAuditedMaxWaypointOffsetCm < 0
            || Lane.NorthRampAuditedMaxWaypointOffsetCm > 100) return false;
        if (Lane.NorthRampAuditedMaxWaypointOffsetCm > 0
            && (Lane.LaneIndex != 2071 || Lane.NorthRampAuditedMaxWaypointOffsetCm < Lane.WidthCm * .5
                || Definition.SurveyReceiptSha256 != TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94"))) return false;
        if (!FMath::IsFinite(Lane.EastRampAuditedMaxWaypointOffsetCm) || Lane.EastRampAuditedMaxWaypointOffsetCm < 0
            || Lane.EastRampAuditedMaxWaypointOffsetCm > 200) return false;
        if (Lane.EastRampAuditedMaxWaypointOffsetCm > 0
            && ((Lane.LaneIndex != 2836 && Lane.LaneIndex != 7422)
                || Lane.EastRampAuditedMaxWaypointOffsetCm < Lane.WidthCm * .5
                || Definition.EastRampSurfaceAuditSha256 != TEXT("E76CF442C718A13A474F1A4DF2E5F990751913F4CC83CF234F7FE3E3E18D7DAE"))) return false;
        if (!FMath::IsFinite(Lane.AuditedMaxWaypointOffsetCm) || Lane.AuditedMaxWaypointOffsetCm < 0
            || Lane.AuditedMaxWaypointOffsetCm > 100) return false;
        if (Lane.AuditedMaxWaypointOffsetCm > 0)
        {
            // Only this frozen A-west native profile establishes the wider
            // measured connector/lamp detour. Other lanes keep native halfwidth.
            if ((Lane.LaneIndex != 2841 && Lane.LaneIndex != 11355 && Lane.LaneIndex != 2836)
                || Lane.AuditedMaxWaypointOffsetCm < Lane.WidthCm * .5
                || Lane.PhysicalOffsetAuditSha256 != TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94")) return false;
        }
    }
    int32 LastLaneOrder = 0, TurnCount = 0;
    double LastTurnArc = -1000;
    for (int32 I = Route.Num(); I < Definition.Points.Num(); ++I)
    {
        const int32 LaneOrder = Definition.Lanes.IndexOfByPredicate([&](const FMAYACityBlockLaneDefinition& L)
            { return L.LaneIndex == Definition.PointLaneIndices[I]; });
        if (LaneOrder == INDEX_NONE || LaneOrder < LastLaneOrder || LaneOrder > LastLaneOrder + 1
            || FVector::Dist2D(Definition.Points[I - 1], Definition.Points[I]) > 100.01) return false;
        LastLaneOrder = LaneOrder;
        FPolyline Lane;
        if (!Lane.Build(Definition.Lanes[LaneOrder].Points)) return false;
        const auto Projection = Lane.Project(Definition.Points[I]);
        const auto& LaneDefinition = Definition.Lanes[LaneOrder];
        double MaxOffsetCm = LaneDefinition.WidthCm * .5;
        if (Projection.DistanceSquaredCm > FMath::Square(MaxOffsetCm))
        {
            const FVector& Point = Definition.Points[I];
            // Independent audit rectangles; neither allowance widens an entire lane.
            const bool West116 = LaneDefinition.AuditedMaxWaypointOffsetCm > 0
                && Point.X >= -27025 - .01 && Point.X <= -26325 + .01
                && Point.Y >= -108.36328125 - .01 && Point.Y <= Projection.Point.Y + .01;
            const bool East121 = LaneDefinition.EastRampAuditedMaxWaypointOffsetCm > 0
                && Point.X >= -24625 - .01 && Point.X <= -24175 + .01
                && Point.Y >= -205.72 - .01 && Point.Y <= 194.28 + .01
                && Point.Y <= Projection.Point.Y + .01;
            const bool North116 = LaneDefinition.NorthRampAuditedMaxWaypointOffsetCm > 0
                && Point.X >= -24357.059 - .01 && Point.X <= -24157.059 + .01
                && Point.Y >= 7725 - .01 && Point.Y <= 8125 + .01
                && Point.X <= Projection.Point.X + .01;
            const bool Approach127 = LaneDefinition.StagedApproachAuditedMaxWaypointOffsetCm > 0
                && Point.X >= -24950 - .01 && Point.X <= -24050 + .01
                && Point.Y >= -305.72 - .01 && Point.Y <= 294.28 + .01
                && Point.Y <= Projection.Point.Y + .01;
            const bool Crest141 = LaneDefinition.CrestAuditedMaxWaypointOffsetCm > 0
                && Point.X >= -24407.059 - .01 && Point.X <= -24157.059 + .01
                && Point.Y >= 8075 - .01 && Point.Y <= 8275 + .01;
            if (!West116 && !East121 && !North116 && !Approach127 && !Crest141) return false;
            MaxOffsetCm = (Crest141 ? LaneDefinition.CrestAuditedMaxWaypointOffsetCm
                : (Approach127 ? LaneDefinition.StagedApproachAuditedMaxWaypointOffsetCm
                : (East121 ? LaneDefinition.EastRampAuditedMaxWaypointOffsetCm
                : (North116 ? LaneDefinition.NorthRampAuditedMaxWaypointOffsetCm
                    : LaneDefinition.AuditedMaxWaypointOffsetCm)))) + .01;
        }
        if (!Projection.bValid || Projection.DistanceSquaredCm > FMath::Square(MaxOffsetCm)
            || FMath::Abs(Projection.Point.Z - Definition.Points[I].Z) > 150) return false;
    }
    if (LastLaneOrder != Definition.Lanes.Num() - 1) return false;
    int32 PreviousStopPoint = INDEX_NONE;
    if (Definition.StopTurns.Num() > 8) return false;
    for (const auto& Stop : Definition.StopTurns)
    {
        // Only the exact audited A crest and final B maneuvers may use60cm; other stops stay30.
        const bool bFinalBCheckpoint=Definition.StopTurns.Num()==6&&&Stop==&Definition.StopTurns.Last()
            &&Definition.Points.IsValidIndex(Stop.PointIndex)&&Definition.PointLaneIndices.IsValidIndex(Stop.PointIndex)
            &&Definition.PointLaneIndices[Stop.PointIndex]==2071
            &&FVector::Dist2D(Definition.Points[Stop.PointIndex],FVector(-24357.059,8200,73))<=.01
            &&Stop.SurfaceAuditSha256==Definition.CrestSurfaceAuditSha256
            &&Definition.CrestSurfaceAuditSha256==TEXT("B58D0006D12E6C2C00D000E0A02B3BFEE56A1398D8C9E863F9AC5A84C8F3D3B0");
        const bool bACrestCheckpoint=Definition.StopTurns.Num()==6 && &Stop==&Definition.StopTurns[2]
            &&Definition.Points.IsValidIndex(Stop.PointIndex)&&Definition.PointLaneIndices.IsValidIndex(Stop.PointIndex)
            &&Definition.PointLaneIndices[Stop.PointIndex]==2836
            &&FVector::Dist2D(Definition.Points[Stop.PointIndex],FVector(-24250,-80.72,73))<=.01
            &&Stop.SurfaceAuditSha256==Definition.ApproachSurfaceAuditSha256
            &&Definition.ApproachSurfaceAuditSha256==TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86");
        if (Stop.PointIndex <= PreviousStopPoint || Stop.PointIndex < Route.Num()
            || !Definition.Points.IsValidIndex(Stop.PointIndex)
            || !Definition.Points.IsValidIndex(Stop.OutgoingEndpointPointIndex)
            || Stop.OutgoingEndpointPointIndex <= Stop.PointIndex
            || !FMath::IsFinite(Stop.ArrivalRadiusCm) || Stop.ArrivalRadiusCm <= 0 || Stop.ArrivalRadiusCm > ((bFinalBCheckpoint||bACrestCheckpoint)?60:30)
            || !FMath::IsFinite(Stop.MaximumCentreExcursionCm)
            || Stop.MaximumCentreExcursionCm < Stop.ArrivalRadiusCm || Stop.MaximumCentreExcursionCm > 150
            || (Stop.SurfaceAuditSha256 != Definition.ApproachSurfaceAuditSha256
                && Stop.SurfaceAuditSha256 != Definition.SurveyReceiptSha256
                && Stop.SurfaceAuditSha256 != Definition.CrestSurfaceAuditSha256)
            || Stop.SurfaceAuditSha256.Len() != 64) return false;
        PreviousStopPoint = Stop.PointIndex;
    }
    for (int32 I = 1; I + 1 < Path.Points.Num(); ++I)
    {
        const FVector In = (Path.Sample(Path.Arc[I]) - Path.Sample(Path.Arc[I] - 100)).GetSafeNormal2D();
        const FVector Out = (Path.Sample(Path.Arc[I] + 100) - Path.Sample(Path.Arc[I])).GetSafeNormal2D();
        if (!In.IsNearlyZero() && !Out.IsNearlyZero() && Path.Arc[I] - LastTurnArc >= 300
            && FVector::DotProduct(In, Out) < FMath::Cos(FMath::DegreesToRadians(30.0)))
        { ++TurnCount; LastTurnArc = Path.Arc[I]; }
    }
    if (TurnCount < 2 || !Finite(Station->GetServicePoint())
        || FVector::Dist2D(Path.Points.Last(), Station->GetServicePoint()) > 25
        || FMath::Abs(Path.Points.Last().Z - Station->GetServicePoint().Z) > 100) return false;
    double PreviousExit = 0;
    for (const auto& C : Definition.Crossings)
    {
        if (!FMath::IsFinite(C.WaitingBeforeArcCm) || !FMath::IsFinite(C.EntryArcCm)
            || !FMath::IsFinite(C.ExitArcCm) || !FMath::IsFinite(C.WaitingAfterArcCm)
            || C.WaitingBeforeArcCm < PreviousExit || C.WaitingBeforeArcCm > C.EntryArcCm
            || C.EntryArcCm >= C.ExitArcCm || C.ExitArcCm > C.WaitingAfterArcCm || C.WaitingAfterArcCm > Path.Arc.Last()) return false;
        PreviousExit = C.WaitingAfterArcCm;
    }
    if (Definition.Crossings[0].IntersectionZoneIndex == Definition.Crossings[1].IntersectionZoneIndex) return false;
    const auto Current = MAYACityBlockRoute::Read(GetWorld(), Definition);
    if (!Current.bBound) return false;
    CityBlockRoute = Definition; CityBlockObservation = Current;
    LastCityBlockReadWorldSeconds = GetWorld()->GetTimeSeconds();
    CityBlockDestinationId = Station->StationId;
    Stations.Add(Station);
    return true;
}

void AMAYADeliveryDirector::RefreshCityBlockRoute()
{
    if (CityBlockDestinationId.IsEmpty() || !GetWorld()) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (LastCityBlockReadWorldSeconds >= 0 && Now - LastCityBlockReadWorldSeconds < .25) return;
    CityBlockObservation = MAYACityBlockRoute::Read(GetWorld(), CityBlockRoute);
    LastCityBlockReadWorldSeconds = Now;
}

bool AMAYADeliveryDirector::IsCityBlockRouteReady() const
{
    if (!GetWorld() || CityBlockDestinationId.IsEmpty() || !FindStation(CityBlockDestinationId)
        || !CityBlockObservation.bBound) return false;
    const double Age = GetWorld()->GetTimeSeconds() - CityBlockObservation.WorldSeconds;
    return FMath::IsFinite(Age) && Age >= 0 && Age <= 1;
}

int32 AMAYADeliveryDirector::GetAvailableProfileCount() const
{ return IsCityBlockRouteReady() ? 4 : 3; }

bool AMAYADeliveryDirector::IsCityBlockStation(const AMAYADeliveryStation* Station) const
{ return Station && !CityBlockDestinationId.IsEmpty() && Station->StationId == CityBlockDestinationId; }

bool AMAYADeliveryDirector::UsesCityBlockGuidance() const
{
    if (CityBlockDestinationId.IsEmpty() || CityBlockRoute.Points.Num() < 2) return false;
    if (Rules.Get().Job.ProfileId == TEXT("city_block")) return true;
    MAYAHUDGeometry::FPolyline Long, Short;
    if (!RobotState.bKnown || !Long.Build(CityBlockRoute.Points) || !Short.Build(Route)) return false;
    const auto Actual = Long.Project(RobotState.ChassisPose.GetLocation());
    // The same connected extension remains the return path after Next selects a
    // short offer at the far destination. Switching offers cannot create a shortcut.
    return Actual.bValid && Actual.ArcCm > Short.Arc.Last() + 25;
}

const TArray<FVector>& AMAYADeliveryDirector::GetActiveGuidanceRoute() const
{ return UsesCityBlockGuidance() ? CityBlockRoute.Points : Route; }

FMAYARouteAdvicePresentation AMAYADeliveryDirector::GetRouteAdvice() const
{
    using namespace MAYAHUDGeometry;
    FMAYARouteAdvicePresentation Advice;
    if (!UsesCityBlockGuidance() || !RobotState.bKnown || !MovingPhase(Rules.Get().Phase)) return Advice;
    FPolyline Path;
    if (!Path.Build(CityBlockRoute.Points)) return Advice;
    const auto Actual = Path.Project(RobotState.ChassisPose.GetLocation());
    const auto* Objective = ObjectiveStation();
    if (!Actual.bValid || Actual.DistanceSquaredCm > FMath::Square(120.0) || !Objective) return Advice;
    const auto Goal = Path.Project(Objective->GetServicePoint());
    if (!Goal.bValid || Goal.DistanceSquaredCm > FMath::Square(25.0)) return Advice;
    const double Direction = Goal.ArcCm >= Actual.ArcCm ? 1.0 : -1.0;
    double Best = TNumericLimits<double>::Max();
    for (int32 I = 0; I < CityBlockRoute.Crossings.Num(); ++I)
    {
        const auto& Crossing = CityBlockRoute.Crossings[I];
        const double Entry = Direction > 0 ? Crossing.EntryArcCm : Crossing.ExitArcCm;
        const double Exit = Direction > 0 ? Crossing.ExitArcCm : Crossing.EntryArcCm;
        const double Wait = Direction > 0 ? Crossing.WaitingBeforeArcCm : Crossing.WaitingAfterArcCm;
        const bool Inside = Actual.ArcCm > Crossing.EntryArcCm && Actual.ArcCm < Crossing.ExitArcCm;
        const double Distance = (Entry - Actual.ArcCm) * Direction;
        if (!Inside && (Distance < 0 || Distance > 600 || (Entry - Goal.ArcCm) * Direction > 0)) continue;
        if (!Inside && Distance >= Best) continue;
        Best = Inside ? -1 : Distance;
        const bool Known = IsCityBlockRouteReady() && CityBlockObservation.Crossings.IsValidIndex(I)
            && CityBlockObservation.Crossings[I].bMatchedIntersection && CityBlockObservation.Crossings[I].bStateKnown;
        const bool Opened = Known && CityBlockObservation.Crossings[I].bOpened;
        Advice.Kind = Inside ? EMAYARouteAdvice::CrossingExit : !Known ? EMAYARouteAdvice::SignalUnavailable
            : Opened ? EMAYARouteAdvice::CrossingOpen : EMAYARouteAdvice::Yield;
        // Crowd admission can remain Closed when no real pedestrians wait.
        // Advice never promises a future green or locks the robot. Once near
        // the waiting point, keep the connected exit bearing ahead of the player.
        const double PointArc = Inside || Opened || (Wait - Actual.ArcCm) * Direction <= 90 ? Exit : Wait;
        Advice.Point = Path.Sample(PointArc);
        Advice.DistanceCm = FMath::Abs(PointArc - Actual.ArcCm);
        Advice.Text = Inside ? TEXT("Continue to the crossing exit") : !Known ? TEXT("Signal unavailable - check traffic")
            : Opened ? TEXT("Crossing open - check traffic") : TEXT("Crossing - yield to traffic");
    }
    return Advice;
}

bool AMAYADeliveryDirector::RequestSelectOffer(int32 Delta)
{
    if (Delta == 0 || !ReadRobot() || !GetPlayerPresentation().bCanChooseOffer) return false;
    RefreshCityBlockRoute();
    const int32 Count = GetAvailableProfileCount();
    const int32 Next = (FMath::Min(ProfileIndex, Count - 1) + (Delta < 0 ? -1 : 1) + Count) % Count;
    return OfferProfile(Next);
}

void AMAYADeliveryDirector::DestroyParcels()
{
    for (AMAYADeliveryParcel* P : Parcels) if (IsValid(P)) P->Destroy();
    Parcels.Reset();
}

bool AMAYADeliveryDirector::SpawnParcels()
{
    const auto& S = Rules.Get();
    AMAYADeliveryStation* Pickup = FindStation(S.Job.PickupStationId);
    if (!Pickup || !GetWorld() || S.Job.ParcelCount < 1 || S.Job.ParcelCount > 2 || Parcels.Num()) return false;
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
    for (int32 Slot = 0; Slot < S.Job.ParcelCount; ++Slot)
    {
        FTransform Stock = Pickup->GetParcelPose();
        Stock.AddToTranslation(Pickup->GetActorForwardVector() * (40.0 * Slot));
        AMAYADeliveryParcel* P = GetWorld()->SpawnActor<AMAYADeliveryParcel>(AMAYADeliveryParcel::StaticClass(), Stock, Params);
        if (!P) { DestroyParcels(); return false; }
        Parcels.Add(P);
        if (!P->Configure(ParcelIdentity(Slot), S.Job.PayloadKg / S.Job.ParcelCount))
        { DestroyParcels(); return false; }
    }
    return Parcels.Num() == S.Job.ParcelCount;
}

bool AMAYADeliveryDirector::StartShift()
{
    if (bPaused || bRecoveryPending || !GetWorld() || !ReadRobot()
        || !GetPlayerPresentation().bReadyForStart || !Rules.StartShift()) return false;
    // A controller/menu action may occur before this frame's postphysics Tick.
    // Its Dt includes preactivation time and cannot spend the initial100%.
    ShiftStartedGameFrame = GFrameCounter;
    RuntimeStatus.Empty();
    if (Robot) Robot->SetDeliveryDriveLocked(Rules.IsDriveLocked());
    return true;
}

FMAYAPlayerPresentation AMAYADeliveryDirector::GetPlayerPresentation() const
{
    FMAYAPlayerPresentation V;
    const auto& S = Rules.Get();
    V.bDroneTotalled = S.bDroneTotalled; V.bVehicleContactPending = S.bVehicleContactPending;
    // Whole-world teardown is safe even when the crashed native body is tipped,
    // unsupported or absent. It is not a claim that depot recovery is possible.
    if (S.bDroneTotalled)
    { V.bKnown = true; V.bCanStartNewShift = true; V.ActionReason = S.VehicleCrash.Reason; return V; }
    if (S.bVehicleContactPending)
    { V.ActionReason = TEXT("Checking actual vehicle contact. Controls and transactions are held briefly."); return V; }
    FMAYADeliveryRobotObservation Actual;
    if (!GetWorld() || !IsValid(Robot) || !Robot->ReadDeliveryState(Actual))
    { V.ActionReason = TEXT("Waiting for the robot to become ready."); return V; }
    const double Age = GetWorld()->GetTimeSeconds() - Actual.ObservationWorldSeconds;
    V.bKnown = FMath::IsFinite(Age) && Age >= 0 && Age <= .25 && Actual.bKnown;
    if (!V.bKnown) { V.ActionReason = TEXT("Waiting for a current robot observation."); return V; }
    const bool SafeClosed = Actual.IsStoppedAndSupported() && Actual.bDoorsClosedLatched;
    const bool Idle = ServiceStep == EServiceStep::None && !Rules.IsServiceCancellationPending() && !bRecoveryPending;
    V.bShiftWon = S.bShiftWon;
    // MainMenu and reset confirmation pause the world. Readiness deliberately
    // ignores that pause; it still requires the actual closed supported body.
    V.bReadyForStart = !S.bShiftStarted && !S.bShiftWon && S.Phase == EMAYADeliveryPhase::Available
        && SafeClosed && Idle && Parcels.IsEmpty() && S.BatteryPercent == 100 && S.TotalCredits == 0;
    V.bCanStartNewShift = S.bShiftStarted && SafeClosed && Idle
        && (S.bShiftWon || (S.BatteryPercent <= 0 && !FMAYADeliveryCashRules::CanAffordRecharge(S.TotalCredits)));
    const bool Gameplay = S.bShiftStarted && !S.bShiftWon && !bPaused;
    V.bCanAccept = Gameplay && Idle && SafeClosed && !S.bCharging
        && S.Phase == EMAYADeliveryPhase::Available && S.BatteryPercent > 0
        && (S.Job.ProfileId != TEXT("city_block") || IsCityBlockRouteReady());
    V.AvailableOfferCount = GetAvailableProfileCount(); V.SelectedOfferIndex = ProfileIndex;
    V.bCityBlockOfferAvailable = V.AvailableOfferCount == 4;
    V.bCanChooseOffer = Gameplay && Idle && SafeClosed && Parcels.IsEmpty()
        && GetSecuredParcelCount() == 0 && !Actual.bFreshMovementIntent
        && S.Phase == EMAYADeliveryPhase::Available && !S.bCharging
        && S.ChargeVisit.Stage != EMAYADeliveryChargeStage::Qualifying
        && S.ChargeVisit.Stage != EMAYADeliveryChargeStage::Charging;
    AMAYADeliveryStation* Objective = ObjectiveStation();
    V.bCanInteract = Gameplay && Idle && SafeClosed && !S.bCharging && Objective
        && (S.Phase == EMAYADeliveryPhase::Accepted || S.Phase == EMAYADeliveryPhase::Carrying)
        && FVector::Dist2D(Objective->GetServicePoint(), Actual.ChassisPose.GetLocation()) <= Rules.GetBalance().ServiceRadiusCm
        && FMath::Abs(Objective->GetServicePoint().Z - Actual.ChassisPose.GetLocation().Z) <= 100
        && Robot->CanRequestDeliveryDoorState(true);
    V.bCanNext = Gameplay && CanRequestNext(); V.bCanReplay = V.bCanNext;
    // R remains an ordinary guarded request. The presentation does not clear
    // faults, delete inventory or claim a reset volume is collision-free.
    V.bCanRecover = Gameplay && !bRecoveryPending
        && S.Phase != EMAYADeliveryPhase::Succeeded
        && (S.Phase != EMAYADeliveryPhase::Available || S.BatteryPercent <= 0 || !Actual.Fault.IsEmpty())
        && (SafeClosed || (Rules.IsServiceCancellationPending() && Actual.IsStoppedAndSupported() && Actual.bDoorsOpen)
            || !Actual.Fault.IsEmpty());
    V.bRecoveryPending = bRecoveryPending;
    V.bRecoveryRobotReady = bRecoveryPending && SafeClosed;
    V.bRecoveryCargoReady = bRecoveryPending && (!bRecoveryParcelExpected ? Parcels.IsEmpty()
        : Parcels.Num() == S.Job.ParcelCount && RecoveryParcelPoses.Num() == Parcels.Num());
    for (int32 Slot = 0; V.bRecoveryCargoReady && Slot < Parcels.Num(); ++Slot)
    {
        const AMAYADeliveryParcel* P = GetParcel(Slot); auto* B = P ? P->GetParcelBody() : nullptr;
        FTransform Native; double Kg = 0;
        const FVector Velocity = B ? B->GetPhysicsLinearVelocity() : FVector::ZeroVector;
        const FVector Omega = B ? B->GetPhysicsAngularVelocityInRadians() : FVector::ZeroVector;
        V.bRecoveryCargoReady = B && P->GetParcelId() == ParcelIdentity(Slot) && !P->IsDormantForDeliveryRemoval()
            && P->ObserveNativePose(Native) && P->ObserveMass(Kg)
            && FMath::Abs(Kg - S.Job.PayloadKg / S.Job.ParcelCount) <= .05
            && P->IsAt(RecoveryParcelPoses[Slot])
            && FVector::Dist(Native.GetLocation(), RecoveryParcelPoses[Slot].GetLocation()) <= 2
            && Native.GetRotation().AngularDistance(RecoveryParcelPoses[Slot].GetRotation()) <= FMath::DegreesToRadians(3.0)
            && FVector::Dist(Native.GetLocation(), B->GetComponentLocation()) <= .1
            && Native.GetRotation().AngularDistance(B->GetComponentQuat()) <= FMath::DegreesToRadians(.1)
            && B->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics && B->IsSimulatingPhysics()
            && !Velocity.ContainsNaN() && !Omega.ContainsNaN() && FMath::IsFinite(Velocity.SizeSquared()) && FMath::IsFinite(Omega.SizeSquared())
            && Velocity.SizeSquared() <= 25 && Omega.SizeSquared() <= .0025;
    }
    V.bServiceOpenObserved = bOpenObserved;
    V.bServiceTransferObserved = bTransferAdmitted && bInstantTransitionApplied;
    V.bServiceClosedObserved = bOpenObserved && V.bServiceTransferObserved && SafeClosed && ServiceStep == EServiceStep::None;
    const TCHAR* Names[] = { TEXT("Ready"), TEXT("Opening cargo doors"), TEXT("Parcels appearing"), TEXT("Lowering parcels"), TEXT("Settling parcels"), TEXT("Raising parcels"), TEXT("Settling raised parcels"), TEXT("Parcels disappearing"), TEXT("Verifying custody"), TEXT("Closing cargo doors"), TEXT("Opening for safe cancellation"), TEXT("Closing after cancellation"), TEXT("Cancellation blocked") };
    V.ServiceStage = bRecoveryPending ? TEXT("Recovering to depot") : Names[static_cast<uint8>(ServiceStep)];
    V.ActionReason = !Actual.Fault.IsEmpty() ? Actual.Fault : !RuntimeStatus.IsEmpty() ? RuntimeStatus : S.Status;
    const auto& C = S.ChargeVisit;
    switch (C.Stage)
    {
    case EMAYADeliveryChargeStage::Qualifying: V.Charge.Stage = EMAYAChargeVisitStage::Qualifying; break;
    case EMAYADeliveryChargeStage::Charging: V.Charge.Stage = EMAYAChargeVisitStage::Charging; break;
    case EMAYADeliveryChargeStage::Full: V.Charge.Stage = EMAYAChargeVisitStage::Full; break;
    case EMAYADeliveryChargeStage::Blocked: V.Charge.Stage = EMAYAChargeVisitStage::Blocked; break;
    default: V.Charge.Stage = EMAYAChargeVisitStage::None; break;
    }
    if (AMAYADeliveryStation* Station = FindStation(C.StationId)) V.Charge.StationName = Station->DisplayName;
    V.Charge.Reason = C.Reason; V.Charge.bInside = C.bInside; V.Charge.bEligible = C.bEligible;
    V.Charge.ElapsedSeconds = C.ElapsedSeconds; V.Charge.RemainingSeconds = C.RemainingSeconds;
    V.RouteAdvice = GetRouteAdvice();
    return V;
}

bool AMAYADeliveryDirector::RequestAccept()
{
    if (bPaused || bRecoveryPending || !ReadRobot() || !GetPlayerPresentation().bCanAccept) return false;
    if (!Rules.Accept(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens))) return false;
    DestroyParcels(); // available stock is not cargo; first bodies appear only after actual opening
    RuntimeStatus.Empty();
    return true;
}

AMAYADeliveryStation* AMAYADeliveryDirector::ObjectiveStation() const
{
    const auto& S = Rules.Get();
    return FindStation(S.Phase == EMAYADeliveryPhase::Carrying || S.Phase == EMAYADeliveryPhase::Unloading
        ? S.Job.DropoffStationId : S.Job.PickupStationId);
}

bool AMAYADeliveryDirector::RequestInteract()
{
    return RequestService(false);
}

bool AMAYADeliveryDirector::RequestService(bool bAutomatic)
{
    if (Rules.Get().bDroneTotalled || Rules.Get().bVehicleContactPending) return false;
    if (bPaused) return false;
    ReadRobot();
    UpdateRingVisit(0);
    // Only actual transaction admission consumes the visit. An unsafe early
    // explicit E or a fresh native rejection retains countdown eligibility.
    ++RingObservation.RequestGeneration;
    if (bAutomatic) ++RingObservation.AutomaticTriggerGeneration;
    RingObservation.LastRequestOrigin = bAutomatic ? TEXT("AutomaticRing") : TEXT("ExplicitInteraction");
    RingObservation.LastRequestWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
    RingObservation.LastRequestElapsedGameplaySeconds = RingObservation.ElapsedGameplaySeconds;
    RingObservation.LastRequestEntryGeneration = RingObservation.EntryGeneration;
    const uint64 BeforeServiceGeneration = Rules.Get().Service.Generation;
    RingObservation.LastRequestAccepted = TryStartService();
    RingObservation.LastRequestServiceGeneration = Rules.Get().Service.Generation;
    RingObservation.ConsumeIfAdmitted(RingObservation.LastRequestAccepted
        || (RingObservation.LastRequestServiceGeneration != 0 && RingObservation.LastRequestServiceGeneration != BeforeServiceGeneration));
    if (bAutomatic)
    {
        RingObservation.LastAutomaticRequestAccepted = RingObservation.LastRequestAccepted;
        RingObservation.LastAutomaticServiceGeneration = RingObservation.LastRequestServiceGeneration;
        RingObservation.LastAutomaticEntryGeneration = RingObservation.EntryGeneration;
        RingObservation.LastAutomaticRequestWorldSeconds = RingObservation.LastRequestWorldSeconds;
        RingObservation.LastAutomaticElapsedGameplaySeconds = RingObservation.LastRequestElapsedGameplaySeconds;
        RingObservation.LastAutomaticStationId = RingObservation.ActiveStationId;
    }
    return RingObservation.LastRequestAccepted;
}

bool AMAYADeliveryDirector::TryStartService()
{
    if (bPaused || bRecoveryPending || ServiceStep != EServiceStep::None || !ReadRobot()) return false;
    AMAYADeliveryStation* Station = ObjectiveStation();
    if (!Station || !Robot) return false;
    if (!Robot->CanRequestDeliveryDoorState(true))
    { RuntimeStatus = TEXT("Let the robot settle, then press E again."); return false; }
    if (!Rules.BeginService(Station->StationId, Station->GetServicePoint(), RobotState, ServiceToken)) return false;
    LastServiceFailureDetail.Empty(); LastServiceCommitEvidenceJson.Empty(); RuntimeStatus.Empty();
    ServiceOrigins.Reset(); ServiceParcelMasses.Reset();
    for (AMAYADeliveryParcel* P : Parcels)
    {
        ServiceOrigins.Add(IsValid(P) ? P->GetParcelBody()->GetComponentTransform() : FTransform::Identity);
        if (IsValid(P)) P->ResetTieDownInitializationEvidence();
    }
    bOpenObserved = bTransferAdmitted = bInstantTransitionApplied = bServiceCreatedInventory = false;
    EndpointWaitStartWorldSeconds = -1; RollbackStartWorldSeconds = -1; AirPhaseStartWorldSeconds = -1;
    AirPickupOpenStartWorldSeconds = AirPickupWindowStartWorldSeconds = AirPickupPreparationSampleWorldSeconds = -1;
    AirPickupPreparationWindowSeconds = AirPickupPeakBaySpeedCmps = AirPickupPeakAngularRadps = 0;
    AirPickupMaxBayExcursionCm = AirPickupMaxAngularExcursionDegrees = 0;
    AirPickupObservedBayMarginCm = AirPickupObservedAngularMarginDegrees = 0;
    AirPickupNativeComponentPositionDeltaCm = AirPickupNativeComponentAngularDeltaDegrees = 0;
    AirPickupPreparationWindowResets = 0; bAirPickupPreparationReady = false;
    ServiceStep = EServiceStep::Opening;
    Robot->SetDeliveryDriveLocked(true);
    if (!Robot->RequestDeliveryDoorState(true)) { InterruptService(TEXT("lids cannot open safely")); return false; }
    return true;
}

bool AMAYADeliveryDirector::BeginAirUnload()
{
    if (ServiceToken.bPickup || !Robot || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen
        || Parcels.Num() != Rules.Get().Job.ParcelCount || GetSecuredParcelCount() != Parcels.Num()) return false;
    FString Reason; ServiceParcelMasses.Reset();
    // Check every same-ID body and complete vertical path before releasing any.
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = GetParcel(Slot); double Kg = 0;
        if (!P || P->GetParcelId() != ParcelIdentity(Slot) || !P->ObserveMass(Kg)
            || FMath::Abs(Kg - Rules.Get().Job.PayloadKg / Parcels.Num()) > 0.05
            || !P->CanBeginAirRaising(Robot, Slot, Reason))
        { RuntimeStatus = TEXT("Unloading blocked. ") + Reason; return false; }
        ServiceParcelMasses.Add(Kg);
    }
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
        if (!Parcels[Slot]->BeginAirRaising(Robot, Slot, Reason))
        { RuntimeStatus = TEXT("Unloading blocked. ") + Reason; return false; }
    ServiceStep = EServiceStep::Raising; AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds();
    RuntimeStatus = TEXT("Raising parcels above the robot. Keep it stopped."); return true;
}

bool AMAYADeliveryDirector::IsCargoAirFadingOut() const
{
    return ServiceStep == EServiceStep::Dematerializing;
}

bool AMAYADeliveryDirector::CancelAirUnloadingForNativeTest()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (bNativeAirUnloadCancellationTestUsed || bPaused || !GetWorld() || !Robot
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoVideo"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01AirLoweringTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || ServiceStep != EServiceStep::Dematerializing || ServiceToken.bPickup
        || Rules.Get().Phase != EMAYADeliveryPhase::Unloading || !ReadRobot()
        || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen
        || GetSecuredParcelCount() != 0 || Parcels.Num() != Rules.Get().Job.ParcelCount
        || Rules.IsServiceCancellationPending() || bTransferAdmitted) return false;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        auto* P = GetParcel(Slot); double Alpha = 0; bool Restored = false;
        if (!P || P->GetParcelId() != ParcelIdentity(Slot) || !P->IsAirRaisingActive()
            || !P->IsAirRaisingEndpointReady() || !P->ObserveAirAppearance(Alpha, Restored)
            || Alpha <= 0 || Alpha >= 1) return false;
    }
    bNativeAirUnloadCancellationTestUsed = true;
    InterruptService(TEXT("explicit native raised-parcel fade cancellation regression (not an unsafe physics event)"));
    return Rules.IsServiceCancellationPending() && ServiceStep == EServiceStep::RollbackOpening;
#else
    return false;
#endif
}

bool AMAYADeliveryDirector::UpdateAirPickupPreparation()
{
    // Reserve half the unchanged fixed-target guards (0.5 cm / 0.1 degree).
    // Actual observed excursion over one continuous second avoids extrapolating
    // harmless solver-rate jitter. This preparation does NOT prove future motion;
    // actual open/support/pose/path admission is still checked every later tick.
    constexpr double WindowSeconds = 1.0;
    constexpr double PositionReserveCm = 0.25, AngularReserveDegrees = 0.05;
    auto ResetWindow = [this]()
    {
        if (AirPickupWindowStartWorldSeconds >= 0) ++AirPickupPreparationWindowResets;
        AirPickupWindowStartWorldSeconds = -1; AirPickupPreparationWindowSeconds = 0;
        bAirPickupPreparationReady = false;
    };
    UPrimitiveComponent* Body = CarrierBody();
    const FBodyInstance* BI = IsValid(Body) ? Body->GetBodyInstance() : nullptr;
    if (!GetWorld() || !ServiceToken.bPickup || !Robot || !RobotState.IsStoppedAndSupported()
        || !RobotState.bDoorsOpen || !Robot->IsDeliveryDriveLocked()
        || !BI || !BI->IsValidBodyInstance() || !Body->IsSimulatingPhysics()
        || Rules.Get().Job.ParcelCount < 1 || Rules.Get().Job.ParcelCount > 2)
    { ResetWindow(); return false; }
    const FTransform Native = BI->GetUnrealWorldTransform(false, true);
    const FTransform Component = Body->GetComponentTransform();
    const FVector V = Body->GetPhysicsLinearVelocity(), W = Body->GetPhysicsAngularVelocityInRadians();
    if (Native.ContainsNaN() || Component.ContainsNaN() || !Native.GetRotation().IsNormalized()
        || !Component.GetRotation().IsNormalized() || V.ContainsNaN() || W.ContainsNaN()
        || !FMath::IsFinite(V.SizeSquared()) || !FMath::IsFinite(W.SizeSquared()))
    { ResetWindow(); return false; }
    const double Now = GetWorld()->GetTimeSeconds();
    AirPickupPreparationSampleWorldSeconds = Now;
    AirPickupLastNativePose = Native;
    const double Omega = W.Size();
    FVector Bays[2]; double MaxBaySpeed = 0, MaxNativeComponentBayDelta = 0;
    for (int32 Slot = 0; Slot < Rules.Get().Job.ParcelCount; ++Slot)
    {
        UPrimitiveComponent* Mount = Robot->GetDeliveryCargoMount(Slot);
        if (!IsValid(Mount) || Mount->GetComponentTransform().ContainsNaN())
        { ResetWindow(); return false; }
        const FVector Local = Component.InverseTransformPosition(Mount->GetComponentLocation());
        Bays[Slot] = Native.TransformPosition(Local);
        const double Radius = FVector::Dist(Bays[Slot], Native.GetLocation());
        MaxBaySpeed = FMath::Max(MaxBaySpeed, V.Size() + Omega * Radius);
        MaxNativeComponentBayDelta = FMath::Max(MaxNativeComponentBayDelta,
            FVector::Dist(Bays[Slot], Mount->GetComponentLocation()));
    }
    if (AirPickupWindowStartWorldSeconds < 0)
    {
        AirPickupWindowStartWorldSeconds = Now; AirPickupInitialNativePose = Native;
        for (int32 Slot = 0; Slot < Rules.Get().Job.ParcelCount; ++Slot) AirPickupInitialBayCm[Slot] = Bays[Slot];
        AirPickupPeakBaySpeedCmps = AirPickupPeakAngularRadps = 0;
        AirPickupMaxBayExcursionCm = AirPickupMaxAngularExcursionDegrees = 0;
    }
    AirPickupPeakBaySpeedCmps = FMath::Max(AirPickupPeakBaySpeedCmps, MaxBaySpeed);
    AirPickupPeakAngularRadps = FMath::Max(AirPickupPeakAngularRadps, Omega);
    for (int32 Slot = 0; Slot < Rules.Get().Job.ParcelCount; ++Slot)
        AirPickupMaxBayExcursionCm = FMath::Max(AirPickupMaxBayExcursionCm,
            FVector::Dist(Bays[Slot], AirPickupInitialBayCm[Slot]));
    AirPickupMaxAngularExcursionDegrees = FMath::Max(AirPickupMaxAngularExcursionDegrees,
        double(FMath::RadiansToDegrees(Native.GetRotation().AngularDistance(AirPickupInitialNativePose.GetRotation()))));
    AirPickupNativeComponentPositionDeltaCm = MaxNativeComponentBayDelta;
    AirPickupNativeComponentAngularDeltaDegrees = FMath::RadiansToDegrees(
        Native.GetRotation().AngularDistance(Component.GetRotation()));
    AirPickupObservedBayMarginCm = AirPickupMaxBayExcursionCm + MaxNativeComponentBayDelta;
    AirPickupObservedAngularMarginDegrees = AirPickupMaxAngularExcursionDegrees
        + AirPickupNativeComponentAngularDeltaDegrees;
    if (!FMath::IsFinite(AirPickupObservedBayMarginCm) || !FMath::IsFinite(AirPickupObservedAngularMarginDegrees)
        || AirPickupObservedBayMarginCm > PositionReserveCm
        || AirPickupObservedAngularMarginDegrees > AngularReserveDegrees)
    { ResetWindow(); return false; }
    AirPickupPreparationWindowSeconds = Now - AirPickupWindowStartWorldSeconds;
    bAirPickupPreparationReady = AirPickupPreparationWindowSeconds >= WindowSeconds;
    return bAirPickupPreparationReady;
}

bool AMAYADeliveryDirector::BeginAirPickup()
{
    if (!ServiceToken.bPickup || !Robot || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen) return false;
    if (Parcels.Num() == 0)
    {
        bServiceCreatedInventory = true;
        if (!SpawnParcels()) return false;
    }
    if (Parcels.Num() != Rules.Get().Job.ParcelCount) return false;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = GetParcel(Slot); FString Reason;
        if (!P || P->GetParcelId() != ParcelIdentity(Slot) || !P->BeginAirLowering(Robot, Slot, Reason))
        { RuntimeStatus = TEXT("Loading blocked. ") + Reason; return false; }
    }
    AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds();
    ServiceStep = EServiceStep::Materializing;
    RuntimeStatus = TEXT("Parcels are appearing above the robot. Keep it stopped.");
    return true;
}

bool AMAYADeliveryDirector::IsCargoAirLowering() const
{
    return ServiceStep == EServiceStep::Lowering;
}

bool AMAYADeliveryDirector::IsCargoAirAnimationActive() const
{
    return ServiceStep == EServiceStep::Materializing || ServiceStep == EServiceStep::Lowering
        || ServiceStep == EServiceStep::AirEndpointWait || ServiceStep == EServiceStep::Raising
        || ServiceStep == EServiceStep::AirRemovalEndpointWait || ServiceStep == EServiceStep::Dematerializing;
}

double AMAYADeliveryDirector::GetCargoAirPhaseElapsedSeconds() const
{
    return IsCargoAirAnimationActive() && GetWorld() && AirPhaseStartWorldSeconds >= 0
        ? FMath::Max(0.0, double(GetWorld()->GetTimeSeconds()) - AirPhaseStartWorldSeconds) : 0;
}

bool AMAYADeliveryDirector::CancelAirLoweringForNativeTest()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (bNativeAirCancellationTestUsed || bPaused || !GetWorld()
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoVideo"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01AirLoweringTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || ServiceStep != EServiceStep::Lowering || !ServiceToken.bPickup || !bServiceCreatedInventory
        || Rules.Get().Phase != EMAYADeliveryPhase::Loading || !ReadRobot()
        || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen
        || GetSecuredParcelCount() != 0 || Parcels.Num() != Rules.Get().Job.ParcelCount
        || Rules.IsServiceCancellationPending() || bTransferAdmitted) return false;
    bNativeAirCancellationTestUsed = true;
    InterruptService(TEXT("explicit native mid-air lowering cancellation regression (not a physical unsafe excursion)"));
    return Rules.IsServiceCancellationPending() && ServiceStep == EServiceStep::RollbackOpening;
#else
    return false;
#endif
}

bool AMAYADeliveryDirector::AllInsertedParcelsSettled() const
{
    if (GetSecuredParcelCount() != Rules.Get().Job.ParcelCount) return false;
    for (AMAYADeliveryParcel* P : Parcels)
    {
        const FVector V = P->GetParcelBody()->GetPhysicsLinearVelocity();
        const FVector W = P->GetParcelBody()->GetPhysicsAngularVelocityInRadians();
        if (V.ContainsNaN() || W.ContainsNaN() || !FMath::IsFinite(V.SizeSquared()) || !FMath::IsFinite(W.SizeSquared())
            || V.SizeSquared() > 25.0 || W.SizeSquared() > 0.0025) return false;
    }
    return true;
}

bool AMAYADeliveryDirector::RestoreServiceInventory()
{
    if (!Robot || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen) return false;
    FString Reason;
    if (ServiceToken.bPickup)
    {
        if (bServiceCreatedInventory) { DestroyParcels(); return true; }
        if (ServiceOrigins.Num() != Parcels.Num()) return false;
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
        {
            AMAYADeliveryParcel* P = Parcels[Slot];
            if (!IsValid(P) || P->GetParcelId() != ParcelIdentity(Slot)
                || !P->PlaceForRecovery(ServiceOrigins[Slot], Reason))
            { RuntimeStatus = TEXT("Cargo rollback blocked. ") + Reason; return false; }
        }
        return true;
    }
    if (Parcels.Num() != Rules.Get().Job.ParcelCount) return false;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = Parcels[Slot];
        if (!IsValid(P) || P->GetParcelId() != ParcelIdentity(Slot)) return false;
        if ((P->IsDormantForDeliveryRemoval() || P->IsAirRaisingActive())
            && !P->RestoreToDeliverySlot(Robot, Slot, Reason))
        { RuntimeStatus = TEXT("Cargo rollback blocked. ") + Reason; return false; }
    }
    return GetSecuredParcelCount() == Rules.Get().Job.ParcelCount;
}

void AMAYADeliveryDirector::RecordServiceFailure(const FString& Stage, bool bNativeAdmissionWasAttempted, const FString& Reason)
{
    if (!LastServiceFailureDetail.IsEmpty()) return;
    // Inventory observation can perform successful native admission queries.
    // Freeze the actual first refusal before those diagnostics overwrite it.
    const FString FirstNativeDetail = Robot ? Robot->ReadDeliveryCargoTransferStatus() : TEXT("robot unavailable");
    const int32 ActualSecuredCount = GetSecuredParcelCount();
    LastServiceFailureDetail = FString::Printf(TEXT("stage=%s; world=%.6f; serviceGeneration=%llu; pickup=%s; nativeEndpointAttempted=%s; actorCount=%d; securedCount=%d; resetGeneration=%llu; status=%s; nativeDetail=%s"),
        *Stage, GetWorld() ? GetWorld()->GetTimeSeconds() : -1, ServiceToken.Generation,
        ServiceToken.bPickup ? TEXT("true") : TEXT("false"), bNativeAdmissionWasAttempted ? TEXT("true") : TEXT("false"),
        Parcels.Num(), ActualSecuredCount, RobotState.ResetGeneration, *RuntimeStatus, *FirstNativeDetail);
    LastServiceFailureDetail += FString::Printf(TEXT("; cancellationReason=%s; known=%s; ready=%s; grounded=%s; upright=%s; supportMask=%u; speedCmps=%.9f; angularSpeedRadps=%.9f; stoppedSupported=%s; doorsOpen=%s; doorsClosedLatched=%s; doorAnglesDeg=(%.9f,%.9f); doorOmegaRadps=(%.9f,%.9f); expectedResetGeneration=%llu; servicePositionDriftCm=%.9f; fault=%s; observedChassis={%s}"),
        *Reason, RobotState.bKnown ? TEXT("true") : TEXT("false"), RobotState.bReady ? TEXT("true") : TEXT("false"),
        RobotState.bGrounded ? TEXT("true") : TEXT("false"), RobotState.bUpright ? TEXT("true") : TEXT("false"),
        RobotState.SupportMask, RobotState.VelocityCmps.Size(), RobotState.AngularVelocityRadps.Size(),
        RobotState.IsStoppedAndSupported() ? TEXT("true") : TEXT("false"), RobotState.bDoorsOpen ? TEXT("true") : TEXT("false"),
        RobotState.bDoorsClosedLatched ? TEXT("true") : TEXT("false"), RobotState.DoorAngleDegrees[0], RobotState.DoorAngleDegrees[1],
        RobotState.DoorOmegaRadps[0], RobotState.DoorOmegaRadps[1], ServiceToken.RobotResetGeneration,
        FVector::Dist(RobotState.ChassisPose.GetLocation(), ServiceToken.RobotServicePosition), *RobotState.Fault,
        *ServicePoseTrace(RobotState.ChassisPose));
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot) if (IsValid(Parcels[Slot]))
        LastServiceFailureDetail += FString::Printf(TEXT("; slot%dId=%s; slot%dPose={%s}"), Slot, *Parcels[Slot]->GetParcelId(), Slot,
            *ServicePoseTrace(Parcels[Slot]->GetParcelBody()->GetComponentTransform()));
    UE_LOG(LogTemp, Warning, TEXT("MAYA instant service failure: %s"), *LastServiceFailureDetail);
}

void AMAYADeliveryDirector::InterruptService(const FString& Reason)
{
    RecordServiceFailure(TEXT("service_interrupt"), false, Reason);
    if (!Rules.IsServiceCancellationPending()) Rules.CancelService(ServiceToken, Reason);
    if (Robot) Robot->SetDeliveryDriveLocked(true);
    RuntimeStatus = TEXT("Service blocked. Closing safely; R recovers if the parcel cannot be restored.");
    if (ServiceStep == EServiceStep::RollbackBlocked) return;
    if (RollbackStartWorldSeconds < 0)
    {
        RollbackStartWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
        LastCancellationReason = Reason; ++CancellationGeneration;
    }
    ServiceStep = EServiceStep::RollbackOpening;
    // Opening/Closing are real native transitions. A duplicate open request is
    // intentionally refused by the pawn; wait for its actual endpoint instead
    // of converting that transitional refusal into permanent RollbackBlocked.
} 

bool AMAYADeliveryDirector::RequestNativeOpeningCancellationForTest()
{
#if UE_BUILD_SHIPPING || UE_BUILD_TEST
    return false;
#else
    if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01CancelOpeningTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01FullInput"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01BoostTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01OperatingTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01RingInteractionTest"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01VisualCapture"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01CargoVideo"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ServiceVideo"))
        || FParse::Param(FCommandLine::Get(), TEXT("MayaV01ObserveInput"))
        || bNativeOpeningCancellationTestUsed || bPaused || !Robot || !ReadRobot()
        || ServiceStep != EServiceStep::Opening || Rules.Get().Phase != EMAYADeliveryPhase::Loading
        || !ServiceToken.bPickup || bTransferAdmitted || bInstantTransitionApplied
        || Parcels.Num() != 0 || Rules.IsServiceCancellationPending()
        || !RobotState.IsStoppedAndSupported() || RobotState.bDoorsOpen || RobotState.bDoorsClosedLatched)
        return false;
    TSharedPtr<FJsonObject> Native;
    double NativeDoorState = -1;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()), Native)
        || !Native.IsValid() || !Native->TryGetNumberField(TEXT("doorState"), NativeDoorState)
        || NativeDoorState != 1) return false; // actual pawn Opening, not merely absent endpoints
    bNativeOpeningCancellationTestUsed = true;
    InterruptService(TEXT("explicit native Opening cancellation regression (not a physical unsafe excursion)"));
    return Rules.IsServiceCancellationPending() && ServiceStep == EServiceStep::RollbackOpening;
#endif
}

FString AMAYADeliveryDirector::ReadServiceCancellationEvidenceJson() const
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.ServiceCancellation.v1"));
    const TCHAR* Names[] = { TEXT("None"), TEXT("Opening"), TEXT("Materializing"), TEXT("Lowering"), TEXT("AirEndpointWait"), TEXT("Raising"), TEXT("AirRemovalEndpointWait"), TEXT("Dematerializing"), TEXT("Settling"), TEXT("Closing"), TEXT("RollbackOpening"), TEXT("RollbackClosing"), TEXT("RollbackBlocked") };
    J->SetStringField(TEXT("serviceStep"), Names[static_cast<uint8>(ServiceStep)]);
    J->SetStringField(TEXT("cancellationGeneration"), LexToString(CancellationGeneration));
    J->SetStringField(TEXT("lastCancellationReason"), LastCancellationReason);
    J->SetBoolField(TEXT("cancellationPending"), Rules.IsServiceCancellationPending());
    J->SetNumberField(TEXT("worldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
    J->SetNumberField(TEXT("rollbackStartedWorldSeconds"), RollbackStartWorldSeconds);
    J->SetNumberField(TEXT("rollbackBoundGameplaySeconds"), 15.0);
    J->SetStringField(TEXT("serviceGeneration"), LexToString(ServiceToken.Generation));
    J->SetBoolField(TEXT("currentStoppedSupported"), RobotState.IsStoppedAndSupported());
    J->SetBoolField(TEXT("originalResetGenerationMatches"), RobotState.ResetGeneration == ServiceToken.RobotResetGeneration);
    J->SetNumberField(TEXT("originalServicePositionDriftCm"), FVector::Dist(RobotState.ChassisPose.GetLocation(), ServiceToken.RobotServicePosition));
    J->SetBoolField(TEXT("currentDoorsOpen"), RobotState.bDoorsOpen);
    J->SetBoolField(TEXT("currentDoorsClosedLatched"), RobotState.bDoorsClosedLatched);
    J->SetBoolField(TEXT("currentNativeOpenRequestAllowed"), Robot && Robot->CanRequestDeliveryDoorState(true));
    J->SetBoolField(TEXT("explicitOpeningRegressionUsed"), bNativeOpeningCancellationTestUsed);
    J->SetStringField(TEXT("cancelledOpenServiceGeneration"), LexToString(CancelledOpenServiceGeneration));
    J->SetNumberField(TEXT("cancelledOpenObservationWorldSeconds"), CancelledOpenObservationWorldSeconds);
    J->SetBoolField(TEXT("thisServiceCancelledOpenEndpointsObserved"), WasCancelledOpenObserved(ServiceToken.Generation));
    FString Result; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
    FJsonSerializer::Serialize(J, Writer); return Result;
#else
    return TEXT("{}");
#endif
}

void AMAYADeliveryDirector::TickService(float)
{
    if (ServiceStep == EServiceStep::None || !Robot) return;
    if (ServiceStep == EServiceStep::RollbackBlocked) return;
    const bool bRollback = ServiceStep == EServiceStep::RollbackOpening || ServiceStep == EServiceStep::RollbackClosing;
    if (Rules.IsServiceCancellationPending() && !bRollback)
    { InterruptService(TEXT("service cancelled")); return; }
    if (bRollback && RollbackStartWorldSeconds >= 0
        && GetWorld()->GetTimeSeconds() - RollbackStartWorldSeconds > 15.0)
    {
        ServiceStep = EServiceStep::RollbackBlocked;
        RuntimeStatus = TEXT("Safe cancellation did not finish in time. R: recover to the depot.");
        return;
    }
    // The original-position tolerance invalidates the original transaction.
    // Cancellation instead verifies the CURRENT stopped support and the same
    // reconstruction generation, plus every original custody/endpoint guard.
    if (!RobotState.IsStoppedAndSupported() || RobotState.ResetGeneration != ServiceToken.RobotResetGeneration
        || (!bRollback && FVector::Dist(RobotState.ChassisPose.GetLocation(), ServiceToken.RobotServicePosition) > 5.0))
    {
        if (!bRollback)
        {
            const FString Cause = !RobotState.IsStoppedAndSupported() ? TEXT("current stopped/support predicate lost")
                : RobotState.ResetGeneration != ServiceToken.RobotResetGeneration ? TEXT("native reset generation changed")
                : TEXT("robot drifted beyond the original five-centimetre service position");
            InterruptService(Cause);
        }
        return;
    }
    if (ServiceStep == EServiceStep::Opening)
    {
        if (!RobotState.bDoorsOpen)
        {
            if (AirPickupWindowStartWorldSeconds >= 0) ++AirPickupPreparationWindowResets;
            AirPickupWindowStartWorldSeconds = -1; AirPickupPreparationWindowSeconds = 0;
            bAirPickupPreparationReady = false; return;
        }
        bOpenObserved = true;
        if (ServiceToken.bPickup)
        {
            const double Now = GetWorld()->GetTimeSeconds();
            if (AirPickupOpenStartWorldSeconds < 0) AirPickupOpenStartWorldSeconds = Now;
            // Separate preparation bound remains inside Core's original 12 s
            // service deadline, checked before a ready sample can start cargo.
            if (Now - AirPickupOpenStartWorldSeconds > 4.0)
            {
                RecordServiceFailure(TEXT("air_pickup_settled_open_preparation"), false,
                    TEXT("Native open robot did not meet the bounded fixed-pose capture margin"));
                InterruptService(TEXT("robot did not settle before parcel appearance")); return;
            }
            if (!UpdateAirPickupPreparation())
            { RuntimeStatus = TEXT("Settling the robot before parcels appear. Keep it stopped."); return; }
            if (!BeginAirPickup()) { RecordServiceFailure(TEXT("air_pickup_preparation"), true); InterruptService(TEXT("air pickup preparation blocked")); }
        }
        else if (!BeginAirUnload()) { RecordServiceFailure(TEXT("air_unloading_preparation"), true); InterruptService(TEXT("air unloading preparation blocked")); }
        return;
    }
    if (ServiceStep == EServiceStep::Materializing || ServiceStep == EServiceStep::Lowering
        || ServiceStep == EServiceStep::AirEndpointWait)
    {
        if (!RobotState.bDoorsOpen || !ServiceToken.bPickup || Parcels.Num() != Rules.Get().Job.ParcelCount)
        { InterruptService(TEXT("air pickup open endpoints or inventory changed")); return; }
        FString Reason;
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
        {
            AMAYADeliveryParcel* P = GetParcel(Slot); double Kg = 0;
            if (!P || P->GetParcelId() != ParcelIdentity(Slot) || !P->IsAirLoweringActive()
                || !P->ObserveMass(Kg) || FMath::Abs(Kg - Rules.Get().Job.PayloadKg / Parcels.Num()) > 0.05
                || !Robot->AdmitDeliveryCargoAirLowering(P->GetParcelBody(), Slot,
                    P->GetParcelBody()->GetComponentTransform(), P->GetAirLoweringTargetPose()))
            { RecordServiceFailure(TEXT("air_pickup_identity_or_path"), true); InterruptService(TEXT("air pickup path or parcel changed")); return; }
        }
        const double Age = GetCargoAirPhaseElapsedSeconds();
        if (ServiceStep == EServiceStep::Materializing)
        {
            const double U = FMath::Clamp(Age, 0.0, 1.0), Ease = U * U * (3.0 - 2.0 * U);
            for (AMAYADeliveryParcel* P : Parcels) if (!P->SetAirAppearanceAlpha(Ease))
            { InterruptService(TEXT("parcel appearance material was unavailable")); return; }
            if (Age >= 1.0)
            {
                ServiceStep = EServiceStep::Lowering; AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds();
                RuntimeStatus = TEXT("Lowering parcels into the compartments. Keep the robot stopped.");
            }
            return;
        }
        if (ServiceStep == EServiceStep::Lowering)
        {
            const double U = FMath::Clamp(Age / 2.0, 0.0, 1.0), Ease = U * U * (3.0 - 2.0 * U);
            for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
                if (!Parcels[Slot]->MoveAirLowering(Robot, Slot, Ease, Reason))
                { RuntimeStatus = TEXT("Loading blocked. ") + Reason; RecordServiceFailure(TEXT("air_pickup_actual_sweep"), true); InterruptService(Reason); return; }
            if (Age >= 2.0)
            { ServiceStep = EServiceStep::AirEndpointWait; AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds(); }
            return;
        }
        if (Age > 1.0) { InterruptService(TEXT("parcel native endpoint did not settle in time")); return; }
        for (AMAYADeliveryParcel* P : Parcels) if (!P->IsAirLoweringEndpointReady()) return;
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
            if (!Parcels[Slot]->FinishAirLowering(Robot, Slot, Reason))
            { RuntimeStatus = TEXT("Loading blocked. ") + Reason; RecordServiceFailure(TEXT("air_pickup_restraint"), true); InterruptService(Reason); return; }
        ServiceParcelMasses.Reset();
        for (AMAYADeliveryParcel* P : Parcels)
        {
            double Kg = 0;
            if (!P->ObserveMass(Kg) || FMath::Abs(Kg - Rules.Get().Job.PayloadKg / Parcels.Num()) > 0.05)
            { InterruptService(TEXT("parcel physical mass changed at loading completion")); return; }
            ServiceParcelMasses.Add(Kg);
        }
        bTransferAdmitted = bInstantTransitionApplied = true; // compatibility: authorized placement, not instantaneous pickup motion
        EndpointWaitStartWorldSeconds = GetWorld()->GetTimeSeconds(); ServiceStep = EServiceStep::Settling;
        RuntimeStatus = TEXT("Settling parcels. Keep the robot stopped."); return;
    }

if (ServiceStep == EServiceStep::Raising || ServiceStep == EServiceStep::AirRemovalEndpointWait
        || ServiceStep == EServiceStep::Dematerializing)
    {
        if (!RobotState.bDoorsOpen || ServiceToken.bPickup || Parcels.Num() != Rules.Get().Job.ParcelCount)
        { InterruptService(TEXT("air unloading open endpoints or inventory changed")); return; }
        FString Reason;
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
        {
            auto* P = GetParcel(Slot); double Kg = 0;
            if (!P || P->GetParcelId() != ParcelIdentity(Slot) || !P->IsAirRaisingActive()
                || !P->ObserveMass(Kg) || FMath::Abs(Kg - Rules.Get().Job.PayloadKg / Parcels.Num()) > 0.05
                || (ServiceStep != EServiceStep::Raising && !P->ValidateAirRaisingRemaining(Robot, Slot, Reason)))
            { RecordServiceFailure(TEXT("air_unloading_identity_or_path"), true); InterruptService(TEXT("air unloading actual path or inventory changed")); return; }
        }
        const double Age = GetCargoAirPhaseElapsedSeconds();
        if (ServiceStep == EServiceStep::Raising)
        {
            const double U = FMath::Clamp(Age / 2.0, 0.0, 1.0), Ease = U * U * (3.0 - 2.0 * U);
            for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
                if (!Parcels[Slot]->MoveAirRaising(Robot, Slot, Ease, Reason))
                { RecordServiceFailure(TEXT("air_unloading_actual_sweep"), true, Reason); InterruptService(Reason); return; }
            if (Age >= 2.0)
            { ServiceStep = EServiceStep::AirRemovalEndpointWait; AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds(); }
            return;
        }
        if (ServiceStep == EServiceStep::AirRemovalEndpointWait)
        {
            if (Age > 1.0) { InterruptService(TEXT("raised parcel native endpoint did not settle in time")); return; }
            for (AMAYADeliveryParcel* P : Parcels) if (!P->IsAirRaisingEndpointReady()) return;
            for (AMAYADeliveryParcel* P : Parcels) if (!P->BeginAirDisappearance())
            { InterruptService(TEXT("actual air unloading opacity material unavailable")); return; }
            ServiceStep = EServiceStep::Dematerializing; AirPhaseStartWorldSeconds = GetWorld()->GetTimeSeconds();
            RuntimeStatus = TEXT("Parcels are disappearing above the robot. Keep it stopped."); return;
        }
        const double U = FMath::Clamp(Age, 0.0, 1.0), Alpha = 1.0 - U * U * (3.0 - 2.0 * U);
        for (AMAYADeliveryParcel* P : Parcels) if (!P->IsAirRaisingEndpointReady() || !P->SetAirDisappearanceAlpha(Alpha))
        { InterruptService(TEXT("raised parcel native pose or opacity changed unexpectedly")); return; }
        if (Age < 1.0) return;
        // Validate every actual zero-opacity endpoint before hiding any body.
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
            if (!Parcels[Slot]->CanFinishAirRemoval(Robot, Slot, Reason))
            { RecordServiceFailure(TEXT("air_unloading_removal_admission"), true, Reason); InterruptService(Reason); return; }
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
            if (!Parcels[Slot]->FinishAirRemoval(Robot, Slot, Reason))
            { RecordServiceFailure(TEXT("air_unloading_removal"), true, Reason); InterruptService(Reason); return; }
        bTransferAdmitted = bInstantTransitionApplied = true; // legacy commit-admission fields only
        EndpointWaitStartWorldSeconds = GetWorld()->GetTimeSeconds(); ServiceStep = EServiceStep::Settling;
        RuntimeStatus = TEXT("Parcels removed. Closing the lids."); return;
    }

    if (ServiceStep == EServiceStep::Settling)
    {
        if (!RobotState.bDoorsOpen) { InterruptService(TEXT("lids left their open endpoints")); return; }
        // A genuine later postphysics sample remains visible to observation/tests.
        if (GetWorld()->GetTimeSeconds() - EndpointWaitStartWorldSeconds < 0.05) return;
        if (ServiceToken.bPickup && !AllInsertedParcelsSettled()) return;
        if (!ServiceToken.bPickup)
            for (AMAYADeliveryParcel* P : Parcels) if (!IsValid(P) || !P->IsDormantForDeliveryRemoval())
            { InterruptService(TEXT("parcel removal did not settle")); return; }
        if (!Robot->RequestDeliveryDoorState(false)) { InterruptService(TEXT("lids cannot close safely")); return; }
        ServiceStep = EServiceStep::Closing; RuntimeStatus = TEXT("Closing the lids. Keep the robot stopped."); return;
    }
    if (ServiceStep == EServiceStep::Closing)
    {
        if (!RobotState.bDoorsClosedLatched) return;
        FMAYADeliveryServiceEvidence E;
        E.Token = ServiceToken; E.Robot = RobotState; E.bOpenEndpointsObserved = bOpenObserved;
        E.bAuthorizedInstantTransferObserved = bInstantTransitionApplied;
        E.bActualPlacementEndpointsClear = bTransferAdmitted;
        E.ObservedParcelCount = Parcels.Num();
        E.bParcelAtCorrectDestination = E.bActualCustodyObserved = Parcels.Num() == Rules.Get().Job.ParcelCount
            && (!ServiceToken.bPickup || GetSecuredParcelCount() == Rules.Get().Job.ParcelCount);
        E.bParcelMassObserved = ServiceParcelMasses.Num() == Parcels.Num();
        for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
        {
            AMAYADeliveryParcel* P = Parcels[Slot];
            E.ObservedParcelIds.Add(IsValid(P) ? P->GetParcelId() : FString());
            if (!IsValid(P) || P->GetParcelId() != ParcelIdentity(Slot))
            { E.bParcelAtCorrectDestination = E.bActualCustodyObserved = false; continue; }
            double Kg = 0;
            if (ServiceToken.bPickup)
            {
                FTransform Target;
                const bool bSecured = Robot->GetDeliveryCargoPlacementTarget(P->GetParcelBody(), Slot, Target)
                    && P->IsSecuredTo(CarrierBody(), Target) && P->IsAt(Target);
                E.bParcelAtCorrectDestination &= bSecured; E.bActualCustodyObserved &= bSecured;
                E.bParcelMassObserved &= P->ObserveMass(Kg);
            }
            else
            {
                E.bParcelAtCorrectDestination &= P->IsDormantForDeliveryRemoval();
                E.bActualCustodyObserved &= P->IsDormantForDeliveryRemoval();
                if (ServiceParcelMasses.IsValidIndex(Slot)) Kg = ServiceParcelMasses[Slot];
                else E.bParcelMassObserved = false;
            }
            E.ObservedParcelKg += Kg;
        }
        E.ObservedParcelId = E.ObservedParcelIds.Num() ? E.ObservedParcelIds[0] : FString();
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        FTransform FirstPose, FirstTarget;
        const bool bFirstNative = ServiceToken.bPickup && GetParcel() && GetParcel()->ObserveNativePose(FirstPose);
        const bool bFirstTarget = ServiceToken.bPickup && GetParcel()
            && Robot->GetDeliveryCargoPlacementTarget(GetParcel()->GetParcelBody(), 0, FirstTarget);
        TSharedRef<FJsonObject> J = MakeServiceCommitReceipt(E, Rules.Get(), Rules.IsServiceCancellationPending(), FirstTarget, FirstPose);
        J->SetBoolField(TEXT("firstParcelCurrentNativePoseKnown"), bFirstNative);
        J->SetBoolField(TEXT("firstParcelCurrentTargetKnown"), bFirstTarget);
        if (!bFirstNative || !bFirstTarget)
        {
            J->RemoveField(TEXT("parcelTargetDistanceCm")); J->RemoveField(TEXT("parcelTargetAngularDeltaDegrees"));
            J->RemoveField(TEXT("actualTargetPose")); J->RemoveField(TEXT("actualParcelPose"));
            J->SetStringField(TEXT("poseComparisonScope"), TEXT("Not applicable: reversible removal has no physical parcel body at destination"));
        }
        J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.CargoServiceCommitEvidence.v2"));
        J->SetStringField(TEXT("pickupPresentation"), ServiceToken.bPickup ? TEXT("MeasuredAirFadeAndSweptLowering") : TEXT("MeasuredAirRiseAndSmoothDisappearance"));
        J->SetStringField(TEXT("authorizedInstantTransferFieldScope"), TEXT("Legacy authorized-placement/removal admission field; normal cargo motion is animated, not instantaneous"));
        J->SetNumberField(TEXT("expectedParcelCount"), Rules.Get().Job.ParcelCount);
        J->SetNumberField(TEXT("observedParcelCount"), E.ObservedParcelCount);
        J->SetBoolField(TEXT("authorizedInstantTransferObserved"), E.bAuthorizedInstantTransferObserved);
        J->SetBoolField(TEXT("actualPlacementEndpointsClear"), E.bActualPlacementEndpointsClear);
        J->SetNumberField(TEXT("cargoConditionPercent"), Rules.Get().CargoConditionPercent);
        FString Receipt; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Receipt);
        FJsonSerializer::Serialize(J, Writer); LastServiceCommitEvidenceJson = Receipt;
#endif
        if (!Rules.CommitService(E))
        { RecordServiceFailure(TEXT("core_commit")); InterruptService(TEXT("parcel or lids did not settle correctly")); return; }
        if (!ServiceToken.bPickup) DestroyParcels();
        ServiceStep = EServiceStep::None; RuntimeStatus.Empty(); return;
    }
    if (ServiceStep == EServiceStep::RollbackOpening)
    {
        if (!RobotState.bDoorsOpen)
        {
            if (Robot->CanRequestDeliveryDoorState(true) && !Robot->RequestDeliveryDoorState(true))
            { ServiceStep = EServiceStep::RollbackBlocked; RuntimeStatus = TEXT("Lids cannot open safely for cancellation. R: recover."); }
            return;
        }
        // Durable actual endpoint sample before the same-tick closing request;
        // observers ticking after Director need no artificial gameplay delay.
        CancelledOpenServiceGeneration = ServiceToken.Generation;
        CancelledOpenObservationWorldSeconds = RobotState.ObservationWorldSeconds;
        if (!RestoreServiceInventory())
        { RecordServiceFailure(TEXT("instant_rollback"), true); ServiceStep = EServiceStep::RollbackBlocked;
          RuntimeStatus = TEXT("Cargo rollback blocked. R: recover to the depot."); return; }
        if (!Robot->RequestDeliveryDoorState(false))
        { ServiceStep = EServiceStep::RollbackBlocked; RuntimeStatus = TEXT("Lids cannot close safely. R: recover."); return; }
        ServiceStep = EServiceStep::RollbackClosing; return;
    }
    if (ServiceStep == EServiceStep::RollbackClosing && RobotState.bDoorsClosedLatched)
    {
        bool bStockRestored = bServiceCreatedInventory ? GetParcelCount() == 0
            : Parcels.Num() == ServiceOrigins.Num() && (ServiceOrigins.IsEmpty() || Parcels.Num() == Rules.Get().Job.ParcelCount);
        if (ServiceToken.bPickup && !bServiceCreatedInventory)
            for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
            {
                AMAYADeliveryParcel* P = GetParcel(Slot); FTransform Native; double Kg = 0;
                bStockRestored &= P && P->GetParcelId() == ParcelIdentity(Slot)
                    && !P->IsDormantForDeliveryRemoval() && P->ObserveMass(Kg)
                    && FMath::Abs(Kg - Rules.Get().Job.PayloadKg / Rules.Get().Job.ParcelCount) <= 0.05
                    && ServiceOrigins.IsValidIndex(Slot) && P->IsAt(ServiceOrigins[Slot])
                    && P->ObserveNativePose(Native) && FVector::Dist(Native.GetLocation(), ServiceOrigins[Slot].GetLocation()) <= 2.0
                    && Native.GetRotation().AngularDistance(ServiceOrigins[Slot].GetRotation()) <= FMath::DegreesToRadians(3.0)
                    && P->GetParcelBody()->IsSimulatingPhysics()
                    && P->GetParcelBody()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics;
            }
        const bool bCustody = ServiceToken.bPickup ? bStockRestored && GetSecuredParcelCount() == 0
            : GetSecuredParcelCount() == Rules.Get().Job.ParcelCount;
        if (bCustody && Rules.ConfirmCancelledClosure(RobotState))
        { ServiceStep = EServiceStep::None; RuntimeStatus.Empty(); }
    }
}

AMAYADeliveryStation* AMAYADeliveryDirector::GetNearbyChargingStation() const
{
    if (!RobotState.bKnown || RobotState.ChassisPose.ContainsNaN()) return nullptr;
    AMAYADeliveryStation* Nearest = nullptr;
    double BestDistance = AMAYADeliveryStation::ChargingRadiusCm;
    for (AMAYADeliveryStation* Station : Stations)
    {
        if (!IsValid(Station) || !Station->IsChargingStation()) continue;
        const double Distance = FVector::Dist2D(Station->GetServicePoint(), RobotState.ChassisPose.GetLocation());
        if (FMath::IsFinite(Distance) && Distance <= BestDistance)
        { Nearest = Station; BestDistance = Distance; }
    }
    return Nearest;
}

FString AMAYADeliveryDirector::GetChargingStationName() const
{
    const AMAYADeliveryStation* Station = GetNearbyChargingStation();
    return Station ? Station->DisplayName : FString();
}

bool AMAYADeliveryDirector::AtCharger() const
{
    return GetNearbyChargingStation() != nullptr;
}

bool AMAYADeliveryDirector::RequestCharging()
{
    // Compatibility API is deliberately not a player connect/disconnect action.
    RuntimeStatus = TEXT("Charging is automatic after ten eligible seconds in a green ring and costs $2. Move to stop.");
    return false;
}

FMAYADeliveryChargingObservation AMAYADeliveryDirector::ReadChargingObservation() const
{
    FMAYADeliveryChargingObservation C;
    C.WorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
    AMAYADeliveryStation* Station = GetNearbyChargingStation();
    C.bInside = Station != nullptr; if (Station) C.StationId = Station->StationId;
    C.bRecoveryPending = bRecoveryPending;
    C.bFreshMovementIntent = RobotState.bFreshMovementIntent;
    AMAYADeliveryStation* Objective = ObjectiveStation();
    // Service owns overlapping rings while an active mission has usable energy.
    // At actual depletion, ordinary recovery can reach paid charging at depot.
    C.bObjectiveServicePriority = Rules.Get().BatteryPercent > 0 && Rules.IsActive() && Objective
        && RobotState.bKnown && FVector::Dist2D(Objective->GetServicePoint(), RobotState.ChassisPose.GetLocation()) <= AMAYADeliveryStation::ObjectiveRingRadiusCm;
    return C;
}

bool AMAYADeliveryDirector::RequestNext(bool bRetry)
{
    if (!ReadRobot() || !CanRequestNext()) return false;
    RefreshCityBlockRoute();
    if (bRetry && ProfileIndex >= GetAvailableProfileCount()) return false;
    DestroyParcels();
    return OfferProfile(bRetry ? ProfileIndex : (FMath::Min(ProfileIndex, GetAvailableProfileCount()-1) + 1) % GetAvailableProfileCount());
}

// Merge these two methods into the count-aware Director; no independent TU.
bool AMAYADeliveryDirector::RequestRecover()
{
    if (Rules.Get().bDroneTotalled || Rules.Get().bVehicleContactPending) return false;
    if (bPaused || !Rules.Get().bShiftStarted || Rules.Get().bShiftWon || !Robot || bRecoveryPending || !ReadRobot()
        || Rules.Get().Phase == EMAYADeliveryPhase::Succeeded
        || (Rules.Get().Phase == EMAYADeliveryPhase::Available && Rules.Get().BatteryPercent > 0 && RobotState.Fault.IsEmpty())) return false;
    bool bHasDormant = false;
    for (AMAYADeliveryParcel* P : Parcels) bHasDormant |= IsValid(P) && (P->IsDormantForDeliveryRemoval() || P->IsAirRaisingActive());
    if (bHasDormant)
    {
        Robot->SetDeliveryDriveLocked(true);
        if (ServiceToken.bPickup || !Rules.IsServiceCancellationPending())
        { RuntimeStatus = TEXT("Recovery blocked. Pending parcel ownership must be restored safely."); return false; }
        if (!RobotState.IsStoppedAndSupported() || !RobotState.bDoorsOpen)
        {
            if (Robot->CanRequestDeliveryDoorState(true) && Robot->RequestDeliveryDoorState(true))
            { ServiceStep = EServiceStep::RollbackOpening; RuntimeStatus = TEXT("Opening the lids to restore the parcels. Press R again when safe."); }
            else RuntimeStatus = TEXT("Recovery blocked. The robot must be stable with safely open lids.");
            return false;
        }
        // Explicit R can retry a previously blocked reversible restore after
        // the real obstruction clears. Same bodies/IDs and endpoint guards apply.
        if (!RestoreServiceInventory())
        { RuntimeStatus = TEXT("Recovery blocked. Clear the parcel bays and press R again."); return false; }
        if (!ReadRobot()) return false;
    }
    LastRecoveryDetail.Empty();
    Robot->SetDeliveryDriveLocked(true);
    const auto S = Rules.Get();
    UPrimitiveComponent* PriorCarrier = CarrierBody();
    const FTransform PriorCarrierPose = PriorCarrier ? PriorCarrier->GetComponentTransform() : FTransform::Identity;
    const uint64 PriorResetGeneration = RobotState.ResetGeneration;
    struct FPriorCargo
    {
        AMAYADeliveryParcel* Actor = nullptr;
        UPrimitiveComponent* Body = nullptr;
        FString Id;
        FTransform Pose = FTransform::Identity;
        FVector Velocity = FVector::ZeroVector, Omega = FVector::ZeroVector;
        double Kg = 0;
        bool bSecured = false, bSimulating = false;
    };
    TArray<FPriorCargo> Prior;
    TArray<FTransform> ReturnPoses;
    AMAYADeliveryStation* Pickup = Parcels.Num() ? FindStation(S.Job.PickupStationId) : nullptr;
    if (Parcels.Num() && (!Pickup || Parcels.Num() != S.Job.ParcelCount || S.Job.ParcelCount < 1 || S.Job.ParcelCount > 2))
    {
        LastRecoveryDetail = TEXT("stage=inventory_preflight; accepted=false; actual count/station mismatch");
        RuntimeStatus = TEXT("Recovery blocked. The parcel inventory is incomplete."); return false;
    }
    const double EachKg = S.Job.ParcelCount > 0 ? S.Job.PayloadKg / S.Job.ParcelCount : 0;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = Parcels[Slot];
        FPriorCargo C; C.Actor = P; C.Body = IsValid(P) ? P->GetParcelBody() : nullptr;
        FTransform Native, SlotTarget;
        if (C.Body)
        {
            C.Id = P->GetParcelId(); C.Pose = C.Body->GetComponentTransform();
            C.Velocity = C.Body->GetPhysicsLinearVelocity(); C.Omega = C.Body->GetPhysicsAngularVelocityInRadians();
            C.bSimulating = C.Body->IsSimulatingPhysics();
            C.bSecured = Robot->GetDeliveryCargoPlacementTarget(C.Body, Slot, SlotTarget)
                && P->IsSecuredTo(PriorCarrier, SlotTarget);
        }
        if (!C.Body || C.Id != ParcelIdentity(Slot) || P->IsDormantForDeliveryRemoval()
            || C.Body->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
            || C.Pose.ContainsNaN() || C.Velocity.ContainsNaN() || C.Omega.ContainsNaN()
            || !FMath::IsFinite(C.Velocity.SizeSquared()) || !FMath::IsFinite(C.Omega.SizeSquared())
            || !P->ObserveMass(C.Kg) || FMath::Abs(C.Kg - EachKg) > 0.05
            || !P->ObserveNativePose(Native)
            || FVector::Dist(Native.GetLocation(), C.Pose.GetLocation()) > 0.1
            || Native.GetRotation().AngularDistance(C.Pose.GetRotation()) > FMath::DegreesToRadians(0.1)
            || (S.Phase == EMAYADeliveryPhase::Carrying && !C.bSecured))
        {
            LastRecoveryDetail = FString::Printf(TEXT("stage=inventory_preflight; accepted=false; slot=%d; identity/mass/native pose/restraint unavailable"), Slot);
            RuntimeStatus = TEXT("Recovery blocked. The parcels are not ready to reset safely."); return false;
        }
        FTransform Stock = Pickup->GetParcelPose();
        Stock.AddToTranslation(Pickup->GetActorForwardVector() * (40.0 * Slot));
        Stock.SetScale3D(C.Pose.GetScale3D()); Stock.SetRotation(C.Pose.GetRotation());
        FString Reason;
        if (!P->CanUseRecoveryPose(Stock, Reason))
        {
            LastRecoveryDetail = FString::Printf(TEXT("stage=parcel_destination; accepted=false; slot=%d; reason=%s"), Slot, *Reason);
            RuntimeStatus = TEXT("Recovery blocked. Clear the parcels' depot spaces."); return false;
        }
        Prior.Add(C); ReturnPoses.Add(Stock);
    }
    const auto RestorePrior = [&]()
    {
        if (Prior.IsEmpty()) return true;
        if (!ReadRobot() || !PriorCarrier || CarrierBody() != PriorCarrier
            || RobotState.ResetGeneration != PriorResetGeneration
            || FVector::Dist(PriorCarrier->GetComponentLocation(), PriorCarrierPose.GetLocation()) > 0.1
            || PriorCarrier->GetComponentQuat().AngularDistance(PriorCarrierPose.GetRotation()) > FMath::DegreesToRadians(0.1))
        { LastRecoveryDetail += TEXT("; rollbackReason=carrier changed or unavailable"); return false; }
        bool AllRestored = true;
        for (int32 Slot = 0; Slot < Prior.Num(); ++Slot)
        {
            const FPriorCargo& C = Prior[Slot]; FString Reason;
            if (!Parcels.IsValidIndex(Slot) || Parcels[Slot] != C.Actor || !IsValid(C.Actor)
                || C.Actor->GetParcelBody() != C.Body || C.Actor->GetParcelId() != C.Id
                || !C.Actor->PlaceForRecovery(C.Pose, Reason))
            {
                LastRecoveryDetail += FString::Printf(TEXT("; rollbackSlot=%d; reason=%s"), Slot, *Reason);
                AllRestored = false; continue;
            }
            bool Restored = true;
            if (C.bSecured) Restored = C.Actor->SecureTo(PriorCarrier, C.Pose);
            else if (!C.bSimulating) Restored = C.Actor->BeginStaging();
            if (Restored && C.bSimulating)
            { C.Body->SetPhysicsLinearVelocity(C.Velocity); C.Body->SetPhysicsAngularVelocityInRadians(C.Omega); }
            FTransform Native; double Kg = 0;
            Restored = Restored && C.Actor->IsAt(C.Pose, 0.1) && C.Actor->ObserveNativePose(Native)
                && FVector::Dist(Native.GetLocation(), C.Pose.GetLocation()) <= 0.1
                && Native.GetRotation().AngularDistance(C.Pose.GetRotation()) <= FMath::DegreesToRadians(0.1)
                && C.Actor->ObserveMass(Kg) && FMath::Abs(Kg - C.Kg) <= 0.05
                && C.Body->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics
                && C.Body->IsSimulatingPhysics() == C.bSimulating
                && (!C.bSecured || C.Actor->IsSecuredTo(PriorCarrier, C.Pose));
            AllRestored = AllRestored && Restored;
        }
        return AllRestored;
    };
    for (int32 Slot = 0; Slot < Prior.Num(); ++Slot)
    {
        FString Reason;
        if (!Prior[Slot].Actor->PlaceForRecovery(ReturnPoses[Slot], Reason))
        {
            LastRecoveryDetail = FString::Printf(TEXT("stage=parcel_reconstruction; accepted=false; slot=%d; reason=%s"), Slot, *Reason);
            const bool Restored = RestorePrior();
            LastRecoveryDetail += FString::Printf(TEXT("; rollbackRestored=%s"), Restored ? TEXT("true") : TEXT("false"));
            if (!Restored) Rules.Fail(TEXT("Recovery could not restore every physical parcel."));
            RuntimeStatus = TEXT("Recovery blocked. The parcels need clear depot spaces."); return false;
        }
    }
    if (!Robot->ResetDeliveryRobot())
    {
        LastRecoveryDetail = FString::Printf(TEXT("stage=robot_reconstruction; accepted=false; sameParcelCountAtDepot=%d; unrelated reset blockers retained"), Prior.Num());
        const bool Restored = RestorePrior();
        LastRecoveryDetail += FString::Printf(TEXT("; rollbackRestored=%s"), Restored ? TEXT("true") : TEXT("false"));
        if (!Restored) Rules.Fail(TEXT("Recovery is blocked; not all parcel restraints were restored."));
        RuntimeStatus = Restored ? TEXT("Recovery blocked. Clear the depot and try again.")
            : TEXT("Recovery blocked. The parcel restraints could not be restored."); return false;
    }
    RecoveryParcelPoses = ReturnPoses;
    bRecoveryParcelExpected = !Prior.IsEmpty(); bRecoveryPending = true; ServiceStep = EServiceStep::None;
    LastRecoveryDetail = FString::Printf(TEXT("stage=awaiting_settlement; accepted=true; priorResetGeneration=%llu; actualParcelCount=%d; collisionRetained=true; serviceOrRewardCommitted=false"), PriorResetGeneration, Prior.Num());
    for (int32 Slot = 0; Slot < Prior.Num(); ++Slot)
        LastRecoveryDetail += FString::Printf(TEXT("; slot=%d; parcelId=%s; nativeKg=%.9f; prior={%s}; depot={%s}"),
            Slot, *Prior[Slot].Id, Prior[Slot].Kg, *ServicePoseTrace(Prior[Slot].Pose), *ServicePoseTrace(ReturnPoses[Slot]));
    RuntimeStatus = TEXT("Recovering to the depot. Waiting for the robot and parcels to settle."); return true;
}

void AMAYADeliveryDirector::CompleteRecovery()
{
    if (!bRecoveryPending || !ReadRobot() || !RobotState.IsStoppedAndSupported() || !RobotState.bDoorsClosedLatched) return;
    const auto S = Rules.Get();
    if (bRecoveryParcelExpected && (Parcels.Num() != S.Job.ParcelCount || Parcels.Num() != RecoveryParcelPoses.Num())) return;
    if (!bRecoveryParcelExpected && !Parcels.IsEmpty()) return;
    const double EachKg = S.Job.ParcelCount > 0 ? S.Job.PayloadKg / S.Job.ParcelCount : 0;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = Parcels[Slot]; auto* B = IsValid(P) ? P->GetParcelBody() : nullptr;
        FTransform Native; double Kg = 0;
        const FTransform& Target = RecoveryParcelPoses[Slot];
        if (!B || P->GetParcelId() != ParcelIdentity(Slot) || P->IsDormantForDeliveryRemoval()
            || !P->ObserveMass(Kg) || FMath::Abs(Kg - EachKg) > 0.05 || !P->IsAt(Target)
            || !P->ObserveNativePose(Native) || FVector::Dist(Native.GetLocation(), Target.GetLocation()) > 2.0
            || Native.GetRotation().AngularDistance(Target.GetRotation()) > FMath::DegreesToRadians(3.0)
            || FVector::Dist(Native.GetLocation(), B->GetComponentLocation()) > 0.1
            || Native.GetRotation().AngularDistance(B->GetComponentQuat()) > FMath::DegreesToRadians(0.1)
            || B->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics || !B->IsSimulatingPhysics()) return;
        const FVector V = B->GetPhysicsLinearVelocity(), W = B->GetPhysicsAngularVelocityInRadians();
        if (V.ContainsNaN() || W.ContainsNaN() || !FMath::IsFinite(V.SizeSquared()) || !FMath::IsFinite(W.SizeSquared())
            || V.SizeSquared() > 25.0 || W.SizeSquared() > 0.0025) return;
    }
    if (S.Phase != EMAYADeliveryPhase::Available && !Rules.RecoverAfterNativeReset(RobotState)) return;
    bRecoveryPending = false; bRecoveryParcelExpected = false;
    LastRecoveryDetail += TEXT("; allParcelNativeSettlement=true; settledRecoveryCommitted=true");
    if (Rules.Get().Phase == EMAYADeliveryPhase::Available) DestroyParcels();
    RuntimeStatus = S.BatteryPercent <= 0 && S.TotalCredits < 2
        ? TEXT("Recovery complete. Start a new shift to restore the battery.")
        : TEXT("Recovery complete. Cash and battery have been preserved.");
    // An active recovered job keeps the same real stock actors/IDs for reload.
}

void AMAYADeliveryDirector::Tick(float Dt)
{
    Super::Tick(Dt);
    ReadRobot();
    ConsumeVehicleContacts();
    RefreshCityBlockRoute();
    if (bPaused || Rules.Get().bDroneTotalled)
    { if (Robot) Robot->SetDeliveryDriveLocked(true); return; }
    if (Rules.Get().bVehicleContactPending)
    { Rules.Tick(Dt, false, RobotState, ReadChargingObservation()); if (Robot) Robot->SetDeliveryDriveLocked(true); return; }
    CompleteRecovery();
    const double EconomyDt = Rules.Get().bShiftStarted && GFrameCounter == ShiftStartedGameFrame ? 0.0 : double(Dt);
    Rules.Tick(EconomyDt, bPaused, RobotState, ReadChargingObservation());
    // Hit callbacks precede postphysics authority. Apply their measured receipts
    // only after the current native observation has advanced Core's clock.
    for (const FMAYADeliveryImpactEvent& Event : PendingChassisImpacts) Rules.ApplyImpactEvent(Event);
    PendingChassisImpacts.Reset();
    if (Rules.Get().Phase == EMAYADeliveryPhase::Carrying && !bRecoveryPending
        && GetSecuredParcelCount() != Rules.Get().Job.ParcelCount)
        Rules.Fail(TEXT("Cargo restraint was lost. Recover to the depot."));
    TickService(Dt);
    TickAutomaticService(float(EconomyDt));
    if (Robot) Robot->SetDeliveryDriveLocked(Rules.IsDriveLocked() || bRecoveryPending);
    AMAYADeliveryStation* Objective = ObjectiveStation();
    const bool bDelivery = Rules.Get().Phase == EMAYADeliveryPhase::Carrying || Rules.Get().Phase == EMAYADeliveryPhase::Unloading;
    AMAYADeliveryStation* ChargingStation = Rules.Get().bCharging ? GetNearbyChargingStation() : nullptr;
    for (AMAYADeliveryStation* Station : Stations) if (Station)
    {
        Station->SetChargingVisualState(Station == ChargingStation);
        Station->SetObjectiveVisualState(Station == Objective && Rules.IsActive(), bDelivery,
            RobotState.bKnown && FVector::Dist2D(Station->GetServicePoint(), RobotState.ChassisPose.GetLocation()) <= AMAYADeliveryStation::ObjectiveRingRadiusCm);
    }
    PreviousChassisVelocity = RobotState.VelocityCmps;
    for (auto It = ContactBursts.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || GetWorld()->GetTimeSeconds() - It.Value().LastContactWorldSeconds > 3.0) It.RemoveCurrent();
}

void AMAYADeliveryDirector::OnRobotHit(UPrimitiveComponent* HitComponent, AActor* Other, UPrimitiveComponent* OtherComponent,
    FVector Impulse, const FHitResult& Hit)
{
    if (bPaused || !GetWorld() || GetWorld()->IsPaused() || Other == Robot
        || !IsValid(OtherComponent) || !ImpactBodies.Contains(HitComponent)
        || Impulse.ContainsNaN() || Hit.ImpactNormal.ContainsNaN()) return;
    for (AMAYADeliveryParcel* P : Parcels) if (Other == P) return;
    if (VehicleContactObserver)
    { VehicleContactObserver->ObserveNativeHit(HitComponent, Other, OtherComponent, Impulse, Hit); ConsumeVehicleContacts(); }
    if (Rules.Get().bDroneTotalled)
    { if (Robot) Robot->SetDeliveryDriveLocked(true); return; }
    // Preserve ordinary measured cargo-impact receipts during a short identity
    // hold; they are applied only once adjudication releases the barrier.
    if (Rules.Get().bVehicleContactPending && Robot) Robot->SetDeliveryDriveLocked(true);
    const double Now = GetWorld()->GetTimeSeconds();
    FContactBurst& Burst = ContactBursts.FindOrAdd(OtherComponent);
    const double Quiet = Burst.LastContactWorldSeconds < 0 ? 1000.0 : Now - Burst.LastContactWorldSeconds;
    // Update EVERY contact, including subthreshold rumble. Only an observed
    // no-contact gap rearms a source burst; elapsed time since first hit does not.
    Burst.LastContactWorldSeconds = Now;
    if (Burst.Generation == 0 || Quiet >= 0.6)
    { Burst.Generation = ++NextChassisImpactGeneration; Burst.PeakImpulseKgMps = Burst.PeakApproachCmps = 0; }
    if (FMath::Abs(Hit.ImpactNormal.Z) >= 0.5) return; // road/overhead support is not a lateral bump
    const FVector OtherVelocity = OtherComponent->GetComponentVelocity();
    const double Approach = FMath::Max(0.0, -FVector::DotProduct(PreviousChassisVelocity - OtherVelocity, Hit.ImpactNormal.GetSafeNormal()));
    const double KgMps = Impulse.Size() / 100.0;
    if (!FMath::IsFinite(Approach) || !FMath::IsFinite(KgMps) || Approach < FMAYADeliveryLateralImpactCalibration::MinimumApproachCmps
        || KgMps < FMAYADeliveryLateralImpactCalibration::MinimumImpulseKgMps) return;
    // Preserve a SAME-callback impulse/speed/component pair. Independent
    // maxima could combine a fast gentle contact and later slow large push
    // into a hard event that never physically occurred.
    const auto ClassifiedSeverity = [](double ContactImpulse, double ContactApproach)
    {
        return ContactImpulse >= FMAYADeliveryLateralImpactCalibration::HardImpulseKgMps
            && ContactApproach >= FMAYADeliveryLateralImpactCalibration::HardApproachCmps ? 2 : 1;
    };
    const int32 Severity = ClassifiedSeverity(KgMps, Approach);
    const int32 PriorSeverity = Burst.PeakImpulseKgMps > 0
        ? ClassifiedSeverity(Burst.PeakImpulseKgMps, Burst.PeakApproachCmps) : 0;
    if (Severity > PriorSeverity || (Severity == PriorSeverity && KgMps > Burst.PeakImpulseKgMps))
    {
        Burst.PeakImpulseKgMps = KgMps; Burst.PeakApproachCmps = Approach;
        Burst.PeakRobotComponent = GetPathNameSafe(HitComponent);
    }
    FMAYADeliveryImpactEvent Event;
    Event.Generation = Burst.Generation; Event.Kind = TEXT("ChassisImpact"); Event.WorldSeconds = Now;
    Event.ImpulseKgMps = Burst.PeakImpulseKgMps; Event.ApproachSpeedCmps = Burst.PeakApproachCmps;
    Event.QuietSecondsBeforeEvent = Quiet;
    Event.OtherActor = IsValid(Other) ? Other->GetPathName() : FString(); Event.OtherComponent = OtherComponent->GetPathName();
    Event.RobotComponent = Burst.PeakRobotComponent;
    // Peak actual callback impulse, never a sum across wheel/substep callbacks.
    // All bodies share OtherComponent's burst generation, so Core charges once.
    // Keep only the strongest pending receipt for each actual contact burst.
    for (FMAYADeliveryImpactEvent& Pending : PendingChassisImpacts)
        if (Pending.Generation == Event.Generation) { Pending = Event; return; }
    if (PendingChassisImpacts.Num() < 64) PendingChassisImpacts.Add(Event);
}

FString AMAYADeliveryDirector::GetPrompt() const
{
    const auto& S = Rules.Get();
    if (S.bDroneTotalled) return S.VehicleCrash.Reason;
    if (S.bVehicleContactPending) return TEXT("Checking actual vehicle contact.");
    if (!RobotState.bKnown) return TEXT("Waiting for the robot to settle...");
    if (!S.bShiftStarted) return TEXT("Start a new shift with $0 and 100% battery.");
    if (S.bShiftWon) return TEXT("Shift complete: net cash reached $100. Play Again starts a new shift.");
    if (bRecoveryPending) return RuntimeStatus;
    if (!RobotState.Fault.IsEmpty()) return TEXT("Robot stopped. R: recover to the depot.");
    if (S.BatteryPercent <= 0 && !FMAYADeliveryCashRules::CanAffordRecharge(S.TotalCredits))
        return TEXT("Battery depleted and recharge needs $2. Recover safely, then start a new shift.");
    if (ServiceStep != EServiceStep::None) return RuntimeStatus.IsEmpty() ? S.Status : RuntimeStatus;
    if (RingObservation.bInside && RingObservation.bEligible && !RingObservation.bVisitConsumed)
        return FString::Printf(TEXT("E: service now. Automatic service in %d s. Keep the robot stopped."), FMath::CeilToInt(RingObservation.RemainingGameplaySeconds));
    if (S.bCharging) return TEXT("Charging: $2 paid for this visit. Move to stop; delivery time keeps running.");
    if (S.ChargeVisit.Stage == EMAYADeliveryChargeStage::Qualifying)
        return FString::Printf(TEXT("Stay parked: charging in %d s. Recharge costs $2; delivery time continues."), FMath::CeilToInt(S.ChargeVisit.RemainingSeconds));
    if (S.Phase == EMAYADeliveryPhase::Available) return TEXT("J ACCEPT  |  N CHOOSE DELIVERY  |  GREEN RING: $2 RECHARGE");
    if (S.Phase == EMAYADeliveryPhase::Succeeded) return TEXT("N NEXT JOB  |  Enter REPLAY THIS JOB");
    if (S.Phase == EMAYADeliveryPhase::Failed) return TEXT("R RECOVER  |  Enter RETRY WHEN SAFE");
    if (GetNearbyChargingStation() && !S.ChargeVisit.Reason.IsEmpty()) return S.ChargeVisit.Reason;
    AMAYADeliveryStation* Sought = ObjectiveStation();
    if (Sought && FVector::Dist2D(Sought->GetServicePoint(), RobotState.ChassisPose.GetLocation()) <= Rules.GetBalance().ServiceRadiusCm)
        return RobotState.IsStoppedAndSupported() && RobotState.bDoorsClosedLatched
            ? TEXT("E SERVICE PARCEL  |  hold Space to stay stopped") : TEXT("Stop and stabilize with Space before service.");
    return TEXT("Follow the destination marker. Space BRAKE  |  R RECOVER");
}

void AMAYADeliveryDirector::EndPlay(EEndPlayReason::Type Reason)
{
    UnbindImpactBodies();
    if (Robot) Robot->SetDeliveryDriveLocked(true);
    if (IsValid(CrowdObstacle)) { CrowdObstacle->StopObserving(); CrowdObstacle->DestroyComponent(); CrowdObstacle = nullptr; }
    DestroyParcels();
    Super::EndPlay(Reason);
}

// Root-authored integration snippet; gameplay owner inserts into its frozen Director.
namespace MAYAInstantInventory
{
    bool ObserveSecured(AMAYADeliveryParcel* Parcel, AMAYANativeDemoPawn* Robot,
        UPrimitiveComponent* Carrier, int32 Slot, const FString& ExpectedId,
        double ExpectedKg, double& OutKg)
    {
        OutKg = 0;
        FTransform Target, Native;
        double Kg = 0;
        if (!IsValid(Parcel) || !IsValid(Parcel->GetParcelBody())
            || Parcel->GetParcelBody()->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
            || !IsValid(Robot) || !IsValid(Carrier)
            || ExpectedId.IsEmpty() || Parcel->GetParcelId() != ExpectedId
            || Parcel->GetDeliverySlot() != Slot || Parcel->IsDormantForDeliveryRemoval()
            || !FMath::IsFinite(ExpectedKg) || ExpectedKg <= 0
            || !Robot->GetDeliveryCargoPlacementTarget(Parcel->GetParcelBody(), Slot, Target)
            || !Parcel->IsSecuredTo(Carrier, Target) || !Parcel->ObserveMass(Kg)
            || FMath::Abs(Kg - ExpectedKg) > 0.05 || !Parcel->ObserveNativePose(Native)
            || FVector::Dist(Native.GetLocation(), Target.GetLocation()) > 2.0
            || FMath::RadiansToDegrees(Native.GetRotation().AngularDistance(Target.GetRotation())) > 3.0)
            return false;
        OutKg = Kg;
        return true;
    }
}

AMAYADeliveryParcel* AMAYADeliveryDirector::GetParcel(int32 Slot) const
{
    return Slot >= 0 && Slot < 2 && Parcels.IsValidIndex(Slot) && IsValid(Parcels[Slot])
        ? Parcels[Slot].Get() : nullptr;
}

FString AMAYADeliveryDirector::ParcelIdentity(int32 Slot) const
{
    const FString& Id = Rules.Get().JobInstanceId;
    if (Id.IsEmpty() || Slot < 0 || Slot >= Rules.Get().Job.ParcelCount || Slot >= 2) return FString();
    return Slot == 0 ? Id : Id + TEXT("/parcel_2");
}

int32 AMAYADeliveryDirector::GetParcelCount() const
{
    int32 Count = 0;
    for (int32 Slot = 0; Slot < 2; ++Slot) if (GetParcel(Slot)) ++Count;
    return Count;
}

int32 AMAYADeliveryDirector::GetSecuredParcelCount() const
{
    const auto& Job = Rules.Get().Job;
    if (Job.ParcelCount < 1 || Job.ParcelCount > 2) return 0;
    int32 Count = 0;
    for (int32 Slot = 0; Slot < Job.ParcelCount; ++Slot)
    {
        double Kg = 0;
        if (MAYAInstantInventory::ObserveSecured(GetParcel(Slot), Robot, CarrierBody(),
            Slot, ParcelIdentity(Slot), Job.PayloadKg / Job.ParcelCount, Kg)) ++Count;
    }
    return Count;
}

double AMAYADeliveryDirector::GetSecuredCargoKg() const
{
    const auto& Job = Rules.Get().Job;
    if (Job.ParcelCount < 1 || Job.ParcelCount > 2) return 0;
    double Total = 0;
    for (int32 Slot = 0; Slot < Job.ParcelCount; ++Slot)
    {
        double Kg = 0;
        if (MAYAInstantInventory::ObserveSecured(GetParcel(Slot), Robot, CarrierBody(),
            Slot, ParcelIdentity(Slot), Job.PayloadKg / Job.ParcelCount, Kg)) Total += Kg;
    }
    return Total;
}

bool AMAYADeliveryDirector::GetRecoveryParcelStockPose(int32 Slot, FTransform& Out) const
{
    if (Slot < 0 || Slot >= 2 || !RecoveryParcelPoses.IsValidIndex(Slot)
        || RecoveryParcelPoses[Slot].ContainsNaN()) return false;
    Out = RecoveryParcelPoses[Slot];
    return true;
}

bool AMAYADeliveryDirector::ReadRobot()
{
    RobotState = FMAYADeliveryRobotObservation();
    if (!GetWorld() || !IsValid(Robot) || !Robot->ReadDeliveryState(RobotState)) return false;
    const double Age = GetWorld()->GetTimeSeconds() - RobotState.ObservationWorldSeconds;
    if (!FMath::IsFinite(Age) || Age < 0 || Age > 0.25)
    { RobotState.bKnown = false; return false; }
    RobotState.bSecuredParcelCountKnown = true;
    RobotState.SecuredParcelCount = GetSecuredParcelCount();
    return RobotState.bKnown;
}

bool AMAYADeliveryDirector::CanRequestNext() const
{
    if (Rules.Get().bDroneTotalled || Rules.Get().bVehicleContactPending) return false;
    if (bPaused || !Rules.Get().bShiftStarted || Rules.Get().bShiftWon || bRecoveryPending || !GetWorld() || !IsValid(Robot)
        || ServiceStep != EServiceStep::None || Rules.IsServiceCancellationPending()
        || (Rules.Get().Phase != EMAYADeliveryPhase::Succeeded && Rules.Get().Phase != EMAYADeliveryPhase::Failed))
        return false;
    FMAYADeliveryRobotObservation Actual;
    if (!Robot->ReadDeliveryState(Actual)) return false;
    const double Age = GetWorld()->GetTimeSeconds() - Actual.ObservationWorldSeconds;
    return FMath::IsFinite(Age) && Age >= 0 && Age <= 0.25
        && Actual.IsStoppedAndSupported() && Actual.bDoorsClosedLatched;
}


FString AMAYADeliveryDirector::ReadCargoInventoryEvidenceJson() const
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.AirCargoInventory.v2"));
    J->SetBoolField(TEXT("airAnimationActive"), IsCargoAirAnimationActive());
    J->SetBoolField(TEXT("airLowering"), IsCargoAirLowering());
    J->SetBoolField(TEXT("airFadingOut"), IsCargoAirFadingOut());
    J->SetNumberField(TEXT("airPhaseElapsedGameplaySeconds"), GetCargoAirPhaseElapsedSeconds());
    J->SetNumberField(TEXT("declaredParcelCount"), Rules.Get().Job.ParcelCount);
    J->SetNumberField(TEXT("actorCount"), GetParcelCount());
    J->SetNumberField(TEXT("actualSecuredCount"), GetSecuredParcelCount());
    J->SetNumberField(TEXT("actualSecuredKg"), GetSecuredCargoKg());
    J->SetBoolField(TEXT("recoveryPending"), bRecoveryPending);
    TSharedRef<FJsonObject> Preparation = MakeShared<FJsonObject>();
    Preparation->SetStringField(TEXT("basis"), TEXT("Pickup-only continuous actual open/native bay capture window; observed pose excursion plus native/component discrepancy reserves half existing fixed-target limits; finite rates diagnostic only; later admission unchanged"));
    Preparation->SetBoolField(TEXT("ready"), bAirPickupPreparationReady);
    Preparation->SetNumberField(TEXT("openStartedWorldSeconds"), AirPickupOpenStartWorldSeconds);
    Preparation->SetNumberField(TEXT("sampleWorldSeconds"), AirPickupPreparationSampleWorldSeconds);
    Preparation->SetNumberField(TEXT("windowSeconds"), AirPickupPreparationWindowSeconds);
    Preparation->SetNumberField(TEXT("requiredWindowSeconds"), 1.0);
    Preparation->SetNumberField(TEXT("preparationBoundSeconds"), 4.0);
    Preparation->SetNumberField(TEXT("windowResets"), AirPickupPreparationWindowResets);
    Preparation->SetNumberField(TEXT("observedBayMarginCm"), AirPickupObservedBayMarginCm);
    Preparation->SetNumberField(TEXT("observedAngularMarginDegrees"), AirPickupObservedAngularMarginDegrees);
    Preparation->SetNumberField(TEXT("allowedObservedBayMarginCm"), 0.25);
    Preparation->SetNumberField(TEXT("allowedObservedAngularMarginDegrees"), 0.05);
    Preparation->SetNumberField(TEXT("peakBaySpeedCmps"), AirPickupPeakBaySpeedCmps);
    Preparation->SetNumberField(TEXT("peakChassisAngularRadps"), AirPickupPeakAngularRadps);
    Preparation->SetNumberField(TEXT("observedBayExcursionCm"), AirPickupMaxBayExcursionCm);
    Preparation->SetNumberField(TEXT("observedAngularExcursionDegrees"), AirPickupMaxAngularExcursionDegrees);
    Preparation->SetNumberField(TEXT("nativeComponentBayDeltaCm"), AirPickupNativeComponentPositionDeltaCm);
    Preparation->SetNumberField(TEXT("nativeComponentAngularDeltaDegrees"), AirPickupNativeComponentAngularDeltaDegrees);
    if (AirPickupPreparationSampleWorldSeconds >= 0)
    {
        Preparation->SetStringField(TEXT("windowInitialNativeChassisPose"), ServicePoseTrace(AirPickupInitialNativePose));
        Preparation->SetStringField(TEXT("lastNativeChassisPose"), ServicePoseTrace(AirPickupLastNativePose));
    }
    J->SetObjectField(TEXT("pickupPreparation"), Preparation);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (int32 Slot = 0; Slot < Parcels.Num(); ++Slot)
    {
        AMAYADeliveryParcel* P = GetParcel(Slot);
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("slot"), Slot); Item->SetBoolField(TEXT("exists"), P != nullptr);
        if (P)
        {
            double Kg = 0; FTransform Native, Target;
            Item->SetStringField(TEXT("parcelId"), P->GetParcelId());
            Item->SetStringField(TEXT("actorPath"), P->GetPathName());
            Item->SetStringField(TEXT("bodyPath"), P->GetParcelBody()->GetPathName());
            Item->SetBoolField(TEXT("dormant"), P->IsDormantForDeliveryRemoval());
            double Alpha = 0; bool Restored = false;
            const bool bAppearanceKnown = P->ObserveAirAppearance(Alpha, Restored);
            Item->SetBoolField(TEXT("airLoweringActive"), P->IsAirLoweringActive());
            Item->SetBoolField(TEXT("airRaisingActive"), P->IsAirRaisingActive());
            if (P->IsAirRaisingActive())
            {
                Item->SetStringField(TEXT("airRaisingTrajectoryBasis"), TEXT("Measured native bay tracking during eased rise; fixed parcel orientation; final upper pose/top latched before native wait/fade"));
                Item->SetBoolField(TEXT("airRaisingUpperLatched"), P->IsAirRaisingUpperLatched());
                Item->SetNumberField(TEXT("airRaisingEasedProgress"), P->GetAirRaisingEasedProgress());
                Item->SetStringField(TEXT("airRaisingReleasePose"), ServicePoseTrace(P->GetAirRaisingReleasePose()));
            }
            Item->SetBoolField(TEXT("appearanceAlphaKnown"), bAppearanceKnown);
            if (bAppearanceKnown) Item->SetNumberField(TEXT("actualAppearanceAlpha"), Alpha);
            Item->SetBoolField(TEXT("originalMaterialsRestored"), Restored);
            if (P->IsAirLoweringActive() || P->IsAirRaisingActive())
            {
                Item->SetStringField(TEXT("airStartPose"), ServicePoseTrace(P->GetAirLoweringStartPose()));
                Item->SetStringField(TEXT("airTargetPose"), ServicePoseTrace(P->GetAirLoweringTargetPose()));
                Item->SetNumberField(TEXT("actualOpenAssemblyTopCm"), P->GetAirOpenAssemblyTopCm());
                Item->SetNumberField(TEXT("stagedBottomClearanceCm"), 60.96);
            }
            Item->SetBoolField(TEXT("massKnown"), P->ObserveMass(Kg)); SetServiceReceiptNumber(Item, TEXT("nativeKg"), Kg);
            Item->SetBoolField(TEXT("nativePoseKnown"), P->ObserveNativePose(Native));
            Item->SetStringField(TEXT("nativePose"), ServicePoseTrace(Native));
            Item->SetStringField(TEXT("componentPose"), ServicePoseTrace(P->GetParcelBody()->GetComponentTransform()));
            Item->SetBoolField(TEXT("simulating"), P->GetParcelBody()->IsSimulatingPhysics());
            Item->SetNumberField(TEXT("collisionEnabled"), static_cast<uint8>(P->GetParcelBody()->GetCollisionEnabled()));
            const bool bTarget = Robot && Robot->GetDeliveryCargoPlacementTarget(P->GetParcelBody(), Slot, Target);
            Item->SetBoolField(TEXT("targetKnown"), bTarget);
            if (bTarget) Item->SetStringField(TEXT("targetPose"), ServicePoseTrace(Target));
            if (bTarget && P->ObserveNativePose(Native))
            {
                SetServiceReceiptNumber(Item, TEXT("nativeTargetDistanceCm"), FVector::Dist(Native.GetLocation(), Target.GetLocation()));
                SetServiceReceiptNumber(Item, TEXT("nativeTargetAngleDegrees"), FMath::RadiansToDegrees(Native.GetRotation().AngularDistance(Target.GetRotation())));
            }
            Item->SetBoolField(TEXT("physicalRestraintValid"), bTarget && P->IsSecuredTo(CarrierBody(), Target));
        }
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    J->SetArrayField(TEXT("parcels"), Items);
    FString Json; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(J, Writer); return Json;
#else
    return TEXT("{}");
#endif
}

void AMAYADeliveryDirector::ConsumeVehicleContacts()
{
    if (!VehicleContactObserver) return;
    FMAYAVehicleContactEvent E;
    while (VehicleContactObserver->ConsumeContact(E)) Rules.ApplyVehicleContact(E);
    Rules.SetVehicleContactAdjudicationPending(VehicleContactObserver->HasPendingIdentity());
}

const FMAYAVehicleContactEvent& AMAYADeliveryDirector::GetLastVehicleContact() const
{
    static const FMAYAVehicleContactEvent Unknown;
    return VehicleContactObserver ? VehicleContactObserver->GetLastContact() : Unknown;
}

FString AMAYADeliveryDirector::ReadVehicleContactEvidenceJson() const
{ return VehicleContactObserver ? VehicleContactObserver->ReadEvidenceJson() : TEXT("{\"known\":false}"); }
