#include "DDSN06PauseMenu.h"

#include "DDSCityRollingPawn.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

void UDDSN06PauseMenu::SetPawn(ADDSCityRollingPawn* InPawn)
{
    Pawn = InPawn;
    RefreshStatus();
}

void UDDSN06PauseMenu::NativeConstruct()
{
    Super::NativeConstruct();
    RefreshStatus();
}

TSharedRef<SWidget> UDDSN06PauseMenu::RebuildWidget()
{
    // UUserWidget builds Slate from RootWidget before NativeConstruct runs.
    // Construct the C++ widget tree first so the menu is visible in game.
    if (WidgetTree->RootWidget)
    {
        return Super::RebuildWidget();
    }
    UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
    WidgetTree->RootWidget = Canvas;
    UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
    Backdrop->SetBrushColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.93f));
    Backdrop->SetPadding(FMargin(18.0f));
    UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Backdrop);
    PanelSlot->SetAnchors(FAnchors(0.5f, 0.16f));
    PanelSlot->SetAlignment(FVector2D(0.5f, 0.0f));
    PanelSlot->SetPosition(FVector2D::ZeroVector);
    PanelSlot->SetSize(FVector2D(490, 200));
    UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
    Backdrop->AddChild(Column);

    UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    Title->SetText(FText::FromString(TEXT("DDS CITY — PAUSED")));
    FSlateFontInfo TitleFont = Title->GetFont();
    TitleFont.Size = 23;
    Title->SetFont(TitleFont);
    Column->AddChildToVerticalBox(Title);
    StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    StatusText->SetAutoWrapText(true);
    UVerticalBoxSlot* StatusSlot = Column->AddChildToVerticalBox(StatusText);
    StatusSlot->SetPadding(FMargin(0, 7, 0, 13));

    UButton* RecoverButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
    UTextBlock* RecoverText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    RecoverText->SetText(FText::FromString(TEXT("Recover robot")));
    RecoverText->SetColorAndOpacity(FSlateColor(FLinearColor(0.02f, 0.03f, 0.05f, 1.0f)));
    RecoverButton->AddChild(RecoverText);
    RecoverButton->OnClicked.AddDynamic(this, &UDDSN06PauseMenu::OnRecoverClicked);
    Column->AddChildToVerticalBox(RecoverButton);

    UButton* ResumeButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
    UTextBlock* ResumeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    ResumeText->SetText(FText::FromString(TEXT("Resume")));
    ResumeText->SetColorAndOpacity(FSlateColor(FLinearColor(0.02f, 0.03f, 0.05f, 1.0f)));
    ResumeButton->AddChild(ResumeText);
    ResumeButton->OnClicked.AddDynamic(this, &UDDSN06PauseMenu::OnResumeClicked);
    Column->AddChildToVerticalBox(ResumeButton);
    return Super::RebuildWidget();
}

void UDDSN06PauseMenu::RefreshStatus()
{
    if (StatusText && Pawn.IsValid())
    {
        StatusText->SetText(FText::FromString(Pawn->GetRecoveryStatus()));
    }
}

void UDDSN06PauseMenu::OnRecoverClicked()
{
    if (Pawn.IsValid())
    {
        UE_LOG(LogTemp, Display, TEXT("N06_MENU recover_clicked"));
        Pawn->RequestRecoveryViaAuthority(true);
        RefreshStatus();
    }
}

void UDDSN06PauseMenu::OnResumeClicked()
{
    if (Pawn.IsValid())
    {
        UE_LOG(LogTemp, Display, TEXT("N06_MENU resume_clicked"));
        Pawn->ResumeFromPauseMenu();
    }
}
