#pragma once
#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class AActor;
class AWorldSettings;
class UWorld;
class IConsoleVariable;
class UPackage;

// Raw/authored assignment and one observed effective resolution are distinct.
// The retained graph is finite and all checks after publication are callback-free.
struct DDSCITY_API FDDSOriginalMaterialReceipt final
{
    struct FMaterialPin { UMaterialInterface* Object = nullptr; FString Path; };
    struct FChainPin
    {
        FMaterialPin Material;
        UMaterialInterface* Parent = nullptr;
        UMaterialInterface* NaniteOverride = nullptr;
        bool bInstance = false;
        bool bEnableOverride = false;
        UClass* NodeClass = nullptr;
        UPackage* Package = nullptr;
        FString ClassPath, PackagePath;
        bool bDynamicInstance = false;
        int32 MaterialDomain = 0, RawBlendMode = 0;
        uint16 ShadingModelField = 0;
        uint32 NaniteUsageWarning = 0;
        bool bMaskedAssumedOpaque = false, bSky = false;
        bool bUsedWithNanite = false, bSpecialEngineMaterial = false;
        bool bAutomaticallySetUsage = false, bCookedForEditor = false;
    };
    struct FConsolePin { FString Name; IConsoleVariable* Variable = nullptr; int32 Value = 0; };
    UStaticMeshComponent* Component = nullptr;
    UStaticMesh* Mesh = nullptr;
    AActor* Owner = nullptr;
    UWorld* World = nullptr;
    AWorldSettings* WorldSettings = nullptr;
    UObject* Archetype = nullptr;
    const void* Scene = nullptr;
    const void* RenderData = nullptr;
    const void* NaniteResources = nullptr;
    int32 NanitePageCount = 0;
    int32 WorldFeatureLevel = 0;
    FTransform Pose;
    FString ComponentPath, MeshPath, OwnerPath, WorldPath, ArchetypePath;
    FName ComponentName;
    bool bRegistered = false;
    // Value uses NaniteObservationBasis; absent runtime data is explicitly unknown.
    bool bNaniteEnabled = false;
    bool bNaniteObservationKnown = false, bAuthoredNaniteEnabledKnown = false;
    FString NaniteObservationBasis;
    bool bDisallowNanite = false;
    bool bForceDisableNanite = false;
    bool bForceNaniteForMasked = false;
    bool bDisplayNaniteFallbackMesh = false;
    bool bWorldAllowsMasked = false;
    FDelegateHandle NaniteResourceDelegate;
    bool bResolved = false;
    bool bUseNaniteOverrides = false;
    FMaterialPin SurfaceDefault;
    bool bEditorUsagePolicy = false, bGameUsagePolicy = false;
    TArray<FName> DefaultSlotNames;
    TArray<FMaterialPin> Defaults, RawOverrides, SelectedRaw, Effective;
    TArray<FChainPin> Chains;
    TArray<FConsolePin> ConsolePolicy;
    TArray<TStrongObjectPtr<UObject>> Lifetime;

    // No GetMaterial/GetMaterials/GetNaniteOverride/UseNaniteOverrideMaterials.
    bool Unchanged(bool bCheckPose = true) const;
    bool SameAuthoredAssignments(const FDDSOriginalMaterialReceipt& Other) const;
    bool SameEffectiveResults(const FDDSOriginalMaterialReceipt& Other) const;
};

class DDSCITY_API FDDSOriginalMaterialObservation final
{
public:
    // Capture every source in the transaction before invoking Resolve on any one.
    static bool CaptureRaw(UStaticMeshComponent& Component,
        TSharedPtr<FDDSOriginalMaterialReceipt>& Out, FString& Error);
    // ValidateWholeTransaction must check all earlier raw/complete receipts and
    // caller owner/world/reentry context immediately after EACH resolver callback.
    static bool Resolve(FDDSOriginalMaterialReceipt& Receipt,
        TFunctionRef<bool()> ValidateWholeTransaction, FString& Error);
};
