#pragma once
#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/GCObject.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSOriginalMaterialReceipt.h"
#include "DDSRobotLifecycleOwner.generated.h"

class UDDSRobotPhysicsRigComponent;
class FPhysScene_Chaos;
class FDDSRobotLifecycleLease;
class UBodySetup;
class UMaterialInterface;
class UPhysicalMaterial;
struct FDDSOriginalRobotNativeGeometry;
struct FDDSOriginalCargoVertexSourceReceipt;

// A02 instantiates this source component with the SAME supplier mesh, material,
// collision and admitted scale. Plain uncontrolled mesh components are refused.
UCLASS()
class DDSCITY_API UDDSRobotLeasedMeshComponent final : public UStaticMeshComponent
{
    GENERATED_BODY()
public:
    virtual bool SetStaticMesh(UStaticMesh* Mesh) override;
    virtual UBodySetup* GetBodySetup() override;
    // Shape-authoring entry only: unregistered, NoCollision, no physics state or lease.
    // Builds its own exact original shape result; no arbitrary bodysetup setter.
    bool PrepareOriginalChassisCompoundFromObservedGeometry(const FDDSOriginalRobotNativeGeometry& Observed, FString& Error);
    bool HasPreparedOriginalChassisCompound() const { return OriginalChassisCompoundSetup != nullptr; }
    bool PreparedOriginalChassisCompoundUnchanged() const;
    bool PrepareOriginalCargoVertexCollisionFromObservedGeometry(const FDDSOriginalRobotNativeGeometry& Observed,FString& Error);
    bool HasPreparedOriginalCargoVertexCollision() const { return OriginalCargoVertexSetup != nullptr; }
    bool PreparedOriginalCargoVertexCollisionUnchanged() const;
    const FString& GetPreparedOriginalCargoVertexSha256() const { return OriginalCargoVertexSha256; }
    int32 GetPreparedOriginalCargoVertexCount() const { return OriginalCargoVertexCount; }
    virtual void BeginDestroy() override;
protected:
    virtual void OnCreatePhysicsState() override;
    virtual void OnDestroyPhysicsState() override;
    virtual void OnUpdateTransform(EUpdateTransformFlags Flags, ETeleportType Teleport) override;
private:
    friend class FDDSRobotLifecycleLease;
    TWeakPtr<FDDSRobotLifecycleLease, ESPMode::ThreadSafe> Lease;
    UPROPERTY(Transient) TObjectPtr<UBodySetup> OriginalChassisCompoundSetup;
    UPROPERTY(Transient) TArray<TObjectPtr<UObject>> OriginalChassisCompoundSources;
    UPROPERTY(Transient) TObjectPtr<UPhysicalMaterial> OriginalChassisCompoundPreparedMaterial;
    bool bOriginalChassisCompoundPrepared=false;
    FGuid OriginalChassisCompoundGuid;
    TArray<uint8> OriginalChassisCompoundCollisionBytes, OriginalChassisCompoundSettingsBytes;
    bool bPreparingOriginalChassisCompound=false;
    UPROPERTY(Transient) TObjectPtr<UBodySetup> OriginalCargoVertexSetup;
    UPROPERTY(Transient) TArray<TObjectPtr<UObject>> OriginalCargoVertexSources;
    UPROPERTY(Transient) TObjectPtr<UPhysicalMaterial> OriginalCargoVertexPreparedMaterial;
    TSharedPtr<const FDDSOriginalCargoVertexSourceReceipt> OriginalCargoVertexSourceReceipt;
    FString OriginalCargoVertexSha256;int32 OriginalCargoVertexCount=0;
    bool bOriginalCargoVertexPrepared=false,bPreparingOriginalCargoVertex=false;
    FGuid OriginalCargoVertexGuid;
    TArray<uint8> OriginalCargoVertexCollisionBytes,OriginalCargoVertexSettingsBytes;
    FVector AdmittedScale = FVector::OneVector;
    uint64 PhysicsStateRevision = 0;
    void RetireBeforeMutation();
};

// Concrete non-UObject owner. FGCObject pins scene-owning World, Actor, Rig and
// all 15 supplied components while physics may dereference them. Explicit actor
// EndPlay/body destruction/world cleanup/scene term retires and removes pins.
class FDDSRobotLifecycleLease final : public FGCObject,
    public TSharedFromThis<FDDSRobotLifecycleLease, ESPMode::ThreadSafe>
{
public:
    static TSharedPtr<FDDSRobotLifecycleLease, ESPMode::ThreadSafe> Acquire(
        UDDSRobotPhysicsRigComponent* Rig, const FDDSRobotMeasuredBinding& B, FString& Error);
    virtual ~FDDSRobotLifecycleLease() override;
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("DDSRobotLifecycleLease"); }
    FPhysScene_Chaos* GetLiveScene() const { return bReleased ? nullptr : Scene; }
    bool IsBuilding() const { return bBuilding && !bReleased && bConstructionValid; }
    void RetireSynchronously();
    void OnWorldTearDown(UWorld* World);
    void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bResources);
    void OnSceneTerm(FPhysScene_Chaos* DyingScene);
    void OnBeforeGC();
    void OnEngineExit();
private:
    friend class UDDSRobotPhysicsRigComponent;
    friend class UDDSRobotLeasedMeshComponent;
    FDDSRobotLifecycleLease() = default;
    struct FPartReceipt
    {
        UStaticMesh* Mesh = nullptr;
        UBodySetup* BodySetup = nullptr;
        FGuid BodySetupGuid;
        TArray<uint8> OriginalCollisionData, OriginalSetupSettings;
        UBodySetup* ComponentBodySetup = nullptr;
        FGuid ComponentBodySetupGuid;
        TArray<uint8> ComponentCollisionData, ComponentSetupSettings;
        UPhysicalMaterial* OriginalSetupMaterial = nullptr;
        UPhysicalMaterial* ComponentSetupMaterial = nullptr;
        TSharedPtr<FDDSOriginalMaterialReceipt> MaterialReceipt;
        FTransform OriginalRigidPose;
        FVector OriginalScale;
        uint64 ExpectedPhysicsRevision = 0;
        const void* ActorHandle = nullptr;
        const void* Geometry = nullptr;
        int32 ParticleId = INDEX_NONE;
        TArray<const void*> Shapes, ShapeGeometry;
    };
    enum class EOwnedHookPhase : uint8 { None, Destroy, Create };
    TArray<FPartReceipt> Receipts;
    UDDSRobotLeasedMeshComponent* OwnedSubject = nullptr;
    EOwnedHookPhase OwnedPhase = EOwnedHookPhase::None;
    uint64 OwnedOperationNonce = 0, ActiveOperationNonce = 0;
    uint64 ConstructionRevision = 1;
    bool bConstructionValid = true;
    uint64 AdmissionGeneration = 0, AdmissionWorldEpoch = 0, AdmissionReadinessRevision = 0;
    void SetAdmissionBarrier(uint64 Generation, uint64 WorldEpoch, uint64 ReadinessRevision);
    bool ExecuteOwnedOperation(UDDSRobotLeasedMeshComponent* Subject, TFunctionRef<void()> Operation);
    bool PrepareOwnedMobility();
    bool ConsumeOwnedPhysicsHook(UDDSRobotLeasedMeshComponent* Subject, bool bCreate);
    void IndependentMutation();
    bool CaptureOwnedBodyReceipt(UDDSRobotLeasedMeshComponent* Subject);
    bool CollisionReceiptsUnchanged(UDDSRobotLeasedMeshComponent* Subject) const;
    bool MaterialReceiptsUnchanged(bool bCheckPose) const;
    bool CommitAdmission(const FDDSRobotMeasuredBinding& Binding,
        const FDDSRobotPhysicsProfile& Profile, UPhysicalMaterial* ContactMaterial, FString& Error);
    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UDDSRobotPhysicsRigComponent* Rig = nullptr;
    FPhysScene_Chaos* Scene = nullptr; // Owned World pinned; scene-term clears BEFORE base destroy.
    TArray<UDDSRobotLeasedMeshComponent*> Parts;
    FDelegateHandle WorldTearDown, WorldCleanup, SceneTerm, PreGC, PreExit;
    bool bBuilding = true, bRetiring = false, bReleased = false;
};
