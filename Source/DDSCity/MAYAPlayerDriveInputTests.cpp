#include "MAYAPlayerDriveInput.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAPlayerReverseDirectionTest, "Maya.Delivery.PlayerReverseDirection",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMAYAPlayerReverseDirectionTest::RunTest(const FString&)
{
    using MAYAPlayerDriveInput::BodyYawSteer;
    TestEqual(TEXT("S+A requests rightward nose yaw for a leftward reverse path"), BodyYawSteer(-1.f,-1.f), 1.f);
    TestEqual(TEXT("S+D requests leftward nose yaw for a rightward reverse path"), BodyYawSteer(-1.f,1.f), -1.f);
    TestEqual(TEXT("Straight reverse retains zero steering"), BodyYawSteer(-1.f,0.f), 0.f);
    TestEqual(TEXT("W+A keeps existing forward-left yaw"), BodyYawSteer(1.f,-1.f), -1.f);
    TestEqual(TEXT("W+D keeps existing forward-right yaw"), BodyYawSteer(1.f,1.f), 1.f);
    TestEqual(TEXT("A alone keeps existing stationary pivot"), BodyYawSteer(0.f,-1.f), -1.f);
    TestEqual(TEXT("D alone keeps existing stationary pivot"), BodyYawSteer(0.f,1.f), 1.f);
    TestEqual(TEXT("Opposed W/S cancellation keeps stationary steering"), BodyYawSteer(1.f-1.f,-1.f), -1.f);
    TestEqual(TEXT("Opposed A/D retains neutral steering"), BodyYawSteer(-1.f,1.f-1.f), 0.f);
    TestEqual(TEXT("Low requested reverse keeps analog magnitude and reverses yaw sign"), BodyYawSteer(-.05f,.3f), -.3f);
    TestEqual(TEXT("Low requested forward preserves analog steering"), BodyYawSteer(.05f,.3f), .3f);
    return true;
}
#endif
