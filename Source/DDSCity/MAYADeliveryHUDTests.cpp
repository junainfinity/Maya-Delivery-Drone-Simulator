#include "MAYADeliveryHUDGeometry.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDRouteTest, "Maya.Delivery.HUD.Route",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDRouteTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    FPolyline P;
    const TArray<FVector> Survey { FVector(0, 0, 0), FVector(100, 0, 2), FVector(100, 100, 4), FVector(200, 100, 6) };
    TestTrue(TEXT("Bent surveyed polyline admitted"), P.Build(Survey));
    const FProjection Robot = P.Project(FVector(25, 5, 10)), Goal = P.Project(FVector(100, 75, 4));
    TestTrue(TEXT("Robot projection valid"), Robot.bValid);
    TestTrue(TEXT("Robot projection keeps XY route position"), Robot.Point.Equals(FVector(25, 0, .5), .001));
    TestTrue(TEXT("Robot arc measured in XY centimetres"), FMath::IsNearlyEqual(Robot.ArcCm, 25.0, .001));
    TestTrue(TEXT("Goal on second segment has correct arc"), FMath::IsNearlyEqual(Goal.ArcCm, 175.0, .001));
    const TArray<FVector> Forward = P.Slice(Robot.ArcCm, Goal.ArcCm), Reverse = P.Slice(Goal.ArcCm, Robot.ArcCm);
    TestEqual(TEXT("Forward includes the measured corner rather than a shortcut"), Forward.Num(), 3);
    TestTrue(TEXT("Forward corner is exact survey sample"), Forward[1].Equals(Survey[1], .001));
    TestEqual(TEXT("Reverse visits same bounded corner"), Reverse.Num(), 3);
    TestTrue(TEXT("Reverse starts at dropoff projection"), Reverse[0].Equals(Goal.Point, .001));
    TestTrue(TEXT("Reverse ends at pickup projection"), Reverse.Last().Equals(Robot.Point, .001));
    TestTrue(TEXT("Reverse tangent points back along the path"), (P.Sample(150) - P.Sample(175)).Y < 0);
    TestEqual(TEXT("Same segment path has two endpoints"), P.Slice(10, 75).Num(), 2);
    TestEqual(TEXT("Same-position path has one point"), P.Slice(100, 100).Num(), 1);
    const FProjection Beyond = P.Project(FVector(-100, 0, 0));
    TestTrue(TEXT("Beyond-end robot projects to endpoint, preserving its true off-route distance"),
        Beyond.bValid && Beyond.Point.Equals(FVector::ZeroVector, .001) && FMath::IsNearlyEqual(Beyond.DistanceSquaredCm, 10000.0, .001));
    TestTrue(TEXT("Sampling beyond either endpoint cannot overshoot"), P.Sample(-100).Equals(Survey[0]) && P.Sample(999).Equals(Survey.Last()));
    TestTrue(TEXT("Duplicate points safely ignored"), P.Build({ FVector::ZeroVector, FVector::ZeroVector, FVector(50, 0, 0) }));
    TestFalse(TEXT("Only duplicate points do not invent a route"), P.Build({ FVector::ZeroVector, FVector::ZeroVector }));
    TestFalse(TEXT("Missing route rejected"), P.Build({}));
    FVector InvalidSample = FVector::ZeroVector; InvalidSample.X = std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Invalid sample cannot bridge an unverified gap"), P.Build({ FVector::ZeroVector, InvalidSample, FVector(100, 0, 0) }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDClipTest, "Maya.Delivery.HUD.Clipping",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDClipTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    const FRect View { 0, 0, 100, 100 };
    FVector2D A(-50, 50), B(150, 50);
    TestTrue(TEXT("Cross-screen segment clips rather than flattens its vertices"), ClipLine(View, A, B));
    TestTrue(TEXT("Clipped left endpoint"), A.Equals(FVector2D(0, 50), .001));
    TestTrue(TEXT("Clipped right endpoint"), B.Equals(FVector2D(100, 50), .001));
    A = FVector2D(-50, -10); B = FVector2D(150, -10);
    TestFalse(TEXT("Parallel outside segment rejected"), ClipLine(View, A, B));
    A = FVector2D(20, -20); B = FVector2D(120, 80);
    TestTrue(TEXT("Diagonal clipped at actual intersections"), ClipLine(View, A, B));
    TestTrue(TEXT("Diagonal shape retained"), A.Equals(FVector2D(40, 0), .001) && B.Equals(FVector2D(100, 60), .001));
    A = FVector2D(0, 20); A.X = std::numeric_limits<double>::infinity(); B = FVector2D(50, 50);
    TestFalse(TEXT("Infinite projection rejected"), ClipLine(View, A, B));
    TestTrue(TEXT("HUD reserved rectangle overlap detectable"), FRect { 20, 20, 30, 30 }.Intersects(FRect { 40, 40, 10, 10 }));
    TestFalse(TEXT("Separated panels stay independent"), FRect { 20, 20, 30, 30 }.Intersects(FRect { 60, 60, 10, 10 }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDNavigationAndMapTest, "Maya.Delivery.HUD.NavigationAndMap",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDNavigationAndMapTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    FPolyline P; P.Build({ FVector::ZeroVector, FVector(2000, 0, 20) });
    FMAYADeliverySnapshot S; FMAYADeliveryRobotObservation R;
    S.Phase = EMAYADeliveryPhase::Carrying; R.bKnown = R.bReady = true;
    R.ChassisPose.SetLocation(FVector(500, 0, 5));
    FGuidance N = Guidance(P, S, R, false, FVector(1800, 0, 18), 90);
    TestTrue(TEXT("Actual production guidance moves forward to interior goal"), N.bMoving && N.Direction == 1 && N.RemainingCm == 1300);
    N = Guidance(P, S, R, false, FVector::ZeroVector, 90);
    TestTrue(TEXT("Actual production guidance reverses to pickup"), N.bMoving && N.Direction == -1 && N.ActivePath.Last().Equals(FVector::ZeroVector));
    R.ChassisPose.SetLocation(FVector(-100, 0, 0));
    N = Guidance(P, S, R, false, FVector::ZeroVector, 90);
    TestTrue(TEXT("Beyond endpoint retains destination bearing at zero route arc"), N.bGuidance && !N.bMoving && !N.bArrival && N.RemainingCm == 0 && N.DirectM == 1);
    R.ChassisPose.SetLocation(FVector(500, 120, 5));
    N = Guidance(P, S, R, false, FVector(1800, 0, 18), 90);
    TestTrue(TEXT("Exact 120 cm display threshold stays on route"), !N.bOffRoute && N.bMoving);
    R.ChassisPose.SetLocation(FVector(500, 120.01, 5));
    N = Guidance(P, S, R, false, FVector(1800, 0, 18), 90);
    TestTrue(TEXT("Off-route keeps bearing without a traversable connector"), N.bOffRoute && N.bGuidance && !N.bMoving && N.ActivePath[0].Equals(FVector(500, 0, 5)));
    N = Guidance(P, S, R, false, FVector(1800, 25.01, 18), 90);
    TestTrue(TEXT("Unmatched goal suppresses destination bearing and ground route"), !N.bRoute && !N.bGuidance && !N.bMoving && N.ActivePath.IsEmpty());
    R.ChassisPose.SetLocation(FVector(1710, 0, 17.1));
    N = Guidance(P, S, R, false, FVector(1800, 0, 18), 90);
    TestTrue(TEXT("Exact 90 cm ring suppresses all travel guidance"), N.bArrival && !N.bGuidance && !N.bMoving);
    R.bReady = false; R.ChassisPose.SetLocation(FVector(500, 0, 5));
    N = Guidance(P, S, R, false, FVector(1800, 0, 18), 90);
    TestFalse(TEXT("Known but not-ready robot has no bearing"), N.bGuidance); R.bReady = true;
    const FRect Inner { 10, 20, 208, 134 };
    const FMapTransform M = MapTransform(Inner, P.Points, { FVector(2500, 200, 0) });
    const FVector2D Centre = M.Project(FVector(M.WorldCentre.X, M.WorldCentre.Y, 0));
    TestTrue(TEXT("Actual minimap transform puts surveyed bounds at card centre"), Centre.Equals(FVector2D(114, 87), .001));
    const FVector2D Origin = M.Project(FVector::ZeroVector), AlongX = M.Project(FVector(100, 0, 0)), AlongY = M.Project(FVector(0, 100, 0));
    TestTrue(TEXT("Actual minimap preserves equal X/Y lengths with one scale"), FMath::IsNearlyEqual((AlongX - Origin).Size(), (AlongY - Origin).Size(), .001));
    TestTrue(TEXT("Actual minimap Y axis points up"), AlongY.Y < Origin.Y);
    TestTrue(TEXT("Actual service points extend stable minimap bounds"), Inner.Contains(M.Project(FVector(2500, 200, 0))));
    TestTrue(TEXT("Optical forward is chassis minus X, not plus X"), M.OpticalHeading(FTransform::Identity).Equals(FVector2D(-1, 0), .001));
    TestTrue(TEXT("Optical heading rotates consistently on fixed map"), M.OpticalHeading(FTransform(FRotator(0, 90, 0))).Equals(FVector2D(0, 1), .001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDLayoutAndGatesTest, "Maya.Delivery.HUD.LayoutAndGates",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDLayoutAndGatesTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    const FVector2D Sizes[] = { FVector2D(854, 480), FVector2D(1280, 720), FVector2D(1920, 1080), FVector2D(1024, 768), FVector2D(2560, 1080), FVector2D(2560,1440) };
    for (const FVector2D& Size : Sizes)
    for (const float UserScale : { .9f, 1.f, 1.1f })
    {
        const FLayout L = Layout(float(Size.X), float(Size.Y),UserScale);
        const FRect Screen { 0, 0, L.Width, L.Height };
        const FRect Cards[] = { L.Identity, L.Battery, L.Navigation, L.Map, L.Telemetry, L.Prompt, L.Controls };
        for (int32 I = 0; I < UE_ARRAY_COUNT(Cards); ++I)
        {
            TestTrue(TEXT("Critical card fully inside safe bounds"), Screen.Contains(FVector2D(Cards[I].X, Cards[I].Y), 4)
                && Screen.Contains(FVector2D(Cards[I].X + Cards[I].W, Cards[I].Y + Cards[I].H), 4));
            for (int32 J = I + 1; J < UE_ARRAY_COUNT(Cards); ++J) TestFalse(TEXT("Anchored cards never overlap"), Cards[I].Intersects(Cards[J]));
        }
        TestTrue(TEXT("Minimap stays bottom left"), L.Map.X<L.Width*.1f && L.Height-L.Map.Y-L.Map.H>=32.f && L.Controls.Y>=L.Map.Y+L.Map.H);
        const FRect DrivingView {L.Width*.35f,L.Height*.25f,L.Width*.30f,L.Height*.53f};
        for (const FRect& Card : Cards) TestFalse(TEXT("Passive HUD preserves central driving view"),Card.Intersects(DrivingView));
        TestFalse(TEXT("Edge mission context preserves central driving view"),L.Modal.Intersects(DrivingView));
        TestFalse(TEXT("Edge mission context stays above radar"),L.Modal.Intersects(L.Map));
        TestTrue(TEXT("Modal remains safely inside viewport"), Screen.Contains(FVector2D(L.Modal.X, L.Modal.Y), 4)
            && Screen.Contains(FVector2D(L.Modal.X + L.Modal.W, L.Modal.Y + L.Modal.H), 4));
    }
    const auto Small=Layout(854,480),Default=Layout(2560,1440);
    TestTrue(TEXT("Small circular surroundings retain128 visible physical pixels"),FMath::IsNearlyEqual((Small.Map.W-8)*Small.Scale,128.f,.01f));
    TestTrue(TEXT("Default1440 driving reference scales2x without changing map world span"),FMath::IsNearlyEqual(Default.Scale,2.f,.001f));
    FMAYADeliverySnapshot S; FMAYADeliveryRobotObservation R;
    R.bKnown = R.bReady = true; S.Phase = EMAYADeliveryPhase::Carrying;
    TestTrue(TEXT("Ready carrying navigation enabled"), NavigationAllowed(S, R, false));
    TestFalse(TEXT("Pause suppresses moving guidance"), NavigationAllowed(S, R, true));
    S.bCharging = true; TestFalse(TEXT("Charging suppresses moving guidance"), NavigationAllowed(S, R, false)); S.bCharging = false;
    S.BatteryPercent = 0; TestFalse(TEXT("Depleted battery suppresses moving guidance"), NavigationAllowed(S, R, false));
    S.BatteryPercent = std::numeric_limits<double>::quiet_NaN(); TestFalse(TEXT("Invalid charge suppresses moving guidance"), NavigationAllowed(S, R, false)); S.BatteryPercent = 100;
    R.bKnown = false; TestFalse(TEXT("Unknown telemetry suppresses guidance"), NavigationAllowed(S, R, false)); R.bKnown = true;
    R.Fault = TEXT("native fault"); TestFalse(TEXT("Native fault suppresses guidance"), NavigationAllowed(S, R, false)); R.Fault.Empty();
    const EMAYADeliveryPhase Suppressed[] = { EMAYADeliveryPhase::Available, EMAYADeliveryPhase::Loading,
        EMAYADeliveryPhase::Unloading, EMAYADeliveryPhase::Succeeded, EMAYADeliveryPhase::Failed };
    for (const EMAYADeliveryPhase Phase : Suppressed) { S.Phase = Phase; TestFalse(TEXT("Offer/service/terminal phase has no moving arrows"), NavigationAllowed(S, R, false)); }
    S.Phase = EMAYADeliveryPhase::Accepted; TestTrue(TEXT("Accepted pickup navigation enabled"), NavigationAllowed(S, R, false));
    TestFalse(TEXT("Terminal objective cannot fall back to pickup"), ObjectivePhase(EMAYADeliveryPhase::Succeeded) || ObjectivePhase(EMAYADeliveryPhase::Failed));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDRadarTest, "Maya.Delivery.HUD.CircularRadar",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDRadarTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    FVector2D A(-200,0), B(200,0);
    TestTrue(TEXT("Cross-circle route retained"), ClipCircle(FVector2D::ZeroVector,100,A,B));
    TestTrue(TEXT("Route clipped at actual circular boundary"), A.Equals(FVector2D(-100,0),.001) && B.Equals(FVector2D(100,0),.001));
    A=FVector2D(-200,101); B=FVector2D(200,101);
    TestFalse(TEXT("Outside route excluded"),ClipCircle(FVector2D::ZeroVector,100,A,B));
    TestEqual(TEXT("Address changes with actual route arc"),AddressNumber(1000,0),31);
    TestEqual(TEXT("Opposite sidewalk side has distinct address parity"),AddressNumber(1000,1),32);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAHUDSurroundingsTest, "Maya.Delivery.HUD.Surroundings",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMAYAHUDSurroundingsTest::RunTest(const FString&)
{
    using namespace MAYAHUDGeometry;
    const FRect View { 0, 0, 200, 200 };
    for (double Boundary : { -500.0, 0.0, 500.0 })
    {
        const FVector Before(Boundary-.1,-25,70), After(Boundary+.1,-25,70), Landmark(100,100,70);
        const auto A=LocalMapTransform(View,Before), B=LocalMapTransform(View,After);
        TestTrue(TEXT("Player remains exactly centred across old centre jumps"),A.Project(Before).Equals(FVector2D(100,100),.00001) && B.Project(After).Equals(FVector2D(100,100),.00001));
        TestTrue(TEXT("Stationary landmark scrolls continuously, with no500cm snap"),FMath::IsNearlyEqual(B.Project(Landmark).X-A.Project(Landmark).X,-.01,.000001));
        TestTrue(TEXT("North remains up during centre movement"),B.Project(After+FVector(0,100,0)).Y<100);
    }
    TestTrue(TEXT("Negative world cells use floor rather than truncation"),MinimapCellKey(FVector2D(-.01,-50.01))==FIntPoint(-1,-2));
    int32 UsedTraces=0,NewTraces=0,RefreshTraces=0;
    for(int32 I=0;I<18;++I) TestTrue(TEXT("New-cell quota consumes actual bounded trace token"),TakeMinimapTrace(UsedTraces,NewTraces,RefreshTraces,false));
    for(int32 I=0;I<6;++I) TestTrue(TEXT("Retry quota shares the same actual trace budget"),TakeMinimapTrace(UsedTraces,NewTraces,RefreshTraces,true));
    TestFalse(TEXT("A twenty-fifth actual trace is refused"),TakeMinimapTrace(UsedTraces,NewTraces,RefreshTraces,true));
    TestEqual(TEXT("Actual new and retry counters reconcile to24"),NewTraces+RefreshTraces,24);
    FSurroundingsCache Cache; Cache.MoveWindow(FIntPoint(-1,-1));
    const FIntPoint KnownKey(-1,-1);
    Cache.Observe(KnownKey,ESurroundingSurface::Sidewalk,0);
    Cache.MoveWindow(FIntPoint(0,-1));
    TestTrue(TEXT("Crossing world-cell boundary preserves overlapping pavement and its sampling time"),Cache.Find(KnownKey) && Cache.Find(KnownKey)->Surface==ESurroundingSurface::Sidewalk && Cache.Find(KnownKey)->LastAttemptSeconds==0);
    Cache.MoveWindow(FIntPoint(0,-1)); // no offer or UI-size input exists in cache addressing
    TestTrue(TEXT("Same world window cannot erase known cells on offer/redraw"),Cache.Find(KnownKey)!=nullptr);
    const FIntPoint UnknownKey(0,0);
    TestTrue(TEXT("Never-sampled and measured-unknown cells are distinct"),Cache.NeverSampled(UnknownKey));
    Cache.Observe(UnknownKey,ESurroundingSurface::Unknown,0);
    TestTrue(TEXT("Attempted unknown remains queued forretry, not treated as never-sampled"),!Cache.NeverSampled(UnknownKey) && !Cache.Find(UnknownKey)->RefreshDue(.99) && Cache.Find(UnknownKey)->RefreshDue(1));
    TestFalse(TEXT("Known surface is not refreshed before10seconds"),Cache.Find(KnownKey)->RefreshDue(9.99));
    TestTrue(TEXT("Known surface refreshes at10seconds"),Cache.Find(KnownKey)->RefreshDue(10));
    Cache.Observe(KnownKey,ESurroundingSurface::Unknown,10);
    TestTrue(TEXT("Failed streamed refresh retains measured class as explicitly stale"),Cache.Find(KnownKey)->EffectiveSurface(19.99)==ESurroundingSurface::Sidewalk && Cache.Find(KnownKey)->IsStale(19.99));
    TestTrue(TEXT("Unknown refresh retries after1second"),Cache.Find(KnownKey)->RefreshDue(11));
    TestTrue(TEXT("Uncertain retained class expires after10seconds without indefinite stale fill"),Cache.Find(KnownKey)->EffectiveSurface(20)==ESurroundingSurface::Unknown);
    Cache.Observe(KnownKey,ESurroundingSurface::Road,20);
    TestTrue(TEXT("Fresh streaming evidence can replace sidewalk with actual road"),Cache.Find(KnownKey)->EffectiveSurface(20)==ESurroundingSurface::Road && !Cache.Find(KnownKey)->IsStale(20));
    Cache.MoveWindow(FIntPoint(200,200));
    TestTrue(TEXT("Distant discarded key cannot alias a new tagged slot"),!Cache.Find(KnownKey));
    for(int32 Y=152;Y<248;++Y) for(int32 X=152;X<248;++X) Cache.Observe(FIntPoint(X,Y),ESurroundingSurface::Road,30);
    TestEqual(TEXT("Retained cache remains bounded by9216slots"),Cache.Slots.Num(),9216);
    TestTrue(TEXT("Exact city road package is measured road"),ClassifyMinimapSurface(TEXT("/Game/Road/Kit_City_Road/SM_ROAD_19_10_0_27.SM_ROAD_19_10_0_27"),true,-15,1)==ESurroundingSurface::Road);
    TestTrue(TEXT("Known collision sidewalk stays sidewalk instead of asphalt"),ClassifyMinimapSurface(TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Curb_Collision_Box_A.SM_Curb_Collision_Box_A"),true,0,.7)==ESurroundingSurface::Sidewalk);
    TestTrue(TEXT("Native curved sidewalk package is classified by identity"),ClassifyMinimapSurface(TEXT("/Game/Megascans/3D_Assets/Modular_Sidewalk_Corner_4m_01/uiswackdw_LOD0.uiswackdw_LOD0"),true,0,1)==ESurroundingSurface::Sidewalk);
    TestTrue(TEXT("Arbitrary ground does not become an invented street"),ClassifyMinimapSurface(TEXT("/Game/City/Small_City/GEOMETRY/SM_arbitrary_ground_collision_109"),true,0,1)==ESurroundingSurface::Unknown);
    TestTrue(TEXT("Nonstatic vehicle/prop never paints a static structure"),ClassifyMinimapSurface(TEXT("/Game/Prop/Vehicle"),false,300,1)==ESurroundingSurface::Unknown);
    TestTrue(TEXT("Measured tall prop is generic structure, not invented pavement"),ClassifyMinimapSurface(TEXT("/Game/Prop/Kit_StreetLamp_A/SM_StreetLamp"),true,300,1)==ESurroundingSurface::Structure);
    return true;
}

#endif
