#pragma once

#include "CoreMinimal.h"

class UWorld;

// Frozen content is supplied only after the native survey is independently
// audited. These are stable identities/snapshots, never runtime graph handles
// or a caller-supplied boolean claiming that a path is physically traversable.
struct FMAYACityBlockLaneDefinition
{
    int32 LaneIndex = INDEX_NONE, ZoneIndex = INDEX_NONE;
    uint32 Tags = 0;
    double WidthCm = 0;
    // Optional point-to-lane-centre allowance from the exact game116 physical
    // profile. Native Width/Tags/Points remain separately bound unchanged.
    // This changes validation only; it never shifts a lane or generates a path.
    double AuditedMaxWaypointOffsetCm = 0;
    FString PhysicalOffsetAuditSha256;
    // Separate actual121 southern ramp audit. This never changes native width.
    double EastRampAuditedMaxWaypointOffsetCm = 0;
    // Separate actual116 B-north western strip; scoped reference allowance only.
    double NorthRampAuditedMaxWaypointOffsetCm = 0;
    // Independent actual127 approach/apron search area; native lane width remains exact.
    double StagedApproachAuditedMaxWaypointOffsetCm = 0;
    // Independent actual141 B crest/apron query area; native geometry stays exact.
    double CrestAuditedMaxWaypointOffsetCm = 0;
    TArray<FVector> Points;
    int32 NextLaneIndex = INDEX_NONE;
    uint8 NextLinkType = 0, NextLinkFlags = 0;
};

struct FMAYACityBlockCrossingDefinition
{
    int32 CrosswalkLaneIndex = INDEX_NONE, IntersectionZoneIndex = INDEX_NONE;
    // Only WaitingLaneBefore is expected in the native period waiting list.
    // The far-side physical waiting point lies on the actual exit connector;
    // graph connectivity does not imply that it is a native waiting lane.
    int32 WaitingLaneBefore = INDEX_NONE, ExitConnectorLaneIndex = INDEX_NONE;
    double WaitingBeforeArcCm = 0, EntryArcCm = 0, ExitArcCm = 0, WaitingAfterArcCm = 0;
};

// Authored diagnostic stop-turn checkpoints. Ordinary player controls remain manual.
// Excursion includes actual braked arrival residual and subsequent native turn walk.
struct FMAYACityBlockStopTurnDefinition
{
    int32 PointIndex = INDEX_NONE, OutgoingEndpointPointIndex = INDEX_NONE;
    double ArrivalRadiusCm = 30, MaximumCentreExcursionCm = 0;
    FString SurfaceAuditSha256;
};

struct FMAYACityBlockRouteDefinition
{
    FString RouteId, SurveyReceiptSha256;
    FString ApproachSurfaceAuditSha256; // Separate actual127 measured approach and apron.
    FString EastRampSurfaceAuditSha256; // Independent of original west116 provenance.
    FString CrestSurfaceAuditSha256; // Separate actual141 north-side B crest/apron.
    FName GraphActorName;
    int32 ExpectedGraphLaneCount = 0, ExpectedGraphZoneCount = 0;
    TArray<FVector> Points;
    // Parallel with Points; legacy-prefix samples use INDEX_NONE, extension
    // samples name their actual audited lane. This prevents a disconnected
    // waypoint polyline from borrowing unrelated graph connectivity.
    TArray<int32> PointLaneIndices;
    TArray<FMAYACityBlockLaneDefinition> Lanes;
    TArray<FMAYACityBlockCrossingDefinition> Crossings;
    TArray<FMAYACityBlockStopTurnDefinition> StopTurns;
    double DeadlineSeconds = 600; // Initial content tuning, not a measured duration claim.
};

struct FMAYACityBlockCrossingObservation
{
    bool bMatchedIntersection = false, bStateKnown = false, bOpened = false;
    int32 EntityIndex = INDEX_NONE, EntitySerial = 0, CurrentPeriodIndex = INDEX_NONE;
    double PeriodTimeRemainingSeconds = 0;
};

struct FMAYACityBlockRouteObservation
{
    bool bBound = false;
    double WorldSeconds = -1;
    FString Reason;
    TArray<FMAYACityBlockCrossingObservation> Crossings;
};

namespace MAYACityBlockRoute
{
    // Rebind and compare the current loaded graph on every read. No raw graph
    // pointers, Mass fragment views or entity handles escape this function.
    FMAYACityBlockRouteObservation Read(UWorld* World, const FMAYACityBlockRouteDefinition& Definition);
}
