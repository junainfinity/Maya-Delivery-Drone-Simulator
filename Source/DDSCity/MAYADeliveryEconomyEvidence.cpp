#include "MAYADeliveryEconomyEvidence.h"
#include "Dom/JsonObject.h"

TSharedRef<FJsonObject> MAYADeliveryEconomyToJson(const FMAYADeliverySnapshot& S)
{
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.ShiftEconomy.v1"));
    J->SetStringField(TEXT("shiftInstanceId"), S.ShiftInstanceId);
    J->SetBoolField(TEXT("shiftStarted"), S.bShiftStarted); J->SetBoolField(TEXT("shiftWon"), S.bShiftWon);
    J->SetNumberField(TEXT("netCashDollars"), S.TotalCredits);
    J->SetNumberField(TEXT("rechargeFeesPaidDollars"), S.RechargeFeesPaidDollars);
    J->SetNumberField(TEXT("resultEarningsDollars"), S.Result.Credits);
    const auto& C = S.ChargeVisit;
    auto Visit = MakeShared<FJsonObject>();
    const TCHAR* Names[] = {TEXT("None"), TEXT("Qualifying"), TEXT("Charging"), TEXT("Full"), TEXT("Blocked")};
    Visit->SetStringField(TEXT("generation"), LexToString(C.Generation));
    Visit->SetStringField(TEXT("stationId"), C.StationId); Visit->SetStringField(TEXT("stage"), Names[static_cast<uint8>(C.Stage)]);
    Visit->SetStringField(TEXT("reason"), C.Reason); Visit->SetBoolField(TEXT("inside"), C.bInside);
    Visit->SetBoolField(TEXT("eligible"), C.bEligible); Visit->SetBoolField(TEXT("paid"), C.bPaid);
    Visit->SetBoolField(TEXT("consumed"), C.bConsumed); Visit->SetNumberField(TEXT("elapsedGameplaySeconds"), C.ElapsedSeconds);
    Visit->SetNumberField(TEXT("remainingGameplaySeconds"), C.RemainingSeconds); J->SetObjectField(TEXT("chargeVisit"), Visit);
    const auto& R = S.LastRecharge; auto Fee = MakeShared<FJsonObject>();
    Fee->SetBoolField(TEXT("observed"), R.Generation != 0);
    Fee->SetStringField(TEXT("generation"), LexToString(R.Generation));
    if (R.Generation != 0)
    {
        Fee->SetStringField(TEXT("visitGeneration"), LexToString(R.VisitGeneration));
        Fee->SetStringField(TEXT("shiftInstanceId"), R.ShiftInstanceId); Fee->SetStringField(TEXT("stationId"), R.StationId);
        Fee->SetNumberField(TEXT("worldSeconds"), R.WorldSeconds); Fee->SetNumberField(TEXT("batteryBefore"), R.BatteryBefore);
        Fee->SetNumberField(TEXT("batteryAfter"), R.BatteryAfter); Fee->SetNumberField(TEXT("feeDollars"), R.FeeDollars);
        Fee->SetNumberField(TEXT("cashBefore"), R.CashBefore); Fee->SetNumberField(TEXT("cashAfter"), R.CashAfter);
    }
    J->SetObjectField(TEXT("lastRecharge"), Fee);
    const auto& W = S.ShiftWin; auto Win = MakeShared<FJsonObject>();
    Win->SetBoolField(TEXT("frozen"), W.bFrozen);
    if (W.bFrozen)
    {
        Win->SetStringField(TEXT("shiftInstanceId"), W.ShiftInstanceId); Win->SetStringField(TEXT("finalJobInstanceId"), W.FinalJobInstanceId);
        Win->SetNumberField(TEXT("cashBefore"), W.CashBefore); Win->SetNumberField(TEXT("deliveryEarningsDollars"), W.DeliveryEarningsDollars);
        Win->SetNumberField(TEXT("finalCashDollars"), W.FinalCashDollars); Win->SetNumberField(TEXT("completedJobs"), W.CompletedJobs);
        Win->SetNumberField(TEXT("cargoConditionPercent"), W.CargoConditionPercent);
    }
    J->SetObjectField(TEXT("shiftWin"), Win); return J;
}
