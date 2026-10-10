#include "DDSRobotPhysicsRigComponent.h"
#include "DDSRobotSimCallback.h"
#include "DDSRobotLifecycleOwner.h"
#include "DDSRobotJointAdmission.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Chaos/PBDJointConstraintData.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PBDRigidsSolver.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "Templates/UnrealTemplate.h"
#include "Misc/ScopeExit.h"

namespace
{
FTransform ToCm(const FTransform& M)
{
    return DDSPhysicalCm(M);
}
void SetPhysicalJointDefaults(UPhysicsConstraintComponent* Joint)
{
    auto& C = Joint->ConstraintInstance;
    C.ProfileInstance.bEnableProjection = false;
    C.ProfileInstance.bEnableShockPropagation = false;
    C.ProfileInstance.bParentDominates = false;
    C.ProfileInstance.bEnableMassConditioning = false;
}
}

UDDSRobotPhysicsRigComponent::UDDSRobotPhysicsRigComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bTickEvenWhenPaused = true; // Poll retirement while paused; no physics integration on GT.
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

bool UDDSRobotPhysicsRigComponent::CreateMeasuredRig(const FDDSRobotMeasuredBinding& BindingIn,
    const FDDSRobotPhysicsProfile& ProfileIn, FString& Error)
{
    check(IsInGameThread());
    // No recipe copy, new lease, generation or physical mutation on nested entry.
    // RetireWithLiveLease must not release this call's serial ownership.
    if (bConstructionInProgress)
    { Error = TEXT("CONSTRUCTION_BUSY: previous construction call has not returned"); return false; }
    if (!IsValid(this)) { Error = TEXT("CONSTRUCTION_OWNER_INVALID"); return false; }
    // Owner outlives the member-reference guard even if cancellation drops the
    // active lease's GC pins. Neither guard rescues explicit garbage flags.
    TStrongObjectPtr<UDDSRobotPhysicsRigComponent> ConstructionRigPin(this);
    TGuardValue<bool> ConstructionGuard(bConstructionInProgress, true);
    // Immutable transaction recipe; delegate reentry cannot edit caller-owned
    // measurement/profile storage underneath construction.
    const FDDSRobotMeasuredBinding B=BindingIn;
    const FDDSRobotPhysicsProfile P=ProfileIn;
    if (State != EDDSRobotRigState::Unbound && State != EDDSRobotRigState::Retired)
    { Error = TEXT("Existing rig must retire and pass fence before rebinding"); return false; }
    if (!GetWorld() || !GetWorld()->GetPhysicsScene() || !DDSValidateMeasuredBinding(B, P, Error)) return false;
    TArray<TStrongObjectPtr<UObject>> ConstructionPins;
    ConstructionPins.Emplace(GetWorld()); ConstructionPins.Emplace(GetOwner());
    for (const auto& Body : B.Bodies) ConstructionPins.Emplace(Body.ActualPart);
    for (const auto& Part : B.SuspensionParts) ConstructionPins.Emplace(Part.ActualPart);
    if (B.Parcel.ActualPart) ConstructionPins.Emplace(B.Parcel.ActualPart);
    auto* NewSolver = GetWorld()->GetPhysicsScene()->GetSolver();
    if (!NewSolver || !FMath::IsNearlyEqual(NewSolver->GetAsyncDeltaTime(), P.TargetStepS, 1.e-7)
        || !FMath::IsNearlyEqual(GetWorld()->GetGravityZ(), -P.GravityMps2 * 100.0, 1.e-4))
    { Error = TEXT("Admitted world must actually supply 120 Hz async and -980.665 cm/s2; no config changed"); return false; }
    const FTransform ChassisWorld = DDSRigidComponentPose(B.Bodies[0].ActualPart->GetComponentTransform());
    for (int32 I = 0; I < 9; ++I)
    {
        auto* Part = B.Bodies[I].ActualPart;
        const FTransform Expected = ToCm(B.Bodies[I].RestInChassisMeters) * ChassisWorld;
        if (Part->GetOwner() != GetOwner() || !Part->IsRegistered() || Part->IsSimulatingPhysics()
            || !Part->GetComponentLocation().Equals(Expected.GetLocation(), B.MaxContactSeparationM * 100.0)
            || !Part->GetComponentQuat().Equals(Expected.GetRotation(), 1.e-6))
        { Error = FString::Printf(TEXT("A02 actual body %d owner/rest/unsimulated precondition failed"), I); return false; }
    }
    for (const auto& Map : B.SuspensionParts)
    {
        auto* OwnerPart = B.Bodies[static_cast<int32>(Map.Owner)].ActualPart;
        const FTransform Expected = ToCm(Map.RestInOwnerMeters) * DDSRigidComponentPose(OwnerPart->GetComponentTransform());
        if (Map.ActualPart->GetOwner() != GetOwner() || Map.ActualPart->IsSimulatingPhysics()
            || !Map.ActualPart->GetComponentLocation().Equals(Expected.GetLocation(), B.MaxContactSeparationM * 100)
            || !Map.ActualPart->GetComponentQuat().Equals(Expected.GetRotation(), 1.e-6))
        { Error = TEXT("A02 suspension piece rest/owner mismatch; topology amendment required"); return false; }
    }
    const auto ConstructionLifecycle = FDDSRobotLifecycleLease::Acquire(this, B, Error);
    if (!ConstructionLifecycle) return false;
    Lifecycle = ConstructionLifecycle;
    Measured = B; Profile = P; Solver = NewSolver; ++Generation;
    const uint64 ConstructionGeneration = Generation;
    Control = MakeShared<FDDSRobotControlBarrier, ESPMode::ThreadSafe>();
    Control->Current.Generation = Generation; Control->Current.WorldEpoch = WorldEpoch;
    Control->Current.Revision = ++ReadinessRevision;
    ConstructionLifecycle->SetAdmissionBarrier(Generation,WorldEpoch,ReadinessRevision);
    State = EDDSRobotRigState::Binding; BindingStartedRealS = FPlatformTime::Seconds();
    bool bConstructionSucceeded = false;
    // Installed scope-exit captures the immutable local owner, whose lifetime
    // encloses this guard; exception/early-return cleanup never follows a new
    // member owner. The normal failure path is idempotent with this fallback.
    ON_SCOPE_EXIT
    {
        if (!bConstructionSucceeded && Lifecycle.Get() == ConstructionLifecycle.Get())
            ConstructionLifecycle->RetireSynchronously();
    };
    const auto OwnsConstruction = [this, ConstructionLifecycle, ConstructionGeneration]()
    {
        return bConstructionInProgress && IsValid(this) && IsValid(GetWorld()) && IsValid(GetOwner())
            && Lifecycle.Get() == ConstructionLifecycle.Get() && Generation == ConstructionGeneration
            && State == EDDSRobotRigState::Binding && ConstructionLifecycle->GetLiveScene();
    };
    const auto FailBinding = [this, &Error, ConstructionLifecycle, ConstructionGeneration](const TCHAR* Cause)
    {
        Error = Cause;
        // An old continuation never follows a substituted current lease. The
        // normal serialized path retains this exact owner even after retirement.
        if (Lifecycle.Get() == ConstructionLifecycle.Get())
        {
            if (Generation == ConstructionGeneration) Fault = Cause;
            ConstructionLifecycle->RetireSynchronously(); // idempotent if already cancelled.
        }
        return false;
    };
    BoundBodies.Reset(); BoundSuspensionParts.Reset(); Joints.Reset();
    if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_OWNER_REVOKED"));
    RobotContactMaterial = NewObject<UPhysicalMaterial>(this);
    if (!OwnsConstruction() || !RobotContactMaterial)
        return FailBinding(TEXT("CONSTRUCTION_MATERIAL_OWNER_REVOKED"));
    ConstructionPins.Emplace(RobotContactMaterial.Get());
    RobotContactMaterial->StaticFriction = P.StaticFriction;
    RobotContactMaterial->Friction = P.DynamicFriction;
    RobotContactMaterial->Restitution = P.Restitution;
    RobotContactMaterial->bOverrideFrictionCombineMode = true;
    RobotContactMaterial->FrictionCombineMode = EFrictionCombineMode::Average;
    RobotContactMaterial->bOverrideRestitutionCombineMode = true;
    RobotContactMaterial->RestitutionCombineMode = EFrictionCombineMode::Average;
    // The original road/curb counterpart material stays original. Combined values
    // require native measurement; scalar Average is a declared game hypothesis.
    if (!ConstructionLifecycle->PrepareOwnedMobility()) return FailBinding(TEXT("OWNED_MOBILITY_PREPARATION_REVOKED"));
    GetOwner()->SetRootComponent(B.Bodies[0].ActualPart);
    if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_ROOT_OWNER_REVOKED"));
    for (int32 I = 0; I < 9; ++I)
    {
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_BODY_OWNER_REVOKED"));
        auto* Part = Cast<UDDSRobotLeasedMeshComponent>(B.Bodies[I].ActualPart);
        BoundBodies.Add(Part);
        Part->BodyInstance.bAutoWeld = false;
        Part->BodyInstance.InertiaTensorScale = FVector::OneVector;
        Part->BodyInstance.SetInertiaConditioningEnabled(false);
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetLinearDamping(0.0f); Part->SetAngularDamping(0.0f); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetCollisionProfileName(B.Bodies[I].MeasuredCollisionProfile); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetEnableGravity(true); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetPhysMaterialOverride(RobotContactMaterial); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetUseCCD(B.Bodies[I].bCCDMeasuredRequired); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetMassOverrideInKg(NAME_None, I == 0 ? P.BareChassisKg : (I < 7 ? P.WheelKg : (I == 7 ? P.Lid0Kg : P.Lid1Kg)), true); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetSimulatePhysics(true); }))
            return FailBinding(TEXT("OWNED_BODY_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->IsBuilding() || !ConstructionLifecycle->GetLiveScene() || !Part->IsRegistered()
            || !Part->IsSimulatingPhysics() || !Part->GetComponentScale().Equals(B.Bodies[I].SupplierScale,1.e-6))
            return FailBinding(TEXT("PARTIAL_BODY_SETUP_REVOKED: owner/registration/scale changed during setup"));
    }
    if (B.Parcel.ActualPart)
    {
        auto* Part=Cast<UDDSRobotLeasedMeshComponent>(B.Parcel.ActualPart);
        BoundParcel=Part;
        Part->BodyInstance.bAutoWeld=false; Part->BodyInstance.InertiaTensorScale=FVector::OneVector;
        Part->BodyInstance.SetInertiaConditioningEnabled(false);
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetCollisionProfileName(B.Parcel.MeasuredCollisionProfile); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetEnableGravity(true); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetLinearDamping(0); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetAngularDamping(0); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetPhysMaterialOverride(RobotContactMaterial); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetUseCCD(B.Parcel.bCCDMeasuredRequired); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetMassOverrideInKg(NAME_None,P.ParcelKg,true); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetSimulatePhysics(true); })) return FailBinding(TEXT("PARCEL_CONFIGURATION_REVOKED"));
    }
    for (const auto& Map : B.SuspensionParts)
    {
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_SUSPENSION_OWNER_REVOKED"));
        BoundSuspensionParts.Add(Map.ActualPart);
        auto* Part=Cast<UDDSRobotLeasedMeshComponent>(Map.ActualPart);
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]()
            { Part->AttachToComponent(B.Bodies[static_cast<int32>(Map.Owner)].ActualPart,
                FAttachmentTransformRules::KeepWorldTransform); }))
            return FailBinding(TEXT("OWNED_SUSPENSION_ATTACHMENT_REVOKED"));
        // Already measured in owner collision: second collider would double-count.
        if (!ConstructionLifecycle->ExecuteOwnedOperation(Part,[&]() { Part->SetCollisionEnabled(ECollisionEnabled::NoCollision); }))
            return FailBinding(TEXT("OWNED_SUSPENSION_COLLISION_REVOKED"));
        if (!ConstructionLifecycle->IsBuilding() || !ConstructionLifecycle->GetLiveScene() || !Map.ActualPart->IsRegistered())
            return FailBinding(TEXT("PARTIAL_SUSPENSION_SETUP_REVOKED"));
    }
    for (int32 I = 0; I < 8; ++I)
    {
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_JOINT_OWNER_REVOKED"));
        auto* Joint = NewObject<UPhysicsConstraintComponent>(GetOwner(),
            *FString::Printf(TEXT("DDSMeasuredJoint%d"), I));
        if (!OwnsConstruction() || !Joint)
            return FailBinding(TEXT("CONSTRUCTION_JOINT_CREATION_REVOKED"));
        ConstructionPins.Emplace(Joint);
        Joints.Add(Joint); // Partial setup cleanup includes this joint before registration.
        GetOwner()->AddInstanceComponent(Joint);
        Joint->AttachToComponent(B.Bodies[0].ActualPart, FAttachmentTransformRules::KeepWorldTransform);
        Joint->SetWorldScale3D(FVector::OneVector);
        Joint->ConstraintInstance.bScaleLinearLimits = false;
        SetPhysicalJointDefaults(Joint);
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_JOINT_ATTACHMENT_REVOKED"));
        Joint->RegisterComponent();
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_JOINT_REGISTRATION_REVOKED"));
        Joint->SetConstrainedComponents(B.Bodies[0].ActualPart, NAME_None, B.Bodies[I + 1].ActualPart, NAME_None);
        if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_JOINT_ENDPOINT_OWNER_REVOKED"));
        Joint->SetConstraintReferenceFrame(EConstraintFrame::Frame1, ToCm(B.Joints[I].InChassisMeters));
        Joint->SetConstraintReferenceFrame(EConstraintFrame::Frame2, ToCm(B.Joints[I].InPartMeters));
        Joint->SetDisableCollision(B.Joints[I].bAdjacentCollisionExclusionJustified);
        Joint->SetLinearXLimit(LCM_Locked, 0); Joint->SetLinearYLimit(LCM_Locked, 0);
        Joint->SetLinearZLimit(I < 6 && B.Joints[I].SuspensionHalfTravelM > 0 ? LCM_Limited : LCM_Locked,
            B.Joints[I].SuspensionHalfTravelM * 100.0);
        Joint->SetAngularSwing1Limit(ACM_Locked, 0); Joint->SetAngularSwing2Limit(ACM_Locked, 0);
        Joint->SetAngularTwistLimit(I < 6 ? ACM_Free : ACM_Locked, 0);
        if (I < 6 && B.Joints[I].SuspensionHalfTravelM > 0)
        {
            Joint->SetLinearPositionDrive(false, false, true);
            Joint->SetLinearVelocityDrive(false, false, true);
            Joint->SetLinearPositionTarget(FVector(0, 0, B.Joints[I].SuspensionRestTargetM * 100));
            // F/cm conversion: (100 N)/(100 cm) leaves K and C numerically unchanged.
            Joint->SetLinearDriveParams(P.SuspensionK_Npm, P.SuspensionC_Nspm, P.SuspensionForceLimitN * 100);
        }
        // IsBroken alone accepts an INVALID handle in installed 5.3. Check
        // validity before wrapper/lambda, then again under its scoped lock.
        if (!ConstructionLifecycle->IsBuilding() || !ConstructionLifecycle->GetLiveScene())
            return FailBinding(TEXT("PARTIAL_JOINT_SETUP_REVOKED"));
        const auto Ref = Joint->ConstraintInstance.GetPhysicsConstraintRef();
        if (!Ref.IsValid() || FPhysInterface_Chaos::IsBroken(Ref))
            return FailBinding(TEXT("JOINT_ADMISSION_FAILED: invalid/broken external joint"));
        bool Verified = false;
        FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref, [&](const FPhysicsConstraintHandle& Locked)
        {
            if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
            auto* Actual = static_cast<Chaos::FJointConstraint*>(Locked.Constraint);
            const auto Endpoints = Actual->GetPhysicsBodies();
            auto* BI0 = B.Bodies[0].ActualPart->GetBodyInstance();
            auto* BI1 = B.Bodies[I+1].ActualPart->GetBodyInstance();
            auto* Proxy0 = BI0 && BI0->IsValidBodyInstance() ? BI0->GetPhysicsActorHandle() : nullptr;
            auto* Proxy1 = BI1 && BI1->IsValidBodyInstance() ? BI1->GetPhysicsActorHandle() : nullptr;
            if (!Proxy0 || !Proxy1 || !Endpoints[0] || !Endpoints[1]
                || Endpoints[0] != Proxy0->GetPhysicsObject() || Endpoints[1] != Proxy1->GetPhysicsObject()) return;
            // Physical SI frames were measured AFTER preserved supplier scaling.
            Actual->SetJointTransforms({ToCm(B.Joints[I].InChassisMeters), ToCm(B.Joints[I].InPartMeters)});
            if (I < 6 && B.Joints[I].SuspensionHalfTravelM > 0)
            {
                Actual->SetLinearDriveForceMode(Chaos::EJointForceMode::Force);
                Actual->SetLinearDriveStiffness(Chaos::FVec3(0,0,P.SuspensionK_Npm));
                Actual->SetLinearDriveDamping(Chaos::FVec3(0,0,P.SuspensionC_Nspm));
                Actual->SetLinearDriveMaxForce(Chaos::FVec3(0,0,P.SuspensionForceLimitN * 100));
            }
            Verified = DDSJointSettingsMatch(Actual->GetJointSettings(), I,
                ToCm(B.Joints[I].InChassisMeters), ToCm(B.Joints[I].InPartMeters),
                B.Joints[I].SuspensionHalfTravelM * 100, B.Joints[I].SuspensionRestTargetM * 100,
                B.Joints[I].bAdjacentCollisionExclusionJustified, P);
        });
        if (!Verified) return FailBinding(TEXT("JOINT_ADMISSION_FAILED: endpoints/frames/limits/physical drive readback"));
    }
    // Earlier endpoints/settings are rechecked after EVERY part has finished.
    // This final pass only reads; it cannot repair/hide an independent mutation.
    if (!OwnsConstruction() || Joints.Num()!=8 || !ConstructionLifecycle->IsBuilding())
        return FailBinding(TEXT("FINAL_TRANSACTION_REVOKED"));
    for (int32 I=0; I<8; ++I)
    {
        const auto Ref=Joints[I]->ConstraintInstance.GetPhysicsConstraintRef();
        if (!Ref.IsValid() || FPhysInterface_Chaos::IsBroken(Ref))
            return FailBinding(TEXT("FINAL_JOINT_HANDLE_REVOKED"));
        bool Verified=false;
        FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref,[&](const FPhysicsConstraintHandle& Locked)
        {
            if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
            auto* Actual=static_cast<Chaos::FJointConstraint*>(Locked.Constraint);
            const auto Endpoints=Actual->GetPhysicsBodies();
            auto* BI0=B.Bodies[0].ActualPart->GetBodyInstance();
            auto* BI1=B.Bodies[I+1].ActualPart->GetBodyInstance();
            auto* A=BI0 && BI0->IsValidBodyInstance() ? BI0->GetPhysicsActorHandle() : nullptr;
            auto* B1=BI1 && BI1->IsValidBodyInstance() ? BI1->GetPhysicsActorHandle() : nullptr;
            Verified=A && B1 && Endpoints[0]==A->GetPhysicsObject() && Endpoints[1]==B1->GetPhysicsObject()
                && DDSJointSettingsMatch(Actual->GetJointSettings(),I,
                    ToCm(B.Joints[I].InChassisMeters),ToCm(B.Joints[I].InPartMeters),
                    B.Joints[I].SuspensionHalfTravelM*100,B.Joints[I].SuspensionRestTargetM*100,
                    B.Joints[I].bAdjacentCollisionExclusionJustified,P);
        });
        if (!Verified) return FailBinding(TEXT("FINAL_JOINT_READBACK_REVOKED"));
    }
    if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_FINAL_OWNER_REVOKED"));
    if (!ConstructionLifecycle->CommitAdmission(B,P,RobotContactMaterial,Error))
        return FailBinding(TEXT("FINAL_WHOLE_BINDING_TRANSACTION_REVOKED"));
    // Eight physical joints; lids have actual locked closed stops for this baseline.
    if (!OwnsConstruction()) return FailBinding(TEXT("CONSTRUCTION_COMMIT_OWNER_REVOKED"));
    Callback = Solver->CreateAndRegisterSimCallbackObject_External<FDDSRobotSimCallback>();
    // Installed registration only allocates/marshals this non-UObject callback;
    // it has no standard component creation delegate. Guard final publication.
    if (!OwnsConstruction() || !Callback) return FailBinding(TEXT("CALLBACK_REGISTRATION_FAILED"));
    State = EDDSRobotRigState::Binding; bSetupSubmitted = false;
    bWorldReady = false; Latest = FDDSRobotSnapshot(); LastSequence = 0;
    bConstructionSucceeded = true;
    return true;
}

bool UDDSRobotPhysicsRigComponent::WriteSetupInput()
{
    if (!Callback || !Solver || BoundBodies.Num() != 9) return false;
    auto* Input = Callback->GetProducerInputData_External();
    auto& S = Input->Setup;
    S = FDDSRobotSimSetup(); S.Generation = Generation; S.Profile = Profile; S.Control = Control;
    for (int32 I = 0; I < 9; ++I)
    {
        auto* BI = BoundBodies[I] ? BoundBodies[I]->GetBodyInstance() : nullptr;
        if (!BI || !BI->IsValidBodyInstance()) return false;
        auto* Proxy = BI->GetPhysicsActorHandle();
        if (!Proxy || Proxy->GetSolverBase() != Solver) return false;
        S.ParticleIds[I] = Proxy->GetGameThreadAPI().UniqueIdx().Idx;
        if (S.ParticleIds[I] == INDEX_NONE) return false;
        S.COMCm[I] = Measured.Bodies[I].COMLocalMeters * 100;
        S.InertiaKgCm2[I] = Measured.Bodies[I].PrincipalInertiaKgM2 * 10000;
        S.PrincipalAxes[I] = Measured.Bodies[I].PrincipalAxesInBody;
        S.RestInChassisCm[I] = ToCm(Measured.Bodies[I].RestInChassisMeters);
    }
    for (int32 I = 0; I < 8; ++I)
    {
        S.JointInChassisCm[I] = ToCm(Measured.Joints[I].InChassisMeters);
        S.JointInPartCm[I] = ToCm(Measured.Joints[I].InPartMeters);
        S.JointTravelCm[I] = Measured.Joints[I].SuspensionHalfTravelM * 100;
        S.JointTargetCm[I] = Measured.Joints[I].SuspensionRestTargetM * 100;
        S.JointExcludeCollision[I] = Measured.Joints[I].bAdjacentCollisionExclusionJustified;
    }
    for (int32 I = 0; I < 6; ++I) S.WheelForwardSign[I] = Measured.Bodies[I + 1].MeasuredMotorForwardSign;
    S.Contact.SeparationCm = Measured.MaxContactSeparationM * 100;
    S.Contact.UpDot = Measured.MinSupportUpDot;
    S.Contact.MinLoadedImpulseKgCmps = Measured.MinLoadedImpulseNs * 100;
    S.Contact.SleepingAnchorCm = Measured.SleepingAnchorToleranceM * 100;
    S.Contact.SleepingSpeedCmps = Measured.SleepingSpeedToleranceMps * 100;
    S.Contact.SleepingOmegaRadps = Measured.SleepingOmegaToleranceRadps;
    S.SlipDomainCmps = Measured.SlipDomainSpeedMps * 100;
    S.ClosedLidAngleToleranceRad = Measured.ClosedLidAngleToleranceRad;
    S.ClosedLidOmegaToleranceRadps = Measured.ClosedLidOmegaToleranceRadps;
    S.Mechanics = Measured.Mechanics;
    if (BoundParcel)
    {
        auto* BI=BoundParcel->GetBodyInstance(); auto* Proxy=BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
        if (!Proxy || Proxy->GetSolverBase()!=Solver) return false;
        S.ParcelParticleId=Proxy->GetGameThreadAPI().UniqueIdx().Idx;
        S.ParcelCOMCm=Measured.Parcel.COMLocalMeters*100; S.ParcelInertiaKgCm2=Measured.Parcel.PrincipalInertiaKgM2*10000;
        S.ParcelPrincipalAxes=Measured.Parcel.PrincipalAxesInBody; S.ParcelMassKg=Profile.ParcelKg;
    }
    Input->bHasSetup = true;
    LastCommand.Generation = Generation; LastCommand.CollisionWorldGeneration = WorldEpoch;
    LastCommand.ReadinessRevision = ReadinessRevision;
    LastCommand.bCollisionWorldReady = bWorldReady; LastCommand.bDriveLocked = true;
    LastCommand.IssuedRealS = FPlatformTime::Seconds(); LastCommand.Sequence = ++LastSequence;
    Input->Command = LastCommand;
    bSetupSubmitted = true;
    return true;
}

bool UDDSRobotPhysicsRigComponent::SubmitCommand(const FDDSRobotCommand& Command)
{
    check(IsInGameThread());
    if (!Callback || State != EDDSRobotRigState::Active || bResetPending
        || Command.Generation != Generation || Command.CollisionWorldGeneration != WorldEpoch
        || Command.Sequence <= LastSequence || !FMath::IsFinite(Command.LeftRequest)
        || !FMath::IsFinite(Command.RightRequest) || !FMath::IsFinite(Command.IssuedRealS)
        || !FMath::IsFinite(Command.MaxAgeRealS) || Command.MaxAgeRealS <= 0
        || FMath::Abs(Command.LeftRequest) > 1 || FMath::Abs(Command.RightRequest) > 1) return false;
    LastCommand = Command; LastCommand.bCollisionWorldReady = bWorldReady;
    LastCommand.ReadinessRevision = ReadinessRevision;
    LastSequence = Command.Sequence;
    Callback->GetProducerInputData_External()->Command = LastCommand;
    return true;
}

void UDDSRobotPhysicsRigComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    RevokeExpiredCollisionReadinessOwner();
    if (State == EDDSRobotRigState::Binding && FPlatformTime::Seconds()-BindingStartedRealS > Profile.BindingTimeoutRealS)
    { Fault = TEXT("BINDING_TIMEOUT: actual nine bodies/eight physics joints not acknowledged"); BeginSafeUnregister(); return; }
    if (State == EDDSRobotRigState::Binding && !bSetupSubmitted) WriteSetupInput();
    if (Callback && (State == EDDSRobotRigState::Binding || State == EDDSRobotRigState::Active))
    {
        while (auto Handle = Callback->PopOutputData_External())
        {
            const auto& Output = *Handle;
            const auto Gate = Control->Read();
            if (Output.AppliedResetId == LastResetRequest && Output.ResetOldGeneration == PendingResetOldGeneration
                && Output.Snapshot.Generation == PendingResetNewGeneration && Gate.Generation == PendingResetNewGeneration)
            {
                Generation = PendingResetNewGeneration; bResetPending = false;
                // The actual reset acknowledgement ends the old provider's authority.
                // Keep the existing PT reset authorization protocol unchanged.
                ClearCollisionReadinessOwner(); bWorldReady = false; RevokeObservation();
            }
            if (bResetPending && Output.CancelledResetId == LastResetRequest
                && Output.ResetOldGeneration == PendingResetOldGeneration)
            {
                bResetPending = false;
                // A refused reset also ends the old provider's authority: it may
                // already have stopped while pending recovery refused withdrawal.
                ClearCollisionReadinessOwner(); bWorldReady = false; RevokeObservation();
            }
            if (Output.Snapshot.Generation != Generation || Output.Snapshot.CollisionWorldGeneration != WorldEpoch
                || Output.Snapshot.ReadinessRevision != ReadinessRevision || Gate.bRetiring) continue;
            if (Output.Snapshot.StepSequence <= Latest.StepSequence) continue;
            if (Latest.StepSequence && Output.Snapshot.StepSequence > Latest.StepSequence + 1)
                CoalescedOutputs += Output.Snapshot.StepSequence - Latest.StepSequence - 1;
            Latest = Output.Snapshot;
            Latest.ConsumedRealS = FPlatformTime::Seconds();
            Latest.ExternalRenderDeltaS = DeltaTime;
            Latest.SnapshotTransportAgeS = Latest.ConsumedRealS - Latest.CapturedRealS;
            Latest.CoalescedOutputCount = CoalescedOutputs;
            if (State == EDDSRobotRigState::Binding && Latest.bAllJointsAdmitted) State = EDDSRobotRigState::Active;
        } // Every handle is destroyed before unregister. No queue handle retained.
    }

}

bool UDDSRobotPhysicsRigComponent::ConsumeSnapshot(FDDSRobotSnapshot& Out) const
{
    const double Now = FPlatformTime::Seconds();
    if (State != EDDSRobotRigState::Active || !bWorldReady || Latest.Generation != Generation
        || Latest.CollisionWorldGeneration != WorldEpoch || Latest.ReadinessRevision != ReadinessRevision
        || !Latest.bAllJointsAdmitted || !Latest.bCompleteState || !Latest.bTimingEnvelopeValid || !Latest.bWorldReady
        || Now < Latest.CapturedRealS || Now-Latest.CapturedRealS > Profile.CurrentStateMaxAgeS) return false;
    Out = Latest; return true;
}

bool UDDSRobotPhysicsRigComponent::ReadPreReadinessCompletedState(FDDSRobotSnapshot& Out) const
{
    check(IsInGameThread());
    const double Now = FPlatformTime::Seconds();
    auto* Scene = Lifecycle ? Lifecycle->GetLiveScene() : nullptr;
    const auto* Chassis = GetActualChassis();
    if (!IsValid(this) || !IsRegistered() || !IsValid(GetOwner()) || !IsValid(GetWorld())
        || GetOwner()->IsActorBeingDestroyed() || !Scene || GetWorld()->GetPhysicsScene() != Scene
        || !Solver || Scene->GetSolver() != Solver || !Callback || !Control || bResetPending
        || !IsValid(Chassis) || !Chassis->IsRegistered() || Chassis->GetOwner() != GetOwner()
        || Chassis->GetWorld() != GetWorld() || State != EDDSRobotRigState::Active
        || Latest.Generation != Generation || Latest.CollisionWorldGeneration != WorldEpoch
        || Latest.ReadinessRevision != ReadinessRevision || !Latest.StepSequence
        || !Latest.bAllJointsAdmitted || !Latest.bCompleteState || !Latest.bTimingEnvelopeValid
        || !FMath::IsFinite(Now) || !FMath::IsFinite(Latest.CapturedRealS)
        || !FMath::IsFinite(Latest.ConsumedRealS) || Latest.ConsumedRealS < Latest.CapturedRealS
        || Now < Latest.CapturedRealS || Now-Latest.CapturedRealS > Profile.CurrentStateMaxAgeS)
        return false;
    const auto Gate = Control->Read();
    if (Gate.bRetiring || Gate.Generation != Generation || Gate.WorldEpoch != WorldEpoch
        || Gate.Revision != ReadinessRevision || Gate.bReady != bWorldReady) return false;
    Out = Latest; return true; // No world-ready or service evidence is synthesized.
}

void UDDSRobotPhysicsRigComponent::ClearCollisionReadinessOwner()
{
    CollisionReadinessOwner.Reset(); CollisionReadinessSerial = 0;
}

void UDDSRobotPhysicsRigComponent::RefreshCollisionReadinessReceipt(FDDSCollisionReadinessCapability& R) const
{
    R.Owner = CollisionReadinessOwner; R.Rig = const_cast<UDDSRobotPhysicsRigComponent*>(this);
    R.Generation = Generation; R.WorldEpoch = WorldEpoch; R.Revision = ReadinessRevision;
    R.Serial = CollisionReadinessSerial; R.bReady = bWorldReady;
}

bool UDDSRobotPhysicsRigComponent::CollisionReadinessMatches(UActorComponent* Owner,
    const FDDSCollisionReadinessCapability& R) const
{
    check(IsInGameThread());
    if (!IsValid(Owner) || !Owner->IsRegistered() || Owner->GetOwner() != GetOwner()
        || Owner->GetWorld() != GetWorld() || !CollisionReadinessSerial
        || CollisionReadinessOwner.Get() != Owner || R.Owner.Get() != Owner || R.Rig.Get() != this
        || R.Serial != CollisionReadinessSerial || R.Generation != Generation
        || R.WorldEpoch != WorldEpoch || R.Revision != ReadinessRevision || R.bReady != bWorldReady
        || !Lifecycle || !Lifecycle->GetLiveScene() || !Control || bResetPending
        || State != EDDSRobotRigState::Active) return false;
    const auto Gate = Control->Read();
    return !Gate.bRetiring && Gate.Generation == Generation && Gate.WorldEpoch == WorldEpoch
        && Gate.Revision == ReadinessRevision && Gate.bReady == bWorldReady;
}

void UDDSRobotPhysicsRigComponent::RevokeExpiredCollisionReadinessOwner()
{
    if (!CollisionReadinessSerial) return;
    auto* Owner = CollisionReadinessOwner.Get();
    if (IsValid(Owner) && Owner->IsRegistered() && IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed()
        && IsValid(GetWorld()) && !GetWorld()->bIsTearingDown && Owner->GetOwner() == GetOwner()
        && Owner->GetWorld() == GetWorld()) return;
    ClearCollisionReadinessOwner(); bWorldReady = false; RevokeObservation();
}

bool UDDSRobotPhysicsRigComponent::AcquireCollisionReadiness(UActorComponent* Owner,
    FDDSCollisionReadinessCapability& Out)
{
    check(IsInGameThread());
    RevokeExpiredCollisionReadinessOwner();
    FDDSRobotSnapshot Current;
    if (CollisionReadinessSerial || !IsValid(Owner) || !Owner->IsRegistered()
        || Owner->GetOwner() != GetOwner() || Owner->GetWorld() != GetWorld()
        || WorldEpoch == MAX_uint64 || ReadinessRevision == MAX_uint64
        || NextCollisionReadinessSerial == MAX_uint64 || !ReadPreReadinessCompletedState(Current)) return false;
    CollisionReadinessOwner = Owner; CollisionReadinessSerial = NextCollisionReadinessSerial++;
    ++WorldEpoch; bWorldReady = false; RevokeObservation(true);
    RefreshCollisionReadinessReceipt(Out); return true;
}

bool UDDSRobotPhysicsRigComponent::CompareAndSetCollisionReadiness(UActorComponent* Owner,
    FDDSCollisionReadinessCapability& R, bool bReady)
{
    check(IsInGameThread());
    if (!CollisionReadinessMatches(Owner, R) || ReadinessRevision == MAX_uint64) return false;
    if (bWorldReady == bReady) return true;
    FDDSRobotSnapshot Current;
    if (bReady && !ReadPreReadinessCompletedState(Current)) return false;
    bWorldReady = bReady; RevokeObservation(true);
    RefreshCollisionReadinessReceipt(R); return true;
}

bool UDDSRobotPhysicsRigComponent::WithdrawCollisionReadiness(UActorComponent* Owner,
    FDDSCollisionReadinessCapability& R, bool bRelease)
{
    check(IsInGameThread());
    if (!CollisionReadinessMatches(Owner, R) || WorldEpoch == MAX_uint64
        || ReadinessRevision == MAX_uint64) return false;
    ++WorldEpoch; bWorldReady = false; RevokeObservation(true);
    if (bRelease) { ClearCollisionReadinessOwner(); R = FDDSCollisionReadinessCapability(); }
    else RefreshCollisionReadinessReceipt(R);
    return true;
}
UStaticMeshComponent* UDDSRobotPhysicsRigComponent::GetActualChassis() const
{ return BoundBodies.Num() == 9 ? BoundBodies[0].Get() : nullptr; }
bool UDDSRobotPhysicsRigComponent::IsServiceEvidenceFresh(double Now, FDDSRobotSnapshot& Out) const
{
    return ConsumeSnapshot(Out) && Profile.Service.bFrozen && Out.bServiceStable && !Out.bRecoveryDriveInhibited
        && FMath::IsFinite(Now) && Now >= Out.CapturedRealS && Now-Out.CapturedRealS <= Profile.Service.MaxSampleAgeS;
}
void UDDSRobotPhysicsRigComponent::RevokeObservation(bool bPreserveReadinessOwner)
{
    // External recovery/lifecycle revocation cannot leave a stale provider owning true.
    if (CollisionReadinessSerial && !bPreserveReadinessOwner)
    { ClearCollisionReadinessOwner(); bWorldReady = false; }
    Latest = FDDSRobotSnapshot(); ++ReadinessRevision;
    if (Control)
    {
        FScopeLock Lock(&Control->Mutex);
        Generation = Control->Current.Generation; // reset may have linearized before this withdrawal.
        Control->Current.Revision = ReadinessRevision; Control->Current.WorldEpoch = WorldEpoch;
        Control->Current.bReady = bWorldReady;
        if (bResetPending) Control->Current.CancelledResetId = FMath::Max(Control->Current.CancelledResetId,LastResetRequest);
    }
    bResetPending = false;
    LastCommand.Generation = Generation; LastCommand.CollisionWorldGeneration = WorldEpoch;
    LastCommand.ReadinessRevision = ReadinessRevision; LastCommand.bCollisionWorldReady = bWorldReady;
    LastCommand.bDriveLocked = true; LastCommand.bBrake = true;
    LastCommand.IssuedRealS = FPlatformTime::Seconds(); LastCommand.Sequence = ++LastSequence;
    if (Callback) Callback->GetProducerInputData_External()->Command = LastCommand;
}
void UDDSRobotPhysicsRigComponent::InvalidateCollisionWorld(uint64 NewEpoch)
{
    check(IsInGameThread());
    RevokeExpiredCollisionReadinessOwner();
    if (CollisionReadinessSerial || NewEpoch <= WorldEpoch) return;
    WorldEpoch = NewEpoch; bWorldReady = false; RevokeObservation();
}
bool UDDSRobotPhysicsRigComponent::SetCollisionWorldReady(uint64 Epoch, bool bReady)
{
    check(IsInGameThread());
    RevokeExpiredCollisionReadinessOwner();
    if (CollisionReadinessSerial || Epoch != WorldEpoch || !Lifecycle || !Lifecycle->GetLiveScene()
        || State == EDDSRobotRigState::Retiring || State == EDDSRobotRigState::Retired) return false;
    if (bWorldReady == bReady) return true;
    bWorldReady = bReady; RevokeObservation(); // even false/true coalescence requires a new completed revision.
    return true;
}
bool UDDSRobotPhysicsRigComponent::CancelPendingRecovery(uint64 Id)
{
    if (!bResetPending || Id != LastResetRequest || !Control) return false;
    RevokeObservation(); return true;
}

bool UDDSRobotPhysicsRigComponent::ValidateFullAssemblyRecoveryPose(const FTransform& ChassisCm) const
{
    if (!GetWorld() || !bWorldReady || ChassisCm.ContainsNaN() || !ChassisCm.GetRotation().IsNormalized()
        || !ChassisCm.GetScale3D().Equals(FVector::OneVector,1.e-9) || BoundBodies.Num() != 9) return false;
    FComponentQueryParams Query(SCENE_QUERY_STAT(DDSFullAssemblyRecovery), GetOwner());
    for (int32 I = 0; I < 9; ++I)
    {
        const FTransform World = ToCm(Measured.Bodies[I].RestInChassisMeters) * ChassisCm;
        TArray<FOverlapResult> Overlaps;
        // Exact supplied component shape, orientation and scale. No proxy box.
        if (BoundBodies[I]->ComponentOverlapMulti(Overlaps, GetWorld(), World.GetLocation(),
            World.GetRotation(), BoundBodies[I]->GetCollisionObjectType(), Query))
            for (const auto& O : Overlaps) if (O.bBlockingHit) return false;
    }
    // This is overlap only. I01/A02 must separately validate actual collision/support
    // readiness for the selected recorded pose before minting a recovery permit.
    return true;
}

bool UDDSRobotPhysicsRigComponent::ResetAssemblyAtApprovedRecoveryPose(const FDDSRobotRecoveryPermit& Permit)
{
    if (!Callback || State != EDDSRobotRigState::Active || bResetPending
        || Measured.Mechanics.bFrozen || BoundParcel // No ten-body recovery adapter yet; fail closed.
        || !bWorldReady || Permit.ExpectedGeneration != Generation || Permit.CollisionWorldGeneration != WorldEpoch
        || Permit.ReadinessRevision != ReadinessRevision || !FMath::IsFinite(Permit.ValidatedRealS)
        || FPlatformTime::Seconds() < Permit.ValidatedRealS
        || FPlatformTime::Seconds()-Permit.ValidatedRealS > Profile.RecoveryPermitMaxAgeS
        || Permit.NewGeneration <= Generation || Permit.RequestId <= LastResetRequest
        || Permit.LedgerFingerprint.IsEmpty() || !Permit.bAuthorityCancelledOldOperation
        || !Permit.bBothActualLidsClosedLatched || !Permit.bN11PersistenceAllowsReset
        || !Permit.bFullAssemblyPoseAndSupportValidated || !ValidateFullAssemblyRecoveryPose(Permit.WorldChassisCm)) return false;
    auto* Input = Callback->GetProducerInputData_External();
    LastCommand.bDriveLocked = true; LastCommand.bBrake = true; LastCommand.Sequence = ++LastSequence;
    LastCommand.IssuedRealS = FPlatformTime::Seconds(); Input->Command = LastCommand;
    Input->bApprovedReset = true; Input->ResetRequestId = Permit.RequestId;
    Input->ResetNewGeneration = Permit.NewGeneration;
    Input->ResetOldGeneration = Permit.ExpectedGeneration;
    Input->ResetWorldEpoch = Permit.CollisionWorldGeneration;
    Input->ResetReadinessRevision = Permit.ReadinessRevision; Input->ResetValidatedRealS = Permit.ValidatedRealS;
    PendingResetOldGeneration = Permit.ExpectedGeneration; PendingResetNewGeneration = Permit.NewGeneration;
    for (int32 I = 0; I < 9; ++I)
        Input->ResetWorldCm[I] = ToCm(Measured.Bodies[I].RestInChassisMeters) * Permit.WorldChassisCm;
    LastResetRequest = Permit.RequestId; bResetPending = true;
    Latest.bCompleteState = false; Latest.bServiceStable = false;
    return true;
}

void UDDSRobotPhysicsRigComponent::BeginSafeUnregister()
{
    check(IsInGameThread());
    if (Lifecycle && Lifecycle->GetLiveScene()) Lifecycle->RetireSynchronously();
}
void UDDSRobotPhysicsRigComponent::RetireWithLiveLease(FDDSRobotLifecycleLease& L)
{
    check(IsInGameThread());
    check(Lifecycle.Get() == &L && L.GetLiveScene());
    auto* Scene = L.GetLiveScene(); // lifetime established BEFORE any solver call.
    check(Scene->GetSolver() == Solver);
    ClearCollisionReadinessOwner();
    bWorldReady = false; RevokeObservation(); State = EDDSRobotRigState::Retiring;
    { FScopeLock Lock(&Control->Mutex); Control->Current.bRetiring = true; ++Control->Current.Generation; }
    if (Callback)
    {
        while (auto Handle = Callback->PopFutureOutputData_External()) {}
        Retirement = MakeShared<FDDSRobotRetirementFence, ESPMode::ThreadSafe>();
        const auto* RetiringCallback = Callback;
        Solver->EnqueueCommandImmediate([RetiringCallback]() { const_cast<FDDSRobotSimCallback*>(RetiringCallback)->ReleaseN12_Internal(); });
        Scene->Flush(); // Remove the callback-owned parcel constraint before callback/bodies.
        Solver->UnregisterAndFreeSimCallbackObject_External(Callback); Callback = nullptr;
        const auto Fence = Retirement;
        Solver->EnqueueCommandImmediate([Fence]() { Fence->bRemovalCommandPassed.Store(true); });
        Scene->Flush(); // actual marshal+wait even while paused; no lock held here.
        check(Retirement->bRemovalCommandPassed.Load());
    }
    // No callback can dereference bodies now. Retire partial or full joint set,
    // then flush their unregister commands BEFORE releasing the enforced pins.
    for (const auto& Joint : Joints) if (Joint) Joint->DestroyComponent();
    Scene->Flush();
    Joints.Reset(); BoundBodies.Reset(); BoundSuspensionParts.Reset(); BoundParcel=nullptr;
    Measured = FDDSRobotMeasuredBinding(); RobotContactMaterial = nullptr;
    Solver = nullptr; Retirement.Reset(); Control.Reset(); Latest = FDDSRobotSnapshot();
    ++Generation; bResetPending = false; bSetupSubmitted = false; State = EDDSRobotRigState::Retired;
}
bool UDDSRobotPhysicsRigComponent::ReleaseMeasuredBindingsAfterFence()
{
    // Kept for existing provider compatibility: retirement is now synchronous
    // and owned, not a caller promise followed by calls through a raw solver.
    return !bConstructionInProgress && State == EDDSRobotRigState::Retired && !Solver && !Callback;
}
void UDDSRobotPhysicsRigComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    BeginSafeUnregister(); // enforced lease knows the still-live scene.
    Super::EndPlay(Reason);
}
