#include "MAYACityBlockRouteContent.h"
#include "MAYADeliveryHUDGeometry.h"

namespace
{
    // Native acquisition receipts: graph115 6ECE109FC3D37C54B1FF8A0296E96A4C71B4B8C7230988FAD47E14B464337AAB
    // Targeted physical profile116 194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94
    // The optional offsets change waypoint validation only. Native graph
    // geometry, tags, widths and exact outgoing links remain unchanged.
    void AddSegment(FMAYACityBlockRouteDefinition& D, const FVector& Target, int32 LaneIndex)
    {
        const FVector Start = D.Points.Last();
        const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(Start, Target) / 50.0));
        for (int32 I = 1; I <= Steps; ++I)
        {
            D.Points.Add(FMath::Lerp(Start, Target, double(I) / Steps));
            D.PointLaneIndices.Add(LaneIndex);
        }
    }
}

FVector MAYACityBlockRouteContent::ServicePoint()
{
    return FVector(-23050, 8173.8000000000002, 73);
}

FTransform MAYACityBlockRouteContent::StationPose()
{
    // South90cm of parking: original measured north edge reaches the wall.
    return FTransform(FRotator::ZeroRotator, FVector(-23050, 8083.8000000000002, 73));
}

bool MAYACityBlockRouteContent::Build(const TArray<FVector>& LegacyPrefix,
    FMAYACityBlockRouteDefinition& OutDefinition, FString& OutReason)
{
    OutDefinition = FMAYACityBlockRouteDefinition();
    OutReason = TEXT("City block is unavailable at this depot.");
    const FVector FrozenStart = FVector(-29898.724609375, -100.74424740000001, 72.999996185302734);
    if (LegacyPrefix.Num() != 61) return false;
    for (int32 I = 0; I < LegacyPrefix.Num(); ++I)
    {
        const FVector Expected = FrozenStart + FVector(I * 50.0, 0, 0);
        if (!MAYAHUDGeometry::Finite(LegacyPrefix[I]) || !LegacyPrefix[I].Equals(Expected, .01)) return false;
    }
    FMAYACityBlockRouteDefinition D;
    D.RouteId = TEXT("maya_city_block_116_121_127_141_crest_maneuvers_v18");
    D.SurveyReceiptSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
    D.EastRampSurfaceAuditSha256 = TEXT("E76CF442C718A13A474F1A4DF2E5F990751913F4CC83CF234F7FE3E3E18D7DAE");
    D.ApproachSurfaceAuditSha256 = TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86");
    D.CrestSurfaceAuditSha256 = TEXT("B58D0006D12E6C2C00D000E0A02B3BFEE56A1398D8C9E863F9AC5A84C8F3D3B0");
    D.GraphActorName = FName(TEXT("ZoneGraphData_UAID_5C879C879000EEEA00_1280800823"));
    D.ExpectedGraphLaneCount = 12279;
    D.ExpectedGraphZoneCount = 2457;
    D.DeadlineSeconds = 600; // Conservative initial tuning; actual motion calibration remains separate.
    D.Points = LegacyPrefix; // Preserve every current measured legacy point exactly.
    D.PointLaneIndices.Init(INDEX_NONE, LegacyPrefix.Num());
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 2841; L.ZoneIndex = 1242; L.Tags = 2376; L.WidthCm = 100;
        L.AuditedMaxWaypointOffsetCm = 100;
        L.PhysicalOffsetAuditSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
        L.Points = { FVector(-38405.5390625, -34.146648406982422, 73), FVector(-26840.802734375, -29.300411224365234, 73) };
        L.NextLaneIndex = 11355; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 11355; L.ZoneIndex = 2296; L.Tags = 2392; L.WidthCm = 100;
        L.AuditedMaxWaypointOffsetCm = 100;
        L.PhysicalOffsetAuditSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
        L.Points = { FVector(-26840.802734375, -29.300411224365234, 73), FVector(-26608.73046875, -8.3629512786865234, 75.266693115234375) };
        L.NextLaneIndex = 2836; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 2836; L.ZoneIndex = 1240; L.Tags = 2888; L.WidthCm = 100;
        L.AuditedMaxWaypointOffsetCm = 100;
        L.PhysicalOffsetAuditSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
        L.EastRampAuditedMaxWaypointOffsetCm = 200;
        L.StagedApproachAuditedMaxWaypointOffsetCm = 250;
        L.Points = { FVector(-26608.73046875, -8.3629512786865234, 75.266693115234375), FVector(-24285.724609375, -5.7198352813720703, 77.256950378417969) };
        L.NextLaneIndex = 7422; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 7422; L.ZoneIndex = 1870; L.Tags = 2392; L.WidthCm = 100;
        L.EastRampAuditedMaxWaypointOffsetCm = 200;
        L.StagedApproachAuditedMaxWaypointOffsetCm = 250;
        L.Points = { FVector(-24285.724609375, -5.7198352813720703, 77.256950378417969), FVector(-24269.078125, 68.910385131835938, 73) };
        L.NextLaneIndex = 1214; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 1214; L.ZoneIndex = 429; L.Tags = 2376; L.WidthCm = 100;
        L.Points = { FVector(-24269.078125, 68.910385131835938, 73), FVector(-24302.396484375, 6335.5439453125, 73) };
        L.NextLaneIndex = 7411; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 7411; L.ZoneIndex = 1869; L.Tags = 2392; L.WidthCm = 100;
        L.Points = { FVector(-24302.396484375, 6335.5439453125, 73), FVector(-24277.736328125, 6577.21923828125, 75.885696411132812) };
        L.NextLaneIndex = 2071; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 2071; L.ZoneIndex = 857; L.Tags = 2888; L.WidthCm = 100;
        L.NorthRampAuditedMaxWaypointOffsetCm = 100;
        L.CrestAuditedMaxWaypointOffsetCm = 180;
        L.Points = { FVector(-24277.736328125, 6577.21923828125, 75.885696411132812), FVector(-24257.05859375, 8054.357421875, 75.418022155761719) };
        L.NextLaneIndex = 9793; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 9793; L.ZoneIndex = 2114; L.Tags = 2392; L.WidthCm = 100;
        L.CrestAuditedMaxWaypointOffsetCm = 160;
        L.Points = { FVector(-24257.05859375, 8054.357421875, 75.418022155761719), FVector(-24234.84765625, 8107.25390625, 76.204269409179688), FVector(-24181.005859375, 8144.51513671875, 75.455795288085938), FVector(-24044.943359375, 8173.81884765625, 73) };
        L.NextLaneIndex = 2077; L.NextLinkType = 1; L.NextLinkFlags = 0;
    }
    {
        auto& L = D.Lanes.AddDefaulted_GetRef();
        L.LaneIndex = 2077; L.ZoneIndex = 860; L.Tags = 2376; L.WidthCm = 100;
        L.Points = { FVector(-24044.943359375, 8173.81884765625, 73), FVector(-13637.0419921875, 8173.87255859375, 73) };
    }
    double ArcCm = 0;
    for (int32 I = 1; I < D.Points.Num(); ++I) ArcCm += FVector::Dist2D(D.Points[I - 1], D.Points[I]);
    {
        const FVector Target = FVector(-26840.802734375, -108.363, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2841);
    }
    {
        const FVector Target = FVector(-26800, -108.363, 73.398527919057756);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 11355);
    }
    const double AwaitingbeforeArcCm = ArcCm;
    {
        const FVector Target = FVector(-26608.73046875, -108.36295127868652, 75.266693115234375);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 11355);
    }
    const double AcrossingentryArcCm = ArcCm;
    {
        const FVector Target = FVector(-26525, -108.2676827538472, 75.338429987057978);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    {
        const FVector Target = FVector(-26325, -8.3629999999999995, 75.509781880567104);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    {
        const FVector Target = FVector(-24850, -30.719999999999999, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    const int32 AstopP0PointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24550, -230.72, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    const int32 AstopP1PointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24250, -80.719999999999999, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    const int32 AstopP2PointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24259.562625757724, -5.7199999999999989, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2836);
    }
    const double AcrossingexitArcCm = ArcCm;
    {
        const FVector Target = FVector(-24269.078125, 68.910385131835938, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 7422);
    }
    const int32 AwaitingafterPointIndex = D.Points.Num()-1;
    const double AwaitingafterArcCm = ArcCm;
    {
        const FVector Target = FVector(-24302.396484375, 6335.5439453125, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 1214);
    }
    const double BwaitingbeforeArcCm = ArcCm;
    {
        const FVector Target = FVector(-24277.736328125, 6577.21923828125, 75.885696411132812);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 7411);
    }
    const double BcrossingentryArcCm = ArcCm;
    {
        // Inside native halfwidth before the exact116 north-ramp rectangle.
        const FVector Target = FVector(-24309.669107078818, 7725, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    const int32 BstopApproachPointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24357.059000000001, 7825, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    const int32 BstopQ0PointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24357.059000000001, 8054.357421875, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    const double BcrossingexitArcCm = ArcCm;
    {
        const FVector Target = FVector(-24357.059000000001, 8055, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    {
        // Stitch the preserved116 bridge to the exact141 search rectangle atY8075.
        const FVector Target = FVector(-24357.059000000001, 8075, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    {
        // Complete the straight climb before turning on the sampled north apron.
        const FVector Target = FVector(-24357.059000000001, 8200, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2071);
    }
    const int32 BstopQ1PointIndex = D.Points.Num()-1;
    {
        // Actual141 east-oriented proxy rows support this prospective level leg.
        // The allowance is reference/context, never a claim of native lane membership.
        const FVector Target = FVector(-24157.059000000001, 8200, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 9793);
    }
    const int32 BQ1outgoingPointIndex = D.Points.Num()-1;
    {
        const FVector Target = FVector(-24044.943359375, 8173.81884765625, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 9793);
    }
    const int32 BwaitingafterPointIndex = D.Points.Num()-1;
    const double BwaitingafterArcCm = ArcCm;
    {
        const FVector Target = FVector(-23050, 8173.8000000000002, 73);
        ArcCm += FVector::Dist2D(D.Points.Last(), Target);
        AddSegment(D, Target, 2077);
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = AstopP0PointIndex; Stop.OutgoingEndpointPointIndex = AstopP1PointIndex;
        Stop.ArrivalRadiusCm = 30; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86");
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = AstopP1PointIndex; Stop.OutgoingEndpointPointIndex = AstopP2PointIndex;
        Stop.ArrivalRadiusCm = 30; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86");
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = AstopP2PointIndex; Stop.OutgoingEndpointPointIndex = AwaitingafterPointIndex;
        Stop.ArrivalRadiusCm = 60; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("ED4FA03366D9EB1BAB2E80FB9638775E5AFD6AD844943FFAE2F9C7C8ED7E0D86");
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = BstopApproachPointIndex; Stop.OutgoingEndpointPointIndex = BstopQ0PointIndex;
        Stop.ArrivalRadiusCm = 30; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = BstopQ0PointIndex; Stop.OutgoingEndpointPointIndex = BstopQ1PointIndex;
        Stop.ArrivalRadiusCm = 30; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("194ED7303636862FF3A8FFE3F657E0228E6F529B4ED772B95C838D1A5D888C94");
    }
    {
        auto& Stop = D.StopTurns.AddDefaulted_GetRef();
        Stop.PointIndex = BstopQ1PointIndex; Stop.OutgoingEndpointPointIndex = BQ1outgoingPointIndex;
        Stop.ArrivalRadiusCm = 60; Stop.MaximumCentreExcursionCm = 150;
        Stop.SurfaceAuditSha256 = TEXT("B58D0006D12E6C2C00D000E0A02B3BFEE56A1398D8C9E863F9AC5A84C8F3D3B0");
    }
    {
        auto& C = D.Crossings.AddDefaulted_GetRef();
        C.CrosswalkLaneIndex = 2836; C.IntersectionZoneIndex = 1663;
        C.WaitingLaneBefore = 11355; C.ExitConnectorLaneIndex = 7422;
        C.WaitingBeforeArcCm = AwaitingbeforeArcCm; C.EntryArcCm = AcrossingentryArcCm;
        C.ExitArcCm = AcrossingexitArcCm; C.WaitingAfterArcCm = AwaitingafterArcCm;
    }
    {
        auto& C = D.Crossings.AddDefaulted_GetRef();
        C.CrosswalkLaneIndex = 2071; C.IntersectionZoneIndex = 1629;
        C.WaitingLaneBefore = 7411; C.ExitConnectorLaneIndex = 9793;
        C.WaitingBeforeArcCm = BwaitingbeforeArcCm; C.EntryArcCm = BcrossingentryArcCm;
        C.ExitArcCm = BcrossingexitArcCm; C.WaitingAfterArcCm = BwaitingafterArcCm;
    }
    MAYAHUDGeometry::FPolyline Path;
    if (D.Points.Num() > 512 || !Path.Build(D.Points) || Path.Points.Num() != D.Points.Num()) return false;
    OutDefinition = MoveTemp(D);
    OutReason.Reset();
    return true;
}
