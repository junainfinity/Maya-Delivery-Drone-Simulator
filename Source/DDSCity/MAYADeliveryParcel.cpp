#include "MAYADeliveryParcel.h"
#include "MAYANativeDemoPawn.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/OverlapResult.h"
#include "CollisionQueryParams.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/ConstructorHelpers.h"
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#include "MAYAV01CargoNativeEvidence.h"
#endif

AMAYADeliveryParcel::AMAYADeliveryParcel()
{
    ParcelBody = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DeliveryParcelBody"));
    SetRootComponent(ParcelBody);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Cube.Succeeded()) ParcelBody->SetStaticMesh(Cube.Object);
    ParcelBody->SetWorldScale3D(FVector(0.24, 0.18, 0.12));
    ParcelBody->SetCollisionProfileName(TEXT("PhysicsActor"));
    ParcelBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    ParcelBody->SetSimulatePhysics(true);
    ParcelBody->SetNotifyRigidBodyCollision(true);
    ParcelBody->SetLinearDamping(0.4f);
    ParcelBody->SetAngularDamping(0.8f);
    // The imported box is presentation only. The independent 24 x 18 x 12 cm
    // cube remains the complete physical envelope used by all service queries.
    ParcelVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DeliveryParcelPaperBoxVisual"));
    ParcelVisual->SetupAttachment(ParcelBody);
    ParcelVisual->SetMobility(EComponentMobility::Movable);
    ParcelVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ParcelVisual->SetCollisionResponseToAllChannels(ECR_Ignore);
    ParcelVisual->SetGenerateOverlapEvents(false);
    ParcelVisual->SetCanEverAffectNavigation(false);
    ParcelVisual->SetSimulatePhysics(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> PaperBox(
        TEXT("/Game/MAYA/Cargo/PaperBox01_V1/SM_PaperBox01_V2.SM_PaperBox01_V2"));
    if (PaperBox.Succeeded())
    {
        const FBoxSphereBounds LocalBounds = PaperBox.Object->GetBounds();
        const FVector Size = LocalBounds.BoxExtent * 2.0;
        // Leave at least 0.1 cm of clearance on every proxy face. The asset's
        // observed upright 40 x 30 x 20 cm bounds need no rotation.
        const FVector AvailableSize(23.8, 17.8, 11.8);
        const FVector PhysicsScale(0.24, 0.18, 0.12);
        if (!LocalBounds.Origin.ContainsNaN() && !Size.ContainsNaN()
            && Size.X > UE_SMALL_NUMBER && Size.Y > UE_SMALL_NUMBER && Size.Z > UE_SMALL_NUMBER)
        {
            const double Fit = FMath::Min3(AvailableSize.X / Size.X,
                AvailableSize.Y / Size.Y, AvailableSize.Z / Size.Z);
            if (FMath::IsFinite(Fit) && Fit > 0)
            {
                ParcelVisual->SetStaticMesh(PaperBox.Object);
                // Compensate the parent's nonuniform physics scale so the
                // imported mesh has uniform world scale, then center its bounds.
                ParcelVisual->SetRelativeTransform(FTransform(FQuat::Identity,
                    -LocalBounds.Origin * Fit / PhysicsScale, FVector(Fit) / PhysicsScale));
                ParcelVisualUniformScale = Fit;
                ParcelBody->SetVisibility(false, false); // do not hide the new child
                ParcelBody->SetCastShadow(false);
            }
        }
    }
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> FadeMaterial(
        TEXT("/Game/MAYA/Cargo/PaperBox01_V1/M_PaperBox01_AirFade_V1.M_PaperBox01_AirFade_V1"));
    if (FadeMaterial.Succeeded()) AirFadeMaterial = FadeMaterial.Object;
    CargoConstraint = CreateDefaultSubobject<UPhysicsConstraintComponent>(TEXT("DeliveryCargoTieDown"));
    CargoConstraint->SetupAttachment(ParcelBody);
    CargoConstraint->SetDisableCollision(false); // retain real cargo/carrier contacts
    CargoConstraint->SetLinearXLimit(LCM_Locked, 0);
    CargoConstraint->SetLinearYLimit(LCM_Locked, 0);
    CargoConstraint->SetLinearZLimit(LCM_Locked, 0);
    CargoConstraint->SetAngularSwing1Limit(ACM_Locked, 0);
    CargoConstraint->SetAngularSwing2Limit(ACM_Locked, 0);
    CargoConstraint->SetAngularTwistLimit(ACM_Locked, 0);
}

bool AMAYADeliveryParcel::Configure(const FString& Id, double Kg)
{
    if (Id.IsEmpty() || !FMath::IsFinite(Kg) || Kg <= 0 || !IsValid(ParcelBody)) return false;
    ParcelId = Id; ConfiguredMassKg = Kg;
    ParcelBody->SetWorldScale3D(FVector(0.24, 0.18, 0.12));
    ParcelBody->SetMassOverrideInKg(NAME_None, float(Kg), true);
    if (ParcelVisual->GetStaticMesh())
    {
        const FVector RenderSize = ParcelVisual->GetStaticMesh()->GetBounds().BoxExtent
            * 2.0 * ParcelVisual->GetComponentScale().GetAbs();
        UE_LOG(LogTemp, Display, TEXT("MAYA parcel render id=%s mesh=%s uniformScale=%.9f renderSizeCm=%s physicalEnvelopeCm=(24,18,12) renderCollision=None"),
            *ParcelId, *ParcelVisual->GetStaticMesh()->GetPathName(), ParcelVisualUniformScale, *RenderSize.ToString());
    }
    else UE_LOG(LogTemp, Warning, TEXT("MAYA parcel paper-box visual unavailable; physical cube remains visible id=%s"), *ParcelId);
    double Measured = 0;
    return ObserveMass(Measured) && FMath::Abs(Measured - Kg) <= 0.05;
}

bool AMAYADeliveryParcel::ObserveMass(double& Kg) const
{
    if (!IsValid(ParcelBody) || !ParcelBody->GetBodyInstance()
        || !ParcelBody->GetBodyInstance()->IsValidBodyInstance()) return false;
    Kg = ParcelBody->GetMass();
    return FMath::IsFinite(Kg) && Kg > 0;
}

bool AMAYADeliveryParcel::ObserveNativePose(FTransform& OutPose) const
{
    const FBodyInstance* Body = IsValid(ParcelBody) ? ParcelBody->GetBodyInstance() : nullptr;
    if (!Body || !Body->IsValidBodyInstance()) return false;
    // Installed UE 5.3 executes an actor read and reads its actual global pose.
    // Disable custom projection; do not substitute the component/kinematic target.
    OutPose = Body->GetUnrealWorldTransform(false, true);
    return !OutPose.ContainsNaN() && OutPose.GetRotation().IsNormalized();
}

void AMAYADeliveryParcel::ReleaseForStaging()
{
    // UE 5.3 ignores null endpoints in SetConstrainedComponents and then
    // initializes the joint again. Terminate the joint without reinitializing it.
    CargoConstraint->BreakConstraint();
}

bool AMAYADeliveryParcel::BeginStaging()
{
    if (!IsValid(ParcelBody)) return false;
    ReleaseForStaging();
    ParcelBody->SetPhysicsLinearVelocity(FVector::ZeroVector);
    ParcelBody->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
    ParcelBody->SetSimulatePhysics(false);
    bStaging = true;
    return true;
}

bool AMAYADeliveryParcel::MoveStaged(const FVector& Position, FHitResult& Hit)
{
    if (!bStaging || Position.ContainsNaN()) return false;
    ParcelBody->SetWorldLocation(Position, true, &Hit, ETeleportType::None);
    return !Hit.bBlockingHit && FVector::Dist(ParcelBody->GetComponentLocation(), Position) <= 0.5;
}

bool AMAYADeliveryParcel::IsAt(const FTransform& Target, double Tolerance) const
{
    return IsValid(ParcelBody) && !Target.ContainsNaN()
        && FVector::Dist(ParcelBody->GetComponentLocation(), Target.GetLocation()) <= Tolerance
        && ParcelBody->GetComponentQuat().AngularDistance(Target.GetRotation()) <= FMath::DegreesToRadians(3.0);
}

bool AMAYADeliveryParcel::PlaceAt(const FTransform& Pose)
{
    if (Pose.ContainsNaN() || !BeginStaging()) return false;
    ParcelBody->SetWorldLocationAndRotation(Pose.GetLocation(), Pose.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
    ParcelBody->SetSimulatePhysics(true);
    bStaging = false;
    return IsAt(Pose);
}

bool AMAYADeliveryParcel::CanUseRecoveryPose(const FTransform& Pose, FString& OutReason) const
{
    OutReason = TEXT("Recovery parcel geometry or destination unavailable");
    const FBodyInstance* Body = IsValid(ParcelBody) ? ParcelBody->GetBodyInstance() : nullptr;
    const UBodySetup* Setup = IsValid(ParcelBody) ? ParcelBody->GetBodySetup() : nullptr;
    if (!GetWorld() || Pose.ContainsNaN() || !Pose.GetRotation().IsNormalized()
        || !Body || !Body->IsValidBodyInstance() || Body->WeldParent || !Setup
        || Setup->AggGeom.GetElementCount() == 0 || Setup->CollisionTraceFlag == CTF_UseComplexAsSimple
        || ParcelBody->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
        || !Pose.GetScale3D().Equals(ParcelBody->GetComponentScale(), 1.e-6)) return false;
    // Recovery explicitly reconstructs poses. This is an endpoint query, not a
    // normal swept transfer receipt. Ignore only this independent parcel itself.
    FComponentQueryParams Query(SCENE_QUERY_STAT(MAYARecoveryParcelEndpoint), this);
    Query.bTraceComplex = false;
    TArray<FOverlapResult> Overlaps;
    ParcelBody->ComponentOverlapMulti(Overlaps, GetWorld(), Pose.GetLocation(), Pose.GetRotation(),
        ParcelBody->GetCollisionObjectType(), Query);
    for (const FOverlapResult& Hit : Overlaps) if (Hit.bBlockingHit)
    {
        OutReason = FString::Printf(TEXT("Recovery parcel destination blocked by %s"), *GetPathNameSafe(Hit.GetComponent()));
        return false;
    }
    OutReason = TEXT("Actual recovery parcel destination clear against world and robot");
    return true;
}

bool AMAYADeliveryParcel::PlaceForRecovery(const FTransform& Pose, FString& OutReason)
{
    if (!CanUseRecoveryPose(Pose, OutReason)) return false;
    bAirLowering = bAirRaising = false; RestoreAirAppearanceMaterials();
    if (!BeginStaging()) return false;
    // An explicitly requested recovery/reset may reconstruct a validated pose.
    // Keep QueryAndPhysics throughout; never hide the parcel from reset queries.
    ParcelBody->SetWorldLocationAndRotation(Pose.GetLocation(), Pose.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
    FTransform NativePose;
    if (!IsAt(Pose, 0.1) || !ObserveNativePose(NativePose)
        || FVector::Dist(NativePose.GetLocation(), Pose.GetLocation()) > 0.1
        || NativePose.GetRotation().AngularDistance(Pose.GetRotation()) > FMath::DegreesToRadians(0.1))
    {
        OutReason = TEXT("Recovery parcel native endpoint did not match the validated reconstruction pose");
        return false;
    }
    ParcelBody->SetSimulatePhysics(true);
    bStaging = false;
    OutReason = TEXT("Same physical parcel reconstructed at validated depot endpoint; collision retained");
    return true;
}

bool AMAYADeliveryParcel::SecureTo(UPrimitiveComponent* Carrier, const FTransform& Pose)
{
    if (!IsValid(Carrier) || !Carrier->IsSimulatingPhysics() || !IsAt(Pose)) return false;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    const bool bCaptureInitialization = TieDownInitializationEvidenceJson.IsEmpty();
    const FString BeforeSimulation = bCaptureInitialization
        ? MAYAV01CaptureCargoPhysicsJson(this, Carrier, TEXT("before_simulation_enable")) : FString();
#endif
    ParcelBody->SetSimulatePhysics(true);
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    const FString AfterSimulation = bCaptureInitialization
        ? MAYAV01CaptureCargoPhysicsJson(this, Carrier, TEXT("after_simulation_enable")) : FString();
#endif
    ParcelBody->SetPhysicsLinearVelocity(Carrier->GetPhysicsLinearVelocity());
    ParcelBody->SetPhysicsAngularVelocityInRadians(Carrier->GetPhysicsAngularVelocityInRadians());
    CargoConstraint->SetWorldLocation(Pose.GetLocation());
    CargoConstraint->SetConstrainedComponents(ParcelBody, NAME_None, Carrier, NAME_None);
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
    if (bCaptureInitialization)
    {
        const FString AfterConstraint = MAYAV01CaptureCargoPhysicsJson(this, Carrier, TEXT("after_constraint_initialization"));
        TieDownInitializationEvidenceJson = FString::Printf(
            TEXT("{\"schema\":\"MayaRobotics.CargoTieDownInitialization.v1\",\"scope\":\"Read-only creation observations; solver-step scope unknown\",\"samples\":[%s,%s,%s]}"),
            *BeforeSimulation, *AfterSimulation, *AfterConstraint);
    }
#endif
    bStaging = false;
    return IsSecuredTo(Carrier, Pose);
}

bool AMAYADeliveryParcel::IsSecuredTo(UPrimitiveComponent* Carrier, const FTransform& Target) const
{
    UPrimitiveComponent *A = nullptr, *B = nullptr;
    FName BoneA, BoneB;
    CargoConstraint->GetConstrainedComponents(A, BoneA, B, BoneB);
    return IsValid(Carrier) && A == ParcelBody && B == Carrier && ParcelBody->IsSimulatingPhysics()
        && CargoConstraint->ConstraintInstance.IsValidConstraintInstance() && !CargoConstraint->IsBroken()
        && IsAt(Target);
}

void AMAYADeliveryParcel::RestoreAirAppearanceMaterials()
{
    if (IsValid(ParcelVisual))
        for (int32 I = 0; I < AirOriginalMaterials.Num(); ++I) ParcelVisual->SetMaterial(I, AirOriginalMaterials[I]);
    AirAppearanceMaterials.Reset(); AirAppearanceAlpha = 1;
}

bool AMAYADeliveryParcel::ObserveAirAppearance(double& OutAlpha, bool& OutOriginalMaterialsRestored) const
{
    OutAlpha = AirAppearanceAlpha; OutOriginalMaterialsRestored = false;
    if (!IsValid(ParcelVisual) || !FMath::IsFinite(OutAlpha) || AirOriginalMaterials.Num() != 1) return false;
    if (AirAppearanceMaterials.Num())
    {
        if (AirAppearanceMaterials.Num() != 1 || !IsValid(AirAppearanceMaterials[0])
            || ParcelVisual->GetMaterial(0) != AirAppearanceMaterials[0]) return false;
        OutAlpha = AirAppearanceMaterials[0]->K2_GetScalarParameterValue(TEXT("MayaCargoAppearanceAlpha"));
        return FMath::IsFinite(OutAlpha) && FMath::Abs(OutAlpha - AirAppearanceAlpha) <= 1.e-6;
    }
    OutOriginalMaterialsRestored = ParcelVisual->GetMaterial(0) == AirOriginalMaterials[0];
    return OutOriginalMaterialsRestored && OutAlpha == 1;
}

bool AMAYADeliveryParcel::CanBeginAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const
{
    FTransform Native, Start, Upper; double Top = 0;
    if (!CanRemoveFromDeliverySlot(Robot, Slot, OutReason) || bAirRaising || bAirLowering
        || !IsValid(AirFadeMaterial) || !IsValid(ParcelVisual) || ParcelVisual->GetNumMaterials() != 1
        || !IsValid(ParcelVisual->GetMaterial(0))
        || ParcelVisual->GetMaterial(0)->GetPathName() != TEXT("/Game/MAYA/Cargo/PaperBox01_V1/M_PaperBox01_V2.M_PaperBox01_V2")
        || !Robot->GetDeliveryCargoAirRemovalPoses(ParcelBody, Slot, Start, Upper, Top)
        || !ObserveNativePose(Native) || FVector::Dist(Native.GetLocation(), Start.GetLocation()) > 0.1
        || Native.GetRotation().AngularDistance(Start.GetRotation()) > FMath::DegreesToRadians(0.1))
    { if (Robot) OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    return true;
}

bool AMAYADeliveryParcel::BeginAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    if (!CanBeginAirRaising(Robot, Slot, OutReason)) return false;
    FTransform Start, Upper; double Top = 0;
    if (!Robot->GetDeliveryCargoAirRemovalPoses(ParcelBody, Slot, Start, Upper, Top)) return false;
    FVector NativeBay; double NativeTop = 0;
    if (!Robot->ReadDeliveryCargoAirRaisingFrame(ParcelBody, Slot, NativeBay, NativeTop)) return false;
    if (!BeginStaging()) return false;
    // Simulation release may expose a slightly newer native actor quaternion
    // than the last component observation. Retain that actual orientation as
    // the canonical fixed query/movement Q, rather than relaxing its guard.
    // Mark the mutated same body immediately so any failure remains restorable.
    bAirRaising = true; AirStartPose = Start; AirTargetPose = Upper; AirOpenAssemblyTopCm = Top;
    FTransform NativeRelease;
    if (!ObserveNativePose(NativeRelease)
        || FVector::Dist(NativeRelease.GetLocation(), Start.GetLocation()) > 0.1
        || NativeRelease.GetRotation().AngularDistance(Start.GetRotation()) > FMath::DegreesToRadians(0.1))
    { OutReason = TEXT("Air unloading actual released native pose differs from its checked component endpoint"); return false; }
    // UE5.3's swept component update can retain a cached rotator when the
    // actual native Q differs by less than its rotator-change threshold. Sync
    // that representation to the observed released body without writing physics.
    // The body already occupies this pose; no cargo motion or collision is skipped.
    ParcelBody->SetWorldLocationAndRotationNoPhysics(NativeRelease.GetLocation(), NativeRelease.Rotator());
    FTransform NativeAfterRepresentationSync;
    const double ComponentReleaseAngularDelta = ParcelBody->GetComponentQuat().AngularDistance(NativeRelease.GetRotation());
    if (!ObserveNativePose(NativeAfterRepresentationSync)
        || FVector::Dist(NativeAfterRepresentationSync.GetLocation(), NativeRelease.GetLocation()) > 0.1
        || NativeAfterRepresentationSync.GetRotation().AngularDistance(NativeRelease.GetRotation()) > 1.e-6
        || FVector::Dist(ParcelBody->GetComponentLocation(), NativeRelease.GetLocation()) > 0.1
        || ComponentReleaseAngularDelta > 1.e-6)
    {
        OutReason = FString::Printf(TEXT("Air unloading released-body component representation could not synchronize without physics movement; componentAngularDeltaRad=%.12f; limitRad=0.000001"),
            ComponentReleaseAngularDelta); return false;
    }
    // Actor pose has native position/Q; retain the original physical mesh scale.
    Start.SetLocation(NativeRelease.GetLocation()); Start.SetRotation(NativeRelease.GetRotation());
    Upper = Start;
    const FQuat Q = Start.GetRotation();
    const double BottomOffset = FMath::Abs(Q.GetAxisX().Z) * 12.0
        + FMath::Abs(Q.GetAxisY().Z) * 9.0 + FMath::Abs(Q.GetAxisZ().Z) * 6.0;
    FVector UpperCm = Start.GetLocation(); UpperCm.Z = Top + 60.96 + BottomOffset; Upper.SetLocation(UpperCm);
    if (!Robot->AdmitDeliveryCargoAirRaising(ParcelBody, Slot, Start, Upper))
    { OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    AirRaisingInitialBayCm = NativeBay; AirRaisingInitialLower = Start;
    bAirRaisingUpperLatched = false; AirRaisingEasedProgress = 0;
    // Release the real restraint at its existing physical pose. Normal unload
    // starts here without an endpoint teleport or a replacement actor.
    bAirRaising = true; AirStartPose = Start; AirTargetPose = Upper; AirOpenAssemblyTopCm = Top;
    RestoreAirAppearanceMaterials();
    if (AirOriginalMaterials.Num() == 0) AirOriginalMaterials.Add(ParcelVisual->GetMaterial(0));
    bDormantForDeliveryRemoval = false;
    OutReason = TEXT("Same original native parcel released for two-second swept upward motion; original opaque material retained");
    return true;
}

bool AMAYADeliveryParcel::MoveAirRaising(AMAYANativeDemoPawn* Robot, int32 Slot, double EasedAlpha, FString& OutReason)
{
    if (!bAirRaising || !bStaging || bAirRaisingUpperLatched || !IsValid(Robot) || !FMath::IsFinite(EasedAlpha)
        || EasedAlpha < AirRaisingEasedProgress || EasedAlpha < 0 || EasedAlpha > 1)
    { OutReason = TEXT("Air unloading progress or physical staging unavailable"); return false; }
    FVector NativeBay; double NativeTop = 0;
    if (!Robot->ReadDeliveryCargoAirRaisingFrame(ParcelBody, Slot, NativeBay, NativeTop))
    { OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    // Only unloading tracks the measured stopped bay. The captured parcel
    // orientation and two-second smoothstep are unchanged; no pose is teleported.
    FTransform Lower = AirRaisingInitialLower;
    FVector LowerCm = AirRaisingInitialLower.GetLocation();
    LowerCm.X += NativeBay.X - AirRaisingInitialBayCm.X;
    LowerCm.Y += NativeBay.Y - AirRaisingInitialBayCm.Y;
    Lower.SetLocation(LowerCm);
    FTransform Upper = Lower;
    const FQuat Q = Lower.GetRotation();
    const double BottomOffset = FMath::Abs(Q.GetAxisX().Z) * 12.0
        + FMath::Abs(Q.GetAxisY().Z) * 9.0 + FMath::Abs(Q.GetAxisZ().Z) * 6.0;
    FVector UpperCm = Lower.GetLocation(); UpperCm.Z = NativeTop + 60.96 + BottomOffset;
    Upper.SetLocation(UpperCm);
    if (!FMath::IsFinite(UpperCm.Z) || UpperCm.Z <= Lower.GetLocation().Z
        || !Robot->AdmitDeliveryCargoAirRaisingRemaining(ParcelBody, Slot, Upper))
    { OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    FHitResult Hit;
    const FVector NextCm = FMath::Lerp(Lower.GetLocation(), Upper.GetLocation(), EasedAlpha);
    // Use the same actual released native Q in the incremental component sweep.
    // This only reconciles the stale component representation during real
    // translated motion; it never teleports or rotates the native body to a
    // guessed orientation. Pickup's existing MoveStaged remains unchanged.
    ParcelBody->SetWorldLocationAndRotation(NextCm, Upper.GetRotation(), true, &Hit, ETeleportType::None);
    if (Hit.bBlockingHit)
    {
        OutReason = FString::Printf(TEXT("Actual measured-bay upward incremental sweep blocked by %s; penetrating=%d; depthCm=%.9f; time=%.9f"),
            *GetPathNameSafe(Hit.GetComponent()), int32(Hit.bStartPenetrating), double(Hit.PenetrationDepth), double(Hit.Time)); return false;
    }
    const double ComponentPositionError = FVector::Dist(ParcelBody->GetComponentLocation(), NextCm);
    const double ComponentFixedAngularDelta = ParcelBody->GetComponentQuat().AngularDistance(Upper.GetRotation());
    if (ComponentPositionError > 0.5 || ComponentFixedAngularDelta > 1.e-6)
    {
        OutReason = FString::Printf(TEXT("Actual measured-bay upward incremental component endpoint differs without blocking hit; positionErrorCm=%.12f; positionLimitCm=0.5; angularDeltaRad=%.12f; angularLimitRad=0.000001"),
            ComponentPositionError, ComponentFixedAngularDelta); return false;
    }
    AirStartPose = Lower; AirTargetPose = Upper; AirOpenAssemblyTopCm = NativeTop;
    AirRaisingEasedProgress = EasedAlpha;
    // Freeze the actual final target for native synchronization and the entire
    // fade/pause interval. Subsequent checks query only the real remaining route.
    if (EasedAlpha == 1) bAirRaisingUpperLatched = true;
    return true;
}

bool AMAYADeliveryParcel::ValidateAirRaisingRemaining(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const
{
    if (!bAirRaising || !bStaging || DeliverySlot != Slot || !IsValid(Robot)
        || !Robot->AdmitDeliveryCargoAirRaisingRemaining(ParcelBody, Slot, AirTargetPose))
    { OutReason = Robot ? Robot->ReadDeliveryCargoTransferStatus() : TEXT("Air unloading remaining route unavailable"); return false; }
    return true;
}

bool AMAYADeliveryParcel::IsAirRaisingEndpointReady() const
{
    FTransform Native;
    return bAirRaising && bStaging && bAirRaisingUpperLatched && IsAt(AirTargetPose, 0.1) && ObserveNativePose(Native)
        && FVector::Dist(Native.GetLocation(), AirTargetPose.GetLocation()) <= 0.1
        && Native.GetRotation().AngularDistance(AirTargetPose.GetRotation()) <= FMath::DegreesToRadians(0.1);
}

bool AMAYADeliveryParcel::BeginAirDisappearance()
{
    if (!IsAirRaisingEndpointReady() || AirAppearanceAlpha != 1 || !IsValid(AirFadeMaterial)
        || !IsValid(ParcelVisual) || AirAppearanceMaterials.Num() != 0) return false;
    UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(AirFadeMaterial, this);
    if (!Dynamic) return false;
    Dynamic->SetScalarParameterValue(TEXT("MayaCargoAppearanceAlpha"), 1.0f);
    ParcelVisual->SetMaterial(0, Dynamic); AirAppearanceMaterials.Add(Dynamic);
    double Alpha = 0; bool Restored = false;
    return ObserveAirAppearance(Alpha, Restored) && Alpha == 1;
}

bool AMAYADeliveryParcel::SetAirDisappearanceAlpha(double Alpha)
{
    if (!bAirRaising || !FMath::IsFinite(Alpha) || Alpha > AirAppearanceAlpha || Alpha < 0 || Alpha > 1
        || AirAppearanceMaterials.Num() != 1) return false;
    for (UMaterialInstanceDynamic* M : AirAppearanceMaterials)
    { if (!IsValid(M)) return false; M->SetScalarParameterValue(TEXT("MayaCargoAppearanceAlpha"), float(Alpha)); }
    AirAppearanceAlpha = Alpha; double Actual = 0; bool Restored = false;
    return ObserveAirAppearance(Actual, Restored) && FMath::Abs(Actual - Alpha) <= 1.e-6;
}

bool AMAYADeliveryParcel::CanFinishAirRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const
{
    double Alpha = 1, Kg = 0; bool Restored = true;
    if (!IsValid(Robot) || DeliverySlot != Slot || !IsAirRaisingEndpointReady()
        || !ObserveAirAppearance(Alpha, Restored) || Alpha != 0
        || ParcelBody->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
        || !ObserveMass(Kg) || FMath::Abs(Kg - ConfiguredMassKg) > 0.05
        || !ValidateAirRaisingRemaining(Robot, Slot, OutReason))
    { if (OutReason.IsEmpty()) OutReason = TEXT("Actual faded upper-endpoint removal unavailable"); return false; }
    return true;
}

bool AMAYADeliveryParcel::FinishAirRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    if (!CanFinishAirRemoval(Robot, Slot, OutReason)) return false;
    // Actual opacity is zero and the native body is at its checked upper pose.
    // Keep this exact actor/ID reversible until the closed-lid Core commit.
    ParcelBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    bStaging = false; bDormantForDeliveryRemoval = true; bAirRaising = false;
    SetActorHiddenInGame(true);
    OutReason = TEXT("Same raised parcel faded to actual opacity0; physical removal reversible pending closed-latch commit");
    return !ParcelBody->IsSimulatingPhysics() && ParcelBody->GetCollisionEnabled() == ECollisionEnabled::NoCollision;
}

bool AMAYADeliveryParcel::BeginAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    OutReason = TEXT("Air pickup appearance or physical staging unavailable");
    if (!IsValid(Robot) || !IsValid(AirFadeMaterial) || !IsValid(ParcelVisual)
        || !ParcelVisual->GetStaticMesh() || ParcelId.IsEmpty() || ConfiguredMassKg <= 0
        || ParcelVisual->GetNumMaterials() != 1 || !IsValid(ParcelVisual->GetMaterial(0))
        || ParcelVisual->GetMaterial(0)->GetPathName() != TEXT("/Game/MAYA/Cargo/PaperBox01_V1/M_PaperBox01_V2.M_PaperBox01_V2")) return false;
    FTransform Start, Target; double Top = 0;
    if (!Robot->GetDeliveryCargoAirPlacementPoses(ParcelBody, Slot, Start, Target, Top))
    { OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    if (!BeginStaging()) return false;
    RestoreAirAppearanceMaterials();
    AirOriginalMaterials.Reset();
    AirOriginalMaterials.Add(ParcelVisual->GetMaterial(0));
    UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(AirFadeMaterial, this);
    if (!Dynamic) { RestoreAirAppearanceMaterials(); return false; }
    Dynamic->SetScalarParameterValue(TEXT("MayaCargoAppearanceAlpha"), 0.0f);
    ParcelVisual->SetMaterial(0, Dynamic); AirAppearanceMaterials.Add(Dynamic);
    AirAppearanceAlpha = 0; AirStartPose = Start; AirTargetPose = Target; AirOpenAssemblyTopCm = Top;
    bAirLowering = true; DeliverySlot = INDEX_NONE; bDormantForDeliveryRemoval = false;
    // The explicitly requested magical appearance initializes the hidden box at
    // its checked airborne endpoint. Every subsequent visible movement is swept.
    ParcelBody->SetWorldLocationAndRotation(Start.GetLocation(), Start.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
    ParcelBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    SetActorHiddenInGame(false);
    FTransform Native; double Kg = 0;
    if (!ObserveNativePose(Native) || FVector::Dist(Native.GetLocation(), Start.GetLocation()) > 0.1
        || Native.GetRotation().AngularDistance(Start.GetRotation()) > FMath::DegreesToRadians(0.1)
        || !ObserveMass(Kg) || FMath::Abs(Kg - ConfiguredMassKg) > 0.05)
    { OutReason = TEXT("Air pickup actual initial native pose or mass differs from its checked endpoint"); return false; }
    OutReason = TEXT("Same real parcel staged with bottom60.96cm above actual open assembly; opacity0; collision retained");
    return true;
}

bool AMAYADeliveryParcel::SetAirAppearanceAlpha(double Alpha)
{
    if (!bAirLowering || !FMath::IsFinite(Alpha) || Alpha < AirAppearanceAlpha || Alpha < 0 || Alpha > 1) return false;
    for (UMaterialInstanceDynamic* M : AirAppearanceMaterials)
    { if (!IsValid(M)) return false; M->SetScalarParameterValue(TEXT("MayaCargoAppearanceAlpha"), float(Alpha)); }
    AirAppearanceAlpha = Alpha;
    if (Alpha >= 1) RestoreAirAppearanceMaterials();
    double Actual = 0; bool Restored = false;
    return ObserveAirAppearance(Actual, Restored) && FMath::Abs(Actual - Alpha) <= 1.e-6;
}

bool AMAYADeliveryParcel::MoveAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, double EasedAlpha, FString& OutReason)
{
    if (!bAirLowering || !bStaging || !IsValid(Robot) || !FMath::IsFinite(EasedAlpha)
        || EasedAlpha < 0 || EasedAlpha > 1 || AirAppearanceAlpha < 1
        || !Robot->AdmitDeliveryCargoAirLowering(ParcelBody, Slot, ParcelBody->GetComponentTransform(), AirTargetPose))
    { OutReason = Robot ? Robot->ReadDeliveryCargoTransferStatus() : TEXT("Air descent state unavailable"); return false; }
    const FVector Next = FMath::Lerp(AirStartPose.GetLocation(), AirTargetPose.GetLocation(), EasedAlpha);
    FHitResult Hit;
    if (!MoveStaged(Next, Hit))
    {
        OutReason = FString::Printf(TEXT("Actual air descent sweep blocked by %s; penetrating=%d; depthCm=%.9f; time=%.9f"),
            *GetPathNameSafe(Hit.GetComponent()), int32(Hit.bStartPenetrating), double(Hit.PenetrationDepth), double(Hit.Time)); return false;
    }
    OutReason = TEXT("Actual fixed-orientation parcel descent swept to its eased world-height sample"); return true;
}

bool AMAYADeliveryParcel::IsAirLoweringEndpointReady() const
{
    FTransform Native;
    return bAirLowering && bStaging && IsAt(AirTargetPose, 0.1) && ObserveNativePose(Native)
        && FVector::Dist(Native.GetLocation(), AirTargetPose.GetLocation()) <= 0.1
        && Native.GetRotation().AngularDistance(AirTargetPose.GetRotation()) <= FMath::DegreesToRadians(0.1);
}

bool AMAYADeliveryParcel::FinishAirLowering(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    if (!IsValid(Robot) || !IsAirLoweringEndpointReady()
        || !Robot->AdmitDeliveryCargoAirLowering(ParcelBody, Slot, ParcelBody->GetComponentTransform(), AirTargetPose))
    { OutReason = TEXT("Air pickup checked native endpoint is not ready for restraint"); return false; }
    auto* Mount = Robot->GetDeliveryCargoMount(Slot);
    auto* Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    double Kg = 0;
    if (!Carrier || !Carrier->IsSimulatingPhysics() || !ObserveMass(Kg)
        || FMath::Abs(Kg - ConfiguredMassKg) > 0.05) return false;
    // No final teleport or component-pose substitution: the actual kinematic
    // actor must already agree before enabling simulation and creating the joint.
    ResetTieDownInitializationEvidence();
    if (!SecureTo(Carrier, AirTargetPose))
    { OutReason = TEXT("Air pickup actual native restraint creation failed"); return false; }
    DeliverySlot = Slot; bAirLowering = false; RestoreAirAppearanceMaterials();
    OutReason = TEXT("Air descent actual endpoint synchronized; same independent mass tied down without endpoint teleport");
    return true;
}

bool AMAYADeliveryParcel::InsertIntoDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    OutReason = TEXT("Instant cargo placement unavailable");
    FTransform Target;
    if (!IsValid(Robot) || ParcelId.IsEmpty() || ConfiguredMassKg <= 0
        || !Robot->GetDeliveryCargoPlacementTarget(ParcelBody, Slot, Target)
        || !Robot->AdmitDeliveryCargoPlacement(ParcelBody, Slot))
    { if (Robot) OutReason = Robot->ReadDeliveryCargoTransferStatus(); return false; }
    auto* Mount = Robot->GetDeliveryCargoMount(Slot);
    auto* Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    if (!Carrier || !Carrier->IsSimulatingPhysics()) return false;
    bAirLowering = false; RestoreAirAppearanceMaterials();
    ReleaseForStaging();
    ParcelBody->SetSimulatePhysics(false);
    // Explicitly authorized instant placement, not a swept transfer. When dormant
    // the endpoint was queried before its native body/collision is recreated.
    ParcelBody->SetWorldLocationAndRotation(Target.GetLocation(), Target.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
    ParcelBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    bDormantForDeliveryRemoval = false;
    SetActorHiddenInGame(false);
    FTransform Native;
    if (!ObserveNativePose(Native) || FVector::Dist(Native.GetLocation(), Target.GetLocation()) > 0.1
        || Native.GetRotation().AngularDistance(Target.GetRotation()) > FMath::DegreesToRadians(0.1))
    { OutReason = TEXT("Instant cargo native endpoint did not match the validated placement"); return false; }
    ParcelBody->SetMassOverrideInKg(NAME_None, float(ConfiguredMassKg), true);
    ResetTieDownInitializationEvidence();
    if (!SecureTo(Carrier, Target))
    { OutReason = TEXT("Instant cargo fixed physical restraint failed at validated endpoint"); return false; }
    double Kg = 0;
    if (!ObserveMass(Kg) || FMath::Abs(Kg - ConfiguredMassKg) > 0.05)
    { OutReason = TEXT("Instant cargo actual mass differs from configured parcel mass"); return false; }
    bDormantForDeliveryRemoval = false; DeliverySlot = Slot; bAirRaising = false;
    SetActorHiddenInGame(false);
    OutReason = TEXT("Same independent parcel physically placed, native endpoint confirmed and tied down in validated bay");
    return true;
}

bool AMAYADeliveryParcel::CanRemoveFromDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason) const
{
    OutReason = TEXT("Instant cargo removal refused: original actual parcel custody or open supported service unavailable");
    FMAYADeliveryRobotObservation O; FTransform Target, Native;
    if (!IsValid(Robot) || !Robot->IsDeliveryDriveLocked() || UGameplayStatics::IsGamePaused(this)
        || bDormantForDeliveryRemoval || DeliverySlot != Slot
        || !Robot->ReadDeliveryState(O) || !O.IsStoppedAndSupported() || !O.bDoorsOpen
        || !Robot->GetDeliveryCargoPlacementTarget(ParcelBody, Slot, Target)) return false;
    auto* Mount = Robot->GetDeliveryCargoMount(Slot);
    auto* Carrier = Mount ? Cast<UPrimitiveComponent>(Mount->GetAttachParent()) : nullptr;
    double Kg = 0;
    if (!IsSecuredTo(Carrier, Target) || !ObserveMass(Kg) || FMath::Abs(Kg - ConfiguredMassKg) > 0.05
        || !ObserveNativePose(Native) || FVector::Dist(Native.GetLocation(), Target.GetLocation()) > 2
        || Native.GetRotation().AngularDistance(Target.GetRotation()) > FMath::DegreesToRadians(3)) return false;
    OutReason = TEXT("Same independent parcel mass, ID, native pose and valid restraint measured at open supported bay");
    return true;
}

bool AMAYADeliveryParcel::HideForDeliveryRemoval(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    if (!CanRemoveFromDeliverySlot(Robot, Slot, OutReason)) return false;
    ReleaseForStaging();
    ParcelBody->SetSimulatePhysics(false);
    ParcelBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    bStaging = false; bDormantForDeliveryRemoval = true;
    SetActorHiddenInGame(true);
    OutReason = TEXT("Same parcel actor/ID retained reversibly; visible and physical body removed pending actual closed delivery commit");
    return !ParcelBody->IsSimulatingPhysics() && ParcelBody->GetCollisionEnabled() == ECollisionEnabled::NoCollision;
}

bool AMAYADeliveryParcel::RestoreToDeliverySlot(AMAYANativeDemoPawn* Robot, int32 Slot, FString& OutReason)
{
    if ((!bDormantForDeliveryRemoval && !bAirRaising) || DeliverySlot != Slot)
    { OutReason = TEXT("No matching reversibly removed parcel exists for restoration"); return false; }
    return InsertIntoDeliverySlot(Robot, Slot, OutReason);
}
