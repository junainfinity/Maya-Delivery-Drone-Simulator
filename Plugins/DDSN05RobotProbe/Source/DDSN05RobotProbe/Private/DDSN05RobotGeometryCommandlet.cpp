#include "DDSN05RobotGeometryCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Chaos/ImplicitObject.h"
#include "Misc/ScopeExit.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "DDSN05ProbeReportIO.h"
#include "DDSOriginalLiveIntake.h"
#include "DDSOriginalMaterialReceipt.h"
#include "DDSRobotLifecycleOwner.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"

namespace DDSN05Probe
{
    constexpr TCHAR Root[] = TEXT("/Game/BendylabStudios");
    constexpr TCHAR MeshRoot[] = TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/");
    constexpr TCHAR BlueprintPath[] = TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1");
    constexpr TCHAR BlueprintPackage[] = TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1");

    bool RecordLoadedIdentity(const UObject* Object, const FString& RequestedObjectPath,
        const FString& RequestedPackageName, const TSharedRef<FJsonObject>& Row)
    {
        const FString LoadedObjectPath = Object ? Object->GetPathName() : TEXT("");
        const FString LoadedPackageName = Object && Object->GetOutermost()
            ? Object->GetOutermost()->GetName() : TEXT("");
        const bool bIdentityMatches = Object && LoadedObjectPath==RequestedObjectPath
            && LoadedPackageName==RequestedPackageName;
        Row->SetStringField(TEXT("requestedObjectPath"),RequestedObjectPath);
        Row->SetStringField(TEXT("requestedPackageName"),RequestedPackageName);
        Row->SetStringField(TEXT("loadedObjectPath"),LoadedObjectPath);
        Row->SetStringField(TEXT("loadedPackageName"),LoadedPackageName);
        Row->SetBoolField(TEXT("matchesRequestedIdentity"),bIdentityMatches);
        if (!bIdentityMatches)
            Row->SetStringField(TEXT("identityFailure"),Object
                ? TEXT("returned object/package differs; redirected or aliased loads are not admitted")
                : TEXT("requested object failed to load"));
        return bIdentityMatches;
    }

    FString ExactMeshPath(const TCHAR* AssetName)
    {
        return FString(MeshRoot)+AssetName+TEXT(".")+AssetName;
    }

    bool HasNoReparseAncestor(FString Directory)
    {
        for (;;)
        {
            if (!ReportDirectoryAncestorSafe(Directory)) return false;
            const FString Parent = FPaths::GetPath(Directory);
            if (Parent.IsEmpty() || FPaths::IsSamePath(Parent,Directory)) break;
            Directory=Parent;
        }
        return true;
    }

    TSharedRef<FJsonObject> Vec(const FVector& V)
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("x"),V.X);
        J->SetNumberField(TEXT("y"),V.Y);
        J->SetNumberField(TEXT("z"),V.Z);
        return J;
    }

    TSharedRef<FJsonObject> Box(const FBox& B)
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetBoolField(TEXT("valid"),B.IsValid != 0);
        if (B.IsValid)
        {
            J->SetObjectField(TEXT("minCm"),Vec(B.Min));
            J->SetObjectField(TEXT("maxCm"),Vec(B.Max));
            J->SetObjectField(TEXT("sizeCm"),Vec(B.GetSize()));
        }
        return J;
    }

    TSharedRef<FJsonObject> Bounds(const FBoxSphereBounds& B)
    {
        TSharedRef<FJsonObject> J = Box(B.GetBox());
        J->SetObjectField(TEXT("originCm"),Vec(B.Origin));
        J->SetObjectField(TEXT("boxExtentCm"),Vec(B.BoxExtent));
        J->SetNumberField(TEXT("sphereRadiusCm"),B.SphereRadius);
        return J;
    }

    TSharedRef<FJsonObject> Transform(const USceneComponent& C,
        const FString& ParentTemplateName)
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetObjectField(TEXT("relativeLocationCm"),Vec(C.GetRelativeLocation()));
        J->SetObjectField(TEXT("relativeRotationPitchYawRollDeg"),
            Vec(FVector(C.GetRelativeRotation().Pitch,C.GetRelativeRotation().Yaw,
                C.GetRelativeRotation().Roll)));
        J->SetObjectField(TEXT("relativeScale"),Vec(C.GetRelativeScale3D()));
        J->SetStringField(TEXT("attachParentTemplate"),ParentTemplateName);
        return J;
    }


    bool FiniteTemplateTransform(const FTransform& T)
    {
        const FVector P=T.GetTranslation(), S=T.GetScale3D(); const FQuat Q=T.GetRotation();
        return !P.ContainsNaN() && !S.ContainsNaN() && !Q.ContainsNaN()
            && FMath::IsFinite(P.X) && FMath::IsFinite(P.Y) && FMath::IsFinite(P.Z)
            && FMath::IsFinite(S.X) && FMath::IsFinite(S.Y) && FMath::IsFinite(S.Z)
            && FMath::IsFinite(Q.SizeSquared()) && FMath::IsNearlyEqual(Q.SizeSquared(),1.0,1.e-4)
            && FMath::Abs(S.X)>UE_SMALL_NUMBER && FMath::Abs(S.Y)>UE_SMALL_NUMBER && FMath::Abs(S.Z)>UE_SMALL_NUMBER;
    }
    TSharedRef<FJsonObject> Pose(const FTransform& T)
    {
        auto J=MakeShared<FJsonObject>(); J->SetObjectField(TEXT("translationCm"),Vec(T.GetTranslation()));
        J->SetObjectField(TEXT("scale"),Vec(T.GetScale3D()));
        auto Q=MakeShared<FJsonObject>(); Q->SetNumberField(TEXT("x"),T.GetRotation().X);
        Q->SetNumberField(TEXT("y"),T.GetRotation().Y); Q->SetNumberField(TEXT("z"),T.GetRotation().Z);
        Q->SetNumberField(TEXT("w"),T.GetRotation().W); J->SetObjectField(TEXT("rotationQuaternion"),Q); return J;
    }

    // Raw accessors only until every reachable edge is proven finite. In Editor,
    // GetAllNodes may rebuild an empty cache recursively; it is NOT a safe prewalk.
    bool ReadBoundedSCSNodes(USimpleConstructionScript& SCS, TArray<USCS_Node*>& Out, FString& Error)
    {
        Out.Reset();
        struct FPending { USCS_Node* Node; int32 Depth; };
        TArray<FPending> Stack; TSet<USCS_Node*> Discovered;
        const auto& Roots=SCS.GetRootNodes(); int32 Edges=0;
        if (Roots.Num()==0 || Roots.Num()>256)
        { Error=TEXT("missing or excessive raw root count"); return false; }
        for (USCS_Node* AuthoredRootNode:Roots)
        {
            if (!IsValid(AuthoredRootNode) || Discovered.Contains(AuthoredRootNode))
            { Error=TEXT("null or duplicate raw root"); return false; }
            Discovered.Add(AuthoredRootNode); Stack.Add({AuthoredRootNode,1});
        }
        while (Stack.Num())
        {
            const FPending Next=Stack.Pop(false);
            if (Next.Depth>256 || Out.Num()>=256)
            { Error=TEXT("raw SCS depth/node budget exceeded"); return false; }
            Out.Add(Next.Node);
            const auto& Children=Next.Node->GetChildNodes();
            if (Children.Num()>256)
            { Error=TEXT("raw SCS child-array budget exceeded"); return false; }
            for (USCS_Node* Child:Children)
            {
                if (++Edges>256 || !IsValid(Child) || Discovered.Contains(Child))
                { Error=TEXT("raw SCS null/cycle/duplicate/multiple-parent or edge budget rejected"); return false; }
                if (Discovered.Num()>=256 || Next.Depth>=256)
                { Error=TEXT("raw SCS depth/node budget exceeded before enqueue"); return false; }
                Discovered.Add(Child); Stack.Add({Child,Next.Depth+1});
            }
        }
        // Only now can an empty Editor cache safely rebuild from checked roots.
        // Nonempty stale caches must match the exact validated tree membership.
        const auto& Full=SCS.GetAllNodes(); TSet<USCS_Node*> Cached;
        if (Full.Num()!=Out.Num())
        { Error=TEXT("cached/full SCS membership contains orphan or omitted node"); Out.Reset(); return false; }
        for (USCS_Node* N:Full)
        {
            if (!Discovered.Contains(N) || Cached.Contains(N))
            { Error=TEXT("cached/full SCS foreign or duplicate membership"); Out.Reset(); return false; }
            Cached.Add(N);
        }
        return true;
    }

    // Local authored SCS graph only. External/native/inherited/socket/absolute
    // relationships remain explicit unknowns; none is inferred as chassis.
    TSharedRef<FJsonObject> SCSGeometry(USimpleConstructionScript& SCS, const TArray<USCS_Node*>& Nodes, bool& Valid)
    {
        auto J=MakeShared<FJsonObject>(); Valid=false;
        J->SetStringField(TEXT("provenance"),TEXT("original Blueprint local SCS GetAllNodes/GetChildNodes/FindParentNode; authored templates, not spawned state"));
        if (Nodes.Num()==0 || Nodes.Num()>256) { J->SetStringField(TEXT("error"),TEXT("missing or oversized bounded SCS graph")); return J; }
        TSet<USCS_Node*> Known; TSet<FName> Names; TSet<const UActorComponent*> Templates;
        TMap<USCS_Node*,USCS_Node*> Parents; TSet<USCS_Node*> Roots;
        for (USCS_Node* N:Nodes)
        {
            if (!IsValid(N) || Known.Contains(N) || N->GetVariableName().IsNone() || Names.Contains(N->GetVariableName())
                || !IsValid(N->ComponentTemplate) || Templates.Contains(N->ComponentTemplate.Get()))
            { J->SetStringField(TEXT("error"),TEXT("duplicate/missing SCS node, variable or template identity")); return J; }
            Known.Add(N); Names.Add(N->GetVariableName()); Templates.Add(N->ComponentTemplate.Get());
            if (const auto* C=Cast<USceneComponent>(N->ComponentTemplate))
                if (!FiniteTemplateTransform(C->GetRelativeTransform()))
                { J->SetStringField(TEXT("error"),TEXT("nonfinite, degenerate or unnormalized authored transform")); return J; }
        }
        for (USCS_Node* R:SCS.GetRootNodes())
        {
            if (!Known.Contains(R) || Roots.Contains(R)) { J->SetStringField(TEXT("error"),TEXT("duplicate or foreign root")); return J; }
            Roots.Add(R);
        }
        for (USCS_Node* N:Nodes)
            for (USCS_Node* Child:N->GetChildNodes())
            {
                if (!Known.Contains(Child) || Parents.Contains(Child))
                { J->SetStringField(TEXT("error"),TEXT("foreign child, duplicate edge or multiple parents")); return J; }
                Parents.Add(Child,N);
            }
        for (USCS_Node* N:Nodes)
        {
            USCS_Node* const* P=Parents.Find(N);
            if (SCS.FindParentNode(N)!=(P?*P:nullptr) || (P && Roots.Contains(N)) || (!P && !Roots.Contains(N)))
            { J->SetStringField(TEXT("error"),TEXT("SCS parent/root disagreement or orphan")); return J; }
            TSet<USCS_Node*> Seen; USCS_Node* At=N;
            while (At)
            {
                if (Seen.Contains(At)) { J->SetStringField(TEXT("error"),TEXT("SCS cycle rejected")); return J; }
                Seen.Add(At); auto PP=Parents.Find(At); At=PP?*PP:nullptr;
            }
        }
        TMap<USCS_Node*,FTransform> Composed; TMap<USCS_Node*,USCS_Node*> RootOf;
        TMap<USCS_Node*,FString> Reasons;
        TFunction<bool(USCS_Node*)> Resolve=[&](USCS_Node* N)
        {
            if (Composed.Contains(N)) return true; if (Reasons.Contains(N)) return false;
            const auto* C=Cast<USceneComponent>(N->ComponentTemplate);
            if (!C) { Reasons.Add(N,TEXT("non-scene component ancestor")); return false; }
            if (!N->AttachToName.IsNone()) { Reasons.Add(N,TEXT("attachment socket/bone transform not resolved")); return false; }
            if (C->IsUsingAbsoluteLocation() || C->IsUsingAbsoluteRotation() || C->IsUsingAbsoluteScale())
            { Reasons.Add(N,TEXT("absolute world transform flags require actual spawned context")); return false; }
            const auto Parent=Parents.Find(N); FTransform T=C->GetRelativeTransform();
            if (Parent)
            {
                if (!Resolve(*Parent)) { Reasons.Add(N,TEXT("unresolved parent: ")+Reasons.FindRef(*Parent)); return false; }
                T=T*Composed.FindChecked(*Parent); RootOf.Add(N,RootOf.FindChecked(*Parent));
            }
            else
            {
                if (!N->ParentComponentOrVariableName.IsNone() || !N->ParentComponentOwnerClassName.IsNone() || N->bIsParentComponentNative)
                { Reasons.Add(N,TEXT("external/native/inherited parent unresolved")); return false; }
                RootOf.Add(N,N); // explicitly verified local root, not inferred from an empty template attach pointer.
            }
            if (!FiniteTemplateTransform(T)) { Reasons.Add(N,TEXT("invalid composed FTransform")); return false; }
            Composed.Add(N,T); return true;
        };
        USCS_Node* Chassis=nullptr; int32 ChassisCount=0;
        for (USCS_Node* N:Nodes)
            if (const auto* C=Cast<UStaticMeshComponent>(N->ComponentTemplate))
                if (C->GetStaticMesh() && C->GetStaticMesh()->GetPathName()==ExactMeshPath(TEXT("SM_DeliveryRobot_Base")))
                { Chassis=N; ++ChassisCount; }
        J->SetNumberField(TEXT("exactBaseMeshTemplateCount"),ChassisCount);
        const bool ChassisResolved=ChassisCount==1 && Resolve(Chassis);
        TArray<TSharedPtr<FJsonValue>> Rows; int32 Unresolved=0;
        for (USCS_Node* N:Nodes)
        {
            auto Row=MakeShared<FJsonObject>(); const auto Parent=Parents.Find(N);
            Row->SetStringField(TEXT("nodeObjectPath"),N->GetPathName());
            Row->SetStringField(TEXT("variableName"),N->GetVariableName().ToString());
            Row->SetStringField(TEXT("componentTemplateObjectPath"),N->ComponentTemplate->GetPathName());
            Row->SetStringField(TEXT("localParentNodeObjectPath"),Parent?(*Parent)->GetPathName():TEXT(""));
            Row->SetBoolField(TEXT("verifiedLocalRoot"),Roots.Contains(N));
            Row->SetStringField(TEXT("externalParentVariableName"),N->ParentComponentOrVariableName.ToString());
            Row->SetStringField(TEXT("externalParentOwnerClass"),N->ParentComponentOwnerClassName.ToString());
            Row->SetBoolField(TEXT("nativeParentDeclared"),N->bIsParentComponentNative);
            Row->SetStringField(TEXT("attachmentSocketOrBone"),N->AttachToName.ToString());
            TArray<TSharedPtr<FJsonValue>> Children;
            for (USCS_Node* Child:N->GetChildNodes()) Children.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
            Row->SetArrayField(TEXT("localChildNodeObjectPaths"),Children);
            const bool Resolved=Resolve(N); bool Relative=false;
            if (const auto* C=Cast<USceneComponent>(N->ComponentTemplate))
            {
                Row->SetObjectField(TEXT("authoredRelativePose"),Pose(C->GetRelativeTransform()));
                Row->SetBoolField(TEXT("absoluteLocation"),C->IsUsingAbsoluteLocation());
                Row->SetBoolField(TEXT("absoluteRotation"),C->IsUsingAbsoluteRotation());
                Row->SetBoolField(TEXT("absoluteScale"),C->IsUsingAbsoluteScale());
            }
            if (Resolved) Row->SetObjectField(TEXT("composedLocalSCSRootPose"),Pose(Composed.FindChecked(N)));
            if (Resolved && ChassisResolved && RootOf.FindChecked(N)==RootOf.FindChecked(Chassis))
            {
                const FTransform R=Composed.FindChecked(N).GetRelativeTransform(Composed.FindChecked(Chassis));
                if (FiniteTemplateTransform(R)) { Row->SetObjectField(TEXT("composedChassisRelativePose"),Pose(R)); Relative=true; }
            }
            Row->SetBoolField(TEXT("chassisRelativeResolved"),Relative);
            if (!Relative) { ++Unresolved; Row->SetStringField(TEXT("unresolvedReason"),Resolved
                ? TEXT("no unique resolved chassis in same local root") : Reasons.FindRef(N)); }
            if (const auto* C=Cast<UStaticMeshComponent>(N->ComponentTemplate))
            {
                const UStaticMesh* Mesh=C->GetStaticMesh(); const FString Path=Mesh?Mesh->GetPathName():TEXT("");
                Row->SetStringField(TEXT("exactMeshObjectPath"),Path); Row->SetStringField(TEXT("meshPackageName"),Mesh?Mesh->GetOutermost()->GetName():TEXT(""));
                FString Role=TEXT("unassigned original mesh");
                if (Path==ExactMeshPath(TEXT("SM_DeliveryRobot_Base"))) Role=TEXT("chassis/base");
                else if (Path==ExactMeshPath(TEXT("SM_DeliveryRobot_Wheel"))) Role=TEXT("wheel visual, no inferred index");
                else if (Path==ExactMeshPath(TEXT("SM_DeliveryRobot_Suspension"))) Role=TEXT("suspension visual, no inferred index");
                else if (Path==ExactMeshPath(TEXT("SM_DeliveryRobot_Door1")) || Path==ExactMeshPath(TEXT("SM_DeliveryRobot_Door2"))) Role=TEXT("original lid visual");
                Row->SetStringField(TEXT("exactMeshRoleProvenance"),Role);
                TArray<TSharedPtr<FJsonValue>> Mats;
                for (int32 I=0; I<C->GetNumMaterials(); ++I) { const auto* M=C->GetMaterial(I); Mats.Add(MakeShared<FJsonValueString>(M?M->GetPathName():TEXT(""))); }
                Row->SetArrayField(TEXT("effectiveOriginalTemplateMaterialObjectPaths"),Mats);
                auto Physics=MakeShared<FJsonObject>(); const auto& B=C->BodyInstance;
                Physics->SetStringField(TEXT("provenance"),TEXT("authored template BodyInstance inputs; not measured or simulated mass/inertia/calibration"));
                Physics->SetBoolField(TEXT("authoredMassOverrideEnabled"),B.bOverrideMass);
                if (FMath::IsFinite(B.GetMassOverride())) Physics->SetNumberField(TEXT("authoredMassOverrideKg"),B.GetMassOverride());
                if (FMath::IsFinite(B.MassScale)) Physics->SetNumberField(TEXT("authoredMassScale"),B.MassScale);
                if (!B.COMNudge.ContainsNaN()) Physics->SetObjectField(TEXT("authoredCOMNudgeCm"),Vec(B.COMNudge));
                if (!B.InertiaTensorScale.ContainsNaN()) Physics->SetObjectField(TEXT("authoredInertiaTensorScale"),Vec(B.InertiaTensorScale));
                Row->SetObjectField(TEXT("authoredBodyInputs"),Physics);
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Valid=true; J->SetBoolField(TEXT("localGraphValidated"),true); J->SetNumberField(TEXT("unresolvedChassisRelativeNodeCount"),Unresolved);
        J->SetArrayField(TEXT("nodes"),Rows); J->SetStringField(TEXT("unitsAndComposition"),TEXT("translation cm; quaternion xyzw; authored scale; childLocal * parentComposed and GetRelativeTransform(chassis), Unreal FTransform model (not full affine shear, spawned pose or manufacturer calibration)"));
        return J;
    }


    bool ReadReportedPose(const TSharedPtr<FJsonObject>& O, FTransform& Out)
    {
        if (!O) return false;
        const auto P=O->GetObjectField(TEXT("translationCm")); const auto S=O->GetObjectField(TEXT("scale"));
        const auto Q=O->GetObjectField(TEXT("rotationQuaternion"));
        Out=FTransform(FQuat(Q->GetNumberField(TEXT("x")),Q->GetNumberField(TEXT("y")),Q->GetNumberField(TEXT("z")),Q->GetNumberField(TEXT("w"))),
            FVector(P->GetNumberField(TEXT("x")),P->GetNumberField(TEXT("y")),P->GetNumberField(TEXT("z"))),
            FVector(S->GetNumberField(TEXT("x")),S->GetNumberField(TEXT("y")),S->GetNumberField(TEXT("z"))));
        return FiniteTemplateTransform(Out);
    }
    TSharedRef<FJsonObject> CreatedBodyMeasurement(UWorld& World, UStaticMesh& Mesh,
        const UStaticMeshComponent* Authored, const FTransform& AuthoredComposedPose, bool& Available)
    {
        auto J=MakeShared<FJsonObject>(); Available=false;
        J->SetStringField(TEXT("meshObjectPath"),Mesh.GetPathName()); J->SetStringField(TEXT("meshPackageName"),Mesh.GetOutermost()->GetName());
        J->SetStringField(TEXT("provenance"),TEXT("native transient Chaos body created from exact original mesh/simple collision at authored composed SCS scale; engine/game collision+dynamic body model, NOT manufacturer calibration or stepped simulation"));
        J->SetStringField(TEXT("sourceTemplateObjectPath"),Authored?Authored->GetPathName():TEXT("none: explicit original City candidate mesh DefaultInstance model"));
        J->SetObjectField(TEXT("requestedAuthoredComposedPoseCm"),Pose(AuthoredComposedPose));
        const UBodySetup* Setup=Mesh.GetBodySetup();
        const int32 SimpleCount=Setup?Setup->AggGeom.BoxElems.Num()+Setup->AggGeom.SphereElems.Num()+Setup->AggGeom.SphylElems.Num()+Setup->AggGeom.ConvexElems.Num()+Setup->AggGeom.TaperedCapsuleElems.Num():0;
        if (!FiniteTemplateTransform(AuthoredComposedPose) || !Setup || SimpleCount<=0 || SimpleCount>128
            || Setup->GetCollisionTraceFlag()==CTF_UseComplexAsSimple)
        { J->SetStringField(TEXT("unavailableReason"),TEXT("finite authored pose and original dynamic-compatible simple collision required; no proxy generated")); return J; }
        FActorSpawnParameters Spawn; Spawn.ObjectFlags=RF_Transient; Spawn.Name=NAME_None;
        AActor* Owner=World.SpawnActor<AActor>(AActor::StaticClass(),FTransform::Identity,Spawn);
        if (!IsValid(Owner)) { J->SetStringField(TEXT("unavailableReason"),TEXT("transient owned calibration actor creation failed")); return J; }
        auto* C=NewObject<UStaticMeshComponent>(Owner,NAME_None,RF_Transient); Owner->AddInstanceComponent(C); Owner->SetRootComponent(C);
        C->SetMobility(EComponentMobility::Movable); C->SetVisibility(false); C->SetHiddenInGame(true);
        if (Authored) C->BodyInstance.CopyBodyInstancePropertiesFrom(&Authored->BodyInstance);
        else C->BodyInstance.CopyBodyInstancePropertiesFrom(&Setup->DefaultInstance);
        C->BodyInstance.bAutoWeld=false;
        if (!C->SetStaticMesh(&Mesh)) { J->SetStringField(TEXT("unavailableReason"),TEXT("exact original mesh installation failed")); return J; }
        if (Authored) for (int32 M=0; M<Authored->GetNumMaterials(); ++M) C->SetMaterial(M,Authored->GetMaterial(M));
        C->SetWorldTransform(AuthoredComposedPose); C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        C->SetEnableGravity(false); C->RegisterComponent(); C->SetSimulatePhysics(true);
        FBodyInstance* Body=C->GetBodyInstance();
        if (!C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=&World || C->GetStaticMesh()!=&Mesh
            || !Body || !Body->IsValidBodyInstance() || !C->IsSimulatingPhysics()
            || !C->GetComponentTransform().Equals(AuthoredComposedPose,1.e-6))
        { J->SetStringField(TEXT("unavailableReason"),TEXT("registered exact-mesh dynamic body or authored supplier pose/scale unavailable")); return J; }
        double Mass=0; FVector Inertia=FVector::ZeroVector; FTransform MassLocal, MassWorld, ActorPose;
        TArray<TSharedPtr<FJsonValue>> Shapes; bool ReadEntered=false, ShapeValid=true; int32 SimulationShapes=0;
        const bool Locked=FPhysicsCommand::ExecuteRead(Body->GetPhysicsActorHandle(),[&](const FPhysicsActorHandle& Actor)
        {
            ReadEntered=true;
            if (!FPhysicsInterface::IsValid(Actor) || !FPhysicsInterface::IsDynamic(Actor)) { ShapeValid=false; return; }
            Mass=FPhysicsInterface::GetMass_AssumesLocked(Actor);
            Inertia=FPhysicsInterface::GetLocalInertiaTensor_AssumesLocked(Actor);
            MassLocal=FPhysicsInterface::GetComTransformLocal_AssumesLocked(Actor);
            MassWorld=FPhysicsInterface::GetComTransform_AssumesLocked(Actor);
            ActorPose=FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor);
            TArray<FPhysicsShapeHandle> NativeShapes;
            if (FPhysicsInterface::GetNumShapes(Actor)<=0 || FPhysicsInterface::GetNumShapes(Actor)>128)
            { ShapeValid=false; return; }
            FPhysicsInterface::GetAllShapes_AssumedLocked(Actor,NativeShapes);
            if (NativeShapes.Num()<=0 || NativeShapes.Num()>128) { ShapeValid=false; return; }
            for (const auto& Shape:NativeShapes)
            {
                if (!Shape.IsValid()) { ShapeValid=false; return; }
                const auto& Geometry=Shape.GetGeometry();
                if (!Geometry.HasBoundingBox()) { ShapeValid=false; return; }
                const auto B=Geometry.BoundingBox(); const FBox Bounds(FVector(B.Min()),FVector(B.Max()));
                const FTransform Local=FPhysicsInterface::GetLocalTransform(Shape);
                if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || !FiniteTemplateTransform(Local))
                { ShapeValid=false; return; }
                auto Row=MakeShared<FJsonObject>();
                const bool Sim=FPhysicsInterface::IsSimulationShape(Shape); if (Sim) ++SimulationShapes;
                Row->SetBoolField(TEXT("simulationShape"),Sim);
                Row->SetNumberField(TEXT("engineShapeTypeEnum"),int32(FPhysicsInterface::GetShapeType(Shape)));
                Row->SetNumberField(TEXT("nativeImplicitGeometryTypeEnum"),int32(Geometry.GetType()));
                Row->SetObjectField(TEXT("nativeImplicitGeometryActorLocalBoundsCm"),Box(Bounds));
                Row->SetObjectField(TEXT("nativeShapeLocalTransformCm"),Pose(Local));
                Row->SetStringField(TEXT("boundsMeaning"),TEXT("actual native implicit collision geometry AABB, includes embedded geometry scaling/transforms; shape diagnostic ONLY, never substituted for COM/inertia"));
                Shapes.Add(MakeShared<FJsonValueObject>(Row));
            }
        });
        if (!Locked || !ReadEntered || !ShapeValid || SimulationShapes<=0 || !FMath::IsFinite(Mass) || Mass<=0
            || Inertia.ContainsNaN() || !FMath::IsFinite(Inertia.X) || !FMath::IsFinite(Inertia.Y) || !FMath::IsFinite(Inertia.Z)
            || Inertia.X<=0 || Inertia.Y<=0 || Inertia.Z<=0 || !FiniteTemplateTransform(MassLocal)
            || !FiniteTemplateTransform(MassWorld) || !FiniteTemplateTransform(ActorPose)
            || !C->IsRegistered() || C->GetStaticMesh()!=&Mesh || !Body->IsValidBodyInstance()
            || !ActorPose.GetTranslation().Equals(AuthoredComposedPose.GetTranslation(),1.e-4)
            || !ActorPose.GetRotation().Equals(AuthoredComposedPose.GetRotation(),1.e-4))
        { J->SetStringField(TEXT("unavailableReason"),TEXT("native coherent mass/COM/principal inertia/shapes/pose sample invalid or unavailable")); return J; }
        J->SetNumberField(TEXT("nativeMassKg"),Mass); J->SetObjectField(TEXT("nativePrincipalInertiaKgCm2"),Vec(Inertia));
        J->SetObjectField(TEXT("nativePrincipalInertiaKgM2"),Vec(Inertia*1.e-4));
        J->SetObjectField(TEXT("nativeMassSpaceActorLocalCm"),Pose(MassLocal)); J->SetObjectField(TEXT("nativeMassSpaceWorldCm"),Pose(MassWorld));
        J->SetObjectField(TEXT("nativePrincipalAxisXActorLocal"),Vec(MassLocal.GetRotation().GetAxisX()));
        J->SetObjectField(TEXT("nativePrincipalAxisYActorLocal"),Vec(MassLocal.GetRotation().GetAxisY()));
        J->SetObjectField(TEXT("nativePrincipalAxisZActorLocal"),Vec(MassLocal.GetRotation().GetAxisZ()));
        J->SetObjectField(TEXT("nativeActorPoseCm"),Pose(ActorPose)); J->SetObjectField(TEXT("observedAuthoredSupplierScale"),Vec(C->GetComponentScale()));
        J->SetNumberField(TEXT("nativeSimulationShapeCount"),SimulationShapes); J->SetArrayField(TEXT("nativeShapes"),Shapes);
        J->SetStringField(TEXT("conditions"),TEXT("isolated transient world; copied authored BodyInstance/materials; gravity disabled; forced dynamic QueryAndPhysics model; NO world tick/contact/suspension/lid/drive/fit/readiness test"));
        Available=true; return J;
    }
    TSharedRef<FJsonObject> NativeOriginalBodyMeasurements(const TArray<USCS_Node*>& Nodes,
        const TSharedRef<FJsonObject>& Graph, const FString& CargoCandidatePath, const bool bObserveCargoBatch)
    {
        auto J=MakeShared<FJsonObject>();
        J->SetStringField(TEXT("scope"),TEXT("engine-calculated native created-body model observations only; no manufacturer calibration, gameplay permission, readiness, certificate or assembled robot"));
        const UWorld::InitializationValues IV=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
            .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
        UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,MakeUniqueObjectName(GetTransientPackage(),UWorld::StaticClass(),TEXT("DDSN05Calibration")),GetTransientPackage(),true,ERHIFeatureLevel::Num,&IV);
        if (!IsValid(World) || !World->GetPhysicsScene()) { if (World) World->DestroyWorld(false); J->SetStringField(TEXT("unavailableReason"),TEXT("transient native physics world unavailable")); return J; }
        ON_SCOPE_EXIT { World->DestroyWorld(false); }; // all read locks exit before world-owned body teardown.
        TArray<TSharedPtr<FJsonValue>> Results; int32 AvailableCount=0, UnavailableCount=0;
        const auto& GraphRows=Graph->GetArrayField(TEXT("nodes"));
        for (USCS_Node* N:Nodes)
        {
            const auto* C=Cast<UStaticMeshComponent>(N->ComponentTemplate); if (!C) continue;
            UStaticMesh* Mesh=C->GetStaticMesh().Get(); bool Available=false; TSharedPtr<FJsonObject> R;
            for (const auto& Value:GraphRows)
            {
                const auto Row=Value->AsObject(); bool Resolved=false;
                if (Row->GetStringField(TEXT("componentTemplateObjectPath"))!=C->GetPathName()) continue;
                FTransform PoseInput;
                if (Mesh && Mesh->GetPathName().StartsWith(MeshRoot) && Row->TryGetBoolField(TEXT("chassisRelativeResolved"),Resolved) && Resolved
                    && ReadReportedPose(Row->GetObjectField(TEXT("composedLocalSCSRootPose")),PoseInput))
                    R=CreatedBodyMeasurement(*World,*Mesh,C,PoseInput,Available);
                break;
            }
            if (!R) { R=MakeShared<FJsonObject>(); R->SetStringField(TEXT("unavailableReason"),TEXT("exact original mesh or resolved authored SCS/chassis frame unavailable")); }
            R->SetStringField(TEXT("sourceNodeObjectPath"),N->GetPathName()); R->SetStringField(TEXT("sourceTemplateObjectPath"),C->GetPathName());
            R->SetBoolField(TEXT("nativeCreatedBodySampleAvailable"),Available);
            Available?++AvailableCount:++UnavailableCount; Results.Add(MakeShared<FJsonValueObject>(R));
        }
        J->SetArrayField(TEXT("originalRobotBodies"),Results); J->SetNumberField(TEXT("availableBodyCount"),AvailableCount); J->SetNumberField(TEXT("unavailableBodyCount"),UnavailableCount);
        // Optional allowlisted ORIGINAL City asset observation. Unit scale is an
        // explicit candidate model hypothesis; no cargo identity or fit is granted.
        auto Cargo=MakeShared<FJsonObject>(); Cargo->SetBoolField(TEXT("acceptedAsGameplayCargo"),false);
        const FString Allowed=TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO");
        if (CargoCandidatePath==Allowed)
        {
            auto* Mesh=LoadObject<UStaticMesh>(nullptr,*Allowed);
            const bool Exact=RecordLoadedIdentity(Mesh,Allowed,TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO"),Cargo);
            if (Exact) { bool Available=false; Cargo->SetObjectField(TEXT("candidateNativeBodyModel"),CreatedBodyMeasurement(*World,*Mesh,nullptr,FTransform::Identity,Available)); Cargo->SetBoolField(TEXT("candidateNativeSampleAvailable"),Available); }
        }
        Cargo->SetStringField(TEXT("remaining"),TEXT("optional -CargoCandidate exact original City path; actual safe fit/mount/parcel ownership/mass-counting remains unvalidated; unit-scale candidate is a hypothesis, not chosen cargo"));
        J->SetObjectField(TEXT("originalCityCargoCandidate"),Cargo);
        // Optional fixed ORIGINAL City candidates. These unit-scale created-body
        // observations neither choose cargo nor prove mount/fit/driving safety.
        J->SetBoolField(TEXT("originalCityCargoBatchRequested"),bObserveCargoBatch);
        if (bObserveCargoBatch)
        {
            const TCHAR* OriginalCargoBatchPaths[] = {
                TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO"),
                TEXT("/Game/Megascans/3D_Assets/Dirty_Red_White_Box_rmtdZ_3d/Dirty_Red_White_Box_LOD0_Var1_rmtdZ.Dirty_Red_White_Box_LOD0_Var1_rmtdZ"),
                TEXT("/Game/Prop/FWY/Kit_Trash/Mesh/SM_FWYTrash_Bag_A01_N1.SM_FWYTrash_Bag_A01_N1"),
                TEXT("/Game/Prop/FWY/Kit_Trash/Mesh/SM_FWYTrash_Bottle_A01_N1.SM_FWYTrash_Bottle_A01_N1"),
                TEXT("/Game/Prop/FWY/Kit_Trash/Mesh/SM_FWYTrash_Cup_A01_N1.SM_FWYTrash_Cup_A01_N1"),
                TEXT("/Game/Prop/CONSTRUCTION/Kit_SandBag/Mesh/SM_Sandbag_H01_N1.SM_Sandbag_H01_N1")
            };
            static_assert(UE_ARRAY_COUNT(OriginalCargoBatchPaths)<=8,"Fixed cargo candidate bound");
            TArray<TSharedPtr<FJsonValue>> CargoRows;
            int32 BatchAvailable=0, BatchUnavailable=0;
            for (const TCHAR* ListedPath:OriginalCargoBatchPaths)
            {
                const FString ObjectPath(ListedPath);
                const FString PackagePath=ObjectPath.Left(ObjectPath.Find(TEXT(".")));
                auto Row=MakeShared<FJsonObject>();
                Row->SetBoolField(TEXT("acceptedAsGameplayCargo"),false);
                Row->SetStringField(TEXT("poseHypothesis"),TEXT("identity pose, original unit mesh scale/DefaultInstance; no chosen cargo/mount/fit"));
                UStaticMesh* CandidateMesh=LoadObject<UStaticMesh>(nullptr,*ObjectPath);
                const bool ExactIdentity=RecordLoadedIdentity(CandidateMesh,ObjectPath,PackagePath,Row);
                bool BodyAvailable=false;
                if (ExactIdentity)
                {
                    const FStaticMeshRenderData* CandidateRender=CandidateMesh->GetRenderData();
                    Row->SetBoolField(TEXT("renderDataPresent"),CandidateRender!=nullptr);
                    if (CandidateRender) Row->SetObjectField(TEXT("rawRenderBoundsMeshLocal"),Bounds(CandidateRender->Bounds));
                    Row->SetObjectField(TEXT("extendedBoundsMeshLocal"),Bounds(CandidateMesh->GetExtendedBounds()));
                    const UBodySetup* CandidateSetup=CandidateMesh->GetBodySetup();
                    Row->SetBoolField(TEXT("bodySetupPresent"),CandidateSetup!=nullptr);
                    if (CandidateSetup)
                    {
                        Row->SetNumberField(TEXT("collisionTraceFlagEnum"),(int32)CandidateSetup->GetCollisionTraceFlag());
                        Row->SetNumberField(TEXT("boxCount"),CandidateSetup->AggGeom.BoxElems.Num());
                        Row->SetNumberField(TEXT("sphereCount"),CandidateSetup->AggGeom.SphereElems.Num());
                        Row->SetNumberField(TEXT("capsuleCount"),CandidateSetup->AggGeom.SphylElems.Num());
                        Row->SetNumberField(TEXT("convexCount"),CandidateSetup->AggGeom.ConvexElems.Num());
                        Row->SetNumberField(TEXT("taperedCapsuleCount"),CandidateSetup->AggGeom.TaperedCapsuleElems.Num());
                    }
                    Row->SetObjectField(TEXT("candidateNativeBodyModel"),
                        CreatedBodyMeasurement(*World,*CandidateMesh,nullptr,FTransform::Identity,BodyAvailable));
                }
                else Row->SetStringField(TEXT("unavailableReason"),TEXT("exact listed original City asset identity unavailable; no replacement/proxy generated"));
                Row->SetBoolField(TEXT("candidateNativeSampleAvailable"),BodyAvailable);
                BodyAvailable?++BatchAvailable:++BatchUnavailable;
                CargoRows.Add(MakeShared<FJsonValueObject>(Row));
            }
            J->SetStringField(TEXT("originalCityCargoBatchSchema"),TEXT("DDSCity.N05.OriginalCityCargoBatch.v1"));
            J->SetArrayField(TEXT("originalCityCargoBatchCandidates"),CargoRows);
            J->SetNumberField(TEXT("batchCandidateCount"),CargoRows.Num());
            J->SetNumberField(TEXT("batchNativeAvailableCount"),BatchAvailable);
            J->SetNumberField(TEXT("batchNativeUnavailableCount"),BatchUnavailable);
            J->SetStringField(TEXT("batchRemaining"),TEXT("No automatic cargo acceptance, safe fit, mounted ownership, mass counting or driving readiness; unusable original collision stays unavailable"));
        }
        return J;
    }


    // Opt-in observation only. Role indices below are stable source bookkeeping,
    // never wheel-side/hinge/calibration or original suspension-motion evidence.
    TSharedRef<FJsonObject> NativeCombinedChassisMeasurement(const FString& Params,UBlueprint& BP)
    {
        auto J=MakeShared<FJsonObject>(); J->SetBoolField(TEXT("nativeCombinedBodySampleAvailable"),false);
        J->SetBoolField(TEXT("acceptedAsGameplayCargo"),false);
        J->SetStringField(TEXT("model"),TEXT("DECLARED GAME PHYSICS MODEL: one rigid chassis containing exact original Base plus six original suspension convex shapes; original kinematics/nine-body calibration/readiness NOT proved"));
        const auto Refuse=[&](const FString& E) { J->SetStringField(TEXT("unavailableReason"),E); return J; };
        static bool bCombinedObservationActive=false;
        if(!IsInGameThread() || bCombinedObservationActive)return Refuse(TEXT("Combined original observation wrong thread/reentrant"));
        TGuardValue<bool> ObservationGuard(bCombinedObservationActive,true);
        const auto ReadInput=[&](const TCHAR* Key,TArray<uint8>& Bytes) {
            FString P; if(!FParse::Value(*Params,Key,P) || FPaths::IsRelative(P))return false;
            const int64 N=IFileManager::Get().FileSize(*P);
            return N>0 && N<=1024*1024 && FFileHelper::LoadFileToArray(Bytes,*P) && Bytes.Num()==N;
        };
        FDDSOriginalLiveIntakeInputs Inputs;
        Inputs.ExpectedNativeGeometrySha256=TEXT("1CC79F31B10984B3DCA0A4ADB4CE0A540AFB6ED552E4C75A5DC66CEF993BB9A3");
        if(!ReadInput(TEXT("CombinedNativeRecord="),Inputs.NativeGeometryJsonBytes)
            || !ReadInput(TEXT("CombinedOriginalLedger="),Inputs.OriginalIntegrityLedgerBytes)
            || !ReadInput(TEXT("CombinedPackagePins="),Inputs.PackagePinsJsonBytes)
            || !FParse::Value(*Params,TEXT("CombinedPackagePinsSha="),Inputs.ExpectedPackagePinsSha256))
            return Refuse(TEXT("Absolute bounded original native/ledger/package inputs required; accepted live intake verifies all digests and exact25 originals"));
        Inputs.ExactCargoObjectPath=TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO");
        Inputs.CargoTargetComponentName=TEXT("N05_OriginalCargoVisualOnly"); Inputs.ActualCargoPartId=TEXT("OriginalCityBurgerBoxVisualOnly");
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_1"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=0; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Base"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Base"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_13"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=5; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension5"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension5"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_14"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=5; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel4"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel4"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_12"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=4; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension4"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension4"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_15"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=6; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel5"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel5"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_10"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=3; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension3"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension3"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_11"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=4; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel3"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel3"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_8"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=2; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension2"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension2"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_9"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=3; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel2"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel2"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_6"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=1; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension1"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension1"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_7"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=2; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel1"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel1"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_4"); R.Role=EDDSOriginalRobotPartRole::Suspension; R.Index=0; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Suspension"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Suspension"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_5"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=1; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Wheel"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Wheel"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_3"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=7; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Door1"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Door1"); Inputs.Original15Roles.Add(R); }
        { FDDSOriginalLiveRole R; R.NativeNodeObjectPath=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C:SimpleConstructionScript_0.SCS_Node_2"); R.Role=EDDSOriginalRobotPartRole::Body; R.Index=8; R.TargetComponentName=TEXT("N05_SM_DeliveryRobot_Door2"); R.SuppliedPartId=TEXT("SM_DeliveryRobot_Door2"); Inputs.Original15Roles.Add(R); }
        const auto IV=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
            .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).CreateFXSystem(false).SetTransactional(false);
        UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,MakeUniqueObjectName(GetTransientPackage(),UWorld::StaticClass(),TEXT("DDSN05CombinedCalibration")),GetTransientPackage(),true,ERHIFeatureLevel::Num,&IV);
        if(!IsValid(World) || !World->GetPhysicsScene()) { if(World)World->DestroyWorld(false);return Refuse(TEXT("Isolated owned physics world unavailable")); }
        ON_SCOPE_EXIT { World->DestroyWorld(false); }; // query read-lock exits before owned world/body teardown; no gameplay lease acquired.
        TArray<TStrongObjectPtr<UObject>> Lifetime; Lifetime.Emplace(World);Lifetime.Emplace(&BP);Lifetime.Emplace(BP.GeneratedClass.Get());
        if(!IsValid(BP.GeneratedClass) || BP.GeneratedClass->GetPathName()!=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C"))return Refuse(TEXT("Exact supplied generated publisher class unavailable"));
        FActorSpawnParameters Spawn;Spawn.ObjectFlags=RF_Transient;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Publisher=World->SpawnActor<AActor>(BP.GeneratedClass,FTransform::Identity,Spawn);
        AActor* Owner=World->SpawnActor<AActor>(AActor::StaticClass(),FTransform::Identity,Spawn);
        if(!IsValid(Publisher) || !IsValid(Owner))return Refuse(TEXT("Transient exact publisher/calibration actor spawn unavailable"));
        Lifetime.Emplace(Publisher);Lifetime.Emplace(Owner); Inputs.ExpectedWorldPackageName=World->GetOutermost()->GetName();
        TArray<UStaticMeshComponent*> Sources;Publisher->GetComponents(Sources);
        if(Sources.Num()!=15)return Refuse(TEXT("Actual spawned exact publisher15 mesh components required"));
        for(auto* Source:Sources) {
            if(!IsValid(Source) || !Source->IsRegistered() || Source->GetOwner()!=Publisher || Source->GetWorld()!=World || !IsValid(Source->GetStaticMesh().Get()))return Refuse(TEXT("Registered actual same-world original sources required"));
            Lifetime.Emplace(Source);Lifetime.Emplace(Source->GetStaticMesh().Get());
            for(const auto& M:Source->GetStaticMesh()->GetStaticMaterials())if(M.MaterialInterface)Lifetime.Emplace(M.MaterialInterface.Get());
            for(const auto& M:Source->OverrideMaterials)if(M)Lifetime.Emplace(M.Get());
            Source->SetSimulatePhysics(false);Source->SetEnableGravity(false);Source->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
        UStaticMesh* CargoMesh=LoadObject<UStaticMesh>(nullptr,*Inputs.ExactCargoObjectPath);
        if(!IsValid(CargoMesh) || CargoMesh->GetPathName()!=Inputs.ExactCargoObjectPath)return Refuse(TEXT("Real exact original City cargo visual unavailable; no proxy"));
        Lifetime.Emplace(CargoMesh);
        auto* Cargo=NewObject<UStaticMeshComponent>(Owner,NAME_None,RF_Transient);if(!Cargo)return Refuse(TEXT("Original cargo visual component unavailable"));
        Lifetime.Emplace(Cargo);Owner->AddInstanceComponent(Cargo);Cargo->SetStaticMesh(CargoMesh);Cargo->SetCollisionEnabled(ECollisionEnabled::NoCollision);Cargo->SetEnableGravity(false);Cargo->RegisterComponent();
        for(const auto& M:CargoMesh->GetStaticMaterials())if(M.MaterialInterface)Lifetime.Emplace(M.MaterialInterface.Get());
        FDDSOriginalRobotNativeGeometry Geometry;FString Placement,Error;
        if(!FDDSOriginalLiveIntake::Observe(*Publisher,*Cargo,Inputs,Geometry,Placement,Error))return Refuse(Error);
        // Accepted intake captures ALL16 raw source graphs before its first
        // resolution. Retain those exact receipts; never recapture after callbacks.
        const UClass* const PublisherClass=BP.GeneratedClass.Get();
        const FString WorldPath=World->GetPathName(),PublisherPath=Publisher->GetPathName(),OwnerPath=Owner->GetPathName();
        const auto PathsMatch=[](const auto& Pins,const TArray<FString>& Paths) {
            if(Pins.Num()!=Paths.Num())return false;
            for(int32 M=0;M<Pins.Num();++M)if(Pins[M].Path!=Paths[M])return false;
            return true;
        };
        const auto SourcesStillExact=[&]() {
            if(!bCombinedObservationActive || !IsInGameThread() || !IsValid(World) || World->bIsTearingDown || World->GetPathName()!=WorldPath
                || !IsValid(&BP) || BP.GeneratedClass.Get()!=PublisherClass || !IsValid(PublisherClass)
                || !IsValid(Publisher) || Publisher->IsActorBeingDestroyed() || Publisher->GetWorld()!=World || Publisher->GetClass()!=PublisherClass || Publisher->GetPathName()!=PublisherPath
                || !IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld()!=World || Owner->GetPathName()!=OwnerPath
                || Geometry.Parts.Num()!=16)return false;
            for(const auto& P:Geometry.Parts) {
                const auto& R=P.OriginalMaterialReceipt;
                if(!R || !R->bResolved || !R->Unchanged() || R->Component!=P.OriginalVisualSource || R->Mesh!=P.SuppliedMesh || R->World!=World
                    || R->ComponentPath!=P.ExactVisualSourceObjectPath || R->MeshPath!=P.ExactMeshObjectPath || !R->Pose.Equals(P.MeasuredWorldCm,0)
                    || !PathsMatch(R->Defaults,P.NativeObservedDefaultMaterialObjectPaths)
                    || !PathsMatch(R->RawOverrides,P.NativeObservedRawOverrideMaterialObjectPaths)
                    || !PathsMatch(R->SelectedRaw,P.NativeObservedSelectedRawMaterialObjectPaths)
                    || !PathsMatch(R->Effective,P.NativeObservedMaterialObjectPaths))return false;
            }return true;
        };
        if(!SourcesStillExact())return Refuse(TEXT("All16 retained original authored/effective source receipts required before owned preparation"));
        const FDDSOriginalRobotNativePart* Base=nullptr;
        for(const auto& P:Geometry.Parts)if(P.Role==EDDSOriginalRobotPartRole::Body && P.Index==0)Base=&P;
        if(!Base)return Refuse(TEXT("Observed exact base role unavailable"));
        const auto& SourceMaterial=*Base->OriginalMaterialReceipt;
        auto* C=NewObject<UDDSRobotLeasedMeshComponent>(Owner,NAME_None,RF_Transient);
        if(!SourcesStillExact() || !IsValid(C))return Refuse(TEXT("Reviewed leased component allocation/context unavailable"));
        Lifetime.Emplace(C);
        const FString ComponentPath=C->GetPathName();const FName ComponentName=C->GetFName();
        const FDelegateHandle AllocatedResourceDelegate=C->UStaticMeshComponent::OnGetNaniteResources().GetHandle();
        C->bDisallowNanite=SourceMaterial.bDisallowNanite;
        C->bForceDisableNanite=SourceMaterial.bForceDisableNanite;
        C->bForceNaniteForMasked=SourceMaterial.bForceNaniteForMasked;
#if WITH_EDITORONLY_DATA
        C->bDisplayNaniteFallbackMesh=SourceMaterial.bDisplayNaniteFallbackMesh;
#endif
        USceneComponent* ExpectedRoot=Owner->GetRootComponent();
        EComponentMobility::Type ExpectedMobility=C->Mobility;
        ECollisionEnabled::Type ExpectedCollision=C->GetCollisionEnabled();
        bool bExpectedGravity=C->IsGravityEnabled(),bExpectedSimulation=C->IsSimulatingPhysics();
        bool bMeshInstalled=false,bRegistered=false,bPoseInstalled=false,bCompoundPrepared=false;
        int32 InstalledOverrides=0;UBodySetup* ExpectedCompoundSetup=nullptr;
        TSharedPtr<FDDSOriginalMaterialReceipt> TargetMaterial;
        const auto TargetStillExact=[&]() {
            // Source/context check comes first after every owned callback, before
            // dereferencing the target or retaining newly returned objects.
            if(!SourcesStillExact() || !IsValid(C) || C->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed)
                || C->GetOuter()!=Owner || C->GetOwner()!=Owner || C->GetWorld()!=World || C->GetPathName()!=ComponentPath || C->GetFName()!=ComponentName
                || Owner->GetRootComponent()!=ExpectedRoot || C->GetAttachParent()!=nullptr || C->Mobility!=ExpectedMobility
                || C->IsRegistered()!=bRegistered || C->GetStaticMesh().Get()!=(bMeshInstalled?Base->SuppliedMesh:nullptr)
                || C->GetCollisionEnabled()!=ExpectedCollision || C->IsGravityEnabled()!=bExpectedGravity || C->IsSimulatingPhysics()!=bExpectedSimulation
                || bool(C->bDisallowNanite)!=SourceMaterial.bDisallowNanite || bool(C->bForceDisableNanite)!=SourceMaterial.bForceDisableNanite
                || bool(C->bForceNaniteForMasked)!=SourceMaterial.bForceNaniteForMasked
                || C->UStaticMeshComponent::OnGetNaniteResources().IsBound()
                || C->UStaticMeshComponent::OnGetNaniteResources().GetHandle()!=AllocatedResourceDelegate
                || C->OverrideMaterials.Num()!=InstalledOverrides)return false;
#if WITH_EDITORONLY_DATA
            if(bool(C->bDisplayNaniteFallbackMesh)!=SourceMaterial.bDisplayNaniteFallbackMesh)return false;
#endif
            for(int32 M=0;M<InstalledOverrides;++M)
                if(C->OverrideMaterials[M].Get()!=SourceMaterial.RawOverrides[M].Object)return false;
            if(bPoseInstalled && !C->GetComponentTransform().Equals(Base->MeasuredWorldCm,1.e-6))return false;
            if(bCompoundPrepared && (C->GetBodySetup()!=ExpectedCompoundSetup || !C->PreparedOriginalChassisCompoundUnchanged()))return false;
            return !TargetMaterial || TargetMaterial->Unchanged();
        };
        const auto Apply=[&](TFunctionRef<void()> Operation) {
            if(!TargetStillExact())return false;
            Operation();return TargetStillExact();
        };
        if(!Apply([&]() { Owner->AddInstanceComponent(C); })
            || !Apply([&]() { Owner->SetRootComponent(C);ExpectedRoot=C; })
            || !Apply([&]() { C->SetMobility(EComponentMobility::Movable);ExpectedMobility=EComponentMobility::Movable; })
            || !Apply([&]() { C->SetCollisionEnabled(ECollisionEnabled::NoCollision);ExpectedCollision=ECollisionEnabled::NoCollision; })
            || !Apply([&]() { C->SetEnableGravity(false);bExpectedGravity=false; })
            || !Apply([&]() { bMeshInstalled=C->SetStaticMesh(Base->SuppliedMesh); }) || !bMeshInstalled)
            return Refuse(TEXT("Original chassis source/context/policy changed during owned mesh installation"));
        // UE5.3.2 CopyBodyInstancePropertiesFrom accepts defaults ONLY. NoCollision
        // alone cannot prove this: always-create/world-trace may retain runtime state.
        const FBodyInstance& OriginalBodyInput=Base->OriginalVisualSource->BodyInstance;
        if(OriginalBodyInput.OwnerComponent.Get()!=nullptr || OriginalBodyInput.GetBodySetup()!=nullptr
           || FPhysicsInterface::IsValid(OriginalBodyInput.ActorHandle)
           || C->BodyInstance.OwnerComponent.Get()!=nullptr || C->BodyInstance.GetBodySetup()!=nullptr
           || FPhysicsInterface::IsValid(C->BodyInstance.ActorHandle))
            return Refuse(TEXT("Original source/destination BodyInstance retains runtime owner/setup/actor; default-only property copy refused before engine assertion (NoCollision is insufficient)"));
        if(!TargetStillExact())return Refuse(TEXT("Original chassis context changed before default-only BodyInstance copy"));
        C->BodyInstance.CopyBodyInstancePropertiesFrom(&OriginalBodyInput);C->BodyInstance.bAutoWeld=false;
        if(!TargetStillExact())return Refuse(TEXT("Original chassis context changed after default-only BodyInstance copy"));
        // Install authored raw overrides only. Supported SetMaterial preserves
        // exact nullptr/trailing slots; original mesh defaults remain unchanged.
        if(InstalledOverrides!=0)return Refuse(TEXT("Fresh chassis unexpectedly contains authored overrides"));
        for(int32 M=0;M<SourceMaterial.RawOverrides.Num();++M)
            if(!Apply([&]() { C->SetMaterial(M,SourceMaterial.RawOverrides[M].Object);InstalledOverrides=M+1; }))
                return Refuse(TEXT("All16 original source graphs or earlier authored chassis assignments changed during SetMaterial"));
        if(!TargetStillExact() || !C->PrepareOriginalChassisCompoundFromObservedGeometry(Geometry,Error))return Refuse(Error.IsEmpty()?TEXT("Original chassis context changed before compound preparation"):Error);
        if(!TargetStillExact())return Refuse(TEXT("All16 original source receipts or chassis context changed during compound preparation"));
        UBodySetup* const ActualSetup=C->GetBodySetup(); UBodySetup* const OriginalSetup=Base->SuppliedMesh->GetBodySetup();
        Lifetime.Emplace(ActualSetup);Lifetime.Emplace(OriginalSetup);
        if(!ActualSetup || ActualSetup==OriginalSetup || !C->PreparedOriginalChassisCompoundUnchanged())return Refuse(TEXT("Separate reviewed component compound setup required"));
        ExpectedCompoundSetup=ActualSetup;bCompoundPrepared=true;
        if(!TargetStillExact())return Refuse(TEXT("Original chassis material/compound state changed after preparation"));
        TArray<TSharedPtr<FJsonValue>> Origins;int32 Hulls=0;
        for(const auto& P:Geometry.Parts)if((P.Role==EDDSOriginalRobotPartRole::Body && P.Index==0) || P.Role==EDDSOriginalRobotPartRole::Suspension) {
            const auto* B=P.SuppliedMesh->GetBodySetup();if(!B)return Refuse(TEXT("Original hull trace unavailable"));
            const auto Rest=P.MeasuredWorldCm.GetRelativeTransform(Base->MeasuredWorldCm);
            for(int32 H=0;H<B->AggGeom.ConvexElems.Num();++H) { auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("partId"),P.SuppliedPartId);R->SetStringField(TEXT("meshObjectPath"),P.ExactMeshObjectPath);
                R->SetStringField(TEXT("meshPackageSha256"),P.MeshPackageSha256);R->SetStringField(TEXT("originalBodySetupGuid"),B->BodySetupGuid.ToString());R->SetNumberField(TEXT("sourceHullIndex"),H);
                R->SetObjectField(TEXT("sourceHullToChassisCm"),Pose(B->AggGeom.ConvexElems[H].GetTransform()*Rest));Origins.Add(MakeShared<FJsonValueObject>(R));++Hulls; }
        }
        if(Hulls<=0 || Hulls>128 || ActualSetup->AggGeom.ConvexElems.Num()!=Hulls)return Refuse(TEXT("Original hull trace/compound roster mismatch"));
        J->SetArrayField(TEXT("originalHullOrigins"),Origins);J->SetStringField(TEXT("originalMeshBodySetup"),OriginalSetup->GetPathName());J->SetStringField(TEXT("actualComponentBodySetup"),ActualSetup->GetPathName());
        J->SetNumberField(TEXT("legacyOriginalBaseConvexCount"),OriginalSetup->AggGeom.ConvexElems.Num());J->SetNumberField(TEXT("actualComponentCompoundConvexCount"),Hulls);
        J->SetStringField(TEXT("nativeGeometrySha256"),Geometry.NativeGeometryRecordSha256);J->SetStringField(TEXT("worldPlacementSha256"),Geometry.WorldPlacementRecordSha256);J->SetStringField(TEXT("actualWorldPlacementRecordJson"),Placement);
        const FTransform AuthoredComposedPose=Base->MeasuredWorldCm;UStaticMesh& Mesh=*Base->SuppliedMesh;
        if(!Apply([&]() { C->SetWorldTransform(AuthoredComposedPose);bPoseInstalled=true; })
            || !Apply([&]() { C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);ExpectedCollision=ECollisionEnabled::QueryAndPhysics; })
            || !Apply([&]() { C->SetEnableGravity(false);bExpectedGravity=false; })
            || !Apply([&]() { C->RegisterComponent();bRegistered=true; }))
            return Refuse(TEXT("All16 original material graphs or chassis context changed during authored pose/registration"));
        // Capture the registered target before resolving it, then validate the
        // complete transaction after every Nanite policy/material callback.
        if(!FDDSOriginalMaterialObservation::CaptureRaw(*C,TargetMaterial,Error)
            || !TargetStillExact() || !TargetMaterial->SameAuthoredAssignments(SourceMaterial)
            || !FDDSOriginalMaterialObservation::Resolve(*TargetMaterial,TargetStillExact,Error)
            || !TargetMaterial->SameEffectiveResults(SourceMaterial))
            return Refuse(Error.IsEmpty()?TEXT("Registered original chassis authored/effective receipt differs from source"):Error);
        if(!Apply([&]() { C->SetSimulatePhysics(true);bExpectedSimulation=true; }))
            return Refuse(TEXT("Original material/compound/context changed during dynamic body creation"));
        FBodyInstance* Body=C->GetBodyInstance();
        if(!C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World || C->GetStaticMesh().Get()!=&Mesh || C->GetBodySetup()!=ActualSetup
           || !C->PreparedOriginalChassisCompoundUnchanged() || !Body || !Body->IsValidBodyInstance() || !C->IsSimulatingPhysics() || C->IsGravityEnabled()
           || !C->GetComponentTransform().Equals(AuthoredComposedPose,1.e-6))return Refuse(TEXT("Coherent reviewed created dynamic compound body unavailable"));
        double Mass=0; FVector Inertia=FVector::ZeroVector; FTransform MassLocal, MassWorld, ActorPose;
        TArray<TSharedPtr<FJsonValue>> Shapes; bool ReadEntered=false, ShapeValid=true; int32 SimulationShapes=0;
        const bool Locked=FPhysicsCommand::ExecuteRead(Body->GetPhysicsActorHandle(),[&](const FPhysicsActorHandle& Actor)
        {
            ReadEntered=true;
            if (!FPhysicsInterface::IsValid(Actor) || !FPhysicsInterface::IsDynamic(Actor)) { ShapeValid=false; return; }
            Mass=FPhysicsInterface::GetMass_AssumesLocked(Actor);
            Inertia=FPhysicsInterface::GetLocalInertiaTensor_AssumesLocked(Actor);
            MassLocal=FPhysicsInterface::GetComTransformLocal_AssumesLocked(Actor);
            MassWorld=FPhysicsInterface::GetComTransform_AssumesLocked(Actor);
            ActorPose=FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor);
            TArray<FPhysicsShapeHandle> NativeShapes;
            if (FPhysicsInterface::GetNumShapes(Actor)<=0 || FPhysicsInterface::GetNumShapes(Actor)>128)
            { ShapeValid=false; return; }
            FPhysicsInterface::GetAllShapes_AssumedLocked(Actor,NativeShapes);
            if (NativeShapes.Num()<=0 || NativeShapes.Num()>128) { ShapeValid=false; return; }
            for (const auto& Shape:NativeShapes)
            {
                if (!Shape.IsValid()) { ShapeValid=false; return; }
                const auto& Geometry=Shape.GetGeometry();
                if (!Geometry.HasBoundingBox()) { ShapeValid=false; return; }
                const auto B=Geometry.BoundingBox(); const FBox Bounds(FVector(B.Min()),FVector(B.Max()));
                const FTransform Local=FPhysicsInterface::GetLocalTransform(Shape);
                if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || !FiniteTemplateTransform(Local))
                { ShapeValid=false; return; }
                auto Row=MakeShared<FJsonObject>();
                const bool Sim=FPhysicsInterface::IsSimulationShape(Shape); if (Sim) ++SimulationShapes;
                Row->SetBoolField(TEXT("simulationShape"),Sim);
                Row->SetNumberField(TEXT("engineShapeTypeEnum"),int32(FPhysicsInterface::GetShapeType(Shape)));
                Row->SetNumberField(TEXT("nativeImplicitGeometryTypeEnum"),int32(Geometry.GetType()));
                Row->SetObjectField(TEXT("nativeImplicitGeometryActorLocalBoundsCm"),Box(Bounds));
                Row->SetObjectField(TEXT("nativeShapeLocalTransformCm"),Pose(Local));
                Row->SetStringField(TEXT("boundsMeaning"),TEXT("actual native implicit collision geometry AABB, includes embedded geometry scaling/transforms; shape diagnostic ONLY, never substituted for COM/inertia"));
                Shapes.Add(MakeShared<FJsonValueObject>(Row));
            }
        });
        if (!Locked || !ReadEntered || !ShapeValid || SimulationShapes<=0 || !FMath::IsFinite(Mass) || Mass<=0
            || Inertia.ContainsNaN() || !FMath::IsFinite(Inertia.X) || !FMath::IsFinite(Inertia.Y) || !FMath::IsFinite(Inertia.Z)
            || Inertia.X<=0 || Inertia.Y<=0 || Inertia.Z<=0 || !FiniteTemplateTransform(MassLocal)
            || !FiniteTemplateTransform(MassWorld) || !FiniteTemplateTransform(ActorPose)
            || !C->IsRegistered() || C->GetOwner()!=Owner || C->GetWorld()!=World || C->GetStaticMesh().Get()!=&Mesh || !Body->IsValidBodyInstance() || C->GetBodySetup()!=ActualSetup || !C->PreparedOriginalChassisCompoundUnchanged() || C->IsGravityEnabled() || SimulationShapes!=Hulls || !TargetStillExact()
            || !TargetMaterial || !TargetMaterial->bResolved || !TargetMaterial->SameEffectiveResults(SourceMaterial)
            || !ActorPose.GetTranslation().Equals(AuthoredComposedPose.GetTranslation(),1.e-4)
            || !ActorPose.GetRotation().Equals(AuthoredComposedPose.GetRotation(),1.e-4))
        { return Refuse(TEXT("native coherent combined mass/COM/principal inertia/shapes/pose sample invalid or unavailable")); }
        J->SetNumberField(TEXT("nativeMassKg"),Mass); J->SetObjectField(TEXT("nativePrincipalInertiaKgCm2"),Vec(Inertia));
        J->SetObjectField(TEXT("nativePrincipalInertiaKgM2"),Vec(Inertia*1.e-4));
        J->SetObjectField(TEXT("nativeMassSpaceActorLocalCm"),Pose(MassLocal)); J->SetObjectField(TEXT("nativeMassSpaceWorldCm"),Pose(MassWorld));
        J->SetObjectField(TEXT("nativePrincipalAxisXActorLocal"),Vec(MassLocal.GetRotation().GetAxisX()));
        J->SetObjectField(TEXT("nativePrincipalAxisYActorLocal"),Vec(MassLocal.GetRotation().GetAxisY()));
        J->SetObjectField(TEXT("nativePrincipalAxisZActorLocal"),Vec(MassLocal.GetRotation().GetAxisZ()));
        J->SetObjectField(TEXT("nativeActorPoseCm"),Pose(ActorPose)); J->SetObjectField(TEXT("observedAuthoredSupplierScale"),Vec(C->GetComponentScale()));
        J->SetNumberField(TEXT("nativeSimulationShapeCount"),SimulationShapes); J->SetArrayField(TEXT("nativeShapes"),Shapes);
        J->SetStringField(TEXT("conditions"),TEXT("isolated transient world; exact live spawned original BP observation and immutable25 package pins; authored Base body/material inputs; one rigid Base+6suspension original convex game model; gravity disabled; no world tick/contact/original suspension kinematics/lid/drive/cargo fit/readiness proof"));
        J->SetNumberField(TEXT("originalMaterialReceiptSourceCount"),Geometry.Parts.Num());
        J->SetStringField(TEXT("materialObservation"),TEXT("all16 pre-resolution raw/authored source graphs retained; one chassis exact authored overrides/null layout/component Nanite policy; effective agreement observed after registration; final callback-free receipt checks"));
        J->SetBoolField(TEXT("nativeCombinedBodySampleAvailable"),true); return J;

    }

    TSharedRef<FJsonObject> MeshReport(UStaticMesh& Mesh)
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("loadedObjectPath"),Mesh.GetPathName());
        J->SetStringField(TEXT("classPath"),Mesh.GetClass()->GetClassPathName().ToString());
        // StaticMesh.cpp:3708-3715 shows GetBounds/GetBoundingBox are EXTENDED.
        // RenderData->Bounds is the separate raw renderable-mesh bound.
        const FStaticMeshRenderData* Render = Mesh.GetRenderData();
        J->SetBoolField(TEXT("renderDataPresent"),Render != nullptr);
        if (Render)
            J->SetObjectField(TEXT("rawRenderBoundsMeshLocal"),Bounds(Render->Bounds));
        J->SetObjectField(TEXT("extendedBoundsMeshLocal"),Bounds(Mesh.GetExtendedBounds()));
        J->SetObjectField(TEXT("negativeBoundsExtensionCm"),Vec(Mesh.GetNegativeBoundsExtension()));
        J->SetObjectField(TEXT("positiveBoundsExtensionCm"),Vec(Mesh.GetPositiveBoundsExtension()));
        J->SetNumberField(TEXT("lodCount"),Mesh.GetNumLODs());
        TArray<TSharedPtr<FJsonValue>> Lods;
        for (int32 L=0; L<Mesh.GetNumLODs(); ++L)
        {
            TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("index"),L);
            O->SetNumberField(TEXT("sections"),Mesh.GetNumSections(L));
            O->SetNumberField(TEXT("vertices"),Mesh.GetNumVertices(L));
            O->SetNumberField(TEXT("triangles"),Mesh.GetNumTriangles(L));
            Lods.Add(MakeShared<FJsonValueObject>(O));
        }
        J->SetArrayField(TEXT("lods"),Lods);
        TArray<TSharedPtr<FJsonValue>> Materials;
        for (const FStaticMaterial& M : Mesh.GetStaticMaterials())
        {
            TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("slot"),M.MaterialSlotName.ToString());
            O->SetStringField(TEXT("materialObjectPath"),
                M.MaterialInterface ? M.MaterialInterface->GetPathName() : TEXT(""));
            Materials.Add(MakeShared<FJsonValueObject>(O));
        }
        J->SetArrayField(TEXT("materialSlots"),Materials);
        TArray<TSharedPtr<FJsonValue>> Sockets;
        for (const UStaticMeshSocket* S : Mesh.Sockets)
        {
            if (!S) continue;
            TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"),S->SocketName.ToString());
            O->SetObjectField(TEXT("meshLocalLocationCm"),Vec(S->RelativeLocation));
            O->SetObjectField(TEXT("meshLocalRotationPitchYawRollDeg"),
                Vec(FVector(S->RelativeRotation.Pitch,S->RelativeRotation.Yaw,
                    S->RelativeRotation.Roll)));
            O->SetObjectField(TEXT("relativeScale"),Vec(S->RelativeScale));
            Sockets.Add(MakeShared<FJsonValueObject>(O));
        }
        J->SetArrayField(TEXT("sockets"),Sockets);
        TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
        const UBodySetup* Body = Mesh.GetBodySetup();
        Collision->SetBoolField(TEXT("bodySetupPresent"),Body != nullptr);
        if (Body)
        {
            Collision->SetNumberField(TEXT("traceFlagEnum"),(int32)Body->GetCollisionTraceFlag());
            const double EngineMass=Body->CalculateMass(nullptr);
            Collision->SetStringField(TEXT("engineMassModelProvenance"),TEXT("UBodySetup::CalculateMass(nullptr), mesh DefaultInstance and unit component scale; engine collision volume/density/mass-override model, NOT manufacturer measurement or simulated inertia"));
            Collision->SetBoolField(TEXT("engineMassModelFinitePositive"),FMath::IsFinite(EngineMass) && EngineMass>0);
            if (FMath::IsFinite(EngineMass)) Collision->SetNumberField(TEXT("engineMassModelUnitScaleKg"),EngineMass);
            Collision->SetNumberField(TEXT("boxCount"),Body->AggGeom.BoxElems.Num());
            Collision->SetNumberField(TEXT("sphereCount"),Body->AggGeom.SphereElems.Num());
            Collision->SetNumberField(TEXT("capsuleCount"),Body->AggGeom.SphylElems.Num());
            Collision->SetNumberField(TEXT("convexCount"),Body->AggGeom.ConvexElems.Num());
            Collision->SetNumberField(TEXT("taperedCapsuleCount"),Body->AggGeom.TaperedCapsuleElems.Num());
        }
        J->SetObjectField(TEXT("collision"),Collision);
        return J;
    }

    bool ReadVec3(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
        if (!O.IsValid() || !O->TryGetArrayField(Key,A) || !A || A->Num()!=3) return false;
        for (const TSharedPtr<FJsonValue>& V : *A)
            if (!V.IsValid() || V->Type != EJson::Number) return false;
        Out = FVector((*A)[0]->AsNumber(),(*A)[1]->AsNumber(),(*A)[2]->AsNumber());
        return !Out.ContainsNaN() && FMath::IsFinite(Out.X)
            && FMath::IsFinite(Out.Y) && FMath::IsFinite(Out.Z);
    }

    // Mesh-local sampled vertex envelope for caller-supplied hinge hypotheses.
    // It is not a physics sweep, reachable opening, or N12 door animation.
    TSharedRef<FJsonObject> SweepCandidate(const TSharedPtr<FJsonObject>& C,
        const TMap<FString,UStaticMesh*>& Meshes)
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetBoolField(TEXT("measured"),false);
        FString ObjectPath;
        FVector Pivot,Axis;
        double Start=0,End=0,Step=0;
        if (!C.IsValid() || !C->TryGetStringField(TEXT("doorObjectPath"),ObjectPath)
            || (ObjectPath!=ExactMeshPath(TEXT("SM_DeliveryRobot_Door1"))
                && ObjectPath!=ExactMeshPath(TEXT("SM_DeliveryRobot_Door2")))
            || !ReadVec3(C,TEXT("pivotMeshLocalCm"),Pivot)
            || !ReadVec3(C,TEXT("axisMeshLocal"),Axis)
            || !C->TryGetNumberField(TEXT("startDeg"),Start)
            || !C->TryGetNumberField(TEXT("endDeg"),End)
            || !C->TryGetNumberField(TEXT("stepDeg"),Step)
            || !FMath::IsFinite(Start) || !FMath::IsFinite(End)
            || !FMath::IsFinite(Step) || Step<=0 || End<Start
            || (End-Start)/Step>180 || Axis.IsNearlyZero())
        {
            J->SetStringField(TEXT("reason"),TEXT("invalid explicit door, pivot, axis or bounded angle range"));
            return J;
        }
        UStaticMesh* const* Found = Meshes.Find(ObjectPath);
        FMeshDescription* Description = Found && *Found ? (*Found)->GetMeshDescription(0) : nullptr;
        if (!Description || Description->Vertices().Num()==0)
        {
            J->SetStringField(TEXT("reason"),TEXT("LOD0 mesh description unavailable"));
            return J;
        }
        FStaticMeshAttributes Attributes(*Description);
        const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
        Axis.Normalize();
        TArray<TSharedPtr<FJsonValue>> Samples;
        FBox Swept(ForceInit);
        for (int32 I=0; I<=180; ++I)
        {
            const double Angle = FMath::Min(End,Start+I*Step);
            const FQuat R(Axis,FMath::DegreesToRadians(Angle));
            FBox AtAngle(ForceInit);
            for (const FVertexID V : Description->Vertices().GetElementIDs())
            {
                const FVector Local = FVector(Positions[V]);
                const FVector Rotated = Pivot + R.RotateVector(Local-Pivot);
                AtAngle += Rotated;
                Swept += Rotated;
            }
            TSharedRef<FJsonObject> Sample = Box(AtAngle);
            Sample->SetNumberField(TEXT("angleDeg"),Angle);
            Samples.Add(MakeShared<FJsonValueObject>(Sample));
            if (Angle >= End) break;
        }
        J->SetBoolField(TEXT("measured"),true);
        J->SetStringField(TEXT("doorObjectPath"),ObjectPath);
        J->SetStringField(TEXT("frame"),TEXT("door mesh-local, cm; Unreal FQuat(axis, angleRadians)"));
        J->SetObjectField(TEXT("pivotMeshLocalCm"),Vec(Pivot));
        J->SetObjectField(TEXT("axisMeshLocalUnit"),Vec(Axis));
        J->SetObjectField(TEXT("sweptAabbMeshLocalCm"),Box(Swept));
        J->SetArrayField(TEXT("samples"),Samples);
        return J;
    }
}

UDDSN05RobotGeometryCommandlet::UDDSN05RobotGeometryCommandlet()
{
    IsEditor = true;
    IsClient = false;
    IsServer = false;
    LogToConsole = true;
    UseCommandletResultAsExitCode = true;
}

int32 UDDSN05RobotGeometryCommandlet::Main(const FString& Params)
{
    const FEngineVersion& Engine = FEngineVersion::Current();
    if (!DDSN05Probe::IsReportIOSupported() || Engine.GetMajor()!=5
        || Engine.GetMinor()!=3 || Engine.GetPatch()!=2 || Engine.GetChangelist()!=29314046
        || Engine.IsLicenseeVersion())
    {
        UE_LOG(LogTemp,Error,TEXT("N05 probe requires pinned Win64 Editor UE 5.3.2-29314046"));
        return 8;
    }
    FString OutputPath, CandidatePath;
    if (!FParse::Value(*Params,TEXT("Output="),OutputPath)
        || FPaths::IsRelative(OutputPath))
    {
        UE_LOG(LogTemp,Error,TEXT("Provide absolute -Output=.../native-probe-runs/<run>/robot-geometry.json"));
        return 2;
    }
    OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
    const FString RunRoot = FPaths::Combine(FPaths::ProjectDir(),TEXT("ddscity"),
        TEXT("Evidence"),TEXT("N05"),TEXT("native-probe-runs"));
    const FString RunDir = FPaths::GetPath(OutputPath);
    const FString RunName = FPaths::GetCleanFilename(RunDir);
    bool bSafeRunName = RunName.Len()>=8 && RunName.Len()<=80;
    for (const TCHAR C : RunName)
        bSafeRunName &= (C>=TEXT('A') && C<=TEXT('Z'))
            || (C>=TEXT('a') && C<=TEXT('z'))
            || (C>=TEXT('0') && C<=TEXT('9'))
            || C==TEXT('_') || C==TEXT('-');
    if (!FPaths::IsSamePath(FPaths::GetPath(RunDir),RunRoot)
        || FPaths::GetCleanFilename(OutputPath)!=TEXT("robot-geometry.json")
        || !bSafeRunName || !DDSN05Probe::ReportEndpointAbsent(OutputPath)
        || !DDSN05Probe::HasNoReparseAncestor(RunDir))
    {
        UE_LOG(LogTemp,Error,TEXT("Output must be a new native-probe-runs/<safe-run>/robot-geometry.json file"));
        return 2;
    }
    FParse::Value(*Params,TEXT("Candidates="),CandidatePath);
    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("schema"),TEXT("DDSCity.N05.RobotNativeProbe.v4"));
    Report->SetStringField(TEXT("engine"),FEngineVersion::Current().ToString());
    Report->SetStringField(TEXT("project"),FPaths::GetProjectFilePath());
    Report->SetStringField(TEXT("packageRoot"),DDSN05Probe::Root);
    Report->SetStringField(TEXT("coordinateFrame"),
        TEXT("Unreal centimeters; X forward/Y right/Z up; raw/extended bounds are mesh-local; Blueprint component transforms are relative templates"));
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();
    TArray<FString> ScanPaths;
    ScanPaths.Add(DDSN05Probe::Root);
    Registry.ScanPathsSynchronous(ScanPaths,true);
    TArray<FAssetData> Assets;
    Registry.GetAssetsByPath(FName(DDSN05Probe::Root),Assets,true,false);
    Assets.Sort([](const FAssetData& A, const FAssetData& B)
        { return A.GetSoftObjectPath().ToString()<B.GetSoftObjectPath().ToString(); });
    TMap<FString,UStaticMesh*> LoadedMeshesByExactObjectPath;
    TMap<FString,TArray<FString>> MeshPathsByShortName;
    TArray<TSharedPtr<FJsonValue>> AssetRows;
    int32 MissingRobotDependencyCount=0, MeshLoadFailureCount=0, MeshIdentityMismatchCount=0;
    int32 UnresolvedDependencyQueryCount=0;
    for (const FAssetData& A : Assets)
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("packageName"),A.PackageName.ToString());
        Row->SetStringField(TEXT("objectPath"),A.GetSoftObjectPath().ToString());
        Row->SetStringField(TEXT("classPath"),A.AssetClassPath.ToString());
        TArray<FName> Dependencies;
        const bool bDependenciesQueried = Registry.GetDependencies(A.PackageName,
            Dependencies,UE::AssetRegistry::EDependencyCategory::Package);
        Dependencies.Sort(FNameLexicalLess());
        Row->SetBoolField(TEXT("dependencyQuerySucceeded"),bDependenciesQueried);
        Row->SetStringField(TEXT("dependencyQueryMeaning"),bDependenciesQueried
            ? TEXT("registry node answered; returned direct package list")
            : TEXT("unavailable node OR legitimate leaf; direct list not established"));
        if (!bDependenciesQueried) ++UnresolvedDependencyQueryCount;
        TArray<TSharedPtr<FJsonValue>> DepRows;
        for (const FName& D : Dependencies)
        {
            TSharedRef<FJsonObject> Dep = MakeShared<FJsonObject>();
            const FString Package = D.ToString();
            Dep->SetStringField(TEXT("packageName"),Package);
            TArray<FAssetData> Found;
            const bool bRegistryPresent = Registry.GetAssetsByPackageName(D,Found);
            Dep->SetBoolField(TEXT("registryPresent"),bRegistryPresent && Found.Num()>0);
            if (Package.StartsWith(DDSN05Probe::Root) && (!bRegistryPresent || Found.Num()==0))
                ++MissingRobotDependencyCount;
            DepRows.Add(MakeShared<FJsonValueObject>(Dep));
        }
        Row->SetArrayField(TEXT("directPackageDependencies"),DepRows);
        if (A.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
        {
            const FString ExactObjectPath = A.GetSoftObjectPath().ToString();
            MeshPathsByShortName.FindOrAdd(A.AssetName.ToString()).Add(ExactObjectPath);
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr,
                *ExactObjectPath);
            Row->SetBoolField(TEXT("loaded"),Mesh!=nullptr);
            const bool bIdentityMatches = DDSN05Probe::RecordLoadedIdentity(Mesh,
                ExactObjectPath,A.PackageName.ToString(),Row);
            if (bIdentityMatches)
            {
                LoadedMeshesByExactObjectPath.Add(ExactObjectPath,Mesh);
                Row->SetObjectField(TEXT("mesh"),DDSN05Probe::MeshReport(*Mesh));
            }
            else if (Mesh) ++MeshIdentityMismatchCount;
            else ++MeshLoadFailureCount;
        }
        AssetRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Report->SetArrayField(TEXT("assets"),AssetRows);
    Report->SetNumberField(TEXT("assetRegistryCount"),Assets.Num());
    Report->SetNumberField(TEXT("missingRobotRootDirectDependencies"),MissingRobotDependencyCount);
    Report->SetNumberField(TEXT("unresolvedDependencyQueries"),UnresolvedDependencyQueryCount);
    Report->SetBoolField(TEXT("dependencyClosureVerified"),false);
    Report->SetStringField(TEXT("dependencyClosureRule"),
        TEXT("direct registry queries only; independently reconcile migration closure and 25-file hash inventory"));
    Report->SetNumberField(TEXT("staticMeshLoadFailures"),MeshLoadFailureCount);
    Report->SetNumberField(TEXT("staticMeshIdentityMismatches"),MeshIdentityMismatchCount);
    TArray<TSharedPtr<FJsonValue>> ShortNameCollisions;
    for (const TPair<FString,TArray<FString>>& Pair : MeshPathsByShortName)
    {
        if (Pair.Value.Num()<2) continue;
        TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
        Collision->SetStringField(TEXT("shortName"),Pair.Key);
        TArray<TSharedPtr<FJsonValue>> Paths;
        for (const FString& Path : Pair.Value)
            Paths.Add(MakeShared<FJsonValueString>(Path));
        Collision->SetArrayField(TEXT("exactObjectPaths"),Paths);
        ShortNameCollisions.Add(MakeShared<FJsonValueObject>(Collision));
    }
    Report->SetArrayField(TEXT("meshShortNameCollisions"),ShortNameCollisions);
    const TCHAR* RequiredMeshes[] = {
        TEXT("SM_DeliveryRobot_Base"),TEXT("SM_DeliveryRobot_Wheel"),
        TEXT("SM_DeliveryRobot_Suspension"),TEXT("SM_DeliveryRobot_Door1"),
        TEXT("SM_DeliveryRobot_Door2"),TEXT("SM_DeliveryRobot_Opened")
    };
    TArray<TSharedPtr<FJsonValue>> MissingRequiredMeshes;
    int32 MissingRequiredRenderData=0;
    for (const TCHAR* Name : RequiredMeshes)
    {
        const FString ExactPath = DDSN05Probe::ExactMeshPath(Name);
        UStaticMesh* const* Required = LoadedMeshesByExactObjectPath.Find(ExactPath);
        if (!Required || !*Required)
            MissingRequiredMeshes.Add(MakeShared<FJsonValueString>(ExactPath));
        else if (!(*Required)->GetRenderData())
            ++MissingRequiredRenderData;
    }
    Report->SetArrayField(TEXT("missingRequiredMeshNames"),MissingRequiredMeshes);
    Report->SetNumberField(TEXT("missingRequiredRenderDataCount"),MissingRequiredRenderData);

    bool bSCSGraphValid=false;
    TArray<TSharedPtr<FJsonValue>> ComponentRows;
    UBlueprint* BP = LoadObject<UBlueprint>(nullptr,DDSN05Probe::BlueprintPath);
    Report->SetBoolField(TEXT("publisherBlueprintLoaded"),BP!=nullptr);
    TSharedRef<FJsonObject> BlueprintIdentity = MakeShared<FJsonObject>();
    const bool bBlueprintIdentityMatches = DDSN05Probe::RecordLoadedIdentity(BP,
        DDSN05Probe::BlueprintPath,DDSN05Probe::BlueprintPackage,BlueprintIdentity);
    Report->SetObjectField(TEXT("publisherBlueprintIdentity"),BlueprintIdentity);
    TArray<USCS_Node*> ValidatedSCSNodes; FString SCSPrewalkError;
    const bool bSCSPrewalkValid=bBlueprintIdentityMatches && BP->SimpleConstructionScript
        && DDSN05Probe::ReadBoundedSCSNodes(*BP->SimpleConstructionScript,ValidatedSCSNodes,SCSPrewalkError);
    if (bSCSPrewalkValid)
    {
        for (USCS_Node* N : ValidatedSCSNodes)
        {
            if (!N) continue;
            const UStaticMeshComponent* C = Cast<UStaticMeshComponent>(N->ComponentTemplate);
            if (!C) continue;
            const USCS_Node* ParentNode = BP->SimpleConstructionScript->FindParentNode(N);
            const USceneComponent* Parent = ParentNode ? Cast<USceneComponent>(ParentNode->ComponentTemplate) : nullptr;
            TSharedRef<FJsonObject> Row = DDSN05Probe::Transform(*C,
                Parent ? Parent->GetName() : TEXT(""));
            Row->SetStringField(TEXT("variableName"),N->GetVariableName().ToString());
            Row->SetStringField(TEXT("componentTemplate"),C->GetPathName());
            Row->SetStringField(TEXT("meshObjectPath"),
                C->GetStaticMesh() ? C->GetStaticMesh()->GetPathName() : TEXT(""));
            ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    Report->SetArrayField(TEXT("publisherBlueprintStaticMeshTemplates"),ComponentRows);
    if (bSCSPrewalkValid)
    {
        const auto Graph=DDSN05Probe::SCSGeometry(*BP->SimpleConstructionScript,ValidatedSCSNodes,bSCSGraphValid);
        Report->SetObjectField(TEXT("originalLocalSCSGeometry"),Graph);
        FString CargoCandidate; FParse::Value(*Params,TEXT("CargoCandidate="),CargoCandidate);
        if (bSCSGraphValid) Report->SetObjectField(TEXT("nativeCreatedOriginalBodyMeasurements"),
            DDSN05Probe::NativeOriginalBodyMeasurements(ValidatedSCSNodes,Graph,CargoCandidate,FParse::Param(*Params,TEXT("CargoBatch"))));
    }
    else
        Report->SetStringField(TEXT("originalLocalSCSPrewalkError"),SCSPrewalkError.IsEmpty()
            ? TEXT("original Blueprint/SCS unavailable") : SCSPrewalkError);
    bool bCombinedAvailable=true;
    if(FParse::Param(*Params,TEXT("CombinedChassis"))) {
        bCombinedAvailable=false;
        auto Combined=(bSCSGraphValid && IsValid(BP))?DDSN05Probe::NativeCombinedChassisMeasurement(Params,*BP):MakeShared<FJsonObject>();
        Combined->TryGetBoolField(TEXT("nativeCombinedBodySampleAvailable"),bCombinedAvailable);
        Report->SetObjectField(TEXT("nativeCombinedOriginalChassisMeasurement"),Combined);
    }
    Report->SetBoolField(TEXT("originalLocalSCSGraphValidated"),bSCSGraphValid);
    Report->SetStringField(TEXT("templateLimit"),
        TEXT("Blueprint SCS templates only; not a spawned or assembled DDSCity robot pose"));

    TArray<TSharedPtr<FJsonValue>> Sweeps;
    int32 FailedCandidateCount=0;
    if (!CandidatePath.IsEmpty())
    {
        FString Input;
        TSharedPtr<FJsonObject> CandidateRoot;
        if (!FPaths::IsRelative(CandidatePath)
            && FFileHelper::LoadFileToString(Input,*CandidatePath)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Input),CandidateRoot)
            && CandidateRoot.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Cases=nullptr;
            if (CandidateRoot->TryGetArrayField(TEXT("doorSweepCandidates"),Cases) && Cases)
                for (const TSharedPtr<FJsonValue>& V : *Cases)
                {
                    TSharedRef<FJsonObject> Result = DDSN05Probe::SweepCandidate(
                        V.IsValid() && V->Type==EJson::Object ? V->AsObject() : nullptr,
                        LoadedMeshesByExactObjectPath);
                    bool bMeasured=false;
                    if (!Result->TryGetBoolField(TEXT("measured"),bMeasured) || !bMeasured)
                        ++FailedCandidateCount;
                    Sweeps.Add(MakeShared<FJsonValueObject>(Result));
                }
            else
            {
                FailedCandidateCount=1;
                Report->SetStringField(TEXT("candidateInputError"),
                    TEXT("candidate JSON lacks doorSweepCandidates array"));
            }
        }
        else
        {
            FailedCandidateCount=1;
            Report->SetStringField(TEXT("candidateInputError"),
                TEXT("absolute candidate JSON failed to load or parse"));
        }
    }
    Report->SetArrayField(TEXT("doorSweepCandidates"),Sweeps);
    Report->SetNumberField(TEXT("failedCandidateCount"),FailedCandidateCount);
    Report->SetStringField(TEXT("sweepLimit"),
        TEXT("Caller-supplied mesh-local pivot/axis/angles; sampled LOD0 vertex AABB only, not physics, hinge proof or safe aperture"));
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(Report,Writer)) return 3;
    // Checked OS write/flush/close precedes same-directory no-replace rename.
    // This diagnostic protocol does not certify power-loss durability.
    if (!IFileManager::Get().MakeDirectory(*RunDir,true)
        || !DDSN05Probe::HasNoReparseAncestor(RunDir)
        || !DDSN05Probe::ReportEndpointAbsent(OutputPath)) return 4;
    const FString TempPath = OutputPath+TEXT(".")+FGuid::NewGuid().ToString(
        EGuidFormats::Digits)+TEXT(".tmp");
    if (!DDSN05Probe::WriteExclusiveFlushedTemp(TempPath,Json))
    {
        UE_LOG(LogTemp,Error,TEXT("N05 output failed; created temp retained if present: %s"),*TempPath);
        return 4;
    }
    if (!DDSN05Probe::HasNoReparseAncestor(RunDir)
        || !IFileManager::Get().Move(*OutputPath,*TempPath,
        false,false,false,true))
    {
        UE_LOG(LogTemp,Error,TEXT("N05 final no-replace move failed; temp retained: %s"),*TempPath);
        return 4;
    }
    UE_LOG(LogTemp,Display,TEXT("DDS_N05_ROBOT_PROBE output=%s assets=%d missingRobotDependencies=%d meshLoadFailures=%d"),
        *OutputPath,Assets.Num(),MissingRobotDependencyCount,MeshLoadFailureCount);
    if (Assets.Num()==0 || MissingRequiredMeshes.Num()>0 || MissingRequiredRenderData>0
        || MeshLoadFailureCount>0 || MeshIdentityMismatchCount>0 || ShortNameCollisions.Num()>0
        || !bBlueprintIdentityMatches || FailedCandidateCount>0) return 5;
    if (!bSCSGraphValid) return 9; // report retained; malformed local graph never completes successfully.
    if (!bCombinedAvailable) return 10; // opt-in observer refusal retains diagnostics, never returns success.
    if (MissingRobotDependencyCount>0) return 7;
    // A false GetDependencies can mean no cached node even for a leaf. The
    // geometry report is still useful, but direct dependency coverage is not
    // established and must not receive a zero-completeness result.
    if (UnresolvedDependencyQueryCount>0) return 6;
    return 0;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN05SCSCompositionControls,
    "DDSCity.N05.SCSCompositionControls", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDDSN05SCSCompositionControls::RunTest(const FString&)
{
    // Synthetic arithmetic controls only, never exported as original geometry.
    const FTransform Parent(FQuat(FVector::UpVector,UE_PI/2),FVector(10,20,30));
    const FTransform Child(FQuat::Identity,FVector(2,0,0));
    const FTransform Composed=Child*Parent;
    TestTrue(TEXT("child translation rotates through actual parent FTransform"),Composed.GetTranslation().Equals(FVector(10,22,30),1.e-6));
    TestTrue(TEXT("chassis-relative inversion recovers local pose"),Composed.GetRelativeTransform(Parent).Equals(Child,1.e-6));
    TestTrue(TEXT("finite authored composition admitted"),DDSN05Probe::FiniteTemplateTransform(Composed));
    TestFalse(TEXT("zero scale cannot invert to chassis"),DDSN05Probe::FiniteTemplateTransform(FTransform(FQuat::Identity,FVector::ZeroVector,FVector(0,1,1))));
    TestFalse(TEXT("invalid quaternion is refused"),DDSN05Probe::FiniteTemplateTransform(FTransform(FQuat(0,0,0,0),FVector::ZeroVector)));
    return true;
}
#endif
