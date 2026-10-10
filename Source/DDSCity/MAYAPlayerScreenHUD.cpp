#include "MAYADeliveryHUD.h"
#include "MAYAUITheme.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYADeliveryPlayerController.h"
#include "MAYADeliveryDirector.h"
#include "MAYADeliveryGameMode.h"
#include "MAYADeliveryStation.h"
#include "MAYAPlayerPresentation.h"
#include "MAYAPresentationSettings.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"

namespace MAYAScreenColors
{
    const FLinearColor White=MAYAUITheme::Text, Muted=MAYAUITheme::Muted, Green=MAYAUITheme::Accent;
    const FLinearColor Navy=MAYAUITheme::Panel, Teal=MAYAUITheme::Border, Coral=MAYAUITheme::DangerRed, Amber=MAYAUITheme::Accent;
    const FLinearColor ServiceGreen=MAYAUITheme::ServiceGreen;
}

void AMAYADeliveryHUD::NotifyHitBoxClick(FName Name)
{
    Super::NotifyHitBoxClick(Name);
    auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    FString Action=Name.ToString(); const bool Previous=Action.RemoveFromEnd(TEXT(".Previous"));
    if (!Previous) Action.RemoveFromEnd(TEXT(".Next"));
    if (PC) PC->ActivateUIAction(FName(*Action),Previous?-1:1);
}

void AMAYADeliveryHUD::NotifyHitBoxBeginCursorOver(FName Name)
{
    Super::NotifyHitBoxBeginCursorOver(Name);
    FString Action=Name.ToString(); Action.RemoveFromEnd(TEXT(".Previous")); Action.RemoveFromEnd(TEXT(".Next"));
    if (auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController())) PC->SetMenuFocus(FName(*Action));
}

void AMAYADeliveryHUD::UIAction(FName Id,const FString& Label,float X,float Y,float W,float H,bool Enabled,bool Primary)
{
    if (bSuppressUI) return;
    using namespace MAYAScreenColors;
    const auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    bool Focused=false;
    if (PC && PC->HasPlayerMenu())
    {
        const auto Actions=PC->GetMenuActions();
        Focused=Actions.IsValidIndex(PC->GetMenuFocus()) && Actions[PC->GetMenuFocus()].Id==Id;
    }
    // Menu emphasis follows the actual focused row; Primary must not mimic a second focus.
    const bool Highlighted=Enabled && (Focused || (!bDrawingPlayerMenus && Primary));
    const float Pixel=1.f/UIScale;
    if(bDrawingPlayerMenus)
    {
        // Clean actual action lists; secondary rows stay quiet, focus remains explicit.
        if(!bMenuListStyle || Highlighted)
        {
            UIRect(MAYAUITheme::Alpha(MAYAUITheme::Panel,bHighContrast?1.f:.92f),X,Y,W,H);
            if(Highlighted)UIRect(MAYAUITheme::Alpha(Amber,bHighContrast?.20f:.14f),X+Pixel,Y+Pixel,W-2*Pixel,H-2*Pixel);
            const FLinearColor Edge=MAYAUITheme::Alpha(Highlighted?Amber:Teal,bHighContrast?1.f:.72f);
            UIRect(Edge,X,Y,W,Pixel);UIRect(Edge,X,Y+H-Pixel,W,Pixel);
            UIRect(Edge,X,Y,Pixel,H);UIRect(Edge,X+W-Pixel,Y,Pixel,H);
        }
        if(Highlighted)UIRect(Amber,X,Y,3.f/UIScale,H);
    }
    else
    {
        UIPanel(X,Y,W,H,Highlighted?Amber:Teal,0.f);
        if(Highlighted)UIRect(MAYAUITheme::Alpha(Amber,bHighContrast?.12f:.08f),X+Pixel,Y+Pixel,W-2*Pixel,H-2*Pixel);
        if(Focused)UIRect(Amber,X+5,Y+H*.5f-3,6,6);
    }
    UITextFit(Label,Enabled?White:Muted,X+(bDrawingPlayerMenus?14.f:12.f),Y+(H-MeasureUIText(Label,1.f).Y)*.5f,bDrawingPlayerMenus?W-44:W-24,1.f);
    if(Enabled&&bDrawingPlayerMenus)UIText(TEXT(">"),Highlighted?Amber:Muted,X+W-25,Y+(H-MeasureUIText(TEXT(">"),1.f).Y)*.5f,1.f);
    if (Enabled) AddHitBox(FVector2D(X,Y)*UIScale,FVector2D(W,H)*UIScale,Id,true,10);
}

void AMAYADeliveryHUD::DrawPlayerMenus(AMAYADeliveryDirector* D)
{
    auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    if (!PC || !PC->HasPlayerMenu() || !Canvas) return;
    using namespace MAYAScreenColors;
    const auto Screen=PC->GetPlayerScreen(); const auto Actions=PC->GetMenuActions();
    const bool ExplicitMenu=Screen!=EMAYAPlayerScreen::ShiftWin && Screen!=EMAYAPlayerScreen::ShiftLost;
    const auto* Settings=PC->GetPresentationSettings();
    const float MenuScale=FMath::Clamp(FMath::Min(Canvas->SizeX/1280.f,Canvas->SizeY/720.f),.8f,1.25f)
        *FMath::Clamp(Settings?Settings->InterfaceScale:1.f,.9f,1.1f);
    TGuardValue<float> MenuScaleGuard(UIScale,ExplicitMenu?MenuScale:UIScale);
    const float W=Canvas->SizeX/UIScale,H=Canvas->SizeY/UIScale;
    TGuardValue<bool> MenuPanelStyle(bDrawingPlayerMenus,Screen!=EMAYAPlayerScreen::ShiftWin && Screen!=EMAYAPlayerScreen::ShiftLost);
    if (Screen==EMAYAPlayerScreen::CityMap) { DrawFullMap(D); return; }
    if (Screen==EMAYAPlayerScreen::ShiftWin || Screen==EMAYAPlayerScreen::ShiftLost)
    {
        // Automatic win summary stays at the edge; preserve screen, pause and actions.
        bSuppressUI=bContextOccluded;
        const float X=ContextX,Y=ContextY,PW=ContextWidth;
        const bool Lost=Screen==EMAYAPlayerScreen::ShiftLost;
        if (Lost && bContextOccluded)
        {
            // A wreck can project across both normal edge placements. Terminal
            // restart must remain reachable, even when full-card placement fails.
            bSuppressUI=false; const float FW=FMath::Min(320.f,W-24.f);
            UIPanel(12,12,FW,106,Coral);
            UITextFit(TEXT("DRONE TOTALLED"),Coral,24,22,FW-24,1.15f);
            UITextFit(TEXT("Shift lost"),White,24,49,FW-24,.9f);
            UIAction(TEXT("NewShift"),TEXT("START OVER"),24,76,FW-24,30,D && D->GetPlayerPresentation().bCanStartNewShift,true);
            return;
        }
        UIPanel(X,Y,PW,284,Lost?Coral:Green);
        UITextFit(Lost?TEXT("DRONE TOTALLED"):TEXT("SHIFT COMPLETE"),Lost?Coral:Green,X+12,Y+12,PW-24,1.2f);
        UITextFit(D?FString::Printf(TEXT("$%d / $100"),D->Snapshot().TotalCredits):TEXT("Goal reached"),White,X+12,Y+43,PW-24,1.5f);
        UITextFit(Lost?TEXT("Shift lost. Start Over to continue."):TEXT("Delivery goal reached"),Muted,X+12,Y+78,PW-24,.9f);
        for(int32 I=0;I<Actions.Num();++I)
            UIAction(Actions[I].Id,Actions[I].Label,X+12,Y+112+I*40,PW-24,34,Actions[I].bEnabled,I==0);
        bSuppressUI=false;
        return;
    }
    UIRect(MAYAUITheme::Alpha(MAYAUITheme::Background,Screen==EMAYAPlayerScreen::MainMenu?.60f:.44f),0,0,W,H);
    if (Screen==EMAYAPlayerScreen::MainMenu)
    {
        TGuardValue<bool> ListStyle(bMenuListStyle,true);
        const float X=FMath::Max(24.f,W*.055f),Top=FMath::Min(42.f,H*.055f),BW=FMath::Min(360.f,W*.44f);
        const float TitleH=MeasureUIText(TEXT("MAYA"),2.7f).Y;
        UIText(TEXT("MAYA"),White,X,Top,2.7f);UIText(TEXT("ROBOTICS"),White,X,Top+TitleH+2.f,2.7f);
        UIRect(Amber,X,Top+2*TitleH+14.f,48.f,2.f/UIScale);
        UITextFit(TEXT("Delivery Drone Simulator"),Muted,X,Top+2*TitleH+30.f,BW,1.1f);
        const float First=Top+2*TitleH+66.f,Gap=6.f;
        const float ButtonH=FMath::Min(46.f,(H-First-100.f)/FMath::Max(1,Actions.Num())-Gap);
        for(int32 I=0;I<Actions.Num();++I)
        {
            const auto& A=Actions[I];const float RowY=First+I*(ButtonH+Gap);
            UIAction(A.Id,A.Label,X,RowY,BW,ButtonH,A.bEnabled,I==0);
        }
        UITextFit(TEXT("Reach $100 to complete the shift."),White,X,First+Actions.Num()*(ButtonH+Gap)+10.f,W-2*X,1.f);
        UITextWrap(PC->GetMenuStatus(),Muted,X,H-68.f,W-2*X,2,.9f);
        UITextFit(TEXT("Mouse or arrows / Tab to choose    Enter to select"),Muted,X,H-28.f,W-2*X,.9f);
        return;
    }
    if(Screen==EMAYAPlayerScreen::Pause)
    {
        TGuardValue<bool> ListStyle(bMenuListStyle,true);
        const float PW=FMath::Min(340.f,W-48.f),PH=FMath::Min(136.f+Actions.Num()*50.f,H-32.f);
        const float X=W-PW-FMath::Max(24.f,W*.045f),Y=(H-PH)*.5f;
        UIPanel(X,Y,PW,PH,Amber);
        UITextFit(TEXT("PAUSED"),White,X+20,Y+20,PW-40,1.8f);
        UIRect(Amber,X+20,Y+58,42.f,2.f/UIScale);
        UITextWrap(TEXT("Delivery, cargo and charging are paused."),Muted,X+20,Y+74,PW-40,2,.9f);
        const float First=Y+126.f,RowH=FMath::Min(42.f,(PH-138.f)/FMath::Max(1,Actions.Num())-8.f);
        for(int32 I=0;I<Actions.Num();++I)UIAction(Actions[I].Id,Actions[I].Label,X+14,First+I*(RowH+8.f),PW-28,RowH,Actions[I].bEnabled,I==0);
        return;
    }
    const float PW=FMath::Min(Screen==EMAYAPlayerScreen::Controls?880.f:700.f,W-60);
    const float DesiredPH=Screen==EMAYAPlayerScreen::Settings?556.f:Screen==EMAYAPlayerScreen::Controls?610.f:Screen==EMAYAPlayerScreen::ShiftWin?500.f:440.f;
    const float PH=FMath::Min(DesiredPH,H-24.f);
    const float X=(W-PW)*.5f,Y=(H-PH)*.5f;
    UIPanel(X,Y,PW,PH,Screen==EMAYAPlayerScreen::StartupError?Coral:Green);
    FString Title,Sub;
    switch(Screen)
    {
    case EMAYAPlayerScreen::Pause: Title=TEXT("DELIVERY PAUSED"); Sub=TEXT("Your delivery timer, cargo and charging are paused."); break;
    case EMAYAPlayerScreen::Controls: Title=TEXT("KEYBOARD & MOUSE"); Sub=TEXT("Plan the route. Protect the cargo. Watch your battery."); break;
    case EMAYAPlayerScreen::Settings: Title=TEXT("SETTINGS"); Sub=TEXT("Display, audio and accessibility"); break;
    case EMAYAPlayerScreen::VideoConfirmation:
        Title=TEXT("KEEP DISPLAY CHANGES?"); Sub=FString::Printf(TEXT("Reverting in %d seconds."),FMath::CeilToInt(PC->GetVideoConfirmationSeconds())); break;
    case EMAYAPlayerScreen::QuitConfirmation: Title=TEXT("END THIS SESSION?"); Sub=TEXT("Shift progress is not saved. Your settings are kept."); break;
    case EMAYAPlayerScreen::NewShiftConfirmation:
        Title=D && D->Snapshot().bDroneTotalled?TEXT("START OVER?"):TEXT("START A NEW SHIFT?");
        Sub=TEXT("The whole shift will reset. Begin again at $0 and 100% battery. Your settings are kept."); break;
    case EMAYAPlayerScreen::StartupError:
        Title=TEXT("UNABLE TO PREPARE THE CITY");
        if(const auto* Mode=Cast<AMAYADeliveryGameMode>(GetWorld()->GetAuthGameMode())) Sub=Mode->GetBootstrapStatus();
        break;
    case EMAYAPlayerScreen::ShiftWin: Title=TEXT("SHIFT COMPLETE"); Sub=TEXT("You reached the $100 cash goal."); break;
    default: break;
    }
    if(Screen==EMAYAPlayerScreen::Controls)
    {
        UITextFit(Title,White,X+20,Y+14,PW-40,1.6f);
        UITextFit(Sub,Muted,X+20,Y+52,PW-40,.9f);
        const TCHAR* Keys[]={TEXT("W / S"),TEXT("A / D"),TEXT("SPACE"),TEXT("LEFT SHIFT"),TEXT("E"),TEXT("J"),TEXT("P"),TEXT("M"),TEXT("C"),TEXT("R"),TEXT("N / ENTER"),TEXT("RIGHT MOUSE"),TEXT("HOME"),TEXT("GREEN RING")};
        const TCHAR* Descriptions[]={TEXT("Drive / reverse"),TEXT("Turn"),TEXT("Brake"),TEXT("Boost: speed / curb power"),TEXT("Load or deliver when stopped"),TEXT("Accept the offered delivery"),TEXT("Pause / resume"),TEXT("City map / close map"),TEXT("Cycle drone POV / chase camera"),TEXT("Recover to the depot"),TEXT("Choose offer / next job; Enter replays"),TEXT("Hold and drag to orbit"),TEXT("Re-centre the camera"),TEXT("Stay 10 seconds to recharge; $2")};
        const float RowH=(PH-178.f)/14.f,KeyW=PW*.27f;
        for(int32 I=0;I<14;++I)
        {
            const float CY=Y+86+I*RowH;
            UITextFit(Keys[I],Green,X+20,CY,KeyW-12,.95f);
            UITextFit(Descriptions[I],White,X+20+KeyW,CY,PW-KeyW-40,.95f);
        }
        UITextWrap(TEXT("Rings: automatic service after 10 seconds. Minimum cargo condition: 80%. Two parcels: 30% shorter battery endurance."),
            Muted,X+20,Y+PH-88,PW-214,3,.9f);
        UIAction(TEXT("Back"),TEXT("BACK"),X+PW-174,Y+PH-50,154,38); return;
    }
    UITextFit(Title,Screen==EMAYAPlayerScreen::StartupError?Coral:White,X+28,Y+24,PW-56,1.9f);
    UITextWrap(Sub,Muted,X+28,Y+65,PW-56,2,1.f);
    UILine(X+28,Y+105,X+PW-28,Y+105,Teal);
    if(Screen==EMAYAPlayerScreen::Settings)
    {
        const float RowH=FMath::Min(40.f,(PH-228.f)/7.f),First=Y+110;
        for(int32 I=0;I<7 && I<Actions.Num();++I)
        {
            const auto& A=Actions[I];const float RY=First+I*RowH;
            const bool Focused=PC->GetMenuFocus()==I;
            if(Focused)UIRect(MAYAUITheme::Alpha(Amber,bHighContrast?.20f:.12f),X+22,RY-2,PW-44,RowH-3);
            UIRect(MAYAUITheme::Alpha(Teal,.40f),X+22,RY+RowH-5,PW-44,1.f/UIScale);
            UITextFit(A.Label,White,X+34,RY+8,PW*.48f,1.f);
            UITextFit(A.Value,Focused?Green:White,X+PW*.60f,RY+8,PW*.30f,1.f);
            UIText(TEXT("<"),Green,X+PW*.55f,RY+8,1.1f); UIText(TEXT(">"),Green,X+PW-45,RY+8,1.1f);
            AddHitBox(FVector2D(X+22,RY-2)*UIScale,FVector2D(PW-44,RowH-3)*UIScale,A.Id,true,10);
            AddHitBox(FVector2D(X+PW*.53f,RY)*UIScale,FVector2D(30,RowH-3)*UIScale,FName(*(A.Id.ToString()+TEXT(".Previous"))),true,20);
            AddHitBox(FVector2D(X+PW-60,RY)*UIScale,FVector2D(38,RowH-3)*UIScale,FName(*(A.Id.ToString()+TEXT(".Next"))),true,20);
        }
        const float BW=(PW-80)/3;
        for(int32 I=7;I<Actions.Num();++I) UIAction(Actions[I].Id,Actions[I].Label,X+28+(I-7)*(BW+12),Y+PH-76,BW,42,true,I==7);
        UITextFit(PC->GetMenuStatus(),Muted,X+28,First+7*RowH+4,PW-56,.9f);
        UITextFit(TEXT("Left / Right adjusts the selected setting. Back discards unapplied changes."),Muted,X+28,Y+PH-25,PW-56,.85f); return;
    }
    if((Screen==EMAYAPlayerScreen::ShiftWin || Screen==EMAYAPlayerScreen::QuitConfirmation) && D)
    {
        UIText(FString::Printf(TEXT("$%d"),D->Snapshot().TotalCredits),Green,X+30,Y+135,3.f);
        UITextFit(FString::Printf(TEXT("CURRENT CASH  /  %d deliveries completed"),D->Snapshot().CompletedJobs),White,X+30,Y+187,PW-60,1.f);
        if(Screen==EMAYAPlayerScreen::ShiftWin) UITextFit(TEXT("Play Again begins a new shift at $0 with a full battery."),Muted,X+30,Y+221,PW-60,1.f);
    }
    const float BW=PW-56,First=Screen==EMAYAPlayerScreen::Pause?Y+130:Y+PH-Actions.Num()*54-22;
    for(int32 I=0;I<Actions.Num();++I) UIAction(Actions[I].Id,Actions[I].Label,X+28,First+I*54,BW,42,Actions[I].bEnabled,I==0);
    if(Screen==EMAYAPlayerScreen::VideoConfirmation) UITextWrap(PC->GetMenuStatus(),White,X+28,Y+150,PW-56,3,1.f);
}

void AMAYADeliveryHUD::DrawContextPanels(AMAYADeliveryDirector* D)
{
    auto* PC=Cast<AMAYADeliveryPlayerController>(GetOwningPlayerController());
    if (bSuppressUI || !D || !Canvas || (PC && PC->HasPlayerMenu())) return;
    using namespace MAYAScreenColors;
    const auto& S=D->Snapshot(); const auto& R=D->Observation(); const auto View=D->GetPlayerPresentation();
    const float X=ContextX,Y=ContextY,W=ContextWidth,H=FMath::Min(218.f,ContextHeight);
    auto Card=[&](const FString& Title,const FString& Detail,const FString& Extra,const FLinearColor& Accent)
    {
        UIPanel(X,Y,W,H,Accent);
        UITextFit(Title,Accent,X+12,Y+12,W-24,1.15f);
        UITextWrap(Detail,White,X+12,Y+43,W-24,2,.95f);
        UITextWrap(Extra,Muted,X+12,Y+FMath::Min(108.f,H-92.f),W-24,2,.9f);
    };
    if (View.bDroneTotalled)
    {
        Card(TEXT("DRONE TOTALLED"),TEXT("A moving car struck the drone. Shift lost."),TEXT("Start Over resets this game; your settings are kept."),Coral);
        UIAction(TEXT("NewShift"),TEXT("START OVER"),X+12,Y+H-45,W-24,33,View.bCanStartNewShift,true); return;
    }
    const bool Charging=View.Charge.bInside && (View.Charge.Stage==EMAYAChargeVisitStage::Qualifying || View.Charge.Stage==EMAYAChargeVisitStage::Charging);
    if (View.bRecoveryPending || !View.bKnown || !R.Fault.IsEmpty() || (S.BatteryPercent<=0 && !Charging))
    {
        const bool NewShift=S.BatteryPercent<=0 && S.TotalCredits<2;
        Card(View.bRecoveryPending?TEXT("RECOVERING"):!View.bKnown?TEXT("STATUS UNAVAILABLE"):
            !R.Fault.IsEmpty()?TEXT("ROBOT STOPPED"):TEXT("BATTERY EMPTY"),
            View.bRecoveryPending?TEXT("Restoring robot and cargo. Cash and battery are kept."):
            !View.bKnown?TEXT("Waiting for valid robot state."):!R.Fault.IsEmpty()?R.Fault:
            NewShift?TEXT("Recharging needs $2. Start a new shift to continue."):TEXT("Recover to the depot, then park in the green ring."),
            View.ActionReason,View.bRecoveryPending?Green:Coral);
        if (!View.bRecoveryPending) UIAction(NewShift?TEXT("NewShift"):TEXT("Recover"),NewShift?TEXT("START NEW SHIFT"):TEXT("[R] RECOVER"),
            X+12,Y+H-45,W-24,33,NewShift?View.bCanStartNewShift:View.bCanRecover,true);
        return;
    }
    if (S.Phase==EMAYADeliveryPhase::Loading || S.Phase==EMAYADeliveryPhase::Unloading)
    {
        Card(S.Phase==EMAYADeliveryPhase::Loading?TEXT("LOADING"):TEXT("UNLOADING"),
            View.ServiceStage.IsEmpty()?D->GetRuntimeStatus():View.ServiceStage,
            TEXT("Keep stopped. Timer continues."),S.Phase==EMAYADeliveryPhase::Loading?ServiceGreen:Coral);
        UITextFit(View.bServiceClosedObserved?TEXT("Doors latched"):View.bServiceTransferObserved?TEXT("Cargo transferred"):
            View.bServiceOpenObserved?TEXT("Doors open"):TEXT("Opening doors"),S.Phase==EMAYADeliveryPhase::Loading?ServiceGreen:Coral,X+12,Y+H-38,W-24,.9f);
        return;
    }
    const auto& Ring=D->GetRingObservation();
    if (Ring.bInside && Ring.bEligible && !Ring.bVisitConsumed)
    {
        const bool Pickup=S.Phase==EMAYADeliveryPhase::Accepted;
        Card(Pickup?TEXT("PICKUP READY"):TEXT("DELIVERY READY"),
            FString::Printf(TEXT("Automatic service in %ds."),FMath::CeilToInt(Ring.RemainingGameplaySeconds)),
            TEXT("Stay stopped inside the ring."),Pickup?ServiceGreen:Coral);
        UIAction(TEXT("Interact"),Pickup?TEXT("[E] LOAD NOW"):TEXT("[E] UNLOAD NOW"),X+12,Y+H-45,W-24,33,View.bCanInteract,true);
        return;
    }
    if (View.Charge.bInside && (S.Phase!=EMAYADeliveryPhase::Available || Charging))
    {
        const auto Stage=View.Charge.Stage;
        const bool Qualifying=Stage==EMAYAChargeVisitStage::Qualifying,Full=Stage==EMAYAChargeVisitStage::Full;
        Card(Full?TEXT("BATTERY FULL"):Stage==EMAYAChargeVisitStage::Charging?TEXT("RECHARGING"):TEXT("CHARGING ZONE"),
            Qualifying?FString::Printf(TEXT("Stay parked: %ds. Recharge costs $2."),FMath::CeilToInt(View.Charge.RemainingSeconds)):
            Stage==EMAYAChargeVisitStage::Charging?FString::Printf(TEXT("%.0f%%. Drive away when ready."),S.BatteryPercent):
            Full?TEXT("Ready to continue."):S.ChargeVisit.bConsumed?TEXT("Visit complete. Leave the ring to recharge again."):View.Charge.Reason,
            View.Charge.StationName,ServiceGreen); return;
    }
    if (S.Phase==EMAYADeliveryPhase::Available)
    {
        const auto* To=D->FindStation(S.Job.DropoffStationId);
        Card(View.SelectedOfferIndex>=View.AvailableOfferCount?TEXT("OFFER UNAVAILABLE"):
            FString::Printf(TEXT("DELIVERY OFFER %d / %d"),View.SelectedOfferIndex+1,View.AvailableOfferCount),To?To->DisplayName:S.Job.Title,
            FString::Printf(TEXT("%d parcel%s / $%d / %ds%s"),S.Job.ParcelCount,S.Job.ParcelCount==1?TEXT(""):TEXT("s"),
                S.Job.ParcelCount==2?16:10,FMath::RoundToInt(S.Job.DeadlineSeconds),
                View.bCityBlockOfferAvailable?TEXT(""):TEXT("\nCity-block offer unavailable")),Green);
        const float Half=(W-32)*.5f;
        UIAction(TEXT("Accept"),TEXT("J ACCEPT"),X+12,Y+H-45,Half,33,View.bCanAccept,true);
        UIAction(TEXT("ChooseOffer"),TEXT("N CHOOSE"),X+20+Half,Y+H-45,Half,33,View.bCanChooseOffer);
        return;
    }
    // One concise edge prompt replaces the old centre-bottom instruction block.
    const bool Low=S.BatteryPercent<20 || (S.CargoConditionPercent<=85 && S.Phase==EMAYADeliveryPhase::Carrying);
    if (!Low && HintOpacity<=0) return;
    UIGroupOpacity=Low?1.f:HintOpacity;
    UIPanel(X,Y,W,Low?130.f:76.f,Low?Amber:Green);
    UITextWrap(D->GetPrompt(),White,X+12,Y+12,W-24,Low?3:2,.95f);
    if (Low) UITextFit(S.BatteryPercent<20?TEXT("Recharge: green ring / $2"):TEXT("Minimum condition: 80%"),Amber,X+12,Y+101,W-24,.9f);
    UIGroupOpacity=1.f;
}
