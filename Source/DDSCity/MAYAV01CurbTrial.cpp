#include "MAYAV01CurbTrial.h"
#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryParcel.h"
#include "MAYADeliveryImpactEvidence.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"

namespace MayaCityCurb
{
TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{ return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)}; }
FString Json(const TSharedRef<FJsonObject>& J)
{ FString S; FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&S)); return S; }
bool SameReceipt(const FMAYADeliveryImpactReceipt& A, const FMAYADeliveryImpactReceipt& B)
{
    const auto& E=A.Event; const auto& F=B.Event;
    return A.EpisodeGeneration==B.EpisodeGeneration && E.Generation==F.Generation && E.Kind==F.Kind
        && E.WorldSeconds==F.WorldSeconds && E.ImpulseKgMps==F.ImpulseKgMps
        && E.PreContactDownCmps==F.PreContactDownCmps && E.DropCm==F.DropCm
        && E.ApproachSpeedCmps==F.ApproachSpeedCmps && E.QuietSecondsBeforeEvent==F.QuietSecondsBeforeEvent
        && E.OtherActor==F.OtherActor && E.OtherComponent==F.OtherComponent && E.RobotComponent==F.RobotComponent
        && A.AppliedConditionPoints==B.AppliedConditionPoints && A.EpisodeConditionPoints==B.EpisodeConditionPoints
        && A.ConditionBefore==B.ConditionBefore && A.ConditionAfter==B.ConditionAfter;
}
bool FiniteReceipt(const FMAYADeliveryImpactReceipt& R)
{
    const auto& E=R.Event;
    return R.EpisodeGeneration!=0 && E.Generation!=0 && FMath::IsFinite(E.WorldSeconds)
        && FMath::IsFinite(E.ImpulseKgMps) && FMath::IsFinite(E.PreContactDownCmps) && FMath::IsFinite(E.DropCm)
        && FMath::IsFinite(E.ApproachSpeedCmps) && FMath::IsFinite(E.QuietSecondsBeforeEvent)
        && FMath::IsFinite(R.AppliedConditionPoints) && R.AppliedConditionPoints>=0
        && FMath::IsFinite(R.EpisodeConditionPoints) && R.EpisodeConditionPoints>=0
        && FMath::IsFinite(R.ConditionBefore) && FMath::IsFinite(R.ConditionAfter);
}
bool ActualUpperFootprintInside(AMAYANativeDemoPawn* Robot,const FVector& Origin,const FVector& Axis,const FVector& Outward,double MarginCm=2)
{
    TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);int32 Count=0;
    for(UPrimitiveComponent* C:Components)
    {
        if(!C||!C->IsPhysicsCollisionEnabled())continue;
        FBodyInstance* B=C->GetBodyInstance();FBodyInstance* Own=C->GetBodyInstance(NAME_None,false);
        if(!B||!Own||B!=Own||!B->IsValidBodyInstance())continue;
        ++Count;const FBox Bounds=B->GetBodyBounds();
        if(!Bounds.IsValid||Bounds.Min.ContainsNaN()||Bounds.Max.ContainsNaN())return false;
        for(double X:{Bounds.Min.X,Bounds.Max.X})for(double Y:{Bounds.Min.Y,Bounds.Max.Y})for(double Z:{Bounds.Min.Z,Bounds.Max.Z})
        {
            const FVector D=FVector(X,Y,Z)-Origin;
            const double A=FVector::DotProduct(D,Axis),L=FVector::DotProduct(D,Outward);
            if(A< -80+MarginCm||A>80-MarginCm||L< -80+MarginCm||L>140-MarginCm)return false;
        }
    }
    return Count==9; // Compound chassis, six wheels and two actually locked lids.
}
const TCHAR* Name(int32 I)
{ const TCHAR* N[]={TEXT("Approach"),TEXT("Align"),TEXT("Settle"),TEXT("Cross"),TEXT("Hold"),TEXT("Complete"),TEXT("Failed"),TEXT("NormalAscent"),TEXT("BrakeBeforeBoost"),TEXT("BoostAscent"),TEXT("UpperHold")}; return I>=0&&I<UE_ARRAY_COUNT(N)?N[I]:TEXT("Unknown"); }
}
AMAYAV01CurbTrial::AMAYAV01CurbTrial() { PrimaryActorTick.bCanEverTick=false; }

bool AMAYAV01CurbTrial::Configure(AMAYANativeDemoPawn* InRobot, AMAYADeliveryDirector* InDirector, const FMAYADeliveryRobotObservation& O)
{
#if UE_BUILD_SHIPPING
    Failure=TEXT("City curb trial is unavailable in Shipping"); return false;
#endif
    if(bConfigured || !GetWorld() || !IsValid(InRobot) || !IsValid(InDirector)
        || !FParse::Param(FCommandLine::Get(),TEXT("MayaV01AutoTest")) || (!FParse::Param(FCommandLine::Get(),TEXT("MayaV01CurbTest"))&&!FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest"))))
    { Failure=TEXT("City curb trial requires explicit AutoTest+CurbTest and one configuration"); return false; }
    bBoostPower=FParse::Param(FCommandLine::Get(),TEXT("MayaV01BoostPowerTest"));
    Robot=InRobot; Director=InDirector; const auto& R=Director->GetMeasuredRoute(); const auto S=Director->Snapshot();
    if(R.Num()<13 || S.Phase!=EMAYADeliveryPhase::Carrying || S.Job.ParcelCount!=1 || Director->GetSecuredParcelCount()!=1
        || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched)
    { Failure=TEXT("Actual loaded one-body stopped/support/latch and measured corridor unavailable"); return false; }
    if(bBoostPower)
    {
        const auto* Parcel=Director->GetParcel(0);if(!Parcel||!Parcel->GetParcelBody()){Failure=TEXT("Actual original ascent parcel identity unavailable");return false;}
        OriginalParcelId=Parcel->GetParcelId();OriginalParcelActor=Parcel->GetPathName();OriginalParcelBody=Parcel->GetParcelBody()->GetPathName();
    }
    CourseOrigin=R[10]; Axis=(R[12]-R[0]).GetSafeNormal2D(); Outward=FVector(Axis.Y,-Axis.X,0);
    if(Axis.IsNearlyZero()) { Failure=TEXT("Measured course axis unavailable"); return false; }
    TArray<TSharedPtr<FJsonValue>> Survey; double UpperSum=0,RoadSum=0; int32 UpperCount=0,RoadCount=0;
    // Fresh first hits only. No vehicle/curb is ignored to expose a lower road.
    // Route500 lies between observed parking-meter390 and lamp600 frontages.
    // Fresh whole-width samples and a real blocking-volume query still decide admission.
    for(int32 Across=-80;Across<=80;Across+=20) for(int32 Along=-80;Along<=440;Along+=10)
    {
        const FVector XY=CourseOrigin+Axis*Across+Outward*Along;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYAOriginalCityCurb),false); Q.MobilityType=EQueryMobilityType::Static; Q.AddIgnoredActor(this);
        FHitResult H; const bool Hit=GetWorld()->LineTraceSingleByChannel(H,FVector(XY.X,XY.Y,CourseOrigin.Z+150),FVector(XY.X,XY.Y,CourseOrigin.Z-500),ECC_Visibility,Q);
        UPrimitiveComponent* C=H.GetComponent(); UStaticMeshComponent* M=Cast<UStaticMeshComponent>(C);
        const UInstancedStaticMeshComponent* Instances=Cast<UInstancedStaticMeshComponent>(C);
        const bool HitInstanceKnown=!Instances||Instances->IsValidInstance(H.Item);
        const FBodyInstance* DefaultBody=C?C->GetBodyInstance():nullptr;
        const FBodyInstance* B=C&&HitInstanceKnown?C->GetBodyInstance(H.BoneName,true,H.Item):nullptr;
        const bool Physical=Hit&&H.bBlockingHit&&!H.bStartPenetrating&&!H.ImpactPoint.ContainsNaN()&&!H.ImpactNormal.ContainsNaN()
            &&C&&C->Mobility==EComponentMobility::Static&&C->IsPhysicsCollisionEnabled()&&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block
            &&HitInstanceKnown&&B&&B->IsValidBodyInstance()&&H.ImpactNormal.Z>=.98;
        auto J=MakeShared<FJsonObject>(); J->SetNumberField(TEXT("alongOutwardCm"),Along);J->SetNumberField(TEXT("acrossAxisCm"),Across);
        J->SetBoolField(TEXT("firstBlockingHit"),Hit&&H.bBlockingHit);J->SetBoolField(TEXT("physicalFlatHit"),Physical);
        J->SetStringField(TEXT("component"),GetPathNameSafe(C)); J->SetStringField(TEXT("mesh"),M?GetPathNameSafe(M->GetStaticMesh()):FString());
        J->SetStringField(TEXT("componentClass"),C?GetPathNameSafe(C->GetClass()):FString());
        J->SetBoolField(TEXT("startPenetrating"),H.bStartPenetrating);J->SetNumberField(TEXT("hitInstanceIndex"),H.Item);
        J->SetBoolField(TEXT("instancedMesh"),Instances!=nullptr);J->SetBoolField(TEXT("hitInstanceKnown"),HitInstanceKnown);
        J->SetNumberField(TEXT("instanceCount"),Instances?Instances->GetInstanceCount():0);
        J->SetNumberField(TEXT("collisionEnabled"),C?int32(C->GetCollisionEnabled()):-1);
        J->SetNumberField(TEXT("mobility"),C?int32(C->Mobility):-1);
        J->SetNumberField(TEXT("responsePhysicsBody"),C?int32(C->GetCollisionResponseToChannel(ECC_PhysicsBody)):-1);
        J->SetBoolField(TEXT("physicsCollisionEnabled"),C&&C->IsPhysicsCollisionEnabled());
        J->SetBoolField(TEXT("defaultBodyValid"),DefaultBody&&DefaultBody->IsValidBodyInstance());
        J->SetBoolField(TEXT("actualHitBodyValid"),B&&B->IsValidBodyInstance());
        if(Hit&&H.bBlockingHit&&!H.ImpactPoint.ContainsNaN()&&!H.ImpactNormal.ContainsNaN())
        { J->SetArrayField(TEXT("pointCm"),MayaCityCurb::Vec(H.ImpactPoint));J->SetArrayField(TEXT("normal"),MayaCityCurb::Vec(H.ImpactNormal)); }
        Survey.Add(MakeShared<FJsonValueObject>(J));
        // Preserve raw failed surveys as well as successful geometry observations.
        auto Partial=MakeShared<FJsonObject>();Partial->SetArrayField(TEXT("firstHitSamples"),Survey);
        Partial->SetArrayField(TEXT("courseOriginCm"),MayaCityCurb::Vec(CourseOrigin));SurveyJson=MayaCityCurb::Json(Partial);
        if(Along<=140)
        {
            if(!Physical||FMath::Abs(H.ImpactPoint.Z-CourseOrigin.Z)>2)
            { Failure=TEXT("Fresh original upper sidewalk turning footprint is obstructed or not physical/flat"); return false; }
            UpperSum+=H.ImpactPoint.Z;++UpperCount;
        }
        if(Along>140&&Along<200)
        {
            const FString Mesh=M&&M->GetStaticMesh()?M->GetStaticMesh()->GetPathName():FString();
            const bool OriginalCurb=Mesh==TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Curb_Collision_Box_A.SM_Curb_Collision_Box_A")
                ||Mesh==TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Curb_Collision_Box_B.SM_Curb_Collision_Box_B")
                ||Mesh==TEXT("/Game/Road/Kit_Sidewalk_A/Mesh/SM_Sidewalk_Collision_Box.SM_Sidewalk_Collision_Box")
                ||Mesh==TEXT("/Game/Road/Kit_City_Road/SM_ROAD_19_20_0_0_road.SM_ROAD_19_20_0_0_road");
            if(!Physical||!OriginalCurb||H.ImpactPoint.Z<50||H.ImpactPoint.Z>CourseOrigin.Z+2)
            { Failure=TEXT("Fresh full-width original curb transition is obstructed or lacks an actual physical surface");return false; }
        }
        if(bBoostPower&&Physical)
        {
            if(Along<=140)UpperSupports.Add(C->GetPathName());
            if(M&&M->GetStaticMesh()&&M->GetStaticMesh()->GetName().StartsWith(TEXT("SM_Curb_Collision_Box_")))CurbSupports.Add(C->GetPathName());
        }
        if(Along>=200)
        {
            const bool OriginalRoad=M&&M->GetStaticMesh()&&M->GetStaticMesh()->GetPathName()==TEXT("/Game/Road/Kit_City_Road/SM_ROAD_19_20_0_0_road.SM_ROAD_19_20_0_0_road");
            if(!Physical||!OriginalRoad||H.ImpactPoint.Z<50||H.ImpactPoint.Z>65)
            { Failure=TEXT("Fresh original lower-road full runway unavailable; first blockers and hidden test floors are not bypassed"); return false; }
            RoadSupports.Add(C->GetPathName()); RoadSum+=H.ImpactPoint.Z;++RoadCount;
        }
    }
    UpperZ=UpperSum/UpperCount;RoadZ=RoadSum/RoadCount;
    // All original support tops are below UpperZ+2. The actual closed assembly
    // extends to roughly UpperZ+60; this conservative band detects corridor props.
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYACurbCorridorVolume),false);Q.MobilityType=EQueryMobilityType::Static;Q.AddIgnoredActor(this);
    TArray<FOverlapResult> Obstacles;
    GetWorld()->OverlapMultiByChannel(Obstacles,CourseOrigin+Outward*180+FVector(0,0,36),Axis.Rotation().Quaternion(),
        ECC_PhysicsBody,FCollisionShape::MakeBox(FVector(80,260,34)),Q);
    TArray<TSharedPtr<FJsonValue>> Blockers;
    for(const auto& Hit:Obstacles)
    {
        UPrimitiveComponent* C=Hit.GetComponent();
        if(C&&C->IsPhysicsCollisionEnabled()&&C->GetCollisionResponseToChannel(ECC_PhysicsBody)==ECR_Block)
            Blockers.Add(MakeShared<FJsonValueString>(C->GetPathName()));
    }
    if(Blockers.Num())
    {
        auto Partial=MakeShared<FJsonObject>();Partial->SetArrayField(TEXT("firstHitSamples"),Survey);
        Partial->SetArrayField(TEXT("courseOriginCm"),MayaCityCurb::Vec(CourseOrigin));Partial->SetArrayField(TEXT("actualStaticCorridorBlockers"),Blockers);
        SurveyJson=MayaCityCurb::Json(Partial);Failure=TEXT("Actual static physics blocker occupies the full closed-robot curb corridor volume");return false;
    }
    if(UpperZ-RoadZ<8||UpperZ-RoadZ>25) { Failure=TEXT("Measured original city curb drop is outside declared8..25cm fixture envelope");return false; }
    auto J=MakeShared<FJsonObject>();J->SetArrayField(TEXT("firstHitSamples"),Survey);J->SetArrayField(TEXT("courseOriginCm"),MayaCityCurb::Vec(CourseOrigin));
    J->SetBoolField(TEXT("actualStaticCorridorVolumeClear"),true);J->SetArrayField(TEXT("upperFootprintAcrossAlongBoundsCm"),{MakeShared<FJsonValueNumber>(-80),MakeShared<FJsonValueNumber>(80),MakeShared<FJsonValueNumber>(-80),MakeShared<FJsonValueNumber>(140)});
    J->SetNumberField(TEXT("upperMeanZCm"),UpperZ);J->SetNumberField(TEXT("roadMeanZCm"),RoadZ);J->SetNumberField(TEXT("physicalDropCm"),UpperZ-RoadZ);
    J->SetStringField(TEXT("scope"),TEXT("Fresh physical first-hit survey of original city sidewalk/curb/road; rays alone do not certify native support"));SurveyJson=MayaCityCurb::Json(J);
    JobId=S.JobInstanceId;ResetGeneration=O.ResetGeneration;HistoryBefore=S.ImpactHistory.Num();ConditionBefore=S.CargoConditionPercent;
    LandingGenerationAtStart=LastSavedLandingGeneration=O.LastNativeImpact.Generation;
    if(bBoostPower)DescentHistoryPrefix=S.ImpactHistory;
    if(bBoostPower)
    {
        TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);
        for(UPrimitiveComponent* C:Components)if(C&&C->IsPhysicsCollisionEnabled()&&C->GetName().Contains(TEXT("Wheel")))
            C->OnComponentHit.AddDynamic(this,&AMAYAV01CurbTrial::ObserveRawCurbHit);
        BindCompoundContactObservers();
    }
    StartWorldS=StepWorldS=GetWorld()->GetTimeSeconds();bConfigured=true;Save(O,true);return true;
}
bool AMAYAV01CurbTrial::Command(float T,float S,bool Brake,bool Boost)
{ bAccepted=Robot&&Robot->SubmitDriveCommand(T,S,Brake,EMAYADriveCommandOrigin::NativeAutomation,.25f,Boost);return bAccepted; }
void AMAYAV01CurbTrial::Enter(EStep Next,double Now) {Step=Next;StepWorldS=Now;StableWorldS=-1;bArrival=false;if(Next==EStep::Complete)UnbindCompoundContactObservers();}
AMAYAV01CurbTrial::EOutcome AMAYAV01CurbTrial::Fail(const FString& R) {Failure=R;Step=EStep::Failed;UnbindCompoundContactObservers();Command(0,0,true);return EOutcome::Failed;}
bool AMAYAV01CurbTrial::SixActualRoadContacts() const
{
    if(!Robot)return false;TSharedPtr<FJsonObject> J;const FString Native=Robot->ReadTelemetryJson();
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Native),J)||!J)return false;
    const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;if(!J->TryGetArrayField(TEXT("parts"),Parts)||!Parts||Parts->Num()<7)return false;
    for(int32 I=1;I<=6;++I)
    { const auto P=(*Parts)[I]->AsObject();FString Support;if(!P||!P->TryGetStringField(TEXT("nativeHitSupport"),Support)||!RoadSupports.Contains(Support))return false; }
    return true;
}
void AMAYAV01CurbTrial::Save(const FMAYADeliveryRobotObservation& O,bool Force)
{
    const double Now=GetWorld()->GetTimeSeconds();if(!Force&&Step!=EStep::Cross&&LastSampleWorldS>=0&&Now-LastSampleWorldS<.25)return;
    LastSampleWorldS=Now;if(Samples.Num()>=1500)return;auto J=MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("worldSeconds"),Now);J->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));
    J->SetArrayField(TEXT("positionCm"),MayaCityCurb::Vec(O.ChassisPose.GetLocation()));J->SetArrayField(TEXT("velocityCmps"),MayaCityCurb::Vec(O.VelocityCmps));
    J->SetNumberField(TEXT("supportMask"),O.SupportMask);J->SetNumberField(TEXT("conditionPercent"),Director->Snapshot().CargoConditionPercent);
    J->SetNumberField(TEXT("opticalForwardCmps"),O.OpticalForwardCmps);J->SetNumberField(TEXT("opticalHeadingDegrees"),(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw);
    J->SetBoolField(TEXT("settleHeadingTrimActive"),bSettleHeadingTrim);
    J->SetNumberField(TEXT("settleHeadingTrimRawYawDemand"),bSettleHeadingTrim?FMath::Clamp(FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,Outward.Rotation().Yaw)/12.,-.35,.35):0.);
    if(Step==EStep::Cross&&(LastContactSampleWorldS<0||Now-LastContactSampleWorldS>=.05))
    {
        // Contact clocks at20Hz plus every-frame typed support/chassis samples.
        // Receipt ages are evidence, not an alternative load-bearing/support policy.
        LastContactSampleWorldS=Now;const double RealNow=FPlatformTime::Seconds();J->SetNumberField(TEXT("platformRealSeconds"),RealNow);
        TSharedPtr<FJsonObject> Native;const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;
        if(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()),Native)
            &&Native&&Native->TryGetArrayField(TEXT("parts"),Parts)&&Parts&&Parts->Num()>=7)
        {
            TArray<TSharedPtr<FJsonValue>> Wheels;
            for(int32 I=1;I<=6;++I)
            {
                const auto P=(*Parts)[I]->AsObject();if(!P)continue;auto W=MakeShared<FJsonObject>();
                W->SetNumberField(TEXT("wheelIndex"),I-1);double HitS=-1,Impulse=0;FString Support;
                P->TryGetNumberField(TEXT("nativeHitRealS"),HitS);P->TryGetNumberField(TEXT("nativeHitImpulseKgCmps"),Impulse);
                P->TryGetStringField(TEXT("nativeHitSupport"),Support);
                W->SetNumberField(TEXT("nativeHitRealSeconds"),HitS);W->SetNumberField(TEXT("receiptAgeSeconds"),RealNow-HitS);
                W->SetNumberField(TEXT("nativeHitImpulseKgCmps"),Impulse);W->SetStringField(TEXT("support"),Support);
                const TArray<TSharedPtr<FJsonValue>>* Normal=nullptr;
                if(P->TryGetArrayField(TEXT("nativeHitNormal"),Normal)&&Normal)W->SetArrayField(TEXT("normal"),*Normal);
                bool Awake=false;P->TryGetBoolField(TEXT("nativeWheelAwake"),Awake);W->SetBoolField(TEXT("nativeWheelAwake"),Awake);
                Wheels.Add(MakeShared<FJsonValueObject>(W));
            }
            J->SetArrayField(TEXT("actualWheelContactReceipts"),Wheels);
        }
    }
    Samples.Add(MakeShared<FJsonValueObject>(J));
}
AMAYAV01CurbTrial::EOutcome AMAYAV01CurbTrial::Advance(const FMAYADeliveryRobotObservation& O)
{
    if(!bConfigured||!Robot||!Director||!GetWorld()||GetWorld()->IsPaused())return Fail(TEXT("Actual curb world/authority unavailable or paused"));
    if(bBoostPower&&int32(Step)>=int32(EStep::NormalAscent))return AdvanceBoostPower(O);
    const double Now=GetWorld()->GetTimeSeconds(),Age=Now-StepWorldS;const auto S=Director->Snapshot();const FVector P=O.ChassisPose.GetLocation();
    if(Now-StartWorldS>90)return Fail(TEXT("Actual original city curb course exceeded90gameplay seconds"));
    const FVector D=P-CourseOrigin;const double Across=FVector::DotProduct(D,Axis),Along=FVector::DotProduct(D,Outward);
    const FRotator Rotation=O.ChassisPose.Rotator();
    // Game59 measured20.325degree pitch on the original17.72cm step, before landing.
    // Adjacent axle spacing29.78cm, relative suspension allowance4cm and cooked
    // tyre support variation1.32cm yield a conservative adjacent-support pitch
    // of46.98degrees (L*sin(theta)-4*cos(theta)=step+tyre variation).
    // This prospective50degree crossing envelope changes no production policy.
    const double PitchLimit=Step==EStep::Cross?50.0:20.0;
    const int32 SecuredCount=Director->GetSecuredParcelCount();
    const bool NativeDoorsLocked=Robot->AreDeliveryDoorsClosedAndPhysicallyLocked();
    const bool ResetMatches=O.ResetGeneration==ResetGeneration,JobMatches=S.JobInstanceId==JobId;
    const bool Carrying=S.Phase==EMAYADeliveryPhase::Carrying;
    const bool RollInside=FMath::IsFinite(Rotation.Roll)&&FMath::Abs(Rotation.Roll)<=20;
    const bool PitchInside=FMath::IsFinite(Rotation.Pitch)&&FMath::Abs(Rotation.Pitch)<=PitchLimit;
    const bool HeightInside=FMath::IsFinite(P.Z)&&P.Z>=40&&P.Z<=UpperZ+40;
    auto Guard=MakeShared<FJsonObject>();
    Guard->SetStringField(TEXT("schema"),TEXT("MayaRobotics.OriginalCityCurbContainment.v1"));
    Guard->SetNumberField(TEXT("worldSeconds"),Now);Guard->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));
    Guard->SetBoolField(TEXT("known"),O.bKnown);Guard->SetBoolField(TEXT("ready"),O.bReady);Guard->SetStringField(TEXT("fault"),O.Fault);
    Guard->SetBoolField(TEXT("resetGenerationMatches"),ResetMatches);Guard->SetBoolField(TEXT("jobIdentityMatches"),JobMatches);
    Guard->SetBoolField(TEXT("missionCarrying"),Carrying);Guard->SetNumberField(TEXT("actualSecuredParcelCount"),SecuredCount);
    Guard->SetBoolField(TEXT("nativeDoorsPhysicallyClosedAndLocked"),NativeDoorsLocked);
    Guard->SetNumberField(TEXT("rollDegrees"),Rotation.Roll);Guard->SetNumberField(TEXT("pitchDegrees"),Rotation.Pitch);
    Guard->SetNumberField(TEXT("rollLimitDegrees"),20);Guard->SetNumberField(TEXT("pitchLimitDegrees"),PitchLimit);
    Guard->SetBoolField(TEXT("rollInside"),RollInside);Guard->SetBoolField(TEXT("pitchInside"),PitchInside);
    Guard->SetNumberField(TEXT("chassisHeightCm"),P.Z);Guard->SetNumberField(TEXT("minimumHeightCm"),40);
    Guard->SetNumberField(TEXT("maximumHeightCm"),UpperZ+40);Guard->SetBoolField(TEXT("heightInside"),HeightInside);
    Guard->SetNumberField(TEXT("supportMask"),O.SupportMask);
    LastContainmentEvidenceJson=MayaCityCurb::Json(Guard);
    if(!O.bKnown||!O.bReady||!O.Fault.IsEmpty()||!ResetMatches||!JobMatches||!Carrying
        ||SecuredCount!=1||!NativeDoorsLocked||!RollInside||!PitchInside||!HeightInside)
    {
        Save(O,true);
        return Fail(TEXT("Actual curb containment predicate refused; exact current identity, restraint, native lock, roll/pitch and height fields are retained in lastContainmentObservation"));
    }
    Save(O);
    if(O.LastNativeImpact.Generation>LastSavedLandingGeneration&&O.LastNativeImpact.Kind==TEXT("CurbLanding"))
    {Landings.Add(MakeShared<FJsonValueObject>(MAYADeliveryImpactEventToJson(O.LastNativeImpact)));LastSavedLandingGeneration=O.LastNativeImpact.Generation;}
    if(Step==EStep::Approach)
    {
        if(Age>25)return Fail(TEXT("Curb course approach did not settle within25seconds"));
        if(FMath::Abs(Along)>20)return Fail(TEXT("Actual approach left the original measured corridor by more than20cm"));
        const FVector Delta=CourseOrigin-P;const double Distance=Delta.Size2D();
        if(Distance<=10)bArrival=true;
        if(bArrival)
        {
            Command(0,0,true);if(Distance>15)return Fail(TEXT("Physical curb approach overshot its declared15cm stopped setup tolerance"));
            if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
            {if(StableWorldS<0)StableWorldS=Now;if(Now-StableWorldS>=.5)Enter(EStep::Align,Now);}else StableWorldS=-1;
        }
        else
        {
            const double Heading=(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
            const bool Reverse=FMath::Abs(FMath::FindDeltaAngleDegrees(Heading,Delta.Rotation().Yaw))>90;
            const double Error=FMath::FindDeltaAngleDegrees(Heading+(Reverse?180:0),Delta.Rotation().Yaw);
            if(FMath::Abs(Error)>35)return Fail(TEXT("Actual surveyed curb approach requires an unplanned large initial bearing"));
            Command(Reverse?-.35f:.35f,float(FMath::Clamp(Error/40,-.35,.35)),false);
        }
    }
    else if(Step==EStep::Align||Step==EStep::Settle)
    {
        if(Age>(Step==EStep::Align?30:10))return Fail(TEXT("Actual upper-sidewalk ninety-degree turn/stable setup exceeded its bound"));
        if(!MayaCityCurb::ActualUpperFootprintInside(Robot,CourseOrigin,Axis,Outward))return Fail(TEXT("Actual nine-body native footprint left the freshly surveyed upper turning patch with2cm margin"));
        const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,Outward.Rotation().Yaw);
        if(Step==EStep::Align)
        {
            // Game61 drifted backward despite saturated simultaneous correction.
            // Before reaching the original hard footprint edge, use real braking
            // and straight translation at its measured heading to regain margin.
            if(AlignmentRecenterStep==0&&!MayaCityCurb::ActualUpperFootprintInside(Robot,CourseOrigin,Axis,Outward,20))
            {
                if(++AlignmentRecenterCount>4)return Fail(TEXT("Actual curb alignment exceeded four bounded physical recenter corrections"));
                AlignmentRecenterStep=1;AlignmentRecenterStartWorldS=Now;StableWorldS=-1;
            }
            if(AlignmentRecenterStep!=0)
            {
                if(Now-AlignmentRecenterStartWorldS>6)return Fail(TEXT("Actual curb alignment recenter did not brake, translate and settle within6seconds inside original30second alignment bound"));
                if(AlignmentRecenterStep==1)
                {
                    Command(0,0,true);
                    if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
                    {
                        if(StableWorldS<0)StableWorldS=Now;
                        if(Now-StableWorldS>=.25)
                        {
                            AlignmentRecenterAxis=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
                            AlignmentRecenterHeading=AlignmentRecenterAxis.Rotation().Yaw;AlignmentRecenterStartCm=P;
                            const double Projection=FVector::DotProduct(CourseOrigin-P,AlignmentRecenterAxis);
                            if(FMath::Abs(Projection)>40)return Fail(TEXT("Actual measured-heading recenter needs more than declared40cm straight translation"));
                            AlignmentRecenterSign=Projection>=0?1:-1;
                            AlignmentRecenterGoalCm=P+AlignmentRecenterAxis*Projection;
                            AlignmentRecenterStep=2;StableWorldS=-1;
                        }
                    }
                    else StableWorldS=-1;
                }
                else if(AlignmentRecenterStep==2)
                {
                    const double Heading=(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
                    if(FMath::Abs(FMath::FindDeltaAngleDegrees(Heading,AlignmentRecenterHeading))>5
                        ||(P-AlignmentRecenterStartCm).Size2D()>40)
                        return Fail(TEXT("Actual straight recenter exceeded preserved5degree heading or40cm measured travel"));
                    AlignmentRecenterRemainingCm=FVector::DotProduct(AlignmentRecenterGoalCm-P,AlignmentRecenterAxis);
                    if(AlignmentRecenterSign*AlignmentRecenterRemainingCm<=5)
                    {Command(0,0,true);AlignmentRecenterStep=3;StableWorldS=-1;}
                    else Command(float(AlignmentRecenterSign*.35),0,false);
                }
                else
                {
                    Command(0,0,true);
                    if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched)
                    {
                        if(StableWorldS<0)StableWorldS=Now;
                        if(Now-StableWorldS>=.25)
                        {
                            if(!MayaCityCurb::ActualUpperFootprintInside(Robot,CourseOrigin,Axis,Outward,20))
                                return Fail(TEXT("Actual settled straight recenter did not regain declared20cm inner footprint margin"));
                            AlignmentRecenterStep=0;StableWorldS=-1;
                        }
                    }
                    else StableWorldS=-1;
                }
                if(!bAccepted)return Fail(TEXT("Actual bounded recenter native command was refused"));
                return EOutcome::Running;
            }
            if(FMath::Abs(Error)<=2){Command(0,0,true);Enter(EStep::Settle,Now);}
            else
            {
                // Game57 measured backward optical drift12..15cm/s under pure steering.
                // Actual position/velocity feedback adds only a bounded ordinary throttle.
                const FVector OpticalAxis=(-O.ChassisPose.GetUnitAxis(EAxis::X)).GetSafeNormal2D();
                const double AlongOpticalError=FVector::DotProduct(CourseOrigin-P,OpticalAxis);
                const float Correction=float(FMath::Clamp(.02*AlongOpticalError-.015*O.OpticalForwardCmps,-.35,.35));
                Command(Correction,Error>0?1.f:-1.f,false);
            }
        }
        else
        {
            // Game108 stopped at3.414degrees error: brake-only settlement could
            // never satisfy3degrees. Trim only after an actual supported/latched stop,
            // retain the original10s clock, then re-brake and prove the full1s dwell.
            if(!bSettleHeadingTrim&&O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&FMath::Abs(Error)>3)
                bSettleHeadingTrim=true;
            if(bSettleHeadingTrim&&FMath::Abs(Error)>3)
            {
                StableWorldS=-1;
                Command(0,float(FMath::Clamp(Error/12.,-.35,.35)),false);
            }
            else
            {
                bSettleHeadingTrim=false;Command(0,0,true);
                if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&FMath::Abs(Error)<=3)
                {if(StableWorldS<0)StableWorldS=Now;if(Now-StableWorldS>=1){CrossStartZ=P.Z;Enter(EStep::Cross,Now);}}
                else StableWorldS=-1;
            }
        }
    }
    else if(Step==EStep::Cross)
    {
        if(Age>20||FMath::Abs(Across)>30||Along< -40||Along>370||O.VelocityCmps.Size()>90)return Fail(TEXT("Actual city curb crossing exceeded declared20seconds/30cm lateral/370cm travel/90cmps bounds"));
        bLostSupport|=O.SupportMask!=63;if(bLostSupport&&O.SupportMask==63)bRegainedSupport=true;
        PeakDropCm=FMath::Max(PeakDropCm,CrossStartZ-P.Z);PeakDownCmps=FMath::Max(PeakDownCmps,FMath::Max(0.0,-O.VelocityCmps.Z));
        if(Along>=300)bArrival=true;
        if(bArrival)
        {
            Command(0,0,true);
            if(O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&SixActualRoadContacts())
            {
                if(StableWorldS<0)StableWorldS=Now;
                if(Now-StableWorldS>=.5){ConditionAtHold=S.CargoConditionPercent;HistoryAtHold=S.ImpactHistory.Num();Enter(EStep::Hold,Now);}
            }
            else StableWorldS=-1;
        }
        else
        {
            if(bBoostPower)
            {
                const FVector DesiredOptical=(Outward-Axis*(Across/200.)).GetSafeNormal2D();
                const double ActualYaw=(-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw;
                const double TargetYaw=DesiredOptical.Rotation().Yaw;
                const double GuidanceError=FMath::FindDeltaAngleDegrees(ActualYaw,TargetYaw);
                if(!FMath::IsFinite(Across)||DesiredOptical.ContainsNaN()||DesiredOptical.IsNearlyZero()
                    ||!FMath::IsFinite(ActualYaw)||!FMath::IsFinite(TargetYaw)||!FMath::IsFinite(GuidanceError))
                    return Fail(TEXT("Boost descent centreline guidance lost finite actual pose or desired direction"));
                const float RawSteer=float(FMath::Clamp(GuidanceError/40,-.35,.35));
                const bool Brake=O.OpticalForwardCmps>65;
                Command(Brake?0:.5f,RawSteer,Brake);
                ++CrossGuidanceObservationsSeen;
                if(CrossGuidanceObservations.Num()<1500)
                {
                    auto Guidance=MakeShared<FJsonObject>();
                    Guidance->SetNumberField(TEXT("worldSeconds"),Now);Guidance->SetNumberField(TEXT("frame"),double(GFrameCounter));
                    Guidance->SetNumberField(TEXT("actualAcrossCm"),Across);Guidance->SetNumberField(TEXT("actualAlongCm"),Along);
                    Guidance->SetNumberField(TEXT("lookaheadCm"),200);Guidance->SetNumberField(TEXT("actualOpticalYawDegrees"),ActualYaw);
                    Guidance->SetArrayField(TEXT("desiredUnitOptical"),MayaCityCurb::Vec(DesiredOptical));
                    Guidance->SetNumberField(TEXT("targetOpticalYawDegrees"),TargetYaw);Guidance->SetNumberField(TEXT("headingErrorDegrees"),GuidanceError);
                    Guidance->SetNumberField(TEXT("requestedThrottle"),Brake?0:.5);Guidance->SetNumberField(TEXT("requestedSteer"),RawSteer);
                    Guidance->SetBoolField(TEXT("requestedBrake"),Brake);Guidance->SetBoolField(TEXT("commandAccepted"),bAccepted);
                    CrossGuidanceObservations.Add(MakeShared<FJsonValueObject>(Guidance));
                }
                else bCrossGuidanceTraceTruncated=true;
            }
            else
            {
                const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,Outward.Rotation().Yaw);
                Command(O.OpticalForwardCmps>65?0:.5f,float(FMath::Clamp(Error/40,-.35,.35)),O.OpticalForwardCmps>65);
            }
        }
    }
    else if(Step==EStep::Hold)
    {
        Command(0,0,true);if(Age>4)return Fail(TEXT("Actual curb neutral road hold did not retain supported settled endpoint within4seconds"));
        if(S.CargoConditionPercent!=ConditionAtHold||S.ImpactHistory.Num()!=HistoryAtHold)return Fail(TEXT("Actual curb neutral road hold added repeated condition loss/history"));
        if(!O.IsStoppedAndSupported()||!O.bDoorsClosedLatched||!SixActualRoadContacts()){StableWorldS=-1;return EOutcome::Running;}
        if(StableWorldS<0)StableWorldS=Now;
        if(Now-StableWorldS>=1.2)
        {
            int32 LandingReceipts=0;bool LandingVerified=false;double AppliedSum=0;
            for(int32 I=HistoryBefore;I<S.ImpactHistory.Num();++I)
            {
                const auto& R=S.ImpactHistory[I];AppliedSum+=R.AppliedConditionPoints;
                if(R.Event.Kind==TEXT("CurbLanding")&&R.Event.Generation>LandingGenerationAtStart)
                {++LandingReceipts;LandingVerified|=R.Event.DropCm>=8&&R.Event.PreContactDownCmps>=40&&R.EpisodeConditionPoints==1;}
            }
            bLegacyLandingCriterionPassed= !(!bLostSupport||!bRegainedSupport||Landings.Num()!=1||LandingReceipts!=1||!LandingVerified
                ||FMath::Abs(ConditionBefore-S.CargoConditionPercent-1)>1.e-6||FMath::Abs(AppliedSum-1)>1.e-6);
            if(!bBoostPower&&!bLegacyLandingCriterionPassed)
                return Fail(TEXT("Original city curb did not produce exactly one measured8cm/40cmps landing receipt at1point episode severity with complete condition accounting"));
            if(bBoostPower)
            {
                bDescentAssessmentPerformed=true;
                const bool IndicesValid=HistoryBefore>=0&&HistoryBefore<=HistoryAtHold&&HistoryAtHold<64
                    &&HistoryAtHold==S.ImpactHistory.Num()&&DescentHistoryPrefix.Num()==HistoryBefore;
                bool PrefixMatches=IndicesValid;
                if(IndicesValid)for(int32 I=0;I<HistoryBefore;++I)
                    PrefixMatches &= MayaCityCurb::FiniteReceipt(DescentHistoryPrefix[I])
                        &&MayaCityCurb::SameReceipt(DescentHistoryPrefix[I],S.ImpactHistory[I]);
                bool ChainComplete=IndicesValid&&FMath::IsFinite(ConditionBefore)&&FMath::IsFinite(ConditionAtHold)
                    &&FMath::IsFinite(S.CargoConditionPercent)&&ConditionBefore>=0&&ConditionBefore<=100
                    &&ConditionAtHold>=80&&ConditionAtHold<=100;
                double RunningCondition=ConditionBefore,AccountedSum=0;
                TArray<FMAYADeliveryImpactReceipt> NewReceipts;
                if(IndicesValid)for(int32 I=HistoryBefore;I<HistoryAtHold;++I)
                {
                    const auto& Receipt=S.ImpactHistory[I];NewReceipts.Add(Receipt);
                    const bool Finite=MayaCityCurb::FiniteReceipt(Receipt);
                    ChainComplete &= Finite&&FMath::Abs(Receipt.ConditionBefore-RunningCondition)<=1.e-6
                        &&FMath::Abs(Receipt.ConditionBefore-Receipt.ConditionAfter-Receipt.AppliedConditionPoints)<=1.e-6;
                    RunningCondition=Receipt.ConditionAfter;AccountedSum+=Receipt.AppliedConditionPoints;
                }
                ChainComplete &= FMath::IsFinite(RunningCondition)&&FMath::IsFinite(AccountedSum)
                    &&FMath::Abs(RunningCondition-ConditionAtHold)<=1.e-6
                    &&FMath::Abs(ConditionBefore-ConditionAtHold-AccountedSum)<=1.e-6;
                bDescentConditionAccountingComplete=PrefixMatches&&ChainComplete;
                const bool FiniteEndpoint=!O.ChassisPose.ContainsNaN()&&!O.VelocityCmps.ContainsNaN()
                    &&!O.AngularVelocityRadps.ContainsNaN()&&FMath::IsFinite(PeakDropCm)&&FMath::IsFinite(PeakDownCmps);
                const auto* DescentParcel=Director->GetParcel(0);
                const bool SameActualParcel=DescentParcel&&DescentParcel->GetParcelBody()
                    &&DescentParcel->GetParcelId()==OriginalParcelId&&DescentParcel->GetPathName()==OriginalParcelActor
                    &&DescentParcel->GetParcelBody()->GetPathName()==OriginalParcelBody;
                bBoostDescentRoadEndpointAccepted=bAccepted&&bLostSupport&&bRegainedSupport&&FiniteEndpoint
                    &&SameActualParcel&&bDescentConditionAccountingComplete;
                DescentAccountingEvidence=MakeShared<FJsonObject>();
                auto Accounting= DescentAccountingEvidence;
                Accounting->SetNumberField(TEXT("historyBefore"),HistoryBefore);Accounting->SetNumberField(TEXT("historyAtHold"),HistoryAtHold);
                Accounting->SetBoolField(TEXT("orderedIndicesBelow64"),IndicesValid);Accounting->SetBoolField(TEXT("capturedPrefixMatches"),PrefixMatches);
                Accounting->SetBoolField(TEXT("finiteConsecutiveConditionChain"),ChainComplete);Accounting->SetBoolField(TEXT("finiteActualRoadEndpoint"),FiniteEndpoint);
                Accounting->SetBoolField(TEXT("sameActualConfiguredParcelIdentity"),SameActualParcel);
                Accounting->SetStringField(TEXT("actualParcelId"),DescentParcel?DescentParcel->GetParcelId():FString());
                Accounting->SetStringField(TEXT("actualParcelActor"),GetPathNameSafe(DescentParcel));
                Accounting->SetStringField(TEXT("actualParcelBody"),DescentParcel?GetPathNameSafe(DescentParcel->GetParcelBody()):FString());
                Accounting->SetNumberField(TEXT("actualLandingEventCount"),Landings.Num());Accounting->SetNumberField(TEXT("actualLandingReceiptCount"),LandingReceipts);
                Accounting->SetBoolField(TEXT("existingLandingReceiptQualification"),LandingVerified);
                Accounting->SetNumberField(TEXT("conditionBefore"),ConditionBefore);Accounting->SetNumberField(TEXT("conditionAtHold"),ConditionAtHold);
                if(FMath::IsFinite(AccountedSum))Accounting->SetNumberField(TEXT("accountedAppliedConditionPoints"),AccountedSum);
                Accounting->SetArrayField(TEXT("capturedHistoryPrefix"),MAYADeliveryImpactHistoryToJson(DescentHistoryPrefix));
                Accounting->SetArrayField(TEXT("completeNewDescentReceipts"),MAYADeliveryImpactHistoryToJson(NewReceipts));
                Accounting->SetStringField(TEXT("scope"),TEXT("Boost-only actual road setup with complete unchanged-prefix and finite receipt-chain accounting below the Core64-entry cap. Zero receipts require zero loss; repeated source generations may represent genuine escalation. No qualifying landing-damage PASS is implied."));
                if(!bBoostDescentRoadEndpointAccepted)
                    return Fail(TEXT("Boost descent did not retain its safe actual road endpoint and complete bounded condition accounting"));
            }
            Save(O,true);
            if(bBoostPower){NormalStartAlongCm=Along;StallAnchorAlongCm=Along;Enter(EStep::NormalAscent,Now);return EOutcome::Running;}
            Enter(EStep::Complete,Now);return EOutcome::Complete;
        }
    }
    if(!bAccepted)return Fail(TEXT("Actual curb course native leased command was refused"));return EOutcome::Running;
}
void AMAYAV01CurbTrial::BindCompoundContactObservers()
{
    CompoundContactBinding=MakeShared<FJsonObject>();CompoundContactBinding->SetBoolField(TEXT("attempted"),true);
    CompoundContactBinding->SetNumberField(TEXT("worldSeconds"),GetWorld()->GetTimeSeconds());
    CompoundContactBinding->SetNumberField(TEXT("platformRealSeconds"),FPlatformTime::Seconds());
    CompoundContactBinding->SetNumberField(TEXT("frame"),double(GFrameCounter));
    UPrimitiveComponent* Mount=Robot?Robot->GetDeliveryCargoMount():nullptr;
    UPrimitiveComponent* Root=Mount?Cast<UPrimitiveComponent>(Mount->GetAttachParent()):nullptr;
    FBodyInstance* Native=Root?Root->GetBodyInstance():nullptr;
    FBodyInstance* Own=Root?Root->GetBodyInstance(NAME_None,false):nullptr;
    const bool RootVerified=Root&&Native&&Own==Native&&Native->IsValidBodyInstance()&&!Own->WeldParent;
    CompoundContactBinding->SetStringField(TEXT("cargoMountComponent"),GetPathNameSafe(Mount));
    CompoundContactBinding->SetStringField(TEXT("actualCompoundRootComponent"),GetPathNameSafe(Root));
    CompoundContactBinding->SetBoolField(TEXT("rootIndependentNativeBodyVerified"),RootVerified);
    if(!RootVerified){CompoundContactBinding->SetBoolField(TEXT("coverageAvailable"),false);CompoundContactBinding->SetStringField(TEXT("status"),TEXT("Unavailable: actual cargo-mount parent is not a verified independent native compound root"));return;}
    CompoundContactRoot=Root;
    TArray<UPrimitiveComponent*> Components,Verified;Robot->GetComponents<UPrimitiveComponent>(Components);
    TArray<TSharedPtr<FJsonValue>> Rows;int32 WeldedObserverCount=0;
    for(UPrimitiveComponent* C:Components)
    {
        if(!C)continue;FBodyInstance* Resolved=C->GetBodyInstance();FBodyInstance* Local=C->GetBodyInstance(NAME_None,false);
        const bool IsRoot=C==Root&&Local==Native&&Resolved==Native;
        const bool IsChild=C!=Root&&Local&&Local->WeldParent==Native&&Resolved==Native;
        if(!IsRoot&&!IsChild)continue;Verified.Add(C);if(IsChild)++WeldedObserverCount;
        auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("component"),C->GetPathName());
        J->SetBoolField(TEXT("verifiedRootOwner"),IsRoot);J->SetBoolField(TEXT("verifiedActualWeldedChild"),IsChild);
        J->SetBoolField(TEXT("resolvedNativeBodyIsRoot"),Resolved==Native);J->SetBoolField(TEXT("ownBodyInstanceIsRoot"),Local==Native);
        J->SetBoolField(TEXT("ownWeldParentIsRoot"),Local&&Local->WeldParent==Native);
        J->SetBoolField(TEXT("physicsCollisionEnabledReadback"),C->IsPhysicsCollisionEnabled());
        J->SetBoolField(TEXT("ownNotifyRigidBodyCollisionReadback"),Local&&Local->bNotifyRigidBodyCollision);
        J->SetBoolField(TEXT("resolvedRootNotifyRigidBodyCollisionReadback"),Native->bNotifyRigidBodyCollision);
        Rows.Add(MakeShared<FJsonValueObject>(J));
    }
    CompoundContactBinding->SetNumberField(TEXT("verifiedWeldedChildCount"),WeldedObserverCount);
    CompoundContactBinding->SetNumberField(TEXT("verifiedComponentCount"),Verified.Num());
    CompoundContactBinding->SetArrayField(TEXT("actualVerifiedComponents"),Rows);
    const bool Available=WeldedObserverCount==6&&Verified.Num()==7;
    CompoundContactBinding->SetBoolField(TEXT("coverageAvailable"),Available);
    CompoundContactBinding->SetNumberField(TEXT("listenersAdded"),Available?Verified.Num():0);
    CompoundContactBinding->SetStringField(TEXT("status"),Available?TEXT("Seven passive listeners on actual compound root and six verified welded children; existing notification flags unchanged"):TEXT("Unavailable: exact root plus six actual welded-child relationship inventory not verified; no diagnostic listener added"));
    if(Available)for(UPrimitiveComponent* C:Verified)
    {C->OnComponentHit.AddUniqueDynamic(this,&AMAYAV01CurbTrial::ObserveCompoundContact);CompoundContactObservers.Add(C);}
}
void AMAYAV01CurbTrial::UnbindCompoundContactObservers()
{
    for(const auto& Weak:CompoundContactObservers)if(UPrimitiveComponent* C=Weak.Get())
        C->OnComponentHit.RemoveDynamic(this,&AMAYAV01CurbTrial::ObserveCompoundContact);
    CompoundContactObservers.Reset();bCompoundContactCleanupPerformed=true;
}
void AMAYAV01CurbTrial::ObserveCompoundContact(UPrimitiveComponent* HitComponent,AActor* OtherActor,UPrimitiveComponent* OtherComponent,FVector NormalImpulse,const FHitResult& Hit)
{
    const bool Phase=Step==EStep::NormalAscent||Step==EStep::BrakeBeforeBoost||Step==EStep::BoostAscent||Step==EStep::UpperHold;
    if(!bBoostPower||!bConfigured||!Phase||!GetWorld())return;
    ++CompoundContactCallbacksSeen;bool Bound=false;for(const auto& Weak:CompoundContactObservers)Bound|=Weak.Get()==HitComponent;
    if(!HitComponent||!OtherComponent||!Bound){++CompoundContactDropped;return;}
    const FString Support=OtherComponent->GetPathName();
    const bool Upper=UpperSupports.Contains(Support),Curb=CurbSupports.Contains(Support),Road=RoadSupports.Contains(Support);
    if(!Upper&&!Curb&&!Road){++CompoundContactFiltered;return;}
    UPrimitiveComponent* Root=CompoundContactRoot.Get();FBodyInstance* RootBody=Root?Root->GetBodyInstance():nullptr;
    FBodyInstance* Own=HitComponent->GetBodyInstance(NAME_None,false);FBodyInstance* Native=HitComponent->GetBodyInstance();
    const bool RootValid=RootBody&&RootBody->IsValidBodyInstance();
    const bool IsRoot=RootValid&&Root&&HitComponent==Root&&Own==RootBody&&Native==RootBody;
    const bool IsChild=RootValid&&Root&&HitComponent!=Root&&Own&&Own->WeldParent==RootBody&&Native==RootBody;
    const double WorldS=GetWorld()->GetTimeSeconds(),RealS=FPlatformTime::Seconds(),ImpulseMagnitude=NormalImpulse.Size();
    const bool PointFinite=!Hit.ImpactPoint.ContainsNaN(),NormalFinite=!Hit.ImpactNormal.ContainsNaN();
    const bool ImpulseFinite=!NormalImpulse.ContainsNaN()&&FMath::IsFinite(ImpulseMagnitude);
    const bool DepthFinite=FMath::IsFinite(Hit.PenetrationDepth),ClocksFinite=FMath::IsFinite(WorldS)&&FMath::IsFinite(RealS);
    const bool Finite=PointFinite&&NormalFinite&&ImpulseFinite&&DepthFinite&&ClocksFinite;
    const bool Relationship=RootValid&&(IsRoot||IsChild);
    if(!Finite||!Relationship)++CompoundContactMalformed;
    const FString PhaseName=MayaCityCurb::Name(int32(Step));++CompoundContactPhaseCounts.FindOrAdd(PhaseName);++CompoundContactRecorded;
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("phase"),PhaseName);J->SetNumberField(TEXT("frame"),double(GFrameCounter));
    J->SetBoolField(TEXT("callbackClocksFinite"),ClocksFinite);if(ClocksFinite){J->SetNumberField(TEXT("worldSeconds"),WorldS);J->SetNumberField(TEXT("platformRealSeconds"),RealS);}
    J->SetStringField(TEXT("reportedHitComponent"),HitComponent->GetPathName());J->SetStringField(TEXT("actualCompoundRootComponent"),GetPathNameSafe(Root));
    J->SetBoolField(TEXT("currentRootNativeBodyValid"),RootValid);J->SetBoolField(TEXT("reportedOwnerIsVerifiedRoot"),IsRoot);J->SetBoolField(TEXT("reportedOwnerIsVerifiedWeldedChild"),IsChild);
    J->SetBoolField(TEXT("ownBodyInstanceIsRoot"),RootBody&&Own==RootBody);J->SetBoolField(TEXT("ownWeldParentIsRoot"),RootBody&&Own&&Own->WeldParent==RootBody);
    J->SetBoolField(TEXT("resolvedNativeBodyIsRoot"),RootBody&&Native==RootBody);J->SetBoolField(TEXT("currentReportedOwnerRelationshipVerified"),Relationship);
    J->SetStringField(TEXT("otherActor"),GetPathNameSafe(OtherActor));J->SetStringField(TEXT("otherComponent"),Support);
    J->SetBoolField(TEXT("otherInOriginalUpperSupports"),Upper);J->SetBoolField(TEXT("otherInOriginalCurbSupports"),Curb);J->SetBoolField(TEXT("otherInOriginalRoadSupports"),Road);
    J->SetBoolField(TEXT("pointFiniteKnown"),PointFinite);if(PointFinite)J->SetArrayField(TEXT("actualImpactPointCm"),MayaCityCurb::Vec(Hit.ImpactPoint));
    J->SetBoolField(TEXT("normalFiniteKnown"),NormalFinite);if(NormalFinite)J->SetArrayField(TEXT("actualImpactNormal"),MayaCityCurb::Vec(Hit.ImpactNormal));
    J->SetBoolField(TEXT("impulseFiniteKnown"),ImpulseFinite);if(ImpulseFinite){J->SetArrayField(TEXT("callbackImpulseKgCmps"),MayaCityCurb::Vec(NormalImpulse));J->SetNumberField(TEXT("callbackImpulseMagnitudeKgCmps"),ImpulseMagnitude);}
    J->SetBoolField(TEXT("penetrationFiniteKnown"),DepthFinite);if(DepthFinite)J->SetNumberField(TEXT("nativePenetrationDepthCm"),Hit.PenetrationDepth);
    J->SetBoolField(TEXT("blockingHit"),Hit.bBlockingHit);J->SetBoolField(TEXT("startPenetrating"),Hit.bStartPenetrating);
    J->SetNumberField(TEXT("reportedMyBodyIndex"),Hit.MyItem);J->SetNumberField(TEXT("reportedOtherBodyIndex"),Hit.Item);
    J->SetStringField(TEXT("reportedMyBoneName"),Hit.MyBoneName.ToString());J->SetStringField(TEXT("reportedOtherBoneName"),Hit.BoneName.ToString());
    J->SetBoolField(TEXT("allCompactOperandsFinite"),Finite);J->SetBoolField(TEXT("malformedReceipt"),!Finite||!Relationship);
    const auto Value=MakeShared<FJsonValueObject>(J);
    if(CompoundContactFirst.Num()<32)CompoundContactFirst.Add(Value);
    else if(CompoundContactRecent.Num()<128)CompoundContactRecent.Add(Value);
    else{CompoundContactRecent[CompoundContactRecentWrite]=Value;CompoundContactRecentWrite=(CompoundContactRecentWrite+1)%128;++CompoundContactOverwritten;}
}
void AMAYAV01CurbTrial::ObserveRawCurbHit(UPrimitiveComponent* HitComponent,AActor* OtherActor,UPrimitiveComponent* OtherComponent,FVector NormalImpulse,const FHitResult& Hit)
{
    if(!bBoostPower||!bConfigured||int32(Step)<int32(EStep::NormalAscent)||!GetWorld()||!CurbSupports.Contains(GetPathNameSafe(OtherComponent))||!OtherComponent
        ||!OtherComponent->IsPhysicsCollisionEnabled()||OtherComponent->GetCollisionResponseToChannel(ECC_PhysicsBody)!=ECR_Block
        ||Hit.ImpactPoint.ContainsNaN()||Hit.ImpactNormal.ContainsNaN()||NormalImpulse.ContainsNaN())return;
    const auto* ISM=Cast<UInstancedStaticMeshComponent>(OtherComponent);if(ISM&&!ISM->IsValidInstance(Hit.Item))return;
    auto* B=OtherComponent->GetBodyInstance(Hit.BoneName,true,Hit.Item);if(!B||!B->IsValidBodyInstance())return;
    LastRawCurbHitWorldS=GetWorld()->GetTimeSeconds();++RawCurbHitCount;
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("worldSeconds"),LastRawCurbHitWorldS);J->SetNumberField(TEXT("frame"),double(GFrameCounter));J->SetStringField(TEXT("wheel"),GetPathNameSafe(HitComponent));J->SetStringField(TEXT("otherActor"),GetPathNameSafe(OtherActor));J->SetStringField(TEXT("otherComponent"),OtherComponent->GetPathName());J->SetNumberField(TEXT("instance"),Hit.Item);J->SetArrayField(TEXT("actualPointCm"),MayaCityCurb::Vec(Hit.ImpactPoint));J->SetArrayField(TEXT("normal"),MayaCityCurb::Vec(Hit.ImpactNormal));J->SetArrayField(TEXT("callbackImpulseKgCmps"),MayaCityCurb::Vec(NormalImpulse));LastRawCurbHit=J;if(RawCurbHits.Num()<256)RawCurbHits.Add(MakeShared<FJsonValueObject>(J));
}
bool AMAYAV01CurbTrial::NativeCurbContactAndMotor() const
{
    TSharedPtr<FJsonObject> J;const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()),J)||!J||!J->TryGetArrayField(TEXT("parts"),Parts)||!Parts||Parts->Num()<7)return false;
    bool Contact=LastRawCurbHitWorldS>=0&&GetWorld()->GetTimeSeconds()-LastRawCurbHitWorldS<=.25,Motor=false;const double Now=FPlatformTime::Seconds();
    for(int32 I=1;I<=6;++I)
    {const auto W=(*Parts)[I]->AsObject();FString Support;double Hit=-1,Nm=0;if(!W||!W->TryGetStringField(TEXT("nativeHitSupport"),Support)||!W->TryGetNumberField(TEXT("nativeHitRealS"),Hit)||!W->TryGetNumberField(TEXT("motorNm"),Nm)||!FMath::IsFinite(Nm))return false;
        Contact|=CurbSupports.Contains(Support)&&Now-Hit>=0&&Now-Hit<=.25;Motor|=FMath::Abs(Nm)>.1;}
    return Contact&&Motor;
}
bool AMAYAV01CurbTrial::NativeUpperWheelFootprint() const
{
    TSharedPtr<FJsonObject> J;const TArray<TSharedPtr<FJsonValue>>* Parts=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Robot->ReadTelemetryJson()),J)||!J||!J->TryGetArrayField(TEXT("parts"),Parts)||!Parts||Parts->Num()<7)return false;
    TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);int32 Wheels=0;
    for(int32 I=1;I<=6;++I)
    {FString Support;if(!(*Parts)[I]->AsObject()->TryGetStringField(TEXT("nativeHitSupport"),Support)||!UpperSupports.Contains(Support))return false;}
    for(UPrimitiveComponent* C:Components)
    {
        if(!C||!C->IsPhysicsCollisionEnabled()||!C->GetName().Contains(TEXT("Wheel")))continue;
        auto* B=C->GetBodyInstance();if(!B||B!=C->GetBodyInstance(NAME_None,false)||!B->IsValidBodyInstance())continue;++Wheels;
        const FBox Bounds=B->GetBodyBounds();if(!Bounds.IsValid||Bounds.Min.ContainsNaN()||Bounds.Max.ContainsNaN())return false;
        for(double X:{Bounds.Min.X,Bounds.Max.X})for(double Y:{Bounds.Min.Y,Bounds.Max.Y})
        {const FVector D=FVector(X,Y,CourseOrigin.Z)-CourseOrigin;const double A=FVector::DotProduct(D,Axis),L=FVector::DotProduct(D,Outward);if(FMath::Abs(A)>78||L< -78||L>138)return false;}
    }
    return Wheels==6;
}
AMAYAV01CurbTrial::EOutcome AMAYAV01CurbTrial::AdvanceBoostPower(const FMAYADeliveryRobotObservation& O)
{
    const double Now=GetWorld()->GetTimeSeconds(),Age=Now-StepWorldS;const auto S=Director->Snapshot();const FVector P=O.ChassisPose.GetLocation(),D=P-CourseOrigin;
    const double Along=FVector::DotProduct(D,Outward),Across=FVector::DotProduct(D,Axis);const FRotator R=O.ChassisPose.Rotator();const auto* Parcel=Director->GetParcel(0);
    if(!O.bKnown||!O.bReady||!O.Fault.IsEmpty()||O.ResetGeneration!=ResetGeneration||S.JobInstanceId!=JobId||S.Phase!=EMAYADeliveryPhase::Carrying||Director->GetSecuredParcelCount()!=1
        ||!Parcel||!Parcel->GetParcelBody()||Parcel->GetParcelId()!=OriginalParcelId||Parcel->GetPathName()!=OriginalParcelActor||Parcel->GetParcelBody()->GetPathName()!=OriginalParcelBody
        ||!Robot->AreDeliveryDoorsClosedAndPhysicallyLocked()||O.VelocityCmps.ContainsNaN()||O.AngularVelocityRadps.ContainsNaN()
        ||!FMath::IsFinite(R.Roll)||!FMath::IsFinite(R.Pitch)||FMath::Abs(R.Roll)>20||FMath::Abs(R.Pitch)>50||P.Z<40||P.Z>UpperZ+40
        ||FMath::Abs(Across)>30||Along< -80||Along>370||O.VelocityCmps.Size()>150||Now-StartWorldS>120||S.CargoConditionPercent<80)
    {
        auto FailedGuard=MakeShared<FJsonObject>();
        FailedGuard->SetStringField(TEXT("scope"),TEXT("Same-instant failed ascent guard; captured before Fail requests native brake. Earlier successful containment samples are not the failing pose."));
        FailedGuard->SetNumberField(TEXT("worldSeconds"),Now);FailedGuard->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));
        FailedGuard->SetArrayField(TEXT("actualChassisCm"),MayaCityCurb::Vec(P));FailedGuard->SetArrayField(TEXT("actualVelocityCmps"),MayaCityCurb::Vec(O.VelocityCmps));
        FailedGuard->SetNumberField(TEXT("actualAcrossCm"),Across);FailedGuard->SetNumberField(TEXT("actualAlongCm"),Along);
        FailedGuard->SetNumberField(TEXT("actualSpeedCmps"),O.VelocityCmps.Size());FailedGuard->SetNumberField(TEXT("actualTrialAgeSeconds"),Now-StartWorldS);
        FailedGuard->SetNumberField(TEXT("rollDegrees"),R.Roll);FailedGuard->SetNumberField(TEXT("pitchDegrees"),R.Pitch);
        FailedGuard->SetNumberField(TEXT("actualHeightCm"),P.Z);FailedGuard->SetNumberField(TEXT("maximumHeightCm"),UpperZ+40);
        FailedGuard->SetBoolField(TEXT("known"),O.bKnown);FailedGuard->SetBoolField(TEXT("ready"),O.bReady);FailedGuard->SetStringField(TEXT("fault"),O.Fault);
        FailedGuard->SetBoolField(TEXT("sameReset"),O.ResetGeneration==ResetGeneration);FailedGuard->SetBoolField(TEXT("sameJob"),S.JobInstanceId==JobId);
        FailedGuard->SetBoolField(TEXT("carrying"),S.Phase==EMAYADeliveryPhase::Carrying);FailedGuard->SetNumberField(TEXT("securedCount"),Director->GetSecuredParcelCount());
        FailedGuard->SetBoolField(TEXT("sameConfiguredParcel"),Parcel&&Parcel->GetParcelBody()&&Parcel->GetParcelId()==OriginalParcelId&&Parcel->GetPathName()==OriginalParcelActor&&Parcel->GetParcelBody()->GetPathName()==OriginalParcelBody);
        FailedGuard->SetBoolField(TEXT("nativeDoorsLocked"),Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());FailedGuard->SetNumberField(TEXT("conditionPercent"),S.CargoConditionPercent);
        LastContainmentEvidenceJson=MayaCityCurb::Json(FailedGuard);
        Save(O,true);return Fail(TEXT("Actual reverse-ascent identity/custody/native/120s/roll20/pitch50/height/30cm lateral/150cmps containment refused"));
    }
    const double Error=FMath::FindDeltaAngleDegrees((-O.ChassisPose.GetUnitAxis(EAxis::X)).Rotation().Yaw,Outward.Rotation().Yaw);
    if(!FMath::IsFinite(Error)||FMath::Abs(Error)>15)return Fail(TEXT("Actual reverse-ascent retained outward nose left15degree heading envelope"));
    auto Evidence=MakeShared<FJsonObject>();Evidence->SetNumberField(TEXT("worldSeconds"),Now);Evidence->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));Evidence->SetArrayField(TEXT("actualChassisCm"),MayaCityCurb::Vec(P));Evidence->SetNumberField(TEXT("alongOutwardCm"),Along);Evidence->SetNumberField(TEXT("rollDegrees"),R.Roll);Evidence->SetNumberField(TEXT("pitchDegrees"),R.Pitch);Evidence->SetNumberField(TEXT("supportMask"),O.SupportMask);Evidence->SetBoolField(TEXT("normalStallQualified"),bNormalStallQualified);Evidence->SetNumberField(TEXT("batteryPercent"),S.BatteryPercent);Evidence->SetNumberField(TEXT("boostMotorEffortFraction"),O.BoostMotorEffortFraction);Evidence->SetStringField(TEXT("actualNativeTelemetry"),Robot->ReadTelemetryJson());LastContainmentEvidenceJson=MayaCityCurb::Json(Evidence);
    // Director TG_PostPhysics has consumed this actual observation; this helper
    // runs TG_PostUpdateWork. Retain both endpoints rather than infer energy
    // from a later moving sample or from Shift intent.
    const bool StallBoostEligible=Step==EStep::BoostAscent&&O.bBoostDriveActive
        &&FMath::IsFinite(O.BoostMotorEffortFraction)&&O.BoostMotorEffortFraction>0
        &&O.VelocityCmps.Size()<5&&NativeCurbContactAndMotor()&&!S.bCharging;
    const double EnergyDt=LastBoostEnergyWorldS>=0?Now-LastBoostEnergyWorldS:0;
    const double EnergyLoss=LastBoostEnergyBattery-S.BatteryPercent;
    const bool StallExtraEnergy=StallBoostEligible&&bPreviousBoostStallEligible
        &&EnergyDt>0&&EnergyDt<=.25&&EnergyLoss>.01*EnergyDt+1.e-8;
    bBoostBatteryLossObserved|=StallExtraEnergy;
    const FString CurrentNative=Robot->ReadTelemetryJson();
    // The Director observation is the actual operand consumed by Core this frame.
    // The later helper pose/native JSON remains a separately timed observation.
    const auto& CoreObservation=Director->Observation();
    const double CoreSpeed=CoreObservation.VelocityCmps.Size();
    TSharedPtr<FJsonObject> EnergyNative,EnergyPT;FString EnergyPTText;
    const bool EnergyPTParsed=FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CurrentNative),EnergyNative)&&EnergyNative
        &&EnergyNative->TryGetStringField(TEXT("nativeWheelFeedbackEvidence"),EnergyPTText)
        &&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(EnergyPTText),EnergyPT)&&EnergyPT;
    auto PTFlag=[&](const TCHAR* Name){bool Value=false;return EnergyPTParsed&&EnergyPT->TryGetBoolField(Name,Value)&&Value;};
    bool LegacyExternal=false;if(EnergyPTParsed)EnergyPT->TryGetBoolField(TEXT("legacyExternalSelected"),LegacyExternal);
    double PTEffort=0;if(EnergyPTParsed)EnergyPT->TryGetNumberField(TEXT("actualSameStepEffortFraction"),PTEffort);
    const TArray<TSharedPtr<FJsonValue>>* EnergyWheels=nullptr;
    bool AppliedPairs=EnergyPTParsed&&EnergyPT->TryGetArrayField(TEXT("wheels"),EnergyWheels)&&EnergyWheels&&EnergyWheels->Num()==6;
    double AppliedMotorAbsSum=0;TArray<TSharedPtr<FJsonValue>> EnergyMotorReceipts;
    if(AppliedPairs)for(int32 I=0;I<6;++I)
    {
        const auto Wheel=(*EnergyWheels)[I]->AsObject();bool Known=false;double Nm=0;
        const bool FiniteApplied=Wheel&&Wheel->TryGetBoolField(TEXT("actualPairApplicationKnown"),Known)&&Known
            &&Wheel->TryGetNumberField(TEXT("actualAppliedMotorNm"),Nm)&&FMath::IsFinite(Nm);
        AppliedPairs&=FiniteApplied;if(FiniteApplied)AppliedMotorAbsSum+=FMath::Abs(Nm);
        auto Motor=MakeShared<FJsonObject>();Motor->SetNumberField(TEXT("wheel"),I);Motor->SetBoolField(TEXT("actualPairApplicationKnown"),FiniteApplied);
        if(FiniteApplied)Motor->SetNumberField(TEXT("actualAppliedMotorNm"),Nm);EnergyMotorReceipts.Add(MakeShared<FJsonValueObject>(Motor));
    }
    const bool MatchingProductionPT=PTFlag(TEXT("productionCallbackRegistered"))&&!LegacyExternal&&PTFlag(TEXT("matchingActualOutputKnown"))
        &&PTFlag(TEXT("ownerCurrentAtApplication"))&&PTFlag(TEXT("nineBodiesBound"))&&PTFlag(TEXT("eightJointIdentities"))
        &&PTFlag(TEXT("nativeStateFinite"))&&PTFlag(TEXT("actualClosedLocksAndPose"))&&PTFlag(TEXT("nativeBrakeReadbackVerified"))
        &&PTFlag(TEXT("freshAtApplication"))&&PTFlag(TEXT("driveAdmittedAtApplication"))&&PTFlag(TEXT("boostActiveAtApplication"))
        &&AppliedPairs&&FMath::IsFinite(AppliedMotorAbsSum)&&AppliedMotorAbsSum>0&&FMath::IsFinite(PTEffort)&&PTEffort>0;
    const bool CoreClockMatchesCurrentWorld=FMath::IsFinite(CoreObservation.ObservationWorldSeconds)&&CoreObservation.ObservationWorldSeconds==Now;
    const bool CoreIdentity=CoreObservation.ResetGeneration==ResetGeneration&&S.JobInstanceId==JobId&&S.Phase==EMAYADeliveryPhase::Carrying
        &&CoreObservation.bSecuredParcelCountKnown&&CoreObservation.SecuredParcelCount==1&&Director->GetSecuredParcelCount()==1;
    const bool CoreStateEligible=CoreObservation.bKnown&&CoreObservation.bReady&&CoreObservation.Fault.IsEmpty()
        &&CoreObservation.bBoostDriveActive&&CoreObservation.bFreshMovementIntent
        &&FMath::IsFinite(CoreObservation.BoostMotorEffortFraction)&&CoreObservation.BoostMotorEffortFraction>0
        &&!CoreObservation.VelocityCmps.ContainsNaN()&&FMath::IsFinite(CoreSpeed)&&CoreSpeed>=20;
    const bool SnapshotEnergyEligible=S.bShiftStarted&&!S.bShiftWon&&!S.bDroneTotalled&&!S.bVehicleContactPending&&!S.bCharging
        &&FMath::IsFinite(S.BatteryPercent)&&S.BatteryPercent>0&&FMath::IsFinite(S.ElapsedSeconds)&&FMath::IsFinite(Now);
    const bool MovingCurbContactAndMotor=Step==EStep::BoostAscent&&NativeCurbContactAndMotor();
    const bool MovingBoostEligible=Step==EStep::BoostAscent&&CoreClockMatchesCurrentWorld&&CoreIdentity&&CoreStateEligible
        &&SnapshotEnergyEligible&&MatchingProductionPT&&MovingCurbContactAndMotor;
    const bool ConsecutiveMovingFrames=bPreviousMovingBoostEligible&&LastMovingBoostEnergyFrame!=MAX_uint64&&GFrameCounter==LastMovingBoostEnergyFrame+1;
    const double CoreEnergyDt=LastMovingBoostEnergyElapsedS>=0?S.ElapsedSeconds-LastMovingBoostEnergyElapsedS:0;
    const double MovingWorldDt=LastMovingBoostEnergyWorldS>=0?Now-LastMovingBoostEnergyWorldS:0;
    const double MovingEnergyLoss=LastMovingBoostEnergyBattery-S.BatteryPercent;
    const double OldCorrespondingMovingRate=FMath::IsFinite(CoreSpeed)?(.01+.54*FMath::Clamp(CoreSpeed/200.,0.,3.)):0;
    const double CurrentMovingRate=1.2*OldCorrespondingMovingRate;
    const double ExpectedMovingLoss=CurrentMovingRate*CoreEnergyDt;
    const double MovingLossError=MovingEnergyLoss-ExpectedMovingLoss;
    const bool MovingIntervalClocks=FMath::IsFinite(CoreEnergyDt)&&CoreEnergyDt>0&&CoreEnergyDt<=.25
        &&FMath::IsFinite(MovingWorldDt)&&FMath::Abs(CoreEnergyDt-MovingWorldDt)<=1.e-6;
    const bool MovingExtraEnergy=MovingBoostEligible&&bPreviousMovingBoostEligible&&ConsecutiveMovingFrames&&MovingIntervalClocks
        &&FMath::IsFinite(MovingEnergyLoss)&&MovingEnergyLoss>0&&FMath::IsFinite(ExpectedMovingLoss)&&ExpectedMovingLoss>0
        &&LastMovingBoostEnergyBattery-ExpectedMovingLoss>0&&S.BatteryPercent>0
        &&FMath::IsFinite(MovingLossError)&&FMath::Abs(MovingLossError)<=1.e-6&&MovingEnergyLoss>.01*CoreEnergyDt+1.e-8;
    bMovingBoostEnergyQualified|=MovingExtraEnergy;if(MovingExtraEnergy)++MovingBoostEnergyQualifiedIntervalCount;
    auto MovingEndpoint=MakeShared<FJsonObject>();MovingEndpoint->SetNumberField(TEXT("frame"),double(GFrameCounter));
    MovingEndpoint->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));MovingEndpoint->SetStringField(TEXT("jobInstanceId"),S.JobInstanceId);
    MovingEndpoint->SetNumberField(TEXT("resetGeneration"),double(CoreObservation.ResetGeneration));MovingEndpoint->SetStringField(TEXT("sameOriginalParcelId"),OriginalParcelId);
    MovingEndpoint->SetStringField(TEXT("sameOriginalParcelActor"),OriginalParcelActor);MovingEndpoint->SetStringField(TEXT("sameOriginalParcelBody"),OriginalParcelBody);
    MovingEndpoint->SetNumberField(TEXT("worldSeconds"),Now);if(FMath::IsFinite(CoreObservation.ObservationWorldSeconds))MovingEndpoint->SetNumberField(TEXT("coreObservationWorldSeconds"),CoreObservation.ObservationWorldSeconds);
    if(FMath::IsFinite(S.ElapsedSeconds))MovingEndpoint->SetNumberField(TEXT("coreElapsedSeconds"),S.ElapsedSeconds);if(FMath::IsFinite(S.BatteryPercent))MovingEndpoint->SetNumberField(TEXT("batteryPercent"),S.BatteryPercent);
    MovingEndpoint->SetBoolField(TEXT("coreClockMatchesCurrentWorld"),CoreClockMatchesCurrentWorld);MovingEndpoint->SetBoolField(TEXT("coreIdentityMatchesOneParcelCarrying"),CoreIdentity);
    MovingEndpoint->SetBoolField(TEXT("coreKnown"),CoreObservation.bKnown);MovingEndpoint->SetBoolField(TEXT("coreReady"),CoreObservation.bReady);
    MovingEndpoint->SetStringField(TEXT("coreFault"),CoreObservation.Fault);MovingEndpoint->SetBoolField(TEXT("coreBoostDriveActive"),CoreObservation.bBoostDriveActive);
    MovingEndpoint->SetBoolField(TEXT("coreFreshMovementIntent"),CoreObservation.bFreshMovementIntent);MovingEndpoint->SetBoolField(TEXT("coreDoorsClosedLatched"),CoreObservation.bDoorsClosedLatched);
    MovingEndpoint->SetBoolField(TEXT("coreSecuredParcelCountKnown"),CoreObservation.bSecuredParcelCountKnown);MovingEndpoint->SetNumberField(TEXT("coreSecuredParcelCount"),CoreObservation.SecuredParcelCount);
    MovingEndpoint->SetBoolField(TEXT("shiftStarted"),S.bShiftStarted);MovingEndpoint->SetBoolField(TEXT("shiftWon"),S.bShiftWon);
    MovingEndpoint->SetBoolField(TEXT("droneTotalled"),S.bDroneTotalled);MovingEndpoint->SetBoolField(TEXT("vehicleContactPending"),S.bVehicleContactPending);MovingEndpoint->SetBoolField(TEXT("charging"),S.bCharging);
    MovingEndpoint->SetBoolField(TEXT("actualExistingCurbContactAndMotor"),MovingCurbContactAndMotor);MovingEndpoint->SetBoolField(TEXT("matchingCurrentProductionPT"),MatchingProductionPT);
    MovingEndpoint->SetBoolField(TEXT("movingPoweredEligible"),MovingBoostEligible);if(!CoreObservation.VelocityCmps.ContainsNaN())MovingEndpoint->SetArrayField(TEXT("coreConsumedVelocityCmps"),MayaCityCurb::Vec(CoreObservation.VelocityCmps));
    if(FMath::IsFinite(CoreSpeed))MovingEndpoint->SetNumberField(TEXT("coreConsumedSpeedCmps"),CoreSpeed);
    if(FMath::IsFinite(CoreObservation.BoostMotorEffortFraction))MovingEndpoint->SetNumberField(TEXT("coreConsumedBoostMotorEffortFraction"),CoreObservation.BoostMotorEffortFraction);
    auto MovingPT=MakeShared<FJsonObject>();MovingPT->SetBoolField(TEXT("matchingCurrentProductionPT"),MatchingProductionPT);
    for(const TCHAR* Name:{TEXT("generation"),TEXT("revision"),TEXT("consumedCommandSequence"),TEXT("actualCallbackSequence"),TEXT("callbackStartSimSeconds"),TEXT("callbackDtSeconds"),TEXT("actualInputSampleRealSeconds"),TEXT("gtObservationRealSeconds"),TEXT("actualSameStepEffortFraction"),TEXT("selectedPeakNm")})
    {double Value=0;if(EnergyPTParsed&&EnergyPT->TryGetNumberField(Name,Value)&&FMath::IsFinite(Value))MovingPT->SetNumberField(Name,Value);}
    for(const TCHAR* Name:{TEXT("productionCallbackRegistered"),TEXT("legacyExternalSelected"),TEXT("matchingActualOutputKnown"),TEXT("ownerCurrentAtApplication"),TEXT("nineBodiesBound"),TEXT("eightJointIdentities"),TEXT("nativeStateFinite"),TEXT("actualClosedLocksAndPose"),TEXT("nativeBrakeReadbackVerified"),TEXT("freshAtApplication"),TEXT("driveAdmittedAtApplication"),TEXT("boostActiveAtApplication")})
    {bool Value=false;if(EnergyPTParsed&&EnergyPT->TryGetBoolField(Name,Value))MovingPT->SetBoolField(Name,Value);}
    MovingPT->SetArrayField(TEXT("actualMotorPairs"),EnergyMotorReceipts);MovingEndpoint->SetObjectField(TEXT("actualProductionPT"),MovingPT);
    auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("worldSeconds"),Now);Row->SetNumberField(TEXT("platformRealSeconds"),FPlatformTime::Seconds());
    Row->SetNumberField(TEXT("frame"),double(GFrameCounter));Row->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));
    Row->SetArrayField(TEXT("actualChassisCm"),MayaCityCurb::Vec(P));Row->SetArrayField(TEXT("velocityCmps"),MayaCityCurb::Vec(O.VelocityCmps));
    Row->SetNumberField(TEXT("normalStallStartWorldSeconds"),StallStartWorldS);Row->SetNumberField(TEXT("normalStallAnchorAlongCm"),StallAnchorAlongCm);
    Row->SetNumberField(TEXT("currentBatteryPercent"),S.BatteryPercent);Row->SetNumberField(TEXT("previousWorldSeconds"),LastBoostEnergyWorldS);
    Row->SetNumberField(TEXT("previousBatteryPercent"),LastBoostEnergyBattery);Row->SetNumberField(TEXT("observedIntervalSeconds"),EnergyDt);Row->SetNumberField(TEXT("actualIntervalBatteryLoss"),EnergyLoss);
    Row->SetBoolField(TEXT("currentNativeStallBoostEligible"),StallBoostEligible);Row->SetBoolField(TEXT("previousNativeStallBoostEligible"),bPreviousBoostStallEligible);
    Row->SetBoolField(TEXT("qualifiedNativeStallExtraEnergy"),StallExtraEnergy);Row->SetStringField(TEXT("actualCurrentNativeTelemetry"),CurrentNative);
    if(StallExtraEnergy)Row->SetStringField(TEXT("actualPreviousNativeTelemetry"),LastBoostEnergyNative);
    Row->SetObjectField(TEXT("movingPoweredBoostCurrentEndpoint"),MovingEndpoint);
    if(LastMovingBoostEnergyEndpoint)Row->SetObjectField(TEXT("movingPoweredBoostPreviousEndpoint"),LastMovingBoostEnergyEndpoint);
    Row->SetBoolField(TEXT("previousMovingPoweredBoostEligible"),bPreviousMovingBoostEligible);Row->SetBoolField(TEXT("movingPoweredBoostConsecutiveHelperFrames"),ConsecutiveMovingFrames);
    Row->SetNumberField(TEXT("movingPoweredBoostCoreIntervalSeconds"),CoreEnergyDt);Row->SetNumberField(TEXT("movingPoweredBoostObservedWorldIntervalSeconds"),MovingWorldDt);
    Row->SetBoolField(TEXT("movingPoweredBoostIntervalClockAgreement"),MovingIntervalClocks);
    Row->SetNumberField(TEXT("movingPoweredBoostObservedBatteryLoss"),MovingEnergyLoss);Row->SetNumberField(TEXT("movingPoweredBoostOldCorrespondingRatePpPerSecond"),OldCorrespondingMovingRate);
    Row->SetNumberField(TEXT("movingPoweredBoostExpectedCurrentRatePpPerSecond"),CurrentMovingRate);Row->SetNumberField(TEXT("movingPoweredBoostExpectedBatteryLoss"),ExpectedMovingLoss);
    Row->SetNumberField(TEXT("movingPoweredBoostBatteryLossError"),MovingLossError);Row->SetBoolField(TEXT("qualifiedMovingPoweredBoostEnergyInterval"),MovingExtraEnergy);
    Row->SetBoolField(TEXT("movingPoweredBoostEnergyQualified"),bMovingBoostEnergyQualified);Row->SetBoolField(TEXT("ascentPoweredEnergyQualified"),bBoostBatteryLossObserved||bMovingBoostEnergyQualified);
    Row->SetStringField(TEXT("movingPoweredBoostEnergyScope"),TEXT("Current Director observation consumed by Core; adjacent same-carrying elapsed delta is actual Core dt, consecutive helper frames and same-frame world stamps. Speed>=20 makes unchanged effort floor zero. Actual battery loss matches current1.2*(.01+.54*clamp(v/200,0,3))*dt within1e-6pp, old corresponding-speed rate is arithmetic only. Current helper pose/PT native JSON retain their separate clocks; no old curb battery trial or electrical-power claim."));
    LastMovingBoostEnergyFrame=GFrameCounter;LastMovingBoostEnergyWorldS=Now;LastMovingBoostEnergyElapsedS=S.ElapsedSeconds;
    LastMovingBoostEnergyBattery=S.BatteryPercent;bPreviousMovingBoostEligible=MovingBoostEligible;LastMovingBoostEnergyEndpoint=MovingEndpoint;
    Row->SetStringField(TEXT("actualCustody"),Director->ReadCargoInventoryEvidenceJson());Row->SetBoolField(TEXT("nativePhysicalLocks"),Robot->AreDeliveryDoorsClosedAndPhysicallyLocked());
    ++BoostPowerObservationCount;if(BoostPowerObservations.Num()<4096)BoostPowerObservations.Add(MakeShared<FJsonValueObject>(Row));
    LastBoostEnergyWorldS=Now;LastBoostEnergyBattery=S.BatteryPercent;bPreviousBoostStallEligible=StallBoostEligible;LastBoostEnergyNative=CurrentNative;
    if(O.LastNativeImpact.Generation>LastSavedLandingGeneration&&O.LastNativeImpact.Kind==TEXT("CurbLanding"))
    {Landings.Add(MakeShared<FJsonValueObject>(MAYADeliveryImpactEventToJson(O.LastNativeImpact)));LastSavedLandingGeneration=O.LastNativeImpact.Generation;}
    Save(O);
    // Existing native/motor/velocity strings above precede this decision. Only
    // compact accumulator state is written after evaluating this observation.
    auto WriteBlockedState=[&]()
    {
        Row->SetNumberField(TEXT("normalBlockedCandidateStartWorldSeconds"),BlockedStartWorldS);
        Row->SetNumberField(TEXT("normalBlockedCandidateLastWorldSeconds"),BlockedLastWorldS);
        Row->SetNumberField(TEXT("normalBlockedCandidateDurationSeconds"),BlockedSampleCount>0?BlockedLastWorldS-BlockedStartWorldS:0);
        Row->SetNumberField(TEXT("normalBlockedCandidateSampleCount"),BlockedSampleCount);
        Row->SetNumberField(TEXT("normalBlockedCandidateResetCount"),BlockedResetCount);
        Row->SetArrayField(TEXT("normalBlockedCandidateMinCm"),MayaCityCurb::Vec(BlockedMinCm));
        Row->SetArrayField(TEXT("normalBlockedCandidateMaxCm"),MayaCityCurb::Vec(BlockedMaxCm));
        Row->SetNumberField(TEXT("normalBlockedCandidateDiagonalCm"),(BlockedMaxCm-BlockedMinCm).Size());
        Row->SetBoolField(TEXT("normalBlockedQualified"),bNormalBlockedQualified);
        Row->SetBoolField(TEXT("normalQuietStallQualifiedAfterCurrentDecision"),bNormalStallQualified);
    };
    Row->SetStringField(TEXT("normalQuietStallStartAnchorTiming"),TEXT("Existing start/anchor above are before the current NormalAscent decision"));
    Row->SetBoolField(TEXT("normalBlockedDecisionEvaluated"),false);
    Row->SetStringField(TEXT("normalBlockedCandidateStateTiming"),TEXT("Candidate unchanged; no current NormalAscent accumulator decision"));
    WriteBlockedState();
    if(Step==EStep::NormalAscent)
    {
        if(Age>8)return Fail(TEXT("Normal reverse ascent did not qualify powered bounded no-progress within8seconds"));
        if(O.SupportMask==63&&NativeUpperWheelFootprint())return Fail(TEXT("Normal motor already climbed this original curb; no boost-only capability claim"));
        Command(-1.f,float(FMath::Clamp(Error/40.,-.35,.35)),false,false);
        const bool Stalled=O.bFreshMovementIntent&&!O.bBoostDriveActive&&O.VelocityCmps.Size()<5&&NativeCurbContactAndMotor();
        if(!Stalled||FMath::Abs(Along-StallAnchorAlongCm)>=5){StallStartWorldS=-1;StallAnchorAlongCm=Along;}
        else{if(StallStartWorldS<0)StallStartWorldS=Now;if(Now-StallStartWorldS>=2){bNormalStallQualified=true;NormalStallPoseCm=P;}}
        const bool FinitePositionClock=FMath::IsFinite(P.X)&&FMath::IsFinite(P.Y)&&FMath::IsFinite(P.Z)&&FMath::IsFinite(Now)
            &&(BlockedSampleCount==0||Now>=BlockedLastWorldS);
        const bool ContactAndMotor=NativeCurbContactAndMotor();
        const bool Eligible=FinitePositionClock&&O.bFreshMovementIntent&&!O.bBoostDriveActive&&ContactAndMotor;
        Row->SetBoolField(TEXT("normalBlockedDecisionEvaluated"),true);
        Row->SetBoolField(TEXT("normalBlockedEligible"),Eligible);
        Row->SetBoolField(TEXT("normalBlockedFinitePositionClock"),FinitePositionClock);
        Row->SetBoolField(TEXT("normalBlockedFreshMovementIntent"),O.bFreshMovementIntent);
        Row->SetBoolField(TEXT("normalBlockedBoostInactive"),!O.bBoostDriveActive);
        Row->SetBoolField(TEXT("normalBlockedActualCurbContactAndMotor"),ContactAndMotor);
        Row->SetBoolField(TEXT("normalBlockedCandidateResetThisObservation"),false);
        Row->SetStringField(TEXT("normalBlockedCandidateResetReason"),TEXT("None"));
        auto SeedBlocked=[&]() {BlockedStartWorldS=BlockedLastWorldS=Now;BlockedSampleCount=1;BlockedMinCm=BlockedMaxCm=P;};
        if(!Eligible)
        {
            Row->SetArrayField(TEXT("normalBlockedRejectedMinCm"),MayaCityCurb::Vec(BlockedMinCm));
            Row->SetArrayField(TEXT("normalBlockedRejectedMaxCm"),MayaCityCurb::Vec(BlockedMaxCm));
            Row->SetNumberField(TEXT("normalBlockedRejectedDiagonalCm"),(BlockedMaxCm-BlockedMinCm).Size());
            Row->SetNumberField(TEXT("normalBlockedRejectedDurationSeconds"),BlockedSampleCount>0?BlockedLastWorldS-BlockedStartWorldS:0);
            Row->SetNumberField(TEXT("normalBlockedRejectedSampleCount"),BlockedSampleCount);
            Row->SetBoolField(TEXT("normalBlockedCandidateResetThisObservation"),BlockedSampleCount>0);
            Row->SetStringField(TEXT("normalBlockedCandidateResetReason"),FinitePositionClock?TEXT("ActualNormalMovementOrCurbContactMotorIneligible"):TEXT("NonfinitePositionOrClockOrClockWentBackwards"));
            if(BlockedSampleCount>0)++BlockedResetCount;
            BlockedStartWorldS=BlockedLastWorldS=-1;BlockedSampleCount=0;BlockedMinCm=BlockedMaxCm=FVector::ZeroVector;
        }
        else if(BlockedSampleCount==0)
        {SeedBlocked();Row->SetStringField(TEXT("normalBlockedCandidateResetReason"),TEXT("StartedAtCurrentEligibleObservation"));}
        else
        {
            BlockedMinCm=FVector(FMath::Min(BlockedMinCm.X,P.X),FMath::Min(BlockedMinCm.Y,P.Y),FMath::Min(BlockedMinCm.Z,P.Z));
            BlockedMaxCm=FVector(FMath::Max(BlockedMaxCm.X,P.X),FMath::Max(BlockedMaxCm.Y,P.Y),FMath::Max(BlockedMaxCm.Z,P.Z));
            BlockedLastWorldS=Now;++BlockedSampleCount;
            if((BlockedMaxCm-BlockedMinCm).Size()>=5)
            {
                Row->SetArrayField(TEXT("normalBlockedRejectedMinCm"),MayaCityCurb::Vec(BlockedMinCm));
                Row->SetArrayField(TEXT("normalBlockedRejectedMaxCm"),MayaCityCurb::Vec(BlockedMaxCm));
                Row->SetNumberField(TEXT("normalBlockedRejectedDiagonalCm"),(BlockedMaxCm-BlockedMinCm).Size());
                Row->SetNumberField(TEXT("normalBlockedRejectedDurationSeconds"),Now-BlockedStartWorldS);
                Row->SetNumberField(TEXT("normalBlockedRejectedSampleCount"),BlockedSampleCount);
                Row->SetBoolField(TEXT("normalBlockedCandidateResetThisObservation"),true);
                Row->SetStringField(TEXT("normalBlockedCandidateResetReason"),TEXT("Observed3DBoundsDiagonalReached5cm_RestartAtCurrentEligibleObservation"));
                ++BlockedResetCount;SeedBlocked();
            }
        }
        if(Eligible&&BlockedSampleCount>0&&Now-BlockedStartWorldS>=2&&(BlockedMaxCm-BlockedMinCm).Size()<5)
        {
            bNormalBlockedQualified=true;NormalBlockedPoseCm=P;NormalBlockedMinCm=BlockedMinCm;NormalBlockedMaxCm=BlockedMaxCm;
            NormalBlockedDurationS=Now-BlockedStartWorldS;NormalBlockedDiagonalCm=(BlockedMaxCm-BlockedMinCm).Size();NormalBlockedQualifiedSampleCount=BlockedSampleCount;
            Command(0,0,true);Enter(EStep::BrakeBeforeBoost,Now);
        }
        Row->SetStringField(TEXT("normalBlockedCandidateStateTiming"),TEXT("After current observed NormalAscent accumulator decision; native telemetry above remains before command/cache readback"));
        WriteBlockedState();
    }
    else if(Step==EStep::BrakeBeforeBoost)
    {
        Command(0,0,true);if(Age>4)return Fail(TEXT("Actual normal-blocked brake did not settle within4seconds"));
        const bool Rest=O.VelocityCmps.Size()<=4&&O.AngularVelocityRadps.Size()<=.15&&O.bDoorsClosedLatched;
        if(Rest){if(StableWorldS<0)StableWorldS=Now;if(Now-StableWorldS>=.5){BoostStartBattery=S.BatteryPercent;Enter(EStep::BoostAscent,Now);}}else StableWorldS=-1;
    }
    else if(Step==EStep::BoostAscent)
    {
        if(Age>15)return Fail(TEXT("New boosted reverse ascent did not reach whole upper wheel footprint within15seconds"));
        // Old near-stall and separate moving-powered energy are qualified above from their own actual operands.
        TSharedPtr<FJsonObject> UpperNative;const TArray<TSharedPtr<FJsonValue>>* UpperParts=nullptr;
        bool AllUpperPaths=FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CurrentNative),UpperNative)&&UpperNative
            &&UpperNative->TryGetArrayField(TEXT("parts"),UpperParts)&&UpperParts&&UpperParts->Num()>=7;
        bool AllUpperContacts=AllUpperPaths;TArray<TSharedPtr<FJsonValue>> UpperContacts;
        if(AllUpperPaths)for(int32 I=1;I<=6;++I)
        {
            const auto Part=(*UpperParts)[I]->AsObject();FString Support;
            const bool Known=Part&&Part->TryGetStringField(TEXT("nativeHitSupport"),Support);
            const bool InUpper=Known&&UpperSupports.Contains(Support);AllUpperPaths&=InUpper;
            auto Contact=MakeShared<FJsonObject>();Contact->SetNumberField(TEXT("wheel"),I-1);
            Contact->SetStringField(TEXT("support"),Support);Contact->SetBoolField(TEXT("supportPathKnown"),Known);
            Contact->SetBoolField(TEXT("supportPathInOriginalUpperSet"),InUpper);
            bool Accepted=false;if(Part)Part->TryGetBoolField(TEXT("nativeObservedSupportAccepted"),Accepted);
            AllUpperContacts&=InUpper&&Accepted;
            Contact->SetBoolField(TEXT("nativeObservedSupportAccepted"),Accepted);
            UpperContacts.Add(MakeShared<FJsonValueObject>(Contact));
        }
        Row->SetBoolField(TEXT("observedAllSixOriginalUpperSupportPaths"),AllUpperPaths);
        Row->SetBoolField(TEXT("observedAllSixAcceptedOriginalUpperContacts"),AllUpperContacts);
        Row->SetArrayField(TEXT("observedUpperSupportContacts"),UpperContacts);
        Row->SetBoolField(TEXT("upperSupportCoastLatchedBeforeCurrentDecision"),bUpperSupportCoastLatched);
        const bool WholeUpperFootprint=O.SupportMask==63&&NativeUpperWheelFootprint();
        if(!WholeUpperFootprint&&!bUpperSupportCoastLatched&&O.SupportMask==63&&AllUpperContacts)
        {
            bUpperSupportCoastLatched=true;UpperSupportCoastWorldS=Now;UpperSupportCoastPoseCm=P;
            UpperSupportCoastReceipt=MakeShared<FJsonObject>();UpperSupportCoastReceipt->SetNumberField(TEXT("worldSeconds"),Now);
            UpperSupportCoastReceipt->SetNumberField(TEXT("platformRealSeconds"),FPlatformTime::Seconds());
            UpperSupportCoastReceipt->SetNumberField(TEXT("frame"),double(GFrameCounter));
            UpperSupportCoastReceipt->SetNumberField(TEXT("elapsedOriginalBoostPhaseSeconds"),Age);
            double CapturedRealS=0;if(UpperNative->TryGetNumberField(TEXT("capturedRealS"),CapturedRealS)&&FMath::IsFinite(CapturedRealS))
                UpperSupportCoastReceipt->SetNumberField(TEXT("nativeCapturedRealSeconds"),CapturedRealS);
            UpperSupportCoastReceipt->SetArrayField(TEXT("actualChassisCm"),MayaCityCurb::Vec(P));
            UpperSupportCoastReceipt->SetArrayField(TEXT("actualVelocityCmps"),MayaCityCurb::Vec(O.VelocityCmps));
            UpperSupportCoastReceipt->SetNumberField(TEXT("actualSpeedCmps"),O.VelocityCmps.Size());
            UpperSupportCoastReceipt->SetNumberField(TEXT("supportMask"),O.SupportMask);
            UpperSupportCoastReceipt->SetArrayField(TEXT("actualUpperSupportContacts"),UpperContacts);
            UpperSupportCoastReceipt->SetStringField(TEXT("latchReason"),TEXT("Typed support63 plus six captured accepted original upper contact identities; unchanged full native footprint not yet true"));
            UpperSupportCoastReceipt->SetNumberField(TEXT("requestedThrottle"),0);UpperSupportCoastReceipt->SetNumberField(TEXT("requestedSteer"),0);
            UpperSupportCoastReceipt->SetBoolField(TEXT("requestedBrake"),false);UpperSupportCoastReceipt->SetBoolField(TEXT("requestedBoost"),false);
            UpperSupportCoastReceipt->SetStringField(TEXT("timingScope"),TEXT("Native pose/speed/support capture before this zero-input coast request; actual applied cached motor evidence remains at its own earlier decision timestamp"));
        }
        Row->SetBoolField(TEXT("actualWholeUpperWheelFootprint"),WholeUpperFootprint);
        Row->SetBoolField(TEXT("upperSupportCoastLatchedAfterCurrentDecision"),bUpperSupportCoastLatched);
        Row->SetNumberField(TEXT("upperSupportCoastLatchedWorldSeconds"),UpperSupportCoastWorldS);
        const bool SpeedCoastBefore=bAscentSpeedCoast;const double ActualAscentSpeed=O.VelocityCmps.Size();
        FString SpeedCoastDecision=TEXT("EndpointOrPermanentUpperCoastPriority");
        if(!WholeUpperFootprint&&!bUpperSupportCoastLatched)
        {
            if(!bAscentSpeedCoast&&ActualAscentSpeed>=90){bAscentSpeedCoast=true;SpeedCoastDecision=TEXT("Actual3DSpeedAtLeast90_LatchZeroInputCoast");}
            else if(bAscentSpeedCoast&&ActualAscentSpeed<=65){bAscentSpeedCoast=false;SpeedCoastDecision=TEXT("Actual3DSpeedAtMost65_ResumeOriginalPoweredCommand");}
            else SpeedCoastDecision=TEXT("RetainExisting90_65HysteresisState");
        }
        if(SpeedCoastBefore!=bAscentSpeedCoast)++AscentSpeedCoastTransitionCount;
        Row->SetNumberField(TEXT("ascentGovernorActual3DSpeedCmps"),ActualAscentSpeed);Row->SetNumberField(TEXT("ascentGovernorElapsedOriginalPhaseSeconds"),Age);
        Row->SetBoolField(TEXT("ascentSpeedCoastBeforeCurrentDecision"),SpeedCoastBefore);Row->SetBoolField(TEXT("ascentSpeedCoastAfterCurrentDecision"),bAscentSpeedCoast);
        Row->SetNumberField(TEXT("ascentSpeedCoastTransitionCount"),AscentSpeedCoastTransitionCount);Row->SetBoolField(TEXT("ascentSpeedCoastTransitionThisDecision"),SpeedCoastBefore!=bAscentSpeedCoast);
        Row->SetStringField(TEXT("ascentSpeedCoastDecision"),SpeedCoastDecision);
        float Throttle=0,Steer=0;bool Brake=false,Boost=false;
        if(WholeUpperFootprint){Brake=true;Command(0,0,true);Enter(EStep::UpperHold,Now);}
        else
        {
            if(!bUpperSupportCoastLatched&&!bAscentSpeedCoast){Throttle=-1.f;Steer=float(FMath::Clamp(Error/40.,-.35,.35));Boost=true;}
            Command(Throttle,Steer,false,Boost);
        }
        Row->SetNumberField(TEXT("diagnosticAscentRequestedThrottleAfterDecision"),Throttle);
        Row->SetNumberField(TEXT("diagnosticAscentRequestedSteerAfterDecision"),Steer);
        Row->SetBoolField(TEXT("diagnosticAscentRequestedBrakeAfterDecision"),Brake);
        Row->SetBoolField(TEXT("diagnosticAscentRequestedBoostAfterDecision"),Boost);
        Row->SetBoolField(TEXT("diagnosticAscentCommandAccepted"),bAccepted);
        Row->SetStringField(TEXT("diagnosticAscentCommandScope"),TEXT("After current observed native sample: original full footprint/support63 native brake wins; permanent six accepted upper-contact coast wins next. Otherwise actual3D speed>=90 requests zero-throttle/zero-steer/brakeOFF/Boostfalse coast until speed<=65 resumes original raw-1/heading-steer Boost. Original15s/150cmps bounds unchanged, no phase restart. Submitted commands and later actual PT motor applications have separate clocks."));
    }
    else if(Step==EStep::UpperHold)
    {
        Command(0,0,true);if(Age>4)return Fail(TEXT("Actual upper reverse-ascent endpoint did not establish1second whole-footprint stopped/upright hold"));
        const bool Quiet=O.IsStoppedAndSupported()&&O.bDoorsClosedLatched&&NativeUpperWheelFootprint();
        if(Quiet){if(StableWorldS<0)StableWorldS=Now;if(Now-StableWorldS>=1){if(!bNormalBlockedQualified||!(bBoostBatteryLossObserved||bMovingBoostEnergyQualified))return Fail(TEXT("Normal powered bounded no-progress or separately qualified actual stalled/moving Boost energy not observed; original stalled-energy truth remains separate"));Save(O,true);Enter(EStep::Complete,Now);return EOutcome::Complete;}}else StableWorldS=-1;
    }
    if(!bAccepted)return Fail(TEXT("Actual reverse-ascent motor lease refused"));return EOutcome::Running;
}
FString AMAYAV01CurbTrial::ReadCurrentStageName() const
{ return MayaCityCurb::Name(int32(Step)); }
FString AMAYAV01CurbTrial::ReadEvidenceJson(bool Full) const
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),bBoostPower?TEXT("MayaRobotics.OriginalCityCurbTrial.v4"):TEXT("MayaRobotics.OriginalCityCurbTrial.v1"));J->SetStringField(TEXT("step"),MayaCityCurb::Name(int32(Step)));
    J->SetStringField(TEXT("failure"),Failure);J->SetBoolField(TEXT("nativeCommandAccepted"),bAccepted);J->SetBoolField(TEXT("allSixPredicateLossObserved"),bLostSupport);J->SetBoolField(TEXT("allSixSupportRegainObserved"),bRegainedSupport);
    J->SetStringField(TEXT("alignmentController"),TEXT("Actual optical longitudinal error*.02 minus measured optical speed*.015; ordinary throttle clamped+/- .35, original signed full yaw steering/30second setup bound; actual nine-body footprint must stay inside surveyed upper patch with2cm margin"));
    J->SetBoolField(TEXT("settleHeadingTrimActive"),bSettleHeadingTrim);
    J->SetStringField(TEXT("settlementController"),TEXT("Original10s Settle clock never resets. Actual stopped/supported/latched error>3degrees arms pure raw yaw clamp(error/12,+/-0.35) with zero throttle until error<=3degrees; every tick retains original native physical lock/custody/upper-footprint guards. Then brake and require original full1s stopped/support/latch/error<=3degrees. Yaw demand is diagnostic command evidence; actual sampled heading establishes achievement."));
    J->SetNumberField(TEXT("alignmentRecenterStep"),AlignmentRecenterStep);
    J->SetNumberField(TEXT("alignmentRecenterCount"),AlignmentRecenterCount);
    J->SetNumberField(TEXT("alignmentRecenterStartWorldSeconds"),AlignmentRecenterStartWorldS);
    J->SetNumberField(TEXT("alignmentRecenterPreservedHeadingDegrees"),AlignmentRecenterHeading);
    J->SetNumberField(TEXT("alignmentRecenterRemainingCm"),AlignmentRecenterRemainingCm);
    J->SetArrayField(TEXT("alignmentRecenterStartCm"),MayaCityCurb::Vec(AlignmentRecenterStartCm));
    J->SetArrayField(TEXT("alignmentRecenterGoalCm"),MayaCityCurb::Vec(AlignmentRecenterGoalCm));
    J->SetStringField(TEXT("alignmentRecenterScope"),TEXT("Actual nine-body inner20cm margin triggers brake-held0.25s supported/latch settlement, then ordinary zero-steer +/-0.35 translation along the current measured heading toward its centre projection. Stop within5cm/projection-crossing, heading stays within5degrees, travel<=40cm, regained inner margin and0.25s stable stop before resuming yaw. At most4 corrections, each6s within original30s alignment deadline; original hard2cm footprint guard remains every frame. No pose, force, geometry or time setter."));
    J->SetNumberField(TEXT("peakMeasuredChassisDropCm"),PeakDropCm);J->SetNumberField(TEXT("peakMeasuredDownCmps"),PeakDownCmps);J->SetNumberField(TEXT("conditionBefore"),ConditionBefore);J->SetNumberField(TEXT("conditionAtRoadHold"),ConditionAtHold);
    J->SetStringField(TEXT("crossingContactObservationCadence"),TEXT("Every observed post-physics crossing frame typed support/chassis sample; actual wheel contact clocks at20Hz, bounded1500 samples; production support receipt lifetime unchanged"));
    J->SetStringField(TEXT("lastContainmentObservation"),LastContainmentEvidenceJson);
    J->SetStringField(TEXT("crossingPitchEnvelopeBasis"),TEXT("Prospective crossing-only50degrees: actual17.7218cm surface step, minimum29.7808cm adjacent axle spacing,4cm relative suspension allowance and1.3194cm cooked tyre support variation give46.9726degree conservative adjacent-support geometry; roll20degrees and pre/post-cross pitch20degrees remain, with original stopped/support/upright endpoint and recovery gates"));
    J->SetArrayField(TEXT("actualNativeLandingEvents"),Landings);J->SetStringField(TEXT("scope"),TEXT("Physical leased driving on fresh-surveyed original city curb/road, no geometry/pose/force/event setters. Crossing permits temporary support loss,20degree roll and50degree pitch; original service/recovery safety is unchanged. Exactly1condition point in total is required; additional lateral/landing episodes are retained and fail this declared curb calibration."));
    if(bBoostPower)
    {
        J->SetStringField(TEXT("boostDescentGuidanceScope"),TEXT("Boost-only forward Cross targets the original centreline with fixed200cm lookahead: normalized(Outward-Axis*(actualAcross/200)), existing target-minus-actual yaw/40 and +/-0.35 steering. Existing throttle/coast/brake, physical bounds and arrival remain. Historical non-Boost Cross and all ascent control are unchanged. Commands are diagnostic intent, not proof of ideal tracking."));
        J->SetNumberField(TEXT("crossGuidanceObservationsSeen"),CrossGuidanceObservationsSeen);
        J->SetNumberField(TEXT("crossGuidanceObservationsRetained"),CrossGuidanceObservations.Num());
        J->SetBoolField(TEXT("crossGuidanceTraceTruncated"),bCrossGuidanceTraceTruncated);
        J->SetArrayField(TEXT("actualCrossGuidanceObservations"),CrossGuidanceObservations);
        J->SetBoolField(TEXT("descentAssessmentPerformed"),bDescentAssessmentPerformed);
        J->SetBoolField(TEXT("descentConditionAccountingComplete"),bDescentConditionAccountingComplete);
        J->SetBoolField(TEXT("boostDescentRoadEndpointAccepted"),bBoostDescentRoadEndpointAccepted);
        J->SetBoolField(TEXT("legacyLandingCriterionPassed"),bLegacyLandingCriterionPassed);
        J->SetBoolField(TEXT("landingDamageCalibrationRequiredForThisScope"),false);
        if(DescentAccountingEvidence.IsValid())J->SetObjectField(TEXT("descentConditionAccounting"),DescentAccountingEvidence);
        J->SetBoolField(TEXT("boostPowerReverseAscentVariant"),true);J->SetBoolField(TEXT("normalActualCurbContactPoweredStall"),bNormalStallQualified);J->SetBoolField(TEXT("actualBoostedMotorBatteryLossObserved"),bBoostBatteryLossObserved);
        J->SetBoolField(TEXT("movingPoweredBoostEnergyQualified"),bMovingBoostEnergyQualified);J->SetBoolField(TEXT("ascentPoweredEnergyQualified"),bBoostBatteryLossObserved||bMovingBoostEnergyQualified);
        J->SetNumberField(TEXT("movingPoweredBoostQualifiedIntervalCount"),MovingBoostEnergyQualifiedIntervalCount);
        J->SetStringField(TEXT("movingPoweredBoostEnergyScope"),TEXT("Separate actual adjacent moving-powered proof uses Core-consumed Director observations and matching current production PT motor/effort. Speed>=20; same job/reset/one parcel/carrying, finite active admitted Boost, curb contact, positive unclamped battery, no charge/shift end/totalled/vehicle adjudication. Consecutive helper frames, same-frame world stamps, elapsed Core dt agrees world dt within1e-6s. Battery loss matches current1.2*(.01+.54*clamp(v/200,0,3))*dt within1e-6pp and exceeds idle. Old corresponding-speed rate is arithmetic, not a measured old curb battery trial. Original near-stall flag/criteria unchanged."));
        J->SetBoolField(TEXT("ascentSpeedCoast"),bAscentSpeedCoast);J->SetNumberField(TEXT("ascentSpeedCoastTransitionCount"),AscentSpeedCoastTransitionCount);
        J->SetStringField(TEXT("ascentSpeedGovernorController"),TEXT("Diagnostic BoostAscent fixed90/65 actual3D-speed hysteresis requests zeroT/S/brakeOFF/Boostfalse at>=90 until<=65 then original raw-1/heading-steer Boost. Original whole-footprint/support63 native brake priority, then permanent six accepted original upper-contact coast without restart. Original150cmps/15s/all physical guards remain; no phase-clock reset or force/velocity setter."));
        J->SetBoolField(TEXT("normalActualCurbContactPoweredBlockedAscent"),bNormalBlockedQualified);
        J->SetArrayField(TEXT("actualNormalBlockedPoseCm"),MayaCityCurb::Vec(NormalBlockedPoseCm));
        J->SetArrayField(TEXT("actualNormalBlockedMinCm"),MayaCityCurb::Vec(NormalBlockedMinCm));
        J->SetArrayField(TEXT("actualNormalBlockedMaxCm"),MayaCityCurb::Vec(NormalBlockedMaxCm));
        J->SetNumberField(TEXT("actualNormalBlockedDurationSeconds"),NormalBlockedDurationS);
        J->SetNumberField(TEXT("actualNormalBlockedDiagonalCm"),NormalBlockedDiagonalCm);
        J->SetNumberField(TEXT("actualNormalBlockedQualifiedSampleCount"),NormalBlockedQualifiedSampleCount);
        J->SetNumberField(TEXT("normalBlockedCandidateStartWorldSeconds"),BlockedStartWorldS);
        J->SetNumberField(TEXT("normalBlockedCandidateLastWorldSeconds"),BlockedLastWorldS);
        J->SetNumberField(TEXT("normalBlockedCandidateSampleCount"),BlockedSampleCount);
        J->SetNumberField(TEXT("normalBlockedCandidateResetCount"),BlockedResetCount);
        J->SetStringField(TEXT("normalPoweredBlockedScope"),TEXT("Fresh observed normal movement with actual nonzero motor torque and genuine surveyed curb contact, Boost inactive. Fixed candidate start and running3D chassis-position minima/maxima must have diagonal<5cm for2world seconds within original8s. Excursion>=5 reseeds at the current eligible finite sample; eligibility loss clears clock/count/bounds. Per-row candidate state follows that observation's decision and retains rejected bounds/reasons. Original quiet-stall flag/pose remain separate truth; bounded rocking is not quiet stall or a normal-servo health claim."));
        J->SetBoolField(TEXT("upperSupportCoastLatched"),bUpperSupportCoastLatched);
        J->SetStringField(TEXT("upperSupportCoastController"),TEXT("Before six-upper acquisition, diagnostic90/65 actual3D-speed hysteresis controls original powered reverse and zero-input coast. Typed support63 plus six captured accepted original upper contact identities permanently latch zero-throttle/zero-steer/brakeOFF/Boostfalse coast without restart, including below65, until unchanged full native wheel footprint. Original full-footprint/support63 native brake transition has priority; existing UpperHold follows. No phase-clock reset or new contact age/tolerance gate."));
        J->SetNumberField(TEXT("upperSupportCoastLatchedWorldSeconds"),UpperSupportCoastWorldS);
        J->SetArrayField(TEXT("upperSupportCoastLatchedPoseCm"),MayaCityCurb::Vec(UpperSupportCoastPoseCm));
        if(UpperSupportCoastReceipt)J->SetObjectField(TEXT("actualUpperSupportCoastLatch"),UpperSupportCoastReceipt);
        if(Full)
        {
            if(CompoundContactBinding)J->SetObjectField(TEXT("passiveCompoundContactBinding"),CompoundContactBinding);
            J->SetBoolField(TEXT("passiveCompoundContactCleanupPerformed"),bCompoundContactCleanupPerformed);
            J->SetNumberField(TEXT("passiveCompoundContactActiveObserverCount"),CompoundContactObservers.Num());
            J->SetNumberField(TEXT("passiveCompoundContactCallbacksSeen"),CompoundContactCallbacksSeen);
            J->SetNumberField(TEXT("passiveCompoundContactFilteredOtherIdentityCount"),CompoundContactFiltered);
            J->SetNumberField(TEXT("passiveCompoundContactDroppedInvalidCallbackCount"),CompoundContactDropped);
            J->SetNumberField(TEXT("passiveCompoundContactRecordedCount"),CompoundContactRecorded);
            J->SetNumberField(TEXT("passiveCompoundContactMalformedRecordedCount"),CompoundContactMalformed);
            J->SetNumberField(TEXT("passiveCompoundContactRetainedCount"),CompoundContactFirst.Num()+CompoundContactRecent.Num());
            J->SetNumberField(TEXT("passiveCompoundContactOverwrittenCount"),CompoundContactOverwritten);
            auto Counts=MakeShared<FJsonObject>();for(const auto& Pair:CompoundContactPhaseCounts)Counts->SetNumberField(Pair.Key,Pair.Value);
            J->SetObjectField(TEXT("passiveCompoundContactRecordedPhaseCounts"),Counts);
            J->SetArrayField(TEXT("passiveCompoundContactFirst32"),CompoundContactFirst);
            TArray<TSharedPtr<FJsonValue>> Recent;for(int32 I=0;I<CompoundContactRecent.Num();++I)
                Recent.Add(CompoundContactRecent[(CompoundContactRecentWrite+I)%CompoundContactRecent.Num()]);
            J->SetArrayField(TEXT("passiveCompoundContactLatest128Chronological"),Recent);
            J->SetStringField(TEXT("passiveCompoundContactScope"),TEXT("Passive actual component-hit callbacks on verified cargo-mount parent compound root plus six actual WeldParent children. Existing notification/collision/physics settings untouched; original wheel/Director listeners unchanged. Only NormalAscent/BrakeBeforeBoost/BoostAscent/UpperHold and exact fresh upper/curb/road identities.32 first plus128 recent raw receipts, chronological recent ring, total/filtered/dropped/malformed/overwritten counts; history serialized only in full terminal evidence. UE reports first-contact mapped component/body indices/bone names and aggregate pair impulse, not a unique convex/manifold or force balance/solver step. Root versus welded-child owner is reported mapping only. Child notification/shape filtering and pair aggregation can limit coverage; unavailable binding or absence of callbacks never proves clearance or absence of compound contact. No acceptance or causal criterion."));
        }
        J->SetArrayField(TEXT("passiveOriginalCurbRawWheelHitCallbacks"),RawCurbHits);
        J->SetArrayField(TEXT("actualReverseAscentObservations"),BoostPowerObservations);J->SetNumberField(TEXT("reverseAscentObservationCount"),BoostPowerObservationCount);
        J->SetBoolField(TEXT("reverseAscentObservationsTruncated"),BoostPowerObservationCount>BoostPowerObservations.Num());
        J->SetStringField(TEXT("stalledBoostEnergyScope"),TEXT("Adjacent actual postphysics observations both require active admitted Boost, positive finite effort, original native curb contact, speed<5 and no charging. Current battery loss must exceed ordinary .01pp/s idle over the observed interval. Both native endpoints of qualified intervals are retained; no minimum stall duration or electrical-power claim."));
        J->SetNumberField(TEXT("rawCurbCallbackCount"),RawCurbHitCount);J->SetBoolField(TEXT("rawCallbackHistoryTruncated"),RawCurbHitCount>RawCurbHits.Num());if(LastRawCurbHit)J->SetObjectField(TEXT("lastRawCurbHit"),LastRawCurbHit);
        J->SetArrayField(TEXT("actualNormalStallPoseCm"),MayaCityCurb::Vec(NormalStallPoseCm));J->SetNumberField(TEXT("boostStartBatteryPercent"),BoostStartBattery);J->SetStringField(TEXT("sameOriginalParcelId"),OriginalParcelId);
        J->SetStringField(TEXT("scope"),TEXT("Boost-only descent admits the same safe actual road hold with complete finite condition accounting; legacy8cm/40cmps/one-point landing calibration is reported separately and is not required. Explicit reverse ascent retains outward optical nose, same edge/body/parcel/reset: full normal-1 actual curb-contact powered bounded no-progress2s with running3D bounds diagonal<5cm within8s, original quiet-stall truth separately retained, realbrake.5s, currentBoost controlled ascent15s with fixed90/65 actual3D-speed zero-input coast/powered reverse hysteresis before permanent zero-throttle/zero-steer/brakeOFF/Boostfalse coast after support63 plus all six captured accepted original upper contact identities, no powered restart after that permanent latch; unchanged whole native wheel bounds beyond edge before realbrake and full upright stopped1s. Temporary support/pitch50/roll20 and current physical locks guarded. No forward-ascent proof or universal curb claim; actual applied motor cache and later readback are distinct observations. Normal blocked and either unchanged near-stall energy or separate actual moving-powered current-law energy are required; old stalled flag stays independent. Additional legitimate ascent damage is recorded; condition must remain>=80."));
    }
    if(Full){J->SetStringField(TEXT("freshSurfaceSurvey"),SurveyJson);J->SetArrayField(TEXT("samples"),Samples);}
    return MayaCityCurb::Json(J);
}
void AMAYAV01CurbTrial::Cleanup()
{
    UnbindCompoundContactObservers();
    if(Robot){TArray<UPrimitiveComponent*> Components;Robot->GetComponents<UPrimitiveComponent>(Components);for(UPrimitiveComponent* C:Components)if(C)C->OnComponentHit.RemoveDynamic(this,&AMAYAV01CurbTrial::ObserveRawCurbHit);Robot->ReleaseDriveCommand();}
}
void AMAYAV01CurbTrial::EndPlay(EEndPlayReason::Type Reason){Cleanup();Super::EndPlay(Reason);}
