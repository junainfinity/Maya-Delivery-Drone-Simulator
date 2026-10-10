#pragma once

#include "CoreMinimal.h"
#include "DDSN08Authority.h"
#include "DDSCityRollingPawn.h"

class ADDSCityRollingPawn;

// Plain text/data for a later UMG view. Neither this type nor Build mutates
// N08, stores receipts/stars, starts a mission, or writes settings.
struct FDDSN15ViewModel final
{
    FString View;
    FString Objective;
    FString Cargo;
    FString Door;
    FString Status;
    FString Timer;
    FString Stars;
    FString Speed;
    FString FrontClearance;
    FString InteractionPrompt;
    FString ResultTitle;
    FString ResultDetail;
    FString RestoreWarning;
    bool bShowResult = false;
    bool bOfferRetrySave = false;
    bool bOfferReplay = false;
    bool bOfferRestore = false;
};

enum class EDDSN15ActionIntent : uint8
{
    Pause,
    Resume,
    Quit,
    RetrySave,
    RestoreCheckpoint,
    ReplayMission
};

struct FDDSN15ProjectionInputs final
{
    FString PickupDisplayName = TEXT("Pickup point");
    FString DeliveryDisplayName = TEXT("Delivery point");
    FString N09InteractionPrompt;
    bool bRealN12MechanicsAvailable = false;
    // Supplied ONLY by the actual N12 observer on this owned pawn, sampled on
    // the same game thread. Availability alone proves no physical door fact.
    FString DoorObservationMissionId;
    double NowWorldSeconds = -1.0;
    double DoorObservationWorldSeconds = -1.0;
    bool bNativeDoorObservationKnown = false;
    bool bLeftLidAtClosedPose = false;
    bool bRightLidAtClosedPose = false;
    bool bLeftLatchVerified = false;
    bool bRightLatchVerified = false;
};

// Read-only samples from the same owned pawn. This overload also permits a
// controlled projection fixture without spawning a replacement pawn/provider.
struct FDDSN15PawnProjection final
{
    EDDSN07CameraMode CameraMode = EDDSN07CameraMode::Chase;
    FDDSN07TelemetrySample Telemetry;
};

class FDDSN15Presentation final
{
public:
    // The caller samples one game-thread N08 authority and the same possessed
    // N06/N07 pawn. A different pawn or controlled adapter is not a source.
    static FDDSN15ViewModel Build(const FDDSN08Authority& Authority,
        const ADDSCityRollingPawn& OwnedPawn,
        const FDDSN15ProjectionInputs& Inputs);

    static FDDSN15ViewModel Build(const FDDSN08Authority& Authority,
        const FDDSN15PawnProjection& Pawn,
        const FDDSN15ProjectionInputs& Inputs);

    static bool IsIntentOffered(EDDSN15ActionIntent Intent,
        const FDDSN15ViewModel& Model);
};
