#include "DDSOriginalChassisCompoundCollision.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
namespace DDSOriginalCompound
{
constexpr TCHAR BaseMesh[]=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Base.SM_DeliveryRobot_Base");
constexpr TCHAR SuspensionMesh[]=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Suspension.SM_DeliveryRobot_Suspension");
constexpr TCHAR BaseSha[]=TEXT("1A0E9EB0BAC3FFBE8B9E44537A27B52B63CFF83A3020B6DFB0654717BA514F93");
constexpr TCHAR SuspensionSha[]=TEXT("4158C0293B9AC62DAA65B8170CEE153C7C70B59C97FB2B3D77F832D6181474DE");
struct FCompoundSourceSnapshot
{
    UStaticMeshComponent* Component=nullptr;UStaticMesh* Mesh=nullptr;UBodySetup* Setup=nullptr;
    AActor* Actor=nullptr;UWorld* World=nullptr;FString SourcePath,MeshPath;FTransform Pose;
    FGuid Guid;TArray<FKConvexElem> Hulls;FBodyInstance Defaults;UPhysicalMaterial* Material=nullptr;
    FVector BuildScale;ECollisionTraceFlag Trace=CTF_UseDefault;uint32 Flags=0;
};
uint32 CompoundSetupFlags(const UBodySetup& B)
{
    return uint32(B.bMeshCollideAll) | uint32(B.bDoubleSidedGeometry)<<1 | uint32(B.bGenerateNonMirroredCollision)<<2
        | uint32(B.bGenerateMirroredCollision)<<3 | uint32(B.bSharedCookedData)<<4 | uint32(B.bSupportUVsAndFaceRemap)<<5 | uint32(B.bNeverNeedsCookedCollisionData)<<6;
}
bool CompoundOnlyConvex(const FKAggregateGeom& A)
{
    return A.ConvexElems.Num()>0 && A.ConvexElems.Num()<=32 && !A.BoxElems.Num() && !A.SphereElems.Num()
        && !A.SphylElems.Num() && !A.TaperedCapsuleElems.Num() && !A.LevelSetElems.Num() && !A.SkinnedLevelSetElems.Num()
        && A.GetElementCount()==A.ConvexElems.Num();
}
bool CompoundLoaded(const UObject* O) { return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed); }
bool CompoundFinitePose(const FTransform& T) { return T.IsValid() && T.GetScale3D().GetMin()>0; }
}
bool DDSBuildOriginalChassisCompoundCollision(UObject& Owner,const FDDSOriginalRobotNativeGeometry& Supplied,
    FDDSOriginalChassisCompoundResult& Out,FString& Error)
{
    using namespace DDSOriginalCompound;Out={};Error.Reset();const auto G=Supplied;
    const auto Refuse=[&](const TCHAR* Why) { Out={};Error=Why;return false; };
    if(!IsInGameThread() || !CompoundLoaded(&Owner) || !G.bActualOriginalAssetsAndTemplatesObserved || G.Parts.Num()!=16
       || G.IntegrityLedgerSha256!=TEXT("88178075AEEC58333E73C576A9ABFDF3411A209E3BAB7B7ACD0445D091BEA7DA"))return Refuse(TEXT("Actual admitted original live-intake snapshot required"));
    UScriptStruct* const DefaultsType=FBodyInstance::StaticStruct();UScriptStruct* const ConvexType=FKConvexElem::StaticStruct(); // Initialize reflection before source snapshots/allocation callbacks.
    const FDDSOriginalRobotNativePart* Source[7]={};TSet<int32> Selected;
    for(const auto& P:G.Parts) { int32 S=INDEX_NONE;
        if(P.Role==EDDSOriginalRobotPartRole::Body && P.Index==0)S=0;
        else if(P.Role==EDDSOriginalRobotPartRole::Suspension && P.Index>=0 && P.Index<6)S=P.Index+1;
        if(S!=INDEX_NONE) { if(Selected.Contains(S))return Refuse(TEXT("Duplicate original compound role"));Selected.Add(S);Source[S]=&P; }
    }
    if(Selected.Num()!=7)return Refuse(TEXT("Exact original base plus all6 suspension sources required"));
    FDDSOriginalChassisCompoundResult Candidate;Candidate.SourceLifetime.Emplace(&Owner);Candidate.NativeSourceRecordSha256=G.NativeGeometryRecordSha256;
    UStaticMeshComponent* C[7]={};UBodySetup* OriginalSetup[7]={};FTransform Poses[7];FString Paths[7];FCompoundSourceSnapshot Snap[7];int32 TotalHulls=0,TotalVertices=0,TotalIndices=0;TSet<UStaticMeshComponent*> Seen;
    for(int32 S=0;S<7;++S) { const auto& P=*Source[S];C[S]=P.OriginalVisualSource;
        if(!P.bNativeTemplateAndPoseObserved || !CompoundLoaded(C[S]) || !C[S]->IsRegistered() || !CompoundLoaded(C[S]->GetOwner()) || C[S]->GetOwner()->IsActorBeingDestroyed() || !CompoundLoaded(C[S]->GetWorld()) || C[S]->GetWorld()->bIsTearingDown || Seen.Contains(C[S])
           || !CompoundLoaded(P.SuppliedMesh) || C[S]->GetStaticMesh().Get()!=P.SuppliedMesh || P.SuppliedMesh->IsCompiling()
           || P.SuppliedMesh->GetPathName()!=(S?SuspensionMesh:BaseMesh) || P.ExactMeshObjectPath!=P.SuppliedMesh->GetPathName()
           || P.MeshPackageSha256!=(S?SuspensionSha:BaseSha) || !CompoundFinitePose(P.MeasuredWorldCm)
           || !P.MeasuredWorldCm.GetScale3D().Equals(FVector::OneVector,1.e-9) || !C[S]->GetComponentTransform().Equals(P.MeasuredWorldCm,1.e-6))return Refuse(TEXT("Exact loaded original mesh/hash/source/observed unit supplier pose required"));
        if(S && (C[S]->GetOwner()!=C[0]->GetOwner() || C[S]->GetWorld()!=C[0]->GetWorld() || C[S]->GetAttachParent()!=C[0]))return Refuse(TEXT("Actual original suspension must have authored base parent; rigid-motion hypothesis still unproved"));
        OriginalSetup[S]=P.SuppliedMesh->GetBodySetup();if(!CompoundLoaded(OriginalSetup[S]))return Refuse(TEXT("Immutable original mesh bodysetup unavailable"));
        const auto& A=OriginalSetup[S]->AggGeom;
        if(!CompoundOnlyConvex(A) || OriginalSetup[S]->GetCollisionTraceFlag()==CTF_UseComplexAsSimple)return Refuse(TEXT("Native current original convex-only collision roster required; every unsupported shape refuses"));
        auto& N=Snap[S];N.Component=C[S];N.Mesh=P.SuppliedMesh;N.Setup=OriginalSetup[S];N.Actor=C[S]->GetOwner();N.World=C[S]->GetWorld();N.SourcePath=C[S]->GetPathName();N.MeshPath=P.SuppliedMesh->GetPathName();N.Pose=P.MeasuredWorldCm;
        N.Guid=N.Setup->BodySetupGuid;N.Material=N.Setup->PhysMaterial.Get();N.Trace=N.Setup->GetCollisionTraceFlag();N.BuildScale=N.Setup->BuildScale3D;N.Flags=CompoundSetupFlags(*N.Setup);
        if(N.Material && !CompoundLoaded(N.Material))return Refuse(TEXT("Original physical material unresolved"));
        if(!N.BuildScale.Equals(FVector::OneVector,1.e-9))return Refuse(TEXT("Unsupported original collision build scale"));
        N.Defaults=N.Setup->DefaultInstance;N.Hulls.Reserve(A.ConvexElems.Num());
        for(const auto& Hull:A.ConvexElems) { TotalHulls++;TotalVertices+=Hull.VertexData.Num();TotalIndices+=Hull.IndexData.Num();
            if(TotalHulls>128 || Hull.VertexData.Num()<4 || Hull.VertexData.Num()>4096 || Hull.IndexData.Num()>24576 || TotalVertices>65536 || TotalIndices>393216
               || !CompoundFinitePose(Hull.GetTransform()) || !FMath::IsFinite(Hull.RestOffset))return Refuse(TEXT("Original snapshot hull/vertex/index budget or transform invalid"));
            for(const FVector& V:Hull.VertexData)if(V.ContainsNaN())return Refuse(TEXT("Original snapshot vertex nonfinite"));
            FKConvexElem& StoredSource=N.Hulls.Add_GetRef(Hull);
#if WITH_EDITORONLY_DATA
            // UE5.3 CloneElem omits this reflected editor field. Preserve after insertion.
            StoredSource.bIsGenerated=Hull.bIsGenerated;
#endif
        }
        if(N.Material)Candidate.SourceLifetime.Emplace(N.Material);
        Candidate.SourceLifetime.Emplace(C[S]);Candidate.SourceLifetime.Emplace(C[S]->GetOwner());Candidate.SourceLifetime.Emplace(C[S]->GetWorld());
        Candidate.SourceLifetime.Emplace(P.SuppliedMesh);Candidate.SourceLifetime.Emplace(OriginalSetup[S]);Seen.Add(C[S]);Poses[S]=P.MeasuredWorldCm;Paths[S]=C[S]->GetPathName();
    }
    UBodySetup* Built=NewObject<UBodySetup>(&Owner,NAME_None,RF_Transient);if(!Built)return Refuse(TEXT("Transient compound setup allocation unavailable"));Candidate.BodySetup.Reset(Built);
    Built->BodySetupGuid=FGuid::NewGuid(); // Native transient object identity ONLY, never an evidence/calibration hash.
    Built->CollisionTraceFlag=CTF_UseSimpleAsComplex;Built->bGenerateMirroredCollision=false;
    Built->DefaultInstance=Snap[0].Defaults;Built->PhysMaterial=Snap[0].Material;Built->AggGeom.ConvexElems.Reserve(TotalHulls);
    for(int32 S=0;S<7;++S) { const FTransform Rest=Poses[S].GetRelativeTransform(Poses[0]);
        if(!CompoundFinitePose(Rest) || !Rest.GetScale3D().Equals(FVector::OneVector,1.e-9))return Refuse(TEXT("Original chassis-relative rigid unit frame unavailable"));
        const auto& Hulls=Snap[S].Hulls;
        for(int32 H=0;H<Hulls.Num();++H) { const auto& Original=Hulls[H];
            if(Original.VertexData.Num()<4 || Original.VertexData.Num()>4096 || !CompoundFinitePose(Original.GetTransform()) || Candidate.HullOrigins.Num()>=128)return Refuse(TEXT("Original convex vertices/transform outside finite supported roster"));
            for(const FVector& V:Original.VertexData)if(V.ContainsNaN())return Refuse(TEXT("Original convex vertex is nonfinite"));
            FKConvexElem Hull(Original);const FTransform Composed=Original.GetTransform()*Rest;if(!CompoundFinitePose(Composed))return Refuse(TEXT("Nonfinite original hull composed frame"));
            Hull.SetTransform(Composed);Hull.BakeTransformToVerts();
            for(const FVector& V:Hull.VertexData)if(V.ContainsNaN())return Refuse(TEXT("Baked original hull vertex nonfinite"));
            Hull.ResetChaosConvexMesh();Hull.UpdateElemBox();
            // Only the new copied geometry is cooked. Original package bodysetup stays byte-preserved.
            FKConvexElem& StoredOutput=Built->AggGeom.ConvexElems.Add_GetRef(Hull);
#if WITH_EDITORONLY_DATA
            StoredOutput.bIsGenerated=Original.bIsGenerated;
#endif
            FDDSOriginalCompoundHullOrigin Origin;Origin.OriginalPartId=Source[S]->SuppliedPartId;Origin.OriginalMeshObjectPath=Source[S]->ExactMeshObjectPath;
            Origin.OriginalMeshPackageSha256=Source[S]->MeshPackageSha256;Origin.OriginalBodySetupGuid=Snap[S].Guid;Origin.SourceHullIndex=H;Origin.SourceHullToChassisCm=Composed;Candidate.HullOrigins.Add(Origin);
        }
    }
    Built->CreatePhysicsMeshes();
    if(!CompoundLoaded(&Owner) || !CompoundLoaded(Built) || Built->AggGeom.ConvexElems.Num()!=Candidate.HullOrigins.Num())return Refuse(TEXT("Compound physics cooking/lifetime failed"));
    for(const auto& Hull:Built->AggGeom.ConvexElems)if(!Hull.GetChaosConvexMesh().IsValid())return Refuse(TEXT("Actual copied original convex cook unavailable"));
    // Exact coherent reread, after all callback-capable allocation/cook operations.
    for(int32 S=0;S<7;++S) { const auto& N=Snap[S];auto* P=N.Component;auto* B=N.Setup;
        if(!CompoundLoaded(P) || !P->IsRegistered() || !CompoundLoaded(N.Actor) || N.Actor->IsActorBeingDestroyed() || !CompoundLoaded(N.World) || N.World->bIsTearingDown
           || P->GetOwner()!=N.Actor || P->GetWorld()!=N.World || P->GetPathName()!=N.SourcePath || P->GetStaticMesh().Get()!=N.Mesh || !CompoundLoaded(N.Mesh) || N.Mesh->IsCompiling()
           || N.Mesh->GetPathName()!=N.MeshPath || N.Mesh->GetBodySetup()!=B || !CompoundLoaded(B) || !P->GetComponentTransform().Equals(N.Pose,0)
           || B->BodySetupGuid!=N.Guid || B->PhysMaterial.Get()!=N.Material || (N.Material && !CompoundLoaded(N.Material)) || B->GetCollisionTraceFlag()!=N.Trace
           || !B->BuildScale3D.Equals(N.BuildScale,0) || CompoundSetupFlags(*B)!=N.Flags || !CompoundOnlyConvex(B->AggGeom) || B->AggGeom.ConvexElems.Num()!=N.Hulls.Num()
           || !DefaultsType->CompareScriptStruct(&B->DefaultInstance,&N.Defaults,0)
           || (S && P->GetAttachParent()!=Snap[0].Component))return Refuse(TEXT("Original compound source content/identity/owner/world changed"));
        for(int32 H=0;H<N.Hulls.Num();++H)if(!ConvexType->CompareScriptStruct(&B->AggGeom.ConvexElems[H],&N.Hulls[H],0))return Refuse(TEXT("Original hull vertices/index/transform/settings changed during compound cook"));
    }
    Out=MoveTemp(Candidate);return true; // Shape production only, not rigid motion, owner mass/COM/inertia or nine-body admission.
}
