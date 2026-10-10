#pragma once

#include "CoreMinimal.h"

// Pure geometry shared by the N10 actor and its static counterexample test.
// The actor supplies these values from the owned N06 collision chassis; this
// helper has no authority, collision-world, or star-award side effect.
struct FDDSN10AccessSweep final
{
    FVector Start = FVector::ZeroVector;
    FVector End = FVector::ZeroVector;
    FVector HalfExtent = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;

    static bool TryBuild(const FVector& ChassisCenter,
                         const FVector& ChassisHalfExtent,
                         const FQuat& ChassisRotation,
                         const FVector& StarSupportedGround,
                         FDDSN10AccessSweep& Out)
    {
        Out = FDDSN10AccessSweep();
        if (ChassisCenter.ContainsNaN() || ChassisHalfExtent.ContainsNaN()
            || ChassisRotation.ContainsNaN() || StarSupportedGround.ContainsNaN()
            || ChassisHalfExtent.X <= 0 || ChassisHalfExtent.Y <= 0
            || ChassisHalfExtent.Z <= 0) return false;
        Out.Start = ChassisCenter;
        Out.End = FVector(StarSupportedGround.X, StarSupportedGround.Y,
                          ChassisCenter.Z);
        Out.HalfExtent = ChassisHalfExtent;
        Out.Rotation = ChassisRotation;
        return true;
    }
};
