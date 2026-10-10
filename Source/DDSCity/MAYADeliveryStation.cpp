#include "MAYADeliveryStation.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

AMAYADeliveryStation::AMAYADeliveryStation()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DeliveryStationRoot"));
    SetRootComponent(SceneRoot);
    RingCore = CreateDefaultSubobject<USceneComponent>(TEXT("ObjectiveRingCore"));
    RingHalo = CreateDefaultSubobject<USceneComponent>(TEXT("ObjectiveRingHalo"));
    RingCore->SetupAttachment(SceneRoot);
    RingHalo->SetupAttachment(SceneRoot);
    RingCore->SetMobility(EComponentMobility::Movable);
    RingHalo->SetMobility(EComponentMobility::Movable);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Glow(TEXT("/Game/MAYA/DeliveryPresentation/M_ObjectiveRingRadial_V2.M_ObjectiveRingRadial_V2"));
    // One flat surface avoids additive cube-face/segment overlap. The shader
    // derives the same90cm radius and smooth12cm halo from this192cm UV plane.
    UStaticMeshComponent* Surface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ObjectiveRingSurface"));
    Surface->SetupAttachment(RingCore);
    Surface->SetMobility(EComponentMobility::Movable);
    Surface->SetForceDisableNanite(true); // Additive material uses the ordinary mesh renderer.
    if (Plane.Succeeded())
    {
        Surface->SetStaticMesh(Plane.Object);
        const FBoxSphereBounds Bounds = Plane.Object->GetBounds();
        if (Bounds.BoxExtent.X > SMALL_NUMBER && Bounds.BoxExtent.Y > SMALL_NUMBER)
        {
            const FVector Scale(192.0 / (2.0 * Bounds.BoxExtent.X), 192.0 / (2.0 * Bounds.BoxExtent.Y), 1.0);
            Surface->SetRelativeScale3D(Scale);
            Surface->SetRelativeLocation(-Bounds.Origin * Scale + FVector(0, 0, 0.9));
        }
    }
    if (Glow.Succeeded())
        for (int32 I = 0; I < Surface->GetNumMaterials(); ++I) Surface->SetMaterial(I, Glow.Object);
    Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Surface->SetCollisionResponseToAllChannels(ECR_Ignore);
    Surface->SetGenerateOverlapEvents(false);
    Surface->SetCanEverAffectNavigation(false);
    Surface->SetCastShadow(false);
    Surface->SetReceivesDecals(false);
    Surface->bAffectDistanceFieldLighting = false;
    Surface->bAffectDynamicIndirectLighting = false;
    Surface->SetHiddenInGame(true);
    RingCoreSegments.Add(Surface);
}

void AMAYADeliveryStation::SetServicePoint(const FVector& MeasuredParkingPoint)
{
    if (MeasuredParkingPoint.ContainsNaN()) return;
    ServicePoint = MeasuredParkingPoint;
    bHasServicePoint = true;
    UpdateRingGroundPlacement();
}

void AMAYADeliveryStation::UpdateRingGroundPlacement()
{
    if (!bHasServicePoint) return;
    double HighestGroundZ = ServicePoint.Z;
    int32 ObservedSamples = 0;
    if (UWorld* World = GetWorld())
    {
        // Match the city's already successful simple Visibility sidewalk
        // survey. Restrict the read-only marker query to static world bodies.
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYAObjectiveRingPavement), false, this);
        Query.MobilityType = EQueryMobilityType::Static;
        // Read existing static surfaces across both edges and the centre of
        // each band. The ring owns no collider and does not move its station.
        constexpr int32 SamplesAround = 96;
        const double Radii[] = { ObjectiveRingRadiusCm - 6.1, ObjectiveRingRadiusCm, ObjectiveRingRadiusCm + 6.1 };
        for (int32 I = 0; I < SamplesAround; ++I)
        {
            const double Angle = 2.0 * PI * (I + 0.5) / SamplesAround;
            for (double Radius : Radii)
            {
                const FVector XY = ServicePoint + FVector(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 0);
                FHitResult Hit;
                if (World->LineTraceSingleByChannel(Hit, XY + FVector(0, 0, 25), XY - FVector(0, 0, 35), ECC_Visibility, Query)
                    && Hit.ImpactNormal.Z >= 0.8 && FMath::Abs(Hit.ImpactPoint.Z - ServicePoint.Z) <= 20)
                {
                    HighestGroundZ = FMath::Max(HighestGroundZ, Hit.ImpactPoint.Z);
                    ++ObservedSamples;
                }
            }
        }
    }
    // Flat surface is0.9cm above its root. Keep that surface at least
    // 4cm above the highest observed nearby pavement, with depth testing on.
    const FVector VisualRoot(ServicePoint.X, ServicePoint.Y, HighestGroundZ + 4.0 - 0.9);
    RingCore->SetWorldLocation(VisualRoot);
    RingHalo->SetWorldLocation(VisualRoot);
    UE_LOG(LogTemp, Display, TEXT("MAYA objective ring %s pavement: static samples%d/288, serviceZ%.3f, highestZ%.3f, visualRootZ%.3f; presentation only."),
        *StationId, ObservedSamples, ServicePoint.Z, HighestGroundZ, VisualRoot.Z);
}

void AMAYADeliveryStation::BeginPlay()
{
    Super::BeginPlay();
    UMaterialInterface* Base = RingCoreSegments.Num() ? RingCoreSegments[0]->GetMaterial(0) : nullptr;
    if (!Base) return;
    TArray<FMaterialParameterInfo> Parameters;
    TArray<FGuid> Ids;
    Base->GetAllVectorParameterInfo(Parameters, Ids);
    for (const FMaterialParameterInfo& Parameter : Parameters)
        if (Parameter.Name == FName(TEXT("Color"))) { RingColorParameter = Parameter.Name; break; }
    if (RingColorParameter.IsNone())
    {
        UE_LOG(LogTemp, Error, TEXT("MAYA objective ring: owned radial emissive Color parameter unavailable; marker hidden."));
        return;
    }
    RingCoreMaterial = UMaterialInstanceDynamic::Create(Base, this);
    RingHaloMaterial = UMaterialInstanceDynamic::Create(Base, this);
    if (!RingCoreMaterial || !RingHaloMaterial) return;
    for (UStaticMeshComponent* Segment : RingCoreSegments)
        for (int32 I = 0; I < Segment->GetNumMaterials(); ++I) Segment->SetMaterial(I, RingCoreMaterial);
    for (UStaticMeshComponent* Segment : RingHaloSegments) Segment->SetMaterial(0, RingHaloMaterial);
    UE_LOG(LogTemp, Display, TEXT("MAYA objective ring %s: radius90cm, one192cm flat radial surface, Color parameter observed; no collision or service-radius change."), *StationId);
    // Materials are now available even if a caller set the desired state earlier.
    bRingVisibilityInitialized = false;
    UpdateRingGlow();
    SetObjectiveVisualState(bObjectiveActive, bDeliveryObjective, bRobotInsideRing);
}

void AMAYADeliveryStation::SetObjectiveActive(bool bActive)
{
    SetObjectiveVisualState(bActive, bDeliveryObjective, bRobotInsideRing);
}

void AMAYADeliveryStation::SetObjectiveVisualState(bool bActive, bool bDelivery, bool bRobotInside)
{
    const bool bGlowInputsChanged = bObjectiveActive != bActive
        || bDeliveryObjective != bDelivery || bRobotInsideRing != bRobotInside;
    bObjectiveActive = bActive;
    bDeliveryObjective = bDelivery;
    bRobotInsideRing = bRobotInside;
    const bool Visible = (bActive || IsChargingStation()) && RingCoreMaterial && RingHaloMaterial;
    if (!bRingVisibilityInitialized || bRingVisible != Visible)
    {
        // Propagation dirties every child render state, even for an unchanged
        // hidden flag. Only perform the traversal when visibility changes.
        RingCore->SetHiddenInGame(!Visible, true);
        RingHalo->SetHiddenInGame(!Visible, true);
        SetActorTickEnabled(Visible);
        bRingVisibilityInitialized = true;
        bRingVisible = Visible;
        if (Visible) bRingGlowDirty = true;
    }
    if (bGlowInputsChanged) bRingGlowDirty = true;
}

void AMAYADeliveryStation::SetChargingVisualState(bool bConnected)
{
    if (bChargerConnected == bConnected) return;
    bChargerConnected = bConnected;
    bRingGlowDirty = true;
}

void AMAYADeliveryStation::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bRingVisible && (bRingGlowDirty || bRobotInsideRing || bChargerConnected)) UpdateRingGlow();
}

void AMAYADeliveryStation::UpdateRingGlow()
{
    if (!RingCoreMaterial || !RingHaloMaterial || RingColorParameter.IsNone()) return;
    // One surface cannot show two hues at the same pose without mixing them.
    // A delivery objective is red; a connected charger takes green priority.
    // Idle chargers stay green even when this station is not the objective.
    const bool bRedDelivery = bObjectiveActive && bDeliveryObjective && !bChargerConnected;
    const FLinearColor Hue = bRedDelivery ? FLinearColor(1.0f, 0.025f, 0.02f) : FLinearColor(0.02f, 1.0f, 0.10f);
    const double Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    const bool bBright = bRobotInsideRing || bChargerConnected;
    const double Pulse = bBright ? 1.0 + 0.15 * FMath::Sin(Time * 2.0 * PI * 1.1) : 1.0;
    const float Emission = float((bBright ? 18.0 : 8.0) * Pulse);
    RingCoreMaterial->SetVectorParameterValue(RingColorParameter, FLinearColor(Hue.R * Emission, Hue.G * Emission, Hue.B * Emission, 1));
    RingHaloMaterial->SetVectorParameterValue(RingColorParameter, FLinearColor(Hue.R * Emission * 0.18f, Hue.G * Emission * 0.18f, Hue.B * Emission * 0.18f, 1));
    bRingGlowDirty = false;
}

FTransform AMAYADeliveryStation::GetParcelPose() const
{
    FTransform Pose = GetActorTransform();
    Pose.SetLocation(GetActorTransform().TransformPosition(ParcelOffsetCm));
    Pose.SetScale3D(FVector::OneVector);
    return Pose;
}
