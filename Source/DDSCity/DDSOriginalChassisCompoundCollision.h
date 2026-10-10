#pragma once
#include "CoreMinimal.h"
#include "DDSOriginalRobotRuntimeOwner.h"
#include "UObject/StrongObjectPtr.h"
#include "PhysicsEngine/BodySetup.h"
class UBodySetup;
struct FDDSOriginalCompoundHullOrigin
{
    FString OriginalPartId, OriginalMeshObjectPath, OriginalMeshPackageSha256;
    FGuid OriginalBodySetupGuid;
    int32 SourceHullIndex=INDEX_NONE;
    FTransform SourceHullToChassisCm=FTransform::Identity;
};
struct FDDSOriginalChassisCompoundResult
{
    // Retained transient geometry, not a binding/readiness/calibration permit.
    TStrongObjectPtr<UBodySetup> BodySetup;
    TArray<TStrongObjectPtr<UObject>> SourceLifetime;
    TArray<FDDSOriginalCompoundHullOrigin> HullOrigins;
    FString NativeSourceRecordSha256;
};
// Reuses exact original convex hull vertices/transforms. Current native roster
// is convex-only at unit supplier scale; other shapes/scales explicitly refuse.
// Caller must keep its admitted live-intake lifetime lease through this call.
DDSCITY_API bool DDSBuildOriginalChassisCompoundCollision(UObject& TransientOwner,
    const FDDSOriginalRobotNativeGeometry& ActualObservedOriginal,
    FDDSOriginalChassisCompoundResult& Out, FString& Error);
