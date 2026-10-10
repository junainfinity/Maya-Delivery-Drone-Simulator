#include "MAYADeliveryHUD.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYAUITheme.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryStation.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "CanvasItem.h"
#include "GlobalRenderResources.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneGraphData.h"
#include "MassTrafficSettings.h"
#include "MassCrowdSettings.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"

struct FMAYAFullMapCache
{
    struct FSegment { FVector2D A, B; float Width; uint8 Kind; };
    TArray<FSegment> Segments;
    FBox2D Bounds{ForceInit};
    uint32 GraphKey=0;
    int32 RoadLanes=0, WalkLanes=0, CrossingLanes=0, SkippedLanes=0;
    bool bCaptured=false;
    FVector2D DrawMin=FVector2D::ZeroVector,DrawMax=FVector2D::ZeroVector,Viewport=FVector2D::ZeroVector;
    float DrawScale=0;
    TUniquePtr<FCanvasTriangleItem> Geometry;
    FString Evidence;
};

namespace MAYAFullMap
{
    using namespace MAYAHUDGeometry;
    bool Configured(const FZoneGraphTagFilter& F)
    { return (F.AnyTags.GetValue()|F.AllTags.GetValue()|F.NotTags.GetValue())!=0; }

    void Capture(UWorld* World, FMAYAFullMapCache& C)
    {
        auto* Graph=World?World->GetSubsystem<UZoneGraphSubsystem>():nullptr;
        if (!Graph) return;
        uint32 Key=0;
        for (const auto& R:Graph->GetRegisteredZoneGraphData())
            if (R.bInUse && IsValid(R.ZoneGraphData.Get()) && R.ZoneGraphData->IsRegistered()) Key=HashCombine(Key,HashCombine(GetTypeHash(R.ZoneGraphData.Get()),GetTypeHash(R.Generation)));
        if(C.bCaptured && C.GraphKey==Key) return;
        C.GraphKey=Key; C.bCaptured=true; C.Segments.Reset(); C.Bounds=FBox2D(ForceInit);
        C.RoadLanes=C.WalkLanes=C.CrossingLanes=C.SkippedLanes=0; C.Geometry.Reset();
        const auto* Traffic=GetDefault<UMassTrafficSettings>(); const auto* Crowd=GetDefault<UMassCrowdSettings>();
        for (const auto& R:Graph->GetRegisteredZoneGraphData())
        {
            auto* A=R.ZoneGraphData.Get(); if(!R.bInUse || !IsValid(A) || !A->IsRegistered()) continue;
            FScopeLock Lock(&A->GetStorageLock()); const auto& S=A->GetStorage();
            for(const auto& L:S.Lanes)
            {
                const bool Road=Configured(Traffic->TrafficLaneFilter)&&Traffic->TrafficLaneFilter.Pass(L.Tags);
                const bool Crossing=Configured(Traffic->CrosswalkLaneFilter)&&Traffic->CrosswalkLaneFilter.Pass(L.Tags);
                const bool Walk=L.Tags.Contains(Crowd->CrowdTag);
                if(!Road&&!Crossing&&!Walk) continue;
                if(L.PointsBegin<0 || L.PointsEnd>S.LanePoints.Num() || L.PointsEnd-L.PointsBegin<2 || !FMath::IsFinite(L.Width) || L.Width<=0)
                {++C.SkippedLanes;continue;}
                bool Finite=true; for(int32 I=L.PointsBegin;I<L.PointsEnd;++I) Finite&=!S.LanePoints[I].ContainsNaN();
                if(!Finite){++C.SkippedLanes;continue;}
                const uint8 Kind=Crossing?2:Road?0:1;
                if(Kind==0)++C.RoadLanes; else if(Kind==1)++C.WalkLanes; else ++C.CrossingLanes;
                for(int32 I=L.PointsBegin;I+1<L.PointsEnd;++I)
                {
                    const FVector2D P(S.LanePoints[I]),Q(S.LanePoints[I+1]);
                    if((Q-P).SizeSquared()<.01)continue;
                    C.Segments.Add({P,Q,L.Width,Kind}); C.Bounds+=P;C.Bounds+=Q;
                }
            }
        }
    }

    void AddTriangle(TArray<FCanvasUVTri>& Out,const FVector2D& A,const FVector2D& B,const FVector2D& C,const FLinearColor& Color)
    {
        FCanvasUVTri T; T.V0_Pos=A;T.V1_Pos=B;T.V2_Pos=C;
        T.V0_UV=T.V1_UV=T.V2_UV=FVector2D::ZeroVector;
        T.V0_Color=T.V1_Color=T.V2_Color=Color; Out.Add(T);
    }
}

FString AMAYADeliveryHUD::ReadFullMapEvidenceJson() const
{ return FullMapCache.IsValid()?FullMapCache->Evidence:TEXT("{}"); }

void AMAYADeliveryHUD::DrawFullMap(AMAYADeliveryDirector* D)
{
    using namespace MAYAFullMap;
    auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    if(!Canvas||!PC||!D)return;
    if(!FullMapCache.IsValid())FullMapCache=MakeShared<FMAYAFullMapCache>();
    auto& C=*FullMapCache; Capture(GetWorld(),C);
    const float W=Canvas->SizeX/UIScale,H=Canvas->SizeY/UIScale,Left=FMath::Clamp(W*.23f,220.f,300.f);
    const FLinearColor White=MAYAUITheme::Text,Muted=MAYAUITheme::Muted,Green=MAYAUITheme::ServiceGreen,Amber=MAYAUITheme::Accent,Red=MAYAUITheme::DangerRed;
    const FRect R{Left+30,86,W-Left-58,H-168};
    UIRect(MAYAUITheme::Background,0,0,W,H);
    UIRect(MAYAUITheme::Panel,R.X,R.Y,R.W,R.H);
    UIText(TEXT("MAYA / CITY MAP"),White,28,22,1.8f);
    UITextFit(TEXT("SMALL CITY"),Muted,28,55,Left,.9f);
    UITextFit(TEXT("NORTH UP"),Muted,W-142,40,116,.85f);
    UILine(28,76,W-28,76,MAYAUITheme::Border);
    const auto& S=D->Snapshot(); const auto& O=D->Observation();
    const bool Known=O.bKnown&&!O.ChassisPose.ContainsNaN();
    auto* Objective=D->ObjectiveStation();
    auto* Destination=D->FindStation(S.Job.DropoffStationId);
    const auto& Route=D->GetActiveGuidanceRoute();
    TArray<AMAYADeliveryStation*> Chargers;
    for(TActorIterator<AMAYADeliveryStation> It(GetWorld());It;++It)
        if(D->FindStation(It->StationId)==*It && It->IsChargingStation()&&!It->GetServicePoint().ContainsNaN())Chargers.Add(*It);
    FBox2D View(ForceInit);
    if(!PC->IsMapRouteFocused()&&C.Bounds.bIsValid)View=C.Bounds;
    for(const auto& P:Route)if(!P.ContainsNaN())View+=FVector2D(P);
    if(Known)View+=FVector2D(O.ChassisPose.GetLocation());
    for(auto* Station:Chargers)View+=FVector2D(Station->GetServicePoint());
    if(Objective&&!Objective->GetServicePoint().ContainsNaN())View+=FVector2D(Objective->GetServicePoint());
    if(Destination&&!Destination->GetServicePoint().ContainsNaN())View+=FVector2D(Destination->GetServicePoint());
    if(!View.bIsValid){UIText(TEXT("Locating the city..."),White,R.X+24,R.Y+24);return;}
    const FVector2D Mid=View.GetCenter(),Extent=View.GetExtent();
    const double X=FMath::Max(Extent.X,1000.0)*1.15,Y=FMath::Max(Extent.Y,1000.0)*1.15;
    View=FBox2D(Mid-FVector2D(X,Y),Mid+FVector2D(X,Y));
    const double Scale=FMath::Min((R.W-44)/(X*2),(R.H-44)/(Y*2));
    const FVector2D Centre(R.X+R.W*.5,R.Y+R.H*.5);
    auto Map=[&](const FVector2D& P){return Centre+FVector2D(P.X-Mid.X,Mid.Y-P.Y)*Scale;};
    if(!C.Geometry || C.DrawMin!=View.Min || C.DrawMax!=View.Max || C.Viewport!=FVector2D(W,H) || C.DrawScale!=UIScale)
    {
        TArray<FCanvasUVTri> Triangles;
        // One cached canvas batch, rather than thousands of individual HUD draw calls.
        for(uint8 Kind:{uint8(1),uint8(0),uint8(2)})for(const auto& Seg:C.Segments)if(Seg.Kind==Kind)
        {
            FVector2D A=Map(Seg.A),B=Map(Seg.B);
            const double Half=FMath::Clamp(Seg.Width*Scale*.5,.35,96.0);
            const FRect Inset{R.X+float(Half),R.Y+float(Half),R.W-float(2*Half),R.H-float(2*Half)};
            if(!ClipLine(Inset,A,B))continue;
            const FVector2D V=(B-A).GetSafeNormal(),N(-V.Y*Half,V.X*Half);
            const FLinearColor Color=Kind==0?MAYAUITheme::Road:Kind==1?MAYAUITheme::Sidewalk:MAYAUITheme::Crossing;
            AddTriangle(Triangles,(A-N)*UIScale,(B-N)*UIScale,(B+N)*UIScale,Color);
            AddTriangle(Triangles,(A-N)*UIScale,(B+N)*UIScale,(A+N)*UIScale,Color);
        }
        C.Geometry.Reset();
        if(!Triangles.IsEmpty()){C.Geometry=MakeUnique<FCanvasTriangleItem>(Triangles,GWhiteTexture);C.Geometry->BlendMode=SE_BLEND_Opaque;}
        C.DrawMin=View.Min;C.DrawMax=View.Max;C.Viewport=FVector2D(W,H);C.DrawScale=UIScale;
    }
    if(C.Geometry)Canvas->DrawItem(*C.Geometry);
    for(int32 I=1;I<Route.Num();++I)
    {FVector2D A=Map(FVector2D(Route[I-1])),B=Map(FVector2D(Route[I]));if(ClipLine(R,A,B))UILine(A.X,A.Y,B.X,B.Y,Amber,2);}
    auto Ring=[&](FVector2D P,float Radius,const FLinearColor& Color)
    {for(int32 I=0;I<32;++I){const float A=I*2*PI/32,B=(I+1)*2*PI/32;UILine(P.X+FMath::Cos(A)*Radius,P.Y+FMath::Sin(A)*Radius,P.X+FMath::Cos(B)*Radius,P.Y+FMath::Sin(B)*Radius,Color,2);}};
    auto Square=[&](FVector2D P,float Radius,const FLinearColor& Color)
    {UILine(P.X-Radius,P.Y-Radius,P.X+Radius,P.Y-Radius,Color,2);UILine(P.X+Radius,P.Y-Radius,P.X+Radius,P.Y+Radius,Color,2);UILine(P.X+Radius,P.Y+Radius,P.X-Radius,P.Y+Radius,Color,2);UILine(P.X-Radius,P.Y+Radius,P.X-Radius,P.Y-Radius,Color,2);};
    auto Diamond=[&](FVector2D P,float Radius,const FLinearColor& Color)
    {UILine(P.X,P.Y-Radius,P.X+Radius,P.Y,Color,2);UILine(P.X+Radius,P.Y,P.X,P.Y+Radius,Color,2);UILine(P.X,P.Y+Radius,P.X-Radius,P.Y,Color,2);UILine(P.X-Radius,P.Y,P.X,P.Y-Radius,Color,2);};
    TArray<TSharedPtr<FJsonValue>> ChargerJson;
    for(auto* Station:Chargers)
    {
        const FVector2D P=Map(FVector2D(Station->GetServicePoint()));Ring(P,7,Green);
        UILine(P.X+1,P.Y-4,P.X-2,P.Y,Green,2);UILine(P.X-2,P.Y,P.X+2,P.Y,Green,2);UILine(P.X+2,P.Y,P.X-1,P.Y+4,Green,2);
        auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("stationId"),Station->StationId);J->SetStringField(TEXT("name"),Station->DisplayName);
        J->SetNumberField(TEXT("x"),P.X);J->SetNumberField(TEXT("y"),P.Y);ChargerJson.Add(MakeShared<FJsonValueObject>(J));
    }
    FVector2D DeliveryPixel=Destination?Map(FVector2D(Destination->GetServicePoint())):FVector2D::ZeroVector;
    if(Destination && Destination!=Objective)
    {
        DeliveryPixel=Map(FVector2D(Destination->GetServicePoint()));const FVector2D P=DeliveryPixel;
        Diamond(P,8,Red);
    }
    FVector2D TargetPixel=FVector2D::ZeroVector;
    if(Objective)
    {
        TargetPixel=Map(FVector2D(Objective->GetServicePoint()));Ring(TargetPixel,13,Amber);
        // Match the director's actual pickup/dropoff choice; hollow glyphs leave a co-located charger bolt visible.
        const bool DeliveryObjective=S.Phase==EMAYADeliveryPhase::Carrying || S.Phase==EMAYADeliveryPhase::Unloading;
        if(DeliveryObjective)Diamond(TargetPixel,10,Red);else Square(TargetPixel,9,Green);
    }
    FVector2D PlayerPixel=FVector2D::ZeroVector;
    if(Known)
    {
        PlayerPixel=Map(FVector2D(O.ChassisPose.GetLocation()));const FVector2D F=FMapTransform().OpticalHeading(O.ChassisPose),Side(-F.Y,F.X);
        const FVector2D Tip=PlayerPixel+F*11,A=PlayerPixel-F*7+Side*6,B=PlayerPixel-F*7-Side*6;
        Ring(PlayerPixel,17,MAYAUITheme::Background);
        UILine(Tip.X,Tip.Y,A.X,A.Y,White,3);UILine(A.X,A.Y,B.X,B.Y,White,3);UILine(B.X,B.Y,Tip.X,Tip.Y,White,3);
    }
    const auto SidebarY=[&](float Y){return 106.f+(Y-106.f)*FMath::Min(1.f,(H-236.f)/385.f);};
    const bool Offered=S.Phase==EMAYADeliveryPhase::Available;
    UIText(Offered?TEXT("NEXT MISSION"):TEXT("DESTINATION"),Amber,28,SidebarY(106),.95f);
    UITextWrap(Objective?Objective->DisplayName:TEXT("No active destination"),White,28,SidebarY(136),Left-24,2,1.35f);
    UITextFit(Destination?TEXT("DELIVER TO"):TEXT("MISSION"),Muted,28,SidebarY(194),Left-24,.8f);
    UITextWrap(Destination?Destination->DisplayName:S.Job.Title,White,28,SidebarY(216),Left-24,2,1.f);
    UITextFit(FString::Printf(TEXT("BATTERY  %.0f%%"),S.BatteryPercent),White,28,SidebarY(258),Left-24,1.f);
    UIText(TEXT("MAP KEY"),Muted,28,SidebarY(308),.9f);
    const TCHAR* Labels[]={TEXT("You / heading"),TEXT("Pickup / delivery"),TEXT("Recharge / $2"),TEXT("Delivery route"),TEXT("Road / sidewalk")};
    for(int32 I=0;I<5;++I)
    {
        const float LegendY=SidebarY(346+I*31);const FVector2D P(31.5f,LegendY+3.5f);
        if(I==0){UILine(P.X,P.Y-4,P.X-3.5f,P.Y+3.5f,White,2);UILine(P.X-3.5f,P.Y+3.5f,P.X+3.5f,P.Y+3.5f,White,2);UILine(P.X+3.5f,P.Y+3.5f,P.X,P.Y-4,White,2);}
        else if(I==1){Square(FVector2D(30.5f,LegendY+3.5f),3.5f,Green);Diamond(FVector2D(39.5f,LegendY+3.5f),3.5f,Red);}
        else if(I==2){Ring(P,4.5f,Green);UILine(P.X+1,P.Y-3,P.X-2,P.Y,Green,2);UILine(P.X-2,P.Y,P.X+2,P.Y,Green,2);UILine(P.X+2,P.Y,P.X-1,P.Y+3,Green,2);}
        else if(I==3)UILine(28,LegendY+3.5f,40,LegendY+3.5f,Amber,2);
        else{UILine(28,LegendY+1,40,LegendY+1,MAYAUITheme::Road,2);UILine(28,LegendY+6,40,LegendY+6,MAYAUITheme::Sidewalk,2);}
        UITextFit(Labels[I],White,45,SidebarY(339+I*31),Left-46,.9f);
    }
    if(C.Segments.IsEmpty())UITextWrap(TEXT("City roads are still loading. Your locations remain visible."),Muted,R.X+18,R.Y+18,R.W-36,2,.95f);
    const float Bar=FMath::Min(100.f,R.W*.2f);const int32 Metres=FMath::Max(1,FMath::RoundToInt(Bar/Scale/100));
    UIRect(MAYAUITheme::Panel,R.X+12,R.Y+R.H-61,Bar+28,48);
    UILine(R.X+20,R.Y+R.H-26,R.X+20+Bar,R.Y+R.H-26,White,2);
    UIText(FString::Printf(TEXT("%d m"),Metres),White,R.X+20,R.Y+R.H-52,.85f);
    UIAction(TEXT("MapFocus"),PC->IsMapRouteFocused()?TEXT("SHOW WHOLE CITY"):TEXT("ZOOM TO DELIVERY"),28,H-114,Left-2,38);
    UIAction(TEXT("Back"),TEXT("M / ESC  BACK"),28,H-67,Left-2,38);
    UITextFit(TEXT("Delivery and charging timers are paused while viewing the map."),Muted,R.X,H-51,R.W,.9f);
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("scope"),TEXT("Registered native city lane geometry; not a driveability certification"));
    J->SetNumberField(TEXT("lastDrawGameFrame"),double(GFrameCounter));J->SetNumberField(TEXT("lastDrawWorldSeconds"),GetWorld()->GetTimeSeconds());
    J->SetStringField(TEXT("view"),PC->IsMapRouteFocused()?TEXT("delivery"):TEXT("whole_city"));
    J->SetNumberField(TEXT("roadLanes"),C.RoadLanes);J->SetNumberField(TEXT("sidewalkLanes"),C.WalkLanes);J->SetNumberField(TEXT("crossingLanes"),C.CrossingLanes);J->SetNumberField(TEXT("segments"),C.Segments.Num());J->SetNumberField(TEXT("invalidLanes"),C.SkippedLanes);
    J->SetNumberField(TEXT("worldMinX"),View.Min.X);J->SetNumberField(TEXT("worldMinY"),View.Min.Y);J->SetNumberField(TEXT("worldMaxX"),View.Max.X);J->SetNumberField(TEXT("worldMaxY"),View.Max.Y);J->SetNumberField(TEXT("pixelsPerCm"),Scale);
    J->SetBoolField(TEXT("playerKnown"),Known);J->SetNumberField(TEXT("playerX"),PlayerPixel.X);J->SetNumberField(TEXT("playerY"),PlayerPixel.Y);J->SetStringField(TEXT("objectiveStationId"),Objective?Objective->StationId:TEXT(""));J->SetNumberField(TEXT("objectiveX"),TargetPixel.X);J->SetNumberField(TEXT("objectiveY"),TargetPixel.Y);J->SetStringField(TEXT("deliveryStationId"),Destination?Destination->StationId:TEXT(""));J->SetNumberField(TEXT("deliveryX"),DeliveryPixel.X);J->SetNumberField(TEXT("deliveryY"),DeliveryPixel.Y);J->SetArrayField(TEXT("chargers"),ChargerJson);J->SetBoolField(TEXT("paused"),D->IsPaused());
    C.Evidence.Reset();const auto Writer=TJsonWriterFactory<>::Create(&C.Evidence);FJsonSerializer::Serialize(J,Writer);
}
