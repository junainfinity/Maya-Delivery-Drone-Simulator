#pragma once
#include "CoreMinimal.h"
#include "MAYADeliveryTypes.h"

class FJsonObject;
class FJsonValue;

// Read-only saved receipts of the native event and the actual applied GAME delta.
TSharedRef<FJsonObject> MAYADeliveryImpactEventToJson(const FMAYADeliveryImpactEvent& Event);
TSharedRef<FJsonObject> MAYADeliveryImpactReceiptToJson(const FMAYADeliveryImpactReceipt& Receipt);
TArray<TSharedPtr<FJsonValue>> MAYADeliveryImpactHistoryToJson(const TArray<FMAYADeliveryImpactReceipt>& History);
