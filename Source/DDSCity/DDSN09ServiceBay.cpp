#include "DDSN09ServiceBay.h"

#include "DDSCityRollingPawn.h"
#include "Components/BoxComponent.h"
#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"

ADDSN09ServiceBay::ADDSN09ServiceBay()
{
    PrimaryActorTick.bCanEverTick = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("N09Root"));
    RootComponent = SceneRoot;
    BayFootprint = CreateDefaultSubobject<UBoxComponent>(TEXT("PaintedBayBounds"));
    BayFootprint->SetupAttachment(SceneRoot);
    BayFootprint->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BayFootprint->SetGenerateOverlapEvents(false);
    BayFootprint->SetBoxExtent(BayHalfExtentCm);
    ServicePoint = CreateDefaultSubobject<USceneComponent>(TEXT("ServicePoint"));
    ServicePoint->SetupAttachment(SceneRoot);
    ServicePoint->SetRelativeLocation(FVector(0, 0, 45)); // Author per measured socket.
    GroundDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("GroundGlowDecal"));
    GroundDecal->SetupAttachment(SceneRoot);
    GroundDecal->SetRelativeLocation(FVector(0, 0, 55));
    GroundDecal->SetRelativeRotation(FRotator(-90, 0, 0));
    GroundDecal->DecalSize = FVector(100, 420, 350);
    GroundLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GroundGlowLight"));
    GroundLight->SetupAttachment(SceneRoot);
    GroundLight->SetRelativeLocation(FVector(0, 0, 35));
    GroundLight->SetIntensity(900.0f);
    GroundLight->SetAttenuationRadius(350.0f);
    GroundLight->SetCastShadows(false);
    IconAndLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("BayIconAndLabel"));
    IconAndLabel->SetupAttachment(SceneRoot);
    IconAndLabel->SetRelativeLocation(FVector(0, 0, 145));
    IconAndLabel->SetWorldSize(24.0f);
    IconAndLabel->SetTextRenderColor(FColor::White);
    PromptText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("BayPrompt"));
    PromptText->SetupAttachment(SceneRoot);
    PromptText->SetRelativeLocation(FVector(0, 0, 180));
    PromptText->SetWorldSize(19.0f);
    PromptText->SetTextRenderColor(FColor::White);
}

void ADDSN09ServiceBay::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    BayFootprint->SetBoxExtent(BayHalfExtentCm.ComponentMax(FVector(1,1,1)));
    GroundDecal->DecalSize = FVector(100.0f,
        2.0f * BayHalfExtentCm.X, 2.0f * BayHalfExtentCm.Y);
    IconAndLabel->SetText(FText::FromString(bPickupStation
        ? TEXT("PICKUP  [<- BOX]") : TEXT("DELIVER  <BOX ->>")));
    PromptText->SetText(FText::GetEmpty());
}

void ADDSN09ServiceBay::BeginPlay()
{
    Super::BeginPlay();
    if (GroundGlowMaterial)
    {
        GroundDecal->SetDecalMaterial(GroundGlowMaterial);
        GlowInstance = GroundDecal->CreateDynamicMaterialInstance();
    }
    UpdateMarker(false, FDDSN09VisualDecision());
}

FVector ADDSN09ServiceBay::GetServicePointWorldCm() const
{
    return ServicePoint ? ServicePoint->GetComponentLocation() : FVector::ZeroVector;
}

bool ADDSN09ServiceBay::HasUniqueStationId() const
{
    if (!GetWorld() || StationId.IsEmpty()) return false;
    for (TActorIterator<ADDSN09ServiceBay> It(GetWorld()); It; ++It)
    {
        if (*It != this && It->StationId.Equals(StationId,ESearchCase::IgnoreCase))
            return false;
    }
    return true;
}

FDDSN09GeometryObservation ADDSN09ServiceBay::ObserveGeometry(
    const ADDSCityRollingPawn& Pawn, const USceneComponent& RobotServiceSocket) const
{
    FDDSN09GeometryObservation Result;
    const UWorld* World = GetWorld();
    if (!World || !HasUniqueStationId() || !RobotServiceSocket.IsRegistered()
        || RobotServiceSocket.GetOwner() != &Pawn || !ServicePoint)
    {
        return Result; // Unknown cannot grant E through N08.
    }
    const FDDSMotionSample& Motion = Pawn.GetMotionSample();
    const FVector Socket = RobotServiceSocket.GetComponentLocation();
    const FVector Point = ServicePoint->GetComponentLocation();
    if (Socket.ContainsNaN() || Point.ContainsNaN()
        || Motion.ActualVelocityCmPerSecond.ContainsNaN()
        || !FMath::IsFinite(Motion.WorldSeconds)
        || FMath::Abs(World->GetTimeSeconds() - Motion.WorldSeconds) > 0.20
        || BayHalfExtentCm.X <= 0.0 || BayHalfExtentCm.Y <= 0.0
        || DeclaredProxySweepHalfExtentCm.X <= 0.0
        || DeclaredProxySweepHalfExtentCm.Y <= 0.0
        || DeclaredProxySweepHalfExtentCm.Z <= 0.0)
    {
        return Result;
    }
    Result.bKnown = true;
    Result.bGrounded = Motion.bGrounded;
    Result.RobotSocketCm = Socket;
    Result.StationPointCm = Point;
    Result.ActualVelocityCmPerSecond = Motion.ActualVelocityCmPerSecond;
    Result.ObservationWorldSeconds = Motion.WorldSeconds;
    Result.SocketDistanceCm = FVector::Dist(Socket,Point);
    Result.SocketHeightDifferenceCm = FMath::Abs(Socket.Z - Point.Z);

    FCollisionQueryParams PathQuery(SCENE_QUERY_STAT(N09ServicePath), false);
    PathQuery.AddIgnoredActor(&Pawn);
    FHitResult PathHit;
    Result.bServicePathObserved = true;
    Result.bServicePathClear = !World->LineTraceSingleByChannel(
        PathHit, Socket, Point, ECC_Visibility, PathQuery);

    // This oversized box is a conservative *declared proxy* clearance check.
    // It is never copied to FDDSN08Observation::bFullLidSweepObserved; only
    // N12's actual complete opening/closing observation can populate that.
    const FVector SweepCenter = Pawn.GetActorLocation()
        + FVector(0,0,DeclaredProxySweepCenterHeightCm);
    FCollisionQueryParams SweepQuery(SCENE_QUERY_STAT(N09DeclaredProxySpace),false);
    SweepQuery.AddIgnoredActor(&Pawn);
    Result.bDeclaredProxyEnvelopeObserved = true;
    Result.bDeclaredProxyEnvelopeClear = !World->OverlapBlockingTestByChannel(
        SweepCenter, Pawn.GetActorQuat(), ECC_Pawn,
        FCollisionShape::MakeBox(DeclaredProxySweepHalfExtentCm), SweepQuery);

    const FVector LocalCenter = GetActorTransform().InverseTransformPosition(Pawn.GetActorLocation());
    const float YawDifference = FMath::DegreesToRadians(
        Pawn.GetActorRotation().Yaw - GetActorRotation().Yaw);
    const float C = FMath::Abs(FMath::Cos(YawDifference));
    const float S = FMath::Abs(FMath::Sin(YawDifference));
    const float ProjectedX = C * DeclaredProxySweepHalfExtentCm.X
        + S * DeclaredProxySweepHalfExtentCm.Y;
    const float ProjectedY = S * DeclaredProxySweepHalfExtentCm.X
        + C * DeclaredProxySweepHalfExtentCm.Y;
    Result.bDeclaredProxyEnvelopeFitsPaintedBay =
        FMath::Abs(LocalCenter.X) + ProjectedX <= BayHalfExtentCm.X
        && FMath::Abs(LocalCenter.Y) + ProjectedY <= BayHalfExtentCm.Y;
    return Result;
}

FDDSN09ApproachProbe ADDSN09ServiceBay::ObserveDeclaredProxyApproaches(
    const ADDSCityRollingPawn& Pawn) const
{
    FDDSN09ApproachProbe Result;
    const UWorld* World = GetWorld();
    if (!World || !HasUniqueStationId() || Pawn.GetActorLocation().ContainsNaN()
        || BayHalfExtentCm.X <= 0.0 || BayHalfExtentCm.Y <= 0.0)
        return Result;
    // N06's measured swept chassis half extents and +11 cm body-center offset.
    // Height uses the observed pawn center on this fixture. A sweep is only an
    // authoring diagnostic; wheel support and actual driving are still needed.
    const FVector BodyOffset(0,0,11.0f);
    const FVector ParkingCenter(GetActorLocation().X,GetActorLocation().Y,
        Pawn.GetActorLocation().Z);
    const FVector End = ParkingCenter + BodyOffset;
    const float OuterDistance = BayHalfExtentCm.X + 125.0f;
    const FVector Along = GetActorForwardVector() * OuterDistance;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(N09DeclaredApproach),false);
    Query.AddIgnoredActor(&Pawn);
    const FCollisionShape Body = FCollisionShape::MakeBox(FVector(45.0,32.5,24.0));
    FHitResult Hit;
    const FQuat BoxRotation = GetActorQuat();
    Result.bFromNegativeLocalXClear = !World->SweepSingleByChannel(
        Hit,End-Along,End,BoxRotation,ECC_Pawn,Body,Query);
    Hit = FHitResult();
    Result.bFromPositiveLocalXClear = !World->SweepSingleByChannel(
        Hit,End+Along,End,BoxRotation,ECC_Pawn,Body,Query);
    Result.ParkingPawnCenterCm = ParkingCenter;
    Result.DeclaredCorridorLengthCm = OuterDistance;
    Result.bObserved = true;
    return Result;
}

FDDSN09VisualDecision ADDSN09ServiceBay::ProjectReadOnly(
    const FString& BayStationId, EDDSN08Leg BayLeg,
    const FDDSN08StableSnapshot& State,
    const FDDSN08Presentation& Presentation,
    const FDDSN08Eligibility& Eligibility)
{
    FDDSN09VisualDecision Out;
    Out.Leg = BayLeg;
    const bool bPickup = BayLeg == EDDSN08Leg::Pickup;
    Out.WordAndIcon = bPickup ? TEXT("PICKUP  [<- BOX]") : TEXT("DELIVER  <BOX ->>");
    Out.OutlineShape = bPickup ? TEXT("RECTANGLE") : TEXT("CHEVRON");
    const FString& ActiveId = bPickup ? State.PickupStationId : State.DeliveryStationId;
    Out.bActive = !BayStationId.IsEmpty() && !State.MissionInstanceId.IsEmpty()
        && BayStationId == ActiveId
        && (bPickup ? Presentation.bPickupBeacon : Presentation.bDeliveryBeacon);
    if (!Out.bActive) return Out;
    // The beacon deliberately remains on the committed leg while N11 saves.
    // N08 eligibility may already describe the *next* leg (or Completed), so
    // its Leg/Reason cannot supply the current bay's persistence status.
    if (Presentation.SaveGate != EDDSN08Gate::None)
    {
        Out.Prompt = Presentation.Status;
        return Out; // Never display usable E through a persistence gate.
    }
    const bool bMatchingDecision = Eligibility.MissionInstanceId == State.MissionInstanceId
        && Eligibility.StationId == BayStationId && Eligibility.Leg == BayLeg;
    Out.bShowE = bMatchingDecision && Eligibility.bAllowed;
    if (Out.bShowE)
        Out.Prompt = bPickup ? TEXT("E  Pick up parcel") : TEXT("E  Deliver parcel");
    else if (!bMatchingDecision)
        Out.Prompt = TEXT("Service status unavailable");
    else switch (Eligibility.Reason)
    {
        case EDDSN08Denial::Moving:
            Out.Prompt = bPickup ? TEXT("Stop to pick up") : TEXT("Stop to deliver");
            break;
        case EDDSN08Denial::WrongStationOrBay:
            Out.Prompt = bPickup ? TEXT("Approach pickup bay") : TEXT("Approach delivery bay");
            break;
        case EDDSN08Denial::ObstructedOrDoorUnavailable:
            Out.Prompt = TEXT("Service path or door unavailable");
            break;
        case EDDSN08Denial::BusyOrSaving:
            Out.Prompt = Presentation.Status;
            break;
        case EDDSN08Denial::InputHeld:
            Out.Prompt = TEXT("Release E to interact");
            break;
        default:
            Out.Prompt = TEXT("Service unavailable");
            break;
    }
    return Out;
}

void ADDSN09ServiceBay::ApplyReadOnly(const FDDSN08StableSnapshot& State,
    const FDDSN08Presentation& Presentation, const FDDSN08Eligibility& Eligibility)
{
    if (!HasUniqueStationId())
    {
        UpdateMarker(false,FDDSN09VisualDecision());
        return;
    }
    UpdateMarker(true, ProjectReadOnly(StationId,GetLeg(),State,Presentation,Eligibility));
}

void ADDSN09ServiceBay::UpdateMarker(bool bHaveProjection,
    const FDDSN09VisualDecision& Decision)
{
    const bool bActive = bHaveProjection && Decision.bActive;
    const FLinearColor BayColor = bPickupStation
        ? FLinearColor(0.05f,0.90f,0.10f) : FLinearColor(0.95f,0.04f,0.03f);
    GroundLight->SetLightColor(BayColor);
    GroundLight->SetVisibility(bActive);
    GroundDecal->SetVisibility(bActive && GlowInstance != nullptr);
    if (GlowInstance)
    {
        GlowInstance->SetVectorParameterValue(TEXT("MarkerColor"),BayColor);
        GlowInstance->SetScalarParameterValue(TEXT("MarkerStrength"),bActive ? 1.0f : 0.0f);
        GlowInstance->SetScalarParameterValue(TEXT("ShapeKind"),bPickupStation ? 0.0f : 1.0f);
    }
    const FString Label = bActive ? Decision.WordAndIcon
        : (bPickupStation ? TEXT("PICKUP BAY (inactive)") : TEXT("DELIVERY BAY (inactive)"));
    IconAndLabel->SetText(FText::FromString(Label));
    PromptText->SetText(FText::FromString(bActive ? Decision.Prompt : FString()));
}
