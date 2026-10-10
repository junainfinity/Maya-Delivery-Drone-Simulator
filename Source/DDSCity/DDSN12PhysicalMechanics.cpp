#include "DDSN12PhysicalMechanics.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"

UDDSN12PhysicalMechanics::UDDSN12PhysicalMechanics()
{ PrimaryComponentTick.bCanEverTick=true; PrimaryComponentTick.bTickEvenWhenPaused=true; PrimaryComponentTick.TickGroup=TG_PostPhysics; }
bool UDDSN12PhysicalMechanics::Bind(UDDSN08Coordinator& N08,UDDSRobotPhysicsRigComponent& Rig,
    const TArray<ADDSN09ServiceBay*>& Bays,FEnvironmentReader InReader)
{
    check(IsInGameThread());
    if(bEnding || Phase!=EDDSN12Cycle::Idle || !InReader || Bays.Num()!=2 || N08.GetOwner()!=GetOwner()
        || Rig.GetOwner()!=GetOwner() || !Rig.GetMeasuredBinding().Mechanics.bFrozen) return false;
    TSet<FString> Ids;
    for(auto* B:Bays) if(!IsValid(B) || B->GetWorld()!=GetWorld() || Ids.Contains(B->GetStationId())) return false; else Ids.Add(B->GetStationId());
    Coordinator=&N08; Physics=&Rig; Reader=MoveTemp(InReader); ServiceBays.Reset();
    for(auto* B:Bays) ServiceBays.Add(B);
    AddTickPrerequisiteComponent(&Rig); AddTickPrerequisiteComponent(&N08);
    return N08.BindNativeMechanics(*this,*this,[this](const ADDSN09ServiceBay& B,FDDSN09ExternalCargoDoorObservation& F,FDDSN15ProjectionInputs& V){return Project(B,F,V);});
}
bool UDDSN12PhysicalMechanics::Read(const ADDSN09ServiceBay& Bay,FDDSRobotSnapshot& S,FDDSN12Environment& E) const
{
    if(bEnding || !Coordinator.IsValid() || !Physics.IsValid() || !Reader || !Coordinator->GetAuthority()
        || !Physics->ConsumeSnapshot(S) || !S.Mechanics.bKnown) return false;
    const auto State=Coordinator->GetAuthority()->GetStableSnapshot();
    const auto LocalReader=Reader;
    if(!LocalReader(Bay,S,State,E) || bEnding || !Physics.IsValid() || !Coordinator.IsValid()
        || !Coordinator->GetAuthority() || Coordinator->GetAuthority()->GetStableSnapshot().MissionInstanceId!=State.MissionInstanceId) return false;
    FDDSRobotSnapshot Current;
    if(!Physics->ConsumeSnapshot(Current) || Current.Generation!=S.Generation || Current.StepSequence!=S.StepSequence
        || Current.ReadinessRevision!=S.ReadinessRevision || Current.CollisionWorldGeneration!=S.CollisionWorldGeneration
        || Current.CapturedRealS!=S.CapturedRealS || E.PhysicsGeneration!=S.Generation || E.WorldEpoch!=S.CollisionWorldGeneration
        || E.ReadinessRevision!=S.ReadinessRevision || E.CompletedStep!=S.StepSequence || E.CapturedRealS!=S.CapturedRealS
        || E.MissionId!=State.MissionInstanceId || E.CargoId!=State.CargoId || E.StationId!=Bay.GetStationId()
        || E.ObservedParcelParticleId!=S.Mechanics.ParcelParticleId || E.NativeGeometryRecordSha256.Len()!=64
        || !E.bFullActualLidSweepObserved || !E.bActualParcelPathObserved) return false;
    for(const auto& T:{E.StableSourceParcelWorldCm,E.RecipientParcelWorldCm})
        if(T.ContainsNaN() || !T.GetRotation().IsNormalized() || !T.GetScale3D().Equals(FVector::OneVector,1.e-9)) return false;
    for(TCHAR H:E.NativeGeometryRecordSha256) if(!FChar::IsHexDigit(H)) return false;
    return true;
}
bool UDDSN12PhysicalMechanics::Project(const ADDSN09ServiceBay& Bay,FDDSN09ExternalCargoDoorObservation& F,FDDSN15ProjectionInputs& V) const
{
    FDDSRobotSnapshot S; FDDSN12Environment E; if(!Read(Bay,S,E)) return false;
    const auto State=Coordinator->GetAuthority()->GetStableSnapshot(); const auto& M=S.Mechanics;
    F.bCargoObserved=M.bParcelLive; F.ObservedCargoId=E.CargoId;
    F.bActiveCargoAtExpectedSource=State.Owner==EDDSN08Owner::Robot ? M.bParcelConstraintObserved : E.bParcelAtStableSourceObserved;
    F.bFullActualLidSweepObserved=E.bFullActualLidSweepObserved; F.bFullActualLidSweepClear=E.bFullActualLidSweepClear;
    F.bClosedOperableDoorObserved=M.bKnown;
    F.bClosedOperableDoor=M.Lids[0].bLatchConstraintObserved && M.Lids[1].bLatchConstraintObserved;
    V.bRealN12MechanicsAvailable=true; V.bNativeDoorObservationKnown=M.bKnown;
    V.DoorObservationMissionId=State.MissionInstanceId; V.DoorObservationWorldSeconds=GetWorld()->GetTimeSeconds();
    V.bLeftLidAtClosedPose=M.Lids[0].bAtClosedPose; V.bRightLidAtClosedPose=M.Lids[1].bAtClosedPose;
    V.bLeftLatchVerified=M.Lids[0].bLatchConstraintObserved; V.bRightLatchVerified=M.Lids[1].bLatchConstraintObserved;
    return true;
}
void UDDSN12PhysicalMechanics::SetPhase(EDDSN12Cycle Next)
{ Phase=Next; PhaseStartRealS=FPlatformTime::Seconds(); }
bool UDDSN12PhysicalMechanics::Submit(const FDDSRobotSnapshot& S,bool Open,EDDSN12ParcelAction Action,const FTransform& Target)
{
    FDDSN12StepCommand C; C.OperationEpoch=Epoch; C.PhysicsGeneration=S.Generation;
    C.WorldEpoch=S.CollisionWorldGeneration; C.ReadinessRevision=S.ReadinessRevision; C.IssuedRealS=FPlatformTime::Seconds();
    C.bOpen=Open; C.ParcelAction=Action; C.ParcelTargetWorldCm=Target; C.bExternalWorkDeclared=Action==EDDSN12ParcelAction::Transfer;
    if(Action==EDDSN12ParcelAction::Attach || (Operation.Leg==EDDSN08Leg::Pickup && Action==EDDSN12ParcelAction::Transfer)
        || (bCancelling && Operation.Leg==EDDSN08Leg::Delivery && Action==EDDSN12ParcelAction::Transfer))
    {
        const auto& Calibration=Physics->GetMeasuredBinding().Mechanics;
        const FVector ParcelCOM=Target.TransformPosition(Physics->GetMeasuredBinding().Parcel.COMLocalMeters*100);
        const FTransform Anchor=DDSPhysicalCm(Calibration.MountInParcelMeters)*Target;
        C.ParcelTargetOmegaRadps=S.Mechanics.MountOmegaRadps;
        C.ParcelTargetVelocityCmps=S.Mechanics.MountVelocityCmps-FVector::CrossProduct(C.ParcelTargetOmegaRadps,Anchor.GetLocation()-ParcelCOM);
    }
    return Physics.IsValid() && Physics->SubmitN12Command(C);
}
void UDDSN12PhysicalMechanics::BeginPhysicalCycle(const FDDSN08Token& T)
{
    if(bEnding || Phase!=EDDSN12Cycle::Idle || !Coordinator.IsValid() || !Physics.IsValid()) return;
    const auto* A=Coordinator->GetAuthority(); FDDSRobotSnapshot S;
    if(!A || !A->HasActiveOperation() || !A->GetActiveToken().Equals(T) || !Physics->IsServiceEvidenceFresh(FPlatformTime::Seconds(),S)) return;
    Operation=T; bCancelling=false; bCommitAccepted=false; Epoch=Physics->RevokeN12Operation();
    if(!Epoch) return; SetPhase(EDDSN12Cycle::Opening);
}
void UDDSN12PhysicalMechanics::CancelAndClose(const FDDSN08Token& T)
{
    if(bEnding || bCommitAccepted || !Operation.Equals(T) || !Physics.IsValid()) return;
    Epoch=Physics->RevokeN12Operation(); bCancelling=true; LastBoundarySequence=0; SetPhase(EDDSN12Cycle::Rollback);
}
void UDDSN12PhysicalMechanics::Fail()
{
    if(bCancelling)
    {
        if(Physics.IsValid()) Epoch=Physics->RevokeN12Operation(); // Immediate queued-command revocation, finite brake retained.
        SetPhase(EDDSN12Cycle::Failed); return;
    }
    if(Coordinator.IsValid()) Coordinator->OnNativeMechanicalFailure(Operation); // N08 cancels generation before synchronous rollback.
}
bool UDDSN12PhysicalMechanics::RetrySafeRollback()
{
    if(bEnding || !bCancelling || bCommitAccepted || Phase!=EDDSN12Cycle::Failed || !Physics.IsValid()) return false;
    ADDSN09ServiceBay* Bay=nullptr;
    for(const auto& B:ServiceBays) if(B.IsValid() && B->GetStationId()==Operation.StationId) Bay=B.Get();
    FDDSRobotSnapshot S; FDDSN12Environment E;
    if(!Bay || !Read(*Bay,S,E) || !S.bChassisSupportStable || !E.bFullActualLidSweepClear
        || !E.bActualParcelPathClear || !E.bExternalStationWorkDeclared || UGameplayStatics::IsGamePaused(this)) return false;
    Epoch=Physics->RevokeN12Operation(); if(!Epoch) return false;
    SetPhase(EDDSN12Cycle::Rollback); return true;
}
FDDSN08PhysicalSafetyWitness UDDSN12PhysicalMechanics::Witness(const FDDSRobotSnapshot& S,const FDDSN12Environment& E) const
{
    FDDSN08PhysicalSafetyWitness W; W.Operation=Operation; W.PhysicsGeneration=S.Generation;
    W.CollisionWorldGeneration=S.CollisionWorldGeneration; W.ReadinessRevision=S.ReadinessRevision;
    W.CompletedStepSequence=S.StepSequence; W.CompletedStateTimeS=S.CompletedStateTimeS; W.CapturedRealS=S.CapturedRealS;
    W.bFreshPhysicalStableSupport=S.bServiceStable;
    W.bCurrentLidSweepAndServicePathClear=E.bFullActualLidSweepClear && E.bActualParcelPathClear;
    W.bBothActualLidsClosedLatched=S.Mechanics.Lids[0].bLatchConstraintObserved && S.Mechanics.Lids[1].bLatchConstraintObserved;
    const bool WantMounted=bCancelling ? Operation.Leg==EDDSN08Leg::Delivery : Operation.Leg==EDDSN08Leg::Pickup;
    W.bExpectedParcelPhysicalOwnerAndMass=S.Mechanics.bParcelLive && S.Mechanics.bParcelConstraintObserved==WantMounted
        && (!S.Mechanics.bParcelMounted || S.Mechanics.bParcelConstraintObserved);
    if(!WantMounted)
    {
        const auto& C=Physics->GetMeasuredBinding().Mechanics;
        const auto Target=bCancelling ? E.StableSourceParcelWorldCm : E.RecipientParcelWorldCm;
        W.bExpectedParcelPhysicalOwnerAndMass &= (bCancelling ? E.bParcelAtStableSourceObserved : E.bParcelAtRecipientObserved)
            && S.Mechanics.ParcelWorldCm.GetLocation().Equals(Target.GetLocation(),C.MountPositionToleranceM*100)
            && S.Mechanics.ParcelWorldCm.GetRotation().AngularDistance(Target.GetRotation())<=C.MountAngleToleranceRad
            && S.Mechanics.ParcelVelocityCmps.Size()<=C.MountSpeedToleranceMps*100
            && S.Mechanics.ParcelOmegaRadps.Size()<=C.MountOmegaToleranceRadps;
    }
    return W;
}
void UDDSN12PhysicalMechanics::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt,Type,Function);
    if(bEnding || Phase==EDDSN12Cycle::Idle || !Coordinator.IsValid() || !Physics.IsValid()) return;
    const auto* A=Coordinator->GetAuthority(); if(!A) return;
    if(Phase==EDDSN12Cycle::Failed) return;
    ADDSN09ServiceBay* Bay=nullptr; for(const auto& B:ServiceBays) if(B.IsValid() && B->GetStationId()==Operation.StationId) Bay=B.Get();
    FDDSRobotSnapshot S; FDDSN12Environment E;
    if(Phase==EDDSN12Cycle::SaveLocked)
    {
        if(Bay && Read(*Bay,S,E) && S.bServiceStable && Witness(S,E).bExpectedParcelPhysicalOwnerAndMass
            && A->GetSaveGate()==EDDSN08Gate::None && !A->IsDriveLocked() && Physics->ReleaseN12DriveLock(Epoch)) SetPhase(EDDSN12Cycle::Idle);
        return;
    }
    // Real-time phase budget includes pause: pause immediately revokes the attempt.
    // Unknown/blocked safety also fails immediately, rather than waiting behind a read-return.
    if(FPlatformTime::Seconds()-PhaseStartRealS>Physics->GetMeasuredBinding().Mechanics.PhaseTimeoutS
        || Physics->GetN12OperationEpoch()!=Epoch) { Fail(); return; }
    if(!Bay || !Read(*Bay,S,E) || !S.bChassisSupportStable || !E.bFullActualLidSweepClear || !E.bActualParcelPathClear
        || !E.bExternalStationWorkDeclared || UGameplayStatics::IsGamePaused(this))
    { Fail(); return; }
    if(S.Mechanics.OperationEpoch!=Epoch) return; // Await only our already-revoked epoch's completed PT observation; budget above remains active.
    const auto& C=Physics->GetMeasuredBinding().Mechanics;
    const FTransform Mount=DDSPhysicalCm(C.MountInChassisMeters)*S.Bodies[0].WorldCm;
    const FTransform RobotTarget=DDSPhysicalCm(C.MountInParcelMeters).Inverse()*Mount;
    const bool WantMounted=bCancelling ? Operation.Leg==EDDSN08Leg::Delivery : Operation.Leg==EDDSN08Leg::Pickup;
    const FTransform Target=WantMounted ? RobotTarget : (bCancelling ? E.StableSourceParcelWorldCm : E.RecipientParcelWorldCm);
    auto Progress=[&](EDDSN08MechanicStep Step){if(!bCancelling) Coordinator->OnNativeMechanicalProgress(Operation,Step);};
    if(Phase==EDDSN12Cycle::Opening || Phase==EDDSN12Cycle::Rollback)
    {
        if(!Submit(S,true,EDDSN12ParcelAction::None,Target)) { Fail(); return; }
        Progress(EDDSN08MechanicStep::Opening);
        if(S.Mechanics.Lids[0].bAtOpenPose && S.Mechanics.Lids[1].bAtOpenPose)
        {
            Progress(EDDSN08MechanicStep::Open);
            if(WantMounted && S.Mechanics.bParcelConstraintObserved) { Progress(EDDSN08MechanicStep::Staged); SetPhase(EDDSN12Cycle::Closing); }
            else SetPhase(S.Mechanics.bParcelMounted && !WantMounted ? EDDSN12Cycle::Detaching : EDDSN12Cycle::Transfer);
        }
    }
    else if(Phase==EDDSN12Cycle::Detaching)
    {
        if(S.Mechanics.bBoundaryCompleted && !S.Mechanics.bParcelMounted) { SetPhase(EDDSN12Cycle::Transfer); return; }
        Submit(S,true,EDDSN12ParcelAction::Detach,Target);
    }
    else if(Phase==EDDSN12Cycle::Transfer)
    {
        Submit(S,true,EDDSN12ParcelAction::Transfer,Target);
        if(S.Mechanics.bParcelAtTarget)
        { SetPhase(WantMounted ? EDDSN12Cycle::Attaching : EDDSN12Cycle::Closing); if(!WantMounted) Progress(EDDSN08MechanicStep::Staged); }
    }
    else if(Phase==EDDSN12Cycle::Attaching)
    {
        if(S.Mechanics.bBoundaryCompleted && S.Mechanics.bParcelConstraintObserved)
        { Progress(EDDSN08MechanicStep::Staged); SetPhase(EDDSN12Cycle::Closing); return; }
        Submit(S,true,EDDSN12ParcelAction::Attach,RobotTarget);
    }
    else if(Phase==EDDSN12Cycle::Closing)
    {
        Submit(S,false,EDDSN12ParcelAction::None,Target); Progress(EDDSN08MechanicStep::Closing);
        if(!S.bServiceStable || !S.Mechanics.Lids[0].bLatchConstraintObserved || !S.Mechanics.Lids[1].bLatchConstraintObserved) return;
        const auto W=Witness(S,E);
        if(bCancelling)
        { if(Coordinator->OnNativeCancelledClosure(Operation,W) && Physics->ReleaseN12DriveLock(Epoch)) SetPhase(EDDSN12Cycle::Idle); }
        else
        {
            FDDSN08LatchEvidence L; L.Token=Operation; L.bMechanicalObservationKnown=S.Mechanics.bKnown;
            L.bParcelStagedAtCorrectDestination=W.bExpectedParcelPhysicalOwnerAndMass;
            L.bFullOpeningAndClosingSweepClear=W.bCurrentLidSweepAndServicePathClear;
            L.bLeftLidAtClosedPose=S.Mechanics.Lids[0].bAtClosedPose; L.bRightLidAtClosedPose=S.Mechanics.Lids[1].bAtClosedPose;
            L.bLeftLatchVerified=S.Mechanics.Lids[0].bLatchConstraintObserved; L.bRightLatchVerified=S.Mechanics.Lids[1].bLatchConstraintObserved;
            if(Coordinator->OnNativeVerifiedLatch(L,W,E.CurrentValidatedSavePose)) { bCommitAccepted=true; SetPhase(EDDSN12Cycle::SaveLocked); }
        }
    }
}
void UDDSN12PhysicalMechanics::EndPlay(EEndPlayReason::Type Reason)
{
    bEnding=true;
    if(Physics.IsValid()) Physics->RevokeN12Operation();
    if(Coordinator.IsValid()) Coordinator->UnbindNativeMechanics();
    Reader=FEnvironmentReader(); Super::EndPlay(Reason);
}
