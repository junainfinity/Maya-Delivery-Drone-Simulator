#include "MAYAStableCameraArm.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

UMAYAStableCameraArm::UMAYAStableCameraArm()
{
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    SetAbsolute(true, true, false);
    bUsePawnControlRotation = false;
    bInheritPitch = bInheritYaw = bInheritRoll = false;
    // The pivot performs one frame-rate-independent filter; avoid double lag.
    bEnableCameraLag = bEnableCameraRotationLag = false;
    bDoCollisionTest = true;
    TargetArmLength = 250;
    ProbeSize = 12;
}

void UMAYAStableCameraArm::SetViewAngles(float Pitch, float YawOffset)
{
    if (FMath::IsFinite(Pitch) && FMath::IsFinite(YawOffset))
    {
        ViewPitch = FMath::Clamp(Pitch, -70.f, -5.f);
        ViewYawOffset = FMath::UnwindDegrees(YawOffset);
    }
}

void UMAYAStableCameraArm::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    const auto* Target = GetOwner() ? GetOwner()->GetRootComponent() : nullptr;
    if (Target)
    {
        const FVector Position = Target->GetComponentLocation();
        const float Yaw = Target->GetComponentRotation().Yaw;
        if (!Position.ContainsNaN() && FMath::IsFinite(Yaw))
        {
            const float Dt = FMath::Max(0.f, DeltaTime);
            if (!bFollowInitialized || FVector::DistSquared(Position, LastTarget) > FMath::Square(200.0))
            {
                // Intentional recovery cuts reset the filter instead of flying
                // the camera across the street. This never moves the robot.
                FollowPosition = Position;
                FollowYaw = Yaw;
                CollisionArmLength = TargetArmLength;
                bFollowInitialized = true;
            }
            else
            {
                const FVector Delta = Position - FollowPosition;
                const double XYDistance = Delta.Size2D();
                if (XYDistance > 1.5)
                {
                    const double A = 1.0 - FMath::Exp(-6.0 * Dt);
                    const FVector XY(Delta.X, Delta.Y, 0);
                    FollowPosition += XY * ((XYDistance - 1.5) / XYDistance) * A;
                }
                const double ZOutsideDeadzone = FMath::Max(0.0, FMath::Abs(Delta.Z) - 3.0);
                FollowPosition.Z += FMath::Sign(Delta.Z) * ZOutsideDeadzone * (1.0 - FMath::Exp(-3.0 * Dt));
                const float YawDelta = FMath::FindDeltaAngleDegrees(FollowYaw, Yaw);
                const float YawOutsideDeadzone = FMath::Max(0.f, FMath::Abs(YawDelta) - 0.75f);
                FollowYaw = FMath::UnwindDegrees(FollowYaw + FMath::Sign(YawDelta)
                    * YawOutsideDeadzone * (1.f - FMath::Exp(-4.f * Dt)));
            }
            LastTarget = Position;
            SetWorldLocationAndRotation(FollowPosition + FVector(0, 0, 60),
                FRotator(ViewPitch, FollowYaw + 180.f + ViewYawOffset, 0));
            SampleWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
            ++FollowSamples;
        }
    }
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

FVector UMAYAStableCameraArm::BlendLocations(const FVector& DesiredArmLocation,
    const FVector& TraceHitLocation, bool bHitSomething, float DeltaTime)
{
    const FVector Origin = GetComponentLocation() + TargetOffset;
    const FVector Direction = DesiredArmLocation - Origin;
    const float DesiredLength = Direction.Size();
    const float SafeLength = bHitSomething ? FMath::Min(DesiredLength,
        float(FVector::Distance(Origin, TraceHitLocation))) : DesiredLength;
    if (SafeLength < CollisionArmLength)
        CollisionArmLength = SafeLength; // never smooth through a blocking wall
    else
        CollisionArmLength += (SafeLength - CollisionArmLength)
            * (1.f - FMath::Exp(-4.f * FMath::Max(0.f, DeltaTime)));
    return Origin + Direction.GetSafeNormal() * CollisionArmLength;
}

FString UMAYAStableCameraArm::ReadEvidenceJson() const
{
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.StablePlayerCamera.v1"));
    J->SetNumberField(TEXT("worldSeconds"), SampleWorldSeconds);
    J->SetNumberField(TEXT("samples"), double(FollowSamples));
    J->SetStringField(TEXT("tickGroup"), TEXT("PostPhysics"));
    J->SetBoolField(TEXT("absoluteLocation"), IsUsingAbsoluteLocation());
    J->SetBoolField(TEXT("absoluteRotation"), IsUsingAbsoluteRotation());
    const FVector P = GetComponentLocation();
    const FRotator R = GetComponentRotation();
    J->SetNumberField(TEXT("pivotXCm"), P.X); J->SetNumberField(TEXT("pivotYCm"), P.Y); J->SetNumberField(TEXT("pivotZCm"), P.Z);
    J->SetNumberField(TEXT("pitchDegrees"), R.Pitch); J->SetNumberField(TEXT("yawDegrees"), R.Yaw); J->SetNumberField(TEXT("rollDegrees"), R.Roll);
    J->SetNumberField(TEXT("rawTargetZCm"), LastTarget.Z);
    J->SetNumberField(TEXT("collisionArmLengthCm"), CollisionArmLength);
    J->SetBoolField(TEXT("collisionTestEnabled"), bDoCollisionTest);
    FString Text; FJsonSerializer::Serialize(J, TJsonWriterFactory<>::Create(&Text)); return Text;
}
