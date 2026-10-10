#include "DDSN10StarBridge.h"

#include "DDSCityRollingPawn.h"

FString DDSN10StarBridge::MakeScopedStarId(const FString& MissionInstanceId,
    const FString& StableSlotId)
{
    if (MissionInstanceId.IsEmpty() || StableSlotId.TrimStartAndEnd().IsEmpty()
        || StableSlotId.Contains(TEXT(":"))) return FString();
    // Length prefix makes the scope unambiguous even if an instance ID itself
    // contains punctuation. A different Replay instance yields different IDs.
    return FString::Printf(TEXT("%d:%s:%s"),MissionInstanceId.Len(),
        *MissionInstanceId,*StableSlotId);
}

bool DDSN10StarBridge::MakeThreeScopedStarIds(const FString& MissionInstanceId,
    const TArray<FString>& ThreeStableSlots, TArray<FString>& OutScopedIds)
{
    OutScopedIds.Reset();
    if (ThreeStableSlots.Num() != 3) return false;
    for (int32 Index=0; Index<ThreeStableSlots.Num(); ++Index)
    {
        for (int32 Earlier=0; Earlier<Index; ++Earlier)
        {
            if (ThreeStableSlots[Index].Equals(ThreeStableSlots[Earlier],
                ESearchCase::IgnoreCase)) { OutScopedIds.Reset(); return false; }
        }
        const FString Id = MakeScopedStarId(MissionInstanceId,ThreeStableSlots[Index]);
        if (Id.IsEmpty()) { OutScopedIds.Reset(); return false; }
        OutScopedIds.Add(Id);
    }
    OutScopedIds.Sort();
    return true;
}

bool DDSN10StarBridge::TryRecordValidatedContact(FDDSN08Authority& Authority,
    const FString& ContactInstanceId, const FString& ScopedStarId,
    const ADDSCityRollingPawn* ContactPawn,
    const ADDSCityRollingPawn* CurrentOwnedPawn)
{
    if (!ContactPawn || ContactPawn != CurrentOwnedPawn
        || ContactInstanceId.IsEmpty() || ScopedStarId.IsEmpty()) return false;
    if (Authority.GetStableSnapshot().MissionInstanceId != ContactInstanceId)
        return false;
    return Authority.RecordN10Star(ContactInstanceId,ScopedStarId);
}

FDDSN10ResultView DDSN10StarBridge::ProjectSavedResult(
    const FDDSN08StableSnapshot& State, const FDDSN08Presentation& Presentation)
{
    FDDSN10ResultView View;
    if (State.Phase != EDDSN08Phase::Completed || !State.bHasFrozenResult
        || !Presentation.bShowDelivered || State.MissionInstanceId.IsEmpty()
        || State.FrozenResult.Count != State.FrozenResult.ExactStarIds.Num()
        || State.FrozenResult.ExactStarIds != State.CollectedStarIds
        || State.FrozenResult.Count < 0 || State.FrozenResult.Count > 3
        || !FMath::IsFinite(State.FrozenResult.FrozenElapsedSeconds)
        || State.FrozenResult.FrozenElapsedSeconds != State.ElapsedGameplaySeconds) return View;
    View.bSavedResultAvailable = true;
    View.MissionInstanceId = State.MissionInstanceId;
    View.ExactStarIds = State.FrozenResult.ExactStarIds;
    View.Count = State.FrozenResult.Count;
    View.FrozenElapsedSeconds = State.FrozenResult.FrozenElapsedSeconds;
    return View;
}
