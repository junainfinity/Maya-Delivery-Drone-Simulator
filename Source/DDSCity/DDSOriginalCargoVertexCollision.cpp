#include "DDSOriginalCargoVertexCollision.h"
#include "DDSStaticMeshNaniteObservation.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "UObject/Class.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "HAL/PlatformProcess.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
namespace DDSOriginalCargoVertices
{
constexpr TCHAR CargoPath[]=TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO");
constexpr TCHAR CargoPackageSha[]=TEXT("C0B31055143F04A1512DB2A795A5A5B2E359D1CE6B611E8F7EFC00502624F895");
bool CargoLoaded(const UObject* O) { return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed); }
bool CargoPose(const FTransform& T) { const FVector S=T.GetScale3D();return T.IsValid() && S.GetMin()>0 && FMath::IsNearlyEqual(S.X,S.Y,1.e-9) && FMath::IsNearlyEqual(S.X,S.Z,1.e-9); }
// UE5.3 CPU pointers are cached even after upload/stream-out deletes storage.
// Game-thread state admission precedes LOD reference acquisition and every reread.
bool CargoResidentRetainedCpuLOD0(UStaticMesh& Mesh,const FStaticMeshRenderData* Render)
{
    if(!IsInGameThread() || Mesh.IsCompiling() || Mesh.HasPendingInitOrStreaming(true)
       || Mesh.HasPendingRenderResourceInitialization() || Mesh.GetRenderData()!=Render || !Render
       || Render->LODResources.Num()==0 || Render->CurrentFirstLODIdx!=0)return false;
    const auto State=Mesh.GetStreamableResourceState();
    if(!State.IsValid() || State.NumResidentLODs==0 || State.NumRequestedLODs!=State.NumResidentLODs
       || State.LODCountToAssetFirstLODIdx(State.NumResidentLODs)!=0
       || State.LODCountToAssetFirstLODIdx(State.NumRequestedLODs)!=0)return false;
    const auto& Positions=Render->LODResources[0].VertexBuffers.PositionVertexBuffer;
    return Positions.GetAllowCPUAccess() && Positions.GetVertexData()!=nullptr;
}
TArray<uint8> CargoSetupBytes(UBodySetup& B)
{
    TArray<uint8> Bytes;FMemoryWriter W(Bytes,false);FObjectAndNameAsStringProxyArchive A(W,false);
    FKAggregateGeom::StaticStruct()->SerializeItem(A,&B.AggGeom,nullptr);FBodyInstance::StaticStruct()->SerializeItem(A,&B.DefaultInstance,nullptr);
    FVector Scale=B.BuildScale3D;A<<Scale;
    uint32 Flags=uint32(B.GetCollisionTraceFlag()) | uint32(B.bMeshCollideAll)<<8 | uint32(B.bDoubleSidedGeometry)<<9 | uint32(B.bGenerateNonMirroredCollision)<<10
        | uint32(B.bGenerateMirroredCollision)<<11 | uint32(B.bSharedCookedData)<<12 | uint32(B.bSupportUVsAndFaceRemap)<<13 | uint32(B.bNeverNeedsCookedCollisionData)<<14;A<<Flags;return Bytes;
}
bool CargoHashVertices(const TArray<FVector3f>& Positions,FString& Out)
{
    TArray<uint8> Bytes;FMemoryWriter W(Bytes,false);for(const auto& P:Positions) { float X=P.X,Y=P.Y,Z=P.Z;W<<X;W<<Y;W<<Z; }
    void* Dll=FPlatformProcess::GetDllHandle(TEXT("bcrypt.dll"));if(!Dll)return false;
    const auto Open=reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptOpenAlgorithmProvider")));
    using FCargoDigest=NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE,PUCHAR,ULONG,PUCHAR,ULONG,PUCHAR,ULONG);
    const auto Digest=reinterpret_cast<FCargoDigest>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptHash")));
    const auto Close=reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptCloseAlgorithmProvider")));
    BCRYPT_ALG_HANDLE Algorithm=nullptr;uint8 Raw[32]={};bool Good=false;Out.Reset();
    if(Open && Digest && Close && Open(&Algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0) { Good=Digest(Algorithm,nullptr,0,Bytes.GetData(),Bytes.Num(),Raw,32)>=0;Close(Algorithm,0); }
    FPlatformProcess::FreeDllHandle(Dll);if(Good)for(uint8 V:Raw){Out.AppendChar(TEXT("0123456789ABCDEF")[V>>4]);Out.AppendChar(TEXT("0123456789ABCDEF")[V&15]);}return Good;
}
}
struct FDDSOriginalCargoVertexSourceReceipt
{
    UStaticMeshComponent* Component=nullptr;AActor* SourceOwner=nullptr;UWorld* World=nullptr;UStaticMesh* Mesh=nullptr;UBodySetup* OriginalSetup=nullptr;
    FString ComponentPath,MeshPath;FTransform Pose;FGuid OriginalGuid;UPhysicalMaterial* OriginalMaterial=nullptr;
    TArray<uint8> OriginalSetupBytes;FBodyInstance Defaults;TArray<UMaterialInterface*> DefaultMaterials,OverrideMaterials;
    const FStaticMeshRenderData* Render=nullptr;TRefCountPtr<const FStaticMeshLODResources> Lod;const void* VertexData=nullptr;uint32 Stride=0;
    TArray<FVector3f> Positions;TArray<FString> DefaultMaterialPaths,OverrideMaterialPaths;FString PhysicalMaterialPath;
};
bool DDSOriginalCargoVertexSourceUnchanged(const TSharedPtr<const FDDSOriginalCargoVertexSourceReceipt>& R)
{
    using namespace DDSOriginalCargoVertices;if(!R)return false;const auto& S=*R;auto* C=S.Component;auto* B=S.OriginalSetup;
    if(!CargoLoaded(C) || !C->IsRegistered() || !CargoLoaded(S.SourceOwner) || S.SourceOwner->IsActorBeingDestroyed() || !CargoLoaded(S.World) || S.World->bIsTearingDown
        || C->GetOwner()!=S.SourceOwner || C->GetWorld()!=S.World || C->GetPathName()!=S.ComponentPath || !C->GetComponentTransform().Equals(S.Pose,0)
        || !CargoLoaded(S.Mesh) || S.Mesh->IsCompiling() || C->GetStaticMesh().Get()!=S.Mesh || S.Mesh->GetPathName()!=S.MeshPath || S.Mesh->GetBodySetup()!=B
        || !CargoLoaded(B) || B->BodySetupGuid!=S.OriginalGuid || B->PhysMaterial.Get()!=S.OriginalMaterial || (S.OriginalMaterial && (!CargoLoaded(S.OriginalMaterial) || S.OriginalMaterial->GetPathName()!=S.PhysicalMaterialPath))
        || CargoSetupBytes(*B)!=S.OriginalSetupBytes || S.Mesh->GetStaticMaterials().Num()!=S.DefaultMaterials.Num() || C->OverrideMaterials.Num()!=S.OverrideMaterials.Num()
        || !CargoResidentRetainedCpuLOD0(*S.Mesh,S.Render) || &S.Render->LODResources[0]!=S.Lod.GetReference())return false;
    for(int32 I=0;I<S.DefaultMaterials.Num();++I)if(S.Mesh->GetStaticMaterials()[I].MaterialInterface.Get()!=S.DefaultMaterials[I] || (S.DefaultMaterials[I] && (!CargoLoaded(S.DefaultMaterials[I]) || S.DefaultMaterials[I]->GetPathName()!=S.DefaultMaterialPaths[I])))return false;
    for(int32 I=0;I<S.OverrideMaterials.Num();++I)if(C->OverrideMaterials[I].Get()!=S.OverrideMaterials[I] || (S.OverrideMaterials[I] && (!CargoLoaded(S.OverrideMaterials[I]) || S.OverrideMaterials[I]->GetPathName()!=S.OverrideMaterialPaths[I])))return false;
    const auto& V=S.Lod->VertexBuffers.PositionVertexBuffer;
    if(!CargoResidentRetainedCpuLOD0(*S.Mesh,S.Render) || !V.GetAllowCPUAccess() || V.GetVertexData()!=S.VertexData || !S.VertexData || V.GetStride()!=S.Stride || V.GetNumVertices()!=uint32(S.Positions.Num()) || V.GetAllocatedSize()<SIZE_T(S.Stride)*S.Positions.Num())return false;
    for(int32 I=0;I<S.Positions.Num();++I)if(!CargoResidentRetainedCpuLOD0(*S.Mesh,S.Render) || V.VertexPosition(I)!=S.Positions[I])return false;
    return true; // raw callback-free material/vertex reread; never GetMaterial.
}
bool DDSBuildOriginalCargoVertexCollision(UObject& Owner,const FDDSOriginalRobotNativeGeometry& Supplied,FDDSOriginalCargoVertexCollisionResult& Out,FString& Error)
{
    using namespace DDSOriginalCargoVertices;Out={};Error.Reset();const auto G=Supplied;
    const auto Refuse=[&](const TCHAR* Why){Out={};Error=Why;return false;};
    if(!IsInGameThread() || !CargoLoaded(&Owner) || !G.bActualOriginalAssetsAndTemplatesObserved || G.Parts.Num()!=16
       || G.IntegrityLedgerSha256!=TEXT("88178075AEEC58333E73C576A9ABFDF3411A209E3BAB7B7ACD0445D091BEA7DA")
       || G.NativeGeometryRecordSha256!=TEXT("1CC79F31B10984B3DCA0A4ADB4CE0A540AFB6ED552E4C75A5DC66CEF993BB9A3"))return Refuse(TEXT("Actual admitted original16 live intake/native provenance required"));
    FBodyInstance::StaticStruct();FKAggregateGeom::StaticStruct(); // initialize reflection before snapshot/allocation callbacks.
    const FDDSOriginalRobotNativePart* P=nullptr;for(const auto& Part:G.Parts)if(Part.Role==EDDSOriginalRobotPartRole::Parcel && Part.Index==0){if(P)return Refuse(TEXT("Duplicate original cargo role"));P=&Part;}
    if(!P || !P->bNativeTemplateAndPoseObserved || P->ExactMeshObjectPath!=CargoPath || P->MeshPackageSha256!=CargoPackageSha || !CargoPose(P->MeasuredWorldCm)
       || !CargoLoaded(P->OriginalVisualSource) || !P->OriginalVisualSource->IsRegistered() || !CargoLoaded(P->SuppliedMesh) || P->SuppliedMesh->IsCompiling() || (!DDSStaticMeshNanite::ObservationKnown(*P->SuppliedMesh) || DDSStaticMeshNanite::SourceEnabledOrCookedData(*P->SuppliedMesh))
       || P->OriginalVisualSource->GetStaticMesh().Get()!=P->SuppliedMesh || P->SuppliedMesh->GetPathName()!=CargoPath
       || P->OriginalVisualSource->GetPathName()!=P->ExactVisualSourceObjectPath || !P->OriginalVisualSource->GetComponentTransform().Equals(P->MeasuredWorldCm,0))return Refuse(TEXT("Exact observed original BurgerBox mesh/package/source/current pose required"));
    FDDSOriginalCargoVertexCollisionResult Candidate;Candidate.SourceLifetime.Emplace(&Owner);
    auto S=MakeShared<FDDSOriginalCargoVertexSourceReceipt>();S->Component=P->OriginalVisualSource;S->SourceOwner=S->Component->GetOwner();S->World=S->Component->GetWorld();S->Mesh=P->SuppliedMesh;S->OriginalSetup=S->Mesh->GetBodySetup();S->Pose=P->MeasuredWorldCm;
    if(!CargoLoaded(S->SourceOwner) || S->SourceOwner->IsActorBeingDestroyed() || !CargoLoaded(S->World) || S->World->bIsTearingDown || !CargoLoaded(S->OriginalSetup)
       || !S->OriginalSetup->BuildScale3D.Equals(FVector::OneVector,1.e-9) || S->OriginalSetup->AggGeom.GetElementCount()!=0)return Refuse(TEXT("Live source with immutable empty original simple collision required; no supplied hull is replaced"));
    Candidate.SourceLifetime.Emplace(S->Component);Candidate.SourceLifetime.Emplace(S->SourceOwner);Candidate.SourceLifetime.Emplace(S->World);Candidate.SourceLifetime.Emplace(S->Mesh);Candidate.SourceLifetime.Emplace(S->OriginalSetup);
    S->ComponentPath=S->Component->GetPathName();S->MeshPath=S->Mesh->GetPathName();S->OriginalGuid=S->OriginalSetup->BodySetupGuid;S->OriginalMaterial=S->OriginalSetup->PhysMaterial.Get();
    if(S->OriginalMaterial){if(!CargoLoaded(S->OriginalMaterial))return Refuse(TEXT("Original physical material unresolved"));Candidate.SourceLifetime.Emplace(S->OriginalMaterial);S->PhysicalMaterialPath=S->OriginalMaterial->GetPathName();}
    const auto& D=S->OriginalSetup->DefaultInstance;
    if(D.OwnerComponent.Get() || D.GetBodySetup() || FPhysicsInterface::IsValid(D.ActorHandle))return Refuse(TEXT("Original authored BodySetup defaults contain runtime ownership"));
    S->Defaults=D;S->OriginalSetupBytes=CargoSetupBytes(*S->OriginalSetup);
    if(S->Mesh->GetStaticMaterials().Num()>64 || S->Component->OverrideMaterials.Num()>64)return Refuse(TEXT("Original material slot budget exceeded"));
    for(const auto& M:S->Mesh->GetStaticMaterials()){auto* V=M.MaterialInterface.Get();if(V && !CargoLoaded(V))return Refuse(TEXT("Original default material unresolved"));S->DefaultMaterials.Add(V);S->DefaultMaterialPaths.Add(V?V->GetPathName():FString());if(V)Candidate.SourceLifetime.Emplace(V);}
    for(const auto& M:S->Component->OverrideMaterials){auto* V=M.Get();if(V && !CargoLoaded(V))return Refuse(TEXT("Original override material unresolved"));S->OverrideMaterials.Add(V);S->OverrideMaterialPaths.Add(V?V->GetPathName():FString());if(V)Candidate.SourceLifetime.Emplace(V);}
    S->Render=S->Mesh->GetRenderData();if(!CargoResidentRetainedCpuLOD0(*S->Mesh,S->Render))return Refuse(TEXT("Original LOD0 is not stable resident retained CPU data: pending init/streaming, nonresident LOD0 or CPU storage unavailable; no original flags changed"));
    S->Lod=&S->Render->LODResources[0];const auto& V=S->Lod->VertexBuffers.PositionVertexBuffer;const uint32 Count=V.GetNumVertices();
    S->VertexData=V.GetVertexData();S->Stride=V.GetStride();
    if(!CargoResidentRetainedCpuLOD0(*S->Mesh,S->Render) || !V.GetAllowCPUAccess() || Count<4 || Count>65536 || S->Stride!=sizeof(FPositionVertex) || !S->VertexData || V.GetAllocatedSize()<SIZE_T(Count)*S->Stride)return Refuse(TEXT("Original bounded LOD0 CPU positions unavailable; packaged geometry artifact is still required, AllowCPUAccess never modified"));
    S->Positions.Reserve(Count);for(uint32 I=0;I<Count;++I){if(!CargoResidentRetainedCpuLOD0(*S->Mesh,S->Render))return Refuse(TEXT("Original LOD0 CPU residency changed before vertex read"));const auto Q=V.VertexPosition(I);if(!FMath::IsFinite(Q.X) || !FMath::IsFinite(Q.Y) || !FMath::IsFinite(Q.Z))return Refuse(TEXT("Original LOD0 vertex nonfinite"));S->Positions.Add(Q);}
    Candidate.SourceReceipt=S;Candidate.SourceVertexCount=Count;Candidate.OriginalMeshPackageSha256=P->MeshPackageSha256;Candidate.NativeGeometryRecordSha256=G.NativeGeometryRecordSha256;Candidate.ObservedSourceWorldCm=S->Pose;
    if(!CargoHashVertices(S->Positions,Candidate.SourceVertexSha256) || !DDSOriginalCargoVertexSourceUnchanged(Candidate.SourceReceipt))return Refuse(TEXT("Original exact vertex snapshot hash/content changed before allocation"));
    UBodySetup* Built=NewObject<UBodySetup>(&Owner,NAME_None,RF_Transient);if(!Built)return Refuse(TEXT("Transient original-vertex setup allocation failed"));Candidate.BodySetup.Reset(Built);
    Built->BodySetupGuid=FGuid::NewGuid();Built->CollisionTraceFlag=CTF_UseSimpleAsComplex;Built->bGenerateMirroredCollision=false;Built->BuildScale3D=FVector::OneVector;
    Built->DefaultInstance=S->Defaults;Built->PhysMaterial=S->OriginalMaterial;
    FKConvexElem Hull;Hull.VertexData.Reserve(Count);for(const auto& Q:S->Positions)Hull.VertexData.Add(FVector(Q));Hull.SetTransform(FTransform::Identity);Hull.UpdateElemBox();Built->AggGeom.ConvexElems.Add(MoveTemp(Hull));
    const FGuid ExpectedBuiltGuid=Built->BodySetupGuid;
    Built->CreatePhysicsMeshes();
    if(!CargoLoaded(&Owner) || !CargoLoaded(Built) || Built->AggGeom.ConvexElems.Num()!=1 || !Built->AggGeom.ConvexElems[0].GetChaosConvexMesh().IsValid()
       || !DDSOriginalCargoVertexSourceUnchanged(Candidate.SourceReceipt))return Refuse(TEXT("Actual transient original-vertex cook or coherent source content/lifetime unavailable"));
    if(Built->GetOuter()!=&Owner || Built->BodySetupGuid!=ExpectedBuiltGuid || Built->PhysMaterial.Get()!=S->OriginalMaterial
       || Built->GetCollisionTraceFlag()!=CTF_UseSimpleAsComplex || !Built->BuildScale3D.Equals(FVector::OneVector,0)
       || Built->AggGeom.GetElementCount()!=1 || !Built->AggGeom.ConvexElems[0].GetTransform().Equals(FTransform::Identity,0)
       || Built->AggGeom.ConvexElems[0].VertexData.Num()!=S->Positions.Num()
       || !FBodyInstance::StaticStruct()->CompareScriptStruct(&Built->DefaultInstance,&S->Defaults,0))return Refuse(TEXT("Derived original-vertex output identity/settings changed during cooking"));
    for(int32 I=0;I<S->Positions.Num();++I)if(Built->AggGeom.ConvexElems[0].VertexData[I]!=FVector(S->Positions[I]))return Refuse(TEXT("Derived shape input vertices changed during cooking"));
    Out=MoveTemp(Candidate);return true; // derived collision only, never measured mass/fit/manufacturer/readiness.
}
