#include "MAYAWheelAppearanceEvidence.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Chaos/Convex.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MayaWheelAppearance
{
    TArray<TSharedPtr<FJsonValue>> WheelVectorJson(const FVector& V)
    { return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)}; }

    TSharedRef<FJsonObject> WheelPoseJson(const FTransform& P)
    {
        auto J = MakeShared<FJsonObject>(); const FQuat Q = P.GetRotation();
        J->SetArrayField(TEXT("positionCm"), WheelVectorJson(P.GetLocation()));
        J->SetArrayField(TEXT("quaternion"), {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
            MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W)});
        J->SetArrayField(TEXT("scale"), WheelVectorJson(P.GetScale3D())); return J;
    }

    bool WheelValidBox(const FBox& B)
    { return B.IsValid && !B.Min.ContainsNaN() && !B.Max.ContainsNaN(); }

    TSharedRef<FJsonObject> WheelBoundsJson(const FBox& B)
    {
        auto J = MakeShared<FJsonObject>(); J->SetBoolField(TEXT("known"), WheelValidBox(B));
        if (WheelValidBox(B)) { J->SetArrayField(TEXT("minCm"), WheelVectorJson(B.Min)); J->SetArrayField(TEXT("maxCm"), WheelVectorJson(B.Max)); }
        return J;
    }

    double WheelMinimumPlaneDistance(const TArray<FVector>& Points, const FHitResult& H)
    {
        double D = TNumericLimits<double>::Max();
        for (const FVector& P : Points) D = FMath::Min(D, FVector::DotProduct(P - H.ImpactPoint, H.ImpactNormal));
        return D;
    }

    TSharedRef<FJsonObject> WheelHitJson(const FHitResult& H, bool bComplex, const TArray<FVector>& Visible, const TArray<FVector>& Cooked)
    {
        auto J = MakeShared<FJsonObject>(); auto* C = H.GetComponent(); const auto* A = H.GetActor();
        J->SetBoolField(TEXT("complexRequested"), bComplex);
        J->SetStringField(TEXT("actor"), A ? A->GetPathName() : TEXT(""));
        J->SetStringField(TEXT("component"), C ? C->GetPathName() : TEXT(""));
        J->SetBoolField(TEXT("componentVisible"), C && C->IsVisible());
        J->SetBoolField(TEXT("componentHiddenInGame"), C && C->bHiddenInGame);
        J->SetBoolField(TEXT("actorHidden"), A && A->IsHidden());
        J->SetArrayField(TEXT("pointCm"), WheelVectorJson(H.ImpactPoint)); J->SetArrayField(TEXT("normal"), WheelVectorJson(H.ImpactNormal));
        J->SetNumberField(TEXT("faceIndex"), H.FaceIndex); J->SetBoolField(TEXT("startPenetrating"), H.bStartPenetrating);
        if (auto* S = Cast<UStaticMeshComponent>(C))
        {
            J->SetStringField(TEXT("mesh"), S->GetStaticMesh() ? S->GetStaticMesh()->GetPathName() : TEXT(""));
            if (const auto* Setup = S->GetBodySetup()) J->SetNumberField(TEXT("collisionTraceFlag"), int32(Setup->CollisionTraceFlag));
        }
        if (!Visible.IsEmpty()) J->SetNumberField(TEXT("cosmeticVertexMinToHitPlaneCm"), WheelMinimumPlaneDistance(Visible, H));
        if (!Cooked.IsEmpty()) J->SetNumberField(TEXT("cookedConvexMinToHitPlaneCm"), WheelMinimumPlaneDistance(Cooked, H));
        return J;
    }
}

FString MAYAReadWheelAppearanceEvidence(UStaticMeshComponent* NativeWheel, UStaticMeshComponent* Cosmetic)
{
    using namespace MayaWheelAppearance;
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.WheelAppearanceNativeObservation.v1"));
    J->SetStringField(TEXT("scope"), TEXT("Current CPU render LOD if resident; exact cooked convex vertices at native body pose; diagnostic simple/complex visibility rays restricted to static mobility. Complex collision may be fallback geometry, not rendered Nanite triangles."));
    const bool bAvailable = IsValid(NativeWheel) && IsValid(Cosmetic) && NativeWheel->GetWorld()
        && NativeWheel->GetStaticMesh() && Cosmetic->GetStaticMesh();
    J->SetBoolField(TEXT("available"), bAvailable);
    if (bAvailable)
    {
        UWorld* World = NativeWheel->GetWorld(); UStaticMesh* VisualMesh = Cosmetic->GetStaticMesh();
        J->SetNumberField(TEXT("worldSeconds"), World->GetTimeSeconds());
        J->SetStringField(TEXT("nativeComponent"), NativeWheel->GetPathName());
        J->SetStringField(TEXT("cosmeticMesh"), VisualMesh->GetPathName());
        const FTransform VisualPose = Cosmetic->GetComponentTransform();
        J->SetObjectField(TEXT("cosmeticComponentPose"), WheelPoseJson(VisualPose));
        J->SetObjectField(TEXT("cosmeticMeshLocalBounds"), WheelBoundsJson(VisualMesh->GetBounds().GetBox()));
        J->SetObjectField(TEXT("cosmeticComponentWorldBounds"), WheelBoundsJson(Cosmetic->Bounds.GetBox()));
        TArray<FVector> VisiblePoints, CookedPoints;
        FBox VisualVertexBounds(ForceInit), CookedVertexBounds(ForceInit);
        // Installed StaticMeshResources.h requires a resident LOD + reader ref:
        // stream-out clears CPU data. Missing CPU data is explicitly unobserved.
        if (auto* RenderData = VisualMesh->GetRenderData())
        {
            const int32 LODIndex = int32(RenderData->CurrentFirstLODIdx);
            if (RenderData->LODResources.IsValidIndex(LODIndex))
            {
                TRefCountPtr<FStaticMeshLODResources> Reader(&RenderData->LODResources[LODIndex]);
                const FPositionVertexBuffer& Positions = Reader->VertexBuffers.PositionVertexBuffer;
                J->SetNumberField(TEXT("observedRenderLOD"), LODIndex);
                J->SetBoolField(TEXT("renderCPUAccessFlag"), Positions.GetAllowCPUAccess());
                if (Positions.GetVertexData() && Positions.GetNumVertices() > 0 && !VisualPose.ContainsNaN())
                {
                    VisiblePoints.Reserve(Positions.GetNumVertices());
                    for (uint32 I = 0; I < Positions.GetNumVertices(); ++I)
                    {
                        const FVector P = VisualPose.TransformPosition(FVector(Positions.VertexPosition(I)));
                        if (P.ContainsNaN()) { VisiblePoints.Reset(); break; }
                        VisiblePoints.Add(P); VisualVertexBounds += P;
                    }
                }
            }
        }
        J->SetBoolField(TEXT("renderVerticesKnown"), !VisiblePoints.IsEmpty());
        J->SetNumberField(TEXT("renderVertexCount"), VisiblePoints.Num());
        if (!VisiblePoints.IsEmpty()) J->SetObjectField(TEXT("renderVertexWorldBounds"), WheelBoundsJson(VisualVertexBounds));

        const FBodyInstance* BI = NativeWheel->GetBodyInstance();
        const bool bNativeBodyKnown = BI && BI->IsValidBodyInstance();
        J->SetBoolField(TEXT("nativeBodyKnown"), bNativeBodyKnown);
        if (bNativeBodyKnown)
        {
            // Force actual global pose rather than an interpolated/projected pose.
            FTransform NativePose = BI->GetUnrealWorldTransform(false, true);
            NativePose.SetScale3D(NativeWheel->GetComponentScale());
            J->SetObjectField(TEXT("nativeBodyPoseWithComponentScale"), WheelPoseJson(NativePose));
            J->SetObjectField(TEXT("nativeActorWorldBounds"), WheelBoundsJson(BI->GetBodyBounds()));
            if (const auto* Setup = NativeWheel->GetBodySetup(); Setup && !NativePose.ContainsNaN())
                for (const FKConvexElem& Elem : Setup->AggGeom.ConvexElems)
                    if (const auto& Convex = Elem.GetChaosConvexMesh(); Convex.IsValid())
                        for (int32 I = 0; I < Convex->NumVertices(); ++I)
                        {
                            const FVector P = NativePose.TransformPosition(Elem.GetTransform().TransformPosition(FVector(Convex->GetVertex(I))));
                            if (!P.ContainsNaN()) { CookedPoints.Add(P); CookedVertexBounds += P; }
                        }
        }
        J->SetNumberField(TEXT("cookedConvexVertexCount"), CookedPoints.Num());
        if (!CookedPoints.IsEmpty()) J->SetObjectField(TEXT("cookedConvexWorldBounds"), WheelBoundsJson(CookedVertexBounds));

        const FVector Center = NativeWheel->GetComponentLocation();
        const FVector Start = Center + FVector(0, 0, 40), End = Center - FVector(0, 0, 160);
        J->SetArrayField(TEXT("rayStartCm"), WheelVectorJson(Start)); J->SetArrayField(TEXT("rayEndCm"), WheelVectorJson(End));
        J->SetStringField(TEXT("rayChannel"), TEXT("ECC_Visibility"));
        J->SetStringField(TEXT("rayMobility"), TEXT("Static"));
        for (int32 Complex = 0; Complex < 2; ++Complex)
        {
            FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYAWheelAppearanceStaticGround), Complex != 0);
            Query.MobilityType = EQueryMobilityType::Static;
            Query.bReturnFaceIndex = true; Query.AddIgnoredActor(NativeWheel->GetOwner());
            TArray<TSharedPtr<FJsonValue>> Hits;
            bool bCapReached = true;
            for (int32 Layer = 0; Layer < 8; ++Layer)
            {
                FHitResult H;
                if (!World->LineTraceSingleByChannel(H, Start, End, ECC_Visibility, Query)) { bCapReached = false; break; }
                if (!H.GetComponent() || H.ImpactPoint.ContainsNaN() || H.ImpactNormal.ContainsNaN()) { bCapReached = false; break; }
                Hits.Add(MakeShared<FJsonValueObject>(WheelHitJson(H, Complex != 0, VisiblePoints, CookedPoints)));
                // Only this diagnostic ray excludes its last reported component
                // to inspect underlying geometry. Physical collision is unchanged.
                Query.AddIgnoredComponent(H.GetComponent());
            }
            J->SetArrayField(Complex ? TEXT("complexStaticHitChain") : TEXT("simpleStaticHitChain"), Hits);
            J->SetBoolField(Complex ? TEXT("complexRayHitCapReached") : TEXT("simpleRayHitCapReached"), bCapReached);
        }
    }
    FString Out;
    FJsonSerializer::Serialize(J, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
    return Out;
}
