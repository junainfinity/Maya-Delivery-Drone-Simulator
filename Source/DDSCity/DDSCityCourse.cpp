#include "DDSCityCourse.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ADDSCityCourse::ADDSCityCourse()
{
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CourseRoot"));
    RootComponent = SceneRoot;
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Cube.Succeeded())
    {
        CubeMesh = Cube.Object;
    }
}

void ADDSCityCourse::AddBox(const TCHAR* Name, const FVector& Center, const FVector& Size, const FRotator& Rotation, bool bRecoverable)
{
    UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this, FName(Name));
    Part->SetupAttachment(SceneRoot);
    Part->SetStaticMesh(CubeMesh);
    Part->SetRelativeLocation(Center);
    Part->SetRelativeRotation(Rotation);
    Part->SetRelativeScale3D(Size / 100.0);
    Part->SetCollisionProfileName(TEXT("BlockAll"));
    if (bRecoverable)
    {
        Part->ComponentTags.Add(TEXT("DDSRecoverableSidewalk"));
    }
    AddInstanceComponent(Part);
    Part->RegisterComponent();
}

void ADDSCityCourse::BeginPlay()
{
    Super::BeginPlay();
    if (!CubeMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("N06 course cube mesh missing"));
        return;
    }

    // Centimeter fixture: flat sidewalk top Z=0, blocking wall, ten-degree ramp,
    // three-centimeter seam, L corner, and split/single-wheel support islands.
    AddBox(TEXT("Sidewalk"), FVector(300, 0, -20), FVector(2000, 1000, 40), FRotator::ZeroRotator, true);
    AddBox(TEXT("BlockingWall"), FVector(700, 0, 90), FVector(20, 300, 200), FRotator::ZeroRotator, false);
    AddBox(TEXT("CornerWall"), FVector(820, 145, 90), FVector(260, 20, 200), FRotator::ZeroRotator, false);
    AddBox(TEXT("SeamThreeCm"), FVector(150, -320, 1.5), FVector(150, 100, 3), FRotator::ZeroRotator, true);
    AddBox(TEXT("SplitCurb"), FVector(-280, 255, 3), FVector(80, 90, 6), FRotator::ZeroRotator, false);
    AddBox(TEXT("SingleWheelCurb"), FVector(-500, 330, 4), FVector(20, 20, 8), FRotator::ZeroRotator, false);
    // +10 pitch raises the +X exit; the -X top edge meets the Z=0 sidewalk.
    AddBox(TEXT("TenDegreeRamp"), FVector(235, 390, 23.1), FVector(300, 160, 6), FRotator(10, 0, 0), true);
    AddBox(TEXT("RampUpperLanding"), FVector(550, 390, 32.1), FVector(330, 160, 40), FRotator::ZeroRotator, true);
    AddBox(TEXT("TenDegreeDownRamp"), FVector(865, 390, 23.1), FVector(300, 160, 6), FRotator(-10, 0, 0), true);
    AddBox(TEXT("LowOverhang"), FVector(1070, 100, 100), FVector(150, 100, 80), FRotator::ZeroRotator, false);
    AddBox(TEXT("SideObstacle"), FVector(1100, -100, 40), FVector(100, 20, 80), FRotator::ZeroRotator, false);
}
