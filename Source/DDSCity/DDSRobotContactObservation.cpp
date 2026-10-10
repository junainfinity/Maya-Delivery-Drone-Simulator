#include "DDSRobotContactObservation.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/ShapeInstance.h"

namespace
{
uint64 Identity(const void* P) { return static_cast<uint64>(reinterpret_cast<UPTRINT>(P)); }
FTransform Pose(const Chaos::FGeometryParticleHandle* H) { return FTransform(FQuat(H->R()), FVector(H->X())); }
FTransform ShapePose(const Chaos::FGeometryParticleHandle* H, const Chaos::FRigidTransform3& Relative)
{ return FTransform(FQuat(Relative.GetRotation()),FVector(Relative.GetTranslation())) * Pose(H); }
FVector CurrentPointVelocity(const Chaos::FGeometryParticleHandle* H, const FVector& Point)
{
    if (const auto* K = H->CastToKinematicParticle())
    {
        const auto* R = H->CastToRigidParticle();
        const FVector COM = R ? Pose(H).TransformPosition(FVector(R->CenterOfMass())) : FVector(H->X());
        return FVector(K->V()) + FVector::CrossProduct(FVector(K->W()),Point-COM);
    }
    return FVector::ZeroVector;
}
bool ShapeLive(const Chaos::FGeometryParticleHandle* H, uint64 ShapeId, uint64 GeometryId)
{
    for (const auto& Shape : H->ShapeInstances())
        if (Identity(Shape.Get()) == ShapeId)
            return Shape->GetSimEnabled() && Identity(Shape->GetGeometry().Get()) == GeometryId;
    return false;
}
}

void FDDSRobotContactObservation::Reset(uint64 WorldEpoch)
{
    Epoch = WorldEpoch;
    for (auto& C : Cache) C = FCache();
}

bool FDDSRobotContactObservation::ValidateSleeping(int32 I, double TimeS,
    const FDDSRobotContactLimits& L, FDDSRobotWheelState& Out) const
{
    const auto& C = Cache[I];
    auto* W = C.Wheel.GetHandle();
    auto* O = C.Other.GetHandle();
    if (!C.bLoaded || !W || !O || W->UniqueIdx().Idx != C.WheelId || O->UniqueIdx().Idx != C.OtherId
        || !ShapeLive(W, C.WheelShape, C.WheelGeometry) || !ShapeLive(O, C.OtherShape, C.OtherGeometry))
        return false;
    const auto* WR = W->CastToRigidParticle();
    const auto* OR = O->CastToRigidParticle();
    // Persistent contact is valid only when both participants cannot change the
    // anchors under active dynamics; a moving kinematic obstacle is not support.
    if (!WR || WR->ObjectState() != Chaos::EObjectStateType::Sleeping
        || (O->ObjectState() != Chaos::EObjectStateType::Static
            && (!OR || OR->ObjectState() != Chaos::EObjectStateType::Sleeping))
        || FVector(WR->V()).Size() > L.SleepingSpeedCmps
        || FVector(WR->W()).Size() > L.SleepingOmegaRadps
        || (OR && (FVector(OR->V()).Size() > L.SleepingSpeedCmps || FVector(OR->W()).Size() > L.SleepingOmegaRadps)))
        return false;
    const FVector A = Pose(W).TransformPosition(C.AnchorInWheelCm);
    const FVector B = Pose(O).TransformPosition(C.AnchorInOtherCm);
    const FVector N = Pose(O).TransformVectorNoScale(C.NormalInOther).GetSafeNormal();
    const FVector CurrentSupportV = CurrentPointVelocity(O,B);
    if (A.ContainsNaN() || B.ContainsNaN() || N.ContainsNaN() || CurrentSupportV.ContainsNaN()
        || (A - B).Size() > L.SleepingAnchorCm || N.Z < L.UpDot) return false;
    Out = C.Evidence;
    Out.Support = EDDSRobotSupport::PersistentSleeping;
    Out.WitnessTimeS = TimeS;
    Out.LastSolvedTimeS = C.LastSolvedS; // Never fabricate a fresh solver impulse.
    Out.ContactPointCm = A;
    Out.ContactNormal = N;
    Out.CurrentSeparationCm = FVector::DotProduct(A-B,N);
    Out.SupportContactVelocityCmps = CurrentSupportV;
    return true;
}

void FDDSRobotContactObservation::CaptureCompleted(const Chaos::FPBDCollisionConstraints& Contacts,
    const Chaos::FWeakParticleHandle (&Bodies)[9], double TimeS, double Dt,
    uint64 WorldEpoch, const FDDSRobotContactLimits& L, FDDSRobotSnapshot& Out)
{
    if (Epoch != WorldEpoch) Reset(WorldEpoch);
    bool Loaded[6] = {};
    bool Seen[6] = {};
    for (auto& W : Out.Wheels) W.Support = EDDSRobotSupport::Unknown;
    if (Dt <= 0) return;
    for (const Chaos::FPBDCollisionConstraint* C : Contacts.GetConstraints())
    {
        if (!C || !C->IsEnabled() || C->GetIsProbe()) continue;
        for (int32 I = 0; I < 6; ++I)
        {
            auto* W = Bodies[I + 1].GetHandle();
            if (!W) continue;
            const int32 Side = C->GetParticle0() == W ? 0 : (C->GetParticle1() == W ? 1 : INDEX_NONE);
            if (Side == INDEX_NONE) continue;
            auto* O = C->GetParticle(1 - Side);
            bool bRigSelf = false;
            for (const auto& B : Bodies) if (B.GetHandle() == O) bRigSelf = true;
            if (!O || bRigSelf) continue;
            const auto* WS = C->GetShape(Side);
            const auto* OS = C->GetShape(1 - Side);
            if (!WS || !OS || !WS->GetSimEnabled() || !OS->GetSimEnabled()) continue;
            for (int32 P = 0; P < C->NumManifoldPoints(); ++P)
            {
                if (!C->IsManifoldPointActive(P)) continue;
                const auto& M = C->GetManifoldPoint(P).ContactPoint;
                const auto& R = C->GetManifoldPointResult(P);
                if (!M.IsSet() || !R.bIsValid) continue;
                Seen[I] = true;
                // Keep collision-detection geometry explicitly RAW. Solve scatter
                // does not refresh cached world transforms. Rebuild PRESENT leaf
                // frames from completed particle pose and exact implicit relative transform.
                const auto& Raw0 = C->GetShapeWorldTransform0();
                const auto& Raw1 = C->GetShapeWorldTransform1();
                const FVector RawN = FVector(Raw1.TransformVector(Chaos::FVec3(M.ShapeContactNormal))) * (Side==0 ? 1.0 : -1.0);
                const FTransform Current0 = ShapePose(C->GetParticle0(),C->GetShapeRelativeTransform0());
                const FTransform Current1 = ShapePose(C->GetParticle1(),C->GetShapeRelativeTransform1());
                const FVector Normal = Current1.TransformVectorNoScale(FVector(M.ShapeContactNormal)).GetSafeNormal() * (Side==0 ? 1.0 : -1.0);
                const FVector Points[2] = {Current0.TransformPosition(FVector(M.ShapeContactPoints[0])),
                    Current1.TransformPosition(FVector(M.ShapeContactPoints[1]))};
                const FVector RawPoints[2] = {FVector(Raw0.TransformPosition(Chaos::FVec3(M.ShapeContactPoints[0]))),
                    FVector(Raw1.TransformPosition(Chaos::FVec3(M.ShapeContactPoints[1])))};
                const FVector Point = Points[Side], OtherPoint = Points[1-Side];
                const double Separation = FVector::DotProduct(Point-OtherPoint,Normal);
                const FVector Impulse = FVector(R.NetImpulse) * (Side==0 ? 1.0 : -1.0);
                const FVector PushOut = FVector(R.NetPushOut) * (Side==0 ? 1.0 : -1.0);
                // Solver load belongs to its RAW normal; current geometry only
                // supplies the present support witness and slip coordinates.
                const double NormalLoadImpulse = FVector::DotProduct(Impulse+PushOut/Dt,RawN);
                if (Point.ContainsNaN() || OtherPoint.ContainsNaN() || Impulse.ContainsNaN() || PushOut.ContainsNaN()
                    || !FMath::IsFinite(NormalLoadImpulse) || !FMath::IsFinite(Separation) || FMath::Abs(Separation)>L.SeparationCm
                    || Normal.ContainsNaN() || Normal.Z < L.UpDot || NormalLoadImpulse < L.MinLoadedImpulseKgCmps) continue;
                auto& E = Out.Wheels[I];
                if (Loaded[I] && FVector::DotProduct(E.SolvedImpulseKgCmps + E.SolvedPushOutKgCm / Dt,
                    E.RawSolveNormal) >= NormalLoadImpulse) continue;
                E.Support = EDDSRobotSupport::SolvedLoaded;
                E.WitnessTimeS = E.LastSolvedTimeS = TimeS;
                E.SupportParticleId = O->UniqueIdx().Idx;
                E.WheelShapeId = Identity(WS); E.SupportShapeId = Identity(OS);
                E.ContactPointCm = Point; E.ContactNormal = Normal;
                E.RawSolveWheelPointCm = RawPoints[Side]; E.RawSolveOtherPointCm = RawPoints[1-Side];
                E.RawSolveNormal = RawN; E.RawSolvePhiCm = M.Phi; E.CurrentSeparationCm = Separation;
                E.SolvedImpulseKgCmps = Impulse; E.SolvedPushOutKgCm = PushOut;
                E.SupportContactVelocityCmps = CurrentPointVelocity(O,OtherPoint);
                auto& K = Cache[I];
                K.bLoaded = true; K.Wheel = W->WeakParticleHandle(); K.Other = O->WeakParticleHandle();
                K.WheelId = W->UniqueIdx().Idx; K.OtherId = O->UniqueIdx().Idx;
                K.WheelShape = Identity(WS); K.OtherShape = Identity(OS);
                K.WheelGeometry = Identity(WS->GetGeometry().Get()); K.OtherGeometry = Identity(OS->GetGeometry().Get());
                K.AnchorInWheelCm = Pose(W).InverseTransformPosition(Point);
                K.AnchorInOtherCm = Pose(O).InverseTransformPosition(OtherPoint);
                K.NormalInOther = Pose(O).InverseTransformVectorNoScale(Normal);
                K.LastSolvedS = TimeS; K.Evidence = E;
                Loaded[I] = true;
            }
        }
    }
    for (int32 I = 0; I < 6; ++I)
    {
        if (!Loaded[I] && !ValidateSleeping(I, TimeS, L, Out.Wheels[I]))
        {
            Out.Wheels[I].Support = Seen[I] ? EDDSRobotSupport::NoLoadedContact : EDDSRobotSupport::Unknown;
            Cache[I].bLoaded = false;
        }
        if (Out.Wheels[I].Support == EDDSRobotSupport::SolvedLoaded
            || Out.Wheels[I].Support == EDDSRobotSupport::PersistentSleeping) Out.SupportMask |= 1 << I;
    }
}
