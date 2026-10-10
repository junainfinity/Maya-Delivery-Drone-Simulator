#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MAYADeliveryCore.h"
#include "MAYAPlayerPresentation.h"
#include "MAYACityBlockRoute.h"
#include "MAYADeliveryDirector.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryStation;
class AMAYADeliveryParcel;
class AMAYACargoWorker;
class UMAYACrowdObstacleComponent;
class UMAYAVehicleContactObserver;
class UPrimitiveComponent;

// Gameplay-time visit state and actual request receipts; no input/focus claim.
struct FMAYADeliveryRingObservation
{
    FString ActiveStationId;
    bool bInside = false, bEligible = false, bVisitConsumed = false;
    uint64 EntryGeneration = 0, RequestGeneration = 0, AutomaticTriggerGeneration = 0;
    double ElapsedGameplaySeconds = 0, RemainingGameplaySeconds = 10;
    FString LastRequestOrigin;
    bool LastRequestAccepted = false;
    uint64 LastRequestServiceGeneration = 0;
    double LastRequestWorldSeconds = -1;
    double LastRequestElapsedGameplaySeconds = 0;
    uint64 LastRequestEntryGeneration = 0;
    bool LastAutomaticRequestAccepted = false;
    uint64 LastAutomaticServiceGeneration = 0, LastAutomaticEntryGeneration = 0;
    double LastAutomaticRequestWorldSeconds = -1;
    double LastAutomaticElapsedGameplaySeconds = 0;
    FString LastAutomaticStationId;
    void Advance(const FString& StationId, bool bActive, bool bObservedInside,
        bool bPhaseEligible, double DeltaSeconds, bool bPaused);
    void ConsumeIfAdmitted(bool bAdmitted) { if (bInside && bEligible && bAdmitted) bVisitConsumed = true; }
};

UCLASS()
class DDSCITY_API AMAYADeliveryDirector : public AActor
{
    GENERATED_BODY()
public:
    AMAYADeliveryDirector();
    virtual void Tick(float Dt) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    bool ConfigureStations(const TArray<AMAYADeliveryStation*>& InStations, const TArray<FVector>& MeasuredRoute);
    void BindRobot(AMAYANativeDemoPawn* InRobot);
    bool StartShift();
    FMAYAPlayerPresentation GetPlayerPresentation() const;
    const FMAYAVehicleContactEvent& GetLastVehicleContact() const;
    FString ReadVehicleContactEvidenceJson() const;
    uint64 GetShiftStartedGameFrame() const { return ShiftStartedGameFrame; }
    bool RequestAccept();
    bool RequestSelectOffer(int32 Delta);
    // Additive content seam. The audited immutable definition must include the
    // exact old measured prefix. No old station/Core/diagnostic route is reset.
    bool InstallCityBlockMission(AMAYADeliveryStation* Station, const FMAYACityBlockRouteDefinition& Definition);
    int32 GetAvailableProfileCount() const;
    bool IsCityBlockRouteReady() const;
    bool UsesCityBlockGuidance() const;
    bool IsCityBlockStation(const AMAYADeliveryStation* Station) const;
    const TArray<FVector>& GetActiveGuidanceRoute() const;
    FMAYARouteAdvicePresentation GetRouteAdvice() const;
    bool RequestInteract();
    // Nonshipping explicit native regression only; invokes the production cancel path.
    bool RequestNativeOpeningCancellationForTest();
    bool WasCancelledOpenObserved(uint64 Generation) const
    { return Generation != 0 && CancelledOpenServiceGeneration == Generation && CancelledOpenObservationWorldSeconds >= 0; }
    FString ReadServiceCancellationEvidenceJson() const;
    bool IsCargoAirFadingOut() const;
    bool CancelAirUnloadingForNativeTest();
    bool IsCargoAirLowering() const;
    bool IsCargoAirAnimationActive() const;
    double GetCargoAirPhaseElapsedSeconds() const;
    bool CancelAirLoweringForNativeTest();
    bool RequestNext(bool bRetryCurrent = false);
    bool RequestRecover();
    bool RequestCharging();
    AMAYADeliveryStation* GetNearbyChargingStation() const;
    FString GetChargingStationName() const;
    void SetPaused(bool bInPaused);
    bool IsNativeOperatingBatteryFixtureApplied() const { return bNativeOperatingBatteryFixtureApplied; }
    double GetNativeOperatingInitialBatteryPercent() const { return NativeOperatingInitialBatteryPercent; }
    const FString& GetNativeOperatingBatteryFixtureId() const { return NativeOperatingBatteryFixtureId; }
    const FMAYADeliverySnapshot& Snapshot() const { return Rules.Get(); }
    const FMAYADeliveryRobotObservation& Observation() const { return RobotState; }
    AMAYANativeDemoPawn* GetRobot() const { return Robot; }
    AMAYADeliveryParcel* GetParcel(int32 Slot = 0) const;
    int32 GetParcelCount() const;
    int32 GetSecuredParcelCount() const;
    double GetSecuredCargoKg() const;
    bool GetRecoveryParcelStockPose(int32 Slot, FTransform& OutPose) const;
    bool CanRequestNext() const;
    FString ReadCargoInventoryEvidenceJson() const;
    AMAYACargoWorker* GetActiveCargoWorker() const { return nullptr; }
    FString ReadCargoWorkerEvidenceJson() const;
    FString ReadCrowdObstacleEvidenceJson() const;
    AMAYADeliveryStation* ObjectiveStation() const;
    AMAYADeliveryStation* FindStation(const FString& Id) const;
    const TArray<FVector>& GetMeasuredRoute() const { return Route; }
    FString GetPrompt() const;
    const FString& GetRuntimeStatus() const { return RuntimeStatus; }
    const FString& GetLastServiceFailureDetail() const { return LastServiceFailureDetail; }
    const FString& GetLastRecoveryDetail() const { return LastRecoveryDetail; }
    const FString& GetLastServiceCommitEvidenceJson() const { return LastServiceCommitEvidenceJson; }
    bool IsPaused() const { return bPaused; }
    const FMAYADeliveryRingObservation& GetRingObservation() const { return RingObservation; }

private:
    enum class EServiceStep : uint8 { None, Opening, Materializing, Lowering, AirEndpointWait, Raising, AirRemovalEndpointWait, Dematerializing, Settling, Closing, RollbackOpening, RollbackClosing, RollbackBlocked };
    bool ReadRobot();
    bool RequestService(bool bAutomatic);
    bool TryStartService();
    void UpdateRingVisit(double DeltaSeconds);
    void TickAutomaticService(double DeltaSeconds);
    bool OfferProfile(int32 Index);
    void RefreshCityBlockRoute();
    bool SpawnParcels();
    void DestroyParcels();
    FString ParcelIdentity(int32 Slot) const;
    bool AtCharger() const;
    FMAYADeliveryChargingObservation ReadChargingObservation() const;
    uint64 ShiftStartedGameFrame = MAX_uint64;
    bool BeginAirUnload();
    bool BeginAirPickup();
    bool UpdateAirPickupPreparation();

    bool RestoreServiceInventory();
    bool AllInsertedParcelsSettled() const;
    void CompleteRecovery();
    void TickService(float Dt);
    void InterruptService(const FString& Reason);
    void RecordServiceFailure(const FString& Stage, bool bNativeAdmissionWasAttempted = false, const FString& Reason = FString());
    UPrimitiveComponent* CarrierBody() const;
    UFUNCTION() void OnRobotHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

    UPROPERTY() TObjectPtr<AMAYANativeDemoPawn> Robot;
    UPROPERTY() TObjectPtr<UMAYACrowdObstacleComponent> CrowdObstacle;
    UPROPERTY() TObjectPtr<UMAYAVehicleContactObserver> VehicleContactObserver;
    void ConsumeVehicleContacts();
    UPROPERTY() TArray<TObjectPtr<AMAYADeliveryParcel>> Parcels;
    UPROPERTY() TArray<TObjectPtr<AMAYADeliveryStation>> Stations;
    FMAYADeliveryCore Rules;
    FMAYADeliveryRobotObservation RobotState;
    FMAYADeliveryServiceToken ServiceToken;
    TArray<FTransform> ServiceOrigins;
    TArray<FTransform> RecoveryParcelPoses;
    TArray<double> ServiceParcelMasses;
    TArray<FVector> Route;
    FMAYACityBlockRouteDefinition CityBlockRoute;
    FMAYACityBlockRouteObservation CityBlockObservation;
    FString CityBlockDestinationId;
    double LastCityBlockReadWorldSeconds = -1;
    EServiceStep ServiceStep = EServiceStep::None;
    int32 ProfileIndex = 0;
    bool bPaused = false, bOpenObserved = false, bTransferAdmitted = false;
    bool bServiceCreatedInventory = false, bInstantTransitionApplied = false;
    bool bRecoveryPending = false, bRecoveryParcelExpected = false;
    double AirPhaseStartWorldSeconds = -1;
    // Pickup-only preparation, before capturing the approved fixed trajectory.
    // Actual pose stability is preparation only; all later path guards remain.
    double AirPickupOpenStartWorldSeconds = -1, AirPickupWindowStartWorldSeconds = -1;
    double AirPickupPreparationSampleWorldSeconds = -1, AirPickupPreparationWindowSeconds = 0;
    double AirPickupPeakBaySpeedCmps = 0, AirPickupPeakAngularRadps = 0;
    double AirPickupMaxBayExcursionCm = 0, AirPickupMaxAngularExcursionDegrees = 0;
    double AirPickupObservedBayMarginCm = 0, AirPickupObservedAngularMarginDegrees = 0;
    double AirPickupNativeComponentPositionDeltaCm = 0, AirPickupNativeComponentAngularDeltaDegrees = 0;
    FTransform AirPickupInitialNativePose = FTransform::Identity, AirPickupLastNativePose = FTransform::Identity;
    FVector AirPickupInitialBayCm[2] = {};
    uint32 AirPickupPreparationWindowResets = 0;
    bool bAirPickupPreparationReady = false;
    bool bNativeAirCancellationTestUsed = false, bNativeAirUnloadCancellationTestUsed = false;
    double EndpointWaitStartWorldSeconds = -1;
    double RollbackStartWorldSeconds = -1;
    uint64 CancellationGeneration = 0;
    uint64 CancelledOpenServiceGeneration = 0;
    double CancelledOpenObservationWorldSeconds = -1;
    bool bNativeOpeningCancellationTestUsed = false;
    FString LastCancellationReason;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> ImpactBodies;
    void UnbindImpactBodies();
    FVector PreviousChassisVelocity = FVector::ZeroVector;
    struct FContactBurst { double LastContactWorldSeconds = -1; uint64 Generation = 0; double PeakImpulseKgMps = 0; double PeakApproachCmps = 0; FString PeakRobotComponent; };
    TMap<TWeakObjectPtr<UPrimitiveComponent>, FContactBurst> ContactBursts;
    TArray<FMAYADeliveryImpactEvent> PendingChassisImpacts;
    uint64 NextChassisImpactGeneration = 0;
    bool bNativeOperatingBatteryFixtureApplied = false;
    double NativeOperatingInitialBatteryPercent = 100;
    FString NativeOperatingBatteryFixtureId;
    FString RuntimeStatus = TEXT("Waiting for native robot and surveyed city stations.");
    FString LastServiceFailureDetail;
    FString LastRecoveryDetail;
    FString LastServiceCommitEvidenceJson;
    FMAYADeliveryRingObservation RingObservation;
};
