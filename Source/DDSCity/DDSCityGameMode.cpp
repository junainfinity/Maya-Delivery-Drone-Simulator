#include "DDSCityGameMode.h"
#include "DDSCityRollingPawn.h"

ADDSCityGameMode::ADDSCityGameMode()
{
    DefaultPawnClass = ADDSCityRollingPawn::StaticClass();
}

void ADDSCityGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
    // Use authored PlayerStart and UE lifecycle. No historical course/probe,
    // generated walls or guessed spawn height enter the normal assembly.
    // Actual CitySample import must retain its required world/game-mode setup;
    // this DDSCity mode is not a replacement for the City's living systems.
    Super::HandleStartingNewPlayer_Implementation(NewPlayer);
}
