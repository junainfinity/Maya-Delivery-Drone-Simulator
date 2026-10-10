#include "MAYAWheelFeedbackCallback.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>

namespace MayaWheelFeedbackTests
{
FMAYAWheelFeedbackOperands ValidOperands()
{
    FMAYAWheelFeedbackOperands O;
    O.Command.Generation = 1; O.Command.Revision = 1; O.Command.Sequence = 1;
    O.Command.IssuedRealS = 100.0; O.Command.LeaseSeconds = 0.25;
    O.Command.Throttle = -1.0; O.Command.Steer = 0.0;
    O.Command.bReady = true; O.Command.bFreshPermission = true;
    O.Command.bDriveInhibited = false; O.Command.bDeliveryLocked = false;
    O.Command.bInputsNeedRelease = false; O.Command.bBrakeRequested = false;
    O.Command.bBoostRequested = true; O.Command.bClosedLatched = true;
    O.SampleRealS = 100.01; O.DeltaSeconds = 1.0 / 120.0;
    O.bNativeStateFinite = true; O.bActualClosedLocks = true;
    O.bChassisSpeedKnown = true; O.ChassisSpeedCmps = 0;
    for (double& Omega : O.RelativeWheelOmegaRadps) Omega = 0;
    return O;
}

bool AllMotorsZero(const FMAYAWheelFeedbackDecision& D)
{
    for (double Nm : D.MotorNm) if (!FMath::IsFinite(Nm) || Nm != 0) return false;
    return true;
}

void CheckRevoked(FAutomationTestBase& T, const TCHAR* Case,
    const FMAYAWheelFeedbackOperands& O)
{
    const auto D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    T.TestFalse(FString(Case) + TEXT(" drive revoked"), D.bDriveAdmitted);
    T.TestFalse(FString(Case) + TEXT(" Boost inactive"), D.bBoostActive);
    T.TestTrue(FString(Case) + TEXT(" native brake requested"), D.bBrakeEnabled);
    T.TestTrue(FString(Case) + TEXT(" all active motors zero"), AllMotorsZero(D));
    T.TestEqual(FString(Case) + TEXT(" no active effort"), D.EffortFraction, 0.0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAFeedbackRevocation,
    "Maya.Delivery.WheelFeedback.PermissionRevocation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMAYAFeedbackRevocation::RunTest(const FString&)
{
    using namespace MayaWheelFeedbackTests;
    CheckRevoked(*this, TEXT("Default uninitialized permit"), FMAYAWheelFeedbackOperands());
    const auto Valid = ValidOperands();
    const auto Active = MAYAWheelFeedback::Evaluate(Valid, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Control begins with an admitted powered Boost"), Active.bDriveAdmitted && Active.bBoostActive);
    auto O = Valid; O.Command.bFreshPermission = false; CheckRevoked(*this, TEXT("Focus/origin permission lost"), O);
    O = Valid; O.Command.bReady = false; CheckRevoked(*this, TEXT("Rig readiness lost"), O);
    O = Valid; O.Command.bDriveInhibited = true; CheckRevoked(*this, TEXT("Door drive inhibit"), O);
    O = Valid; O.Command.bDeliveryLocked = true; CheckRevoked(*this, TEXT("Delivery lock"), O);
    O = Valid; O.Command.bInputsNeedRelease = true; CheckRevoked(*this, TEXT("Input release interlock"), O);
    O = Valid; O.Command.bBrakeRequested = true; CheckRevoked(*this, TEXT("Player brake"), O);
    O = Valid; O.Command.bClosedLatched = false; CheckRevoked(*this, TEXT("Closed-latched permit revoked"), O);
    O = Valid; O.bActualClosedLocks = false; CheckRevoked(*this, TEXT("Current native closed lock absent"), O);
    O = Valid; O.bNativeStateFinite = false; CheckRevoked(*this, TEXT("Native state unavailable"), O);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAFeedbackInvalidOperands,
    "Maya.Delivery.WheelFeedback.ClockAndFiniteOperands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMAYAFeedbackInvalidOperands::RunTest(const FString&)
{
    using namespace MayaWheelFeedbackTests;
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const float ControlNaN = std::numeric_limits<float>::quiet_NaN();
    const auto Valid = ValidOperands();
    auto O = Valid; O.SampleRealS = 100.25;
    TestTrue(TEXT("Command is valid at inclusive existing lease boundary"),
        MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile()).bDriveAdmitted);
    O.SampleRealS = 100.250001; CheckRevoked(*this, TEXT("Expired command"), O);
    O = Valid; O.SampleRealS = 99.999; CheckRevoked(*this, TEXT("Future-issued command"), O);
    O = Valid; O.Command.LeaseSeconds = 0; CheckRevoked(*this, TEXT("Zero lease"), O);
    O = Valid; O.Command.LeaseSeconds = -1; CheckRevoked(*this, TEXT("Negative lease"), O);
    O = Valid; O.Command.LeaseSeconds = 0.251; CheckRevoked(*this, TEXT("Lease exceeds existing watchdog"), O);
    O = Valid; O.Command.LeaseSeconds = NaN; CheckRevoked(*this, TEXT("Nonfinite lease"), O);
    O = Valid; O.Command.IssuedRealS = NaN; CheckRevoked(*this, TEXT("Nonfinite issue clock"), O);
    O = Valid; O.SampleRealS = NaN; CheckRevoked(*this, TEXT("Nonfinite sample clock"), O);
    O = Valid; O.DeltaSeconds = 0; CheckRevoked(*this, TEXT("Zero step duration"), O);
    O = Valid; O.DeltaSeconds = -0.01; CheckRevoked(*this, TEXT("Negative step duration"), O);
    O = Valid; O.DeltaSeconds = NaN; CheckRevoked(*this, TEXT("Nonfinite step duration"), O);
    O = Valid; O.Command.Throttle = ControlNaN; CheckRevoked(*this, TEXT("Nonfinite throttle"), O);
    O = Valid; O.Command.Steer = ControlNaN; CheckRevoked(*this, TEXT("Nonfinite steer"), O);
    O = Valid; O.RelativeWheelOmegaRadps[3] = NaN;
    CheckRevoked(*this, TEXT("One invalid wheel revokes all active motors"), O);
    O = Valid; O.Command.bReady = false; O.RelativeWheelOmegaRadps[0] = 10;
    const auto Braked = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Valid passive bearing remains dissipative when active drive is revoked"),
        FMath::IsNearlyEqual(Braked.BearingNm[0], -0.2, 1.e-9));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAFeedbackReserveAndEffort,
    "Maya.Delivery.WheelFeedback.ReserveAndActualEffort",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMAYAFeedbackReserveAndEffort::RunTest(const FString&)
{
    using namespace MayaWheelFeedbackTests;
    auto O = ValidOperands();
    auto D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestEqual(TEXT("Stopped current Boost selects existing maximum reserve"), D.SelectedPeakNm, 18.0);
    TestTrue(TEXT("Calibrated current no-load target is nominal42.4"), FMath::IsNearlyEqual(D.NoLoadRadps, 42.4, 1.e-9));
    TestTrue(TEXT("Six stalled powered wheels have full actual effort"), FMath::IsNearlyEqual(D.EffortFraction, 1.0, 1.e-9));
    O.ChassisSpeedCmps = 25;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestEqual(TEXT("Reserve midpoint uses actual chassis speed"), D.SelectedPeakNm, 15.0);
    TestTrue(TEXT("Midpoint torque uses the same peak as effort normalization"),
        FMath::IsNearlyEqual(D.MotorNm[0], 15.0, 1.e-9) && FMath::IsNearlyEqual(D.EffortFraction, 1.0, 1.e-9));
    O.ChassisSpeedCmps = 50;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestEqual(TEXT("Road speed removes reserve"), D.SelectedPeakNm, 12.0);
    O.bChassisSpeedKnown = false; O.ChassisSpeedCmps = 0;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Unknown reserve speed retains admitted road power only"), D.bDriveAdmitted && D.SelectedPeakNm == 12.0);
    O.bChassisSpeedKnown = true; O.ChassisSpeedCmps = std::numeric_limits<double>::quiet_NaN();
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Nonfinite reserve speed cannot create extra power"), D.bDriveAdmitted && D.SelectedPeakNm == 12.0);
    O = ValidOperands();
    for (int W = 0; W < 3; ++W) O.RelativeWheelOmegaRadps[W] = 60;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Three coasting wheels reduce actual effort to half"),
        D.bBoostActive && FMath::IsNearlyEqual(D.EffortFraction, 0.5, 1.e-9));
    O = ValidOperands(); O.Command.bBoostRequested = false;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Normal mode retains20rad/s5Nm and has no Boost energy flag"),
        D.bDriveAdmitted && !D.bBoostActive && D.NoLoadRadps == 20 && D.SelectedPeakNm == 5 && D.MotorNm[0] == 5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMAYAFeedbackCoastAndReversal,
    "Maya.Delivery.WheelFeedback.CoastAndReversal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMAYAFeedbackCoastAndReversal::RunTest(const FString&)
{
    using namespace MayaWheelFeedbackTests;
    auto O = ValidOperands(); O.ChassisSpeedCmps = 100;
    for (double& Omega : O.RelativeWheelOmegaRadps) Omega = 60;
    auto D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Overspeed under same direction coasts without active braking"),
        D.bDriveAdmitted && !D.bBrakeEnabled && !D.bBoostActive && AllMotorsZero(D));
    TestEqual(TEXT("All-wheel coast has no active Boost effort"), D.EffortFraction, 0.0);
    TestTrue(TEXT("Coast retains passive bearing"), FMath::IsNearlyEqual(D.BearingNm[0], -1.2, 1.e-9));
    O.Command.Throttle = 1;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Intentional reverse input still supplies reverse motor power"),
        D.bDriveAdmitted && D.bBoostActive && D.MotorNm[0] == -12 && !D.MotorDecisions[0].bCounterTorqueSuppressed);
    O = ValidOperands(); O.Command.Throttle = 0; O.Command.Steer = 0;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Shift without movement request cannot create powered Boost"),
        D.bDriveAdmitted && !D.bBoostActive && AllMotorsZero(D) && D.EffortFraction == 0);
    O = ValidOperands(); O.Command.bBoostRequested = false;
    O.Command.Throttle = 0.5; O.Command.Steer = 0.25;
    D = MAYAWheelFeedback::Evaluate(O, FDDSRobotPhysicsProfile());
    TestTrue(TEXT("Existing signed bank steering is preserved"),
        D.MotorNm[0] == -5.0 && D.MotorNm[2] == -5.0 && D.MotorNm[3] == -2.5 && D.MotorNm[5] == -2.5);
    return true;
}
#endif
