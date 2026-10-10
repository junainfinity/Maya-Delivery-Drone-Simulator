#include "MAYANativeDemoPawn.h"
#include "MAYADeliveryBoostProfile.h"
#include "MAYAWheelFeedbackCallback.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"
#include "MAYALatchedDoorDrive.h"
#include "MAYAPlayerDriveInput.h"
#include "MAYAWheelAppearanceEvidence.h"
#include "DDSRobotPhysicsTypes.h"
#include "DDSRobotJointAdmission.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputCoreTypes.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/PhysicsFiltering.h"
#include "Chaos/PBDJointConstraintData.h"
#include "Chaos/Convex.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "MAYAStableCameraArm.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/OverlapResult.h"
#include "UnrealClient.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Kismet/GameplayStatics.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
// Reviewed candidate02 mesh-local centimetre bounds. A unit mistake must fail
// before simulation; bounds are an identity guard, not a full clearance proof.
struct FMayaNativeV02ReviewedBounds { FVector Min, Max; };
constexpr double MayaNativeV02BoundsToleranceCm = 0.02;
const FMayaNativeV02ReviewedBounds MayaNativeV02HullBounds[] = {
    {FVector(3.000000000000, -19.634446773214, -0.873544568028), FVector(7.000000000000, 19.286212448902, 10.832958145356)},
    {FVector(7.000000000000, 16.000000000000, 0.004139731164), FVector(31.135555587845, 19.255601846952, 11.379471778870)},
    {FVector(7.000000000000, -19.606116309256, 1.642072889368), FVector(31.683368682861, -16.000000000000, 10.932357844497)},
    {FVector(7.000000000000, -16.000000000000, -0.868206156733), FVector(31.630316498752, 16.000000000000, 11.342272720981)},
    {FVector(-30.907052993774, -19.872903823853, 4.773725287354), FVector(-30.000000000000, 19.266560498860, 9.893992429267)},
    {FVector(-30.000000000000, 16.000000000000, -0.912867963314), FVector(3.000000000000, 19.534330368042, 10.731448338211)},
    {FVector(-30.000000000000, -19.868173100866, 0.609265865533), FVector(3.000000000000, -16.000000000000, 10.223008464588)},
    {FVector(-30.000000000000, -16.000000000000, -0.910808079160), FVector(3.000000000000, 16.000000000000, 10.690036710270)},
    {FVector(-38.444015502930, -21.298866271973, 9.393125534058), FVector(-30.000000000000, 21.087197203809, 46.590686798096)},
    {FVector(-30.000000000000, 16.000000000000, 10.021857423271), FVector(-19.825421168201, 21.200876235962, 44.635532760791)},
    {FVector(-30.000000000000, -21.173229149379, 9.466246339037), FVector(-19.745189666748, -16.000000000000, 45.041317056925)},
    {FVector(-30.000000000000, -16.000000000000, 9.489547778392), FVector(-19.752105655470, 16.000000000000, 18.000000000000)},
    {FVector(37.000000000000, -0.478434576814, 15.109525066325), FVector(42.686164855957, 20.006161433053, 45.457663193196)},
    {FVector(3.000000000000, -0.620820343494, 11.850488407364), FVector(7.000000000000, 21.351071889442, 47.671287536621)},
    {FVector(7.000000000000, 16.000000000000, 12.318615231447), FVector(37.000000000000, 21.371736526489, 47.607825072611)},
    {FVector(7.000000000000, -0.321519641538, 11.916587039208), FVector(37.000000000000, 16.000000000000, 21.000000000000)},
    {FVector(-20.336767864110, 16.000000000000, 12.003266057010), FVector(3.000000000000, 21.346123158857, 46.612218349463)},
    {FVector(-21.058486938477, -0.396429449914, 11.519019126892), FVector(3.000000000000, 16.000000000000, 18.000000000000)},
    {FVector(37.000000000000, -19.960349539137, 15.041857676916), FVector(42.460655212402, -0.452377078835, 46.369549087696)},
    {FVector(3.000000000000, -21.522330252988, 10.621866846454), FVector(7.000000000000, 0.552292416513, 47.395114898682)},
    {FVector(7.000000000000, -21.695142745972, 11.281397677779), FVector(37.000000000000, -16.000000000000, 46.042403046913)},
    {FVector(7.000000000000, -16.000000000000, 10.760167204041), FVector(37.000000000000, 0.556030690670, 21.000000000000)},
    {FVector(-20.602269930852, -21.456030040797, 9.843797683716), FVector(3.000000000000, -16.000000000000, 45.043402966678)},
    {FVector(-21.087512969971, -16.000000000000, 10.113600463353), FVector(3.000000000000, 0.551647439554, 18.000000000000)},
    {FVector(-0.350398898125, -15.353761672974, -1.095634222031), FVector(37.398575922600, -0.169222553615, 2.647027492523)},
    {FVector(-0.307752698660, -0.224731683731, -1.098362207413), FVector(37.398575922600, 15.298577513411, 2.647201538086)},
    {FVector(-0.859157562256, -22.008450516239, -8.542351722717), FVector(37.398575922600, -15.297856232151, 2.180490732193)},
    {FVector(-0.834579467773, 15.242836952209, -8.544988632202), FVector(37.398575922600, 21.803583145142, 2.392816055849)},
    {FVector(-33.098739653424, -14.290699005127, -1.101572990417), FVector(0.362094879150, 14.892377751645, 2.900887489319)},
    {FVector(-33.098739653424, 14.846691131592, -8.227730751038), FVector(0.216896057129, 21.993454209325, 2.714013261711)},
    {FVector(-33.094386053328, -22.016143798828, -8.229973793030), FVector(0.045182120055, -17.526096343994, -0.538265907431)},
    {FVector(-33.098739653424, -20.403919219971, -3.040364027023), FVector(0.298937320709, -14.247160659653, 2.753138065338)},
};
const FMayaNativeV02ReviewedBounds MayaNativeV02RenderBounds[] = {
    {FVector(-38.444015502930, -21.695142745972, -0.912867963314), FVector(42.686164855957, 21.371736526489, 47.671287536621)},
    {FVector(-0.859157562256, -22.008450516239, -8.544988632202), FVector(37.398575922600, 21.803583145142, 2.647201538086)},
    {FVector(-33.098739653424, -22.016143798828, -8.229973793030), FVector(0.362094879150, 21.993454209325, 2.900887489319)},
    {FVector(-0.634711887687, -21.854452788830, -8.286789059639), FVector(37.398575922600, 21.854494512081, 2.980282902718)},
    {FVector(-33.098739653424, -21.854270994663, -8.103510737419), FVector(0.037304518628, 21.854104101658, 3.269353136420)},
};
bool MayaNativeV02BoundsMatch(const FBox& Actual, const FMayaNativeV02ReviewedBounds& Expected)
{
    return Actual.IsValid && !Actual.Min.ContainsNaN() && !Actual.Max.ContainsNaN()
        && Actual.Min.Equals(Expected.Min, MayaNativeV02BoundsToleranceCm)
        && Actual.Max.Equals(Expected.Max, MayaNativeV02BoundsToleranceCm);
}
bool MayaNativeV02ConvexBoundsMatch(const UBodySetup& Setup, int32 Start, int32 Count)
{
    TArray<bool> Matched; Matched.Init(false, Count);
    for (const auto& Elem : Setup.AggGeom.ConvexElems)
    {
        const auto& Cooked = Elem.GetChaosConvexMesh();
        if (!Cooked.IsValid()) return false;
        FBox RawBounds(ForceInit), CookedBounds(ForceInit);
        for (const FVector& Vertex : Elem.VertexData) RawBounds += Elem.GetTransform().TransformPosition(Vertex);
        for (int32 V = 0; V < Cooked->NumVertices(); ++V)
            CookedBounds += Elem.GetTransform().TransformPosition(FVector(Cooked->GetVertex(V)));
        bool Found = false;
        for (int32 K = 0; K < Count; ++K)
            if (!Matched[K] && MayaNativeV02BoundsMatch(RawBounds, MayaNativeV02HullBounds[Start + K])
                && MayaNativeV02BoundsMatch(CookedBounds, MayaNativeV02HullBounds[Start + K]))
            { Matched[K] = true; Found = true; break; }
        if (!Found) return false;
    }
    return !Matched.Contains(false);
}

struct FSourcePart { const TCHAR* Name; const TCHAR* MeshName; FTransform Rest; };
const FSourcePart SourceParts[] = {
    { TEXT("SM_DeliveryRobot_Base"), TEXT("SM_DeliveryRobot_Base"), FTransform(FQuat(0, -0, 0, 1), FVector(0, 0, 0)) },
    { TEXT("SM_DeliveryRobot_Wheel"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(0, -0, 0, 1), FVector(1.9457469857542264E-08, 21.604815404099558, -3.423074207891677)) },
    { TEXT("SM_DeliveryRobot_Wheel1"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(-0, 0.49999999648741195, 0, 0.8660254058124321), FVector(-29.78075496739769, 21.60481500000006, -3.275914598335127)) },
    { TEXT("SM_DeliveryRobot_Wheel2"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(0, -0.49999999648741306, 0, 0.8660254058124315), FVector(31.29601019673717, 21.604814999999945, -3.2759147501829475)) },
    { TEXT("SM_DeliveryRobot_Wheel3"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(0.4999999929748159, -1.2167963784365349E-08, -0.86602540784043, -8.434748381212096E-16), FVector(31.296010433225398, -21.428732119490352, -3.2759146239657713)) },
    { TEXT("SM_DeliveryRobot_Wheel4"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(-0, 0, -0.9999999999999999, 1.2167958257817344E-08), FVector(5.450919624684248E-08, -21.42873211328738, -3.4230751670366493)) },
    { TEXT("SM_DeliveryRobot_Wheel5"), TEXT("SM_DeliveryRobot_Wheel"), FTransform(FQuat(0.499999992974824, 1.2167966554983274E-08, 0.8660254078404255, -1.6739070579575138E-15), FVector(-29.780754758799628, -21.373720625239333, -3.275914592641863)) },
    { TEXT("SM_DeliveryRobot_Door1"), TEXT("SM_DeliveryRobot_Door1"), FTransform(FQuat(0, -0, 0, 1), FVector(-31.999999998514145, -3.552713678800501E-15, 45.61109274129914)) },
    { TEXT("SM_DeliveryRobot_Door2"), TEXT("SM_DeliveryRobot_Door2"), FTransform(FQuat(0, -0, 0, 1), FVector(39.18231853264595, 0, 45.322025493174316)) },
    { TEXT("SM_DeliveryRobot_Suspension"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(0, -0, 0, 1), FVector(-1.4073424381422228E-07, 19.36495730237998, -3.1287535072102157)) },
    { TEXT("SM_DeliveryRobot_Suspension1"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(-0, 0.49999999648741195, 0, 0.8660254058124321), FVector(-29.525865505724212, 19.364957000000057, -3.1287540962674703)) },
    { TEXT("SM_DeliveryRobot_Suspension2"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(0, -0.49999999648741306, 0, 0.8660254058124315), FVector(31.041120735063693, 19.364956999999944, -3.1287542481152917)) },
    { TEXT("SM_DeliveryRobot_Suspension3"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(0.4999999929748159, -1.2167963784365349E-08, -0.86602540784043, -8.434748381212096E-16), FVector(31.0411210000002, -19.188874113287383, -3.128754167036649)) },
    { TEXT("SM_DeliveryRobot_Suspension4"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(0.499999992974824, 1.2167966554983274E-08, 0.8660254078404255, -1.6739070579575138E-15), FVector(-29.525865325574436, -19.133862619036364, -3.1287541357127546)) },
    { TEXT("SM_DeliveryRobot_Suspension5"), TEXT("SM_DeliveryRobot_Suspension"), FTransform(FQuat(-0, 0, -0.9999999999999999, 1.2167958257817344E-08), FVector(1.9895196601282805E-13, -19.188874113287383, -3.128754167036649)) },
};
static_assert(UE_ARRAY_COUNT(SourceParts) == 15, "Exact accepted source roster");
constexpr double NmToUE = 10000.0;
constexpr double InputMaxAgeS = 0.25;
constexpr double ContactMaxAgeS = 0.25;
constexpr double MinContactImpulseKgCmps = 0.01;
constexpr double MinContactUpDot = 0.7;
constexpr double StableSupportS = 0.5;
constexpr double StopSpeedCmps = 5.0;
constexpr double StopOmegaRadps = 0.05;
constexpr double SleepingAnchorCm = 0.2;
constexpr double DoorTimeoutS = 5.0;
constexpr double ClosedAngleRad = MAYALatchedDoorDrive::ClosedAngleRad;
constexpr double ClosedOmegaRadps = MAYALatchedDoorDrive::ClosedOmegaRadps;
constexpr double OpenAngleToleranceRad = 2.0 * PI / 180.0;
constexpr double OpenOmegaRadps = 0.15;
constexpr double ChassisCompoundKg = 37.841;
constexpr double SuspensionContributionKg = 0.1;
constexpr double BaseContributionKg = ChassisCompoundKg - 6 * SuspensionContributionKg;
constexpr double AggregateMassRelativeTolerance = 0.001;

// Registered front navigation lens: combined centreZ31.6cm transformed by
// accepted M01 registration Z-13.6586230296cm. X comes from the actual owned
// cosmetic envelope, not a guessed source-shell dimension. Original meshes
// and their component frames are observations only.
constexpr double MayaPovOpticalEyeHeightCm = 17.9413769704;
constexpr double MayaPovFrontClearanceCm = 3.0;
constexpr float MayaPovNearClipCm = 2.f;
constexpr double MayaPovChassisMotionFraction = 0.03;
constexpr double MayaPovResidualFollowRate = 6.0;
constexpr double MayaPovVerticalCapCm = 0.2;
constexpr float MayaPovAngularCapDegrees = 0.25f;

// Existing pure motor/brake law, with explicitly declared GAME hypotheses.
FDDSRobotPhysicsProfile MotorProfile()
{
    FDDSRobotPhysicsProfile P;
    P.MotorPeakNm = 5.0; P.BrakePeakNm = 8.0; P.BearingNmPerRadps = 0.02;
    P.MotorNoLoadRadps = 20.0; P.MotorErrorNmPerRadps = 0.5;
    P.BrakeRegularizationRadps = 0.1;
    P.SuspensionK_Npm = 2500.0; P.SuspensionC_Nspm = 180.0; P.SuspensionForceLimitN = 500.0;
    return P;
}
// Active brake's original near-zero slope; native solver evaluates current velocity.
// Nm/(rad/s) -> kg*cm^2/s. Passive bearing remains separate and unchanged.
double NativeBrakeDampingUE()
{
    const auto P = MotorProfile();
    return (P.BrakePeakNm / P.BrakeRegularizationRadps) * NmToUE;
}
bool WheelBrakeSettingsMatch(const Chaos::FPBDJointSettings& S, bool Enabled)
{
    return S.AngularMotionTypes[0] == Chaos::EJointMotionType::Free
        && S.AngularDriveForceMode == Chaos::EJointForceMode::Force
        && S.bAngularTwistVelocityDriveEnabled == Enabled
        && !S.bAngularSwingVelocityDriveEnabled && !S.bAngularSLerpVelocityDriveEnabled
        && !S.bAngularTwistPositionDriveEnabled && !S.bAngularSwingPositionDriveEnabled
        && !S.bAngularSLerpPositionDriveEnabled
        && FVector(S.AngularDriveVelocityTarget).Equals(FVector::ZeroVector, 1.e-6)
        && FVector(S.AngularDriveStiffness).Equals(FVector::ZeroVector, 1.e-6)
        && FVector(S.AngularDriveDamping).Equals(FVector(NativeBrakeDampingUE(), 0, 0), 1.e-5)
        && FVector(S.AngularDriveMaxTorque).Equals(FVector(MotorProfile().BrakePeakNm * NmToUE, 0, 0), 1.e-5);
}
double MayaNativeTwist(const FQuat& A, const FQuat& B)
{
    FQuat Q = A.Inverse() * B; Q.Normalize();
    return FMath::UnwindRadians(2.0 * FMath::Atan2(Q.X, Q.W));
}
void TorquePair(UStaticMeshComponent* A, UStaticMeshComponent* B, const FVector& TorqueNm)
{
    if (TorqueNm.IsNearlyZero(1.e-12)) return;
    A->WakeAllRigidBodies(); B->WakeAllRigidBodies();
    A->AddTorqueInRadians(TorqueNm * NmToUE, NAME_None, false);
    B->AddTorqueInRadians(-TorqueNm * NmToUE, NAME_None, false);
}
TSharedRef<FJsonObject> Pose(const FTransform& T)
{
    auto O = MakeShared<FJsonObject>();
    const FVector P = T.GetLocation(); const FQuat Q = T.GetRotation();
    O->SetNumberField(TEXT("xCm"), P.X); O->SetNumberField(TEXT("yCm"), P.Y); O->SetNumberField(TEXT("zCm"), P.Z);
    O->SetNumberField(TEXT("qx"), Q.X); O->SetNumberField(TEXT("qy"), Q.Y); O->SetNumberField(TEXT("qz"), Q.Z); O->SetNumberField(TEXT("qw"), Q.W);
    return O;
}
}

AMAYANativeDemoPawn::AMAYANativeDemoPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    PrimaryActorTick.bTickEvenWhenPaused = true;
    AutoPossessPlayer = EAutoReceiveInput::Player0;
    for (int32 I = 0; I < 15; ++I)
    {
        auto* Part = CreateDefaultSubobject<UStaticMeshComponent>(FName(SourceParts[I].Name));
        Part->SetMobility(EComponentMobility::Movable);
        Part->BodyInstance.bAutoWeld = false;
        Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        const FString Path = FString(TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/"))
            + SourceParts[I].MeshName + TEXT(".") + SourceParts[I].MeshName;
        Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *Path));
        Parts.Add(Part); RestInChassis[I] = SourceParts[I].Rest;
        if (I == 0) RootComponent = Part;
        else
        {
            Part->SetupAttachment(Parts[0]);
            Part->SetRelativeTransform(RestInChassis[I]);
        }
    }
    // Restore the source attachment hierarchy before physical simulation detaches wheels.
    const int32 WheelSuspension[] = { 9, 10, 11, 12, 14, 13 };
    for (int32 W = 0; W < 6; ++W)
    {
        const int32 S = WheelSuspension[W];
        Parts[W + 1]->SetupAttachment(Parts[S]);
        Parts[W + 1]->SetRelativeTransform(RestInChassis[W + 1].GetRelativeTransform(RestInChassis[S]));
    }
    CameraArm = CreateDefaultSubobject<UMAYAStableCameraArm>(TEXT("MAYADemoCameraArm"));
    CameraArm->SetupAttachment(RootComponent);
    CameraArm->SetViewAngles(-20, 0);
    CameraArm->AddTickPrerequisiteActor(this);
    CameraArm->TargetArmLength = 250;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("MAYADemoCamera"));
    Camera->SetupAttachment(CameraArm); Camera->FieldOfView = 75;
    Camera->PostProcessSettings.bOverride_MotionBlurAmount = true;
    Camera->PostProcessSettings.MotionBlurAmount = 0;
    CargoMount = CreateDefaultSubobject<UBoxComponent>(TEXT("MAYADeliveryCargoMount"));
    CargoMount->SetupAttachment(RootComponent);
    CargoMount->SetRelativeLocation(FVector(0, 0, 27));
    CargoMount->SetBoxExtent(FVector(12, 9, 6));
    CargoMount->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CargoMount->SetGenerateOverlapEvents(false);
    RearCargoMount = CreateDefaultSubobject<UBoxComponent>(TEXT("MAYADeliveryRearCargoMount"));
    RearCargoMount->SetupAttachment(RootComponent);
    RearCargoMount->SetRelativeLocation(FVector(19.5, 0, 27.5));
    RearCargoMount->SetBoxExtent(FVector(12, 9, 6));
    RearCargoMount->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RearCargoMount->SetGenerateOverlapEvents(false);
    RearCargoMount->SetCanEverAffectNavigation(false);
}

UStaticMeshComponent* AMAYANativeDemoPawn::Body(int32 I) const
{ return Parts.IsValidIndex(I) ? Parts[I].Get() : nullptr; }

void AMAYANativeDemoPawn::BeginPlay()
{
    Super::BeginPlay();
    StartChassisPose = Body(0)->GetComponentTransform();
    if (!bReviewedCandidateProfile) { Fail(TEXT("GAME profile not selected after exact source review")); return; }
    if (!PreparePhysics()) return;
    bReady = true; ClearInput();
    if (!RegisterWheelFeedback()) Fail(TEXT("Production wheel feedback registration unavailable"));
}

bool AMAYANativeDemoPawn::ConfigurePlayerCameraPovMount()
{
    bPlayerCameraPovMountKnown = false;
    if (!bReady || !Body(0) || !Camera || !CameraArm || Cosmetics.Num() != 15) return false;
    FBox ShellInChassis(ForceInit);
    for (int32 I = 0; I < Cosmetics.Num(); ++I)
    {
        const auto* V = Cosmetics[I].Get();
        const UStaticMesh* Mesh = V ? V->GetStaticMesh().Get() : nullptr;
        if (!Mesh) return false;
        const FBox Local = Mesh->GetBoundingBox();
        if (!Local.IsValid || Local.Min.ContainsNaN() || Local.Max.ContainsNaN()) return false;
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector P((Corner & 1) ? Local.Max.X : Local.Min.X,
                (Corner & 2) ? Local.Max.Y : Local.Min.Y,
                (Corner & 4) ? Local.Max.Z : Local.Min.Z);
            ShellInChassis += RestInChassis[I].TransformPosition(P);
        }
    }
    if (!ShellInChassis.IsValid || MayaPovOpticalEyeHeightCm < ShellInChassis.Min.Z
        || MayaPovOpticalEyeHeightCm > ShellInChassis.Max.Z) return false;
    PlayerCameraPovEyeChassisCm = FVector(ShellInChassis.Min.X - MayaPovFrontClearanceCm,
        0, MayaPovOpticalEyeHeightCm);
    bPlayerCameraPovMountKnown = true;
    return true;
}

bool AMAYANativeDemoPawn::SetPlayerCameraPov(bool bEnabled)
{
    if (bEnding) return false;
    if (bEnabled == bPlayerCameraPov) return true;
    if (bEnabled && !ConfigurePlayerCameraPovMount()) return false;
    bPlayerCameraPov = bEnabled;
    PovResidualZCm = 0; PovResidualPitchDegrees = PovResidualRollDegrees = PovResidualYawDegrees = 0;
    PovClearanceCorrectionCm = PovFrontClearanceCm = 0;
    PovFollowResetGeneration = 0;
    ++PlayerCameraModeSequence;
    return true;
}

bool AMAYANativeDemoPawn::TogglePlayerCameraMode()
{ return SetPlayerCameraPov(!bPlayerCameraPov); }

void AMAYANativeDemoPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    // Stock camera component supplies the complete existing chase view and
    // post-process settings. The arm keeps its orbit/collision state in POV.
    Super::CalcCamera(DeltaTime, OutResult);
    bLastPlayerCameraViewPov = false;
    if (bPlayerCameraPov && bPlayerCameraPovMountKnown && bReady && Body(0)
        && CameraArm && Cosmetics.Num() == 15)
    {
        const FRotator ChassisDegrees = Body(0)->GetComponentRotation();
        const FVector ChassisCm = Body(0)->GetComponentLocation();
        // Planar travel follows the native chassis. Height and steering use
        // the stable arm frame; subtracting its view offsets isolates filtered
        // chassis yaw and keeps chase orbit independent of the optical view.
        const FVector StableChassisCm(ChassisCm.X, ChassisCm.Y,
            CameraArm->GetComponentLocation().Z - 60);
        const float StableChassisYaw = FMath::UnwindDegrees(CameraArm->GetComponentRotation().Yaw
            - 180.f - CameraYawOffset);
        if (!StableChassisCm.ContainsNaN() && !ChassisCm.ContainsNaN()
            && !ChassisDegrees.ContainsNaN() && FMath::IsFinite(StableChassisYaw))
        {
            if (PovFollowResetGeneration != ResetGeneration)
            {
                // A real admitted robot recovery cuts the camera filter too.
                // This branch cannot request recovery or move any body.
                PovResidualZCm = 0; PovResidualPitchDegrees = PovResidualRollDegrees = PovResidualYawDegrees = 0;
                PovFollowResetGeneration = ResetGeneration;
            }
            const double Dt = UGameplayStatics::IsGamePaused(this) || !FMath::IsFinite(DeltaTime)
                ? 0.0 : FMath::Max(0.0, double(DeltaTime));
            const double A = 1.0 - FMath::Exp(-MayaPovResidualFollowRate * Dt);
            const double TargetZ = FMath::Clamp((ChassisCm.Z - StableChassisCm.Z)
                * MayaPovChassisMotionFraction, -MayaPovVerticalCapCm, MayaPovVerticalCapCm);
            // Facing -X reverses the optical pitch/roll signs relative to the
            // chassis +X frame. Heading follows filtered chassis yaw plus only
            // a small shortest-angle fraction of its measured native residual.
            const float TargetPitch = FMath::Clamp(float(-FMath::UnwindDegrees(ChassisDegrees.Pitch)
                * MayaPovChassisMotionFraction), -MayaPovAngularCapDegrees, MayaPovAngularCapDegrees);
            const float TargetRoll = FMath::Clamp(float(-FMath::UnwindDegrees(ChassisDegrees.Roll)
                * MayaPovChassisMotionFraction), -MayaPovAngularCapDegrees, MayaPovAngularCapDegrees);
            const float TargetYaw = FMath::Clamp(float(FMath::FindDeltaAngleDegrees(StableChassisYaw,
                ChassisDegrees.Yaw) * MayaPovChassisMotionFraction), -MayaPovAngularCapDegrees, MayaPovAngularCapDegrees);
            PovResidualZCm += (TargetZ - PovResidualZCm) * A;
            PovResidualPitchDegrees += (TargetPitch - PovResidualPitchDegrees) * float(A);
            PovResidualRollDegrees += (TargetRoll - PovResidualRollDegrees) * float(A);
            PovResidualYawDegrees += (TargetYaw - PovResidualYawDegrees) * float(A);
            const FRotator LevelChassis(0, StableChassisYaw, 0);
            FVector EyeCm = StableChassisCm + LevelChassis.RotateVector(PlayerCameraPovEyeChassisCm);
            EyeCm.Z += PovResidualZCm;
            const FVector OpticalForward = -LevelChassis.Vector();

            // Chassis pitch/roll and moving cosmetics may extend the current
            // front envelope beyond its rest bound. Keep the optical eye just
            // in front of that envelope; only camera position is corrected.
            double FrontProjection = -TNumericLimits<double>::Max();
            bool bShellKnown = true;
            for (const auto& Visual : Cosmetics)
            {
                const auto* V = Visual.Get();
                const UStaticMesh* Mesh = V ? V->GetStaticMesh().Get() : nullptr;
                if (!Mesh) { bShellKnown = false; break; }
                const FBox Local = Mesh->GetBoundingBox();
                const FTransform World = V->GetComponentTransform();
                if (!Local.IsValid || Local.Min.ContainsNaN() || Local.Max.ContainsNaN()
                    || World.ContainsNaN()) { bShellKnown = false; break; }
                for (int32 Corner = 0; Corner < 8; ++Corner)
                {
                    const FVector P((Corner & 1) ? Local.Max.X : Local.Min.X,
                        (Corner & 2) ? Local.Max.Y : Local.Min.Y,
                        (Corner & 4) ? Local.Max.Z : Local.Min.Z);
                    FrontProjection = FMath::Max(FrontProjection,
                        FVector::DotProduct(World.TransformPosition(P), OpticalForward));
                }
            }
            if (bShellKnown)
            {
                PovClearanceCorrectionCm = FMath::Max(0.0, FrontProjection + MayaPovFrontClearanceCm
                    - FVector::DotProduct(EyeCm, OpticalForward));
                EyeCm += OpticalForward * PovClearanceCorrectionCm;
                PovFrontClearanceCm = FVector::DotProduct(EyeCm, OpticalForward) - FrontProjection;
                OutResult.Location = EyeCm;
                OutResult.Rotation = FRotator(PovResidualPitchDegrees,
                    FMath::UnwindDegrees(StableChassisYaw + 180.f + PovResidualYawDegrees), PovResidualRollDegrees);
                OutResult.PerspectiveNearClipPlane = MayaPovNearClipCm;
                bLastPlayerCameraViewPov = true;
            }
        }
    }
    LastPlayerCameraViewCm = OutResult.Location;
    LastPlayerCameraViewDegrees = OutResult.Rotation;
    LastPlayerCameraFov = OutResult.FOV;
    LastPlayerCameraNearClipCm = OutResult.PerspectiveNearClipPlane;
    LastPlayerCameraViewWorldS = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
    bPlayerCameraViewKnown = !OutResult.Location.ContainsNaN() && !OutResult.Rotation.ContainsNaN();
    ++PlayerCameraViewSamples;
}

FString AMAYANativeDemoPawn::ReadPlayerCameraModeJson() const
{
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.PlayerCameraMode.v1"));
    J->SetStringField(TEXT("selectedMode"), bPlayerCameraPov ? TEXT("POV") : TEXT("Chase"));
    J->SetStringField(TEXT("lastCalculatedMode"), !bPlayerCameraViewKnown ? TEXT("Unobserved")
        : (bLastPlayerCameraViewPov ? TEXT("POV") : TEXT("Chase")));
    J->SetBoolField(TEXT("calculatedViewKnown"), bPlayerCameraViewKnown);
    J->SetBoolField(TEXT("povMountKnown"), bPlayerCameraPovMountKnown);
    J->SetNumberField(TEXT("modeSequence"), double(PlayerCameraModeSequence));
    J->SetNumberField(TEXT("viewSamples"), double(PlayerCameraViewSamples));
    J->SetNumberField(TEXT("worldSeconds"), LastPlayerCameraViewWorldS);
    if (bPlayerCameraViewKnown)
        J->SetObjectField(TEXT("calculatedPawnViewWorldPose"), Pose(FTransform(LastPlayerCameraViewDegrees, LastPlayerCameraViewCm)));
    J->SetNumberField(TEXT("calculatedPitchDegrees"), LastPlayerCameraViewDegrees.Pitch);
    J->SetNumberField(TEXT("calculatedYawDegrees"), LastPlayerCameraViewDegrees.Yaw);
    J->SetNumberField(TEXT("calculatedRollDegrees"), LastPlayerCameraViewDegrees.Roll);
    J->SetNumberField(TEXT("fovDegrees"), LastPlayerCameraFov);
    J->SetNumberField(TEXT("perspectiveNearClipCm"), LastPlayerCameraNearClipCm);
    J->SetObjectField(TEXT("opticalEyeInChassis"), Pose(FTransform(PlayerCameraPovEyeChassisCm)));
    J->SetNumberField(TEXT("residualMotionFraction"), MayaPovChassisMotionFraction);
    J->SetNumberField(TEXT("residualFilterRatePerSecond"), MayaPovResidualFollowRate);
    J->SetNumberField(TEXT("verticalCapCm"), MayaPovVerticalCapCm);
    J->SetNumberField(TEXT("angularCapDegrees"), MayaPovAngularCapDegrees);
    J->SetNumberField(TEXT("residualZCm"), PovResidualZCm);
    J->SetNumberField(TEXT("residualPitchDegrees"), PovResidualPitchDegrees);
    J->SetNumberField(TEXT("residualRollDegrees"), PovResidualRollDegrees);
    J->SetNumberField(TEXT("residualYawDegrees"), PovResidualYawDegrees);
    if (CameraArm)
        J->SetNumberField(TEXT("stableChassisYawDegrees"), FMath::UnwindDegrees(CameraArm->GetComponentRotation().Yaw
            - 180.f - CameraYawOffset));
    if (Body(0))
        J->SetNumberField(TEXT("nativeChassisYawDegrees"), Body(0)->GetComponentRotation().Yaw);
    J->SetNumberField(TEXT("frontClearanceCorrectionCm"), PovClearanceCorrectionCm);
    J->SetNumberField(TEXT("actualCosmeticFrontClearanceCm"), PovFrontClearanceCm);
    J->SetNumberField(TEXT("preservedChaseYawOffsetDegrees"), CameraYawOffset);
    J->SetNumberField(TEXT("preservedChasePitchDegrees"), CameraPitch);
    const auto* PC = Cast<APlayerController>(GetController());
    const auto* Manager = PC ? PC->PlayerCameraManager.Get() : nullptr;
    const bool bManagerIsRobotView = Manager && PC->GetViewTarget() == this;
    J->SetBoolField(TEXT("cameraManagerIsRobotView"), bManagerIsRobotView);
    if (bManagerIsRobotView)
    {
        J->SetObjectField(TEXT("actualCameraManagerWorldPose"),
            Pose(FTransform(Manager->GetCameraRotation(), Manager->GetCameraLocation())));
        J->SetNumberField(TEXT("actualCameraManagerFovDegrees"), Manager->GetFOVAngle());
        J->SetNumberField(TEXT("actualCameraManagerCacheWorldSeconds"), Manager->GetCameraCacheTime());
    }
    J->SetStringField(TEXT("scope"), TEXT("Last pawn CalcCamera output and current final camera-manager cache when view target is this robot; cache timestamp is explicit and pixels remain separate. Existing playerCameraWorldPose/stablePlayerCamera are chase component/arm only. POV XY follows actual chassis; postphysics stable height/yaw independent of orbit plus3% bounded low-pass native Z/pitch/roll/shortest-angle yaw residual, registered optical-X eye and actual cosmetic OBB clearance. No synthetic shake or gameplay mutation."));
    FString Text; FJsonSerializer::Serialize(J, TJsonWriterFactory<>::Create(&Text)); return Text;
}

bool AMAYANativeDemoPawn::PrepareDeliveryCollisionProfile()
{
    if (!bDeliveryOwnsInteraction || bDeliveryCollisionProfileLoaded) return true;
    // Candidate02: exact original hull envelope minus measured empty bays;
    // underside seam radii37.4/33.1cm retain original hinge pivots and >=0.6823cm
    // ideal independent-angle gap. Actual native travel/clearance still decides admission.
    const int32 Indices[] = {0, 7, 8};
    const TCHAR* Roles[] = {TEXT("Base"), TEXT("Door1"), TEXT("Door2")};
    const int32 HullCounts[] = {24, 4, 4};
    UStaticMesh* PhysicsMeshes[3] = {};
    UStaticMesh* VisualMeshes[2] = {};
    for (int32 K = 0; K < 3; ++K)
    {
        const FString Asset = FString(TEXT("SM_MAYA_Physics_")) + Roles[K];
        PhysicsMeshes[K] = LoadObject<UStaticMesh>(nullptr,
            *(TEXT("/Game/MAYA/DeliveryPhysics/v02/") + Asset + TEXT(".") + Asset));
        auto* Setup = PhysicsMeshes[K] ? PhysicsMeshes[K]->GetBodySetup() : nullptr;
        if (!Setup || Setup->AggGeom.ConvexElems.Num() != HullCounts[K]
            || Setup->CollisionTraceFlag == CTF_UseComplexAsSimple)
        { Fail(TEXT("Reviewed owned delivery collider asset/count absent")); return false; }
        Setup->CreatePhysicsMeshes();
        for (const auto& Elem : Setup->AggGeom.ConvexElems)
        {
            const auto& Cooked = Elem.GetChaosConvexMesh();
            if (!Cooked.IsValid() || Cooked->NumVertices() < 4 || !FMath::IsFinite(Cooked->GetVolume()) || Cooked->GetVolume() <= 0)
            { Fail(TEXT("Reviewed delivery convex cooking produced invalid/empty native hull")); return false; }
        }
        const int32 FirstHull[] = {0, 24, 28};
        if (!MayaNativeV02ConvexBoundsMatch(*Setup, FirstHull[K], HullCounts[K])
            || !MayaNativeV02BoundsMatch(PhysicsMeshes[K]->GetBoundingBox(), MayaNativeV02RenderBounds[K]))
        { Fail(TEXT("Owned delivery collider raw/cooked/render cm bounds disagree with reviewed candidate02")); return false; }
        if (K > 0)
        {
            const FString Visual = FString(TEXT("SM_MAYA_Visual_")) + Roles[K];
            VisualMeshes[K - 1] = LoadObject<UStaticMesh>(nullptr,
                *(TEXT("/Game/MAYA/DeliveryPhysics/v02/") + Visual + TEXT(".") + Visual));
            if (!VisualMeshes[K - 1]) { Fail(TEXT("Matching owned delivery seam visual absent")); return false; }
            if (!MayaNativeV02BoundsMatch(VisualMeshes[K - 1]->GetBoundingBox(), MayaNativeV02RenderBounds[K + 2]))
            { Fail(TEXT("Owned delivery visual cm bounds disagree with reviewed seam geometry")); return false; }
        }
    }
    for (int32 K = 0; K < 3; ++K) Body(Indices[K])->SetStaticMesh(PhysicsMeshes[K]);
    CosmeticMeshes.Add(FName(SourceParts[7].Name), VisualMeshes[0]);
    CosmeticMeshes.Add(FName(SourceParts[8].Name), VisualMeshes[1]);
    // The original X0 box crosses the real X4.78..5.70cm divider. This measured
    // left bay target preserves that wall, floor and both lids in every cargo query.
    CargoMount->SetRelativeLocation(FVector(-10, 0, 27));
    bDeliveryCollisionProfileLoaded = true;
    return true;
}

bool AMAYANativeDemoPawn::PreparePhysics()
{
    if (!GetWorld() || Parts.Num() != 15 || !StartChassisPose.GetScale3D().Equals(FVector::OneVector, 1.e-6))
    { Fail(TEXT("World, exact 15 components and unit actor scale required")); return false; }
    if (!PrepareDeliveryCollisionProfile()) return false;
    if (!FMath::IsFinite(Door1OpenDegrees) || !FMath::IsFinite(Door2OpenDegrees)
        || FMath::Abs(Door1OpenDegrees) < 10 || FMath::Abs(Door1OpenDegrees) >= 180
        || FMath::Abs(Door2OpenDegrees) < 10 || FMath::Abs(Door2OpenDegrees) >= 180
        || !FMath::IsFinite(LidKpNmPerRad) || LidKpNmPerRad <= 0
        || !FMath::IsFinite(LidKdNmPerRadps) || LidKdNmPerRadps <= 0
        || !FMath::IsFinite(LidPeakNm) || LidPeakNm <= 0)
    { Fail(TEXT("Explicit finite signed hinge range and positive finite PD/torque required")); return false; }
    for (int32 I = 0; I < 15; ++I)
    {
        auto* P = Body(I); auto* Setup = P ? P->GetBodySetup() : nullptr;
        if (!P || !P->IsRegistered() || !P->GetStaticMesh() || !Setup
            || Setup->AggGeom.GetElementCount() == 0 || Setup->CollisionTraceFlag == CTF_UseComplexAsSimple
            || !P->GetComponentScale().Equals(FVector::OneVector, 1.e-6)
            || !CosmeticMeshes.FindRef(FName(SourceParts[I].Name)))
        { Fail(TEXT("Reviewed simple shape, unit-scale component or corresponding imported cosmetic absent")); return false; }
    }
    ContactMaterial = NewObject<UPhysicalMaterial>(this);
    ContactMaterial->StaticFriction = 0.8f; ContactMaterial->Friction = 0.6f; ContactMaterial->Restitution = 0.05f;
    // This is the single actively controlled robot's private material. Keep its
    // nine native bodies solving even at rest, so pause/resume cannot strand
    // ground-support admission on stale hits plus an asynchronous awake flag.
    // UE5.3 IslandManager treats both zero sleep velocity thresholds as never
    // sleep (isolated and connected island paths). No city/global sleep change.
    ContactMaterial->SleepLinearVelocityThreshold = 0.0f;
    ContactMaterial->SleepAngularVelocityThreshold = 0.0f;
    ContactMaterial->bOverrideFrictionCombineMode = true; ContactMaterial->FrictionCombineMode = EFrictionCombineMode::Average;
    ContactMaterial->bOverrideRestitutionCombineMode = true; ContactMaterial->RestitutionCombineMode = EFrictionCombineMode::Average;
    for (int32 I = 0; I < 15; ++I)
    {
        auto* P = Body(I);
        P->SetCollisionProfileName(TEXT("PhysicsActor"));
        P->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        P->SetEnableGravity(true); P->SetPhysMaterialOverride(ContactMaterial);
        P->SetLinearDamping(0); P->SetAngularDamping(0);
        P->BodyInstance.SetInertiaConditioningEnabled(false);
        if (I < 9) P->SetSimulatePhysics(true);
    }
    // UE5.3 applies overrides per shape-owner batch, then sums welded contributions.
    // Freeze each owning instance BEFORE weld: Base37.241 + six suspensions0.1 =37.841kg.
    Body(0)->SetMassOverrideInKg(NAME_None, float(BaseContributionKg), true);
    for (int32 S = 9; S < 15; ++S)
    {
        auto* OwnBI = Body(S)->GetBodyInstance(NAME_None, false);
        if (!OwnBI || OwnBI->WeldParent) { Fail(TEXT("Unwelded suspension owning body instance required")); return false; }
        OwnBI->SetMassOverride(float(SuspensionContributionKg), true);
        OwnBI->UpdateMassProperties();
    }
    // Standard UE compound weld: original suspension geometry and source poses stay unchanged.
    for (int32 S = 9; S < 15; ++S)
    {
        auto* P = Body(S); P->WeldTo(Body(0));
        auto* ChildBI = P->GetBodyInstance(NAME_None, false);
        if (!ChildBI || ChildBI->WeldParent != Body(0)->GetBodyInstance())
        { Fail(TEXT("Actual suspension shape weld absent")); return false; }
    }
    Body(0)->GetBodyInstance()->UpdateMassProperties();
    const double AggregateMassKg = Body(0)->GetMass();
    const FVector AggregateCOM = Body(0)->GetCenterOfMass();
    if (!FMath::IsFinite(AggregateMassKg) || FMath::Abs(AggregateMassKg - ChassisCompoundKg)
            > ChassisCompoundKg * AggregateMassRelativeTolerance || AggregateCOM.ContainsNaN())
    { Fail(TEXT("Native welded aggregate mass/COM disagree with explicit37.841kg distribution")); return false; }
    for (int32 I = 1; I <= 6; ++I)
    {
        Body(I)->SetMassOverrideInKg(NAME_None, 1.f, true);
        Body(I)->SetNotifyRigidBodyCollision(true);
        Body(I)->OnComponentHit.AddUniqueDynamic(this, &ThisClass::OnWheelHit);
    }
    Body(7)->SetMassOverrideInKg(NAME_None, 2.710249f, true);
    Body(8)->SetMassOverrideInKg(NAME_None, 2.421605f, true);
    if (!CreateJoints()) return false;
    bPhysicalAssembly = true;
    for (int32 I = 0; I < 15; ++I)
    {
        auto* V = NewObject<UStaticMeshComponent>(this, FName(*FString::Printf(TEXT("MAYAVisual%d"), I)));
        AddInstanceComponent(V); Cosmetics.Add(V);
        V->SetMobility(EComponentMobility::Movable);
        V->SetStaticMesh(CosmeticMeshes.FindRef(FName(SourceParts[I].Name)));
        V->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        V->SetGenerateOverlapEvents(false); V->SetCanEverAffectNavigation(false);
        V->BodyInstance.bAutoWeld = false; V->SetSimulatePhysics(false);
        V->AttachToComponent(Body(I), FAttachmentTransformRules::KeepRelativeTransform);
        V->SetRelativeTransform(FTransform::Identity); V->RegisterComponent();
        Body(I)->SetVisibility(false, false); // Do not propagate to cosmetic children.
    }
    return true;
}

bool AMAYANativeDemoPawn::CreateJoints()
{
    const FQuat Axes = FRotationMatrix::MakeFromXZ(FVector(0, 1, 0), FVector(0, 0, 1)).ToQuat();
    for (int32 I = 0; I < 8; ++I)
    {
        JointInPart[I] = FTransform(RestInChassis[I + 1].GetRotation().Inverse() * Axes, FVector::ZeroVector);
        JointInChassis[I] = JointInPart[I] * RestInChassis[I + 1];
        auto* J = NewObject<UPhysicsConstraintComponent>(this, FName(*FString::Printf(TEXT("MAYADemoJoint%d"), I)));
        AddInstanceComponent(J); Joints.Add(J);
        J->SetWorldTransform(JointInChassis[I] * Body(0)->GetComponentTransform());
        J->ConstraintInstance.bScaleLinearLimits = false;
        auto& C = J->ConstraintInstance.ProfileInstance;
        C.bEnableProjection = false; C.bEnableShockPropagation = false;
        C.bParentDominates = false; C.bEnableMassConditioning = false;
        J->SetLinearXLimit(LCM_Locked, 0); J->SetLinearYLimit(LCM_Locked, 0);
        J->SetLinearZLimit(I < 6 ? LCM_Limited : LCM_Locked, I < 6 ? 2.f : 0.f);
        J->SetAngularSwing1Limit(ACM_Locked, 0); J->SetAngularSwing2Limit(ACM_Locked, 0);
        J->SetAngularTwistLimit(I < 6 ? ACM_Free : ACM_Locked, 0);
        J->SetDisableCollision(true); // Only declared immediate chassis/part adjacency.
        if (I < 6)
        {
            J->SetLinearPositionDrive(false, false, true); J->SetLinearVelocityDrive(false, false, true);
            J->SetLinearPositionTarget(FVector::ZeroVector);
            J->SetLinearDriveParams(2500, 180, 500 * 100); // Native F/cm units.
        }
        J->RegisterComponent();
        // UE 5.3 reverses endpoints for the solver. Wheel-first makes solver
        // frame zero the chassis, keeping suspension Z fixed while wheels roll.
        // Semantic owner frames stay unchanged; original lid order is retained.
        UStaticMeshComponent* Endpoint0 = I < 6 ? Body(I + 1) : Body(0);
        UStaticMeshComponent* Endpoint1 = I < 6 ? Body(0) : Body(I + 1);
        const FTransform& Frame0 = I < 6 ? JointInPart[I] : JointInChassis[I];
        const FTransform& Frame1 = I < 6 ? JointInChassis[I] : JointInPart[I];
        J->SetConstrainedComponents(Endpoint0, NAME_None, Endpoint1, NAME_None);
        J->SetConstraintReferenceFrame(EConstraintFrame::Frame1, Frame0);
        J->SetConstraintReferenceFrame(EConstraintFrame::Frame2, Frame1);
        const auto Ref = J->ConstraintInstance.GetPhysicsConstraintRef();
        if (!Ref.IsValid() || J->IsBroken())
        { Fail(TEXT("Native constraint handle invalid/broken")); return false; }
        bool Verified = false;
        FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref, [&](const FPhysicsConstraintHandle& Locked)
        {
            if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
            auto* Actual = static_cast<Chaos::FJointConstraint*>(Locked.Constraint);
            const auto Endpoints = Actual->GetPhysicsBodies();
            auto* A = Endpoint0->GetBodyInstance()->GetPhysicsActorHandle();
            auto* B = Endpoint1->GetBodyInstance()->GetPhysicsActorHandle();
            if (!A || !B || Endpoints[0] != A->GetPhysicsObject() || Endpoints[1] != B->GetPhysicsObject()) return;
            Actual->SetJointTransforms({Frame0, Frame1});
            if (I < 6)
            {
                Actual->SetLinearDriveForceMode(Chaos::EJointForceMode::Force);
                Actual->SetLinearDriveStiffness(Chaos::FVec3(0, 0, 2500));
                Actual->SetLinearDriveDamping(Chaos::FVec3(0, 0, 180));
                Actual->SetLinearDriveMaxForce(Chaos::FVec3(0, 0, 500 * 100));
            }
            Verified = DDSJointSettingsMatch(Actual->GetJointSettings(), I, Frame0, Frame1,
                I < 6 ? 2.0 : 0.0, 0, true, MotorProfile());
        });
        if (!Verified) { Fail(TEXT("Actual joint endpoints, full frames, limits or force mode disagree")); return false; }
        if (I < 6 && !ConfigureWheelBrake(I, true))
        { Fail(TEXT("Actual native wheel brake configuration unavailable")); return false; }
    }
    return true;
}

void AMAYANativeDemoPawn::ConfigureDoorJoint(int32 Lid, bool ClosedLock)
{
    const int32 I = Lid + 6;
    FTransform Frame = JointInChassis[I];
    const double Open = FMath::DegreesToRadians(Lid == 0 ? Door1OpenDegrees : Door2OpenDegrees);
    if (!ClosedLock) Frame.SetRotation(Frame.GetRotation() * FQuat(FVector::ForwardVector, Open / 2));
    Joints[I]->SetConstraintReferenceFrame(EConstraintFrame::Frame1, Frame);
    Joints[I]->SetAngularTwistLimit(ClosedLock ? ACM_Locked : ACM_Limited,
        ClosedLock ? 0.f : FMath::Abs(FMath::RadiansToDegrees(Open)) / 2.f);
}

void AMAYANativeDemoPawn::EnsureInput()
{
    if (InputContext) return;
    InputContext = NewObject<UInputMappingContext>(this, TEXT("MAYADemoInput"), RF_Transient);
    for (int32 I = 0; I < 5; ++I)
    {
        auto* A = NewObject<UInputAction>(this, NAME_None, RF_Transient);
        A->ValueType = I < 2 ? EInputActionValueType::Axis1D : EInputActionValueType::Boolean;
        A->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
        InputActions.Add(A);
    }
    const auto Map = [this](int32 I, FKey Key, bool Negative = false)
    {
        auto& M = InputContext->MapKey(InputActions[I], Key);
        if (Negative) M.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
    };
    Map(0, EKeys::W); Map(0, EKeys::S, true); Map(1, EKeys::A, true); Map(1, EKeys::D);
    Map(2, EKeys::SpaceBar); Map(3, EKeys::E); Map(4, EKeys::R);
}
void AMAYANativeDemoPawn::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input); EnsureInput();
    if (auto* E = Cast<UEnhancedInputComponent>(Input))
    {
        for (int32 I = 0; I < 3; ++I) E->BindAction(InputActions[I], ETriggerEvent::Triggered, this, &ThisClass::ConsumeAxis);
        if (!bDeliveryOwnsInteraction)
        {
            E->BindAction(InputActions[3], ETriggerEvent::Started, this, &ThisClass::ToggleDoors);
            E->BindAction(InputActions[4], ETriggerEvent::Started, this, &ThisClass::OnResetKey);
        }
    }
}
void AMAYANativeDemoPawn::ConsumeAxis(const FInputActionValue&) {}
void AMAYANativeDemoPawn::AddInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& S)
{
    if (!S.GetInterface() || bEnding) return;
    EnsureInput(); if (InputSubsystem.Get() != S.GetObject()) RemoveInput();
    InputSubsystem = S.GetObject(); bContextActive = true;
    S->AddMappingContext(InputContext, 2); ClearInput();
}
void AMAYANativeDemoPawn::RemoveInputContext_Implementation(const TScriptInterface<IEnhancedInputSubsystemInterface>& S)
{ if (InputSubsystem.Get() == S.GetObject()) RemoveInput(); }
void AMAYANativeDemoPawn::RemoveInput()
{
    bContextActive = false; ClearInput();
    if (auto* S = Cast<IEnhancedInputSubsystemInterface>(InputSubsystem.Get()))
        if (InputContext) S->RemoveMappingContext(InputContext);
    InputSubsystem.Reset();
}
void AMAYANativeDemoPawn::ClearInput()
{
    Throttle = Steer = 0; bBrake = true; bBoostRequested = bBoostMotorApplied = false; InputIssuedRealS = -1; InputLeaseS = 0;
    InputOrigin = EMAYADriveCommandOrigin::None; ++InputSequence; bInputsNeedRelease = true;
    for (double& Nm : MotorNm) Nm = 0;
    InvalidateWheelFeedbackEvidence(); PublishWheelFeedbackCommand();
}
bool AMAYANativeDemoPawn::CanReadLocalInput() const
{
    const auto* PC = Cast<APlayerController>(GetController());
    const auto* V = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    return !bEnding && bReady && bContextActive && PC && PC->IsLocalController() && PC->GetPawn() == this
        && !PC->IsMoveInputIgnored() && !UGameplayStatics::IsGamePaused(this)
        && V && V->Viewport && V->Viewport->HasFocus();
}
void AMAYANativeDemoPawn::PollInput()
{
    // A bounded native automation lease is explicit and cannot be overwritten by raw keys.
    if (InputOrigin == EMAYADriveCommandOrigin::NativeAutomation && HasFreshDriveCommand()) return;
    if (!CanReadLocalInput()) { ClearInput(); return; }
    const auto* PC = Cast<APlayerController>(GetController());
    const bool W = PC->IsInputKeyDown(EKeys::W), S = PC->IsInputKeyDown(EKeys::S);
    const bool A = PC->IsInputKeyDown(EKeys::A), D = PC->IsInputKeyDown(EKeys::D);
    const bool Brake = PC->IsInputKeyDown(EKeys::SpaceBar);
    const bool Boost = PC->IsInputKeyDown(EKeys::LeftShift);
    if (bInputsNeedRelease && (W || S || A || D || Brake || Boost))
    { Throttle = Steer = 0; bBrake = true; bBoostRequested = bBoostMotorApplied = false; return; }
    bInputsNeedRelease = false;
    const float PlayerThrottle=float(W)-float(S);
    SubmitDriveCommand(PlayerThrottle,MAYAPlayerDriveInput::BodyYawSteer(PlayerThrottle,float(D)-float(A)),
        Brake,EMAYADriveCommandOrigin::LocalPlayer,0.25f,Boost);
    if (!bPlayerCameraPov && PC->IsInputKeyDown(EKeys::RightMouseButton))
    {
        float MouseX = 0, MouseY = 0;
        PC->GetInputMouseDelta(MouseX, MouseY);
        CameraYawOffset = FMath::UnwindDegrees(CameraYawOffset + MouseX * 0.3f);
        CameraPitch = FMath::Clamp(CameraPitch - MouseY * 0.3f, -70.f, -5.f);
    }
    if (!bPlayerCameraPov && PC->IsInputKeyDown(EKeys::Home)) { CameraYawOffset = 0; CameraPitch = -20; }
    CameraArm->SetViewAngles(CameraPitch, CameraYawOffset);
}

bool AMAYANativeDemoPawn::SubmitDriveCommand(float InThrottle, float InSteer, bool bInBrake,
    EMAYADriveCommandOrigin Origin, float LeaseSeconds, bool bInBoost)
{
    if (bEnding || !bReady || UGameplayStatics::IsGamePaused(this)
        || !FMath::IsFinite(InThrottle) || !FMath::IsFinite(InSteer)
        || !FMath::IsFinite(LeaseSeconds) || LeaseSeconds <= 0 || LeaseSeconds > InputMaxAgeS
        || Origin == EMAYADriveCommandOrigin::None) return false;
    if (Origin == EMAYADriveCommandOrigin::LocalPlayer && !CanReadLocalInput()) return false;
    if (Origin == EMAYADriveCommandOrigin::NativeAutomation)
    {
#if UE_BUILD_SHIPPING
        return false;
#else
        if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))) return false;
#endif
    }
    if (bInputsNeedRelease)
    {
        if (!FMath::IsNearlyZero(InThrottle) || !FMath::IsNearlyZero(InSteer) || bInBoost) return false;
        bInputsNeedRelease = false;
    }
    Throttle = FMath::Clamp(InThrottle, -1.f, 1.f); Steer = FMath::Clamp(InSteer, -1.f, 1.f);
    bBrake = bInBrake; bBoostRequested = bInBoost; bBoostMotorApplied = false; InputOrigin = Origin; InputLeaseS = LeaseSeconds;
    InputIssuedRealS = FPlatformTime::Seconds(); ++InputSequence;
    PublishWheelFeedbackCommand();
    return true;
}

bool AMAYANativeDemoPawn::HasFreshDriveCommand() const
{
    const double Age = FPlatformTime::Seconds() - InputIssuedRealS;
    if (bEnding || !bReady || UGameplayStatics::IsGamePaused(this)
        || Age < 0 || Age > InputLeaseS || Age > InputMaxAgeS) return false;
    return InputOrigin == EMAYADriveCommandOrigin::NativeAutomation
        || (InputOrigin == EMAYADriveCommandOrigin::LocalPlayer && CanReadLocalInput());
}

bool AMAYANativeDemoPawn::HasFreshMovementIntent() const
{
    // Observe a fresh accepted command through the existing focus/lease/release
    // boundary, even while charging owns the delivery lock. Authority may end
    // that charge; this getter itself never enables motors or clears a lock.
    return HasFreshDriveCommand() && !bResetStabilizing && Fault.IsEmpty() && !bBrake
        && !bDriveInhibited && !bInputsNeedRelease && DoorState == EDoorState::ClosedLatched
        && (!FMath::IsNearlyZero(Throttle) || !FMath::IsNearlyZero(Steer));
}

bool AMAYANativeDemoPawn::IsBoostDriveActive() const
{
    if (!bBoostRequested || !bBoostMotorApplied || !HasFreshMovementIntent() || bBrake
        || bDriveInhibited || bDeliveryDriveLocked || bInputsNeedRelease || DoorState != EDoorState::ClosedLatched)
        return false;
    if (!bNativePreviousBoostProfile && (!HasCurrentWheelFeedback() || !ActualWheelFeedback.Decision.bBoostActive)) return false;
    return MAYADeliveryBoostProfile::HasActualMotorTorque(MotorNm);
}

double AMAYANativeDemoPawn::ReadBoostMotorEffortFraction() const
{
    if (!IsBoostDriveActive()) return 0.0;
    // Reuse the peak that produced the actual motor cache, never a later speed.
    if (!DriveAdmissionEvidence.bMotorDecisionKnown) return 0.0;
    return MAYADeliveryBoostProfile::ActualMotorEffortFraction(MotorNm, DriveAdmissionEvidence.MotorPeakNm, true);
}

void AMAYANativeDemoPawn::ReleaseDriveCommand() { ClearInput(); }

bool AMAYANativeDemoPawn::SelectNativeBoostComparisonProfile(bool bPreviousProfile)
{
#if UE_BUILD_SHIPPING
    return false;
#else
    if (!FParse::Param(FCommandLine::Get(), TEXT("MayaV01AutoTest"))
        || !FParse::Param(FCommandLine::Get(), TEXT("MayaV01BoostPowerTest"))
        || (bPreviousProfile && !FParse::Param(FCommandLine::Get(), TEXT("MayaV01BoostLegacyComparison")))
        || !HasFreshDriveCommand() || !bBrake || bBoostRequested || bBoostMotorApplied
        || bDeliveryDriveLocked || bDriveInhibited || !FMath::IsNearlyZero(Throttle) || !FMath::IsNearlyZero(Steer))
        return false;
    if (bWheelComparisonSelectionPending)
        return bWheelComparisonDesiredPrevious == bPreviousProfile;
    if (bNativePreviousBoostProfile == bPreviousProfile && IsNativeBoostComparisonProfileReady(bPreviousProfile)) return true;
    FMAYADeliveryRobotObservation Observation;
    if (!ReadDeliveryState(Observation) || !Observation.IsStoppedAndSupported()
        || !Observation.bDoorsClosedLatched || !AreDeliveryDoorsClosedAndPhysicallyLocked()) return false;
    for (const double Nm : MotorNm)
        if (!FMath::IsFinite(Nm) || FMath::Abs(Nm) > 1.e-6) return false;
    bWheelComparisonSelectionPending = true; bWheelComparisonDesiredPrevious = bPreviousProfile;
    bWheelExternalWritesStopped = true; InvalidateWheelFeedbackEvidence();
    if (bPreviousProfile)
    {
        PublishWheelFeedbackCommand(); // Block the permanent owner before waiting for completed output.
        RecordWheelFeedbackOwnership(TEXT("ProductionToLegacyBlockedRequested"));
    }
    else
    {
        // The exclusive external route is stopped before the live-scene flush.
        bNativePreviousBoostProfile = false;
        auto* Scene = GetWorld() ? GetWorld()->GetPhysicsScene() : nullptr;
        if (!Scene) { Fail(TEXT("Legacy return has no live physics scene")); return false; }
        Scene->Flush();
        if (!RegisterWheelFeedback()) { Fail(TEXT("Production return callback registration failed")); return false; }
        RecordWheelFeedbackOwnership(TEXT("LegacyToProductionBlockedRegistered"));
    }
    return true;
#endif
}

void AMAYANativeDemoPawn::OnWheelHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, FVector Impulse, const FHitResult& Hit)
{
    if (!bReady || bEnding || OtherActor == this || !IsValid(OtherComponent)
        || Impulse.ContainsNaN() || Impulse.Size() < MinContactImpulseKgCmps
        || Hit.ImpactNormal.ContainsNaN() || Hit.ImpactNormal.Z < MinContactUpDot) return;
    for (int32 W = 0; W < 6; ++W) if (HitComponent == Body(W + 1))
    {
        auto& C = Contacts[W]; C.Support = OtherComponent; C.SupportPose = OtherComponent->GetComponentTransform();
        C.WheelPose = Body(W + 1)->GetComponentTransform(); C.Point = Hit.ImpactPoint; C.Normal = Hit.ImpactNormal;
        C.IssuedRealS = FPlatformTime::Seconds();
        C.IssuedActiveRealS = GetWorld() ? GetWorld()->GetAudioTimeSeconds() : -1;
        C.ImpulseKgCmps = Impulse.Size();
    }
}
bool AMAYANativeDemoPawn::HasObservedWheelSupport(int32 W) const
{
    if (!GetWorld() || W < 0 || W >= UE_ARRAY_COUNT(Contacts)) return false;
    const auto& C = Contacts[W]; auto* P = Body(W + 1);
    const auto* BI = P ? P->GetBodyInstance() : nullptr;
    if (!BI || !C.Support.IsValid() || C.Support->GetWorld() != GetWorld()
        || !FMath::IsFinite(C.IssuedRealS) || C.IssuedRealS < 0
        || !FMath::IsFinite(C.IssuedActiveRealS) || C.IssuedActiveRealS < 0) return false;
    const double RealNow = FPlatformTime::Seconds();
    const double ActiveNow = GetWorld()->GetAudioTimeSeconds();
    const bool FreshWall = RealNow >= C.IssuedRealS && RealNow - C.IssuedRealS <= ContactMaxAgeS;
    const bool FreshActive = FMath::IsFinite(ActiveNow) && ActiveNow >= C.IssuedActiveRealS
        && ActiveNow - C.IssuedActiveRealS <= ContactMaxAgeS;
    const FTransform WheelNow = P->GetComponentTransform(), SupportNow = C.Support->GetComponentTransform();
    const FVector V = P->GetPhysicsLinearVelocity(), Omega = P->GetPhysicsAngularVelocityInRadians();
    const bool AnchorsQuiet = !WheelNow.ContainsNaN() && !SupportNow.ContainsNaN()
        && !C.WheelPose.ContainsNaN() && !C.SupportPose.ContainsNaN()
        && !V.ContainsNaN() && !Omega.ContainsNaN()
        && WheelNow.GetScale3D().Equals(C.WheelPose.GetScale3D(), 1.e-6)
        && SupportNow.GetScale3D().Equals(C.SupportPose.GetScale3D(), 1.e-6)
        && WheelNow.GetLocation().Equals(C.WheelPose.GetLocation(), SleepingAnchorCm)
        && WheelNow.GetRotation().AngularDistance(C.WheelPose.GetRotation()) <= 0.01
        && SupportNow.GetLocation().Equals(C.SupportPose.GetLocation(), SleepingAnchorCm)
        && SupportNow.GetRotation().AngularDistance(C.SupportPose.GetRotation()) <= 0.01
        && V.Size() <= 2 && Omega.Size() <= 0.1;
    // A real loaded receipt cannot gain lifetime during frozen physics. Paused or
    // wall-stale reuse additionally requires the same quiet measured anchors;
    // after unpause its remaining 0.25 active-second age resumes immediately.
    const bool Fresh = FreshActive
        && ((FreshWall && !UGameplayStatics::IsGamePaused(this)) || AnchorsQuiet);
    const bool Sleeping = !BI->IsInstanceAwake() && AnchorsQuiet;
    return Fresh || Sleeping;
}
void AMAYANativeDemoPawn::ObserveSupport()
{
    SupportMask = 0; const double Now = GetWorld() ? GetWorld()->GetAudioTimeSeconds() : -1;
    for (int32 W = 0; W < 6; ++W)
        if (HasObservedWheelSupport(W)) SupportMask |= 1 << W;
    const FRotator R = Body(0)->GetComponentRotation();
    const bool Stable = SupportMask == 0x3f && Body(0)->GetPhysicsLinearVelocity().Size() <= StopSpeedCmps
        && Body(0)->GetPhysicsAngularVelocityInRadians().Size() <= StopOmegaRadps
        && FMath::Abs(R.Pitch) <= 8 && FMath::Abs(R.Roll) <= 8;
    if (!Stable) SupportStableSinceActiveRealS = -1;
    else if (SupportStableSinceActiveRealS < 0) SupportStableSinceActiveRealS = Now;
    if (Stable && Now - SupportStableSinceActiveRealS >= StableSupportS) bResetStabilizing = false;
}
void AMAYANativeDemoPawn::SampleDoorPhysicalState()
{
    DoorPhysicalSampleFrame = GFrameCounter;
    DoorPhysicalSampleWorldS = GetWorld() ? GetWorld()->GetTimeSeconds() : -1;
    DoorPhysicalSampleRealS = FPlatformTime::Seconds();
    // Synchronized native pose/COM/mass/omega are refreshed even after Fail revokes bReady.
    // Invalid/broken physical hinge state never receives a cached or guessed damping torque.
    for (int32 L = 0; L < 2; ++L)
    {
        bDoorPhysicalStateKnown[L] = false; DoorGravityNm[L] = DoorMotorNm[L] = 0;
        const int32 I = L + 6;
        if (!GetWorld() || !IsValid(Body(0)) || !IsValid(Body(I + 1))
            || !Body(0)->IsSimulatingPhysics() || !Body(I + 1)->IsSimulatingPhysics()
            || !Joints.IsValidIndex(I) || !Joints[I] || Joints[I]->IsBroken()
            || !Joints[I]->ConstraintInstance.GetPhysicsConstraintRef().IsValid()) continue;
        const FTransform A = JointInChassis[I] * Body(0)->GetComponentTransform();
        const FTransform B = JointInPart[I] * Body(I + 1)->GetComponentTransform();
        const FVector LidCOM = Body(I + 1)->GetCenterOfMass();
        const FVector RelativeOmega = Body(I + 1)->GetPhysicsAngularVelocityInRadians()
            - Body(0)->GetPhysicsAngularVelocityInRadians();
        const double MassKg = Body(I + 1)->GetMass(), GravityCmps2 = GetWorld()->GetGravityZ();
        if (A.ContainsNaN() || B.ContainsNaN() || !A.GetRotation().IsNormalized() || !B.GetRotation().IsNormalized()
            || LidCOM.ContainsNaN() || RelativeOmega.ContainsNaN() || !FMath::IsFinite(MassKg)
            || MassKg <= 0 || !FMath::IsFinite(GravityCmps2)) continue;
        const FVector Axis = A.GetUnitAxis(EAxis::X);
        DoorAngleRad[L] = MayaNativeTwist(A.GetRotation(), B.GetRotation());
        DoorOmegaRadps[L] = FVector::DotProduct(RelativeOmega, Axis);
        // cm->m at BOTH lever and acceleration boundaries. m*g cross lever produces Nm.
        const FVector LeverM = (LidCOM - A.GetLocation()) / 100.0;
        const FVector GravityForceN(0, 0, MassKg * GravityCmps2 / 100.0);
        DoorGravityNm[L] = FVector::DotProduct(FVector::CrossProduct(LeverM, GravityForceN), Axis);
        bDoorPhysicalStateKnown[L] = FMath::IsFinite(DoorAngleRad[L]) && FMath::IsFinite(DoorOmegaRadps[L])
            && FMath::IsFinite(DoorGravityNm[L]);
    }
}
void AMAYANativeDemoPawn::ObserveDoors()
{
    bool ClosedPose = true, ClosedSettled = true, Open = true, BothActualLocks = true;
    auto& E = DriveAdmissionEvidence;
    E.bDoorDecisionKnown = false; E.bClosedPose = E.bClosedSettled = false;
    E.bClosedLatched = DoorState == EDoorState::ClosedLatched; E.bResetStabilizing = bResetStabilizing; E.bDoorDriveInhibited = true;
    E.DoorDecisionFrame = GFrameCounter;
    E.DoorDecisionWorldS = GetWorld()->GetTimeSeconds(); E.DoorEvaluationStartedRealS = FPlatformTime::Seconds();
    E.DoorSampleFrame = DoorPhysicalSampleFrame; E.DoorSampleWorldS = DoorPhysicalSampleWorldS; E.DoorSampleRealS = DoorPhysicalSampleRealS;
    for (int32 L = 0; L < 2; ++L)
    { E.FullDoorAngleRad[L] = -1; E.CachedDoorRateRadps[L] = DoorOmegaRadps[L]; E.DoorKnown[L] = bDoorPhysicalStateKnown[L]; E.NativeLockReadKnown[L] = E.ActualNativeClosedLock[L] = false; }
    for (int32 L = 0; L < 2; ++L)
    {
        const int32 I = L + 6;
        if (!bDoorPhysicalStateKnown[L]) { Fail(TEXT("Current native door pose/COM/mass/omega/hinge state unavailable")); return; }
        const FTransform A = JointInChassis[I] * Body(0)->GetComponentTransform();
        const FTransform B = JointInPart[I] * Body(I + 1)->GetComponentTransform();
        const double Target = FMath::DegreesToRadians(L == 0 ? Door1OpenDegrees : Door2OpenDegrees);
        E.FullDoorAngleRad[L] = A.GetRotation().AngularDistance(B.GetRotation());
        ClosedPose &= MAYALatchedDoorDrive::HasClosedPose(bDoorPhysicalStateKnown[L], E.FullDoorAngleRad[L], DoorOmegaRadps[L]);
        ClosedSettled &= MAYALatchedDoorDrive::HasSettledClosure(bDoorPhysicalStateKnown[L], E.FullDoorAngleRad[L], DoorOmegaRadps[L]);
        Open &= FMath::Abs(DoorAngleRad[L] - Target) <= OpenAngleToleranceRad && FMath::Abs(DoorOmegaRadps[L]) <= OpenOmegaRadps;
        bool ActualLocked = false;
        const auto Ref = Joints[I]->ConstraintInstance.GetPhysicsConstraintRef();
        if (Ref.IsValid())
            FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref, [&](const FPhysicsConstraintHandle& Locked)
            {
                if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                    || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
                const auto* Actual = static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);
                E.NativeLockReadKnown[L] = true;
                ActualLocked = DDSJointSettingsMatch(Actual->GetJointSettings(), I,
                    JointInChassis[I], JointInPart[I], 0, 0, true, MotorProfile());
            });
        E.ActualNativeClosedLock[L] = ActualLocked;
        BothActualLocks &= ActualLocked;
    }
    if (DoorState == EDoorState::Opening && Open) DoorState = EDoorState::Open;
    if (DoorState == EDoorState::Closing && ClosedSettled)
    {
        ConfigureDoorJoint(0, true); ConfigureDoorJoint(1, true);
        DoorState = EDoorState::ClosedLatched; ClearInput();
    }
    if ((DoorState == EDoorState::Opening || DoorState == EDoorState::Closing)
        && GetWorld()->GetAudioTimeSeconds() - DoorPhaseStartActiveRealS > DoorTimeoutS)
    { Fail(TEXT("Physical lid travel exceeded declared five-second timeout")); return; }
    // Settled relative rate confirms closing/service; an already latched lid
    // drives on unchanged full closed pose and actual native physical locks.
    bDriveInhibited = MAYALatchedDoorDrive::DoorDriveInhibited(bResetStabilizing,
        DoorState == EDoorState::ClosedLatched, ClosedPose, BothActualLocks);
    E.bDoorDecisionKnown = true; E.DoorDecisionRealS = FPlatformTime::Seconds();
    E.bClosedPose = ClosedPose; E.bClosedSettled = ClosedSettled; E.bClosedLatched = DoorState == EDoorState::ClosedLatched;
    E.bResetStabilizing = bResetStabilizing; E.bDoorDriveInhibited = bDriveInhibited;
}
void AMAYANativeDemoPawn::ToggleDoors()
{
    if (!CanReadLocalInput()) return;
    RequestDeliveryDoorState(DoorState == EDoorState::ClosedLatched);
}
bool AMAYANativeDemoPawn::AreDeliveryDoorsClosedAndPhysicallyLocked() const
{
    FMAYADeliveryRobotObservation O;
    if (!ReadDeliveryState(O) || !O.bKnown || !O.Fault.IsEmpty() || DoorState != EDoorState::ClosedLatched) return false;
    for (int32 L = 0; L < 2; ++L)
    {
        const int32 I = L + 6;
        const FTransform A = JointInChassis[I] * O.ChassisPose;
        const FTransform B = JointInPart[I] * Body(L + 7)->GetComponentTransform();
        if (A.ContainsNaN() || B.ContainsNaN() || !FMath::IsFinite(O.DoorOmegaRadps[L])
            || A.GetRotation().AngularDistance(B.GetRotation()) > ClosedAngleRad) return false;
        bool ActualLocked = false;
        FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadOnly(Joints[I]->ConstraintInstance.GetPhysicsConstraintRef(),
            [&](const FPhysicsConstraintHandle& Locked)
        {
            if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
            const auto* Actual = static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);
            ActualLocked = DDSJointSettingsMatch(Actual->GetJointSettings(), I,
                JointInChassis[I], JointInPart[I], 0, 0, true, MotorProfile());
        });
        if (!ActualLocked) return false;
    }
    return true;
}

bool AMAYANativeDemoPawn::CanRequestDeliveryDoorState(bool bOpen) const
{
    if (!bReady || bEnding || UGameplayStatics::IsGamePaused(this)) return false;
    if ((bOpen && DoorState == EDoorState::Open) || (!bOpen && DoorState == EDoorState::ClosedLatched)) return true;
    if (bOpen && DoorState == EDoorState::ClosedLatched)
        return GetWorld() && SupportStableSinceActiveRealS >= 0
            && GetWorld()->GetAudioTimeSeconds() - SupportStableSinceActiveRealS >= StableSupportS;
    return !bOpen && DoorState == EDoorState::Open;
}
bool AMAYANativeDemoPawn::RequestDeliveryDoorState(bool bOpen)
{
    if (!CanRequestDeliveryDoorState(bOpen)) return false;
    if ((bOpen && DoorState == EDoorState::Open) || (!bOpen && DoorState == EDoorState::ClosedLatched)) return true;
    if (bOpen && DoorState == EDoorState::ClosedLatched)
    {
        ClearInput(); bDriveInhibited = true;
        ConfigureDoorJoint(0, false); ConfigureDoorJoint(1, false); DoorState = EDoorState::Opening;
    }
    else if (!bOpen && DoorState == EDoorState::Open) { ClearInput(); bDriveInhibited = true; DoorState = EDoorState::Closing; }
    else return false;
    DoorPhaseStartRealS = FPlatformTime::Seconds(); // Legacy absolute diagnostic clock.
    // LevelTick.cpp advances AudioTimeSeconds only while unpaused, before
    // dilation/clamping. The declared five-second active-real-time limit remains.
    DoorPhaseStartActiveRealS = GetWorld()->GetAudioTimeSeconds();
    return true;
}
bool AMAYANativeDemoPawn::ConfigureWheelBrake(int32 Wheel, bool bEnabled)
{
    if (Wheel < 0 || Wheel >= 6 || !Joints.IsValidIndex(Wheel) || Joints[Wheel]->IsBroken()) return false;
    auto* J = Joints[Wheel].Get();
    // Keep editor-facing intent consistent. Higher-level angular setters implicitly
    // select Acceleration and scale damping, so set the exact native Force route below.
    auto& D = J->ConstraintInstance.ProfileInstance.AngularDrive;
    D.AngularDriveMode = EAngularDriveMode::TwistAndSwing;
    D.AngularVelocityTarget = FVector::ZeroVector;
    D.TwistDrive.bEnablePositionDrive = false; D.TwistDrive.bEnableVelocityDrive = bEnabled;
    D.SwingDrive.bEnablePositionDrive = false; D.SwingDrive.bEnableVelocityDrive = false;
    D.SlerpDrive.bEnablePositionDrive = false; D.SlerpDrive.bEnableVelocityDrive = false;
    D.TwistDrive.Stiffness = 0; D.TwistDrive.Damping = float(NativeBrakeDampingUE());
    D.TwistDrive.MaxForce = float(MotorProfile().BrakePeakNm * NmToUE);
    const auto Ref = J->ConstraintInstance.GetPhysicsConstraintRef();
    bool Verified = false;
    FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(Ref, [&](const FPhysicsConstraintHandle& Locked)
    {
        if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
            || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
        auto* Actual = static_cast<Chaos::FJointConstraint*>(Locked.Constraint);
        if (!WheelBrakeSettingsMatch(Actual->GetJointSettings(), bEnabled))
        {
            Actual->SetAngularDriveForceMode(Chaos::EJointForceMode::Force);
            Actual->SetAngularTwistPositionDriveEnabled(false); Actual->SetAngularSwingPositionDriveEnabled(false);
            Actual->SetAngularSLerpPositionDriveEnabled(false); Actual->SetAngularTwistVelocityDriveEnabled(bEnabled);
            Actual->SetAngularSwingVelocityDriveEnabled(false); Actual->SetAngularSLerpVelocityDriveEnabled(false);
            Actual->SetAngularDriveVelocityTarget(Chaos::FVec3(0));
            Actual->SetAngularDriveStiffness(Chaos::FVec3(0));
            Actual->SetAngularDriveDamping(Chaos::FVec3(NativeBrakeDampingUE(), 0, 0));
            Actual->SetAngularDriveMaxTorque(Chaos::FVec3(MotorProfile().BrakePeakNm * NmToUE, 0, 0));
        }
        Verified = WheelBrakeSettingsMatch(Actual->GetJointSettings(), bEnabled);
    });
    return Verified;
}
void AMAYANativeDemoPawn::ApplyPhysicalTorques()
{
    if (!bNativePreviousBoostProfile || bWheelExternalWritesStopped)
    {
        DrainWheelFeedback(); PublishWheelFeedbackCommand(); ProgressWheelFeedbackSelection();
        ApplyPhysicalDoorTorques(HasFreshDriveCommand());
        return; // No external production wheel motor/bearing or brake-setting writes.
    }
    const auto P = MotorProfile();
    const bool Fresh = HasFreshDriveCommand();
    bool Drive = MAYALatchedDoorDrive::MotorDriveAdmitted(Fresh, bBrake, bDriveInhibited, bDeliveryDriveLocked, bInputsNeedRelease);
    auto& E = DriveAdmissionEvidence;
    E.bMotorDecisionKnown = true; E.MotorFrame = GFrameCounter;
    E.MotorWorldS = GetWorld()->GetTimeSeconds(); E.MotorRealS = FPlatformTime::Seconds();
    E.CommandSequence = InputSequence; E.CommandIssuedRealS = InputIssuedRealS;
    E.RequestedThrottle = Throttle; E.RequestedSteer = Steer;
    E.bFresh = Fresh; E.bBrakeRequested = bBrake; E.bDoorInhibitedAtMotorDecision = bDriveInhibited; E.bDeliveryLocked = bDeliveryDriveLocked; E.bInputsNeedRelease = bInputsNeedRelease;
    E.bBoostRequested = bBoostRequested; E.bPreviousBoostProfile = bNativePreviousBoostProfile;
    // Only the accepted wheel-drive envelope changes. Native brake, bearing,
    // lid torque, collision, masses, world time and all interlocks retain P.
    const bool Boost = Drive && bBoostRequested && DoorState == EDoorState::ClosedLatched;
    E.bReserveSpeedSampleAttempted = Boost && !bNativePreviousBoostProfile;
    E.bReserveNativeBodyAvailable = E.bReserveSpeedKnown = false;
    E.ReserveChassisVelocityCmps = FVector::ZeroVector; E.ReserveChassisSpeedCmps = 0;
    E.ReserveSpeedSampleRealS = -1; E.LowSpeedReserveFactor = 0;
    const auto RoadBoostProfile = MAYADeliveryBoostProfile::Apply(P, true, false);
    E.ConfiguredCurrentBoostRoadPeakNm = RoadBoostProfile.MotorPeakNm;
    E.ConfiguredCurrentBoostMaximumReservePeakNm = RoadBoostProfile.MotorPeakNm + MAYADeliveryBoostProfile::LowSpeedReserveNm;
    if (E.bReserveSpeedSampleAttempted)
    {
        const FBodyInstance* ChassisBody = Body(0) ? Body(0)->GetBodyInstance() : nullptr;
        E.bReserveNativeBodyAvailable = ChassisBody && ChassisBody->IsValidBodyInstance();
        if (E.bReserveNativeBodyAvailable)
        {
            const FVector NativeVelocityCmps = Body(0)->GetPhysicsLinearVelocity();
            const double NativeSpeedCmps = NativeVelocityCmps.Size();
            E.bReserveSpeedKnown = FMath::IsFinite(NativeVelocityCmps.X) && FMath::IsFinite(NativeVelocityCmps.Y)
                && FMath::IsFinite(NativeVelocityCmps.Z) && FMath::IsFinite(NativeSpeedCmps) && NativeSpeedCmps >= 0;
            if (E.bReserveSpeedKnown) { E.ReserveChassisVelocityCmps = NativeVelocityCmps; E.ReserveChassisSpeedCmps = NativeSpeedCmps; }
        }
        E.ReserveSpeedSampleRealS = FPlatformTime::Seconds();
        E.LowSpeedReserveFactor = MAYADeliveryBoostProfile::CurrentBoostReserveFactor(E.bReserveSpeedKnown, E.ReserveChassisSpeedCmps);
    }
    const auto DriveProfile = MAYADeliveryBoostProfile::ApplyAtChassisSpeed(P, Boost, bNativePreviousBoostProfile, E.bReserveSpeedKnown, E.ReserveChassisSpeedCmps);
    E.MotorPeakNm = DriveProfile.MotorPeakNm; E.MotorNoLoadRadps = DriveProfile.MotorNoLoadRadps;
    E.MotorErrorNmPerRadps = DriveProfile.MotorErrorNmPerRadps;
    E.bCurrentBoostPolicyEligible = Boost && !bNativePreviousBoostProfile;
    bBoostMotorApplied = false;
    for (int32 W = 0; W < 6; ++W)
    {
        const FTransform Frame = JointInChassis[W] * Body(0)->GetComponentTransform();
        const FVector Axis = Frame.GetUnitAxis(EAxis::X);
        const double Omega = FVector::DotProduct(Body(W + 1)->GetPhysicsAngularVelocityInRadians()
            - Body(0)->GetPhysicsAngularVelocityInRadians(), Axis);
        // Optical forward is chassis -X; all axle axes are +Y; W < 3 is optical left.
        const double Request = FMath::Clamp(double(-Throttle + (W < 3 ? -Steer : Steer)), -1.0, 1.0);
        // Evidence copies exact operands and the shared current-Boost policy decision.
        E.RelativeWheelOmegaRadps[W] = Omega; E.UnclampedBankRequest[W] = double(-Throttle + (W < 3 ? -Steer : Steer)); E.ClampedBankRequest[W] = Request;
        E.WheelInputSampleRealS[W] = FPlatformTime::Seconds(); E.WheelInputSampled[W] = true;
        const auto MotorDecision = MAYADeliveryBoostProfile::EvaluateMotorTorque(Request, Omega, DriveProfile, Boost, bNativePreviousBoostProfile, Drive);
        E.WheelDriveAdmitted[W] = Drive; E.OriginalLawMotorNm[W] = MotorDecision.OriginalLawNm;
        E.CounterTorqueSuppressed[W] = MotorDecision.bCounterTorqueSuppressed; E.MotorNmAfterPolicy[W] = MotorDecision.AfterPolicyNm; E.ExpectedAppliedMotorNm[W] = MotorDecision.AppliedNm;
        MotorNm[W] = MotorDecision.AppliedNm;
        E.VerifiedNativeBrakeEnabled[W] = false;
        if (!ConfigureWheelBrake(W, bBrake || !Drive || !bReady))
        {
            Fail(TEXT("Native wheel brake configuration invalid")); Drive = false;
            E.bMotorDecisionKnown = false; E.AppliedMotorNm[W] = E.PassiveBearingNm[W] = 0;
            MotorNm[W] = BrakeNm[W] = 0; continue; // Do not send torque through an unknown/broken joint.
        }
        // No explicit active-brake torque: the capped native drive evaluates its own
        // current solver velocity. This field now contains only unchanged passive bearing.
        BrakeNm[W] = DDSBrakeBearingTorqueNm(false, Omega, P);
        E.VerifiedNativeBrakeEnabled[W] = bBrake || !Drive || !bReady;
        E.AppliedMotorNm[W] = MotorNm[W]; E.PassiveBearingNm[W] = BrakeNm[W];
        TorquePair(Body(W + 1), Body(0), Axis * (MotorNm[W] + BrakeNm[W]));
    }
    if (Boost && Drive && HasFreshDriveCommand())
        for (const double Nm : MotorNm)
            bBoostMotorApplied |= FMath::IsFinite(Nm) && FMath::Abs(Nm) > 1.e-12;
    E.bDriveAdmitted = Drive; E.bBoostMotorApplied = bBoostMotorApplied;
    E.bBoostDriveActiveAtMotorDecision = IsBoostDriveActive();
    E.ActualBoostEffortFractionAtMotorDecision = MAYADeliveryBoostProfile::ActualMotorEffortFraction(MotorNm, E.MotorPeakNm, E.bBoostDriveActiveAtMotorDecision);
    ApplyPhysicalDoorTorques(Fresh);
}
void AMAYANativeDemoPawn::ApplyPhysicalDoorTorques(bool Fresh)
{
    if (DoorState == EDoorState::ClosedLatched) return;
    for (int32 L = 0; L < 2; ++L)
    {
        DoorMotorNm[L] = 0;
        if (!bDoorPhysicalStateKnown[L]) continue; // Explicit zero lid torque for broken/unknown state.
        // Focus loss/fault damps CURRENT relative motion plus CURRENT gravity compensation.
        // The fault policy cannot change phase/readiness and never pursues the old opening target.
        const double Target = !Fresh || DoorState == EDoorState::Fault ? DoorAngleRad[L]
            : DoorState == EDoorState::Closing ? 0 : FMath::DegreesToRadians(L == 0 ? Door1OpenDegrees : Door2OpenDegrees);
        DoorMotorNm[L] = FMath::Clamp(-DoorGravityNm[L]
            + double(LidKpNmPerRad) * (Target - DoorAngleRad[L])
            - double(LidKdNmPerRadps) * DoorOmegaRadps[L], -double(LidPeakNm), double(LidPeakNm));
        const FVector Axis = (JointInChassis[L + 6] * Body(0)->GetComponentTransform()).GetUnitAxis(EAxis::X);
        TorquePair(Body(L + 7), Body(0), Axis * DoorMotorNm[L]);
    }
}
void AMAYANativeDemoPawn::Tick(float Dt)
{
    Super::Tick(Dt); if (!bPhysicalAssembly || bEnding) return;
    SampleDoorPhysicalState(); // Independent of bReady, including joint-fault and timeout paths below.
    for (int32 I = 0; I < 8; ++I)
        if (!Joints.IsValidIndex(I) || Joints[I]->IsBroken())
        {
            Fail(TEXT("Physical joint broken/absent"));
            if (!UGameplayStatics::IsGamePaused(this)) ApplyPhysicalTorques();
            return;
        }
    PollInput(); ObserveSupport(); ObserveNativeCargoLanding();
    if (bReady) ObserveDoors();
    if (UGameplayStatics::IsGamePaused(this)) return;
    ApplyPhysicalTorques();
}
void AMAYANativeDemoPawn::OnResetKey() { if (CanReadLocalInput()) ResetDemo(); }
void AMAYANativeDemoPawn::ObserveNativeCargoLanding()
{
    // Game60 traversed a real17cm curb with60cm/s downward motion. Temporary
    // all-six reacquisition split it into7.09cm and4.80cm observation windows.
    // Keep one measured multi-axle episode until full support persists0.5s.
    // This observer does not change the drive/service support predicate.
    constexpr double LandingRegainSettleS = 0.5, LandingEpisodeLimitS = 5.0;
    const auto ClearEpisode = [&]()
    {
        bLandingSupportLost = bLandingEpisodeEmitted = false;
        LandingLossStartWorldS = LandingCurrentLossStartWorldS = LandingSupportRegainedWorldS = -1;
        LandingObservedLossWorldS = LandingPeakChassisZ = LandingPeakDownCmps = 0;
    };
    if (!GetWorld() || UGameplayStatics::IsGamePaused(this) || !bReady || bResetStabilizing || bEnding || !IsValid(Body(0)))
    { bLandingSupportInitialized = false; ClearEpisode(); return; }
    const double Now = GetWorld()->GetTimeSeconds();
    const double Z = Body(0)->GetComponentLocation().Z;
    const FVector V = Body(0)->GetPhysicsLinearVelocity();
    if (!FMath::IsFinite(Now) || !FMath::IsFinite(Z) || V.ContainsNaN())
    { bLandingSupportInitialized = false; ClearEpisode(); return; }
    const bool Supported = SupportMask == 0x3f;
    if (!bLandingSupportInitialized)
    { if (Supported) bLandingSupportInitialized = true; return; }
    if (!bLandingSupportLost)
    {
        if (Supported) return;
        bLandingSupportLost = true; bLandingEpisodeEmitted = false;
        LandingLossStartWorldS = LandingCurrentLossStartWorldS = Now;
        LandingSupportRegainedWorldS = -1; LandingObservedLossWorldS = 0;
        LandingPeakChassisZ = Z; LandingPeakDownCmps = 0;
    }
    LandingPeakChassisZ = FMath::Max(LandingPeakChassisZ, Z);
    LandingPeakDownCmps = FMath::Max(LandingPeakDownCmps, FMath::Max(0.0, -V.Z));
    if (Now - LandingLossStartWorldS > LandingEpisodeLimitS)
    { ClearEpisode(); bLandingSupportInitialized = Supported; return; }
    if (!Supported)
    {
        if (LandingCurrentLossStartWorldS < 0) LandingCurrentLossStartWorldS = Now;
        LandingSupportRegainedWorldS = -1; return;
    }
    if (LandingCurrentLossStartWorldS >= 0)
    {
        LandingObservedLossWorldS += FMath::Max(0.0, Now - LandingCurrentLossStartWorldS);
        LandingCurrentLossStartWorldS = -1;
    }
    if (LandingSupportRegainedWorldS < 0) LandingSupportRegainedWorldS = Now;
    const double Drop = LandingPeakChassisZ - Z;
    if (!bLandingEpisodeEmitted && LandingObservedLossWorldS >= 0.1 && Drop >= 8.0 && LandingPeakDownCmps >= 40.0
        && (LastLandingEventWorldS < 0 || Now - LastLandingEventWorldS >= 0.5))
    {
        double SumImpulseKgCmps = 0, LargestImpulseKgCmps = 0;
        UPrimitiveComponent* Support = nullptr;
        const double RealNow = FPlatformTime::Seconds();
        for (const auto& Contact : Contacts)
            if (Contact.Support.IsValid() && Contact.IssuedRealS >= 0
                && RealNow >= Contact.IssuedRealS && RealNow - Contact.IssuedRealS <= 0.1
                && FMath::IsFinite(Contact.ImpulseKgCmps) && Contact.ImpulseKgCmps > 0)
            {
                SumImpulseKgCmps += Contact.ImpulseKgCmps;
                if (Contact.ImpulseKgCmps > LargestImpulseKgCmps)
                { LargestImpulseKgCmps = Contact.ImpulseKgCmps; Support = Contact.Support.Get(); }
            }
        if (Support && SumImpulseKgCmps > 0)
        {
            LastNativeImpact = FMAYADeliveryImpactEvent();
            LastNativeImpact.Generation = ++NativeImpactGeneration;
            LastNativeImpact.Kind = TEXT("CurbLanding"); LastNativeImpact.WorldSeconds = Now;
            LastNativeImpact.ImpulseKgMps = SumImpulseKgCmps / 100.0;
            LastNativeImpact.PreContactDownCmps = LandingPeakDownCmps; LastNativeImpact.DropCm = Drop;
            LastNativeImpact.OtherComponent = Support->GetPathName();
            LastNativeImpact.OtherActor = GetPathNameSafe(Support->GetOwner());
            LastLandingEventWorldS = Now; bLandingEpisodeEmitted = true;
        }
    }
    if (Now - LandingSupportRegainedWorldS >= LandingRegainSettleS) ClearEpisode();
}

bool AMAYANativeDemoPawn::ReadDeliveryState(FMAYADeliveryRobotObservation& Out) const
{
    Out = FMAYADeliveryRobotObservation(); Out.Fault = Fault; Out.ResetGeneration = ResetGeneration; Out.LastNativeImpact = LastNativeImpact;
    if (!GetWorld() || !bPhysicalAssembly || bEnding || !IsValid(Body(0))
        || !Body(0)->IsSimulatingPhysics() || Joints.Num() != 8) return false;
    Out.ChassisPose = Body(0)->GetComponentTransform();
    Out.VelocityCmps = Body(0)->GetPhysicsLinearVelocity();
    Out.AngularVelocityRadps = Body(0)->GetPhysicsAngularVelocityInRadians();
    Out.ObservationWorldSeconds = GetWorld()->GetTimeSeconds();
    Out.OpticalForwardCmps = FVector::DotProduct(Out.VelocityCmps, -Out.ChassisPose.GetUnitAxis(EAxis::X));
    bool Closed = true, Open = true, Locks = true, Known = true;
    for (int32 W = 0; W < 6; ++W)
        if (HasObservedWheelSupport(W)) Out.SupportMask |= 1 << W;
    for (int32 I = 0; I < 8; ++I)
        Known &= IsValid(Joints[I].Get()) && !Joints[I]->IsBroken()
            && Joints[I]->ConstraintInstance.GetPhysicsConstraintRef().IsValid();
    for (int32 L = 0; L < 2 && Known; ++L)
    {
        const int32 I = L + 6;
        const FTransform A = JointInChassis[I] * Out.ChassisPose;
        const FTransform B = JointInPart[I] * Body(L + 7)->GetComponentTransform();
        const double Angle = MayaNativeTwist(A.GetRotation(), B.GetRotation());
        const double Omega = FVector::DotProduct(Body(L + 7)->GetPhysicsAngularVelocityInRadians()
            - Out.AngularVelocityRadps, A.GetUnitAxis(EAxis::X));
        Out.DoorAngleDegrees[L] = FMath::RadiansToDegrees(Angle); Out.DoorOmegaRadps[L] = Omega;
        Known &= !A.ContainsNaN() && !B.ContainsNaN() && FMath::IsFinite(Angle) && FMath::IsFinite(Omega);
        Closed &= A.GetRotation().AngularDistance(B.GetRotation()) <= ClosedAngleRad && FMath::Abs(Omega) <= ClosedOmegaRadps;
        Open &= FMath::Abs(Angle - FMath::DegreesToRadians(L == 0 ? Door1OpenDegrees : Door2OpenDegrees)) <= OpenAngleToleranceRad
            && FMath::Abs(Omega) <= OpenOmegaRadps;
        bool ActualLocked = false;
        FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadOnly(Joints[I]->ConstraintInstance.GetPhysicsConstraintRef(),
            [&](const FPhysicsConstraintHandle& Locked)
        {
            if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
            const auto* Actual = static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);
            ActualLocked = DDSJointSettingsMatch(Actual->GetJointSettings(), I, JointInChassis[I], JointInPart[I], 0, 0, true, MotorProfile());
        });
        Locks &= ActualLocked;
    }
    Out.bKnown = Known && !Out.ChassisPose.ContainsNaN() && !Out.VelocityCmps.ContainsNaN()
        && !Out.AngularVelocityRadps.ContainsNaN();
    Out.bReady = bReady && !bResetStabilizing;
    Out.bGrounded = Out.SupportMask == 0x3f;
    const FRotator R = Out.ChassisPose.Rotator();
    Out.bUpright = FMath::Abs(R.Pitch) <= 8 && FMath::Abs(R.Roll) <= 8;
    Out.bDoorsOpen = Out.bKnown && DoorState == EDoorState::Open && Open;
    Out.bDoorsClosedLatched = Out.bKnown && DoorState == EDoorState::ClosedLatched && Closed && Locks;
    Out.bFreshMovementIntent = Out.bKnown && HasFreshMovementIntent();
    Out.bBoostDriveActive = Out.bKnown && IsBoostDriveActive();
    Out.BoostMotorEffortFraction = Out.bBoostDriveActive ? ReadBoostMotorEffortFraction() : 0.0;
    return Out.bKnown;
}

void AMAYANativeDemoPawn::SetDeliveryDriveLocked(bool bLocked)
{
    if (bDeliveryDriveLocked == bLocked) return;
    bDeliveryDriveLocked = bLocked;
    if (bLocked) ClearInput();
    PublishWheelFeedbackCommand();
}

UPrimitiveComponent* AMAYANativeDemoPawn::GetDeliveryCargoMount(int32 Slot) const
{
    return Slot == 0 ? CargoMount.Get() : Slot == 1 ? RearCargoMount.Get() : nullptr;
}

bool AMAYANativeDemoPawn::GetDeliveryCargoPlacementTarget(UPrimitiveComponent* Parcel, int32 Slot, FTransform& Out) const
{
    Out = FTransform::Identity;
    const UPrimitiveComponent* Mount = GetDeliveryCargoMount(Slot);
    if (!IsValid(Parcel) || !IsValid(Mount) || !IsValid(Body(0))) return false;
    const FTransform Chassis = Body(0)->GetComponentTransform();
    const FVector Scale = Parcel->GetComponentScale();
    if (Chassis.ContainsNaN() || !Chassis.GetRotation().IsNormalized()
        || !Chassis.GetScale3D().Equals(FVector::OneVector, 1.e-6)
        || Scale.ContainsNaN() || !Scale.Equals(FVector(0.24, 0.18, 0.12), 1.e-6)) return false;
    Out = FTransform(Chassis.GetRotation(), Mount->GetComponentLocation(), Scale);
    return !Out.ContainsNaN();
}

bool AMAYANativeDemoPawn::AdmitDeliveryCargoPlacement(UPrimitiveComponent* Parcel, int32 Slot) const
{
    CargoTransferStatus = TEXT("Instant cargo placement refused: physical service or slot shape unavailable");
    FMAYADeliveryRobotObservation O; FTransform Target;
    const bool ProfileLoaded = bDeliveryCollisionProfileLoaded;
    const bool DriveLocked = bDeliveryDriveLocked;
    const bool Paused = UGameplayStatics::IsGamePaused(this);
    const bool ReadKnown = ReadDeliveryState(O);
    const bool StoppedSupported = ReadKnown && O.IsStoppedAndSupported();
    const bool OpenMeasured = ReadKnown && O.bDoorsOpen;
    const bool TargetKnown = GetDeliveryCargoPlacementTarget(Parcel, Slot, Target);
    if (!ProfileLoaded || !DriveLocked || Paused || !ReadKnown || !StoppedSupported || !OpenMeasured || !TargetKnown)
    {
        CargoTransferStatus = FString::Printf(TEXT("Instant cargo placement gate refused: profile=%s; driveLocked=%s; paused=%s; readKnown=%s; stoppedSupported=%s; openMeasured=%s; targetKnown=%s; slot=%d; world=%.9f; known=%s; ready=%s; support=%u; upright=%s; speed=%.9f; angularSpeed=%.9f; doorAngles=(%.9f,%.9f); doorRates=(%.9f,%.9f); fault=%s; parcel=%s; parcelScale=%s"),
            ProfileLoaded ? TEXT("true") : TEXT("false"), DriveLocked ? TEXT("true") : TEXT("false"),
            Paused ? TEXT("true") : TEXT("false"), ReadKnown ? TEXT("true") : TEXT("false"),
            StoppedSupported ? TEXT("true") : TEXT("false"), OpenMeasured ? TEXT("true") : TEXT("false"),
            TargetKnown ? TEXT("true") : TEXT("false"), Slot, O.ObservationWorldSeconds,
            O.bKnown ? TEXT("true") : TEXT("false"), O.bReady ? TEXT("true") : TEXT("false"), uint32(O.SupportMask),
            O.bUpright ? TEXT("true") : TEXT("false"), O.VelocityCmps.Size(), O.AngularVelocityRadps.Size(),
            O.DoorAngleDegrees[0], O.DoorAngleDegrees[1], O.DoorOmegaRadps[0], O.DoorOmegaRadps[1],
            *O.Fault, *GetPathNameSafe(Parcel), IsValid(Parcel) ? *Parcel->GetComponentScale().ToString() : TEXT("unavailable"));
        return false;
    }
    // Same immutable physical cube envelope is also usable for a dormant box:
    // its actor/ID remain, but its physics is deliberately removed during the
    // user-authorized disappearance. Query before recreating its native body.
    auto* MeshComponent = Cast<UStaticMeshComponent>(Parcel);
    const UBodySetup* Setup = MeshComponent ? MeshComponent->GetBodySetup() : nullptr;
    if (!Setup || !MeshComponent->GetStaticMesh()
        || MeshComponent->GetStaticMesh()->GetPathName() != TEXT("/Engine/BasicShapes/Cube.Cube")
        || Setup->AggGeom.GetElementCount() != 1 || Setup->CollisionTraceFlag == CTF_UseComplexAsSimple
        || Parcel->GetCollisionObjectType() != ECC_PhysicsBody)
    {
        CargoTransferStatus = FString::Printf(TEXT("Instant cargo shape identity refused: mesh=%s; setup=%s; elements=%d; traceFlag=%d; objectType=%d; expectedObjectType=%d; boxCount=%d; convexCount=%d; sphereCount=%d; sphylCount=%d; slot=%d"),
            *GetPathNameSafe(MeshComponent ? MeshComponent->GetStaticMesh() : nullptr), *GetPathNameSafe(Setup),
            Setup ? Setup->AggGeom.GetElementCount() : -1, Setup ? int32(Setup->CollisionTraceFlag.GetValue()) : -1,
            int32(Parcel->GetCollisionObjectType()), int32(ECC_PhysicsBody), Setup ? Setup->AggGeom.BoxElems.Num() : -1,
            Setup ? Setup->AggGeom.ConvexElems.Num() : -1, Setup ? Setup->AggGeom.SphereElems.Num() : -1,
            Setup ? Setup->AggGeom.SphylElems.Num() : -1, Slot);
        return false;
    }
    FBox Envelope(ForceInit);
    const TCHAR* EnvelopeBasis = TEXT("single_convex_per_axis_AggGeom_CalcAABB");
    if (Setup->AggGeom.BoxElems.Num() == 1)
    {
        // UE5.3 aggregate CalcAABB applies the MINIMUM scale to FKBoxElem.
        // Native Chaos box creation instead calls this exact per-axis helper
        // (ChaosInterfaceUtils.cpp), so validate that physical shape definition.
        const FKBoxElem Scaled = Setup->AggGeom.BoxElems[0].GetFinalScaled(Target.GetScale3D(), FTransform::Identity);
        const FTransform ShapePose = Scaled.GetTransform();
        if (ShapePose.ContainsNaN() || !ShapePose.GetRotation().IsNormalized()
            || !ShapePose.GetLocation().Equals(FVector::ZeroVector, 0.001)
            || ShapePose.GetRotation().AngularDistance(FQuat::Identity) > 1.e-6
            || !FMath::IsFinite(Scaled.X) || !FMath::IsFinite(Scaled.Y) || !FMath::IsFinite(Scaled.Z)
            || Scaled.X <= 0 || Scaled.Y <= 0 || Scaled.Z <= 0)
        {
            CargoTransferStatus = FString::Printf(TEXT("Instant cargo box definition refused: localPose=%s; scaledLengths=(%.9f,%.9f,%.9f); required=centered axis-aligned positive finite single native box; slot=%d"),
                *ShapePose.ToString(), double(Scaled.X), double(Scaled.Y), double(Scaled.Z), Slot);
            return false;
        }
        Envelope = Scaled.CalcAABB(FTransform::Identity, 1.0f);
        EnvelopeBasis = TEXT("FKBoxElem_GetFinalScaled_per_axis_native_Chaos_box");
    }
    else if (Setup->AggGeom.ConvexElems.Num() == 1)
        Envelope = Setup->AggGeom.CalcAABB(FTransform(FQuat::Identity, FVector::ZeroVector, Target.GetScale3D()));
    else
    { CargoTransferStatus = TEXT("Instant cargo shape type refused: single box or convex physical cube required"); return false; }
    if (!Envelope.IsValid || Envelope.Min.ContainsNaN() || Envelope.Max.ContainsNaN()
        || !Envelope.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Envelope.GetExtent().Equals(FVector(12, 9, 6), 0.001))
    {
        CargoTransferStatus = FString::Printf(TEXT("Instant cargo exact envelope refused: valid=%d; min=%s; max=%s; center=%s; extent=%s; expectedExtent=(12,9,6); toleranceCm=0.001; scale=%s; slot=%d"),
            int32(Envelope.IsValid), *Envelope.Min.ToString(), *Envelope.Max.ToString(),
            *Envelope.GetCenter().ToString(), *Envelope.GetExtent().ToString(), *Target.GetScale3D().ToString(), Slot);
        return false;
    }
    // Retain this shape observation through read-only invalid-slot probes.
    // This historical summary field is not a continuous transfer certificate.
    CargoTransferPathSummary = FString::Printf(TEXT("Instant endpoint shape observation only: basis=%s; mesh=%s; boxCount=%d; convexCount=%d; scale=%s; center=%s; extent=%s; fixedExpectedCm=(24,18,12); no swept path certificate"),
        EnvelopeBasis, *MeshComponent->GetStaticMesh()->GetPathName(), Setup->AggGeom.BoxElems.Num(), Setup->AggGeom.ConvexElems.Num(),
        *Target.GetScale3D().ToString(), *Envelope.GetCenter().ToString(), *Envelope.GetExtent().ToString());
    const FVector LocalCenter = O.ChassisPose.InverseTransformPosition(Target.GetLocation());
    const FVector Min = LocalCenter - Envelope.GetExtent(), Max = LocalCenter + Envelope.GetExtent();
    // These columns were independently checked against the preserved shell and
    // partition. Z=36 is below both measured closed-lid material envelopes.
    const FVector BayMin = Slot == 0 ? FVector(-30, -16, 18) : FVector(7, -16, 21);
    const FVector BayMax = Slot == 0 ? FVector(3, 16, 36) : FVector(37, 16, 36);
    constexpr double FitMarginCm = 0.1;
    if (Min.X < BayMin.X + FitMarginCm || Min.Y < BayMin.Y + FitMarginCm || Min.Z < BayMin.Z + FitMarginCm
        || Max.X > BayMax.X - FitMarginCm || Max.Y > BayMax.Y - FitMarginCm || Max.Z > BayMax.Z - FitMarginCm)
    { CargoTransferStatus = TEXT("Instant cargo placement refused: actual box is outside the reviewed closed bay envelope"); return false; }
    FCollisionQueryParams Q(SCENE_QUERY_STAT(MAYAInstantCargoEndpoint), false, Parcel->GetOwner());
    TArray<FOverlapResult> Hits;
    GetWorld()->OverlapMultiByChannel(Hits, Target.GetLocation(), Target.GetRotation(),
        Parcel->GetCollisionObjectType(), FCollisionShape::MakeBox(Envelope.GetExtent()), Q,
        FCollisionResponseParams(Parcel->GetCollisionResponseToChannels()));
    for (const FOverlapResult& Hit : Hits) if (Hit.bBlockingHit)
    {
        CargoTransferStatus = FString::Printf(TEXT("Instant cargo slot%d endpoint blocked by %s"), Slot, *GetPathNameSafe(Hit.GetComponent()));
        return false;
    }
    CargoTransferStatus = FString::Printf(TEXT("Instant GAME slot%d endpoint clear; actual24x18x12cm box inside preserved%s bay, world/robot/other parcels queried, closed-envelope margin>=%.3fcm; no swept transfer certificate"),
        Slot, Slot == 0 ? TEXT("front optical-X") : TEXT("rear"), FitMarginCm);
    return true;
}

bool AMAYANativeDemoPawn::GetDeliveryCargoAirRemovalPoses(UPrimitiveComponent* Parcel, int32 Slot,
    FTransform& OutStart, FTransform& OutUpper, double& OutOpenAssemblyTopCm) const
{
    OutStart = OutUpper = FTransform::Identity; OutOpenAssemblyTopCm = 0;
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)) return false;
    OutStart = Parcel->GetComponentTransform();
    FBox Assembly(ForceInit);
    for (int32 I = 0; I < 9; ++I)
    {
        const FBodyInstance* BI = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        if (!BI || !BI->IsValidBodyInstance()) return false;
        const FBox B = BI->GetBodyBounds();
        if (!B.IsValid || B.Min.ContainsNaN() || B.Max.ContainsNaN() || B.GetSize().GetMin() <= 0) return false;
        Assembly += B;
    }
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() != 1 || Setup->AggGeom.BoxElems.Num() != 1) return false;
    const FKBoxElem Box = Setup->AggGeom.BoxElems[0].GetFinalScaled(OutStart.GetScale3D(), FTransform::Identity);
    const FBox Local = Box.CalcAABB(FTransform::Identity, 1.0f);
    if (!Local.IsValid || !Local.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Local.GetExtent().Equals(FVector(12, 9, 6), 0.001)) return false;
    const FVector E = Local.GetExtent(); const FQuat Q = OutStart.GetRotation();
    const double BottomOffset = FMath::Abs(Q.GetAxisX().Z) * E.X
        + FMath::Abs(Q.GetAxisY().Z) * E.Y + FMath::Abs(Q.GetAxisZ().Z) * E.Z;
    OutOpenAssemblyTopCm = Assembly.Max.Z; OutUpper = OutStart;
    FVector UpperPosition = OutStart.GetLocation(); UpperPosition.Z = Assembly.Max.Z + 60.96 + BottomOffset;
    if (!FMath::IsFinite(UpperPosition.Z) || UpperPosition.Z <= OutStart.GetLocation().Z) return false;
    OutUpper.SetLocation(UpperPosition);
    return AdmitDeliveryCargoAirRaising(Parcel, Slot, OutStart, OutUpper);
}

bool AMAYANativeDemoPawn::AdmitDeliveryCargoAirRaising(UPrimitiveComponent* Parcel, int32 Slot,
    const FTransform& Lower, const FTransform& Upper) const
{
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)) return false;
    FTransform MountTarget;
    if (!GetDeliveryCargoPlacementTarget(Parcel, Slot, MountTarget)
        || Lower.ContainsNaN() || Upper.ContainsNaN() || !Lower.GetRotation().IsNormalized()
        || !Upper.GetRotation().IsNormalized() || !Lower.GetScale3D().Equals(Upper.GetScale3D(), 1.e-6)
        || !Lower.GetScale3D().Equals(Parcel->GetComponentScale(), 1.e-6)
        || Lower.GetRotation().AngularDistance(Upper.GetRotation()) > 1.e-6
        || Lower.GetRotation().AngularDistance(MountTarget.GetRotation()) > FMath::DegreesToRadians(3.0)
        || FVector::Dist(Lower.GetLocation(), MountTarget.GetLocation()) > 2.0
        || FMath::Abs(Lower.GetLocation().X - Upper.GetLocation().X) > 1.e-4
        || FMath::Abs(Lower.GetLocation().Y - Upper.GetLocation().Y) > 1.e-4
        || Upper.GetLocation().Z <= Lower.GetLocation().Z)
    { CargoTransferStatus = TEXT("Air unloading refused: original slot pose drifted or lift is not fixed-orientation vertical translation"); return false; }
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() != 1 || Setup->AggGeom.BoxElems.Num() != 1) return false;
    const FKBoxElem Box = Setup->AggGeom.BoxElems[0].GetFinalScaled(Lower.GetScale3D(), FTransform::Identity);
    const FBox Local = Box.CalcAABB(FTransform::Identity, 1.0f);
    if (!Local.IsValid || !Local.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Local.GetExtent().Equals(FVector(12, 9, 6), 0.001)) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYACargoAirRaising), false, Parcel->GetOwner());
    const FCollisionResponseParams Responses(Parcel->GetCollisionResponseToChannels());
    const FCollisionShape Collision = FCollisionShape::MakeBox(Local.GetExtent());
    for (const FVector& Position : { Lower.GetLocation(), Upper.GetLocation() })
    {
        TArray<FOverlapResult> Hits;
        GetWorld()->OverlapMultiByChannel(Hits, Position, Lower.GetRotation(),
            Parcel->GetCollisionObjectType(), Collision, Query, Responses);
        for (const FOverlapResult& Hit : Hits) if (Hit.bBlockingHit)
        {
            CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d endpoint blocked by %s at%s"),
                Slot, *GetPathNameSafe(Hit.GetComponent()), *Position.ToString()); return false;
        }
    }
    TArray<FHitResult> Hits;
    GetWorld()->SweepMultiByChannel(Hits, Lower.GetLocation(), Upper.GetLocation(), Lower.GetRotation(),
        Parcel->GetCollisionObjectType(), Collision, Query, Responses);
    for (const FHitResult& Hit : Hits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
    {
        CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d upward path blocked by %s; time=%.9f; penetrating=%d; depthCm=%.9f; normal=%s"),
            Slot, *GetPathNameSafe(Hit.GetComponent()), double(Hit.Time), int32(Hit.bStartPenetrating),
            double(Hit.PenetrationDepth), *Hit.Normal.ToString()); return false;
    }
    CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d full vertical path and both endpoints clear; actual world, lids, shell and opposite parcel queried"), Slot);
    CargoTransferPathSummary = FString::Printf(TEXT("Air unloading actual upward sweep: slot=%d; lower=%s; upper=%s; actualBoxExtent=(12,9,6); same rotation and IDs; no normal teleport/removal fallback"),
        Slot, *Lower.GetLocation().ToString(), *Upper.GetLocation().ToString());
    return true;
}

bool AMAYANativeDemoPawn::ReadDeliveryCargoAirRaisingFrame(UPrimitiveComponent* Parcel, int32 Slot,
    FVector& OutNativeBayCm, double& OutOpenAssemblyTopCm) const
{
    OutNativeBayCm = FVector::ZeroVector; OutOpenAssemblyTopCm = 0;
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)) return false;
    FTransform Bay;
    const FBodyInstance* ChassisBI = Body(0) ? Body(0)->GetBodyInstance() : nullptr;
    if (!GetDeliveryCargoPlacementTarget(Parcel, Slot, Bay) || !ChassisBI || !ChassisBI->IsValidBodyInstance()) return false;
    const FTransform NativeChassis = ChassisBI->GetUnrealWorldTransform(false, true);
    const FTransform ComponentChassis = Body(0)->GetComponentTransform();
    if (NativeChassis.ContainsNaN() || !NativeChassis.GetRotation().IsNormalized()
        || ComponentChassis.ContainsNaN()) return false;
    // The mount's existing local offset is composed with the freshly read native
    // actor pose; this does not reposition the robot or change either cargo slot.
    OutNativeBayCm = NativeChassis.TransformPosition(ComponentChassis.InverseTransformPosition(Bay.GetLocation()));
    FBox Assembly(ForceInit);
    for (int32 I = 0; I < 9; ++I)
    {
        const FBodyInstance* BI = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        if (!BI || !BI->IsValidBodyInstance()) return false;
        const FBox B = BI->GetBodyBounds();
        if (!B.IsValid || B.Min.ContainsNaN() || B.Max.ContainsNaN()) return false;
        Assembly += B;
    }
    OutOpenAssemblyTopCm = Assembly.Max.Z;
    return !OutNativeBayCm.ContainsNaN() && FMath::IsFinite(OutOpenAssemblyTopCm);
}

bool AMAYANativeDemoPawn::AdmitDeliveryCargoAirRaisingRemaining(UPrimitiveComponent* Parcel, int32 Slot,
    const FTransform& Upper) const
{
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)) return false;
    const FBodyInstance* BI = Parcel->GetBodyInstance();
    if (!BI || !BI->IsValidBodyInstance() || Upper.ContainsNaN() || !Upper.GetRotation().IsNormalized()
        || !Upper.GetScale3D().Equals(Parcel->GetComponentScale(), 1.e-6))
    { CargoTransferStatus = TEXT("Air unloading remaining query refused: native body or upper pose/scale unavailable"); return false; }
    FTransform Native = BI->GetUnrealWorldTransform(false, true);
    if (Native.ContainsNaN() || !Native.GetRotation().IsNormalized())
    { CargoTransferStatus = TEXT("Air unloading remaining query refused: actual native pose unavailable"); return false; }
    const double NativeFixedAngularDelta = Native.GetRotation().AngularDistance(Upper.GetRotation());
    if (NativeFixedAngularDelta > 1.e-6 || Native.GetLocation().Z > Upper.GetLocation().Z + 0.1)
    {
        CargoTransferStatus = FString::Printf(TEXT("Air unloading remaining query refused: actual native fixed orientation/height mismatch; angularDeltaRad=%.12f; limitRad=0.000001; nativeZ=%.9f; upperZ=%.9f"),
            NativeFixedAngularDelta, Native.GetLocation().Z, Upper.GetLocation().Z); return false;
    }
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() != 1 || Setup->AggGeom.BoxElems.Num() != 1)
    { CargoTransferStatus = TEXT("Air unloading remaining query refused: exact single native box shape unavailable"); return false; }
    const FKBoxElem Box = Setup->AggGeom.BoxElems[0].GetFinalScaled(Upper.GetScale3D(), FTransform::Identity);
    const FBox Local = Box.CalcAABB(FTransform::Identity, 1.0f);
    if (!Local.IsValid || !Local.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Local.GetExtent().Equals(FVector(12, 9, 6), 0.001))
    { CargoTransferStatus = TEXT("Air unloading remaining query refused: native scaled box envelope differs from its reviewed24x18x12cm shape"); return false; }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYACargoAirRaisingRemaining), false, Parcel->GetOwner());
    const FCollisionResponseParams Responses(Parcel->GetCollisionResponseToChannels());
    const FCollisionShape Collision = FCollisionShape::MakeBox(Local.GetExtent());
    for (const FVector& Position : { Native.GetLocation(), Upper.GetLocation() })
    {
        TArray<FOverlapResult> Hits;
        GetWorld()->OverlapMultiByChannel(Hits, Position, Upper.GetRotation(),
            Parcel->GetCollisionObjectType(), Collision, Query, Responses);
        for (const FOverlapResult& Hit : Hits) if (Hit.bBlockingHit)
        {
            CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d current/native or remaining upper endpoint blocked by %s at%s"),
                Slot, *GetPathNameSafe(Hit.GetComponent()), *Position.ToString()); return false;
        }
    }
    TArray<FHitResult> Hits;
    GetWorld()->SweepMultiByChannel(Hits, Native.GetLocation(), Upper.GetLocation(), Upper.GetRotation(),
        Parcel->GetCollisionObjectType(), Collision, Query, Responses);
    for (const FHitResult& Hit : Hits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
    {
        CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d actual remaining path blocked by %s; time=%.9f; penetrating=%d; depthCm=%.9f; normal=%s"),
            Slot, *GetPathNameSafe(Hit.GetComponent()), double(Hit.Time), int32(Hit.bStartPenetrating),
            double(Hit.PenetrationDepth), *Hit.Normal.ToString()); return false;
    }
    CargoTransferStatus = FString::Printf(TEXT("Air unloading slot%d native current-to-upper route clear; measured bay tracking then latched upper; world/lids/shell/opposite parcel queried"), Slot);
    CargoTransferPathSummary = FString::Printf(TEXT("Air unloading bounded measured-bay tracking: slot=%d; nativeCurrent=%s; upper=%s; same fixed orientation; actual remaining sweep and swept incremental movement; upper latched at eased progress1"),
        Slot, *Native.GetLocation().ToString(), *Upper.GetLocation().ToString());
    return true;
}

bool AMAYANativeDemoPawn::GetDeliveryCargoAirObservationBounds(FBox& OutBounds) const
{
    OutBounds = FBox(ForceInit);
    if (!GetWorld() || !Body(0) || Parts.Num() != 15) return false;
    for (int32 I = 0; I < 9; ++I)
    {
        const FBodyInstance* BI = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        if (!BI || !BI->IsValidBodyInstance()) return false;
        const FBox B = BI->GetBodyBounds();
        if (!B.IsValid || B.Min.ContainsNaN() || B.Max.ContainsNaN()) return false;
        OutBounds += B;
        if (I == 7 || I == 8)
        {
            const FVector Hinge = (JointInChassis[I - 1] * Body(0)->GetComponentTransform()).GetLocation();
            double Radius = 0;
            for (int32 Corner = 0; Corner < 8; ++Corner)
            {
                const FVector P(Corner & 1 ? B.Max.X : B.Min.X, Corner & 2 ? B.Max.Y : B.Min.Y, Corner & 4 ? B.Max.Z : B.Min.Z);
                Radius = FMath::Max(Radius, FVector::Dist(P, Hinge));
            }
            // Conservative pose-independent lid envelope for a fixed diagnostic
            // camera established before opening. Actual staging uses current OPEN
            // body bounds separately, not this visualization envelope.
            OutBounds.Max.Z = FMath::Max(OutBounds.Max.Z, Hinge.Z + Radius);
        }
    }
    OutBounds.Max.Z += 60.96 + 2.0 * FVector(12, 9, 6).Size();
    return OutBounds.IsValid && !OutBounds.Min.ContainsNaN() && !OutBounds.Max.ContainsNaN();
}

bool AMAYANativeDemoPawn::GetDeliveryCargoAirPlacementPoses(UPrimitiveComponent* Parcel, int32 Slot,
    FTransform& OutStart, FTransform& OutTarget, double& OutOpenAssemblyTopCm) const
{
    OutStart = OutTarget = FTransform::Identity; OutOpenAssemblyTopCm = 0;
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)
        || !GetDeliveryCargoPlacementTarget(Parcel, Slot, OutTarget)) return false;
    FBox AssemblyBounds(ForceInit);
    for (int32 I = 0; I < 9; ++I)
    {
        const FBodyInstance* BI = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        if (!BI || !BI->IsValidBodyInstance()) return false;
        const FBox Bounds = BI->GetBodyBounds();
        if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN()
            || Bounds.GetSize().GetMin() <= 0) return false;
        AssemblyBounds += Bounds;
    }
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() != 1 || Setup->AggGeom.BoxElems.Num() != 1) return false;
    const FKBoxElem Shape = Setup->AggGeom.BoxElems[0].GetFinalScaled(OutTarget.GetScale3D(), FTransform::Identity);
    const FBox Local = Shape.CalcAABB(FTransform::Identity, 1.0f);
    if (!Local.IsValid || !Local.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Local.GetExtent().Equals(FVector(12, 9, 6), 0.001)) return false;
    const FVector E = Local.GetExtent(); const FQuat Q = OutTarget.GetRotation();
    const double BottomOffset = FMath::Abs(Q.GetAxisX().Z) * E.X
        + FMath::Abs(Q.GetAxisY().Z) * E.Y + FMath::Abs(Q.GetAxisZ().Z) * E.Z;
    constexpr double AirBottomClearanceCm = 60.96; // exactly two feet
    OutOpenAssemblyTopCm = AssemblyBounds.Max.Z;
    OutStart = OutTarget;
    FVector Position = OutTarget.GetLocation();
    Position.Z = OutOpenAssemblyTopCm + AirBottomClearanceCm + BottomOffset;
    if (!FMath::IsFinite(Position.Z) || Position.Z <= OutTarget.GetLocation().Z) return false;
    OutStart.SetLocation(Position);
    if (!AdmitDeliveryCargoAirLowering(Parcel, Slot, OutStart, OutTarget)) return false;
    CargoTransferPathSummary = FString::Printf(TEXT("Air pickup: actualOpenAssemblyTopCm=%.9f; stagedBoxBottomClearanceCm=60.96; actualBoxBottomOffsetCm=%.9f; stageCenterZCm=%.9f; slot=%d; fadeWorldSeconds=1; descentWorldSeconds=2; fullWorldAndRobotVerticalSweep=true"),
        OutOpenAssemblyTopCm, BottomOffset, Position.Z, Slot);
    return true;
}

bool AMAYANativeDemoPawn::AdmitDeliveryCargoAirLowering(UPrimitiveComponent* Parcel, int32 Slot,
    const FTransform& Start, const FTransform& Destination) const
{
    // Reuse every actual stopped/open/drive-lock/shape/closed-bay/endpoint guard.
    if (!AdmitDeliveryCargoPlacement(Parcel, Slot)) return false;
    FTransform CurrentTarget;
    if (!GetDeliveryCargoPlacementTarget(Parcel, Slot, CurrentTarget)
        || Start.ContainsNaN() || Destination.ContainsNaN()
        || !Start.GetRotation().IsNormalized() || !Destination.GetRotation().IsNormalized()
        || !Start.GetScale3D().Equals(Destination.GetScale3D(), 1.e-6)
        || !Destination.GetScale3D().Equals(Parcel->GetComponentScale(), 1.e-6)
        || Start.GetRotation().AngularDistance(Destination.GetRotation()) > 1.e-6
        || Destination.GetRotation().AngularDistance(CurrentTarget.GetRotation()) > FMath::DegreesToRadians(0.1)
        || FVector::Dist(Destination.GetLocation(), CurrentTarget.GetLocation()) > 0.5
        || FMath::Abs(Start.GetLocation().X - Destination.GetLocation().X) > 1.e-4
        || FMath::Abs(Start.GetLocation().Y - Destination.GetLocation().Y) > 1.e-4
        || Start.GetLocation().Z + 1.e-4 < Destination.GetLocation().Z)
    { CargoTransferStatus = TEXT("Air pickup refused: latched slot pose drifted or descent is not fixed-orientation world-vertical translation"); return false; }
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() != 1 || Setup->AggGeom.BoxElems.Num() != 1)
    { CargoTransferStatus = TEXT("Air pickup refused: exact single native box required for the swept descent"); return false; }
    const FKBoxElem Shape = Setup->AggGeom.BoxElems[0].GetFinalScaled(Destination.GetScale3D(), FTransform::Identity);
    const FBox Local = Shape.CalcAABB(FTransform::Identity, 1.0f);
    if (!Local.IsValid || !Local.GetCenter().Equals(FVector::ZeroVector, 0.001)
        || !Local.GetExtent().Equals(FVector(12, 9, 6), 0.001)) return false;
    // Query the latched endpoint too; do not substitute a nearby live mount pose.
    FCollisionQueryParams Query(SCENE_QUERY_STAT(MAYACargoAirLowering), false, Parcel->GetOwner());
    const FCollisionResponseParams Responses(Parcel->GetCollisionResponseToChannels());
    const FCollisionShape Collision = FCollisionShape::MakeBox(Local.GetExtent());
    for (const FVector& Position : { Start.GetLocation(), Destination.GetLocation() })
    {
        TArray<FOverlapResult> Overlaps;
        GetWorld()->OverlapMultiByChannel(Overlaps, Position, Destination.GetRotation(),
            Parcel->GetCollisionObjectType(), Collision, Query, Responses);
        for (const FOverlapResult& Hit : Overlaps) if (Hit.bBlockingHit)
        {
            CargoTransferStatus = FString::Printf(TEXT("Air pickup slot%d endpoint blocked by %s at%s"),
                Slot, *GetPathNameSafe(Hit.GetComponent()), *Position.ToString()); return false;
        }
    }
    TArray<FHitResult> Hits;
    GetWorld()->SweepMultiByChannel(Hits, Start.GetLocation(), Destination.GetLocation(),
        Destination.GetRotation(), Parcel->GetCollisionObjectType(), Collision, Query, Responses);
    for (const FHitResult& Hit : Hits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
    {
        CargoTransferStatus = FString::Printf(TEXT("Air pickup slot%d descent blocked by %s; time=%.9f; penetrating=%d; depthCm=%.9f; normal=%s; start=%s; target=%s"),
            Slot, *GetPathNameSafe(Hit.GetComponent()), double(Hit.Time), int32(Hit.bStartPenetrating),
            double(Hit.PenetrationDepth), *Hit.Normal.ToString(), *Start.GetLocation().ToString(), *Destination.GetLocation().ToString());
        return false;
    }
    CargoTransferStatus = FString::Printf(TEXT("Air pickup slot%d actual fixed-box full vertical path and both endpoints clear; world, lids, shell and other parcels queried"), Slot);
    CargoTransferPathSummary = FString::Printf(TEXT("Air pickup actual world vertical sweep: slot=%d; start=%s; target=%s; nativeBoxExtent=(12,9,6); all world/robot/other parcel collisions queried"),
        Slot, *Start.GetLocation().ToString(), *Destination.GetLocation().ToString());
    return true;
}

bool AMAYANativeDemoPawn::GetDeliveryTransferWaypoints(UPrimitiveComponent* Parcel,
    const FTransform& Destination, TArray<FVector>& OutPoints) const
{
    OutPoints.Reset(); CargoTransferPathSummary.Empty();
    if (!IsValid(Parcel) || Destination.ContainsNaN() || !Destination.GetRotation().IsNormalized()
        || !Destination.GetScale3D().Equals(Parcel->GetComponentScale(), 1.e-6)) return false;
    const FTransform Start = Parcel->GetComponentTransform();
    if (Start.ContainsNaN() || !Start.GetRotation().IsNormalized()
        || Start.GetRotation().AngularDistance(Destination.GetRotation()) > 1.e-4) return false;
    if (!GetWorld() || Parcel->GetWorld() != GetWorld() || !Parcel->GetBodyInstance()
        || !Parcel->GetBodyInstance()->IsValidBodyInstance() || Parts.Num() != 15) return false;
    const auto ValidBounds = [](const FBox& Bounds)
    {
        return Bounds.IsValid && !Bounds.Min.ContainsNaN() && !Bounds.Max.ContainsNaN()
            && Bounds.GetSize().GetMin() > 0;
    };
    FBox AssemblyBounds(ForceInit);
    for (int32 I = 0; I < 9; ++I)
    {
        const auto* Instance = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        if (!Instance || !Instance->IsValidBodyInstance()) return false;
        const FBox Bounds = Instance->GetBodyBounds();
        if (!ValidBounds(Bounds)) return false;
        AssemblyBounds += Bounds;
    }
    // UE5.3 GetBodyBounds reads the actual native implicit under a physics read
    // lock and transforms its enclosing bounds by the current actor pose. The
    // chassis native implicit includes its six welded suspension colliders.
    const FBox ParcelBounds = Parcel->GetBodyInstance()->GetBodyBounds();
    if (!ValidBounds(AssemblyBounds) || !ValidBounds(ParcelBounds)) return false;
    // Enclose the actual parcel below its component origin even for an offset or
    // rotated collider. This is a transit plan; exact sweeps still decide admission.
    const double ParcelVerticalRadius = FMath::Max(FMath::Abs(Start.GetLocation().Z - ParcelBounds.Min.Z),
        FMath::Abs(ParcelBounds.Max.Z - Start.GetLocation().Z));
    constexpr double MayaNativeTransferClearanceCm = 2.0;
    const double RaisedZ = FMath::Max(FMath::Max(Start.GetLocation().Z, Destination.GetLocation().Z),
        AssemblyBounds.Max.Z + ParcelVerticalRadius + MayaNativeTransferClearanceCm);
    if (!FMath::IsFinite(ParcelVerticalRadius) || ParcelVerticalRadius <= 0 || !FMath::IsFinite(RaisedZ)) return false;
    CargoTransferPathSummary = FString::Printf(TEXT("actualNativeAssemblyTopCm=%.6f actualNativeParcelVerticalRadiusCm=%.6f transitCenterZCm=%.6f declaredClearanceCm=%.3f"),
        AssemblyBounds.Max.Z, ParcelVerticalRadius, RaisedZ, MayaNativeTransferClearanceCm);
    OutPoints.Add(Start.GetLocation());
    OutPoints.Add(FVector(Start.GetLocation().X, Start.GetLocation().Y, RaisedZ));
    OutPoints.Add(FVector(Destination.GetLocation().X, Destination.GetLocation().Y, RaisedZ));
    OutPoints.Add(Destination.GetLocation());
    return true;
}

bool AMAYANativeDemoPawn::AdmitDeliveryCargoTransfer(UPrimitiveComponent* Parcel, const FTransform& Destination) const
{
    CargoTransferStatus = TEXT("Transfer refused: native parcel/current service state unavailable");
    FMAYADeliveryRobotObservation O;
    if (!ReadDeliveryState(O) || !O.IsStoppedAndSupported() || O.AngularVelocityRadps.Size() > StopOmegaRadps
        || !O.bDoorsOpen || !bDeliveryDriveLocked || !IsValid(Parcel) || Parcel->GetWorld() != GetWorld()
        || Parcel->GetOwner() == this || Parcel->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics
        || !Parcel->GetBodyInstance() || !Parcel->GetBodyInstance()->IsValidBodyInstance()
        || Parcel->GetBodyInstance()->WeldParent || Parcel->IsZeroExtent()) return false;
    const UBodySetup* Setup = Parcel->GetBodySetup();
    if (!Setup || Setup->AggGeom.GetElementCount() == 0 || Setup->CollisionTraceFlag == CTF_UseComplexAsSimple)
    { CargoTransferStatus = TEXT("Transfer refused: actual simple independent parcel geometry required"); return false; }
    TArray<FVector> Points;
    if (!GetDeliveryTransferWaypoints(Parcel, Destination, Points))
    {
        const FTransform Actual = Parcel->GetComponentTransform();
        CargoTransferStatus = FString::Printf(TEXT("Transfer refused: unsupported parcel rotation/scale or nonfinite path; rotationDeltaRad=%.9g currentScale=(%.9g,%.9g,%.9g) destinationScale=(%.9g,%.9g,%.9g) currentPositionCm=(%.6f,%.6f,%.6f) destinationPositionCm=(%.6f,%.6f,%.6f)"),
            Actual.GetRotation().AngularDistance(Destination.GetRotation()), Actual.GetScale3D().X, Actual.GetScale3D().Y, Actual.GetScale3D().Z,
            Destination.GetScale3D().X, Destination.GetScale3D().Y, Destination.GetScale3D().Z,
            Actual.GetLocation().X, Actual.GetLocation().Y, Actual.GetLocation().Z,
            Destination.GetLocation().X, Destination.GetLocation().Y, Destination.GetLocation().Z);
        return false;
    }
    // Ignore only the independent parcel actor's own shapes. World, floor, shell and lids stay queried.
    FComponentQueryParams Q(SCENE_QUERY_STAT(MAYADeliveryTransfer), Parcel->GetOwner());
    Q.bTraceComplex = false;
    const FQuat Rotation = Parcel->GetComponentQuat();
    // A supported box may begin a lift with solver-scale floor contact. This
    // exception is confined to a proven convex separating prefix; it never
    // removes physical collision or ignores that floor along the actual path.
    constexpr double MayaCargoMaxSeparatingStartDepthCm = 0.001;
    constexpr double MayaCargoSeparatingProbeDistanceCm = 0.02;
    const auto SingleNativeSimpleConvex = [](UPrimitiveComponent* Component, bool bRequireStatic)
    {
        const FBodyInstance* BI = Component ? Component->GetBodyInstance() : nullptr;
        const UBodySetup* BS = Component ? Component->GetBodySetup() : nullptr;
        if (!BI || !BI->IsValidBodyInstance() || BI->WeldParent || !BS
            || BS->AggGeom.GetElementCount() != 1 || BS->CollisionTraceFlag == CTF_UseComplexAsSimple
            || (bRequireStatic && Component->Mobility != EComponentMobility::Static)) return false;
        bool bConvexKnown = false;
        const bool bRead = FPhysInterface_Chaos::ExecuteRead(BI->GetPhysicsActorHandle(), [&](const FPhysicsActorHandle& Actor)
        {
            if (!FPhysInterface_Chaos::IsValid(Actor) || (bRequireStatic && !FPhysInterface_Chaos::IsStatic(Actor))) return;
            const int32 ShapeCount = FPhysInterface_Chaos::GetNumShapes(Actor);
            if (ShapeCount < 1 || ShapeCount > 64) return;
            TArray<FPhysicsShapeHandle> Shapes;
            FPhysInterface_Chaos::GetAllShapes_AssumedLocked(Actor, Shapes);
            if (Shapes.Num() != ShapeCount) return;
            int32 SimpleCount = 0;
            bool bAllConvex = true;
            for (const FPhysicsShapeHandle& Shape : Shapes)
            {
                if (!FPhysInterface_Chaos::IsQueryShape(Shape)
                    || (FPhysInterface_Chaos::GetQueryFilter(Shape).Word3 & EPDF_SimpleCollision) == 0) continue;
                ++SimpleCount;
                const ECollisionShapeType Type = FPhysInterface_Chaos::GetShapeType(Shape);
                bAllConvex &= (Type == ECollisionShapeType::Box || Type == ECollisionShapeType::Sphere
                    || Type == ECollisionShapeType::Capsule || Type == ECollisionShapeType::Convex);
                bAllConvex &= BI->GetOriginalBodyInstance(Shape) == BI;
            }
            bConvexKnown = SimpleCount == 1 && bAllConvex;
        });
        return bRead && bConvexKnown;
    };
    for (int32 Segment = 0; Segment + 1 < Points.Num(); ++Segment)
    {
        TArray<FHitResult> Hits;
        TArray<UPrimitiveComponent*> SeparatingComponents;
        double MaximumSeparatingDepthCm = 0;
        GetWorld()->ComponentSweepMulti(Hits, Parcel, Points[Segment], Points[Segment + 1], Rotation, Q);
        for (const FHitResult& Hit : Hits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
        {
            const FVector Delta = Points[Segment + 1] - Points[Segment];
            UPrimitiveComponent* Other = Hit.GetComponent();
            const bool bSeparatingPrefix = Segment == 0 && Hit.bStartPenetrating
                && FMath::IsFinite(Hit.Time) && FMath::Abs(Hit.Time) <= 1.e-6
                && FMath::IsFinite(Hit.PenetrationDepth) && Hit.PenetrationDepth >= 0
                && Hit.PenetrationDepth <= MayaCargoMaxSeparatingStartDepthCm
                && !Hit.Normal.ContainsNaN() && !Hit.ImpactNormal.ContainsNaN()
                && FMath::Abs(Hit.Normal.SizeSquared() - 1.0) <= 0.001
                && FMath::Abs(Hit.ImpactNormal.SizeSquared() - 1.0) <= 0.001
                && Hit.Normal.Z >= 0.999 && Hit.ImpactNormal.Z >= 0.999
                && FMath::Abs(Delta.X) <= 1.e-6 && FMath::Abs(Delta.Y) <= 1.e-6
                && Delta.Z > MayaCargoSeparatingProbeDistanceCm
                && FVector::DotProduct(Delta, Hit.Normal) > Hit.PenetrationDepth + MayaCargoSeparatingProbeDistanceCm
                && SingleNativeSimpleConvex(Other, true) && SingleNativeSimpleConvex(Parcel, false);
            if (bSeparatingPrefix)
            {
                SeparatingComponents.AddUnique(Other);
                MaximumSeparatingDepthCm = FMath::Max(MaximumSeparatingDepthCm, double(Hit.PenetrationDepth));
                continue;
            }
            CargoTransferStatus = FString::Printf(TEXT("Transfer blocked segment %d by %s (penetration=%s depth=%.6fcm normal=(%.6f,%.6f,%.6f) impactNormal=(%.6f,%.6f,%.6f) startCm=(%.6f,%.6f,%.6f) endCm=(%.6f,%.6f,%.6f) parcelScale=(%.9g,%.9g,%.9g) parcelQuaternion=(%.9g,%.9g,%.9g,%.9g))"),
                Segment, *GetPathNameSafe(Hit.GetComponent()), Hit.bStartPenetrating ? TEXT("true") : TEXT("false"), Hit.PenetrationDepth,
                Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z, Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z,
                Points[Segment].X, Points[Segment].Y, Points[Segment].Z, Points[Segment + 1].X, Points[Segment + 1].Y, Points[Segment + 1].Z,
                Parcel->GetComponentScale().X, Parcel->GetComponentScale().Y, Parcel->GetComponentScale().Z,
                Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
            return false;
        }
        if (SeparatingComponents.Num())
        {
            if (SeparatingComponents.Num() > 4)
            { CargoTransferStatus = TEXT("Transfer refused: separating-contact query cap exceeded"); return false; }
            const FVector Probe = Points[Segment] + FVector(0, 0, MayaCargoSeparatingProbeDistanceCm);
            // For a single convex static shape, translation along the measured
            // separating normal cannot re-enter that shape during this prefix.
            // Query every OTHER collider through the prefix, because a sweep's
            // first initial blocking hit could otherwise hide a nearby obstacle.
            FComponentQueryParams PrefixQ = Q;
            FString ContactNames;
            for (UPrimitiveComponent* Component : SeparatingComponents)
            {
                PrefixQ.AddIgnoredComponent(Component);
                ContactNames += GetPathNameSafe(Component) + TEXT(";");
            }
            TArray<FHitResult> PrefixHits;
            GetWorld()->ComponentSweepMulti(PrefixHits, Parcel, Points[Segment], Probe, Rotation, PrefixQ);
            for (const FHitResult& Hit : PrefixHits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
            {
                CargoTransferStatus = FString::Printf(TEXT("Transfer separating prefix blocked by %s; physical collision retained"),
                    *GetPathNameSafe(Hit.GetComponent()));
                return false;
            }
            // Put the contact component back into both endpoint and full path
            // queries. These observations never move the actual parcel.
            TArray<FOverlapResult> ProbeOverlaps;
            Parcel->ComponentOverlapMulti(ProbeOverlaps, GetWorld(), Probe, Rotation, Parcel->GetCollisionObjectType(), Q);
            for (const FOverlapResult& Hit : ProbeOverlaps) if (Hit.bBlockingHit)
            {
                CargoTransferStatus = FString::Printf(TEXT("Transfer separating prefix endpoint blocked by %s; physical collision retained"),
                    *GetPathNameSafe(Hit.GetComponent()));
                return false;
            }
            TArray<FHitResult> RemainingHits;
            GetWorld()->ComponentSweepMulti(RemainingHits, Parcel, Probe, Points[Segment + 1], Rotation, Q);
            for (const FHitResult& Hit : RemainingHits) if (Hit.bBlockingHit || Hit.bStartPenetrating)
            {
                CargoTransferStatus = FString::Printf(TEXT("Transfer path after separating prefix blocked by %s (penetration=%s depth=%.9gcm); physical collision retained"),
                    *GetPathNameSafe(Hit.GetComponent()), Hit.bStartPenetrating ? TEXT("true") : TEXT("false"), Hit.PenetrationDepth);
                return false;
            }
            CargoTransferPathSummary += FString::Printf(TEXT(" separatingInitialContact={staticSingleNativeSimpleConvex=true parcelSingleNativeSimpleConvex=true maximumDepthCm=%.9g declaredMaximumDepthCm=%.6f upwardProbeCm=%.6f prefixOtherCollidersClear=true probeWholeSceneClear=true remainingWholeSceneSweepClear=true components=%s}"),
                MaximumSeparatingDepthCm, MayaCargoMaxSeparatingStartDepthCm, MayaCargoSeparatingProbeDistanceCm, *ContactNames);
        }
    }
    TArray<FOverlapResult> Overlaps;
    Parcel->ComponentOverlapMulti(Overlaps, GetWorld(), Destination.GetLocation(), Rotation,
        Parcel->GetCollisionObjectType(), Q);
    for (const FOverlapResult& Hit : Overlaps) if (Hit.bBlockingHit)
    {
        CargoTransferStatus = FString::Printf(TEXT("Transfer destination blocked by %s"), *GetPathNameSafe(Hit.GetComponent()));
        return false;
    }
    CargoTransferStatus = TEXT("Current stopped open GAME parcel path clear; no historical domain certificate; ") + CargoTransferPathSummary;
    return true;
}

bool AMAYANativeDemoPawn::CanResetDeliveryRobot() const
{
    return !bEnding && bReviewedCandidateProfile && GetWorld() && Parts.Num() == 15 && CurrentStartPoseClear();
}

bool AMAYANativeDemoPawn::ResetDeliveryRobot()
{
    ClearInput(); bDriveInhibited = true;
    if (bEnding || !bReviewedCandidateProfile || !GetWorld() || Parts.Num() != 15 || !CurrentStartPoseClear())
    { Fail(TEXT("Recovery refused: exact full-assembly start volume unavailable or obstructed")); return false; }
    if (!RetireWheelFeedback()) return false;
    ClearInput(); bReady = false; bDriveInhibited = true; bPhysicalAssembly = false; DestroyJoints();
    for (int32 S = 9; S < 15; ++S) Body(S)->UnWeldFromParent();
    for (const auto& V : Cosmetics) if (V) V->DestroyComponent();
    Cosmetics.Reset();
    for (int32 I = 0; I < 15; ++I)
    {
        auto* P = Body(I);
        P->SetSimulatePhysics(false);
        if (I > 0) P->AttachToComponent(Body(0), FAttachmentTransformRules::KeepWorldTransform);
        P->SetWorldTransform(RestInChassis[I] * StartChassisPose, false, nullptr, ETeleportType::TeleportPhysics);
        P->SetPhysicsLinearVelocity(FVector::ZeroVector); P->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
    }
    const int32 WheelSuspension[] = { 9, 10, 11, 12, 14, 13 };
    for (int32 W = 0; W < 6; ++W)
        Body(W + 1)->AttachToComponent(Body(WheelSuspension[W]), FAttachmentTransformRules::KeepWorldTransform);
    for (auto& C : Contacts) C = FContactReceipt();
    SupportMask = 0; SupportStableSinceActiveRealS = -1; DoorPhaseStartRealS = DoorPhaseStartActiveRealS = -1; bResetStabilizing = true;
    DoorState = EDoorState::ClosedLatched; ++ResetGeneration;
    if (!PreparePhysics()) return false;
    bReady = true; Fault.Reset(); CargoTransferStatus.Reset(); CargoTransferPathSummary.Reset();
    CameraArm->ResetFollow();
    if (!bNativePreviousBoostProfile && !RegisterWheelFeedback()) { Fail(TEXT("Wheel feedback rebind after recovery failed")); return false; }
    return true;
}
bool AMAYANativeDemoPawn::ResetDemo()
{
    if (!bReady || bEnding || DoorState != EDoorState::ClosedLatched || bDriveInhibited) return false;
    ClearInput();
    if (!CurrentStartPoseClear()) { Fault = TEXT("Reset refused: current exact-shape full-assembly start pose obstructed"); return false; }
    if (!RetireWheelFeedback()) return false;
    bReady = false; bDriveInhibited = true; bPhysicalAssembly = false; DestroyJoints();
    for (int32 I = 0; I < 9; ++I)
    {
        Body(I)->SetPhysicsLinearVelocity(FVector::ZeroVector);
        Body(I)->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
        Body(I)->SetWorldTransform(RestInChassis[I] * StartChassisPose, false, nullptr, ETeleportType::TeleportPhysics);
    }
    for (auto& C : Contacts) C = FContactReceipt();
    SupportMask = 0; SupportStableSinceActiveRealS = -1; DoorPhaseStartRealS = DoorPhaseStartActiveRealS = -1; bResetStabilizing = true;
    ++ResetGeneration; DoorState = EDoorState::ClosedLatched;
    if (!CreateJoints()) return false;
    bPhysicalAssembly = true; bReady = true; Fault.Reset();
    CameraArm->ResetFollow();
    if (!bNativePreviousBoostProfile && !RegisterWheelFeedback()) { Fail(TEXT("Wheel feedback rebind after demo reset failed")); return false; }
    // Drive stays inhibited through a synchronized closed-state observation and release of held keys.
    return true;
}
bool AMAYANativeDemoPawn::CurrentStartPoseClear() const
{
    auto Receipt = MakeShared<FJsonObject>();
    Receipt->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeResetClearanceQuery.v1"));
    Receipt->SetStringField(TEXT("scope"), TEXT("Actual blocking ComponentOverlapMulti of each retained physical part at the complete proposed reset pose; no parcel/world exception"));
    Receipt->SetNumberField(TEXT("worldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
    Receipt->SetObjectField(TEXT("candidateChassisPose"), Pose(StartChassisPose));
    const auto Finish = [&](bool Clear, const TCHAR* Reason)
    {
        Receipt->SetBoolField(TEXT("clear"), Clear); Receipt->SetStringField(TEXT("reason"), Reason);
        FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Receipt, Writer); LastResetClearanceQueryJson = Text; return Clear;
    };
    if (!GetWorld() || StartChassisPose.ContainsNaN() || !StartChassisPose.GetScale3D().Equals(FVector::OneVector, 1.e-6))
        return Finish(false, TEXT("World or proposed reset chassis pose invalid"));
    FComponentQueryParams Q(SCENE_QUERY_STAT(MAYAExactAssemblyReset), this);
    for (int32 I = 0; I < 15; ++I)
    {
        if (!IsValid(Body(I))) return Finish(false, TEXT("Proposed reset physical part unavailable"));
        const FTransform Target = RestInChassis[I] * StartChassisPose;
        TArray<FOverlapResult> Overlaps;
        if (Body(I)->ComponentOverlapMulti(Overlaps, GetWorld(), Target.GetLocation(), Target.GetRotation(),
            Body(I)->GetCollisionObjectType(), Q))
            for (const auto& Hit : Overlaps) if (Hit.bBlockingHit && Hit.GetActor() != this)
            {
                Receipt->SetNumberField(TEXT("blockingRobotPartIndex"), I);
                Receipt->SetStringField(TEXT("blockingRobotPartRole"), SourceParts[I].Name);
                Receipt->SetObjectField(TEXT("blockingRobotPartCurrentPose"), Pose(Body(I)->GetComponentTransform()));
                Receipt->SetObjectField(TEXT("blockingRobotPartProposedPose"), Pose(Target));
                Receipt->SetStringField(TEXT("blockingActorPath"), Hit.GetActor() ? Hit.GetActor()->GetPathName() : TEXT("None"));
                Receipt->SetStringField(TEXT("blockingComponentPath"), Hit.GetComponent() ? Hit.GetComponent()->GetPathName() : TEXT("None"));
                if (Hit.GetComponent()) Receipt->SetObjectField(TEXT("blockingComponentPose"), Pose(Hit.GetComponent()->GetComponentTransform()));
                return Finish(false, TEXT("Actual blocking overlap in proposed complete reset assembly"));
            }
    }
    return Finish(true, TEXT("All proposed reset physical parts clear of actual blocking world overlaps"));
}
void AMAYANativeDemoPawn::DestroyJoints()
{
    if (!RetireWheelFeedback()) return;
    for (const auto& J : Joints) if (J) { J->BreakConstraint(); J->DestroyComponent(); }
    Joints.Reset();
}
void AMAYANativeDemoPawn::Fail(const TCHAR* Why)
{
    Fault = Why; ClearInput(); bDriveInhibited = true; bReady = false; DoorState = EDoorState::Fault;
    if (!bPhysicalAssembly)
    {
        if (!RetireWheelFeedback()) return;
        DestroyJoints();
        for (int32 S = 9; S < Parts.Num(); ++S) if (Body(S)) Body(S)->UnWeldFromParent();
        for (int32 I = 0; I < FMath::Min(9, Parts.Num()); ++I) if (Body(I)) Body(I)->SetSimulatePhysics(false);
    }
    // Failures remain visible. Never unlock a broken/unknown assembly to manufacture a demo.
}
void AMAYANativeDemoPawn::UnPossessed() { RemoveInput(); Super::UnPossessed(); }
void AMAYANativeDemoPawn::EndPlay(EEndPlayReason::Type Reason)
{
    bEnding = true; ClearInput(); RemoveInput();
    if (!RetireWheelFeedback()) { Super::EndPlay(Reason); return; }
    DestroyJoints();
    for (int32 S = 9; S < Parts.Num(); ++S) if (Body(S)) Body(S)->UnWeldFromParent();
    for (const auto& V : Cosmetics) if (V) V->DestroyComponent(); Cosmetics.Reset();
    for (const auto& P : Parts) if (P) P->SetVisibility(true, false);
    Super::EndPlay(Reason);
}
FString AMAYANativeDemoPawn::ReadDriveAdmissionEvidenceJson(bool bIncludeOwnershipHistory) const
{
    const auto& E = DriveAdmissionEvidence; auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeDriveAdmissionDecision.v1"));
    J->SetStringField(TEXT("scope"), TEXT("GT ObserveDoors and current intent are separate from acknowledged nonfuture production PT wheel operands/application; genuine legacy comparator retains external GT motor path. Later synchronized chassis poses are separate."));
    J->SetStringField(TEXT("wheelFeedbackEvidence"), ReadWheelFeedbackEvidenceJson(bIncludeOwnershipHistory));
    J->SetStringField(TEXT("motorFrameAndWorldScope"), bNativePreviousBoostProfile ? TEXT("External GT motor decision") : TEXT("GT output-consumption frame/world stamp; actual solver callback start/dt/real operands are in wheelFeedbackEvidence"));
    J->SetBoolField(TEXT("doorDecisionKnown"), E.bDoorDecisionKnown); J->SetBoolField(TEXT("motorDecisionKnown"), E.bMotorDecisionKnown);
    J->SetNumberField(TEXT("doorPhysicalSampleFrame"), double(E.DoorSampleFrame)); J->SetNumberField(TEXT("doorPhysicalSampleWorldSeconds"), E.DoorSampleWorldS); J->SetNumberField(TEXT("doorPhysicalSampleRealSeconds"), E.DoorSampleRealS);
    J->SetNumberField(TEXT("doorDecisionFrame"), double(E.DoorDecisionFrame)); J->SetNumberField(TEXT("doorDecisionWorldSeconds"), E.DoorDecisionWorldS); J->SetNumberField(TEXT("doorEvaluationStartedRealSeconds"), E.DoorEvaluationStartedRealS); J->SetNumberField(TEXT("doorDecisionRealSeconds"), E.DoorDecisionRealS);
    J->SetNumberField(TEXT("closedFullAngleBoundRad"), ClosedAngleRad); J->SetNumberField(TEXT("settledClosureRateBoundRadps"), ClosedOmegaRadps);
    TArray<TSharedPtr<FJsonValue>> Doors, Motors;
    for (int32 L = 0; L < 2; ++L)
    {
        auto D = MakeShared<FJsonObject>(); D->SetNumberField(TEXT("lid"), L); D->SetBoolField(TEXT("physicalStateKnown"), E.DoorKnown[L]);
        const bool FiniteAngle = FMath::IsFinite(E.FullDoorAngleRad[L]), FiniteRate = FMath::IsFinite(E.CachedDoorRateRadps[L]);
        D->SetBoolField(TEXT("decisionFullConnectorAngleFinite"), FiniteAngle); D->SetBoolField(TEXT("cachedRelativeRateFinite"), FiniteRate);
        if (FiniteAngle) D->SetNumberField(TEXT("decisionFullConnectorAngleRad"), E.FullDoorAngleRad[L]);
        if (FiniteRate) D->SetNumberField(TEXT("cachedRelativeRateRadps"), E.CachedDoorRateRadps[L]);
        D->SetBoolField(TEXT("nativeLockReadKnown"), E.NativeLockReadKnown[L]); D->SetBoolField(TEXT("actualNativeClosedLockMatched"), E.ActualNativeClosedLock[L]); Doors.Add(MakeShared<FJsonValueObject>(D));
    }
    J->SetArrayField(TEXT("decisionDoors"), Doors); J->SetBoolField(TEXT("bothClosedPoses"), E.bClosedPose); J->SetBoolField(TEXT("bothSettledClosures"), E.bClosedSettled);
    J->SetBoolField(TEXT("alreadyClosedLatched"), E.bClosedLatched); J->SetBoolField(TEXT("resetStabilizing"), E.bResetStabilizing); J->SetBoolField(TEXT("doorDriveInhibited"), E.bDoorDriveInhibited);
    J->SetNumberField(TEXT("motorDecisionFrame"), double(E.MotorFrame)); J->SetNumberField(TEXT("motorDecisionWorldSeconds"), E.MotorWorldS); J->SetNumberField(TEXT("motorDecisionRealSeconds"), E.MotorRealS);
    J->SetNumberField(TEXT("commandSequenceAtMotorDecision"), double(E.CommandSequence)); J->SetNumberField(TEXT("commandIssuedRealSecondsAtMotorDecision"), E.CommandIssuedRealS);
    J->SetNumberField(TEXT("requestedThrottleAtMotorDecision"), E.RequestedThrottle); J->SetNumberField(TEXT("requestedSteerAtMotorDecision"), E.RequestedSteer);
    J->SetBoolField(TEXT("freshAtMotorDecision"), E.bFresh); J->SetBoolField(TEXT("brakeRequestedAtMotorDecision"), E.bBrakeRequested); J->SetBoolField(TEXT("doorInhibitedAtMotorDecision"), E.bDoorInhibitedAtMotorDecision); J->SetBoolField(TEXT("deliveryDriveLockedAtMotorDecision"), E.bDeliveryLocked); J->SetBoolField(TEXT("inputsNeedReleaseAtMotorDecision"), E.bInputsNeedRelease);
    J->SetBoolField(TEXT("boostRequestedAtMotorDecision"), E.bBoostRequested); J->SetBoolField(TEXT("previousBoostProfileAtMotorDecision"), E.bPreviousBoostProfile); J->SetBoolField(TEXT("motorDriveAdmitted"), E.bDriveAdmitted); J->SetBoolField(TEXT("boostMotorAppliedBeforeLaterCommand"), E.bBoostMotorApplied);
    J->SetNumberField(TEXT("selectedMotorPeakNm"), E.MotorPeakNm); J->SetNumberField(TEXT("selectedMotorNoLoadRadps"), E.MotorNoLoadRadps);
    J->SetBoolField(TEXT("currentBoostLowSpeedReserveSampleAttempted"), E.bReserveSpeedSampleAttempted);
    J->SetBoolField(TEXT("reserveNativeChassisBodyAvailable"), E.bReserveNativeBodyAvailable);
    J->SetBoolField(TEXT("reserveChassisSpeedFiniteKnown"), E.bReserveSpeedKnown);
    J->SetNumberField(TEXT("reserveChassisSpeedSampleRealSeconds"), E.ReserveSpeedSampleRealS);
    if (E.bReserveSpeedKnown)
    {
        TArray<TSharedPtr<FJsonValue>> VelocityCmps;
        VelocityCmps.Add(MakeShared<FJsonValueNumber>(E.ReserveChassisVelocityCmps.X));
        VelocityCmps.Add(MakeShared<FJsonValueNumber>(E.ReserveChassisVelocityCmps.Y));
        VelocityCmps.Add(MakeShared<FJsonValueNumber>(E.ReserveChassisVelocityCmps.Z));
        J->SetArrayField(TEXT("sampledNativeChassisVelocityCmps"), VelocityCmps);
        J->SetNumberField(TEXT("sampledNativeChassisSpeedCmps"), E.ReserveChassisSpeedCmps);
    }
    J->SetNumberField(TEXT("currentBoostLowSpeedReserveFactor"), E.LowSpeedReserveFactor);
    J->SetNumberField(TEXT("configuredCurrentBoostRoadPeakNm"), E.ConfiguredCurrentBoostRoadPeakNm);
    J->SetNumberField(TEXT("configuredCurrentBoostMaximumReservePeakNm"), E.ConfiguredCurrentBoostMaximumReservePeakNm);
    J->SetStringField(TEXT("currentBoostLowSpeedReserveScope"), TEXT("Admitted current Boost only: one native chassis velocity sample, finite3D speed in cm/s, factor clamp(1-speed/50,0,1), road12 plus6*factor Nm cap. Unknown/nonfinite speed gives no reserve. Cached selectedMotorPeakNm supplies original motor arithmetic and final actual effort; no later-speed recomputation. Normal/previous profiles unchanged; current calibrated42.4rad/s, gain.5, falloff/coast/brake/bearing/interlocks retained. Production sample clock/operands belong to acknowledged actual PT callback start/dt; genuine legacy uses separately stamped external GT readback."));
    J->SetBoolField(TEXT("currentBoostOverspeedCoastPolicyEligible"), E.bCurrentBoostPolicyEligible); J->SetBoolField(TEXT("actualBoostDriveActiveAtMotorDecision"), E.bBoostDriveActiveAtMotorDecision);
    J->SetNumberField(TEXT("actualBoostEffortFractionAtMotorDecision"), E.ActualBoostEffortFractionAtMotorDecision);
    J->SetStringField(TEXT("motorPolicyScope"), TEXT("Only current Boost suppresses original-law motor Nm when nonzero clamped request times original Nm is negative. Normal/previous comparison unchanged; zero request coasts, intentional requested reversal remains. Original-law Nm is a counterfactual; post-policy and admission-aware expected applied Nm are separate. Final actual motor cache determines activity/effort, so all-six coast is inactive powered Boost with zero effort and existing ordinary motion drain. Native brakes and bearing unchanged."));
    for (int32 W = 0; W < 6; ++W)
    {
        auto M = MakeShared<FJsonObject>(); M->SetNumberField(TEXT("wheel"), W); M->SetNumberField(TEXT("appliedMotorNm"), E.AppliedMotorNm[W]); M->SetNumberField(TEXT("passiveBearingNm"), E.PassiveBearingNm[W]); M->SetBoolField(TEXT("verifiedNativeBrakeEnabled"), E.VerifiedNativeBrakeEnabled[W]);
        const bool FiniteInputs = E.WheelInputSampled[W] && FMath::IsFinite(E.RelativeWheelOmegaRadps[W]) && FMath::IsFinite(E.UnclampedBankRequest[W]) && FMath::IsFinite(E.ClampedBankRequest[W]) && FMath::IsFinite(E.MotorNoLoadRadps) && E.MotorNoLoadRadps > 0 && FMath::IsFinite(E.MotorPeakNm) && E.MotorPeakNm >= 0 && FMath::IsFinite(E.MotorErrorNmPerRadps);
        M->SetBoolField(TEXT("wheelDecisionOperandsFiniteKnown"), FiniteInputs); M->SetBoolField(TEXT("wheelInputSampled"), E.WheelInputSampled[W]);
        M->SetBoolField(TEXT("driveAdmittedAtWheelCalculation"), E.WheelDriveAdmitted[W]); M->SetBoolField(TEXT("currentBoostPolicyEligible"), E.bCurrentBoostPolicyEligible); M->SetBoolField(TEXT("counterTorqueSuppressed"), E.CounterTorqueSuppressed[W]);
        const bool CachedTorqueFinite = FMath::IsFinite(E.OriginalLawMotorNm[W]) && FMath::IsFinite(E.MotorNmAfterPolicy[W]) && FMath::IsFinite(E.ExpectedAppliedMotorNm[W]);
        M->SetBoolField(TEXT("cachedPolicyTorqueFinite"), CachedTorqueFinite); M->SetBoolField(TEXT("expectedAppliedMotorKnown"), E.bMotorDecisionKnown && FiniteInputs && CachedTorqueFinite);
        if (CachedTorqueFinite){M->SetNumberField(TEXT("originalLawMotorTorqueNm"), E.OriginalLawMotorNm[W]); M->SetNumberField(TEXT("motorTorqueAfterCurrentBoostPolicyNm"), E.MotorNmAfterPolicy[W]); if(E.bMotorDecisionKnown)M->SetNumberField(TEXT("expectedAppliedMotorTorqueAfterAdmissionNm"), E.ExpectedAppliedMotorNm[W]);}
        if (FiniteInputs)
        {
            M->SetNumberField(TEXT("inputSampleRealSeconds"), E.WheelInputSampleRealS[W]); M->SetNumberField(TEXT("sampledRelativeWheelOmegaRadps"), E.RelativeWheelOmegaRadps[W]); M->SetNumberField(TEXT("unclampedBankRequest"), E.UnclampedBankRequest[W]); M->SetNumberField(TEXT("clampedMotorFunctionRequest"), E.ClampedBankRequest[W]); M->SetNumberField(TEXT("selectedMotorErrorNmPerRadps"), E.MotorErrorNmPerRadps);
            const double Target = E.ClampedBankRequest[W] * E.MotorNoLoadRadps, ErrorTorque = (Target - E.RelativeWheelOmegaRadps[W]) * E.MotorErrorNmPerRadps;
            const double Capped = FMath::Clamp(ErrorTorque, -E.MotorPeakNm, E.MotorPeakNm);
            const double Curve = Capped * E.RelativeWheelOmegaRadps[W] > 0 ? FMath::Clamp(1.0 - FMath::Abs(E.RelativeWheelOmegaRadps[W]) / E.MotorNoLoadRadps, 0.0, 1.0) : 1.0;
            const bool PassiveCoast = E.ClampedBankRequest[W] == 0; const double AfterCurve = PassiveCoast ? 0 : Capped * Curve;
            const bool FiniteDerived = FMath::IsFinite(Target) && FMath::IsFinite(ErrorTorque) && FMath::IsFinite(Capped) && FMath::IsFinite(Curve) && FMath::IsFinite(AfterCurve);
            M->SetBoolField(TEXT("derivedServoArithmeticFinite"), FiniteDerived); M->SetBoolField(TEXT("zeroRequestPassiveCoastBypass"), PassiveCoast);
            if (FiniteDerived)
            {
                const bool Suppressed = E.bCurrentBoostPolicyEligible && E.ClampedBankRequest[W] != 0 && E.ClampedBankRequest[W] * AfterCurve < 0;
                const double AfterPolicy = Suppressed ? 0 : AfterCurve, ExpectedApplied = E.WheelDriveAdmitted[W] ? AfterPolicy : 0;
                M->SetNumberField(TEXT("derivedTargetWheelOmegaRadps"), Target); M->SetNumberField(TEXT("derivedUnclampedServoErrorTorqueNm"), ErrorTorque); M->SetNumberField(TEXT("derivedCappedServoTorqueNm"), Capped); M->SetNumberField(TEXT("derivedSpeedCurveFactor"), Curve);
                M->SetNumberField(TEXT("derivedOriginalLawTorqueAfterCoastAndCurveNm"), AfterCurve); M->SetBoolField(TEXT("derivedCounterTorqueSuppressed"), Suppressed); M->SetNumberField(TEXT("derivedTorqueAfterCurrentBoostPolicyNm"), AfterPolicy);
                if(E.bMotorDecisionKnown)M->SetNumberField(TEXT("derivedExpectedAppliedMotorTorqueAfterAdmissionNm"), ExpectedApplied);
            }
        }
        M->SetStringField(TEXT("operandScope"), TEXT("Exact relative Omega and bank request operands copied from acknowledged production PT output or genuine external legacy motor decision. Derived original servo arithmetic and current-Boost policy are read-only from that cache, not a second physical read or force application. Original-law Nm is counterfactual; post-policy and admission-aware expected actual Nm are distinct. Zero request coasts; native-brake status remains separate. Failed native brake configuration makes expected applied torque unknown. Motor frame/world above are GT consumption stamps for production; per-wheel real operands and callback start/dt are identified in wheelFeedbackEvidence. Genuine legacy keeps external GT timing.")); Motors.Add(MakeShared<FJsonValueObject>(M));
    }
    J->SetArrayField(TEXT("motorDecisionWheels"), Motors); FString Text; FJsonSerializer::Serialize(J, TJsonWriterFactory<>::Create(&Text)); return Text;
}

FString AMAYANativeDemoPawn::ReadTelemetryJson() const
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("scope"), TEXT("Additive GAME physical demo; no DDS admission/service certificate"));
    O->SetStringField(TEXT("observationClock"), TEXT("Game-thread synchronized component state and native hit receipts; not an identified solver step"));
    O->SetBoolField(TEXT("ready"), bReady); O->SetBoolField(TEXT("driveInhibited"), bDriveInhibited);
    O->SetStringField(TEXT("cachedNativeDriveAdmissionDecision"), ReadDriveAdmissionEvidenceJson());
    O->SetStringField(TEXT("fault"), Fault); O->SetNumberField(TEXT("capturedRealS"), FPlatformTime::Seconds());
    O->SetNumberField(TEXT("inputIssuedRealS"), InputIssuedRealS); O->SetNumberField(TEXT("inputSequence"), double(InputSequence));
    O->SetStringField(TEXT("inputOrigin"), InputOrigin == EMAYADriveCommandOrigin::NativeAutomation ? TEXT("NativeAutomation")
        : InputOrigin == EMAYADriveCommandOrigin::LocalPlayer ? TEXT("LocalPlayer") : TEXT("None"));
    O->SetNumberField(TEXT("inputLeaseSeconds"), InputLeaseS);
    O->SetNumberField(TEXT("inputThrottle"), Throttle); O->SetNumberField(TEXT("inputSteer"), Steer);
    O->SetBoolField(TEXT("inputBrakeRequested"), bBrake); O->SetBoolField(TEXT("inputCommandFresh"), HasFreshDriveCommand());
    O->SetBoolField(TEXT("inputBoostRequested"), bBoostRequested);
    O->SetBoolField(TEXT("boostDriveActive"), IsBoostDriveActive());
    O->SetStringField(TEXT("nativeWheelFeedbackEvidence"), ReadWheelFeedbackEvidenceJson());
    O->SetNumberField(TEXT("boostMotorNoLoadMultiplier"), bNativePreviousBoostProfile ? MAYADeliveryBoostProfile::PreviousMultiplier : MAYADeliveryBoostProfile::NoLoadMultiplier);
    const double ConfiguredBoostRoadPeak = MAYADeliveryBoostProfile::Apply(MotorProfile(), true, bNativePreviousBoostProfile).MotorPeakNm;
    const bool EffectiveCurrentBoostPeakKnown = DriveAdmissionEvidence.bMotorDecisionKnown && DriveAdmissionEvidence.bCurrentBoostPolicyEligible;
    const double EffectiveBoostPeak = EffectiveCurrentBoostPeakKnown ? DriveAdmissionEvidence.MotorPeakNm : ConfiguredBoostRoadPeak;
    O->SetNumberField(TEXT("boostMotorPeakMultiplier"), EffectiveCurrentBoostPeakKnown ? EffectiveBoostPeak / MotorProfile().MotorPeakNm : bNativePreviousBoostProfile ? MAYADeliveryBoostProfile::PreviousMultiplier : MAYADeliveryBoostProfile::PeakTorqueMultiplier);
    O->SetBoolField(TEXT("nativeBoostComparisonPreviousProfile"), bNativePreviousBoostProfile);
    O->SetNumberField(TEXT("boostMotorNoLoadRatioToPreviousProfile"), bNativePreviousBoostProfile ? 1.0 : MAYADeliveryBoostProfile::MotorNoLoadRatioToPreviousBoost);
    O->SetNumberField(TEXT("boostEnergyRoadSpeedTargetRatioToPreviousProfile"), MAYADeliveryBoostProfile::EnergyRoadSpeedTargetRatioToPreviousBoost);
    O->SetNumberField(TEXT("boostMotorPeakNm"), EffectiveBoostPeak);
    O->SetNumberField(TEXT("boostConfiguredRoadPeakNm"), ConfiguredBoostRoadPeak);
    O->SetNumberField(TEXT("boostConfiguredMaximumReservePeakNm"), ConfiguredBoostRoadPeak + (bNativePreviousBoostProfile ? 0 : MAYADeliveryBoostProfile::LowSpeedReserveNm));
    O->SetBoolField(TEXT("boostEffectiveCurrentPeakFromCachedDecisionKnown"), EffectiveCurrentBoostPeakKnown);
    O->SetStringField(TEXT("boostPeakScope"), TEXT("Current Boost effective peak is the last known admitted current motor-decision cap12..18Nm; configured road12/max18 reported separately. Without such a current decision, boostMotorPeakNm reports the configured envelope; cached selectedMotorPeakNm still identifies actual normal5/previous6/current peak. No later-speed recomputation."));
    O->SetNumberField(TEXT("boostMotorNoLoadRadps"), MAYADeliveryBoostProfile::Apply(MotorProfile(), true, bNativePreviousBoostProfile).MotorNoLoadRadps);
    O->SetStringField(TEXT("boostComparisonEnergyScope"), TEXT("Diagnostic previous profile recreates24rad/s6Nm motors only; current boost energy rules remain active. Previous-release battery comparison is a declared formula counterfactual, not old-runtime battery."));
    O->SetNumberField(TEXT("boostMotorEffortFraction"), ReadBoostMotorEffortFraction());
    O->SetStringField(TEXT("boostObservationScope"), TEXT("Fresh accepted nonzero boost drive with six finite applied motor torques; effort is mean absolute acknowledged production PT applied motor Nm (or genuine legacy GT cache) / the same decision selected peak; brake/bearing excluded. Callback operands and later GT body telemetry are separately timed; this is not mechanical power. Achieved2x oldboost ground speed requires matched native trials."));
    O->SetBoolField(TEXT("deliveryDriveLocked"), bDeliveryDriveLocked);
    O->SetStringField(TEXT("collisionProfile"), bDeliveryCollisionProfileLoaded
        ? TEXT("MayaRobotics.v01.Candidate02.UnitsV02.OwnedBayAndSeam") : TEXT("OriginalPublisherDemo"));
    O->SetStringField(TEXT("cargoTransferStatus"), CargoTransferStatus);
    O->SetNumberField(TEXT("physicalCargoSlotCount"), 2);
    auto Landing = MakeShared<FJsonObject>();
    Landing->SetNumberField(TEXT("generation"), double(LastNativeImpact.Generation));
    Landing->SetStringField(TEXT("kind"), LastNativeImpact.Kind);
    Landing->SetNumberField(TEXT("worldSeconds"), LastNativeImpact.WorldSeconds);
    Landing->SetNumberField(TEXT("impulseKgMps"), LastNativeImpact.ImpulseKgMps);
    Landing->SetNumberField(TEXT("preContactDownCmps"), LastNativeImpact.PreContactDownCmps);
    Landing->SetNumberField(TEXT("dropCm"), LastNativeImpact.DropCm);
    Landing->SetStringField(TEXT("otherActor"), LastNativeImpact.OtherActor);
    Landing->SetStringField(TEXT("otherComponent"), LastNativeImpact.OtherComponent);
    Landing->SetStringField(TEXT("scope"), TEXT("Distinct actual chassis multi-axle landing episode: >=0.1s accumulated all-six-predicate loss, >=8cm drop, >=40cm/s downward speed, new positive native wheel hit impulse; retained across brief regain until0.5s continuous full support, bounded5s and one event per episode; latest-hit impulse sum, no solver force/dt claim. Cargo-condition eligibility belongs to Core."));
    O->SetObjectField(TEXT("lastNativeImpact"), Landing);
    auto LandingEpisode = MakeShared<FJsonObject>();
    LandingEpisode->SetBoolField(TEXT("initialized"), bLandingSupportInitialized);
    LandingEpisode->SetBoolField(TEXT("active"), bLandingSupportLost);
    LandingEpisode->SetBoolField(TEXT("emittedThisEpisode"), bLandingEpisodeEmitted);
    LandingEpisode->SetNumberField(TEXT("startWorldSeconds"), LandingLossStartWorldS);
    LandingEpisode->SetNumberField(TEXT("currentUnsupportedStartWorldSeconds"), LandingCurrentLossStartWorldS);
    LandingEpisode->SetNumberField(TEXT("observedUnsupportedSeconds"), LandingObservedLossWorldS);
    LandingEpisode->SetNumberField(TEXT("fullSupportRegainedWorldSeconds"), LandingSupportRegainedWorldS);
    LandingEpisode->SetNumberField(TEXT("peakChassisHeightCm"), LandingPeakChassisZ);
    LandingEpisode->SetNumberField(TEXT("peakDownCmps"), LandingPeakDownCmps);
    LandingEpisode->SetStringField(TEXT("scope"), TEXT("Actual all-six-predicate loss intervals accumulated within one bounded5s multi-axle episode; brief full-support reacquisition does not erase drop/downspeed. Clear after0.5s continuous full support; emit once. Original drive/service support and8cm/40cmps/.1s-loss/fresh-hit gates unchanged; not a claim all wheels airborne."));
    O->SetObjectField(TEXT("nativeLandingEpisode"), LandingEpisode);
    if (CargoMount) O->SetObjectField(TEXT("frontCargoMountWorldPose"), Pose(CargoMount->GetComponentTransform()));
    if (RearCargoMount) O->SetObjectField(TEXT("rearCargoMountWorldPose"), Pose(RearCargoMount->GetComponentTransform()));
    O->SetStringField(TEXT("cargoTransferPathSummary"), CargoTransferPathSummary);
    O->SetNumberField(TEXT("resetGeneration"), double(ResetGeneration)); O->SetNumberField(TEXT("supportMask"), SupportMask);
    O->SetStringField(TEXT("ownedRobotSleepPolicy"), TEXT("Private robot material requests zero linear/angular sleep thresholds; fresh native hit support is still independently required. No global sleep CVar, geometry, mass, force or support-gate change."));
    if (ContactMaterial)
    {
        O->SetNumberField(TEXT("requestedRobotSleepLinearThreshold"), ContactMaterial->SleepLinearVelocityThreshold);
        O->SetNumberField(TEXT("requestedRobotSleepAngularThreshold"), ContactMaterial->SleepAngularVelocityThreshold);
    }
    O->SetStringField(TEXT("lastResetClearanceQuery"), LastResetClearanceQueryJson);
    O->SetStringField(TEXT("stablePlayerCamera"), CameraArm ? CameraArm->ReadEvidenceJson() : TEXT(""));
    O->SetStringField(TEXT("playerCameraMode"), ReadPlayerCameraModeJson());
    O->SetStringField(TEXT("playerCameraWorldPoseScope"), TEXT("Existing chase camera component; see playerCameraMode for selected pawn view and actual camera-manager cache."));
    if (Camera) O->SetObjectField(TEXT("playerCameraWorldPose"), Pose(Camera->GetComponentTransform()));
    O->SetNumberField(TEXT("doorState"), int32(DoorState));
    O->SetNumberField(TEXT("doorPhaseStartedRealS"), DoorPhaseStartRealS);
    O->SetStringField(TEXT("doorTimeoutClock"), TEXT("WorldAudioTimeSeconds: unpaused, undilated, unclamped"));
    O->SetNumberField(TEXT("doorTimeoutSeconds"), DoorTimeoutS);
    O->SetNumberField(TEXT("doorPhaseStartedActiveRealS"), DoorPhaseStartActiveRealS);
    const double DoorClockNow = GetWorld() ? GetWorld()->GetAudioTimeSeconds() : -1;
    O->SetNumberField(TEXT("doorClockActiveRealS"), DoorClockNow);
    O->SetNumberField(TEXT("doorPhaseElapsedActiveRealS"), DoorPhaseStartActiveRealS >= 0 && DoorClockNow >= DoorPhaseStartActiveRealS
        ? DoorClockNow - DoorPhaseStartActiveRealS : -1);
    O->SetNumberField(TEXT("configuredLidKpNmPerRad"), LidKpNmPerRad);
    O->SetNumberField(TEXT("configuredLidKdNmPerRadps"), LidKdNmPerRadps);
    O->SetNumberField(TEXT("configuredLidPeakNm"), LidPeakNm);
    O->SetNumberField(TEXT("currentWorldGravityCmps2"), GetWorld() ? GetWorld()->GetGravityZ() : 0);
    auto FrameSettings = MakeShared<FJsonObject>();
    FrameSettings->SetStringField(TEXT("scope"), TEXT("Current project physics settings and world frame clock only; no identified solver step or dt for joint impulse-to-force conversion"));
    FrameSettings->SetBoolField(TEXT("solverStepIdentityKnown"), false);
    FrameSettings->SetBoolField(TEXT("solverImpulseDtKnown"), false);
    FrameSettings->SetNumberField(TEXT("worldDeltaSecondsNotSolverDt"), GetWorld() ? GetWorld()->GetDeltaSeconds() : -1);
    if (const UPhysicsSettings* Settings = UPhysicsSettings::Get())
    {
        FrameSettings->SetBoolField(TEXT("configuredSubstepping"), Settings->bSubstepping);
        FrameSettings->SetBoolField(TEXT("configuredAsyncSubstepping"), Settings->bSubsteppingAsync);
        FrameSettings->SetBoolField(TEXT("configuredAsyncPhysicsTick"), Settings->bTickPhysicsAsync);
        FrameSettings->SetNumberField(TEXT("configuredAsyncFixedStepSeconds"), Settings->AsyncFixedTimeStepSize);
        FrameSettings->SetNumberField(TEXT("configuredMinPhysicsDeltaSeconds"), Settings->MinPhysicsDeltaTime);
        FrameSettings->SetNumberField(TEXT("configuredMaxPhysicsDeltaSeconds"), Settings->MaxPhysicsDeltaTime);
        FrameSettings->SetNumberField(TEXT("configuredMaxSubstepDeltaSeconds"), Settings->MaxSubstepDeltaTime);
        FrameSettings->SetNumberField(TEXT("configuredMaxSubsteps"), Settings->MaxSubsteps);
    }
    O->SetObjectField(TEXT("physicsFrameSettings"), FrameSettings);
    // Fresh GAME-thread Chaos external readback under native read locks. This is
    // not a solver-step receipt, and known settings need not be closed/locked.
    const auto MotionName = [](Chaos::EJointMotionType Motion) -> const TCHAR*
    {
        switch (Motion)
        {
        case Chaos::EJointMotionType::Free: return TEXT("Free");
        case Chaos::EJointMotionType::Limited: return TEXT("Limited");
        case Chaos::EJointMotionType::Locked: return TEXT("Locked");
        default: return TEXT("Unknown");
        }
    };
    const auto VectorJson = [](const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(V.X));
        Values.Add(MakeShared<FJsonValueNumber>(V.Y));
        Values.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Values;
    };
    const auto ContactPose = [](const FTransform& T)
    {
        auto P = Pose(T); const FVector Scale = T.GetScale3D();
        P->SetNumberField(TEXT("sx"), Scale.X); P->SetNumberField(TEXT("sy"), Scale.Y);
        P->SetNumberField(TEXT("sz"), Scale.Z); return P;
    };
    FTransform NativeChassisPose = FTransform::Identity;
    const FBodyInstance* ChassisBI = IsValid(Body(0)) ? Body(0)->GetBodyInstance() : nullptr;
    const bool ChassisBodyValid = ChassisBI && ChassisBI->IsValidBodyInstance();
    if (ChassisBodyValid) NativeChassisPose = ChassisBI->GetUnrealWorldTransform(false, true);
    const bool NativeChassisPoseKnown = ChassisBodyValid && !NativeChassisPose.ContainsNaN()
        && NativeChassisPose.GetRotation().IsNormalized();
    TArray<TSharedPtr<FJsonObject>> ActualJoints;
    TArray<TSharedPtr<FJsonValue>> JointValues;
    int32 ValidUnbrokenCount = 0;
    bool EightJointReadbackKnown = Joints.Num() == 8;
    for (int32 JointIndex = 0; JointIndex < 8; ++JointIndex)
    {
        auto J = MakeShared<FJsonObject>();
        bool Known = false;
        J->SetNumberField(TEXT("jointIndex"), JointIndex);
        J->SetBoolField(TEXT("valid"), false); J->SetBoolField(TEXT("broken"), true);
        J->SetStringField(TEXT("type"), TEXT("Unknown"));
        J->SetBoolField(TEXT("finite"), false); J->SetBoolField(TEXT("endpointsMatch"), false);
        J->SetBoolField(TEXT("closedLockMatches"), false); J->SetNumberField(TEXT("sampleRealS"), -1);
        if (Joints.IsValidIndex(JointIndex) && IsValid(Joints[JointIndex].Get()))
        {
            const auto Ref = Joints[JointIndex]->ConstraintInstance.GetPhysicsConstraintRef();
            FPhysInterface_Chaos::ExecuteRead(Ref, [&](const FPhysicsConstraintHandle& Locked)
            {
                const bool Valid = Locked.IsValid() && Locked.Constraint;
                J->SetBoolField(TEXT("valid"), Valid);
                if (!Valid) return;
                if (!Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType))
                { J->SetStringField(TEXT("type"), TEXT("OtherConstraint")); return; }
                J->SetStringField(TEXT("type"), TEXT("ChaosJoint"));
                const bool Broken = FPhysInterface_Chaos::IsBroken(Locked);
                J->SetBoolField(TEXT("broken"), Broken);
                const auto* Actual = static_cast<const Chaos::FJointConstraint*>(Locked.Constraint);
                const auto& S = Actual->GetJointSettings();
                const FTransform FrameA(FQuat(S.ConnectorTransforms[0].GetRotation()), FVector(S.ConnectorTransforms[0].GetTranslation()));
                const FTransform FrameB(FQuat(S.ConnectorTransforms[1].GetRotation()), FVector(S.ConnectorTransforms[1].GetTranslation()));
                bool Finite = FMath::IsFinite(S.LinearLimit) && !FVector(S.AngularLimits).ContainsNaN()
                    && !FrameA.ContainsNaN() && !FrameB.ContainsNaN()
                    && FrameA.GetRotation().IsNormalized() && FrameB.GetRotation().IsNormalized();
                TArray<TSharedPtr<FJsonValue>> LinearMotion, AngularMotion;
                for (int32 Axis = 0; Axis < 3; ++Axis)
                {
                    const TCHAR* LinearName = MotionName(S.LinearMotionTypes[Axis]);
                    const TCHAR* AngularName = MotionName(S.AngularMotionTypes[Axis]);
                    Finite &= FCString::Strcmp(LinearName, TEXT("Unknown")) != 0
                        && FCString::Strcmp(AngularName, TEXT("Unknown")) != 0;
                    LinearMotion.Add(MakeShared<FJsonValueString>(LinearName));
                    AngularMotion.Add(MakeShared<FJsonValueString>(AngularName));
                }
                const auto Endpoints = Actual->GetPhysicsBodies();
                const auto* BaseBI = IsValid(Body(0)) ? Body(0)->GetBodyInstance() : nullptr;
                const auto* PartBI = IsValid(Body(JointIndex + 1)) ? Body(JointIndex + 1)->GetBodyInstance() : nullptr;
                const auto* BaseActor = BaseBI ? BaseBI->GetPhysicsActorHandle() : nullptr;
                const auto* PartActor = PartBI ? PartBI->GetPhysicsActorHandle() : nullptr;
                const bool EndpointsMatch = BaseActor && PartActor
                    && Endpoints[0] == (JointIndex < 6 ? PartActor : BaseActor)->GetPhysicsObject()
                    && Endpoints[1] == (JointIndex < 6 ? BaseActor : PartActor)->GetPhysicsObject();
                J->SetStringField(TEXT("nativeEndpoint0Role"), JointIndex < 6 ? TEXT("Wheel") : TEXT("Chassis"));
                J->SetStringField(TEXT("nativeEndpoint1Role"), JointIndex < 6 ? TEXT("Chassis") : TEXT("Lid"));
                J->SetStringField(TEXT("solverLinearFrame0Owner"), JointIndex < 6 ? TEXT("Chassis") : TEXT("Lid"));
                J->SetStringField(TEXT("endpointOrderScope"), TEXT("Expected roles checked against native endpoints; installed UE5.3 solver selects index 1-ParticleIndex; joint impulses retain native endpoint order"));
                J->SetBoolField(TEXT("finite"), Finite); J->SetBoolField(TEXT("endpointsMatch"), EndpointsMatch);
                Known = Finite && EndpointsMatch;
                J->SetBoolField(TEXT("nativeSuspensionSolverAxisKnown"), Known && JointIndex < 6 && NativeChassisPoseKnown);
                if (Known && JointIndex < 6 && NativeChassisPoseKnown)
                {
                    const FVector Axis = (FrameB * NativeChassisPose).GetUnitAxis(EAxis::Z);
                    J->SetArrayField(TEXT("nativeSuspensionSolverZWorld"), VectorJson(Axis));
                    J->SetNumberField(TEXT("nativeSuspensionAxisChassisZAlignment"), FVector::DotProduct(Axis, NativeChassisPose.GetUnitAxis(EAxis::Z)));
                }
                if (Finite)
                {
                    J->SetArrayField(TEXT("linearMotion"), LinearMotion); J->SetArrayField(TEXT("angularMotion"), AngularMotion);
                    J->SetNumberField(TEXT("linearLimitCm"), S.LinearLimit);
                    // Native Chaos angular vector order: Twist, Swing2, Swing1; radians.
                    J->SetArrayField(TEXT("angularLimitsRad"), VectorJson(FVector(S.AngularLimits)));
                    TArray<TSharedPtr<FJsonValue>> Frames;
                    Frames.Add(MakeShared<FJsonValueObject>(Pose(FrameA))); Frames.Add(MakeShared<FJsonValueObject>(Pose(FrameB)));
                    J->SetArrayField(TEXT("connectorTransforms"), Frames);
                }
                const bool DriveKnown = !FVector(S.LinearDrivePositionTarget).ContainsNaN()
                    && !FVector(S.LinearDriveVelocityTarget).ContainsNaN() && !FVector(S.LinearDriveStiffness).ContainsNaN()
                    && !FVector(S.LinearDriveDamping).ContainsNaN() && !FVector(S.LinearDriveMaxForce).ContainsNaN()
                    && (S.LinearDriveForceMode == Chaos::EJointForceMode::Force || S.LinearDriveForceMode == Chaos::EJointForceMode::Acceleration);
                J->SetBoolField(TEXT("nativeLinearDriveReadbackKnown"), DriveKnown);
                if (DriveKnown)
                {
                    J->SetArrayField(TEXT("nativeLinearDrivePositionTargetCm"), VectorJson(FVector(S.LinearDrivePositionTarget)));
                    J->SetArrayField(TEXT("nativeLinearDriveVelocityTargetCmps"), VectorJson(FVector(S.LinearDriveVelocityTarget)));
                    J->SetArrayField(TEXT("nativeLinearDriveStiffness"), VectorJson(FVector(S.LinearDriveStiffness)));
                    J->SetArrayField(TEXT("nativeLinearDriveDamping"), VectorJson(FVector(S.LinearDriveDamping)));
                    J->SetArrayField(TEXT("nativeLinearDriveMaximumForce"), VectorJson(FVector(S.LinearDriveMaxForce)));
                    J->SetStringField(TEXT("nativeLinearDriveForceMode"), S.LinearDriveForceMode == Chaos::EJointForceMode::Force ? TEXT("Force") : TEXT("Acceleration"));
                    TArray<TSharedPtr<FJsonValue>> PositionEnabled, VelocityEnabled;
                    for (int32 Axis = 0; Axis < 3; ++Axis)
                    {
                        PositionEnabled.Add(MakeShared<FJsonValueBoolean>(S.bLinearPositionDriveEnabled[Axis]));
                        VelocityEnabled.Add(MakeShared<FJsonValueBoolean>(S.bLinearVelocityDriveEnabled[Axis]));
                    }
                    J->SetArrayField(TEXT("nativeLinearPositionDriveEnabled"), PositionEnabled);
                    J->SetArrayField(TEXT("nativeLinearVelocityDriveEnabled"), VelocityEnabled);
                }
                FVector LinearImpulse = FVector::ZeroVector, AngularImpulse = FVector::ZeroVector;
                if (Known && !Broken) FPhysInterface_Chaos::GetForce(Locked, LinearImpulse, AngularImpulse);
                const bool ImpulseKnown = Known && !Broken && !LinearImpulse.ContainsNaN() && !AngularImpulse.ContainsNaN();
                J->SetBoolField(TEXT("nativeJointImpulseReadbackKnown"), ImpulseKnown);
                J->SetStringField(TEXT("nativeJointImpulseScope"), TEXT("Latest synchronized external output under native read lock; UE5.3 JointConstraintProxy stores GetLinearImpulse/GetAngularImpulse in fields called Force/Torque; solver step/dt unidentified, no force conversion"));
                if (ImpulseKnown)
                {
                    J->SetArrayField(TEXT("nativeJointLinearImpulseKgCmps"), VectorJson(LinearImpulse));
                    J->SetArrayField(TEXT("nativeJointAngularImpulseKgCm2ps"), VectorJson(AngularImpulse));
                }
                J->SetBoolField(TEXT("closedLockMatches"), Known && !Broken && JointIndex >= 6
                    && DDSJointSettingsMatch(S, JointIndex, JointInChassis[JointIndex], JointInPart[JointIndex], 0, 0, true, MotorProfile()));
                J->SetNumberField(TEXT("sampleRealS"), FPlatformTime::Seconds());
                if (Known && !Broken) ++ValidUnbrokenCount;
            });
        }
        J->SetBoolField(TEXT("known"), Known); EightJointReadbackKnown &= Known;
        ActualJoints.Add(J); JointValues.Add(MakeShared<FJsonValueObject>(J));
    }
    auto EightJoints = MakeShared<FJsonObject>();
    EightJoints->SetBoolField(TEXT("known"), EightJointReadbackKnown);
    EightJoints->SetNumberField(TEXT("validUnbrokenCount"), ValidUnbrokenCount);
    EightJoints->SetNumberField(TEXT("sampleRealS"), FPlatformTime::Seconds());
    EightJoints->SetArrayField(TEXT("joints"), JointValues);
    O->SetObjectField(TEXT("nativeEightJointReadback"), EightJoints);
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 I = 0; I < Parts.Num(); ++I)
    {
        auto R = Pose(Body(I)->GetComponentTransform()); R->SetStringField(TEXT("sourceRole"), SourceParts[I].Name);
        R->SetStringField(TEXT("meshObjectPath"), Body(I)->GetStaticMesh() ? Body(I)->GetStaticMesh()->GetPathName() : TEXT(""));
        R->SetBoolField(TEXT("simulating"), Body(I)->IsSimulatingPhysics());
        if (I < 9)
        {
            R->SetNumberField(TEXT("nativeMassKg"), Body(I)->GetMass());
            R->SetObjectField(TEXT("nativeCOMWorldCm"), Pose(FTransform(Body(I)->GetCenterOfMass())));
            const auto* BI = Body(I)->GetBodyInstance();
            const FVector Inertia = BI ? BI->GetBodyInertiaTensor() : FVector::ZeroVector;
            const bool InertiaKnown = BI && !Inertia.ContainsNaN() && Inertia.GetMin() > 0;
            R->SetBoolField(TEXT("nativeInertiaMassSpaceKnown"), InertiaKnown);
            if (InertiaKnown) R->SetArrayField(TEXT("nativeInertiaMassSpaceKgCm2"), VectorJson(Inertia));
            bool GravityObserved = false, GravityEnabled = false;
            const bool GravityReadSucceeded = BI && FPhysInterface_Chaos::ExecuteRead(BI->GetPhysicsActorHandle(), [&](const FPhysicsActorHandle& Actor)
            {
                if (!FPhysInterface_Chaos::IsValid(Actor) || !FPhysInterface_Chaos::IsDynamic(Actor)) return;
                GravityEnabled = FPhysInterface_Chaos::IsGravityEnabled_AssumesLocked(Actor);
                GravityObserved = true;
            });
            R->SetBoolField(TEXT("nativeGravityReadbackKnown"), GravityReadSucceeded && GravityObserved);
            if (GravityReadSucceeded && GravityObserved) R->SetBoolField(TEXT("nativeGravityEnabled"), GravityEnabled);
            const FVector V = Body(I)->GetPhysicsLinearVelocity(), W = Body(I)->GetPhysicsAngularVelocityInRadians();
            R->SetNumberField(TEXT("vxCmps"), V.X); R->SetNumberField(TEXT("vyCmps"), V.Y); R->SetNumberField(TEXT("vzCmps"), V.Z);
            R->SetNumberField(TEXT("wxRadps"), W.X); R->SetNumberField(TEXT("wyRadps"), W.Y); R->SetNumberField(TEXT("wzRadps"), W.Z);
        }
        if (I >= 1 && I <= 6)
        {
            R->SetNumberField(TEXT("motorNm"), MotorNm[I - 1]); R->SetNumberField(TEXT("brakeNm"), BrakeNm[I - 1]);
            R->SetNumberField(TEXT("externalBearingNm"), BrakeNm[I - 1]);
            R->SetStringField(TEXT("bearingApplicationObservationScope"), bNativePreviousBoostProfile ? TEXT("Genuine external legacy passive bearing cache") : TEXT("Compatibility key externalBearingNm contains acknowledged actual PT passive bearing; no external production wheel contribution"));
            bool BrakeConfigurationKnown = false;
            if (Joints.IsValidIndex(I - 1))
            {
                const auto Ref = Joints[I - 1]->ConstraintInstance.GetPhysicsConstraintRef();
                FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadOnly(Ref, [&](const FPhysicsConstraintHandle& Locked)
                {
                    if (!Locked.IsValid() || !Locked.Constraint || FPhysInterface_Chaos::IsBroken(Locked)
                        || !Locked.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) return;
                    const auto* Actual = static_cast<Chaos::FJointConstraint*>(Locked.Constraint);
                    const auto& S = Actual->GetJointSettings();
                    BrakeConfigurationKnown = WheelBrakeSettingsMatch(S, S.bAngularTwistVelocityDriveEnabled);
                    if (!BrakeConfigurationKnown) return;
                    R->SetBoolField(TEXT("externalJointSettingsBrakeEnabled"), S.bAngularTwistVelocityDriveEnabled);
                    R->SetBoolField(TEXT("nativeBrakeEnabled"), S.bAngularTwistVelocityDriveEnabled);
                    R->SetBoolField(TEXT("nativeBrakeFreeTwist"), S.AngularMotionTypes[0] == Chaos::EJointMotionType::Free);
                    R->SetStringField(TEXT("nativeBrakeForceMode"), TEXT("Force"));
                    R->SetNumberField(TEXT("nativeBrakeDampingNmPerRadps"), S.AngularDriveDamping[0] / NmToUE);
                    R->SetNumberField(TEXT("nativeBrakeMaxTorqueNm"), S.AngularDriveMaxTorque[0] / NmToUE);
                    R->SetNumberField(TEXT("nativeBrakeTargetRadps"), S.AngularDriveVelocityTarget[0]);
                    R->SetNumberField(TEXT("nativeBrakeStiffnessNmPerRad"), S.AngularDriveStiffness[0] / NmToUE);
                });
            }
            R->SetBoolField(TEXT("externalJointSettingsBrakeConfigurationKnown"), BrakeConfigurationKnown);
            R->SetStringField(TEXT("externalJointSettingsBrakeScope"), TEXT("External FJointConstraint settings mirror; internal production brake writes do not imply this mirror is current solver state"));
            if (!bNativePreviousBoostProfile)
            {
                BrakeConfigurationKnown = HasCurrentWheelFeedback() && ActualWheelFeedback.bWheelBrakesVerified;
                R->SetBoolField(TEXT("nativeBrakeEnabled"), BrakeConfigurationKnown && ActualWheelFeedback.NativeBrakeEnabled[I - 1]);
                R->SetBoolField(TEXT("nativeBrakeFreeTwist"), BrakeConfigurationKnown);
                R->SetStringField(TEXT("nativeBrakeForceMode"), TEXT("Force"));
                R->SetNumberField(TEXT("nativeBrakeDampingNmPerRadps"), MotorProfile().BrakePeakNm / MotorProfile().BrakeRegularizationRadps);
                R->SetNumberField(TEXT("nativeBrakeMaxTorqueNm"), MotorProfile().BrakePeakNm);
                R->SetNumberField(TEXT("nativeBrakeTargetRadps"), 0);
                R->SetNumberField(TEXT("nativeBrakeStiffnessNmPerRad"), 0);
            }
            R->SetStringField(TEXT("nativeBrakeObservationScope"), bNativePreviousBoostProfile ? TEXT("Genuine external legacy joint readback") : TEXT("Latest acknowledged matching-generation/revision PT brake SetSettings/readback, separately timed from this GT body observation"));
            R->SetBoolField(TEXT("nativeBrakeConfigurationKnown"), BrakeConfigurationKnown);
            R->SetNumberField(TEXT("nativeHitRealS"), Contacts[I - 1].IssuedRealS);
            R->SetNumberField(TEXT("nativeHitActiveRealS"), Contacts[I - 1].IssuedActiveRealS);
            R->SetNumberField(TEXT("nativeHitCurrentActiveRealS"), GetWorld() ? GetWorld()->GetAudioTimeSeconds() : -1);
            R->SetStringField(TEXT("nativeSupportAgeBasis"), TEXT("UnpausedUndilatedWorldAudioClock_0.25s_WithQuietAnchorsForFrozenReceipts"));
            R->SetBoolField(TEXT("nativeObservedSupportAccepted"), HasObservedWheelSupport(I - 1));
            R->SetNumberField(TEXT("nativeHitImpulseKgCmps"), Contacts[I - 1].ImpulseKgCmps);
            R->SetStringField(TEXT("nativeHitSupport"), Contacts[I - 1].Support.IsValid() ? Contacts[I - 1].Support->GetPathName() : TEXT(""));
            const auto& Contact = Contacts[I - 1];
            const bool HitKnown = Contact.Support.IsValid() && Contact.Support->GetWorld() == GetWorld()
                && FMath::IsFinite(Contact.IssuedRealS) && Contact.IssuedRealS >= 0
                && FMath::IsFinite(Contact.ImpulseKgCmps) && !Contact.Point.ContainsNaN() && !Contact.Normal.ContainsNaN()
                && !Contact.SupportPose.ContainsNaN() && !Contact.WheelPose.ContainsNaN()
                && Contact.SupportPose.GetRotation().IsNormalized() && Contact.WheelPose.GetRotation().IsNormalized();
            R->SetBoolField(TEXT("nativeHitKnown"), HitKnown);
            if (HitKnown)
            {
                R->SetArrayField(TEXT("nativeHitPointCm"), VectorJson(Contact.Point));
                R->SetArrayField(TEXT("nativeHitNormal"), VectorJson(Contact.Normal));
                R->SetObjectField(TEXT("nativeHitSupportPose"), ContactPose(Contact.SupportPose));
                R->SetObjectField(TEXT("nativeHitWheelPose"), ContactPose(Contact.WheelPose));
                const FTransform SupportNow = Contact.Support->GetComponentTransform();
                const FTransform WheelNow = Body(I)->GetComponentTransform();
                const bool AnchorsKnown = !SupportNow.ContainsNaN() && !WheelNow.ContainsNaN()
                    && SupportNow.GetRotation().IsNormalized() && WheelNow.GetRotation().IsNormalized();
                R->SetBoolField(TEXT("nativeHitCurrentAnchorsKnown"), AnchorsKnown);
                if (AnchorsKnown)
                {
                    R->SetObjectField(TEXT("nativeHitCurrentSupportPose"), ContactPose(SupportNow));
                    R->SetObjectField(TEXT("nativeHitCurrentWheelPose"), ContactPose(WheelNow));
                }
            }
            // Explicit read-only wheel/road diagnostic; normal telemetry stays unchanged.
            if (FParse::Param(FCommandLine::Get(), TEXT("MayaV01WheelAppearance")))
                R->SetStringField(TEXT("wheelAppearanceEvidence"), MAYAReadWheelAppearanceEvidence(Body(I),
                    Cosmetics.IsValidIndex(I) ? Cosmetics[I].Get() : nullptr));
            const auto* WheelBI = Body(I)->GetBodyInstance();
            bool AwakeKnown = false, Awake = false, AwakeReadEntered = false, AwakeObserved = false;
            if (WheelBI)
            {
                const bool AwakeReadSucceeded = FPhysInterface_Chaos::ExecuteRead(WheelBI->GetPhysicsActorHandle(), [&](const FPhysicsActorHandle& Actor)
                {
                    AwakeReadEntered = true;
                    if (!FPhysInterface_Chaos::IsValid(Actor) || !FPhysInterface_Chaos::IsDynamic(Actor)) return;
                    // UE5.3 IsDynamic means !IsStatic, including kinematic actors.
                    // Actual dynamic/sleeping object state is required for this observation.
                    const auto ObjectState = Actor->GetGameThreadAPI().ObjectState();
                    if (ObjectState != Chaos::EObjectStateType::Dynamic && ObjectState != Chaos::EObjectStateType::Sleeping) return;
                    Awake = !FPhysInterface_Chaos::IsSleeping(Actor);
                    AwakeObserved = true;
                });
                AwakeKnown = AwakeReadSucceeded && AwakeReadEntered && AwakeObserved;
            }
            R->SetBoolField(TEXT("nativeWheelAwakeKnown"), AwakeKnown);
            if (AwakeKnown) R->SetBoolField(TEXT("nativeWheelAwake"), Awake);
            const FTransform A = JointInChassis[I - 1] * Body(0)->GetComponentTransform();
            const FTransform B = JointInPart[I - 1] * Body(I)->GetComponentTransform();
            R->SetNumberField(TEXT("suspensionTravelCm"), FVector::DotProduct(B.GetLocation() - A.GetLocation(), A.GetUnitAxis(EAxis::Z)));
            R->SetNumberField(TEXT("axleOmegaRadps"), FVector::DotProduct(Body(I)->GetPhysicsAngularVelocityInRadians()
                - Body(0)->GetPhysicsAngularVelocityInRadians(), A.GetUnitAxis(EAxis::X)));
        }
        if (I == 7 || I == 8)
        {
            R->SetObjectField(TEXT("nativeHingeActual"), ActualJoints[I - 1].ToSharedRef());
            R->SetBoolField(TEXT("currentDoorStateKnown"), bDoorPhysicalStateKnown[I - 7]);
            R->SetNumberField(TEXT("hingeRad"), DoorAngleRad[I - 7]); R->SetNumberField(TEXT("hingeOmegaRadps"), DoorOmegaRadps[I - 7]);
            R->SetNumberField(TEXT("gravityMomentNm"), DoorGravityNm[I - 7]); R->SetNumberField(TEXT("appliedDoorMotorNm"), DoorMotorNm[I - 7]);
        }
        if (I >= 9)
        {
            const auto* OwnBI = Body(I)->GetBodyInstance(NAME_None, false);
            R->SetNumberField(TEXT("declaredOwnerContributionKg"), OwnBI ? OwnBI->GetMassOverride() : 0);
            R->SetBoolField(TEXT("weldedIntoBase"), OwnBI && OwnBI->WeldParent == Body(0)->GetBodyInstance());
        }
        Rows.Add(MakeShared<FJsonValueObject>(R));
    }
    O->SetArrayField(TEXT("parts"), Rows);
    // Captured after all read locks so per-joint stamps cannot appear in the future.
    O->SetNumberField(TEXT("capturedRealS"), FPlatformTime::Seconds());
    FString Result;
    FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Result)); return Result;
}

void AMAYANativeDemoPawn::InvalidateWheelFeedbackEvidence()
{
    bActualWheelFeedbackKnown = false; bBoostMotorApplied = false;
    DriveAdmissionEvidence.bMotorDecisionKnown = false;
    for (double& Nm : MotorNm) Nm = 0;
    for (double& Nm : BrakeNm) Nm = 0;
}

bool AMAYANativeDemoPawn::RegisterWheelFeedback()
{
    if (bWheelFeedbackRetirementFailed) return false;
    if (WheelFeedbackCallback) return true;
    auto* World = GetWorld();
    auto* Scene = World ? World->GetPhysicsScene() : nullptr;
    auto* Solver = Scene ? Scene->GetSolver() : nullptr;
    if (!Solver || bEnding || World->bIsTearingDown || Joints.Num() != 8) return false;
    FMAYAWheelFeedbackSetup Setup;
    Setup.Generation = ResetGeneration; Setup.BaseProfile = MotorProfile();
    for (int32 I = 0; I < 9; ++I)
    {
        auto* BI = Body(I) ? Body(I)->GetBodyInstance() : nullptr;
        auto* Proxy = BI && BI->IsValidBodyInstance() ? BI->GetPhysicsActorHandle() : nullptr;
        if (!Proxy || Proxy->GetSolverBase() != Solver) return false;
        Setup.ParticleIds[I] = Proxy->GetGameThreadAPI().UniqueIdx().Idx;
        if (Setup.ParticleIds[I] == INDEX_NONE) return false;
        for (int32 J = 0; J < I; ++J) if (Setup.ParticleIds[J] == Setup.ParticleIds[I]) return false;
    }
    for (int32 I = 0; I < 8; ++I)
    { Setup.JointInChassis[I] = JointInChassis[I]; Setup.JointInPart[I] = JointInPart[I]; }
    WheelFeedbackControl = MakeShared<FMAYAWheelFeedbackControl, ESPMode::ThreadSafe>();
    WheelFeedbackControl->Generation = ResetGeneration; WheelFeedbackControl->bRetiring = false;
    Setup.Control = WheelFeedbackControl;
    WheelFeedbackSolver = Solver; WheelFeedbackWorld = World;
    WheelFeedbackWorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &ThisClass::OnWheelFeedbackWorldTearDown);
    WheelFeedbackCallback = Solver->CreateAndRegisterSimCallbackObject_External<FMAYAWheelFeedbackCallback>();
    if (!WheelFeedbackCallback)
    {
        FWorldDelegates::OnWorldBeginTearDown.Remove(WheelFeedbackWorldTearDownHandle);
        WheelFeedbackWorldTearDownHandle.Reset(); WheelFeedbackControl.Reset(); WheelFeedbackSolver = nullptr; WheelFeedbackWorld.Reset();
        return false;
    }
    bWheelFeedbackActivationPending = true; LastWheelFeedbackStep = 0;
    InvalidateWheelFeedbackEvidence(); LastPublishedWheelCommand = FMAYAWheelFeedbackCommand();
    auto* Input = WheelFeedbackCallback->GetProducerInputData_External();
    Input->bHasSetup = true; Input->Setup = Setup;
    PublishWheelFeedbackCommand(); RecordWheelFeedbackOwnership(TEXT("ProductionBlockedRegistration"));
    return true;
}

bool AMAYANativeDemoPawn::RetireWheelFeedback()
{
    if (bWheelFeedbackRetirementFailed) return false;
    if (!WheelFeedbackCallback) return true;
    if (WheelFeedbackControl)
    {
        FScopeLock Lock(&WheelFeedbackControl->Mutex);
        WheelFeedbackControl->bRetiring = true; ++WheelFeedbackControl->Revision;
    }
    InvalidateWheelFeedbackEvidence();
    // Establish the still-live registered world/scene before any solver call.
    auto* World = WheelFeedbackWorld.Get();
    auto* Scene = World ? World->GetPhysicsScene() : nullptr;
    if (!Scene || Scene->GetSolver() != WheelFeedbackSolver)
    { bWheelFeedbackRetirementFailed = true; Fault = TEXT("Wheel feedback retirement has no established live registered scene; body mutation refused"); return false; }
    while (auto Output = WheelFeedbackCallback->PopFutureOutputData_External()) {}
    WheelFeedbackSolver->UnregisterAndFreeSimCallbackObject_External(WheelFeedbackCallback);
    WheelFeedbackCallback = nullptr;
    auto Fence = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
    WheelFeedbackSolver->EnqueueCommandImmediate([Fence]() { Fence->Store(true); });
    Scene->Flush(); // No ownership mutex is held across marshal/wait.
    if (!Fence->Load())
    { bWheelFeedbackRetirementFailed = true; Fault = TEXT("Wheel feedback retirement removal fence did not complete; body mutation refused"); return false; }
    FWorldDelegates::OnWorldBeginTearDown.Remove(WheelFeedbackWorldTearDownHandle);
    WheelFeedbackWorldTearDownHandle.Reset(); WheelFeedbackSolver = nullptr; WheelFeedbackWorld.Reset();
    WheelFeedbackControl.Reset(); bWheelFeedbackActivationPending = true;
    RecordWheelFeedbackOwnership(TEXT("ProductionRetirementFenceCompleted"));
    return true;
}

void AMAYANativeDemoPawn::OnWheelFeedbackWorldTearDown(UWorld* World)
{
    if (WheelFeedbackWorld.Get() != World) return;
    ClearInput(); bWheelExternalWritesStopped = true;
    RetireWheelFeedback(); // Before the registered world's physics scene teardown.
}

void AMAYANativeDemoPawn::PublishWheelFeedbackCommand()
{
    if (!WheelFeedbackCallback || !WheelFeedbackControl) return;
    FMAYAWheelFeedbackCommand C;
    C.Generation = ResetGeneration; C.Sequence = InputSequence;
    C.IssuedRealS = InputIssuedRealS; C.LeaseSeconds = InputLeaseS;
    C.Throttle = Throttle; C.Steer = Steer; C.bBrakeRequested = bBrake; C.bBoostRequested = bBoostRequested;
    C.bReady = bReady && !bEnding && !bWheelFeedbackActivationPending && !bWheelComparisonSelectionPending;
    C.bFreshPermission = HasFreshDriveCommand(); C.bDriveInhibited = bDriveInhibited;
    C.bDeliveryLocked = bDeliveryDriveLocked; C.bInputsNeedRelease = bInputsNeedRelease;
    C.bClosedLatched = DoorState == EDoorState::ClosedLatched;
    const auto& P = LastPublishedWheelCommand;
    // Ordinary intent/Boost updates use Sequence; only safety revocation/admission changes Revision.
    const bool Changed = C.Generation != P.Generation
        || C.bBrakeRequested != P.bBrakeRequested || (P.bBoostRequested && !C.bBoostRequested)
        || C.bReady != P.bReady || C.bFreshPermission != P.bFreshPermission || C.bDriveInhibited != P.bDriveInhibited
        || C.bDeliveryLocked != P.bDeliveryLocked || C.bInputsNeedRelease != P.bInputsNeedRelease || C.bClosedLatched != P.bClosedLatched;
    {
        FScopeLock Lock(&WheelFeedbackControl->Mutex);
        if (WheelFeedbackControl->bRetiring) return;
        if (Changed) { ++WheelFeedbackControl->Revision; InvalidateWheelFeedbackEvidence(); }
        C.Revision = WheelFeedbackControl->Revision;
    }
    LastPublishedWheelCommand = C;
    WheelFeedbackCallback->GetProducerInputData_External()->Command = C;
}

bool AMAYANativeDemoPawn::HasCurrentWheelFeedback() const
{
    if (!bActualWheelFeedbackKnown || !WheelFeedbackCallback || !WheelFeedbackControl || bNativePreviousBoostProfile) return false;
    const double Age = FPlatformTime::Seconds() - ActualWheelFeedback.SampleRealS;
    FScopeLock Lock(&WheelFeedbackControl->Mutex);
    return !WheelFeedbackControl->bRetiring && ActualWheelFeedback.bOwnerCurrent
        && ActualWheelFeedback.Generation == ResetGeneration && ActualWheelFeedback.Generation == WheelFeedbackControl->Generation
        && ActualWheelFeedback.Revision == WheelFeedbackControl->Revision
        && FMath::IsFinite(Age) && Age >= 0 && Age <= InputMaxAgeS;
}

bool AMAYANativeDemoPawn::HasCompletedBlockedWheelFeedback() const
{
    if (!HasCurrentWheelFeedback() || !ActualWheelFeedback.bNativeStateFinite || !ActualWheelFeedback.bActualClosedLocks
        || !ActualWheelFeedback.bNineBodiesBound || !ActualWheelFeedback.bEightJointIdentities
        || !ActualWheelFeedback.bWheelBrakesVerified || ActualWheelFeedback.Decision.bDriveAdmitted) return false;
    for (int32 W = 0; W < 6; ++W)
        if (!ActualWheelFeedback.NativeBrakeEnabled[W] || !ActualWheelFeedback.AppliedPairKnown[W]
            || !FMath::IsFinite(ActualWheelFeedback.Decision.MotorNm[W]) || FMath::Abs(ActualWheelFeedback.Decision.MotorNm[W]) > 1.e-6) return false;
    return true; // Only PopOutputData_External supplied this finalized, nonfuture receipt.
}

void AMAYANativeDemoPawn::DrainWheelFeedback()
{
    if (!WheelFeedbackCallback || !WheelFeedbackControl) return;
    while (auto Output = WheelFeedbackCallback->PopOutputData_External())
    {
        const auto& S = Output->Snapshot;
        uint64 Revision = 0; bool Retiring = true;
        { FScopeLock Lock(&WheelFeedbackControl->Mutex); Revision = WheelFeedbackControl->Revision; Retiring = WheelFeedbackControl->bRetiring; }
        if (Retiring || !S.bOwnerCurrent || S.Generation != ResetGeneration || S.Revision != Revision
            || S.StepSequence <= LastWheelFeedbackStep || !FMath::IsFinite(S.SampleRealS)
            || !FMath::IsFinite(S.DeltaSeconds) || S.DeltaSeconds <= 0) continue;
        LastWheelFeedbackStep = S.StepSequence; ActualWheelFeedback = S; bActualWheelFeedbackKnown = true;
        const auto& C = S.Operands.Command; const auto& D = S.Decision;
        auto& E = DriveAdmissionEvidence;
        E.bMotorDecisionKnown = true; E.MotorFrame = GFrameCounter;
        E.MotorWorldS = GetWorld() ? GetWorld()->GetTimeSeconds() : -1; E.MotorRealS = S.SampleRealS;
        E.CommandSequence = S.Sequence; E.CommandIssuedRealS = C.IssuedRealS;
        E.RequestedThrottle = C.Throttle; E.RequestedSteer = C.Steer; E.bFresh = D.bFresh;
        E.bBrakeRequested = C.bBrakeRequested; E.bDoorInhibitedAtMotorDecision = C.bDriveInhibited;
        E.bDeliveryLocked = C.bDeliveryLocked; E.bInputsNeedRelease = C.bInputsNeedRelease;
        E.bBoostRequested = C.bBoostRequested; E.bPreviousBoostProfile = false;
        E.bReserveSpeedSampleAttempted = C.bBoostRequested && D.bDriveAdmitted;
        E.bReserveNativeBodyAvailable = S.bChassisNativeBodyAvailable; E.bReserveSpeedKnown = S.bChassisSpeedKnown;
        E.ReserveChassisVelocityCmps = S.ChassisVelocityCmps; E.ReserveChassisSpeedCmps = S.Operands.ChassisSpeedCmps;
        E.ReserveSpeedSampleRealS = S.SampleRealS; E.LowSpeedReserveFactor = D.ReserveFactor;
        const auto Road = MAYADeliveryBoostProfile::Apply(MotorProfile(), true, false);
        E.ConfiguredCurrentBoostRoadPeakNm = Road.MotorPeakNm;
        E.ConfiguredCurrentBoostMaximumReservePeakNm = Road.MotorPeakNm + MAYADeliveryBoostProfile::LowSpeedReserveNm;
        E.MotorPeakNm = D.SelectedPeakNm; E.MotorNoLoadRadps = D.NoLoadRadps;
        E.MotorErrorNmPerRadps = MotorProfile().MotorErrorNmPerRadps;
        E.bCurrentBoostPolicyEligible = C.bBoostRequested && D.bDriveAdmitted;
        for (int32 W = 0; W < 6; ++W)
        {
            E.RelativeWheelOmegaRadps[W] = FMath::IsFinite(S.Operands.RelativeWheelOmegaRadps[W]) ? S.Operands.RelativeWheelOmegaRadps[W] : 0;
            E.UnclampedBankRequest[W] = double(-C.Throttle + (W < 3 ? -C.Steer : C.Steer));
            E.ClampedBankRequest[W] = FMath::Clamp(E.UnclampedBankRequest[W], -1.0, 1.0);
            E.WheelInputSampleRealS[W] = S.SampleRealS; E.WheelInputSampled[W] = S.bNativeStateFinite;
            E.WheelDriveAdmitted[W] = D.bDriveAdmitted && S.AppliedPairKnown[W];
            E.OriginalLawMotorNm[W] = D.MotorDecisions[W].OriginalLawNm;
            E.CounterTorqueSuppressed[W] = D.MotorDecisions[W].bCounterTorqueSuppressed;
            E.MotorNmAfterPolicy[W] = D.MotorDecisions[W].AfterPolicyNm;
            E.ExpectedAppliedMotorNm[W] = S.CalculatedDecision.MotorNm[W];
            MotorNm[W] = E.AppliedMotorNm[W] = D.MotorNm[W];
            BrakeNm[W] = E.PassiveBearingNm[W] = D.BearingNm[W];
            E.VerifiedNativeBrakeEnabled[W] = S.NativeBrakeEnabled[W];
        }
        E.bDriveAdmitted = D.bDriveAdmitted; bBoostMotorApplied = E.bBoostMotorApplied = D.bBoostActive;
        E.bBoostDriveActiveAtMotorDecision = D.bBoostActive; E.ActualBoostEffortFractionAtMotorDecision = D.EffortFraction;
    }
    if (bWheelFeedbackActivationPending && !bWheelComparisonSelectionPending && HasCompletedBlockedWheelFeedback())
    { bWheelFeedbackActivationPending = false; RecordWheelFeedbackOwnership(TEXT("InitialProductionBlockedCompletionAcknowledged")); }
}

void AMAYANativeDemoPawn::ProgressWheelFeedbackSelection()
{
    if (!bWheelComparisonSelectionPending) return;
    FMAYADeliveryRobotObservation O;
    if (!ReadDeliveryState(O) || !O.IsStoppedAndSupported() || !O.bDoorsClosedLatched
        || !AreDeliveryDoorsClosedAndPhysicallyLocked() || !HasFreshDriveCommand() || !bBrake || bBoostRequested
        || !FMath::IsNearlyZero(Throttle) || !FMath::IsNearlyZero(Steer) || !HasCompletedBlockedWheelFeedback()) return;
    RecordWheelFeedbackOwnership(TEXT("FinalizedNonfutureZeroMotorNativeBrakeQuietAcknowledged"));
    if (bWheelComparisonDesiredPrevious)
    {
        if (!RetireWheelFeedback()) { Fail(TEXT("Legacy selector retirement unavailable")); return; }
        bNativePreviousBoostProfile = true;
    }
    else bWheelFeedbackActivationPending = false;
    bWheelComparisonSelectionPending = false; bWheelExternalWritesStopped = false;
    PublishWheelFeedbackCommand(); RecordWheelFeedbackOwnership(TEXT("SelectedOwnerReadyAfterCompletedBoundary"));
}

bool AMAYANativeDemoPawn::IsNativeBoostComparisonProfileReady(bool bPreviousProfile) const
{
    if (bWheelComparisonSelectionPending || bNativePreviousBoostProfile != bPreviousProfile) return false;
    return bPreviousProfile ? !WheelFeedbackCallback && !bWheelExternalWritesStopped
        : WheelFeedbackCallback && !bWheelFeedbackActivationPending;
}

void AMAYANativeDemoPawn::RecordWheelFeedbackOwnership(const TCHAR* Event)
{
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), Event);
    J->SetNumberField(TEXT("worldSeconds"), GetWorld() ? GetWorld()->GetTimeSeconds() : -1);
    J->SetNumberField(TEXT("platformRealSeconds"), FPlatformTime::Seconds());
    J->SetNumberField(TEXT("resetGeneration"), double(ResetGeneration));
    J->SetBoolField(TEXT("legacyExternalSelected"), bNativePreviousBoostProfile);
    J->SetBoolField(TEXT("productionCallbackRegistered"), WheelFeedbackCallback != nullptr);
    J->SetBoolField(TEXT("selectionPending"), bWheelComparisonSelectionPending);
    J->SetBoolField(TEXT("externalWritesStopped"), bWheelExternalWritesStopped);
    J->SetNumberField(TEXT("acknowledgedStepSequence"), double(ActualWheelFeedback.StepSequence));
    J->SetNumberField(TEXT("acknowledgedGeneration"), double(ActualWheelFeedback.Generation));
    J->SetNumberField(TEXT("acknowledgedRevision"), double(ActualWheelFeedback.Revision));
    J->SetNumberField(TEXT("acknowledgedSimStartSeconds"), ActualWheelFeedback.SimTimeS);
    J->SetNumberField(TEXT("acknowledgedDtSeconds"), ActualWheelFeedback.DeltaSeconds);
    J->SetStringField(TEXT("acknowledgementScope"), TEXT("Nonfuture finalized output follows EndFrame; sim timestamp is step start. Retirement event is after live-scene solver-removal fence/flush; no broad torque clear."));
    ++WheelFeedbackOwnershipTransitionsSeen;
    if (WheelFeedbackOwnershipTransitions.Num() < 64) WheelFeedbackOwnershipTransitions.Add(MakeShared<FJsonValueObject>(J));
}

FString AMAYANativeDemoPawn::ReadWheelFeedbackEvidenceJson(bool bIncludeOwnershipHistory) const
{
    auto J = MakeShared<FJsonObject>();
    const auto& S = ActualWheelFeedback; const auto& C = S.Operands.Command; const auto& D = S.Decision;
    J->SetStringField(TEXT("schema"), TEXT("MayaRobotics.NativeWheelSolverFeedback.v1"));
    J->SetStringField(TEXT("scope"), TEXT("One permanent production PT wheel motor/bearing/native-brake owner; GT intent separately stamped. Latest nonfuture finalized callback output: current internal operands at callback start, torque/brake decisions before evolution, EndFrame completed before output publication. Genuine external legacy route only after explicit zero/brake/Quiet retirement boundary."));
    J->SetBoolField(TEXT("productionCallbackRegistered"), WheelFeedbackCallback != nullptr);
    J->SetBoolField(TEXT("legacyExternalSelected"), bNativePreviousBoostProfile);
    J->SetBoolField(TEXT("initialBlockedCompletionPending"), bWheelFeedbackActivationPending);
    J->SetBoolField(TEXT("selectionPending"), bWheelComparisonSelectionPending);
    J->SetBoolField(TEXT("matchingActualOutputKnown"), HasCurrentWheelFeedback());
    J->SetNumberField(TEXT("generation"), double(S.Generation)); J->SetNumberField(TEXT("revision"), double(S.Revision));
    J->SetNumberField(TEXT("consumedCommandSequence"), double(S.Sequence)); J->SetNumberField(TEXT("actualCallbackSequence"), double(S.StepSequence));
    J->SetNumberField(TEXT("callbackStartSimSeconds"), S.SimTimeS); J->SetNumberField(TEXT("callbackDtSeconds"), S.DeltaSeconds);
    J->SetNumberField(TEXT("actualInputSampleRealSeconds"), S.SampleRealS);
    J->SetNumberField(TEXT("gtObservationRealSeconds"), FPlatformTime::Seconds());
    J->SetNumberField(TEXT("gtCurrentCommandSequence"), double(InputSequence));
    J->SetNumberField(TEXT("gtRequestedThrottle"), Throttle); J->SetNumberField(TEXT("gtRequestedSteer"), Steer);
    J->SetBoolField(TEXT("ownerCurrentAtApplication"), S.bOwnerCurrent);
    J->SetBoolField(TEXT("nineBodiesBound"), S.bNineBodiesBound); J->SetBoolField(TEXT("eightJointIdentities"), S.bEightJointIdentities);
    J->SetBoolField(TEXT("nativeStateFinite"), S.bNativeStateFinite); J->SetBoolField(TEXT("actualClosedLocksAndPose"), S.bActualClosedLocks);
    J->SetBoolField(TEXT("nativeBrakeReadbackVerified"), S.bWheelBrakesVerified);
    J->SetBoolField(TEXT("freshAtApplication"), D.bFresh); J->SetBoolField(TEXT("driveAdmittedAtApplication"), D.bDriveAdmitted);
    J->SetBoolField(TEXT("boostActiveAtApplication"), D.bBoostActive);
    J->SetNumberField(TEXT("selectedPeakNm"), D.SelectedPeakNm); J->SetNumberField(TEXT("selectedNoLoadRadps"), D.NoLoadRadps);
    J->SetNumberField(TEXT("lowSpeedReserveFactor"), D.ReserveFactor); J->SetNumberField(TEXT("actualSameStepEffortFraction"), D.EffortFraction);
    J->SetNumberField(TEXT("consumedIssuedRealSeconds"), C.IssuedRealS); J->SetNumberField(TEXT("consumedLeaseSeconds"), C.LeaseSeconds);
    J->SetBoolField(TEXT("currentChassisNativeBodyAvailable"), S.bChassisNativeBodyAvailable);
    auto Consumed = MakeShared<FJsonObject>();
    Consumed->SetNumberField(TEXT("generation"), double(C.Generation)); Consumed->SetNumberField(TEXT("revision"), double(C.Revision));
    Consumed->SetNumberField(TEXT("sequence"), double(C.Sequence));
    Consumed->SetBoolField(TEXT("ready"), C.bReady); Consumed->SetBoolField(TEXT("freshPermission"), C.bFreshPermission);
    Consumed->SetBoolField(TEXT("brakeRequested"), C.bBrakeRequested); Consumed->SetBoolField(TEXT("boostRequested"), C.bBoostRequested);
    Consumed->SetBoolField(TEXT("driveInhibited"), C.bDriveInhibited); Consumed->SetBoolField(TEXT("deliveryLocked"), C.bDeliveryLocked);
    Consumed->SetBoolField(TEXT("inputsNeedRelease"), C.bInputsNeedRelease); Consumed->SetBoolField(TEXT("closedLatched"), C.bClosedLatched);
    if (FMath::IsFinite(C.Throttle)) Consumed->SetNumberField(TEXT("throttle"), C.Throttle);
    if (FMath::IsFinite(C.Steer)) Consumed->SetNumberField(TEXT("steer"), C.Steer);
    J->SetObjectField(TEXT("consumedCommand"), Consumed);
    J->SetBoolField(TEXT("evaluationNativeStateFinite"), S.Operands.bNativeStateFinite);
    J->SetBoolField(TEXT("evaluationActualClosedLocks"), S.Operands.bActualClosedLocks);
    J->SetBoolField(TEXT("sampledChassisSpeedKnown"), S.bChassisSpeedKnown);
    if (S.bChassisSpeedKnown) J->SetNumberField(TEXT("sampledChassisSpeedCmps"), S.Operands.ChassisSpeedCmps);
    TArray<TSharedPtr<FJsonValue>> Wheels;
    for (int32 W = 0; W < 6; ++W)
    {
        auto R = MakeShared<FJsonObject>(); R->SetNumberField(TEXT("wheel"), W);
        R->SetBoolField(TEXT("actualPairApplicationKnown"), S.AppliedPairKnown[W]);
        R->SetBoolField(TEXT("nativeBrakeEnabled"), S.NativeBrakeEnabled[W]);
        R->SetNumberField(TEXT("actualAppliedMotorNm"), D.MotorNm[W]); R->SetNumberField(TEXT("actualAppliedBearingNm"), D.BearingNm[W]);
        if (FMath::IsFinite(S.Operands.RelativeWheelOmegaRadps[W])) R->SetNumberField(TEXT("sampledRelativeOmegaRadps"), S.Operands.RelativeWheelOmegaRadps[W]);
        const double Raw = double(-C.Throttle + (W < 3 ? -C.Steer : C.Steer));
        if (FMath::IsFinite(Raw)) { R->SetNumberField(TEXT("rawBankRequest"), Raw); R->SetNumberField(TEXT("clampedBankRequest"), FMath::Clamp(Raw, -1.0, 1.0)); }
        R->SetNumberField(TEXT("originalCurveNm"), D.MotorDecisions[W].OriginalLawNm);
        R->SetBoolField(TEXT("coastSuppressed"), D.MotorDecisions[W].bCounterTorqueSuppressed);
        R->SetNumberField(TEXT("afterCoastNm"), D.MotorDecisions[W].AfterPolicyNm);
        if (!S.WheelAxes[W].ContainsNaN())
        {
            TArray<TSharedPtr<FJsonValue>> Axis;
            Axis.Add(MakeShared<FJsonValueNumber>(S.WheelAxes[W].X)); Axis.Add(MakeShared<FJsonValueNumber>(S.WheelAxes[W].Y)); Axis.Add(MakeShared<FJsonValueNumber>(S.WheelAxes[W].Z));
            R->SetArrayField(TEXT("sampledWheelAxisWorld"), Axis);
        }
        Wheels.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetArrayField(TEXT("wheels"), Wheels);
    if (bIncludeOwnershipHistory)
    {
        J->SetArrayField(TEXT("ownershipTransitions"), WheelFeedbackOwnershipTransitions);
        J->SetNumberField(TEXT("ownershipTransitionsSeen"), double(WheelFeedbackOwnershipTransitionsSeen));
        J->SetNumberField(TEXT("ownershipTransitionsRetained"), WheelFeedbackOwnershipTransitions.Num());
        J->SetBoolField(TEXT("ownershipTransitionsTruncated"), WheelFeedbackOwnershipTransitionsSeen > uint64(WheelFeedbackOwnershipTransitions.Num()));
    }
    FString Text; auto Writer = TJsonWriterFactory<>::Create(&Text); FJsonSerializer::Serialize(J, Writer); return Text;
}
