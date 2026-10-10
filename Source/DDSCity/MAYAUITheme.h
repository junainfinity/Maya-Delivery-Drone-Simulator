#pragma once

#include "CoreMinimal.h"

// Atlas Noir presentation palette. Explicit sRGB conversion is shared by HUD,
// menus and the native city map; no simulation or game state belongs here.
namespace MAYAUITheme
{
    inline const FLinearColor Background = FLinearColor::FromSRGBColor(FColor(0x0A,0x0E,0x12,255));
    inline const FLinearColor Panel = FLinearColor::FromSRGBColor(FColor(0x13,0x19,0x1E,255));
    inline const FLinearColor Text = FLinearColor::FromSRGBColor(FColor(0xF3,0xF1,0xE9,255));
    inline const FLinearColor Muted = FLinearColor::FromSRGBColor(FColor(0xB5,0xBA,0xBD,255));
    inline const FLinearColor Border = FLinearColor::FromSRGBColor(FColor(0x6A,0x70,0x74,255));
    inline const FLinearColor Accent = FLinearColor::FromSRGBColor(FColor(0xE9,0xB8,0x5B,255));
    inline const FLinearColor Track = FLinearColor::FromSRGBColor(FColor(0x3B,0x41,0x46,255));
    inline const FLinearColor Road = FLinearColor::FromSRGBColor(FColor(0x9D,0xA5,0xAB,255));
    inline const FLinearColor Sidewalk = FLinearColor::FromSRGBColor(FColor(0x4C,0x55,0x5B,255));
    inline const FLinearColor Crossing = FLinearColor::FromSRGBColor(FColor(0x70,0x7A,0x7F,255));
    inline const FLinearColor Structure = FLinearColor::FromSRGBColor(FColor(0x23,0x2A,0x30,255));
    // Existing functional colors stay distinct from ordinary amber selection.
    inline const FLinearColor ServiceGreen(.035f,.93f,.36f,1.f);
    inline const FLinearColor DangerRed(1.f,.075f,.10f,1.f);
    inline FLinearColor Alpha(const FLinearColor& Color,float Opacity)
    { FLinearColor Result=Color; Result.A=Opacity; return Result; }
}
