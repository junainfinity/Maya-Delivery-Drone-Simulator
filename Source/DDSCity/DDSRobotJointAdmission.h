#pragma once
#include "DDSRobotPhysicsTypes.h"
#include "Chaos/PBDJointConstraintTypes.h"

// Shared external/internal readback law; external validity and endpoint checks
// precede this function. Native force-law/solver behavior remains UNRUN.
inline bool DDSJointSettingsMatch(const Chaos::FPBDJointSettings& S, int32 I,
    const FTransform& A, const FTransform& B, double TravelCm, double TargetCm,
    bool bExcludeCollision, const FDDSRobotPhysicsProfile& P)
{
    using M = Chaos::EJointMotionType;
    const bool Driven = I < 6 && TravelCm > 0;
    const auto FrameMatches = [](const Chaos::FRigidTransform3& X, const FTransform& Y)
    { return FVector(X.GetTranslation()).Equals(Y.GetLocation(), 1.e-5)
        && FQuat(X.GetRotation()).Equals(Y.GetRotation(), 1.e-6); };
    if (!FrameMatches(S.ConnectorTransforms[0], A) || !FrameMatches(S.ConnectorTransforms[1], B)
        || S.bCollisionEnabled != !bExcludeCollision || S.bProjectionEnabled
        || S.bShockPropagationEnabled || S.bMassConditioningEnabled
        || !FMath::IsNearlyEqual(S.ParentInvMassScale, 1.0, 1.e-6)
        || S.LinearMotionTypes[0] != M::Locked || S.LinearMotionTypes[1] != M::Locked
        || S.LinearMotionTypes[2] != (Driven ? M::Limited : M::Locked)
        || S.AngularMotionTypes[0] != (I < 6 ? M::Free : M::Locked)
        || S.AngularMotionTypes[1] != M::Locked || S.AngularMotionTypes[2] != M::Locked
        || (Driven && !FMath::IsNearlyEqual(S.LinearLimit, TravelCm, 1.e-5))) return false;
    for (int32 Axis = 0; Axis < 3; ++Axis)
        if (S.bLinearPositionDriveEnabled[Axis] != (Driven && Axis == 2)
            || S.bLinearVelocityDriveEnabled[Axis] != (Driven && Axis == 2)) return false;
    return !Driven || (S.LinearDriveForceMode == Chaos::EJointForceMode::Force
        && FVector(S.LinearDriveStiffness).Equals(FVector(0,0,P.SuspensionK_Npm), 1.e-6)
        && FVector(S.LinearDriveDamping).Equals(FVector(0,0,P.SuspensionC_Nspm), 1.e-6)
        && FVector(S.LinearDriveMaxForce).Equals(FVector(0,0,P.SuspensionForceLimitN * 100), 1.e-5)
        && FVector(S.LinearDrivePositionTarget).Equals(FVector(0,0,TargetCm), 1.e-5));
}
