#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSOriginalMaterialReceipt.h"
#include "DDSOriginalRobotRuntimeOwner.generated.h"

class ADDSCityRollingPawn;
class UStaticMesh;
class UStaticMeshComponent;
class UDDSRobotLeasedMeshComponent;
class UDDSRobotPhysicsRigComponent;

// Inputs come from actual native supplied-asset/template/world measurements.
// No constructor default is evidence. Indexes are body0..8/suspension0..5/parcel0.
enum class EDDSOriginalRobotPartRole : uint8 { Body, Suspension, Parcel };
struct FDDSOriginalRobotNativePart
{
    EDDSOriginalRobotPartRole Role = EDDSOriginalRobotPartRole::Body;
    int32 Index = INDEX_NONE;
    FName ComponentName;
    FString SuppliedPartId;
    FString ExactMeshObjectPath;
    FString MeshPackageSha256;
    FString NativeTemplateRecordSha256;
    FString ExactVisualSourceObjectPath;
    TArray<FString> NativeObservedMaterialObjectPaths; // observed effective results
    TArray<FString> NativeObservedDefaultMaterialObjectPaths;
    TArray<FString> NativeObservedRawOverrideMaterialObjectPaths; // exact null/trailing layout
    TArray<FString> NativeObservedSelectedRawMaterialObjectPaths;
    TSharedPtr<const FDDSOriginalMaterialReceipt> OriginalMaterialReceipt;
    UStaticMesh* SuppliedMesh = nullptr;
    UStaticMeshComponent* OriginalVisualSource = nullptr;
    FTransform MeasuredWorldCm = FTransform::Identity;
    bool bNativeTemplateAndPoseObserved = false;
};
struct FDDSOriginalRobotNativeGeometry
{
    FString IntegrityLedgerSha256;
    FString NativeGeometryRecordSha256;
    FString WorldPlacementRecordSha256;
    TArray<FDDSOriginalRobotNativePart> Parts;
    bool bActualOriginalAssetsAndTemplatesObserved = false;
};

// Explicit C++ measured-input owner. No automatic spawn, mission, calibration,
// input/controller/camera activation or collision readiness is supplied.
UCLASS(ClassGroup=(DDSCity), meta=(BlueprintSpawnableComponent))
class DDSCITY_API UDDSOriginalRobotRuntimeOwner final : public UActorComponent
{
    GENERATED_BODY()
public:
    UDDSOriginalRobotRuntimeOwner();
    // True means registered original visual assembly only; it grants no drive/readiness.
    bool CreateOriginalAssembly(const FDDSOriginalRobotNativeGeometry& ActualInput, FString& Error,
        EDDSOriginalAssemblyScope Scope = EDDSOriginalAssemblyScope::FullService);
    bool BindMeasuredOriginalAssembly(const FDDSRobotMeasuredBinding& ActualMeasurement,
        const FDDSRobotPhysicsProfile& ActualProfile, FString& Error,
        EDDSOriginalAssemblyScope Scope = EDDSOriginalAssemblyScope::FullService);
    // Refuses release after a binding attempt until the accepted retirement fence passes.
    UFUNCTION(BlueprintCallable, Category="DDSCity|Original Robot")
    bool ReleaseOriginalAssembly(FString& Error);
    UFUNCTION(BlueprintCallable, Category="DDSCity|Original Robot")
    FString GetOriginalRobotRuntimeDiagnostic() const;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UPROPERTY() TArray<TObjectPtr<UDDSRobotLeasedMeshComponent>> CreatedParts;
    UPROPERTY() TArray<TObjectPtr<UObject>> InputReferences;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> Rig;
    FDDSOriginalRobotNativeGeometry Geometry;
    // Written once for a creation transaction; retained through partial creation,
    // binding/retirement, and reset only after owned component release. Observed
    // source Geometry stays exact16; ClosedLidDrive owns only robot slots0..14.
    EDDSOriginalAssemblyScope CreationScope = EDDSOriginalAssemblyScope::FullService;
    bool bCreationScopeSet = false;
    TArray<TSharedPtr<FDDSOriginalMaterialReceipt>> CreatedMaterialReceipts;
    FString LastResult = TEXT("UNINITIALIZED: actual native geometry and calibration required");
    bool bBusy = false;
    bool bEnding = false;
    bool bBindingAttempted = false;
    bool bBindingAccepted = false;
    bool bReleaseRequested = false;
    FString LastBindingSha256;
    FString LastProfileSha256;
    FString LastCalibrationSha256;
    bool bMeasurementInputsSupplied = false;
    int32 LastMeasuredBodyFlagCount = 0;
    int32 LastMeasuredSuspensionFlagCount = 0;
    bool bLastFrozenServiceFlag = false;
    bool bLastFrozenMechanicsFlag = false;
    bool ResolveContext(ADDSCityRollingPawn*& Pawn, UDDSRobotPhysicsRigComponent*& OutRig, FString& Error) const;
    bool OwnsRigBinding(const FDDSRobotMeasuredBinding& Binding) const;
    bool AnyCreatedPartInRig(const FDDSRobotMeasuredBinding& Binding) const;
    bool ReleaseCreatedParts(FString& Error);
};
