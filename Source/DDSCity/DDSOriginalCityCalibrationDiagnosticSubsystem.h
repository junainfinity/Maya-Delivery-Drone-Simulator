#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DDSOriginalRobotMeasurementProducer.h"
#include "DDSOriginalCityCalibrationDiagnosticSubsystem.generated.h"
class ADDSCityRollingPawn;
class AActor;
class APawn;
class APlayerController;
class AGameModeBase;
class UStaticMesh;
class UDDSOriginalRobotRuntimeOwner;

// Explicit development diagnostic ONLY. One actual-City source observation and
// one private fixture attempt; no possession, measured binding or readiness.
UCLASS()
class DDSCITY_API UDDSOriginalCityCalibrationDiagnosticSubsystem final : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsTickable() const override;
    virtual TStatId GetStatId() const override;
    virtual void Deinitialize() override;
private:
    void RunOnce();
    bool StartupContextMatches(FString& Error) const;
    void ClearStartupSources();
    UPROPERTY(Transient) TObjectPtr<UClass> PreloadedPublisherClass;
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> PreloadedCargoMesh;
    TWeakObjectPtr<UWorld> StartupWorld;
    TWeakObjectPtr<AGameModeBase> StartupMode;
    TWeakObjectPtr<APlayerController> StartupController;
    TWeakObjectPtr<APawn> StartupPawn;
    TWeakObjectPtr<AActor> StartupView;
    const void* StartupSceneIdentity=nullptr;
    bool bStartupSourcesLoaded=false,bStartupLoading=false,bStartupHadView=false,bStartupWaitingLogged=false;
    bool ReleaseOwned(FString& Error);
    UPROPERTY(Transient) TObjectPtr<AActor> OwnedPublisher;
    UPROPERTY(Transient) TObjectPtr<AActor> OwnedCargoHolder;
    UPROPERTY(Transient) TObjectPtr<ADDSCityRollingPawn> OwnedCarrier;
    UPROPERTY(Transient) TObjectPtr<UDDSOriginalRobotRuntimeOwner> OwnedOwner;
    FDDSOriginalRobotGameMountLease OwnedGameMounts;
    bool bOwnedGameMountLease=false;
    bool bFinished=false,bRunning=false;
    double StartedRealS=0.0;
};
