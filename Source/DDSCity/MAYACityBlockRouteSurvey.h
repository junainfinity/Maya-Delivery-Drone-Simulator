#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassEntityQuery.h"
#include "ZoneGraphTypes.h"
#include "MAYADeliveryTypes.h"
#include "MAYACityBlockRouteSurvey.generated.h"

class AMAYANativeDemoPawn;
class AMAYADeliveryDirector;
class AZoneGraphData;
class UWorldPartitionStreamingSourceComponent;
class FJsonObject;
class FJsonValue;

// Explicit Development survey only. No native drive, resource or signal setters.
UCLASS()
class DDSCITY_API AMAYACityBlockRouteSurvey : public AActor
{
    GENERATED_BODY()
public:
    AMAYACityBlockRouteSurvey();
    void Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector);
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    struct FBodyProxy { FVector Offset, Extent; FString Component; };
    struct FNode { FZoneGraphLaneHandle Lane; FVector Point, Forward; int32 Sequence=0; FString Profile; double ProfileOffsetCm=0; };
    struct FGraphBinding { TWeakObjectPtr<AZoneGraphData> Actor; FZoneGraphDataHandle Handle; int32 LaneCount=0; };
    struct FAuditLane { FZoneGraphLaneHandle Handle; TArray<FVector> Points; TSharedPtr<FJsonObject> Row; double Priority=0; };
    UPROPERTY() TObjectPtr<UWorldPartitionStreamingSourceComponent> SurveyStreaming;
    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TWeakObjectPtr<AMAYADeliveryDirector> Director;
    FMassEntityQuery IntersectionQuery;
    TArray<FGraphBinding> Graphs;
    TArray<FZoneGraphLaneHandle> Crosswalks;
    TArray<FBodyProxy> Proxies;
    TArray<FNode> Nodes;
    TArray<FAuditLane> AuditLanes;
    TArray<TSharedPtr<FJsonValue>> LaneRows, GeometryRows, SignalRows, Checks;
    TSharedPtr<FJsonObject> Report;
    FMAYADeliverySnapshot Initial;
    FTransform CapturedChassis;
    FVector InitialRobotCm;
    double LowestBodyOffset=0, HalfWidthCm=0;
    uint64 InitialReset=0;
    FString Output;
    double Started=-1, StableSince=-1, SnapshotStarted=-1, NextSignal=-1;
    int32 NodeCursor=0, PointCount=0, SignalCount=0, SafeMassReads=0, QueryCount=0;
    bool bStopped=false, bGraphCaptured=false, bNativeCaptured=false, bTruncated=false;
    bool Check(bool bPassed, const FString& Name);
    bool CaptureNativeClosedBodies();
    bool CaptureGraph();
    bool ScheduleGeometry();
    bool SnapshotSignals();
    void AuditNode(int32 Index);
    bool InvariantsHold() const;
    void Finish(bool bSuccess, const FString& Reason);
};
