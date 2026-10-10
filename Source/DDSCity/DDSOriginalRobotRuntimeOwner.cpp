#include "DDSOriginalRobotRuntimeOwner.h"
#include "DDSCityRollingPawn.h"
#include "DDSRobotLifecycleOwner.h"
#include "PhysicsEngine/BodySetup.h"
#include "DDSRobotOriginalBinding.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Geometry validation retained from the saved partial. Native calibration/asset
// evidence is a caller obligation; source validation never creates it.
namespace DDSOriginalRobotRuntime
{
bool Hash(const FString& Value)
{
    if (Value.Len() != 64) return false;
    for (TCHAR C : Value) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
int32 CreatedPartCount(EDDSOriginalAssemblyScope Scope)
{
    switch (Scope)
    {
    case EDDSOriginalAssemblyScope::FullService: return 16;
    case EDDSOriginalAssemblyScope::ClosedLidDrive: return 15;
    default: return 0;
    }
}
bool FinitePose(const FTransform& Pose)
{
    const FVector Location = Pose.GetLocation();
    const FVector Scale = Pose.GetScale3D();
    const FQuat Rotation = Pose.GetRotation();
    return !Pose.ContainsNaN() && Rotation.IsNormalized()
        && FMath::IsFinite(Location.X) && FMath::IsFinite(Location.Y) && FMath::IsFinite(Location.Z)
        && FMath::IsFinite(Scale.X) && FMath::IsFinite(Scale.Y) && FMath::IsFinite(Scale.Z)
        && FMath::IsFinite(Rotation.X) && FMath::IsFinite(Rotation.Y)
        && FMath::IsFinite(Rotation.Z) && FMath::IsFinite(Rotation.W)
        && Scale.GetMin() > 0 && FMath::IsNearlyEqual(Scale.X, Scale.Y, 1.e-6)
        && FMath::IsNearlyEqual(Scale.X, Scale.Z, 1.e-6);
}
int32 Slot(const FDDSOriginalRobotNativePart& Part)
{
    switch (Part.Role)
    {
    case EDDSOriginalRobotPartRole::Body: return Part.Index >= 0 && Part.Index < 9 ? Part.Index : INDEX_NONE;
    case EDDSOriginalRobotPartRole::Suspension: return Part.Index >= 0 && Part.Index < 6 ? Part.Index + 9 : INDEX_NONE;
    case EDDSOriginalRobotPartRole::Parcel: return Part.Index == 0 ? 15 : INDEX_NONE;
    default: return INDEX_NONE;
    }
}
bool ValidateGeometry(const FDDSOriginalRobotNativeGeometry& Input, FString& Error)
{
    if (!Input.bActualOriginalAssetsAndTemplatesObserved || Input.Parts.Num() != 16
        || !Hash(Input.IntegrityLedgerSha256) || !Hash(Input.NativeGeometryRecordSha256)
        || !Hash(Input.WorldPlacementRecordSha256))
    { Error = TEXT("Actual original geometry/template/world placement records and exact16 parts required"); return false; }
    TSet<int32> Slots;
    TSet<FName> Names;
    TSet<FString> PartIds;
    for (const auto& Part : Input.Parts)
    {
        const int32 At = Slot(Part);
        if (At == INDEX_NONE || Slots.Contains(At) || Part.ComponentName.IsNone()
            || Names.Contains(Part.ComponentName) || Part.SuppliedPartId.TrimStartAndEnd().IsEmpty()
            || PartIds.Contains(Part.SuppliedPartId) || !Part.bNativeTemplateAndPoseObserved
            || !Hash(Part.MeshPackageSha256) || !Hash(Part.NativeTemplateRecordSha256)
            || !FinitePose(Part.MeasuredWorldCm) || !IsValid(Part.SuppliedMesh)
            || !IsValid(Part.OriginalVisualSource)
            || Part.SuppliedMesh->GetPathName() != Part.ExactMeshObjectPath
            || Part.OriginalVisualSource->GetStaticMesh() != Part.SuppliedMesh)
        { Error = FString::Printf(TEXT("Missing/duplicate/nonfinite original native part slot%d"), At); return false; }
        // Actual original City cargo object identity is explicit; there is no fallback mesh.
        const bool OriginalRoot = Part.Role == EDDSOriginalRobotPartRole::Parcel
            ? Part.ExactMeshObjectPath.StartsWith(TEXT("/Game/"))
            : Part.ExactMeshObjectPath.StartsWith(TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/"));
        if (!OriginalRoot || !Part.SuppliedMesh->GetOutermost())
        { Error = TEXT("Exact loaded supplied original mesh identity absent"); return false; }
        Slots.Add(At); Names.Add(Part.ComponentName); PartIds.Add(Part.SuppliedPartId);
    }
    return Slots.Num() == 16;
}
}

namespace DDSOriginalRobotRuntime
{
const FDDSOriginalRobotNativePart* PartAt(const FDDSOriginalRobotNativeGeometry& Input, int32 At)
{
    for (const auto& Part : Input.Parts) if (Slot(Part) == At) return &Part;
    return nullptr;
}
FTransform ToCm(const FTransform& SI)
{
    return FTransform(SI.GetRotation(), SI.GetLocation() * 100.0, FVector::OneVector);
}
bool OriginalMaterials(const FDDSOriginalRobotNativePart& Part, FString& Error)
{
    const auto& Receipt = Part.OriginalMaterialReceipt;
    if (!Receipt || !Receipt->bResolved || !Receipt->Unchanged()
        || Receipt->Component != Part.OriginalVisualSource || Receipt->Mesh != Part.SuppliedMesh
        || Receipt->ComponentPath != Part.ExactVisualSourceObjectPath)
    { Error = TEXT("Actual original authored/effective material receipt changed or absent"); return false; }
    const auto PathsMatch = [](const auto& Pins, const TArray<FString>& Paths)
    {
        if (Pins.Num() != Paths.Num()) return false;
        for (int32 I = 0; I < Pins.Num(); ++I) if (Pins[I].Path != Paths[I]) return false;
        return true;
    };
    if (!PathsMatch(Receipt->Defaults, Part.NativeObservedDefaultMaterialObjectPaths)
        || !PathsMatch(Receipt->RawOverrides, Part.NativeObservedRawOverrideMaterialObjectPaths)
        || !PathsMatch(Receipt->SelectedRaw, Part.NativeObservedSelectedRawMaterialObjectPaths)
        || !PathsMatch(Receipt->Effective, Part.NativeObservedMaterialObjectPaths))
    { Error = TEXT("Original raw/default/effective material paths disagree with native receipt"); return false; }
    return true; // Never resolve material callbacks during owner validation.
}
bool SamePose(const FTransform& Actual, const FTransform& Expected, double CmTolerance)
{
    return Actual.GetLocation().Equals(Expected.GetLocation(), CmTolerance)
        && Actual.GetRotation().Equals(Expected.GetRotation(), 1.e-6);
}
}

UDDSOriginalRobotRuntimeOwner::UDDSOriginalRobotRuntimeOwner()
{
    PrimaryComponentTick.bCanEverTick = false;
}

bool UDDSOriginalRobotRuntimeOwner::ResolveContext(ADDSCityRollingPawn*& Pawn,
    UDDSRobotPhysicsRigComponent*& OutRig, FString& Error) const
{
    Pawn = Cast<ADDSCityRollingPawn>(GetOwner()); OutRig = nullptr;
    if (!IsInGameThread() || !IsValid(this) || !IsRegistered() || bEnding || !IsValid(Pawn)
        || !GetWorld() || GetWorld() != Pawn->GetWorld() || !Pawn->GetRootComponent())
    { Error = TEXT("Registered live same-world original rolling pawn owner required"); return false; }
    TArray<UDDSRobotPhysicsRigComponent*> Rigs; Pawn->GetComponents(Rigs);
    if (Rigs.Num() != 1 || !IsValid(Rigs[0]) || !Rigs[0]->IsRegistered()
        || Rigs[0]->GetOwner() != Pawn || Rigs[0]->GetWorld() != GetWorld())
    { Error = TEXT("Exactly one existing registered pawn-owned physics rig required"); return false; }
    OutRig = Rigs[0]; return true;
}

bool UDDSOriginalRobotRuntimeOwner::OwnsRigBinding(const FDDSRobotMeasuredBinding& B) const
{
    if (!bCreationScopeSet || DDSOriginalRobotRuntime::CreatedPartCount(CreationScope) == 0
        || CreatedParts.Num() != DDSOriginalRobotRuntime::CreatedPartCount(CreationScope)) return false;
    for (int32 I = 0; I < 9; ++I) if (B.Bodies[I].ActualPart != CreatedParts[I].Get()) return false;
    for (int32 I = 0; I < 6; ++I) if (B.SuspensionParts[I].ActualPart != CreatedParts[I + 9].Get()) return false;
    return CreationScope == EDDSOriginalAssemblyScope::ClosedLidDrive
        ? B.Parcel.ActualPart == nullptr : B.Parcel.ActualPart == CreatedParts[15].Get();
}
bool UDDSOriginalRobotRuntimeOwner::AnyCreatedPartInRig(const FDDSRobotMeasuredBinding& B) const
{
    for (const auto& Part : CreatedParts)
    {
        if (!Part) continue;
        for (const auto& Body : B.Bodies) if (Body.ActualPart == Part.Get()) return true;
        for (const auto& Suspension : B.SuspensionParts) if (Suspension.ActualPart == Part.Get()) return true;
        if (B.Parcel.ActualPart == Part.Get()) return true;
    }
    return false;
}

bool UDDSOriginalRobotRuntimeOwner::CreateOriginalAssembly(
    const FDDSOriginalRobotNativeGeometry& ActualInput, FString& Error, EDDSOriginalAssemblyScope Scope)
{
    Error.Reset();
    if (!IsInGameThread() || bBusy) { Error = TEXT("Original assembly owner busy or wrong thread"); return false; }
    const int32 PartCount = DDSOriginalRobotRuntime::CreatedPartCount(Scope);
    if (PartCount == 0) { Error = TEXT("Unsupported original assembly scope"); LastResult = Error; return false; }
    TStrongObjectPtr<UDDSOriginalRobotRuntimeOwner> OwnerPin(this);
    TGuardValue<bool> Guard(bBusy, true);
    ON_SCOPE_EXIT { if (bReleaseRequested || bEnding) { FString Deferred; ReleaseCreatedParts(Deferred); } };
    // Private value snapshot: later callbacks cannot redefine completion by mutating caller inputs.
    const FDDSOriginalRobotNativeGeometry Input = ActualInput;
    ADDSCityRollingPawn* Pawn = nullptr; UDDSRobotPhysicsRigComponent* ExistingRig = nullptr;
    if (!ResolveContext(Pawn, ExistingRig, Error) || !DDSOriginalRobotRuntime::ValidateGeometry(Input, Error))
    { LastResult = Error; return false; }
    TStrongObjectPtr<ADDSCityRollingPawn> PawnPin(Pawn);
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(ExistingRig);
    if (!CreatedParts.IsEmpty() || bBindingAttempted
        || (ExistingRig->GetRigState() != EDDSRobotRigState::Unbound
            && ExistingRig->GetRigState() != EDDSRobotRigState::Retired))
    { Error = TEXT("Explicit safe release required before creation; an existing rig is never replaced"); LastResult = Error; return false; }
    if (ExistingRig->GetRigState() == EDDSRobotRigState::Retired && !ExistingRig->ReleaseMeasuredBindingsAfterFence())
    { Error = TEXT("Prior rig retirement fence has not permitted a new component assembly"); LastResult = Error; return false; }
    TArray<TArray<UMaterialInterface*>> RawMaterials;
    RawMaterials.SetNum(16);
    TArray<TStrongObjectPtr<UObject>> Pins;
    for (int32 I = 0; I < 16; ++I)
    {
        const auto* Part = DDSOriginalRobotRuntime::PartAt(Input, I);
        if (!Part || !DDSOriginalRobotRuntime::OriginalMaterials(*Part, Error)
            || Part->OriginalMaterialReceipt->World != Pawn->GetWorld()
            || (I < PartCount && StaticFindObjectFast(UObject::StaticClass(), Pawn, Part->ComponentName)))
        { if (Error.IsEmpty()) Error = TEXT("Original material world or component name conflict"); LastResult = Error; return false; }
        Pins.Emplace(Part->SuppliedMesh); Pins.Emplace(Part->OriginalVisualSource);
        for (const auto& Material : Part->OriginalMaterialReceipt->RawOverrides)
        { RawMaterials[I].Add(Material.Object); if (Material.Object) Pins.Emplace(Material.Object); }
    }
    Geometry = Input; Rig = ExistingRig; CreationScope = Scope; bCreationScopeSet = true;
    CreatedParts.Reserve(PartCount);
    CreatedMaterialReceipts.Reset(); CreatedMaterialReceipts.Reserve(PartCount);
    TArray<TStrongObjectPtr<UDDSRobotLeasedMeshComponent>> CreatedIdentityPins;
    CreatedIdentityPins.Reserve(PartCount);
    TStrongObjectPtr<UBodySetup> ExpectedOriginalChassisCompound,ExpectedOriginalCargoVertexCollision;
    const bool PrepareOriginalCargoVertexHull=Scope==EDDSOriginalAssemblyScope::FullService
        && DDSOriginalRobotRuntime::PartAt(Input,15)->ExactMeshObjectPath==TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO");
    for (const auto& Pin : Pins) InputReferences.Add(Pin.Get());
    const auto Fail = [&]()
    {
        LastResult = Error; FString ReleaseError;
        if (!ReleaseCreatedParts(ReleaseError)) LastResult += TEXT("; partial components retained: ") + ReleaseError;
        return false;
    };
    const auto AllMaterialsUnchanged = [&]()
    {
        ADDSCityRollingPawn* CurrentPawn = nullptr; UDDSRobotPhysicsRigComponent* CurrentRig = nullptr;
        if (!bBusy || bEnding || bReleaseRequested || !bCreationScopeSet || CreationScope != Scope
            || !ResolveContext(CurrentPawn, CurrentRig, Error)
            || CurrentPawn != Pawn || CurrentRig != ExistingRig || Rig.Get() != ExistingRig
            || CreatedMaterialReceipts.Num() != CreatedParts.Num()) return false;
        for (int32 I = 0; I < 16; ++I)
        {
            const auto* Source = DDSOriginalRobotRuntime::PartAt(Input, I);
            if (!Source || !DDSOriginalRobotRuntime::OriginalMaterials(*Source, Error)
                || Source->OriginalMaterialReceipt->World != Pawn->GetWorld()) return false;
        }
        for (int32 I = 0; I < CreatedMaterialReceipts.Num(); ++I)
            if (CreatedMaterialReceipts[I] && (!CreatedMaterialReceipts[I]->Unchanged()
                || CreatedMaterialReceipts[I]->Component != CreatedParts[I].Get())) return false;
        return true;
    };
    for (int32 I = 0; I < PartCount; ++I)
    {
        const auto* Part = DDSOriginalRobotRuntime::PartAt(Input, I);
        if (bEnding || bReleaseRequested || !IsValid(Pawn) || !IsValid(ExistingRig)
            || (ExistingRig->GetRigState() != EDDSRobotRigState::Unbound
                && ExistingRig->GetRigState() != EDDSRobotRigState::Retired))
        { Error = TEXT("Original creation cancelled or existing rig ownership changed"); return Fail(); }
        auto* Component = NewObject<UDDSRobotLeasedMeshComponent>(Pawn, Part->ComponentName, RF_Transient);
        if (!IsValid(Component)) { Error = TEXT("Original managed component allocation failed"); return Fail(); }
        CreatedParts.Add(Component); // retained before any setter/registration callback.
        CreatedMaterialReceipts.Add(nullptr);
        CreatedIdentityPins.Emplace(Component); // immutable allocated-instance identity across later callbacks.
        TStrongObjectPtr<UDDSRobotLeasedMeshComponent> ComponentPin(Component);
        const FDelegateHandle AllocatedResourceDelegate = Component->UStaticMeshComponent::OnGetNaniteResources().GetHandle();
        const auto& SourceMaterial = *Part->OriginalMaterialReceipt;
        bool bAuthoredMaterialsInstalled = false;
        const auto Live = [&]()
        {
#if WITH_EDITORONLY_DATA
            if (IsValid(Component) && bool(Component->bDisplayNaniteFallbackMesh) != SourceMaterial.bDisplayNaniteFallbackMesh) return false;
#endif
            if (bAuthoredMaterialsInstalled && IsValid(Component))
            {
                if (Component->OverrideMaterials.Num() != RawMaterials[I].Num()) return false;
                for (int32 M = 0; M < RawMaterials[I].Num(); ++M)
                    if (Component->OverrideMaterials[M].Get() != RawMaterials[I][M]) return false;
            }
            return !bEnding && !bReleaseRequested && bCreationScopeSet && CreationScope == Scope
                && IsValid(this) && IsValid(Pawn)
                && IsValid(ExistingRig) && IsValid(Component) && Component->GetOwner() == Pawn
                && GetWorld() == Pawn->GetWorld() && ExistingRig->GetOwner() == Pawn
                && bool(Component->bDisallowNanite) == SourceMaterial.bDisallowNanite
                && bool(Component->bForceDisableNanite) == SourceMaterial.bForceDisableNanite
                && bool(Component->bForceNaniteForMasked) == SourceMaterial.bForceNaniteForMasked
                && !Component->UStaticMeshComponent::OnGetNaniteResources().IsBound()
                && Component->UStaticMeshComponent::OnGetNaniteResources().GetHandle() == AllocatedResourceDelegate
                && (ExistingRig->GetRigState() == EDDSRobotRigState::Unbound
                    || ExistingRig->GetRigState() == EDDSRobotRigState::Retired);
        };
        const auto Apply = [&](TFunctionRef<void()> Operation)
        {
            if (!Live() || !AllMaterialsUnchanged()) return false;
            Operation(); return Live() && AllMaterialsUnchanged();
        };
        Component->bDisallowNanite = SourceMaterial.bDisallowNanite;
        Component->bForceDisableNanite = SourceMaterial.bForceDisableNanite;
        Component->bForceNaniteForMasked = SourceMaterial.bForceNaniteForMasked;
#if WITH_EDITORONLY_DATA
        Component->bDisplayNaniteFallbackMesh = SourceMaterial.bDisplayNaniteFallbackMesh;
#endif
        bool MeshInstalled = false;
        if (!Apply([&]() { Pawn->AddInstanceComponent(Component); })
            || !Apply([&]() { Component->SetAbsolute(true, true, true); })
            || !Apply([&]() { Component->SetMobility(EComponentMobility::Movable); })
            || !Apply([&]() { Component->BodyInstance.bAutoWeld = false; })
            || !Apply([&]() { Component->SetCollisionEnabled(ECollisionEnabled::NoCollision); })
            || !Apply([&]() { MeshInstalled = Component->SetStaticMesh(Part->SuppliedMesh); }) || !MeshInstalled)
        { Error = TEXT("Original creation cancelled during owned mesh preparation"); return Fail(); }
        // Install only the exact authored override array, including every nullptr
        // and trailing slot. Mesh defaults remain on the original mesh unchanged.
        if (Component->OverrideMaterials.Num() != 0)
        { Error = TEXT("Fresh original component unexpectedly has material overrides"); return Fail(); }
        for (int32 M = 0; M < RawMaterials[I].Num(); ++M)
        {
            if (!Apply([&]() { Component->SetMaterial(M, RawMaterials[I][M]); }))
            { Error = TEXT("Original creation cancelled during authored material preparation"); return Fail(); }
            if (Component->OverrideMaterials.Num() != M + 1)
            { Error = TEXT("Authored override layout changed during material callback"); return Fail(); }
            for (int32 Previous = 0; Previous <= M; ++Previous)
                if (Component->OverrideMaterials[Previous].Get() != RawMaterials[I][Previous])
                { Error = TEXT("Earlier authored assignment changed during later material callback"); return Fail(); }
        }
        bAuthoredMaterialsInstalled = true;
        if (!Live() || !AllMaterialsUnchanged())
        { Error = TEXT("Original authored material/policy state changed before collision preparation"); return Fail(); }
        if(I==0)
        {
            if(!Live() || !AllMaterialsUnchanged() || !Component->PrepareOriginalChassisCompoundFromObservedGeometry(Input,Error) || !Live() || !AllMaterialsUnchanged())return Fail();
            ExpectedOriginalChassisCompound.Reset(Component->GetBodySetup());
        }
        if(I==15 && PrepareOriginalCargoVertexHull)
        {
            if(!Live() || !AllMaterialsUnchanged() || !Component->PrepareOriginalCargoVertexCollisionFromObservedGeometry(Input,Error) || !Live() || !AllMaterialsUnchanged())return Fail();
            ExpectedOriginalCargoVertexCollision.Reset(Component->GetBodySetup());
        }
        // Explicit measured creation pose only. This owner never moves bound physics parts.
        if (!Apply([&]() { Component->SetWorldTransform(Part->MeasuredWorldCm, false, nullptr, ETeleportType::None); })
            || !Apply([&]() { Component->RegisterComponent(); }))
        { Error = TEXT("Original creation cancelled during measured pose/registration"); return Fail(); }
        if (bEnding || bReleaseRequested || !IsValid(Component) || Component->GetOwner() != Pawn
            || !Component->IsRegistered() || Component->GetWorld() != GetWorld()
            || Component->IsSimulatingPhysics() || Component->GetStaticMesh() != Part->SuppliedMesh
            || !Component->GetComponentTransform().Equals(Part->MeasuredWorldCm, 1.e-6))
        { Error = TEXT("Original managed registration/owner/scale/pose changed during creation"); return Fail(); }
        // Capture this complete registered target BEFORE its first explicit
        // resolver; all source and earlier target receipts survive every callback.
        if (!FDDSOriginalMaterialObservation::CaptureRaw(*Component, CreatedMaterialReceipts[I], Error)
            || !CreatedMaterialReceipts[I]->SameAuthoredAssignments(SourceMaterial)
            || !FDDSOriginalMaterialObservation::Resolve(*CreatedMaterialReceipts[I], AllMaterialsUnchanged, Error)
            || !CreatedMaterialReceipts[I]->SameEffectiveResults(SourceMaterial))
        { if (Error.IsEmpty()) Error = TEXT("Original target raw/effective material receipt differs after registration"); return Fail(); }
    }
    // R01: raw override reads only; effective GetMaterial can resolve Nanite/PostLoad callbacks.
    // Authored SetMaterial writes and guarded effective resolution completed above.
    // Registration of a later part may have invalidated any earlier part, including slot0.
    const auto FinalContext = [&]()
    {
        ADDSCityRollingPawn* CurrentPawn = nullptr; UDDSRobotPhysicsRigComponent* CurrentRig = nullptr;
        return bBusy && !bEnding && !bReleaseRequested && !bBindingAttempted
            && bCreationScopeSet && CreationScope == Scope
            && CreatedParts.Num() == PartCount && CreatedIdentityPins.Num() == PartCount
            && AllMaterialsUnchanged()
            && ResolveContext(CurrentPawn, CurrentRig, Error) && CurrentPawn == Pawn
            && CurrentRig == ExistingRig && Rig.Get() == ExistingRig
            && (ExistingRig->GetRigState() == EDDSRobotRigState::Unbound
                || ExistingRig->GetRigState() == EDDSRobotRigState::Retired);
    };
    if (!FinalContext())
    { if (Error.IsEmpty()) Error = TEXT("Original complete assembly context cancelled or changed"); return Fail(); }
    for (int32 I = 0; I < PartCount; ++I)
    {
        const auto* Part = DDSOriginalRobotRuntime::PartAt(Input, I);
        auto* Component = CreatedParts[I].Get();
        if (!Part || Component != CreatedIdentityPins[I].Get() || !IsValid(Component)
            || !IsValid(Part->SuppliedMesh) || Component->GetFName() != Part->ComponentName
            || Component->GetOuter() != Pawn || Component->GetOwner() != Pawn
            || !Component->IsRegistered() || Component->GetWorld() != GetWorld()
            || Component->GetStaticMesh() != Part->SuppliedMesh
            || (I==0 && (!Component->PreparedOriginalChassisCompoundUnchanged() || !IsValid(ExpectedOriginalChassisCompound.Get()) || Component->GetBodySetup()!=ExpectedOriginalChassisCompound.Get()
                || Component->GetBodySetup()==Part->SuppliedMesh->GetBodySetup()))
            || (I==15 && PrepareOriginalCargoVertexHull && (!Component->PreparedOriginalCargoVertexCollisionUnchanged() || !IsValid(ExpectedOriginalCargoVertexCollision.Get())
                || Component->GetBodySetup()!=ExpectedOriginalCargoVertexCollision.Get() || Component->GetBodySetup()==Part->SuppliedMesh->GetBodySetup()))
            || Part->SuppliedMesh->GetPathName() != Part->ExactMeshObjectPath
            || Component->GetAttachParent() || !Component->IsUsingAbsoluteLocation()
            || !Component->IsUsingAbsoluteRotation() || !Component->IsUsingAbsoluteScale()
            || Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision
            || Component->IsSimulatingPhysics()
            || !DDSOriginalRobotRuntime::FinitePose(Component->GetComponentTransform())
            || !Component->GetComponentTransform().Equals(Part->MeasuredWorldCm, 1.e-6)
            || Component->OverrideMaterials.Num() != RawMaterials[I].Num())
        { Error = FString::Printf(TEXT("Original complete assembly final part%d identity/lifetime/collision/pose mismatch"), I); return Fail(); }
        if (!CreatedMaterialReceipts[I] || !CreatedMaterialReceipts[I]->bResolved
            || !CreatedMaterialReceipts[I]->Unchanged()
            || !CreatedMaterialReceipts[I]->SameEffectiveResults(*Part->OriginalMaterialReceipt))
        { Error = FString::Printf(TEXT("Original complete assembly final part%d authored/effective receipt mismatch"), I); return Fail(); }
    }
    if (!FinalContext())
    { if (Error.IsEmpty()) Error = TEXT("Original complete assembly context changed during final reread"); return Fail(); }
    LastResult = TEXT("ORIGINAL_VISUALS_REGISTERED_ONLY: actual calibration/binding/readiness still required");
    return true;
}

bool UDDSOriginalRobotRuntimeOwner::BindMeasuredOriginalAssembly(
    const FDDSRobotMeasuredBinding& ActualMeasurement, const FDDSRobotPhysicsProfile& ActualProfile, FString& Error,
    EDDSOriginalAssemblyScope Scope)
{
    Error.Reset();
    if (!IsInGameThread() || bBusy) { Error = TEXT("Original binding owner busy or wrong thread"); return false; }
    TStrongObjectPtr<UDDSOriginalRobotRuntimeOwner> OwnerPin(this);
    TGuardValue<bool> Guard(bBusy, true);
    ON_SCOPE_EXIT { if (bReleaseRequested || bEnding) { FString Deferred; ReleaseCreatedParts(Deferred); } };
    const FDDSRobotMeasuredBinding Measurement = ActualMeasurement;
    const FDDSRobotPhysicsProfile Profile = ActualProfile;
    const int32 PartCount = DDSOriginalRobotRuntime::CreatedPartCount(Scope);
    if (PartCount == 0 || !bCreationScopeSet || CreationScope != Scope)
    { Error = TEXT("Binding scope must match the immutable original creation scope"); LastResult = Error; return false; }
    bMeasurementInputsSupplied = true; LastMeasuredBodyFlagCount = 0; LastMeasuredSuspensionFlagCount = 0;
    for (const auto& Body : Measurement.Bodies) if (Body.bMeasured && Body.bSimpleCollisionMeasured) ++LastMeasuredBodyFlagCount;
    for (const auto& Part : Measurement.SuspensionParts) if (Part.bMeasured) ++LastMeasuredSuspensionFlagCount;
    bLastFrozenServiceFlag = Profile.Service.bFrozen; bLastFrozenMechanicsFlag = Measurement.Mechanics.bFrozen;
    LastCalibrationSha256 = Measurement.Mechanics.RecordSha256;
    LastBindingSha256 = Measurement.BindingSha256; LastProfileSha256 = Profile.ProfileSha256;
    ADDSCityRollingPawn* Pawn = nullptr; UDDSRobotPhysicsRigComponent* ExistingRig = nullptr;
    if (!ResolveContext(Pawn, ExistingRig, Error)) { LastResult = Error; return false; }
    TStrongObjectPtr<ADDSCityRollingPawn> PawnPin(Pawn);
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(ExistingRig);
    if (Rig.Get() != ExistingRig || CreatedParts.Num() != PartCount || !DDSOriginalRobotRuntime::ValidateGeometry(Geometry, Error))
    { if (Error.IsEmpty()) Error = TEXT("Exact scoped created original owned components and original16 source geometry required"); LastResult = Error; return false; }
    if (ExistingRig->GetRigState() != EDDSRobotRigState::Unbound && ExistingRig->GetRigState() != EDDSRobotRigState::Retired)
    { Error = TEXT("Existing physics binding must retire before any rebind"); LastResult = Error; return false; }
    if (!Measurement.bNineBodyTopologyVerified || !Measurement.bMountsMeasured || !Measurement.bFramesAreScaledPhysicalSI
        || Profile.Provenance.TrimStartAndEnd().IsEmpty()
        || Profile.Provenance == FDDSRobotPhysicsProfile().Provenance
        || !DDSValidateRobotProfile(Profile, Error))
    { if (Error.IsEmpty()) Error = TEXT("Actual measured topology/mount/scaled-frame data and declared source profile absent"); LastResult = Error; return false; }
    if (Scope == EDDSOriginalAssemblyScope::FullService)
    {
        if (!Measurement.Mechanics.bFrozen || !Profile.Service.bFrozen || !DDSValidateN12Calibration(Measurement.Mechanics, Error))
        { if (Error.IsEmpty()) Error = TEXT("FullService requires actual frozen N12 mechanics/service calibration"); LastResult = Error; return false; }
    }
    else if (Measurement.Parcel.ActualPart || Measurement.Mechanics.bFrozen || Profile.Service.bFrozen)
    { Error = TEXT("ClosedLidDrive requires no bound parcel and unfrozen mechanics/service"); LastResult = Error; return false; }
    const auto AllBindingMaterialsUnchanged = [&]()
    {
        ADDSCityRollingPawn* CurrentPawn = nullptr; UDDSRobotPhysicsRigComponent* CurrentRig = nullptr;
        if (bEnding || bReleaseRequested || !bBusy || !bCreationScopeSet || CreationScope != Scope
            || !ResolveContext(CurrentPawn, CurrentRig, Error)
            || CurrentPawn != Pawn || CurrentRig != ExistingRig || CreatedParts.Num() != PartCount
            || CreatedMaterialReceipts.Num() != PartCount) return false;
        for (int32 I = 0; I < 16; ++I)
        {
            const auto* Source = DDSOriginalRobotRuntime::PartAt(Geometry, I);
            if (!Source || !DDSOriginalRobotRuntime::OriginalMaterials(*Source, Error)) return false;
            if (I >= PartCount) continue; // retained source parcel is outside ClosedLidDrive ownership.
            const auto& Target = CreatedMaterialReceipts[I];
            if (!Target || !Target->Unchanged(false) || Target->Component != CreatedParts[I].Get()
                || !Target->SameEffectiveResults(*Source->OriginalMaterialReceipt)) return false;
        }
        return true;
    };
    if (!AllBindingMaterialsUnchanged())
    { if (Error.IsEmpty()) Error = TEXT("Original complete material receipts changed before binding"); LastResult = Error; return false; }
    FDDSRobotOriginalComponentMap Names; Names.IntegrityLedgerSha256 = Geometry.IntegrityLedgerSha256;
    for (int32 I = 0; I < PartCount; ++I)
    {
        const auto* Part = DDSOriginalRobotRuntime::PartAt(Geometry, I);
        auto* Component = CreatedParts[I].Get();
        if (!Part || !IsValid(Component) || Component->GetOwner() != Pawn || !Component->IsRegistered()
            || Component->GetWorld() != GetWorld() || Component->IsSimulatingPhysics()
            || Component->GetFName() != Part->ComponentName || Component->GetStaticMesh() != Part->SuppliedMesh
            || !Component->GetComponentScale().Equals(Part->MeasuredWorldCm.GetScale3D(), 1.e-6)
            || !DDSOriginalRobotRuntime::OriginalMaterials(*Part, Error))
        { if (Error.IsEmpty()) Error = TEXT("Created original part ownership/registration/identity/scale unavailable"); LastResult = Error; return false; }
        if (!AllBindingMaterialsUnchanged())
        { if (Error.IsEmpty()) Error = TEXT("Original authored/effective receipts changed before physics binding"); LastResult = Error; return false; }
        if (I < 9 || I == 15)
        {
            const auto& Body = I == 15 ? Measurement.Parcel : Measurement.Bodies[I];
            if (!Body.bMeasured || !Body.bSimpleCollisionMeasured || Body.SuppliedPartId != Part->SuppliedPartId
                || Body.SuppliedMeshPackage != Part->ExactMeshObjectPath || Body.MeshPackageSha256 != Part->MeshPackageSha256
                || (Body.ActualPart && Body.ActualPart != Component)
                || !Body.SupplierScale.Equals(Component->GetComponentScale(), 1.e-6))
            { Error = TEXT("Native original body/cargo measurement does not match observed role/mesh/scale"); LastResult = Error; return false; }
            if (I == 15) Names.Parcel = Part->ComponentName; else Names.Bodies[I] = Part->ComponentName;
        }
        else
        {
            const auto& Suspension = Measurement.SuspensionParts[I - 9];
            if (!Suspension.bMeasured || Suspension.SuppliedPartId != Part->SuppliedPartId
                || (Suspension.ActualPart && Suspension.ActualPart != Component)
                || !Suspension.SupplierScale.Equals(Component->GetComponentScale(), 1.e-6))
            { Error = TEXT("Native suspension measurement does not match observed role/scale"); LastResult = Error; return false; }
            Names.Suspension[I - 9] = Part->ComponentName;
        }
    }
    FDDSRobotMeasuredBinding Binding;
    if (!DDSConsumeOriginalRobotBinding(*Pawn, Names, Measurement, Profile, Binding, Error, CreationScope))
    { if (Error.IsEmpty()) Error = TEXT("Existing original measured binding consumer refused native inputs"); LastResult = Error; return false; }
    // Validate current registered poses against measured physical SI frames, not a template guess.
    const FTransform ChassisWorld = DDSRigidComponentPose(CreatedParts[0]->GetComponentTransform());
    for (int32 I = 0; I < 9; ++I)
    {
        const FTransform Expected = DDSOriginalRobotRuntime::ToCm(Binding.Bodies[I].RestInChassisMeters) * ChassisWorld;
        if (!DDSOriginalRobotRuntime::SamePose(CreatedParts[I]->GetComponentTransform(), Expected, Binding.MaxContactSeparationM * 100.0))
        { Error = TEXT("Original body current pose differs from actual measured rest frame"); LastResult = Error; return false; }
    }
    for (int32 I = 0; I < 6; ++I)
    {
        const auto& Map = Binding.SuspensionParts[I];
        const int32 Owner = static_cast<int32>(Map.Owner);
        const FTransform Expected = DDSOriginalRobotRuntime::ToCm(Map.RestInOwnerMeters)
            * DDSRigidComponentPose(CreatedParts[Owner]->GetComponentTransform());
        if (!DDSOriginalRobotRuntime::SamePose(CreatedParts[I + 9]->GetComponentTransform(), Expected, Binding.MaxContactSeparationM * 100.0))
        { Error = TEXT("Original suspension current pose differs from measured rigid-owner rest frame"); LastResult = Error; return false; }
    }
    if (bEnding || bReleaseRequested || !AllBindingMaterialsUnchanged()) { Error = TEXT("Original binding cancelled or material receipt changed before physics submission"); LastResult = Error; return false; }
    bBindingAttempted = true; LastBindingSha256 = Binding.BindingSha256; LastProfileSha256 = Profile.ProfileSha256;
    bBindingAccepted = Pawn->BindMeasuredOriginalRobot(Binding, Profile, Error);
    if (bEnding || bReleaseRequested || !AllBindingMaterialsUnchanged())
    { bBindingAccepted = false; Error = TEXT("Original physics binding cancelled by owner lifetime/release/material receipt"); }
    LastResult = bBindingAccepted ? TEXT("MEASURED_BINDING_SUBMITTED: completed physics and original collision readiness remain required") : Error;
    if (LastResult.IsEmpty()) LastResult = TEXT("Existing pawn physics binding refused; components retained pending safe release");
    return bBindingAccepted;
}

bool UDDSOriginalRobotRuntimeOwner::ReleaseCreatedParts(FString& Error)
{
    Error.Reset();
    if (!IsInGameThread()) { Error = TEXT("Original release requires game thread"); return false; }
    if (CreatedParts.IsEmpty())
    { bCreationScopeSet = false; CreationScope = EDDSOriginalAssemblyScope::FullService; bReleaseRequested = false; return true; }
    auto* ExistingRig = Rig.Get();
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> RigPin(ExistingRig);
    TStrongObjectPtr<AActor> PawnPin(GetOwner());
    if (!IsValid(GetOwner())) { Error = TEXT("Original owner unavailable; retain components for actor/lease teardown"); return false; }
    for (const auto& Part : CreatedParts)
        if (!IsValid(Part.Get()) || Part->GetOwner() != GetOwner())
        { Error = TEXT("Original component ownership invalid; explicit release refused"); return false; }
    if (bBindingAttempted)
    {
        if (!IsValid(ExistingRig) || ExistingRig->GetOwner() != GetOwner())
        { Error = TEXT("Binding rig unavailable; retirement fence cannot be verified"); return false; }
        const auto& Current = ExistingRig->GetMeasuredBinding();
        if (AnyCreatedPartInRig(Current) && !OwnsRigBinding(Current))
        { Error = TEXT("Partial/foreign current rig binding; never retire another owner's assembly"); return false; }
        if (OwnsRigBinding(Current)) ExistingRig->BeginSafeUnregister();
        if (!ExistingRig->ReleaseMeasuredBindingsAfterFence())
        { Error = TEXT("Accepted retirement fence has not permitted release; components retained"); return false; }
    }
    else
    {
        // Never-submitted partial creation may release only while still unsimulated and unclaimed.
        if (IsValid(ExistingRig) && AnyCreatedPartInRig(ExistingRig->GetMeasuredBinding()))
        { Error = TEXT("Another binding acquired created parts; explicit release refused"); return false; }
        for (const auto& Part : CreatedParts) if (Part->IsSimulatingPhysics())
        { Error = TEXT("Unexpected physical activation before owned binding; release refused"); return false; }
    }
    if (bBindingAccepted)
        if (auto* Pawn = Cast<ADDSCityRollingPawn>(GetOwner())) Pawn->ReleaseOriginalCityCamera();
    // Callback/body reader retirement is established above before any component destruction.
    while (!CreatedParts.IsEmpty())
    {
        auto* Part = CreatedParts.Last().Get();
        if (!IsValid(GetOwner()) || !IsValid(Part) || Part->GetOwner() != GetOwner()
            || (bBindingAttempted && (!IsValid(ExistingRig) || !ExistingRig->ReleaseMeasuredBindingsAfterFence()))
            || (IsValid(ExistingRig) && AnyCreatedPartInRig(ExistingRig->GetMeasuredBinding())))
        { Error = TEXT("Lifetime/fence/foreign claim changed during release; remaining parts retained"); return false; }
        GetOwner()->RemoveInstanceComponent(Part);
        if (!IsValid(Part)) { Error = TEXT("Original part lifetime changed during instance removal"); return false; }
        Part->DestroyComponent(); CreatedParts.Pop();
    }
    CreatedParts.Reset(); CreatedMaterialReceipts.Reset(); InputReferences.Reset(); Geometry = FDDSOriginalRobotNativeGeometry(); Rig.Reset();
    bCreationScopeSet = false; CreationScope = EDDSOriginalAssemblyScope::FullService;
    bBindingAttempted = false; bBindingAccepted = false; bReleaseRequested = false;
    LastResult = TEXT("ORIGINAL_COMPONENTS_RELEASED_AFTER_OWNED_SAFETY_CHECKS");
    return true;
}

bool UDDSOriginalRobotRuntimeOwner::ReleaseOriginalAssembly(FString& Error)
{
    if (!IsInGameThread()) { Error = TEXT("Original release requires game thread"); return false; }
    if (bBusy) { bReleaseRequested = true; Error = TEXT("Original owner busy; release deferred to outer operation boundary"); return false; }
    TStrongObjectPtr<UDDSOriginalRobotRuntimeOwner> OwnerPin(this);
    TGuardValue<bool> Guard(bBusy, true);
    const bool Released = ReleaseCreatedParts(Error);
    if (!Released) LastResult = Error;
    return Released;
}

void UDDSOriginalRobotRuntimeOwner::EndPlay(const EEndPlayReason::Type Reason)
{
    bEnding = true; bReleaseRequested = true;
    if (!bBusy)
    {
        TGuardValue<bool> Guard(bBusy, true);
        FString Error; if (!ReleaseCreatedParts(Error)) LastResult = Error;
    }
    // An in-progress factory/binding call retains pins and handles deferred release.
    // Actor/managed-component lifecycle remains the accepted last-resort retirement owner.
    Super::EndPlay(Reason);
}

FString UDDSOriginalRobotRuntimeOwner::GetOriginalRobotRuntimeDiagnostic() const
{
    if (!IsInGameThread()) return TEXT("{\"error\":\"game thread required\"}");
    auto Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("schema"), TEXT("DDSCity.OriginalRobotRuntimeDiagnostic.v1"));
    Report->SetStringField(TEXT("result"), LastResult);
    Report->SetBoolField(TEXT("ownerRegistered"), IsRegistered());
    Report->SetBoolField(TEXT("busy"), bBusy); Report->SetBoolField(TEXT("ending"), bEnding);
    Report->SetBoolField(TEXT("bindingAttempted"), bBindingAttempted);
    Report->SetBoolField(TEXT("bindingAcceptedAtSubmission"), bBindingAccepted);
    Report->SetNumberField(TEXT("createdPartCount"), CreatedParts.Num());
    Report->SetBoolField(TEXT("creationScopeRecorded"), bCreationScopeSet);
    Report->SetStringField(TEXT("creationScope"), !bCreationScopeSet ? TEXT("NONE")
        : (CreationScope == EDDSOriginalAssemblyScope::ClosedLidDrive ? TEXT("ClosedLidDrive") : TEXT("FullService")));
    Report->SetNumberField(TEXT("expectedCreatedPartCountForScope"), bCreationScopeSet ? DDSOriginalRobotRuntime::CreatedPartCount(CreationScope) : 0);
    Report->SetNumberField(TEXT("observedSourcePartCount"), Geometry.Parts.Num());
    Report->SetStringField(TEXT("nativeGeometryRecordSha256"), Geometry.NativeGeometryRecordSha256);
    Report->SetStringField(TEXT("worldPlacementRecordSha256"), Geometry.WorldPlacementRecordSha256);
    Report->SetStringField(TEXT("bindingSha256"), LastBindingSha256);
    Report->SetStringField(TEXT("profileSha256"), LastProfileSha256);
    Report->SetStringField(TEXT("calibrationSha256FromCaller"), LastCalibrationSha256);
    Report->SetBoolField(TEXT("measurementInputsSupplied"), bMeasurementInputsSupplied);
    Report->SetNumberField(TEXT("bodyMeasuredFlagsFromCaller"), LastMeasuredBodyFlagCount);
    Report->SetNumberField(TEXT("suspensionMeasuredFlagsFromCaller"), LastMeasuredSuspensionFlagCount);
    Report->SetBoolField(TEXT("frozenServiceFlagFromCaller"), bLastFrozenServiceFlag);
    Report->SetBoolField(TEXT("frozenMechanicsFlagFromCaller"), bLastFrozenMechanicsFlag);
    auto* ExistingRig = Rig.Get();
    FDDSRobotSnapshot Snapshot;
    const bool Fresh = IsValid(ExistingRig) && ExistingRig->ConsumeSnapshot(Snapshot);
    Report->SetBoolField(TEXT("bindingCurrentlyOwned"), IsValid(ExistingRig) && OwnsRigBinding(ExistingRig->GetMeasuredBinding())
        && (ExistingRig->GetRigState() == EDDSRobotRigState::Binding || ExistingRig->GetRigState() == EDDSRobotRigState::Active));
    Report->SetBoolField(TEXT("freshCompletedPhysicalSnapshot"), Fresh);
    Report->SetStringField(TEXT("collisionReadiness"), Fresh && Snapshot.bWorldReady ? TEXT("CURRENT_PHYSICAL_SNAPSHOT_READY") : TEXT("UNAVAILABLE_NO_PERMISSION"));
    Report->SetBoolField(TEXT("visualCreationGrantsReadiness"), false);
    if (IsValid(ExistingRig))
    {
        Report->SetNumberField(TEXT("rigState"), static_cast<int32>(ExistingRig->GetRigState()));
        Report->SetStringField(TEXT("rigFault"), ExistingRig->GetFault());
        Report->SetStringField(TEXT("generation"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(ExistingRig->GetGeneration())));
        Report->SetStringField(TEXT("readinessRevision"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(ExistingRig->GetReadinessRevision())));
        if (Fresh) Report->SetStringField(TEXT("collisionWorldEpoch"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Snapshot.CollisionWorldGeneration)));
    }
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 I = 0; I < CreatedParts.Num(); ++I)
    {
        const auto* Part = CreatedParts[I].Get(); const auto* Input = DDSOriginalRobotRuntime::PartAt(Geometry, I);
        auto Row = MakeShared<FJsonObject>(); Row->SetNumberField(TEXT("slot"), I);
        Row->SetBoolField(TEXT("valid"), IsValid(Part));
        if (Input)
        {
            Row->SetStringField(TEXT("suppliedPartId"), Input->SuppliedPartId);
            Row->SetStringField(TEXT("expectedMeshObjectPath"), Input->ExactMeshObjectPath);
            Row->SetStringField(TEXT("meshPackageSha256FromNativeInput"), Input->MeshPackageSha256);
            Row->SetStringField(TEXT("nativeTemplateRecordSha256"), Input->NativeTemplateRecordSha256);
        }
        if (IsValid(Part))
        {
            Row->SetStringField(TEXT("component"), Part->GetPathName());
            Row->SetStringField(TEXT("owner"), IsValid(Part->GetOwner()) ? Part->GetOwner()->GetPathName() : FString());
            Row->SetBoolField(TEXT("samePawnOwner"), Part->GetOwner() == GetOwner());
            Row->SetBoolField(TEXT("registered"), Part->IsRegistered());
            Row->SetBoolField(TEXT("simulating"), Part->IsSimulatingPhysics());
            const UStaticMesh* Mesh = Part->GetStaticMesh().Get();
            Row->SetStringField(TEXT("loadedMeshObjectPath"), IsValid(Mesh) ? Mesh->GetPathName() : FString());
            Row->SetStringField(TEXT("loadedMeshPackage"), IsValid(Mesh) && Mesh->GetOutermost() ? Mesh->GetOutermost()->GetName() : FString());
            Row->SetStringField(TEXT("worldTransformCm"), Part->GetComponentTransform().ToString());
            TArray<TSharedPtr<FJsonValue>> MaterialRows, RawRows, DefaultRows, SelectedRows;
            const auto Receipt = CreatedMaterialReceipts.IsValidIndex(I) ? CreatedMaterialReceipts[I] : nullptr;
            const bool MaterialsCurrent = Receipt && Receipt->bResolved && Receipt->Unchanged(false)
                && Receipt->Component == Part && Input && Input->OriginalMaterialReceipt
                && Input->OriginalMaterialReceipt->Unchanged()
                && Receipt->SameEffectiveResults(*Input->OriginalMaterialReceipt);
            Row->SetBoolField(TEXT("cachedMaterialReceiptCurrent"), MaterialsCurrent);
            if (MaterialsCurrent)
            {
                for (const auto& Material : Receipt->Effective) MaterialRows.Add(MakeShared<FJsonValueString>(Material.Path));
                for (const auto& Material : Receipt->RawOverrides) RawRows.Add(MakeShared<FJsonValueString>(Material.Path));
                for (const auto& Material : Receipt->Defaults) DefaultRows.Add(MakeShared<FJsonValueString>(Material.Path));
                for (const auto& Material : Receipt->SelectedRaw) SelectedRows.Add(MakeShared<FJsonValueString>(Material.Path));
                Row->SetStringField(TEXT("naniteObservationBasis"), Receipt->NaniteObservationBasis);
                Row->SetBoolField(TEXT("naniteObservationKnown"), Receipt->bNaniteObservationKnown);
                Row->SetBoolField(TEXT("authoredNaniteEnabledKnown"), Receipt->bAuthoredNaniteEnabledKnown);
                if (Receipt->bAuthoredNaniteEnabledKnown) Row->SetBoolField(TEXT("originalNaniteEnabled"), Receipt->bNaniteEnabled);
                else if (Receipt->bNaniteObservationKnown) Row->SetBoolField(TEXT("observedCookedNaniteDataValid"), Receipt->bNaniteEnabled);
                Row->SetBoolField(TEXT("useNaniteOverrideMaterialsFromReceipt"), Receipt->bUseNaniteOverrides);
            }
            Row->SetArrayField(TEXT("actualMaterialObjectPaths"), MaterialRows);
            Row->SetArrayField(TEXT("authoredRawOverrideMaterialObjectPaths"), RawRows);
            Row->SetArrayField(TEXT("originalDefaultMaterialObjectPaths"), DefaultRows);
            Row->SetArrayField(TEXT("selectedRawMaterialObjectPaths"), SelectedRows);
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Report->SetArrayField(TEXT("parts"), Rows);
    FString Json; auto Writer = TJsonWriterFactory<>::Create(&Json);
    return FJsonSerializer::Serialize(Report, Writer) ? Json : TEXT("{\"error\":\"diagnostic serialization failed\"}");
}
