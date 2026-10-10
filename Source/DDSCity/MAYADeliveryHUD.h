#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Fonts/SlateFontInfo.h"
#include "MAYADeliveryHUDGeometry.h"
#include "MAYADeliveryHUD.generated.h"

class AMAYADeliveryDirector;
class UFont;
struct FMAYAHUDCaptureSession;
struct FMAYAFullMapCache;

UCLASS()
class DDSCITY_API AMAYADeliveryHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
    FString ReadFullMapEvidenceJson() const;
    FString ReadDrivingHUDEvidenceJson() const { return DrivingHUDEvidence; }
    FString ReadMinimapEvidenceJson() const; // Read-only bounded presentation receipt for native QA.
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void NotifyHitBoxClick(FName BoxName) override;
    virtual void NotifyHitBoxBeginCursorOver(FName BoxName) override;
private:
    FString DrivingHUDEvidence = TEXT("{}"); // Actual last driving DrawHUD values, never synthesized phase/data.
    TSharedPtr<FMAYAFullMapCache> FullMapCache;
    void DrawFullMap(AMAYADeliveryDirector* Director);
    void DrawPlayerMenus(AMAYADeliveryDirector* Director);
    void DrawContextPanels(AMAYADeliveryDirector* Director);
    void UIAction(FName Id, const FString& Label, float X, float Y, float W, float H, bool bEnabled = true, bool bPrimary = false);
    UPROPERTY(Transient) TObjectPtr<UFont> RuntimeUIFont;
    FSlateFontInfo TextFontInfo(float Scale);
    FVector2D MeasureUIText(const FString& Text, float Scale);
    FString HintContext;
    double HintStartedWorldSeconds = -1;
    float HintOpacity = 1.f, UIGroupOpacity = 1.f;
    bool bContextOccluded = false;
    bool bSuppressUI = false; // Passive group yields when it covers the projected robot.
    float ContextX = 16, ContextY = 94, ContextWidth = 280, ContextHeight = 218;
    float TextSizeFactor = 1.f;
    bool bHighContrast = false;
    bool bDrawingPlayerMenus = false; // Explicit menu panels retain their existing opacity.
    bool bMenuListStyle = false; // Main/Pause actual action lists; focus/hitboxes share the same rectangle.
    FString PresentedResultId;
    double ResultPresentedWorldSeconds = -1;
    // World-aligned measured surface cache; moving/render transforms never clear overlap.
    MAYAHUDGeometry::FSurroundingsCache RadarSurfaceCache;
    TArray<FIntPoint> RadarSampleOrder, RadarLastSampleKeys;
    int32 RadarRefreshCursor = 0, RadarLastFrameTraces = 0, RadarNewTraces = 0, RadarRefreshTraces = 0;
    int32 RadarDrawRuns = 0, RadarVisibleCells = 0;
    int32 RadarVisibleClassCounts[4] = {0,0,0,0};
    double RadarSampleNow = 0;
    FVector2D RadarWorldCentre = FVector2D::ZeroVector;
    double RadarWorldSpan = 0;
    int32 RadarAddressSide = -1; // retained across the35cm centre hysteresis band
    float UIScale = 1.f;
    TWeakObjectPtr<AMAYADeliveryDirector> CachedDirector;
    bool bQACaptureChecked = false;
    TSharedPtr<FMAYAHUDCaptureSession> QACapture;
    void UpdateQACapture(AMAYADeliveryDirector* Director);
    void OnQAScreenshotProcessed();
    void FinishQACapture(const FString& Reason, bool bSaved = false);
    void WriteQACaptureManifest();
    void UIText(const FString& Text, const FLinearColor& Color, float X, float Y, float Scale = 1.f);
    void UITextFit(const FString& Text, const FLinearColor& Color, float X, float Y, float Width, float Scale = 1.f);
    void UITextWrap(const FString& Text, const FLinearColor& Color, float X, float Y, float Width, int32 MaxLines, float Scale = 1.f);
    void UIRect(const FLinearColor& Color, float X, float Y, float Width, float Height);
    void UILine(float X1, float Y1, float X2, float Y2, const FLinearColor& Color, float Thickness = 1.f);
    void UIPanel(float X, float Y, float Width, float Height, const FLinearColor& Accent, float FillOpacity = -1.f);
    void UIRoundedPanel(float X, float Y, float Width, float Height, float Radius, float FillOpacity);
    void UIArc(float X, float Y, float Radius, double Fraction, const FLinearColor& Color, float Thickness);
};
