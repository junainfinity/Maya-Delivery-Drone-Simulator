#include "MAYANativeDemoPawn.h"
#include "DDSStaticMeshNaniteObservation.h"
#include "DDSRobotJointAdmission.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Chaos/ShapeInstance.h"
#include "Chaos/Convex.h"
#include "Chaos/Box.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/PBDJointConstraintData.h"
#include "StaticMeshResources.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/Class.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

namespace MAYACargoGeometry
{
constexpr int32 MaxRobotShapes = 256, MaxSceneEntries = 256, MaxSceneShapes = 1024;
constexpr int32 MaxWrapperDepth = 8, MaxVertices = 200000, MaxTriangles = 400000;
constexpr int64 MaxResponseBytes = 32 * 1024 * 1024;
constexpr double StopAfterReturnTargetS = 2.0;
// These are bounded query ceilings only. They do not define or certify a motion domain.
constexpr double MaxEnvelopeSideCm = 1000.0, MaxEnvelopeCenterDistanceCm = 100.0;
const TCHAR* Roles[] = { TEXT("SM_DeliveryRobot_Base"), TEXT("SM_DeliveryRobot_Wheel"),
    TEXT("SM_DeliveryRobot_Wheel1"), TEXT("SM_DeliveryRobot_Wheel2"), TEXT("SM_DeliveryRobot_Wheel3"),
    TEXT("SM_DeliveryRobot_Wheel4"), TEXT("SM_DeliveryRobot_Wheel5"), TEXT("SM_DeliveryRobot_Door1"),
    TEXT("SM_DeliveryRobot_Door2"), TEXT("SM_DeliveryRobot_Suspension"), TEXT("SM_DeliveryRobot_Suspension1"),
    TEXT("SM_DeliveryRobot_Suspension2"), TEXT("SM_DeliveryRobot_Suspension3"), TEXT("SM_DeliveryRobot_Suspension4"),
    TEXT("SM_DeliveryRobot_Suspension5") };
bool Loaded(const UObject* O)
{ return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_BeginDestroyed | RF_FinishDestroyed); }
bool Rigid(const FTransform& T)
{ return !T.ContainsNaN() && T.GetRotation().IsNormalized() && T.GetScale3D().Equals(FVector::OneVector, 1.e-6); }
FString Id(const void* P) { return FString::Printf(TEXT("%llx"), uint64(reinterpret_cast<UPTRINT>(P))); }
FString Hash(const TArray<uint8>& Bytes)
{ return FSHA1::HashBuffer(Bytes.GetData(), uint64(Bytes.Num())).ToString(); }
TSharedRef<FJsonObject> PoseJson(const FTransform& T)
{
    auto O = MakeShared<FJsonObject>(); const FVector P = T.GetLocation(), S = T.GetScale3D(); const FQuat Q = T.GetRotation();
    O->SetNumberField(TEXT("xCm"), P.X); O->SetNumberField(TEXT("yCm"), P.Y); O->SetNumberField(TEXT("zCm"), P.Z);
    O->SetNumberField(TEXT("qx"), Q.X); O->SetNumberField(TEXT("qy"), Q.Y); O->SetNumberField(TEXT("qz"), Q.Z); O->SetNumberField(TEXT("qw"), Q.W);
    O->SetNumberField(TEXT("sx"), S.X); O->SetNumberField(TEXT("sy"), S.Y); O->SetNumberField(TEXT("sz"), S.Z); return O;
}
TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& V)
{ return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) }; }
void BoxJson(TSharedRef<FJsonObject> O, const TCHAR* Name, const FBox& B)
{ auto R = MakeShared<FJsonObject>(); R->SetArrayField(TEXT("min"), VectorJson(B.Min)); R->SetArrayField(TEXT("max"), VectorJson(B.Max)); O->SetObjectField(Name, R); }
bool ValidBox(const FBox& B)
{ return B.IsValid && !B.Min.ContainsNaN() && !B.Max.ContainsNaN() && B.Min.X <= B.Max.X && B.Min.Y <= B.Max.Y && B.Min.Z <= B.Max.Z; }
struct FWrapper { FString Kind; FTransform Pose = FTransform::Identity; FVector Scale = FVector::OneVector; double Margin = 0; };
struct FPlane { FVector Normal, Point; TArray<int32> Indices; };
struct FGeometry
{
    FString Kind; TArray<FVector> Vertices; TArray<int32> Indices; TArray<FPlane> Planes;
    TArray<FWrapper> Wrappers; FBox BoundsActor = FBox(ForceInit); double Margin = 0;
};
struct FShape
{
    FString ShapeIdentity, GeometryIdentity, OriginalBodyIdentity; int32 Role = INDEX_NONE;
    bool Simulation = false, Query = false, Probe = false, Welded = false;
    FTransform LocalPose = FTransform::Identity, WeldPose = FTransform::Identity;
    FBox InflatedWorld = FBox(ForceInit); FGeometry Geometry;
};
struct FBody { FString ActorIdentity, ComponentPath, OwnerPath; int32 PhysicalIndex = INDEX_NONE; FTransform ActorWorld = FTransform::Identity; TArray<FShape> Shapes; };
struct FJoint { int32 Index = 0; FTransform Frames[2]; FVector AngularLimits; double LinearLimit = 0; int32 LinearMotion[3] = {}, AngularMotion[3] = {}; bool ClosedLock = false; };
struct FSceneGuard { UPrimitiveComponent* Component = nullptr; FString ComponentPath, ClassPath, OwnerPath; FTransform Pose; ECollisionEnabled::Type Collision; UStaticMesh* Mesh = nullptr; UBodySetup* Setup = nullptr; FGuid SetupGuid; };
struct FLOD { int32 Index = 0; TArray<FVector> Vertices; TArray<int32> Indices; FString Digest; };
struct FMesh
{
    FString ComponentPath, ParentPath, MeshPath, PackagePath, PackageFile, PackageSha1, SetupPath, SetupGuid;
    FTransform Relative = FTransform::Identity, World = FTransform::Identity; int32 Role = INDEX_NONE, Instance = INDEX_NONE;
    bool GeometryKnown = false, DeformationKnown = false; TArray<FLOD> LODs;
};
struct FBudget
{
    double Started; int32 Vertices = 0, Triangles = 0, RobotShapes = 0, SceneShapes = 0; FString Error;
    bool Time() { if (FPlatformTime::Seconds() - Started <= StopAfterReturnTargetS) return true; Error = TEXT("PROVIDER_STOP_AFTER_RETURN_TARGET_EXCEEDED"); return false; }
    bool Add(int32 V, int32 T)
    {
        if (V < 0 || T < 0 || V > MaxVertices - Vertices || T > MaxTriangles - Triangles)
        { Error = TEXT("GEOMETRY_VERTEX_OR_TRIANGLE_CAP"); return false; }
        Vertices += V; Triangles += T; return Time();
    }
};
// Retain exact outer-to-inner decorators. Applying them in reverse order preserves
// nonuniform-scale/rotation composition without collapsing a possible shear.
bool Decode(const Chaos::FImplicitObject* G, FGeometry& O, FBudget& B, int32 Depth, bool AllowTriangleMesh)
{
    if (!G || !G->HasBoundingBox() || Depth > MaxWrapperDepth || !B.Time())
    { if (B.Error.IsEmpty()) B.Error = TEXT("UNKNOWN_GEOMETRY_OR_WRAPPER_DEPTH"); return false; }
    const auto Type = G->GetType();
    if (Type == Chaos::ImplicitObjectType::Transformed)
    {
        const auto* W = G->GetObject<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>();
        if (!W) return false; FWrapper R; R.Kind = TEXT("Transformed"); R.Pose = FTransform(W->GetTransform());
        // UE5.3's derived transformed GetMargin instantiates an unavailable base GetRadius.
        // Observe the decorator's actual stored FImplicitObject margin through G instead.
        // The inner shape's own margin is retained by recursive Decode; quadratic shapes
        // remain unsupported, so no radius or quadratic-margin inference is required.
        R.Margin = G->GetMargin();
        if (!Rigid(R.Pose) || !FMath::IsFinite(R.Margin) || R.Margin < 0) return false;
        O.Wrappers.Add(R); return Decode(W->GetTransformedObject(), O, B, Depth + 1, AllowTriangleMesh);
    }
    if (const auto* W = G->GetObject<Chaos::TImplicitObjectScaled<Chaos::FConvex>>())
    {
        FWrapper R; R.Kind = TEXT("Scaled"); R.Scale = FVector(W->GetScale()); R.Margin = W->GetMargin();
        if (R.Scale.ContainsNaN() || R.Scale.GetMin() <= 0 || !FMath::IsFinite(R.Margin) || R.Margin < 0) return false;
        O.Wrappers.Add(R); return Decode(W->GetUnscaledObject(), O, B, Depth + 1, AllowTriangleMesh);
    }
    if (const auto* W = G->GetObject<Chaos::TImplicitObjectInstanced<Chaos::FConvex>>())
    {
        FWrapper R; R.Kind = TEXT("Instanced"); R.Margin = W->GetMargin();
        if (!FMath::IsFinite(R.Margin) || R.Margin < 0) return false; O.Wrappers.Add(R);
        return Decode(W->GetInstancedObject(), O, B, Depth + 1, AllowTriangleMesh);
    }
    if (AllowTriangleMesh)
    {
        if (const auto* W = G->GetObject<Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>>())
        {
            FWrapper R; R.Kind = TEXT("Scaled"); R.Scale = FVector(W->GetScale()); R.Margin = W->GetMargin();
            if (R.Scale.ContainsNaN() || R.Scale.GetMin() <= 0 || !FMath::IsFinite(R.Margin) || R.Margin < 0) return false;
            O.Wrappers.Add(R); return Decode(W->GetUnscaledObject(), O, B, Depth + 1, true);
        }
        if (const auto* W = G->GetObject<Chaos::TImplicitObjectInstanced<Chaos::FTriangleMeshImplicitObject>>())
        {
            FWrapper R; R.Kind = TEXT("Instanced"); R.Margin = W->GetMargin();
            if (!FMath::IsFinite(R.Margin) || R.Margin < 0) return false; O.Wrappers.Add(R);
            return Decode(W->GetInstancedObject(), O, B, Depth + 1, true);
        }
    }
    if (Type == Chaos::ImplicitObjectType::Convex)
    {
        const auto* C = G->GetObject<Chaos::FConvex>(); if (!C) return false;
        const int32 V = C->NumVertices(), P = C->NumPlanes();
        if (V < 4 || P < 4 || V > 4096 || P > 4096 || !C->GetStructureData().IsValid()) return false;
        int32 T = 0;
        for (int32 F = 0; F < P; ++F) { const int32 N = C->NumPlaneVertices(F); if (N < 3 || N > V || T > MaxTriangles - (N - 2)) return false; T += N - 2; }
        if (!B.Add(V, T)) return false; O.Kind = TEXT("Convex"); O.Margin = C->GetMargin();
        for (int32 I = 0; I < V; ++I) { const FVector X(C->GetVertex(I)); if (X.ContainsNaN()) return false; O.Vertices.Add(X); }
        for (int32 F = 0; F < P; ++F)
        {
            FPlane R; Chaos::FVec3 N, X; C->GetPlaneNX(F, N, X); R.Normal = FVector(N); R.Point = FVector(X);
            if (R.Normal.ContainsNaN() || R.Point.ContainsNaN() || !R.Normal.IsNormalized()) return false;
            for (int32 I = 0; I < C->NumPlaneVertices(F); ++I) { const int32 K = C->GetPlaneVertex(F, I); if (K < 0 || K >= V) return false; R.Indices.Add(K); }
            O.Planes.Add(MoveTemp(R));
        }
    }
    else if (Type == Chaos::ImplicitObjectType::Box)
    {
        const auto* C = G->GetObject<Chaos::TBox<Chaos::FReal, 3>>(); if (!C || !B.Add(8, 12)) return false;
        const FVector Min(C->Min()), Max(C->Max()); if (Min.ContainsNaN() || Max.ContainsNaN() || (Max - Min).GetMin() <= 0) return false;
        O.Kind = TEXT("Box"); O.Margin = C->GetMargin();
        for (int32 I = 0; I < 8; ++I) O.Vertices.Add(FVector(I & 1 ? Max.X : Min.X, I & 2 ? Max.Y : Min.Y, I & 4 ? Max.Z : Min.Z));
        const int32 Faces[][4] = { {0,4,6,2}, {1,3,7,5}, {0,1,5,4}, {2,6,7,3}, {0,2,3,1}, {4,5,7,6} };
        const FVector Normals[] = { FVector(-1,0,0), FVector(1,0,0), FVector(0,-1,0), FVector(0,1,0), FVector(0,0,-1), FVector(0,0,1) };
        for (int32 F = 0; F < 6; ++F) { FPlane R; R.Normal = Normals[F]; R.Point = O.Vertices[Faces[F][0]]; for (int32 K : Faces[F]) R.Indices.Add(K); O.Planes.Add(MoveTemp(R)); }
    }
    else if (Type == Chaos::ImplicitObjectType::TriangleMesh && AllowTriangleMesh)
    {
        const auto* C = G->GetObject<Chaos::FTriangleMeshImplicitObject>(); if (!C) return false;
        const int32 V = C->Particles().Size(), T = C->Elements().GetNumTriangles();
        if (V < 3 || T < 1 || !B.Add(V, T)) return false; O.Kind = TEXT("TriangleMesh"); O.Margin = C->GetMargin();
        for (int32 I = 0; I < V; ++I) { const FVector X(C->Particles().X(I)); if (X.ContainsNaN()) return false; O.Vertices.Add(X); }
        const auto Copy = [&](const auto& Indices)
        { for (int32 F = 0; F < Indices.Num(); ++F) for (int32 A = 0; A < 3; ++A) { const int32 K = int32(Indices[F][A]); if (K < 0 || K >= V) return false; O.Indices.Add(K); } return true; };
        if (C->Elements().RequiresLargeIndices()) { if (!Copy(C->Elements().GetLargeIndexBuffer())) return false; }
        else if (!Copy(C->Elements().GetSmallIndexBuffer())) return false;
    }
    else { B.Error = TEXT("UNSUPPORTED_ACTUAL_IMPLICIT_TYPE_OR_WRAPPER"); return false; }
    return FMath::IsFinite(O.Margin) && O.Margin >= 0 && B.Time();
}
FVector InActor(const FGeometry& G, FVector V)
{ for (int32 I = G.Wrappers.Num() - 1; I >= 0; --I) { const auto& W = G.Wrappers[I]; if (W.Kind == TEXT("Transformed")) V = W.Pose.TransformPosition(V); else if (W.Kind == TEXT("Scaled")) V *= W.Scale; } return V; }
bool CaptureShape(const FPhysicsShapeHandle& S, FShape& O, FBudget& B, bool SceneShape)
{
    if (!S.IsValid() || !S.Shape) return false; const auto* G = S.Shape->GetGeometry().Get(); if (!G || !G->HasBoundingBox()) return false;
    if (SceneShape) { if (++B.SceneShapes > MaxSceneShapes) { B.Error = TEXT("SCENE_SHAPE_CAP"); return false; } }
    else if (++B.RobotShapes > MaxRobotShapes) { B.Error = TEXT("ROBOT_SHAPE_CAP"); return false; }
    O.ShapeIdentity = Id(S.Shape); O.GeometryIdentity = Id(G); O.Simulation = S.Shape->GetSimEnabled();
    O.Query = S.Shape->GetQueryEnabled(); O.Probe = S.Shape->GetIsProbe(); O.LocalPose = FPhysInterface_Chaos::GetLocalTransform(S);
    const auto Native = G->BoundingBox(), Inflated = S.Shape->GetWorldSpaceInflatedShapeBounds();
    O.Geometry.BoundsActor = FBox(FVector(Native.Min()), FVector(Native.Max()));
    O.InflatedWorld = FBox(FVector(Inflated.Min()), FVector(Inflated.Max()));
    if (!Rigid(O.LocalPose) || !ValidBox(O.Geometry.BoundsActor) || !ValidBox(O.InflatedWorld) || !Decode(G, O.Geometry, B, 0, SceneShape)) return false;
    // Native local-shape and welded transforms are metadata only: the actual
    // implicit decorator chain already embeds them. Verify every decoded vertex.
    const FBox Expanded = O.Geometry.BoundsActor.ExpandBy(1.e-4);
    for (const FVector& V : O.Geometry.Vertices) { const FVector X = InActor(O.Geometry, V); if (X.ContainsNaN() || !Expanded.IsInsideOrOn(X)) { B.Error = TEXT("ACTUAL_WRAPPER_COMPOSITION_BOUNDS_MISMATCH"); return false; } }
    return true;
}
bool PackageDigest(UStaticMesh& M, FString& File, FString& Digest, FBudget& B)
{
    auto* P = M.GetOutermost(); if (!Loaded(P) || P->IsDirty() || !FPackageName::DoesPackageExist(P->GetName(), &File)) return false;
    TUniquePtr<FArchive> A(IFileManager::Get().CreateFileReader(*File)); if (!A || A->TotalSize() < 1 || A->TotalSize() > MaxResponseBytes) return false;
    const int64 Size = A->TotalSize(); FSHA1 H; TArray<uint8> Chunk; Chunk.SetNumUninitialized(65536);
    for (int64 At = 0; At < Size; At += Chunk.Num()) { if (!B.Time()) return false; const int64 N = FMath::Min<int64>(Chunk.Num(), Size - At); A->Serialize(Chunk.GetData(), N); if (A->IsError()) return false; H.Update(Chunk.GetData(), uint32(N)); }
    H.Final(); FSHAHash Result; H.GetHash(Result.Hash); Digest = Result.ToString();
    return !A->IsError() && IFileManager::Get().FileSize(*File) == Size && B.Time();
}
bool CaptureMesh(UStaticMeshComponent& C, FMesh& O, FBudget& B)
{
    auto* M = C.GetStaticMesh().Get(); if (!Loaded(&C) || !C.IsRegistered() || !Loaded(M) || M->IsCompiling() || (!DDSStaticMeshNanite::ObservationKnown(*M) || DDSStaticMeshNanite::BuildEnabledOrCookedData(*M))) return false;
    O.ComponentPath = C.GetPathName(); O.ParentPath = C.GetAttachParent() ? C.GetAttachParent()->GetPathName() : TEXT("");
    O.MeshPath = M->GetPathName(); O.PackagePath = M->GetOutermost()->GetName(); O.Relative = C.GetRelativeTransform(); O.World = C.GetComponentTransform();
    if (!Rigid(O.Relative) || !Rigid(O.World)) return false;
    auto* Setup = C.GetBodySetup(); if (Setup) { O.SetupPath = Setup->GetPathName(); O.SetupGuid = Setup->BodySetupGuid.ToString(); }
    const auto* Data = M->GetRenderData(); if (!Data || Data->LODResources.Num() < 1 || Data->LODResources.Num() > 8) return false;
    for (int32 L = 0; L < Data->LODResources.Num(); ++L)
    {
        const auto& Resource = Data->LODResources[L]; const auto& P = Resource.VertexBuffers.PositionVertexBuffer; const auto& I = Resource.IndexBuffer;
        const int32 V = int32(P.GetNumVertices()), N = I.GetNumIndices();
        // Count is cached after discarded buffers; require actual CPU storage too.
        if (V < 3 || N < 3 || N % 3 || P.GetVertexData() == nullptr || P.GetStride() < sizeof(FVector3f) || P.GetAllocatedSize() < SIZE_T(V) * P.GetStride()
            || I.GetIndexDataSize() < 1 || !B.Add(V, N / 3)) return false;
        FLOD R; R.Index = L; TArray<uint8> Bytes; FMemoryWriter Writer(Bytes, true);
        Writer << R.Index; int32 VertexCount = V, IndexCount = N; Writer << VertexCount << IndexCount;
        for (int32 K = 0; K < V; ++K) { FVector X(P.VertexPosition(uint32(K))); if (X.ContainsNaN()) return false; R.Vertices.Add(X); double PX = X.X, PY = X.Y, PZ = X.Z; Writer << PX << PY << PZ; }
        const FIndexArrayView View = I.GetArrayView(); if (View.Num() != N) return false;
        for (int32 K = 0; K < N; ++K) { int32 X = int32(View[uint32(K)]); if (X < 0 || X >= V) return false; R.Indices.Add(X); Writer << X; }
        if (Writer.IsError() || Bytes.Num() > MaxResponseBytes) return false; R.Digest = Hash(Bytes); O.LODs.Add(MoveTemp(R));
    }
    O.GeometryKnown = true; O.DeformationKnown = true;
    for (int32 I = 0; I < C.GetNumMaterials(); ++I)
    {
        const auto* Material = C.GetMaterial(I); const auto* Resource = Material ? Material->GetMaterialResource(C.GetWorld()->GetFeatureLevel()) : nullptr;
        if (!Resource || !Resource->GetGameThreadShaderMap() || Resource->MaterialModifiesMeshPosition_GameThread() || Resource->MaterialUsesWorldPositionOffset_GameThread()) O.DeformationKnown = false;
    }
    return PackageDigest(*M, O.PackageFile, O.PackageSha1, B);
}
TSharedRef<FJsonObject> GeometryJson(const FGeometry& G)
{
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("kind"), G.Kind); O->SetNumberField(TEXT("marginCm"), G.Margin); BoxJson(O, TEXT("nativeBoundsActorCm"), G.BoundsActor);
    TArray<TSharedPtr<FJsonValue>> V, I, P, W;
    for (const FVector& X : G.Vertices) V.Add(MakeShared<FJsonValueArray>(VectorJson(X)));
    for (int32 X : G.Indices) I.Add(MakeShared<FJsonValueNumber>(X));
    for (const auto& X : G.Planes) { auto R = MakeShared<FJsonObject>(); R->SetArrayField(TEXT("normal"), VectorJson(X.Normal)); R->SetArrayField(TEXT("point"), VectorJson(X.Point)); TArray<TSharedPtr<FJsonValue>> K; for (int32 A : X.Indices) K.Add(MakeShared<FJsonValueNumber>(A)); R->SetArrayField(TEXT("indices"), K); P.Add(MakeShared<FJsonValueObject>(R)); }
    for (const auto& X : G.Wrappers) { auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("kind"), X.Kind); R->SetObjectField(TEXT("pose"), PoseJson(X.Pose)); R->SetArrayField(TEXT("scale"), VectorJson(X.Scale)); R->SetNumberField(TEXT("marginCm"), X.Margin); W.Add(MakeShared<FJsonValueObject>(R)); }
    O->SetArrayField(TEXT("verticesCm"), V); O->SetArrayField(TEXT("indices"), I); O->SetArrayField(TEXT("planes"), P); O->SetArrayField(TEXT("wrappers"), W); return O;
}
TSharedRef<FJsonObject> BodyJson(const FBody& B)
{
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("actorIdentity"), B.ActorIdentity); O->SetStringField(TEXT("componentPath"), B.ComponentPath); O->SetStringField(TEXT("ownerPath"), B.OwnerPath); O->SetNumberField(TEXT("physicalIndex"), B.PhysicalIndex); O->SetObjectField(TEXT("actorWorldPose"), PoseJson(B.ActorWorld)); TArray<TSharedPtr<FJsonValue>> Shapes;
    for (const auto& S : B.Shapes) { auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("shapeIdentity"), S.ShapeIdentity); R->SetStringField(TEXT("geometryIdentity"), S.GeometryIdentity); R->SetStringField(TEXT("originalBodyIdentity"), S.OriginalBodyIdentity); R->SetNumberField(TEXT("sourceRoleIndex"), S.Role); R->SetBoolField(TEXT("simulation"), S.Simulation); R->SetBoolField(TEXT("query"), S.Query); R->SetBoolField(TEXT("probe"), S.Probe); R->SetBoolField(TEXT("welded"), S.Welded); R->SetObjectField(TEXT("localShapePose"), PoseJson(S.LocalPose)); R->SetObjectField(TEXT("weldRelativePose"), PoseJson(S.WeldPose)); BoxJson(R, TEXT("nativeInflatedBoundsWorldCm"), S.InflatedWorld); R->SetObjectField(TEXT("geometry"), GeometryJson(S.Geometry)); Shapes.Add(MakeShared<FJsonValueObject>(R)); }
    O->SetArrayField(TEXT("shapes"), Shapes); return O;
}
TSharedRef<FJsonObject> MeshJson(const FMesh& M)
{
    auto O = MakeShared<FJsonObject>(); O->SetNumberField(TEXT("sourceRoleIndex"), M.Role); O->SetNumberField(TEXT("instanceIndex"), M.Instance);
    O->SetStringField(TEXT("componentPath"), M.ComponentPath); O->SetStringField(TEXT("parentPath"), M.ParentPath); O->SetStringField(TEXT("meshPath"), M.MeshPath); O->SetStringField(TEXT("packagePath"), M.PackagePath); O->SetStringField(TEXT("packageFile"), M.PackageFile); O->SetStringField(TEXT("packageFileSha1"), M.PackageSha1); O->SetStringField(TEXT("bodySetupPath"), M.SetupPath); O->SetStringField(TEXT("bodySetupGuid"), M.SetupGuid);
    O->SetObjectField(TEXT("relativePose"), PoseJson(M.Relative)); O->SetObjectField(TEXT("worldPose"), PoseJson(M.World)); O->SetBoolField(TEXT("geometryKnown"), M.GeometryKnown); O->SetBoolField(TEXT("deformationKnown"), M.DeformationKnown);
    O->SetStringField(TEXT("digestEncoding"), TEXT("UE5.3 FMemoryWriter int32LOD,int32vertexCount,int32indexCount,FVector float64XYZ, int32indices; little-endian SHA1. Consumer freezes SHA256 of exact exported JSON and packages separately."));
    TArray<TSharedPtr<FJsonValue>> LODs;
    for (const auto& L : M.LODs) { auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("lod"), L.Index); R->SetStringField(TEXT("contentSha1"), L.Digest); TArray<TSharedPtr<FJsonValue>> V, I; for (const FVector& X : L.Vertices) V.Add(MakeShared<FJsonValueArray>(VectorJson(X))); for (int32 X : L.Indices) I.Add(MakeShared<FJsonValueNumber>(X)); R->SetArrayField(TEXT("verticesCm"), V); R->SetArrayField(TEXT("indices"), I); LODs.Add(MakeShared<FJsonValueObject>(R)); }
    O->SetArrayField(TEXT("LODs"), LODs); return O;
}
FString SerializeMayaCargoGeometryJson(TSharedRef<FJsonObject> O)
{ FString Result; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Result)); return Result; }
}

FString AMAYANativeDemoPawn::CaptureCargoClearanceGeometryJson(FVector EnvelopeMinWorldCm, FVector EnvelopeMaxWorldCm, FString ReviewedSpecSha256)
{
    using namespace MAYACargoGeometry;
    const double Started = FPlatformTime::Seconds();
    const auto Refuse = [&](const FString& Reason)
    { auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("schema"), TEXT("MAYA.CargoGeometry.v1")); O->SetBoolField(TEXT("complete"), false); O->SetStringField(TEXT("reason"), Reason); O->SetBoolField(TEXT("oneShotConsumed"), true); O->SetNumberField(TEXT("startedRealS"), Started); O->SetNumberField(TEXT("capturedRealS"), FPlatformTime::Seconds()); return SerializeMayaCargoGeometryJson(O); };
    if (bCargoClearanceGeometryCaptureConsumed) return Refuse(TEXT("ONE_SHOT_ALREADY_CONSUMED"));
    bCargoClearanceGeometryCaptureConsumed = true;
    if (!IsInGameThread() || !Loaded(this) || bEnding || !GetWorld() || GetWorld()->bIsTearingDown || GetWorld()->IsPaused()
        || !bReady || !bPhysicalAssembly || !Fault.IsEmpty() || bResetStabilizing || DoorState != EDoorState::ClosedLatched
        || Parts.Num() != 15 || Cosmetics.Num() != 15 || Joints.Num() != 8 || SupportMask != 63
        || Throttle != 0 || Steer != 0 || SupportStableSinceActiveRealS < 0 || GetWorld()->GetAudioTimeSeconds() - SupportStableSinceActiveRealS < 2.0)
        return Refuse(TEXT("FIXED_CLOSED_NEUTRAL_PHYSICAL_BASELINE_UNAVAILABLE"));
    for (double Nm : MotorNm) if (!FMath::IsFinite(Nm) || FMath::Abs(Nm) > 1.e-9) return Refuse(TEXT("MOTOR_NOT_NEUTRAL"));
    const auto* PC = Cast<APlayerController>(GetController());
    if (!PC || !PC->IsLocalController() || PC->GetPawn() != this) return Refuse(TEXT("EXACT_LOCAL_POSSESSION_UNAVAILABLE"));
    const FKey NeutralKeys[] = { EKeys::W, EKeys::S, EKeys::A, EKeys::D, EKeys::SpaceBar, EKeys::E, EKeys::R };
    for (const FKey& Key : NeutralKeys) if (PC->IsInputKeyDown(Key)) return Refuse(TEXT("ACTUAL_INPUT_NOT_NEUTRAL"));
    const FVector Velocity = Body(0)->GetPhysicsLinearVelocity(), Omega = Body(0)->GetPhysicsAngularVelocityInRadians(); const FRotator Rotation = Body(0)->GetComponentRotation();
    if (Velocity.ContainsNaN() || Omega.ContainsNaN() || Velocity.Size() > 5 || Omega.Size() > .05 || FMath::Abs(Rotation.Pitch) > 8 || FMath::Abs(Rotation.Roll) > 8) return Refuse(TEXT("STATIONARY_BASELINE_OUTSIDE_UNCHANGED_LIMITS"));
    for (int32 I = 0; I < 2; ++I) if (!bDoorPhysicalStateKnown[I] || FMath::Abs(DoorAngleRad[I]) > PI / 180 || FMath::Abs(DoorOmegaRadps[I]) > .1) return Refuse(TEXT("CLOSED_LID_PHYSICAL_STATE_UNAVAILABLE"));
    const bool SceneRequested = !ReviewedSpecSha256.IsEmpty(); const FBox Envelope(EnvelopeMinWorldCm, EnvelopeMaxWorldCm);
    if (SceneRequested)
    {
        bool ShaValid = ReviewedSpecSha256.Len() == 64; for (TCHAR C : ReviewedSpecSha256) ShaValid &= FChar::IsHexDigit(C);
        if (!ShaValid || !ValidBox(Envelope) || Envelope.GetSize().GetMin() <= 0 || Envelope.GetSize().GetMax() > MaxEnvelopeSideCm
            || (Envelope.GetCenter() - Body(0)->GetComponentLocation()).Size() > MaxEnvelopeCenterDistanceCm) return Refuse(TEXT("INVALID_OR_UNBOUNDED_NUMERIC_ENVELOPE"));
    }
    TStrongObjectPtr<UWorld> WorldPin(GetWorld()); TStrongObjectPtr<AMAYANativeDemoPawn> PawnPin(this);
    auto* Scene = GetWorld()->GetPhysicsScene(); auto* Solver = Scene ? Scene->GetSolver() : nullptr; if (!Scene || !Solver) return Refuse(TEXT("NATIVE_SCENE_SOLVER_UNAVAILABLE"));
    const uint64 Generation = ResetGeneration, Frame = GFrameCounter;
    TArray<TStrongObjectPtr<UObject>> Pins; FTransform RolePoses[15]; UBodySetup* Setups[15] = {}; FGuid SetupGuids[15];
    const FBodyInstance* OwnBodies[15] = {}; FPhysicsActorHandle Actors[9] = {}; UStaticMesh* Meshes[15] = {}; UStaticMesh* CosmeticMeshesBefore[15] = {};
    TMap<const FBodyInstance*, int32> RoleOfBody;
    for (int32 I = 0; I < 15; ++I)
    {
        auto* C = Body(I); auto* V = Cosmetics[I].Get();
        if (!Loaded(C) || !Loaded(V) || !C->IsRegistered() || !V->IsRegistered() || C->GetFName() != FName(Roles[I])
            || C->GetOwner() != this || V->GetOwner() != this || V->GetAttachParent() != C || V->GetCollisionEnabled() != ECollisionEnabled::NoCollision
            || V->IsSimulatingPhysics() || !Rigid(V->GetRelativeTransform()) || !V->GetRelativeTransform().Equals(FTransform::Identity, 1.e-6)) return Refuse(TEXT("FIXED_ROLE_OR_COSMETIC_OWNER_DIFFERS"));
        RolePoses[I] = C->GetComponentTransform(); Meshes[I] = C->GetStaticMesh(); CosmeticMeshesBefore[I] = V->GetStaticMesh(); Setups[I] = C->GetBodySetup();
        OwnBodies[I] = C->GetBodyInstance(NAME_None, false); if (!Rigid(RolePoses[I]) || !Loaded(Meshes[I]) || !Loaded(CosmeticMeshesBefore[I]) || !Loaded(Setups[I]) || !OwnBodies[I] || RoleOfBody.Contains(OwnBodies[I])) return Refuse(TEXT("ROLE_MESH_SETUP_OWN_BODY_UNAVAILABLE"));
        SetupGuids[I] = Setups[I]->BodySetupGuid; if (!SetupGuids[I].IsValid()) return Refuse(TEXT("ROLE_SETUP_GUID_UNAVAILABLE")); RoleOfBody.Add(OwnBodies[I], I);
        Pins.Emplace(C); Pins.Emplace(V); Pins.Emplace(Meshes[I]); Pins.Emplace(CosmeticMeshesBefore[I]); Pins.Emplace(Setups[I]); Pins.Emplace(Meshes[I]->GetOutermost()); Pins.Emplace(CosmeticMeshesBefore[I]->GetOutermost());
        if (I < 9) Actors[I] = C->GetBodyInstance()->GetPhysicsActorHandle();
        if (I >= 9 && OwnBodies[I]->WeldParent != OwnBodies[0]) return Refuse(TEXT("SIX_ACTUAL_WELDS_UNAVAILABLE"));
    }
    FBudget Budget{ Started }; TArray<FBody> Bodies; TArray<FJoint> JointSnapshot; bool Entered = false, RobotComplete = true;
    const bool ReadSucceeded = FPhysInterface_Chaos::ExecuteRead(Scene, [&]
    {
        Entered = true; TSet<FPhysicsActorHandle> Seen;
        for (int32 I = 0; I < 9; ++I)
        {
            const auto& Actor = Actors[I]; if (!FPhysInterface_Chaos::IsValid(Actor) || Seen.Contains(Actor)) { RobotComplete = false; break; }
            const auto State = Actor->GetGameThreadAPI().ObjectState(); if (State != Chaos::EObjectStateType::Dynamic && State != Chaos::EObjectStateType::Sleeping) { RobotComplete = false; break; }
            Seen.Add(Actor); FBody R; R.PhysicalIndex = I; R.ActorIdentity = Id(Actor); R.ComponentPath = Body(I)->GetPathName(); R.OwnerPath = GetPathName(); R.ActorWorld = FPhysInterface_Chaos::GetGlobalPose_AssumesLocked(Actor); if (!Rigid(R.ActorWorld)) { RobotComplete = false; break; }
            const int32 Count = FPhysInterface_Chaos::GetNumShapes(Actor); if (Count < 1 || Count > MaxRobotShapes - Budget.RobotShapes) { Budget.Error = TEXT("ROBOT_SHAPE_CAP"); RobotComplete = false; break; }
            TArray<FPhysicsShapeHandle> Shapes; FPhysInterface_Chaos::GetAllShapes_AssumedLocked(Actor, Shapes); if (Shapes.Num() != Count) { RobotComplete = false; break; }
            for (const auto& S : Shapes)
            {
                FShape Q; const auto* Original = OwnBodies[I]->GetOriginalBodyInstance(S); const int32* Role = RoleOfBody.Find(Original);
                if (!Role || (*Role < 9 && *Role != I) || (*Role >= 9 && I != 0)) { RobotComplete = false; break; }
                Q.Role = *Role; Q.OriginalBodyIdentity = Id(Original); Q.Welded = *Role >= 9;
                const auto* WeldMap = OwnBodies[I]->GetCurrentWeldInfo(); const auto* Weld = WeldMap ? WeldMap->Find(S) : nullptr;
                if (Q.Welded) { if (!Weld || Weld->ChildBI != Original || Original->WeldParent != OwnBodies[0] || !Rigid(Weld->RelativeTM)) { RobotComplete = false; break; } Q.WeldPose = Weld->RelativeTM; }
                else if (Weld) { RobotComplete = false; break; }
                if (!CaptureShape(S, Q, Budget, false)) { RobotComplete = false; break; }
                const FBox WorldBounds = Q.InflatedWorld.ExpandBy(1.e-4);
                for (const FVector& V : Q.Geometry.Vertices) if (!WorldBounds.IsInsideOrOn(R.ActorWorld.TransformPosition(InActor(Q.Geometry, V)))) { Budget.Error = TEXT("ACTUAL_WORLD_SHAPE_BOUNDS_MISMATCH"); RobotComplete = false; break; }
                if (!RobotComplete) break; R.Shapes.Add(MoveTemp(Q));
            }
            if (!RobotComplete) break; Bodies.Add(MoveTemp(R));
        }
        // Closure is read from actual native joints, not the controller enum.
        for (int32 I = 0; RobotComplete && I < 8; ++I)
        {
            const auto Ref = Joints[I]->ConstraintInstance.GetPhysicsConstraintRef();
            if (!Ref.IsValid() || !Ref.Constraint || !Ref.Constraint->IsType(Chaos::EConstraintType::JointConstraintType) || FPhysInterface_Chaos::IsBroken(Ref)) { RobotComplete = false; break; }
            const auto* J = static_cast<const Chaos::FJointConstraint*>(Ref.Constraint); const auto E = J->GetPhysicsBodies();
            if (E[0] != Actors[I < 6 ? I + 1 : 0]->GetPhysicsObject() || E[1] != Actors[I < 6 ? 0 : I + 1]->GetPhysicsObject()) { RobotComplete = false; break; }
            const auto& S = J->GetJointSettings(); FJoint R; R.Index = I; R.LinearLimit = S.LinearLimit; R.AngularLimits = FVector(S.AngularLimits);
            for (int32 F = 0; F < 2; ++F) R.Frames[F] = FTransform(FQuat(S.ConnectorTransforms[F].GetRotation()), FVector(S.ConnectorTransforms[F].GetTranslation()));
            if (!Rigid(R.Frames[0]) || !Rigid(R.Frames[1]) || !FMath::IsFinite(R.LinearLimit) || R.AngularLimits.ContainsNaN()) { RobotComplete = false; break; }
            for (int32 A = 0; A < 3; ++A) { R.LinearMotion[A] = int32(S.LinearMotionTypes[A]); R.AngularMotion[A] = int32(S.AngularMotionTypes[A]); if (R.LinearMotion[A] < 0 || R.LinearMotion[A] > 2 || R.AngularMotion[A] < 0 || R.AngularMotion[A] > 2) RobotComplete = false; }
            R.ClosedLock = I >= 6 && DDSJointSettingsMatch(S, I, JointInChassis[I], JointInPart[I], 0, 0, true, FDDSRobotPhysicsProfile());
            if (I >= 6 && !R.ClosedLock) RobotComplete = false; JointSnapshot.Add(R);
        }
    });
    if (!ReadSucceeded || !Entered || !RobotComplete || Bodies.Num() != 9) return Refuse(Budget.Error.IsEmpty() ? TEXT("ACTUAL_ROBOT_SHAPE_OWNER_OR_CLOSED_JOINT_UNKNOWN") : Budget.Error);
    int32 ShapePerRole[15] = {}; for (const auto& B : Bodies) for (const auto& S : B.Shapes) ++ShapePerRole[S.Role];
    for (int32 N : ShapePerRole) if (N < 1) return Refuse(TEXT("ACTUAL_ROLE_SHAPE_MISSING"));
    TArray<FMesh> CosmeticsSnapshot; bool CosmeticComplete = true;
    for (int32 I = 0; I < 15; ++I) { FMesh R; R.Role = I; if (!CaptureMesh(*Cosmetics[I].Get(), R, Budget) || !R.DeformationKnown) CosmeticComplete = false; CosmeticsSnapshot.Add(MoveTemp(R)); if (!Budget.Time()) return Refuse(Budget.Error); }
    // Loaded scoped scene observations are distinct from descriptor coverage and
    // motion-domain proof. No requested region means no scene enumeration at all.
    TArray<FBody> SceneBodies; TArray<FMesh> SceneMeshes; TArray<FSceneGuard> SceneGuards; bool SceneComplete = SceneRequested; int32 SceneEntries = 0; TArray<TStrongObjectPtr<UPrimitiveComponent>> ScenePins;
    if (SceneRequested)
    {
        for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
        {
            if (!Budget.Time()) return Refuse(Budget.Error); auto* C = *It;
            if (!Loaded(C) || C->GetWorld() != GetWorld() || C->GetOwner() == this || !C->IsRegistered()) continue;
            if (!C->Bounds.GetBox().Intersect(Envelope)) continue;
            if (++SceneEntries > MaxSceneEntries) return Refuse(TEXT("SCENE_COMPONENT_INSTANCE_CAP")); ScenePins.Emplace(C);
            FSceneGuard Guard; Guard.Component = C; Guard.ComponentPath = C->GetPathName(); Guard.ClassPath = C->GetClass()->GetPathName(); Guard.OwnerPath = C->GetOwner() ? C->GetOwner()->GetPathName() : TEXT(""); Guard.Pose = C->GetComponentTransform(); Guard.Collision = C->GetCollisionEnabled();
            if (!Rigid(Guard.Pose) || !Loaded(C->GetOwner())) { SceneComplete = false; continue; }
            if (auto* MeshComponent = Cast<UStaticMeshComponent>(C)) { Guard.Mesh = MeshComponent->GetStaticMesh(); Guard.Setup = MeshComponent->GetBodySetup(); if (Guard.Setup) Guard.SetupGuid = Guard.Setup->BodySetupGuid; }
            SceneGuards.Add(Guard); Pins.Emplace(C->GetOwner()); if (Guard.Mesh) Pins.Emplace(Guard.Mesh); if (Guard.Setup) Pins.Emplace(Guard.Setup);
            auto* SM = Cast<UStaticMeshComponent>(C); if (!SM || C->Mobility != EComponentMobility::Static) { SceneComplete = false; continue; }
            auto* ISM = Cast<UInstancedStaticMeshComponent>(SM);
            const int32 Instances = ISM ? ISM->GetInstanceCount() : 1;
            if (Instances < 1 || Instances > MaxSceneEntries - SceneEntries + 1) return Refuse(TEXT("SCENE_COMPONENT_INSTANCE_CAP")); SceneEntries += Instances - 1;
            // All native instances are read; component bounds only rejects a whole
            // component that cannot intersect. Missing instance bodies are unknown.
            for (int32 Instance = 0; Instance < Instances; ++Instance)
            {
                const auto* BI = ISM ? ISM->GetBodyInstance(NAME_None, true, Instance) : SM->GetBodyInstance();
                if (SM->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
                {
                    if (!BI) { SceneComplete = false; continue; } bool NativeKnown = false; FBody R; R.PhysicalIndex = Instance; R.ComponentPath = C->GetPathName(); R.OwnerPath = Guard.OwnerPath;
                    const bool Locked = FPhysInterface_Chaos::ExecuteRead(Scene, [&]
                    {
                        const auto& Actor = BI->GetPhysicsActorHandle(); if (!FPhysInterface_Chaos::IsValid(Actor)) return;
                        R.ActorIdentity = Id(Actor); R.ActorWorld = FPhysInterface_Chaos::GetGlobalPose_AssumesLocked(Actor); if (!Rigid(R.ActorWorld)) return;
                        const int32 Count = FPhysInterface_Chaos::GetNumShapes(Actor); if (Count < 1 || Count > MaxSceneShapes - Budget.SceneShapes) { Budget.Error = TEXT("SCENE_SHAPE_CAP"); return; }
                        TArray<FPhysicsShapeHandle> Shapes; FPhysInterface_Chaos::GetAllShapes_AssumedLocked(Actor, Shapes); if (Shapes.Num() != Count) return;
                        for (const auto& S : Shapes) { FShape Q; Q.OriginalBodyIdentity = Id(BI->GetOriginalBodyInstance(S)); if (!CaptureShape(S, Q, Budget, true)) return; R.Shapes.Add(MoveTemp(Q)); }
                        NativeKnown = true;
                    });
                    if (!Locked || !NativeKnown) SceneComplete = false; else SceneBodies.Add(MoveTemp(R));
                }
            }
            FMesh R; if (!CaptureMesh(*SM, R, Budget) || !R.DeformationKnown || ISM) SceneComplete = false;
            // ISM native colliders are observed above; instanced render matrices
            // are not silently replaced by the aggregate component render pose.
            SceneMeshes.Add(MoveTemp(R)); if (!Budget.Time()) return Refuse(Budget.Error);
        }
    }
    if (!Loaded(this) || bEnding || GetWorld() != WorldPin.Get() || GetWorld()->bIsTearingDown || GetWorld()->GetPhysicsScene() != Scene
        || Scene->GetSolver() != Solver || ResetGeneration != Generation || GFrameCounter != Frame) return Refuse(TEXT("NATIVE_CONTEXT_OR_RESET_GENERATION_CHANGED"));
    for (int32 I = 0; I < 15; ++I)
    {
        if (!Loaded(Body(I)) || !Body(I)->IsRegistered() || Body(I)->GetStaticMesh() != Meshes[I] || Body(I)->GetBodySetup() != Setups[I]
            || Setups[I]->BodySetupGuid != SetupGuids[I] || Body(I)->GetBodyInstance(NAME_None, false) != OwnBodies[I]
            || !Body(I)->GetComponentTransform().Equals(RolePoses[I], 1.e-6) || !Loaded(Cosmetics[I])
            || Cosmetics[I]->GetStaticMesh() != CosmeticMeshesBefore[I] || Cosmetics[I]->GetAttachParent() != Body(I)
            || (I < 9 && Body(I)->GetBodyInstance()->GetPhysicsActorHandle() != Actors[I])) return Refuse(TEXT("ROLE_MESH_SETUP_ACTOR_OR_POSE_STALE"));
    }
    for (const auto& P : ScenePins) if (!Loaded(P.Get()) || !P->IsRegistered() || P->GetWorld() != WorldPin.Get()) return Refuse(TEXT("SCENE_COMPONENT_STALE"));
    for (const auto& G : SceneGuards)
    {
        auto* C = G.Component;
        if (C->GetPathName() != G.ComponentPath || C->GetClass()->GetPathName() != G.ClassPath || !Loaded(C->GetOwner()) || C->GetOwner()->GetPathName() != G.OwnerPath
            || !C->GetComponentTransform().Equals(G.Pose, 1.e-6) || C->GetCollisionEnabled() != G.Collision) return Refuse(TEXT("SCENE_TRANSFORM_OWNER_OR_COLLISION_STALE"));
        if (auto* SM = Cast<UStaticMeshComponent>(C)) if (SM->GetStaticMesh() != G.Mesh || SM->GetBodySetup() != G.Setup || (G.Setup && G.Setup->BodySetupGuid != G.SetupGuid)) return Refuse(TEXT("SCENE_MESH_SETUP_STALE"));
    }
    if (!Budget.Time()) return Refuse(Budget.Error);
    auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("schema"), TEXT("MAYA.CargoGeometry.v1")); O->SetBoolField(TEXT("actual_native"), true); O->SetBoolField(TEXT("actualNativeGeometry"), true); O->SetBoolField(TEXT("beforeAfterIdentityChecksPassed"), true);
    O->SetBoolField(TEXT("oneShotConsumed"), true); O->SetBoolField(TEXT("robotGeometryComplete"), true); O->SetBoolField(TEXT("cosmeticGeometryComplete"), CosmeticComplete);
    O->SetBoolField(TEXT("sceneGeometryComplete"), SceneComplete); O->SetBoolField(TEXT("sceneLoadedRosterOnly"), true);
    O->SetBoolField(TEXT("sceneDescriptorCoverageVerified"), false); O->SetBoolField(TEXT("domainKnown"), false); O->SetBoolField(TEXT("complete"), false);
    O->SetBoolField(TEXT("configurationDomainClearancePass"), false); O->SetBoolField(TEXT("nativeTrajectoryContained"), false);
    O->SetStringField(TEXT("reason"), SceneRequested ? TEXT("DOMAIN_AND_SCENE_DESCRIPTOR_COVERAGE_REQUIRE_INDEPENDENT_PROOF") : TEXT("DOMAIN_UNBOUNDED"));
    O->SetStringField(TEXT("observationClock"), TEXT("GAME-thread native read-lock snapshots; no solver-step identity; scene reads separate from robot read"));
    O->SetStringField(TEXT("pawnPath"), GetPathName()); O->SetStringField(TEXT("worldPath"), GetWorld()->GetPathName()); O->SetStringField(TEXT("sceneIdentity"), Id(Scene)); O->SetStringField(TEXT("solverIdentity"), Id(Solver));
    O->SetStringField(TEXT("telemetrySourceSha256"), TEXT("02CA20A6F54EA1D1364219F18893CD594DD159257DFBE4FFFD0102B0E3A723FA"));
    O->SetStringField(TEXT("pawnSourceSha256"), TEXT("02CA20A6F54EA1D1364219F18893CD594DD159257DFBE4FFFD0102B0E3A723FA"));
    O->SetBoolField(TEXT("sourceBindingRequiresBuildManifest"), true); O->SetStringField(TEXT("reviewedSpecSha256"), ReviewedSpecSha256); O->SetBoolField(TEXT("physicalErrorBoundsKnown"), false);
    if (SceneRequested) { O->SetArrayField(TEXT("envelopeMinWorldCm"), VectorJson(EnvelopeMinWorldCm)); O->SetArrayField(TEXT("envelopeMaxWorldCm"), VectorJson(EnvelopeMaxWorldCm)); }
    O->SetNumberField(TEXT("engineFrame"), double(Frame)); O->SetNumberField(TEXT("resetGeneration"), double(Generation)); O->SetNumberField(TEXT("startedRealS"), Started);
    TArray<TSharedPtr<FJsonValue>> RoleRows, BodyRows, CosmeticRows, SceneBodyRows, SceneMeshRows, GuardRows, JointRows, Errors, Unresolved;
    for (int32 I = 0; I < 15; ++I) { auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("roleIndex"), I); R->SetStringField(TEXT("sourceRole"), Roles[I]); R->SetStringField(TEXT("sourceComponentPath"), Body(I)->GetPathName()); R->SetObjectField(TEXT("componentWorldPose"), PoseJson(RolePoses[I])); R->SetStringField(TEXT("originalOwnBodyIdentity"), Id(OwnBodies[I])); R->SetStringField(TEXT("physicalActorIdentity"), Id(Actors[I < 9 ? I : 0])); R->SetStringField(TEXT("meshPath"), Meshes[I]->GetPathName()); R->SetStringField(TEXT("bodySetupPath"), Setups[I]->GetPathName()); R->SetStringField(TEXT("bodySetupGuid"), SetupGuids[I].ToString()); RoleRows.Add(MakeShared<FJsonValueObject>(R)); }
    for (const auto& B : Bodies) BodyRows.Add(MakeShared<FJsonValueObject>(BodyJson(B))); for (const auto& M : CosmeticsSnapshot) CosmeticRows.Add(MakeShared<FJsonValueObject>(MeshJson(M)));
    for (const auto& B : SceneBodies) SceneBodyRows.Add(MakeShared<FJsonValueObject>(BodyJson(B))); for (const auto& M : SceneMeshes) SceneMeshRows.Add(MakeShared<FJsonValueObject>(MeshJson(M)));
    for (const auto& G : SceneGuards)
    {
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("componentPath"), G.ComponentPath); R->SetStringField(TEXT("classPath"), G.ClassPath); R->SetStringField(TEXT("ownerPath"), G.OwnerPath); R->SetArrayField(TEXT("locationCm"), VectorJson(G.Pose.GetLocation())); R->SetArrayField(TEXT("scale"), VectorJson(G.Pose.GetScale3D()));
        const FQuat Q = G.Pose.GetRotation(); TArray<TSharedPtr<FJsonValue>> Quat = { MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y), MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W) }; R->SetArrayField(TEXT("quaternion"), Quat);
        R->SetStringField(TEXT("collisionEnabledName"), UEnum::GetValueAsString(G.Collision)); R->SetNumberField(TEXT("collisionEnabled"), int32(G.Collision)); GuardRows.Add(MakeShared<FJsonValueObject>(R));
    }
    for (const auto& J : JointSnapshot)
    {
        auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("jointIndex"), J.Index); R->SetBoolField(TEXT("known"), true); R->SetBoolField(TEXT("valid"), true); R->SetBoolField(TEXT("broken"), false); R->SetBoolField(TEXT("endpointsMatch"), true); R->SetBoolField(TEXT("closedLockMatches"), J.ClosedLock);
        TArray<TSharedPtr<FJsonValue>> F, L, A; F.Add(MakeShared<FJsonValueObject>(PoseJson(J.Frames[0]))); F.Add(MakeShared<FJsonValueObject>(PoseJson(J.Frames[1]))); for (int32 I = 0; I < 3; ++I) { L.Add(MakeShared<FJsonValueNumber>(J.LinearMotion[I])); A.Add(MakeShared<FJsonValueNumber>(J.AngularMotion[I])); }
        R->SetArrayField(TEXT("connectorTransforms"), F); R->SetArrayField(TEXT("linearMotionNativeEnums"), L); R->SetArrayField(TEXT("angularMotionNativeEnums"), A); R->SetArrayField(TEXT("angularLimitsRad"), VectorJson(J.AngularLimits)); R->SetNumberField(TEXT("linearLimitCm"), J.LinearLimit); JointRows.Add(MakeShared<FJsonValueObject>(R));
    }
    if (!CosmeticComplete) Errors.Add(MakeShared<FJsonValueString>(TEXT("CURRENT_COSMETIC_GEOMETRY_DEFORMATION_OR_PACKAGE_BINDING_UNKNOWN")));
    if (!SceneComplete) Errors.Add(MakeShared<FJsonValueString>(TEXT("LOADED_SCENE_GEOMETRY_UNKNOWN_OR_NOT_REQUESTED")));
    Unresolved.Add(MakeShared<FJsonValueString>(TEXT("RELEVANT_UNLOADED_DESCRIPTOR_COVERAGE_UNVERIFIED")));
    O->SetArrayField(TEXT("sceneTransformGuards"), GuardRows); O->SetArrayField(TEXT("nativeJoints"), JointRows); O->SetArrayField(TEXT("errors"), Errors); O->SetArrayField(TEXT("unresolvedDescriptors"), Unresolved);
    O->SetArrayField(TEXT("roles"), RoleRows); O->SetArrayField(TEXT("correctedRoleMap"), RoleRows); O->SetArrayField(TEXT("bodies"), BodyRows); O->SetArrayField(TEXT("cosmetics"), CosmeticRows); O->SetArrayField(TEXT("sceneBodies"), SceneBodyRows); O->SetArrayField(TEXT("sceneMeshes"), SceneMeshRows);
    O->SetNumberField(TEXT("capturedRealS"), FPlatformTime::Seconds()); O->SetNumberField(TEXT("totalVertices"), Budget.Vertices); O->SetNumberField(TEXT("totalTriangles"), Budget.Triangles);
    const FString Result = SerializeMayaCargoGeometryJson(O); const FTCHARToUTF8 Encoded(*Result);
    if (Encoded.Length() > MaxResponseBytes) return Refuse(TEXT("BULK_RESPONSE_32MIB_CAP"));
    if (!Budget.Time()) return Refuse(Budget.Error); return Result;
}
