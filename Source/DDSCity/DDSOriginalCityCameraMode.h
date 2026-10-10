#pragma once

#include "Camera/CitySampleCameraMode.h"
#include "DDSOriginalCityCameraMode.generated.h"

// Installed explicitly through the existing original City camera manager.
UCLASS()
class DDSCITY_API UDDSOriginalCityCameraMode : public UCitySampleCameraMode
{
    GENERATED_BODY()
public:
    UDDSOriginalCityCameraMode();
    virtual void UpdateCamera(AActor* ViewTarget, UCineCameraComponent* CineCamComp,
        float DeltaTime, FTViewTarget& OutVT) override;
};
