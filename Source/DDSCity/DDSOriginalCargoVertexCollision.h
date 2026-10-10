#pragma once
#include "CoreMinimal.h"
#include "DDSOriginalRobotRuntimeOwner.h"
#include "UObject/StrongObjectPtr.h"
#include "PhysicsEngine/BodySetup.h"
struct FDDSOriginalCargoVertexSourceReceipt;
struct FDDSOriginalCargoVertexCollisionResult
{
    TStrongObjectPtr<UBodySetup> BodySetup;
    TArray<TStrongObjectPtr<UObject>> SourceLifetime;
    TSharedPtr<const FDDSOriginalCargoVertexSourceReceipt> SourceReceipt;
    FString SourceVertexSha256, OriginalMeshPackageSha256, NativeGeometryRecordSha256;
    int32 SourceVertexCount=0;
    FTransform ObservedSourceWorldCm=FTransform::Identity;
};
// One component-local convex hull of exact loaded original LOD0 positions.
// Explicit authored GAME collision model; no supplied shape/mass/fit/readiness claim.
DDSCITY_API bool DDSBuildOriginalCargoVertexCollision(UObject& TransientOwner,
    const FDDSOriginalRobotNativeGeometry& ActualObservedOriginal,
    FDDSOriginalCargoVertexCollisionResult& Out,FString& Error);
DDSCITY_API bool DDSOriginalCargoVertexSourceUnchanged(
    const TSharedPtr<const FDDSOriginalCargoVertexSourceReceipt>& Receipt);
