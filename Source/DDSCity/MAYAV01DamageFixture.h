#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryTypes.h"
#include "MAYAV01DamageFixture.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UPrimitiveComponent;
class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;

// Named, explicitly opted-in physical test object. No production world or damage setters.
UCLASS()
class DDSCITY_API AMAYAV01DamageFixture : public AActor
{
    GENERATED_BODY()
public:
    AMAYAV01DamageFixture();
    enum class EOutcome : uint8 { Running, Complete, Failed };
    bool Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector);
    bool BeginTrial(bool bLoaded, bool bHard, const FMAYADeliveryRobotObservation& Observation);
    EOutcome AdvanceTrial(const FMAYADeliveryRobotObservation& Observation);
    bool WasLastCommandAccepted() const { return bCommandAccepted; }
    int32 GetCompletedTrialCount() const { return TrialReceipts.Num(); }
    const FString& GetFailureReason() const { return Failure; }
    FString ReadEvidenceJson(bool bIncludeHistory = false) const;
    void Cleanup();
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    enum class EStep : uint8 { Idle, Approach, Impact, ImpactSettle, Hold, Backoff, Complete, Failed };
    UPROPERTY() TObjectPtr<UBoxComponent> Obstacle;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> VisibleBox;
    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<AMAYADeliveryDirector> Director;
    TWeakObjectPtr<UPrimitiveComponent> Carrier;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> ObservedBodies;
    struct FPassiveBodyPeak
    {
        uint64 Callbacks = 0;
        double ImpulseKgMps = 0, ApproachCmps = 0, WorldSeconds = -1;
        FVector RawImpulse = FVector::ZeroVector, Normal = FVector::ZeroVector;
        FString Step;
    };
    TMap<TWeakObjectPtr<UPrimitiveComponent>, FPassiveBodyPeak> PassiveBodyPeaks;
    TArray<TSharedPtr<class FJsonValue>> RawBodyPeaksToJson() const;
    FVector Axis = FVector::ForwardVector, Lateral = FVector::RightVector;
    FVector RouteOrigin = FVector::ZeroVector, Anchor = FVector::ZeroVector, FixtureCenter = FVector::ZeroVector;
    FMAYADeliveryRobotObservation PreviousObservation;
    EStep Step = EStep::Idle;
    bool bConfigured = false, bCommandAccepted = false, bLoadedTrial = false, bHardTrial = false;
    bool bArrivalLatched = false, bQualifiedContact = false, bCalibrationOnly = false;
    double TrialStartWorldS = -1, StepStartWorldS = -1, StableSinceWorldS = -1;
    double ConditionBefore = 100, ConditionAtHold = 100, PeakSpeed = 0, PeakImpulseKgMps = 0, PeakApproachCmps = 0;
    double TrialStartChassisZ = 0;
    double ContactWorldS = -1, LastContactWorldS = -1, HitQuietBeforeS = 0;
    double LastRecordedContactWorldS = -1;
    double HoldFirstPositiveContactWorldS = -1, HoldLastPositiveContactWorldS = -1, HoldMaxPositiveContactGapS = 0;
    int32 HoldPositiveContactCallbacks = 0;
    uint64 PassiveHitGeneration = 0, HitEpisodeGeneration = 0, ResetGeneration = 0;
    int32 HistoryAtStart = 0, HistoryAtHold = 0, FixtureHitCount = 0;
    FString JobId, Failure, FixtureSetupJson, StaticSurfaceSurveyJson;
    TArray<TSharedPtr<class FJsonValue>> TrialReceipts, ContactReceipts;
    bool Command(float Throttle, float Steer, bool bBrake);
    bool GroundSample(const FVector& XY, FHitResult& Hit) const;
    bool FixtureIdentityValid() const;
    void EnterStep(EStep Next, double WorldS);
    EOutcome FailTrial(const FString& Reason);
    bool ApproachAnchor(const FMAYADeliveryRobotObservation& Observation, bool bReverse);
    UFUNCTION() void OnChassisHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
};
