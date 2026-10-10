#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DDSN08Coordinator.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "DDSN12PhysicalMechanics.generated.h"

struct FDDSN12Environment
{
    uint64 PhysicsGeneration=0, WorldEpoch=0, ReadinessRevision=0, CompletedStep=0;
    double CapturedRealS=0;
    FString MissionId, StationId, CargoId, NativeGeometryRecordSha256;
    int32 ObservedParcelParticleId=INDEX_NONE;
    bool bFullActualLidSweepObserved=false, bFullActualLidSweepClear=false;
    bool bActualParcelPathObserved=false, bActualParcelPathClear=false;
    bool bParcelAtStableSourceObserved=false;
    bool bParcelAtRecipientObserved=false;
    bool bExternalStationWorkDeclared=false;
    FTransform StableSourceParcelWorldCm=FTransform::Identity;
    FTransform RecipientParcelWorldCm=FTransform::Identity;
    FDDSN11Pose CurrentValidatedSavePose;
};
enum class EDDSN12Cycle : uint8 { Idle, Opening, Detaching, Transfer, Attaching, Closing, SaveLocked, Rollback, Failed };

UCLASS(ClassGroup=(DDSCity))
class DDSCITY_API UDDSN12PhysicalMechanics final : public UActorComponent, public IDDSN08MechanicsPort
{
    GENERATED_BODY()
public:
    UDDSN12PhysicalMechanics();
    using FEnvironmentReader=TFunction<bool(const ADDSN09ServiceBay&,const FDDSRobotSnapshot&,const FDDSN08StableSnapshot&,FDDSN12Environment&)>;
    bool Bind(UDDSN08Coordinator& N08,UDDSRobotPhysicsRigComponent& Rig,
        const TArray<ADDSN09ServiceBay*>& Bays,FEnvironmentReader Reader);
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Function) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void BeginPhysicalCycle(const FDDSN08Token& Token) override;
    virtual void CancelAndClose(const FDDSN08Token& Token) override;
    bool RetrySafeRollback(); // Explicit retry; each attempt remains bounded and cannot commit.
private:
    bool Read(const ADDSN09ServiceBay& Bay,FDDSRobotSnapshot& S,FDDSN12Environment& E) const;
    bool Project(const ADDSN09ServiceBay& Bay,FDDSN09ExternalCargoDoorObservation& Facts,FDDSN15ProjectionInputs& View) const;
    bool Submit(const FDDSRobotSnapshot& S,bool Open,EDDSN12ParcelAction Action,const FTransform& Target);
    FDDSN08PhysicalSafetyWitness Witness(const FDDSRobotSnapshot& S,const FDDSN12Environment& E) const;
    void Fail();
    void SetPhase(EDDSN12Cycle Next);
    TWeakObjectPtr<UDDSN08Coordinator> Coordinator;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> Physics;
    TArray<TWeakObjectPtr<ADDSN09ServiceBay>> ServiceBays;
    FEnvironmentReader Reader;
    FDDSN08Token Operation;
    EDDSN12Cycle Phase=EDDSN12Cycle::Idle;
    uint64 Epoch=0, LastBoundarySequence=0;
    double PhaseStartRealS=0;
    bool bCancelling=false, bCommitAccepted=false, bEnding=false;
};
