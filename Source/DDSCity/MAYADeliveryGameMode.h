#pragma once

#include "CoreMinimal.h"
#include "Game/CitySampleGameMode.h"
#include "MAYADeliveryGameMode.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class UWorldPartitionStreamingSourceComponent;
class ACameraActor;

// Runtime-only city setup. No publisher map, collision or asset is rewritten.
UCLASS()
class DDSCITY_API AMAYADeliveryGameMode : public ACitySampleGameMode
{
    GENERATED_BODY()
public:
    AMAYADeliveryGameMode();
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
    const FString& GetBootstrapStatus() const { return BootstrapStatus; }
    bool HasCompletedDeliveryBootstrap() const { return bBootstrapped; }
    bool HasStoppedDeliveryBootstrap() const { return bBootstrapStopped; }
private:
    bool TraceSidewalk(const FVector& XY, FHitResult& Hit) const;
    bool SurveyRoute(TArray<FVector>& OutRoute) const;
    bool Bootstrap();
    bool ApplyCosmetics(AMAYANativeDemoPawn& Pawn) const;
    void StopBootstrap(const FString& Reason);
    UPROPERTY() TObjectPtr<UWorldPartitionStreamingSourceComponent> CityStreaming;
    UPROPERTY() TObjectPtr<UWorldPartitionStreamingSourceComponent> CityBlockBootstrapStreaming;
    UPROPERTY() TObjectPtr<ACameraActor> LoadingCamera;
    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<AMAYADeliveryDirector> Director;
    TWeakObjectPtr<APlayerController> DeliveryPlayer;
    double NextBootstrapWorldS = 0;
    double BootstrapStartWorldS = -1;
    double CityBlockPreparationStartedWorldS = -1;
    FString CityBlockUnavailableReason;
    bool bBootstrapped = false;
    bool bBootstrapStopped = false;
    FString BootstrapStatus = TEXT("Preparing the city delivery route...");
    const FVector HistoricalDepot = FVector(-29898.724609375, -100.7442474, 73.0);
};
