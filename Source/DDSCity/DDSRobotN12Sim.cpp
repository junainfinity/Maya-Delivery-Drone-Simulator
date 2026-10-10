#include "DDSRobotSimCallback.h"
#include "DDSRobotJointAdmission.h"
#include "PBDRigidsSolver.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/Collision/SpatialAccelerationBroadPhase.h"
#include "Chaos/Collision/CollisionConstraintFlags.h"
#include "HAL/PlatformTime.h"

namespace
{
FTransform DDSN12ParticlePose(const Chaos::FGeometryParticleHandle* H) { return FTransform(FQuat(H->R()),FVector(H->X())); }
double Twist(const FQuat& A,const FQuat& B)
{ FQuat Q=A.Inverse()*B; Q.Normalize(); return FMath::UnwindRadians(2*FMath::Atan2(Q.X,Q.W)); }
void Wake(Chaos::FPBDRigidsEvolutionGBF& E,Chaos::FPBDRigidParticleHandle* R)
{ if (R->ObjectState()==Chaos::EObjectStateType::Sleeping) E.SetParticleObjectState(R,Chaos::EObjectStateType::Dynamic); }
FTransform DynamicFrame(const FDDSRobotSimSetup& S,int32 I)
{
    FTransform A=S.JointInChassisCm[I];
    A.SetRotation(A.GetRotation()*FQuat(FVector::ForwardVector,S.Mechanics.OpenAngleRad[I-6]/2));
    return A;
}
bool FixedSettings(const Chaos::FPBDJointSettings& S,const FTransform& A,const FTransform& B,bool Exclude)
{
    if (!FVector(S.ConnectorTransforms[0].GetTranslation()).Equals(A.GetLocation(),1.e-5)
        || !FQuat(S.ConnectorTransforms[0].GetRotation()).Equals(A.GetRotation(),1.e-6)
        || !FVector(S.ConnectorTransforms[1].GetTranslation()).Equals(B.GetLocation(),1.e-5)
        || !FQuat(S.ConnectorTransforms[1].GetRotation()).Equals(B.GetRotation(),1.e-6)
        || S.bProjectionEnabled || S.bShockPropagationEnabled || S.bMassConditioningEnabled
        || !FMath::IsNearlyEqual(S.ParentInvMassScale,1.0,1.e-6) || S.bCollisionEnabled!=!Exclude) return false;
    for(int32 I=0;I<3;++I) if(S.LinearMotionTypes[I]!=Chaos::EJointMotionType::Locked
        || S.AngularMotionTypes[I]!=Chaos::EJointMotionType::Locked
        || S.bLinearPositionDriveEnabled[I] || S.bLinearVelocityDriveEnabled[I]) return false;
    return !S.bAngularSLerpPositionDriveEnabled && !S.bAngularSLerpVelocityDriveEnabled
        && !S.bAngularTwistPositionDriveEnabled && !S.bAngularTwistVelocityDriveEnabled
        && !S.bAngularSwingPositionDriveEnabled && !S.bAngularSwingVelocityDriveEnabled;
}
}
bool FDDSRobotSimCallback::N12JointMatches(const Chaos::FPBDJointSettings& S,int32 I) const
{
    auto Expected=S;
    if (I>=6 && bLidUnlocked[I-6])
    {
        if (S.AngularMotionTypes[0]!=Chaos::EJointMotionType::Limited
            || !FMath::IsNearlyEqual(S.AngularLimits[0],FMath::Abs(Setup.Mechanics.OpenAngleRad[I-6])/2,1.e-6)
            || !FVector(S.ConnectorTransforms[0].GetTranslation()).Equals(Setup.JointInChassisCm[I].GetLocation(),1.e-5)
            || !FQuat(S.ConnectorTransforms[0].GetRotation()).Equals(DynamicFrame(Setup,I).GetRotation(),1.e-6)) return false;
        Expected.AngularMotionTypes[0]=Chaos::EJointMotionType::Locked;
        Expected.ConnectorTransforms[0]=Chaos::FRigidTransform3(Setup.JointInChassisCm[I]);
    }
    return DDSJointSettingsMatch(Expected,I,Setup.JointInChassisCm[I],Setup.JointInPartCm[I],
        Setup.JointTravelCm[I],Setup.JointTargetCm[I],Setup.JointExcludeCollision[I],Setup.Profile);
}
bool FDDSRobotSimCallback::CreateParcelJoint_Internal(const Chaos::FPBDJointSettings& Settings)
{
    if (ParcelJoint) return false;
    auto* CallbackSolver=static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
    auto& Evolution=*CallbackSolver->GetEvolution();
    auto* A=Bodies[0].GetHandle(); auto* B=Parcel.GetHandle();
    if (!A || !B || A==B || !A->CastToRigidParticle() || !B->CastToRigidParticle()) return false;
    ParcelJointEndpoints[0]=A->WeakParticleHandle(); ParcelJointEndpoints[1]=B->WeakParticleHandle();
    ParcelJoint=Evolution.GetJointConstraints().AddConstraint({A,B},Settings);
    if (!ParcelJoint) { ParcelJointEndpoints[0]=ParcelJointEndpoints[1]=Chaos::FWeakParticleHandle(); return false; }
    // Same endpoint/filter side effects as installed JointConstraintProxy, owned once.
    A->AddConstraintHandle(ParcelJoint); bParcelEndpointRegistered[0]=true;
    B->AddConstraintHandle(ParcelJoint); bParcelEndpointRegistered[1]=true;
    Chaos::FGenericParticleHandle(A)->SetInertiaConditioningDirty();
    Chaos::FGenericParticleHandle(B)->SetInertiaConditioningDirty();
    auto& Ignore=Evolution.GetBroadPhase().GetIgnoreCollisionManager();
    if (!Settings.bCollisionEnabled)
    { Ignore.AddIgnoreCollisions(A,B); bParcelIgnorePairOwned=true; }
    const auto Pair=ParcelJoint->GetConstrainedParticles();
    const bool Valid=ParcelJoint->IsValid() && ParcelJoint->IsConstraintEnabled() && !ParcelJoint->IsConstraintBroken()
        && Pair[0]==A && Pair[1]==B && A->ParticleConstraints().Contains(ParcelJoint) && B->ParticleConstraints().Contains(ParcelJoint)
        && (Settings.bCollisionEnabled ? !bParcelIgnorePairOwned : bParcelIgnorePairOwned && Ignore.IgnoresCollision(A,B))
        && FixedSettings(ParcelJoint->GetSettings(),DDSPhysicalCm(Setup.Mechanics.MountInChassisMeters),
            DDSPhysicalCm(Setup.Mechanics.MountInParcelMeters),Setup.Mechanics.bMountCollisionExclusionMeasured);
    if (!Valid) RemoveParcelJoint_Internal(); // Partial construction uses the identical teardown.
    return Valid;
}
void FDDSRobotSimCallback::RemoveParcelJoint_Internal()
{
    if (!ParcelJoint) return; // Idempotent detach/partial setup/retirement.
    auto* CallbackSolver=static_cast<Chaos::FPBDRigidsSolver*>(GetSolver()); auto& Evolution=*CallbackSolver->GetEvolution();
    auto* A=ParcelJointEndpoints[0].GetHandle(); auto* B=ParcelJointEndpoints[1].GetHandle();
    // The existing live scene/body lease fences callback cleanup before body destruction.
    if (bParcelIgnorePairOwned)
    { check(A && B); Evolution.GetBroadPhase().GetIgnoreCollisionManager().RemoveIgnoreCollisions(A,B); bParcelIgnorePairOwned=false; }
    if (A && bParcelEndpointRegistered[0]) { A->RemoveConstraintHandle(ParcelJoint); Chaos::FGenericParticleHandle(A)->SetInertiaConditioningDirty(); }
    if (B && bParcelEndpointRegistered[1]) { B->RemoveConstraintHandle(ParcelJoint); Chaos::FGenericParticleHandle(B)->SetInertiaConditioningDirty(); }
    bParcelEndpointRegistered[0]=bParcelEndpointRegistered[1]=false;
    // Graph unlink MUST precede handle destruction; handles compact by current index.
    Evolution.RemoveConstraintFromConstraintGraph(ParcelJoint);
    Evolution.GetJointConstraints().RemoveConstraint(ParcelJoint->GetConstraintIndex());
    ParcelJoint=nullptr; ParcelJointEndpoints[0]=ParcelJointEndpoints[1]=Chaos::FWeakParticleHandle();
}
void FDDSRobotSimCallback::ReleaseN12_Internal()
{
    RemoveParcelJoint_Internal();
    Parcel=Chaos::FWeakParticleHandle();
}
void FDDSRobotSimCallback::StepN12_Internal(FDDSRobotSnapshot& Out,const FDDSN12StepCommand* Input)
{
    // Linearize cancellation with this fixed-size physical command execution.
    FScopeLock ExecutionLock(&Setup.Control->Mutex);
    auto& N=Out.Mechanics; const auto Gate=Setup.Control->Current; const auto& C=Setup.Mechanics;
    N.bDriveInhibited=Gate.bMechanicsDriveLocked;
    if (!C.bFrozen) return;
    auto* CallbackSolver=static_cast<Chaos::FPBDRigidsSolver*>(GetSolver()); auto& E=*CallbackSolver->GetEvolution();
    auto& J=E.GetJointConstraints();
    const double Now=FPlatformTime::Seconds();
    if (Input && Input->OperationEpoch==Gate.MechanicsEpoch && Input->PhysicsGeneration==Generation
        && Input->WorldEpoch==Gate.WorldEpoch && Input->ReadinessRevision==Gate.Revision
        && Input->Sequence>N12Command.Sequence) N12Command=*Input;
    const double Age=Now-N12Command.IssuedRealS;
    const bool Fresh=N12Command.OperationEpoch==Gate.MechanicsEpoch && N12Command.PhysicsGeneration==Generation
        && N12Command.WorldEpoch==Gate.WorldEpoch && N12Command.ReadinessRevision==Gate.Revision
        && Age>=0 && Age<=C.MaxCommandAgeS && Gate.bReady && !Gate.bRetiring;
    N.OperationEpoch=Gate.MechanicsEpoch; N.CommandSequence=N12Command.Sequence;
    // Actual chassis/support stays eligible while lids move; physical closure is separately gated.
    auto* PH=Parcel.GetHandle(); auto* PR=PH ? PH->CastToRigidParticle() : nullptr;
    if (!PR || PH->UniqueIdx().Idx!=Setup.ParcelParticleId || !PR->GravityEnabled()
        || (PR->ObjectState()!=Chaos::EObjectStateType::Dynamic && PR->ObjectState()!=Chaos::EObjectStateType::Sleeping))
    { N.bDriveInhibited=true; return; }
    // Admit actual tenth-body state before target, force, constraint or evidence arithmetic.
    const FTransform ParcelPose=DDSN12ParticlePose(PH);
    const FVector ParcelV=FVector(PR->V()), ParcelW=FVector(PR->W());
    const double ParcelMass=PR->M(), ParcelInvMass=PR->InvM();
    const FVector ParcelCOM=FVector(PR->CenterOfMass()), ParcelI=FVector(PR->I()), ParcelInvI=FVector(PR->InvI());
    const FQuat ParcelAxes=FQuat(PR->RotationOfMass());
    const FVector ParcelGravity=FVector(E.GetGravityForces().GetAcceleration(PR->GravityGroupIndex()));
    bool ParcelAdmitted=!ParcelPose.ContainsNaN() && ParcelPose.GetRotation().IsNormalized()
        && !ParcelV.ContainsNaN() && !ParcelW.ContainsNaN() && FMath::IsFinite(ParcelMass) && ParcelMass>0
        && FMath::IsFinite(ParcelInvMass) && ParcelInvMass>0 && FMath::IsNearlyEqual(ParcelMass,Setup.ParcelMassKg,1.e-4)
        && FMath::IsNearlyEqual(ParcelInvMass,1.0/ParcelMass,1.e-6)
        && !ParcelCOM.ContainsNaN() && ParcelCOM.Equals(Setup.ParcelCOMCm,1.e-4)
        && !ParcelI.ContainsNaN() && ParcelI.GetMin()>0 && !ParcelInvI.ContainsNaN() && ParcelInvI.GetMin()>0
        && ParcelI.X<=ParcelI.Y+ParcelI.Z && ParcelI.Y<=ParcelI.X+ParcelI.Z && ParcelI.Z<=ParcelI.X+ParcelI.Y
        && !ParcelAxes.ContainsNaN() && ParcelAxes.IsNormalized() && ParcelAxes.Equals(Setup.ParcelPrincipalAxes,1.e-6)
        && !ParcelGravity.ContainsNaN() && ParcelGravity.Equals(FVector(0,0,-Setup.Profile.GravityMps2*100),1.e-4);
    for(int32 Axis=0;Axis<3 && ParcelAdmitted;++Axis)
        ParcelAdmitted=FMath::IsNearlyEqual(ParcelI[Axis],Setup.ParcelInertiaKgCm2[Axis],FMath::Max(1.e-4,FMath::Abs(Setup.ParcelInertiaKgCm2[Axis])*1.e-5))
            && FMath::IsNearlyEqual(ParcelInvI[Axis],1.0/ParcelI[Axis],FMath::Max(1.e-8,FMath::Abs(1.0/ParcelI[Axis])*1.e-5));
    if (!ParcelAdmitted) { N.bKnown=false; N.bParcelLive=false; N.bDriveInhibited=true; return; }
    N.bParcelLive=true; N.ParcelParticleId=PH->UniqueIdx().Idx; N.ParcelWorldCm=ParcelPose;
    N.ParcelMassKg=ParcelMass; N.ParcelVelocityCmps=ParcelV; N.ParcelOmegaRadps=ParcelW;
    const bool Actuate=Fresh && Out.bChassisSupportStable && Out.bAllJointsAdmitted && Gate.bMechanicsDriveLocked;
    bool LidKnown=Out.bCompleteState && Out.bAllJointsAdmitted;
    for(int32 L=0;L<2;++L)
    {
        const int32 I=L+6; auto* H=J.GetConstraintHandles().IsValidIndex(JointIndices[I]) ? J.GetConstraintHandles()[JointIndices[I]] : nullptr;
        auto* R=Bodies[I+1].GetHandle()->CastToRigidParticle(); auto* Ch=Bodies[0].GetHandle()->CastToRigidParticle();
        if(!H || !H->IsValid() || !H->IsConstraintEnabled() || H->IsConstraintBroken()) { LidKnown=false; continue; }
        const FTransform A=Setup.JointInChassisCm[I]*Out.Bodies[0].WorldCm;
        const FTransform B=Setup.JointInPartCm[I]*Out.Bodies[I+1].WorldCm;
        const FVector Axis=A.GetRotation().RotateVector(FVector::ForwardVector);
        auto& Ls=N.Lids[L]; Ls.AngleRad=Twist(A.GetRotation(),B.GetRotation());
        Ls.OmegaRadps=FVector::DotProduct(Out.Bodies[I+1].AngularVelocityRadps-Out.Bodies[0].AngularVelocityRadps,Axis);
        Ls.bAtClosedPose=A.GetRotation().AngularDistance(B.GetRotation())<=Setup.ClosedLidAngleToleranceRad
            && (Out.Bodies[I+1].AngularVelocityRadps-Out.Bodies[0].AngularVelocityRadps).Size()<=Setup.ClosedLidOmegaToleranceRadps;
        Ls.bAtOpenPose=FMath::Abs(Ls.AngleRad-C.OpenAngleRad[L])<=C.OpenAngleToleranceRad
            && FMath::Abs(Ls.OmegaRadps)<=C.OpenOmegaToleranceRadps;
        Ls.bLatchConstraintObserved=!bLidUnlocked[L] && N12JointMatches(H->GetSettings(),I) && Ls.bAtClosedPose;
        if (Actuate && N12Command.bOpen && !bLidUnlocked[L])
        {
            auto Settings=H->GetSettings(); Settings.ConnectorTransforms[0]=Chaos::FRigidTransform3(DynamicFrame(Setup,I));
            Settings.AngularMotionTypes[0]=Chaos::EJointMotionType::Limited;
            Settings.AngularLimits[0]=FMath::Abs(C.OpenAngleRad[L])/2;
            H->SetSettings(Settings); bLidUnlocked[L]=true; Ls.bLatchConstraintObserved=false;
        }
        if (Actuate && !N12Command.bOpen && bLidUnlocked[L] && Ls.bAtClosedPose)
        {
            auto Settings=H->GetSettings(); Settings.ConnectorTransforms[0]=Chaos::FRigidTransform3(Setup.JointInChassisCm[I]);
            Settings.AngularMotionTypes[0]=Chaos::EJointMotionType::Locked;
            H->SetSettings(Settings); bLidUnlocked[L]=false;
            // New lock must survive a completed evolution before it is evidence.
            Ls.bLatchConstraintObserved=false;
        }
        if (bLidUnlocked[L])
        {
            const double Target=Actuate ? (N12Command.bOpen ? C.OpenAngleRad[L] : 0.0) : Ls.AngleRad;
            Ls.TorqueNm=FMath::Clamp(C.LidKpNmPerRad*(Target-Ls.AngleRad)-C.LidKdNmPerRadps*Ls.OmegaRadps,-C.LidPeakNm,C.LidPeakNm);
            Wake(E,R); Wake(E,Ch); R->AddTorque(Chaos::FVec3(Axis*Ls.TorqueNm*10000),false);
            Ch->AddTorque(Chaos::FVec3(-Axis*Ls.TorqueNm*10000),false);
        }
    }
    const auto Mount=DDSPhysicalCm(C.MountInChassisMeters)*Out.Bodies[0].WorldCm;
    const FVector ChCOM=Out.Bodies[0].WorldCm.TransformPosition(Out.Bodies[0].COMLocalCm);
    N.MountVelocityCmps=Out.Bodies[0].VelocityCmps+FVector::CrossProduct(Out.Bodies[0].AngularVelocityRadps,Mount.GetLocation()-ChCOM);
    N.MountOmegaRadps=Out.Bodies[0].AngularVelocityRadps;
    N.bParcelMounted=ParcelJoint!=nullptr;
    if (ParcelJoint)
    {
        const auto P=ParcelJoint->GetConstrainedParticles();
        N.bParcelConstraintObserved=ParcelJoint->IsValid() && ParcelJoint->IsConstraintEnabled() && !ParcelJoint->IsConstraintBroken()
            && P[0]==Bodies[0].GetHandle() && P[1]==PH
            && bParcelEndpointRegistered[0] && bParcelEndpointRegistered[1]
            && P[0]->ParticleConstraints().Contains(ParcelJoint) && P[1]->ParticleConstraints().Contains(ParcelJoint)
            && (C.bMountCollisionExclusionMeasured ? bParcelIgnorePairOwned && E.GetBroadPhase().GetIgnoreCollisionManager().IgnoresCollision(P[0],P[1]) : !bParcelIgnorePairOwned)
            && FixedSettings(ParcelJoint->GetSettings(),DDSPhysicalCm(C.MountInChassisMeters),DDSPhysicalCm(C.MountInParcelMeters),C.bMountCollisionExclusionMeasured);
        if(!N.bParcelConstraintObserved) N.bDriveInhibited=true;
    }
    N.BoundarySequence=N12BoundarySequence; N.BoundaryAppliedStep=N12MutationStep;
    N.BoundaryParcelVelocityCmps=BoundaryWitness.BoundaryParcelVelocityCmps;
    N.BoundaryParcelOmegaRadps=BoundaryWitness.BoundaryParcelOmegaRadps;
    N.BoundaryMountVelocityCmps=BoundaryWitness.BoundaryMountVelocityCmps;
    N.BoundaryMountOmegaRadps=BoundaryWitness.BoundaryMountOmegaRadps;
    N.bBoundaryCompleted=N12BoundaryEpoch==Gate.MechanicsEpoch && N12BoundarySequence>0 && Out.StepSequence>N12MutationStep && Out.bCompleteState
        && ((N12Command.ParcelAction==EDDSN12ParcelAction::Attach && N.bParcelConstraintObserved)
            || (N12Command.ParcelAction==EDDSN12ParcelAction::Detach && !N.bParcelMounted));
    const auto Target=N12Command.ParcelTargetWorldCm;
    N.bParcelAtTarget=N.ParcelWorldCm.GetLocation().Equals(Target.GetLocation(),C.MountPositionToleranceM*100)
        && N.ParcelWorldCm.GetRotation().AngularDistance(Target.GetRotation())<=C.MountAngleToleranceRad
        && (N.ParcelVelocityCmps-N12Command.ParcelTargetVelocityCmps).Size()<=C.MountSpeedToleranceMps*100
        && (N.ParcelOmegaRadps-N12Command.ParcelTargetOmegaRadps).Size()<=C.MountOmegaToleranceRadps;
    const bool BothOpen=N.Lids[0].bAtOpenPose && N.Lids[1].bAtOpenPose;
    if (Actuate && BothOpen && N12Command.Sequence>N12AppliedSequence)
    {
        bool Changed=false;
        if (N12Command.ParcelAction==EDDSN12ParcelAction::Attach && !ParcelJoint)
        {
            const auto PMount=DDSPhysicalCm(C.MountInParcelMeters)*N.ParcelWorldCm;
            const FVector ParcelCOMWorld=N.ParcelWorldCm.TransformPosition(ParcelCOM);
            const FVector ParcelMountV=N.ParcelVelocityCmps+FVector::CrossProduct(N.ParcelOmegaRadps,PMount.GetLocation()-ParcelCOMWorld);
            const bool Coincident=Mount.GetLocation().Equals(PMount.GetLocation(),C.MountPositionToleranceM*100)
                && Mount.GetRotation().AngularDistance(PMount.GetRotation())<=C.MountAngleToleranceRad
                && (ParcelMountV-N.MountVelocityCmps).Size()<=C.MountSpeedToleranceMps*100
                && (N.ParcelOmegaRadps-N.MountOmegaRadps).Size()<=C.MountOmegaToleranceRadps;
            if (Coincident)
            {
                Chaos::FPBDJointSettings Settings;
                Settings.ConnectorTransforms={Chaos::FRigidTransform3(DDSPhysicalCm(C.MountInChassisMeters)),Chaos::FRigidTransform3(DDSPhysicalCm(C.MountInParcelMeters))};
                for(int32 I=0;I<3;++I) { Settings.LinearMotionTypes[I]=Chaos::EJointMotionType::Locked; Settings.AngularMotionTypes[I]=Chaos::EJointMotionType::Locked; }
                Settings.bProjectionEnabled=false; Settings.bShockPropagationEnabled=false; Settings.bMassConditioningEnabled=false;
                Settings.ParentInvMassScale=1; Settings.bCollisionEnabled=!C.bMountCollisionExclusionMeasured;
                Changed=CreateParcelJoint_Internal(Settings);
            }
        }
        else if(N12Command.ParcelAction==EDDSN12ParcelAction::Detach && ParcelJoint && N.bParcelConstraintObserved)
        { RemoveParcelJoint_Internal(); Changed=true; }
        if(Changed)
        {
            N12AppliedSequence=N12BoundarySequence=N12Command.Sequence; N12MutationStep=Out.StepSequence;
            N.BoundaryParcelVelocityCmps=N.ParcelVelocityCmps; N.BoundaryParcelOmegaRadps=N.ParcelOmegaRadps;
            N.BoundaryMountVelocityCmps=N.MountVelocityCmps; N.BoundaryMountOmegaRadps=N.MountOmegaRadps;
            BoundaryWitness=N; N12BoundaryEpoch=Gate.MechanicsEpoch;
            // No body velocity or pose assignment at either boundary.
        }
    }
    if (Actuate && BothOpen && N12Command.ParcelAction==EDDSN12ParcelAction::Transfer
        && N12Command.bExternalWorkDeclared && !ParcelJoint)
    {
        FVector Force=-Out.ActualGravityCmps2*N.ParcelMassKg/100
            +C.ParcelKpNPerM*(Target.GetLocation()-N.ParcelWorldCm.GetLocation())/100
            +C.ParcelKdNPerMps*(N12Command.ParcelTargetVelocityCmps-N.ParcelVelocityCmps)/100;
        Force=Force.GetClampedToMaxSize(C.ParcelForceLimitN);
        FQuat Q=Target.GetRotation()*N.ParcelWorldCm.GetRotation().Inverse(); Q.Normalize(); if(Q.W<0) Q=Q*-1;
        FVector Axis; double Angle; Q.ToAxisAndAngle(Axis,Angle);
        FVector Torque=C.ParcelKpNmPerRad*Axis*Angle+C.ParcelKdNmPerRadps*(N12Command.ParcelTargetOmegaRadps-N.ParcelOmegaRadps);
        Torque=Torque.GetClampedToMaxSize(C.ParcelTorqueLimitNm);
        Wake(E,PR); PR->AddForce(Chaos::FVec3(Force*100),false); PR->AddTorque(Chaos::FVec3(Torque*10000),false);
        N.ExternalForceN=Force; N.ExternalTorqueNm=Torque;
        N.ExternalPowerW=FVector::DotProduct(Force,N.ParcelVelocityCmps/100)+FVector::DotProduct(Torque,N.ParcelOmegaRadps);
    }
    N.bKnown=LidKnown && Out.bCompleteState && FMath::IsNearlyEqual(N.ParcelMassKg,Setup.ParcelMassKg,1.e-4);
    if (N.bParcelMounted && N.bParcelConstraintObserved)
    {
        const FVector OldCOM=Out.AssemblyCOMWorldCm;
        const FVector ParcelCOMWorld=N.ParcelWorldCm.TransformPosition(ParcelCOM);
        const double OldMass=Out.TotalMassKg;
        Out.AssemblyCOMWorldCm=(OldCOM*OldMass+ParcelCOMWorld*N.ParcelMassKg)/(OldMass+N.ParcelMassKg);
        const FQuat Rotation=N.ParcelWorldCm.GetRotation()*ParcelAxes;
        const FVector Axes[3]={Rotation.RotateVector(FVector::ForwardVector),Rotation.RotateVector(FVector::RightVector),Rotation.RotateVector(FVector::UpVector)};
        const FVector A=OldCOM-Out.AssemblyCOMWorldCm, B=ParcelCOMWorld-Out.AssemblyCOMWorldCm;
        for(int32 I=0;I<3;++I) for(int32 K=0;K<3;++K)
        {
            Out.AssemblyInertiaWorldKgCm2[I][K]+=OldMass*((I==K ? A.SizeSquared() : 0)-A[I]*A[K]);
            Out.AssemblyInertiaWorldKgCm2[I][K]+=N.ParcelMassKg*((I==K ? B.SizeSquared() : 0)-B[I]*B[K]);
            for(int32 Axis=0;Axis<3;++Axis) Out.AssemblyInertiaWorldKgCm2[I][K]+=ParcelI[Axis]*Axes[Axis][I]*Axes[Axis][K];
        }
        Out.TotalMassKg+=N.ParcelMassKg;
    }
}
