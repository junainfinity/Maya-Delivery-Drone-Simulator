#include "MAYAVehicleContactObserver.h"
#include "MAYANativeDemoPawn.h"
#include "Vehicles/CitySampleVehicleBase.h"
#include "MassTrafficVehicleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#include "CoreGlobals.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassEntityView.h"
#include "MassActorSubsystem.h"
#include "MassTrafficFragments.h"
#include "MassRepresentationFragments.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    uint64 NativeIdentity(const FBodyInstance* Body)
    { return Body && Body->IsValidBodyInstance() ? uint64(reinterpret_cast<UPTRINT>(Body->GetPhysicsActorHandle())) : 0; }
    bool BlockingNative(const UPrimitiveComponent* C)
    {
        const FBodyInstance* B = C ? C->GetBodyInstance() : nullptr;
        return IsValid(C) && C->IsPhysicsCollisionEnabled() && C->GetCollisionResponseToChannel(ECC_PhysicsBody) == ECR_Block
            && B && B->IsValidBodyInstance() && NativeIdentity(B) != 0;
    }
    TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
    { return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) }; }
}

UMAYAVehicleContactObserver::UMAYAVehicleContactObserver()
{
    PrimaryComponentTick.bCanEverTick = true;
    // Completed Mass PostPhysics; before next frame's physics. No fragment read
    // is attempted while the manager is processing, and no pointer is retained.
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UMAYAVehicleContactObserver::BindRobot(AMAYANativeDemoPawn* InRobot)
{ Robot = InRobot; Tracks.Reset(); ContactEpisodes.Reset(); Pending.Reset(); Ready.Reset(); LastDiscoveryWorldSeconds = -1; }

bool UMAYAVehicleContactObserver::ReadMassIdentity(AActor* Actor, int32& Index, int32& Serial,
    int32& Representation, double& Speed, double& Throttle, double& Brake) const
{
    if (!GetWorld() || !IsValid(Actor) || Actor->GetWorld() != GetWorld()) return false;
    auto* Actors = GetWorld()->GetSubsystem<UMassActorSubsystem>();
    auto* Entities = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
    if (!Actors || !Entities) return false;
    const auto& M = Entities->GetMutableEntityManager(); if (M.IsProcessing()) return false;
    const FMassEntityHandle E = Actors->GetEntityHandleFromActor(TObjectKey<const AActor>(Actor));
    if (!M.IsEntityActive(E)) return false;
    const FMassEntityView V(M, E);
    const auto* AF = V.GetFragmentDataPtr<FMassActorFragment>();
    if (!V.HasTag<FMassTrafficVehicleTag>() || !AF || AF->Get() != Actor) return false;
    Index = E.Index; Serial = E.SerialNumber;
    const auto* R = V.GetFragmentDataPtr<FMassRepresentationFragment>(); Representation = R ? int32(R->CurrentRepresentation) : -1;
    const auto* C = V.GetFragmentDataPtr<FMassTrafficVehicleControlFragment>(); Speed = C ? C->Speed : 0;
    const auto* P = V.GetFragmentDataPtr<FMassTrafficPIDVehicleControlFragment>();
    Throttle = P ? P->Throttle : 0; Brake = P ? P->Brake : 0;
    return FMath::IsFinite(Speed) && FMath::IsFinite(Throttle) && FMath::IsFinite(Brake);
}

void UMAYAVehicleContactObserver::TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt, Type, Function);
    if (!GetWorld() || GetWorld()->IsPaused() || !Robot.IsValid()) return;
    const double Now = GetWorld()->GetTimeSeconds();
    // Deferred adjudication keeps raw callback timestamp; unknown/expired data
    // is recorded and released, never promoted to an invented crash.
    for (int32 I = Pending.Num() - 1; I >= 0; --I)
    {
        auto& P = Pending[I]; int32 Index = -1, Serial = 0, Rep = -1; double Speed = 0, Throttle = 0, Brake = 0;
        const bool Expired = Now - P.FirstCallbackWorldSeconds > .25;
        const bool NativeSame = P.Actor.IsValid() && P.Body.IsValid() && P.Actor->GetWorld() == GetWorld()
            && BlockingNative(P.Body.Get()) && NativeIdentity(P.Body->GetBodyInstance()) == P.NativeActorIdentity;
        if (Expired || !NativeSame)
        { P.Event.Outcome = Expired ? TEXT("unresolved_identity_timeout") : TEXT("native_body_changed_before_identity"); Record(P.Event); Pending.RemoveAt(I); continue; }
        if (ReadMassIdentity(P.Actor.Get(), Index, Serial, Rep, Speed, Throttle, Brake))
        {
            P.Event.bVehicleIdentityKnown = Index == P.Event.MassEntityIndex && Serial == P.Event.MassEntitySerial;
            P.Event.ReconciledWorldSeconds = Now;
            P.Event.IdentityBasis = TEXT("actual_callback_body_and_same_mass_serial_actor_mapping_reconciled_within_0_25s");
            P.Event.Outcome = P.Event.IsFatal() ? TEXT("moving_car_front_or_rear_strike") : TEXT("ordinary_or_unresolved_vehicle_contact");
            if (auto* Episode = ContactEpisodes.Find(P.Actor))
                if (Episode->First.Generation == P.Event.Generation && Episode->NativeActorIdentity == P.NativeActorIdentity)
                    Episode->First = P.Event;
            Record(P.Event); Pending.RemoveAt(I);
        }
    }
    FMAYADeliveryRobotObservation R; if (!Robot->ReadDeliveryState(R) || !R.bKnown) return;
    auto* Mount = Robot->GetDeliveryCargoMount();
    auto* Chassis = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    auto* RobotNative = Chassis ? Chassis->GetBodyInstance() : nullptr;
    if (!RobotNative || !RobotNative->IsValidBodyInstance()) return;
    for (auto It = Tracks.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || It.Value().Samples.IsEmpty() || Now - It.Value().Samples.Last().Time > .5) It.RemoveCurrent();
    for (auto It = ContactEpisodes.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || Now - It.Value().LastContactWorldSeconds > 3) It.RemoveCurrent();
    if (LastDiscoveryWorldSeconds < 0 || Now - LastDiscoveryWorldSeconds >= .5)
    {
        LastDiscoveryWorldSeconds = Now;
        const auto Discover = [&](AActor* A)
        {
            if (!IsValid(A) || A->GetWorld() != GetWorld() || A == Robot.Get() || (Tracks.Num() >= 64 && !Tracks.Contains(A))
                || FVector::DistSquared2D(A->GetActorLocation(), R.ChassisPose.GetLocation()) > FMath::Square(3000.0)) return;
            auto* Typed = Cast<ACitySampleVehicleBase>(A);
            UPrimitiveComponent* C = Typed ? Cast<UPrimitiveComponent>(Typed->GetMesh()) : Cast<UPrimitiveComponent>(A->GetRootComponent());
            if (!BlockingNative(C)) return;
            FTrack& T = Tracks.FindOrAdd(A); T.Actor = A; T.Body = C; T.bTyped = Typed != nullptr;
        };
        // Sparse vehicle classes only, twice per gameplay second. Traverse the
        // complete sparse set so a later near vehicle is never prefix-starved;
        // the subsequent high-frequency native sampling remains capped at64.
        for (TActorIterator<ACitySampleVehicleBase> It(GetWorld()); It; ++It) Discover(*It);
        for (TObjectIterator<UMassTrafficVehicleComponent> It; It; ++It) Discover(It->GetOwner());
    }
    for (auto& Pair : Tracks)
    {
        auto& T = Pair.Value; AActor* A = T.Actor.Get(); auto* C = T.Body.Get();
        if (!IsValid(A) || A->GetWorld() != GetWorld() || !BlockingNative(C)
            || FVector::DistSquared2D(A->GetActorLocation(), R.ChassisPose.GetLocation()) > FMath::Square(3000.0)) continue;
        auto* B = C->GetBodyInstance(); const uint64 Native = NativeIdentity(B);
        const int32 PreviousIndex = T.EntityIndex, PreviousSerial = T.EntitySerial;
        const bool MassKnown = ReadMassIdentity(A, T.EntityIndex, T.EntitySerial, T.Representation, T.Speed, T.Throttle, T.Brake);
        if (T.NativeActorIdentity != Native || (!T.bTyped && MassKnown && (PreviousIndex != T.EntityIndex || PreviousSerial != T.EntitySerial))) T.Samples.Reset();
        T.NativeActorIdentity = Native; T.bMassKnown = MassKnown;
        if (!T.bTyped && !T.bMassKnown) continue;
        const FVector COM = B->GetCOMPosition(), Velocity = B->GetUnrealWorldVelocity();
        if (COM.ContainsNaN() || Velocity.ContainsNaN() || R.VelocityCmps.ContainsNaN() || R.ChassisPose.ContainsNaN()) continue;
        FSample S; S.Time = Now; S.Frame = GFrameCounter; S.COM = COM; S.Velocity = Velocity;
        S.RobotCOM = RobotNative->GetCOMPosition(); S.RobotVelocity = RobotNative->GetUnrealWorldVelocity();
        S.RobotForward = -R.ChassisPose.GetUnitAxis(EAxis::X);
        if (S.RobotCOM.ContainsNaN() || S.RobotVelocity.ContainsNaN() || S.RobotForward.ContainsNaN()) continue;
        if (!T.Samples.IsEmpty() && T.Samples.Last().Time >= Now) continue;
        T.Samples.Add(S);
        while (T.Samples.Num() > 64 || (!T.Samples.IsEmpty() && Now - T.Samples[0].Time > .5)) T.Samples.RemoveAt(0);
    }
}

void UMAYAVehicleContactObserver::ObserveNativeHit(UPrimitiveComponent* RobotBody, AActor* Other,
    UPrimitiveComponent* OtherBody, const FVector& Impulse, const FHitResult& Hit)
{
    if (!GetWorld() || GetWorld()->IsPaused() || !Robot.IsValid() || !IsValid(Other) || Other->GetWorld() != GetWorld() || Other == Robot.Get()
        || !BlockingNative(OtherBody) || OtherBody->GetOwner() != Other || !RobotBody || Impulse.ContainsNaN() || Hit.ImpactNormal.ContainsNaN()) return;
    const double Now = GetWorld()->GetTimeSeconds();
    const FTrack* T = Tracks.Find(Other);
    if (!T && !Cast<ACitySampleVehicleBase>(Other) && !Other->FindComponentByClass<UMassTrafficVehicleComponent>()) return;
    FContactEpisode& Episode = ContactEpisodes.FindOrAdd(Other);
    const double Quiet = Episode.LastContactWorldSeconds < 0 ? 1000 : Now - Episode.LastContactWorldSeconds;
    Episode.LastContactWorldSeconds = Now;
    if (Episode.First.Generation != 0 && Quiet < .6)
    {
        FMAYAVehicleContactEvent Later = Episode.First; Later.bEpisodeContinuation = true;
        Later.WorldSeconds = Now; Later.CallbackGameFrame = GFrameCounter; Later.Normal = Hit.ImpactNormal;
        Later.ContactPointCm = Hit.ImpactPoint; Later.ImpulseKgMps = Impulse.Size() / 100;
        Later.RobotComponent = RobotBody->GetPathName(); Later.VehicleComponent = OtherBody->GetPathName();
        Later.bBlockingHit = Hit.bBlockingHit; Later.bStartPenetrating = Hit.bStartPenetrating;
        Later.bSameNativeBody = T && Episode.Body.IsValid() && NativeIdentity(Episode.Body->GetBodyInstance()) == Episode.NativeActorIdentity
            && NativeIdentity(OtherBody->GetBodyInstance()) == Episode.NativeActorIdentity
            && (Episode.bTyped || (T->EntityIndex == Episode.First.MassEntityIndex && T->EntitySerial == Episode.First.MassEntitySerial));
        auto* Mount = Robot->GetDeliveryCargoMount();
        auto* Chassis = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
        auto* Native = Chassis ? Chassis->GetBodyInstance() : nullptr;
        if (Native && Native->IsValidBodyInstance())
        { Later.RobotContactCOMCm = Native->GetCOMPosition(); Later.RobotOpticalForward = -Native->GetUnrealWorldTransform().GetUnitAxis(EAxis::X); }
        else Later.bSameNativeBody = false;
        // Geometry may become longitudinal on a later subscribed robot body.
        // Motion stays FIRST/pre-strike and must still satisfy the original
        // <=.25s cache age at this callback; newly pushed car motion never arms it.
        for (auto& P : Pending)
            if (P.Event.Generation == Later.Generation)
            {
                auto PriorGeometry = P.Event, NewGeometry = Later;
                PriorGeometry.bVehicleIdentityKnown = NewGeometry.bVehicleIdentityKnown = true;
                if (!PriorGeometry.IsFatal() && NewGeometry.IsFatal()) { P.Event = Later; P.Event.bIdentityDeferred = true; }
            }
        Later.Outcome = TEXT("same_contact_episode_first_prestrike_classification_retained_no_postcollision_motion_promotion");
        Record(Later); return;
    }
    FMAYAVehicleContactEvent Unknown; Unknown.Generation = ++NextGeneration; Unknown.WorldSeconds = Now;
    Unknown.CallbackGameFrame = GFrameCounter; Unknown.bNativeContact = true; Unknown.bBlockingHit = Hit.bBlockingHit; Unknown.bStartPenetrating = Hit.bStartPenetrating;
    Unknown.VehicleActor = Other->GetPathName(); Unknown.VehicleComponent = OtherBody->GetPathName(); Unknown.RobotComponent = GetPathNameSafe(RobotBody);
    Unknown.Normal = Hit.ImpactNormal; Unknown.ContactPointCm = Hit.ImpactPoint; Unknown.Outcome = TEXT("first_contact_precontact_history_unknown_or_stale");
    Episode.First = Unknown;
    Episode.NativeActorIdentity = T ? T->NativeActorIdentity : NativeIdentity(OtherBody->GetBodyInstance());
    Episode.Body = OtherBody; Episode.bTyped = T && T->bTyped;
    if (!T || T->Samples.IsEmpty()) { Record(Unknown); return; }
    const FSample* Latest = nullptr; int32 LatestIndex = -1;
    for (int32 I = T->Samples.Num() - 1; I >= 0; --I)
        if (T->Samples[I].Time < Now && T->Samples[I].Frame < GFrameCounter) { Latest = &T->Samples[I]; LatestIndex = I; break; }
    if (!Latest || Now - Latest->Time > .25) { Record(Unknown); return; }
    FMAYAVehicleContactEvent E; E.Generation = Unknown.Generation; E.WorldSeconds = Now; E.CallbackGameFrame = GFrameCounter;
    E.LatestSampleWorldSeconds = Latest->Time; E.bNativeContact = true;
    E.bBlockingHit = Hit.bBlockingHit; E.bStartPenetrating = Hit.bStartPenetrating;
    E.VehicleActor = Other->GetPathName(); E.VehicleComponent = OtherBody->GetPathName(); E.RobotComponent = RobotBody->GetPathName();
    E.Normal = Hit.ImpactNormal.GetSafeNormal(); E.ContactPointCm = Hit.ImpactPoint;
    E.CarCOMCm = Latest->COM; E.RobotCOMCm = Latest->RobotCOM;
    auto* Mount = Robot->GetDeliveryCargoMount();
    auto* Chassis = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    auto* RobotNative = Chassis ? Chassis->GetBodyInstance() : nullptr;
    if (!RobotNative || !RobotNative->IsValidBodyInstance()) return;
    E.RobotContactCOMCm = RobotNative->GetCOMPosition();
    E.RobotOpticalForward = -RobotNative->GetUnrealWorldTransform().GetUnitAxis(EAxis::X);
    E.CarVelocityCmps = FVector(Latest->Velocity.X, Latest->Velocity.Y, 0);
    E.RobotVelocityCmps = FVector(Latest->RobotVelocity.X, Latest->RobotVelocity.Y, 0);
    E.bSameNativeBody = NativeIdentity(OtherBody->GetBodyInstance()) == T->NativeActorIdentity;
    E.bMassKnown = T->bMassKnown; E.MassEntityIndex = T->EntityIndex; E.MassEntitySerial = T->EntitySerial;
    E.Representation = T->Representation; E.MassSpeedCmps = T->Speed; E.MassThrottle = T->Throttle; E.MassBrake = T->Brake;
    E.ImpulseKgMps = Impulse.Size() / 100;
    const FVector Toward = (E.RobotCOMCm - E.CarCOMCm).GetSafeNormal2D();
    int32 First = LatestIndex;
    // Only the most recent coherent approach suffix qualifies. An observed
    // stopped/away sample breaks it; older progress cannot arm a parked bump.
    if (FVector::DotProduct(E.CarVelocityCmps, Toward) > 0)
        for (int32 I = LatestIndex - 1; I >= 0; --I)
        {
            const auto& Older = T->Samples[I]; const auto& Newer = T->Samples[I + 1];
            if (Latest->Time - Older.Time > .5 || FVector::DotProduct(FVector(Older.Velocity.X, Older.Velocity.Y, 0), Toward) <= 0
                || FVector::DotProduct(Newer.COM - Older.COM, Toward) <= 0) break;
            First = I;
        }
    E.CoherentSamples = LatestIndex - First + 1; E.CoherentSpanSeconds = Latest->Time - T->Samples[First].Time;
    E.CoherentCarProgressCm = Latest->COM - T->Samples[First].COM; E.CoherentCarProgressCm.Z = 0;
    for (int32 I = First; I <= LatestIndex; ++I)
    {
        const auto& S = T->Samples[I]; FMAYAVehiclePrecontactSample Raw;
        Raw.WorldSeconds = S.Time; Raw.GameFrame = S.Frame; Raw.CarCOMCm = S.COM; Raw.CarVelocityCmps = S.Velocity;
        Raw.RobotCOMCm = S.RobotCOM; Raw.RobotVelocityCmps = S.RobotVelocity; E.PrecontactSamples.Add(Raw);
    }
    E.bHistoryKnown = E.CoherentSamples >= 3 && E.CoherentSpanSeconds >= .1;
    E.bVehicleIdentityKnown = T->bTyped && Cast<ACitySampleVehicleBase>(Other) != nullptr && E.bSameNativeBody;
    E.IdentityBasis = TEXT("exported_original_citysample_vehicle_type_and_same_actual_native_body_precontact_history");
    Episode.First = E;
    if (E.bBlockingHit && !E.bStartPenetrating && !E.bVehicleIdentityKnown && T->bMassKnown && E.bSameNativeBody)
    {
        if (Pending.Num() < 8)
        { E.bIdentityDeferred = true; FPending P; P.Event = E; P.Actor = Other; P.Body = OtherBody; P.NativeActorIdentity = T->NativeActorIdentity; P.FirstCallbackWorldSeconds = Now; Pending.Add(P); LastContact = E; }
        else { E.Outcome = TEXT("unresolved_identity_queue_full"); Record(E); }
        return;
    }
    E.Outcome = E.IsFatal() ? TEXT("moving_car_front_or_rear_strike") : TEXT("ordinary_vehicle_bump"); Record(E);
}

void UMAYAVehicleContactObserver::Record(FMAYAVehicleContactEvent E)
{
    if (FirstQualifyingContact.Generation == 0 && E.IsFatal()) FirstQualifyingContact = E;
    LastContact = E; History.Add(E); if (History.Num() > 64) History.RemoveAt(0);
    if (Ready.Num() < 64) Ready.Add(E);
    else if (E.IsFatal()) Ready.Last() = E; // never discard the decisive actual contact behind incidental hits
}

bool UMAYAVehicleContactObserver::ConsumeContact(FMAYAVehicleContactEvent& Out)
{ if (Ready.IsEmpty()) return false; Out = Ready[0]; Ready.RemoveAt(0); return true; }

FString UMAYAVehicleContactObserver::ReadEvidenceJson() const
{
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeVehicleContact.v1"));
    J->SetStringField(TEXT("scope"), TEXT("Actual native hits plus strictly precontact native COM motion; no proximity loss, speed floor or injected contact."));
    J->SetBoolField(TEXT("pendingIdentity"), HasPendingIdentity());
    TArray<TSharedPtr<FJsonValue>> Rows;
    TArray<FMAYAVehicleContactEvent> Evidence = History;
    if (FirstQualifyingContact.Generation != 0) Evidence.Insert(FirstQualifyingContact, 0);
    bool DurableWritten = false;
    for (const auto& E : Evidence)
    {
        auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("generation"), double(E.Generation)); R->SetNumberField(TEXT("callbackFrame"), double(E.CallbackGameFrame));
        R->SetNumberField(TEXT("worldSeconds"), E.WorldSeconds); R->SetNumberField(TEXT("latestPrecontactSeconds"), E.LatestSampleWorldSeconds);
        R->SetStringField(TEXT("vehicleActor"), E.VehicleActor); R->SetStringField(TEXT("vehicleComponent"), E.VehicleComponent); R->SetStringField(TEXT("robotComponent"), E.RobotComponent);
        R->SetStringField(TEXT("identityBasis"), E.IdentityBasis); R->SetStringField(TEXT("outcome"), E.Outcome);
        R->SetBoolField(TEXT("blockingHit"), E.bBlockingHit); R->SetBoolField(TEXT("startPenetrating"), E.bStartPenetrating);
        R->SetBoolField(TEXT("episodeContinuation"), E.bEpisodeContinuation);
        R->SetBoolField(TEXT("fatal"), E.IsFatal()); R->SetBoolField(TEXT("identityDeferred"), E.bIdentityDeferred); R->SetNumberField(TEXT("reconciledSeconds"), E.ReconciledWorldSeconds);
        R->SetArrayField(TEXT("carPrecontactVelocityCmps"), Vec(E.CarVelocityCmps)); R->SetArrayField(TEXT("robotPrecontactVelocityCmps"), Vec(E.RobotVelocityCmps));
        R->SetArrayField(TEXT("coherentCarProgressCm"), Vec(E.CoherentCarProgressCm)); R->SetArrayField(TEXT("contactNormal"), Vec(E.Normal)); R->SetArrayField(TEXT("contactPointCm"), Vec(E.ContactPointCm));
        R->SetArrayField(TEXT("carPrecontactCOMCm"), Vec(E.CarCOMCm)); R->SetArrayField(TEXT("robotPrecontactCOMCm"), Vec(E.RobotCOMCm));
        R->SetArrayField(TEXT("robotContactCOMCm"), Vec(E.RobotContactCOMCm)); R->SetArrayField(TEXT("robotOpticalForward"), Vec(E.RobotOpticalForward));
        TArray<TSharedPtr<FJsonValue>> Samples;
        for (const auto& S : E.PrecontactSamples)
        {
            auto Sample = MakeShared<FJsonObject>(); Sample->SetNumberField(TEXT("worldSeconds"), S.WorldSeconds); Sample->SetNumberField(TEXT("gameFrame"), double(S.GameFrame));
            Sample->SetArrayField(TEXT("carCOMCm"), Vec(S.CarCOMCm)); Sample->SetArrayField(TEXT("carVelocityCmps"), Vec(S.CarVelocityCmps));
            Sample->SetArrayField(TEXT("robotCOMCm"), Vec(S.RobotCOMCm)); Sample->SetArrayField(TEXT("robotVelocityCmps"), Vec(S.RobotVelocityCmps)); Samples.Add(MakeShared<FJsonValueObject>(Sample));
        }
        R->SetArrayField(TEXT("strictlyPrecontactNativeSamples"), Samples);
        R->SetNumberField(TEXT("coherentSamples"), E.CoherentSamples); R->SetNumberField(TEXT("coherentSpanSeconds"), E.CoherentSpanSeconds); R->SetNumberField(TEXT("impulseKgMps"), E.ImpulseKgMps);
        R->SetBoolField(TEXT("massKnown"), E.bMassKnown); R->SetNumberField(TEXT("entityIndex"), E.MassEntityIndex); R->SetNumberField(TEXT("entitySerial"), E.MassEntitySerial);
        R->SetNumberField(TEXT("representation"), E.Representation); R->SetNumberField(TEXT("massSpeedCmps"), E.MassSpeedCmps); R->SetNumberField(TEXT("massThrottle"), E.MassThrottle); R->SetNumberField(TEXT("massBrake"), E.MassBrake);
        if (!DurableWritten && FirstQualifyingContact.Generation != 0)
        { J->SetObjectField(TEXT("firstQualifyingEvent"), R); DurableWritten = true; }
        else Rows.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetArrayField(TEXT("contacts"), Rows); FString Out; auto Writer = TJsonWriterFactory<>::Create(&Out); FJsonSerializer::Serialize(J, Writer); return Out;
}
