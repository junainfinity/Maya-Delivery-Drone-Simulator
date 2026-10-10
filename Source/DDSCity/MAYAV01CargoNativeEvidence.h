#pragma once

#include "CoreMinimal.h"

class AMAYADeliveryParcel;
class UPrimitiveComponent;

// Read-only GAME-thread/native external diagnostics. No physics or custody mutation.
DDSCITY_API FString MAYAV01CaptureCargoPhysicsJson(AMAYADeliveryParcel* Parcel, UPrimitiveComponent* Carrier, const TCHAR* SampleLabel);
