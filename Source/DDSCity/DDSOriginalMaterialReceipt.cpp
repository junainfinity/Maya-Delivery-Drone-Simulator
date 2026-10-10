#include "DDSOriginalMaterialReceipt.h"
#include "DDSStaticMeshNaniteObservation.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "Rendering/NaniteResources.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialDomain.h"
#include "UObject/Package.h"
#include "Misc/App.h"
#include "HAL/IConsoleManager.h"

namespace
{
constexpr int32 MaxMaterialSlots = 64, MaxChainNodes = 256, MaxChainDepth = 32;
bool MaterialObjectLive(const UObject* Object)
{
    return IsValid(Object) && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad
        | RF_NeedPostLoadSubobjects | RF_BeginDestroyed | RF_FinishDestroyed);
}
FDDSOriginalMaterialReceipt::FMaterialPin PinMaterial(UMaterialInterface* Material)
{
    FDDSOriginalMaterialReceipt::FMaterialPin Pin;
    Pin.Object = Material; Pin.Path = Material ? Material->GetPathName() : FString(); return Pin;
}
bool SameMaterial(const FDDSOriginalMaterialReceipt::FMaterialPin& Pin, UMaterialInterface* Actual)
{
    return Actual == Pin.Object && (!Actual || (MaterialObjectLive(Actual) && Actual->GetPathName() == Pin.Path));
}
bool ReadChain(UMaterialInterface* Material, FDDSOriginalMaterialReceipt::FChainPin& Out)
{
    if (!MaterialObjectLive(Material)) return false;
    Out.Material = PinMaterial(Material); Out.NodeClass = Material->GetClass();
    Out.Package = Material->GetOutermost();
    if (!MaterialObjectLive(Out.NodeClass) || !MaterialObjectLive(Out.Package)) return false;
    Out.ClassPath = Out.NodeClass->GetPathName(); Out.PackagePath = Out.Package->GetPathName();
#if WITH_EDITORONLY_DATA
    Out.bCookedForEditor = Out.Package->bIsCookedForEditor;
#endif
    // Only the installed native material implementations below have the finite
    // cached admission inputs used here. Opaque custom virtuals are refused.
    if (Out.NodeClass != UMaterial::StaticClass() && Out.NodeClass != UMaterialInstance::StaticClass()
        && Out.NodeClass != UMaterialInstanceConstant::StaticClass()
        && Out.NodeClass != UMaterialInstanceDynamic::StaticClass()) return false;
    if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
    {
        Out.bInstance = true; Out.bDynamicInstance = Out.NodeClass == UMaterialInstanceDynamic::StaticClass();
        Out.Parent = Instance->Parent.Get();
        Out.bEnableOverride = Instance->NaniteOverrideMaterial.bEnableOverride;
        Out.NaniteOverride = Instance->NaniteOverrideMaterial.GetOverrideMaterial();
        // Direct cached fields. MID's virtual getters dispatch through Parent;
        // its dynamic kind and complete finite Parent graph are pinned instead.
        Out.RawBlendMode = static_cast<int32>(Instance->BlendMode.GetValue());
        Out.ShadingModelField = Instance->ShadingModels.GetShadingModelField();
        return true;
    }
    if (const UMaterial* Base = Cast<UMaterial>(Material))
    {
        Out.bEnableOverride = Base->NaniteOverrideMaterial.bEnableOverride;
        Out.NaniteOverride = Base->NaniteOverrideMaterial.GetOverrideMaterial();
        Out.MaterialDomain = static_cast<int32>(Base->MaterialDomain.GetValue());
        Out.RawBlendMode = static_cast<int32>(Base->BlendMode.GetValue());
        if (Out.MaterialDomain < MD_Surface || Out.MaterialDomain >= MD_MAX) return false;
        // Installed Material.cpp:6539 reads domain/private cached shading only;
        // class qualification prevents virtual dispatch or Parent traversal.
        Out.ShadingModelField = Base->UMaterial::GetShadingModels().GetShadingModelField();
        Out.bMaskedAssumedOpaque = Base->bCanMaskedBeAssumedOpaque;
        Out.bSky = Base->bIsSky; Out.bUsedWithNanite = Base->bUsedWithNanite;
        Out.bSpecialEngineMaterial = Base->bUsedAsSpecialEngineMaterial;
        Out.bAutomaticallySetUsage = Base->bAutomaticallySetUsageInEditor;
        Out.NaniteUsageWarning = Base->UsageFlagWarnings & (1u << static_cast<uint32>(MATUSAGE_Nanite));
        return true;
    }
    return false;
}
bool SameChain(const FDDSOriginalMaterialReceipt::FChainPin& A,
    const FDDSOriginalMaterialReceipt::FChainPin& B)
{
    return A.Parent == B.Parent && A.NaniteOverride == B.NaniteOverride
        && A.bInstance == B.bInstance && A.bEnableOverride == B.bEnableOverride
        && A.NodeClass == B.NodeClass && A.Package == B.Package
        && A.ClassPath == B.ClassPath && A.PackagePath == B.PackagePath
        && A.bDynamicInstance == B.bDynamicInstance && A.MaterialDomain == B.MaterialDomain
        && A.RawBlendMode == B.RawBlendMode && A.ShadingModelField == B.ShadingModelField
        && A.NaniteUsageWarning == B.NaniteUsageWarning && A.bMaskedAssumedOpaque == B.bMaskedAssumedOpaque
        && A.bSky == B.bSky && A.bUsedWithNanite == B.bUsedWithNanite
        && A.bSpecialEngineMaterial == B.bSpecialEngineMaterial
        && A.bAutomaticallySetUsage == B.bAutomaticallySetUsage && A.bCookedForEditor == B.bCookedForEditor;
}
bool CaptureChain(UMaterialInterface* Material, FDDSOriginalMaterialReceipt& Receipt,
    TSet<UMaterialInterface*>& Visiting, int32 Depth)
{
    if (!Material) return true;
    if (Depth >= MaxChainDepth || Visiting.Contains(Material)) return false;
    for (const auto& Pin : Receipt.Chains) if (Pin.Material.Object == Material) return true;
    if (Receipt.Chains.Num() >= MaxChainNodes || !MaterialObjectLive(Material)) return false;
    Receipt.Lifetime.Emplace(Material); // Retain before inspecting any further edge.
    FDDSOriginalMaterialReceipt::FChainPin Pin;
    if (!ReadChain(Material, Pin)) return false;
    Receipt.Lifetime.Emplace(Pin.NodeClass); Receipt.Lifetime.Emplace(Pin.Package);
    Receipt.Chains.Add(Pin); Visiting.Add(Material);
    const bool Valid = CaptureChain(Pin.Parent, Receipt, Visiting, Depth + 1)
        && CaptureChain(Pin.NaniteOverride, Receipt, Visiting, Depth + 1);
    Visiting.Remove(Material); return Valid;
}
UMaterialInterface* CachedNaniteOverride(const FDDSOriginalMaterialReceipt& Receipt, UMaterialInterface* Raw)
{
    for (int32 Depth = 0; Raw && Depth < MaxChainDepth; ++Depth)
    {
        const auto* Pin = Receipt.Chains.FindByPredicate([Raw](const auto& Node) { return Node.Material.Object == Raw; });
        if (!Pin) return nullptr;
        // UE5.3 UMaterial ignores bEnableOverride; MI stops at an enabled edge,
        // including an enabled nullptr edge, otherwise it traverses Parent.
        if (!Pin->bInstance || Pin->bEnableOverride) return Pin->NaniteOverride;
        Raw = Pin->Parent;
    }
    return nullptr;
}
bool SameLayout(const TArray<FDDSOriginalMaterialReceipt::FMaterialPin>& A,
    const TArray<FDDSOriginalMaterialReceipt::FMaterialPin>& B)
{
    if (A.Num() != B.Num()) return false;
    for (int32 I = 0; I < A.Num(); ++I)
        if (A[I].Object != B[I].Object || A[I].Path != B[I].Path) return false;
    return true;
}
bool MaterialResolverActive = false; // GT-only; rejects callback reentry across receipts.
}

bool FDDSOriginalMaterialObservation::CaptureRaw(UStaticMeshComponent& Component,
    TSharedPtr<FDDSOriginalMaterialReceipt>& Out, FString& Error)
{
    Out.Reset();
    const auto Fail = [&](const TCHAR* Why) { Error = Why; return false; };
    if (!IsInGameThread() || MaterialResolverActive || !MaterialObjectLive(&Component))
        return Fail(TEXT("Original material capture wrong thread/reentry/unloaded component"));
    auto Receipt = MakeShared<FDDSOriginalMaterialReceipt>();
    Receipt->bEditorUsagePolicy = GIsEditor; Receipt->bGameUsagePolicy = FApp::IsGame();
    Receipt->Component = &Component; Receipt->Mesh = Component.GetStaticMesh().Get();
    Receipt->Owner = Component.GetOwner(); Receipt->World = Component.GetWorld();
    Receipt->Archetype = Component.GetArchetype();
    if (!MaterialObjectLive(Receipt->Mesh) || Receipt->Mesh->IsCompiling()
        || !MaterialObjectLive(Receipt->Owner) || Receipt->Owner->IsActorBeingDestroyed()
        || !MaterialObjectLive(Receipt->World) || Receipt->World->bIsTearingDown
        || !MaterialObjectLive(Receipt->Archetype))
        return Fail(TEXT("Original material source mesh/owner/world/archetype unsettled"));
    Receipt->WorldSettings = Receipt->World->GetWorldSettings();
    if (!MaterialObjectLive(Receipt->WorldSettings)) return Fail(TEXT("Original material world settings unavailable"));
    Receipt->Lifetime.Emplace(&Component); Receipt->Lifetime.Emplace(Receipt->Mesh);
    Receipt->Lifetime.Emplace(Receipt->Owner); Receipt->Lifetime.Emplace(Receipt->World);
    Receipt->Lifetime.Emplace(Receipt->Archetype); Receipt->Lifetime.Emplace(Receipt->WorldSettings);
    Receipt->ComponentName = Component.GetFName(); Receipt->ComponentPath = Component.GetPathName();
    Receipt->MeshPath = Receipt->Mesh->GetPathName(); Receipt->OwnerPath = Receipt->Owner->GetPathName();
    Receipt->WorldPath = Receipt->World->GetPathName(); Receipt->ArchetypePath = Receipt->Archetype->GetPathName();
    Receipt->Pose = Component.GetComponentTransform(); Receipt->bRegistered = Component.IsRegistered();
    Receipt->Scene = Component.GetScene();
    Receipt->bNaniteObservationKnown = DDSStaticMeshNanite::ObservationKnown(*Receipt->Mesh);
    Receipt->bAuthoredNaniteEnabledKnown = DDSStaticMeshNanite::AuthoredEnabledKnown();
    Receipt->NaniteObservationBasis = DDSStaticMeshNanite::ObservationBasis();
    Receipt->bNaniteEnabled = DDSStaticMeshNanite::SourceEnabledOrCookedData(*Receipt->Mesh);
    Receipt->bDisallowNanite = Component.bDisallowNanite;
    Receipt->bForceDisableNanite = Component.bForceDisableNanite;
    Receipt->bForceNaniteForMasked = Component.bForceNaniteForMasked;
#if WITH_EDITORONLY_DATA
    Receipt->bDisplayNaniteFallbackMesh = Component.bDisplayNaniteFallbackMesh;
#endif
    Receipt->bWorldAllowsMasked = Receipt->WorldSettings->NaniteSettings.bAllowMaskedMaterials;
    // Arbitrary bound resource callbacks do not expose callback-free output
    // receipts. Actual original components use their settled mesh resources.
    if (Component.UStaticMeshComponent::OnGetNaniteResources().IsBound())
        return Fail(TEXT("Original material receipt does not admit a bound Nanite resource delegate"));
    Receipt->NaniteResourceDelegate = Component.UStaticMeshComponent::OnGetNaniteResources().GetHandle();
    Receipt->WorldFeatureLevel = static_cast<int32>(Receipt->World->GetFeatureLevel());
    // IsCompiling includes every async property lock in installed UE5.3; with
    // no locks this supported accessor is a raw pointer read, not a wait callback.
    const auto* RenderData = Receipt->Mesh->GetRenderData();
    Receipt->RenderData = RenderData;
    const auto* Resources = RenderData ? RenderData->NaniteResourcesPtr.Get() : nullptr;
    Receipt->NaniteResources = Resources;
    Receipt->NanitePageCount = Resources ? Resources->PageStreamingStates.Num() : 0;
    if (Receipt->Pose.ContainsNaN() || !Receipt->Pose.GetRotation().IsNormalized())
        return Fail(TEXT("Original material source pose invalid"));
    for (const TCHAR* Name : { TEXT("r.Nanite"), TEXT("r.Nanite.MaterialOverrides"), TEXT("r.Nanite.AllowMaskedMaterials") })
    {
        FDDSOriginalMaterialReceipt::FConsolePin Pin; Pin.Name = Name;
        Pin.Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        if (!Pin.Variable) return Fail(TEXT("Original Nanite material policy variable unavailable"));
        Pin.Value = Pin.Variable->GetInt(); Receipt->ConsolePolicy.Add(Pin);
    }
    const auto& Defaults = Receipt->Mesh->GetStaticMaterials();
    if (Defaults.Num() > MaxMaterialSlots || Component.OverrideMaterials.Num() > MaxMaterialSlots)
        return Fail(TEXT("Original material slot budget exceeded"));
    TSet<UMaterialInterface*> Visiting;
    for (const auto& Slot : Defaults)
    {
        auto* Raw = Slot.MaterialInterface.Get();
        if (!CaptureChain(Raw, *Receipt, Visiting, 0)) return Fail(TEXT("Original default material chain unloaded/cyclic/over budget"));
        Receipt->Defaults.Add(PinMaterial(Raw)); Receipt->DefaultSlotNames.Add(Slot.MaterialSlotName);
    }
    for (const auto& Slot : Component.OverrideMaterials)
    {
        auto* Raw = Slot.Get();
        if (!CaptureChain(Raw, *Receipt, Visiting, 0)) return Fail(TEXT("Original override material chain unloaded/cyclic/over budget"));
        Receipt->RawOverrides.Add(PinMaterial(Raw));
    }
    for (int32 I = 0; I < Defaults.Num(); ++I)
        Receipt->SelectedRaw.Add(Receipt->RawOverrides.IsValidIndex(I) && Receipt->RawOverrides[I].Object
            ? Receipt->RawOverrides[I] : Receipt->Defaults[I]);
    if (!Receipt->Unchanged()) return Fail(TEXT("Original raw material receipt changed during capture"));
    // Audit null slots and null Parent paths use SurfaceDefault. This supported
    // lookup may initialize defaults, so it occurs during pre-resolution capture,
    // under a reentry guard, and the source receipt is checked before pointer reuse.
    // Callers check ALL captured source receipts before the first explicit resolver.
    UMaterial* Default = nullptr;
    {
        TGuardValue<bool> CaptureGuard(MaterialResolverActive, true);
        Default = UMaterial::GetDefaultMaterial(MD_Surface);
    }
    if (!Receipt->Unchanged()) return Fail(TEXT("Original raw source changed during SurfaceDefault capture"));
    if (!MaterialObjectLive(Default)) return Fail(TEXT("Original SurfaceDefault material unsettled"));
    Receipt->Lifetime.Emplace(Default); Receipt->SurfaceDefault = PinMaterial(Default);
    if (!CaptureChain(Default, *Receipt, Visiting, 0)) return Fail(TEXT("Original SurfaceDefault graph unloaded/cyclic/over budget"));
    if (!Receipt->Unchanged()) return Fail(TEXT("Original admission receipt changed during SurfaceDefault graph capture"));
    Out = MoveTemp(Receipt); return true;
}

bool FDDSOriginalMaterialReceipt::Unchanged(bool bCheckPose) const
{
    if (!IsInGameThread() || GIsEditor != bEditorUsagePolicy || FApp::IsGame() != bGameUsagePolicy
        || !SameMaterial(SurfaceDefault, SurfaceDefault.Object)
        || !MaterialObjectLive(Component) || !MaterialObjectLive(Mesh)
        || Mesh->IsCompiling() || !MaterialObjectLive(Owner) || Owner->IsActorBeingDestroyed()
        || !MaterialObjectLive(World) || World->bIsTearingDown || !MaterialObjectLive(Archetype)
        || !MaterialObjectLive(WorldSettings) || Component->GetOwner() != Owner
        || Component->GetWorld() != World || Owner->GetWorld() != World
        || Component->GetStaticMesh().Get() != Mesh || Component->GetArchetype() != Archetype
        || World->GetWorldSettings() != WorldSettings || Component->GetScene() != Scene
        || Component->IsRegistered() != bRegistered || Component->GetFName() != ComponentName
        || Component->GetPathName() != ComponentPath || Mesh->GetPathName() != MeshPath
        || Owner->GetPathName() != OwnerPath || World->GetPathName() != WorldPath
        || Archetype->GetPathName() != ArchetypePath
        || (bCheckPose && !Component->GetComponentTransform().Equals(Pose, 1.e-6))
        || DDSStaticMeshNanite::ObservationKnown(*Mesh) != bNaniteObservationKnown
        || DDSStaticMeshNanite::AuthoredEnabledKnown() != bAuthoredNaniteEnabledKnown
        || NaniteObservationBasis != DDSStaticMeshNanite::ObservationBasis()
        || DDSStaticMeshNanite::SourceEnabledOrCookedData(*Mesh) != bNaniteEnabled
        || bool(Component->bDisallowNanite) != bDisallowNanite
        || bool(Component->bForceDisableNanite) != bForceDisableNanite
        || bool(Component->bForceNaniteForMasked) != bForceNaniteForMasked
        || WorldSettings->NaniteSettings.bAllowMaskedMaterials != bWorldAllowsMasked
        || Component->UStaticMeshComponent::OnGetNaniteResources().IsBound()
        || Component->UStaticMeshComponent::OnGetNaniteResources().GetHandle() != NaniteResourceDelegate
        || static_cast<int32>(World->GetFeatureLevel()) != WorldFeatureLevel)
        return false;
#if WITH_EDITORONLY_DATA
    if (bool(Component->bDisplayNaniteFallbackMesh) != bDisplayNaniteFallbackMesh) return false;
#endif
    const auto* CurrentRenderData = Mesh->GetRenderData(); // IsCompiling false above; no async property wait.
    if (CurrentRenderData != RenderData) return false;
    const auto* CurrentResources = CurrentRenderData ? CurrentRenderData->NaniteResourcesPtr.Get() : nullptr;
    if (CurrentResources != NaniteResources
        || (CurrentResources ? CurrentResources->PageStreamingStates.Num() : 0) != NanitePageCount) return false;
    for (const auto& Pin : ConsolePolicy)
        if (IConsoleManager::Get().FindConsoleVariable(*Pin.Name) != Pin.Variable
            || !Pin.Variable || Pin.Variable->GetInt() != Pin.Value) return false;
    const auto& CurrentDefaults = Mesh->GetStaticMaterials();
    if (CurrentDefaults.Num() != Defaults.Num() || DefaultSlotNames.Num() != Defaults.Num()
        || Component->OverrideMaterials.Num() != RawOverrides.Num()) return false;
    for (int32 I = 0; I < Defaults.Num(); ++I)
        if (CurrentDefaults[I].MaterialSlotName != DefaultSlotNames[I]
            || !SameMaterial(Defaults[I], CurrentDefaults[I].MaterialInterface.Get())) return false;
    for (int32 I = 0; I < RawOverrides.Num(); ++I)
        if (!SameMaterial(RawOverrides[I], Component->OverrideMaterials[I].Get())) return false;
    for (const auto& Pin : Chains)
    {
        FDDSOriginalMaterialReceipt::FChainPin Current;
        if (!SameMaterial(Pin.Material, Pin.Material.Object) || !ReadChain(Pin.Material.Object, Current)
            || !SameChain(Pin, Current)) return false;
    }
    for (const auto& Pin : Effective) if (!SameMaterial(Pin, Pin.Object)) return false;
    return true; // raw and cached reads only, including policy and parent/override edges.
}

bool FDDSOriginalMaterialObservation::Resolve(FDDSOriginalMaterialReceipt& Receipt,
    TFunctionRef<bool()> ValidateWholeTransaction, FString& Error)
{
    const auto Fail = [&](const TCHAR* Why) { Error = Why; return false; };
    if (!IsInGameThread() || MaterialResolverActive || Receipt.bResolved
        || !ValidateWholeTransaction() || !Receipt.Unchanged())
        return Fail(TEXT("Original material resolution reentry/unsettled transaction refused"));
    TGuardValue<bool> Guard(MaterialResolverActive, true);
    // This policy query can audit materials or execute a resource delegate.
    const bool UseNanite = Receipt.Component->UseNaniteOverrideMaterials();
    if (!ValidateWholeTransaction() || !Receipt.Unchanged())
        return Fail(TEXT("Original material transaction changed during Nanite policy callback"));
    Receipt.bUseNaniteOverrides = UseNanite;
    for (int32 Slot = 0; Slot < Receipt.SelectedRaw.Num(); ++Slot)
    {
        if (!ValidateWholeTransaction() || !Receipt.Unchanged()) return Fail(TEXT("Original material transaction changed before slot resolution"));
        UMaterialInterface* Effective = Receipt.Component->GetMaterial(Slot); // callback boundary
        // Do not inspect or pin the returned pointer until ALL earlier receipts
        // and the caller lifetime/world/reentry context have been revalidated.
        if (!ValidateWholeTransaction() || !Receipt.Unchanged()) return Fail(TEXT("Original material transaction changed during slot callback"));
        if (Effective && !MaterialObjectLive(Effective)) return Fail(TEXT("Original effective material lifetime unresolved"));
        if (Effective) Receipt.Lifetime.Emplace(Effective);
        UMaterialInterface* Raw = Receipt.SelectedRaw[Slot].Object;
        UMaterialInterface* Override = UseNanite ? CachedNaniteOverride(Receipt, Raw) : nullptr;
        if (Effective != (Override ? Override : Raw)) return Fail(TEXT("Original effective material disagrees with pinned authored Nanite chain"));
        Receipt.Effective.Add(PinMaterial(Effective));
    }
    if (!ValidateWholeTransaction() || !Receipt.Unchanged()) return Fail(TEXT("Original material final callback-free receipt check failed"));
    Receipt.bResolved = true; return true;
}

bool FDDSOriginalMaterialReceipt::SameAuthoredAssignments(const FDDSOriginalMaterialReceipt& Other) const
{
    return Mesh == Other.Mesh && bNaniteEnabled == Other.bNaniteEnabled
        && bNaniteObservationKnown == Other.bNaniteObservationKnown
        && bAuthoredNaniteEnabledKnown == Other.bAuthoredNaniteEnabledKnown
        && NaniteObservationBasis == Other.NaniteObservationBasis
        && bDisallowNanite == Other.bDisallowNanite && bForceDisableNanite == Other.bForceDisableNanite
        && bForceNaniteForMasked == Other.bForceNaniteForMasked
        && bDisplayNaniteFallbackMesh == Other.bDisplayNaniteFallbackMesh
        && DefaultSlotNames == Other.DefaultSlotNames
        && SameLayout(Defaults, Other.Defaults) && SameLayout(RawOverrides, Other.RawOverrides)
        && SameLayout(SelectedRaw, Other.SelectedRaw);
}
bool FDDSOriginalMaterialReceipt::SameEffectiveResults(const FDDSOriginalMaterialReceipt& Other) const
{
    return bResolved && Other.bResolved && bUseNaniteOverrides == Other.bUseNaniteOverrides
        && SameAuthoredAssignments(Other) && SameLayout(Effective, Other.Effective);
}
