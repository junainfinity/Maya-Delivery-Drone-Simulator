#pragma once
#include "CoreMinimal.h"
#include "DDSOriginalRobotRuntimeOwner.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSOriginalChassisCompoundCollision.h"
#include "Components/StaticMeshComponent.h"
#include "DDSOriginalRobotMeasurementProducer.generated.h"

class AActor;
class USceneComponent;
class UPhysicsConstraintComponent;
class FPhysScene_Chaos;
struct FDDSOriginalCalibrationCompoundReceipt;

// Private native-fixture adapter. This is deliberately NOT a production leased
// component; the live binding consumer cannot admit it. No arbitrary setup setter.
UCLASS(Transient, NotBlueprintable)
class DDSCITY_API UDDSOriginalCalibrationMeshComponent final : public UStaticMeshComponent
{
    GENERATED_BODY()
public:
    UDDSOriginalCalibrationMeshComponent(const FObjectInitializer& ObjectInitializer);
    UDDSOriginalCalibrationMeshComponent(FVTableHelper& Helper);
    virtual ~UDDSOriginalCalibrationMeshComponent() override;
    virtual bool SetStaticMesh(UStaticMesh* Mesh) override;
    virtual UBodySetup* GetBodySetup() override;
    virtual void BeginDestroy() override;
private:
    friend class FDDSOriginalRobotMeasurementProducer;
    bool PrepareDiagnosticCompound(const FDDSOriginalRobotNativeGeometry& OriginalCityObservation, FString& Error);
    bool DiagnosticCompoundUnchanged() const;
    UPROPERTY(Transient) TObjectPtr<UBodySetup> DiagnosticSetup;
    UPROPERTY(Transient) TArray<TObjectPtr<UObject>> DiagnosticLifetime;
    TUniquePtr<FDDSOriginalCalibrationCompoundReceipt> DiagnosticReceipt;
    bool bPreparingDiagnosticCompound = false;
};

// All source observations below are drafts. They are not a measured binding,
// a manufacturer specification, collision readiness, N12 or service permission.
struct FDDSOriginalRobotNativeBodyObservation
{
    bool bAvailable = false;
    FString UnavailableReason;
    FString ComponentObjectPath, ComponentBodySetupObjectPath, MeshBodySetupObjectPath;
    FGuid ComponentBodySetupGuid;
    int32 NativeSimulationShapeCount = 0;
    double NativeMassKg = 0.0;
    FVector NativePrincipalInertiaKgM2 = FVector::ZeroVector;
    FTransform NativeMassSpaceBodyLocalMeters = FTransform::Identity;
    FTransform NativeActorWorldCm = FTransform::Identity;
    FVector NativeVelocityCmps = FVector::ZeroVector;
    FVector NativeOmegaRadps = FVector::ZeroVector;
    FName NativeComponentCollisionProfile;
    // Linear constant-density ratio at the SAME preserved physical shape/scale.
    // This GAME hypothesis keeps native COM/principal axes; it is not calibration.
    double DeclaredGameMassKg = 0.0;
    double GameToNativeMassRatio = 0.0;
    FVector DeclaredGamePrincipalInertiaKgM2 = FVector::ZeroVector;
};

struct FDDSOriginalRobotJointSourceObservation
{
    bool bAvailable = false;
    FString SourceObjectPath;
    FTransform NativeInChassisMeters = FTransform::Identity;
    FTransform NativeInPartMeters = FTransform::Identity;
    double NativeLinearLimitM = 0.0;
    bool bNativeAdjacentCollisionDisabled = false;
    uint64 NativeConstraintIdentity = 0; // Run-local address identity; never dereferenced externally.
    bool bNativeParticleIdsAvailable = false;
    int32 ChassisParticleId = INDEX_NONE, PartParticleId = INDEX_NONE;
    int32 NativeLinearMotion[3] = {}, NativeAngularMotion[3] = {};
    FVector NativeAngularLimitsRad = FVector::ZeroVector;
    int32 NativeLinearDriveForceMode = INDEX_NONE;
    bool NativeLinearPositionDriveEnabled[3] = {}, NativeLinearVelocityDriveEnabled[3] = {};
    FVector NativeLinearDriveTargetMeters = FVector::ZeroVector;
    FVector NativeLinearDriveVelocityTargetMps = FVector::ZeroVector;
    FVector NativeLinearDriveStiffnessNpm = FVector::ZeroVector;
    FVector NativeLinearDriveDampingNspm = FVector::ZeroVector;
    FVector NativeLinearDriveMaxForceN = FVector::ZeroVector;
};

// Authored GAME frames on the actual carrier, separate from supplier asset facts.
// All four physical SI declarations and their provenance must be supplied.
struct FDDSOriginalRobotGameMountDeclarations
{
    bool bExplicitGameDesign = false;
    FString Provenance;
    FTransform InChassisMeters[4]; // sensor/chase/service/cargo; no parcel object.
};
struct FDDSOriginalRobotGameMountLease
{
private:
    friend class FDDSOriginalRobotMeasurementProducer;
    TWeakObjectPtr<AActor> Actor;
    TWeakObjectPtr<UStaticMeshComponent> Chassis;
    TWeakObjectPtr<UDDSRobotPhysicsRigComponent> Rig;
    const FPhysScene_Chaos* Scene = nullptr;
    uint64 Generation=0,ReadinessRevision=0;
    TWeakObjectPtr<USceneComponent> Sources[4];
};
enum class EDDSOriginalCalibrationTrialPhase : uint8
{ Settle, PositivePulse, CoastAfterPositive, NegativePulse, CoastAfterNegative, Drain };

struct FDDSOriginalRobotDraftInputs
{
    // Explicit GAME targets; caller supplies both independent lid masses.
    // A nonempty profile hash is a source input, never a native result.
    bool bExplicitGameTargets = false;
    FDDSRobotPhysicsProfile DeclaredGameProfile;
    // Optional actual registered sources. Absence stays absent in the draft.
    TWeakObjectPtr<UPhysicsConstraintComponent> JointSources[8];
    // Sensor, chase, service socket, cargo mount. These are frames, no parcel.
    TWeakObjectPtr<USceneComponent> MountSources[4];
};

struct FDDSOriginalRobotMeasurementDraft
{
    FString Scope = TEXT("CAPTURE_DRAFT_ONLY: original15 robot parts; no parcel, no admission");
    // Every bMeasured/topology/mount/rigidity/admission flag remains false,
    // and every binding/certificate/measurement/collision-map hash stays empty.
    FDDSRobotMeasuredBinding UnadmittedBinding;
    bool bExplicitGameTargets = false;
    FDDSRobotPhysicsProfile DeclaredGameProfile;
    FDDSOriginalRobotNativeGeometry SourceGeometry;
    TWeakObjectPtr<AActor> SourceActor;
    TWeakObjectPtr<UStaticMeshComponent> SourceVisuals[15];
    FDDSOriginalRobotNativeBodyObservation Bodies[9];
    FDDSOriginalRobotJointSourceObservation Joints[8];
    bool bMountSourceAvailable[4] = {};
    FString MountSourceObjectPaths[4];
    FTransform MountsInChassisMeters[4];
    // Exact content snapshot, not a guessed collision hash or render-bound proxy.
    TArray<uint8> ComponentConvexContent[9];
    TArray<FString> MissingEvidence;
};

// Original-support-only diagnostic receipt. IDs are run-local; source and fixture
// IDs are distinct and never grant a body binding, collision readiness or drive.
struct FDDSOriginalRobotSupportNativeShapeObservation
{
    int32 ShapeIndex = INDEX_NONE, GeometryType = INDEX_NONE, CollisionShapeType = INDEX_NONE;
    uint64 ShapeIdentity = 0, GeometryIdentity = 0, AssetQueryGeometryIdentity = 0;
    bool bSimulation = false, bQuery = false, bProbe = false;
    int32 CollisionTraceType = INDEX_NONE, NativeMaterialCount = 0;
    uint32 QueryFilter[4] = {}, SimulationFilter[4] = {};
    FVector NativeBoundsMinCm = FVector::ZeroVector, NativeBoundsMaxCm = FVector::ZeroVector;
    FVector NativeBoxMinCm = FVector::ZeroVector, NativeBoxMaxCm = FVector::ZeroVector;
    double NativeBoxMarginCm = 0.0;
    // Exact bounded geometry/material content, separate from address identity.
    TArray<uint8> GeometryContent, MaterialContent;
};
struct FDDSOriginalRobotSupportNativeBodyObservation
{
    bool bAvailable = false, bStatic = false;
    uint64 SceneIdentity = 0, SolverIdentity = 0, ActorIdentity = 0;
    int32 ParticleId = INDEX_NONE, OwnerObjectId = INDEX_NONE, ComponentObjectId = INDEX_NONE;
    FTransform NativeActorWorldCm = FTransform::Identity;
    TArray<FDDSOriginalRobotSupportNativeShapeObservation> Shapes;
};
struct FDDSOriginalRobotSupportAdapterObservation
{
    FString AdapterTag = TEXT("LEGACY_CONVEX_UNIFORM");
    bool bRawSourceAvailable = false, bNativeEquivalent = false;
    FString SourceComponentPath, SourceOwnerPath, SourceWorldPath, OriginalMeshPath, OriginalSetupPath;
    FGuid OriginalSetupGuid;
    FTransform OriginalComponentWorldCm = FTransform::Identity;
    FVector BuildScale3D = FVector::ZeroVector, BoxDimensionsCm = FVector::ZeroVector, BoxCenterCm = FVector::ZeroVector;
    FRotator BoxRotation = FRotator::ZeroRotator;
    int32 CollisionTraceFlag = INDEX_NONE, BoxCollisionEnabled = INDEX_NONE;
    FName ComponentCollisionProfile;
    TArray<uint8> AggregateGeometryContent, SetupSettingsContent, ComponentBodySettingsContent;
    FDDSOriginalRobotSupportNativeBodyObservation SourceNative, FixtureNative;
};

struct FDDSOriginalRobotCalibrationRecipe
{
    // Authoring a GAME fixture does not certify original supplier kinematics.
    // Caller must explicitly opt in; no default joint/frame/travel is evidence.
    bool bExplicitGameFixtureHypotheses = false;
    FString HypothesisDescription;
    FDDSRobotJointMeasurement JointHypotheses[8];
    // Current component compound includes all six suspension shapes in chassis0.
    // Positive allocations are declared shares INCLUDED in total chassis mass.
    double DeclaredSuspensionMassAllocationKg[6] = {};
    // Exact original support component; copied original simple collision only.
    TWeakObjectPtr<UStaticMeshComponent> OriginalSupportSource;
    // Default OFF: only an unchanged original single axis-aligned simple box.
    bool bExplicitOriginalSingleBoxSupportAdapter = false;
    FTransform FixtureChassisWorldCm = FTransform::Identity;
    FTransform FixtureSupportWorldCm = FTransform::Identity;
    FVector DeclaredForwardInChassis = FVector::ZeroVector;
    double PulseTorqueNm = 0.0;
    int32 SettleSteps = 0, PulseStepsPerWheel = 0, CoastStepsPerWheel = 0;
    // Explicit engineering limits, never measured physical constants. Legacy
    // diagnostic callers keep their old positive-pulse schedule until opted in.
    bool bExplicitBidirectionalTrials = false;
    bool bExplicitGameAcceptanceLimits = false;
    double GameSlipDomainSpeedMps = 0.0;
    double GameSleepingAnchorToleranceM = 0.0;
    double GameSleepingSpeedToleranceMps = 0.0;
    double GameSleepingOmegaToleranceRadps = 0.0;
    double GameMinSupportUpDot = 0.0;
    double GameMinLoadedImpulseNs = 0.0;
    double GameMinAxleResponseRadps = 0.0;
    double GameMinRollingForwardMps = 0.0;
    double GameMinCOMForwardMps = 0.0;
    int32 GameMinLoadedSamplesPerDirection = 0;
    double GameMaxContactSeparationM = 0.0, GameClosedLidAngleToleranceRad = 0.0, GameClosedLidOmegaToleranceRadps = 0.0;
};

struct FDDSOriginalRobotCalibrationWheelSample
{
    bool bSolvedContactObserved = false;
    int32 SupportParticleId = INDEX_NONE;
    FVector ContactPointCm = FVector::ZeroVector, ContactNormal = FVector::ZeroVector;
    double RawDetectionPhiCm = 0.0, CurrentSeparationCm = 0.0;
    double RawNormalLoadImpulseKgCmps = 0.0;
    double RelativeAxleOmegaRadps = 0.0;
    double AngularContactRollingForwardCmps = 0.0;
    double WheelCOMForwardRelativeToSupportCmps = 0.0;
    int32 WheelParticleId = INDEX_NONE;
    uint64 WheelShapeId = 0, SupportShapeId = 0, WheelGeometryId = 0, SupportGeometryId = 0;
    FVector WheelLocalAnchorCm = FVector::ZeroVector, SupportLocalAnchorCm = FVector::ZeroVector;
    FVector NormalInSupport = FVector::ZeroVector, SupportPointCm = FVector::ZeroVector;
    FVector RawSolveWheelPointCm = FVector::ZeroVector, RawSolveOtherPointCm = FVector::ZeroVector, RawSolveNormal = FVector::ZeroVector;
    FVector SolvedImpulseKgCmps = FVector::ZeroVector, SolvedPushOutKgCm = FVector::ZeroVector;
    FVector SupportContactVelocityCmps = FVector::ZeroVector, WheelContactVelocityCmps = FVector::ZeroVector;
    double GroundFrameForwardSpeedCmps = 0.0, LongitudinalSlip = 0.0;
    bool bSlipDefined = false, bWheelSleeping = false, bSupportSleeping = false, bSupportStatic = false;
    bool bSleepingAnchorTransportObserved = false, bSleepingWitnessWithinDeclaredGameLimits = false;
    double SleepingAnchorDistanceCm = 0.0, LastSolvedCallbackTimeS = 0.0;
    double WheelSpeedCmps = 0.0, SupportSpeedCmps = 0.0, WheelOmegaRadps = 0.0, SupportOmegaRadps = 0.0;
    bool bConsecutiveLoadedResponseAvailable = false;
    double AxleOmegaDeltaRadps = 0.0, COMForwardVelocityDeltaCmps = 0.0;
    bool bContactWithinDeclaredGameLimits = false;
};

struct FDDSOriginalRobotCalibrationStepSample
{
    uint64 Sequence = 0;
    // PreviousCallbackDtS is the prior callback delta, not an independent
    // evolution-step observation.
    double CallbackStartS = 0.0, CallbackDtS = 0.0, PreviousCallbackDtS = 0.0;
    double ConfiguredSolverAsyncDtS = 0.0;
    bool bPreviousCompletedState = false;
    int32 NativeDynamicBodyCount = 0, NativeMatchingJointCount = 0;
    int32 AppliedPulseWheel = INDEX_NONE, PreviousPulseWheel = INDEX_NONE;
    int32 AppliedTrialWheel = INDEX_NONE, PreviousTrialWheel = INDEX_NONE;
    EDDSOriginalCalibrationTrialPhase AppliedTrialPhase = EDDSOriginalCalibrationTrialPhase::Settle;
    EDDSOriginalCalibrationTrialPhase PreviousTrialPhase = EDDSOriginalCalibrationTrialPhase::Settle;
    double AppliedPulseNm = 0.0, PreviousPulseNm = 0.0;
    FVector ActualGravityCmps2 = FVector::ZeroVector;
    FDDSRobotBodyState Bodies[9];
    FDDSOriginalRobotCalibrationWheelSample Wheels[6];
    double ClosedLidAngleRad[2] = {}, ClosedLidOmegaRadps[2] = {};
    FDDSOriginalRobotJointSourceObservation NativeJoints[8];
    bool NativeBodyCCDEnabled[9] = {};
    double ClosedLidFullAngularErrorRad[2] = {}, ClosedLidRelativeOmegaMagnitudeRadps[2] = {};
};
struct FDDSOriginalRobotSuspensionMotionSample
{
    int32 NativeFrameIndex = INDEX_NONE;
    uint64 ObservedCallbackSequenceThrough = 0;
    int32 ChassisParticleId = INDEX_NONE;
    FTransform NativeChassisWorldCm = FTransform::Identity, ComponentChassisWorldCm = FTransform::Identity;
    FTransform ComponentWorldCm[6], RelativeToOwnerMeters[6];
};

struct FDDSOriginalRobotSuspensionConstituentObservation
{
    FString OriginalPartId, OriginalMeshObjectPath, OriginalMeshPackageSha256, FixtureComponentPath;
    FGuid OriginalBodySetupGuid;
    int32 ObservedOwnerBodyIndex = INDEX_NONE;
    bool bObservedNoIndependentSimulation = false, bObservedNoIndependentCollision = false;
    double DeclaredIncludedOwnerMassShareKg = 0.0, ObservedOwnerNativeMassKg = 0.0;
    FTransform ObservedRestInOwnerMeters = FTransform::Identity;
    TArray<FDDSOriginalCompoundHullOrigin> IncludedCompoundHullOrigins;
};
struct FDDSOriginalRobotBidirectionalWheelObservation
{
    int32 PositiveLoadedResponseSamples = 0, NegativeLoadedResponseSamples = 0, CoastSamples = 0;
    int32 PositiveCoastSamples = 0, NegativeCoastSamples = 0;
    int32 CoherentForwardSignCandidate = 0;
    bool bContradictoryResponseObserved = false;
    // A completed trial is a native diagnostic observation, never admission.
    bool bBothLoadedDirectionsObserved = false;
};

struct FDDSOriginalRobotNativeCalibrationDraft
{
    FString Scope = TEXT("NATIVE_GAME_FIXTURE_DRAFT_ONLY: collision diagnostics, no binding certificate");
    // Exact ordered refusal; later recipe leaves stay UNKNOWN_NOT_EVALUATED.
    FString FirstFailedPredicate;
    TArray<FString> EvaluatedRecipePredicates;
    FDDSOriginalRobotSupportAdapterObservation SupportAdapterObservation;
    bool bBoundedRunCompleted = false;
    // This is an observed diagnostic result, not a binding topology admission.
    int32 ConstructedBodyCount = 0, ConstructedJointCount = 0;
    double ObservedWorldGravityCmps2 = 0.0, ObservedSolverAsyncDtS = 0.0;
    // Native created bodies using original mesh DefaultInstance, preserved shape
    // and declared GAME contact material; separate from the old unstepped report.
    FDDSOriginalRobotNativeBodyObservation NativeReferenceBodies[9];
    FDDSOriginalRobotNativeBodyObservation DeclaredMassEngineBodies[9];
    // Only the draft candidate sign from a native pulse/contact response.
    // UnadmittedBinding.Bodies[].MeasuredMotorForwardSign is never set here.
    int32 CandidateMotorForwardSign[6] = {};
    double ObservedMaximumSuspensionRelativePositionErrorM[6] = {};
    double ObservedMaximumSuspensionRelativeAngleErrorRad[6] = {};
    TArray<FDDSOriginalRobotCalibrationStepSample> Steps;
    TArray<FString> RemainingEvidence;
    FString FixtureWorldObjectPath, SourceActorObjectPath;
    uint64 FixtureSceneIdentity = 0, FixtureSolverIdentity = 0;
    TArray<uint8> NativeFixtureConvexContent[9];
    TArray<FDDSOriginalCompoundHullOrigin> ChassisCompoundHullOrigins;
    FDDSOriginalRobotSuspensionConstituentObservation SuspensionConstituents[6];
    bool bBidirectionalTrialsRequested = false;
    FDDSOriginalRobotBidirectionalWheelObservation BidirectionalWheels[6];
    TArray<FDDSOriginalRobotSuspensionMotionSample> PostEndFrameSuspensionMotion;
    FDDSOriginalRobotJointSourceObservation NativeCreatedJoints[8];
    bool bSuppliedDeclaredRecipeAvailable = false;
    FDDSOriginalRobotCalibrationRecipe SuppliedDeclaredRecipe;
    int32 CompletedGameLimitSamples = 0, LoadedContactSamples = 0, ContactOutsideDeclaredGameLimits = 0;
    int32 SleepingAnchorSamples = 0, SleepingAnchorOutsideDeclaredGameLimits = 0, DefinedSlipSamples = 0, ClosedLidOutsideDeclaredGameLimits = 0;
};

class DDSCITY_API FDDSOriginalRobotMeasurementProducer final
{
public:
    // Read-only to original/managed source components. Resolves exact ComponentName
    // values by Actor.GetComponents; requires exact9body+6suspension from15/16 roster.
    static bool CaptureDraft(AActor& ActualAssemblyActor,
        const FDDSOriginalRobotNativeGeometry& Geometry,
        const FDDSOriginalRobotDraftInputs& Inputs,
        FDDSOriginalRobotMeasurementDraft& Out, FString& Error);
    // Explicit caller-owned GAME mount creation. Returns actual registered source
    // components for CaptureDraft; no bMountsMeasured or permission is assigned.
    static bool CreateDeclaredGameMountSources(AActor& ActualAssemblyActor,
        UStaticMeshComponent& ActualChassis, const FDDSOriginalRobotGameMountDeclarations& Declarations,
        FDDSOriginalRobotDraftInputs& Inputs, FDDSOriginalRobotGameMountLease& Out, FString& Error);
    static bool ReleaseDeclaredGameMountSources(FDDSOriginalRobotGameMountLease& Lease, FString& Error);
    // Creates/destroys its OWN transient physics scene, original mesh components,
    // component-owned compound, eight constraints and exact original support copy.
    // Actually advances native physics; true means bounded diagnostic run ONLY.
    static bool RunCalibration(const FDDSOriginalRobotMeasurementDraft& Draft,
        const FDDSOriginalRobotCalibrationRecipe& Recipe,
        FDDSOriginalRobotNativeCalibrationDraft& Out, FString& Error);
    // Read-only source/native coherence for retained diagnostic serialization.
    static bool ConfirmOriginalSupportDiagnosticReceipt(UStaticMeshComponent& Actual,
        const FDDSOriginalRobotSupportAdapterObservation& Expected, FString& Error);
    // BuildVerifiedBinding is intentionally absent from this increment. No code
    // path promotes these draft results into a live consumer or certificate.
};
