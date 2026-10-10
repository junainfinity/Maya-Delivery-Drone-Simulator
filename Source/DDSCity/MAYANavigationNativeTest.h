#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "MAYADeliveryTypes.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYANavigationNativeTest.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class AMAYADeliveryPlayerController;
class AMAYADeliveryHUD;
class AMAYADeliveryParcel;
class UStaticMeshComponent;
class FJsonObject;
class FJsonValue;

// Explicit rendered Development QA. Controller callbacks are not OS keyboard evidence.
UCLASS()
class DDSCITY_API AMAYANavigationNativeTest : public AActor
{
    GENERATED_BODY()
public:
    AMAYANavigationNativeTest();
    void Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector, const TArray<FVector>& MeasuredRoute);
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TWeakObjectPtr<AMAYADeliveryDirector> Director;
    TWeakObjectPtr<AMAYADeliveryPlayerController> PC;
    TWeakObjectPtr<AMAYADeliveryHUD> HUD;
    MAYAHUDGeometry::FPolyline Path;
    FString Output, ActualSettingsPath, InitialProfile, ShiftId, PendingCapture;
    FVector InitialPosition=FVector::ZeroVector;
    double StartedWorld=-1, StartedReal=-1, StepReal=-1, StepWorld=-1, StableSince=-1, NextRecord=-1;
    double StartArc=0, GoalArc=0, TurnStartYaw=0, MeasuredTurn=0, MaximumTranslation=0;
    double FrozenWorld=-1, FrozenElapsed=0, FrozenBattery=0, FrozenCharge=0;
    double InitialCacheCount=0, MinimumMovingCacheCount=DBL_MAX, LastViewSamples=-1, InitialOrbitYaw=0, InitialOrbitPitch=0;
    double NextMovingCameraRecord=-1, MaximumMovingResidualZ=0, MaximumMovingResidualAngle=0;
    uint64 ResetGeneration=0, EnteredFrame=0, KeyFrame=0, CaptureRequestFrame=0, CaptureProcessedFrame=0;
    double CaptureRequestWorld=-1, CaptureProcessedWorld=-1, CaptureStartedReal=-1;
    FIntPoint CaptureSize=FIntPoint::ZeroValue;
    FDelegateHandle ScreenshotHandle;
    FKey PendingRelease;
    FKey CaptureNextKey;
    bool bCaptureHoldW=false;
    TSet<FKey> HeldKeys;
    TArray<TSharedPtr<FJsonValue>> Checks, Samples, Captures, Actions, CacheIdentities, MovingCameraSamples;
    TMap<FIntPoint,TSharedPtr<FJsonObject>> InitialCells;
    TSharedPtr<FJsonObject> InitialMap, MovedMap, OfferMap, WholeMap, FocusMap, PovCamera, ChaseCamera;
    int32 Step=0, BaseCash=0, BaseFees=0, BaseCompleted=0, DriveCalls=0, UnknownObservations=0, RefreshObservations=0;
    bool bFinished=false, bCaptureProcessed=false, bOverlapRefreshed=false, bSawRoadIdentity=false, bSawSidewalkIdentity=false;
    bool bHUDStyleTest=false;
    TWeakObjectPtr<AMAYADeliveryParcel> HUDStyleParcel;
    TWeakObjectPtr<UStaticMeshComponent> HUDStyleParcelBody;
    FString HUDStyleParcelId,HUDStyleJobId;
    double HUDStyleCondition=0,HUDStyleCarryWorld=-1;
    TSharedPtr<FJsonObject> PendingHUDStyleView,PendingHUDStyleCamera;
    void TickHUDStyle(const FMAYADeliveryRobotObservation& O);
    bool ObserveHUDStyle(EMAYADeliveryPhase Expected,TSharedPtr<FJsonObject>& Out) const;
    bool bMovingPovAdmitted=false;
    int32 TranslatingPovSamples=0, TurningPovSamples=0;
    bool Check(bool Passed,const FString& Name);
    bool Drive(float Throttle=0,float Steer=0,bool Brake=true);
    bool Key(FKey Value,EInputEvent Event);
    bool Action(FName Name,int32 Direction=1);
    void Enter(int32 Next);
    bool Quiet(const FMAYADeliveryRobotObservation& O) const;
    bool ObserveMinimap(const FMAYADeliveryRobotObservation& O);
    bool ObserveFullMap(const FString& ExpectedView,TSharedPtr<FJsonObject>& Out);
    bool ObserveCamera(bool Pov,TSharedPtr<FJsonObject>& Out);
    void Record(const FMAYADeliveryRobotObservation& O,bool Force=false);
    bool Capture(const FString& Name);
    bool PollCapture();
    void ScreenshotProcessed();
    void Finish(bool Success,const FString& Reason);
};
