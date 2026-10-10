#pragma once

#include "CoreMinimal.h"
#include "MAYACityBlockRoute.h"

// Immutable content selected from original Small City graph115 and physical
// profile116. These receipts establish acquisition, not loaded traversal.
namespace MAYACityBlockRouteContent
{
    bool Build(const TArray<FVector>& LegacyPrefix, FMAYACityBlockRouteDefinition& OutDefinition, FString& OutReason);
    FVector ServicePoint();
    FTransform StationPose();
}
