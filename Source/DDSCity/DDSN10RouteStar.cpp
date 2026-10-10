#include "DDSN10RouteStar.h"
#include "DDSN10StarBridge.h"
#include "DDSN10AccessSweep.h"

#include "DDSCityRollingPawn.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PhysicsEngine/BodyInstance.h"

ADDSN10RouteStar::ADDSN10RouteStar()
{
    PrimaryActorTick.bCanEverTick = true;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("N10Root"));
    RootComponent = SceneRoot;
    Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("NonBlockingStarTrigger"));
    Trigger->SetupAttachment(SceneRoot);
    Trigger->SetRelativeLocation(FVector(0,0,80));
    Trigger->SetSphereRadius(55.0f);
    Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
    Trigger->SetCollisionResponseToChannel(ECC_Pawn,ECR_Overlap);
    Trigger->SetGenerateOverlapEvents(true);
    VisualPivot = CreateDefaultSubobject<USceneComponent>(TEXT("GoldStarPivot"));
    VisualPivot->SetupAttachment(SceneRoot);
    VisualPivot->SetRelativeLocation(FVector(0,0,80));
    OptionalStarMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AuthoredStarMesh"));
    OptionalStarMesh->SetupAttachment(VisualPivot);
    OptionalStarMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    OptionalStarMesh->SetGenerateOverlapEvents(false);
    GoldFallbackGlyph = CreateDefaultSubobject<UTextRenderComponent>(TEXT("GoldFallbackGlyph"));
    GoldFallbackGlyph->SetupAttachment(VisualPivot);
    GoldFallbackGlyph->SetText(FText::FromString(TEXT("*")));
    GoldFallbackGlyph->SetWorldSize(55.0f);
    GoldFallbackGlyph->SetTextRenderColor(FColor(255,200,35));
    GoldFallbackGlyph->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    OptionalLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("OptionalRouteStarLabel"));
    OptionalLabel->SetupAttachment(SceneRoot);
    OptionalLabel->SetRelativeLocation(FVector(0,0,135));
    OptionalLabel->SetText(FText::FromString(TEXT("OPTIONAL ROUTE STAR")));
    OptionalLabel->SetWorldSize(17.0f);
    OptionalLabel->SetTextRenderColor(FColor(255,220,100));
    OptionalLabel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GoldLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GoldStarLight"));
    GoldLight->SetupAttachment(VisualPivot);
    GoldLight->SetLightColor(FLinearColor(1.0f,0.62f,0.06f));
    GoldLight->SetIntensity(450.0f);
    GoldLight->SetAttenuationRadius(150.0f);
    GoldLight->SetCastShadows(false);
}

void ADDSN10RouteStar::BeginPlay()
{
    Super::BeginPlay();
    Trigger->OnComponentBeginOverlap.AddDynamic(this,&ADDSN10RouteStar::OnTriggerBeginOverlap);
    SetProjectedActive(bCollectionEnabled); // Preserve an early coordinator projection.
}

void ADDSN10RouteStar::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bCollectionEnabled && FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0)
        VisualPivot->AddLocalRotation(FRotator(0,35.0f*DeltaSeconds,0));
}

bool ADDSN10RouteStar::HasUniqueSlotId() const
{
    if (!GetWorld() || StableSlotId.TrimStartAndEnd().IsEmpty()
        || StableSlotId.Contains(TEXT(":"))) return false;
    for (TActorIterator<ADDSN10RouteStar> It(GetWorld()); It; ++It)
    {
        if (*It != this && It->StableSlotId.Equals(StableSlotId,ESearchCase::IgnoreCase))
            return false;
    }
    return true;
}

void ADDSN10RouteStar::SetProjectedActive(bool bActive)
{
    bCollectionEnabled = bActive;
    Trigger->SetCollisionEnabled(bActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    OptionalStarMesh->SetVisibility(bActive && OptionalStarMesh->GetStaticMesh() != nullptr);
    GoldFallbackGlyph->SetVisibility(bActive && OptionalStarMesh->GetStaticMesh() == nullptr);
    OptionalLabel->SetVisibility(bActive);
    GoldLight->SetVisibility(bActive);
    SetActorTickEnabled(bActive);
}

void ADDSN10RouteStar::ApplyAuthoritySnapshot(const FDDSN08StableSnapshot& State,
    EDDSN08Gate Gate, ADDSCityRollingPawn* InOwnedPawn)
{
    OwnedPawn = InOwnedPawn;
    BoundMissionInstanceId = State.MissionInstanceId;
    ScopedStarId = FString();
    if (HasUniqueSlotId() && !BoundMissionInstanceId.IsEmpty())
        ScopedStarId = DDSN10StarBridge::MakeScopedStarId(
            BoundMissionInstanceId,StableSlotId);
    const bool bActive = InOwnedPawn != nullptr && !ScopedStarId.IsEmpty()
        && State.ScopedStarIds.Contains(ScopedStarId)
        && !State.CollectedStarIds.Contains(ScopedStarId)
        && State.Phase != EDDSN08Phase::Completed
        && Gate == EDDSN08Gate::None;
    SetProjectedActive(bActive);
}

bool ADDSN10RouteStar::HasAccessibleGroundAndClearPath(
    const ADDSCityRollingPawn& Pawn) const
{
    const UWorld* World = GetWorld();
    if (!World) return false;
    FDDSRobotSnapshot Physical;
    if (!Pawn.GetFreshPhysicalStateEvidence(Physical)
        || FMath::CountBits(static_cast<uint64>(Physical.SupportMask)) < 3) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(N10StarAccess),false);
    Query.AddIgnoredActor(this);
    Query.AddIgnoredActor(&Pawn);
    const FVector Center = VisualPivot->GetComponentLocation();
    FHitResult GroundHit;
    if (!World->LineTraceSingleByChannel(GroundHit,Center+FVector(0,0,15),
        Center-FVector(0,0,180),ECC_Visibility,Query)) return false;
    const float MinimumGroundNormalZ = FMath::Cos(FMath::DegreesToRadians(10.0f));
    if (GroundHit.ImpactNormal.Z < MinimumGroundNormalZ
        || FMath::Abs(Pawn.GetActorLocation().Z-GroundHit.ImpactPoint.Z) > 70.0f)
        return false;
    // The trigger can overlap across a low wall while a visual-center ray
    // clears its top. Sweep the *owned N06 collision chassis* toward the
    // star's supported ground XY at its current body height. This is a direct
    // physical contact route, not a pathfinding claim or a second star ledger.
    const UStaticMeshComponent* Chassis = Pawn.GetN06ChassisCollision();
    if (!Chassis || Chassis->GetOwner() != &Pawn || !Chassis->IsRegistered()
        || Chassis->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
        return false;
    const FBodyInstance* BodyInstance = Chassis->GetBodyInstance();
    if (!BodyInstance || !BodyInstance->IsValidBodyInstance()) return false;
    const FBox PhysicalBounds = BodyInstance->GetBodyBounds();
    if (!PhysicalBounds.IsValid || PhysicalBounds.Min.ContainsNaN()
        || PhysicalBounds.Max.ContainsNaN()) return false;
    FDDSN10AccessSweep Access;
    // Installed 5.3.2 GetBodyBounds reads the live actor's physical bounds
    // under the scene read lock. World bounds already include supplier scale
    // and physical pitch/roll. Sweep a conservative axis-aligned enclosure
    // without rotating it again, using visual bounds or proxy dimensions.
    if (!FDDSN10AccessSweep::TryBuild(PhysicalBounds.GetCenter(),
        PhysicalBounds.GetExtent(), FQuat::Identity,
        GroundHit.ImpactPoint, Access))
        return false;
    const FCollisionShape BodyShape = FCollisionShape::MakeBox(Access.HalfExtent);
    FHitResult BodyHit;
    if (World->SweepSingleByChannel(BodyHit, Access.Start, Access.End,
        Access.Rotation, ECC_Pawn, BodyShape, Query)
        || World->SweepSingleByChannel(BodyHit, Access.Start, Access.End,
        Access.Rotation, ECC_Visibility, BodyShape, Query)) return false;
    FHitResult PathHit;
    const FVector RobotCenter = Pawn.GetActorLocation()+FVector(0,0,30);
    if (World->LineTraceSingleByChannel(PathHit,RobotCenter,Center,
        ECC_Visibility,Query)) return false;
    FDDSRobotSnapshot AtContact;
    return Pawn.GetFreshPhysicalStateEvidence(AtContact)
        && AtContact.Generation == Physical.Generation
        && AtContact.CollisionWorldGeneration == Physical.CollisionWorldGeneration
        && AtContact.ReadinessRevision == Physical.ReadinessRevision
        && FMath::CountBits(static_cast<uint64>(AtContact.SupportMask)) >= 3;
}

void ADDSN10RouteStar::OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent,
    AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult)
{
    ADDSCityRollingPawn* Pawn = OwnedPawn.Get();
    if (!bCollectionEnabled || !Pawn || !Pawn->IsPlayerControlled()
        || OtherActor != Pawn || !OtherComp
        || OtherComp->GetOwner() != Pawn || !HasUniqueSlotId()
        || BoundMissionInstanceId.IsEmpty() || ScopedStarId.IsEmpty()
        || !HasAccessibleGroundAndClearPath(*Pawn)) return;
    ValidatedContact.Broadcast(BoundMissionInstanceId,ScopedStarId,Pawn);
    // No local award. Coordinator asks N08.RecordN10Star and then projects the
    // authoritative snapshot; duplicate overlap is harmless at that boundary.
}
