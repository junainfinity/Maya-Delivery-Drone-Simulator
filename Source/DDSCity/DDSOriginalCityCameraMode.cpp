#include "DDSOriginalCityCameraMode.h"
#include "DDSCityRollingPawn.h"

UDDSOriginalCityCameraMode::UDDSOriginalCityCameraMode()
{
    bUseCineCam = false;
    bUseCineCamSettings = false;
    TransitionInTime = 0.0f; // Pawn CalcCamera alone owns the existing C transition.
}

void UDDSOriginalCityCameraMode::UpdateCamera(AActor* ViewTarget, UCineCameraComponent* CineCamComp,
    float DeltaTime, FTViewTarget& OutVT)
{
    Super::UpdateCamera(ViewTarget, CineCamComp, DeltaTime, OutVT);
    if (ADDSCityRollingPawn* Pawn = Cast<ADDSCityRollingPawn>(ViewTarget))
    {
        if (Pawn->HasOriginalCityPresentation()) Pawn->CalcCamera(DeltaTime, OutVT.POV);
    }
}
