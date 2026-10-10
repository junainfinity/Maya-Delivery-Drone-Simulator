#pragma once
#include "DDSRobotPhysicsTypes.h"
class AActor;
// A02 supplies exact native component names, asset paths and measured physical SI frames.
// This consumer creates no render geometry, guessed rig, replacement asset or calibration.
struct FDDSRobotOriginalComponentMap
{
    FString IntegrityLedgerSha256;
    FName Bodies[9], Suspension[6], Parcel;
};
bool DDSConsumeOriginalRobotBinding(AActor& OriginalActor,const FDDSRobotOriginalComponentMap& Names,
    const FDDSRobotMeasuredBinding& Measured,const FDDSRobotPhysicsProfile& Profile,
    FDDSRobotMeasuredBinding& Out,FString& Error,
    EDDSOriginalAssemblyScope Scope=EDDSOriginalAssemblyScope::FullService);
