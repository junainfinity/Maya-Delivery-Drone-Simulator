#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DDSCityGameMode.generated.h"

UCLASS()
class DDSCITY_API ADDSCityGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ADDSCityGameMode();
    virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
};
