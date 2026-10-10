#pragma once
#include "CoreMinimal.h"

// All values supplied by an actual frozen native calibration; no geometry defaults.
struct FDDSN12Calibration
{
    bool bFrozen = false;
    FString RecordSha256;
    double OpenAngleRad[2] = {}; // Signed measured opening about each original hinge X.
    double LidKpNmPerRad = 0, LidKdNmPerRadps = 0, LidPeakNm = 0;
    double OpenAngleToleranceRad = 0, OpenOmegaToleranceRadps = 0;
    double ParcelKpNPerM = 0, ParcelKdNPerMps = 0, ParcelForceLimitN = 0;
    double ParcelKpNmPerRad = 0, ParcelKdNmPerRadps = 0, ParcelTorqueLimitNm = 0;
    double MountPositionToleranceM = 0, MountAngleToleranceRad = 0;
    double MountSpeedToleranceMps = 0, MountOmegaToleranceRadps = 0;
    double MaxCommandAgeS = 0, PhaseTimeoutS = 0;
    FTransform MountInChassisMeters = FTransform::Identity;
    FTransform MountInParcelMeters = FTransform::Identity;
    bool bMountCollisionExclusionMeasured = false;
};

enum class EDDSN12ParcelAction : uint8 { None, Transfer, Attach, Detach };
struct FDDSN12StepCommand
{
    uint64 PhysicsGeneration = 0, WorldEpoch = 0, ReadinessRevision = 0;
    uint64 OperationEpoch = 0, Sequence = 0;
    double IssuedRealS = 0;
    bool bOpen = false;
    bool bExternalWorkDeclared = false;
    EDDSN12ParcelAction ParcelAction = EDDSN12ParcelAction::None;
    FTransform ParcelTargetWorldCm = FTransform::Identity;
    FVector ParcelTargetVelocityCmps = FVector::ZeroVector;
    FVector ParcelTargetOmegaRadps = FVector::ZeroVector;
};
struct FDDSN12LidState
{
    double AngleRad = 0, OmegaRadps = 0, TorqueNm = 0;
    bool bAtOpenPose = false, bAtClosedPose = false, bLatchConstraintObserved = false;
};
struct FDDSN12StepState
{
    uint64 OperationEpoch = 0, CommandSequence = 0, BoundarySequence = 0;
    uint64 BoundaryAppliedStep = 0;
    bool bKnown = false, bDriveInhibited = false;
    bool bParcelLive = false, bParcelMounted = false, bParcelConstraintObserved = false;
    bool bBoundaryCompleted = false, bParcelAtTarget = false;
    FDDSN12LidState Lids[2];
    int32 ParcelParticleId = INDEX_NONE;
    double ParcelMassKg = 0, ExternalPowerW = 0;
    FTransform ParcelWorldCm = FTransform::Identity;
    FVector ParcelVelocityCmps, ParcelOmegaRadps;
    FVector MountVelocityCmps, MountOmegaRadps;
    FVector BoundaryParcelVelocityCmps, BoundaryParcelOmegaRadps;
    FVector BoundaryMountVelocityCmps, BoundaryMountOmegaRadps;
    FVector ExternalForceN, ExternalTorqueNm;
};
inline bool DDSValidateN12Calibration(const FDDSN12Calibration& C, FString& Error)
{
    if (!C.bFrozen || C.RecordSha256.Len()!=64) { Error=TEXT("Native N12 calibration absent"); return false; }
    for (TCHAR H : C.RecordSha256) if (!FChar::IsHexDigit(H)) return false;
    const double V[]={C.LidKpNmPerRad,C.LidKdNmPerRadps,C.LidPeakNm,C.OpenAngleToleranceRad,
        C.OpenOmegaToleranceRadps,C.ParcelKpNPerM,C.ParcelKdNPerMps,C.ParcelForceLimitN,
        C.ParcelKpNmPerRad,C.ParcelKdNmPerRadps,C.ParcelTorqueLimitNm,C.MountPositionToleranceM,
        C.MountAngleToleranceRad,C.MountSpeedToleranceMps,C.MountOmegaToleranceRadps,C.MaxCommandAgeS,C.PhaseTimeoutS};
    for (double X:V) if (!FMath::IsFinite(X) || X<=0) { Error=TEXT("Incomplete frozen N12 limits"); return false; }
    for (double A:C.OpenAngleRad) if (!FMath::IsFinite(A) || FMath::Abs(A)<=C.OpenAngleToleranceRad || FMath::Abs(A)>=PI) return false;
    for (const auto& T:{C.MountInChassisMeters,C.MountInParcelMeters})
        if (T.ContainsNaN() || !T.GetRotation().IsNormalized() || !T.GetScale3D().Equals(FVector::OneVector,1.e-9)) return false;
    return true;
}
