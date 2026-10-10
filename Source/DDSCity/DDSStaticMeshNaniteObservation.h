#pragma once
#include "Engine/StaticMesh.h"

// UE5.3 strips authored mesh Nanite settings from non-editor targets.
// Runtime observations describe actual cooked render data, not authored flags.
namespace DDSStaticMeshNanite
{
inline bool AuthoredEnabledKnown()
{
#if WITH_EDITORONLY_DATA
    return true;
#else
    return false;
#endif
}
inline const TCHAR* ObservationBasis()
{
#if WITH_EDITORONLY_DATA
    return TEXT("AuthoredNaniteSettings.bEnabled");
#else
    return TEXT("ObservedCookedNaniteRenderData");
#endif
}
inline bool ObservationKnown(const UStaticMesh& Mesh)
{
#if WITH_EDITORONLY_DATA
    return true;
#else
    // Never wait on compiling render data or infer a non-Nanite mesh from absence.
    return !Mesh.IsCompiling() && Mesh.GetRenderData() != nullptr;
#endif
}
inline bool SourceEnabledOrCookedData(const UStaticMesh& Mesh)
{
#if WITH_EDITORONLY_DATA
    return Mesh.NaniteSettings.bEnabled;
#else
    return ObservationKnown(Mesh) && Mesh.HasValidNaniteData();
#endif
}
inline bool BuildEnabledOrCookedData(const UStaticMesh& Mesh)
{
#if WITH_EDITORONLY_DATA
    return Mesh.IsNaniteEnabled();
#else
    return ObservationKnown(Mesh) && Mesh.HasValidNaniteData();
#endif
}
}
