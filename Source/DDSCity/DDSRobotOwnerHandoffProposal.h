#pragma once
#include "DDSRobotPhysicsTypes.h"
#include "DDSN08Authority.h"
#include "DDSRobotPhysicsRigComponent.h"

// PROPOSAL for N08/N09/N11/N12 owner review. This header does not change their
// authority or invent a production provider. No caller in this bundle commits.
struct FDDSN08PhysicalSafetyWitness
{
    FDDSN08Token Operation;
    uint64 PhysicsGeneration = 0;
    uint64 CollisionWorldGeneration = 0;
    uint64 ReadinessRevision = 0;
    uint64 CompletedStepSequence = 0;
    double CompletedStateTimeS = 0;
    double CapturedRealS = 0;
    bool bFreshPhysicalStableSupport = false;
    bool bCurrentLidSweepAndServicePathClear = false;
    bool bBothActualLidsClosedLatched = false;
    bool bExpectedParcelPhysicalOwnerAndMass = false;
};

enum class EDDSRobotMechanicsBoundary : uint8
{
    PrecommitNoTransfer, PrecommitStaging, PrecommitClosing, CommittedSaveLocked
};
struct FDDSRobotLidActuationRequest
{
    FDDSN08Token Operation;
    uint64 PhysicsGeneration = 0;
    uint64 CollisionWorldGeneration = 0;
    uint64 ReadinessRevision = 0;
    EDDSRobotMechanicsBoundary Boundary = EDDSRobotMechanicsBoundary::PrecommitNoTransfer;
    double TargetAngleRad[2] = {};
    bool bCancelOldActuation = false;
};
struct FDDSRobotParcelLatchRequest
{
    FDDSN08Token Operation;
    uint64 PhysicsGeneration = 0;
    FString ParcelId;
    FDDSRobotBodyMeasurement MeasuredParcel;
    double DeclaredParcelMassKg = 0;
    FTransform MeasuredMountInChassisMeters = FTransform::Identity;
    FVector ActualMountVelocityCmps = FVector::ZeroVector;
    FVector ActualMountAngularVelocityRadps = FVector::ZeroVector;
    bool bAttach = false;
    bool bN08GenerationMatched = false;
    bool bExternalStationWorkDisclosed = false;
};
struct FDDSRobotParcelLatchAck
{
    FDDSN08Token Operation;
    uint64 PhysicsGeneration = 0;
    uint64 CompletedStepSequence = 0;
    FString ParcelId;
    bool bActualConstraintAndBodyVerified = false;
    bool bParcelMassCountedExactlyOnce = false;
};

class IDDSRobotN12PhysicalActuatorPort
{
public:
    virtual ~IDDSRobotN12PhysicalActuatorPort() = default;
    // N12 implementation must produce physical torque/constraint state and
    // acknowledged step boundary. This baseline intentionally owns closed lids.
    virtual bool SubmitLidActuation(const FDDSRobotLidActuationRequest& Request) = 0;
    virtual bool SubmitParcelLatch(const FDDSRobotParcelLatchRequest& Request) = 0;
    virtual bool ReadParcelLatchAck(FDDSRobotParcelLatchAck& Out) const = 0;
};

inline bool DDSCurrentPhysicalWitnessMatches(const FDDSN08PhysicalSafetyWitness& W,
    const FDDSN08Token& Token, const UDDSRobotPhysicsRigComponent& Rig, double NowRealS, double MaxAgeS)
{
    FDDSRobotSnapshot S;
    if (!FMath::IsFinite(MaxAgeS) || MaxAgeS <= 0 || !Rig.IsServiceEvidenceFresh(NowRealS,S)) return false;
    return W.Operation.Equals(Token) && S.bCompleteState && S.bServiceStable && S.bWorldReady
        && W.PhysicsGeneration == S.Generation && W.CollisionWorldGeneration == S.CollisionWorldGeneration
        && W.ReadinessRevision == S.ReadinessRevision && W.CapturedRealS == S.CapturedRealS
        && W.CompletedStepSequence == S.StepSequence && W.CompletedStateTimeS == S.CompletedStateTimeS
        && NowRealS >= S.CapturedRealS && NowRealS - S.CapturedRealS <= MaxAgeS
        && W.bFreshPhysicalStableSupport && W.bCurrentLidSweepAndServicePathClear
        && W.bBothActualLidsClosedLatched && W.bExpectedParcelPhysicalOwnerAndMass;
}
