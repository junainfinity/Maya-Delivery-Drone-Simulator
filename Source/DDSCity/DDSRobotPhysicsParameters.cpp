#include "DDSRobotPhysicsTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
bool Positive(double V) { return FMath::IsFinite(V) && V > 0.0; }
bool FiniteTransform(const FTransform& T)
{
    return !T.ContainsNaN() && T.GetRotation().IsNormalized()
        && T.GetScale3D().Equals(FVector::OneVector, 1.e-9);
}
bool Hash(const FString& S)
{
    if (S.Len() != 64) return false;
    for (TCHAR C : S) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
}

bool DDSValidateRobotProfile(const FDDSRobotPhysicsProfile& P, FString& Error)
{
    const double Values[] = {P.GravityMps2, P.TargetStepS, P.BareChassisKg, P.WheelKg,
        P.Lid0Kg, P.Lid1Kg, P.ParcelKg, P.MotorPeakNm, P.BrakePeakNm, P.LidPeakNm,
        P.MotorNoLoadRadps, P.MotorErrorNmPerRadps, P.BrakeRegularizationRadps,
        P.SuspensionK_Npm, P.SuspensionC_Nspm, P.SuspensionForceLimitN,
        P.CurrentStateMaxAgeS, P.RecoveryPermitMaxAgeS, P.BindingTimeoutRealS};
    for (double V : Values) if (!Positive(V)) { Error = TEXT("Nonfinite/nonpositive profile parameter"); return false; }
    if (!Hash(P.ProfileSha256) || !FMath::IsFinite(P.BearingNmPerRadps) || P.BearingNmPerRadps < 0
        || !FMath::IsFinite(P.StaticFriction) || !FMath::IsFinite(P.DynamicFriction)
        || P.StaticFriction < P.DynamicFriction || P.DynamicFriction < 0
        || !FMath::IsFinite(P.Restitution) || P.Restitution < 0 || P.Restitution > 1
        || !FMath::IsNearlyEqual(P.GravityMps2, 9.80665, 1.e-9)
        || !FMath::IsNearlyEqual(P.TargetStepS, 1.0 / 120.0, 1.e-12))
    { Error = TEXT("Invalid hash/material/conventional gravity/adopted step"); return false; }
    const auto& S = P.Service;
    if (S.bFrozen && (!Positive(S.MaxSampleAgeS) || !Positive(S.MaxLinearSpeedMps)
        || !Positive(S.MaxAngularSpeedRadps) || !Positive(S.MaxVerticalSpeedMps)
        || !Positive(S.MaxPitchRad) || !Positive(S.MaxRollRad) || !Positive(S.RequiredStableDurationS)
        || S.MinLoadedWheels < 1 || S.MinLoadedWheels > 6))
    { Error = TEXT("Frozen service limits incomplete"); return false; }
    return true;
}

bool DDSValidateMeasuredBinding(const FDDSRobotMeasuredBinding& B,
    const FDDSRobotPhysicsProfile& P, FString& Error)
{
    if (!DDSValidateRobotProfile(P, Error)) return false;
    if (!Hash(B.BindingSha256) || !Hash(B.A02MeasurementCertificateSha256)
        || !B.bNineBodyTopologyVerified || !B.bMountsMeasured || !B.bFramesAreScaledPhysicalSI)
    { Error = TEXT("A02 measured binding/topology/mount certificate absent"); return false; }
    if (B.Mechanics.bFrozen && !DDSValidateN12Calibration(B.Mechanics,Error)) return false;
    if (B.Mechanics.bFrozen && !B.Parcel.ActualPart) { Error=TEXT("N12 frozen mechanics requires declared actual parcel"); return false; }
    if (B.Parcel.ActualPart)
    {
        const auto& M=B.Parcel; const auto J=M.PrincipalInertiaKgM2;
        if (!M.bMeasured || !M.bSimpleCollisionMeasured || M.SuppliedPartId.IsEmpty()
            || !M.ActualPart->GetStaticMesh() || M.ActualPart->GetStaticMesh()->GetPathName()!=M.SuppliedMeshPackage
            || !M.ActualPart->GetBodySetup()
            || M.ActualPart->GetBodySetup()->AggGeom.GetElementCount()<1
            || M.ActualPart->GetBodySetup()->CollisionTraceFlag==CTF_UseComplexAsSimple
            || M.MeasuredCollisionProfile.IsNone() || !Hash(M.MeshPackageSha256) || !Hash(M.SimpleCollisionSha256)
            || !Hash(M.MeasurementRecordSha256) || M.COMLocalMeters.ContainsNaN() || !M.PrincipalAxesInBody.IsNormalized()
            || !M.ActualPart->GetComponentScale().Equals(M.SupplierScale,1.e-6)
            || M.SupplierScale.GetMin()<=0 || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Y,1.e-6)
            || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Z,1.e-6)
            || !Positive(J.X) || !Positive(J.Y) || !Positive(J.Z) || J.X>J.Y+J.Z || J.Y>J.X+J.Z || J.Z>J.X+J.Y)
        { Error=TEXT("N12 parcel geometry/mass-distribution input missing"); return false; }
    }
    if (B.Mechanics.bFrozen)
    {
        if (!FMath::IsNearlyEqual(B.Mechanics.LidPeakNm,P.LidPeakNm,1.e-9)
            || !B.Mechanics.MountInChassisMeters.Equals(B.CargoMountInChassisMeters,1.e-9))
        { Error=TEXT("N12 calibrated torque/mount does not match frozen N06 profile/binding"); return false; }
        for(int32 L=0;L<2;++L) if(!FMath::IsNearlyEqual(FMath::Abs(B.Mechanics.OpenAngleRad[L]),B.Joints[L+6].LidOpenLimitRad,1.e-9))
        { Error=TEXT("N12 opening range disagrees with measured original hinge"); return false; }
    }
    TSet<UStaticMeshComponent*> Seen;
    if (B.Parcel.ActualPart) Seen.Add(B.Parcel.ActualPart);
    TSet<FString> PartIds;
    if (B.Parcel.ActualPart) PartIds.Add(B.Parcel.SuppliedPartId);
    for (int32 I = 0; I < 9; ++I)
    {
        const auto& M = B.Bodies[I];
        const FVector J = M.PrincipalInertiaKgM2;
        if (M.SupplierScale.ContainsNaN() || M.SupplierScale.GetMin() <= 0
            || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Y,1.e-6)
            || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Z,1.e-6))
        { Error = TEXT("A02_UNSUPPORTED_TRANSFORM: positive uniform supplier scale supported; nonuniform/reflected/sheared scale requires a measured affine collision/joint adapter"); return false; }
        if (!M.bMeasured || !M.bSimpleCollisionMeasured || !M.ActualPart
            || M.MeasuredCollisionProfile.IsNone() || !M.ActualPart->GetStaticMesh() || !M.ActualPart->GetBodySetup()
            || M.ActualPart->GetBodySetup()->AggGeom.GetElementCount() < 1
            || M.ActualPart->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple
            || Seen.Contains(M.ActualPart) || PartIds.Contains(M.SuppliedPartId) || M.SuppliedPartId.IsEmpty()
            || M.ActualPart->GetStaticMesh()->GetPathName() != M.SuppliedMeshPackage
            || !Hash(M.MeshPackageSha256) || !Hash(M.SimpleCollisionSha256)
            || !Hash(M.MeasurementRecordSha256) || !FiniteTransform(M.RestInChassisMeters)
            || !M.ActualPart->GetComponentScale().Equals(M.SupplierScale, 1.e-6)
            || M.SupplierScale.GetMin() <= 0 || M.COMLocalMeters.ContainsNaN()
            || !M.PrincipalAxesInBody.IsNormalized() || !Positive(J.X) || !Positive(J.Y) || !Positive(J.Z)
            || J.X > J.Y + J.Z || J.Y > J.X + J.Z || J.Z > J.X + J.Y)
        { Error = FString::Printf(TEXT("A02 invalid/absent measured body %d"), I); return false; }
        Seen.Add(M.ActualPart); PartIds.Add(M.SuppliedPartId);
        if (I >= 1 && I <= 6 && M.MeasuredMotorForwardSign != -1 && M.MeasuredMotorForwardSign != 1)
        { Error = TEXT("A02 wheel motor rolling direction sign is unmeasured"); return false; }
    }
    if (!B.Bodies[0].RestInChassisMeters.Equals(FTransform::Identity, 1.e-9))
    { Error = TEXT("Chassis rest frame must be identity in its own frame"); return false; }
    for (int32 I = 0; I < 8; ++I)
    {
        const auto& J = B.Joints[I];
        if (!J.bMeasured || !FiniteTransform(J.InChassisMeters) || !FiniteTransform(J.InPartMeters)
            || !FMath::IsFinite(J.SuspensionHalfTravelM) || J.SuspensionHalfTravelM < 0
            || !FMath::IsFinite(J.SuspensionRestTargetM)
            || FMath::Abs(J.SuspensionRestTargetM) > J.SuspensionHalfTravelM
            || !FMath::IsFinite(J.LidOpenLimitRad) || J.LidOpenLimitRad < 0 || J.LidOpenLimitRad > PI)
        { Error = FString::Printf(TEXT("A02 invalid measured joint %d"), I); return false; }
        const FTransform InChassis = J.InPartMeters * B.Bodies[I + 1].RestInChassisMeters;
        if (!InChassis.GetLocation().Equals(J.InChassisMeters.GetLocation(), B.MaxContactSeparationM)
            || !InChassis.GetRotation().Equals(J.InChassisMeters.GetRotation(), 1.e-6))
        { Error = FString::Printf(TEXT("Joint %d reference frames do not meet at measured rest"), I); return false; }
    }
    double Allocations[9] = {};
    for (const auto& M : B.SuspensionParts)
    {
        const int32 Owner = static_cast<int32>(M.Owner);
        if (M.SupplierScale.ContainsNaN() || M.SupplierScale.GetMin() <= 0
            || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Y,1.e-6)
            || !FMath::IsNearlyEqual(M.SupplierScale.X,M.SupplierScale.Z,1.e-6)
            || (M.ActualPart && !M.ActualPart->GetComponentScale().Equals(M.SupplierScale,1.e-6)))
        { Error = TEXT("A02_UNSUPPORTED_SUSPENSION_TRANSFORM: measured positive uniform supplier scale required"); return false; }
        if (!M.bMeasured || !M.ActualPart || Seen.Contains(M.ActualPart) || M.SuppliedPartId.IsEmpty()
            || PartIds.Contains(M.SuppliedPartId) || Owner < 0 || Owner >= 9
            || !M.bRigidRelativeOwnerThroughoutMotion || !M.bCollisionAlreadyIncludedInOwnerShape
            || !Hash(M.GeometryAndMassMapSha256) || !FiniteTransform(M.RestInOwnerMeters)
            || !Positive(M.DeclaredMassAllocationKg))
        { Error = TEXT("A02_TOPOLOGY_GAP: all six suspension pieces require faithful rigid owner/collision/mass map"); return false; }
        Seen.Add(M.ActualPart); PartIds.Add(M.SuppliedPartId); Allocations[Owner] += M.DeclaredMassAllocationKg;
    }
    for (int32 I = 0; I < 9; ++I)
    {
        const double Mass = I == 0 ? P.BareChassisKg : (I < 7 ? P.WheelKg : (I == 7 ? P.Lid0Kg : P.Lid1Kg));
        if (Allocations[I] >= Mass) { Error = TEXT("Suspension allocation consumes/exceeds whole owner mass"); return false; }
    }
    if (!Positive(B.MaxContactSeparationM) || !Positive(B.MinLoadedImpulseNs)
        || !Positive(B.SleepingAnchorToleranceM) || !Positive(B.SleepingSpeedToleranceMps)
        || !Positive(B.SleepingOmegaToleranceRadps)
        || !Positive(B.SlipDomainSpeedMps) || !Positive(B.ClosedLidAngleToleranceRad)
        || !Positive(B.ClosedLidOmegaToleranceRadps) || !Positive(B.MinSupportUpDot) || B.MinSupportUpDot > 1
        || !FiniteTransform(B.SensorMountInChassisMeters) || !FiniteTransform(B.ChaseMountInChassisMeters)
        || !FiniteTransform(B.ServiceSocketInChassisMeters) || !FiniteTransform(B.CargoMountInChassisMeters))
    { Error = TEXT("A02 missing measured contact/shape/slip error budget or mount frame"); return false; }
    return true;
}

double DDSMotorTorqueNm(double Request, double Omega, const FDDSRobotPhysicsProfile& P)
{
    if (!FMath::IsFinite(Request) || !FMath::IsFinite(Omega)) return 0;
    Request = FMath::Clamp(Request, -1.0, 1.0);
    if (Request == 0.0) return 0.0; // Passive coast, not zero-speed servo.
    const double Target = Request * P.MotorNoLoadRadps;
    const double Raw = FMath::Clamp((Target - Omega) * P.MotorErrorNmPerRadps, -P.MotorPeakNm, P.MotorPeakNm);
    const bool bMotoring = Raw * Omega > 0;
    const double Curve = bMotoring ? FMath::Clamp(1.0 - FMath::Abs(Omega) / P.MotorNoLoadRadps, 0.0, 1.0) : 1.0;
    return Raw * Curve;
}

double DDSBrakeBearingTorqueNm(bool bBrake, double Omega, const FDDSRobotPhysicsProfile& P)
{
    if (!FMath::IsFinite(Omega)) return 0;
    // Finite dissipative torque; no angular-velocity assignment/clamp.
    const double Brake = bBrake ? -P.BrakePeakNm * FMath::Tanh(Omega / P.BrakeRegularizationRadps) : 0;
    return Brake - P.BearingNmPerRadps * Omega;
}


#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSRobotIndependentLidMassControls,
    "DDSCity.Physics.IndependentLidMassControls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSRobotIndependentLidMassControls::RunTest(const FString&)
{
    FDDSRobotPhysicsProfile P;FString Error;
    P.ProfileSha256=TEXT("1111111111111111111111111111111111111111111111111111111111111111");
    // Synthetic profile validation only. Never a supplied-body observation or drive permit.
    TestEqual(TEXT("body7 lid mass starts unset"),P.Lid0Kg,0.0);
    TestEqual(TEXT("body8 lid mass starts unset"),P.Lid1Kg,0.0);
    TestFalse(TEXT("both unset refuse profile"),DDSValidateRobotProfile(P,Error));
    P.Lid0Kg=11.3;
    TestFalse(TEXT("only body7 supplied still refuses"),DDSValidateRobotProfile(P,Error));
    P.Lid0Kg=0.0;P.Lid1Kg=17.1;
    TestFalse(TEXT("only body8 supplied still refuses"),DDSValidateRobotProfile(P,Error));
    P.Lid0Kg=11.3;
    TestTrue(TEXT("distinct finite positive supplied masses are allowed"),DDSValidateRobotProfile(P,Error));
    for(int32 Lid=0;Lid<2;++Lid)
    {
        double& Mass=Lid==0?P.Lid0Kg:P.Lid1Kg;const double Original=Mass;
        for(double Invalid:{0.0,-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        {
            Mass=Invalid;
            TestFalse(FString::Printf(TEXT("lid%d zero/negative/nonfinite refuses independently"),Lid),DDSValidateRobotProfile(P,Error));
        }
        Mass=Original;
    }
    TestTrue(TEXT("restored distinct masses still validate"),DDSValidateRobotProfile(P,Error));
    return true;
}
#endif
