#include "DDSRobotLifecycleOwner.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "DDSOriginalChassisCompoundCollision.h"
#include "DDSOriginalCargoVertexCollision.h"
#include "UObject/StrongObjectPtr.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsPublic.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Chaos/ShapeInstance.h"
#include "UObject/Class.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "DDSRobotControlBarrier.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/CoreDelegates.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsSolver.h"

namespace
{
TArray<uint8> CollisionSourceBytes(UBodySetup* Setup)
{
    TArray<uint8> Bytes;
    if (Setup)
    {
        // Read-only reflected aggregate geometry. No DDC/build/cook or asset write.
        FMemoryWriter Writer(Bytes, false);
        FObjectAndNameAsStringProxyArchive Archive(Writer, false);
        FKAggregateGeom::StaticStruct()->SerializeItem(Archive, &Setup->AggGeom, nullptr);
    }
    return Bytes;
}
TArray<uint8> DDSCompoundSetupSettingsBytes(UBodySetup* Setup)
{
    TArray<uint8> Bytes;if(!Setup)return Bytes;FMemoryWriter W(Bytes,false);FObjectAndNameAsStringProxyArchive A(W,false);
    FBodyInstance::StaticStruct()->SerializeItem(A,&Setup->DefaultInstance,nullptr);
    FVector Scale=Setup->BuildScale3D;A<<Scale;
    uint32 Flags=uint32(Setup->GetCollisionTraceFlag()) | uint32(Setup->bMeshCollideAll)<<8 | uint32(Setup->bDoubleSidedGeometry)<<9
       | uint32(Setup->bGenerateNonMirroredCollision)<<10 | uint32(Setup->bGenerateMirroredCollision)<<11 | uint32(Setup->bSharedCookedData)<<12
       | uint32(Setup->bSupportUVsAndFaceRemap)<<13 | uint32(Setup->bNeverNeedsCookedCollisionData)<<14;A<<Flags;return Bytes;
}
bool DDSCompoundObjectLive(const UObject* O)
{ return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed); }

}

TSharedPtr<FDDSRobotLifecycleLease, ESPMode::ThreadSafe> FDDSRobotLifecycleLease::Acquire(
    UDDSRobotPhysicsRigComponent* R, const FDDSRobotMeasuredBinding& B, FString& Error)
{
    check(IsInGameThread());
    if (!R || !R->GetWorld() || !R->GetWorld()->GetPhysicsScene()) return nullptr;
    TSharedPtr<FDDSRobotLifecycleLease, ESPMode::ThreadSafe> L(new FDDSRobotLifecycleLease());
    L->World = R->GetWorld(); L->Actor = R->GetOwner(); L->Rig = R;
    L->Scene = L->World->GetPhysicsScene();
    TArray<UStaticMeshComponent*> All;
    for (const auto& M : B.Bodies) All.Add(M.ActualPart);
    for (const auto& M : B.SuspensionParts) All.Add(M.ActualPart);
    if (B.Parcel.ActualPart) All.Add(B.Parcel.ActualPart);
    for (auto* P : All)
    {
        auto* Guard = Cast<UDDSRobotLeasedMeshComponent>(P);
        if (!Guard || Guard->GetOwner() != L->Actor || !Guard->IsRegistered() || Guard->Lease.IsValid())
        { Error = TEXT("A02_LIFECYCLE_SOURCE_REQUIRED: all 15 original parts must be unleased managed supplier components");
          L->bReleased = true; return nullptr; }
        if (!Guard->GetStaticMesh())
        { Error = TEXT("A02 original mesh identity missing"); L->bReleased = true; return nullptr; }
        if(Guard->HasPreparedOriginalChassisCompound() && !Guard->PreparedOriginalChassisCompoundUnchanged())
        { Error=TEXT("Prepared original compound altered before lease acquisition");L->bReleased=true;return nullptr; }
        if(Guard->HasPreparedOriginalCargoVertexCollision() && !Guard->PreparedOriginalCargoVertexCollisionUnchanged())
        { Error=TEXT("Prepared original cargo vertex setup/source altered before lease acquisition");L->bReleased=true;return nullptr; }
        L->Parts.Add(Guard);
        FPartReceipt Receipt;
        Receipt.Mesh = Guard->GetStaticMesh().Get(); Receipt.BodySetup = Receipt.Mesh->GetBodySetup();
        Receipt.ComponentBodySetup = Guard->GetBodySetup();
        if (Receipt.BodySetup) Receipt.BodySetupGuid = Receipt.BodySetup->BodySetupGuid;
        Receipt.OriginalCollisionData = CollisionSourceBytes(Receipt.BodySetup);
        Receipt.OriginalSetupSettings = DDSCompoundSetupSettingsBytes(Receipt.BodySetup);
        Receipt.OriginalSetupMaterial = Receipt.BodySetup ? Receipt.BodySetup->PhysMaterial.Get() : nullptr;
        if(Receipt.ComponentBodySetup) Receipt.ComponentBodySetupGuid=Receipt.ComponentBodySetup->BodySetupGuid;
        Receipt.ComponentCollisionData=CollisionSourceBytes(Receipt.ComponentBodySetup);
        Receipt.ComponentSetupSettings=DDSCompoundSetupSettingsBytes(Receipt.ComponentBodySetup);
        Receipt.ComponentSetupMaterial=Receipt.ComponentBodySetup?Receipt.ComponentBodySetup->PhysMaterial.Get():nullptr;
        if (!FDDSOriginalMaterialObservation::CaptureRaw(*Guard, Receipt.MaterialReceipt, Error))
        { L->bReleased = true; return nullptr; }
        Receipt.OriginalScale = Guard->GetComponentScale();
        Receipt.OriginalRigidPose = DDSRigidComponentPose(Guard->GetComponentTransform());
        Receipt.ExpectedPhysicsRevision = Guard->PhysicsStateRevision;
        L->Receipts.Add(MoveTemp(Receipt));
    }
    // Capture ALL managed parts and raw Parent/Nanite graphs before resolving any.
    const auto WholeAcquisitionUnchanged = [&]()
    {
        if (!L->MaterialReceiptsUnchanged(true) || !L->bBuilding || L->bRetiring || L->bReleased) return false;
        for (auto* Part : L->Parts)
            if (Part->Lease.IsValid() || !L->CollisionReceiptsUnchanged(Part)) return false;
        return true;
    };
    for (auto& Receipt : L->Receipts)
        if (!FDDSOriginalMaterialObservation::Resolve(*Receipt.MaterialReceipt, WholeAcquisitionUnchanged, Error))
        { L->bReleased = true; return nullptr; }
    if (!WholeAcquisitionUnchanged())
    { Error = TEXT("Original material acquisition final callback-free receipt check failed"); L->bReleased = true; return nullptr; }
    for(auto* P:L->Parts)if(!L->CollisionReceiptsUnchanged(P)) { Error=TEXT("Original/actual component setup changed during lease capture");L->bReleased=true;return nullptr; }
    for (auto* P : L->Parts) { P->Lease = L; P->AdmittedScale = P->GetComponentScale(); }
    L->WorldTearDown = FWorldDelegates::OnWorldBeginTearDown.AddRaw(L.Get(), &FDDSRobotLifecycleLease::OnWorldTearDown);
    L->WorldCleanup = FWorldDelegates::OnWorldCleanup.AddRaw(L.Get(), &FDDSRobotLifecycleLease::OnWorldCleanup);
    L->SceneTerm = FPhysicsDelegates::OnPhysSceneTerm.AddRaw(L.Get(), &FDDSRobotLifecycleLease::OnSceneTerm);
    L->PreGC = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddRaw(L.Get(), &FDDSRobotLifecycleLease::OnBeforeGC);
    L->PreExit = FCoreDelegates::OnEnginePreExit.AddRaw(L.Get(), &FDDSRobotLifecycleLease::OnEngineExit);
    return L;
}
FDDSRobotLifecycleLease::~FDDSRobotLifecycleLease()
{
    check(bReleased); // Every admitted reference has a synchronous release path.
    FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDown);
    FWorldDelegates::OnWorldCleanup.Remove(WorldCleanup);
    FPhysicsDelegates::OnPhysSceneTerm.Remove(SceneTerm);
    FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGC);
    FCoreDelegates::OnEnginePreExit.Remove(PreExit);
}
void FDDSRobotLifecycleLease::AddReferencedObjects(FReferenceCollector& C)
{
    if (bReleased) return;
    C.AddReferencedObject(World); C.AddReferencedObject(Actor); C.AddReferencedObject(Rig);
    for (auto*& P : Parts) C.AddReferencedObject(P);
    for (auto& R : Receipts)
    { C.AddReferencedObject(R.Mesh); C.AddReferencedObject(R.BodySetup); C.AddReferencedObject(R.ComponentBodySetup);
      C.AddReferencedObject(R.OriginalSetupMaterial); C.AddReferencedObject(R.ComponentSetupMaterial);
      // MaterialReceipt retains its complete source/material graph until release.
    }
}
void FDDSRobotLifecycleLease::SetAdmissionBarrier(uint64 G, uint64 W, uint64 R)
{ AdmissionGeneration=G; AdmissionWorldEpoch=W; AdmissionReadinessRevision=R; }
void FDDSRobotLifecycleLease::IndependentMutation()
{
    ++ConstructionRevision; bConstructionValid = false;
    if (Rig && bBuilding) Rig->Fault = TEXT("CONSTRUCTION_INVALIDATED_BY_INDEPENDENT_MANAGED_MUTATION");
    RetireSynchronously(); // no construction-wide exemption; before superclass mutation.
}
bool FDDSRobotLifecycleLease::ConsumeOwnedPhysicsHook(UDDSRobotLeasedMeshComponent* Subject, bool bCreate)
{
    // Capability is private, subject-specific and consumed before Super. Closed
    // phases cannot be reused by GlobalCreate/Destroy delegate reentry.
    if (!IsBuilding() || !ActiveOperationNonce || Subject != OwnedSubject) return false;
    if (bCreate && OwnedPhase == EOwnedHookPhase::Create)
    { OwnedPhase = EOwnedHookPhase::None; return true; }
    if (!bCreate && OwnedPhase == EOwnedHookPhase::Destroy)
    { OwnedPhase = EOwnedHookPhase::Create; return true; }
    return false;
}
bool FDDSRobotLifecycleLease::CollisionReceiptsUnchanged(UDDSRobotLeasedMeshComponent* Subject) const
{
    const int32 I=Parts.IndexOfByKey(Subject);if(!Receipts.IsValidIndex(I) || !DDSCompoundObjectLive(Subject))return false;
    const auto& R=Receipts[I];if(!DDSCompoundObjectLive(R.Mesh) || R.Mesh->IsCompiling() || Subject->GetStaticMesh().Get()!=R.Mesh
       || R.Mesh->GetBodySetup()!=R.BodySetup || Subject->GetBodySetup()!=R.ComponentBodySetup)return false;
    const auto Same=[&](UBodySetup* B,const FGuid& Guid,const TArray<uint8>& Geometry,const TArray<uint8>& Settings,UPhysicalMaterial* Material)
    { return DDSCompoundObjectLive(B) && B->BodySetupGuid==Guid && B->PhysMaterial.Get()==Material && (!Material || DDSCompoundObjectLive(Material))
        && CollisionSourceBytes(B)==Geometry && DDSCompoundSetupSettingsBytes(B)==Settings; };
    return Same(R.BodySetup,R.BodySetupGuid,R.OriginalCollisionData,R.OriginalSetupSettings,R.OriginalSetupMaterial)
        && Same(R.ComponentBodySetup,R.ComponentBodySetupGuid,R.ComponentCollisionData,R.ComponentSetupSettings,R.ComponentSetupMaterial);
}
bool FDDSRobotLifecycleLease::MaterialReceiptsUnchanged(bool bCheckPose) const
{
    if (!DDSCompoundObjectLive(World) || World->bIsTearingDown || !DDSCompoundObjectLive(Actor)
        || Actor->IsActorBeingDestroyed() || !DDSCompoundObjectLive(Rig)
        || Rig->GetOwner() != Actor || Rig->GetWorld() != World || Actor->GetWorld() != World
        || World->GetPhysicsScene() != Scene || Parts.Num() != Receipts.Num()) return false;
    for (int32 I = 0; I < Parts.Num(); ++I)
    {
        const auto& Material = Receipts[I].MaterialReceipt;
        if (!Material || !Material->Unchanged(bCheckPose) || Material->Component != Parts[I]
            || Material->Owner != Actor || Material->World != World || !Material->bRegistered) return false;
    }
    return true; // Cached/raw material checks; never GetMaterial/GetMaterials.
}
bool FDDSRobotLifecycleLease::CaptureOwnedBodyReceipt(UDDSRobotLeasedMeshComponent* Subject)
{
    const int32 I = Parts.IndexOfByKey(Subject);
    if (!IsBuilding() || !Receipts.IsValidIndex(I) || !CollisionReceiptsUnchanged(Subject)) return false;
    auto& R = Receipts[I]; R.ExpectedPhysicsRevision = Subject->PhysicsStateRevision;
    R.ActorHandle=nullptr; R.Geometry=nullptr; R.ParticleId=INDEX_NONE;
    R.Shapes.Reset(); R.ShapeGeometry.Reset();
    auto* BI = Subject->GetBodyInstance();
    auto* Proxy = BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
    if (Proxy)
    {
        if (Proxy->GetSolverBase() != Scene->GetSolver()) return false;
        const auto& Body = Proxy->GetGameThreadAPI();
        R.ActorHandle=Proxy; R.Geometry=Body.Geometry().Get(); R.ParticleId=Body.UniqueIdx().Idx;
        for (const auto& Shape : Body.ShapesArray())
        { R.Shapes.Add(Shape.Get()); R.ShapeGeometry.Add(Shape->GetGeometry().Get()); }
    }
    return true;
}
bool FDDSRobotLifecycleLease::ExecuteOwnedOperation(UDDSRobotLeasedMeshComponent* Subject, TFunctionRef<void()> Operation)
{
    check(IsInGameThread());
    if (!IsBuilding() || OwnedSubject || !Parts.Contains(Subject)) return false;
    if(!CollisionReceiptsUnchanged(Subject) || !MaterialReceiptsUnchanged(false)) { IndependentMutation();return false; }
    auto KeepAlive=AsShared();
    const uint64 Revision=ConstructionRevision, Nonce=++OwnedOperationNonce;
    OwnedSubject=Subject; ActiveOperationNonce=Nonce;
    OwnedPhase=Subject->IsPhysicsStateCreated() ? EOwnedHookPhase::Destroy : EOwnedHookPhase::Create;
    Operation(); // private Rig producer closure, never a caller-provided ownership flag.
    const bool Valid=IsBuilding() && ConstructionRevision==Revision && ActiveOperationNonce==Nonce
        && MaterialReceiptsUnchanged(false);
    OwnedSubject=nullptr; ActiveOperationNonce=0; OwnedPhase=EOwnedHookPhase::None;
    return Valid && CaptureOwnedBodyReceipt(Subject);
}
bool FDDSRobotLifecycleLease::PrepareOwnedMobility()
{
    // Descendants first: a later parent SetMobility then encounters already
    // movable managed descendants, so they never inherit another part's permit.
    TArray<UDDSRobotLeasedMeshComponent*> Ordered=Parts;
    const auto Depth=[](const USceneComponent& C)
    { int32 N=0; for (auto* P=C.GetAttachParent(); P && N<64; P=P->GetAttachParent()) ++N; return N; };
    for (auto* P : Ordered) if (Depth(*P)>=64) return false;
    Ordered.Sort([&](const UDDSRobotLeasedMeshComponent& A, const UDDSRobotLeasedMeshComponent& B)
    { return Depth(A)>Depth(B); });
    for (auto* P : Ordered)
        if (!ExecuteOwnedOperation(P,[P]() { P->SetMobility(EComponentMobility::Movable); })) return false;
    return true;
}
bool FDDSRobotLifecycleLease::CommitAdmission(const FDDSRobotMeasuredBinding& B,
    const FDDSRobotPhysicsProfile& P, UPhysicalMaterial* ContactMaterial, FString& Error)
{
    const uint64 Revision=ConstructionRevision;
    if (!IsBuilding() || bRetiring || OwnedSubject || ActiveOperationNonce || !Scene
        || !IsValid(World) || !IsValid(Actor) || !IsValid(Rig) || World->GetPhysicsScene()!=Scene
        || Parts.Num()!=(B.Parcel.ActualPart ? 16 : 15) || Receipts.Num()!=Parts.Num() || !Rig->Control
        || !DDSValidateMeasuredBinding(B,P,Error)) return false;
    const auto Gate=Rig->Control->Read();
    if (Gate.Generation!=AdmissionGeneration || Gate.WorldEpoch!=AdmissionWorldEpoch
        || Gate.Revision!=AdmissionReadinessRevision || Gate.bReady || Gate.bRetiring) return false;
    for (int32 I=0; I<Parts.Num(); ++I)
    {
        auto* Part=Parts[I]; const auto& R=Receipts[I];
        auto* Expected=I<9 ? B.Bodies[I].ActualPart : (I==15 ? B.Parcel.ActualPart : B.SuspensionParts[I-9].ActualPart);
        if (!IsValid(Part) || Part!=Expected || Part->Lease.Pin().Get()!=this || Part->GetOwner()!=Actor
            || !Part->IsRegistered() || Part->Mobility!=EComponentMobility::Movable
            || Part->PhysicsStateRevision!=R.ExpectedPhysicsRevision || Part->GetStaticMesh()!=R.Mesh
            || !CollisionReceiptsUnchanged(Part) || !R.MaterialReceipt
            || !R.MaterialReceipt->bResolved || !R.MaterialReceipt->Unchanged()
            || !Part->GetComponentScale().Equals(R.OriginalScale,1.e-6)
            || !DDSRigidComponentPose(Part->GetComponentTransform()).Equals(R.OriginalRigidPose,1.e-5)
            || (R.BodySetup && (R.BodySetup->BodySetupGuid!=R.BodySetupGuid
                || CollisionSourceBytes(R.BodySetup)!=R.OriginalCollisionData))) return false;
        auto* BI=Part->GetBodyInstance();
        auto* Proxy=BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
        if (Proxy!=R.ActorHandle) return false;
        if (Proxy)
        {
            const auto& Body=Proxy->GetGameThreadAPI();
            if (Proxy->GetSolverBase()!=Scene->GetSolver() || Body.UniqueIdx().Idx!=R.ParticleId
                || Body.Geometry().Get()!=R.Geometry || Body.ShapesArray().Num()!=R.Shapes.Num()) return false;
            for (int32 J=0; J<R.Shapes.Num(); ++J)
                if (Body.ShapesArray()[J].Get()!=R.Shapes[J]
                    || Body.ShapesArray()[J]->GetGeometry().Get()!=R.ShapeGeometry[J]) return false;
        }
        if (I<9 || I==15)
        {
            const auto& Measurement=I==15 ? B.Parcel : B.Bodies[I];
            const double Mass=I==15 ? P.ParcelKg : (I==0 ? P.BareChassisKg : (I<7 ? P.WheelKg : (I==7 ? P.Lid0Kg : P.Lid1Kg)));
            if (!BI || !Proxy || !Part->IsSimulatingPhysics() || !Part->IsGravityEnabled()
                || !BI->bOverrideMass || !FMath::IsNearlyEqual(BI->GetMassOverride(),Mass,1.e-5)
                || !FMath::IsNearlyEqual(Part->GetMass(),Mass,1.e-4) || BI->bAutoWeld
                || BI->IsInertiaConditioningEnabled() || !BI->InertiaTensorScale.Equals(FVector::OneVector,1.e-6)
                || BI->LinearDamping!=0 || BI->AngularDamping!=0
                || BI->GetSimplePhysicalMaterial()!=ContactMaterial
                || Part->GetCollisionEnabled()!=ECollisionEnabled::QueryAndPhysics
                || Part->GetCollisionProfileName()!=Measurement.MeasuredCollisionProfile
                || static_cast<bool>(BI->bUseCCD)!=Measurement.bCCDMeasuredRequired) return false;
        }
        else if (Part->IsSimulatingPhysics() || Part->GetCollisionEnabled()!=ECollisionEnabled::NoCollision
            || Part->GetAttachParent()!=B.Bodies[static_cast<int32>(B.SuspensionParts[I-9].Owner)].ActualPart) return false;
    }
    const auto FinalGate=Rig->Control->Read();
    if (!IsBuilding() || ConstructionRevision!=Revision || !MaterialReceiptsUnchanged(true) || World->GetPhysicsScene()!=Scene
        || FinalGate.Generation!=Gate.Generation || FinalGate.WorldEpoch!=Gate.WorldEpoch
        || FinalGate.Revision!=Gate.Revision || FinalGate.bReady || FinalGate.bRetiring) return false;
    bBuilding=false; return true; // one whole-binding transaction commit, no future owner flag.
}
void FDDSRobotLifecycleLease::RetireSynchronously()
{
    check(IsInGameThread());
    if (bReleased || bRetiring) return;
    auto KeepAlive = AsShared();
    bRetiring = true; ++ConstructionRevision; bConstructionValid=false;
    // Scene still live either by pinned World or inside pre-base SceneTerm.
    // RetireWithLiveLease does not infer lifetime from a raw solver pointer.
    check(Scene && Rig);
    Rig->RetireWithLiveLease(*this);
    for (auto* P : Parts) P->Lease.Reset();
    Scene = nullptr; bReleased = true; bRetiring = false;
    FWorldDelegates::OnWorldBeginTearDown.Remove(WorldTearDown);
    FWorldDelegates::OnWorldCleanup.Remove(WorldCleanup);
    FPhysicsDelegates::OnPhysSceneTerm.Remove(SceneTerm);
    FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGC);
    FCoreDelegates::OnEnginePreExit.Remove(PreExit);
    Parts.Reset(); Receipts.Reset(); Rig = nullptr; Actor = nullptr; World = nullptr;
}
void FDDSRobotLifecycleLease::OnWorldTearDown(UWorld* W) { if (W == World) RetireSynchronously(); }
void FDDSRobotLifecycleLease::OnWorldCleanup(UWorld* W, bool, bool) { if (W == World) RetireSynchronously(); }
void FDDSRobotLifecycleLease::OnSceneTerm(FPhysScene_Chaos* S) { if (S == Scene) RetireSynchronously(); }
void FDDSRobotLifecycleLease::OnBeforeGC()
{
    // Valid objects remain rooted by FGCObject. Explicit garbage flags must
    // retire BEFORE GC's guard/reachability/ConditionalBeginDestroy ordering.
    bool Valid = IsValid(World) && IsValid(Actor) && IsValid(Rig);
    for (auto* P : Parts) Valid &= IsValid(P) && CollisionReceiptsUnchanged(P);
    if (!Valid) RetireSynchronously();
}
void FDDSRobotLifecycleLease::OnEngineExit() { RetireSynchronously(); }
void UDDSRobotLeasedMeshComponent::RetireBeforeMutation()
{ if (auto L = Lease.Pin()) L->IndependentMutation(); }
bool UDDSRobotLeasedMeshComponent::SetStaticMesh(UStaticMesh* Mesh)
{
    if(Mesh!=GetStaticMesh().Get()) { RetireBeforeMutation();if(OriginalChassisCompoundSetup || OriginalCargoVertexSetup || bPreparingOriginalChassisCompound || bPreparingOriginalCargoVertex)return false; }
    return Super::SetStaticMesh(Mesh);
}
UBodySetup* UDDSRobotLeasedMeshComponent::GetBodySetup()
{ return OriginalChassisCompoundSetup ? OriginalChassisCompoundSetup.Get() : (OriginalCargoVertexSetup ? OriginalCargoVertexSetup.Get() : Super::GetBodySetup()); }
bool UDDSRobotLeasedMeshComponent::PrepareOriginalChassisCompoundFromObservedGeometry(const FDDSOriginalRobotNativeGeometry& Supplied,FString& Error)
{
    Error.Reset();if(!IsInGameThread() || bPreparingOriginalChassisCompound || bPreparingOriginalCargoVertex) { Error=TEXT("Compound preparation wrong thread/reentrant");return false; }
    TStrongObjectPtr<UDDSRobotLeasedMeshComponent> SelfPin(this);TGuardValue<bool> Preparing(bPreparingOriginalChassisCompound,true);
    const auto G=Supplied;AActor* const Target=GetOwner();UWorld* const TargetWorld=GetWorld();UStaticMesh* const OriginalMesh=GetStaticMesh().Get();
    const auto Live=[&]() { return DDSCompoundObjectLive(this) && DDSCompoundObjectLive(Target) && !Target->IsActorBeingDestroyed()
        && DDSCompoundObjectLive(TargetWorld) && !TargetWorld->bIsTearingDown && GetOwner()==Target && GetOuter()==Target && GetWorld()==TargetWorld
        && !Lease.IsValid() && !IsRegistered() && !IsPhysicsStateCreated() && !BodyInstance.IsValidBodyInstance() && !IsSimulatingPhysics()
        && GetCollisionEnabled()==ECollisionEnabled::NoCollision && GetStaticMesh().Get()==OriginalMesh && !OriginalChassisCompoundSetup && !OriginalCargoVertexSetup; };
    if(!Live() || !DDSCompoundObjectLive(OriginalMesh) || OriginalMesh->GetPathName()!=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Base.SM_DeliveryRobot_Base"))
    { Error=TEXT("Only live unregistered unleased exact original Base may prepare compound collision");return false; }
    TArray<TStrongObjectPtr<UObject>> InputLifetime;InputLifetime.Emplace(Target);InputLifetime.Emplace(TargetWorld);InputLifetime.Emplace(OriginalMesh);
    for(const auto& P:G.Parts) { if(!DDSCompoundObjectLive(P.OriginalVisualSource) || !DDSCompoundObjectLive(P.SuppliedMesh)
       || P.OriginalVisualSource->GetWorld()!=TargetWorld) { Error=TEXT("Actual original same-world source lifetime required");return false; }
       InputLifetime.Emplace(P.OriginalVisualSource);InputLifetime.Emplace(P.OriginalVisualSource->GetOwner());InputLifetime.Emplace(P.SuppliedMesh); }
    FDDSOriginalChassisCompoundResult Result;if(!DDSBuildOriginalChassisCompoundCollision(*this,G,Result,Error))return false;
    if(!Live() || !DDSCompoundObjectLive(Result.BodySetup.Get()) || Result.BodySetup->GetOuter()!=this) { Error=TEXT("Compound preparation target/context changed");return false; }
    for(const auto& P:InputLifetime)OriginalChassisCompoundSources.Add(P.Get());
    for(const auto& P:Result.SourceLifetime)OriginalChassisCompoundSources.Add(P.Get());
    OriginalChassisCompoundPreparedMaterial=Result.BodySetup->PhysMaterial.Get();
    OriginalChassisCompoundGuid=Result.BodySetup->BodySetupGuid;
    OriginalChassisCompoundCollisionBytes=CollisionSourceBytes(Result.BodySetup.Get());
    OriginalChassisCompoundSettingsBytes=DDSCompoundSetupSettingsBytes(Result.BodySetup.Get());
    OriginalChassisCompoundSetup=Result.BodySetup.Get();bOriginalChassisCompoundPrepared=true;return true; // Declared rigid geometry model ONLY, no mass/topology/readiness permission.
}
bool UDDSRobotLeasedMeshComponent::PreparedOriginalChassisCompoundUnchanged() const
{
    UBodySetup* const B=OriginalChassisCompoundSetup.Get();
    return bOriginalChassisCompoundPrepared && DDSCompoundObjectLive(B) && B->GetOuter()==this && B->BodySetupGuid==OriginalChassisCompoundGuid
        && B->PhysMaterial.Get()==OriginalChassisCompoundPreparedMaterial.Get()
        && (!OriginalChassisCompoundPreparedMaterial || DDSCompoundObjectLive(OriginalChassisCompoundPreparedMaterial.Get()))
        && CollisionSourceBytes(B)==OriginalChassisCompoundCollisionBytes && DDSCompoundSetupSettingsBytes(B)==OriginalChassisCompoundSettingsBytes;
}

bool UDDSRobotLeasedMeshComponent::PrepareOriginalCargoVertexCollisionFromObservedGeometry(const FDDSOriginalRobotNativeGeometry& Supplied,FString& Error)
{
    Error.Reset();if(!IsInGameThread() || bPreparingOriginalCargoVertex || bPreparingOriginalChassisCompound){Error=TEXT("Cargo vertex preparation wrong thread/reentrant");return false;}
    TStrongObjectPtr<UDDSRobotLeasedMeshComponent> SelfPin(this);TGuardValue<bool> Preparing(bPreparingOriginalCargoVertex,true);const auto G=Supplied;
    AActor* const Target=GetOwner();UWorld* const W=GetWorld();UStaticMesh* const Mesh=GetStaticMesh().Get();
    TStrongObjectPtr<AActor> ActorPin(Target);TStrongObjectPtr<UWorld> WorldPin(W);TStrongObjectPtr<UStaticMesh> MeshPin(Mesh);
    const auto Live=[&](){return DDSCompoundObjectLive(this) && DDSCompoundObjectLive(Target) && !Target->IsActorBeingDestroyed() && DDSCompoundObjectLive(W) && !W->bIsTearingDown
       && GetOwner()==Target && GetOuter()==Target && GetWorld()==W && GetStaticMesh().Get()==Mesh && !Lease.IsValid() && !IsRegistered() && !IsPhysicsStateCreated()
       && !BodyInstance.IsValidBodyInstance() && !IsSimulatingPhysics() && GetCollisionEnabled()==ECollisionEnabled::NoCollision && !OriginalCargoVertexSetup && !OriginalChassisCompoundSetup;};
    if(!Live() || !DDSCompoundObjectLive(Mesh) || Mesh->GetPathName()!=TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO"))
    {Error=TEXT("Only exact live unregistered unleased original BurgerBox may prepare local vertex collision");return false;}
    for(const auto& P:G.Parts)if(!DDSCompoundObjectLive(P.OriginalVisualSource) || P.OriginalVisualSource->GetWorld()!=W){Error=TEXT("Actual same-world original input source required");return false;}
    FDDSOriginalCargoVertexCollisionResult Result;if(!DDSBuildOriginalCargoVertexCollision(*this,G,Result,Error))return false;
    if(!Live() || !DDSCompoundObjectLive(Result.BodySetup.Get()) || Result.BodySetup->GetOuter()!=this || !DDSOriginalCargoVertexSourceUnchanged(Result.SourceReceipt))
    {Error=TEXT("Cargo vertex preparation target/source changed");return false;}
    OriginalCargoVertexSources.Add(Target);OriginalCargoVertexSources.Add(W);OriginalCargoVertexSources.Add(Mesh);for(const auto& P:Result.SourceLifetime)OriginalCargoVertexSources.Add(P.Get());
    OriginalCargoVertexPreparedMaterial=Result.BodySetup->PhysMaterial.Get();OriginalCargoVertexGuid=Result.BodySetup->BodySetupGuid;
    OriginalCargoVertexCollisionBytes=CollisionSourceBytes(Result.BodySetup.Get());OriginalCargoVertexSettingsBytes=DDSCompoundSetupSettingsBytes(Result.BodySetup.Get());
    OriginalCargoVertexSourceReceipt=Result.SourceReceipt;OriginalCargoVertexSha256=Result.SourceVertexSha256;OriginalCargoVertexCount=Result.SourceVertexCount;
    OriginalCargoVertexSetup=Result.BodySetup.Get();bOriginalCargoVertexPrepared=true;return true;
}
bool UDDSRobotLeasedMeshComponent::PreparedOriginalCargoVertexCollisionUnchanged() const
{
    UBodySetup* const B=OriginalCargoVertexSetup.Get();return bOriginalCargoVertexPrepared && DDSCompoundObjectLive(B) && B->GetOuter()==this && B->BodySetupGuid==OriginalCargoVertexGuid
       && B->PhysMaterial.Get()==OriginalCargoVertexPreparedMaterial.Get() && (!OriginalCargoVertexPreparedMaterial || DDSCompoundObjectLive(OriginalCargoVertexPreparedMaterial.Get()))
       && CollisionSourceBytes(B)==OriginalCargoVertexCollisionBytes && DDSCompoundSetupSettingsBytes(B)==OriginalCargoVertexSettingsBytes && DDSOriginalCargoVertexSourceUnchanged(OriginalCargoVertexSourceReceipt);
}

void UDDSRobotLeasedMeshComponent::OnCreatePhysicsState()
{
    if (auto L=Lease.Pin()) if (!L->ConsumeOwnedPhysicsHook(this,true)) L->IndependentMutation();
    ++PhysicsStateRevision; Super::OnCreatePhysicsState();
}
void UDDSRobotLeasedMeshComponent::OnDestroyPhysicsState()
{
    if (auto L=Lease.Pin()) if (!L->ConsumeOwnedPhysicsHook(this,false)) L->IndependentMutation();
    ++PhysicsStateRevision; Super::OnDestroyPhysicsState();
}
void UDDSRobotLeasedMeshComponent::BeginDestroy()
{ RetireBeforeMutation(); Super::BeginDestroy(); }
void UDDSRobotLeasedMeshComponent::OnUpdateTransform(EUpdateTransformFlags F, ETeleportType T)
{
    if (!GetComponentScale().Equals(AdmittedScale, 1.e-6)) RetireBeforeMutation();
    Super::OnUpdateTransform(F, T);
}
