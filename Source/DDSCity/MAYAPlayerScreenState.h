#pragma once

#include "CoreMinimal.h"

enum class EMAYAPlayerScreen : uint8 { Gameplay, MainMenu, Pause, Controls, Settings, QuitConfirmation, NewShiftConfirmation, VideoConfirmation, StartupError, ShiftWin, ShiftLost, CityMap };

// One bounded navigation stack; nested menus return to their actual caller.
struct FMAYAPlayerScreenState
{
    TArray<EMAYAPlayerScreen> Stack;
    int32 FocusIndex = 0;
    EMAYAPlayerScreen Current() const { return Stack.IsEmpty() ? EMAYAPlayerScreen::Gameplay : Stack.Last(); }
    bool IsOpen() const { return !Stack.IsEmpty(); }
    void SetRoot(EMAYAPlayerScreen Screen) { Stack.Reset(); if (Screen != EMAYAPlayerScreen::Gameplay) Stack.Add(Screen); FocusIndex = 0; }
    bool Push(EMAYAPlayerScreen Screen)
    {
        if (Screen == EMAYAPlayerScreen::Gameplay || Stack.Num() >= 6 || Current() == Screen) return false;
        Stack.Add(Screen); FocusIndex = 0; return true;
    }
    void Back() { if (!Stack.IsEmpty()) Stack.Pop(false); FocusIndex = 0; }
    void MoveFocus(int32 Delta, int32 Count) { FocusIndex = Count > 0 ? (FocusIndex + Delta % Count + Count) % Count : 0; }
};

struct FMAYAPlayerMenuAction
{
    FName Id;
    FString Label, Value;
    bool bEnabled = true;
    bool bAdjustable = false;
};
