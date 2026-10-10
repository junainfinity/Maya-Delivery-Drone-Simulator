#include "MAYACityBlockRouteSurvey.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYAV01NativeTestDriver.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassExecutionContext.h"
#include "MassTrafficFragments.h"
#include "MassTrafficSettings.h"
#include "MassCrowdSubsystem.h"
#include "MassCrowdSettings.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneGraphData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"

namespace MayaBlockSurvey
{
constexpr int32 MaxLanes=2048, MaxPoints=32768, MaxNodes=1024, MaxSignals=31;
constexpr double MaximumSeconds=120, SignalSeconds=5, SampleSpacingCm=100;
const FBox Area(FVector(-30500,-1500,-500),FVector(-22500,9000,1000));
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{ TArray<TSharedPtr<FJsonValue>> A; for(double C:{V.X,V.Y,V.Z}) A.Add(FMath::IsFinite(C)?TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(C)):TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())); return A; }
TSharedRef<FJsonObject> Handle(FZoneGraphLaneHandle H)
{ auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("dataIndex"),H.DataHandle.Index);J->SetNumberField(TEXT("dataGeneration"),H.DataHandle.Generation);J->SetNumberField(TEXT("laneIndex"),H.Index);return J; }
TSharedRef<FJsonObject> PlayerStreaming(APlayerController* P)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("controller"),GetPathNameSafe(P));J->SetBoolField(TEXT("providerKnown"),P!=nullptr);
    TArray<FWorldPartitionStreamingSource> Sources;const bool Available=P&&P->GetStreamingSources(Sources);J->SetBoolField(TEXT("actualProviderEnabled"),P&&P->IsStreamingSourceEnabled());J->SetBoolField(TEXT("actualSourcesReturned"),Available);
    TArray<TSharedPtr<FJsonValue>> Rows;for(const auto& S:Sources){auto K=MakeShared<FJsonObject>();K->SetStringField(TEXT("name"),S.Name.ToString());K->SetArrayField(TEXT("locationCm"),Vec(S.Location));K->SetArrayField(TEXT("rotationDegrees"),Vec(S.Rotation.Euler()));K->SetNumberField(TEXT("targetState"),int32(S.TargetState));K->SetBoolField(TEXT("blockOnSlowLoading"),S.bBlockOnSlowLoading);K->SetBoolField(TEXT("emptyShapesUseGridLoadingRange"),S.Shapes.IsEmpty());
        TArray<TSharedPtr<FJsonValue>> Shapes;for(const auto& Shape:S.Shapes){auto Q=MakeShared<FJsonObject>();Q->SetBoolField(TEXT("useGridLoadingRange"),Shape.bUseGridLoadingRange);Q->SetNumberField(TEXT("loadingRangeScale"),Shape.LoadingRangeScale);Q->SetNumberField(TEXT("radiusCm"),Shape.Radius);Q->SetArrayField(TEXT("relativeLocationCm"),Vec(Shape.Location));Q->SetBoolField(TEXT("sector"),Shape.bIsSector);Q->SetNumberField(TEXT("sectorAngle"),Shape.SectorAngle);Shapes.Add(MakeShared<FJsonValueObject>(Q));}K->SetArrayField(TEXT("actualShapes"),Shapes);Rows.Add(MakeShared<FJsonValueObject>(K));}
    J->SetArrayField(TEXT("actualStreamingSources"),Rows);J->SetStringField(TEXT("scope"),TEXT("Read-only current PlayerController provider output; installed default follows GetPlayerViewPoint. This is distinct from the probe-owned temporary source and does not prove future route loading."));return J;
}
bool Inside(const FVector& P)
{return !P.ContainsNaN()&&P.X>=Area.Min.X&&P.X<=Area.Max.X&&P.Y>=Area.Min.Y&&P.Y<=Area.Max.Y;}
double CorridorDistance(const FVector& P)
{
    const FVector Anchors[]={FVector(-29898.725,-100.744,0),FVector(-26916,157,0),FVector(-24016,157,0),FVector(-24016,6260,0),FVector(-24016,8360,0),FVector(-23016,8360,0)};
    double Distance=MAX_dbl;for(int32 I=0;I<5;++I){const FVector Closest=FMath::ClosestPointOnSegment(FVector(P.X,P.Y,0),Anchors[I],Anchors[I+1]);Distance=FMath::Min(Distance,FVector::Dist2D(P,Closest));}return Distance;
}
TSharedRef<FJsonObject> Hit(const FHitResult& H,bool bHit)
{
    auto J=MakeShared<FJsonObject>();auto* C=H.GetComponent();auto* I=Cast<UInstancedStaticMeshComponent>(C);
    FBodyInstance* B=C&&(!I||I->IsValidInstance(H.Item))?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
    J->SetBoolField(TEXT("hit"),bHit);J->SetBoolField(TEXT("blocking"),H.bBlockingHit);J->SetBoolField(TEXT("startPenetrating"),H.bStartPenetrating);
    J->SetStringField(TEXT("actor"),GetPathNameSafe(H.GetActor()));J->SetStringField(TEXT("component"),GetPathNameSafe(C));
    J->SetStringField(TEXT("componentClass"),C?C->GetClass()->GetPathName():FString());J->SetNumberField(TEXT("instanceIndex"),H.Item);
    J->SetStringField(TEXT("bone"),H.BoneName.ToString());J->SetBoolField(TEXT("actualHitInstanceBodyValid"),B&&B->IsValidBodyInstance());
    J->SetNumberField(TEXT("mobility"),C?int32(C->Mobility):-1);J->SetNumberField(TEXT("collisionEnabled"),C?int32(C->GetCollisionEnabled()):-1);
    J->SetNumberField(TEXT("physicsBodyResponse"),C?int32(C->GetCollisionResponseToChannel(ECC_PhysicsBody)):-1);
    J->SetStringField(TEXT("physicalMaterial"),GetPathNameSafe(H.PhysMaterial.Get()));
    if(auto* M=Cast<UStaticMeshComponent>(C)){J->SetStringField(TEXT("staticMesh"),GetPathNameSafe(M->GetStaticMesh()));J->SetStringField(TEXT("assignedMaterial0"),GetPathNameSafe(M->GetMaterial(0)));}
    if(bHit){J->SetArrayField(TEXT("impactCm"),Vec(H.ImpactPoint));J->SetArrayField(TEXT("normal"),Vec(H.ImpactNormal));J->SetNumberField(TEXT("time"),H.Time);}
    return J;
}
bool PhysicalSurface(const FHitResult& H,bool bHit)
{
    auto* C=H.GetComponent();auto* I=Cast<UInstancedStaticMeshComponent>(C);
    const FBodyInstance* B=C&&(!I||I->IsValidInstance(H.Item))?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
    return bHit&&H.bBlockingHit&&!H.bStartPenetrating&&!H.ImpactPoint.ContainsNaN()&&!H.ImpactNormal.ContainsNaN()
        &&H.ImpactNormal.Z>=.7&&C&&C->Mobility==EComponentMobility::Static&&C->IsPhysicsCollisionEnabled()
        &&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block&&B&&B->IsValidBodyInstance();
}
}

AMAYACityBlockRouteSurvey::AMAYACityBlockRouteSurvey()
{
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PostUpdateWork;PrimaryActorTick.TickInterval=.05f;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("ReadonlySurveyAnchor")));
    SurveyStreaming=CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("ReadonlyBlockStreaming"));
    FStreamingSourceShape Shape;Shape.bUseGridLoadingRange=false;Shape.Radius=8000;SurveyStreaming->Shapes.Add(Shape);
    SurveyStreaming->DisableStreamingSource();
    IntersectionQuery.AddRequirement<FMassTrafficIntersectionFragment>(EMassFragmentAccess::ReadOnly);
}
void AMAYACityBlockRouteSurvey::Configure(AMAYANativeDemoPawn* R,AMAYADeliveryDirector* D){Robot=R;Director=D;}
bool AMAYACityBlockRouteSurvey::Check(bool bPassed,const FString& Name)
{auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),Name);J->SetBoolField(TEXT("passed"),bPassed);Checks.Add(MakeShared<FJsonValueObject>(J));return bPassed;}
void AMAYACityBlockRouteSurvey::BeginPlay()
{
    Super::BeginPlay();
#if UE_BUILD_SHIPPING || UE_BUILD_TEST || !WITH_EDITOR
    SetActorTickEnabled(false);return;
#endif
    Started=GetWorld()->GetTimeSeconds();Report=MakeShared<FJsonObject>();
    if(!FParse::Param(FCommandLine::Get(),TEXT("MayaV01BlockRouteSurvey"))||!FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest"))
        ||!FParse::Value(FCommandLine::Get(),TEXT("MayaV01Output="),Output)||Output.IsEmpty())
    {SetActorTickEnabled(false);return;}
    Output=FPaths::ConvertRelativePathToFull(Output);
    if(IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("result.json")))||IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("block-survey.json"))))
    {bStopped=true;SetActorTickEnabled(false);UE_LOG(LogTemp,Error,TEXT("MAYA BLOCK SURVEY refused existing sealed output"));FPlatformMisc::RequestExit(false,TEXT("MAYACityBlockRouteSurvey"));return;}
    IFileManager::Get().MakeDirectory(*Output,true);
    const TCHAR* Forbidden[]={TEXT("MayaV01CargoTest"),TEXT("MayaV01CurbTest"),TEXT("MayaV01TrafficEncounterTest"),TEXT("MayaV01OperatingTest"),TEXT("MayaV01DamageTest"),TEXT("MayaV01FullInput"),TEXT("MayaV01ReverseSteerTest"),TEXT("MayaV01CashShiftTest"),TEXT("MayaV01UIScreenTest"),TEXT("MayaV01ObserveInput"),TEXT("MayaV01NewShiftReloadTest"),TEXT("MayaV01RingInteractionTest"),TEXT("MayaV01BoostTest"),TEXT("MayaV01VisualCapture"),TEXT("MayaV01CancelOpeningTest"),TEXT("MayaV01CargoVideo"),TEXT("MayaV01AirLoweringTest"),TEXT("MayaV01DamageCalibration"),TEXT("MayaV01PackageUIScreenSmoke"),TEXT("nullrhi")};
    bool Exclusive=true;for(const TCHAR* F:Forbidden)Exclusive=Exclusive&&!FParse::Param(FCommandLine::Get(),F);
    FString Fixture;Exclusive=Exclusive&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01InitialBattery="),Fixture)&&!FParse::Value(FCommandLine::Get(),TEXT("MayaV01CrashFixture="),Fixture);
    FString Expected=FPaths::Combine(Output,TEXT("GameUserSettings.ini"));
    FString Actual=FPaths::ConvertRelativePathToFull(FConfigCacheIni::GetDestIniFilename(TEXT("GameUserSettings"),nullptr,*FPaths::GeneratedConfigDir()));
    FPaths::NormalizeFilename(Expected);FPaths::NormalizeFilename(Actual);
    Report->SetStringField(TEXT("settingsCacheKey"),GGameUserSettingsIni);Report->SetStringField(TEXT("resolvedSettingsDestination"),Actual);
    Report->SetStringField(TEXT("map"),GetWorld()->GetMapName());Report->SetStringField(TEXT("gameMode"),GetPathNameSafe(GetWorld()->GetAuthGameMode()));
    Report->SetArrayField(TEXT("searchMinCm"),MayaBlockSurvey::Vec(MayaBlockSurvey::Area.Min));Report->SetArrayField(TEXT("searchMaxCm"),MayaBlockSurvey::Vec(MayaBlockSurvey::Area.Max));
    if(!Check(Exclusive,TEXT("Exclusive rendered editor survey without gameplay/resource fixtures"))
        ||!Check(GConfig&&GConfig->FindConfigFile(GGameUserSettingsIni)&&Expected.Equals(Actual,ESearchCase::IgnoreCase),TEXT("Actual settings destination is the isolated attempt file"))
        ||!Check(Robot.IsValid()&&Director.IsValid()&&GetWorld()->GetMapName().Contains(TEXT("Small_City"))&&Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode()),TEXT("Actual Small City/MAYA bootstrap and owned robot/director"))
        ||!Check(!IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("result.json")))&&!IFileManager::Get().FileExists(*FPaths::Combine(Output,TEXT("block-survey.json"))),TEXT("Fresh survey result paths")))
    {Finish(false,TEXT("Read-only probe initialization invariant failed"));return;}
    for(TActorIterator<AMAYAV01NativeTestDriver> It(GetWorld());It;++It){Check(false,TEXT("No normal native test driver spawned"));Finish(false,TEXT("Competing native driver exists"));return;}
    Check(true,TEXT("No normal native test driver spawned"));
    Initial=Director->Snapshot();InitialRobotCm=Director->Observation().ChassisPose.GetLocation();
    TArray<TSharedPtr<FJsonValue>> ShortRoute;for(const auto& P:Director->GetMeasuredRoute())ShortRoute.Add(MakeShared<FJsonValueArray>(MayaBlockSurvey::Vec(P)));Report->SetArrayField(TEXT("existingMeasuredShortRouteCm"),ShortRoute);
    TArray<TSharedPtr<FJsonValue>> Anchors;for(const FVector& P:{FVector(-29898.725,-100.744,0),FVector(-26916,157,0),FVector(-24016,157,0),FVector(-24016,6260,0),FVector(-24016,8360,0),FVector(-23016,8360,0)})Anchors.Add(MakeShared<FJsonValueArray>(MayaBlockSurvey::Vec(P)));Report->SetArrayField(TEXT("prospectiveSearchAnchorPolylineCm"),Anchors);
    Report->SetStringField(TEXT("anchorScope"),TEXT("Source-plan search anchors only; graph membership, physical accessibility and service positions are not certified by these coordinates."));
    Report->SetObjectField(TEXT("actualPlayerStreamingBeforeSurvey"),MayaBlockSurvey::PlayerStreaming(Cast<APlayerController>(Robot->GetController())));
    GetRootComponent()->SetWorldLocation(FVector(-26500,3500,73));SurveyStreaming->EnableStreamingSource();
    Report->SetArrayField(TEXT("streamingAnchorCm"),MayaBlockSurvey::Vec(GetActorLocation()));Report->SetNumberField(TEXT("streamingRadiusCm"),8000);
}

bool AMAYACityBlockRouteSurvey::CaptureNativeClosedBodies()
{
    FMAYADeliveryRobotObservation O;if(!Robot->ReadDeliveryState(O)||!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked())return false;
    TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);
    TArray<UPrimitiveComponent*> Independent;TArray<FPhysicsActorHandle> Actors;
    for(auto* C:Components)if(C&&C->IsPhysicsCollisionEnabled())
    {auto* B=C->GetBodyInstance();if(B&&B==C->GetBodyInstance(NAME_None,false)&&B->IsValidBodyInstance()){Independent.Add(C);Actors.Add(B->GetPhysicsActorHandle());}}
    if(Actors.Num()!=9||!GetWorld()->GetPhysicsScene())return false;
    CapturedChassis=O.ChassisPose;InitialReset=O.ResetGeneration;InitialRobotCm=CapturedChassis.GetLocation();
    bool Valid=true;FBox Whole(ForceInit);TArray<FBodyProxy> Captured;
    const bool Read=FPhysicsCommand::ExecuteRead(GetWorld()->GetPhysicsScene(),[&]()
    {
        for(int32 I=0;I<Actors.Num();++I)
        {const FBox B=FPhysicsInterface::GetBounds_AssumesLocked(Actors[I]);
            if(!B.IsValid||B.Min.ContainsNaN()||B.Max.ContainsNaN()||B.GetExtent().GetMin()<=0){Valid=false;return;}
            FBodyProxy P;P.Offset=B.GetCenter()-CapturedChassis.GetLocation();P.Extent=B.GetExtent();P.Component=Independent[I]->GetPathName();Captured.Add(P);Whole+=B;}
    });
    if(!Read||!Valid)return false;Proxies=MoveTemp(Captured);LowestBodyOffset=Whole.Min.Z-CapturedChassis.GetLocation().Z;
    const FVector Right=-CapturedChassis.GetUnitAxis(EAxis::Y);HalfWidthCm=0;
    for(int32 I=0;I<8;++I){const FVector P((I&1)?Whole.Max.X:Whole.Min.X,(I&2)?Whole.Max.Y:Whole.Min.Y,(I&4)?Whole.Max.Z:Whole.Min.Z);HalfWidthCm=FMath::Max(HalfWidthCm,FMath::Abs(FVector::DotProduct(P-CapturedChassis.GetLocation(),Right)));}
    auto Native=MakeShared<FJsonObject>();Native->SetArrayField(TEXT("chassisCm"),MayaBlockSurvey::Vec(InitialRobotCm));Native->SetArrayField(TEXT("boundsMinCm"),MayaBlockSurvey::Vec(Whole.Min));Native->SetArrayField(TEXT("boundsMaxCm"),MayaBlockSurvey::Vec(Whole.Max));Native->SetNumberField(TEXT("resetGeneration"),double(InitialReset));Native->SetNumberField(TEXT("halfWidthCm"),HalfWidthCm);
    TArray<TSharedPtr<FJsonValue>> Bodies;for(const auto& P:Proxies){auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("component"),P.Component);J->SetArrayField(TEXT("worldAabbOffsetCm"),MayaBlockSurvey::Vec(P.Offset));J->SetArrayField(TEXT("worldAabbExtentCm"),MayaBlockSurvey::Vec(P.Extent));Bodies.Add(MakeShared<FJsonValueObject>(J));}
    Native->SetArrayField(TEXT("nineNativeBodyAabbProxies"),Bodies);Report->SetObjectField(TEXT("closedNativeRobotCapture"),Native);return true;
}

bool AMAYACityBlockRouteSurvey::CaptureGraph()
{
    auto* Z=GetWorld()->GetSubsystem<UZoneGraphSubsystem>();if(!Z)return false;
    const auto* Traffic=GetDefault<UMassTrafficSettings>();const auto* Crowd=GetDefault<UMassCrowdSettings>();
    auto Filters=MakeShared<FJsonObject>();Filters->SetNumberField(TEXT("crosswalkAnyTags"),Traffic->CrosswalkLaneFilter.AnyTags.GetValue());Filters->SetNumberField(TEXT("crosswalkAllTags"),Traffic->CrosswalkLaneFilter.AllTags.GetValue());Filters->SetNumberField(TEXT("crosswalkNotTags"),Traffic->CrosswalkLaneFilter.NotTags.GetValue());
    Filters->SetNumberField(TEXT("crowdTagBit"),Crowd->CrowdTag.Get());Filters->SetNumberField(TEXT("crossingTagBit"),Crowd->CrossingTag.Get());Report->SetObjectField(TEXT("actualConfiguredLaneFilters"),Filters);
    TArray<TSharedPtr<FJsonValue>> LaneTagBits;for(const auto& T:Z->GetTagInfos()){auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),T.Name.ToString());J->SetNumberField(TEXT("bit"),T.Tag.Get());LaneTagBits.Add(MakeShared<FJsonValueObject>(J));}Report->SetArrayField(TEXT("actualTagNames"),LaneTagBits);
    int32 ScannedLanes=0;
    TArray<TSharedPtr<FJsonValue>> GraphRows,ZoneRows;int32 RegisteredIndex=-1,BoundaryCount=0,InvalidLaneRanges=0;
    for(const auto& Registered:Z->GetRegisteredZoneGraphData())
    {
        ++RegisteredIndex;
        auto* A=Registered.ZoneGraphData.Get();if(!Registered.bInUse||!IsValid(A)||!A->IsRegistered())continue;
        FScopeLock Lock(&A->GetStorageLock());const auto& S=A->GetStorage();if(!S.DataHandle.IsValid()||S.DataHandle.Index!=RegisteredIndex||int32(S.DataHandle.Generation)!=Registered.Generation||!S.Bounds.Intersect(MayaBlockSurvey::Area))continue;
        FGraphBinding G;G.Actor=A;G.Handle=S.DataHandle;G.LaneCount=S.Lanes.Num();Graphs.Add(G);
        auto Graph=MayaBlockSurvey::Handle(FZoneGraphLaneHandle(INDEX_NONE,S.DataHandle));Graph->SetStringField(TEXT("actor"),A->GetPathName());Graph->SetStringField(TEXT("currentOuterPackage"),GetPathNameSafe(A->GetOutermost()));Graph->SetNumberField(TEXT("actualLaneCount"),S.Lanes.Num());Graph->SetNumberField(TEXT("actualZoneCount"),S.Zones.Num());Graph->SetArrayField(TEXT("boundsMinCm"),MayaBlockSurvey::Vec(S.Bounds.Min));Graph->SetArrayField(TEXT("boundsMaxCm"),MayaBlockSurvey::Vec(S.Bounds.Max));GraphRows.Add(MakeShared<FJsonValueObject>(Graph));
        for(int32 Zone=0;Zone<S.Zones.Num();++Zone)
        {
            const auto& D=S.Zones[Zone];if(!D.Bounds.Intersect(MayaBlockSurvey::Area))continue;if(ZoneRows.Num()>=1024){bTruncated=true;break;}
            auto Row=MayaBlockSurvey::Handle(FZoneGraphLaneHandle(INDEX_NONE,S.DataHandle));Row->SetNumberField(TEXT("zoneIndex"),Zone);Row->SetNumberField(TEXT("tagMask"),D.Tags.GetValue());Row->SetNumberField(TEXT("lanesBegin"),D.LanesBegin);Row->SetNumberField(TEXT("lanesEnd"),D.LanesEnd);Row->SetArrayField(TEXT("boundsMinCm"),MayaBlockSurvey::Vec(D.Bounds.Min));Row->SetArrayField(TEXT("boundsMaxCm"),MayaBlockSurvey::Vec(D.Bounds.Max));
            const bool RangeValid=D.BoundaryPointsBegin>=0&&D.BoundaryPointsEnd>=D.BoundaryPointsBegin&&D.BoundaryPointsEnd<=S.BoundaryPoints.Num();Row->SetBoolField(TEXT("boundaryRangeValid"),RangeValid);TArray<TSharedPtr<FJsonValue>> Boundary;bool Partial=false;
            if(RangeValid)for(int32 P=D.BoundaryPointsBegin;P<D.BoundaryPointsEnd;++P){if(Boundary.Num()>=64||BoundaryCount>=4096){Partial=true;bTruncated=true;break;}Boundary.Add(MakeShared<FJsonValueArray>(MayaBlockSurvey::Vec(S.BoundaryPoints[P])));++BoundaryCount;}
            Row->SetArrayField(TEXT("boundaryPointsCm"),Boundary);Row->SetBoolField(TEXT("boundaryTruncated"),Partial);ZoneRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        for(int32 L=0;L<S.Lanes.Num();++L)
        {
            if(++ScannedLanes>65536){bTruncated=true;break;}
            const auto& D=S.Lanes[L];if(D.PointsBegin<0||D.PointsEnd>D.PointsBegin+4096||D.PointsEnd>S.LanePoints.Num()||D.PointsEnd-D.PointsBegin<2||!FMath::IsFinite(D.Width)){++InvalidLaneRanges;continue;}
            bool FinitePoints=true;for(int32 P=D.PointsBegin;P<D.PointsEnd;++P)FinitePoints=FinitePoints&&!S.LanePoints[P].ContainsNaN();if(!FinitePoints){++InvalidLaneRanges;continue;}
            FBox B(ForceInit);for(int32 P=D.PointsBegin;P<D.PointsEnd;++P)B+=S.LanePoints[P];if(!B.Intersect(MayaBlockSurvey::Area))continue;
            if(LaneRows.Num()>=MayaBlockSurvey::MaxLanes||PointCount+D.GetNumPoints()>MayaBlockSurvey::MaxPoints){bTruncated=true;break;}
            const FZoneGraphLaneHandle H(L,S.DataHandle);auto J=MayaBlockSurvey::Handle(H);J->SetStringField(TEXT("graphActor"),A->GetPathName());J->SetNumberField(TEXT("zoneIndex"),D.ZoneIndex);J->SetNumberField(TEXT("widthCm"),D.Width);J->SetNumberField(TEXT("tagMask"),D.Tags.GetValue());
            const bool FilterConfigured=(Traffic->CrosswalkLaneFilter.AnyTags.GetValue()|Traffic->CrosswalkLaneFilter.AllTags.GetValue()|Traffic->CrosswalkLaneFilter.NotTags.GetValue())!=0;
            const bool Crosswalk=FilterConfigured&&Traffic->CrosswalkLaneFilter.Pass(D.Tags);const bool CrowdLane=D.Tags.Contains(Crowd->CrowdTag);
            J->SetBoolField(TEXT("passesConfiguredCrosswalkFilter"),Crosswalk);J->SetBoolField(TEXT("containsConfiguredCrowdTag"),CrowdLane);if(Crosswalk)Crosswalks.Add(H);
            J->SetBoolField(TEXT("crosswalkFilterConfigured"),FilterConfigured);J->SetStringField(TEXT("geometryCoverage"),TEXT("unexamined_non_candidate_lane"));
            TArray<TSharedPtr<FJsonValue>> Points,Links;for(int32 P=D.PointsBegin;P<D.PointsEnd;++P)Points.Add(MakeShared<FJsonValueArray>(MayaBlockSurvey::Vec(S.LanePoints[P])));PointCount+=Points.Num();J->SetArrayField(TEXT("pointsCm"),Points);
            bool LinksValid=D.LinksBegin>=0&&D.LinksEnd>=D.LinksBegin&&D.LinksEnd<=S.LaneLinks.Num();J->SetBoolField(TEXT("linkRangeValid"),LinksValid);
            if(LinksValid)for(int32 P=D.LinksBegin;P<D.LinksEnd&&P<D.LinksBegin+64;++P){const auto& Link=S.LaneLinks[P];auto K=MayaBlockSurvey::Handle(FZoneGraphLaneHandle(Link.DestLaneIndex,S.DataHandle));K->SetNumberField(TEXT("type"),int32(Link.Type));K->SetNumberField(TEXT("flags"),Link.Flags);K->SetBoolField(TEXT("destinationIndexValid"),S.Lanes.IsValidIndex(Link.DestLaneIndex));Links.Add(MakeShared<FJsonValueObject>(K));}J->SetArrayField(TEXT("actualLinks"),Links);J->SetBoolField(TEXT("linksTruncated"),LinksValid&&D.GetLinkCount()>64);
            LaneRows.Add(MakeShared<FJsonValueObject>(J));
            if(Crosswalk||CrowdLane){FAuditLane Audit;Audit.Handle=H;Audit.Row=J;Audit.Priority=MAX_dbl;
                for(int32 P=D.PointsBegin;P<D.PointsEnd;++P){Audit.Points.Add(S.LanePoints[P]);Audit.Priority=FMath::Min(Audit.Priority,MayaBlockSurvey::CorridorDistance(S.LanePoints[P]));
                    if(P+1<D.PointsEnd)Audit.Priority=FMath::Min(Audit.Priority,MayaBlockSurvey::CorridorDistance((S.LanePoints[P]+S.LanePoints[P+1])*.5));}
                AuditLanes.Add(MoveTemp(Audit));}
        }
    }
    Report->SetArrayField(TEXT("registeredGraphSnapshots"),GraphRows);Report->SetArrayField(TEXT("zones"),ZoneRows);Report->SetNumberField(TEXT("invalidOrOver4096PointLaneRangesSkipped"),InvalidLaneRanges);
    Report->SetNumberField(TEXT("scannedRegisteredLanes"),ScannedLanes);Report->SetNumberField(TEXT("maxScannedRegisteredLanes"),65536);
    return LaneRows.Num()>0;
}

bool AMAYACityBlockRouteSurvey::ScheduleGeometry()
{
    // Bounded follow-up to actual115: these are search profiles, not certified waypoints.
    struct FProfile { const TCHAR* Name; int32 ReferenceLane; FVector From, To; double Spacing, OffsetMinimum, OffsetMaximum, OffsetSpacing; };
    const FProfile Profiles[]={
        // Actual140 turned while still on the grade. Query the unsurveyed B crest/apron.
        // Both orientations cover the same99 locations; graph lanes are context only.
        {TEXT("B_crest_north_orientation"),2071,FVector(-24282.059,8075,73),FVector(-24282.059,8275,73),25,-125,125,25},
        {TEXT("B_crest_east_orientation"),9793,FVector(-24407.059,8175,73),FVector(-24157.059,8175,73),25,-100,100,25}
    };
    TArray<TSharedPtr<FJsonValue>> ProfileRows;
    for(auto& Lane:AuditLanes)
    {
        Lane.Row->SetNumberField(TEXT("scheduledGeometryNodes"),0);
        Lane.Row->SetBoolField(TEXT("geometryScheduleTruncated"),false);
        Lane.Row->SetStringField(TEXT("geometryCoverage"),TEXT("metadata_only_in_targeted_followup"));
    }
    for(const auto& Profile:Profiles)
    {
        FAuditLane* Reference=nullptr;
        for(auto& Lane:AuditLanes)if(Lane.Handle.Index==Profile.ReferenceLane&&Lane.Row->GetStringField(TEXT("graphActor")).EndsWith(TEXT("ZoneGraphData_UAID_5C879C879000EEEA00_1280800823")))
        {Reference=&Lane;break;}
        if(!Reference||Reference->Points.Num()<2)return false;
        const FVector Forward=(Profile.To-Profile.From).GetSafeNormal2D(),Right=FVector::CrossProduct(FVector::UpVector,Forward);
        const int32 Steps=FMath::CeilToInt(FVector::Dist(Profile.From,Profile.To)/Profile.Spacing);
        auto Description=MayaBlockSurvey::Handle(Reference->Handle);Description->SetStringField(TEXT("profile"),Profile.Name);
        Description->SetArrayField(TEXT("fromSearchAnchorCm"),MayaBlockSurvey::Vec(Profile.From));Description->SetArrayField(TEXT("toSearchAnchorCm"),MayaBlockSurvey::Vec(Profile.To));
        Description->SetNumberField(TEXT("longitudinalSpacingCm"),Profile.Spacing);Description->SetNumberField(TEXT("offsetMinimumCm"),Profile.OffsetMinimum);Description->SetNumberField(TEXT("offsetMaximumCm"),Profile.OffsetMaximum);Description->SetNumberField(TEXT("offsetSpacingCm"),Profile.OffsetSpacing);
        Description->SetBoolField(TEXT("queryPointsCertifiedOnReferenceLane"),false);ProfileRows.Add(MakeShared<FJsonValueObject>(Description));
        int32 Added=0;
        for(double Offset=Profile.OffsetMinimum;Offset<=Profile.OffsetMaximum;Offset+=Profile.OffsetSpacing)
        {
            const FString Group=FString::Printf(TEXT("%s_offset_%.0f"),Profile.Name,Offset);
            for(int32 K=0;K<=Steps;++K)
            {
                const FVector At=FMath::Lerp(Profile.From,Profile.To,double(K)/Steps)+Right*Offset;
                if(!MayaBlockSurvey::Inside(At)||Nodes.Num()>=MayaBlockSurvey::MaxNodes){bTruncated=true;return false;}
                FNode N;N.Lane=Reference->Handle;N.Point=At;N.Forward=Forward;N.Sequence=K;N.Profile=Group;N.ProfileOffsetCm=Offset;Nodes.Add(N);++Added;
            }
        }
        Reference->Row->SetNumberField(TEXT("scheduledGeometryNodes"),Reference->Row->GetNumberField(TEXT("scheduledGeometryNodes"))+Added);
        Reference->Row->SetStringField(TEXT("geometryCoverage"),TEXT("targeted_search_profiles_reference_this_lane_not_full_lane_audit"));
    }
    Report->SetStringField(TEXT("acquisitionMode"),TEXT("Focused actual B crest and north sidewalk apron after actual140 on-grade turn refusal"));
    Report->SetArrayField(TEXT("targetedSearchProfiles"),ProfileRows);
    Report->SetStringField(TEXT("targetedProfileScope"),TEXT("198 finite query nodes cover99 positions in X[-24407.059,-24157.059] and Y[8075,8275] at25cm spacing, each in north and east proxy orientations. Actual native closed-body aggregate bounds/half-width surface rays and original height/normal/body/component identities are retained. Actual140 completed the west-strip climb and stopped but lost upright during the final turn;116 ended atY8125 and115 upper samples do not establish full-footprint crest/turn clearance. This focused search complements those preserved receipts; it does not certify a level turn, compound-shape traversal or route change. Reference lanes2071 and9793 identify same-graph context only; these off-lane queries are not certified lane membership, route points, traversability or service placement. All current signal/query/node/streaming/120-second caps remain unchanged. No graph/lane/signal/resource/native robot mutation."));
    return Nodes.Num()==198;
}

bool AMAYACityBlockRouteSurvey::SnapshotSignals()
{
    auto* Sub=GetWorld()->GetSubsystem<UMassEntitySubsystem>();if(!Sub)return false;
    auto& Manager=Sub->GetMutableEntityManager();if(Manager.IsProcessing())return false;
    auto Snapshot=MakeShared<FJsonObject>();Snapshot->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());Snapshot->SetNumberField(TEXT("frame"),double(GFrameCounter));Snapshot->SetBoolField(TEXT("massManagerIdleAtRead"),true);
    TArray<TSharedPtr<FJsonValue>> Intersections;bool Truncated=false;
    FMassExecutionContext Context(Manager,0.f);
    IntersectionQuery.ForEachEntityChunk(Manager,Context,[&](FMassExecutionContext& C)
    {
        const auto Fs=C.GetFragmentView<FMassTrafficIntersectionFragment>();const auto Es=C.GetEntities();
        for(int32 I=0;I<C.GetNumEntities();++I)
        {
            const auto& F=Fs[I];bool Relevant=false;
            for(const auto& Light:F.TrafficLights)Relevant=Relevant||MayaBlockSurvey::Inside(Light.Position);
            for(const auto& Period:F.Periods)for(int32 L:Period.CrosswalkLanes)Relevant=Relevant||Crosswalks.Contains(FZoneGraphLaneHandle(L,F.ZoneGraphDataHandle));
            if(!Relevant)continue;if(Intersections.Num()>=128){Truncated=true;continue;}
            auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("entityIndex"),Es[I].Index);J->SetNumberField(TEXT("entitySerial"),Es[I].SerialNumber);
            J->SetNumberField(TEXT("dataIndex"),F.ZoneGraphDataHandle.Index);J->SetNumberField(TEXT("dataGeneration"),F.ZoneGraphDataHandle.Generation);J->SetNumberField(TEXT("zoneIndex"),F.ZoneIndex);J->SetBoolField(TEXT("hasTrafficLights"),F.bHasTrafficLights);
            J->SetNumberField(TEXT("currentPeriodIndex"),F.CurrentPeriodIndex);J->SetBoolField(TEXT("currentPeriodIndexValid"),F.Periods.IsValidIndex(F.CurrentPeriodIndex));J->SetNumberField(TEXT("periodTimeRemainingSeconds"),float(F.PeriodTimeRemaining));
            J->SetNumberField(TEXT("lastPedestrianLaneAction"),int32(F.LastPedestrianLanesActionAppliedToCurrentPeriod));J->SetNumberField(TEXT("lastVehicleLaneAction"),int32(F.LastVehicleLanesActionAppliedToCurrentPeriod));
            TArray<TSharedPtr<FJsonValue>> Periods,Lights;
            for(int32 P=0;P<F.Periods.Num()&&P<32;++P)
            {
                const auto& Period=F.Periods[P];auto K=MakeShared<FJsonObject>();K->SetNumberField(TEXT("index"),P);K->SetNumberField(TEXT("durationSeconds"),float(Period.Duration));
                TArray<TSharedPtr<FJsonValue>> Lanes,Waiting,Controls;
                for(int32 N=0;N<Period.CrosswalkLanes.Num()&&N<128;++N){const FZoneGraphLaneHandle H(Period.CrosswalkLanes[N],F.ZoneGraphDataHandle);auto V=MayaBlockSurvey::Handle(H);V->SetBoolField(TEXT("sameGraphLanePresentInSurvey"),Crosswalks.Contains(H));Lanes.Add(MakeShared<FJsonValueObject>(V));}
                for(int32 N=0;N<Period.CrosswalkWaitingLanes.Num()&&N<128;++N)Waiting.Add(MakeShared<FJsonValueObject>(MayaBlockSurvey::Handle(FZoneGraphLaneHandle(Period.CrosswalkWaitingLanes[N],F.ZoneGraphDataHandle))));
                for(int32 N=0;N<Period.TrafficLightControls.Num()&&N<128;++N){const auto& Light=Period.TrafficLightControls[N];auto V=MakeShared<FJsonObject>();V->SetNumberField(TEXT("lightIndex"),N);V->SetBoolField(TEXT("valid"),Light.bIsValid);V->SetNumberField(TEXT("stateFlags"),int32(Light.TrafficLightStateFlags));V->SetBoolField(TEXT("vehicleLanesCloseNextPeriod"),Light.bWillAllVehicleLanesCloseInNextPeriodForThisTrafficLight);Controls.Add(MakeShared<FJsonValueObject>(V));}
                K->SetArrayField(TEXT("crosswalkLanes"),Lanes);K->SetArrayField(TEXT("waitingLanes"),Waiting);K->SetArrayField(TEXT("lightControls"),Controls);
                K->SetBoolField(TEXT("membershipTruncated"),Period.CrosswalkLanes.Num()>128||Period.CrosswalkWaitingLanes.Num()>128||Period.TrafficLightControls.Num()>128);Periods.Add(MakeShared<FJsonValueObject>(K));
            }
            for(int32 N=0;N<F.TrafficLights.Num()&&N<128;++N){const auto& Light=F.TrafficLights[N];auto K=MakeShared<FJsonObject>();K->SetNumberField(TEXT("index"),N);K->SetArrayField(TEXT("positionCm"),MayaBlockSurvey::Vec(Light.Position));K->SetNumberField(TEXT("zRotation"),Light.ZRotation);K->SetNumberField(TEXT("typeIndex"),Light.TrafficLightTypeIndex);K->SetNumberField(TEXT("currentStateFlags"),int32(Light.TrafficLightStateFlags));Lights.Add(MakeShared<FJsonValueObject>(K));}
            J->SetArrayField(TEXT("periods"),Periods);J->SetArrayField(TEXT("lights"),Lights);J->SetBoolField(TEXT("arraysTruncated"),F.Periods.Num()>32||F.TrafficLights.Num()>128);Intersections.Add(MakeShared<FJsonValueObject>(J));
        }
    });
    auto* Crowd=GetWorld()->GetSubsystem<UMassCrowdSubsystem>();TArray<TSharedPtr<FJsonValue>> States;
    for(const auto H:Crosswalks)
    {
        auto J=MayaBlockSurvey::Handle(H);bool Known=false;
        bool CurrentGraph=false;for(const auto& G:Graphs)if(G.Handle==H.DataHandle&&G.Actor.IsValid()&&G.Actor->IsRegistered())
        {FScopeLock Lock(&G.Actor->GetStorageLock());const auto& S=G.Actor->GetStorage();CurrentGraph=S.DataHandle==H.DataHandle&&S.Lanes.IsValidIndex(H.Index);break;}
        if(CurrentGraph&&Crowd&&Crowd->HasCrowdDataForZoneGraph(H.DataHandle))
        {const auto* Data=Crowd->GetCrowdData(H.DataHandle);Known=Data&&Data->CrowdLaneDataArray.IsValidIndex(H.Index)&&Crowd->GetCrowdLaneData(H).IsSet();if(Known)J->SetStringField(TEXT("actualCrowdState"),Crowd->GetLaneState(H)==ECrowdLaneState::Opened?TEXT("Opened"):TEXT("Closed"));}
        J->SetBoolField(TEXT("actualCrowdLaneStateKnown"),Known);if(!Known)J->SetStringField(TEXT("actualCrowdState"),TEXT("unknown_missing_validated_data"));States.Add(MakeShared<FJsonValueObject>(J));
    }
    Snapshot->SetArrayField(TEXT("intersections"),Intersections);Snapshot->SetArrayField(TEXT("crosswalkCrowdStates"),States);Snapshot->SetBoolField(TEXT("intersectionRowsTruncated"),Truncated);
    Snapshot->SetStringField(TEXT("signalScope"),TEXT("Current native period/light and validated same-graph lane states at this frame. Five seconds of targeted-followup snapshots do not cover a full cycle; no crossing clearance or green-duration guarantee."));
    SignalRows.Add(MakeShared<FJsonValueObject>(Snapshot));++SafeMassReads;++SignalCount;return true;
}

void AMAYACityBlockRouteSurvey::AuditNode(int32 Index)
{
    const auto& N=Nodes[Index];auto J=MayaBlockSurvey::Handle(N.Lane);J->SetNumberField(TEXT("sequence"),N.Sequence);J->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());J->SetArrayField(TEXT("graphPointCm"),MayaBlockSurvey::Vec(N.Point));J->SetArrayField(TEXT("travelForward"),MayaBlockSurvey::Vec(N.Forward));J->SetStringField(TEXT("targetedProfile"),N.Profile);J->SetNumberField(TEXT("profileLateralOffsetCm"),N.ProfileOffsetCm);J->SetBoolField(TEXT("pointCertifiedOnReferenceGraphLane"),false);J->SetStringField(TEXT("queryPointScope"),TEXT("Fixed targeted search location; lane handle is crossing context, not validated point membership."));
    const FVector Right=FVector::CrossProduct(FVector::UpVector,N.Forward).GetSafeNormal();
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYABlockReadonlySurface),false,Robot.Get());Q.AddIgnoredActor(this);Q.bReturnPhysicalMaterial=true;Q.MobilityType=EQueryMobilityType::Static;
    TArray<TSharedPtr<FJsonValue>> Rays;FHitResult Center;bool CenterPhysical=false;
    for(double Offset:{0.,-HalfWidthCm,HalfWidthCm})
    {
        const FVector At=N.Point+Right*Offset;FHitResult H;const bool Found=GetWorld()->LineTraceSingleByChannel(H,At+FVector(0,0,250),At-FVector(0,0,350),ECC_Visibility,Q);++QueryCount;
        const bool Physical=MayaBlockSurvey::PhysicalSurface(H,Found);auto Ray=MayaBlockSurvey::Hit(H,Found);Ray->SetNumberField(TEXT("lateralOffsetCm"),Offset);Ray->SetBoolField(TEXT("staticPhysicalUpSurface"),Physical);Rays.Add(MakeShared<FJsonValueObject>(Ray));
        if(Offset==0){Center=H;CenterPhysical=Physical;}
    }
    J->SetArrayField(TEXT("centerAndNativeWidthEdgeSurfaceRays"),Rays);J->SetBoolField(TEXT("centerGroundKnown"),CenterPhysical);
    const bool LinkedNext=Index+1<Nodes.Num()&&Nodes[Index+1].Lane==N.Lane&&Nodes[Index+1].Profile==N.Profile&&Nodes[Index+1].Sequence==N.Sequence+1;
    J->SetBoolField(TEXT("adjacentSameLaneSampleExists"),LinkedNext);
    TArray<TSharedPtr<FJsonValue>> Sweeps;bool SweepKnown=false,Clear=true;
    if(LinkedNext&&CenterPhysical&&!N.Forward.IsNearlyZero())
    {
        const auto& Next=Nodes[Index+1];FHitResult EndGround;const bool EndFound=GetWorld()->LineTraceSingleByChannel(EndGround,Next.Point+FVector(0,0,250),Next.Point-FVector(0,0,350),ECC_Visibility,Q);++QueryCount;
        const bool EndPhysical=MayaBlockSurvey::PhysicalSurface(EndGround,EndFound);J->SetObjectField(TEXT("nextCenterSurface"),MayaBlockSurvey::Hit(EndGround,EndFound));
        if(EndPhysical)
        {
            const FVector Start(Center.ImpactPoint.X,Center.ImpactPoint.Y,Center.ImpactPoint.Z-LowestBodyOffset+1),End(EndGround.ImpactPoint.X,EndGround.ImpactPoint.Y,EndGround.ImpactPoint.Z-LowestBodyOffset+1);
            const FQuat Rotation=(-N.Forward).Rotation().Quaternion()*CapturedChassis.GetRotation().Inverse();SweepKnown=true;
            J->SetNumberField(TEXT("centerSurfaceHeightChangeCm"),EndGround.ImpactPoint.Z-Center.ImpactPoint.Z);J->SetArrayField(TEXT("queryChassisStartCm"),MayaBlockSurvey::Vec(Start));J->SetArrayField(TEXT("queryChassisEndCm"),MayaBlockSurvey::Vec(End));
            J->SetArrayField(TEXT("aabbRotationEulerDegrees"),MayaBlockSurvey::Vec(Rotation.Rotator().Euler()));
            FCollisionQueryParams SweepQ(SCENE_QUERY_STAT(MAYABlockReadonlyClosedProxy),false,Robot.Get());SweepQ.AddIgnoredActor(this);SweepQ.bReturnPhysicalMaterial=true;
            for(const auto& P:Proxies)
            {
                FHitResult H;const FVector Offset=Rotation.RotateVector(P.Offset);const bool Hit=GetWorld()->SweepSingleByChannel(H,Start+Offset,End+Offset,Rotation,ECC_PhysicsBody,FCollisionShape::MakeBox(P.Extent),SweepQ);++QueryCount;
                auto K=MayaBlockSurvey::Hit(H,Hit);K->SetStringField(TEXT("nativeSourceBody"),P.Component);K->SetArrayField(TEXT("proxyHalfExtentCm"),MayaBlockSurvey::Vec(P.Extent));K->SetBoolField(TEXT("proxyClear"),!Hit&&!H.bStartPenetrating);Sweeps.Add(MakeShared<FJsonValueObject>(K));Clear=Clear&&!Hit&&!H.bStartPenetrating;
            }
        }
    }
    J->SetBoolField(TEXT("closedNativeAabbProxySweepKnown"),SweepKnown);J->SetBoolField(TEXT("closedNativeAabbProxySweepClear"),SweepKnown&&Clear);J->SetArrayField(TEXT("nineBodyProxySweeps"),Sweeps);
    J->SetStringField(TEXT("geometryScope"),TEXT("First static Visibility surface at center and captured closed-native half-width edges, preserving native hit Item/body/props. Adjacent same-lane nodes use nine native world-AABB boxes rotated by the proposed chassis heading; the captured aggregate lowest-body offset is placed1cm above sampled ground, preserving each body offset, and proxies sweep along the interpolated surface. This is a conservative proxy query, not exact compound-shape traversal, ramp/curb capability, loading support, signal clearance or route PASS. Other actors remain blockers; only exact survey/robot self actors ignored."));
    GeometryRows.Add(MakeShared<FJsonValueObject>(J));
}

bool AMAYACityBlockRouteSurvey::InvariantsHold() const
{
    if(!Robot.IsValid()||!Director.IsValid()||Robot->GetWorld()!=GetWorld()||Director->GetWorld()!=GetWorld())return false;
    const auto& S=Director->Snapshot();FMAYADeliveryRobotObservation O;if(!Robot->ReadDeliveryState(O))return false;
    return S.Phase==EMAYADeliveryPhase::Available&&S.JobInstanceId==Initial.JobInstanceId&&S.ShiftInstanceId==Initial.ShiftInstanceId
        &&S.Service.Generation==Initial.Service.Generation&&S.TotalCredits==Initial.TotalCredits&&S.RechargeFeesPaidDollars==Initial.RechargeFeesPaidDollars
        &&S.CompletedJobs==Initial.CompletedJobs&&S.Recoveries==Initial.Recoveries&&S.CargoConditionPercent==Initial.CargoConditionPercent
        &&!S.bDroneTotalled&&!S.bVehicleContactPending&&Director->GetParcelCount()==0&&!Robot->HasFreshMovementIntent()
        &&(!bNativeCaptured||(O.ResetGeneration==InitialReset&&FVector::Dist(O.ChassisPose.GetLocation(),InitialRobotCm)<=20));
}

void AMAYACityBlockRouteSurvey::Tick(float Dt)
{
    Super::Tick(Dt);if(bStopped||Started<0||!Report.IsValid())return;
    const double Now=GetWorld()->GetTimeSeconds();
    if(Now-Started>MayaBlockSurvey::MaximumSeconds){Finish(false,TEXT("Bounded120-gameplay-second survey timed out; partial observations retained"));return;}
    if(!InvariantsHold()){Check(false,TEXT("Read-only job/cash/cargo/reset/no-movement invariants remained intact"));Finish(false,TEXT("Actual gameplay/input/native identity changed during read-only survey"));return;}
    if(!bNativeCaptured)
    {
        FMAYADeliveryRobotObservation O;const bool Quiet=Robot->ReadDeliveryState(O)&&O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();
        if(!Quiet)StableSince=-1;else if(StableSince<0)StableSince=Now;
        if(StableSince>=0&&Now-StableSince>=.5){bNativeCaptured=CaptureNativeClosedBodies();if(!bNativeCaptured){Finish(false,TEXT("Actual nine closed native bodies could not be captured"));return;}}
        if(!bNativeCaptured&&Now-Started>20){Finish(false,TEXT("Actual quiet closed baseline was not observed within20s"));return;}
    }
    if(!bNativeCaptured||!SurveyStreaming->IsStreamingCompleted())return;
    if(!bGraphCaptured)
    {
        if(!CaptureGraph()){Check(false,TEXT("Registered bounded-area lane graph captured"));Finish(false,TEXT("No registered lane graph in streamed survey area"));return;}
        bGraphCaptured=true;
        if(!Check(ScheduleGeometry(),TEXT("All198 focused B crest/apron north/east profile nodes scheduled against the actual reference graph without truncation")))
        {Finish(false,TEXT("Actual expected crossing graph/profile bounds unavailable; no route points certified"));return;}
        SnapshotStarted=Now;NextSignal=Now;Report->SetBoolField(TEXT("actualProbeStreamingCompleteAtAcquisition"),true);
        Check(true,TEXT("Actual streaming completion and registered bounded-area graph snapshot"));
    }
    for(int32 I=0;I<4&&NodeCursor<Nodes.Num();++I)AuditNode(NodeCursor++);
    if(Now>=NextSignal&&SignalCount<MayaBlockSurvey::MaxSignals&&SnapshotSignals())NextSignal=Now+1;
    if(NodeCursor>=Nodes.Num()&&Now-SnapshotStarted>=MayaBlockSurvey::SignalSeconds&&SafeMassReads>0)
    {
        Check(true,TEXT("Bounded graph/geometry queues completed; failures and truncation remain explicit observations"));Check(true,TEXT("Current native intersections read only while Mass manager was idle"));
        const bool Intact=Check(InvariantsHold(),TEXT("Read-only job/cash/cargo/reset/no-movement invariants remained intact"));Finish(Intact,TEXT("Bounded read-only survey export completed; route viability and signal clearance remain unverified"));
    }
}

void AMAYACityBlockRouteSurvey::Finish(bool bSuccess,const FString& Reason)
{
    if(bStopped)return;bStopped=true;SetActorTickEnabled(false);SurveyStreaming->DisableStreamingSource();if(!Report.IsValid())return;
    Report->SetStringField(TEXT("schema"),TEXT("MayaRobotics.NativeBlockRouteSurvey.v1"));Report->SetStringField(TEXT("phase"),TEXT("NativeBlockRouteSurvey"));Report->SetBoolField(TEXT("terminal"),true);Report->SetBoolField(TEXT("success"),bSuccess);Report->SetStringField(TEXT("reason"),Reason);
    Report->SetBoolField(TEXT("routeViabilityVerified"),false);Report->SetBoolField(TEXT("loadedTraversalVerified"),false);Report->SetBoolField(TEXT("fullSignalCycleVerified"),false);Report->SetBoolField(TEXT("humanPhysicalInputVerified"),false);
    Report->SetStringField(TEXT("scope"),TEXT("Development editor read-only streamed native graph/crossing/surface export; success means acquisition/export invariants only. No mission, traversal, crossing-clearance or ordinary-product QA PASS."));
    Report->SetNumberField(TEXT("startedWorldSeconds"),Started);Report->SetNumberField(TEXT("endWorldSeconds"),GetWorld()->GetTimeSeconds());Report->SetNumberField(TEXT("maximumGameplaySeconds"),120);
    Report->SetNumberField(TEXT("signalObservationTargetSeconds"),MayaBlockSurvey::SignalSeconds);Report->SetNumberField(TEXT("signalSnapshotCount"),SignalCount);Report->SetNumberField(TEXT("safeMassReads"),SafeMassReads);
    Report->SetNumberField(TEXT("laneCount"),LaneRows.Num());Report->SetNumberField(TEXT("copiedPointCount"),PointCount);Report->SetNumberField(TEXT("scheduledGeometryNodes"),Nodes.Num());Report->SetNumberField(TEXT("auditedGeometryNodes"),GeometryRows.Num());Report->SetNumberField(TEXT("collisionQueryCount"),QueryCount);
    Report->SetBoolField(TEXT("boundedExportTruncated"),bTruncated);Report->SetNumberField(TEXT("maxExportLanes"),MayaBlockSurvey::MaxLanes);Report->SetNumberField(TEXT("maxCopiedPoints"),MayaBlockSurvey::MaxPoints);Report->SetNumberField(TEXT("maxGeometryNodes"),MayaBlockSurvey::MaxNodes);
    Report->SetArrayField(TEXT("checks"),Checks);Report->SetArrayField(TEXT("lanes"),LaneRows);Report->SetArrayField(TEXT("geometry"),GeometryRows);Report->SetArrayField(TEXT("signalSnapshots"),SignalRows);
    Report->SetNumberField(TEXT("batteryBeforePercent"),Initial.BatteryPercent);Report->SetNumberField(TEXT("batteryAfterPercent"),Director.IsValid()?Director->Snapshot().BatteryPercent:-1);Report->SetBoolField(TEXT("probeResourceWrites"),false);Report->SetBoolField(TEXT("probeDriveCommands"),false);Report->SetBoolField(TEXT("probeSignalWrites"),false);
    Report->SetObjectField(TEXT("actualPlayerStreamingAtFinish"),MayaBlockSurvey::PlayerStreaming(Robot.IsValid()?Cast<APlayerController>(Robot->GetController()):nullptr));
    Report->SetStringField(TEXT("energyObservationScope"),TEXT("Normal active-world idle drain is observed, not bypassed. Probe makes no resource/drive/signal/settings calls."));
    FString Json;FJsonSerializer::Serialize(Report.ToSharedRef(),TJsonWriterFactory<>::Create(&Json));const bool Saved=FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Output,TEXT("block-survey.json")));
    auto Result=MakeShared<FJsonObject>();for(const FString& Key:{FString(TEXT("schema")),FString(TEXT("phase")),FString(TEXT("scope")),FString(TEXT("reason"))})Result->SetStringField(Key,Report->GetStringField(Key));
    Result->SetBoolField(TEXT("terminal"),true);Result->SetBoolField(TEXT("success"),bSuccess&&Saved);Result->SetBoolField(TEXT("routeViabilityVerified"),false);Result->SetBoolField(TEXT("humanPhysicalInputVerified"),false);Result->SetArrayField(TEXT("checks"),Checks);Result->SetStringField(TEXT("surveyPath"),FPaths::Combine(Output,TEXT("block-survey.json")));Result->SetNumberField(TEXT("endWorldSeconds"),GetWorld()->GetTimeSeconds());
    FString ResultJson;FJsonSerializer::Serialize(Result,TJsonWriterFactory<>::Create(&ResultJson));FFileHelper::SaveStringToFile(ResultJson,*FPaths::Combine(Output,TEXT("result.json")));
    UE_LOG(LogTemp,Display,TEXT("MAYA BLOCK SURVEY %s: %s; lanes=%d geometry=%d signals=%d"),bSuccess&&Saved?TEXT("EXPORT_COMPLETE"):TEXT("FAILED"),*Reason,LaneRows.Num(),GeometryRows.Num(),SignalCount);
    FPlatformMisc::RequestExit(false,TEXT("MAYACityBlockRouteSurvey"));
}
void AMAYACityBlockRouteSurvey::EndPlay(EEndPlayReason::Type Reason)
{if(SurveyStreaming)SurveyStreaming->DisableStreamingSource();Super::EndPlay(Reason);}
