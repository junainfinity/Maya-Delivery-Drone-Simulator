#pragma once
#include "MAYADeliveryTypes.h"
#include "Templates/SharedPointer.h"
class FJsonObject;
DDSCITY_API TSharedRef<FJsonObject> MAYADeliveryEconomyToJson(const FMAYADeliverySnapshot& State);
