#pragma once

// Player left/right describes the travel path. Native SubmitDriveCommand still
// describes chassis yaw, so route controllers retain their existing contract.
namespace MAYAPlayerDriveInput
{
    inline float BodyYawSteer(float RequestedThrottle, float DirectionalSteer)
    {
        return RequestedThrottle < 0.f ? -DirectionalSteer : DirectionalSteer;
    }
}
