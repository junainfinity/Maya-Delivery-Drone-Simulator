#include "MAYADeliveryChargeEvidence.h"
#include "Dom/JsonObject.h"

namespace
{
    TSharedPtr<FJsonValue> MeasuredNumber(double Value)
    {
        if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
        // Preserve a nonfinite observation as an explicit raw string, rather
        // than inventing zero or writing invalid JSON numeric tokens.
        return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
    }
    TArray<TSharedPtr<FJsonValue>> MeasuredVector(const FVector& Value)
    { return { MeasuredNumber(Value.X), MeasuredNumber(Value.Y), MeasuredNumber(Value.Z) }; }
}

TSharedRef<FJsonObject> MAYADeliveryChargeDisconnectToJson(const FMAYADeliveryChargeDisconnect& Receipt)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("schema"), TEXT("MayaRobotics.V01.ChargeDisconnect.v1"));
    O->SetStringField(TEXT("scope"), TEXT("Recorded automatic charger eligibility loss; no native charge acceptance decision"));
    O->SetStringField(TEXT("nonfiniteMeasurementPolicy"), TEXT("Finite numeric values stay numbers; nonfinite values are raw strings"));
    O->SetBoolField(TEXT("observed"), Receipt.Generation != 0);
    O->SetNumberField(TEXT("generation"), static_cast<double>(Receipt.Generation));
    // Default receipt fields describe no real event and must not be mistaken
    // for a world sample. Emit no measured fields before the first event.
    if (Receipt.Generation == 0) return O;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (const FString& Condition : Receipt.FailedConditions) Failed.Add(MakeShared<FJsonValueString>(Condition));
    O->SetArrayField(TEXT("failedConditions"), Failed);
    O->SetNumberField(TEXT("missionPhase"), static_cast<int32>(Receipt.Phase));
    O->SetBoolField(TEXT("atCharger"), Receipt.bAtCharger);
    O->SetField(TEXT("batteryPercentBeforeTick"), MeasuredNumber(Receipt.BatteryPercentBeforeTick));
    const auto& R = Receipt.Robot;
    auto Robot = MakeShared<FJsonObject>();
    Robot->SetBoolField(TEXT("known"), R.bKnown); Robot->SetBoolField(TEXT("ready"), R.bReady);
    Robot->SetBoolField(TEXT("sixWheelSupported"), R.bGrounded); Robot->SetBoolField(TEXT("upright"), R.bUpright);
    Robot->SetBoolField(TEXT("stoppedAndSupported"), R.IsStoppedAndSupported());
    Robot->SetBoolField(TEXT("doorsOpen"), R.bDoorsOpen); Robot->SetBoolField(TEXT("doorsClosedLatched"), R.bDoorsClosedLatched);
    Robot->SetNumberField(TEXT("supportMask"), R.SupportMask);
    Robot->SetNumberField(TEXT("resetGeneration"), static_cast<double>(R.ResetGeneration));
    Robot->SetField(TEXT("observationWorldSeconds"), MeasuredNumber(R.ObservationWorldSeconds));
    Robot->SetField(TEXT("opticalForwardCmps"), MeasuredNumber(R.OpticalForwardCmps));
    Robot->SetStringField(TEXT("fault"), R.Fault);
    Robot->SetBoolField(TEXT("chassisPoseFinite"), !R.ChassisPose.ContainsNaN());
    auto Pose = MakeShared<FJsonObject>();
    Pose->SetArrayField(TEXT("positionCm"), MeasuredVector(R.ChassisPose.GetLocation()));
    const FQuat Q = R.ChassisPose.GetRotation();
    Pose->SetArrayField(TEXT("quaternionXYZW"), { MeasuredNumber(Q.X), MeasuredNumber(Q.Y), MeasuredNumber(Q.Z), MeasuredNumber(Q.W) });
    Pose->SetArrayField(TEXT("scale"), MeasuredVector(R.ChassisPose.GetScale3D()));
    Robot->SetObjectField(TEXT("chassisPose"), Pose);
    Robot->SetArrayField(TEXT("velocityCmps"), MeasuredVector(R.VelocityCmps));
    Robot->SetArrayField(TEXT("angularVelocityRadps"), MeasuredVector(R.AngularVelocityRadps));
    Robot->SetArrayField(TEXT("doorAnglesDegrees"), { MeasuredNumber(R.DoorAngleDegrees[0]), MeasuredNumber(R.DoorAngleDegrees[1]) });
    Robot->SetArrayField(TEXT("doorOmegaRadps"), { MeasuredNumber(R.DoorOmegaRadps[0]), MeasuredNumber(R.DoorOmegaRadps[1]) });
    O->SetObjectField(TEXT("robotObservationAtDisconnect"), Robot);
    return O;
}
