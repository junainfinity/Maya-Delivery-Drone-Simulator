#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYACityBlockRoute.h"
#include "MAYADeliveryTypes.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYACityBlockMissionTrial.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class AMAYADeliveryParcel;
class UPrimitiveComponent;
class FJsonObject;
class FJsonValue;

// One genuine fourth-profile delivery. Explicit Development automation only.
UCLASS()
class DDSCITY_API AMAYACityBlockMissionTrial : public AActor
{
    GENERATED_BODY()
public:
    AMAYACityBlockMissionTrial();
    void Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector,
        const FMAYACityBlockRouteDefinition& InDefinition);
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    enum class EStep : uint8 { Baseline, Pickup, Loading, ArrowView, Driving, CrossWait, Dropoff, Unloading, FinalView };
    enum class EFollowerMotion : uint8 { Forward, BrakeForTurn, Turn, BrakeAfterTurn };
    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TWeakObjectPtr<AMAYADeliveryDirector> Director;
    TWeakObjectPtr<AMAYADeliveryParcel> Parcel;
    TWeakObjectPtr<UPrimitiveComponent> ParcelBody;
    FMAYACityBlockRouteDefinition Definition;
    MAYAHUDGeometry::FPolyline Path;
    EStep Step = EStep::Baseline;
    EFollowerMotion FollowerMotion=EFollowerMotion::Forward;
    FVector FollowerAlignmentAim=FVector::ZeroVector, LastFollowerAim=FVector::ZeroVector;
    double FollowerAlignmentStarted=-1, FollowerStableSince=-1, LastFollowerPolicyWorldSeconds=-1;
    double LastFollowerDesiredYaw=0, LastFollowerHeadingError=0, LastCommandWorldSeconds=-1;
    uint64 LastCommandFrame=0;
    float LastRequestedThrottle=0, LastRequestedSteer=0;
    FString LastCommandSteeringMode=TEXT("initial");
    bool LastRequestedBrake=true;
    int32 FollowerAlignmentEpisodes=0;
    int32 NextAuthoredStop=0;
    double AuthoredStopStarted=-1, AuthoredStopStableSince=-1;
    double LastAuthoredStopDistanceCm=0, MaximumAuthoredStopExcursionCm=0;
    double AuthoredStopOutgoingYawDegrees=0;
    FVector AuthoredStopTurnAnchorCm=FVector::ZeroVector;
    TArray<TSharedPtr<FJsonValue>> AuthoredStopReceipts;
    TSharedPtr<FJsonObject> LastAuthoredBrakeCapture;
    double RollingHalfTrackCm=-1, LastRollingX=0, LastRollingY=0, LastRollingCurvature=0, LastRollingRatio=0;
    double LastRollingRawThrottle=0, LastRollingRawSteer=0, LastRollingNormalization=1;
    TSharedPtr<FJsonObject> RollingGeometryCapture;
    FString Output, JobId, ShiftId, ParcelId, PendingCapture;
    uint64 ResetGeneration=0, CaptureFrame=0;
    double Started=-1, StepStarted=-1, StableSince=-1, NextRecord=-1, NextHeavy=-1;
    double Progress=0, CrossWaitStarted=-1, LastAdvanceWorldS=-1, LastAdvanceArc=0;
    double CaptureWorldSeconds=-1, CaptureProcessedWorldSeconds=-1, CaptureRealSeconds=-1, NextArrowCapture=-1;
    FIntPoint CaptureSize=FIntPoint::ZeroValue;
    uint64 CaptureProcessedFrame=0;
    int32 SelectedOffers=0, ActiveCrossing=INDEX_NONE, ArrowCaptureCount=0, DriveCalls=0, ServiceCalls=0;
    int32 CrossingEntranceMask=0, CompletedCrossingMask=0, BaseCash=0, BaseFees=0, BaseCompleted=0;
    bool bFinished=false, bCaptureProcessed=false, bFinalCaptureCompleted=false, bLastCommandAccepted=false;
    FDelegateHandle ScreenshotHandle;
    TArray<TSharedPtr<FJsonValue>> Checks, Samples, Captures, CrossingReceipts;
    TSharedPtr<FJsonObject> ArrowFirstGuardMiss;
    bool Check(bool Passed, const FString& Name);
    bool Drive(float Throttle=0, float Steer=0, bool Brake=true);
    bool Quiet(const FMAYADeliveryRobotObservation& O) const;
    bool AlignmentRest(const FMAYADeliveryRobotObservation& O) const;
    bool AdvanceAuthoredStop(const FMAYADeliveryRobotObservation& O, double Now);
    bool CaptureRollingGeometry(const FMAYADeliveryRobotObservation& O);
    bool InventoryIntact() const;
    bool CrossingTrafficClear(int32 CrossingIndex, TSharedPtr<FJsonObject>& Evidence) const;
    void Enter(EStep Next);
    void Record(const FMAYADeliveryRobotObservation& O, bool Force=false, const TCHAR* AlignmentEndpoint=nullptr);
    bool RequestCapture(const FString& Name);
    bool PollCapture();
    void ScreenshotProcessed();
    void Finish(bool Success, const FString& Reason);
};
