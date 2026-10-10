#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDSN08Authority.h"
#include "DDSN10RouteStar.generated.h"

class ADDSCityRollingPawn;
class UPointLightComponent;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;
class UPrimitiveComponent;

// Coordinator listens and calls N08.RecordN10Star once. The event carries the
// instance observed at contact, so a delayed event cannot enter a replay run.
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDDSN10ValidatedContact,
    const FString& /*MissionInstanceId*/, const FString& /*ScopedStarId*/,
    ADDSCityRollingPawn* /*OwnedPawn*/);

UCLASS()
class DDSCITY_API ADDSN10RouteStar final : public AActor
{
    GENERATED_BODY()
public:
    ADDSN10RouteStar();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    const FString& GetStableSlotId() const { return StableSlotId; }
    const FString& GetScopedStarId() const { return ScopedStarId; }
    FDDSN10ValidatedContact& OnValidatedContact() { return ValidatedContact; }

    // Sole coordinator refreshes this on mission start, collection, restore,
    // save-gate changes and explicit replay. Projection never changes N08.
    void ApplyAuthoritySnapshot(const FDDSN08StableSnapshot& State,
                                EDDSN08Gate Gate,
                                ADDSCityRollingPawn* InOwnedPawn);

private:
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<USphereComponent> Trigger;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<USceneComponent> VisualPivot;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<UStaticMeshComponent> OptionalStarMesh;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<UTextRenderComponent> GoldFallbackGlyph;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<UTextRenderComponent> OptionalLabel;
    UPROPERTY(VisibleAnywhere, Category="N10")
    TObjectPtr<UPointLightComponent> GoldLight;

    // Three unique authored slots (e.g. Main, Passage, Detour) persist across
    // replay. ScopedStarId includes a NEW mission instance on each replay.
    UPROPERTY(EditInstanceOnly, Category="N10|Identity")
    FString StableSlotId;
    FString BoundMissionInstanceId;
    FString ScopedStarId;
    TWeakObjectPtr<ADDSCityRollingPawn> OwnedPawn;
    FDDSN10ValidatedContact ValidatedContact;
    bool bCollectionEnabled = false;

    bool HasUniqueSlotId() const;
    bool HasAccessibleGroundAndClearPath(const ADDSCityRollingPawn& Pawn) const;
    void SetProjectedActive(bool bActive);

    UFUNCTION()
    void OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent,
        AActor* OtherActor, UPrimitiveComponent* OtherComp,
        int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};
