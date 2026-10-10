#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDSCityRollingPawn.h"
#include "DDSN06RuntimeProbe.generated.h"

class ADDSCityRollingPawn;
class UBoxComponent;

UCLASS()
class DDSCITY_API ADDSN06RuntimeProbe : public AActor
{
    GENERATED_BODY()

public:
    ADDSN06RuntimeProbe();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    void StartScenario();
    void Finish();
    void SpawnBlocker();
    void SpawnPoseBlockers();

    TWeakObjectPtr<ADDSCityRollingPawn> Pawn;
    FString Csv;
    FString OutputPath;
    FString ScenarioName;
    int32 ScenarioIndex = -1;
    float ScenarioSeconds = 0.0f;
    float ScenarioDuration = 0.0f;
    bool bRecoveryResult = false;
    FVector BeforeRecovery = FVector::ZeroVector;

    UPROPERTY()
    TObjectPtr<UBoxComponent> RecoveryBlocker;

    UPROPERTY()
    TArray<TObjectPtr<UBoxComponent>> RecoveryPoseBlockers;

    FDDSRecoveryLedgerView SuppliedLedger;
    FString LedgerBefore;
    bool bVisualProof = false;
    bool bCapturedUphill = false;
    bool bCapturedDownhill = false;
    bool bCapturedSingleWheel = false;
};
