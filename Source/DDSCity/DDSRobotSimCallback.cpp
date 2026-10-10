#include "DDSRobotSimCallback.h"
#include "PBDRigidsSolver.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDJointConstraints.h"
#include "DDSRobotJointAdmission.h"
#include "HAL/PlatformTime.h"

namespace
{
FTransform BodyPose(const Chaos::FGeometryParticleHandle* H) { return FTransform(FQuat(H->R()), FVector(H->X())); }
double RelativeTwist(const FQuat& A, const FQuat& B)
{
    FQuat Q = A.Inverse() * B;
    Q.Normalize();
    return FMath::UnwindRadians(2.0 * FMath::Atan2(Q.X, Q.W));
}
void TorquePair(Chaos::FPBDRigidsEvolutionGBF& E, Chaos::FPBDRigidParticleHandle* A,
    Chaos::FPBDRigidParticleHandle* B, const FVector& TorqueUE)
{
    if (TorqueUE.IsNearlyZero(1.e-12)) return;
    if (A->ObjectState() == Chaos::EObjectStateType::Sleeping) E.SetParticleObjectState(A, Chaos::EObjectStateType::Dynamic);
    if (B->ObjectState() == Chaos::EObjectStateType::Sleeping) E.SetParticleObjectState(B, Chaos::EObjectStateType::Dynamic);
    A->AddTorque(Chaos::FVec3(TorqueUE), false);
    B->AddTorque(Chaos::FVec3(-TorqueUE), false);
}
}

bool FDDSRobotSimCallback::BindInternal(const FDDSRobotSimSetup& S)
{
    auto* CallbackSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Handles = CallbackSolver->GetParticles().GetParticleHandles();
    Chaos::FWeakParticleHandle Found[9];
    int32 Count = 0;
    // One finite scan when binding a generation, never one all-city scan per step.
    // GT lease keeps these nine bodies registered while IDs are handed over.
    for (int32 I = 0; I < static_cast<int32>(Handles.Size()); ++I)
    {
        auto* H = Handles.Handle(I).Get();
        if (!H) continue;
        for (int32 J = 0; J < 9; ++J)
            if (H->UniqueIdx().Idx == S.ParticleIds[J] && !Found[J].GetHandle())
            { Found[J] = H->WeakParticleHandle(); ++Count; }
    }
    if (Count != 9) return false;
    for (int32 I = 0; I < 9; ++I)
    {
        auto* R = Found[I].GetHandle()->CastToRigidParticle();
        if (!R || (R->ObjectState() != Chaos::EObjectStateType::Dynamic
            && R->ObjectState() != Chaos::EObjectStateType::Sleeping) || !R->GravityEnabled()) return false;
    }
    Setup = S;
    for (int32 I=0; I<9; ++I) Bodies[I] = Found[I];
    if (!S.Control || !ValidateInternalJoints(true)) return false;
    if (S.ParcelParticleId != INDEX_NONE)
    {
        for (int32 I=0; I<static_cast<int32>(Handles.Size()); ++I)
        { auto* H=Handles.Handle(I).Get(); if (H && H->UniqueIdx().Idx==S.ParcelParticleId) Parcel=H->WeakParticleHandle(); }
        auto* H=Parcel.GetHandle(); auto* R=H ? H->CastToRigidParticle() : nullptr;
        if (!R || !R->GravityEnabled() || (R->ObjectState()!=Chaos::EObjectStateType::Dynamic && R->ObjectState()!=Chaos::EObjectStateType::Sleeping)) return false;
        R->SetM(S.ParcelMassKg); R->SetInvM(1.0/S.ParcelMassKg);
        R->SetCenterOfMass(Chaos::FVec3(S.ParcelCOMCm)); R->SetRotationOfMass(Chaos::FRotation3(S.ParcelPrincipalAxes));
        R->SetI(Chaos::TVec3<Chaos::FRealSingle>(S.ParcelInertiaKgCm2));
        R->SetInvI(Chaos::TVec3<Chaos::FRealSingle>(1/S.ParcelInertiaKgCm2.X,1/S.ParcelInertiaKgCm2.Y,1/S.ParcelInertiaKgCm2.Z));
        R->SetInertiaConditioningEnabled(false);
    }
    Generation = S.Generation;
    for (int32 I = 0; I < 9; ++I)
    {
        Bodies[I] = Found[I];
        auto* R = Bodies[I].GetHandle()->CastToRigidParticle();
        const double Mass = I == 0 ? S.Profile.BareChassisKg : (I < 7 ? S.Profile.WheelKg : (I == 7 ? S.Profile.Lid0Kg : S.Profile.Lid1Kg));
        // Exact declared measured mass distribution, no duplicate cargo mass.
        R->SetM(Mass); R->SetInvM(1.0 / Mass);
        R->SetCenterOfMass(Chaos::FVec3(S.COMCm[I]));
        R->SetRotationOfMass(Chaos::FRotation3(S.PrincipalAxes[I]));
        R->SetI(Chaos::TVec3<Chaos::FRealSingle>(S.InertiaKgCm2[I]));
        R->SetInvI(Chaos::TVec3<Chaos::FRealSingle>(
            1.0 / S.InertiaKgCm2[I].X, 1.0 / S.InertiaKgCm2[I].Y, 1.0 / S.InertiaKgCm2[I].Z));
        R->SetInertiaConditioningEnabled(false);
    }
    Contacts.Reset(LastCommand.CollisionWorldGeneration);
    bPreviousStepAdmissible = false;
    StableSinceS = -1;
    for (int32 I = 0; I < 6; ++I) bAngleInitialized[I] = false;
    return true;
}

bool FDDSRobotSimCallback::ValidateInternalJoints(bool bFind)
{
    auto* CallbackSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Handles = CallbackSolver->GetEvolution()->GetJointConstraints().GetConstraintHandles();
    for (int32 I=0; I<8; ++I)
    {
        const auto Matches = [&](int32 Index)
        {
            if (!Handles.IsValidIndex(Index)) return false;
            const auto* H = Handles[Index]; // Current-call handle only; never retained.
            if (!H || !H->IsValid() || !H->IsConstraintEnabled() || H->IsConstraintBroken()) return false;
            const auto Pair = H->GetConstrainedParticles();
            return Pair[0] && Pair[1] && Pair[0] == Bodies[0].GetHandle() && Pair[1] == Bodies[I+1].GetHandle()
                && N12JointMatches(H->GetSettings(), I);
        };
        if (bFind || !Matches(JointIndices[I]))
        {
            JointIndices[I] = INDEX_NONE;
            for (int32 J=0; J<Handles.Num(); ++J) if (Matches(J))
            { if (JointIndices[I] != INDEX_NONE) return false; JointIndices[I]=J; }
        }
        if (!Matches(JointIndices[I])) return false;
    }
    return true;
}

void FDDSRobotSimCallback::OnPreSimulate_Internal()
{
    auto* CallbackSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Evolution = *CallbackSolver->GetEvolution();
    const auto* Input = GetConsumerInput_Internal();
    const double TimeS = GetSimTime_Internal();
    const double Dt = GetDeltaTime_Internal();
    if (Input && Input->bHasSetup && Generation == 0)
        if (!BindInternal(Input->Setup)) return; // Retry only on later real registration state.
    if (Generation == 0) return;
    const auto Gate = Setup.Control->Read();
    if (Gate.bRetiring) return;
    if (AppliedRecoveryGeneration == Generation && RecoveryQualifiedRevision != Gate.Revision)
        RecoveryInhibitGeneration = Generation;
    if (Input && Input->Command.Generation == Generation
        && Input->Command.Sequence >= LastCommand.Sequence)
    {
        if (LastCommand.Sequence && Input->Command.Sequence > LastCommand.Sequence + 1)
            CoalescedCommands += Input->Command.Sequence - LastCommand.Sequence - 1;
        LastCommand = Input->Command;
    }
    auto& Output = GetProducerOutputData_Internal();
    auto& Out = Output.Snapshot;
    Out = FDDSRobotSnapshot();
    Out.Generation = Generation; Out.CollisionWorldGeneration = Gate.WorldEpoch;
    Out.ReadinessRevision = Gate.Revision; Out.bAllJointsAdmitted = ValidateInternalJoints(false);
    Out.bRecoveryDriveInhibited = RecoveryInhibitGeneration == Generation;
    Out.StepSequence = ++StepSequence; Out.CommandSequence = LastCommand.Sequence;
    Out.CoalescedCommandCount = CoalescedCommands;
    Out.CallbackStartTimeS = TimeS; Out.CompletedStateTimeS = TimeS;
    Out.ActualCallbackDtS = Dt; Out.PreviousEvolutionDtS = PreviousDtS;
    Out.CapturedRealS = FPlatformTime::Seconds();
    Out.InputAgeRealS = Out.CapturedRealS - LastCommand.IssuedRealS;
    Out.bCommandFresh = LastCommand.Generation == Generation && LastCommand.CollisionWorldGeneration == Gate.WorldEpoch
        && LastCommand.ReadinessRevision == Gate.Revision && Out.InputAgeRealS >= 0
        && Out.InputAgeRealS <= LastCommand.MaxAgeRealS;
    const double MaxEvolutionDt = CallbackSolver->GetMaxDeltaTime_External();
    Out.bTimingEnvelopeValid = FMath::IsFinite(Dt) && Dt > CallbackSolver->GetMinDeltaTime_External()
        && (MaxEvolutionDt <= 0 || Dt <= MaxEvolutionDt)
        && FMath::IsNearlyEqual(Dt, Setup.Profile.TargetStepS, 1.e-7)
        && FMath::IsNearlyEqual(CallbackSolver->GetAsyncDeltaTime(), Setup.Profile.TargetStepS, 1.e-7);
    Out.bCompleteState = bPreviousStepAdmissible && Out.bAllJointsAdmitted
        && PreviousReadinessRevision == Gate.Revision && FMath::IsNearlyEqual(TimeS, PreviousStartS + PreviousDtS, 1.e-7);
    Out.bWorldReady = Gate.bReady && LastCommand.bCollisionWorldReady
        && LastCommand.ReadinessRevision == Gate.Revision && LastCommand.CollisionWorldGeneration == Gate.WorldEpoch;
    Chaos::FPBDRigidParticleHandle* Rigid[9] = {};
    bool Live = true;
    for (int32 I = 0; I < 9; ++I)
    {
        auto* H = Bodies[I].GetHandle(); // PT-only; engine resets weak handle before destruction.
        if (!H || H->UniqueIdx().Idx != Setup.ParticleIds[I]) { Live = false; continue; }
        auto* R = H->CastToRigidParticle();
        if (!R || (R->ObjectState() != Chaos::EObjectStateType::Dynamic && R->ObjectState() != Chaos::EObjectStateType::Sleeping))
        { Live = false; continue; }
        Rigid[I] = R;
        auto& B = Out.Bodies[I];
        B.bLive = true; B.bSleeping = R->ObjectState() == Chaos::EObjectStateType::Sleeping;
        B.ParticleId = H->UniqueIdx().Idx; B.WorldCm = BodyPose(H);
        B.VelocityCmps = FVector(R->V()); B.AngularVelocityRadps = FVector(R->W());
        if (B.WorldCm.ContainsNaN() || !B.WorldCm.GetRotation().IsNormalized()
            || B.VelocityCmps.ContainsNaN() || B.AngularVelocityRadps.ContainsNaN()) Live = false;
        B.AccelerationCmps2 = Out.bCompleteState && PreviousDtS > 0
            ? (B.VelocityCmps - PreviousVelocityCmps[I]) / PreviousDtS : FVector::ZeroVector;
        B.MassKg = R->M();
        B.COMLocalCm = FVector(R->CenterOfMass()); B.InertiaKgCm2 = FVector(R->I());
        B.PrincipalAxesInBody = FQuat(R->RotationOfMass());
        const FVector G = FVector(Evolution.GetGravityForces().GetAcceleration(R->GravityGroupIndex()));
        if (I == 0) Out.ActualGravityCmps2 = G;
        if (!R->GravityEnabled() || !G.Equals(FVector(0, 0, -Setup.Profile.GravityMps2 * 100.0), 1.e-4))
            Out.bTimingEnvelopeValid = false;
    }
    Out.bCompleteState &= Live;
    if (!Live) { bPreviousStepAdmissible = false; return; }
    for (const auto& B : Out.Bodies)
    {
        Out.TotalMassKg += B.MassKg;
        Out.AssemblyCOMWorldCm += B.WorldCm.TransformPosition(B.COMLocalCm) * B.MassKg;
    }
    Out.AssemblyCOMWorldCm /= Out.TotalMassKg;
    for (const auto& B : Out.Bodies)
    {
        const FQuat Rotation = B.WorldCm.GetRotation() * B.PrincipalAxesInBody;
        const FVector Axis[3] = { Rotation.RotateVector(FVector::ForwardVector),
            Rotation.RotateVector(FVector::RightVector), Rotation.RotateVector(FVector::UpVector) };
        const FVector Offset = B.WorldCm.TransformPosition(B.COMLocalCm) - Out.AssemblyCOMWorldCm;
        for (int32 I = 0; I < 3; ++I)
            for (int32 J = 0; J < 3; ++J)
            {
                for (int32 K = 0; K < 3; ++K)
                    Out.AssemblyInertiaWorldKgCm2[I][J] += B.InertiaKgCm2[K] * Axis[K][I] * Axis[K][J];
                Out.AssemblyInertiaWorldKgCm2[I][J] += B.MassKg
                    * ((I == J ? Offset.SizeSquared() : 0.0) - Offset[I] * Offset[J]);
            }
    }
    // Immutable request does NOT inherit permission from the latest ordinary command.
    if (Input && Input->bApprovedReset && Input->ResetRequestId > LastResetId)
    {
        FScopeLock Lock(&Setup.Control->Mutex);
        const auto& Execution = Setup.Control->Current;
        const double Age = FPlatformTime::Seconds()-Input->ResetValidatedRealS;
        const bool Allowed = Input->ResetOldGeneration == Generation && Execution.Generation == Generation
            && Input->ResetNewGeneration > Generation && Input->ResetWorldEpoch == Execution.WorldEpoch
            && Input->ResetReadinessRevision == Execution.Revision && Execution.bReady && !Execution.bRetiring
            && Input->ResetRequestId > Execution.CancelledResetId && Out.bAllJointsAdmitted
            && Setup.ParcelParticleId == INDEX_NONE && !Setup.Mechanics.bFrozen
            && FMath::IsFinite(Age) && Age >= 0 && Age <= Setup.Profile.RecoveryPermitMaxAgeS;
        LastResetId = Input->ResetRequestId; Output.ResetOldGeneration = Input->ResetOldGeneration;
        if (!Allowed) Output.CancelledResetId = LastResetId;
        else
        {
            // Nine-body reset linearizes with revocation under this short lock.
            for (int32 I=0; I<9; ++I)
            {
                Evolution.SetParticleTransform(Rigid[I], Chaos::FVec3(Input->ResetWorldCm[I].GetLocation()),
                    Chaos::FRotation3(Input->ResetWorldCm[I].GetRotation()), true);
                Rigid[I]->SetV(Chaos::FVec3(0)); Rigid[I]->SetW(Chaos::FVec3(0));
                Rigid[I]->SetAcceleration(Chaos::FVec3(0)); Rigid[I]->SetAngularAcceleration(Chaos::FVec3(0));
                Evolution.SetParticleObjectState(Rigid[I], Chaos::EObjectStateType::Dynamic);
            }
            Generation = Input->ResetNewGeneration; Setup.Generation = Generation;
            AppliedRecoveryGeneration = Generation;
            Setup.Control->Current.Generation = Generation; RecoveryInhibitGeneration = Generation;
            Output.AppliedResetId = LastResetId; Out.Generation = Generation;
            Out.bCompleteState = false; Out.bRecoveryDriveInhibited = true; Out.SupportMask = 0;
            Contacts.Reset(Execution.WorldEpoch); StableSinceS = -1; bPreviousStepAdmissible = false;
            for (int32 I=0; I<6; ++I) bAngleInitialized[I] = false;
            LastCommand.bDriveLocked = true; LastCommand.bBrake = true;
            return; // Ack confirms relocation only. It cannot arm drive.
        }
    }
    if (Out.bCompleteState && Out.bWorldReady)
        Contacts.CaptureCompleted(Evolution.GetCollisionConstraints(), Bodies, TimeS, PreviousDtS,
            Gate.WorldEpoch, Setup.Contact, Out);
    else Contacts.Reset(Gate.WorldEpoch);
    for (int32 I = 0; I < 6; ++I)
    {
        auto& W = Out.Wheels[I];
        const FTransform A = Setup.JointInChassisCm[I] * Out.Bodies[0].WorldCm;
        const FTransform B = Setup.JointInPartCm[I] * Out.Bodies[I + 1].WorldCm;
        const FVector Axis = A.GetRotation().RotateVector(FVector::ForwardVector);
        W.AxleOmegaRadps = FVector::DotProduct(Out.Bodies[I + 1].AngularVelocityRadps - Out.Bodies[0].AngularVelocityRadps, Axis);
        W.SuspensionTravelM = FVector::DotProduct(B.GetLocation() - A.GetLocation(),
            A.GetRotation().RotateVector(FVector::UpVector)) / 100.0;
        const double Angle = RelativeTwist(A.GetRotation(), B.GetRotation());
        if (!bAngleInitialized[I]) { UnwrappedAngle[I] = Angle; bAngleInitialized[I] = true; }
        else UnwrappedAngle[I] += FMath::UnwindRadians(Angle - PreviousWrappedAngle[I]);
        PreviousWrappedAngle[I] = Angle; W.AxleAngleRad = UnwrappedAngle[I];
        // Actual contact velocities include rigid-body angular velocity. Tire slip
        // uses wheel contact velocity; no proxy radius or chassis displacement.
        if ((Out.SupportMask & (1 << I)) != 0)
        {
            const FVector Forward = FVector::CrossProduct(Axis, W.ContactNormal).GetSafeNormal();
            const FVector ChassisCOM = Out.Bodies[0].WorldCm.TransformPosition(Out.Bodies[0].COMLocalCm);
            const FVector WheelCOM = Out.Bodies[I + 1].WorldCm.TransformPosition(Out.Bodies[I + 1].COMLocalCm);
            const FVector GroundFrameV = Out.Bodies[0].VelocityCmps
                + FVector::CrossProduct(Out.Bodies[0].AngularVelocityRadps, W.ContactPointCm - ChassisCOM);
            const FVector WheelPointV = Out.Bodies[I + 1].VelocityCmps
                + FVector::CrossProduct(Out.Bodies[I + 1].AngularVelocityRadps, W.ContactPointCm - WheelCOM);
            const double Speed = FMath::Abs(FVector::DotProduct(GroundFrameV - W.SupportContactVelocityCmps, Forward));
            W.bSlipDefined = Speed >= Setup.SlipDomainCmps;
            if (W.bSlipDefined) W.LongitudinalSlip = FVector::DotProduct(WheelPointV - W.SupportContactVelocityCmps, Forward) / Speed;
        }

    }
    Out.bClosedLidBaseline = true;
    for (int32 I = 6; I < 8; ++I)
    {
        const FTransform A = Setup.JointInChassisCm[I] * Out.Bodies[0].WorldCm;
        const FTransform B = Setup.JointInPartCm[I] * Out.Bodies[I + 1].WorldCm;
        if (A.GetRotation().AngularDistance(B.GetRotation()) > Setup.ClosedLidAngleToleranceRad
            || (Out.Bodies[I + 1].AngularVelocityRadps - Out.Bodies[0].AngularVelocityRadps).Size()
                > Setup.ClosedLidOmegaToleranceRadps) Out.bClosedLidBaseline = false;
    }
    const auto& S = Setup.Profile.Service;
    const FRotator Attitude = Out.Bodies[0].WorldCm.Rotator();
    const bool ChassisStableNow = S.bFrozen && Out.bCompleteState && Out.bTimingEnvelopeValid && Out.bWorldReady
        && FMath::CountBits(static_cast<uint64>(Out.SupportMask)) >= S.MinLoadedWheels
        && Out.Bodies[0].VelocityCmps.Size() <= S.MaxLinearSpeedMps * 100
        && Out.Bodies[0].AngularVelocityRadps.Size() <= S.MaxAngularSpeedRadps
        && FMath::Abs(Out.Bodies[0].VelocityCmps.Z) <= S.MaxVerticalSpeedMps * 100
        && FMath::Abs(FMath::DegreesToRadians(Attitude.Pitch)) <= S.MaxPitchRad
        && FMath::Abs(FMath::DegreesToRadians(Attitude.Roll)) <= S.MaxRollRad;
    if (!ChassisStableNow) ChassisStableSinceS = -1;
    else if (ChassisStableSinceS < 0) ChassisStableSinceS = TimeS;
    Out.bChassisSupportStable = ChassisStableNow && TimeS-ChassisStableSinceS >= S.RequiredStableDurationS;
    const bool StableNow = ChassisStableNow && Out.bClosedLidBaseline;
    if (!StableNow) StableSinceS = -1;
    else if (StableSinceS < 0) StableSinceS = TimeS;
    Out.bServiceStable = StableNow && TimeS - StableSinceS >= S.RequiredStableDurationS;
    if (RecoveryInhibitGeneration == Generation && Out.bServiceStable)
    {
        FScopeLock Lock(&Setup.Control->Mutex);
        const auto& Current = Setup.Control->Current;
        if (Current.Revision == Gate.Revision && Current.WorldEpoch == Gate.WorldEpoch
            && Current.Generation == Generation && Current.bReady && !Current.bRetiring)
        { RecoveryInhibitGeneration = 0; RecoveryQualifiedRevision = Gate.Revision; }
    } // Re-arm is revision-tagged; readiness interruption requires new stabilization.
    Out.bRecoveryDriveInhibited = RecoveryInhibitGeneration == Generation;
    StepN12_Internal(Out, Input ? &Input->Mechanics : nullptr);
    Out.bClosedLidBaseline &= (!Setup.Mechanics.bFrozen || (Out.Mechanics.Lids[0].bLatchConstraintObserved && Out.Mechanics.Lids[1].bLatchConstraintObserved));
    Out.bServiceStable &= Out.bClosedLidBaseline;
    {
    // Linearize the latest mechanics epoch/brake and all six wheel torque pairs.
    FScopeLock ActuationLock(&Setup.Control->Mutex);
    const auto ActuationGate = Setup.Control->Current;
    const bool MechanicsEpochCurrent = !Setup.Mechanics.bFrozen || ActuationGate.MechanicsEpoch == Out.Mechanics.OperationEpoch;
    const bool MechanicsInhibited = ActuationGate.bMechanicsDriveLocked || Out.Mechanics.bDriveInhibited || !MechanicsEpochCurrent;
    Out.Mechanics.bDriveInhibited = MechanicsInhibited;
    const bool CurrentAdmission = ActuationGate.Revision == Gate.Revision && ActuationGate.WorldEpoch == Gate.WorldEpoch
        && ActuationGate.bReady && !ActuationGate.bRetiring && ActuationGate.Generation == Generation && MechanicsEpochCurrent;
    for (int32 I=0; I<6; ++I)
    {
        auto& W = Out.Wheels[I];
        const FTransform A = Setup.JointInChassisCm[I] * Out.Bodies[0].WorldCm;
        const FVector Axis = A.GetRotation().RotateVector(FVector::ForwardVector);
        const double Request = (I%2==0 ? LastCommand.LeftRequest : LastCommand.RightRequest)*Setup.WheelForwardSign[I];
        W.MotorNm = CurrentAdmission && Out.bAllJointsAdmitted && Out.bCommandFresh && Out.bWorldReady
            && Out.bTimingEnvelopeValid && !Out.bRecoveryDriveInhibited && !LastCommand.bDriveLocked && !Out.Mechanics.bDriveInhibited
            ? DDSMotorTorqueNm(Request,W.AxleOmegaRadps,Setup.Profile) : 0.0;
        W.BrakeAndBearingNm = DDSBrakeBearingTorqueNm(LastCommand.bBrake || LastCommand.bDriveLocked || Out.Mechanics.bDriveInhibited
            || !Out.bCommandFresh || !CurrentAdmission || Out.bRecoveryDriveInhibited || !Out.bAllJointsAdmitted,
            W.AxleOmegaRadps,Setup.Profile);
        TorquePair(Evolution,Rigid[I+1],Rigid[0],Axis*((W.MotorNm+W.BrakeAndBearingNm)*DDSRobotUnits::UETorquePerNm));
    }
    } // ActuationLock releases only after the bounded six-wheel torque application.
    PreviousReadinessRevision = Gate.Revision;
    PreviousStartS = TimeS; PreviousDtS = Dt;
    for (int32 I = 0; I < 9; ++I) PreviousVelocityCmps[I] = Out.Bodies[I].VelocityCmps;
    bPreviousStepAdmissible = Out.bTimingEnvelopeValid;
}
