#pragma once
#include "CoreMinimal.h"
#include "DDSRobotN12Types.h"

class UStaticMeshComponent;

// SI at every profile/binding boundary. UE conversion occurs once in the adapter.
namespace DDSRobotUnits
{
    constexpr double CmPerM = 100.0;
    constexpr double UETorquePerNm = 10000.0;
    constexpr double UEInertiaPerKgM2 = 10000.0;
    inline FVector Position(const FVector& M) { return M * CmPerM; }
    inline FVector Force(const FVector& N) { return N * CmPerM; }
    inline FVector Torque(const FVector& Nm) { return Nm * UETorquePerNm; }
}

enum class EDDSRobotSupport : uint8 { Unknown, NoLoadedContact, SolvedLoaded, PersistentSleeping };
enum class EDDSRobotRigState : uint8 { Unbound, Binding, Active, Retiring, Retired, Fault };
// Ownership/admission scope only. Selecting a scope never creates measurements,
// solver readiness, drive permission or frozen mechanics/service evidence.
enum class EDDSOriginalAssemblyScope : uint8 { FullService, ClosedLidDrive };
enum class EDDSRobotPartOwner : uint8 { Chassis, Wheel0, Wheel1, Wheel2, Wheel3, Wheel4, Wheel5, Lid0, Lid1 };

struct FDDSRobotServiceLimits
{
    // No default constitutes a measured service capability.
    bool bFrozen = false;
    double MaxSampleAgeS = 0.0;
    double MaxLinearSpeedMps = 0.0;
    double MaxAngularSpeedRadps = 0.0;
    double MaxVerticalSpeedMps = 0.0;
    double MaxPitchRad = 0.0;
    double MaxRollRad = 0.0;
    double RequiredStableDurationS = 0.0;
    int32 MinLoadedWheels = 6;
};

struct FDDSRobotPhysicsProfile
{
    FString ProfileSha256;
    FString Provenance = TEXT("Declared GAME calibration hypotheses; not Bendylab specifications");
    double GravityMps2 = 9.80665;
    double TargetStepS = 1.0 / 120.0;
    double BareChassisKg = 30.0;
    double WheelKg = 1.0;
    // Independent caller-supplied masses aligned with binding Bodies[7] and [8].
    // Zero means unset; native individual observations alone grant no calibration.
    double Lid0Kg = 0.0; // Bodies[7] / EDDSRobotPartOwner::Lid0
    double Lid1Kg = 0.0; // Bodies[8] / EDDSRobotPartOwner::Lid1
    double ParcelKg = 2.0;
    double StaticFriction = 0.8;
    double DynamicFriction = 0.6;
    double Restitution = 0.05;
    double MotorPeakNm = 5.0;
    double BrakePeakNm = 8.0;
    double BearingNmPerRadps = 0.02;
    double LidPeakNm = 2.0;
    // Explicit additional hypotheses which require freezing before calibration.
    double MotorNoLoadRadps = 20.0;
    double MotorErrorNmPerRadps = 0.5;
    double BrakeRegularizationRadps = 0.1;
    double SuspensionK_Npm = 2574.245625;
    double SuspensionC_Nspm = 186.00500315851724;
    double SuspensionForceLimitN = 500.0;
    // Declared current-state/permit freshness and setup timeout hypotheses.
    double CurrentStateMaxAgeS = 0.05;
    double RecoveryPermitMaxAgeS = 0.05;
    double BindingTimeoutRealS = 1.0;
    FDDSRobotServiceLimits Service;
};

struct FDDSRobotBodyMeasurement
{
    FString SuppliedPartId;
    FString SuppliedMeshPackage;
    FString MeshPackageSha256;
    FString SimpleCollisionSha256;
    FString MeasurementRecordSha256;
    UStaticMeshComponent* ActualPart = nullptr; // Game thread only; never callback input.
    FTransform RestInChassisMeters = FTransform::Identity;
    FVector SupplierScale = FVector::OneVector;
    FVector COMLocalMeters = FVector::ZeroVector;
    FVector PrincipalInertiaKgM2 = FVector::ZeroVector;
    FQuat PrincipalAxesInBody = FQuat::Identity;
    bool bMeasured = false;
    bool bSimpleCollisionMeasured = false;
    bool bCCDMeasuredRequired = false;
    FName MeasuredCollisionProfile;
    int32 MeasuredMotorForwardSign = 0; // Wheels only: +1/-1 on measured joint X.
};

struct FDDSRobotJointMeasurement
{
    FTransform InChassisMeters = FTransform::Identity;
    FTransform InPartMeters = FTransform::Identity;
    // X is axle/hinge, Z is measured suspension. Frames must actually meet at rest.
    double SuspensionHalfTravelM = 0.0;
    double SuspensionRestTargetM = 0.0;
    double LidOpenLimitRad = 0.0;
    bool bMeasured = false;
    bool bAdjacentCollisionExclusionJustified = false;
};

struct FDDSRobotSuspensionPartMap
{
    FString SuppliedPartId;
    UStaticMeshComponent* ActualPart = nullptr;
    EDDSRobotPartOwner Owner = EDDSRobotPartOwner::Chassis;
    FTransform RestInOwnerMeters = FTransform::Identity;
    FVector SupplierScale = FVector::OneVector; // Preserved supplier component world scale.
    double DeclaredMassAllocationKg = 0.0; // Included in owner mass, NEVER added twice.
    bool bMeasured = false;
    bool bRigidRelativeOwnerThroughoutMotion = false;
    bool bCollisionAlreadyIncludedInOwnerShape = false;
    FString GeometryAndMassMapSha256;
};

struct FDDSRobotMeasuredBinding
{
    FDDSRobotBodyMeasurement Parcel; // Optional tenth pre-existing dynamic body.
    FDDSN12Calibration Mechanics;
    FString BindingSha256;
    FString A02MeasurementCertificateSha256;
    FDDSRobotBodyMeasurement Bodies[9]; // Chassis, six wheels, two lids.
    FDDSRobotJointMeasurement Joints[8];
    FDDSRobotSuspensionPartMap SuspensionParts[6];
    FTransform SensorMountInChassisMeters = FTransform::Identity;
    FTransform ChaseMountInChassisMeters = FTransform::Identity;
    FTransform ServiceSocketInChassisMeters = FTransform::Identity;
    FTransform CargoMountInChassisMeters = FTransform::Identity;
    // Rest, COM, joint and mount frames are PHYSICAL scaled SI measured after
    // supplier component scale, with rigid orientation and unit frame scale.
    bool bFramesAreScaledPhysicalSI = false;
    bool bMountsMeasured = false;
    bool bNineBodyTopologyVerified = false;
    // A02 contact/collision approximation error budget; never guessed from proxy.
    double MaxContactSeparationM = 0.0;
    double MinSupportUpDot = 0.0;
    double MinLoadedImpulseNs = 0.0;
    double SleepingAnchorToleranceM = 0.0;
    double SleepingSpeedToleranceMps = 0.0;
    double SleepingOmegaToleranceRadps = 0.0;
    double SlipDomainSpeedMps = 0.0;
    double ClosedLidAngleToleranceRad = 0.0;
    double ClosedLidOmegaToleranceRadps = 0.0;
};

struct FDDSRobotCommand
{
    uint64 Generation = 0;
    uint64 CollisionWorldGeneration = 0;
    uint64 ReadinessRevision = 0;
    uint64 Sequence = 0;
    double IssuedRealS = 0.0;
    double MaxAgeRealS = 0.25; // Input watchdog hypothesis; no physics dt clamp.
    double LeftRequest = 0.0;
    double RightRequest = 0.0;
    bool bBrake = false;
    bool bDriveLocked = true;
    bool bCollisionWorldReady = false;
};

struct FDDSRobotBodyState
{
    int32 ParticleId = INDEX_NONE;
    bool bLive = false;
    bool bSleeping = false;
    FTransform WorldCm = FTransform::Identity;
    FVector VelocityCmps = FVector::ZeroVector;
    FVector AngularVelocityRadps = FVector::ZeroVector;
    FVector AccelerationCmps2 = FVector::ZeroVector;
    double MassKg = 0.0;
    FVector COMLocalCm = FVector::ZeroVector;
    FVector InertiaKgCm2 = FVector::ZeroVector;
    FQuat PrincipalAxesInBody = FQuat::Identity;
};

struct FDDSRobotWheelState
{
    EDDSRobotSupport Support = EDDSRobotSupport::Unknown;
    double WitnessTimeS = 0.0;
    double LastSolvedTimeS = 0.0;
    int32 SupportParticleId = INDEX_NONE;
    uint64 WheelShapeId = 0;
    uint64 SupportShapeId = 0;
    FVector ContactPointCm = FVector::ZeroVector;
    FVector ContactNormal = FVector::ZeroVector;
    // Raw detection-frame geometry belongs to the prior solve, NOT current support.
    FVector RawSolveWheelPointCm = FVector::ZeroVector;
    FVector RawSolveOtherPointCm = FVector::ZeroVector;
    FVector RawSolveNormal = FVector::ZeroVector;
    double RawSolvePhiCm = 0;
    double CurrentSeparationCm = 0;
    FVector SolvedImpulseKgCmps = FVector::ZeroVector;
    FVector SolvedPushOutKgCm = FVector::ZeroVector;
    FVector SupportContactVelocityCmps = FVector::ZeroVector;
    double AxleAngleRad = 0.0; // Unwrapped actual relative quaternion, not commanded travel.
    double AxleOmegaRadps = 0.0;
    double SuspensionTravelM = 0.0;
    double MotorNm = 0.0;
    double BrakeAndBearingNm = 0.0;
    bool bSlipDefined = false;
    double LongitudinalSlip = 0.0;
};

struct FDDSRobotSnapshot
{
    uint64 Generation = 0;
    uint64 CollisionWorldGeneration = 0;
    uint64 ReadinessRevision = 0;
    uint64 CommandSequence = 0;
    uint64 StepSequence = 0;
    uint64 CoalescedCommandCount = 0;
    double CompletedStateTimeS = 0.0;
    double CallbackStartTimeS = 0.0;
    double ActualCallbackDtS = 0.0;
    double PreviousEvolutionDtS = 0.0;
    double CapturedRealS = 0.0;
    double InputAgeRealS = 0.0;
    double ExternalRenderDeltaS = 0.0; // Set by GT consumer, never integration dt.
    double ConsumedRealS = 0.0;
    double SnapshotTransportAgeS = 0.0;
    uint64 CoalescedOutputCount = 0;
    bool bCompleteState = false;
    bool bTimingEnvelopeValid = false;
    bool bCommandFresh = false;
    bool bWorldReady = false;
    bool bClosedLidBaseline = false;
    bool bChassisSupportStable = false; // Valid while lids are open; NEVER admits initial E/commit.
    bool bServiceStable = false;
    FDDSN12StepState Mechanics;
    bool bAllJointsAdmitted = false;
    bool bRecoveryDriveInhibited = false;
    uint8 SupportMask = 0;
    FVector ActualGravityCmps2 = FVector::ZeroVector;
    double TotalMassKg = 0.0;
    FVector AssemblyCOMWorldCm = FVector::ZeroVector;
    double AssemblyInertiaWorldKgCm2[3][3] = {};
    FDDSRobotBodyState Bodies[9];
    FDDSRobotWheelState Wheels[6];
};

// Mint only after N08 generation cancellation, N12 actual closed/latch proof,
// N11 save permission and full assembly overlap/support validation.
struct FDDSRobotRecoveryPermit
{
    uint64 ExpectedGeneration = 0;
    uint64 CollisionWorldGeneration = 0;
    uint64 ReadinessRevision = 0;
    uint64 NewGeneration = 0;
    uint64 RequestId = 0;
    double ValidatedRealS = 0;
    FString LedgerFingerprint;
    bool bAuthorityCancelledOldOperation = false;
    bool bBothActualLidsClosedLatched = false;
    bool bN11PersistenceAllowsReset = false;
    bool bFullAssemblyPoseAndSupportValidated = false;
    FTransform WorldChassisCm = FTransform::Identity;
};

// Physical coordinates never inherit supplier component scale twice.
inline FTransform DDSPhysicalCm(const FTransform& M)
{ return FTransform(M.GetRotation(), M.GetLocation()*100.0, FVector::OneVector); }
inline FTransform DDSRigidComponentPose(const FTransform& C)
{ return FTransform(C.GetRotation(), C.GetLocation(), FVector::OneVector); }
inline FTransform DDSVisualAttachmentCm(const FTransform& PhysicalMeters, const FVector& SupplierScale)
{ return FTransform(PhysicalMeters.GetRotation(), PhysicalMeters.GetLocation()*100.0/SupplierScale, FVector::OneVector); }

bool DDSValidateRobotProfile(const FDDSRobotPhysicsProfile& P, FString& Error);
bool DDSValidateMeasuredBinding(const FDDSRobotMeasuredBinding& B,
    const FDDSRobotPhysicsProfile& P, FString& Error);
double DDSMotorTorqueNm(double Request, double RelativeOmega, const FDDSRobotPhysicsProfile& P);
double DDSBrakeBearingTorqueNm(bool bBrake, double RelativeOmega, const FDDSRobotPhysicsProfile& P);
