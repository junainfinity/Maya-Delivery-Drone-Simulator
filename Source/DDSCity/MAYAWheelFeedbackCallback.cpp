#include "MAYAWheelFeedbackCallback.h"
#include "DDSRobotJointAdmission.h"
#include "PBDRigidsSolver.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PBDJointConstraints.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace
{
bool FiniteVector(const FVector& V)
{ return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); }
bool BrakeMatches(const Chaos::FPBDJointSettings& S, bool Enabled, const FDDSRobotPhysicsProfile& P)
{
    const double Damping = P.BrakePeakNm / P.BrakeRegularizationRadps * DDSRobotUnits::UETorquePerNm;
    return S.AngularMotionTypes[0] == Chaos::EJointMotionType::Free
        && S.AngularDriveForceMode == Chaos::EJointForceMode::Force
        && S.bAngularTwistVelocityDriveEnabled == Enabled
        && !S.bAngularSwingVelocityDriveEnabled && !S.bAngularSLerpVelocityDriveEnabled
        && !S.bAngularTwistPositionDriveEnabled && !S.bAngularSwingPositionDriveEnabled
        && !S.bAngularSLerpPositionDriveEnabled
        && FVector(S.AngularDriveVelocityTarget).Equals(FVector::ZeroVector, 1.e-6)
        && FVector(S.AngularDriveStiffness).Equals(FVector::ZeroVector, 1.e-6)
        && FVector(S.AngularDriveDamping).Equals(FVector(Damping, 0, 0), 1.e-5)
        && FVector(S.AngularDriveMaxTorque).Equals(FVector(P.BrakePeakNm * DDSRobotUnits::UETorquePerNm, 0, 0), 1.e-5);
}
bool ConfigureBrake(Chaos::FPBDJointConstraintHandle* H, bool Enabled, const FDDSRobotPhysicsProfile& P)
{
    if (!H || !H->IsValid() || !H->IsConstraintEnabled() || H->IsConstraintBroken()) return false;
    if (!BrakeMatches(H->GetSettings(), Enabled, P))
    {
        auto S = H->GetSettings(); // Preserve every original non-brake field.
        S.AngularDriveForceMode = Chaos::EJointForceMode::Force;
        S.bAngularTwistPositionDriveEnabled = S.bAngularSwingPositionDriveEnabled = S.bAngularSLerpPositionDriveEnabled = false;
        S.bAngularTwistVelocityDriveEnabled = Enabled;
        S.bAngularSwingVelocityDriveEnabled = S.bAngularSLerpVelocityDriveEnabled = false;
        S.AngularDriveVelocityTarget = Chaos::FVec3(0);
        S.AngularDriveStiffness = Chaos::FVec3(0);
        S.AngularDriveDamping = Chaos::FVec3(P.BrakePeakNm / P.BrakeRegularizationRadps * DDSRobotUnits::UETorquePerNm, 0, 0);
        S.AngularDriveMaxTorque = Chaos::FVec3(P.BrakePeakNm * DDSRobotUnits::UETorquePerNm, 0, 0);
        H->SetSettings(S);
    }
    return BrakeMatches(H->GetSettings(), Enabled, P);
}
void ApplyPair(Chaos::FPBDRigidsEvolutionGBF& Evolution, Chaos::FPBDRigidParticleHandle* Wheel,
    Chaos::FPBDRigidParticleHandle* Chassis, const FVector& Torque)
{
    if (!Wheel || !Chassis || !FiniteVector(Torque) || Torque.IsNearlyZero(1.e-12)) return;
    if (Wheel->ObjectState() == Chaos::EObjectStateType::Sleeping)
        Evolution.SetParticleObjectState(Wheel, Chaos::EObjectStateType::Dynamic);
    if (Chassis->ObjectState() == Chaos::EObjectStateType::Sleeping)
        Evolution.SetParticleObjectState(Chassis, Chaos::EObjectStateType::Dynamic);
    Wheel->AddTorque(Chaos::FVec3(Torque), false);
    Chassis->AddTorque(Chaos::FVec3(-Torque), false);
}
}

bool FMAYAWheelFeedbackCallback::Bind_Internal(const FMAYAWheelFeedbackSetup& S)
{
    if (!S.Control || S.Generation == 0) return false;
    auto* CallbackSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Handles = CallbackSolver->GetParticles().GetParticleHandles();
    Chaos::FWeakParticleHandle Found[9];
    int32 Count = 0;
    for (int32 I = 0; I < int32(Handles.Size()); ++I)
    {
        auto* H = Handles.Handle(I).Get();
        if (!H) continue;
        for (int32 J = 0; J < 9; ++J)
            if (S.ParticleIds[J] != INDEX_NONE && H->UniqueIdx().Idx == S.ParticleIds[J] && !Found[J].GetHandle())
            { Found[J] = H->WeakParticleHandle(); ++Count; }
    }
    if (Count != 9) return false;
    for (int32 I = 0; I < 9; ++I)
    {
        auto* R = Found[I].GetHandle()->CastToRigidParticle();
        if (!R || (R->ObjectState() != Chaos::EObjectStateType::Dynamic
            && R->ObjectState() != Chaos::EObjectStateType::Sleeping)) return false;
        for (int32 J = 0; J < I; ++J) if (Found[I].GetHandle() == Found[J].GetHandle()) return false;
    }
    Setup = S; Generation = S.Generation;
    for (int32 I = 0; I < 9; ++I) Bodies[I] = Found[I];
    return true; // Read-only binding: no mass, inertia, geometry or joint creation.
}

void FMAYAWheelFeedbackCallback::OnPreSimulate_Internal()
{
    const auto* Input = GetConsumerInput_Internal();
    auto& Out = GetProducerOutputData_Internal().Snapshot;
    Out = FMAYAWheelFeedbackSnapshot();
    Out.StepSequence = ++StepSequence; Out.SimTimeS = GetSimTime_Internal();
    Out.DeltaSeconds = GetDeltaTime_Internal(); Out.SampleRealS = FPlatformTime::Seconds();
    if (Generation == 0 && Input && Input->bHasSetup) Bind_Internal(Input->Setup);
    if (Generation == 0 || !Setup.Control) return;
    Out.Generation = Generation;
    auto* CallbackSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Evolution = *CallbackSolver->GetEvolution();
    FScopeLock OwnerLock(&Setup.Control->Mutex); // Revocation linearizes with all six pairs.
    if (Setup.Control->bRetiring || Setup.Control->Generation != Generation) return;
    Out.bOwnerCurrent = true;
    if (Input && Input->Command.Generation == Generation && Input->Command.Sequence >= LastCommand.Sequence)
        LastCommand = Input->Command;
    FMAYAWheelFeedbackCommand Command = LastCommand;
    Out.Revision = Command.Revision; Out.Sequence = Command.Sequence;
    const bool CurrentCommand = Command.Generation == Generation && Command.Revision == Setup.Control->Revision;
    Command.bFreshPermission &= CurrentCommand;
    Out.Operands.Command = Command; Out.Operands.SampleRealS = Out.SampleRealS;
    Out.Operands.DeltaSeconds = Out.DeltaSeconds;
    Chaos::FPBDRigidParticleHandle* Rigid[9] = {};
    FTransform Poses[9];
    bool Finite = true;
    for (int32 I = 0; I < 9; ++I)
    {
        auto* H = Bodies[I].GetHandle();
        auto* R = H ? H->CastToRigidParticle() : nullptr;
        if (!R || H->UniqueIdx().Idx != Setup.ParticleIds[I]
            || (R->ObjectState() != Chaos::EObjectStateType::Dynamic && R->ObjectState() != Chaos::EObjectStateType::Sleeping))
        { Finite = false; continue; }
        Rigid[I] = R; Poses[I] = FTransform(FQuat(R->R()), FVector(R->X()));
        Finite &= !Poses[I].ContainsNaN() && Poses[I].GetRotation().IsNormalized()
            && FiniteVector(FVector(R->V())) && FiniteVector(FVector(R->W()));
    }
    Out.bNineBodiesBound = true;
    for (int32 I = 0; I < 9; ++I) Out.bNineBodiesBound &= Rigid[I] != nullptr;
    Out.bChassisNativeBodyAvailable = Rigid[0] != nullptr;
    auto& Joints = Evolution.GetJointConstraints().GetConstraintHandles();
    Chaos::FPBDJointConstraintHandle* Actual[8] = {};
    bool WheelSettingsKnown[6] = {};
    bool AllJoints = true;
    for (int32 I = 0; I < 8; ++I)
    {
        const auto Matches = [&](int32 Index)
        {
            if (!Rigid[0] || !Rigid[I + 1] || !Joints.IsValidIndex(Index)) return false;
            auto* H = Joints[Index];
            if (!H || !H->IsValid() || !H->IsConstraintEnabled() || H->IsConstraintBroken()) return false;
            const auto Pair = H->GetConstrainedParticles();
            return (Pair[0] == Rigid[0] && Pair[1] == Rigid[I + 1])
                || (Pair[1] == Rigid[0] && Pair[0] == Rigid[I + 1]);
        };
        if (!Matches(JointIndices[I]))
        {
            JointIndices[I] = INDEX_NONE;
            for (int32 J = 0; J < Joints.Num(); ++J) if (Matches(J))
            { if (JointIndices[I] != INDEX_NONE) { JointIndices[I] = INDEX_NONE; break; } JointIndices[I] = J; }
        }
        if (!Matches(JointIndices[I])) { AllJoints = false; continue; }
        Actual[I] = Joints[JointIndices[I]];
        if (I < 6)
        {
            const auto Pair = Actual[I]->GetConstrainedParticles();
            const bool ChassisFirst = Pair[0] == Rigid[0];
            WheelSettingsKnown[I] = DDSJointSettingsMatch(Actual[I]->GetSettings(), I,
                ChassisFirst ? Setup.JointInChassis[I] : Setup.JointInPart[I],
                ChassisFirst ? Setup.JointInPart[I] : Setup.JointInChassis[I], 2.0, 0, true, Setup.BaseProfile);
            AllJoints &= WheelSettingsKnown[I];
        }
    }
    Out.bEightJointIdentities = AllJoints;
    bool Closed = Finite && AllJoints;
    for (int32 L = 0; L < 2; ++L)
    {
        const int32 I = L + 6;
        if (!Actual[I] || !Rigid[0] || !Rigid[I + 1]) { Closed = false; continue; }
        const auto Pair = Actual[I]->GetConstrainedParticles();
        const bool ChassisFirst = Pair[0] == Rigid[0];
        const FTransform A = Setup.JointInChassis[I] * Poses[0];
        const FTransform B = Setup.JointInPart[I] * Poses[I + 1];
        const FVector Axis = A.GetUnitAxis(EAxis::X);
        const double Rate = FVector::DotProduct(FVector(Rigid[I + 1]->W() - Rigid[0]->W()), Axis);
        Closed &= DDSJointSettingsMatch(Actual[I]->GetSettings(), I,
            ChassisFirst ? Setup.JointInChassis[I] : Setup.JointInPart[I],
            ChassisFirst ? Setup.JointInPart[I] : Setup.JointInChassis[I], 0, 0, true, Setup.BaseProfile)
            && MAYALatchedDoorDrive::HasClosedPose(true, A.GetRotation().AngularDistance(B.GetRotation()), Rate);
    }
    FVector Axes[6];
    for (int32 W = 0; W < 6; ++W)
    {
        Axes[W] = (Setup.JointInChassis[W] * Poses[0]).GetUnitAxis(EAxis::X);
        Out.WheelAxes[W] = Axes[W];
        Out.Operands.RelativeWheelOmegaRadps[W] = Rigid[0] && Rigid[W + 1]
            ? FVector::DotProduct(FVector(Rigid[W + 1]->W() - Rigid[0]->W()), Axes[W]) : 0;
        Finite &= FiniteVector(Axes[W]) && FMath::IsFinite(Out.Operands.RelativeWheelOmegaRadps[W]);
    }
    if (Rigid[0])
    {
        Out.ChassisVelocityCmps = FVector(Rigid[0]->V());
        const double Speed = Out.ChassisVelocityCmps.Size();
        Out.bChassisSpeedKnown = FiniteVector(Out.ChassisVelocityCmps) && FMath::IsFinite(Speed) && Speed >= 0;
        Out.Operands.bChassisSpeedKnown = Out.bChassisSpeedKnown;
        if (Out.bChassisSpeedKnown) Out.Operands.ChassisSpeedCmps = Speed;
    }
    Out.bNativeStateFinite = Finite; Out.bActualClosedLocks = Closed;
    Out.Operands.bNativeStateFinite = Finite && AllJoints;
    Out.Operands.bActualClosedLocks = Closed;
    Out.Decision = MAYAWheelFeedback::Evaluate(Out.Operands, Setup.BaseProfile);
    bool Brakes = true;
    bool BrakeKnown[6] = {};
    for (int32 W = 0; W < 6; ++W)
    {
        const bool Known = WheelSettingsKnown[W] && ConfigureBrake(Actual[W], Out.Decision.bBrakeEnabled, Setup.BaseProfile);
        BrakeKnown[W] = Known;
        Brakes &= Known; Out.NativeBrakeEnabled[W] = Known && Out.Decision.bBrakeEnabled;
    }
    if (!Brakes)
    {
        Out.Operands.bNativeStateFinite = false;
        Out.Decision = MAYAWheelFeedback::Evaluate(Out.Operands, Setup.BaseProfile);
        for (int32 W = 0; W < 6; ++W)
        {
            BrakeKnown[W] = WheelSettingsKnown[W] && ConfigureBrake(Actual[W], true, Setup.BaseProfile);
            Out.NativeBrakeEnabled[W] = BrakeKnown[W];
        }
    }
    Out.bWheelBrakesVerified = Brakes;
    Out.CalculatedDecision = Out.Decision;
    for (int32 W = 0; W < 6; ++W)
    {
        Out.AppliedPairKnown[W] = Rigid[0] && Rigid[W + 1] && FiniteVector(Axes[W])
            && Actual[W] && WheelSettingsKnown[W] && BrakeKnown[W];
        if (Out.AppliedPairKnown[W])
            ApplyPair(Evolution, Rigid[W + 1], Rigid[0], Axes[W]
                * ((Out.Decision.MotorNm[W] + Out.Decision.BearingNm[W]) * DDSRobotUnits::UETorquePerNm));
        else Out.Decision.MotorNm[W] = Out.Decision.BearingNm[W] = 0;
    }
    Out.Decision.bBoostActive &= MAYADeliveryBoostProfile::HasActualMotorTorque(Out.Decision.MotorNm);
    Out.Decision.EffortFraction = MAYADeliveryBoostProfile::ActualMotorEffortFraction(
        Out.Decision.MotorNm, Out.Decision.SelectedPeakNm, Out.Decision.bBoostActive);
}
