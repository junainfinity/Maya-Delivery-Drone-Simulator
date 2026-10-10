#pragma once
#include "CoreMinimal.h"
class UStaticMeshComponent;

// Read-only, explicit diagnostics. No geometry, collision, body or asset mutation.
FString MAYAReadWheelAppearanceEvidence(UStaticMeshComponent* NativeWheel, UStaticMeshComponent* Cosmetic);
