#include "MAYACargoWorker.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ConstructorHelpers.h"

namespace MayaCargoWorker
{
    const FName UpperArm[2] = {TEXT("upperarm_l"), TEXT("upperarm_r")};
    const FName LowerArm[2] = {TEXT("lowerarm_l"), TEXT("lowerarm_r")};
    const FName Hand[2] = {TEXT("hand_l"), TEXT("hand_r")};
    const FName Middle[2] = {TEXT("middle_01_l"), TEXT("middle_01_r")};
    const FName Thumb[2] = {TEXT("thumb_01_l"), TEXT("thumb_01_r")};
    const FName Thigh[2] = {TEXT("thigh_l"), TEXT("thigh_r")};
    const FName Calf[2] = {TEXT("calf_l"), TEXT("calf_r")};
    const FName Foot[2] = {TEXT("foot_l"), TEXT("foot_r")};
    TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& V)
    {
        return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
    }
}

AMAYACargoWorker::AMAYACargoWorker()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("WorkerWorldCollision"));
    SetRootComponent(Capsule);
    Capsule->InitCapsuleSize(22, 88);
    Capsule->SetCollisionProfileName(TEXT("Pawn"));
    Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    BaseAnimation = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WorkerSourceAnimation"));
    BaseAnimation->SetupAttachment(Capsule);
    BaseAnimation->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BaseAnimation->SetForcedLOD(1);
    BaseAnimation->SetVisibility(false);
    BaseAnimation->SetHiddenInGame(true);
    BaseAnimation->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Person = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("AnimatedWorker"));
    Person->SetupAttachment(Capsule);
    Person->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Person->SetForcedLOD(1); // retain the actual arm/finger bones used by IK
    AddTickPrerequisiteComponent(BaseAnimation);
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> Human(TEXT("/Game/Character/Player/Male/Meshes/SKM_PlayerMale.SKM_PlayerMale"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Idle(TEXT("/Game/Crowd/Character/Anims/Loco/MTN_Set/MTN_N_Idle.MTN_N_Idle"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Walk(TEXT("/Game/Crowd/Character/Anims/Loco/MTN_Set/MTN_N_WalkInPlace.MTN_N_WalkInPlace"));
    DefaultPersonMesh = Human.Object; IdleAnimation = Idle.Object; WalkAnimation = Walk.Object;
}

void AMAYACargoWorker::BeginPlay()
{
    Super::BeginPlay();
    // Exact mesh/idle/walk skeleton identity was measured by the retained UE
    // asset inspection. Mesh +Y faces forward, so -90 yaw aligns it to actor +X.
    Configure(DefaultPersonMesh, IdleAnimation, WalkAnimation, -90);
}

bool AMAYACargoWorker::Configure(USkeletalMesh* Mesh, UAnimSequence* Idle, UAnimSequence* Walk, float MeshYaw)
{
    if (!Mesh || !Idle || !Walk || !FMath::IsFinite(MeshYaw)
        || Idle->GetSkeleton() != Mesh->GetSkeleton() || Walk->GetSkeleton() != Mesh->GetSkeleton())
    { Fault = TEXT("Worker mesh and actual animation skeletons do not match."); return false; }
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    TArray<FName> Required = {TEXT("pelvis"), TEXT("spine_01")};
    for (int32 K = 0; K < 2; ++K)
        Required.Append({MayaCargoWorker::UpperArm[K], MayaCargoWorker::LowerArm[K], MayaCargoWorker::Hand[K],
            MayaCargoWorker::Middle[K], MayaCargoWorker::Thumb[K], MayaCargoWorker::Thigh[K],
            MayaCargoWorker::Calf[K], MayaCargoWorker::Foot[K]});
    for (const FName Bone : Required)
        if (Ref.FindBoneIndex(Bone) == INDEX_NONE)
        { Fault = TEXT("Measured worker skeleton lacks required bone: ") + Bone.ToString(); return false; }
    BaseAnimation->SetSkeletalMesh(Mesh);
    Person->SetSkinnedAssetAndUpdate(Mesh);
    const FBox Bounds = Mesh->GetBounds().GetBox();
    if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN())
    { Fault = TEXT("Worker render bounds are unavailable."); return false; }
    const FVector Offset(0, 0, -Capsule->GetScaledCapsuleHalfHeight() - Bounds.Min.Z);
    BaseAnimation->SetRelativeLocationAndRotation(Offset, FRotator(0, MeshYaw, 0));
    Person->SetRelativeLocationAndRotation(Offset, FRotator(0, MeshYaw, 0));
    IdleAnimation = Idle; WalkAnimation = Walk;
    BaseAnimation->PlayAnimation(Idle, true);
    BaseAnimation->RefreshBoneTransforms();
    Person->CopyPoseFromSkeletalComponent(BaseAnimation);
    RebuildComponentPose();
    for (int32 K = 0; K < 2; ++K)
    {
        FTransform H, M, T;
        if (!GetComponentBone(MayaCargoWorker::Hand[K], H)
            || !GetComponentBone(MayaCargoWorker::Middle[K], M)
            || !GetComponentBone(MayaCargoWorker::Thumb[K], T)) return false;
        const FVector Finger = H.InverseTransformPosition(M.GetLocation());
        const FVector ThumbVector = H.InverseTransformPosition(T.GetLocation());
        PalmOffset[K] = Finger * 0.55;
        FingerAxis[K] = Finger.GetSafeNormal();
        PalmNormal[K] = FVector::CrossProduct(Finger, ThumbVector).GetSafeNormal();
        if (FingerAxis[K].IsNearlyZero() || PalmNormal[K].IsNearlyZero())
        { Fault = TEXT("Worker palm basis is degenerate."); return false; }
    }
    Fault.Empty(); bConfigured = true;
    SetActorTickInterval(0.1f);
    BaseAnimation->SetComponentTickInterval(0.1f);
    return true;
}

bool AMAYACargoWorker::BeginCargoService(UStaticMeshComponent* ParcelBody, UStaticMeshComponent* ParcelVisual, UPrimitiveComponent* Chassis,
    const FTransform& Source, const FTransform& Destination, bool bPickup)
{
    if (!bConfigured || !IsValid(ParcelBody) || !IsValid(ParcelVisual) || !ParcelVisual->GetStaticMesh() || !IsValid(Chassis)
        || Source.ContainsNaN() || Destination.ContainsNaN()) return false;
    FVector Direction = bPickup ? Destination.GetLocation() - Source.GetLocation() : Source.GetLocation() - Destination.GetLocation();
    Direction.Z = 0;
    if (!Direction.Normalize()) Direction = -Chassis->GetComponentTransform().GetUnitAxis(EAxis::Y);
    Facing = OriginalFacing = Direction;
    ServiceDestinationPosition = Destination.GetLocation();
    bFacingSelected = false; LastStanceSearchWorldSeconds = -1;
    StanceSelectionCount = TangentSweepCount = 0;
    SelectedStanceAngleDegrees = 0; bSelectedSourcePathClear = false;
    StanceSelectionDetail.Empty(); LastTangentSweepEvidence.Empty();
    Cargo = ParcelBody; CargoVisual = ParcelVisual; Carrier = Chassis;
    LastParcelPose = ParcelBody->GetComponentTransform();
    bServiceActive = true; bGrippingEnabled = bReleasing = bGripReady = false;
    SetActorTickInterval(0);
    BaseAnimation->SetComponentTickInterval(0);
    GripAlpha = 0; Fault.Empty();
    bApproachBlocked = false; ApproachBlockedSweepCount = 0;
    FirstApproachBlockWorldSeconds = LastApproachBlockWorldSeconds = -1;
    LastApproachBlockEvidence.Empty();
    SelectClearCargoFacing(LastParcelPose.GetLocation());
    if (!ReadGripTargets(LastParcelPose, GripTarget[0], GripTarget[1]))
    { Fault = TEXT("Actual imported cargo grip bounds are unavailable."); return false; }
    return true;
}

void AMAYACargoWorker::SetGrippingEnabled(bool bEnabled)
{
    bGrippingEnabled = bEnabled && bServiceActive;
    if (bEnabled) bReleasing = false;
}

bool AMAYACargoWorker::ReadGripTargets(const FTransform& ParcelPose, FVector& Left, FVector& Right) const
{
    if (!Cargo || !CargoVisual || !CargoVisual->GetStaticMesh() || ParcelPose.ContainsNaN()) return false;
    // Grip the actual imported visible box, including its non-centred origin,
    // uniform child scale and recenter offset. Physical admission still sweeps
    // the unchanged independent ParcelBody; this actor cannot change it.
    const FBox Local = CargoVisual->GetStaticMesh()->GetBounds().GetBox();
    const FTransform VisualPose = CargoVisual->GetComponentTransform().GetRelativeTransform(Cargo->GetComponentTransform()) * ParcelPose;
    if (!Local.IsValid || Local.Min.ContainsNaN() || Local.Max.ContainsNaN()
        || Local.GetExtent().GetMin() <= 0 || VisualPose.ContainsNaN() || VisualPose.GetScale3D().GetMin() <= 0) return false;
    const FVector Lateral = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
    // Intersect the worker's true lateral line with the actual visible box.
    // Fixed face centres put one palm farther forward under a diagonal approach
    // (game31: +6 cm), exhausting one original arm while the other still reaches.
    // InverseTransformVector includes scale; this also respects a tilted box.
    const FVector LocalLateral = VisualPose.InverseTransformVector(Lateral);
    if (LocalLateral.ContainsNaN() || LocalLateral.IsNearlyZero()) return false;
    double Distance = MAX_dbl;
    const FVector Extent = Local.GetExtent();
    for (int32 Axis = 0; Axis < 3; ++Axis)
        if (FMath::Abs(LocalLateral[Axis]) > 1.e-9)
            Distance = FMath::Min(Distance, Extent[Axis] / FMath::Abs(LocalLateral[Axis]));
    if (!FMath::IsFinite(Distance) || Distance <= 0 || Distance >= MAX_dbl) return false;
    Left = VisualPose.TransformPosition(Local.GetCenter() - LocalLateral * Distance);
    Right = VisualPose.TransformPosition(Local.GetCenter() + LocalLateral * Distance);
    return !Left.ContainsNaN() && !Right.ContainsNaN();
}

bool AMAYACargoWorker::ReadGround(const FVector& XY, double& OutHeight) const
{
    if (!GetWorld()) return false;
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MayaWorkerGround), false, this);
    if (Cargo) Q.AddIgnoredActor(Cargo->GetOwner());
    if (Carrier) Q.AddIgnoredActor(Carrier->GetOwner());
    Q.MobilityType = EQueryMobilityType::Static;
    FHitResult Hit;
    const FVector Start(XY.X, XY.Y, XY.Z + 250);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0, 0, 700), ECC_Visibility, Q)
        || !Hit.bBlockingHit || Hit.ImpactNormal.Z < 0.9 || !FMath::IsFinite(Hit.ImpactPoint.Z)) return false;
    OutHeight = Hit.ImpactPoint.Z;
    return true;
}

bool AMAYACargoWorker::ReadGroundedStance(const FVector& CargoPosition, const FVector& Direction, FVector& Out) const
{
    if (CargoPosition.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
    Out = CargoPosition - Direction * 65.0;
    double Ground = 0;
    if (!ReadGround(Out, Ground)) return false;
    Out.Z = Ground + Capsule->GetScaledCapsuleHalfHeight() + 0.5;
    return !Out.ContainsNaN() && FMath::Abs(Out.Z - GetActorLocation().Z) <= 22.0;
}

bool AMAYACargoWorker::IsCapsuleStanceClear(const FVector& Position, const FVector& Direction) const
{
    if (!GetWorld() || !Capsule || Position.ContainsNaN() || Direction.ContainsNaN()) return false;
    FComponentQueryParams Q(SCENE_QUERY_STAT(MayaWorkerStance), this);
    Q.bTraceComplex = false;
    TArray<FOverlapResult> Hits;
    Capsule->ComponentOverlapMulti(Hits, GetWorld(), Position, Direction.Rotation().Quaternion(),
        Capsule->GetCollisionObjectType(), Q);
    for (const FOverlapResult& Hit : Hits) if (Hit.bBlockingHit) return false;
    return true;
}

bool AMAYACargoWorker::SelectClearCargoFacing(const FVector& CargoPosition)
{
    if (!GetWorld() || !Capsule) return false;
    LastStanceSearchWorldSeconds = GetWorld()->GetTimeSeconds();
    // Keep the original 65 cm reach radius. Choose a real grounded, collision-
    // clear stance at BOTH ends rather than aiming the worker into a bollard.
    const double Angles[] = {0, 15, -15, 30, -30, 45, -45, 60, -60, 75, -75,
        90, -90, 120, -120, 150, -150, 180};
    double BestScore = MAX_dbl, BestAngle = 0;
    FVector BestFacing, BestSource, BestDestination;
    bool bFound = false, bBestPathClear = false;
    for (const double Angle : Angles)
    {
        const FVector Direction = OriginalFacing.RotateAngleAxis(Angle, FVector::UpVector).GetSafeNormal();
        FVector SourceStance, DestinationStance;
        if (!ReadGroundedStance(CargoPosition, Direction, SourceStance)
            || !ReadGroundedStance(ServiceDestinationPosition, Direction, DestinationStance)
            || !IsCapsuleStanceClear(SourceStance, Direction)
            || !IsCapsuleStanceClear(DestinationStance, Direction)) continue;
        FComponentQueryParams Q(SCENE_QUERY_STAT(MayaWorkerStancePath), this);
        Q.bTraceComplex = false;
        TArray<FHitResult> Hits;
        GetWorld()->ComponentSweepMulti(Hits, Capsule, GetActorLocation(), SourceStance, Direction.Rotation().Quaternion(), Q);
        bool bPathClear = true;
        for (const FHitResult& Hit : Hits) if (Hit.bBlockingHit || Hit.bStartPenetrating) { bPathClear = false; break; }
        const double Score = FVector::Dist2D(GetActorLocation(), SourceStance) + FMath::Abs(Angle) * 0.05;
        if (!bFound || (bPathClear && !bBestPathClear) || (bPathClear == bBestPathClear && Score < BestScore))
        {
            bFound = true; BestScore = Score; BestAngle = Angle; bBestPathClear = bPathClear;
            BestFacing = Direction; BestSource = SourceStance; BestDestination = DestinationStance;
        }
    }
    if (!bFound)
    {
        bFacingSelected = false; bApproachBlocked = true; bGripReady = false;
        StanceSelectionDetail = TEXT("No measured grounded collision-clear 65 cm source/destination stance; held under original service timeout.");
        return false;
    }
    Facing = BestFacing; bFacingSelected = true; bGripReady = false;
    SelectedStanceAngleDegrees = BestAngle; bSelectedSourcePathClear = bBestPathClear;
    SelectedSourceStance = BestSource; SelectedDestinationStance = BestDestination;
    ++StanceSelectionCount;
    StanceSelectionDetail = TEXT("Actual capsule overlap-clear at grounded source/destination; direct path query is recorded; every movement still sweeps actual capsule.");
    return true;
}

bool AMAYACargoWorker::MoveFeetToCargo(const FVector& CargoPosition, float Dt)
{
    if (!FMath::IsFinite(Dt) || Dt <= 0 || Dt > 0.25) return false;
    if (MovementFrame != GFrameCounter)
    {
        MovementFrame = GFrameCounter;
        MovementBudgetCm = 150.0 * Dt;
        FrameStartPosition = GetActorLocation();
    }
    FVector Wanted;
    const bool bCurrentGroundedStance = ReadGroundedStance(CargoPosition, Facing, Wanted);
    if (!bFacingSelected || !bCurrentGroundedStance || !IsCapsuleStanceClear(Wanted, Facing))
    {
        const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
        if (LastStanceSearchWorldSeconds < 0 || Now - LastStanceSearchWorldSeconds >= 0.25)
            SelectClearCargoFacing(CargoPosition);
        if (!bFacingSelected || !ReadGroundedStance(CargoPosition, Facing, Wanted)
            || !IsCapsuleStanceClear(Wanted, Facing))
        { bApproachBlocked = true; bGripReady = false; ActualWalkCmps = 0; return false; }
    }
    double Ground = 0;
    if (!ReadGround(Wanted, Ground))
    { Fault = TEXT("Worker has no measured walkable station surface."); return false; }
    Wanted.Z = Ground + Capsule->GetScaledCapsuleHalfHeight() + 0.5;
    const FVector Before = GetActorLocation();
    if (bServiceActive && FMath::Abs(Wanted.Z - Before.Z) > 22.0)
    { Fault = TEXT("Worker approach requires an unsupported step."); return false; }
    FVector Delta = Wanted - Before;
    Delta = Delta.GetClampedToMaxSize(MovementBudgetCm);
    FHitResult Hit;
    Capsule->MoveComponent(Delta, Facing.Rotation(), true, &Hit, MOVECOMP_NoFlags, ETeleportType::None);
    MovementBudgetCm = FMath::Max(0.0, MovementBudgetCm - FVector::Dist(Before, GetActorLocation()));
    ActualWalkCmps = FVector::Dist2D(FrameStartPosition, GetActorLocation()) / Dt;
    FootGroundCm = Ground;
    // A real blocker holds this swept approach; the existing service deadline
    // bounds retries. Never ignore a pedestrian or move the parcel through it.
    bApproachBlocked = Hit.bBlockingHit && Hit.Time < 0.99;
    if (bApproachBlocked)
    {
        bGripReady = false;
        ++ApproachBlockedSweepCount;
        LastApproachBlockWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
        if (FirstApproachBlockWorldSeconds < 0) FirstApproachBlockWorldSeconds = LastApproachBlockWorldSeconds;
        LastApproachBlockEvidence = FString::Printf(TEXT("actor=%s; component=%s; worldSeconds=%.9f; hitTime=%.9f; startPenetrating=%s; penetrationDepthCm=%.9f; actualPositionCm=%s; requestedDeltaCm=%s; impactNormal=%s"),
            *GetPathNameSafe(Hit.GetActor()), *GetPathNameSafe(Hit.GetComponent()), LastApproachBlockWorldSeconds, Hit.Time,
            Hit.bStartPenetrating ? TEXT("true") : TEXT("false"), Hit.PenetrationDepth,
            *GetActorLocation().ToString(), *Delta.ToString(), *Hit.ImpactNormal.ToString());
    }
    if (bApproachBlocked && !Hit.bStartPenetrating && MovementBudgetCm > 1.e-4
        && !Hit.ImpactNormal.ContainsNaN() && FMath::Abs(Hit.ImpactNormal.Z) < 0.25)
    {
        FVector Normal = Hit.ImpactNormal; Normal.Z = 0;
        if (Normal.Normalize())
        {
            // Route tangentially around the measured obstruction. A small
            // outward component separates contact; both legs are real sweeps
            // sharing the same 150 cm/s total frame path-length budget.
            const FVector Remaining = Wanted - GetActorLocation();
            FVector Tangent = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
            if (FVector::DotProduct(Tangent, Remaining) < 0) Tangent = -Tangent;
            FVector Slide = (Tangent + Normal * 0.15).GetSafeNormal() * MovementBudgetCm;
            const FVector SlideStart = GetActorLocation();
            FVector SlideEnd = SlideStart + Slide;
            double SlideGround = 0;
            if (ReadGround(SlideEnd, SlideGround)
                && FMath::Abs(SlideGround + Capsule->GetScaledCapsuleHalfHeight() + 0.5 - SlideStart.Z) <= 22.0)
            {
                SlideEnd.Z = SlideGround + Capsule->GetScaledCapsuleHalfHeight() + 0.5;
                Slide = (SlideEnd - SlideStart).GetClampedToMaxSize(MovementBudgetCm);
                // Resample after clamping so the actual projected endpoint, not
                // an unclamped proposal, has known supporting ground.
                SlideEnd = SlideStart + Slide;
                double ActualEndpointGround = 0;
                if (ReadGround(SlideEnd, ActualEndpointGround)
                    && FMath::Abs(SlideEnd.Z - ActualEndpointGround - Capsule->GetScaledCapsuleHalfHeight() - 0.5) <= 0.1)
                {
                    const double BeforeBudget = MovementBudgetCm;
                    FHitResult SlideHit;
                    Capsule->MoveComponent(Slide, Facing.Rotation(), true, &SlideHit, MOVECOMP_NoFlags, ETeleportType::None);
                    const double ActualSlideCm = FVector::Dist(SlideStart, GetActorLocation());
                    MovementBudgetCm = FMath::Max(0.0, MovementBudgetCm - ActualSlideCm);
                    ++TangentSweepCount;
                    LastTangentSweepEvidence = FString::Printf(TEXT("worldSeconds=%.9f; startCm=%s; proposedEndCm=%s; actualEndCm=%s; actualSlideCm=%.9f; remainingFrameBudgetBeforeCm=%.9f; remainingFrameBudgetAfterCm=%.9f; groundedEndpointZCm=%.9f; blockingHit=%s; startPenetrating=%s; hitComponent=%s"),
                        GetWorld()->GetTimeSeconds(), *SlideStart.ToString(), *SlideEnd.ToString(), *GetActorLocation().ToString(),
                        ActualSlideCm, BeforeBudget, MovementBudgetCm, ActualEndpointGround,
                        SlideHit.bBlockingHit ? TEXT("true") : TEXT("false"), SlideHit.bStartPenetrating ? TEXT("true") : TEXT("false"),
                        *GetPathNameSafe(SlideHit.GetComponent()));
                    bApproachBlocked = SlideHit.bBlockingHit && SlideHit.Time < 0.99;
                    ActualWalkCmps = FVector::Dist2D(FrameStartPosition, GetActorLocation()) / Dt;
                }
            }
        }
    }
    const bool bWalk = ActualWalkCmps > 2;
    if (bWalk != bPlayingWalk)
    {
        BaseAnimation->PlayAnimation(bWalk ? WalkAnimation : IdleAnimation, true);
        bPlayingWalk = bWalk;
    }
    return !bApproachBlocked;
}

void AMAYACargoWorker::RebuildComponentPose()
{
    const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Person->GetSkinnedAsset());
    if (!Mesh) { ComponentPose.Empty(); return; }
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    ComponentPose.SetNum(Person->BoneSpaceTransforms.Num());
    for (int32 I = 0; I < ComponentPose.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        ComponentPose[I] = Parent == INDEX_NONE ? Person->BoneSpaceTransforms[I]
            : Person->BoneSpaceTransforms[I] * ComponentPose[Parent];
    }
}

bool AMAYACargoWorker::GetComponentBone(FName Bone, FTransform& Pose) const
{
    const int32 I = Person->GetBoneIndex(Bone);
    if (!ComponentPose.IsValidIndex(I)) return false;
    Pose = ComponentPose[I];
    return !Pose.ContainsNaN();
}

bool AMAYACargoWorker::SetComponentBone(FName Bone, const FTransform& Pose)
{
    const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Person->GetSkinnedAsset());
    const int32 I = Person->GetBoneIndex(Bone);
    if (!Mesh || !ComponentPose.IsValidIndex(I) || Pose.ContainsNaN()) return false;
    const int32 Parent = Mesh->GetRefSkeleton().GetParentIndex(I);
    Person->BoneSpaceTransforms[I] = Parent == INDEX_NONE ? Pose : Pose.GetRelativeTransform(ComponentPose[Parent]);
    RebuildComponentPose();
    return true;
}

bool AMAYACargoWorker::SolveLimb(FName Upper, FName Lower, FName End, const FVector& TargetWorld, const FVector& PoleWorld)
{
    FTransform U, L, E;
    if (!GetComponentBone(Upper, U) || !GetComponentBone(Lower, L) || !GetComponentBone(End, E)) return false;
    const FTransform MeshWorld = Person->GetComponentTransform();
    const FVector Target = MeshWorld.InverseTransformPosition(TargetWorld);
    const FVector Pole = MeshWorld.InverseTransformPosition(PoleWorld);
    const FVector Root = U.GetLocation();
    const double A = FVector::Dist(Root, L.GetLocation()), B = FVector::Dist(L.GetLocation(), E.GetLocation());
    const double RawD = FVector::Dist(Root, Target);
    if (A <= 0.1 || B <= 0.1 || RawD <= 0.1 || !FMath::IsFinite(RawD)) return false;
    const double D = FMath::Clamp(RawD, FMath::Abs(A - B) + 0.01, A + B - 0.01);
    const FVector Direction = (Target - Root).GetSafeNormal();
    FVector Bend = Pole - Root;
    Bend = (Bend - Direction * FVector::DotProduct(Bend, Direction)).GetSafeNormal();
    if (Bend.IsNearlyZero()) Bend = FVector::CrossProduct(Direction, FVector::UpVector).GetSafeNormal();
    if (Bend.IsNearlyZero()) return false;
    const double Along = (A*A + D*D - B*B) / (2*D);
    const FVector Elbow = Root + Direction * Along + Bend * FMath::Sqrt(FMath::Max(0.0, A*A - Along*Along));
    U.SetRotation((FQuat::FindBetweenVectors(L.GetLocation() - Root, Elbow - Root) * U.GetRotation()).GetNormalized());
    if (!SetComponentBone(Upper, U) || !GetComponentBone(Lower, L) || !GetComponentBone(End, E)) return false;
    L.SetRotation((FQuat::FindBetweenVectors(E.GetLocation() - L.GetLocation(), Target - L.GetLocation()) * L.GetRotation()).GetNormalized());
    return SetComponentBone(Lower, L) && RawD <= A + B + 0.5;
}

FVector AMAYACargoWorker::PalmWorld(bool bLeft) const
{
    const int32 K = bLeft ? 0 : 1;
    FTransform H;
    return GetComponentBone(MayaCargoWorker::Hand[K], H)
        ? Person->GetComponentTransform().TransformPosition(H.TransformPosition(PalmOffset[K])) : FVector(NAN, NAN, NAN);
}

bool AMAYACargoWorker::PoseForCargo(const FTransform& CargoPose, float Dt)
{
    if (!ReadGripTargets(CargoPose, GripTarget[0], GripTarget[1]))
    { Fault = TEXT("Actual cargo collision bounds are unavailable for the worker grip."); return false; }
    Person->CopyPoseFromSkeletalComponent(BaseAnimation);
    RebuildComponentPose();
    if (ComponentPose.IsEmpty()) return false;
    if (AlphaFrame != GFrameCounter)
    {
        AlphaFrame = GFrameCounter;
        GripAlpha = FMath::FInterpConstantTo(GripAlpha, bGrippingEnabled && !bReleasing ? 1.0f : 0.0f, Dt, 2.0f);
    }
    const FTransform MeshWorld = Person->GetComponentTransform();
    FVector FootTargets[2];
    FTransform U[2], Pelvis, Spine;
    for (int32 K = 0; K < 2; ++K)
    {
        FTransform F;
        if (!GetComponentBone(MayaCargoWorker::Foot[K], F) || !GetComponentBone(MayaCargoWorker::UpperArm[K], U[K])) return false;
        FootTargets[K] = MeshWorld.TransformPosition(F.GetLocation());
    }
    if (!GetComponentBone(TEXT("pelvis"), Pelvis) || !GetComponentBone(TEXT("spine_01"), Spine)) return false;
    const double ShouldersZ = (MeshWorld.TransformPosition(U[0].GetLocation()).Z + MeshWorld.TransformPosition(U[1].GetLocation()).Z) * 0.5;
    const double HandsZ = (GripTarget[0].Z + GripTarget[1].Z) * 0.5;
    const double Drop = FMath::Clamp(ShouldersZ - HandsZ - 35.0, 0.0, 75.0) * GripAlpha;
    FVector P = MeshWorld.TransformPosition(Pelvis.GetLocation());
    P.Z -= Drop;
    Pelvis.SetLocation(MeshWorld.InverseTransformPosition(P));
    if (!SetComponentBone(TEXT("pelvis"), Pelvis) || !GetComponentBone(TEXT("spine_01"), Spine)) return false;
    const FVector LocalRight = MeshWorld.InverseTransformVectorNoScale(FVector::CrossProduct(FVector::UpVector, Facing)).GetSafeNormal();
    // Even at the raised loading waypoint the worker remains outside the robot
    // capsule envelope. A measured forward reach therefore needs torso lean,
    // rather than stretching the original 55 cm arm chain or moving through it.
    const double Lean = FMath::DegreesToRadians(FMath::Clamp(35.0 + Drop / 3.0, 35.0, 60.0) * GripAlpha);
    Spine.SetRotation((FQuat(LocalRight, Lean) * Spine.GetRotation()).GetNormalized());
    if (!SetComponentBone(TEXT("spine_01"), Spine)) return false;
    for (int32 K = 0; K < 2; ++K)
        if (!SolveLimb(MayaCargoWorker::Thigh[K], MayaCargoWorker::Calf[K], MayaCargoWorker::Foot[K],
            FootTargets[K], FootTargets[K] + Facing * 80)) return false;
    const FVector Lateral = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
    bool bArmsReach = true;
    for (int32 K = 0; K < 2; ++K)
    {
        FTransform H;
        if (!GetComponentBone(MayaCargoWorker::Hand[K], H)) return false;
        const FVector IdlePalm = PalmWorld(K == 0);
        const FVector PalmTarget = FMath::Lerp(IdlePalm, GripTarget[K], double(GripAlpha));
        const FQuat Basis = FRotationMatrix::MakeFromXY(FingerAxis[K], PalmNormal[K]).ToQuat();
        const FQuat Desired = FRotationMatrix::MakeFromXY(-FVector::UpVector, K == 0 ? Lateral : -Lateral).ToQuat() * Basis.Inverse();
        const FQuat HandWorld = FQuat::Slerp((H * MeshWorld).GetRotation(), Desired, GripAlpha).GetNormalized();
        const FVector Wrist = PalmTarget - HandWorld.RotateVector(PalmOffset[K]);
        bArmsReach &= SolveLimb(MayaCargoWorker::UpperArm[K], MayaCargoWorker::LowerArm[K], MayaCargoWorker::Hand[K],
            Wrist, GetActorLocation() + Lateral * (K == 0 ? -75 : 75) - Facing * 30);
        if (!GetComponentBone(MayaCargoWorker::Hand[K], H)) return false;
        H.SetRotation((MeshWorld.GetRotation().Inverse() * HandWorld).GetNormalized());
        if (!SetComponentBone(MayaCargoWorker::Hand[K], H)) return false;
        GripErrorCm[K] = FVector::Dist(PalmWorld(K == 0), GripTarget[K]);
    }
    Person->MarkRefreshTransformDirty();
    Person->RefreshBoneTransforms();
    bGripReady = bArmsReach && GripAlpha >= 0.999f && GripErrorCm[0] <= 2 && GripErrorCm[1] <= 2
        && FVector::Dist2D(GetActorLocation(), CargoPose.GetLocation() - Facing * 65.0) <= 2;
    LastParcelPose = CargoPose;
    LastWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
    return Fault.IsEmpty();
}

bool AMAYACargoWorker::PrepareParcelMove(const FTransform& ProposedPose, float Dt)
{
    if (!bServiceActive || !bGrippingEnabled || bReleasing || !Fault.IsEmpty() || ProposedPose.ContainsNaN()) return false;
    return MoveFeetToCargo(ProposedPose.GetLocation(), Dt) && PoseForCargo(ProposedPose, Dt) && bGripReady;
}

void AMAYACargoWorker::Tick(float Dt)
{
    Super::Tick(Dt);
    if (!bConfigured || Dt <= 0 || Dt > 0.25) return;
    if (!bServiceActive)
    {
        Person->CopyPoseFromSkeletalComponent(BaseAnimation);
        Person->MarkRefreshTransformDirty();
        Person->RefreshBoneTransforms();
        RebuildComponentPose();
        return;
    }
    if (!Cargo) return;
    const FTransform ActualCargoPose = Cargo->GetComponentTransform();
    // A collision refusal must still allow actual hands to withdraw. The fault
    // stays recorded, feet stay put, and closure uses measured arm clearance.
    if (bReleasing) { PoseForCargo(ActualCargoPose, Dt); return; }
    if (!Fault.IsEmpty()) return;
    if (MoveFeetToCargo(ActualCargoPose.GetLocation(), Dt)) PoseForCargo(ActualCargoPose, Dt);
}

void AMAYACargoWorker::BeginRelease()
{
    bReleasing = true;
    bGrippingEnabled = bGripReady = false;
}

bool AMAYACargoWorker::ObserveArmClearance() const
{
    ArmMinimumClearanceCm = MAX_dbl;
    ArmClearanceDetail.Empty();
    if (!Carrier || !Carrier->GetOwner()) return false;
    TArray<UPrimitiveComponent*> RobotParts;
    Carrier->GetOwner()->GetComponents<UPrimitiveComponent>(RobotParts);
    for (int32 K = 0; K < 2; ++K)
    {
        FTransform U, L, H;
        if (!GetComponentBone(MayaCargoWorker::UpperArm[K], U) || !GetComponentBone(MayaCargoWorker::LowerArm[K], L)
            || !GetComponentBone(MayaCargoWorker::Hand[K], H)) return false;
        const FVector Points[] = {Person->GetComponentTransform().TransformPosition(U.GetLocation()),
            Person->GetComponentTransform().TransformPosition(L.GetLocation()), PalmWorld(K == 0)};
        for (int32 Segment = 0; Segment < 2; ++Segment)
        {
            const int32 Samples = FMath::Max(1, FMath::CeilToInt(FVector::Dist(Points[Segment], Points[Segment+1]) / 2.0));
            for (int32 S = 0; S <= Samples; ++S)
                for (UPrimitiveComponent* Part : RobotParts)
                    if (Part && Part->GetCollisionEnabled() != ECollisionEnabled::NoCollision
                        && Part->GetBodyInstance() && Part->GetBodyInstance()->IsValidBodyInstance())
                    {
                        FVector Closest;
                        const float Distance = Part->GetClosestPointOnCollision(FMath::Lerp(Points[Segment], Points[Segment+1], double(S)/Samples), Closest);
                        if (FMath::IsFinite(Distance) && Distance >= 0) ArmMinimumClearanceCm = FMath::Min(ArmMinimumClearanceCm, double(Distance));
                        if (!FMath::IsFinite(Distance) || Distance < 0 || Distance <= 3.0f)
                        {
                            ArmClearanceDetail = FString::Printf(TEXT("part=%s; arm=%d; segment=%d; sample=%d/%d; actualClosestDistanceCm=%.9f"),
                                *Part->GetPathName(), K, Segment, S, Samples, Distance);
                            return false;
                        }
                    }
        }
    }
    return ArmMinimumClearanceCm < MAX_dbl;
}

bool AMAYACargoWorker::IsClearForLidClosure() const
{
    return bServiceActive && bReleasing && GripAlpha <= 0.001f && ObserveArmClearance();
}

void AMAYACargoWorker::CancelService()
{
    if (!Fault.IsEmpty()) LastServiceFault = Fault;
    // Caller uses this only after observed commit/rollback, explicit recovery,
    // or EndPlay. It is never a substitute for release/arm-clear observations.
    Fault.Empty();
    bServiceActive = bGrippingEnabled = bReleasing = bGripReady = false;
    bApproachBlocked = false;
    Cargo = nullptr; CargoVisual = nullptr; Carrier = nullptr; GripAlpha = 0;
    if (IdleAnimation) BaseAnimation->PlayAnimation(IdleAnimation, true);
    bPlayingWalk = false;
    SetActorTickInterval(0.1f);
    BaseAnimation->SetComponentTickInterval(0.1f);
    Person->CopyPoseFromSkeletalComponent(BaseAnimation);
    Person->MarkRefreshTransformDirty();
    RebuildComponentPose();
}

FString AMAYACargoWorker::ReadWorkerEvidenceJson() const
{
    const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeCargoWorker.v1"));
    J->SetStringField(TEXT("scope"), TEXT("Native skeletal animation and geometric limb IK; authoritative swept cargo and custody remain in Director. No ragdoll-force claim."));
    J->SetStringField(TEXT("mesh"), GetPathNameSafe(Person->GetSkinnedAsset()));
    J->SetStringField(TEXT("baseAnimation"), GetPathNameSafe(bPlayingWalk ? WalkAnimation.Get() : IdleAnimation.Get()));
    J->SetStringField(TEXT("cargoVisualMesh"), GetPathNameSafe(CargoVisual ? CargoVisual->GetStaticMesh() : nullptr));
    J->SetBoolField(TEXT("configured"), bConfigured);
    J->SetBoolField(TEXT("serviceActive"), bServiceActive);
    J->SetBoolField(TEXT("gripReady"), bGripReady);
    J->SetBoolField(TEXT("releaseClear"), IsClearForLidClosure());
    if (FMath::IsFinite(ArmMinimumClearanceCm) && ArmMinimumClearanceCm >= 0 && ArmMinimumClearanceCm < MAX_dbl)
        J->SetNumberField(TEXT("armMinimumClearanceCm"), ArmMinimumClearanceCm);
    J->SetStringField(TEXT("armClearanceDetail"), ArmClearanceDetail);
    J->SetStringField(TEXT("fault"), Fault);
    J->SetStringField(TEXT("lastServiceFault"), LastServiceFault);
    J->SetBoolField(TEXT("approachBlocked"), bApproachBlocked);
    J->SetStringField(TEXT("approachBlockedSweepCount"), LexToString(ApproachBlockedSweepCount));
    J->SetNumberField(TEXT("firstApproachBlockWorldSeconds"), FirstApproachBlockWorldSeconds);
    J->SetNumberField(TEXT("lastApproachBlockWorldSeconds"), LastApproachBlockWorldSeconds);
    J->SetStringField(TEXT("lastApproachBlockEvidence"), LastApproachBlockEvidence);
    J->SetStringField(TEXT("approachRetryBound"), TEXT("Original Director/Core mechanical service deadline; actual capsule sweeps remain blocking"));
    J->SetBoolField(TEXT("collisionClearFacingSelected"), bFacingSelected);
    J->SetNumberField(TEXT("selectedStanceAngleDegrees"), SelectedStanceAngleDegrees);
    J->SetStringField(TEXT("stanceSelectionCount"), LexToString(StanceSelectionCount));
    J->SetBoolField(TEXT("selectedSourceDirectPathClear"), bSelectedSourcePathClear);
    J->SetArrayField(TEXT("selectedSourceStanceWorldCm"), MayaCargoWorker::VectorJson(SelectedSourceStance));
    J->SetArrayField(TEXT("selectedDestinationStanceWorldCm"), MayaCargoWorker::VectorJson(SelectedDestinationStance));
    J->SetStringField(TEXT("stanceSelectionDetail"), StanceSelectionDetail);
    J->SetStringField(TEXT("tangentSweepCount"), LexToString(TangentSweepCount));
    J->SetStringField(TEXT("lastTangentSweepEvidence"), LastTangentSweepEvidence);
    J->SetNumberField(TEXT("worldSeconds"), LastWorldSeconds);
    J->SetNumberField(TEXT("walkCmps"), ActualWalkCmps);
    J->SetNumberField(TEXT("groundHeightCm"), FootGroundCm);
    J->SetNumberField(TEXT("gripAlpha"), GripAlpha);
    J->SetArrayField(TEXT("capsuleWorldCm"), MayaCargoWorker::VectorJson(GetActorLocation()));
    for (int32 K = 0; K < 2; ++K)
    {
        const FString Prefix = K == 0 ? TEXT("left") : TEXT("right");
        const FVector Palm = PalmWorld(K == 0);
        if (!Palm.ContainsNaN()) J->SetArrayField(Prefix + TEXT("PalmWorldCm"), MayaCargoWorker::VectorJson(Palm));
        J->SetArrayField(Prefix + TEXT("GripTargetWorldCm"), MayaCargoWorker::VectorJson(GripTarget[K]));
        J->SetNumberField(Prefix + TEXT("GripErrorCm"), GripErrorCm[K]);
    }
    FString Out;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    FJsonSerializer::Serialize(J, Writer);
    return Out;
}
