#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MAYAVehicleContactTypes.h"
#include "MAYAVehicleContactObserver.generated.h"

class AMAYANativeDemoPawn;
class UPrimitiveComponent;
struct FBodyInstance;

// Passive native vehicle history; no vehicle/input/pose/physics mutation.
UCLASS()
class DDSCITY_API UMAYAVehicleContactObserver : public UActorComponent
{
    GENERATED_BODY()
public:
    UMAYAVehicleContactObserver();
    void BindRobot(AMAYANativeDemoPawn* InRobot);
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Function) override;
    bool HasPendingIdentity() const { return Pending.Num() != 0; }
    bool ConsumeContact(FMAYAVehicleContactEvent& Out);
    const FMAYAVehicleContactEvent& GetLastContact() const { return LastContact; }
    FString ReadEvidenceJson() const;
private:
    friend class AMAYADeliveryDirector;
    void ObserveNativeHit(UPrimitiveComponent* RobotBody, AActor* Other, UPrimitiveComponent* OtherBody,
        const FVector& Impulse, const FHitResult& Hit);
    struct FSample
    {
        double Time = -1; uint64 Frame = 0;
        FVector COM = FVector::ZeroVector, Velocity = FVector::ZeroVector;
        FVector RobotCOM = FVector::ZeroVector, RobotVelocity = FVector::ZeroVector, RobotForward = FVector::ZeroVector;
    };
    struct FTrack
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Body;
        uint64 NativeActorIdentity = 0;
        bool bTyped = false, bMassKnown = false;
        int32 EntityIndex = INDEX_NONE, EntitySerial = 0, Representation = -1;
        double Speed = 0, Throttle = 0, Brake = 0;
        TArray<FSample> Samples;
    };
    struct FPending
    {
        FMAYAVehicleContactEvent Event;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UPrimitiveComponent> Body;
        uint64 NativeActorIdentity = 0;
        double FirstCallbackWorldSeconds = -1;
    };
    bool ReadMassIdentity(AActor* Actor, int32& Index, int32& Serial, int32& Representation,
        double& Speed, double& Throttle, double& Brake) const;
    void Record(FMAYAVehicleContactEvent Event);
    TWeakObjectPtr<AMAYANativeDemoPawn> Robot;
    TMap<TWeakObjectPtr<AActor>, FTrack> Tracks;
    struct FContactEpisode
    {
        double LastContactWorldSeconds = -1;
        uint64 NativeActorIdentity = 0;
        TWeakObjectPtr<UPrimitiveComponent> Body;
        bool bTyped = false;
        FMAYAVehicleContactEvent First;
    };
    TMap<TWeakObjectPtr<AActor>, FContactEpisode> ContactEpisodes;
    TArray<FPending> Pending;
    TArray<FMAYAVehicleContactEvent> Ready, History;
    FMAYAVehicleContactEvent LastContact;
    FMAYAVehicleContactEvent FirstQualifyingContact; // one durable actual strike, never rolling-evicted
    uint64 NextGeneration = 0;
    double LastDiscoveryWorldSeconds = -1;
};
