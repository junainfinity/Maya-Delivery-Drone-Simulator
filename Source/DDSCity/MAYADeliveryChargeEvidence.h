#pragma once

#include "MAYADeliveryTypes.h"

class FJsonObject;

// Read-only serialization of the existing charge diagnostic receipt. This
// function reads no world/input, changes no state, and decides no acceptance.
DDSCITY_API TSharedRef<FJsonObject> MAYADeliveryChargeDisconnectToJson(const FMAYADeliveryChargeDisconnect& Receipt);
