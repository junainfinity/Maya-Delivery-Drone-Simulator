#include "DDSRobotPhysicsRigComponent.h"
#include "DDSRobotSimCallback.h"
#include "DDSRobotControlBarrier.h"
#include "HAL/PlatformTime.h"

uint64 UDDSRobotPhysicsRigComponent::RevokeN12Operation()
{
    check(IsInGameThread());
    if (!Control || State!=EDDSRobotRigState::Active || !Measured.Mechanics.bFrozen) return 0;
    FScopeLock Lock(&Control->Mutex);
    N12Epoch=++Control->Current.MechanicsEpoch;
    Control->Current.bMechanicsDriveLocked=true;
    return N12Epoch; // Old queued actuation is revoked before rollback is submitted.
}
bool UDDSRobotPhysicsRigComponent::SubmitN12Command(FDDSN12StepCommand C)
{
    check(IsInGameThread());
    FDDSRobotSnapshot S;
    if (!Callback || bResetPending || !Measured.Mechanics.bFrozen || !BoundParcel || !ConsumeSnapshot(S)
        || C.OperationEpoch!=N12Epoch || C.PhysicsGeneration!=Generation || C.WorldEpoch!=WorldEpoch
        || C.ReadinessRevision!=ReadinessRevision || !FMath::IsFinite(C.IssuedRealS)
        || FPlatformTime::Seconds()<C.IssuedRealS || FPlatformTime::Seconds()-C.IssuedRealS>Measured.Mechanics.MaxCommandAgeS
        || C.ParcelTargetWorldCm.ContainsNaN() || !C.ParcelTargetWorldCm.GetRotation().IsNormalized()
        || !C.ParcelTargetWorldCm.GetScale3D().Equals(FVector::OneVector,1.e-9)
        || C.ParcelTargetVelocityCmps.ContainsNaN() || C.ParcelTargetOmegaRadps.ContainsNaN()
        || ((C.ParcelAction==EDDSN12ParcelAction::Transfer) && !C.bExternalWorkDeclared)) return false;
    C.Sequence=++N12Sequence;
    Callback->GetProducerInputData_External()->Mechanics=C;
    return true;
}
bool UDDSRobotPhysicsRigComponent::ReleaseN12DriveLock(uint64 Epoch)
{
    check(IsInGameThread());
    FDDSRobotSnapshot S;
    if (!Control || Epoch!=N12Epoch || !IsServiceEvidenceFresh(FPlatformTime::Seconds(),S)
        || !S.Mechanics.bKnown || S.Mechanics.OperationEpoch!=Epoch
        || !S.Mechanics.Lids[0].bLatchConstraintObserved || !S.Mechanics.Lids[1].bLatchConstraintObserved) return false;
    FScopeLock Lock(&Control->Mutex);
    if (Control->Current.MechanicsEpoch!=Epoch || Control->Current.bRetiring) return false;
    Control->Current.bMechanicsDriveLocked=false;
    ++Control->Current.MechanicsEpoch; // Reject delayed commands after durable release.
    N12Epoch=Control->Current.MechanicsEpoch;
    return true;
}
