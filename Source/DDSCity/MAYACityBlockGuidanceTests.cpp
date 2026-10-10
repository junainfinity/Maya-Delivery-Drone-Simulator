#include "MAYADeliveryHUDGeometry.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYACityBlockGuidanceTest, "MAYA.Delivery.CityBlock.ConnectedGuidance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMAYACityBlockGuidanceTest::RunTest(const FString& Parameters)
{
    using namespace MAYAHUDGeometry;
    FPolyline Route;
    TestTrue(TEXT("Turned connected path builds"), Route.Build({FVector(0,0,0),FVector(1000,0,0),FVector(1000,2000,0),FVector(3000,2000,0)}));
    FMAYADeliverySnapshot S;
    S.Phase=EMAYADeliveryPhase::Carrying; S.BatteryPercent=100;
    FMAYADeliveryRobotObservation R;
    R.bKnown=true; R.bReady=true; R.ChassisPose=FTransform(FVector(100,0,0));
    auto N=Guidance(Route,S,R,false,Route.Points.Last(),90);
    TestTrue(TEXT("Connected forward route permits guidance"),N.bGuidance && N.bMoving);
    TestTrue(TEXT("Forward cue points to first bend, not distant goal"),N.CuePoint.Equals(FVector(1000,0,0),.01));
    TestEqual(TEXT("UE right turn cue"),N.CueText,FString(TEXT("Turn right")));
    R.ChassisPose=FTransform(FVector(2900,2000,0));
    N=Guidance(Route,S,R,false,Route.Points[0],90);
    TestTrue(TEXT("Reverse return uses same connected route"),N.Direction<0 && N.bGuidance);
    TestTrue(TEXT("Reverse cue chooses nearest bend"),N.CuePoint.Equals(FVector(1000,2000,0),.01));
    TestEqual(TEXT("Reverse turn cue changes direction"),N.CueText,FString(TEXT("Turn right")));
    R.ChassisPose=FTransform(FVector(100,500,0));
    N=Guidance(Route,S,R,false,Route.Points.Last(),90);
    TestTrue(TEXT("Off-route robot gets no world ribbon"),N.bOffRoute && !N.bMoving);
    TestTrue(TEXT("Off-route cue is measured route projection"),N.CuePoint.Equals(FVector(100,0,0),.01));
    R.ChassisPose=FTransform(FVector(100,0,0));
    N=Guidance(Route,S,R,false,FVector(3000,2500,0),90);
    TestFalse(TEXT("Unmatched distant goal never creates direct guidance"),N.bGuidance);
    N=Guidance(Route,S,R,true,Route.Points.Last(),90);
    TestFalse(TEXT("Paused guidance remains suppressed"),N.bGuidance);
    const FRect Area{0,0,140,140};
    const auto A=LocalMapTransform(Area,FVector(-29900,20,0));
    const auto B=LocalMapTransform(Area,FVector(-29860,40,0));
    const auto C=LocalMapTransform(Area,FVector(-29499,40,0));
    TestTrue(TEXT("Local map follows actual motion continuously within the former5m cell"),(B.WorldCentre-A.WorldCentre).Equals(FVector2D(40,20),.01));
    TestTrue(TEXT("Actual drone projects to map centre after small movement"),B.Project(FVector(-29860,40,0)).Equals(B.ScreenCentre,.01));
    TestEqual(TEXT("Local map remains40m across"),140.0/A.PixelsPerCm,4000.0);
    TestEqual(TEXT("Crossing former5m boundary preserves actual401cm motion"),C.WorldCentre.X-A.WorldCentre.X,401.0);
    return true;
}
#endif
