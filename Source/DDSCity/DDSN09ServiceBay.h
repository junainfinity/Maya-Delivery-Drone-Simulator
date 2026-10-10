#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDSN08Authority.h"
#include "DDSN09ServiceBay.generated.h"

class ADDSCityRollingPawn;
class UBoxComponent;
class UDecalComponent;
class UPointLightComponent;
class USceneComponent;
class UTextRenderComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

// N09 only reports world geometry. This is a DECLARED proxy envelope, not N12
// evidence of either actual lid or parcel. No mission/cargo state lives here.
struct FDDSN09GeometryObservation
{
    bool bKnown = false;
    bool bGrounded = false;
    FVector RobotSocketCm = FVector::ZeroVector;
    FVector StationPointCm = FVector::ZeroVector;
    FVector ActualVelocityCmPerSecond = FVector::ZeroVector;
    double ObservationWorldSeconds = 0.0;
    bool bServicePathObserved = false;
    bool bServicePathClear = false;
    bool bDeclaredProxyEnvelopeObserved = false;
    bool bDeclaredProxyEnvelopeClear = false;
    bool bDeclaredProxyEnvelopeFitsPaintedBay = false;
    float SocketDistanceCm = 0.0f; // diagnostic only; N08 applies 125 cm guard.
    float SocketHeightDifferenceCm = 0.0f; // N08 applies 50 cm guard.
};

struct FDDSN09VisualDecision
{
    bool bActive = false;
    bool bShowE = false;
    EDDSN08Leg Leg = EDDSN08Leg::Pickup;
    FString Prompt;
    FString WordAndIcon;
    FString OutlineShape;
};

struct FDDSN09ApproachProbe
{
    bool bObserved = false;
    bool bFromNegativeLocalXClear = false;
    bool bFromPositiveLocalXClear = false;
    FVector ParkingPawnCenterCm = FVector::ZeroVector;
    float DeclaredCorridorLengthCm = 0.0f;
};

UCLASS()
class DDSCITY_API ADDSN09ServiceBay final : public AActor
{
    GENERATED_BODY()
public:
    ADDSN09ServiceBay();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;

    // World point must be authored at the robot's real service-socket height.
    FVector GetServicePointWorldCm() const;
    const FString& GetStationId() const { return StationId; }
    EDDSN08Leg GetLeg() const { return bPickupStation ? EDDSN08Leg::Pickup : EDDSN08Leg::Delivery; }

    FDDSN09GeometryObservation ObserveGeometry(
        const ADDSCityRollingPawn& Pawn, const USceneComponent& RobotServiceSocket) const;
    // Diagnostic corridor only. N13 must actually drive both approaches with
    // final N05 contact and real route geometry; this does not grant E.
    FDDSN09ApproachProbe ObserveDeclaredProxyApproaches(
        const ADDSCityRollingPawn& Pawn) const;

    // Pure projection of N08's authoritative state and the same eligibility
    // returned to the coordinator. No radius, speed or cargo recheck here.
    static FDDSN09VisualDecision ProjectReadOnly(
        const FString& BayStationId, EDDSN08Leg BayLeg,
        const FDDSN08StableSnapshot& State,
        const FDDSN08Presentation& Presentation,
        const FDDSN08Eligibility& Eligibility);
    void ApplyReadOnly(const FDDSN08StableSnapshot& State,
                       const FDDSN08Presentation& Presentation,
                       const FDDSN08Eligibility& Eligibility);

private:
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<UBoxComponent> BayFootprint;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<USceneComponent> ServicePoint;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<UDecalComponent> GroundDecal;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<UPointLightComponent> GroundLight;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<UTextRenderComponent> IconAndLabel;
    UPROPERTY(VisibleAnywhere, Category="N09")
    TObjectPtr<UTextRenderComponent> PromptText;

    UPROPERTY(EditInstanceOnly, Category="N09|Identity")
    FString StationId;
    UPROPERTY(EditInstanceOnly, Category="N09|Identity")
    bool bPickupStation = true;
    UPROPERTY(EditInstanceOnly, Category="N09|Geometry", meta=(ClampMin="0"))
    FVector BayHalfExtentCm = FVector(210.0, 175.0, 8.0);
    // Conservative proxy fixture: N06 box is half (45,32.5,24) cm. This
    // larger box is only planned bay space, never N12 full-lid-sweep proof.
    UPROPERTY(EditInstanceOnly, Category="N09|Proxy Fixture", meta=(ClampMin="0"))
    FVector DeclaredProxySweepHalfExtentCm = FVector(95.0, 105.0, 48.0);
    UPROPERTY(EditInstanceOnly, Category="N09|Proxy Fixture", meta=(ClampMin="0"))
    float DeclaredProxySweepCenterHeightCm = 55.0f;
    // Author a deferred-decal material with MarkerColor/MarkerStrength params.
    // GroundLight and text remain visible if this asset is not assigned.
    UPROPERTY(EditDefaultsOnly, Category="N09|Presentation")
    TObjectPtr<UMaterialInterface> GroundGlowMaterial;
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> GlowInstance;

    bool HasUniqueStationId() const;
    void UpdateMarker(bool bActive, const FDDSN09VisualDecision& Decision);
};
