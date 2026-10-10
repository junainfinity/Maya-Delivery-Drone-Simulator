#include "DDSN07SafeSpringArm.h"

#include "Engine/World.h"

FVector UDDSN07SafeSpringArm::BlendLocations(const FVector& DesiredArmLocation,
    const FVector& TraceHitLocation, bool bHitSomething, float DeltaTime)
{
    // This project uses no spring-arm target/socket offset. The engine's sweep
    // is pivot -> desired camera, so every shorter point on this same ray is
    // inside its clear segment. Never ease outward beyond a current hit.
    const FVector Pivot = GetComponentLocation();
    const FVector DesiredRay = DesiredArmLocation - Pivot;
    const float DesiredDistance = DesiredRay.Size();
    if (DesiredDistance <= KINDA_SMALL_NUMBER)
    {
        RenderedArmDistanceCm = 0.0f;
        return Pivot;
    }

    const float SafeDistance = bHitSomething
        ? FMath::Clamp((TraceHitLocation - Pivot).Size(), 0.0f, DesiredDistance)
        : DesiredDistance;
    if (RenderedArmDistanceCm < 0.0f)
    {
        RenderedArmDistanceCm = SafeDistance;
    }
    else if (SafeDistance < RenderedArmDistanceCm)
    {
        // Retraction is immediate for collision safety, including a new hit
        // encountered during a previous release.
        RenderedArmDistanceCm = SafeDistance;
    }
    else if (SafeDistance > RenderedArmDistanceCm)
    {
        RenderedArmDistanceCm = FMath::FInterpTo(RenderedArmDistanceCm, SafeDistance,
            FMath::Max(DeltaTime, 0.0f), 12.0f);
        if (SafeDistance - RenderedArmDistanceCm < 0.5f)
        {
            RenderedArmDistanceCm = SafeDistance;
        }
    }

    const bool bIsReturning = RenderedArmDistanceCm < SafeDistance - 0.5f;
    if (bHitSomething && !bWasBlocked)
    {
        UE_LOG(LogTemp, Display, TEXT("N07_ARM_HIT safeCm=%.3f renderedCm=%.3f desiredCm=%.3f"),
            SafeDistance, RenderedArmDistanceCm, DesiredDistance);
    }
    if (!bHitSomething && bWasBlocked)
    {
        UE_LOG(LogTemp, Display, TEXT("N07_ARM_RELEASE_START renderedCm=%.3f desiredCm=%.3f"),
            RenderedArmDistanceCm, DesiredDistance);
    }
    if (bIsReturning && GetWorld() && GetWorld()->GetRealTimeSeconds() - LastStepLogSeconds >= 0.05)
    {
        LastStepLogSeconds = GetWorld()->GetRealTimeSeconds();
        UE_LOG(LogTemp, Display, TEXT("N07_ARM_RELEASE_STEP blocked=%d renderedCm=%.3f safeCm=%.3f desiredCm=%.3f"),
            bHitSomething, RenderedArmDistanceCm, SafeDistance, DesiredDistance);
    }
    if (bReturning && !bIsReturning)
    {
        UE_LOG(LogTemp, Display, TEXT("N07_ARM_RELEASE_SETTLED renderedCm=%.3f desiredCm=%.3f"),
            RenderedArmDistanceCm, DesiredDistance);
    }
    bWasBlocked = bHitSomething;
    bReturning = bIsReturning;
    return Pivot + DesiredRay.GetSafeNormal() * RenderedArmDistanceCm;
}
