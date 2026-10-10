#include "DDSOriginalRobotMeasurementProducer.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "StaticMeshCompiler.h"
#endif
#include "DDSRobotLifecycleOwner.h"
#include "DDSOriginalChassisCompoundCollision.h"
#include "DDSRobotJointAdmission.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Chaos/SimCallbackObject.h"
#include "Chaos/PBDJointConstraintData.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/ShapeInstance.h"
#include "Chaos/Box.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PBDRigidsSolver.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"

namespace DDSOriginalMeasurement
{
constexpr double StepS = 1.0 / 120.0;
constexpr double GravityCmps2 = -980.665;
bool Loaded(const UObject* O)
{ return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed); }
bool Positive(double V) { return FMath::IsFinite(V) && V > 0.0; }
bool Sha(const FString& S)
{ if(S.Len()!=64)return false; for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false; return true; }
bool Rigid(const FTransform& T)
{ return T.IsValid() && T.GetRotation().IsNormalized() && T.GetScale3D().Equals(FVector::OneVector,1.e-9); }
bool Uniform(const FVector& S)
{ return !S.ContainsNaN() && S.GetMin()>0 && FMath::IsNearlyEqual(S.X,S.Y,1.e-6) && FMath::IsNearlyEqual(S.X,S.Z,1.e-6); }
FTransform Meters(const FTransform& T)
{ return FTransform(T.GetRotation(),T.GetTranslation()*0.01,FVector::OneVector); }
double GameMass(const FDDSRobotPhysicsProfile& P,int32 I)
{ return I==0 ? P.BareChassisKg : (I<7 ? P.WheelKg : (I==7 ? P.Lid0Kg : P.Lid1Kg)); }
bool ConvexContent(UBodySetup* B,TArray<uint8>& Bytes)
{
    Bytes.Reset(); if(!Loaded(B) || B->GetCollisionTraceFlag()==CTF_UseComplexAsSimple)return false;
    const auto& A=B->AggGeom;
    // The supplied robot currently has convex-only simple geometry. Every other
    // shape refuses explicitly; a render box is never a substitute.
    if(A.ConvexElems.Num()<1 || A.ConvexElems.Num()>128 || A.GetElementCount()!=A.ConvexElems.Num())return false;
    FMemoryWriter W(Bytes,true); int32 Count=A.ConvexElems.Num(); W<<Count;
    FVector Scale=B->BuildScale3D; W<<Scale; int32 Trace=int32(B->GetCollisionTraceFlag()); W<<Trace;
    int32 VertexTotal=0;
    for(const auto& H:A.ConvexElems)
    {
        if(H.VertexData.Num()<4 || H.VertexData.Num()>4096 || H.IndexData.Num()>24576 || !H.GetTransform().IsValid())return false;
        VertexTotal+=H.VertexData.Num();if(VertexTotal>65536)return false;
        FTransform T=H.GetTransform(); W<<T; float Offset=H.RestOffset; W<<Offset;
        int32 N=H.VertexData.Num(); W<<N;
        for(const FVector& Input:H.VertexData) { if(Input.ContainsNaN())return false; FVector V=Input;W<<V; }
        int32 Indices=H.IndexData.Num(); W<<Indices;
        for(int32 Input:H.IndexData) { if(Input<0 || Input>=N)return false; W<<Input; }
    }
    return !W.IsError() && Bytes.Num()>0 && Bytes.Num()<=4*1024*1024;
}
bool SetupReceiptBytes(UBodySetup* Setup,TArray<uint8>& Collision,TArray<uint8>& Settings)
{
    Collision.Reset();Settings.Reset();if(!Loaded(Setup))return false;
    FMemoryWriter CW(Collision,false);FObjectAndNameAsStringProxyArchive CA(CW,false);
    FKAggregateGeom::StaticStruct()->SerializeItem(CA,&Setup->AggGeom,nullptr);
    FMemoryWriter SW(Settings,false);FObjectAndNameAsStringProxyArchive SA(SW,false);
    FBodyInstance::StaticStruct()->SerializeItem(SA,&Setup->DefaultInstance,nullptr);
    FVector Scale=Setup->BuildScale3D;SA<<Scale;
    uint32 Flags=uint32(Setup->GetCollisionTraceFlag()) | uint32(Setup->bMeshCollideAll)<<8 | uint32(Setup->bDoubleSidedGeometry)<<9
        | uint32(Setup->bGenerateNonMirroredCollision)<<10 | uint32(Setup->bGenerateMirroredCollision)<<11 | uint32(Setup->bSharedCookedData)<<12
        | uint32(Setup->bSupportUVsAndFaceRemap)<<13 | uint32(Setup->bNeverNeedsCookedCollisionData)<<14;SA<<Flags;
    return !CW.IsError() && !SW.IsError() && Collision.Num()>0 && Settings.Num()>0
        && Collision.Num()<=4*1024*1024 && Settings.Num()<=4*1024*1024;
}
bool ObserveBody(UStaticMeshComponent& C,double DeclaredKg,
    FDDSOriginalRobotNativeBodyObservation& Out,FString& Error)
{
    Out={};Out.DeclaredGameMassKg=DeclaredKg;
    auto Refuse=[&](const TCHAR* Why){Out.UnavailableReason=Why;Error=Why;return false;};
    auto* Mesh=C.GetStaticMesh().Get();auto* Setup=C.GetBodySetup();auto* B=C.GetBodyInstance();
    if(!Loaded(&C) || !Loaded(C.GetOwner()) || C.GetOwner()->IsActorBeingDestroyed()
       || !Loaded(C.GetWorld()) || C.GetWorld()->bIsTearingDown || !C.IsRegistered()
       || !Loaded(Mesh) || Mesh->IsCompiling() || !Loaded(Setup) || !Uniform(C.GetComponentScale())
       || !B || !B->IsValidBodyInstance() || !C.IsSimulatingPhysics())return Refuse(TEXT("Actual registered dynamic component body unavailable"));
    const FTransform Pose=C.GetComponentTransform();const FName Profile=C.GetCollisionProfileName();
    const FGuid Guid=Setup->BodySetupGuid;TArray<uint8> Before,After;
    if(!ConvexContent(Setup,Before))return Refuse(TEXT("Actual bounded convex component collision unavailable"));
    TStrongObjectPtr<UStaticMeshComponent> CPin(&C);TStrongObjectPtr<UStaticMesh> MPin(Mesh);
    TStrongObjectPtr<UBodySetup> SPin(Setup);TStrongObjectPtr<AActor> APin(C.GetOwner());TStrongObjectPtr<UWorld> WPin(C.GetWorld());
    double Mass=0;FVector Inertia=FVector::ZeroVector;FTransform Local,World,Actor;
    FVector Velocity=FVector::ZeroVector,Omega=FVector::ZeroVector;bool Entered=false,Valid=false;int32 Shapes=0;
    const bool Locked=FPhysicsCommand::ExecuteRead(B->GetPhysicsActorHandle(),[&](const FPhysicsActorHandle& H)
    {
        Entered=true;if(!FPhysicsInterface::IsValid(H) || !FPhysicsInterface::IsDynamic(H))return;
        Mass=FPhysicsInterface::GetMass_AssumesLocked(H);
        Inertia=FPhysicsInterface::GetLocalInertiaTensor_AssumesLocked(H);
        Local=FPhysicsInterface::GetComTransformLocal_AssumesLocked(H);
        World=FPhysicsInterface::GetComTransform_AssumesLocked(H);
        Actor=FPhysicsInterface::GetGlobalPose_AssumesLocked(H);
        Velocity=FPhysicsInterface::GetLinearVelocity_AssumesLocked(H);
        Omega=FPhysicsInterface::GetAngularVelocity_AssumesLocked(H);
        TArray<FPhysicsShapeHandle> Native;
        if(FPhysicsInterface::GetNumShapes(H)<1 || FPhysicsInterface::GetNumShapes(H)>128)return;
        FPhysicsInterface::GetAllShapes_AssumedLocked(H,Native);
        for(const auto& S:Native) { if(!S.IsValid())return; if(FPhysicsInterface::IsSimulationShape(S))++Shapes; }
        Valid=Shapes>0 && Positive(Mass) && !Inertia.ContainsNaN() && Positive(Inertia.X) && Positive(Inertia.Y) && Positive(Inertia.Z)
            && Rigid(Local) && Rigid(World) && Rigid(Actor) && !Velocity.ContainsNaN() && !Omega.ContainsNaN();
    });
    if(!Locked || !Entered || !Valid || !Loaded(&C) || !C.IsRegistered() || C.GetOwner()!=APin.Get() || APin->IsActorBeingDestroyed()
       || C.GetWorld()!=WPin.Get() || WPin->bIsTearingDown || C.GetStaticMesh().Get()!=Mesh || C.GetBodySetup()!=Setup
       || Setup->BodySetupGuid!=Guid || C.GetBodyInstance()!=B || !B->IsValidBodyInstance() || !C.IsSimulatingPhysics()
       || C.GetCollisionProfileName()!=Profile || !C.GetComponentTransform().Equals(Pose,1.e-6)
       || !ConvexContent(Setup,After) || Before!=After)return Refuse(TEXT("Native component mass/COM/inertia read lost coherent owner/setup/shape/pose"));
    Out.bAvailable=true;Out.ComponentObjectPath=C.GetPathName();Out.ComponentBodySetupObjectPath=Setup->GetPathName();
    Out.MeshBodySetupObjectPath=Mesh->GetBodySetup()?Mesh->GetBodySetup()->GetPathName():FString();Out.ComponentBodySetupGuid=Guid;
    Out.NativeSimulationShapeCount=Shapes;Out.NativeMassKg=Mass;Out.NativePrincipalInertiaKgM2=Inertia*1.e-4;
    Out.NativeMassSpaceBodyLocalMeters=Meters(Local);Out.NativeActorWorldCm=Actor;
    Out.NativeVelocityCmps=Velocity;Out.NativeOmegaRadps=Omega;
    Out.NativeComponentCollisionProfile=Profile;
    if(Positive(DeclaredKg)) { Out.GameToNativeMassRatio=DeclaredKg/Mass;
        Out.DeclaredGamePrincipalInertiaKgM2=Out.NativePrincipalInertiaKgM2*Out.GameToNativeMassRatio; }
    Error.Reset();return true;
}
const FDDSOriginalRobotNativePart* Part(const FDDSOriginalRobotNativeGeometry& G,EDDSOriginalRobotPartRole Role,int32 Index)
{ const FDDSOriginalRobotNativePart* Found=nullptr;for(const auto& P:G.Parts)if(P.Role==Role && P.Index==Index){if(Found)return nullptr;Found=&P;}return Found; }
bool SourceStillExact(const FDDSOriginalRobotNativePart& P)
{
    auto* C=P.OriginalVisualSource;
    return P.bNativeTemplateAndPoseObserved && Loaded(C) && C->IsRegistered() && Loaded(C->GetOwner())
        && !C->GetOwner()->IsActorBeingDestroyed() && Loaded(C->GetWorld()) && !C->GetWorld()->bIsTearingDown
        && C->GetStaticMesh().Get()==P.SuppliedMesh && Loaded(P.SuppliedMesh) && !P.SuppliedMesh->IsCompiling()
        && C->GetPathName()==P.ExactVisualSourceObjectPath && P.SuppliedMesh->GetPathName()==P.ExactMeshObjectPath
        && C->GetComponentTransform().Equals(P.MeasuredWorldCm,1.e-6) && Uniform(P.MeasuredWorldCm.GetScale3D());
}
void RetainJointSettings(const Chaos::FPBDJointSettings& S,FDDSOriginalRobotJointSourceObservation& Out)
{
    Out.NativeInChassisMeters=Meters(FTransform(FQuat(S.ConnectorTransforms[0].GetRotation()),FVector(S.ConnectorTransforms[0].GetTranslation())));
    Out.NativeInPartMeters=Meters(FTransform(FQuat(S.ConnectorTransforms[1].GetRotation()),FVector(S.ConnectorTransforms[1].GetTranslation())));
    Out.NativeLinearLimitM=S.LinearLimit*0.01;Out.bNativeAdjacentCollisionDisabled=!S.bCollisionEnabled;
    for(int32 Axis=0;Axis<3;++Axis){Out.NativeLinearMotion[Axis]=int32(S.LinearMotionTypes[Axis]);Out.NativeAngularMotion[Axis]=int32(S.AngularMotionTypes[Axis]);
        Out.NativeLinearPositionDriveEnabled[Axis]=S.bLinearPositionDriveEnabled[Axis];Out.NativeLinearVelocityDriveEnabled[Axis]=S.bLinearVelocityDriveEnabled[Axis];}
    Out.NativeAngularLimitsRad=FVector(S.AngularLimits);Out.NativeLinearDriveForceMode=int32(S.LinearDriveForceMode);
    Out.NativeLinearDriveTargetMeters=FVector(S.LinearDrivePositionTarget)*0.01;
    Out.NativeLinearDriveVelocityTargetMps=FVector(S.LinearDriveVelocityTarget)*0.01;
    Out.NativeLinearDriveStiffnessNpm=FVector(S.LinearDriveStiffness);Out.NativeLinearDriveDampingNspm=FVector(S.LinearDriveDamping);
    Out.NativeLinearDriveMaxForceN=FVector(S.LinearDriveMaxForce)*0.01;
}
bool NativeJoint(UPhysicsConstraintComponent& C,UStaticMeshComponent& A,UStaticMeshComponent& B,
    FDDSOriginalRobotJointSourceObservation& Out)
{
    Out={};if(!Loaded(&C) || !C.IsRegistered() || C.GetOwner()!=A.GetOwner() || C.GetWorld()!=A.GetWorld())return false;
    auto Ref=C.ConstraintInstance.GetPhysicsConstraintRef();if(!Ref.IsValid() || FPhysInterface_Chaos::IsBroken(Ref))return false;
    auto* BA=A.GetBodyInstance();auto* BB=B.GetBodyInstance();
    if(!BA || !BB || !BA->IsValidBodyInstance() || !BB->IsValidBodyInstance())return false;
    bool Good=false;FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadOnly(Ref,[&](const FPhysicsConstraintHandle& Locked)
    {
        if(!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
           || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType))return;
        const auto* J=static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);const auto E=J->GetPhysicsBodies();
        if(E[0]!=BA->GetPhysicsActorHandle()->GetPhysicsObject() || E[1]!=BB->GetPhysicsActorHandle()->GetPhysicsObject())return;
        const auto& S=J->GetJointSettings();FTransform T[2]={FTransform(FQuat(S.ConnectorTransforms[0].GetRotation()),FVector(S.ConnectorTransforms[0].GetTranslation())),
            FTransform(FQuat(S.ConnectorTransforms[1].GetRotation()),FVector(S.ConnectorTransforms[1].GetTranslation()))};
        if(!Rigid(T[0]) || !Rigid(T[1]) || !FMath::IsFinite(S.LinearLimit))return;
        RetainJointSettings(S,Out);Out.NativeConstraintIdentity=uint64(reinterpret_cast<UPTRINT>(Locked.Constraint));Good=true;
    });
    Out.bAvailable=Good && Loaded(&C) && C.IsRegistered() && C.ConstraintInstance.GetPhysicsConstraintRef().Constraint==Ref.Constraint;
    if(Out.bAvailable)Out.SourceObjectPath=C.GetPathName();return Out.bAvailable;
}
FTransform ParticlePose(const Chaos::FGeometryParticleHandle* H)
{ return FTransform(FQuat(H->R()),FVector(H->X())); }
uint64 RunIdentity(const void* Pointer){return uint64(reinterpret_cast<UPTRINT>(Pointer));}
bool PositiveScale(const FVector& S)
{ return !S.ContainsNaN() && Positive(S.X) && Positive(S.Y) && Positive(S.Z); }
bool ComponentBodyReceipt(UStaticMeshComponent& C,TArray<uint8>& Bytes)
{
    Bytes.Reset();FMemoryWriter W(Bytes,false);FObjectAndNameAsStringProxyArchive A(W,false);
    FBodyInstance::StaticStruct()->SerializeItem(A,&C.BodyInstance,nullptr);
    return !W.IsError() && !Bytes.IsEmpty() && Bytes.Num()<=1024*1024;
}
bool OriginalSingleBoxRaw(UStaticMeshComponent& C,FDDSOriginalRobotSupportAdapterObservation& O,FString& Error)
{
    const auto Fail=[&](const TCHAR* P){Error=P;return false;};
    auto* M=C.GetStaticMesh().Get();auto* B=C.GetBodySetup();
    if(!Loaded(&C) || C.GetClass()!=UStaticMeshComponent::StaticClass() || !C.IsRegistered()
       || !Loaded(C.GetOwner()) || C.GetOwner()->IsActorBeingDestroyed() || !C.GetOwner()->GetActorEnableCollision()
       || !Loaded(C.GetWorld()) || C.GetWorld()->bIsTearingDown || !Loaded(M) || M->IsCompiling() || !Loaded(B))
        return Fail(TEXT("SUPPORT_BOX_SOURCE_LIFETIME_OR_CLASS"));
    if(B!=M->GetBodySetup() || !B->BodySetupGuid.IsValid())return Fail(TEXT("SUPPORT_BOX_MESH_OWNED_SETUP_GUID"));
    if(B->GetCollisionTraceFlag()==CTF_UseComplexAsSimple)return Fail(TEXT("SUPPORT_BOX_COMPLEX_AS_SIMPLE"));
    if(B->AggGeom.BoxElems.Num()!=1 || B->AggGeom.GetElementCount()!=1)return Fail(TEXT("SUPPORT_BOX_SINGLE_SIMPLE_SHAPE"));
    const auto& Box=B->AggGeom.BoxElems[0];const FTransform T=C.GetComponentTransform();
    if(!T.IsValid() || !T.GetRotation().IsNormalized() || !T.GetRotation().IsIdentity(1.e-6)
       || !PositiveScale(T.GetScale3D()))return Fail(TEXT("SUPPORT_BOX_AXIS_ALIGNED_POSITIVE_COMPONENT_TRANSFORM"));
    if(!Positive(Box.X) || !Positive(Box.Y) || !Positive(Box.Z) || Box.Center.ContainsNaN()
       || Box.Rotation.ContainsNaN() || !Box.GetTransform().GetRotation().IsNormalized()
       || !Box.GetTransform().GetRotation().IsIdentity(1.e-6))return Fail(TEXT("SUPPORT_BOX_RAW_DIMENSIONS_CENTER_IDENTITY_ROTATION"));
    if(!PositiveScale(B->BuildScale3D) || !FMath::IsFinite(Box.RestOffset))return Fail(TEXT("SUPPORT_BOX_BUILD_SCALE_OR_REST_OFFSET"));
    if(C.Mobility!=EComponentMobility::Static || C.IsSimulatingPhysics()
       || C.GetCollisionEnabled()!=ECollisionEnabled::QueryAndPhysics || Box.GetCollisionEnabled()!=ECollisionEnabled::QueryAndPhysics
       || !C.GetBodyInstance() || C.BodyInstance.WeldParent || C.GetBodyInstance()!=&C.BodyInstance)return Fail(TEXT("SUPPORT_BOX_STATIC_UNWELDED_QUERY_AND_PHYSICS"));
    if(!SetupReceiptBytes(B,O.AggregateGeometryContent,O.SetupSettingsContent) || !ComponentBodyReceipt(C,O.ComponentBodySettingsContent))
        return Fail(TEXT("SUPPORT_BOX_RAW_RECEIPT_UNAVAILABLE"));
    O.AdapterTag=TEXT("ORIGINAL_SINGLE_BOX_PRESERVED");O.SourceComponentPath=C.GetPathName();O.SourceOwnerPath=C.GetOwner()->GetPathName();
    O.SourceWorldPath=C.GetWorld()->GetPathName();O.OriginalMeshPath=M->GetPathName();O.OriginalSetupPath=B->GetPathName();O.OriginalSetupGuid=B->BodySetupGuid;
    O.OriginalComponentWorldCm=T;O.BuildScale3D=B->BuildScale3D;O.BoxDimensionsCm=FVector(Box.X,Box.Y,Box.Z);O.BoxCenterCm=Box.Center;O.BoxRotation=Box.Rotation;
    O.CollisionTraceFlag=int32(B->GetCollisionTraceFlag());O.BoxCollisionEnabled=int32(Box.GetCollisionEnabled());O.ComponentCollisionProfile=C.GetCollisionProfileName();
    O.bRawSourceAvailable=true;return true;
}
bool SupportGeometryContent(const FPhysicsShapeHandle& S,FDDSOriginalRobotSupportNativeShapeObservation& O)
{
    const auto* G=S.Shape->GetGeometry().Get();if(!G || !G->HasBoundingBox())return false;
    O.GeometryType=int32(G->GetType());O.CollisionShapeType=int32(FPhysicsInterface::GetShapeType(S));
    const auto Bounds=G->BoundingBox();O.NativeBoundsMinCm=FVector(Bounds.Min());O.NativeBoundsMaxCm=FVector(Bounds.Max());
    if(O.NativeBoundsMinCm.ContainsNaN() || O.NativeBoundsMaxCm.ContainsNaN())return false;
    FMemoryWriter W(O.GeometryContent,true);W<<O.GeometryType;
    if(G->GetType()==Chaos::ImplicitObjectType::Box)
    {
        const auto* Box=G->GetObject<Chaos::TBox<Chaos::FReal,3>>();if(!Box)return false;
        O.NativeBoxMinCm=FVector(Box->Min());O.NativeBoxMaxCm=FVector(Box->Max());O.NativeBoxMarginCm=Box->GetMargin();
        if(!PositiveScale(O.NativeBoxMaxCm-O.NativeBoxMinCm) || !FMath::IsFinite(O.NativeBoxMarginCm) || O.NativeBoxMarginCm<0)return false;
        W<<O.NativeBoxMinCm<<O.NativeBoxMaxCm<<O.NativeBoxMarginCm;
    }
    else
    {
        // Only original query-only triangle geometry is additionally supported.
        // Bound its exact vertex/index/material-index receipt; no convex conversion.
        if(O.bSimulation || !O.bQuery || O.CollisionShapeType!=int32(ECollisionShapeType::Trimesh))return false;
        const Chaos::FTriangleMeshImplicitObject* Tri=nullptr;FVector Scale=FVector::OneVector;
        if(const auto* Scaled=G->GetObject<Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>>())
        {Tri=Scaled->GetUnscaledObject();Scale=FVector(Scaled->GetScale());}
        else if(const auto* Instanced=G->GetObject<Chaos::TImplicitObjectInstanced<Chaos::FTriangleMeshImplicitObject>>())Tri=Instanced->GetInstancedObject();
        else Tri=G->GetObject<Chaos::FTriangleMeshImplicitObject>();
        if(!Tri || !PositiveScale(Scale))return false;
        O.AssetQueryGeometryIdentity=RunIdentity(Tri);W<<Scale;
        int32 N=Tri->Particles().Size(),T=Tri->Elements().GetNumTriangles();
        if(N<3 || N>4096 || T<1 || T>8192)return false;W<<N<<T;
        for(int32 V=0;V<N;++V){FVector P=FVector(Tri->Particles().X(V));if(P.ContainsNaN())return false;W<<P;}
        const auto CopyIndices=[&](const auto& Indices)
        {
            for(int32 F=0;F<Indices.Num();++F)
            {for(int32 A=0;A<3;++A){int32 I=int32(Indices[F][A]);if(I<0 || I>=N)return false;W<<I;}
             uint16 Material=Tri->GetMaterialIndex(F);W<<Material;}
            return true;
        };
        if(Tri->Elements().RequiresLargeIndices()) {if(!CopyIndices(Tri->Elements().GetLargeIndexBuffer()))return false;}
        else if(!CopyIndices(Tri->Elements().GetSmallIndexBuffer()))return false;
        bool BackFace=Tri->GetCullsBackFaceRaycast();W<<BackFace;
    }
    return !W.IsError() && !O.GeometryContent.IsEmpty() && O.GeometryContent.Num()<=1024*1024;
}
bool SupportNativeRoster(UStaticMeshComponent& C,FDDSOriginalRobotSupportNativeBodyObservation& O,FString& Error)
{
    O={};const auto Fail=[&](const TCHAR* P){Error=P;return false;};
    auto* Owner=C.GetOwner();auto* World=C.GetWorld();auto* B=C.GetBodyInstance();auto* Scene=World?World->GetPhysicsScene():nullptr;
    if(!Loaded(&C) || !C.IsRegistered() || !Loaded(Owner) || Owner->IsActorBeingDestroyed() || !Loaded(World) || World->bIsTearingDown
       || !Scene || !Scene->GetSolver() || !B || !B->IsValidBodyInstance())return Fail(TEXT("SUPPORT_NATIVE_CONTEXT_UNAVAILABLE"));
    const auto Actor=B->GetPhysicsActorHandle();const auto Pose=C.GetComponentTransform();auto* Solver=Scene->GetSolver();
    TStrongObjectPtr<UStaticMeshComponent> CPin(&C);TStrongObjectPtr<AActor> APin(Owner);TStrongObjectPtr<UWorld> WPin(World);
    bool Entered=false,Valid=false;FString Predicate=TEXT("SUPPORT_NATIVE_LOCK_UNAVAILABLE");
    const bool Locked=FPhysicsCommand::ExecuteRead(Actor,[&](const FPhysicsActorHandle& H)
    {
        Entered=true;Predicate=TEXT("SUPPORT_NATIVE_STATIC_SCENE_BODY_REQUIRED");
        if(!FPhysicsInterface::IsValid(H) || !FPhysicsInterface::IsStatic(H) || H->GetSolverBase()!=Solver)return;
        O.bStatic=true;O.ActorIdentity=RunIdentity(H);O.ParticleId=H->GetGameThreadAPI().UniqueIdx().Idx;
        O.SceneIdentity=RunIdentity(Scene);O.SolverIdentity=RunIdentity(Solver);O.OwnerObjectId=int32(Owner->GetUniqueID());O.ComponentObjectId=int32(C.GetUniqueID());
        O.NativeActorWorldCm=FPhysicsInterface::GetGlobalPose_AssumesLocked(H);
        if(!Rigid(O.NativeActorWorldCm) || !O.NativeActorWorldCm.Equals(FTransform(Pose.GetRotation(),Pose.GetTranslation(),FVector::OneVector),1.e-6))
        {Predicate=TEXT("SUPPORT_NATIVE_ACTOR_COMPONENT_POSE_MISMATCH");return;}
        TArray<FPhysicsShapeHandle> Shapes;int32 SimBoxes=0;
        Predicate=TEXT("SUPPORT_NATIVE_SHAPE_ROSTER_BOUNDS");
        if(FPhysicsInterface::GetNumShapes(H)<1 || FPhysicsInterface::GetNumShapes(H)>8)return;
        FPhysicsInterface::GetAllShapes_AssumedLocked(H,Shapes);
        if(Shapes.Num()!=FPhysicsInterface::GetNumShapes(H))return;
        for(const auto& S:Shapes)
        {
            Predicate=TEXT("SUPPORT_NATIVE_SHAPE_OR_POLICY_UNSUPPORTED");if(!S.IsValid() || !S.Shape->GetGeometry().Get())return;
            FDDSOriginalRobotSupportNativeShapeObservation Q;Q.ShapeIndex=S.Shape->GetShapeIndex();Q.ShapeIdentity=RunIdentity(S.Shape);Q.GeometryIdentity=RunIdentity(S.Shape->GetGeometry().Get());
            Q.bSimulation=FPhysicsInterface::IsSimulationShape(S);Q.bQuery=FPhysicsInterface::IsQueryShape(S);Q.bProbe=S.Shape->GetIsProbe();Q.CollisionTraceType=int32(S.Shape->GetCollisionTraceType());
            if(Q.bProbe || (!Q.bSimulation && !Q.bQuery) || !SupportGeometryContent(S,Q))return;
            const auto Query=FPhysicsInterface::GetQueryFilter(S),Sim=FPhysicsInterface::GetSimulationFilter(S);
            Q.QueryFilter[0]=Query.Word0;Q.QueryFilter[1]=Query.Word1;Q.QueryFilter[2]=Query.Word2;Q.QueryFilter[3]=Query.Word3;
            Q.SimulationFilter[0]=Sim.Word0;Q.SimulationFilter[1]=Sim.Word1;Q.SimulationFilter[2]=Sim.Word2;Q.SimulationFilter[3]=Sim.Word3;
            if(Query.Word0!=Owner->GetUniqueID() || Sim.Word2!=C.GetUniqueID())return;
            if(Q.bSimulation)
            {
                if(Q.CollisionShapeType!=int32(ECollisionShapeType::Box) || !Q.bQuery)return;
                auto* Setup=C.GetBodySetup();if(!Loaded(Setup) || Setup->AggGeom.BoxElems.Num()!=1)return;
                const auto Scaled=Setup->AggGeom.BoxElems[0].GetFinalScaled(Pose.GetScale3D(),FTransform::Identity);
                const FVector Half=FVector(Scaled.X,Scaled.Y,Scaled.Z)*0.5;
                if(!Q.NativeBoxMinCm.Equals(Scaled.Center-Half,1.e-6) || !Q.NativeBoxMaxCm.Equals(Scaled.Center+Half,1.e-6))
                {Predicate=TEXT("SUPPORT_NATIVE_BOX_RAW_SCALED_GEOMETRY_MISMATCH");return;}
                ++SimBoxes;
            }
            if(!S.Shape->GetMaterialMasks().IsEmpty() || !S.Shape->GetMaterialMaskMaps().IsEmpty() || !S.Shape->GetMaterialMaskMapMaterials().IsEmpty())return;
            Q.NativeMaterialCount=S.Shape->NumMaterials();if(Q.NativeMaterialCount<1 || Q.NativeMaterialCount>32)return;
            FMemoryWriter MW(Q.MaterialContent,true);
            for(auto Handle:S.Shape->GetMaterials())
            {
                auto* M=Handle.Get();if(!M)return;Chaos::FChaosPhysicsMaterial Copy=*M;
                const double Values[]={Copy.Friction,Copy.StaticFriction,Copy.Restitution,Copy.LinearEtherDrag,Copy.AngularEtherDrag,Copy.SleepingLinearThreshold,Copy.SleepingAngularThreshold,Copy.DisabledLinearThreshold,Copy.DisabledAngularThreshold};
                for(double V:Values)if(!FMath::IsFinite(V) || V<0)return;
                MW<<Handle;Copy.Serialize(MW);int32 FC=int32(Copy.FrictionCombineMode),RC=int32(Copy.RestitutionCombineMode);MW<<FC<<RC;
                uint64 MaterialUser=RunIdentity(Copy.UserData);MW<<MaterialUser;
            }
            if(MW.IsError() || Q.MaterialContent.IsEmpty() || Q.MaterialContent.Num()>65536)return;
            O.Shapes.Add(MoveTemp(Q));
        }
        Predicate=TEXT("SUPPORT_NATIVE_EXACTLY_ONE_SIMULATION_BOX");if(SimBoxes!=1)return;
        Valid=true;
    });
    if(!Locked || !Entered || !Valid)return Fail(*Predicate);
    if(!Loaded(&C) || !C.IsRegistered() || C.GetOwner()!=Owner || Owner->IsActorBeingDestroyed() || C.GetWorld()!=World || World->bIsTearingDown
       || World->GetPhysicsScene()!=Scene || Scene->GetSolver()!=Solver || C.GetBodyInstance()!=B || B->GetPhysicsActorHandle()!=Actor
       || !B->IsValidBodyInstance() || !C.GetComponentTransform().Equals(Pose,1.e-6))return Fail(TEXT("SUPPORT_NATIVE_READ_CONTEXT_CHANGED"));
    O.bAvailable=true;return true;
}
bool SupportRostersMatch(const FDDSOriginalRobotSupportNativeBodyObservation& A,const FDDSOriginalRobotSupportNativeBodyObservation& B,bool SameIdentity)
{
    if(!A.bAvailable || !B.bAvailable || !A.bStatic || !B.bStatic || A.Shapes.Num()!=B.Shapes.Num()
       || !A.NativeActorWorldCm.Equals(B.NativeActorWorldCm,1.e-6))return false;
    if(SameIdentity && (A.SceneIdentity!=B.SceneIdentity || A.SolverIdentity!=B.SolverIdentity || A.ActorIdentity!=B.ActorIdentity || A.ParticleId!=B.ParticleId
       || A.OwnerObjectId!=B.OwnerObjectId || A.ComponentObjectId!=B.ComponentObjectId))return false;
    for(int32 I=0;I<A.Shapes.Num();++I)
    {
        const auto& X=A.Shapes[I];const auto& Y=B.Shapes[I];
        if(X.ShapeIndex!=Y.ShapeIndex || X.GeometryType!=Y.GeometryType || X.CollisionShapeType!=Y.CollisionShapeType || X.bSimulation!=Y.bSimulation || X.bQuery!=Y.bQuery || X.bProbe!=Y.bProbe
           || X.CollisionTraceType!=Y.CollisionTraceType || X.NativeMaterialCount!=Y.NativeMaterialCount || X.GeometryContent!=Y.GeometryContent || X.MaterialContent!=Y.MaterialContent
           || X.AssetQueryGeometryIdentity!=Y.AssetQueryGeometryIdentity || !X.NativeBoundsMinCm.Equals(Y.NativeBoundsMinCm,1.e-6) || !X.NativeBoundsMaxCm.Equals(Y.NativeBoundsMaxCm,1.e-6))return false;
        if(SameIdentity && (X.ShapeIdentity!=Y.ShapeIdentity || X.GeometryIdentity!=Y.GeometryIdentity))return false;
        for(int32 W=0;W<4;++W)
        {
            // Query Word0 is owner identity, Sim Word2 is component identity.
            // Those are checked locally when observed; all semantic bits match.
            if((SameIdentity || W!=0) && X.QueryFilter[W]!=Y.QueryFilter[W])return false;
            if((SameIdentity || W!=2) && X.SimulationFilter[W]!=Y.SimulationFilter[W])return false;
        }
    }
    return true;
}
bool CalibrationShapeLive(const Chaos::FGeometryParticleHandle* Particle,uint64 Shape,uint64 Geometry)
{for(const auto& Instance:Particle->ShapeInstances())if(RunIdentity(Instance.Get())==Shape)return Instance->GetSimEnabled() && RunIdentity(Instance->GetGeometry().Get())==Geometry;return false;}
FVector PointVelocity(const Chaos::FGeometryParticleHandle* Particle,const FVector& Point)
{
    if(const auto* Kinematic=Particle->CastToKinematicParticle())
    {const auto* RigidParticle=Particle->CastToRigidParticle();const FVector Center=RigidParticle?ParticlePose(Particle).TransformPosition(FVector(RigidParticle->CenterOfMass())):FVector(Particle->X());
     return FVector(Kinematic->V())+FVector::CrossProduct(FVector(Kinematic->W()),Point-Center);}
    return FVector::ZeroVector;
}
bool FiniteWheelSample(const FDDSOriginalRobotCalibrationWheelSample& Sample)
{
    const double Scalars[]={Sample.RawDetectionPhiCm,Sample.CurrentSeparationCm,Sample.RawNormalLoadImpulseKgCmps,Sample.RelativeAxleOmegaRadps,
        Sample.AngularContactRollingForwardCmps,Sample.WheelCOMForwardRelativeToSupportCmps,Sample.GroundFrameForwardSpeedCmps,Sample.LongitudinalSlip,
        Sample.SleepingAnchorDistanceCm,Sample.LastSolvedCallbackTimeS,Sample.WheelSpeedCmps,Sample.SupportSpeedCmps,Sample.WheelOmegaRadps,Sample.SupportOmegaRadps,
        Sample.AxleOmegaDeltaRadps,Sample.COMForwardVelocityDeltaCmps};
    for(double Scalar:Scalars)if(!FMath::IsFinite(Scalar))return false;
    const FVector Vectors[]={Sample.ContactPointCm,Sample.ContactNormal,Sample.WheelLocalAnchorCm,Sample.SupportLocalAnchorCm,Sample.NormalInSupport,Sample.SupportPointCm,
        Sample.RawSolveWheelPointCm,Sample.RawSolveOtherPointCm,Sample.RawSolveNormal,Sample.SolvedImpulseKgCmps,Sample.SolvedPushOutKgCm,Sample.SupportContactVelocityCmps,Sample.WheelContactVelocityCmps};
    for(const FVector& Vector:Vectors)if(Vector.ContainsNaN())return false;
    return !(Sample.bSolvedContactObserved || Sample.bSleepingAnchorTransportObserved) || (Sample.WheelParticleId!=INDEX_NONE && Sample.SupportParticleId!=INDEX_NONE
        && Sample.WheelShapeId!=0 && Sample.SupportShapeId!=0 && Sample.WheelGeometryId!=0 && Sample.SupportGeometryId!=0 && Sample.ContactNormal.IsNormalized());
}
double Twist(const FQuat& A,const FQuat& B)
{ FQuat Q=A.Inverse()*B;Q.Normalize();return FMath::UnwindRadians(2*FMath::Atan2(Q.X,Q.W)); }

struct FCalibrationInput : Chaos::FSimCallbackInput
{
    bool bSetup=false;int32 Id[9]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    FTransform JointChassisCm[8],JointPartCm[8];FVector Forward=FVector::ZeroVector;
    FDDSRobotPhysicsProfile Profile;double TravelCm[8]={},TargetCm[8]={};bool ExcludeCollision[8]={};
    int32 PulseWheel=INDEX_NONE;double PulseNm=0;
    int32 TrialWheel=INDEX_NONE;EDDSOriginalCalibrationTrialPhase TrialPhase=EDDSOriginalCalibrationTrialPhase::Settle;
    bool ExplicitGameLimits=false;
    double SlipDomainCmps=0,SleepingAnchorCm=0,SleepingSpeedCmps=0,SleepingOmegaRadps=0,MinUpDot=0,MinLoadedImpulseKgCmps=0;
    void Reset(){*this=FCalibrationInput();}
};
struct FCalibrationOutput : Chaos::FSimCallbackOutput
{ FDDSOriginalRobotCalibrationStepSample Sample;void Reset(){Sample={};} };
class FCalibrationCallback final : public Chaos::TSimCallbackObject<FCalibrationInput,FCalibrationOutput,Chaos::ESimCallbackOptions::Presimulate>
{
    FCalibrationInput Setup;Chaos::FWeakParticleHandle Bodies[9];uint64 Seq=0;double PriorTime=0,PriorDt=0;
    int32 LastPulse=INDEX_NONE;bool Bound=false;
    double LastPulseNm=0;int32 LastTrialWheel=INDEX_NONE;EDDSOriginalCalibrationTrialPhase LastTrialPhase=EDDSOriginalCalibrationTrialPhase::Settle;
    struct FContactCache{Chaos::FWeakParticleHandle Other;FDDSOriginalRobotCalibrationWheelSample Solved;bool Available=false;} ContactCache[6];
public:
    virtual FName GetFNameForStatId()const override {static const FName N(TEXT("DDSOriginalCalibrationDraft"));return N;}
    virtual void OnPreSimulate_Internal() override
    {
        auto* CallbackSolver=static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());auto& E=*CallbackSolver->GetEvolution();
        const auto* Input=GetConsumerInput_Internal();
        if(!Bound && Input && Input->bSetup)
        {
            Setup=*Input;auto& Handles=CallbackSolver->GetParticles().GetParticleHandles();int32 Found=0;
            for(int32 I=0;I<int32(Handles.Size());++I) {auto* H=Handles.Handle(I).Get();if(!H)continue;
                for(int32 B=0;B<9;++B)if(H->UniqueIdx().Idx==Setup.Id[B] && !Bodies[B].GetHandle()){Bodies[B]=H->WeakParticleHandle();++Found;} }
            Bound=Found==9;
        }
        if(!Bound)return;
        auto& O=GetProducerOutputData_Internal().Sample;O={};O.Sequence=++Seq;
        O.CallbackStartS=GetSimTime_Internal();O.CallbackDtS=GetDeltaTime_Internal();O.PreviousCallbackDtS=PriorDt;
        O.ConfiguredSolverAsyncDtS=CallbackSolver->GetAsyncDeltaTime();O.PreviousPulseWheel=LastPulse;
        O.AppliedPulseWheel=Input?Input->PulseWheel:INDEX_NONE;
        O.AppliedPulseNm=Input?Input->PulseNm:0;O.PreviousPulseNm=LastPulseNm;
        O.AppliedTrialWheel=Input?Input->TrialWheel:INDEX_NONE;O.PreviousTrialWheel=LastTrialWheel;
        O.AppliedTrialPhase=Input?Input->TrialPhase:EDDSOriginalCalibrationTrialPhase::Settle;O.PreviousTrialPhase=LastTrialPhase;
        O.bPreviousCompletedState=PriorDt>0 && FMath::IsNearlyEqual(O.CallbackStartS,PriorTime+PriorDt,1.e-7);
        auto& All=CallbackSolver->GetParticles().GetParticleHandles();
        for(int32 N=0;N<int32(All.Size());++N) {auto* H=All.Handle(N).Get();auto* Rigid=H?H->CastToRigidParticle():nullptr;
            if(Rigid && (Rigid->ObjectState()==Chaos::EObjectStateType::Dynamic || Rigid->ObjectState()==Chaos::EObjectStateType::Sleeping))++O.NativeDynamicBodyCount;}
        Chaos::FPBDRigidParticleHandle* R[9]={};
        for(int32 I=0;I<9;++I)
        {
            auto* H=Bodies[I].GetHandle();if(!H || H->UniqueIdx().Idx!=Setup.Id[I])return;
            R[I]=H->CastToRigidParticle();if(!R[I] || (R[I]->ObjectState()!=Chaos::EObjectStateType::Dynamic && R[I]->ObjectState()!=Chaos::EObjectStateType::Sleeping))return;auto& B=O.Bodies[I];
            B.bLive=true;B.ParticleId=H->UniqueIdx().Idx;B.bSleeping=R[I]->ObjectState()==Chaos::EObjectStateType::Sleeping;
            B.WorldCm=ParticlePose(H);B.VelocityCmps=FVector(R[I]->V());B.AngularVelocityRadps=FVector(R[I]->W());
            B.MassKg=R[I]->M();B.COMLocalCm=FVector(R[I]->CenterOfMass());B.InertiaKgCm2=FVector(R[I]->I());B.PrincipalAxesInBody=FQuat(R[I]->RotationOfMass());
            O.NativeBodyCCDEnabled[I]=R[I]->CCDEnabled();
            const FVector G=FVector(E.GetGravityForces().GetAcceleration(R[I]->GravityGroupIndex()));if(I==0)O.ActualGravityCmps2=G;
            if(!R[I]->GravityEnabled() || !G.Equals(FVector(0,0,GravityCmps2),1.e-4))O.bPreviousCompletedState=false;
        }
        const auto& JointHandles=E.GetJointConstraints().GetConstraintHandles();
        for(int32 J=0;J<8;++J)
        {
            int32 Matches=0;
            for(const auto* H:JointHandles)
            {if(!H || !H->IsValid() || !H->IsConstraintEnabled() || H->IsConstraintBroken())continue;
                const auto Pair=H->GetConstrainedParticles();
                if(Pair[0]==Bodies[0].GetHandle() && Pair[1]==Bodies[J+1].GetHandle()
                   && DDSJointSettingsMatch(H->GetSettings(),J,Setup.JointChassisCm[J],Setup.JointPartCm[J],Setup.TravelCm[J],Setup.TargetCm[J],Setup.ExcludeCollision[J],Setup.Profile))
                {++Matches;auto& Observed=O.NativeJoints[J];RetainJointSettings(H->GetSettings(),Observed);
                 Observed.NativeConstraintIdentity=uint64(reinterpret_cast<UPTRINT>(H));Observed.bNativeParticleIdsAvailable=true;
                 Observed.ChassisParticleId=Pair[0]->UniqueIdx().Idx;Observed.PartParticleId=Pair[1]->UniqueIdx().Idx;}}
            O.NativeJoints[J].bAvailable=Matches==1;
            if(Matches==1)++O.NativeMatchingJointCount;
        }
        O.bPreviousCompletedState&=O.NativeDynamicBodyCount==9 && O.NativeMatchingJointCount==8;
        for(int32 I=0;I<6;++I)
        {
            const FTransform A=Setup.JointChassisCm[I]*O.Bodies[0].WorldCm;
            O.Wheels[I].RelativeAxleOmegaRadps=FVector::DotProduct(O.Bodies[I+1].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps,A.GetUnitAxis(EAxis::X));
        }
        for(int32 L=0;L<2;++L)
        {
            const FTransform A=Setup.JointChassisCm[L+6]*O.Bodies[0].WorldCm,B=Setup.JointPartCm[L+6]*O.Bodies[L+7].WorldCm;
            O.ClosedLidAngleRad[L]=Twist(A.GetRotation(),B.GetRotation());
            O.ClosedLidOmegaRadps[L]=FVector::DotProduct(O.Bodies[L+7].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps,A.GetUnitAxis(EAxis::X));
            O.ClosedLidFullAngularErrorRad[L]=A.GetRotation().AngularDistance(B.GetRotation());
            O.ClosedLidRelativeOmegaMagnitudeRadps[L]=(O.Bodies[L+7].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps).Size();
        }
        if(O.bPreviousCompletedState)
        for(const auto* C:E.GetCollisionConstraints().GetConstraints())
        {
            if(!C || !C->IsEnabled() || C->GetIsProbe())continue;
            for(int32 W=0;W<6;++W)
            {
                auto* Wheel=Bodies[W+1].GetHandle();const int32 Side=C->GetParticle0()==Wheel?0:(C->GetParticle1()==Wheel?1:INDEX_NONE);if(Side==INDEX_NONE)continue;
                auto* Other=C->GetParticle(1-Side);bool Self=false;for(auto& H:Bodies)if(H.GetHandle()==Other)Self=true;if(!Other || Self)continue;
                const auto* S0=C->GetShape(0);const auto* S1=C->GetShape(1);if(!S0 || !S1 || !S0->GetSimEnabled() || !S1->GetSimEnabled())continue;
                const auto Current=[&](int32 S){const auto& T=S?C->GetShapeRelativeTransform1():C->GetShapeRelativeTransform0();
                    return FTransform(FQuat(T.GetRotation()),FVector(T.GetTranslation()))*ParticlePose(C->GetParticle(S));};
                const FTransform T0=Current(0),T1=Current(1);
                for(int32 P=0;P<C->NumManifoldPoints();++P)
                {
                    if(!C->IsManifoldPointActive(P))continue;const auto& M=C->GetManifoldPoint(P).ContactPoint;const auto& Result=C->GetManifoldPointResult(P);
                    if(!M.IsSet() || !Result.bIsValid)continue;
                    const double Sign=Side==0?1.0:-1.0;const FVector RawNormal=FVector(C->GetShapeWorldTransform1().TransformVector(Chaos::FVec3(M.ShapeContactNormal)))*Sign;
                    const FVector Normal=T1.TransformVectorNoScale(FVector(M.ShapeContactNormal)).GetSafeNormal()*Sign;
                    const FVector Point[2]={T0.TransformPosition(FVector(M.ShapeContactPoints[0])),T1.TransformPosition(FVector(M.ShapeContactPoints[1]))};
                    const double Load=FVector::DotProduct((FVector(Result.NetImpulse)+FVector(Result.NetPushOut)/PriorDt)*Sign,RawNormal);
                    auto& S=O.Wheels[W];if(!FMath::IsFinite(Load) || Load<=0 || Normal.Z<=0 || Point[Side].ContainsNaN() || Normal.ContainsNaN()
                       || (S.bSolvedContactObserved && Load<=S.RawNormalLoadImpulseKgCmps))continue;
                    S.bSolvedContactObserved=true;S.SupportParticleId=Other->UniqueIdx().Idx;S.ContactPointCm=Point[Side];S.ContactNormal=Normal;
                    S.RawDetectionPhiCm=M.Phi;S.CurrentSeparationCm=FVector::DotProduct(Point[Side]-Point[1-Side],Normal);S.RawNormalLoadImpulseKgCmps=Load;
                    const FVector Center=O.Bodies[W+1].WorldCm.TransformPosition(O.Bodies[W+1].COMLocalCm);
                    const FVector Forward=O.Bodies[0].WorldCm.TransformVectorNoScale(Setup.Forward);
                    S.AngularContactRollingForwardCmps=-FVector::DotProduct(FVector::CrossProduct(O.Bodies[W+1].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps,Point[Side]-Center),Forward);
                    FVector SupportV=FVector::ZeroVector;
                    if(const auto* K=Other->CastToKinematicParticle())
                    {const auto* OtherRigid=Other->CastToRigidParticle();const FVector OtherCenter=OtherRigid?ParticlePose(Other).TransformPosition(FVector(OtherRigid->CenterOfMass())):FVector(Other->X());
                     SupportV=FVector(K->V())+FVector::CrossProduct(FVector(K->W()),Point[1-Side]-OtherCenter);}
                    S.WheelCOMForwardRelativeToSupportCmps=FVector::DotProduct(O.Bodies[W+1].VelocityCmps-SupportV,Forward);
                    S.WheelParticleId=Wheel->UniqueIdx().Idx;S.WheelShapeId=RunIdentity(C->GetShape(Side));S.SupportShapeId=RunIdentity(C->GetShape(1-Side));
                    S.WheelGeometryId=RunIdentity(C->GetShape(Side)->GetGeometry().Get());S.SupportGeometryId=RunIdentity(C->GetShape(1-Side)->GetGeometry().Get());
                    S.SupportPointCm=Point[1-Side];S.WheelLocalAnchorCm=ParticlePose(Wheel).InverseTransformPosition(Point[Side]);
                    S.SupportLocalAnchorCm=ParticlePose(Other).InverseTransformPosition(Point[1-Side]);S.NormalInSupport=ParticlePose(Other).InverseTransformVectorNoScale(Normal);
                    S.RawSolveWheelPointCm=FVector((Side==0?C->GetShapeWorldTransform0():C->GetShapeWorldTransform1()).TransformPosition(Chaos::FVec3(M.ShapeContactPoints[Side])));
                    S.RawSolveOtherPointCm=FVector((Side==0?C->GetShapeWorldTransform1():C->GetShapeWorldTransform0()).TransformPosition(Chaos::FVec3(M.ShapeContactPoints[1-Side])));
                    S.RawSolveNormal=RawNormal;S.SolvedImpulseKgCmps=FVector(Result.NetImpulse)*Sign;S.SolvedPushOutKgCm=FVector(Result.NetPushOut)*Sign;
                    S.SupportContactVelocityCmps=SupportV;S.WheelContactVelocityCmps=PointVelocity(Wheel,Point[Side]);
                    S.GroundFrameForwardSpeedCmps=FMath::Abs(FVector::DotProduct(PointVelocity(Bodies[0].GetHandle(),Point[Side])-SupportV,Forward));
                    S.bSlipDefined=Setup.ExplicitGameLimits && S.GroundFrameForwardSpeedCmps>=Setup.SlipDomainCmps;
                    if(S.bSlipDefined)S.LongitudinalSlip=FVector::DotProduct(S.WheelContactVelocityCmps-SupportV,Forward)/S.GroundFrameForwardSpeedCmps;
                    S.bWheelSleeping=O.Bodies[W+1].bSleeping;S.bSupportStatic=Other->ObjectState()==Chaos::EObjectStateType::Static;
                    if(const auto* SupportRigid=Other->CastToRigidParticle())
                    {S.bSupportSleeping=SupportRigid->ObjectState()==Chaos::EObjectStateType::Sleeping;S.SupportSpeedCmps=FVector(SupportRigid->V()).Size();S.SupportOmegaRadps=FVector(SupportRigid->W()).Size();}
                    S.WheelSpeedCmps=O.Bodies[W+1].VelocityCmps.Size();S.WheelOmegaRadps=O.Bodies[W+1].AngularVelocityRadps.Size();S.LastSolvedCallbackTimeS=O.CallbackStartS;
                    ContactCache[W].Available=true;ContactCache[W].Other=Other->WeakParticleHandle();ContactCache[W].Solved=S;
                }
            }
        }
        // A prior solve can supply a transported sleeping anchor only while its
        // particles and exact simulation shape/geometry identities stay live.
        // Never fabricate a fresh impulse or mark this as a new solved contact.
        if(O.bPreviousCompletedState)for(int32 WheelIndex=0;WheelIndex<6;++WheelIndex)
        {
            auto& Sample=O.Wheels[WheelIndex];const auto& Cached=ContactCache[WheelIndex];auto* Other=Cached.Other.GetHandle();auto* Wheel=Bodies[WheelIndex+1].GetHandle();
            if(Sample.bSolvedContactObserved || !Cached.Available || !Wheel || !Other || !O.Bodies[WheelIndex+1].bSleeping
               || Wheel->UniqueIdx().Idx!=Cached.Solved.WheelParticleId || Other->UniqueIdx().Idx!=Cached.Solved.SupportParticleId
               || !CalibrationShapeLive(Wheel,Cached.Solved.WheelShapeId,Cached.Solved.WheelGeometryId)
               || !CalibrationShapeLive(Other,Cached.Solved.SupportShapeId,Cached.Solved.SupportGeometryId))continue;
            const auto* OtherRigid=Other->CastToRigidParticle();const bool Static=Other->ObjectState()==Chaos::EObjectStateType::Static;
            const bool Sleeping=OtherRigid && OtherRigid->ObjectState()==Chaos::EObjectStateType::Sleeping;if(!Static && !Sleeping)continue;
            const FVector WheelPoint=ParticlePose(Wheel).TransformPosition(Cached.Solved.WheelLocalAnchorCm),OtherPoint=ParticlePose(Other).TransformPosition(Cached.Solved.SupportLocalAnchorCm);
            const FVector Normal=ParticlePose(Other).TransformVectorNoScale(Cached.Solved.NormalInSupport).GetSafeNormal();
            if(WheelPoint.ContainsNaN() || OtherPoint.ContainsNaN() || !Normal.IsNormalized())continue;
            Sample=Cached.Solved;Sample.bSolvedContactObserved=false;Sample.bSleepingAnchorTransportObserved=true;
            const FTransform Joint=Setup.JointChassisCm[WheelIndex]*O.Bodies[0].WorldCm;
            Sample.RelativeAxleOmegaRadps=FVector::DotProduct(O.Bodies[WheelIndex+1].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps,Joint.GetUnitAxis(EAxis::X));
            Sample.ContactPointCm=WheelPoint;Sample.SupportPointCm=OtherPoint;Sample.ContactNormal=Normal;
            Sample.CurrentSeparationCm=FVector::DotProduct(WheelPoint-OtherPoint,Normal);Sample.SleepingAnchorDistanceCm=(WheelPoint-OtherPoint).Size();
            Sample.bWheelSleeping=true;Sample.bSupportSleeping=Sleeping;Sample.bSupportStatic=Static;
            Sample.WheelSpeedCmps=O.Bodies[WheelIndex+1].VelocityCmps.Size();Sample.WheelOmegaRadps=O.Bodies[WheelIndex+1].AngularVelocityRadps.Size();
            Sample.SupportSpeedCmps=OtherRigid?FVector(OtherRigid->V()).Size():0;Sample.SupportOmegaRadps=OtherRigid?FVector(OtherRigid->W()).Size():0;
            Sample.SupportContactVelocityCmps=PointVelocity(Other,OtherPoint);Sample.WheelContactVelocityCmps=PointVelocity(Wheel,WheelPoint);
            Sample.bSleepingWitnessWithinDeclaredGameLimits=Setup.ExplicitGameLimits && Sample.SleepingAnchorDistanceCm<=Setup.SleepingAnchorCm
                && Sample.WheelSpeedCmps<=Setup.SleepingSpeedCmps && Sample.SupportSpeedCmps<=Setup.SleepingSpeedCmps
                && Sample.WheelOmegaRadps<=Setup.SleepingOmegaRadps && Sample.SupportOmegaRadps<=Setup.SleepingOmegaRadps
                && Normal.Z>=Setup.MinUpDot && Sample.RawNormalLoadImpulseKgCmps>=Setup.MinLoadedImpulseKgCmps;
            // The current speeds/slip are recomputed. RawSolve/impulse/last-solved
            // time remain explicitly the older solve, and are never refreshed.
            const FVector Forward=O.Bodies[0].WorldCm.TransformVectorNoScale(Setup.Forward);
            const FVector WheelCenter=O.Bodies[WheelIndex+1].WorldCm.TransformPosition(O.Bodies[WheelIndex+1].COMLocalCm);
            Sample.AngularContactRollingForwardCmps=-FVector::DotProduct(FVector::CrossProduct(O.Bodies[WheelIndex+1].AngularVelocityRadps-O.Bodies[0].AngularVelocityRadps,WheelPoint-WheelCenter),Forward);
            Sample.WheelCOMForwardRelativeToSupportCmps=FVector::DotProduct(O.Bodies[WheelIndex+1].VelocityCmps-Sample.SupportContactVelocityCmps,Forward);
            Sample.GroundFrameForwardSpeedCmps=FMath::Abs(FVector::DotProduct(PointVelocity(Bodies[0].GetHandle(),WheelPoint)-Sample.SupportContactVelocityCmps,Forward));
            Sample.bSlipDefined=Setup.ExplicitGameLimits && Sample.GroundFrameForwardSpeedCmps>=Setup.SlipDomainCmps;
            Sample.LongitudinalSlip=Sample.bSlipDefined?FVector::DotProduct(Sample.WheelContactVelocityCmps-Sample.SupportContactVelocityCmps,Forward)/Sample.GroundFrameForwardSpeedCmps:0;
        }
        // Equal/opposite actual torque in the native solver, not a velocity assignment.
        if(Input && Input->PulseWheel>=0 && Input->PulseWheel<6 && FMath::IsFinite(Input->PulseNm) && Input->PulseNm!=0)
        {
            const int32 W=Input->PulseWheel;const FTransform Joint=Setup.JointChassisCm[W]*O.Bodies[0].WorldCm;
            const FVector Torque=Joint.GetUnitAxis(EAxis::X)*Input->PulseNm*DDSRobotUnits::UETorquePerNm;
            for(int32 B:{0,W+1})if(R[B]->ObjectState()==Chaos::EObjectStateType::Sleeping)E.SetParticleObjectState(R[B],Chaos::EObjectStateType::Dynamic);
            R[W+1]->AddTorque(Chaos::FVec3(Torque),false);R[0]->AddTorque(Chaos::FVec3(-Torque),false);
        }
        PriorTime=O.CallbackStartS;PriorDt=O.CallbackDtS;LastPulse=O.AppliedPulseWheel;LastPulseNm=O.AppliedPulseNm;
        LastTrialWheel=O.AppliedTrialWheel;LastTrialPhase=O.AppliedTrialPhase;
    }
};
}

struct FDDSOriginalCalibrationCompoundReceipt
{
    struct FSource
    {
        UBodySetup* Setup=nullptr;FGuid Guid;UPhysicalMaterial* Material=nullptr;
        AActor* Actor=nullptr;UWorld* World=nullptr;
        TArray<uint8> Collision,Settings;
    };
    FDDSOriginalRobotNativeGeometry OriginalCityGeometry;
    TArray<FSource> Sources;
    TArray<FDDSOriginalCompoundHullOrigin> HullOrigins;
    AActor* TargetOwner=nullptr;UWorld* TargetWorld=nullptr;UStaticMesh* TargetMesh=nullptr;
    FPhysScene_Chaos* TargetScene=nullptr;
    UBodySetup* OwnedSetup=nullptr;FGuid OwnedGuid;UPhysicalMaterial* OwnedMaterial=nullptr;
    FString NativeSourceRecordSha256;
    TArray<uint8> OwnedCollision,OwnedSettings;
};

namespace DDSOriginalMeasurement
{
bool DiagnosticSourcesUnchanged(const FDDSOriginalCalibrationCompoundReceipt& R)
{
    const auto& G=R.OriginalCityGeometry;
    if(G.Parts.Num()!=16 || R.Sources.Num()!=16 || !Sha(G.NativeGeometryRecordSha256)
       || !Sha(G.WorldPlacementRecordSha256) || !G.bActualOriginalAssetsAndTemplatesObserved)return false;
    const auto* Base=Part(G,EDDSOriginalRobotPartRole::Body,0);
    if(!Base || !Loaded(Base->OriginalVisualSource))return false;
    AActor* SourceActor=Base->OriginalVisualSource->GetOwner();UWorld* SourceWorld=Base->OriginalVisualSource->GetWorld();
    for(int32 I=0;I<16;++I)
    {
        const auto& P=G.Parts[I];const auto& S=R.Sources[I];auto* C=P.OriginalVisualSource;
        if(!SourceStillExact(P) || !P.OriginalMaterialReceipt.IsValid() || !P.OriginalMaterialReceipt->Unchanged(true)
           || P.OriginalMaterialReceipt->Component!=C || P.OriginalMaterialReceipt->Mesh!=P.SuppliedMesh
           || P.OriginalMaterialReceipt->Owner!=C->GetOwner() || P.OriginalMaterialReceipt->World!=C->GetWorld())return false;
        if(C->GetOwner()!=S.Actor || C->GetWorld()!=S.World || C->GetWorld()!=SourceWorld
           || (P.Role!=EDDSOriginalRobotPartRole::Parcel && C->GetOwner()!=SourceActor)
           || !Loaded(S.Setup) || P.SuppliedMesh->GetBodySetup()!=S.Setup
           || S.Setup->BodySetupGuid!=S.Guid || S.Setup->PhysMaterial.Get()!=S.Material || (S.Material && !Loaded(S.Material)))return false;
        TArray<uint8> Collision,Settings;
        if(!SetupReceiptBytes(S.Setup,Collision,Settings) || Collision!=S.Collision || Settings!=S.Settings)return false;
        if(P.Role==EDDSOriginalRobotPartRole::Suspension)
        {const auto* SuspensionBase=Part(G,EDDSOriginalRobotPartRole::Body,0);if(!SuspensionBase || C->GetAttachParent()!=SuspensionBase->OriginalVisualSource)return false;}
    }
    return true; // Original City sources remain in their own coherent source world.
}
bool DiagnosticOriginsExact(const FDDSOriginalCalibrationCompoundReceipt& R)
{
    const auto& G=R.OriginalCityGeometry;const auto* Base=Part(G,EDDSOriginalRobotPartRole::Body,0);
    if(!Base || R.NativeSourceRecordSha256!=G.NativeGeometryRecordSha256 || R.HullOrigins.IsEmpty())return false;
    int32 Output=0;
    for(int32 Slot=0;Slot<7;++Slot)
    {
        const auto* P=Part(G,Slot==0?EDDSOriginalRobotPartRole::Body:EDDSOriginalRobotPartRole::Suspension,Slot==0?0:Slot-1);
        if(!P || !Loaded(P->SuppliedMesh) || !Loaded(P->SuppliedMesh->GetBodySetup()))return false;
        auto* Setup=P->SuppliedMesh->GetBodySetup();const auto Rest=P->MeasuredWorldCm.GetRelativeTransform(Base->MeasuredWorldCm);
        for(int32 H=0;H<Setup->AggGeom.ConvexElems.Num();++H)
        {
            if(!R.HullOrigins.IsValidIndex(Output))return false;const auto& O=R.HullOrigins[Output++];
            if(O.OriginalPartId!=P->SuppliedPartId || O.OriginalMeshObjectPath!=P->ExactMeshObjectPath || O.OriginalMeshPackageSha256!=P->MeshPackageSha256
               || O.OriginalBodySetupGuid!=Setup->BodySetupGuid || O.SourceHullIndex!=H
               || !O.SourceHullToChassisCm.Equals(Setup->AggGeom.ConvexElems[H].GetTransform()*Rest,0))return false;
        }
    }
    return Output==R.HullOrigins.Num();
}
}

UDDSOriginalCalibrationMeshComponent::UDDSOriginalCalibrationMeshComponent(const FObjectInitializer& Initializer)
    : Super(Initializer) {}
UDDSOriginalCalibrationMeshComponent::UDDSOriginalCalibrationMeshComponent(FVTableHelper& Helper)
    : Super(Helper) {}
UDDSOriginalCalibrationMeshComponent::~UDDSOriginalCalibrationMeshComponent() = default;
bool UDDSOriginalCalibrationMeshComponent::SetStaticMesh(UStaticMesh* Mesh)
{
    if((DiagnosticSetup || bPreparingDiagnosticCompound) && Mesh!=GetStaticMesh().Get())return false;
    return Super::SetStaticMesh(Mesh);
}
UBodySetup* UDDSOriginalCalibrationMeshComponent::GetBodySetup()
{ return DiagnosticSetup ? DiagnosticSetup.Get() : Super::GetBodySetup(); }
void UDDSOriginalCalibrationMeshComponent::BeginDestroy()
{
    Super::BeginDestroy(); // Retain the owned setup/source pins through base physics teardown.
    DiagnosticReceipt.Reset();DiagnosticLifetime.Reset();DiagnosticSetup=nullptr;
}
bool UDDSOriginalCalibrationMeshComponent::PrepareDiagnosticCompound(const FDDSOriginalRobotNativeGeometry& Supplied,FString& Error)
{
    using namespace DDSOriginalMeasurement;Error.Reset();
    if(!IsInGameThread() || bPreparingDiagnosticCompound || DiagnosticSetup || DiagnosticReceipt)
    {Error=TEXT("Private diagnostic compound wrong thread/reentrant/already prepared");return false;}
    TStrongObjectPtr<UDDSOriginalCalibrationMeshComponent> SelfPin(this);TGuardValue<bool> Guard(bPreparingDiagnosticCompound,true);
    auto Candidate=MakeUnique<FDDSOriginalCalibrationCompoundReceipt>();Candidate->OriginalCityGeometry=Supplied;
    auto& R=*Candidate;R.TargetOwner=GetOwner();R.TargetWorld=GetWorld();R.TargetMesh=GetStaticMesh().Get();
    R.TargetScene=Loaded(R.TargetWorld)?R.TargetWorld->GetPhysicsScene():nullptr;
    const auto TargetUnregistered=[&]() {return Loaded(this) && Loaded(R.TargetOwner) && !R.TargetOwner->IsActorBeingDestroyed()
        && Loaded(R.TargetWorld) && !R.TargetWorld->bIsTearingDown && GetOwner()==R.TargetOwner && GetOuter()==R.TargetOwner
        && GetWorld()==R.TargetWorld && R.TargetWorld->GetPhysicsScene()==R.TargetScene && R.TargetScene
        && GetStaticMesh().Get()==R.TargetMesh && Loaded(R.TargetMesh) && !R.TargetMesh->IsCompiling()
        && !IsRegistered() && !IsPhysicsStateCreated() && !BodyInstance.IsValidBodyInstance() && !IsSimulatingPhysics()
        && GetCollisionEnabled()==ECollisionEnabled::NoCollision && !DiagnosticSetup;};
    if(!TargetUnregistered() || R.OriginalCityGeometry.Parts.Num()!=16)
    {Error=TEXT("Only private unregistered diagnostic component with original16 source observation may prepare copied compound");return false;}
    const auto* Base=Part(R.OriginalCityGeometry,EDDSOriginalRobotPartRole::Body,0);
    if(!Base || R.TargetMesh!=Base->SuppliedMesh)
    {Error=TEXT("Private diagnostic target must retain exact original Base mesh");return false;}
    // Initialize reflection before snapshots or callback-capable allocation/cook.
    FKAggregateGeom::StaticStruct();FBodyInstance::StaticStruct();
    TArray<TStrongObjectPtr<UObject>> Pins;Pins.Emplace(R.TargetOwner);Pins.Emplace(R.TargetWorld);Pins.Emplace(R.TargetMesh);
    for(const auto& P:R.OriginalCityGeometry.Parts)
    {
        if(!SourceStillExact(P) || !P.OriginalMaterialReceipt.IsValid() || !P.OriginalMaterialReceipt->Unchanged(true))
        {Error=TEXT("Original City source/material receipt unavailable for private copied compound");return false;}
        auto* Setup=P.SuppliedMesh->GetBodySetup();if(!Loaded(Setup))
        {Error=TEXT("Original City source collision setup unavailable");return false;}
        auto& S=R.Sources.AddDefaulted_GetRef();S.Setup=Setup;S.Guid=Setup->BodySetupGuid;S.Material=Setup->PhysMaterial.Get();
        S.Actor=P.OriginalVisualSource->GetOwner();S.World=P.OriginalVisualSource->GetWorld();
        if(!SetupReceiptBytes(Setup,S.Collision,S.Settings))
        {Error=TEXT("Original City collision/settings snapshot outside diagnostic budget");return false;}
        Pins.Emplace(P.OriginalVisualSource);Pins.Emplace(P.OriginalVisualSource->GetOwner());Pins.Emplace(P.OriginalVisualSource->GetWorld());
        Pins.Emplace(P.SuppliedMesh);Pins.Emplace(Setup);if(S.Material)Pins.Emplace(S.Material);
    }
    if(!TargetUnregistered() || !DiagnosticSourcesUnchanged(R))
    {Error=TEXT("Private target or coherent original City source changed before compound allocation");return false;}
    FDDSOriginalChassisCompoundResult Built;
    // The pure builder copies exact original hulls. OriginalCityGeometry is NEVER
    // rewritten as a private-world receipt, and production same-world guards stay intact.
    if(!DDSBuildOriginalChassisCompoundCollision(*this,R.OriginalCityGeometry,Built,Error))return false;
    if(!TargetUnregistered() || !DiagnosticSourcesUnchanged(R) || !Loaded(Built.BodySetup.Get()) || Built.BodySetup->GetOuter()!=this)
    {Error=TEXT("Diagnostic compound allocation/cook revoked target or original City source");return false;}
    R.HullOrigins=MoveTemp(Built.HullOrigins);R.NativeSourceRecordSha256=Built.NativeSourceRecordSha256;
    R.OwnedSetup=Built.BodySetup.Get();R.OwnedGuid=R.OwnedSetup->BodySetupGuid;R.OwnedMaterial=R.OwnedSetup->PhysMaterial.Get();
    if(!DiagnosticOriginsExact(R) || R.OwnedSetup->AggGeom.ConvexElems.Num()!=R.HullOrigins.Num()
       || !SetupReceiptBytes(R.OwnedSetup,R.OwnedCollision,R.OwnedSettings))
    {Error=TEXT("Private compound exact hull provenance/setup receipt unavailable");return false;}
    for(const auto& P:Pins)DiagnosticLifetime.Add(P.Get());
    for(const auto& P:Built.SourceLifetime)DiagnosticLifetime.Add(P.Get()); // UPROPERTY pins; no retained strong self-cycle.
    if(R.OwnedMaterial)DiagnosticLifetime.Add(R.OwnedMaterial);
    DiagnosticSetup=R.OwnedSetup;DiagnosticReceipt=MoveTemp(Candidate);
    if(!DiagnosticCompoundUnchanged())
    {DiagnosticSetup=nullptr;DiagnosticReceipt.Reset();DiagnosticLifetime.Reset();Error=TEXT("Private compound receipt changed at publication");return false;}
    return true; // Collision-only model; no production component, lease or admission.
}
bool UDDSOriginalCalibrationMeshComponent::DiagnosticCompoundUnchanged() const
{
    using namespace DDSOriginalMeasurement;
    if(!DiagnosticReceipt || !DiagnosticSetup)return false;const auto& R=*DiagnosticReceipt;
    if(!Loaded(this) || !Loaded(R.TargetOwner) || R.TargetOwner->IsActorBeingDestroyed() || !Loaded(R.TargetWorld) || R.TargetWorld->bIsTearingDown
       || GetOwner()!=R.TargetOwner || GetOuter()!=R.TargetOwner || GetWorld()!=R.TargetWorld || R.TargetWorld->GetPhysicsScene()!=R.TargetScene
       || GetStaticMesh().Get()!=R.TargetMesh || !Loaded(R.TargetMesh) || R.TargetMesh->IsCompiling()
       || DiagnosticSetup.Get()!=R.OwnedSetup || !Loaded(R.OwnedSetup) || R.OwnedSetup->GetOuter()!=this || R.OwnedSetup->BodySetupGuid!=R.OwnedGuid
       || R.OwnedSetup->PhysMaterial.Get()!=R.OwnedMaterial || (R.OwnedMaterial && !Loaded(R.OwnedMaterial))
       || !DiagnosticSourcesUnchanged(R) || !DiagnosticOriginsExact(R) || R.OwnedSetup->AggGeom.ConvexElems.Num()!=R.HullOrigins.Num())return false;
    TArray<uint8> Collision,Settings;
    if(!SetupReceiptBytes(R.OwnedSetup,Collision,Settings) || Collision!=R.OwnedCollision || Settings!=R.OwnedSettings)return false;
    for(const auto& Hull:R.OwnedSetup->AggGeom.ConvexElems)if(!Hull.GetChaosConvexMesh().IsValid())return false;
    return true;
}

bool FDDSOriginalRobotMeasurementProducer::CaptureDraft(AActor& Actor,const FDDSOriginalRobotNativeGeometry& Input,
    const FDDSOriginalRobotDraftInputs& Inputs,FDDSOriginalRobotMeasurementDraft& Out,FString& Error)
{
    using namespace DDSOriginalMeasurement;Out={};Error.Reset();
    const auto Refuse=[&](const TCHAR* Why){Out={};Error=Why;return false;};
    const auto G=Input;
    if(!IsInGameThread() || !Loaded(&Actor) || Actor.IsActorBeingDestroyed() || !Loaded(Actor.GetWorld()) || Actor.GetWorld()->bIsTearingDown
       || !G.bActualOriginalAssetsAndTemplatesObserved || (G.Parts.Num()!=15 && G.Parts.Num()!=16)
       || !Sha(G.IntegrityLedgerSha256) || !Sha(G.NativeGeometryRecordSha256) || !Sha(G.WorldPlacementRecordSha256))return Refuse(TEXT("Actual coherent original geometry/actor/record pins required"));
    if(Inputs.bExplicitGameTargets && (!DDSValidateRobotProfile(Inputs.DeclaredGameProfile,Error) || Inputs.DeclaredGameProfile.Service.bFrozen))
        return Refuse(TEXT("Explicit GAME targets need a valid source profile with independent lid masses and unfrozen service"));
    int32 ParcelRoles=0;
    for(const auto& P:G.Parts)
    {if(P.Role==EDDSOriginalRobotPartRole::Parcel && P.Index==0)++ParcelRoles;
     else if(!((P.Role==EDDSOriginalRobotPartRole::Body && P.Index>=0 && P.Index<9) || (P.Role==EDDSOriginalRobotPartRole::Suspension && P.Index>=0 && P.Index<6)))return Refuse(TEXT("Unsupported original role/index in draft roster"));}
    if(ParcelRoles!=(G.Parts.Num()==16?1:0))return Refuse(TEXT("Draft must select exact15 robot roles with optional single excluded parcel0"));
    TStrongObjectPtr<AActor> ActorPin(&Actor);TStrongObjectPtr<UWorld> WorldPin(Actor.GetWorld());
    TArray<UStaticMeshComponent*> Components;Actor.GetComponents(Components);TSet<UStaticMeshComponent*> Seen;TSet<FString> Ids;
    FDDSOriginalRobotMeasurementDraft Candidate;Candidate.SourceGeometry=G;Candidate.SourceActor=&Actor;Candidate.DeclaredGameProfile=Inputs.DeclaredGameProfile;Candidate.bExplicitGameTargets=Inputs.bExplicitGameTargets;
    TArray<TStrongObjectPtr<UObject>> Pins;Pins.Emplace(&Actor);Pins.Emplace(Actor.GetWorld());
    UStaticMeshComponent* Actual[15]={};
    for(int32 Slot=0;Slot<15;++Slot)
    {
        const auto* P=Part(G,Slot<9?EDDSOriginalRobotPartRole::Body:EDDSOriginalRobotPartRole::Suspension,Slot<9?Slot:Slot-9);
        if(!P || !SourceStillExact(*P) || !Sha(P->MeshPackageSha256) || !Sha(P->NativeTemplateRecordSha256) || P->SuppliedPartId.IsEmpty())return Refuse(TEXT("Exact original9 body +6 suspension role/source pins required"));
        UStaticMeshComponent* C=nullptr;for(auto* Found:Components)if(Found && Found->GetFName()==P->ComponentName){if(C)return Refuse(TEXT("Ambiguous original target component name"));C=Found;}
        if(!Loaded(C) || !C->IsRegistered() || C->GetOwner()!=&Actor || C->GetWorld()!=WorldPin.Get() || C->GetStaticMesh().Get()!=P->SuppliedMesh
           || Seen.Contains(C) || Ids.Contains(P->SuppliedPartId) || !C->GetComponentTransform().Equals(P->MeasuredWorldCm,1.e-6))return Refuse(TEXT("Exact current target original component/name/mesh/pose unavailable"));
        Actual[Slot]=C;Seen.Add(C);Ids.Add(P->SuppliedPartId);Pins.Emplace(C);Pins.Emplace(P->OriginalVisualSource);Pins.Emplace(P->SuppliedMesh);
        Candidate.SourceVisuals[Slot]=P->OriginalVisualSource;
        if(Slot<9)
        {
            if(!ConvexContent(C->GetBodySetup(),Candidate.ComponentConvexContent[Slot]))return Refuse(TEXT("Actual component-owned bounded convex shape snapshot unavailable"));
            Pins.Emplace(C->GetBodySetup());auto& B=Candidate.UnadmittedBinding.Bodies[Slot];B.ActualPart=C;B.SuppliedPartId=P->SuppliedPartId;
            B.SuppliedMeshPackage=P->ExactMeshObjectPath;B.MeshPackageSha256=P->MeshPackageSha256;B.SupplierScale=C->GetComponentScale();
            B.RestInChassisMeters=Meters(DDSRigidComponentPose(C->GetComponentTransform()).GetRelativeTransform(DDSRigidComponentPose(Actual[0]->GetComponentTransform())));
            FString Why;ObserveBody(*C,Inputs.bExplicitGameTargets?GameMass(Inputs.DeclaredGameProfile,Slot):0.0,Candidate.Bodies[Slot],Why);
            if(!Candidate.Bodies[Slot].bAvailable)Candidate.MissingEvidence.Add(FString::Printf(TEXT("body%d native dynamic observation: %s"),Slot,*Why));
        }
        else
        {
            auto& S=Candidate.UnadmittedBinding.SuspensionParts[Slot-9];S.ActualPart=C;S.SuppliedPartId=P->SuppliedPartId;S.SupplierScale=C->GetComponentScale();
            // Owner is deliberately unassigned as evidence; the enum default is
            // not a measured owner. bMeasured/rigidity/collision flags stay false.
            Candidate.MissingEvidence.Add(FString::Printf(TEXT("suspension%d stepped original rigid-owner/collision/mass allocation unverified"),Slot-9));
        }
    }
    auto* Chassis=Cast<UDDSRobotLeasedMeshComponent>(Actual[0]);
    if(!Chassis || !Chassis->HasPreparedOriginalChassisCompound() || !Chassis->PreparedOriginalChassisCompoundUnchanged()
       || Chassis->GetBodySetup()==Chassis->GetStaticMesh()->GetBodySetup())return Refuse(TEXT("Actual prepared component-owned chassis compound required; mesh legacy setup is insufficient"));
    for(int32 J=0;J<8;++J)
    { auto* Source=Inputs.JointSources[J].Get();if(Source)Pins.Emplace(Source);
      if(!Source || !NativeJoint(*Source,*Actual[0],*Actual[J+1],Candidate.Joints[J]))Candidate.MissingEvidence.Add(FString::Printf(TEXT("joint%d actual registered native connector source unavailable"),J)); }
    for(int32 M=0;M<4;++M)
    {
        auto* C=Inputs.MountSources[M].Get();
        if(Loaded(C) && C->IsRegistered() && C->GetOwner()==&Actor && C->GetWorld()==WorldPin.Get() && C->GetComponentTransform().IsValid())
        { Pins.Emplace(C);Candidate.bMountSourceAvailable[M]=true;Candidate.MountSourceObjectPaths[M]=C->GetPathName();
          Candidate.MountsInChassisMeters[M]=Meters(DDSRigidComponentPose(C->GetComponentTransform()).GetRelativeTransform(DDSRigidComponentPose(Actual[0]->GetComponentTransform()))); }
        else Candidate.MissingEvidence.Add(FString::Printf(TEXT("mount%d actual registered source frame unavailable"),M));
    }
    // Complete source/component reread after allocation and native read callbacks.
    for(int32 Slot=0;Slot<15;++Slot)
    {
        const auto* P=Part(G,Slot<9?EDDSOriginalRobotPartRole::Body:EDDSOriginalRobotPartRole::Suspension,Slot<9?Slot:Slot-9);auto* C=Actual[Slot];TArray<uint8> Bytes;
        if(!P || !SourceStillExact(*P) || !Loaded(C) || !C->IsRegistered() || C->GetOwner()!=&Actor || C->GetWorld()!=WorldPin.Get()
           || C->GetStaticMesh().Get()!=P->SuppliedMesh || !C->GetComponentTransform().Equals(P->MeasuredWorldCm,1.e-6)
           || (Slot<9 && (!ConvexContent(C->GetBodySetup(),Bytes) || Bytes!=Candidate.ComponentConvexContent[Slot])))return Refuse(TEXT("Draft source/component identity/pose/collision changed during observation"));
    }
    if(!Loaded(&Actor) || Actor.IsActorBeingDestroyed() || Actor.GetWorld()!=WorldPin.Get() || WorldPin->bIsTearingDown
       || !Chassis->PreparedOriginalChassisCompoundUnchanged())return Refuse(TEXT("Draft actor/world/compound lifetime revoked"));
    Candidate.MissingEvidence.Add(TEXT("Six pulse/loaded-rolling signs, contact/slip/sleep/closed-lid budgets and binding/certificate freeze remain unverified"));
    if(!Inputs.bExplicitGameTargets)Candidate.MissingEvidence.Add(TEXT("Explicit GAME target profile not supplied; no native-to-target mass/inertia ratio is assigned"));
    Out=MoveTemp(Candidate);return true;
}

bool FDDSOriginalRobotMeasurementProducer::CreateDeclaredGameMountSources(AActor& Actor,UStaticMeshComponent& Chassis,
    const FDDSOriginalRobotGameMountDeclarations& Design,FDDSOriginalRobotDraftInputs& Inputs,FDDSOriginalRobotGameMountLease& Out,FString& Error)
{
    using namespace DDSOriginalMeasurement;Error.Reset();static bool Creating=false;
    if(!IsInGameThread() || Creating || !Design.bExplicitGameDesign || Design.Provenance.IsEmpty() || Out.Actor.IsValid())
    {Error=TEXT("Explicit GAME mount design/provenance, empty owned lease and nonreentrant game thread required");return false;}
    for(const auto& ExistingSource:Out.Sources)if(ExistingSource.IsValid()){Error=TEXT("Existing GAME mount owned lease cannot be overwritten");return false;}
    for(const auto& Mount:Design.InChassisMeters)if(!Rigid(Mount)){Error=TEXT("Four finite unit-scale physical SI GAME mount declarations required");return false;}
    auto* World=Actor.GetWorld();auto* ManagedChassis=Cast<UDDSRobotLeasedMeshComponent>(&Chassis);
    TArray<UDDSRobotPhysicsRigComponent*> Rigs;Actor.GetComponents(Rigs,false);
    auto* SelectedRig=Rigs.Num()==1?Rigs[0]:nullptr;
    if(!Loaded(&Actor) || Actor.IsActorBeingDestroyed() || !Loaded(World) || World->bIsTearingDown || !Loaded(ManagedChassis)
       || !Chassis.IsRegistered() || Chassis.GetOwner()!=&Actor || Chassis.GetWorld()!=World || Chassis.IsSimulatingPhysics()
       || !ManagedChassis->HasPreparedOriginalChassisCompound() || !ManagedChassis->PreparedOriginalChassisCompoundUnchanged()
       || !World->GetPhysicsScene() || !Loaded(SelectedRig) || !SelectedRig->IsRegistered() || SelectedRig->GetOwner()!=&Actor || SelectedRig->GetOuter()!=&Actor || SelectedRig->GetWorld()!=World
       || SelectedRig->GetFName()!=FName(TEXT("MeasuredPhysicalRig")) || SelectedRig->GetClass()!=UDDSRobotPhysicsRigComponent::StaticClass()
       || SelectedRig->GetRigState()!=EDDSRobotRigState::Unbound)
    {Error=TEXT("Actual original managed chassis/unbound sole rig/context required for GAME mount sources");return false;}
    const FTransform ChassisPose=Chassis.GetComponentTransform();const auto* Scene=World->GetPhysicsScene();
    TStrongObjectPtr<AActor> ActorPin(&Actor);TStrongObjectPtr<UWorld> WorldPin(World);TStrongObjectPtr<UStaticMeshComponent> ChassisPin(&Chassis);TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(SelectedRig);
    TGuardValue<bool> CreationGuard(Creating,true);FDDSOriginalRobotGameMountLease Candidate;Candidate.Actor=&Actor;Candidate.Chassis=&Chassis;Candidate.Rig=SelectedRig;
    Candidate.Scene=Scene;Candidate.Generation=SelectedRig->GetGeneration();Candidate.ReadinessRevision=SelectedRig->GetReadinessRevision();
    int32 CreatedMountCount=0;
    const FName Names[4]={TEXT("DDSGameSensorMount"),TEXT("DDSGameChaseMount"),TEXT("DDSGameServiceMount"),TEXT("DDSGameCargoMount")};
    const auto Coherent=[&]()
    {
        TArray<UDDSRobotPhysicsRigComponent*> CurrentRigs;Actor.GetComponents(CurrentRigs,false);
        if(!Loaded(&Actor) || Actor.IsActorBeingDestroyed() || Actor.GetWorld()!=World || !Loaded(World) || World->bIsTearingDown || World->GetPhysicsScene()!=Scene
           || !Loaded(&Chassis) || !Chassis.IsRegistered() || Chassis.GetOwner()!=&Actor || Chassis.GetWorld()!=World || Chassis.IsSimulatingPhysics()
           || !Chassis.GetComponentTransform().Equals(ChassisPose,0) || !ManagedChassis->PreparedOriginalChassisCompoundUnchanged()
           || CurrentRigs.Num()!=1 || CurrentRigs[0]!=SelectedRig || !Loaded(SelectedRig) || !SelectedRig->IsRegistered()
           || SelectedRig->GetOwner()!=&Actor || SelectedRig->GetOuter()!=&Actor || SelectedRig->GetWorld()!=World || SelectedRig->GetRigState()!=EDDSRobotRigState::Unbound
           || SelectedRig->GetFName()!=FName(TEXT("MeasuredPhysicalRig")) || SelectedRig->GetClass()!=UDDSRobotPhysicsRigComponent::StaticClass()
           || SelectedRig->GetGeneration()!=Candidate.Generation || SelectedRig->GetReadinessRevision()!=Candidate.ReadinessRevision)return false;
        for(int32 Index=0;Index<CreatedMountCount;++Index)
        {const FTransform Expected=DDSPhysicalCm(Design.InChassisMeters[Index])*DDSRigidComponentPose(ChassisPose);
         auto* Source=Candidate.Sources[Index].Get();
         if(!Loaded(Source) || !Source->IsRegistered() || Source->GetOwner()!=&Actor || Source->GetOuter()!=&Actor || Source->GetWorld()!=World || Source->GetAttachParent()!=&Chassis
            || Source->GetFName()!=Names[Index] || !Source->GetComponentTransform().Equals(Expected,1.e-6))return false;}
        return true;
    };
    TArray<USceneComponent*> Existing;Actor.GetComponents(Existing,false);
    for(auto* Component:Existing)for(FName Name:Names)if(Component && Component->GetFName()==Name)
    {Error=TEXT("Declared GAME mount name already exists; no existing component replaced");return false;}
    for(int32 Index=0;Index<4;++Index)
    {
        if(!Coherent()){Error=TEXT("GAME mount source context revoked before creation");Out=Candidate;return false;}
        auto* Source=NewObject<USceneComponent>(&Actor,Names[Index],RF_Transient);
        if(!Loaded(Source)){Error=TEXT("GAME mount source allocation failed; partial owned lease retained");Out=Candidate;return false;}
        Candidate.Sources[Index]=Source;++CreatedMountCount;Actor.AddInstanceComponent(Source);Source->SetMobility(EComponentMobility::Movable);Source->SetupAttachment(&Chassis);
        Source->SetWorldTransform(DDSPhysicalCm(Design.InChassisMeters[Index])*DDSRigidComponentPose(ChassisPose));Source->RegisterComponent();
        if(!Coherent()){Error=TEXT("GAME mount registration/physical readback revoked; partial owned lease retained");Out=Candidate;return false;}
    }
    // CaptureDraft independently reads these actual registered source transforms.
    for(int32 Index=0;Index<4;++Index)Inputs.MountSources[Index]=Candidate.Sources[Index];Out=Candidate;return true;
}
bool FDDSOriginalRobotMeasurementProducer::ReleaseDeclaredGameMountSources(FDDSOriginalRobotGameMountLease& Lease,FString& Error)
{
    using namespace DDSOriginalMeasurement;Error.Reset();static bool Releasing=false;
    if(!IsInGameThread() || Releasing){Error=TEXT("GAME mount source cleanup requires nonreentrant game thread");return false;}
    auto* Actor=Lease.Actor.Get();auto* Chassis=Lease.Chassis.Get();auto* Rig=Lease.Rig.Get();
    if(!Actor){Lease={};return true;}
    if(!Loaded(Actor) || Actor->IsActorBeingDestroyed() || !Loaded(Chassis) || Chassis->GetOwner()!=Actor || !Loaded(Rig) || Rig->GetOwner()!=Actor || Rig->GetOuter()!=Actor
       || Rig->GetGeneration()!=Lease.Generation || Rig->GetReadinessRevision()!=Lease.ReadinessRevision || Rig->GetRigState()!=EDDSRobotRigState::Unbound)
    {Error=TEXT("GAME mount owned source cleanup retained until exact unbound owner/generation context permits release");return false;}
    TStrongObjectPtr<AActor> ActorPin(Actor);TStrongObjectPtr<UStaticMeshComponent> ChassisPin(Chassis);TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(Rig);
    TStrongObjectPtr<UWorld> WorldPin(Actor->GetWorld());TGuardValue<bool> ReleaseGuard(Releasing,true);
    const auto Context=[&]()
    {TArray<UDDSRobotPhysicsRigComponent*> Current;Actor->GetComponents(Current,false);
     return Loaded(Actor) && !Actor->IsActorBeingDestroyed() && Loaded(WorldPin.Get()) && !WorldPin->bIsTearingDown && Actor->GetWorld()==WorldPin.Get() && Lease.Scene && WorldPin->GetPhysicsScene()==Lease.Scene
        && Loaded(Chassis) && Chassis->GetOwner()==Actor && Chassis->GetWorld()==WorldPin.Get() && Loaded(Rig) && Rig->IsRegistered()
        && Current.Num()==1 && Current[0]==Rig && Rig->GetOwner()==Actor && Rig->GetOuter()==Actor && Rig->GetWorld()==WorldPin.Get()
        && Rig->GetFName()==FName(TEXT("MeasuredPhysicalRig")) && Rig->GetClass()==UDDSRobotPhysicsRigComponent::StaticClass()
        && Rig->GetGeneration()==Lease.Generation && Rig->GetReadinessRevision()==Lease.ReadinessRevision && Rig->GetRigState()==EDDSRobotRigState::Unbound;};
    if(!Context()){Error=TEXT("GAME mount cleanup context revoked; owned sources retained");return false;}
    for(auto& WeakSource:Lease.Sources)if(auto* Source=WeakSource.Get())
    {if(!Loaded(Source) || Source->GetOwner()!=Actor || Source->GetOuter()!=Actor || Source->GetWorld()!=Actor->GetWorld() || Source->GetAttachParent()!=Chassis)
     {Error=TEXT("GAME mount source ownership changed; no foreign component destroyed");return false;}}
    for(auto& WeakSource:Lease.Sources)if(auto* Source=WeakSource.Get())
    {if(!Context() || !Loaded(Source) || Source->GetOwner()!=Actor || Source->GetOuter()!=Actor || Source->GetWorld()!=WorldPin.Get() || Source->GetAttachParent()!=Chassis)
     {Error=TEXT("GAME mount cleanup context revoked between owned operations; remaining sources retained");return false;}
     TStrongObjectPtr<USceneComponent> SourcePin(Source);Source->DestroyComponent();WeakSource.Reset();
     if(!Context()){Error=TEXT("GAME mount cleanup callback revoked owner context; remaining sources retained");return false;}}
    Lease={};return true;
}

bool FDDSOriginalRobotMeasurementProducer::RunCalibration(const FDDSOriginalRobotMeasurementDraft& Draft,
    const FDDSOriginalRobotCalibrationRecipe& Recipe,FDDSOriginalRobotNativeCalibrationDraft& Out,FString& Error)
{
    using namespace DDSOriginalMeasurement;Out={};Error.Reset();
    // Serialized native-writer entry. Settings overrides exist ONLY around this
    // private scene's creation; no config file, existing solver or City world changes.
    static bool bRunning=false;
    const auto Refuse=[&](const FString& Why){if(Out.FirstFailedPredicate.IsEmpty())Out.FirstFailedPredicate=Why;Error=Why;return false;};
    if(!IsInGameThread() || bRunning)return Refuse(TEXT("CALIBRATION_BUSY_OR_WRONG_THREAD"));
    TGuardValue<bool> RunGuard(bRunning,true);
    Out.bSuppliedDeclaredRecipeAvailable=true;Out.SuppliedDeclaredRecipe=Recipe;
    Out.SupportAdapterObservation.AdapterTag=Recipe.bExplicitOriginalSingleBoxSupportAdapter?TEXT("ORIGINAL_SINGLE_BOX_PRESERVED"):TEXT("LEGACY_CONVEX_UNIFORM");
    const auto G=Draft.SourceGeometry;const auto P=Draft.DeclaredGameProfile;
    if(G.Parts.Num()!=16)return Refuse(TEXT("Current compound factory requires original16 observed shape-source record; parcel remains excluded from this15-part/9-body fixture. Supply that original observation record before RunCalibration."));
    if(!Draft.bExplicitGameTargets || !Recipe.bExplicitGameFixtureHypotheses || Recipe.HypothesisDescription.IsEmpty()
       || !DDSValidateRobotProfile(P,Error) || P.Service.bFrozen || Draft.UnadmittedBinding.Mechanics.bFrozen
       || Draft.UnadmittedBinding.Parcel.ActualPart || Draft.UnadmittedBinding.bNineBodyTopologyVerified
       || Draft.UnadmittedBinding.bMountsMeasured || Draft.UnadmittedBinding.bFramesAreScaledPhysicalSI
       || !Draft.UnadmittedBinding.BindingSha256.IsEmpty() || !Draft.UnadmittedBinding.A02MeasurementCertificateSha256.IsEmpty())
        return Refuse(Error.IsEmpty()?TEXT("Explicit unadmitted closed-lid GAME fixture hypotheses/profile required"):Error);
    for(const auto& B:Draft.UnadmittedBinding.Bodies)
        if(B.bMeasured || B.bSimpleCollisionMeasured || B.bCCDMeasuredRequired || B.MeasuredMotorForwardSign!=0 || !B.MeasurementRecordSha256.IsEmpty() || !B.SimpleCollisionSha256.IsEmpty())
            return Refuse(TEXT("Draft body admission flags/signs/hashes must remain unset"));
    for(const auto& J:Draft.UnadmittedBinding.Joints)if(J.bMeasured)return Refuse(TEXT("Draft joint admission flag must remain unset"));
    for(const auto& S:Draft.UnadmittedBinding.SuspensionParts)
        if(S.bMeasured || S.bRigidRelativeOwnerThroughoutMotion || S.bCollisionAlreadyIncludedInOwnerShape || !S.GeometryAndMassMapSha256.IsEmpty())
            return Refuse(TEXT("Draft suspension admission flags/hash must remain unset"));
    const auto RecipeLeaf=[&](const TCHAR* Name,bool Failed)
    {Out.EvaluatedRecipePredicates.Add(FString(Name)+(Failed?TEXT("=TRUE"):TEXT("=FALSE")));if(Failed)Out.FirstFailedPredicate=Name;return Failed;};
    if(RecipeLeaf(TEXT("RECIPE_SETTLE_STEP_BOUNDS"),Recipe.SettleSteps<2 || Recipe.SettleSteps>600)
       || RecipeLeaf(TEXT("RECIPE_PULSE_STEP_BOUNDS"),Recipe.PulseStepsPerWheel<2 || Recipe.PulseStepsPerWheel>120)
       || RecipeLeaf(TEXT("RECIPE_COAST_STEP_BOUNDS"),Recipe.CoastStepsPerWheel<2 || Recipe.CoastStepsPerWheel>120)
       || RecipeLeaf(TEXT("RECIPE_POSITIVE_FINITE_TORQUE"),!Positive(Recipe.PulseTorqueNm))
       || RecipeLeaf(TEXT("RECIPE_FINITE_NATIVE_TORQUE"),!FMath::IsFinite(Recipe.PulseTorqueNm*DDSRobotUnits::UETorquePerNm))
       || RecipeLeaf(TEXT("RECIPE_TORQUE_ABOVE_GAME_LIMIT"),Recipe.PulseTorqueNm>P.MotorPeakNm)
       || RecipeLeaf(TEXT("RECIPE_RIGID_CHASSIS_POSE"),!Rigid(Recipe.FixtureChassisWorldCm))
       || RecipeLeaf(TEXT("RECIPE_VALID_SUPPORT_POSE"),!Recipe.FixtureSupportWorldCm.IsValid())
       || RecipeLeaf(TEXT("RECIPE_SUPPORT_SCALE_ADAPTER"),Recipe.bExplicitOriginalSingleBoxSupportAdapter?!PositiveScale(Recipe.FixtureSupportWorldCm.GetScale3D()):!Uniform(Recipe.FixtureSupportWorldCm.GetScale3D()))
       || RecipeLeaf(TEXT("RECIPE_NORMALIZED_FORWARD"),!Recipe.DeclaredForwardInChassis.IsNormalized()))
        return Refuse(TEXT("Finite explicit fixture pose/forward/torque and bounded native-step counts required"));
    if(Recipe.bExplicitBidirectionalTrials && !Recipe.bExplicitGameAcceptanceLimits)
        return Refuse(TEXT("Bidirectional trials require explicit GAME response/contact/slip/sleep engineering limits"));
    if(Recipe.bExplicitGameAcceptanceLimits)
    {
        const double Limits[]={Recipe.GameSlipDomainSpeedMps,Recipe.GameSleepingAnchorToleranceM,Recipe.GameSleepingSpeedToleranceMps,
            Recipe.GameSleepingOmegaToleranceRadps,Recipe.GameMinSupportUpDot,Recipe.GameMinLoadedImpulseNs,
            Recipe.GameMinAxleResponseRadps,Recipe.GameMinRollingForwardMps,Recipe.GameMinCOMForwardMps};
        for(double Limit:Limits)if(!Positive(Limit))return Refuse(TEXT("Explicit GAME native-trial limits must be finite and positive"));
        if(!Positive(Recipe.GameMaxContactSeparationM) || !Positive(Recipe.GameClosedLidAngleToleranceRad) || !Positive(Recipe.GameClosedLidOmegaToleranceRadps))
            return Refuse(TEXT("Explicit finite positive GAME contact separation and closed-lid angular/omega limits required"));
        const double Converted[]={Recipe.GameSlipDomainSpeedMps*100,Recipe.GameSleepingAnchorToleranceM*100,Recipe.GameSleepingSpeedToleranceMps*100,
            Recipe.GameMinLoadedImpulseNs*100,Recipe.GameMinRollingForwardMps*100,Recipe.GameMinCOMForwardMps*100,Recipe.GameMaxContactSeparationM*100};
        for(double ConvertedLimit:Converted)if(!Positive(ConvertedLimit))return Refuse(TEXT("Explicit GAME limit SI-to-native conversion is nonfinite"));
        if(Recipe.GameMinSupportUpDot>1 || Recipe.GameMinLoadedSamplesPerDirection<2 || Recipe.GameMinLoadedSamplesPerDirection>Recipe.PulseStepsPerWheel)
            return Refuse(TEXT("Finite GAME up-normal and per-direction loaded sample coverage required"));
    }
    const int32 StepsPerWheel=(Recipe.PulseStepsPerWheel+Recipe.CoastStepsPerWheel)*(Recipe.bExplicitBidirectionalTrials?2:1);
    const int32 Total=Recipe.SettleSteps+6*StepsPerWheel+2;
    if(Total>4096)return Refuse(TEXT("Bounded native diagnostic maximum4096 frames exceeded"));
    double Allocations=0;
    for(double M:Recipe.DeclaredSuspensionMassAllocationKg){if(!Positive(M))return Refuse(TEXT("Six positive declared included-in-chassis GAME mass shares required"));Allocations+=M;}
    if(Allocations>=P.BareChassisKg)return Refuse(TEXT("Suspension shares consume/exceed declared total chassis mass"));
    for(int32 J=0;J<8;++J)
    {
        const auto& H=Recipe.JointHypotheses[J];
        if(H.bMeasured || !Rigid(H.InChassisMeters) || !Rigid(H.InPartMeters) || !FMath::IsFinite(H.SuspensionHalfTravelM)
           || H.SuspensionHalfTravelM<0 || !FMath::IsFinite(H.SuspensionRestTargetM)
           || FMath::Abs(H.SuspensionRestTargetM)>H.SuspensionHalfTravelM)
            return Refuse(TEXT("Fixture joints are explicit finite hypotheses, never measured-admission flags"));
        const FTransform Meets=H.InPartMeters*Draft.UnadmittedBinding.Bodies[J+1].RestInChassisMeters;
        if(!Meets.Equals(H.InChassisMeters,1.e-6))return Refuse(FString::Printf(TEXT("Fixture joint%d candidate frames do not meet at observed physical rest"),J));
    }
    auto* SupportSource=Recipe.OriginalSupportSource.Get();TArray<uint8> SupportBytes;
    if(!Loaded(Draft.SourceActor.Get()) || !Loaded(SupportSource) || !SupportSource->IsRegistered() || !Loaded(SupportSource->GetOwner())
       || SupportSource->GetOwner()->IsActorBeingDestroyed() || !Loaded(SupportSource->GetWorld()) || SupportSource->GetWorld()->bIsTearingDown || !Loaded(SupportSource->GetStaticMesh().Get())
       || SupportSource->GetStaticMesh()->IsCompiling() || (!Recipe.bExplicitOriginalSingleBoxSupportAdapter && !ConvexContent(SupportSource->GetBodySetup(),SupportBytes)))
        return Refuse(TEXT("Exact registered original support with supported convex collision required; no generated floor/proxy"));
    const FTransform SupportSourcePose=SupportSource->GetComponentTransform();auto* SupportMesh=SupportSource->GetStaticMesh().Get();
    auto* SupportSetup=SupportSource->GetBodySetup();
    if(!Loaded(SupportSetup))return Refuse(TEXT("SUPPORT_SOURCE_BODY_SETUP_UNAVAILABLE"));
    const FGuid SupportGuid=SupportSetup->BodySetupGuid;
    if(SupportSetup!=SupportMesh->GetBodySetup() || !Recipe.FixtureSupportWorldCm.GetScale3D().Equals(SupportSourcePose.GetScale3D(),1.e-6))
        return Refuse(TEXT("Original support must retain its exact mesh-owned collision and supplier scale; custom support setups require an explicit adapter"));
    auto* SupportOwner=SupportSource->GetOwner();auto* SupportWorld=SupportSource->GetWorld();auto* SupportBody=SupportSource->GetBodyInstance();
    if(Recipe.bExplicitOriginalSingleBoxSupportAdapter)
    {
        if(SupportWorld!=Draft.SourceActor->GetWorld() || !Recipe.FixtureSupportWorldCm.Equals(SupportSourcePose,1.e-6))
            return Refuse(TEXT("SUPPORT_BOX_EXACT_ORIGINAL_WORLD_AND_FULL_TRANSFORM"));
        if(!OriginalSingleBoxRaw(*SupportSource,Out.SupportAdapterObservation,Error)
           || !SupportNativeRoster(*SupportSource,Out.SupportAdapterObservation.SourceNative,Error))return Refuse(Error);
    }
    const FDDSOriginalRobotNativePart* Sources[15]={};
    TArray<TStrongObjectPtr<UObject>> Pins;Pins.Emplace(SupportSource);Pins.Emplace(SupportSource->GetOwner());Pins.Emplace(SupportSource->GetWorld());Pins.Emplace(SupportMesh);Pins.Emplace(SupportSetup);
    const auto SupportSourceUnchanged=[&]()
    {
        if(!Recipe.bExplicitOriginalSingleBoxSupportAdapter)return true;
        FDDSOriginalRobotSupportAdapterObservation Now;FDDSOriginalRobotSupportNativeBodyObservation NativeNow;FString Why;
        if(!Loaded(SupportSource) || SupportSource->GetOwner()!=SupportOwner || SupportSource->GetWorld()!=SupportWorld || SupportSource->GetBodyInstance()!=SupportBody
           || SupportSource->GetStaticMesh().Get()!=SupportMesh || SupportSource->GetBodySetup()!=SupportSetup || SupportSetup->BodySetupGuid!=SupportGuid
           || !SupportSource->GetComponentTransform().Equals(SupportSourcePose,1.e-6) || !OriginalSingleBoxRaw(*SupportSource,Now,Why))
        {Out.FirstFailedPredicate=Why.IsEmpty()?TEXT("SUPPORT_BOX_SOURCE_CONTEXT_CHANGED"):Why;return false;}
        const auto& Was=Out.SupportAdapterObservation;
        if(Now.AggregateGeometryContent!=Was.AggregateGeometryContent || Now.SetupSettingsContent!=Was.SetupSettingsContent
           || Now.ComponentBodySettingsContent!=Was.ComponentBodySettingsContent || Now.ComponentCollisionProfile!=Was.ComponentCollisionProfile)
        {Out.FirstFailedPredicate=TEXT("SUPPORT_BOX_SOURCE_RAW_OR_POLICY_CHANGED");return false;}
        if(!SupportNativeRoster(*SupportSource,NativeNow,Why)){Out.FirstFailedPredicate=Why;return false;}
        if(!SupportRostersMatch(Was.SourceNative,NativeNow,true)){Out.FirstFailedPredicate=TEXT("SUPPORT_BOX_SOURCE_NATIVE_CHANGED");return false;}
        return true;
    };
    for(int32 I=0;I<15;++I)
    {
        Sources[I]=Part(G,I<9?EDDSOriginalRobotPartRole::Body:EDDSOriginalRobotPartRole::Suspension,I<9?I:I-9);
        if(!Sources[I] || !Draft.SourceVisuals[I].IsValid() || Draft.SourceVisuals[I].Get()!=Sources[I]->OriginalVisualSource || !SourceStillExact(*Sources[I]))return Refuse(TEXT("Original source receipt changed before isolated fixture creation"));
        Pins.Emplace(Sources[I]->OriginalVisualSource);Pins.Emplace(Sources[I]->OriginalVisualSource->GetOwner());Pins.Emplace(Sources[I]->OriginalVisualSource->GetWorld());
        Pins.Emplace(Sources[I]->SuppliedMesh);Pins.Emplace(Sources[I]->SuppliedMesh->GetBodySetup());
    }
    UWorld* World=nullptr;
    {
        auto* Settings=GetMutableDefault<UPhysicsSettings>();if(!Settings)return Refuse(TEXT("Physics settings unavailable before scene construction"));
        const bool OldAsync=Settings->bTickPhysicsAsync;const float OldDt=Settings->AsyncFixedTimeStepSize,OldGravity=Settings->DefaultGravityZ;
        ON_SCOPE_EXIT { Settings->bTickPhysicsAsync=OldAsync;Settings->AsyncFixedTimeStepSize=OldDt;Settings->DefaultGravityZ=OldGravity; };
        Settings->bTickPhysicsAsync=true;Settings->AsyncFixedTimeStepSize=float(StepS);Settings->DefaultGravityZ=float(GravityCmps2);
        const auto IV=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
        World=UWorld::CreateWorld(EWorldType::Game,false,MakeUniqueObjectName(GetTransientPackage(),UWorld::StaticClass(),TEXT("DDSOriginalNativeCalibrationDraft")),GetTransientPackage(),true,ERHIFeatureLevel::Num,&IV);
    }
    if(!Loaded(World) || !World->GetPhysicsScene()) {if(World)World->DestroyWorld(false);return Refuse(TEXT("Private calibration native physics world creation failed"));}
    TStrongObjectPtr<UWorld> WorldPin(World);auto* Scene=World->GetPhysicsScene();auto* Solver=Scene->GetSolver();FCalibrationCallback* Callback=nullptr;
    Out.FixtureWorldObjectPath=World->GetPathName();Out.SourceActorObjectPath=GetPathNameSafe(Draft.SourceActor.Get());
    Out.FixtureSceneIdentity=RunIdentity(Scene);Out.FixtureSolverIdentity=RunIdentity(Solver);Out.bBidirectionalTrialsRequested=Recipe.bExplicitBidirectionalTrials;
    // Every output handle/read lock is scope-local. Removal is marshalled and
    // waited while this OWN world/scene/bodies remain strongly pinned.
    ON_SCOPE_EXIT
    {
        if(Loaded(World) && World->GetPhysicsScene()==Scene && Scene->GetSolver()==Solver)
        {if(Callback && Solver) {while(auto H=Callback->PopFutureOutputData_External()){} Solver->UnregisterAndFreeSimCallbackObject_External(Callback);Callback=nullptr;Scene->Flush();}
         Scene->WaitPhysScenes();World->DestroyWorld(false);}
        Callback=nullptr; // An independently destroyed private solver owns its callback teardown.
    };
    auto* Settings=World->GetWorldSettings();if(!Settings || !Solver)return Refuse(TEXT("Private world settings/solver unavailable"));
    Settings->bGlobalGravitySet=true;Settings->GlobalGravityZ=float(GravityCmps2);
    Settings->WorldGravityZ=float(GravityCmps2);Settings->bWorldGravitySet=true;
    Out.ObservedWorldGravityCmps2=World->GetGravityZ();Out.ObservedSolverAsyncDtS=Solver->GetAsyncDeltaTime();
    if(!FMath::IsNearlyEqual(Out.ObservedWorldGravityCmps2,GravityCmps2,1.e-4)
       || !FMath::IsNearlyEqual(Out.ObservedSolverAsyncDtS,StepS,1.e-7))return Refuse(TEXT("Constructed private solver/world failed native120Hz/gravity readback"));
    FActorSpawnParameters Spawn;Spawn.ObjectFlags=RF_Transient;
    auto* Owner=World->SpawnActor<AActor>(AActor::StaticClass(),FTransform::Identity,Spawn);if(!Loaded(Owner))return Refuse(TEXT("Fixture actor allocation failed"));Pins.Emplace(Owner);
    UDDSOriginalCalibrationMeshComponent* Bodies[9]={};UDDSOriginalCalibrationMeshComponent* Suspension[6]={};UPhysicsConstraintComponent* Joints[8]={};
    UBodySetup* ExpectedSetups[9]={};TArray<uint8> FixtureShapeBytes[9];
    auto* Contact=NewObject<UPhysicalMaterial>(Owner,NAME_None,RF_Transient);if(!Loaded(Contact))return Refuse(TEXT("Fixture GAME contact material unavailable"));Pins.Emplace(Contact);
    Contact->StaticFriction=P.StaticFriction;Contact->Friction=P.DynamicFriction;Contact->Restitution=P.Restitution;
    Contact->bOverrideFrictionCombineMode=true;Contact->FrictionCombineMode=EFrictionCombineMode::Average;
    Contact->bOverrideRestitutionCombineMode=true;Contact->RestitutionCombineMode=EFrictionCombineMode::Average;
    const auto CopyRawVisual=[&](UStaticMeshComponent& C,const UStaticMeshComponent& Source)
    {
        // Collision-only hidden fixture preserves the source raw slot assignments
        // and component Nanite policy. It grants no production material permit.
        C.OverrideMaterials=Source.OverrideMaterials;C.bDisallowNanite=Source.bDisallowNanite;
        C.bForceNaniteForMasked=Source.bForceNaniteForMasked;C.SetForceDisableNanite(Source.bForceDisableNanite);
        C.SetVisibility(false);C.SetHiddenInGame(true);
    };
    if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked before robot construction"));
    for(int32 I=0;I<9;++I)
    {
        const auto& S=*Sources[I];auto* C=NewObject<UDDSOriginalCalibrationMeshComponent>(Owner,NAME_None,RF_Transient);if(!Loaded(C))return Refuse(TEXT("Fixture original mesh component allocation failed"));
        Pins.Emplace(C);Bodies[I]=C;Owner->AddInstanceComponent(C);if(I==0)Owner->SetRootComponent(C);
        C->SetMobility(EComponentMobility::Movable);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if(!C->SetStaticMesh(S.SuppliedMesh))return Refuse(TEXT("Fixture original mesh installation failed"));
        auto* MeshSetup=S.SuppliedMesh->GetBodySetup();if(!Loaded(MeshSetup))return Refuse(TEXT("Original mesh default body setup unavailable"));
        const auto& Defaults=MeshSetup->DefaultInstance;
        if(Defaults.OwnerComponent.Get()!=nullptr || Defaults.GetBodySetup()!=nullptr || FPhysicsInterface::IsValid(Defaults.ActorHandle))return Refuse(TEXT("Original mesh DefaultInstance contains runtime owner/setup/actor; unsafe copy refused"));
        C->BodyInstance.CopyBodyInstancePropertiesFrom(&Defaults);C->BodyInstance.bAutoWeld=false;C->BodyInstance.InertiaTensorScale=FVector::OneVector;
        C->BodyInstance.SetInertiaConditioningEnabled(false);CopyRawVisual(*C,*S.OriginalVisualSource);
        if(I==0 && !C->PrepareDiagnosticCompound(G,Error))return false;
        if(I==0 && (!C->DiagnosticCompoundUnchanged() || C->GetBodySetup()==MeshSetup))return Refuse(TEXT("Fixture must observe actual component-owned pure-builder compound, not legacy mesh setup"));
        const FTransform Pose=DDSPhysicalCm(Draft.UnadmittedBinding.Bodies[I].RestInChassisMeters)*Recipe.FixtureChassisWorldCm;
        C->SetWorldTransform(FTransform(Pose.GetRotation(),Pose.GetLocation(),Draft.UnadmittedBinding.Bodies[I].SupplierScale));
        C->SetCollisionProfileName(TEXT("PhysicsActor"));C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);C->SetPhysMaterialOverride(Contact);
        C->SetEnableGravity(false);C->SetLinearDamping(0);C->SetAngularDamping(0);C->RegisterComponent();C->SetSimulatePhysics(true);
        if(!Loaded(C) || !C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World
           || (I==0 && !C->DiagnosticCompoundUnchanged()))return Refuse(TEXT("Fixture registration revoked diagnostic target/source receipts"));
        if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked by robot registration"));
        if(!ObserveBody(*C,GameMass(P,I),Out.NativeReferenceBodies[I],Error))return false;
        if(I==0 && !C->DiagnosticCompoundUnchanged())return Refuse(TEXT("Native reference observation revoked exact diagnostic compound receipts"));
        // Deliberately separate engine-generated reference model from chosen GAME
        // mass. Preserve physical collision/COM/axes; verify linear inertia ratio.
        C->SetMassOverrideInKg(NAME_None,GameMass(P,I),true);
        if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked by robot mass setup"));
        if(!ObserveBody(*C,GameMass(P,I),Out.DeclaredMassEngineBodies[I],Error))return false;
        if(I==0 && !C->DiagnosticCompoundUnchanged())return Refuse(TEXT("Native declared-mass observation revoked diagnostic compound receipts"));
        ExpectedSetups[I]=C->GetBodySetup();Pins.Emplace(ExpectedSetups[I]);
        if(!ConvexContent(ExpectedSetups[I],FixtureShapeBytes[I]))return Refuse(TEXT("Fixture native component convex content snapshot failed"));
        Out.NativeFixtureConvexContent[I]=FixtureShapeBytes[I];
        const auto& N=Out.NativeReferenceBodies[I];const auto& D=Out.DeclaredMassEngineBodies[I];
        const double Tolerance=FMath::Max(1.e-8,N.DeclaredGamePrincipalInertiaKgM2.GetMax()*1.e-5);
        if(!FMath::IsNearlyEqual(D.NativeMassKg,GameMass(P,I),1.e-4)
           || !D.NativePrincipalInertiaKgM2.Equals(N.DeclaredGamePrincipalInertiaKgM2,Tolerance)
           || !D.NativeMassSpaceBodyLocalMeters.Equals(N.NativeMassSpaceBodyLocalMeters,1.e-6))
            return Refuse(FString::Printf(TEXT("body%d actual declared-mass inertia/COM/axes disagrees with same-shape linear density ratio"),I));
        C->SetEnableGravity(true);++Out.ConstructedBodyCount;
        if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked during robot body construction"));
    }
    if(!Bodies[0]->DiagnosticReceipt || !DiagnosticOriginsExact(*Bodies[0]->DiagnosticReceipt))return Refuse(TEXT("Actual diagnostic constituent provenance receipt unavailable"));
    Out.ChassisCompoundHullOrigins=Bodies[0]->DiagnosticReceipt->HullOrigins;
    for(int32 I=0;I<6;++I)
    {
        const auto& S=*Sources[I+9];auto* C=NewObject<UDDSOriginalCalibrationMeshComponent>(Owner,NAME_None,RF_Transient);if(!Loaded(C))return Refuse(TEXT("Fixture original suspension allocation failed"));
        Pins.Emplace(C);Suspension[I]=C;Owner->AddInstanceComponent(C);C->SetMobility(EComponentMobility::Movable);
        C->SetCollisionEnabled(ECollisionEnabled::NoCollision);if(!C->SetStaticMesh(S.SuppliedMesh))return Refuse(TEXT("Original suspension mesh installation failed"));CopyRawVisual(*C,*S.OriginalVisualSource);
        const FTransform Rest=DDSRigidComponentPose(S.MeasuredWorldCm).GetRelativeTransform(DDSRigidComponentPose(Sources[0]->MeasuredWorldCm));
        const FTransform Pose=Rest*Recipe.FixtureChassisWorldCm;C->SetWorldTransform(FTransform(Pose.GetRotation(),Pose.GetLocation(),S.MeasuredWorldCm.GetScale3D()));
        C->AttachToComponent(Bodies[0],FAttachmentTransformRules::KeepWorldTransform);C->RegisterComponent();
        auto* OriginalSetup=Loaded(S.SuppliedMesh)?S.SuppliedMesh->GetBodySetup():nullptr;
        if(!SourceStillExact(S) || !Loaded(OriginalSetup) || !Loaded(World) || World->bIsTearingDown || World->GetPhysicsScene()!=Scene || Scene->GetSolver()!=Solver
           || !Loaded(Owner) || Owner->IsActorBeingDestroyed() || !Loaded(C) || !C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World)
            return Refuse(TEXT("Suspension registration revoked original setup/private owner/scene before constituent readback"));
        auto& Constituent=Out.SuspensionConstituents[I];Constituent.OriginalPartId=S.SuppliedPartId;Constituent.OriginalMeshObjectPath=S.ExactMeshObjectPath;
        Constituent.OriginalMeshPackageSha256=S.MeshPackageSha256;Constituent.OriginalBodySetupGuid=OriginalSetup->BodySetupGuid;
        Constituent.FixtureComponentPath=C->GetPathName();Constituent.ObservedOwnerBodyIndex=C->GetAttachParent()==Bodies[0]?0:INDEX_NONE;
        Constituent.bObservedNoIndependentSimulation=!C->IsSimulatingPhysics();Constituent.bObservedNoIndependentCollision=C->GetCollisionEnabled()==ECollisionEnabled::NoCollision;
        Constituent.DeclaredIncludedOwnerMassShareKg=Recipe.DeclaredSuspensionMassAllocationKg[I];Constituent.ObservedOwnerNativeMassKg=Out.DeclaredMassEngineBodies[0].NativeMassKg;
        Constituent.ObservedRestInOwnerMeters=Meters(DDSRigidComponentPose(C->GetComponentTransform()).GetRelativeTransform(DDSRigidComponentPose(Bodies[0]->GetComponentTransform())));
        for(const auto& Origin:Out.ChassisCompoundHullOrigins)if(Origin.OriginalPartId==S.SuppliedPartId)Constituent.IncludedCompoundHullOrigins.Add(Origin);
        if(Constituent.ObservedOwnerBodyIndex!=0 || !Constituent.bObservedNoIndependentSimulation || !Constituent.bObservedNoIndependentCollision
           || Constituent.IncludedCompoundHullOrigins.Num()!=OriginalSetup->AggGeom.ConvexElems.Num())
            return Refuse(TEXT("Actual suspension owner/no-double-collider/compound constituent inclusion readback failed"));
        if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked during suspension construction"));
    }
    auto* Floor=NewObject<UStaticMeshComponent>(Owner,NAME_None,RF_Transient);if(!Loaded(Floor))return Refuse(TEXT("Original support copy allocation failed"));Pins.Emplace(Floor);Owner->AddInstanceComponent(Floor);
    Floor->SetMobility(EComponentMobility::Static);if(!Floor->SetStaticMesh(SupportMesh))return Refuse(TEXT("Exact original support mesh installation failed"));CopyRawVisual(*Floor,*SupportSource);
    if(Recipe.bExplicitOriginalSingleBoxSupportAdapter)
    {
        // Copy only properties to our own unregistered body; never original handles.
        const auto& Defaults=SupportSetup->DefaultInstance;
        if(Defaults.OwnerComponent.Get()!=nullptr || Defaults.GetBodySetup()!=nullptr || FPhysicsInterface::IsValid(Defaults.ActorHandle))
            return Refuse(TEXT("SUPPORT_BOX_UNSAFE_DEFAULT_PROPERTY_COPY"));
        if(!SupportSource->BodyInstance.DoesUseCollisionProfile())return Refuse(TEXT("SUPPORT_BOX_NAMED_COLLISION_PROFILE_REQUIRED"));
        Floor->BodyInstance.CopyBodyInstancePropertiesFrom(&Defaults);
        Floor->SetCollisionProfileName(SupportSource->GetCollisionProfileName());
        Floor->BodyInstance.bSimulatePhysics=SupportSource->BodyInstance.bSimulatePhysics;
        Floor->BodyInstance.bUseCCD=SupportSource->BodyInstance.bUseCCD;
        Floor->BodyInstance.bNotifyRigidBodyCollision=SupportSource->BodyInstance.bNotifyRigidBodyCollision;
        Floor->BodyInstance.bContactModification=SupportSource->BodyInstance.bContactModification;
        Floor->BodyInstance.SetMaskFilter(SupportSource->BodyInstance.GetMaskFilter());
        if(Floor->GetCollisionObjectType()!=SupportSource->GetCollisionObjectType())return Refuse(TEXT("SUPPORT_BOX_NAMED_PROFILE_OBJECT_TYPE_MISMATCH"));
        auto* OriginalSimpleMaterial=SupportSource->BodyInstance.GetSimplePhysicalMaterial();
        if(!Loaded(OriginalSimpleMaterial))return Refuse(TEXT("SUPPORT_BOX_ORIGINAL_SIMPLE_MATERIAL_UNAVAILABLE"));
        Pins.Emplace(OriginalSimpleMaterial);Floor->SetPhysMaterialOverride(OriginalSimpleMaterial);
        Floor->SetWorldTransform(SupportSourcePose);Floor->RegisterComponent();
        if(Floor->GetBodySetup()!=SupportSetup || !Floor->GetComponentTransform().Equals(SupportSourcePose,1.e-6)
           || Floor->GetCollisionProfileName()!=SupportSource->GetCollisionProfileName())return Refuse(TEXT("SUPPORT_BOX_REGISTERED_ORIGINAL_SETUP_POSE_POLICY"));
    }
    else
    {
    Floor->SetWorldTransform(Recipe.FixtureSupportWorldCm);Floor->SetCollisionProfileName(TEXT("BlockAll"));Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);Floor->RegisterComponent();
    }
    if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked by support registration"));
    for(int32 I=0;I<8;++I)
    {
        const auto& H=Recipe.JointHypotheses[I];auto* C=NewObject<UPhysicsConstraintComponent>(Owner,NAME_None,RF_Transient);if(!Loaded(C))return Refuse(TEXT("Fixture joint allocation failed"));
        Pins.Emplace(C);Joints[I]=C;Owner->AddInstanceComponent(C);C->SetWorldScale3D(FVector::OneVector);C->ConstraintInstance.bScaleLinearLimits=false;
        auto& CI=C->ConstraintInstance;CI.ProfileInstance.bEnableProjection=false;CI.ProfileInstance.bEnableShockPropagation=false;CI.ProfileInstance.bParentDominates=false;CI.ProfileInstance.bEnableMassConditioning=false;
        C->RegisterComponent();C->SetConstrainedComponents(Bodies[0],NAME_None,Bodies[I+1],NAME_None);
        C->SetConstraintReferenceFrame(EConstraintFrame::Frame1,DDSPhysicalCm(H.InChassisMeters));C->SetConstraintReferenceFrame(EConstraintFrame::Frame2,DDSPhysicalCm(H.InPartMeters));
        C->SetDisableCollision(H.bAdjacentCollisionExclusionJustified);C->SetLinearXLimit(LCM_Locked,0);C->SetLinearYLimit(LCM_Locked,0);
        C->SetLinearZLimit(I<6 && H.SuspensionHalfTravelM>0?LCM_Limited:LCM_Locked,H.SuspensionHalfTravelM*100);
        C->SetAngularSwing1Limit(ACM_Locked,0);C->SetAngularSwing2Limit(ACM_Locked,0);C->SetAngularTwistLimit(I<6?ACM_Free:ACM_Locked,0);
        if(I<6 && H.SuspensionHalfTravelM>0)
        {C->SetLinearPositionDrive(false,false,true);C->SetLinearVelocityDrive(false,false,true);C->SetLinearPositionTarget(FVector(0,0,H.SuspensionRestTargetM*100));C->SetLinearDriveParams(P.SuspensionK_Npm,P.SuspensionC_Nspm,P.SuspensionForceLimitN*100);}
        auto Ref=CI.GetPhysicsConstraintRef();if(!Ref.IsValid() || FPhysInterface_Chaos::IsBroken(Ref))return Refuse(TEXT("Fixture joint native handle invalid/broken"));
        bool Verified=false;FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref,[&](const FPhysicsConstraintHandle& Locked)
        {
            if(!Locked.IsValid() || !Locked.Constraint || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType))return;
            auto* J=static_cast<Chaos::FJointConstraint*>(Locked.Constraint);const auto E=J->GetPhysicsBodies();
            if(E[0]!=Bodies[0]->GetBodyInstance()->GetPhysicsActorHandle()->GetPhysicsObject() || E[1]!=Bodies[I+1]->GetBodyInstance()->GetPhysicsActorHandle()->GetPhysicsObject())return;
            J->SetJointTransforms({DDSPhysicalCm(H.InChassisMeters),DDSPhysicalCm(H.InPartMeters)});
            if(I<6 && H.SuspensionHalfTravelM>0){J->SetLinearDriveForceMode(Chaos::EJointForceMode::Force);J->SetLinearDriveStiffness(Chaos::FVec3(0,0,P.SuspensionK_Npm));J->SetLinearDriveDamping(Chaos::FVec3(0,0,P.SuspensionC_Nspm));J->SetLinearDriveMaxForce(Chaos::FVec3(0,0,P.SuspensionForceLimitN*100));}
            Verified=DDSJointSettingsMatch(J->GetJointSettings(),I,DDSPhysicalCm(H.InChassisMeters),DDSPhysicalCm(H.InPartMeters),H.SuspensionHalfTravelM*100,H.SuspensionRestTargetM*100,H.bAdjacentCollisionExclusionJustified,P);
        });
        if(!Verified || !NativeJoint(*Joints[I],*Bodies[0],*Bodies[I+1],Out.NativeCreatedJoints[I]))return Refuse(TEXT("Fixture joint physical endpoints/frames/limits/force mode readback failed"));++Out.ConstructedJointCount;
        if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked during joint construction"));
    }
    Scene->Flush();
    if(!SupportSourceUnchanged())return Refuse(TEXT("Support receipt revoked by scene flush"));
    if(Recipe.bExplicitOriginalSingleBoxSupportAdapter)
    {
        FDDSOriginalRobotSupportNativeBodyObservation SourceNow;
        if(!SupportNativeRoster(*SupportSource,SourceNow,Error) || !SupportNativeRoster(*Floor,Out.SupportAdapterObservation.FixtureNative,Error))return Refuse(Error);
        if(!SupportRostersMatch(Out.SupportAdapterObservation.SourceNative,SourceNow,true)
           || !SupportRostersMatch(SourceNow,Out.SupportAdapterObservation.FixtureNative,false))return Refuse(TEXT("SUPPORT_BOX_NATIVE_SOURCE_FIXTURE_GEOMETRY_POLICY_MISMATCH"));
        Out.SupportAdapterObservation.bNativeEquivalent=true;
    }
    Callback=Solver->CreateAndRegisterSimCallbackObject_External<FCalibrationCallback>();if(!Callback)return Refuse(TEXT("Fixture native observation callback registration failed"));
    const auto Coherent=[&]()
    {
        if(!Loaded(World) || World->bIsTearingDown || World->GetPhysicsScene()!=Scene || Scene->GetSolver()!=Solver || !Loaded(Owner) || Owner->IsActorBeingDestroyed())return false;
        for(int32 I=0;I<15;++I)if(!SourceStillExact(*Sources[I]))return false;
        for(int32 I=0;I<9;++I) {auto* C=Bodies[I];TArray<uint8> ShapeNow;if(!Loaded(C) || !C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World || C->GetStaticMesh().Get()!=Sources[I]->SuppliedMesh || !C->IsSimulatingPhysics()
            || C->GetBodySetup()!=ExpectedSetups[I] || !ConvexContent(C->GetBodySetup(),ShapeNow) || ShapeNow!=FixtureShapeBytes[I]
            || !C->GetComponentScale().Equals(Draft.UnadmittedBinding.Bodies[I].SupplierScale,1.e-6) || (I==0 && !C->DiagnosticCompoundUnchanged()))return false;}
        for(int32 I=0;I<6;++I){auto* C=Suspension[I];if(!Loaded(C) || !C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World
            || C->GetAttachParent()!=Bodies[0] || C->GetStaticMesh().Get()!=Sources[I+9]->SuppliedMesh || C->IsSimulatingPhysics() || C->GetCollisionEnabled()!=ECollisionEnabled::NoCollision
            || !C->GetComponentScale().Equals(Sources[I+9]->MeasuredWorldCm.GetScale3D(),1.e-6))return false;}
        if(Recipe.bExplicitOriginalSingleBoxSupportAdapter)
        {
            FDDSOriginalRobotSupportAdapterObservation RawNow;FDDSOriginalRobotSupportNativeBodyObservation SourceNow,FixtureNow;FString Why;
            if(!Loaded(SupportSource) || SupportSource->GetOwner()!=SupportOwner || SupportSource->GetWorld()!=SupportWorld
               || SupportSource->GetBodyInstance()!=SupportBody || SupportSource->GetStaticMesh().Get()!=SupportMesh || SupportSource->GetBodySetup()!=SupportSetup
               || SupportSetup->BodySetupGuid!=SupportGuid || !SupportSource->GetComponentTransform().Equals(SupportSourcePose,1.e-6)
               || !Loaded(Floor) || !Floor->IsRegistered() || Floor->GetOwner()!=Owner || Floor->GetWorld()!=World || Floor->GetBodySetup()!=SupportSetup || Floor->GetStaticMesh().Get()!=SupportMesh
               || !Floor->GetComponentTransform().Equals(SupportSourcePose,1.e-6) || !OriginalSingleBoxRaw(*SupportSource,RawNow,Why))
            {Out.FirstFailedPredicate=Why.IsEmpty()?TEXT("SUPPORT_BOX_SOURCE_OR_FIXTURE_CONTEXT_CHANGED"):Why;return false;}
            if(RawNow.AggregateGeometryContent!=Out.SupportAdapterObservation.AggregateGeometryContent || RawNow.SetupSettingsContent!=Out.SupportAdapterObservation.SetupSettingsContent
               || RawNow.ComponentBodySettingsContent!=Out.SupportAdapterObservation.ComponentBodySettingsContent || RawNow.ComponentCollisionProfile!=Out.SupportAdapterObservation.ComponentCollisionProfile)
            {Out.FirstFailedPredicate=TEXT("SUPPORT_BOX_RAW_SETUP_OR_COMPONENT_POLICY_CHANGED");return false;}
            if(!SupportNativeRoster(*SupportSource,SourceNow,Why) || !SupportNativeRoster(*Floor,FixtureNow,Why))
            {Out.FirstFailedPredicate=Why;return false;}
            if(!SupportRostersMatch(Out.SupportAdapterObservation.SourceNative,SourceNow,true) || !SupportRostersMatch(Out.SupportAdapterObservation.FixtureNative,FixtureNow,true)
               || !SupportRostersMatch(SourceNow,FixtureNow,false)) {Out.FirstFailedPredicate=TEXT("SUPPORT_BOX_NATIVE_IDENTITY_GEOMETRY_OR_POLICY_CHANGED");return false;}
            return true;
        }
        TArray<uint8> Now;
        return Loaded(SupportSource) && SupportSource->IsRegistered() && SupportSource->GetStaticMesh().Get()==SupportMesh && SupportSource->GetBodySetup()==SupportSetup && SupportSetup->BodySetupGuid==SupportGuid
            && SupportSource->GetComponentTransform().Equals(SupportSourcePose,1.e-6) && ConvexContent(SupportSetup,Now) && Now==SupportBytes;
    };
    Out.Steps.Reserve(Total+8);const FVector Gravity(0,0,GravityCmps2);uint64 LastSequence=0;
    FDDSOriginalRobotCalibrationStepSample PreviousSample;bool PreviousSampleAvailable=false;
    for(int32 Step=0;Step<Total;++Step)
    {
        if(!Coherent())return Refuse(TEXT("Fixture/source lifetime or preserved component collision/scale revoked"));
        auto* I=Callback->GetProducerInputData_External();I->bSetup=true;I->Forward=Recipe.DeclaredForwardInChassis;I->Profile=P;
        I->ExplicitGameLimits=Recipe.bExplicitGameAcceptanceLimits;I->SlipDomainCmps=Recipe.GameSlipDomainSpeedMps*100;
        I->SleepingAnchorCm=Recipe.GameSleepingAnchorToleranceM*100;I->SleepingSpeedCmps=Recipe.GameSleepingSpeedToleranceMps*100;
        I->SleepingOmegaRadps=Recipe.GameSleepingOmegaToleranceRadps;I->MinUpDot=Recipe.GameMinSupportUpDot;I->MinLoadedImpulseKgCmps=Recipe.GameMinLoadedImpulseNs*100;
        for(int32 B=0;B<9;++B){auto* BI=Bodies[B]->GetBodyInstance();if(!BI || !BI->IsValidBodyInstance())return Refuse(TEXT("Fixture body lost native actor"));I->Id[B]=BI->GetPhysicsActorHandle()->GetGameThreadAPI().UniqueIdx().Idx;}
        for(int32 J=0;J<8;++J){I->JointChassisCm[J]=DDSPhysicalCm(Recipe.JointHypotheses[J].InChassisMeters);I->JointPartCm[J]=DDSPhysicalCm(Recipe.JointHypotheses[J].InPartMeters);
            I->TravelCm[J]=Recipe.JointHypotheses[J].SuspensionHalfTravelM*100;I->TargetCm[J]=Recipe.JointHypotheses[J].SuspensionRestTargetM*100;I->ExcludeCollision[J]=Recipe.JointHypotheses[J].bAdjacentCollisionExclusionJustified;}
        I->PulseWheel=INDEX_NONE;I->PulseNm=0;I->TrialWheel=INDEX_NONE;
        I->TrialPhase=Step<Total-2?EDDSOriginalCalibrationTrialPhase::Settle:EDDSOriginalCalibrationTrialPhase::Drain;
        if(Step>=Recipe.SettleSteps && Step<Total-2)
        {
            const int32 Relative=Step-Recipe.SettleSteps,Within=Relative%StepsPerWheel,Cycle=Recipe.PulseStepsPerWheel+Recipe.CoastStepsPerWheel;
            I->TrialWheel=Relative/StepsPerWheel;
            if(Within<Recipe.PulseStepsPerWheel){I->TrialPhase=EDDSOriginalCalibrationTrialPhase::PositivePulse;I->PulseWheel=I->TrialWheel;I->PulseNm=Recipe.PulseTorqueNm;}
            else if(Within<Cycle)I->TrialPhase=EDDSOriginalCalibrationTrialPhase::CoastAfterPositive;
            else if(Within<Cycle+Recipe.PulseStepsPerWheel){I->TrialPhase=EDDSOriginalCalibrationTrialPhase::NegativePulse;I->PulseWheel=I->TrialWheel;I->PulseNm=-Recipe.PulseTorqueNm;}
            else I->TrialPhase=EDDSOriginalCalibrationTrialPhase::CoastAfterNegative;
        }
        // Direct native scene stepping avoids editor/gameplay/global-world ticks.
        Scene->SetUpForFrame(&Gravity,float(StepS),0,float(StepS),float(StepS),1,false);Scene->StartFrame();Scene->WaitPhysScenes();Scene->EndFrame();
        if(!Coherent())return Refuse(TEXT("Native frame callback revoked private/source world ownership"));
        while(auto Handle=Callback->PopFutureOutputData_External())
        {
            auto S=Handle->Sample;if(S.Sequence!=LastSequence+1)return Refuse(TEXT("Native callback sequence repeated, regressed or has a transport gap"));LastSequence=S.Sequence;
            if(!FMath::IsNearlyEqual(S.CallbackDtS,StepS,1.e-7) || !FMath::IsNearlyEqual(S.ConfiguredSolverAsyncDtS,StepS,1.e-7)
               || !S.ActualGravityCmps2.Equals(Gravity,1.e-4))return Refuse(TEXT("Actual native callback timestep/gravity differs from adopted120Hz/-980.665"));
            for(const auto& B:S.Bodies)if(!B.bLive || !Positive(B.MassKg) || !Rigid(B.WorldCm) || B.VelocityCmps.ContainsNaN() || B.AngularVelocityRadps.ContainsNaN()
                || B.COMLocalCm.ContainsNaN() || B.InertiaKgCm2.ContainsNaN() || !Positive(B.InertiaKgCm2.X) || !Positive(B.InertiaKgCm2.Y) || !Positive(B.InertiaKgCm2.Z)
                || !B.PrincipalAxesInBody.IsNormalized())return Refuse(TEXT("Native callback body state incomplete/nonfinite"));
            if(S.Sequence>1 && !S.bPreviousCompletedState)return Refuse(TEXT("Native completed-state/topology/timing continuity was revoked"));
            for(const auto& WheelSample:S.Wheels)if(!FiniteWheelSample(WheelSample))return Refuse(TEXT("Native wheel contact/anchor/slip response has nonfinite or invalid identities"));
            for(int32 JointIndex=0;JointIndex<8;++JointIndex)
            {FDDSOriginalRobotJointSourceObservation CurrentJoint;if(!NativeJoint(*Joints[JointIndex],*Bodies[0],*Bodies[JointIndex+1],CurrentJoint))return Refuse(TEXT("Fixture native joint disappeared/broke during stepped run"));
             S.NativeJoints[JointIndex].SourceObjectPath=CurrentJoint.SourceObjectPath;}
            if(Recipe.bExplicitGameAcceptanceLimits && S.bPreviousCompletedState)
            {
                ++Out.CompletedGameLimitSamples;
                for(auto& WheelSample:S.Wheels)
                {
                    if(WheelSample.bSolvedContactObserved)
                    {++Out.LoadedContactSamples;WheelSample.bContactWithinDeclaredGameLimits=FMath::IsFinite(WheelSample.CurrentSeparationCm)
                        && FMath::Abs(WheelSample.CurrentSeparationCm)<=Recipe.GameMaxContactSeparationM*100 && WheelSample.ContactNormal.Z>=Recipe.GameMinSupportUpDot
                        && WheelSample.RawNormalLoadImpulseKgCmps>=Recipe.GameMinLoadedImpulseNs*100;
                     if(!WheelSample.bContactWithinDeclaredGameLimits)++Out.ContactOutsideDeclaredGameLimits;}
                    if(WheelSample.bSleepingAnchorTransportObserved){++Out.SleepingAnchorSamples;if(!WheelSample.bSleepingWitnessWithinDeclaredGameLimits)++Out.SleepingAnchorOutsideDeclaredGameLimits;}
                    if(WheelSample.bSlipDefined)++Out.DefinedSlipSamples;
                }
                for(int32 LidIndex=0;LidIndex<2;++LidIndex)if(S.ClosedLidFullAngularErrorRad[LidIndex]>Recipe.GameClosedLidAngleToleranceRad
                    || S.ClosedLidRelativeOmegaMagnitudeRadps[LidIndex]>Recipe.GameClosedLidOmegaToleranceRadps)++Out.ClosedLidOutsideDeclaredGameLimits;
            }
            if(!Recipe.bExplicitBidirectionalTrials && S.bPreviousCompletedState && S.PreviousPulseWheel>=0 && S.PreviousPulseWheel<6)
            {const int32 W=S.PreviousPulseWheel;const auto& WS=S.Wheels[W];
                if(WS.bSolvedContactObserved && FMath::Abs(WS.RelativeAxleOmegaRadps)>1.e-6 && FMath::Abs(WS.AngularContactRollingForwardCmps)>1.e-4)
                {const int32 Sign=WS.AngularContactRollingForwardCmps>0?1:-1;if(Out.CandidateMotorForwardSign[W] && Out.CandidateMotorForwardSign[W]!=Sign)return Refuse(TEXT("Native pulse/rolling candidate sign inconsistent; joint/contact calibration requires repair"));Out.CandidateMotorForwardSign[W]=Sign;}}
            if(Recipe.bExplicitBidirectionalTrials && S.bPreviousCompletedState && S.PreviousTrialWheel>=0 && S.PreviousTrialWheel<6)
            {
                const int32 WheelIndex=S.PreviousTrialWheel;auto& Trial=Out.BidirectionalWheels[WheelIndex];auto& WheelSample=S.Wheels[WheelIndex];
                const bool Coast=S.PreviousTrialPhase==EDDSOriginalCalibrationTrialPhase::CoastAfterPositive || S.PreviousTrialPhase==EDDSOriginalCalibrationTrialPhase::CoastAfterNegative;
                if(Coast && S.PreviousPulseNm==0){++Trial.CoastSamples;
                    if(S.PreviousTrialPhase==EDDSOriginalCalibrationTrialPhase::CoastAfterPositive)++Trial.PositiveCoastSamples;else ++Trial.NegativeCoastSamples;}
                const bool PositivePhase=S.PreviousTrialPhase==EDDSOriginalCalibrationTrialPhase::PositivePulse && S.PreviousPulseNm>0;
                const bool NegativePhase=S.PreviousTrialPhase==EDDSOriginalCalibrationTrialPhase::NegativePulse && S.PreviousPulseNm<0;
                if(PreviousSampleAvailable && PreviousSample.bPreviousCompletedState && S.Sequence==PreviousSample.Sequence+1 && WheelSample.bSolvedContactObserved)
                {
                    const auto& PriorWheel=PreviousSample.Wheels[WheelIndex];
                    WheelSample.bConsecutiveLoadedResponseAvailable=PriorWheel.bSolvedContactObserved && PriorWheel.WheelParticleId==WheelSample.WheelParticleId
                        && PriorWheel.SupportParticleId==WheelSample.SupportParticleId && PriorWheel.WheelShapeId==WheelSample.WheelShapeId && PriorWheel.SupportShapeId==WheelSample.SupportShapeId
                        && PriorWheel.WheelGeometryId==WheelSample.WheelGeometryId && PriorWheel.SupportGeometryId==WheelSample.SupportGeometryId;
                    if(WheelSample.bConsecutiveLoadedResponseAvailable)
                    {WheelSample.AxleOmegaDeltaRadps=WheelSample.RelativeAxleOmegaRadps-PriorWheel.RelativeAxleOmegaRadps;
                     WheelSample.COMForwardVelocityDeltaCmps=WheelSample.WheelCOMForwardRelativeToSupportCmps-PriorWheel.WheelCOMForwardRelativeToSupportCmps;}
                }
                if((PositivePhase || NegativePhase) && WheelSample.bConsecutiveLoadedResponseAvailable && WheelSample.bContactWithinDeclaredGameLimits
                   && WheelSample.RawNormalLoadImpulseKgCmps>=Recipe.GameMinLoadedImpulseNs*100 && WheelSample.ContactNormal.Z>=Recipe.GameMinSupportUpDot
                   && FMath::Abs(WheelSample.AxleOmegaDeltaRadps)>=Recipe.GameMinAxleResponseRadps && WheelSample.AxleOmegaDeltaRadps*S.PreviousPulseNm>0
                   && FMath::Abs(WheelSample.AngularContactRollingForwardCmps)>=Recipe.GameMinRollingForwardMps*100
                   && FMath::Abs(WheelSample.WheelCOMForwardRelativeToSupportCmps)>=Recipe.GameMinCOMForwardMps*100
                   && WheelSample.WheelCOMForwardRelativeToSupportCmps*WheelSample.AngularContactRollingForwardCmps>0
                   && WheelSample.COMForwardVelocityDeltaCmps*WheelSample.AngularContactRollingForwardCmps>0)
                {
                    const int32 Sign=(WheelSample.AngularContactRollingForwardCmps>0?1:-1)*(S.PreviousPulseNm>0?1:-1);
                    if(Trial.CoherentForwardSignCandidate && Trial.CoherentForwardSignCandidate!=Sign)Trial.bContradictoryResponseObserved=true;
                    else Trial.CoherentForwardSignCandidate=Sign;
                    if(PositivePhase)++Trial.PositiveLoadedResponseSamples;else ++Trial.NegativeLoadedResponseSamples;
                }
            }
            for(const auto& WheelSample:S.Wheels)if(!FiniteWheelSample(WheelSample))return Refuse(TEXT("Derived native loaded response is nonfinite"));
            if(Recipe.bExplicitOriginalSingleBoxSupportAdapter && !Coherent())return Refuse(TEXT("SUPPORT_BOX_OUTPUT_POLL_COHERENCE_REVOKED"));
            PreviousSample=S;PreviousSampleAvailable=true;Out.Steps.Add(S);
        }
        FDDSOriginalRobotNativeBodyObservation PostFrameOwner;if(!ObserveBody(*Bodies[0],P.BareChassisKg,PostFrameOwner,Error))return false;
        if(!Coherent())return Refuse(TEXT("Native post-EndFrame owner readback revoked fixture/source coherence"));
        FDDSOriginalRobotSuspensionMotionSample Motion;Motion.NativeFrameIndex=Step;Motion.ObservedCallbackSequenceThrough=LastSequence;
        Motion.ChassisParticleId=Bodies[0]->GetBodyInstance()->GetPhysicsActorHandle()->GetGameThreadAPI().UniqueIdx().Idx;
        Motion.NativeChassisWorldCm=PostFrameOwner.NativeActorWorldCm;Motion.ComponentChassisWorldCm=DDSRigidComponentPose(Bodies[0]->GetComponentTransform());
        if(!Motion.NativeChassisWorldCm.Equals(Motion.ComponentChassisWorldCm,1.e-4))return Refuse(TEXT("Post-EndFrame native/GT owner pose is incoherent for suspension motion readback"));
        for(int32 S=0;S<6;++S)
        {
            const FTransform Expected=DDSRigidComponentPose(Sources[S+9]->MeasuredWorldCm).GetRelativeTransform(DDSRigidComponentPose(Sources[0]->MeasuredWorldCm));
            const FTransform Current=DDSRigidComponentPose(Suspension[S]->GetComponentTransform()).GetRelativeTransform(DDSRigidComponentPose(Bodies[0]->GetComponentTransform()));
            Out.ObservedMaximumSuspensionRelativePositionErrorM[S]=FMath::Max(Out.ObservedMaximumSuspensionRelativePositionErrorM[S],(Current.GetLocation()-Expected.GetLocation()).Size()*0.01);
            Out.ObservedMaximumSuspensionRelativeAngleErrorRad[S]=FMath::Max(Out.ObservedMaximumSuspensionRelativeAngleErrorRad[S],Current.GetRotation().AngularDistance(Expected.GetRotation()));
            Motion.ComponentWorldCm[S]=Suspension[S]->GetComponentTransform();Motion.RelativeToOwnerMeters[S]=Meters(Current);
        }
        if(!Coherent())return Refuse(TEXT("Suspension post-EndFrame coherent readback revoked"));Out.PostEndFrameSuspensionMotion.Add(Motion);
    }
    if(Out.Steps.Num()<2 || !Out.Steps.Last().bPreviousCompletedState)return Refuse(TEXT("No continuous completed native-step observations were produced"));
    if(Recipe.bExplicitOriginalSingleBoxSupportAdapter && !Coherent())return Refuse(TEXT("SUPPORT_BOX_FINAL_OUTPUT_COHERENCE_REVOKED"));
    Out.bBoundedRunCompleted=true;
    if(Recipe.bExplicitBidirectionalTrials)for(int32 WheelIndex=0;WheelIndex<6;++WheelIndex)
    {auto& Trial=Out.BidirectionalWheels[WheelIndex];Trial.bBothLoadedDirectionsObserved=!Trial.bContradictoryResponseObserved
        && Trial.PositiveLoadedResponseSamples>=Recipe.GameMinLoadedSamplesPerDirection && Trial.NegativeLoadedResponseSamples>=Recipe.GameMinLoadedSamplesPerDirection
        && Trial.PositiveCoastSamples>=2 && Trial.NegativeCoastSamples>=2;
     if(Trial.bBothLoadedDirectionsObserved)Out.CandidateMotorForwardSign[WheelIndex]=Trial.CoherentForwardSignCandidate;
     else Out.RemainingEvidence.Add(FString::Printf(TEXT("wheel%d bidirectional loaded axle/rolling/COM response missing or contradictory; positive=%d negative=%d coast=%d"),WheelIndex,Trial.PositiveLoadedResponseSamples,Trial.NegativeLoadedResponseSamples,Trial.CoastSamples));}
    for(int32 W=0;W<6;++W)if(Out.CandidateMotorForwardSign[W]==0)Out.RemainingEvidence.Add(FString::Printf(TEXT("wheel%d candidate sign absent: no sufficient actual loaded pulse/rolling response"),W));
    Out.RemainingEvidence.Add(TEXT("This hidden original-collision GAME fixture uses explicit candidate joints and mesh DefaultInstance/contact hypotheses; it does not prove supplier suspension kinematics or production materials"));
    Out.RemainingEvidence.Add(TEXT("Actual native GAME connector settings/constituent provenance/post-EndFrame rigid visual motion are retained. Four explicit registered GAME mounts still require caller creation and CaptureDraft inputs; declared included mass shares remain separate from observed owner mass"));
    Out.RemainingEvidence.Add(TEXT("Freeze contact/slip/sleep/closed-lid budgets only after prescribed native support/drive/brake/reverse/turn/curb coverage and six complete pulse/loaded-rolling observations"));
    Out.RemainingEvidence.Add(TEXT("No BuildVerifiedBinding, measurement hashes, certificate, topology/mount flags, N12/service freeze or production readiness is emitted"));
    return true;
}

bool FDDSOriginalRobotMeasurementProducer::ConfirmOriginalSupportDiagnosticReceipt(
    UStaticMeshComponent& Actual,const FDDSOriginalRobotSupportAdapterObservation& Expected,FString& Error)
{
    using namespace DDSOriginalMeasurement;
    FDDSOriginalRobotSupportAdapterObservation Current;
    if(!Expected.bRawSourceAvailable || !OriginalSingleBoxRaw(Actual,Current,Error))return false;
    if(Current.SourceComponentPath!=Expected.SourceComponentPath || Current.SourceOwnerPath!=Expected.SourceOwnerPath
       || Current.SourceWorldPath!=Expected.SourceWorldPath || Current.OriginalMeshPath!=Expected.OriginalMeshPath
       || Current.OriginalSetupPath!=Expected.OriginalSetupPath || Current.OriginalSetupGuid!=Expected.OriginalSetupGuid
       || !Current.OriginalComponentWorldCm.Equals(Expected.OriginalComponentWorldCm,1.e-6)
       || Current.AggregateGeometryContent!=Expected.AggregateGeometryContent || Current.SetupSettingsContent!=Expected.SetupSettingsContent
       || Current.ComponentBodySettingsContent!=Expected.ComponentBodySettingsContent || Current.ComponentCollisionProfile!=Expected.ComponentCollisionProfile)
    {Error=TEXT("SUPPORT_BOX_CALLER_WRITE_SOURCE_RECEIPT_REVOKED");return false;}
    if(Expected.SourceNative.bAvailable)
    {
        FDDSOriginalRobotSupportNativeBodyObservation Native;
        if(!SupportNativeRoster(Actual,Native,Error))return false;
        if(!SupportRostersMatch(Expected.SourceNative,Native,true))
        {Error=TEXT("SUPPORT_BOX_CALLER_WRITE_NATIVE_RECEIPT_REVOKED");return false;}
    }
    return true; // Read-only diagnostic coherence, never binding/readiness.
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
// Each deliberately malformed input is an owned transient duplicate. These
// validate refusal domains only; they do not prove City contacts/calibration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSOriginalSingleBoxSupportDomainTest,
    "DDSCity.M04.OriginalSingleBoxSupportDomain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSOriginalSingleBoxSupportDomainTest::RunTest(const FString& Parameters)
{
    using namespace DDSOriginalMeasurement;
    auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    if(!TestNotNull(TEXT("Existing Engine Cube is a read-only test source"),Cube))return false;
    auto* OriginalSetup=Cube->GetBodySetup();
    if(!TestNotNull(TEXT("Existing source setup"),OriginalSetup))return false;
    if(!TestTrue(TEXT("Existing test source has exactly one authored simple box"),OriginalSetup->AggGeom.BoxElems.Num()==1 && OriginalSetup->AggGeom.GetElementCount()==1))return false;
    TArray<uint8> OriginalGeometry,OriginalSettings;
    if(!TestTrue(TEXT("Original raw receipts available"),SetupReceiptBytes(OriginalSetup,OriginalGeometry,OriginalSettings)))return false;
    const auto IV=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,MakeUniqueObjectName(GetTransientPackage(),UWorld::StaticClass(),TEXT("DDSOriginalSupportDomainTest")),GetTransientPackage(),true,ERHIFeatureLevel::Num,&IV);
    if(!TestNotNull(TEXT("Owned private test world"),World))return false;
    TStrongObjectPtr<UWorld> WorldPin(World);ON_SCOPE_EXIT {World->DestroyWorld(false);};
    const TCHAR* Cases[]={TEXT("positive_original_box"),TEXT("component_rotation"),TEXT("local_box_rotation"),TEXT("negative_component_scale"),TEXT("negative_build_scale"),TEXT("multiple_simple_boxes"),TEXT("complex_as_simple"),TEXT("welded_input"),TEXT("dynamic_input"),TEXT("query_only"),TEXT("missing_setup")};
    for(int32 Case=0;Case<UE_ARRAY_COUNT(Cases);++Case)
    {
        FActorSpawnParameters Spawn;Spawn.ObjectFlags=RF_Transient;
        auto* Owner=World->SpawnActor<AActor>(AActor::StaticClass(),FTransform::Identity,Spawn);
        if(!TestNotNull(Cases[Case],Owner))return false;
        TStrongObjectPtr<AActor> OwnerPin(Owner);
        auto* Mesh=DuplicateObject<UStaticMesh>(Cube,Owner,MakeUniqueObjectName(Owner,UStaticMesh::StaticClass(),TEXT("OwnedMalformedSupportMesh")));
        if(!TestNotNull(TEXT("Owned source duplicate"),Mesh))return false;
        TStrongObjectPtr<UStaticMesh> MeshPin(Mesh);UStaticMesh* Meshes[]={Mesh};FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
        auto* Setup=Mesh->GetBodySetup();
        if(!TestTrue(TEXT("Only private duplicated setup may be mutated"),Setup && Setup!=OriginalSetup && Setup->GetOuter()==Mesh))return false;
        auto* C=NewObject<UStaticMeshComponent>(Owner,NAME_None,RF_Transient);
        if(!TestNotNull(TEXT("Owned diagnostic component"),C))return false;
        Owner->AddInstanceComponent(C);Owner->SetRootComponent(C);C->SetMobility(EComponentMobility::Static);
        if(!TestTrue(TEXT("Copied existing mesh installed on owned component"),C->SetStaticMesh(Mesh)))return false;
        C->SetCollisionProfileName(TEXT("BlockAll"));C->BodyInstance.bAutoWeld=false;
        FTransform Pose(FQuat::Identity,FVector::ZeroVector,FVector(14000,14000,2));
        if(Case==1)Pose.SetRotation(FQuat(FRotator(0,1,0)));
        if(Case==2)Setup->AggGeom.BoxElems[0].Rotation=FRotator(0,1,0);
        if(Case==3)Pose.SetScale3D(FVector(-14000,14000,2));
        if(Case==4)Setup->BuildScale3D=FVector(-1,1,1);
        if(Case==5){const FKBoxElem ExtraBox=Setup->AggGeom.BoxElems[0];Setup->AggGeom.BoxElems.Add(ExtraBox);}
        if(Case==6)Setup->CollisionTraceFlag=CTF_UseComplexAsSimple;
        if(Case==8)C->SetMobility(EComponentMobility::Movable);
        if(Case==9)C->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        if(Case==10)C->SetStaticMesh(nullptr); // Registered malformed source must refuse without a setup dereference.
        C->SetWorldTransform(Pose);C->RegisterComponent();
        FBodyInstance DeclaredOwnedWeldParent;
        if(Case==7)C->BodyInstance.WeldParent=&DeclaredOwnedWeldParent;
        FDDSOriginalRobotSupportAdapterObservation O;FString Error;
        const bool Accepted=OriginalSingleBoxRaw(*C,O,Error);
        C->BodyInstance.WeldParent=nullptr; // Invalid diagnostic input only; no native welding claim.
        if(Case==0)TestTrue(TEXT("Exact single-axis-aligned positive nonuniform box raw domain accepted"),Accepted);
        else {TestFalse(FString::Printf(TEXT("%s refused"),Cases[Case]),Accepted);TestFalse(TEXT("Refusal reason retained"),Error.IsEmpty());}
        C->DestroyComponent();Owner->Destroy();
    }
    TArray<uint8> AfterGeometry,AfterSettings;
    TestTrue(TEXT("Read-only original Engine setup remains exact"),SetupReceiptBytes(OriginalSetup,AfterGeometry,AfterSettings) && AfterGeometry==OriginalGeometry && AfterSettings==OriginalSettings);
    return true;
}
#endif
