#pragma once

#include "CoreMinimal.h"
#include "DDSN08Authority.h"

class ADDSCityRollingPawn;

struct FDDSN10ResultView
{
    bool bSavedResultAvailable = false;
    FString MissionInstanceId;
    TArray<FString> ExactStarIds;
    int32 Count = 0;
    double FrozenElapsedSeconds = 0.0;
};

namespace DDSN10StarBridge
{
    // Stable authored slot plus mission instance creates the three new IDs
    // needed by N08.StartFreshMission/ReplayExplicit. No local ledger exists.
    FString MakeScopedStarId(const FString& MissionInstanceId,
                             const FString& StableSlotId);
    bool MakeThreeScopedStarIds(const FString& MissionInstanceId,
                                const TArray<FString>& ThreeStableSlots,
                                TArray<FString>& OutScopedIds);

    // The actor validated owned overlap/ground/path before this call. This
    // second owner+instance check rejects delayed replay contacts; N08 owns
    // phase, save gate, duplicate ID and exact collection state.
    bool TryRecordValidatedContact(FDDSN08Authority& Authority,
        const FString& ContactInstanceId, const FString& ScopedStarId,
        const ADDSCityRollingPawn* ContactPawn,
        const ADDSCityRollingPawn* CurrentOwnedPawn);

    // Read-only result for N15. Dismiss/reopen simply reprojects N08 state;
    // N11 restores the validated snapshot and N08 verifies it.
    FDDSN10ResultView ProjectSavedResult(const FDDSN08StableSnapshot& State,
                                          const FDDSN08Presentation& Presentation);
}
