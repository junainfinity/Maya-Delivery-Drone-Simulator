#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryTypes.h"
#include "MassEntityQuery.h"
#include "MassEntityTypes.h"
#include "MAYAV01TrafficEncounterTrial.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class AMAYADeliveryParcel;
class UPrimitiveComponent;
struct FMassEntityManager;
class FJsonObject;

// Explicit Development diagnostic. It observes original traffic and issues only
// the same leased native drive commands used by existing native road tests.
UCLASS()
class DDSCITY_API AMAYAV01TrafficEncounterTrial : public AActor
{
    GENERATED_BODY()
public:
    AMAYAV01TrafficEncounterTrial();
    enum class EOutcome : uint8 { Running, Complete, Failed };
    bool Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector,
        const FMAYADeliveryRobotObservation& Observation, const FString& InOutputDirectory);
    EOutcome Advance(const FMAYADeliveryRobotObservation& Observation);
    bool IsCrashFixture() const { return !CrashFixtureMode.IsEmpty(); }
    bool CrashFixturePassed() const { return bCrashFixturePassed; }
    bool LastCommandAccepted() const { return bAccepted; }
    const FString& FailureReason() const { return Failure; }
    FString ReadEvidenceJson(bool bFull = false) const;
    void Cleanup();
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    FString CrashFixtureMode;
    bool bCrashImpulseApplied = false, bCrashFixturePassed = false;
    double CrashImpulseWorldS = -1, CrashStoppedSinceWorldS = -1;
    TSharedPtr<FJsonObject> CrashActuation, CrashOutcome, CrashLaneEvidence;
    enum class ECrashLaneStep : uint8 { FaceFirst, LaneJoin, LaneApproach, Ready };
    ECrashLaneStep CrashLaneStep = ECrashLaneStep::FaceFirst;
    double CrashApproachTravelAxisSign = 1;
    TWeakObjectPtr<UPrimitiveComponent> CrashSelectedBody;
    uint64 CrashSelectedNativeActor = 0;
    double CrashLaneStartedWorldS = -1;
    FVector CrashTurnAnchorCm = FVector::ZeroVector;
    EOutcome AdvanceCrashFixture(const FMAYADeliveryRobotObservation& Observation);
    enum class EStep : uint8 { StageRoad, FindDirection, Align, FindOncoming, Intercept, PostContact, RecoveryWait, Complete, Failed };
    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<AMAYADeliveryDirector> Director;
    TWeakObjectPtr<AMAYADeliveryParcel> OriginalParcel;
    TWeakObjectPtr<UPrimitiveComponent> OriginalParcelBody;
    TWeakObjectPtr<AActor> TargetActor;
    FMassEntityQuery VehicleQuery, SensorQuery;
    FMassEntityHandle TargetEntity, RobotSensor, DirectionEntity;
    double DirectionObservedSpeedCmps = 0;
    bool bWaitingForPhysicalRepresentation = false;
    uint64 TargetBindingEpoch = 0;
    double LastPhysicalBindingReadWorldS = -1;
    struct FDeferredNativeHit
    {
        FMassEntityHandle Entity;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Component;
        TSharedPtr<FJsonObject> Receipt;
        uint64 BindingEpoch = 0;
        int32 Representation = -1;
        double CallbackWorldS = -1, BeforeReadWorldS = -1;
    };
    TArray<FDeferredNativeHit> DeferredNativeHits;
    TArray<TSharedPtr<class FJsonValue>> DeferredNativeHitReceipts;
    FVector EntryCm = FVector::ZeroVector, IntendedHeading = FVector::ForwardVector;
    FVector RoadStagingHeading = FVector::ForwardVector, AlignmentAnchorCm = FVector::ZeroVector;
    bool bRoadStagingDistanceReached = false, bRoadStagingComplete = false;
    FVector TargetPositionCm = FVector::ZeroVector, TargetForward = FVector::ForwardVector;
    double StartWorldS = -1, StepWorldS = -1, StableWorldS = -1, LastSampleWorldS = -1;
    double FirstContactWorldS = -1, RecoveryRequestWorldS = -1;
    double TargetReadWaitStartedWorldS = -1;
    int32 TargetRepresentationAtLastRead = -1;
    bool bTrafficReadRetryable = false, bObservedTargetActorGap = false;
    FString LastTrafficReadRefusal, OriginalSelectedActorPath;
    TArray<TSharedPtr<class FJsonValue>> TrafficReadEvents, TrafficActorLineage;
    double TargetSpeedAtSelection = 0, MinimumTargetSpeed = MAX_dbl;
    double OriginalMassKg = 0, ConditionBefore = 0;
    int32 CashBefore = 0, HistoryBefore = 0;
    uint64 ResetBefore = 0;
    FString JobId, ShiftId, ParcelId, Failure, LastTargetJson, LastRoadJson, LastCameraJson, BeforeRecoveryJson, AfterRecoveryJson, OutputDirectory, FirstSelectedContactJson;
    TArray<TSharedPtr<class FJsonValue>> Samples, Contacts;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> BoundBodies;
    bool bConfigured = false, bAccepted = false, bContact = false, bPostStable = false;
    bool bFinalTargetSelected = false, bPostCaptureRequested = false;
    bool bSensorRecognized = false, bBrakingObserved = false, bRecoveryRequested = false, bRecoveryAccepted = false, bRecoveryCompleted = false;
    FDelegateHandle ScreenshotHandle;
    FString PendingScreenshotPath;
    TArray<TSharedPtr<class FJsonValue>> Screenshots;
    double ScreenshotRequestWorldS = -1;
    uint64 ScreenshotRequestFrame = 0;
    void Capture(const TCHAR* Label);
    void ScreenshotProcessed();
    uint64 ContactCount = 0;
    EStep Step = EStep::StageRoad;
    bool Command(float Throttle, float Steer, bool bBrake);
    bool ReadTraffic(const FMAYADeliveryRobotObservation& Observation, bool bSelect, bool bRequireOncoming);
    bool ReadRoad(const FMAYADeliveryRobotObservation& Observation, bool bLookAhead, double OpticalTravelSign = 1);
    void ReconcileDeferredNativeHits(const FMassEntityManager& Manager);
    void QualifyNativeContact(const TSharedRef<FJsonObject>& Receipt, double ActualHitWorldS);
    FString ReadCamera(const FMAYADeliveryRobotObservation& Observation) const;
    void Save(const FMAYADeliveryRobotObservation& Observation, bool bForce = false);
    void Enter(EStep Next, double WorldS);
    EOutcome Fail(const FString& Reason);
    UFUNCTION() void OnNativeHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
};
