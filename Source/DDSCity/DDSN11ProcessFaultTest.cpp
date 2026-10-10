#include "DDSN11Storage.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Companion for staging-handoff/N11-ProcessFaultHarness.ps1. This deliberately
// uses a new isolated Saved/SaveGames/N11FaultFixtures/<run-id> directory only
// when run later under the exclusive project writer lock. It is never called
// while this source is merely staged. Hard termination is performed by the
// harness against its own child process, not by this automation test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDDSN11ProcessFaultTest,
    "DDSCity.N11.ProcessFault", EAutomationTestFlags::EditorContext |
    EAutomationTestFlags::EngineFilter)

bool FDDSN11ProcessFaultTest::RunTest(const FString& Parameters)
{
    FString Stage, RunId, VerifyFault;
    FParse::Value(FCommandLine::Get(), TEXT("N11Stage="), Stage);
    FParse::Value(FCommandLine::Get(), TEXT("N11RunId="), RunId);
    FParse::Value(FCommandLine::Get(), TEXT("N11VerifyFault="), VerifyFault);
    if (RunId.IsEmpty() || RunId.Len() > 64 || Stage.IsEmpty())
    {
        AddError(TEXT("Specify bounded N11Stage and N11RunId"));
        return false;
    }
    for (TCHAR C : RunId)
    {
        if (!FChar::IsAlnum(C) && C != TCHAR('-'))
        {
            AddError(TEXT("N11RunId may contain only letters, digits and dash"));
            return false;
        }
    }
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("SaveGames"), TEXT("N11FaultFixtures"), RunId);
    FDDSN11UnrealFileIO IO;
    FDDSN11SerializedStore Store(IO, Directory);
    auto MakePose = []()
    {
        FDDSN11Pose P;
        P.LocationCm = FVector(140.0, 70.0, 20.0);
        P.Rotation = FRotator(0.0, 90.0, 0.0);
        P.bGroundedSidewalkValidated = true; // Controlled pose fixture only.
        P.bClosedLatchedReconstructionValidated = true;
        P.bOccupancyValidated = true;
        return P;
    };
    auto MakeSnapshot = [](int32 Phase)
    {
        FDDSN08StableSnapshot S;
        S.MissionInstanceId = TEXT("process-fixture-instance");
        S.CargoId = TEXT("process-fixture-parcel");
        S.PickupStationId = TEXT("cafe");
        S.DeliveryStationId = TEXT("office");
        S.ScopedStarIds = { TEXT("star-a"), TEXT("star-b"), TEXT("star-c") };
        S.CollectedStarIds = { TEXT("star-a") };
        S.ElapsedGameplaySeconds = 11.0;
        if (Phase >= 1)
        {
            FDDSN08Receipt R;
            R.MissionInstanceId = S.MissionInstanceId;
            R.PrePhase = EDDSN08Phase::AwaitingPickup;
            R.StationId = S.PickupStationId;
            R.OperationId = TEXT("pickup-op");
            R.Generation = 1;
            R.CargoId = S.CargoId;
            S.Receipts.Add(R);
            S.Phase = EDDSN08Phase::Carrying;
            S.Owner = EDDSN08Owner::Robot;
        }
        if (Phase >= 2)
        {
            FDDSN08Receipt R;
            R.MissionInstanceId = S.MissionInstanceId;
            R.PrePhase = EDDSN08Phase::Carrying;
            R.StationId = S.DeliveryStationId;
            R.OperationId = TEXT("delivery-op");
            R.Generation = 2;
            R.CargoId = S.CargoId;
            S.Receipts.Add(R);
            S.Phase = EDDSN08Phase::Completed;
            S.Owner = EDDSN08Owner::Recipient;
            S.bHasFrozenResult = true;
            S.FrozenResult.ExactStarIds = S.CollectedStarIds;
            S.FrozenResult.Count = 1;
            S.FrozenResult.FrozenElapsedSeconds = S.ElapsedGameplaySeconds;
        }
        return S;
    };

    FDDSN11Record Record;
    if (Stage == TEXT("Seed"))
    {
        if (Store.LoadLatest(Record) != EDDSN11Load::NoRecord)
        {
            AddError(TEXT("Seed requires a never-used isolated run ID"));
            return false;
        }
        Store.AuthorizeExplicitFreshMission();
        for (int32 Phase = 0; Phase <= 1; ++Phase)
        {
            if (!Store.NextRevision(Record.Revision)) return false;
            Record.Snapshot = MakeSnapshot(Phase);
            Record.Pose = MakePose();
            if (!TestTrue(TEXT("seed stable checkpoint"), Store.Save(Record))) return false;
        }
        return true;
    }

    if (Stage == TEXT("Verify"))
    {
        if (VerifyFault != TEXT("FaultAfterTemp")
            && VerifyFault != TEXT("FaultAfterMove"))
        {
            AddError(TEXT("Verify requires exact N11VerifyFault stage identity"));
            return false;
        }
        if (!TestEqual(TEXT("at least one validated checkpoint survives"),
            int32(Store.LoadLatest(Record)), int32(EDDSN11Load::Found))) return false;
        const bool bPickup = Record.Revision == 2
            && Record.Snapshot.Phase == EDDSN08Phase::Carrying
            && Record.Snapshot.Receipts.Num() == 1
            && !Record.Snapshot.bHasFrozenResult;
        const bool bDelivery = Record.Revision == 3
            && Record.Snapshot.Phase == EDDSN08Phase::Completed
            && Record.Snapshot.Receipts.Num() == 2
            && Record.Snapshot.bHasFrozenResult
            && Record.Snapshot.FrozenResult.Count == 1;
        // After-temp marker is before MoveReplace: seeded pickup rev2 must
        // remain selected. After-move marker fires only after MoveReplace has
        // returned success: a normal killed child must select the complete
        // delivery rev3 on this tested backend. This is not a power-loss claim.
        const bool bExactExpected = VerifyFault == TEXT("FaultAfterTemp")
            ? bPickup : bDelivery;
        TestTrue(TEXT("stage-specific exact revision/phase/receipt/result"),
                 bExactExpected);
        TestFalse(TEXT("loaded pose requires fresh occupancy validation"),
                  Record.Pose.bOccupancyValidated);
        const FString ResultPath = FPaths::Combine(Directory, TEXT("Verify-result.txt"));
        const FString Result = FString::Printf(
            TEXT("fault=%s\nselected_revision=%llu\nselected_phase=%d\nreceipts=%d\nfrozen_count=%d\nassertions_passed=%d\n"),
            *VerifyFault, static_cast<unsigned long long>(Record.Revision),
            int32(Record.Snapshot.Phase), Record.Snapshot.Receipts.Num(),
            Record.Snapshot.FrozenResult.Count, bExactExpected ? 1 : 0);
        const bool bResultRecorded = FFileHelper::SaveStringToFile(Result, *ResultPath);
        TestTrue(TEXT("stage-specific result artifact written"), bResultRecorded);
        UE_LOG(LogTemp, Warning, TEXT("N11_VERIFY_SELECTED fault=%s revision=%llu phase=%d pass=%d"),
               *VerifyFault, static_cast<unsigned long long>(Record.Revision),
               int32(Record.Snapshot.Phase), bExactExpected ? 1 : 0);
        return bExactExpected && bResultRecorded;
    }

    if (Stage != TEXT("FaultAfterTemp") && Stage != TEXT("FaultAfterMove"))
    {
        AddError(TEXT("Unsupported N11Stage"));
        return false;
    }
    if (Store.LoadLatest(Record) != EDDSN11Load::Found || Record.Revision != 2)
    {
        AddError(TEXT("Fault stage requires successful Seed in same run ID"));
        return false;
    }
    const FString TargetPoint = Stage == TEXT("FaultAfterTemp")
        ? TEXT("after-temp-validated") : TEXT("after-move-before-readback");
    const FString Marker = FPaths::Combine(Directory, Stage + TEXT(".marker"));
    Store.SetFaultPointHook([Marker, TargetPoint, Stage](const TCHAR* Point)
    {
        if (TargetPoint != Point) return;
        const int32 ExpectedHead = Stage == TEXT("FaultAfterTemp") ? 2 : 3;
        const FString MarkerText = FString::Printf(
            TEXT("fault=%s\npoint=%s\ncandidate_revision=3\nexpected_head_revision=%d\n"),
            *Stage, Point, ExpectedHead);
        FFileHelper::SaveStringToFile(MarkerText, *Marker);
        UE_LOG(LogTemp, Warning, TEXT("N11_FAULT_POINT %s"), Point);
        // The external harness terminates only the child process it launched.
        // If it does not, this eventually returns and the harness must fail.
        FPlatformProcess::Sleep(30.0f);
    });
    FDDSN11Record Delivery;
    if (!Store.NextRevision(Delivery.Revision)) return false;
    Delivery.Snapshot = MakeSnapshot(2);
    Delivery.Pose = MakePose();
    Store.Save(Delivery);
    AddError(TEXT("Fault process was not terminated at the marker"));
    return false;
}

#endif // WITH_DEV_AUTOMATION_TESTS
