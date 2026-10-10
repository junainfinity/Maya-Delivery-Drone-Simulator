#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryTypes.h"
#include "MAYAV01CurbTrial.generated.h"
class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class UPrimitiveComponent;
UCLASS()
class DDSCITY_API AMAYAV01CurbTrial : public AActor
{
    GENERATED_BODY()
public:
    AMAYAV01CurbTrial();
    enum class EOutcome : uint8 { Running, Complete, Failed };
    bool Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector, const FMAYADeliveryRobotObservation& O);
    EOutcome Advance(const FMAYADeliveryRobotObservation& O);
    bool LastCommandAccepted() const { return bAccepted; }
    const FString& FailureReason() const { return Failure; }
    FString ReadCurrentStageName() const;
    FString ReadEvidenceJson(bool bFull = false) const;
    void Cleanup();
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    enum class EStep : uint8 { Approach, Align, Settle, Cross, Hold, Complete, Failed, NormalAscent, BrakeBeforeBoost, BoostAscent, UpperHold };
    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<AMAYADeliveryDirector> Director;
    FVector CourseOrigin = FVector::ZeroVector, Axis = FVector::ForwardVector, Outward = FVector::RightVector;
    double UpperZ = 0, RoadZ = 0, StartWorldS = -1, StepWorldS = -1, StableWorldS = -1, LastSampleWorldS = -1;
    double LastContactSampleWorldS = -1;
    int32 AlignmentRecenterStep = 0, AlignmentRecenterCount = 0;
    bool bSettleHeadingTrim = false;
    double AlignmentRecenterStartWorldS = -1, AlignmentRecenterHeading = 0, AlignmentRecenterSign = 1, AlignmentRecenterRemainingCm = 0;
    FVector AlignmentRecenterAxis = FVector::ForwardVector, AlignmentRecenterStartCm = FVector::ZeroVector, AlignmentRecenterGoalCm = FVector::ZeroVector;
    double ConditionBefore = 100, ConditionAtHold = 100, PeakDownCmps = 0, PeakDropCm = 0, CrossStartZ = 0;
    uint64 ResetGeneration = 0, LandingGenerationAtStart = 0, LastSavedLandingGeneration = 0;
    int32 HistoryBefore = 0, HistoryAtHold = 0;
    TArray<FMAYADeliveryImpactReceipt> DescentHistoryPrefix;
    bool bDescentAssessmentPerformed = false, bDescentConditionAccountingComplete = false;
    bool bBoostDescentRoadEndpointAccepted = false, bLegacyLandingCriterionPassed = false;
    TSharedPtr<class FJsonObject> DescentAccountingEvidence;
    TArray<TSharedPtr<class FJsonValue>> CrossGuidanceObservations;
    int32 CrossGuidanceObservationsSeen = 0;
    bool bCrossGuidanceTraceTruncated = false;
    bool bConfigured = false, bAccepted = false, bArrival = false, bLostSupport = false, bRegainedSupport = false;
    FString Failure, JobId, SurveyJson, LastContainmentEvidenceJson;
    EStep Step = EStep::Approach;
    TSet<FString> RoadSupports, UpperSupports, CurbSupports;
    bool bBoostPower = false, bNormalStallQualified = false, bBoostBatteryLossObserved = false;
    double StallStartWorldS = -1, StallAnchorAlongCm = 0, NormalStartAlongCm = 0, BoostStartBattery = 0;
    FVector NormalStallPoseCm = FVector::ZeroVector;
    bool bNormalBlockedQualified = false;
    bool bUpperSupportCoastLatched = false;
    bool bAscentSpeedCoast = false, bMovingBoostEnergyQualified = false, bPreviousMovingBoostEligible = false;
    int32 AscentSpeedCoastTransitionCount = 0, MovingBoostEnergyQualifiedIntervalCount = 0;
    uint64 LastMovingBoostEnergyFrame = MAX_uint64;
    double LastMovingBoostEnergyWorldS = -1, LastMovingBoostEnergyElapsedS = -1, LastMovingBoostEnergyBattery = 0;
    TSharedPtr<class FJsonObject> LastMovingBoostEnergyEndpoint;
    double UpperSupportCoastWorldS = -1;
    FVector UpperSupportCoastPoseCm = FVector::ZeroVector;
    TSharedPtr<class FJsonObject> UpperSupportCoastReceipt;
    double BlockedStartWorldS = -1, BlockedLastWorldS = -1;
    int32 BlockedSampleCount = 0, BlockedResetCount = 0, NormalBlockedQualifiedSampleCount = 0;
    FVector BlockedMinCm = FVector::ZeroVector, BlockedMaxCm = FVector::ZeroVector;
    FVector NormalBlockedPoseCm = FVector::ZeroVector, NormalBlockedMinCm = FVector::ZeroVector, NormalBlockedMaxCm = FVector::ZeroVector;
    double NormalBlockedDurationS = 0, NormalBlockedDiagonalCm = 0;
    FString OriginalParcelId, OriginalParcelActor, OriginalParcelBody;
    double LastRawCurbHitWorldS = -1;
    int32 RawCurbHitCount = 0;
    TSharedPtr<class FJsonObject> LastRawCurbHit;
    TArray<TSharedPtr<class FJsonValue>> RawCurbHits, BoostPowerObservations;
    double LastBoostEnergyWorldS = -1, LastBoostEnergyBattery = 0;
    bool bPreviousBoostStallEligible = false;
    int32 BoostPowerObservationCount = 0;
    FString LastBoostEnergyNative;
    TWeakObjectPtr<UPrimitiveComponent> CompoundContactRoot;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> CompoundContactObservers;
    TSharedPtr<class FJsonObject> CompoundContactBinding;
    TArray<TSharedPtr<class FJsonValue>> CompoundContactFirst, CompoundContactRecent;
    TMap<FString,int32> CompoundContactPhaseCounts;
    int32 CompoundContactCallbacksSeen = 0, CompoundContactFiltered = 0, CompoundContactDropped = 0;
    int32 CompoundContactRecorded = 0, CompoundContactMalformed = 0, CompoundContactOverwritten = 0, CompoundContactRecentWrite = 0;
    bool bCompoundContactCleanupPerformed = false;
    void BindCompoundContactObservers();
    void UnbindCompoundContactObservers();
    UFUNCTION() void ObserveCompoundContact(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
    UFUNCTION() void ObserveRawCurbHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
    EOutcome AdvanceBoostPower(const FMAYADeliveryRobotObservation& O);
    bool NativeUpperWheelFootprint() const;
    bool NativeCurbContactAndMotor() const;
    TArray<TSharedPtr<class FJsonValue>> Samples, Landings;
    bool Command(float Throttle, float Steer, bool bBrake, bool bBoost = false);
    void Enter(EStep Next, double WorldS);
    EOutcome Fail(const FString& Reason);
    bool SixActualRoadContacts() const;
    void Save(const FMAYADeliveryRobotObservation& O, bool bForce = false);
};
