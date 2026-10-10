#include "MAYAPresentationSettings.h"
#include "AudioDevice.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"

void UMAYAPresentationSettings::LoadPreferences()
{
    LoadConfig(nullptr, *GGameUserSettingsIni);
    MasterVolume = FMath::IsFinite(MasterVolume) ? FMath::Clamp(MasterVolume, 0.f, 1.f) : .8f;
    InterfaceScale = FMath::IsFinite(InterfaceScale) ? FMath::Clamp(InterfaceScale, .9f, 1.1f) : 1.f;
}

void UMAYAPresentationSettings::SavePreferences()
{
    SaveConfig(CPF_Config, *GGameUserSettingsIni);
}

void UMAYAPresentationSettings::ApplyAudio(UWorld* World) const
{
    if (World)
    {
        FAudioDeviceHandle Device = World->GetAudioDevice();
        if (Device.IsValid()) Device->SetTransientPrimaryVolume(MasterVolume);
    }
}
