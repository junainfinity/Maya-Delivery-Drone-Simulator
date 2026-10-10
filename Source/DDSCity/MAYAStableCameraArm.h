#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpringArmComponent.h"
#include "MAYAStableCameraArm.generated.h"

// A level chase camera with its own world-space pivot. Chassis suspension and
// body roll remain visible on the robot without moving the player's horizon.
UCLASS()
class DDSCITY_API UMAYAStableCameraArm : public USpringArmComponent
{
    GENERATED_BODY()
public:
    UMAYAStableCameraArm();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    void SetViewAngles(float Pitch, float YawOffset);
    void ResetFollow() { bFollowInitialized = false; }
    FString ReadEvidenceJson() const;

protected:
    virtual FVector BlendLocations(const FVector& DesiredArmLocation,
        const FVector& TraceHitLocation, bool bHitSomething, float DeltaTime) override;

private:
    FVector FollowPosition = FVector::ZeroVector;
    FVector LastTarget = FVector::ZeroVector;
    float FollowYaw = 0;
    float ViewPitch = -20;
    float ViewYawOffset = 0;
    float CollisionArmLength = 250;
    double SampleWorldSeconds = -1;
    uint64 FollowSamples = 0;
    bool bFollowInitialized = false;
};
