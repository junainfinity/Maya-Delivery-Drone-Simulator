#include "DDSN08Authority.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
// Real pure N08 authority, controlled ports; NOT native world/physics/storage proof.
namespace DDSMissionEntryControlled
{
struct FPorts final : IDDSN08MotionPort, IDDSN08MechanicsPort, IDDSN08SavePort
{
    FDDSN08RestoreBarrier Barrier;
    int32 BarrierCalls = 0;
    void SetDriveLocked(bool) override {}
    void BeginPhysicalCycle(const FDDSN08Token&) override {}
    void CancelAndClose(const FDDSN08Token&) override {}
    FDDSN08SaveCandidate Prepare(const FDDSN08StableSnapshot&, const FDDSN08Token&) override { return {}; }
    bool Submit(const FDDSN08SaveCandidate&) override { return false; }
    FDDSN08RestoreBarrier EstablishRestoreBarrier(const FDDSN08StableSnapshot&,
        const FDDSN08SaveCandidate*) override { ++BarrierCalls; return Barrier; }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSMissionEntryAuthorityBoundary,
    "DDSCity.N08.ExplicitEntryAuthorityBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSMissionEntryAuthorityBoundary::RunTest(const FString&)
{
    using namespace DDSMissionEntryControlled;
    FPorts Ports; FDDSN08Authority Authority(Ports, Ports, Ports);
    const TArray<FString> Stars = {TEXT("a"), TEXT("b"), TEXT("c")};
    TestFalse(TEXT("missing actual cargo identity refuses"), Authority.StartFreshMission(
        TEXT("instance"), TEXT(""), TEXT("pickup"), TEXT("delivery"), Stars));
    TestTrue(TEXT("explicit valid identities initialize sole authority"), Authority.StartFreshMission(
        TEXT("instance"), TEXT("observed-cargo"), TEXT("pickup"), TEXT("delivery"), Stars));
    const auto Initial = Authority.GetStableSnapshot();
    TestFalse(TEXT("fresh never replaces initialized authority"), Authority.StartFreshMission(
        TEXT("other"), TEXT("other-cargo"), TEXT("pickup"), TEXT("delivery"), Stars));
    TestFalse(TEXT("replay cannot skip delivery/save acknowledgment"), Authority.ReplayExplicit(
        TEXT("replay"), TEXT("other-cargo"), {TEXT("d"), TEXT("e"), TEXT("f")}));
    TestTrue(TEXT("one real scoped star recorded"), Authority.RecordN10Star(TEXT("instance"), TEXT("b")));
    TestFalse(TEXT("same star cannot award twice"), Authority.RecordN10Star(TEXT("instance"), TEXT("b")));
    TestFalse(TEXT("old/foreign instance cannot award"), Authority.RecordN10Star(TEXT("foreign"), TEXT("c")));
    TestFalse(TEXT("missing native lid proof refuses restore"), Authority.RestoreValidatedCheckpoint(Initial, true, false, true));
    TestEqual(TEXT("missing proof never reaches storage barrier"), Ports.BarrierCalls, 0);
    TestFalse(TEXT("validated bytes alone cannot bypass durable barrier"), Authority.RestoreValidatedCheckpoint(Initial, true, true, true));
    TestEqual(TEXT("barrier failure keeps accepted star"), Authority.GetStableSnapshot().CollectedStarIds.Num(), 1);
    Ports.Barrier.MissionInstanceId = Initial.MissionInstanceId;
    Ports.Barrier.bCheckpointDurablyCurrent = true; // controlled port only.
    Ports.Barrier.bOlderWritesCannotOvertake = true;
    TestTrue(TEXT("explicit matched controlled barrier permits restore"), Authority.RestoreValidatedCheckpoint(Initial, true, true, true));
    TestEqual(TEXT("restored star list exactly checkpoint"), Authority.GetStableSnapshot().CollectedStarIds.Num(), 0);
    return true;
}
#endif
