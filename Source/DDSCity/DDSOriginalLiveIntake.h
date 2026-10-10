#pragma once
#include "CoreMinimal.h"
#include "DDSOriginalRobotRuntimeOwner.h"
class AActor;
// Explicit reviewed records; no default role/parcel/calibration is manufactured.
struct FDDSOriginalLiveRole
{
    FString NativeNodeObjectPath;
    EDDSOriginalRobotPartRole Role = EDDSOriginalRobotPartRole::Body;
    int32 Index = INDEX_NONE;
    FName TargetComponentName;
    FString SuppliedPartId;
};
struct FDDSOriginalLiveIntakeInputs
{
    TArray<uint8> NativeGeometryJsonBytes;
    FString ExpectedNativeGeometrySha256;
    TArray<uint8> OriginalIntegrityLedgerBytes;
    // Fresh root-pinned DDSCity.OriginalLivePackagePins.v1 JSON containing
    // originalLedgerSha256 and packages[{objectPath,packageSha256,extension}]: exact25
    // original Bendylab packages plus ONE supplied original City cargo package.
    TArray<uint8> PackagePinsJsonBytes;
    FString ExpectedPackagePinsSha256;
    FString ExactCargoObjectPath;
    FString ExpectedWorldPackageName;
    TArray<FDDSOriginalLiveRole> Original15Roles;
    FName CargoTargetComponentName;
    FString ActualCargoPartId;
};
class DDSCITY_API FDDSOriginalLiveIntake final
{
public:
    static bool Observe(AActor& ExactPublisherActor, UStaticMeshComponent& ActualCityCargoSource,
        const FDDSOriginalLiveIntakeInputs& Inputs, FDDSOriginalRobotNativeGeometry& OutGeometry,
        FString& OutWorldPlacementJson, FString& Error);
    // Explicit development C++ entry only. Existing pawn/City mode/controller
    // and source originals are retained; registered visuals ONLY, no activation.
    static bool CreateVisualsOnExistingPawn(ADDSCityRollingPawn& ExistingPawn,
        AActor& ExactPublisherActor, UStaticMeshComponent& ActualCityCargoSource,
        const FDDSOriginalLiveIntakeInputs& Inputs, UDDSOriginalRobotRuntimeOwner*& OutOwner,
        FString& OutWorldPlacementJson, FString& Error);
};
