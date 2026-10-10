#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryStation.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

// Functional station marker placed on measured existing city surfaces.
UCLASS()
class DDSCITY_API AMAYADeliveryStation : public AActor
{
    GENERATED_BODY()
public:
    AMAYADeliveryStation();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    static constexpr double ObjectiveRingRadiusCm = 90.0;
    static constexpr double ChargingRadiusCm = ObjectiveRingRadiusCm; // physical eligibility matches visible90cm green ring
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery") FString StationId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery") FString DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery") bool bDepot = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery") bool bChargingStation = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery") FVector ParcelOffsetCm = FVector(0, 0, 6.5);
    FVector GetServicePoint() const { return bHasServicePoint ? ServicePoint : GetActorLocation(); }
    void SetServicePoint(const FVector& MeasuredParkingPoint);
    FTransform GetParcelPose() const;
    void SetObjectiveActive(bool bActive);
    void SetObjectiveVisualState(bool bActive, bool bDelivery, bool bRobotInside);
    bool IsObjectiveActive() const { return bObjectiveActive; }
    bool IsChargingStation() const { return bDepot || bChargingStation; }
    void SetChargingVisualState(bool bConnected);
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> RingCore;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> RingHalo;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> RingCoreSegments;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> RingHaloSegments;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RingCoreMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RingHaloMaterial;
    void UpdateRingGlow();
    void UpdateRingGroundPlacement();
    FName RingColorParameter = NAME_None;
    FVector ServicePoint = FVector::ZeroVector;
    bool bHasServicePoint = false;
    bool bObjectiveActive = false;
    bool bDeliveryObjective = false;
    bool bChargerConnected = false;
    bool bRobotInsideRing = false;
    bool bRingVisibilityInitialized = false;
    bool bRingVisible = false;
    bool bRingGlowDirty = true;
};
