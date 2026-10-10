#pragma once

#include "CoreMinimal.h"
#include "MAYADeliveryTypes.h"

// Presentation-only calculations. No route, input or delivery state is changed.
namespace MAYAHUDGeometry
{
    struct FRect
    {
        float X = 0, Y = 0, W = 0, H = 0;
        bool Contains(const FVector2D& P, float Inset = 0) const
        { return P.X >= X + Inset && P.X <= X + W - Inset && P.Y >= Y + Inset && P.Y <= Y + H - Inset; }
        bool Intersects(const FRect& B) const
        { return X < B.X + B.W && X + W > B.X && Y < B.Y + B.H && Y + H > B.Y; }
    };

    struct FLayout
    {
        float Scale = 1, Width = 1280, Height = 720;
        FRect Identity, Battery, Navigation, Map, Telemetry, Prompt, Controls, Modal;
    };

    inline FLayout Layout(float PixelWidth, float PixelHeight, float UserScale = 1.f)
    {
        FLayout L;
        // Selected Concept C: four edge anchors; larger driving HUD only.
        // Explicit menus and the full map retain their separate original scale.
        L.Scale = FMath::Clamp(FMath::Min(PixelWidth / 1280.f, PixelHeight / 720.f), .8f, 2.f)
            * FMath::Clamp(UserScale, .9f, 1.1f);
        L.Width = PixelWidth / L.Scale; L.Height = PixelHeight / L.Scale;
        const bool Compact = PixelWidth < 1100 || PixelHeight < 600;
        const float Margin = Compact ? 20.f : 24.f;
        const float MissionW = FMath::Min(Compact ? 312.5f : 292.f, L.Width * .34f - Margin);
        const float MissionH = Compact ? 105.f : 104.f;
        const float Top = Compact ? 17.5f : 20.f;
        const float CashW = Compact ? 180.f : 156.f;
        L.Navigation = { Margin, Top, MissionW, MissionH };
        L.Battery = { L.Width - Margin - CashW, Top, CashW, 50 };
        L.Identity = { L.Battery.X, Top + 50, CashW, 50 };
        const float MapW = Compact ? 168.f : 176.f;
        L.Map = { Margin, L.Height - MapW - (Compact ? 32.f : 50.f), MapW, MapW };
        const float StripW = FMath::Min(Compact ? 400.f : 416.f, L.Width * .42f - Margin);
        L.Telemetry = { L.Width - Margin - StripW, L.Height - (Compact ? 20.f : 30.f) - (Compact ? 90.f : 94.f), StripW, Compact ? 90.f : 94.f };
        const float ContextY = Top + MissionH + 16.f;
        L.Modal = { Margin, ContextY, MissionW, FMath::Min(232.f, L.Map.Y - ContextY - 12.f) };
        L.Prompt = { Margin, ContextY, MissionW, FMath::Min(76.f, L.Modal.H) };
        L.Controls = { Margin, L.Height - 26.f, FMath::Max(MapW, 300.f), 18 };
        return L;
    }

    inline bool ClipCircle(const FVector2D& Centre, double Radius, FVector2D& A, FVector2D& B)
    {
        if (Radius <= 0 || !FMath::IsFinite(A.X) || !FMath::IsFinite(A.Y)
            || !FMath::IsFinite(B.X) || !FMath::IsFinite(B.Y)) return false;
        const FVector2D Delta = B - A, Offset = A - Centre;
        const double AA = Delta.SizeSquared(), BB = 2 * FVector2D::DotProduct(Offset, Delta);
        const double CC = Offset.SizeSquared() - Radius * Radius;
        if (AA < 1e-12) return CC <= 0;
        const double Disc = BB * BB - 4 * AA * CC;
        if (Disc < 0) return false;
        const double Root = FMath::Sqrt(Disc);
        const double Start = FMath::Max(0.0, (-BB - Root) / (2 * AA));
        const double End = FMath::Min(1.0, (-BB + Root) / (2 * AA));
        if (End < Start) return false;
        B = A + End * Delta; A += Start * Delta; return true;
    }

    // Authored fictional address grid; spatial input is the actual surveyed route arc.
    inline int32 AddressNumber(double ArcCm, double SideCm)
    { return 27 + 2 * FMath::FloorToInt(FMath::Max(0.0, ArcCm) / 500.0) + (SideCm > 0 ? 1 : 0); }

    inline bool Finite(const FVector& P)
    { return FMath::IsFinite(P.X) && FMath::IsFinite(P.Y) && FMath::IsFinite(P.Z); }
    inline bool Finite(const FVector2D& P)
    { return FMath::IsFinite(P.X) && FMath::IsFinite(P.Y); }

    struct FMapTransform
    {
        FVector2D ScreenCentre = FVector2D::ZeroVector, WorldCentre = FVector2D::ZeroVector;
        double PixelsPerCm = 0;
        FVector2D Project(const FVector& P) const
        { return ScreenCentre + FVector2D(P.X - WorldCentre.X, WorldCentre.Y - P.Y) * PixelsPerCm; }
        FVector2D OpticalHeading(const FTransform& Chassis) const
        {
            const FVector Forward = -Chassis.GetUnitAxis(EAxis::X);
            return FVector2D(Forward.X, -Forward.Y).GetSafeNormal();
        }
    };

    inline FMapTransform MapTransform(const FRect& Inner, const TArray<FVector>& Route, const TArray<FVector>& ServicePoints)
    {
        FMapTransform M; M.ScreenCentre = FVector2D(Inner.X + Inner.W * .5, Inner.Y + Inner.H * .5);
        if (Route.IsEmpty()) return M;
        FVector2D Min(Route[0]), Max = Min;
        auto Extend = [&](const FVector& P)
        {
            if (!Finite(P)) return;
            Min.X = FMath::Min(Min.X, P.X); Min.Y = FMath::Min(Min.Y, P.Y);
            Max.X = FMath::Max(Max.X, P.X); Max.Y = FMath::Max(Max.Y, P.Y);
        };
        for (const FVector& P : Route) Extend(P);
        for (const FVector& P : ServicePoints) Extend(P);
        M.WorldCentre = (Min + Max) * .5;
        M.PixelsPerCm = FMath::Min(Inner.W / FMath::Max(600.0, (Max.X - Min.X) * 1.2),
            Inner.H / FMath::Max(600.0, (Max.Y - Min.Y) * 1.2));
        return M;
    }

    inline FMapTransform LocalMapTransform(const FRect& Inner, const FVector& Actual)
    {
        FMapTransform M;
        if (!Finite(Actual)) return M;
        constexpr double SpanCm = 4000;
        M.ScreenCentre = FVector2D(Inner.X + Inner.W * .5, Inner.Y + Inner.H * .5);
        M.WorldCentre = FVector2D(Actual.X, Actual.Y); // continuous player-centred north-up view
        M.PixelsPerCm = FMath::Min(Inner.W, Inner.H) / SpanCm;
        return M;
    }


    // Fixed world cells; render motion never changes a measured cell's address.
    enum class ESurroundingSurface : uint8 { Unknown, Road, Sidewalk, Structure };
    constexpr double MinimapSpanCm = 4000, MinimapCellCm = 50;
    inline FIntPoint MinimapCellKey(const FVector2D& World)
    { return FIntPoint(FMath::FloorToInt(World.X / MinimapCellCm), FMath::FloorToInt(World.Y / MinimapCellCm)); }
    inline FVector2D MinimapCellCentre(const FIntPoint& Key)
    { return FVector2D((double(Key.X) + .5) * MinimapCellCm, (double(Key.Y) + .5) * MinimapCellCm); }
    inline bool TakeMinimapTrace(int32& Used, int32& NewCount, int32& RefreshCount, bool bRefresh)
    {
        if (Used >= 24) return false;
        ++Used; if (bRefresh) ++RefreshCount; else ++NewCount;
        return true;
    }
    inline ESurroundingSurface ClassifyMinimapSurface(const FString& MeshPath, bool bStatic,
        double HeightAbovePlayerCm, double NormalZ)
    {
        if (!bStatic || !FMath::IsFinite(HeightAbovePlayerCm) || !FMath::IsFinite(NormalZ))
            return ESurroundingSurface::Unknown;
        // Installed City Sample families confirmed by native127/141. Heights do
        // not turn arbitrary ground, vehicles or props into an invented road.
        if (MeshPath.StartsWith(TEXT("/Game/Road/Kit_City_Road/SM_ROAD_")))
            return ESurroundingSurface::Road;
        if (MeshPath.StartsWith(TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Sidewalk_"))
            || MeshPath.StartsWith(TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Curb_"))
            || MeshPath.StartsWith(TEXT("/Game/Megascans/3D_Assets/Modular_Sidewalk_Corner_4m_00/uiswcahdw_"))
            || MeshPath.StartsWith(TEXT("/Game/Megascans/3D_Assets/Modular_Sidewalk_Corner_4m_01/uiswackdw_")))
            return ESurroundingSurface::Sidewalk;
        if (MeshPath.StartsWith(TEXT("/Game/Building/"))
            || (!MeshPath.IsEmpty() && HeightAbovePlayerCm > 28))
            return ESurroundingSurface::Structure; // measured elevated geometry, not a building footprint claim
        return ESurroundingSurface::Unknown;
    }

    struct FSurroundingsCell
    {
        FIntPoint WorldKey = FIntPoint::ZeroValue;
        ESurroundingSurface Surface = ESurroundingSurface::Unknown;
        bool bOccupied = false, bHasSource = false, bLastQueryKnown = false;
        double LastAttemptSeconds = -1, LastKnownSeconds = -1, FirstUncertainSeconds = -1;
        double LastQueryWorldZ = 0, LastQueryNormalZ = 0;
        TWeakObjectPtr<UObject> Source;
        FName KnownMesh, KnownComponent, LastQueryMesh;
        ESurroundingSurface EffectiveSurface(double Now) const
        { return FirstUncertainSeconds >= 0 && Now - FirstUncertainSeconds >= 10
            ? ESurroundingSurface::Unknown : Surface; }
        bool IsStale(double Now) const
        { return EffectiveSurface(Now) != ESurroundingSurface::Unknown
            && (!bLastQueryKnown || (bHasSource && !Source.IsValid())); }
        bool RefreshDue(double Now) const
        {
            if (LastAttemptSeconds < 0) return false;
            const bool bUncertain = Surface == ESurroundingSurface::Unknown || !bLastQueryKnown;
            return Now - LastAttemptSeconds >= (bUncertain ? 1.0 : 10.0)
                || (bHasSource && !Source.IsValid() && bLastQueryKnown);
        }
    };

    // Fixed-size tagged slots prevent negative-coordinate/modulo aliases. Only
    // cells outside the moved96x96 window are pruned; overlapping evidence stays.
    class FSurroundingsCache
    {
    public:
        static constexpr int32 Side = 96, Capacity = Side * Side;
        FIntPoint WindowCentre = FIntPoint::ZeroValue;
        TArray<FSurroundingsCell> Slots;
        bool bWindowReady = false;
        bool Contains(const FIntPoint& Key) const
        { return bWindowReady && Key.X >= WindowCentre.X - Side / 2 && Key.X < WindowCentre.X + Side / 2
            && Key.Y >= WindowCentre.Y - Side / 2 && Key.Y < WindowCentre.Y + Side / 2; }
        static int32 SlotIndex(const FIntPoint& Key)
        { return ((Key.Y % Side + Side) % Side) * Side + (Key.X % Side + Side) % Side; }
        void MoveWindow(const FIntPoint& CentreKey)
        {
            if (bWindowReady && CentreKey == WindowCentre) return;
            if (Slots.Num() != Capacity) Slots.SetNum(Capacity);
            WindowCentre = CentreKey; bWindowReady = true;
            for (FSurroundingsCell& Cell : Slots)
                if (Cell.bOccupied && !Contains(Cell.WorldKey)) Cell = FSurroundingsCell();
        }
        const FSurroundingsCell* Find(const FIntPoint& Key) const
        {
            if (!Contains(Key)) return nullptr;
            const FSurroundingsCell& Cell = Slots[SlotIndex(Key)];
            return Cell.bOccupied && Cell.WorldKey == Key ? &Cell : nullptr;
        }
        bool NeverSampled(const FIntPoint& Key) const { return Contains(Key) && !Find(Key); }
        void Observe(const FIntPoint& Key, ESurroundingSurface Measured, double Now,
            UObject* SourceObject = nullptr, FName Mesh = NAME_None, FName Component = NAME_None,
            double WorldZ = 0, double NormalZ = 0)
        {
            if (!Contains(Key) || !FMath::IsFinite(Now)) return;
            FSurroundingsCell& Cell = Slots[SlotIndex(Key)];
            if (!Cell.bOccupied || Cell.WorldKey != Key) Cell = FSurroundingsCell();
            Cell.bOccupied = true; Cell.WorldKey = Key; Cell.LastAttemptSeconds = Now;
            Cell.LastQueryMesh = Mesh; Cell.LastQueryWorldZ = WorldZ; Cell.LastQueryNormalZ = NormalZ;
            Cell.bLastQueryKnown = Measured != ESurroundingSurface::Unknown;
            if (Cell.bLastQueryKnown)
            {
                Cell.Surface = Measured; Cell.LastKnownSeconds = Now; Cell.FirstUncertainSeconds = -1;
                Cell.Source = SourceObject; Cell.bHasSource = SourceObject != nullptr;
                Cell.KnownMesh = Mesh; Cell.KnownComponent = Component;
            }
            else if (Cell.Surface != ESurroundingSurface::Unknown && Cell.FirstUncertainSeconds < 0)
                Cell.FirstUncertainSeconds = Now;
        }
    };

    struct FProjection
    {
        bool bValid = false;
        FVector Point = FVector::ZeroVector;
        double ArcCm = 0, DistanceSquaredCm = 0;
    };

    struct FPolyline
    {
        TArray<FVector> Points;
        TArray<double> Arc;
        bool Build(const TArray<FVector>& Source)
        {
            Points.Reset(); Arc.Reset();
            // A nonfinite sample is a broken survey: never connect across the gap.
            for (const FVector& P : Source) if (!Finite(P)) return false;
            for (const FVector& P : Source)
            {
                if (Points.IsEmpty()) { Points.Add(P); Arc.Add(0); }
                else
                {
                    const double Length = FVector::Dist2D(Points.Last(), P);
                    if (Length > .01) { Points.Add(P); Arc.Add(Arc.Last() + Length); }
                }
            }
            return Points.Num() >= 2;
        }
        FProjection Project(const FVector& P) const
        {
            FProjection Result;
            if (!Finite(P) || Points.Num() < 2) return Result;
            double Best = TNumericLimits<double>::Max();
            for (int32 I = 1; I < Points.Num(); ++I)
            {
                const FVector2D A(Points[I - 1]), B(Points[I]), Query(P), Delta = B - A;
                const double T = FMath::Clamp(FVector2D::DotProduct(Query - A, Delta) / Delta.SizeSquared(), 0.0, 1.0);
                const FVector Q = FMath::Lerp(Points[I - 1], Points[I], T);
                const double Distance = FVector::DistSquared2D(Q, P);
                if (Distance < Best)
                {
                    Best = Distance; Result.bValid = true; Result.Point = Q;
                    Result.DistanceSquaredCm = Distance;
                    Result.ArcCm = FMath::Lerp(Arc[I - 1], Arc[I], T);
                }
            }
            return Result;
        }
        FVector Sample(double DistanceCm) const
        {
            if (Points.Num() < 2) return FVector::ZeroVector;
            const double Distance = FMath::Clamp(DistanceCm, 0.0, Arc.Last());
            for (int32 I = 1; I < Points.Num(); ++I)
                if (Distance <= Arc[I]) return FMath::Lerp(Points[I - 1], Points[I],
                    (Distance - Arc[I - 1]) / (Arc[I] - Arc[I - 1]));
            return Points.Last();
        }
        TArray<FVector> Slice(double Start, double End) const
        {
            TArray<FVector> Result;
            if (Points.Num() < 2) return Result;
            Start = FMath::Clamp(Start, 0.0, Arc.Last()); End = FMath::Clamp(End, 0.0, Arc.Last());
            Result.Add(Sample(Start));
            if (End >= Start)
            {
                for (int32 I = 1; I < Points.Num(); ++I) if (Arc[I] > Start && Arc[I] < End) Result.Add(Points[I]);
            }
            else
            {
                for (int32 I = Points.Num() - 2; I >= 0; --I) if (Arc[I] < Start && Arc[I] > End) Result.Add(Points[I]);
            }
            if (FMath::Abs(End - Start) > .01) Result.Add(Sample(End));
            return Result;
        }
    };

    // Liang-Barsky clipping preserves arrow shape; vertices are never clamped individually.
    inline bool ClipLine(const FRect& R, FVector2D& A, FVector2D& B)
    {
        if (!Finite(A) || !Finite(B)) return false;
        const FVector2D Delta = B - A;
        double First = 0, Last = 1;
        auto Bound = [&](double P, double Q)
        {
            if (FMath::Abs(P) < 1.e-9) return Q >= 0;
            const double T = Q / P;
            if (P < 0) { if (T > Last) return false; First = FMath::Max(First, T); }
            else { if (T < First) return false; Last = FMath::Min(Last, T); }
            return true;
        };
        if (!Bound(-Delta.X, A.X - R.X) || !Bound(Delta.X, R.X + R.W - A.X)
            || !Bound(-Delta.Y, A.Y - R.Y) || !Bound(Delta.Y, R.Y + R.H - A.Y)) return false;
        B = A + Delta * Last; A += Delta * First;
        return true;
    }

    inline bool MovingPhase(EMAYADeliveryPhase Phase)
    { return Phase == EMAYADeliveryPhase::Accepted || Phase == EMAYADeliveryPhase::Carrying; }
    inline bool ObjectivePhase(EMAYADeliveryPhase Phase)
    { return MovingPhase(Phase) || Phase == EMAYADeliveryPhase::Loading || Phase == EMAYADeliveryPhase::Unloading; }
    inline bool NavigationAllowed(const FMAYADeliverySnapshot& S, const FMAYADeliveryRobotObservation& R, bool bPaused)
    {
        return MovingPhase(S.Phase) && R.bKnown && R.bReady && R.Fault.IsEmpty() && !bPaused && !S.bCharging
            && FMath::IsFinite(S.BatteryPercent) && S.BatteryPercent > 0
            && !R.ChassisPose.ContainsNaN() && Finite(R.VelocityCmps);
    }

    struct FGuidance
    {
        FProjection Robot, Goal;
        TArray<FVector> ActivePath;
        bool bRoute = false, bMoving = false, bGuidance = false, bArrival = false, bOffRoute = false;
        double Direction = 1, RemainingCm = 0, DirectM = 0;
        FVector CuePoint = FVector::ZeroVector;
        double CueDistanceCm = 0;
        FString CueText;
    };

    inline void NextRouteCue(const FPolyline& Survey, FGuidance& N)
    {
        if (!N.bRoute) return;
        N.CuePoint = N.bOffRoute ? N.Robot.Point : Survey.Sample(N.Goal.ArcCm);
        N.CueDistanceCm = N.bOffRoute ? FMath::Sqrt(N.Robot.DistanceSquaredCm) : N.RemainingCm;
        N.CueText = N.bOffRoute ? TEXT("Return to the route") : TEXT("Destination");
        if (N.bOffRoute) return;
        double Best = N.RemainingCm;
        for (int32 I = 1; I + 1 < Survey.Points.Num(); ++I)
        {
            const double Ahead = (Survey.Arc[I] - N.Robot.ArcCm) * N.Direction;
            if (Ahead < 90 || Ahead >= Best) continue;
            const FVector In = (Survey.Sample(Survey.Arc[I]) - Survey.Sample(Survey.Arc[I] - 100 * N.Direction)).GetSafeNormal2D();
            const FVector Out = (Survey.Sample(Survey.Arc[I] + 100 * N.Direction) - Survey.Sample(Survey.Arc[I])).GetSafeNormal2D();
            if (In.IsNearlyZero() || Out.IsNearlyZero() || FVector::DotProduct(In, Out) >= FMath::Cos(FMath::DegreesToRadians(25.0))) continue;
            Best = Ahead; N.CuePoint = Survey.Points[I]; N.CueDistanceCm = Ahead;
            N.CueText = FVector::CrossProduct(In, Out).Z > 0 ? TEXT("Turn right") : TEXT("Turn left");
        }
    }

    inline FGuidance Guidance(const FPolyline& Survey, const FMAYADeliverySnapshot& S,
        const FMAYADeliveryRobotObservation& R, bool bPaused, const FVector& ServicePoint, double RingRadiusCm)
    {
        FGuidance N;
        if (!ObjectivePhase(S.Phase) || !R.bKnown || R.ChassisPose.ContainsNaN() || !Finite(ServicePoint)) return N;
        N.DirectM = FVector::Dist2D(ServicePoint, R.ChassisPose.GetLocation()) / 100.0;
        N.bArrival = N.DirectM * 100.0 <= RingRadiusCm;
        N.bGuidance = NavigationAllowed(S, R, bPaused) && !N.bArrival;
        N.Robot = Survey.Project(R.ChassisPose.GetLocation()); N.Goal = Survey.Project(ServicePoint);
        N.bRoute = N.Robot.bValid && N.Goal.bValid && N.Goal.DistanceSquaredCm <= FMath::Square(25.0);
        if (!N.bRoute) { N.bGuidance = false; return N; }
        N.Direction = N.Goal.ArcCm >= N.Robot.ArcCm ? 1.0 : -1.0;
        N.RemainingCm = FMath::Abs(N.Goal.ArcCm - N.Robot.ArcCm);
        N.bOffRoute = N.Robot.DistanceSquaredCm > FMath::Square(120.0);
        if (!S.bCharging) N.ActivePath = Survey.Slice(N.Robot.ArcCm, N.Goal.ArcCm);
        N.bMoving = N.bGuidance && !N.bOffRoute && N.RemainingCm > RingRadiusCm;
        // A disconnected/unmatched objective never enables a direct fallback
        // bearing. Off-route guidance targets the known path, with no world ribbon.
        N.bGuidance = N.bGuidance && N.bRoute;
        NextRouteCue(Survey, N);
        return N;
    }
}
