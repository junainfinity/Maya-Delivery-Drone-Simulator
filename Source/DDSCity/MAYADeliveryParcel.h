#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryParcel.generated.h"

class UStaticMeshComponent;
class UPrimitiveComponent;
class UPhysicsConstraintComponent;
class AMAYANativeDemoPawn;
class UMaterialInterface;
class UMaterialInstanceDynamic;

// An owned physical parcel, kept independent from the carrier's welded body.
UCLASS()
class DDSCITY_API AMAYADeliveryParcel : public AActor
{
    GENERATED_BODY()
public:
    AMAYADeliveryParcel();
    bool Configure(const FString& InParcelId, double InMassKg);
    UStaticMeshComponent* GetParcelBody() const { return ParcelBody; }
    UStaticMeshComponent* GetParcelVisual() const { return ParcelVisual; }
    const FString& GetParcelId() const { return ParcelId; }
    bool BeginStaging();
    bool CanBeginAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const;
    bool BeginAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool MoveAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, double EasedAlpha, FString& OutReason);
    bool ValidateAirRaisingRemaining(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const;
    bool IsAirRaisingUpperLatched() const { return bAirRaising && bAirRaisingUpperLatched; }
    double GetAirRaisingEasedProgress() const { return AirRaisingEasedProgress; }
    const FTransform& GetAirRaisingReleasePose() const { return AirRaisingInitialLower; }
    bool IsAirRaisingEndpointReady() const;
    bool BeginAirDisappearance();
    bool SetAirDisappearanceAlpha(double Alpha);
    bool CanFinishAirRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const;
    bool FinishAirRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool IsAirRaisingActive() const { return bAirRaising; }
    bool BeginAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool SetAirAppearanceAlpha(double Alpha);
    bool ObserveAirAppearance(double& OutAlpha, bool& OutOriginalMaterialsRestored) const;
    bool MoveAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, double EasedAlpha, FString& OutReason);
    bool IsAirLoweringEndpointReady() const;
    bool FinishAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool IsAirLoweringActive() const { return bAirLowering; }
    const FTransform& GetAirLoweringStartPose() const { return AirStartPose; }
    const FTransform& GetAirLoweringTargetPose() const { return AirTargetPose; }
    double GetAirOpenAssemblyTopCm() const { return AirOpenAssemblyTopCm; }
    bool InsertIntoDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool CanRemoveFromDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const;
    bool HideForDeliveryRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool RestoreToDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason);
    bool IsDormantForDeliveryRemoval() const { return bDormantForDeliveryRemoval; }
    int32 GetDeliverySlot() const { return DeliverySlot; }
    bool MoveStaged(const FVector& Position, FHitResult& Hit);
    bool SecureTo(UPrimitiveComponent* Carrier, const FTransform& Pose);
    void ReleaseForStaging();
    bool PlaceAt(const FTransform& Pose);
    bool CanUseRecoveryPose(const FTransform& Pose, FString& OutReason) const;
    bool PlaceForRecovery(const FTransform& Pose, FString& OutReason);
    bool IsSecuredTo(UPrimitiveComponent* Carrier, const FTransform& Target) const;
    bool IsAt(const FTransform& Target, double ToleranceCm = 2.0) const;
    bool ObserveMass(double& OutMassKg) const;
    bool ObserveNativePose(FTransform& OutPose) const;
    const FString& GetTieDownInitializationEvidenceJson() const { return TieDownInitializationEvidenceJson; }
    void ResetTieDownInitializationEvidence() { TieDownInitializationEvidenceJson.Empty(); }
private:
    void RestoreAirAppearanceMaterials();
    UPROPERTY() TObjectPtr<UMaterialInterface> AirFadeMaterial;
    UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> AirOriginalMaterials;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> AirAppearanceMaterials;
    bool bAirLowering = false, bAirRaising = false;
    bool bAirRaisingUpperLatched = false;
    double AirRaisingEasedProgress = 0;
    FVector AirRaisingInitialBayCm = FVector::ZeroVector;
    FTransform AirRaisingInitialLower = FTransform::Identity;
    double AirAppearanceAlpha = 1, AirOpenAssemblyTopCm = 0;
    FTransform AirStartPose = FTransform::Identity, AirTargetPose = FTransform::Identity;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> ParcelBody;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> ParcelVisual;
    double ParcelVisualUniformScale = 0;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UPhysicsConstraintComponent> CargoConstraint;
    FString ParcelId;
    bool bStaging = false;
    bool bDormantForDeliveryRemoval = false;
    int32 DeliverySlot = INDEX_NONE;
    double ConfiguredMassKg = 0;
    FString TieDownInitializationEvidenceJson;
};
