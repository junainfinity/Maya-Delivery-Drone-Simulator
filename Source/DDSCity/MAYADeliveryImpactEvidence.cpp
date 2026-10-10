#include "MAYADeliveryImpactEvidence.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MayaAppliedImpactEvidence
{
void Number(const TSharedRef<FJsonObject>& O, const TCHAR* Name, double Value)
{
    if (FMath::IsFinite(Value)) O->SetNumberField(Name, Value);
    else O->SetStringField(Name, FMath::IsNaN(Value) ? TEXT("NaN") : Value > 0 ? TEXT("+Infinity") : TEXT("-Infinity"));
}
}

TSharedRef<FJsonObject> MAYADeliveryImpactEventToJson(const FMAYADeliveryImpactEvent& E)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("sourceGeneration"), LexToString(E.Generation));
    O->SetBoolField(TEXT("sourceEventObserved"), E.Generation != 0);
    if (E.Generation == 0) return O;
    O->SetStringField(TEXT("kind"), E.Kind);
    MayaAppliedImpactEvidence::Number(O, TEXT("worldSeconds"), E.WorldSeconds);
    MayaAppliedImpactEvidence::Number(O, TEXT("impulseKgMps"), E.ImpulseKgMps);
    MayaAppliedImpactEvidence::Number(O, TEXT("preContactDownCmps"), E.PreContactDownCmps);
    MayaAppliedImpactEvidence::Number(O, TEXT("dropCm"), E.DropCm);
    MayaAppliedImpactEvidence::Number(O, TEXT("approachSpeedCmps"), E.ApproachSpeedCmps);
    MayaAppliedImpactEvidence::Number(O, TEXT("quietSecondsBeforeEvent"), E.QuietSecondsBeforeEvent);
    O->SetStringField(TEXT("otherActor"), E.OtherActor); O->SetStringField(TEXT("otherComponent"), E.OtherComponent);
    O->SetStringField(TEXT("robotComponent"), E.RobotComponent);
    return O;
}

TSharedRef<FJsonObject> MAYADeliveryImpactReceiptToJson(const FMAYADeliveryImpactReceipt& R)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("episodeGeneration"), LexToString(R.EpisodeGeneration));
    O->SetBoolField(TEXT("appliedReceiptObserved"), R.EpisodeGeneration != 0 && R.Event.Generation != 0);
    if (R.EpisodeGeneration == 0 || R.Event.Generation == 0) return O;
    O->SetObjectField(TEXT("sourceEvent"), MAYADeliveryImpactEventToJson(R.Event));
    MayaAppliedImpactEvidence::Number(O, TEXT("appliedConditionPoints"), R.AppliedConditionPoints);
    MayaAppliedImpactEvidence::Number(O, TEXT("episodeConditionPoints"), R.EpisodeConditionPoints);
    MayaAppliedImpactEvidence::Number(O, TEXT("conditionBefore"), R.ConditionBefore);
    MayaAppliedImpactEvidence::Number(O, TEXT("conditionAfter"), R.ConditionAfter);
    return O;
}

TArray<TSharedPtr<FJsonValue>> MAYADeliveryImpactHistoryToJson(const TArray<FMAYADeliveryImpactReceipt>& History)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (int32 I = FMath::Max(0, History.Num() - 64); I < History.Num(); ++I)
        Values.Add(MakeShared<FJsonValueObject>(MAYADeliveryImpactReceiptToJson(History[I])));
    return Values;
}
