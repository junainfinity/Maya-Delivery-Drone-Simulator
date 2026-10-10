#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDSN07RuntimeProbe.generated.h"

class ADDSCityRollingPawn;

UCLASS()
class DDSCITY_API ADDSN07RuntimeProbe : public AActor
{
    GENERATED_BODY()

public:
    ADDSN07RuntimeProbe();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    UPROPERTY()
    TObjectPtr<ADDSCityRollingPawn> Pawn;
    float Elapsed = 0.0f;
    int32 Step = 0;
    int32 RowCount = 0;
    FString Scenario = TEXT("OpenForward");
    FString Csv;
    FString OutputPath;
    void AdvanceScenario();
};
