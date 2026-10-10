#include "MAYACityBlockRoute.h"
#include "Engine/World.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneGraphData.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassEntityQuery.h"
#include "MassExecutionContext.h"
#include "MassTrafficFragments.h"
#include "MassTrafficSettings.h"
#include "MassCrowdSubsystem.h"
#include "MassCrowdSettings.h"
#include "Misc/ScopeLock.h"

FMAYACityBlockRouteObservation MAYACityBlockRoute::Read(UWorld* World, const FMAYACityBlockRouteDefinition& D)
{
    FMAYACityBlockRouteObservation O;
    O.WorldSeconds = World ? World->GetTimeSeconds() : -1;
    O.Crossings.SetNum(D.Crossings.Num());
    O.Reason = TEXT("City-block route data is unavailable.");
    if (!World || D.GraphActorName.IsNone() || D.Lanes.IsEmpty() || D.Lanes.Num() > 64
        || D.Crossings.Num() != 2 || D.ExpectedGraphLaneCount <= 0 || D.ExpectedGraphZoneCount <= 0) return O;
    auto* Graph = World->GetSubsystem<UZoneGraphSubsystem>();
    auto* Crowd = World->GetSubsystem<UMassCrowdSubsystem>();
    auto* Mass = World->GetSubsystem<UMassEntitySubsystem>();
    const auto* TrafficSettings = GetDefault<UMassTrafficSettings>();
    const auto* CrowdSettings = GetDefault<UMassCrowdSettings>();
    if (!Graph || !Crowd || !Mass || !TrafficSettings || !CrowdSettings) return O;
    const auto& Filter = TrafficSettings->CrosswalkLaneFilter;
    if ((Filter.AnyTags.GetValue() | Filter.AllTags.GetValue() | Filter.NotTags.GetValue()) == 0) return O;
    FZoneGraphDataHandle CurrentHandle;
    bool Bound = false;
    int32 RegisteredIndex = -1;
    for (const auto& Registered : Graph->GetRegisteredZoneGraphData())
    {
        ++RegisteredIndex;
        auto* Actor = Registered.ZoneGraphData.Get();
        if (!Registered.bInUse || !IsValid(Actor) || !Actor->IsRegistered() || Actor->GetFName() != D.GraphActorName) continue;
        FScopeLock Lock(&Actor->GetStorageLock());
        const auto& S = Actor->GetStorage();
        if (!S.DataHandle.IsValid() || S.DataHandle.Index != RegisteredIndex
            || int32(S.DataHandle.Generation) != Registered.Generation
            || S.Lanes.Num() != D.ExpectedGraphLaneCount || S.Zones.Num() != D.ExpectedGraphZoneCount) return O;
        int32 PointCount = 0;
        TSet<int32> Seen;
        for (int32 I = 0; I < D.Lanes.Num(); ++I)
        {
            const auto& Expected = D.Lanes[I];
            if (!S.Lanes.IsValidIndex(Expected.LaneIndex) || Seen.Contains(Expected.LaneIndex)) return O;
            Seen.Add(Expected.LaneIndex);
            const auto& Actual = S.Lanes[Expected.LaneIndex];
            if (Actual.ZoneIndex != Expected.ZoneIndex || !S.Zones.IsValidIndex(Actual.ZoneIndex)
                || Actual.Tags.GetValue() != Expected.Tags || !Actual.Tags.Contains(CrowdSettings->CrowdTag)
                || !FMath::IsFinite(Expected.WidthCm) || Expected.WidthCm <= 0
                || FMath::Abs(Actual.Width - Expected.WidthCm) > .01
                || Actual.PointsBegin < 0 || Actual.PointsEnd > S.LanePoints.Num()
                || Actual.PointsEnd - Actual.PointsBegin != Expected.Points.Num() || Expected.Points.Num() < 2) return O;
            PointCount += Expected.Points.Num();
            if (PointCount > 4096) return O;
            for (int32 P = 0; P < Expected.Points.Num(); ++P)
                if (Expected.Points[P].ContainsNaN() || !S.LanePoints[Actual.PointsBegin + P].Equals(Expected.Points[P], .01)) return O;
            if (I + 1 < D.Lanes.Num())
            {
                if (Expected.NextLaneIndex != D.Lanes[I + 1].LaneIndex
                    || Actual.LinksBegin < 0 || Actual.LinksEnd > S.LaneLinks.Num() || Actual.LinksEnd < Actual.LinksBegin) return O;
                bool Linked = false;
                for (int32 L = Actual.LinksBegin; L < Actual.LinksEnd; ++L)
                {
                    const auto& Link = S.LaneLinks[L];
                    Linked |= Link.DestLaneIndex == Expected.NextLaneIndex
                        && uint8(Link.Type) == Expected.NextLinkType && Link.Flags == Expected.NextLinkFlags
                        && (Link.Type == EZoneLaneLinkType::Outgoing || Link.Type == EZoneLaneLinkType::Incoming);
                }
                if (!Linked) return O; // Proximity and adjacent lanes do not establish end connectivity.
            }
            else if (Expected.NextLaneIndex != INDEX_NONE) return O;
        }
        for (const auto& C : D.Crossings)
        {
            if (!S.Lanes.IsValidIndex(C.CrosswalkLaneIndex) || !S.Lanes.IsValidIndex(C.WaitingLaneBefore)
                || !S.Lanes.IsValidIndex(C.ExitConnectorLaneIndex) || !S.Zones.IsValidIndex(C.IntersectionZoneIndex)
                || !Seen.Contains(C.CrosswalkLaneIndex) || !Seen.Contains(C.WaitingLaneBefore) || !Seen.Contains(C.ExitConnectorLaneIndex)
                || !Filter.Pass(S.Lanes[C.CrosswalkLaneIndex].Tags)) return O;
        }
        CurrentHandle = S.DataHandle; Bound = true; break;
    }
    if (!Bound) return O;
    O.bBound = true; // Static topology binds independently of live signal availability.
    auto& Manager = Mass->GetMutableEntityManager();
    if (Manager.IsProcessing()) { O.Reason = TEXT("City-block signal state is temporarily unavailable."); return O; }
    FMassEntityQuery Query;
    Query.AddRequirement<FMassTrafficIntersectionFragment>(EMassFragmentAccess::ReadOnly);
    FMassExecutionContext Context(Manager, 0.f);
    int32 Matches[2] = {};
    Query.ForEachEntityChunk(Manager, Context, [&](FMassExecutionContext& Chunk)
    {
        const auto Fragments = Chunk.GetFragmentView<FMassTrafficIntersectionFragment>();
        const auto Entities = Chunk.GetEntities();
        for (int32 I = 0; I < Chunk.GetNumEntities(); ++I)
        {
            const auto& F = Fragments[I];
            if (F.ZoneGraphDataHandle != CurrentHandle || !F.bHasTrafficLights || F.TrafficLights.IsEmpty()
                || F.Periods.IsEmpty() || F.Periods.Num() > 32 || !F.Periods.IsValidIndex(F.CurrentPeriodIndex)) continue;
            for (int32 C = 0; C < D.Crossings.Num(); ++C)
            {
                const auto& Crossing = D.Crossings[C];
                if (F.ZoneIndex != Crossing.IntersectionZoneIndex) continue;
                bool Matched = false;
                for (const auto& Period : F.Periods)
                    Matched |= Period.CrosswalkLanes.Contains(Crossing.CrosswalkLaneIndex)
                        && Period.CrosswalkWaitingLanes.Contains(Crossing.WaitingLaneBefore);
                if (!Matched) continue;
                ++Matches[C];
                auto& State = O.Crossings[C];
                State.bMatchedIntersection = true;
                State.EntityIndex = Entities[I].Index; State.EntitySerial = Entities[I].SerialNumber;
                State.CurrentPeriodIndex = F.CurrentPeriodIndex;
                State.PeriodTimeRemainingSeconds = float(F.PeriodTimeRemaining);
                const FZoneGraphLaneHandle Lane(Crossing.CrosswalkLaneIndex, CurrentHandle);
                if (Crowd->HasCrowdDataForZoneGraph(CurrentHandle))
                {
                    const auto* Data = Crowd->GetCrowdData(CurrentHandle);
                    State.bStateKnown = Data && Data->CrowdLaneDataArray.IsValidIndex(Lane.Index)
                        && Crowd->GetCrowdLaneData(Lane).IsSet();
                    if (State.bStateKnown) State.bOpened = Crowd->GetLaneState(Lane) == ECrowdLaneState::Opened;
                }
            }
        }
    });
    bool SignalsMatched = true;
    for (int32 C = 0; C < 2; ++C)
        if (Matches[C] != 1) { O.Crossings[C] = FMAYACityBlockCrossingObservation(); SignalsMatched = false; }
    O.Reason = SignalsMatched ? TEXT("Current graph and two signalized crosswalks match the frozen audited route.")
        : TEXT("Route graph matches; current crossing signals are unavailable.");
    return O;
}
