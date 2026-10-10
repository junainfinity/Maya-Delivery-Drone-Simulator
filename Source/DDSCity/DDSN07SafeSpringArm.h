#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpringArmComponent.h"
#include "DDSN07SafeSpringArm.generated.h"

// Keeps the collision sweep authoritative while easing only the safe return
// toward the nominal chase distance after an obstruction retreats.
UCLASS(ClassGroup=(Camera), meta=(BlueprintSpawnableComponent))
class DDSCITY_API UDDSN07SafeSpringArm : public USpringArmComponent
{
    GENERATED_BODY()

public:
    float GetRenderedArmDistanceCm() const { return RenderedArmDistanceCm; }

protected:
    virtual FVector BlendLocations(const FVector& DesiredArmLocation, const FVector& TraceHitLocation,
        bool bHitSomething, float DeltaTime) override;

private:
    float RenderedArmDistanceCm = -1.0f;
    bool bWasBlocked = false;
    bool bReturning = false;
    double LastStepLogSeconds = -100.0;
};
