#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryTypes.h"
#include "Serialization/Archive.h"
#include "Containers/Ticker.h"
#include "MAYAV01NativeTestDriver.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class AMAYADeliveryVisualCapture;
class AMAYAV01DamageFixture;
class AMAYAV01CurbTrial;
class AMAYAV01TrafficEncounterTrial;
class ACameraActor;
class AHUD;
class UCanvas;
class APlayerController;
class FMAYAInstantCargoVideoCapture;
class AMAYADeliveryParcel;
class UPrimitiveComponent;

// Development-only, explicit command-line opt-in. Observes real post-physics bodies.
UCLASS()
class DDSCITY_API AMAYAV01NativeTestDriver : public AActor
{
    GENERATED_BODY()
public:
    AMAYAV01NativeTestDriver();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;
    void Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector);
private:
    enum class EPhase : uint8
    {
        Settle, Forward, ForwardBrake, Reverse, ReverseBrake,
        LeftTurn, LeftBrake, RightTurn, RightBrake,
        FullEmptyPosition, FullEmptyAlign, FullEmptyForward, FullEmptyForwardBrake,
        FullEmptyReverse, FullEmptyReverseBrake, FullEmptyLeftCenter, FullEmptyLeftSettle, FullEmptyLeft, FullEmptyLeftBrake,
        FullEmptyRightCenter, FullEmptyRightSettle, FullEmptyRight, FullEmptyRightBrake,
        FullLoadedPosition, FullLoadedAlign, FullLoadedForward, FullLoadedForwardBrake,
        FullLoadedReverse, FullLoadedReverseBrake, FullLoadedLeftCenter, FullLoadedLeftSettle, FullLoadedLeft, FullLoadedLeftBrake,
        FullLoadedRightCenter, FullLoadedRightSettle, FullLoadedRight, FullLoadedRightBrake,
        DoorOpen, DoorDriveProbe, DoorClose, VisualOpen, VisualClosed, Recover, RecoverSettle,
        ChargeSettle, Charge, PaidChargeReturn, PaidChargeWait, PaidChargeDeparture, PaidChargeStop, Accept, ToPickup, Loading, LoadedDrive, LoadedBrake,
        CarriedRecover, CarriedRecoverSettle, ToDropoff, Unloading, NextJob, CashOpeningRetry, CancelOpeningAwaitClosure, CancelOpeningRetry,
        RingAccept, RingEarlyMovingInteraction, RingExit, RingOutsideSettle,
        RingReenter, RingInsideSettle, RingAwaitAutomatic,
        OperatingAccept, OperatingDrain, OperatingDepletedHold, OperatingRecover,
        OperatingRecoverSettle, OperatingCharge, OperatingUrgentAccept,
        OperatingUrgentWait, OperatingDeadlineHold, OperatingRetryAccept, DamageEmptyTrial, DamageEmptyHard, DamageEmptyReturn, DamageLoadedSettle, DamageLoadedSmall, DamageLoadedHard, CurbTrial, TrafficEncounter, ReverseSteering, Complete, Failed
    };
    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<AMAYADeliveryDirector> Director;
    UPROPERTY() TObjectPtr<AMAYADeliveryVisualCapture> VisualCapture;
    EPhase Phase = EPhase::Settle;
    FTransform PhaseStartPose = FTransform::Identity;
    FVector LastPosition = FVector::ZeroVector;
    double RunStartWorldS = -1, PhaseStartWorldS = -1, StableSinceWorldS = -1, LastSampleWorldS = -1;
    double BrakeStartSpeed = 0, BrakeTravelCm = 0;
    int32 MissionCount = 0;
    bool bStopped = false, bGeometryCaptured = false, bLastCommandAccepted = false;
    bool bCargoDiagnosticTest = false;
    void CashWinRendered(AHUD* HUD, UCanvas* Canvas);
    void CashWinScreenshotProcessed();
    bool TickCashWinFinalizer(float DeltaSeconds);
    void FinishCashWinFinalizer(bool bSuccess, const FString& Reason);
    FDelegateHandle CashWinRenderHandle, CashWinScreenshotHandle;
    FTSTicker::FDelegateHandle CashWinTickerHandle;
    double CashWinFinalizerStartedRealS = -1, CashWinFrozenBattery = 0;
    int32 CashWinFrozenCash = 0, CashWinFrozenFees = 0, CashWinFrozenJobs = 0;
    uint64 CashWinDrawFrame = 0, CashWinRequestFrame = 0, CashWinProcessedFrame = 0;
    FIntPoint CashWinViewportSize = FIntPoint::ZeroValue;
    FString CashWinCapturePath, CashWinCompletionReason;
    bool bCashWinCaptureRequested = false, bCashWinCaptureProcessed = false;

    bool bDamageTest = false, bDamageTrialInitialized = false, bDamageCalibration = false;
    UPROPERTY() TObjectPtr<AMAYAV01DamageFixture> DamageFixture;
    bool bCurbTest = false, bTrafficEncounterTest = false;
    UPROPERTY() TObjectPtr<AMAYAV01TrafficEncounterTrial> TrafficEncounterTrial;
    UPROPERTY() TObjectPtr<AMAYAV01CurbTrial> CurbTrial;
    bool bCancelOpeningTest = false, bOpeningCancelTriggered = false, bCancelledOpenObserved = false, bOpeningCancelRetried = false;
    bool bAirLoweringTest = false, bAirLoweringPauseComplete = false, bAirLoweringCancelTriggered = false;
    bool bAirUnloadPauseComplete = false, bAirUnloadCancelTriggered = false, bAirUnloadRetried = false;
    bool bAirUnloadCancellationInProgress = false, bPausedDuringAirUnloading = false;
    TArray<TWeakObjectPtr<AMAYADeliveryParcel>> AirUnloadActors;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> AirUnloadBodies;
    TArray<FString> AirUnloadIds;
    TArray<double> AirUnloadMasses;
    bool TryStartAirUnloadingPause(const FMAYADeliveryRobotObservation& Observation);
    bool bPausedDuringAirLowering = false;
    FString PausedAirEvidenceJson;
    TArray<TSharedPtr<class FJsonValue>> AirLoweringRegressionReceipts;
    bool TryStartAirLoweringPause(const FMAYADeliveryRobotObservation& Observation);
    FString OpeningCancelJobId;
    uint64 OpeningCancelServiceGeneration = 0, OpeningCancelResetGeneration = 0;
    int32 OpeningCancelCredits = 0;
    double OpeningCancelBatteryPercent = 0, OpeningCancelConditionPercent = 100;
    TArray<TSharedPtr<class FJsonValue>> OpeningCancellationReceipts;
    FString RunDirectory;
    int32 SampleCount = 0;
    uint64 TelemetryBytes = 0;
    TUniquePtr<FArchive> TelemetryWriter;
    bool bTelemetryWriterClosed = false;
    void CloseTelemetryWriter();
    double LastWrittenSampleWorldS = -1;
    FString LastRecordedAppliedImpactEvidence;
    bool bTelemetryCapped = false, bTelemetryWriteFailed = false;
    FString TelemetryStatus = TEXT("within_declared_limits");
    FString LastRecordedCargoInitializationEvidence, LastRecordedServiceCommitEvidence;
    FVector ServiceApproachCm = FVector::ZeroVector, ServiceStationCm = FVector::ZeroVector;
    FString ServiceStationId;
    bool bStationArrivalLatched = false, bRingViewRequested = false;
    double ArrivalLatchWorldS = -1;
    int32 ArrivalLatchExitCount = 0;
    bool bAwaitAutomaticRingPickup = false, bAutomaticRingPickupObserved = false;
    bool bUnexpectedAutomaticServiceObserved = false;
    int32 AutomaticWaitFrames = 0, AutomaticWaitEntryInteractionCalls = 0;
    int32 StationInteractionCalls = 0, AutomaticRingAdmissionCount = 0, ExplicitRingAdmissionCount = 0;
    TArray<TSharedPtr<class FJsonValue>> RingServiceReceipts, RingViewReceipts;
    void SaveRingServiceReceipt(const FMAYADeliveryRobotObservation& Observation, bool bDriverRequested);
    uint8 SupportedWheelMotionMask = 0;
    bool bBlockedPathChecked = false, bCarriedRecoveryChecked = false;
    uint8 CarriedRecoveryCountMask = 0;
    bool bFullLoadedTrialsComplete = false;
    double ChargeStartPercent = 0;
    bool bInitialUnfundedNoGainObserved = false, bPaidRingChargeObserved = false, bPaidMovementStopObserved = false;
    int32 PaidChargeCashBefore = 0;
    uint64 PaidChargeReceiptBefore = 0;
    FVector PaidChargeStationCm = FVector::ZeroVector;
    FString PaidChargeStationId, PaidChargeShiftId;
    TArray<TSharedPtr<class FJsonValue>> PaidChargeReceipts;
    bool bCashShiftTest = false, bActualCashShiftWinObserved = false;
    int32 CashShiftCompletedJobsObserved = 0, CashShiftDeliveryEarnings = 0;
    double LastCashShiftHeavyReceiptWorldS = -1;
    FString CashShiftInstanceId;
    TSet<FString> CashShiftCompletedIds;
    TArray<TSharedPtr<class FJsonValue>> CashShiftDeliveryReceipts;
    bool ObserveCashShift(const FMAYADeliveryRobotObservation& Observation);
    // CashShift or explicit TrafficEncounter only: one natural, empty Opening angular-stop cancellation may
    // return through the normal API after bounded settlement. No product bypass.
    FMAYADeliverySnapshot CashOpeningBefore;
    uint64 CashOpeningResetGeneration = 0, CashOpeningCancellationBefore = 0;
    uint64 CashNaturalRetryServiceGeneration = 0;
    bool bCashOpeningObserved = false, bCashNaturalRetryUsed = false;
    bool bCashNaturalRetryAdmitted = false, bCashNaturalRetrySecured = false;
    FString CashOpeningNativeEvidence, CashNaturalOriginalFailure;
    FString CashNaturalCancellationEvidence;
    double CashNaturalClosureBatteryPercent = 0;
    FTransform CashNaturalRestWindowPose = FTransform::Identity;
    double CashNaturalMaxWindowCm = 0, CashNaturalMaxWindowDegrees = 0;
    TArray<TSharedPtr<class FJsonValue>> CashNaturalOpeningRetryReceipts;
    void ObserveCashOpening(const FMAYADeliveryRobotObservation& Observation);
    bool BeginCashOpeningRetry(const FMAYADeliveryRobotObservation& Observation);
    bool CashOpeningIdentityUnchanged(const FMAYADeliveryRobotObservation& Observation) const;
    void TickCashOpeningRetry(const FMAYADeliveryRobotObservation& Observation, double Age, double Now);
    bool bOperatingTest = false, bOperatingRechargeObserved = false;
    uint64 OperatingInitialAutomaticServiceGeneration = 0;
    bool bRingInteractionTest = false, bRingEarlyInteractionRejected = false;
    bool bRingExitResetObserved = false, bRingReentryResetObserved = false;
    bool bRingCountdownPauseComplete = false, bPausedDuringRingCountdown = false;
    uint64 RingFirstEntryGeneration = 0;
    FString RingTestJobId, PausedRingEvidenceJson;
    int32 RingTestCreditsBefore = 0;
    TArray<TSharedPtr<class FJsonValue>> RingInteractionReceipts;
    bool TryStartRingCountdownPause(const FMAYADeliveryRobotObservation& Observation);
    bool bOpeningPauseComplete = false, bClosingPauseComplete = false, bNativePauseActive = false;
    bool bPausedDuringOpening = false;
    double PauseStartedRealS = -1, LastPauseSampleRealS = -1, PauseWorldS = -1, PauseAudioS = -1;
    FMAYADeliverySnapshot PausedMission;
    FMAYADeliveryRobotObservation PausedRobot;
    FTransform PausedParcelPose = FTransform::Identity;
    FString PausedParcelId, OperatingFailedJobId;
    FMAYADeliveryResult OperatingFrozenResult;
    int32 OperatingCreditsBefore = 0, OperatingZeroMotorSamples = 0;
    uint64 OperatingPreResetGeneration = 0;
    bool ReadOperatingZeroMotorEvidence() const;
    bool TryStartNativeLidPause(const FMAYADeliveryRobotObservation& Observation);
    void TickNativeLidPause();
    bool bReverseSteerTest = false;
    int32 ReverseSegmentIndex = 0;
    uint8 ReverseStep = 0;
    FVector ReverseEntryCm = FVector::ZeroVector, ReverseLastBrakeCm = FVector::ZeroVector;
    FTransform ReverseStartPose = FTransform::Identity;
    double ReverseStageWorldS = -1, ReverseStableSinceWorldS = -1, ReverseBrakeTravelCm = 0;
    uint64 ReverseResetGeneration = 0;
    FString ReverseGroundEvidence, ReverseLastDriveEvidence;
    TArray<TSharedPtr<class FJsonValue>> ReverseSteeringReceipts;
    bool CheckReverseSteeringGround(const FMAYADeliveryRobotObservation& Observation);
    void TickReverseSteering(const FMAYADeliveryRobotObservation& Observation);
    bool bFullInputTrials = false, bVisualCaptureRequested = false, bBoostTest = false;
    bool bLastBoostRequested = false;
    int32 ActualBoostDriveFrames = 0, PhaseBoostDriveFrames = 0;
    bool bFullSetupArrivalLatched = false;
    bool bFullCenterReverse = false;
    FTransform FullRestWindowPose = FTransform::Identity;
    double FullRestMaxTranslationCm = 0, FullRestMaxYawDegrees = 0;
    TArray<TSharedPtr<class FJsonValue>> VisualCaptureReceipts;
    void CancelOwnedVisualCapture();
    double PhaseStartAngularSpeedRadps = 0;
    double PhasePeakSpeedCmps = 0, PhasePeakTiltDegrees = 0, PhasePeakSampledSuspensionCm = 0;
    int32 PhaseUnsupportedFrames = 0, DoorClosingProbeSamples = 0;
    bool bDoorCloseRequested = false, bDoorClosingProbePending = false;
    uint64 DoorClosingProbeIssuedFrame = 0;
    double LastServiceCaptureWorldS = -1;
    int32 ServiceCaptureCount = 0;
    bool bCargoVideoRequested = false, bServiceVideoAttempted = false;
    uint64 ServiceVideoStartFrame = 0;
    double ServiceViewStartWorldS = -1;
    int32 ServiceVideoClipCount = 0, ServiceVideoRequestCoverageMask = 0, ActiveServiceVideoParcelCount = 0;
    bool bActiveServiceVideoLoading = false;
    FString ActiveServiceVideoLabel;
    TArray<TSharedPtr<class FJsonValue>> ServiceVideoReceipts;
    FString ReadServiceVideoFrameEvidenceJson(const FMAYADeliveryRobotObservation& Observation) const;
    TSharedPtr<FMAYAInstantCargoVideoCapture> ServiceVideo;
    // Explicit AutoTest-only observation view. No cargo, actor or input writes.
    UPROPERTY(Transient) TObjectPtr<ACameraActor> ServiceObservationCamera;
    TWeakObjectPtr<APlayerController> ServiceViewController;
    TWeakObjectPtr<AActor> ServicePriorView;
    TWeakObjectPtr<AHUD> ServiceViewHud;
    bool bServicePriorHudVisible = true, bServiceHudTaken = false;
    FString ServiceObservationViewStatus = TEXT("not_started");
    int32 ServiceObservationViewCount = 0;
    void UpdateInstantServiceObservationView(const FMAYADeliveryRobotObservation& Observation);
    void RestoreInstantServiceObservationView();
    bool ReadZeroWheelMotorEvidence() const;
    uint64 CarriedResetGeneration = 0;
    FString CarriedJobId;
    FTransform LoadedParcelRelativeToChassis = FTransform::Identity; // Slot zero compatibility only.
    TArray<FTransform> LoadedParcelRelatives, PausedParcelPoses;
    TArray<TWeakObjectPtr<AMAYADeliveryParcel>> LoadedParcelActors, CarriedParcelActors;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> LoadedParcelBodies;
    TArray<FString> LoadedParcelIds, PausedParcelIds, CarriedParcelIds;
    TArray<double> PausedParcelMasses;
    TArray<bool> PausedParcelDormant, PausedParcelSimulating;
    TArray<int32> PausedParcelCollision;
    FString InventoryFailure;
    bool VerifyLoadedInventory(const FMAYADeliveryRobotObservation& O, bool bCapture = false);
    TArray<TSharedPtr<class FJsonValue>> ReadParcelInventoryJson() const;
    FString ReadParcelIdentityPoseJson() const;
    bool CapturePausedInventory();
    bool PausedInventoryUnchanged() const;
    bool bCarriedPreflightInitialized = false;
    int32 CarriedPreflightCredits = 0, CarriedPreflightQueryCount = 0;
    double LastCarriedPreflightQueryWorldS = -1;
    TArray<TSharedPtr<class FJsonValue>> CarriedRecoveryPreflightReceipts;
    TArray<TSharedPtr<class FJsonValue>> PhaseResults;
    void Enter(EPhase Next, const FMAYADeliveryRobotObservation& Observation);
    void Stop(bool bSuccess, const FString& Reason);
    void Record(const FMAYADeliveryRobotObservation& Observation, bool bForce = false, const TCHAR* Event = TEXT("periodic"));
    void WriteReport(bool bTerminal, bool bSuccess, const FString& Reason);
    void CaptureCollisionGeometry();
    bool Drive(float Throttle, float Steer, bool bBrake = false);
    bool NavigateTo(const FVector& Destination, const FMAYADeliveryRobotObservation& Observation);
    bool FindServiceApproach(const FVector& StationPoint, FVector& OutApproach) const;
    static const TCHAR* PhaseName(EPhase Value);
};
