#include "MAYAV01DamageFixture.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryImpactEvidence.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace MayaNativeDamageFixture
{
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{
    return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
}
FString Json(const TSharedRef<FJsonObject>& Object)
{
    FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
    FJsonSerializer::Serialize(Object, Writer); return Text;
}
const TCHAR* StepName(int32 Value)
{
    const TCHAR* Names[] = { TEXT("Idle"), TEXT("Approach"), TEXT("Impact"), TEXT("ImpactSettle"), TEXT("Hold"), TEXT("Backoff"), TEXT("Complete"), TEXT("Failed") };
    return Value >= 0 && Value < UE_ARRAY_COUNT(Names) ? Names[Value] : TEXT("Unknown");
}
}

AMAYAV01DamageFixture::AMAYAV01DamageFixture()
{
    PrimaryActorTick.bCanEverTick = false;
    Obstacle = CreateDefaultSubobject<UBoxComponent>(TEXT("ActualDamageFixtureBody"));
    RootComponent = Obstacle;
    Obstacle->SetMobility(EComponentMobility::Movable);
    Obstacle->SetBoxExtent(FVector(5, 50, 40));
    Obstacle->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Obstacle->SetCollisionObjectType(ECC_WorldStatic);
    Obstacle->SetCollisionResponseToAllChannels(ECR_Block);
    VisibleBox = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FixtureAppearance"));
    VisibleBox->SetupAttachment(Obstacle);
    VisibleBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    VisibleBox->SetMobility(EComponentMobility::Movable);
}

bool AMAYAV01DamageFixture::GroundSample(const FVector& XY, FHitResult& Hit) const
{
    if (!GetWorld()) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYAActualDamageGround), false);
    Query.MobilityType = EQueryMobilityType::Static;
    Query.AddIgnoredActor(this);
    const FVector Start(XY.X, XY.Y, RouteOrigin.Z + 150), End(XY.X, XY.Y, RouteOrigin.Z - 100);
    return GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Query)
        && Hit.bBlockingHit && !Hit.bStartPenetrating && !Hit.ImpactPoint.ContainsNaN()
        && !Hit.ImpactNormal.ContainsNaN() && Hit.ImpactNormal.Z >= 0.98;
}

bool AMAYAV01DamageFixture::Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector)
{
#if UE_BUILD_SHIPPING
    Failure = TEXT("Physical damage fixtures are unavailable in Shipping"); return false;
#else
    if (bConfigured || !GetWorld() || !IsValid(InRobot) || !IsValid(InDirector)
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageTest")))
    { Failure = TEXT("Damage fixture requires explicit native AutoTest+DamageTest and one valid configuration"); return false; }
#endif
    bCalibrationOnly = FParse::Param(FCommandLine::Get(), TEXT("MayaV01DamageCalibration"));
    const auto& Route = InDirector->GetMeasuredRoute();
    if (Route.Num() < 13) { Failure = TEXT("Damage fixture lacks the existing six-metre surveyed corridor"); return false; }
    Robot = InRobot; Director = InDirector; RouteOrigin = Route[0]; Anchor = Route[2];
    Axis = (Route[12] - RouteOrigin).GetSafeNormal2D(); Lateral = FVector(-Axis.Y, Axis.X, 0);
    if (Axis.IsNearlyZero() || FVector::Dist2D(Anchor, RouteOrigin) < 90 || FVector::Dist2D(Route[6], RouteOrigin) < 290)
    { Failure = TEXT("Damage fixture corridor spacing is not the measured fifty-centimetre course"); return false; }
    auto Ground = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> GroundSamples;
    double GroundZ = 0; int32 SampleCount = 0;
    for (double Along : { -5.0, 0.0, 5.0 }) for (double Across : { -50.0, 0.0, 50.0 })
    {
        FHitResult Hit;
        if (!GroundSample(Route[6] + Axis * Along + Lateral * Across, Hit)
            || FMath::Abs(Hit.ImpactPoint.Z - RouteOrigin.Z) > 2)
        { Failure = TEXT("Physical impact fixture footprint lacks nine measured flat static pavement samples"); return false; }
        auto Sample = MakeShared<FJsonObject>(); Sample->SetArrayField(TEXT("pointCm"), MayaNativeDamageFixture::Vec(Hit.ImpactPoint));
        Sample->SetArrayField(TEXT("normal"), MayaNativeDamageFixture::Vec(Hit.ImpactNormal));
        Sample->SetStringField(TEXT("component"), GetPathNameSafe(Hit.GetComponent()));
        GroundSamples.Add(MakeShared<FJsonValueObject>(Sample)); GroundZ += Hit.ImpactPoint.Z; ++SampleCount;
    }
    GroundZ /= SampleCount; Ground->SetArrayField(TEXT("fixtureGroundSamples"), GroundSamples);
    // Actual city cross-section observation only. No curb drive or ray-derived support claim.
    TArray<TSharedPtr<FJsonValue>> CrossSection;
    for (double SurveyStationAlong : { 100.0, 350.0, 600.0 })
    for (double AlongOffset : { -40.0, 0.0, 40.0 })
    for (int32 Offset = -400; Offset <= 0; Offset += 10)
    {
        FHitResult Hit; auto Sample = MakeShared<FJsonObject>();
        Sample->SetNumberField(TEXT("lateralOffsetCm"), Offset);
        Sample->SetNumberField(TEXT("alongOffsetCm"), AlongOffset);
        Sample->SetNumberField(TEXT("courseStationAlongCm"), SurveyStationAlong);
        const FVector XY = RouteOrigin + Axis * (SurveyStationAlong + AlongOffset) + Lateral * Offset;
        const bool Valid = GroundSample(XY, Hit); Sample->SetBoolField(TEXT("staticFlatHit"), Valid);
        // Distinguish a miss from an angled/penetrating hit, with a deeper
        // explicit range. Query observations do not replace native support.
        const FVector Start(XY.X, XY.Y, RouteOrigin.Z + 150), End(XY.X, XY.Y, RouteOrigin.Z - 500);
        FCollisionQueryParams SurveyQuery(SCENE_QUERY_STAT(MAYAActualCurbSurvey), false);
        SurveyQuery.MobilityType = EQueryMobilityType::Static; SurveyQuery.AddIgnoredActor(this);
        FHitResult Raw;
        const bool RawHit = GetWorld()->LineTraceSingleByChannel(Raw, Start, End, ECC_Visibility, SurveyQuery);
        Sample->SetArrayField(TEXT("deepTraceStartCm"), MayaNativeDamageFixture::Vec(Start));
        Sample->SetArrayField(TEXT("deepTraceEndCm"), MayaNativeDamageFixture::Vec(End));
        Sample->SetBoolField(TEXT("deepBlockingHit"), RawHit && Raw.bBlockingHit);
        Sample->SetBoolField(TEXT("deepStartPenetrating"), Raw.bStartPenetrating);
        if (RawHit && Raw.bBlockingHit && !Raw.ImpactPoint.ContainsNaN() && !Raw.ImpactNormal.ContainsNaN())
        {
            Sample->SetArrayField(TEXT("deepPointCm"), MayaNativeDamageFixture::Vec(Raw.ImpactPoint));
            Sample->SetArrayField(TEXT("deepNormal"), MayaNativeDamageFixture::Vec(Raw.ImpactNormal));
            Sample->SetStringField(TEXT("deepComponent"), GetPathNameSafe(Raw.GetComponent()));
            Sample->SetBoolField(TEXT("deepFlatNormal"), Raw.ImpactNormal.Z >= 0.98);
            UPrimitiveComponent* Component = Raw.GetComponent();
            if (Component)
            {
                Sample->SetNumberField(TEXT("collisionEnabled"), int32(Component->GetCollisionEnabled()));
                Sample->SetNumberField(TEXT("mobility"), int32(Component->Mobility));
                Sample->SetNumberField(TEXT("responsePhysicsBody"), int32(Component->GetCollisionResponseToChannel(ECC_PhysicsBody)));
                Sample->SetArrayField(TEXT("componentBoundsMinCm"), MayaNativeDamageFixture::Vec(Component->Bounds.GetBox().Min));
                Sample->SetArrayField(TEXT("componentBoundsMaxCm"), MayaNativeDamageFixture::Vec(Component->Bounds.GetBox().Max));
                if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
                    Sample->SetStringField(TEXT("meshPath"), GetPathNameSafe(Mesh->GetStaticMesh()));
            }
        }
        if (Valid)
        {
            Sample->SetArrayField(TEXT("pointCm"), MayaNativeDamageFixture::Vec(Hit.ImpactPoint));
            Sample->SetStringField(TEXT("component"), GetPathNameSafe(Hit.GetComponent()));
        }
        CrossSection.Add(MakeShared<FJsonValueObject>(Sample));
    }
    Ground->SetArrayField(TEXT("cityStaticCrossSection"), CrossSection);
    Ground->SetStringField(TEXT("curbScope"), TEXT("Static visibility survey only; no city-curb landing test or load-bearing support inference"));
    StaticSurfaceSurveyJson = MayaNativeDamageFixture::Json(Ground);
    FixtureCenter = FVector(Route[6].X, Route[6].Y, GroundZ + 40.1);
    const FQuat Rotation = Axis.Rotation().Quaternion();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYAActualDamageFixtureSpawn), false); Query.AddIgnoredActor(this);
    if (GetWorld()->OverlapBlockingTestByChannel(FixtureCenter, Rotation, ECC_WorldStatic,
        FCollisionShape::MakeBox(FVector(5, 50, 40)), Query))
    { Failure = TEXT("Named physical fixture's full volume is already obstructed by the actual scene"); return false; }
    if (!SetActorTransform(FTransform(Rotation, FixtureCenter)))
    { Failure = TEXT("Named fixture could not establish its measured setup pose before collision activation"); return false; }
    VisibleBox->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    VisibleBox->SetRelativeScale3D(FVector(0.1, 1, 0.8));
    if (!VisibleBox->GetStaticMesh()) { Failure = TEXT("Owned diagnostic fixture cannot load the engine cube appearance"); return false; }
    Obstacle->SetMobility(EComponentMobility::Static);
    VisibleBox->SetMobility(EComponentMobility::Static);
    Obstacle->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    auto* Mount = Robot->GetDeliveryCargoMount(0);
    Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    if (!Carrier.IsValid()) { Cleanup(); Failure = TEXT("Actual native chassis body unavailable for passive hit observations"); return false; }
    TArray<UPrimitiveComponent*> Components; Robot->GetComponents<UPrimitiveComponent>(Components);
    for (UPrimitiveComponent* Component : Components)
    {
        if (!Component || !Component->IsPhysicsCollisionEnabled()) continue;
        FBodyInstance* Native = Component->GetBodyInstance();
        FBodyInstance* Own = Component->GetBodyInstance(NAME_None, false);
        if (!Native || !Own || Native != Own || !Native->IsValidBodyInstance()) continue;
        // Director enables notification; this diagnostic only observes it.
        Component->OnComponentHit.AddUniqueDynamic(this, &AMAYAV01DamageFixture::OnChassisHit);
        ObservedBodies.Add(Component);
    }
    if (ObservedBodies.Num() != 9) { Cleanup(); Failure = TEXT("Passive fixture requires all nine independent native robot bodies"); return false; }
    auto Setup = MakeShared<FJsonObject>(); Setup->SetStringField(TEXT("fixtureActor"), GetPathName());
    Setup->SetStringField(TEXT("fixtureComponent"), Obstacle->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Observed;
    for (const auto& Body : ObservedBodies) Observed.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Body.Get())));
    Setup->SetArrayField(TEXT("passiveObservedNativeBodies"), Observed);
    Setup->SetStringField(TEXT("impactMetric"), TEXT("Peak actual per-callback impulse across nine independent native bodies; no substep/body impulse summation; shared obstacle episode"));
    Setup->SetArrayField(TEXT("centerCm"), MayaNativeDamageFixture::Vec(FixtureCenter));
    Setup->SetArrayField(TEXT("halfExtentsCm"), MayaNativeDamageFixture::Vec(Obstacle->GetScaledBoxExtent()));
    Setup->SetArrayField(TEXT("approachAnchorCm"), MayaNativeDamageFixture::Vec(Anchor));
    Setup->SetArrayField(TEXT("routeAxis"), MayaNativeDamageFixture::Vec(Axis));
    Setup->SetNumberField(TEXT("collisionEnabled"), int32(Obstacle->GetCollisionEnabled()));
    Setup->SetNumberField(TEXT("collisionObjectType"), int32(Obstacle->GetCollisionObjectType()));
    Setup->SetStringField(TEXT("surfaceSurvey"), StaticSurfaceSurveyJson);
    Setup->SetStringField(TEXT("scope"), TEXT("New explicit test-only static blocking fixture on measured city pavement; production geometry/configuration/physics settings and damage rules unchanged"));
    FixtureSetupJson = MayaNativeDamageFixture::Json(Setup); bConfigured = true; return FixtureIdentityValid();
}

bool AMAYAV01DamageFixture::FixtureIdentityValid() const
{
    return bConfigured && IsValid(Obstacle) && !GetActorTransform().ContainsNaN()
        && FVector::Dist(GetActorLocation(), FixtureCenter) <= 0.001
        && GetActorQuat().AngularDistance(Axis.Rotation().Quaternion()) <= 1.e-4
        && Obstacle->Mobility == EComponentMobility::Static && !Obstacle->IsSimulatingPhysics()
        && Obstacle->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics
        && Obstacle->GetCollisionResponseToChannel(ECC_PhysicsBody) == ECR_Block
        && Obstacle->GetScaledBoxExtent().Equals(FVector(5, 50, 40), 0.001)
        && Obstacle->GetBodyInstance() && Obstacle->GetBodyInstance()->IsValidBodyInstance();
}

bool AMAYAV01DamageFixture::Command(float Throttle, float Steer, bool bBrake)
{
    bCommandAccepted = Robot && Robot->SubmitDriveCommand(Throttle, Steer, bBrake,
        EMAYADriveCommandOrigin::NativeAutomation, 0.25f, false);
    return bCommandAccepted;
}

void AMAYAV01DamageFixture::EnterStep(EStep Next, double WorldS)
{
    Step = Next; StepStartWorldS = WorldS; StableSinceWorldS = -1; bArrivalLatched = false;
}

bool AMAYAV01DamageFixture::BeginTrial(bool bLoaded, bool bHard, const FMAYADeliveryRobotObservation& O)
{
    if (!FixtureIdentityValid() || !Robot || !Director || Director->IsPaused()
        || (Step != EStep::Idle && Step != EStep::Complete) || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched)
    { Failure = TEXT("Physical impact trial requires actual stopped/supported/closed fixture and one valid boundary"); return false; }
    const auto S = Director->Snapshot();
    if (bCalibrationOnly && (bLoaded || S.CargoConditionPercent != 100 || S.ImpactHistory.Num() != 0))
    { Failure = TEXT("Actual contact calibration requires an empty robot with pristine condition/history"); return false; }
    if (bLoaded ? (S.Phase != EMAYADeliveryPhase::Carrying || Director->GetSecuredParcelCount() != 1 || S.Job.ParcelCount != 1)
        : (Director->GetParcelCount() != 0 || Director->GetSecuredParcelCount() != 0 || S.Phase != EMAYADeliveryPhase::Available))
    { Failure = TEXT("Physical impact trial's actual empty/one-body cargo state is unavailable"); return false; }
    const double Heading = FMath::Abs(FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw, Axis.Rotation().Yaw));
    if (Heading > 2) { Failure = TEXT("Physical impact trial lacks the original two-degree forward corridor alignment"); return false; }
    bLoadedTrial = bLoaded; bHardTrial = bHard; ConditionBefore = S.CargoConditionPercent;
    HistoryAtStart = S.ImpactHistory.Num(); JobId = S.JobInstanceId; ResetGeneration = O.ResetGeneration;
    TrialStartWorldS = GetWorld()->GetTimeSeconds(); PreviousObservation = O;
    TrialStartChassisZ = O.ChassisPose.GetLocation().Z;
    PeakSpeed = PeakImpulseKgMps = PeakApproachCmps = 0; ContactWorldS = -1;
    FixtureHitCount = 0; bQualifiedContact = false; PassiveBodyPeaks.Reset();
    HoldFirstPositiveContactWorldS = HoldLastPositiveContactWorldS = -1;
    HoldMaxPositiveContactGapS = 0; HoldPositiveContactCallbacks = 0;
    EnterStep(EStep::Approach, TrialStartWorldS); Robot->ReleaseDriveCommand(); return Command(0, 0, true);
}

bool AMAYAV01DamageFixture::ApproachAnchor(const FMAYADeliveryRobotObservation& O, bool bReverse)
{
    const FVector Delta = Anchor - O.ChassisPose.GetLocation();
    if (Delta.Size2D() <= 20) bArrivalLatched = true;
    if (bArrivalLatched)
    {
        Command(0, 0, true);
        if (O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = GetWorld()->GetTimeSeconds();
            return GetWorld()->GetTimeSeconds() - StableSinceWorldS >= 0.5;
        }
        StableSinceWorldS = -1; return false;
    }
    const double TravelHeading = (-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw + (bReverse ? 180 : 0);
    const double Error = FMath::FindDeltaAngleDegrees(TravelHeading, Delta.Rotation().Yaw);
    if (FMath::Abs(Error) > 15) { Command(0, 0, true); return false; }
    Command(bReverse ? -0.35f : 0.35f, float(FMath::Clamp(Error / 40.0, -0.3, 0.3)), false); return false;
}

void AMAYAV01DamageFixture::OnChassisHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, FVector Impulse, const FHitResult& Hit)
{
    if (!bConfigured || !GetWorld() || GetWorld()->IsPaused() || !ObservedBodies.Contains(HitComponent)
        || OtherActor != this || OtherComponent != Obstacle || Impulse.ContainsNaN() || Hit.ImpactNormal.ContainsNaN()) return;
    const double Now = GetWorld()->GetTimeSeconds();
    const double Quiet = LastContactWorldS < 0 ? 1000 : Now - LastContactWorldS; LastContactWorldS = Now;
    if (HitEpisodeGeneration == 0 || Quiet >= 0.6) HitEpisodeGeneration = ++PassiveHitGeneration;
    const double KgMps = Impulse.Size() / 100.0;
    const double Approach = FMath::Max(0.0, -FVector::DotProduct(PreviousObservation.VelocityCmps, Hit.ImpactNormal.GetSafeNormal()));
    if (!FMath::IsFinite(KgMps) || !FMath::IsFinite(Approach)) return;
    ++FixtureHitCount;
    FPassiveBodyPeak& Peak = PassiveBodyPeaks.FindOrAdd(HitComponent); ++Peak.Callbacks;
    if (Peak.WorldSeconds < 0 || KgMps > Peak.ImpulseKgMps)
    {
        Peak.ImpulseKgMps = KgMps; Peak.ApproachCmps = Approach; Peak.WorldSeconds = Now;
        Peak.RawImpulse = Impulse; Peak.Normal = Hit.ImpactNormal;
        Peak.Step = MayaNativeDamageFixture::StepName(int32(Step));
    }
    if (Step == EStep::Hold && KgMps > 0 && FMath::Abs(Hit.ImpactNormal.Z) < 0.5)
    {
        if (HoldFirstPositiveContactWorldS < 0) HoldFirstPositiveContactWorldS = Now;
        if (HoldLastPositiveContactWorldS >= 0)
            HoldMaxPositiveContactGapS = FMath::Max(HoldMaxPositiveContactGapS, Now - HoldLastPositiveContactWorldS);
        HoldLastPositiveContactWorldS = Now; ++HoldPositiveContactCallbacks;
    }
    // Calibration only ends on measured positive moving lateral contact;
    // it neither changes production thresholds nor creates a GAME event.
    const bool Qualified = Step == EStep::Impact && FMath::Abs(Hit.ImpactNormal.Z) < 0.5
        && (bCalibrationOnly ? KgMps > 0 : KgMps >= FMAYADeliveryLateralImpactCalibration::MinimumImpulseKgMps)
        && Approach >= FMAYADeliveryLateralImpactCalibration::MinimumApproachCmps;
    if (ContactReceipts.Num() < 128 && (Qualified || LastRecordedContactWorldS < 0 || Now - LastRecordedContactWorldS >= 0.1))
    {
        LastRecordedContactWorldS = Now;
        auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("worldSeconds"), Now);
        R->SetStringField(TEXT("trialStep"), MayaNativeDamageFixture::StepName(int32(Step)));
        R->SetStringField(TEXT("passiveFixtureHitGeneration"), LexToString(HitEpisodeGeneration));
        R->SetNumberField(TEXT("quietSecondsBeforeCallback"), Quiet); R->SetNumberField(TEXT("impulseKgMps"), KgMps);
        R->SetNumberField(TEXT("previousObservedApproachSpeedCmps"), Approach);
        R->SetArrayField(TEXT("rawNormalImpulseKgCmps"), MayaNativeDamageFixture::Vec(Impulse));
        R->SetArrayField(TEXT("impactNormal"), MayaNativeDamageFixture::Vec(Hit.ImpactNormal));
        R->SetStringField(TEXT("actualOtherComponent"), GetPathNameSafe(OtherComponent));
        R->SetStringField(TEXT("actualRobotComponent"), GetPathNameSafe(HitComponent));
        R->SetStringField(TEXT("provenance"), TEXT("Passive actual independent robot body OnComponentHit; independent fixture observer generation, not a synthetic GAME impact or force"));
        ContactReceipts.Add(MakeShared<FJsonValueObject>(R));
    }
    if (!Qualified) return;
    if (!bQualifiedContact) { ContactWorldS = Now; HitQuietBeforeS = Quiet; }
    bQualifiedContact = true;
    PeakImpulseKgMps = FMath::Max(PeakImpulseKgMps, KgMps); PeakApproachCmps = FMath::Max(PeakApproachCmps, Approach);
}

AMAYAV01DamageFixture::EOutcome AMAYAV01DamageFixture::FailTrial(const FString& Reason)
{
    Failure = Reason; Step = EStep::Failed; Command(0, 0, true); return EOutcome::Failed;
}

AMAYAV01DamageFixture::EOutcome AMAYAV01DamageFixture::AdvanceTrial(const FMAYADeliveryRobotObservation& O)
{
    if (Step == EStep::Failed) { Command(0, 0, true); return EOutcome::Failed; }
    if (Step == EStep::Complete) { Command(0, 0, true); return EOutcome::Complete; }
    if (!GetWorld() || !FixtureIdentityValid() || !Robot || !Director || Director->IsPaused())
        return FailTrial(TEXT("Actual impact fixture identity/world changed during the bounded trial"));
    const double Now = GetWorld()->GetTimeSeconds(), Age = Now - StepStartWorldS;
    if (Now - TrialStartWorldS > 50) return FailTrial(TEXT("Physical impact fixture exceeded its fifty-second trial bound"));
    const auto S = Director->Snapshot();
    const FVector Delta = O.ChassisPose.GetLocation() - RouteOrigin;
    const double Along = FVector::DotProduct(Delta, Axis), Across = FMath::Abs(FVector::DotProduct(Delta, Lateral));
    if (!O.bKnown || !O.bReady || !O.bUpright || !O.Fault.IsEmpty() || O.ResetGeneration != ResetGeneration
        || !Robot->AreDeliveryDoorsClosedAndPhysicallyLocked() || Along < -60 || Along > 380 || Across > 20
        || FMath::Abs(O.ChassisPose.GetLocation().Z - TrialStartChassisZ) > 8)
        return FailTrial(TEXT("Physical impact trial lost actual native identity, closed locks, upright or predeclared corridor containment"));
    if (bLoadedTrial ? (S.Phase != EMAYADeliveryPhase::Carrying || S.JobInstanceId != JobId || Director->GetSecuredParcelCount() != 1)
        : (Director->GetParcelCount() != 0 || Director->GetSecuredParcelCount() != 0))
        return FailTrial(TEXT("Actual cargo inventory changed during empty/loaded impact trial"));
    PeakSpeed = FMath::Max(PeakSpeed, double(O.VelocityCmps.Size()));
    PreviousObservation = O;
    if (Step == EStep::Approach || Step == EStep::Backoff)
    {
        if (Age > 15) return FailTrial(TEXT("Physical impact anchor setup/backoff exceeded fifteen seconds"));
        if (ApproachAnchor(O, Step == EStep::Backoff))
        {
            if (Step == EStep::Approach) EnterStep(EStep::Impact, Now);
            else
            {
                // Contact callbacks may continue during withdrawal. A late
                // extra damage episode must not be hidden by an earlier Hold.
                const double ExpectedLoss = bLoadedTrial ? (bHardTrial ? 1.0 : 0.5) : 0.0;
                if (FMath::Abs(ConditionBefore - S.CargoConditionPercent - ExpectedLoss) > 1.e-6
                    || S.CargoConditionPercent != (bLoadedTrial ? ConditionAtHold : ConditionBefore)
                    || S.ImpactHistory.Num() != (bLoadedTrial ? HistoryAtHold : HistoryAtStart))
                    return FailTrial(TEXT("Actual impact withdrawal added or changed condition loss/history after the neutral hold"));
                auto R = MakeShared<FJsonObject>(); R->SetBoolField(TEXT("loaded"), bLoadedTrial);
                R->SetBoolField(TEXT("hardRequested"), bHardTrial); R->SetNumberField(TEXT("startWorldSeconds"), TrialStartWorldS);
                R->SetNumberField(TEXT("endWorldSeconds"), Now); R->SetNumberField(TEXT("peakSpeedCmps"), PeakSpeed);
                R->SetNumberField(TEXT("peakQualifiedImpulseKgMps"), PeakImpulseKgMps);
                R->SetNumberField(TEXT("peakQualifiedApproachSpeedCmps"), PeakApproachCmps);
                R->SetNumberField(TEXT("conditionBefore"), ConditionBefore); R->SetNumberField(TEXT("conditionAfter"), S.CargoConditionPercent);
                R->SetNumberField(TEXT("actualConditionLoss"), ConditionBefore - S.CargoConditionPercent);
                R->SetObjectField(TEXT("actualAppliedReceipt"), MAYADeliveryImpactReceiptToJson(S.LastImpact));
                R->SetNumberField(TEXT("contactCallbackCount"), FixtureHitCount); R->SetNumberField(TEXT("contactWorldSeconds"), ContactWorldS);
                R->SetNumberField(TEXT("quietSecondsBeforeQualifiedContact"), HitQuietBeforeS);
                const double ContactSpan = HoldLastPositiveContactWorldS - HoldFirstPositiveContactWorldS;
                const bool bContactSpanObserved = HoldPositiveContactCallbacks >= 2 && HoldFirstPositiveContactWorldS >= 0
                    && ContactSpan >= 0.6 && HoldMaxPositiveContactGapS < 0.6;
                R->SetNumberField(TEXT("neutralHoldPositiveContactCallbacks"), HoldPositiveContactCallbacks);
                R->SetNumberField(TEXT("neutralHoldFirstPositiveContactWorldSeconds"), HoldFirstPositiveContactWorldS);
                R->SetNumberField(TEXT("neutralHoldLastPositiveContactWorldSeconds"), HoldLastPositiveContactWorldS);
                R->SetNumberField(TEXT("neutralHoldPositiveContactSpanSeconds"), HoldFirstPositiveContactWorldS >= 0 ? ContactSpan : 0);
                R->SetNumberField(TEXT("neutralHoldMaxPositiveContactGapSeconds"), HoldMaxPositiveContactGapS);
                R->SetBoolField(TEXT("neutralHoldContactBurstSpanningPointSixSecondsObserved"), bContactSpanObserved);
                R->SetStringField(TEXT("noRepeatDamageScope"), bContactSpanObserved
                    ? TEXT("Actual positive native robot-body/fixture contact callbacks span at least0.6 gameplay seconds with every recorded intercallback gap below0.6; no extra condition loss/history through final withdrawal")
                    : TEXT("Stopped post-impact neutral hold and withdrawal have no extra condition loss/history; continued native contact callbacks over0.6 seconds were not observed, so continuous-contact deduplication is not assessed"));
                R->SetArrayField(TEXT("actualRawPeaksByNativeBody"), RawBodyPeaksToJson());
                R->SetBoolField(TEXT("finalWithdrawalConditionAndHistoryVerified"), true);
                R->SetBoolField(TEXT("calibrationOnly"), bCalibrationOnly);
                R->SetBoolField(TEXT("cargoDamageAccepted"), !bCalibrationOnly && bLoadedTrial);
                R->SetBoolField(TEXT("pass"), true); TrialReceipts.Add(MakeShared<FJsonValueObject>(R));
                EnterStep(EStep::Complete, Now); return EOutcome::Complete;
            }
        }
    }
    else if (Step == EStep::Impact)
    {
        if (Age > 15) return FailTrial(TEXT("No threshold-qualified actual robot-body/fixture impact within fifteen seconds"));
        if (O.VelocityCmps.Size() > (bHardTrial ? 90 : 35)) return FailTrial(TEXT("Actual impact approach exceeded its predeclared small/hard speed ceiling"));
        if (bQualifiedContact)
        {
            if (bCalibrationOnly && (bHardTrial ? PeakApproachCmps < 45 : PeakApproachCmps > 35))
                return FailTrial(TEXT("Actual calibration contact did not establish its declared small<=35/hard>=45cmps approach envelope"));
            // Braking follow-through may legitimately upgrade the same source
            // once. Establish its expected severity and actual settled baseline
            // before asserting the later neutral hold has no extra loss.
            Command(0, 0, true); EnterStep(EStep::ImpactSettle, Now);
        }
        else
        {
            const double Speed = FMath::Max(0.0, O.OpticalForwardCmps);
            const double TargetSpeed = bHardTrial ? 65.0 : 20.0;
            if (Speed >= TargetSpeed + (bHardTrial ? 10 : 6)) Command(0, 0, true);
            else Command(bHardTrial ? 1.f : Speed < 18 ? 0.35f : 0.20f, 0, false);
        }
    }
    else if (Step == EStep::ImpactSettle)
    {
        Command(0, 0, true);
        if (Age > 3) return FailTrial(TEXT("Actual impact follow-through did not reach expected condition and supported stable stop within three seconds"));
        const double ExpectedLoss = bLoadedTrial ? (bHardTrial ? 1.0 : 0.5) : 0.0;
        const double Loss = ConditionBefore - S.CargoConditionPercent;
        if (Loss > ExpectedLoss + 1.e-6 || (!bLoadedTrial && S.ImpactHistory.Num() != HistoryAtStart))
            return FailTrial(TEXT("Actual impact episode exceeded expected severity before neutral baseline"));
        const bool SeverityEstablished = FMath::Abs(Loss - ExpectedLoss) <= 1.e-6;
        if (SeverityEstablished && O.IsStoppedAndSupported() && O.bDoorsClosedLatched)
        {
            if (StableSinceWorldS < 0) StableSinceWorldS = Now;
            if (Age >= 0.5 && Now - StableSinceWorldS >= 0.25)
            {
                ConditionAtHold = S.CargoConditionPercent; HistoryAtHold = S.ImpactHistory.Num();
                EnterStep(EStep::Hold, Now);
            }
        }
        else StableSinceWorldS = -1;
    }
    else if (Step == EStep::Hold)
    {
        Command(0, 0, true);
        if (Age > 5) return FailTrial(TEXT("Actual impact hold did not retain supported stop within five seconds"));
        const double Loss = ConditionBefore - S.CargoConditionPercent;
        if (bLoadedTrial)
        {
            const auto& Applied = S.LastImpact;
            if (Applied.Event.Kind != TEXT("ChassisImpact") || Applied.Event.OtherActor != GetPathName()
                || Applied.Event.WorldSeconds < TrialStartWorldS || Applied.Event.Generation == 0 || HitQuietBeforeS < 0.6
                || FMath::Abs(Loss - (bHardTrial ? 1.0 : 0.5)) > 1.e-6)
                return FailTrial(TEXT("Actual loaded contact did not produce the requested measured small/hard source receipt and exact bounded shared-condition delta"));
            if (S.CargoConditionPercent != ConditionAtHold || S.ImpactHistory.Num() != HistoryAtHold)
                return FailTrial(TEXT("Neutral post-impact hold added an unexpected second condition loss/receipt"));
        }
        else if (S.CargoConditionPercent != ConditionBefore || S.ImpactHistory.Num() != HistoryAtStart)
            return FailTrial(TEXT("Actual empty contact incorrectly applied cargo damage"));
        if (Age >= 1.2 && O.IsStoppedAndSupported() && O.bDoorsClosedLatched) EnterStep(EStep::Backoff, Now);
    }
    if (!bCommandAccepted) return FailTrial(TEXT("Physical damage trial's explicit native command was refused"));
    return EOutcome::Running;
}

TArray<TSharedPtr<FJsonValue>> AMAYAV01DamageFixture::RawBodyPeaksToJson() const
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Pair : PassiveBodyPeaks)
    {
        const FPassiveBodyPeak& Peak = Pair.Value;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("actualRobotComponent"), GetPathNameSafe(Pair.Key.Get()));
        J->SetStringField(TEXT("callbackCount"), LexToString(Peak.Callbacks));
        J->SetNumberField(TEXT("peakCallbackImpulseKgMps"), Peak.ImpulseKgMps);
        J->SetNumberField(TEXT("sameCallbackPreviousObservedApproachCmps"), Peak.ApproachCmps);
        J->SetNumberField(TEXT("worldSeconds"), Peak.WorldSeconds);
        J->SetStringField(TEXT("trialStep"), Peak.Step);
        J->SetArrayField(TEXT("rawNormalImpulseKgCmps"), MayaNativeDamageFixture::Vec(Peak.RawImpulse));
        J->SetArrayField(TEXT("impactNormal"), MayaNativeDamageFixture::Vec(Peak.Normal));
        J->SetStringField(TEXT("scope"), TEXT("Actual peak individual callback for this native body; includes subthreshold impulses, never summed; solver step unknown"));
        Values.Add(MakeShared<FJsonValueObject>(J));
    }
    return Values;
}

FString AMAYAV01DamageFixture::ReadEvidenceJson(bool bIncludeHistory) const
{
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativePhysicalDamageFixture.v1"));
    J->SetBoolField(TEXT("calibrationOnly"), bCalibrationOnly);
    J->SetBoolField(TEXT("configured"), bConfigured); J->SetBoolField(TEXT("currentPhysicalFixtureIdentityValid"), FixtureIdentityValid());
    // Dense fixed survey/setup is retained in the terminal/history receipt,
    // not repeated in every0.5-second observation under the64MiB log bound.
    if (bIncludeHistory) J->SetStringField(TEXT("setup"), FixtureSetupJson);
    J->SetBoolField(TEXT("fixedSetupAndSurveyStoredInFinalHistoryReceipt"), true);
    J->SetStringField(TEXT("step"), MayaNativeDamageFixture::StepName(int32(Step)));
    J->SetStringField(TEXT("failure"), Failure); J->SetBoolField(TEXT("loadedTrial"), bLoadedTrial); J->SetBoolField(TEXT("hardRequested"), bHardTrial);
    J->SetBoolField(TEXT("lastNativeCommandAccepted"), bCommandAccepted); J->SetNumberField(TEXT("peakQualifiedImpulseKgMps"), PeakImpulseKgMps);
    J->SetNumberField(TEXT("peakSpeedCmps"), PeakSpeed); J->SetNumberField(TEXT("qualifiedContactWorldSeconds"), ContactWorldS);
    if (bIncludeHistory)
    {
        J->SetArrayField(TEXT("trialReceipts"), TrialReceipts); J->SetArrayField(TEXT("passiveActualContacts"), ContactReceipts);
        J->SetStringField(TEXT("staticSurfaceSurvey"), StaticSurfaceSurveyJson);
    }
    J->SetNumberField(TEXT("completedActualTrialCount"), TrialReceipts.Num());
    J->SetNumberField(TEXT("savedPassiveContactCount"), ContactReceipts.Num());
    J->SetArrayField(TEXT("actualRawPeaksByNativeBody"), RawBodyPeaksToJson());
    J->SetStringField(TEXT("scope"), TEXT("Real static blocking object and native leased commands only; no synthetic hit, impulse, condition, clock, camera motion or robot/cargo teleport; city-curb survey remains separate from an unexecuted landing test"));
    return MayaNativeDamageFixture::Json(J);
}

void AMAYAV01DamageFixture::Cleanup()
{
    for (const auto& Body : ObservedBodies) if (Body.IsValid())
        Body->OnComponentHit.RemoveDynamic(this, &AMAYAV01DamageFixture::OnChassisHit);
    ObservedBodies.Reset();
    if (Obstacle) Obstacle->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (VisibleBox) VisibleBox->SetVisibility(false);
    bConfigured = false;
}

void AMAYAV01DamageFixture::EndPlay(EEndPlayReason::Type Reason)
{
    Cleanup(); Super::EndPlay(Reason);
}
