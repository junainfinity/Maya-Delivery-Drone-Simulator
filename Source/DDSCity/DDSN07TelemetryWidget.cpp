#include "DDSN07TelemetryWidget.h"
#include "DDSN15Presentation.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

TSharedRef<SWidget> UDDSN07TelemetryWidget::RebuildWidget()
{
    if (WidgetTree->RootWidget)
    {
        return Super::RebuildWidget();
    }

    UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
    WidgetTree->RootWidget = Canvas;
    UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
    Backdrop->SetBrushColor(FLinearColor(0.012f, 0.025f, 0.04f, 0.82f));
    Backdrop->SetPadding(FMargin(17.0f, 12.0f));
    UCanvasPanelSlot* CanvasSlot = Canvas->AddChildToCanvas(Backdrop);
    CanvasSlot->SetAnchors(FAnchors(0.025f, 0.035f));
    CanvasSlot->SetPosition(FVector2D::ZeroVector);
    CanvasSlot->SetSize(FVector2D(690.0f, 390.0f));

    UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
    Backdrop->AddChild(Column);
    auto AddLine = [this, Column](const TCHAR* Name, int32 FontSize, const FLinearColor& Color) -> UTextBlock*
    {
        UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
        FSlateFontInfo Font = Line->GetFont();
        Font.Size = FontSize;
        Line->SetFont(Font);
        Line->SetColorAndOpacity(FSlateColor(Color));
        UVerticalBoxSlot* LineSlot = Column->AddChildToVerticalBox(Line);
        LineSlot->SetPadding(FMargin(0, 1, 0, 1));
        return Line;
    };

    ViewText = AddLine(TEXT("N07View"), 22, FLinearColor(0.19f, 0.93f, 1.0f));
    SpeedText = AddLine(TEXT("N07Speed"), 20, FLinearColor::White);
    ClearanceText = AddLine(TEXT("N07Clearance"), 20, FLinearColor::White);
    ObjectiveText = AddLine(TEXT("N07Objective"), 16, FLinearColor(0.75f, 0.83f, 0.89f));
    CargoText = AddLine(TEXT("N07Cargo"), 16, FLinearColor(0.75f, 0.83f, 0.89f));
    AdapterText = AddLine(TEXT("N07Adapter"), 15, FLinearColor(1.0f, 0.74f, 0.36f));
    PromptText = AddLine(TEXT("N08Prompt"), 15, FLinearColor(0.80f, 0.95f, 0.74f));
    ControlsText = AddLine(TEXT("N07Controls"), 14, FLinearColor(0.67f, 0.78f, 0.86f));
    RecoveryText = AddLine(TEXT("N07Recovery"), 14, FLinearColor(0.67f, 0.78f, 0.86f));
    MissionStatusText = AddLine(TEXT("MissionStatus"), 16, FLinearColor::White);
    TimerStarsText = AddLine(TEXT("TimerStars"), 16, FLinearColor(1.0f, 0.82f, 0.35f));
    ResultText = AddLine(TEXT("SavedResult"), 17, FLinearColor(0.80f, 0.95f, 0.74f));
    return Super::RebuildWidget();
}

void UDDSN07TelemetryWidget::SetViewModel(const FDDSN15ViewModel& Model,
    bool bInstantComfort, const FString& Recovery)
{
    if (!ViewText) return;
    ViewText->SetText(FText::FromString(TEXT("DDS CITY | ") + Model.View));
    SpeedText->SetText(FText::FromString(Model.Speed));
    ClearanceText->SetText(FText::FromString(Model.FrontClearance));
    ObjectiveText->SetText(FText::FromString(Model.Objective));
    CargoText->SetText(FText::FromString(Model.Cargo));
    AdapterText->SetText(FText::FromString(Model.Door));
    PromptText->SetText(FText::FromString(Model.InteractionPrompt));
    ControlsText->SetText(FText::FromString(FString::Printf(
        TEXT("W/S drive  A/D steer  Space brake  E interact  C view  V instant %s"),
        bInstantComfort ? TEXT("ON") : TEXT("OFF"))));
    RecoveryText->SetText(FText::FromString(TEXT("R hold 1s  Esc pause | ") + Recovery));
    MissionStatusText->SetText(FText::FromString(Model.Status));
    TimerStarsText->SetText(FText::FromString(Model.Timer + TEXT(" | ") + Model.Stars));
    ResultText->SetText(FText::FromString(Model.bShowResult
        ? Model.ResultTitle + TEXT(" | ") + Model.ResultDetail : FString()));
    // Saved result is a projection only. Dismiss/reopen/retry/replay controls
    // need the actual provider-backed menu integration; no auto replay occurs.
}

void UDDSN07TelemetryWidget::SetReadouts(const FString& View, const FString& Speed, const FString& Clearance,
    const FString& Prompt, const FString& Recovery,
    bool bInstantComfort, int32 ControlledAdapterPhase)
{
    if (!ViewText) { return; }
    ViewText->SetText(FText::FromString(FString::Printf(TEXT("DDS CITY | %s VIEW | N06/N07 PROXY"), *View)));
    SpeedText->SetText(FText::FromString(Speed));
    ClearanceText->SetText(FText::FromString(Clearance));
    ObjectiveText->SetText(FText::FromString(TEXT("Objective distance: Unavailable (no route/mission provider)")));
    CargoText->SetText(FText::FromString(TEXT("Cargo: Unavailable (no mission provider)")));
    static const TCHAR* Phases[] = { TEXT("Idle"), TEXT("Opening"), TEXT("Loading"), TEXT("Closing"), TEXT("Latched") };
    const int32 Phase = FMath::Clamp(ControlledAdapterPhase, 0, 4);
    AdapterText->SetText(FText::FromString(FString::Printf(
        TEXT("TEST ADAPTER only | phase %s | audio marker %s"), Phases[Phase], Phases[Phase])));
    PromptText->SetText(FText::FromString(Prompt));
    ControlsText->SetText(FText::FromString(FString::Printf(
        TEXT("W/S drive  A/D steer  Space brake  C view  V %s  F8 test phase"),
        bInstantComfort ? TEXT("instant ON") : TEXT("instant OFF"))));
    RecoveryText->SetText(FText::FromString(FString::Printf(TEXT("R hold 1s  Esc pause | %s"), *Recovery)));
}
