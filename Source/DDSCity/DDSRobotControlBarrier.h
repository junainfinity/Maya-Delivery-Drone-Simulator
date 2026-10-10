#pragma once
#include "CoreMinimal.h"
#include "Misc/ScopeLock.h"

// One shared non-UObject execution barrier. No lock survives a flush/wait.
// Revocation and a nine-body reset linearize under the same short critical section.
struct FDDSRobotControlState
{
    uint64 Generation = 0, WorldEpoch = 1, Revision = 1, CancelledResetId = 0;
    uint64 MechanicsEpoch = 1;
    bool bMechanicsDriveLocked = false;
    bool bReady = false, bRetiring = false;
};
struct FDDSRobotControlBarrier
{
    FCriticalSection Mutex;
    FDDSRobotControlState Current;
    FDDSRobotControlState Read() { FScopeLock Lock(&Mutex); return Current; }
};
