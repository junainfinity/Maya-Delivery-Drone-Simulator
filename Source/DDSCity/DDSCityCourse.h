#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDSCityCourse.generated.h"

class UStaticMesh;
class USceneComponent;

// Temporary N06 motion fixture. N13 replaces it with a measured City Sample route.
UCLASS()
class DDSCITY_API ADDSCityCourse : public AActor
{
    GENERATED_BODY()

public:
    ADDSCityCourse();
    virtual void BeginPlay() override;

private:
    UPROPERTY()
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY()
    TObjectPtr<UStaticMesh> CubeMesh;

    void AddBox(const TCHAR* Name, const FVector& Center, const FVector& Size, const FRotator& Rotation, bool bRecoverable);
};
