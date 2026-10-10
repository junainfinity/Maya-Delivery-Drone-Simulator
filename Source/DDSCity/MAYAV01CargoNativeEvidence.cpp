#include "MAYAV01CargoNativeEvidence.h"
#include "MAYADeliveryParcel.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Chaos/PBDJointConstraintData.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
TArray<TSharedPtr<FJsonValue>> MayaV01CargoVector(const FVector& V)
{
    return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
}

TSharedRef<FJsonObject> MayaV01CargoPose(const FTransform& T)
{
    auto O = MakeShared<FJsonObject>();
    const bool Known = !T.ContainsNaN() && T.GetRotation().IsNormalized();
    O->SetBoolField(TEXT("known"), Known);
    if (Known)
    {
        O->SetArrayField(TEXT("positionCm"), MayaV01CargoVector(T.GetLocation()));
        O->SetArrayField(TEXT("scale"), MayaV01CargoVector(T.GetScale3D()));
        const FQuat Q = T.GetRotation();
        O->SetArrayField(TEXT("quaternion"), { MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
            MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W) });
    }
    return O;
}

TSharedRef<FJsonObject> MayaV01CargoBody(UPrimitiveComponent* Component, FTransform& NativeActorPose, bool& bNativeKnown)
{
    auto O = MakeShared<FJsonObject>(); bNativeKnown = false;
    O->SetBoolField(TEXT("componentKnown"), IsValid(Component));
    if (!IsValid(Component)) return O;
    O->SetStringField(TEXT("component"), Component->GetPathName());
    O->SetBoolField(TEXT("componentSimulating"), Component->IsSimulatingPhysics());
    const FTransform ComponentPose = Component->GetComponentTransform();
    O->SetObjectField(TEXT("componentPose"), MayaV01CargoPose(ComponentPose));
    const auto* BI = Component->GetBodyInstance();
    O->SetBoolField(TEXT("bodyInstanceValid"), BI && BI->IsValidBodyInstance());
    if (!BI || !BI->IsValidBodyInstance()) return O;
    bool Entered = false, Observed = false;
    const bool ReadSucceeded = FPhysInterface_Chaos::ExecuteRead(BI->GetPhysicsActorHandle(), [&](const FPhysicsActorHandle& Actor)
    {
        Entered = true;
        if (!FPhysInterface_Chaos::IsValid(Actor)) return;
        NativeActorPose = FPhysInterface_Chaos::GetGlobalPose_AssumesLocked(Actor);
        if (NativeActorPose.ContainsNaN() || !NativeActorPose.GetRotation().IsNormalized()) return;
        Observed = true;
        O->SetObjectField(TEXT("nativeActorPose"), MayaV01CargoPose(NativeActorPose));
        O->SetNumberField(TEXT("nativeObjectState"), int32(Actor->GetGameThreadAPI().ObjectState()));
        if (!ComponentPose.ContainsNaN())
        {
            O->SetArrayField(TEXT("componentMinusNativePositionCm"), MayaV01CargoVector(ComponentPose.GetLocation() - NativeActorPose.GetLocation()));
            O->SetNumberField(TEXT("componentNativeAngularDeltaDegrees"), FMath::RadiansToDegrees(ComponentPose.GetRotation().AngularDistance(NativeActorPose.GetRotation())));
        }
        const FBox Bounds = FPhysInterface_Chaos::GetBounds_AssumesLocked(Actor);
        const bool BoundsKnown = Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN() && Bounds.GetSize().GetMin() > 0;
        O->SetBoolField(TEXT("nativeCollisionBoundsKnown"), BoundsKnown);
        if (BoundsKnown)
        {
            O->SetArrayField(TEXT("nativeCollisionBoundsMinCm"), MayaV01CargoVector(Bounds.Min));
            O->SetArrayField(TEXT("nativeCollisionBoundsMaxCm"), MayaV01CargoVector(Bounds.Max));
        }
        if (FPhysInterface_Chaos::IsDynamic(Actor))
        {
            const double Mass = FPhysInterface_Chaos::GetMass_AssumesLocked(Actor);
            const FVector Inertia = FPhysInterface_Chaos::GetLocalInertiaTensor_AssumesLocked(Actor);
            O->SetBoolField(TEXT("nativeMassKnown"), FMath::IsFinite(Mass) && Mass > 0);
            if (FMath::IsFinite(Mass) && Mass > 0) O->SetNumberField(TEXT("nativeMassKg"), Mass);
            O->SetBoolField(TEXT("nativeInertiaKnown"), !Inertia.ContainsNaN() && Inertia.GetMin() > 0);
            if (!Inertia.ContainsNaN() && Inertia.GetMin() > 0) O->SetArrayField(TEXT("nativeInertiaMassSpaceKgCm2"), MayaV01CargoVector(Inertia));
            O->SetObjectField(TEXT("nativeCOMWorldPose"), MayaV01CargoPose(FPhysInterface_Chaos::GetComTransform_AssumesLocked(Actor)));
            O->SetObjectField(TEXT("nativeCOMLocalPose"), MayaV01CargoPose(FPhysInterface_Chaos::GetComTransformLocal_AssumesLocked(Actor)));
            O->SetBoolField(TEXT("nativeGravityEnabled"), FPhysInterface_Chaos::IsGravityEnabled_AssumesLocked(Actor));
            const FVector V = FPhysInterface_Chaos::GetLinearVelocity_AssumesLocked(Actor), W = FPhysInterface_Chaos::GetAngularVelocity_AssumesLocked(Actor);
            if (!V.ContainsNaN()) O->SetArrayField(TEXT("nativeLinearVelocityCmps"), MayaV01CargoVector(V));
            if (!W.ContainsNaN()) O->SetArrayField(TEXT("nativeAngularVelocityRadps"), MayaV01CargoVector(W));
        }
    });
    bNativeKnown = ReadSucceeded && Entered && Observed;
    O->SetBoolField(TEXT("nativeActorReadKnown"), bNativeKnown);
    O->SetNumberField(TEXT("sampleRealSeconds"), FPlatformTime::Seconds());
    return O;
}
}

FString MAYAV01CaptureCargoPhysicsJson(AMAYADeliveryParcel* Parcel, UPrimitiveComponent* Carrier, const TCHAR* SampleLabel)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("schema"), TEXT("MayaRobotics.V01.CargoNativeReadback.v1"));
    O->SetStringField(TEXT("sampleLabel"), SampleLabel ? SampleLabel : TEXT("unspecified"));
    O->SetStringField(TEXT("scope"), TEXT("Read-only synchronized GAME-thread external actor/body/constraint observations; solver step and joint-impulse dt unidentified; no acceptance or force conversion"));
    O->SetBoolField(TEXT("solverStepIdentityKnown"), false);
    O->SetBoolField(TEXT("parcelKnown"), IsValid(Parcel));
    if (IsValid(Parcel))
    {
        O->SetStringField(TEXT("parcelId"), Parcel->GetParcelId());
        if (const UWorld* World = Parcel->GetWorld())
        {
            O->SetNumberField(TEXT("worldSeconds"), World->GetTimeSeconds());
            O->SetNumberField(TEXT("audioSeconds"), World->GetAudioTimeSeconds());
            O->SetNumberField(TEXT("worldDeltaSecondsNotSolverDt"), World->GetDeltaSeconds());
        }
        UPrimitiveComponent* ParcelBody = Parcel->GetParcelBody();
        FTransform ParcelNative = FTransform::Identity, CarrierNative = FTransform::Identity;
        bool ParcelKnown = false, CarrierKnown = false;
        O->SetObjectField(TEXT("parcelBody"), MayaV01CargoBody(ParcelBody, ParcelNative, ParcelKnown));
        O->SetObjectField(TEXT("carrierBody"), MayaV01CargoBody(Carrier, CarrierNative, CarrierKnown));
        TArray<UPhysicsConstraintComponent*> Constraints;
        Parcel->GetComponents<UPhysicsConstraintComponent>(Constraints);
        O->SetNumberField(TEXT("parcelConstraintCount"), Constraints.Num());
        const bool OwnedConstraint = Constraints.Num() == 1 && IsValid(Constraints[0])
            && Constraints[0]->GetFName() == FName(TEXT("DeliveryCargoTieDown"));
        O->SetBoolField(TEXT("ownedTieDownComponentKnown"), OwnedConstraint);
        if (OwnedConstraint)
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("component"), Constraints[0]->GetPathName());
            J->SetObjectField(TEXT("componentPose"), MayaV01CargoPose(Constraints[0]->GetComponentTransform()));
            bool Entered = false, Observed = false;
            const auto Ref = Constraints[0]->ConstraintInstance.GetPhysicsConstraintRef();
            const bool ReadSucceeded = FPhysInterface_Chaos::ExecuteRead(Ref, [&](const FPhysicsConstraintHandle& Locked)
            {
                Entered = true;
                const bool Valid = Locked.IsValid() && Locked.Constraint && Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType);
                J->SetBoolField(TEXT("nativeValid"), Valid);
                if (!Valid) return;
                Observed = true;
                J->SetBoolField(TEXT("nativeBroken"), FPhysInterface_Chaos::IsBroken(Locked));
                const auto* Actual = static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);
                const auto& S = Actual->GetJointSettings();
                const FTransform FrameA(FQuat(S.ConnectorTransforms[0].GetRotation()), FVector(S.ConnectorTransforms[0].GetTranslation()));
                const FTransform FrameB(FQuat(S.ConnectorTransforms[1].GetRotation()), FVector(S.ConnectorTransforms[1].GetTranslation()));
                J->SetObjectField(TEXT("nativeConnector0"), MayaV01CargoPose(FrameA));
                J->SetObjectField(TEXT("nativeConnector1"), MayaV01CargoPose(FrameB));
                const auto Endpoints = Actual->GetPhysicsBodies();
                const auto* ParcelBI = IsValid(ParcelBody) ? ParcelBody->GetBodyInstance() : nullptr;
                const auto* CarrierBI = IsValid(Carrier) ? Carrier->GetBodyInstance() : nullptr;
                const auto* ParcelActor = ParcelBI ? ParcelBI->GetPhysicsActorHandle() : nullptr;
                const auto* CarrierActor = CarrierBI ? CarrierBI->GetPhysicsActorHandle() : nullptr;
                const bool EndpointsMatch = ParcelActor && CarrierActor
                    && Endpoints[0] == ParcelActor->GetPhysicsObject() && Endpoints[1] == CarrierActor->GetPhysicsObject();
                J->SetBoolField(TEXT("nativeEndpointsMatchParcelCarrier"), EndpointsMatch);
                if (EndpointsMatch && ParcelKnown && CarrierKnown && !FrameA.ContainsNaN() && !FrameB.ContainsNaN())
                {
                    const FTransform WorldA = FrameA * ParcelNative, WorldB = FrameB * CarrierNative;
                    J->SetObjectField(TEXT("connector0UsingNativeActorWorldPose"), MayaV01CargoPose(WorldA));
                    J->SetObjectField(TEXT("connector1UsingNativeActorWorldPose"), MayaV01CargoPose(WorldB));
                    J->SetArrayField(TEXT("connectorWorldPositionResidualCm"), MayaV01CargoVector(WorldA.GetLocation() - WorldB.GetLocation()));
                    J->SetNumberField(TEXT("connectorWorldAngularResidualDegrees"), FMath::RadiansToDegrees(WorldA.GetRotation().AngularDistance(WorldB.GetRotation())));
                }
                FVector LinearImpulse = FVector::ZeroVector, AngularImpulse = FVector::ZeroVector;
                FPhysInterface_Chaos::GetForce(Locked, LinearImpulse, AngularImpulse);
                if (!LinearImpulse.ContainsNaN()) J->SetArrayField(TEXT("nativeLinearImpulseKgCmps"), MayaV01CargoVector(LinearImpulse));
                if (!AngularImpulse.ContainsNaN()) J->SetArrayField(TEXT("nativeAngularImpulseKgCm2ps"), MayaV01CargoVector(AngularImpulse));
                J->SetStringField(TEXT("impulseScope"), TEXT("UE5.3 JointConstraintProxy stores raw linear/angular impulses in fields called Force/Torque; no identified solver dt or force conversion"));
            });
            J->SetBoolField(TEXT("nativeReadKnown"), ReadSucceeded && Entered && Observed);
            O->SetObjectField(TEXT("tieDown"), J);
        }
    }
    O->SetNumberField(TEXT("capturedRealSeconds"), FPlatformTime::Seconds());
    FString Json;
    FJsonSerializer::Serialize(O, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}
