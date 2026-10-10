#include "DDSOriginalCityCalibrationDiagnosticSubsystem.h"
#include "PhysicsInterfaceTypesCore.h"
#include "DDSOriginalRobotMeasurementProducer.h"
#include "DDSOriginalLiveIntake.h"
#include "DDSRobotLifecycleOwner.h"
#include "DDSRobotPhysicsRigComponent.h"
#include "DDSCityRollingPawn.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PBDRigidsSolver.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/Base64.h"
#include "Math/RotationMatrix.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "UObject/StrongObjectPtr.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogDDSCityCalibrationDiagnostic,Log,All);

namespace DDSCityCalibrationDiagnostic
{
constexpr TCHAR CityMap[]=TEXT("/Game/Map/Small_City_LVL");
constexpr TCHAR CityMode[]=TEXT("/Game/Gameplay/Framework/BP_CitySampleGameMode.BP_CitySampleGameMode_C");
constexpr TCHAR PublisherClass[]=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C");
constexpr TCHAR CargoMeshPath[]=TEXT("/Game/Megascans/3D_Assets/Burger_Box_sbciO_3d/Burger_Box_LOD0_Var1_sbciO.Burger_Box_LOD0_Var1_sbciO");
constexpr TCHAR NativeSha[]=TEXT("1CC79F31B10984B3DCA0A4ADB4CE0A540AFB6ED552E4C75A5DC66CEF993BB9A3");
constexpr TCHAR LedgerSha[]=TEXT("88178075AEEC58333E73C576A9ABFDF3411A209E3BAB7B7ACD0445D091BEA7DA");
constexpr TCHAR PackagePinsSha[]=TEXT("D0BCE1D2E178D62505DBB3B9D9E0236C15220682695518C99F1235E7EC561838");
constexpr TCHAR OutputParent[]=TEXT("E:/Projects/DDS City/ddscity/Evidence/LocalDevelopment/");
constexpr double CallerBudgetS=5.0;
constexpr int64 RootBudgetBytes=256ll*1024*1024,MaxReportBytes=8ll*1024*1024;
bool Live(const UObject* O)
{return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed);}
UDDSRobotPhysicsRigComponent* SoleOwnedRig(AActor* Carrier,UWorld* World)
{
    if(!Live(Carrier) || Carrier->IsActorBeingDestroyed() || Carrier->GetWorld()!=World)return nullptr;
    TArray<UDDSRobotPhysicsRigComponent*> Rigs;Carrier->GetComponents(Rigs,false);
    if(Rigs.Num()!=1)return nullptr;
    auto* Rig=Rigs[0];
    return Live(Rig) && Rig->IsRegistered() && Rig->GetOwner()==Carrier && Rig->GetOuter()==Carrier && Rig->GetWorld()==World
        && Rig->GetFName()==FName(TEXT("MeasuredPhysicalRig")) && Rig->GetClass()==UDDSRobotPhysicsRigComponent::StaticClass()?Rig:nullptr;
}
bool Literal(const FString& S,int32 Min,int32 Max,bool HexOnly=false)
{if(S.Len()<Min || S.Len()>Max)return false;for(TCHAR C:S)if(HexOnly?!FChar::IsHexDigit(C):!((C>='A' && C<='Z') || (C>='a' && C<='z') || (C>='0' && C<='9') || C=='_' || C=='-'))return false;return true;}
bool NoReparse(const FString& Full)
{
    FString Path=Full;FPaths::NormalizeFilename(Path);
    if(FPaths::IsRelative(Path) || Path.Contains(TEXT("..")) || Path.Contains(TEXT("//")) || !FPaths::ValidatePath(Path))return false;
    for(;;)
    {
        const DWORD Attributes=GetFileAttributesW(*Path);
        if(Attributes==INVALID_FILE_ATTRIBUTES || (Attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0)return false;
        const FString Parent=FPaths::GetPath(Path);
        if(Parent.IsEmpty() || Parent==Path || Path.Len()<=3)break;
        Path=Parent;if(Path.Len()==2 && Path[1]==':')Path+=TEXT("/");
    }
    return true;
}
bool HashBytes(const TArray<uint8>& Bytes,FString& Out)
{
    Out.Reset();if(Bytes.IsEmpty() || Bytes.Num()>1024*1024)return false;
    void* Dll=FPlatformProcess::GetDllHandle(TEXT("bcrypt.dll"));if(!Dll)return false;
    const auto Open=reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptOpenAlgorithmProvider")));
    using FHash=NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE,PUCHAR,ULONG,PUCHAR,ULONG,PUCHAR,ULONG);
    const auto Digest=reinterpret_cast<FHash>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptHash")));
    const auto Close=reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptCloseAlgorithmProvider")));
    BCRYPT_ALG_HANDLE Algorithm=nullptr;uint8 Raw[32]={};TArray<uint8> Copy=Bytes;bool Good=false;
    if(Open && Digest && Close && Open(&Algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0)
    {Good=Digest(Algorithm,nullptr,0,Copy.GetData(),Copy.Num(),Raw,32)>=0;Close(Algorithm,0);}FPlatformProcess::FreeDllHandle(Dll);
    if(Good)for(uint8 B:Raw){Out.AppendChar(TEXT("0123456789ABCDEF")[B>>4]);Out.AppendChar(TEXT("0123456789ABCDEF")[B&15]);}return Good;
}
bool ReadPinned(const TCHAR* Key,const TCHAR* Expected,TArray<uint8>& Bytes,FString& Error)
{
    FString Path,Digest;
    if(!FParse::Value(FCommandLine::Get(),Key,Path) || !NoReparse(Path))
    {Error=FString(TEXT("Absolute existing non-reparse input required: "))+Key;return false;}
    const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<=0 || Size>1024*1024 || !FFileHelper::LoadFileToArray(Bytes,*Path) || Bytes.Num()!=Size || !HashBytes(Bytes,Digest) || Digest!=Expected)
    {Error=FString(TEXT("Bounded root-pinned input bytes/SHA256 mismatch: "))+Key;return false;}return true;
}
struct FOutput
{
    FString Dir,Nonce;TSet<FString> Written;int64 WrittenBytes=0;
    bool Configure(FString& Error)
    {
        FString Guard;
        if(!FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationOutputRoot="),Dir)
           || !FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationNonce="),Nonce)
           || !FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationGuard="),Guard)
           || Guard!=TEXT("RootNoReparse5s256MiB") || !Literal(Nonce,32,32,true))
        {Error=TEXT("Explicit existing output root,32hex nonce and RootNoReparse5s256MiB outer guard contract required");return false;}
        FPaths::NormalizeFilename(Dir);
        if(!Dir.StartsWith(OutputParent,ESearchCase::CaseSensitive) || !Literal(Dir.Mid(UE_ARRAY_COUNT(OutputParent)-1),8,80)
           || !NoReparse(Dir) || !IFileManager::Get().DirectoryExists(*Dir))
        {Error=TEXT("Output must be an existing literal direct child under the exact LocalDevelopment root, with no reparse ancestors");return false;}
        TArray<FString> Entries;IFileManager::Get().FindFiles(Entries,*FPaths::Combine(Dir,TEXT("*")),true,true);
        if(!Entries.IsEmpty()){Error=TEXT("Fresh output child must be empty; existing evidence is never replaced");return false;}return true;
    }
    bool Guarded() const
    {
        if(!NoReparse(Dir))return false;TArray<FString> Entries;
        IFileManager::Get().FindFiles(Entries,*FPaths::Combine(Dir,TEXT("*")),true,true);
        if(Entries.Num()!=Written.Num())return false;int64 Current=0;
        for(const FString& Name:Entries)
        {if(!Written.Contains(Name))return false;const int64 Size=IFileManager::Get().FileSize(*FPaths::Combine(Dir,Name));if(Size<0)return false;Current+=Size;}
        return Current==WrittenBytes && Current<=RootBudgetBytes;
    }
    bool Write(const TCHAR* Stage,const TSharedRef<FJsonObject>& J,FString& Error)
    {
        FString Json;auto Writer=TJsonWriterFactory<>::Create(&Json);
        if(!FJsonSerializer::Serialize(J,Writer)){Error=TEXT("Diagnostic JSON serialization refused");return false;}
        FTCHARToUTF8 Utf8(*Json);const int64 Size=Utf8.Length();
        if(Size<=0 || Size>MaxReportBytes || WrittenBytes+Size>RootBudgetBytes || !Guarded())
        {Error=TEXT("Diagnostic output finite byte/root/foreign-file guard refused");return false;}
        const FString Name=TEXT("engine-owned-city-calibration-")+Nonce+TEXT("-")+Stage+TEXT(".json");
        const FString Path=FPaths::Combine(Dir,Name);
        if(Written.Contains(Name) || IFileManager::Get().FileExists(*Path) || IFileManager::Get().DirectoryExists(*Path))
        {Error=TEXT("Diagnostic output already exists; never overwrite");return false;}
        TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*Path,FILEWRITE_NoReplaceExisting));
        if(!File){Error=TEXT("Diagnostic exclusive file creation failed");return false;}
        File->Serialize(const_cast<ANSICHAR*>(Utf8.Get()),Size);const bool Good=!File->IsError() && File->Close();File.Reset();
        if(!Good || IFileManager::Get().FileSize(*Path)!=Size || !NoReparse(Path))
        {Error=TEXT("Diagnostic write/size/root reread failed; partial file retained");return false;}
        Written.Add(Name);WrittenBytes+=Size;
        if(!Guarded()){Error=TEXT("Output root/foreign-file/byte guard revoked after exclusive write; partial evidence retained");return false;}return true;
    }
};

// Values are explicitly authored GAME diagnostics. These are not supplier
// masses/kinematics; the three cited mass choices are rounded prior ENGINE models.
constexpr TCHAR HypothesisRecord[]=TEXT("{\"schema\":\"DDSCity.OriginalCityCalibrationGameHypotheses.v1\",\"id\":\"DiagGame20261001v1\",\"scope\":\"GAME hypotheses only; not supplier physical specifications or calibration\",\"massChoiceOrigin\":\"prior independent engine DefaultInstance/reference observations; rounded choices only\",\"gravityMps2\":9.80665,\"targetStepExpression\":\"1/120\",\"bareChassisKg\":37.841,\"wheelKg\":1.0,\"lid0Kg\":2.710249,\"lid1Kg\":2.421605,\"excludedParcelKg\":2.0,\"staticFriction\":0.8,\"dynamicFriction\":0.6,\"restitution\":0.05,\"motorPeakNm\":5.0,\"brakePeakNm\":8.0,\"bearingNmPerRadps\":0.02,\"lidPeakNm\":2.0,\"motorNoLoadRadps\":20.0,\"motorErrorNmPerRadps\":0.5,\"brakeRegularizationRadps\":0.1,\"suspensionK_Npm\":2500.0,\"suspensionC_Nspm\":180.0,\"suspensionForceLimitN\":500.0,\"currentStateMaxAgeS\":0.05,\"recoveryPermitMaxAgeS\":0.05,\"bindingTimeoutRealS\":1.0,\"serviceFrozen\":false,\"mechanicsFrozen\":false,\"sourceBaseHeightAboveHitCm\":45.0,\"declaredForwardInChassis\":[1,0,0],\"declaredAxleInChassis\":[0,1,0],\"declaredTravelInChassis\":[0,0,1],\"includedSuspensionSharesKg\":[0.1,0.1,0.1,0.1,0.1,0.1],\"wheelHalfTravelM\":0.02,\"restTargetM\":0.0,\"jointOrigins\":\"actual observed part origins; shared declared axis transformed into each observed rest frame\",\"adjacentCollisionSuppression\":\"explicit GAME fixture hypothesis only\",\"lidPolicy\":\"both fully locked at observed source rest; no service opening\",\"pulseTorqueNm\":0.5,\"settleSteps\":24,\"pulseStepsPerWheel\":4,\"coastStepsPerWheel\":4}");
bool Hypotheses(FDDSOriginalRobotDraftInputs& Inputs,FDDSOriginalRobotCalibrationRecipe& Recipe,FString& Error)
{
    FString Choice;if(!FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationHypothesis="),Choice) || Choice!=TEXT("DiagGame20261001v1"))
    {Error=TEXT("Explicit DiagGame20261001v1 GAME hypothesis selection required; no default profile is evidence");return false;}
    FTCHARToUTF8 U(HypothesisRecord);TArray<uint8> Bytes;Bytes.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length());FString Hash;
    if(!HashBytes(Bytes,Hash)){Error=TEXT("Actual authored GAME hypothesis record digest unavailable");return false;}
    auto& P=Inputs.DeclaredGameProfile;
    P.ProfileSha256=Hash;P.Provenance=TEXT("Explicit DiagGame20261001v1 GAME hypotheses; rounded prior engine models, not supplier specifications or native calibration");
    P.GravityMps2=9.80665;P.TargetStepS=1.0/120.0;P.BareChassisKg=37.841;P.WheelKg=1.0;P.Lid0Kg=2.710249;P.Lid1Kg=2.421605;P.ParcelKg=2.0;
    P.StaticFriction=0.8;P.DynamicFriction=0.6;P.Restitution=0.05;P.MotorPeakNm=5.0;P.BrakePeakNm=8.0;P.BearingNmPerRadps=0.02;P.LidPeakNm=2.0;
    P.MotorNoLoadRadps=20.0;P.MotorErrorNmPerRadps=0.5;P.BrakeRegularizationRadps=0.1;
    P.SuspensionK_Npm=2500.0;P.SuspensionC_Nspm=180.0;P.SuspensionForceLimitN=500.0;
    P.CurrentStateMaxAgeS=0.05;P.RecoveryPermitMaxAgeS=0.05;P.BindingTimeoutRealS=1.0;
    if(P.Service.bFrozen || !DDSValidateRobotProfile(P,Error))return false;
    Inputs.bExplicitGameTargets=true;Recipe.bExplicitGameFixtureHypotheses=true;
    Recipe.HypothesisDescription=HypothesisRecord;Recipe.DeclaredForwardInChassis=FVector(1,0,0);
    Recipe.PulseTorqueNm=0.5;Recipe.SettleSteps=24;Recipe.PulseStepsPerWheel=4;Recipe.CoastStepsPerWheel=4;
    for(double& Share:Recipe.DeclaredSuspensionMassAllocationKg)Share=0.1;
    return true;
}
void Roles(FDDSOriginalLiveIntakeInputs& Inputs)
{
    struct FRole {int32 Node;EDDSOriginalRobotPartRole Role;int32 Index;const TCHAR* Name;};
    const FRole Map[]={
        {1,EDDSOriginalRobotPartRole::Body,0,TEXT("SM_DeliveryRobot_Base")},
        {13,EDDSOriginalRobotPartRole::Suspension,5,TEXT("SM_DeliveryRobot_Suspension5")},
        {14,EDDSOriginalRobotPartRole::Body,5,TEXT("SM_DeliveryRobot_Wheel4")},
        {12,EDDSOriginalRobotPartRole::Suspension,4,TEXT("SM_DeliveryRobot_Suspension4")},
        {15,EDDSOriginalRobotPartRole::Body,6,TEXT("SM_DeliveryRobot_Wheel5")},
        {10,EDDSOriginalRobotPartRole::Suspension,3,TEXT("SM_DeliveryRobot_Suspension3")},
        {11,EDDSOriginalRobotPartRole::Body,4,TEXT("SM_DeliveryRobot_Wheel3")},
        {8,EDDSOriginalRobotPartRole::Suspension,2,TEXT("SM_DeliveryRobot_Suspension2")},
        {9,EDDSOriginalRobotPartRole::Body,3,TEXT("SM_DeliveryRobot_Wheel2")},
        {6,EDDSOriginalRobotPartRole::Suspension,1,TEXT("SM_DeliveryRobot_Suspension1")},
        {7,EDDSOriginalRobotPartRole::Body,2,TEXT("SM_DeliveryRobot_Wheel1")},
        {4,EDDSOriginalRobotPartRole::Suspension,0,TEXT("SM_DeliveryRobot_Suspension")},
        {5,EDDSOriginalRobotPartRole::Body,1,TEXT("SM_DeliveryRobot_Wheel")},
        {3,EDDSOriginalRobotPartRole::Body,7,TEXT("SM_DeliveryRobot_Door1")},
        {2,EDDSOriginalRobotPartRole::Body,8,TEXT("SM_DeliveryRobot_Door2")}};
    static_assert(UE_ARRAY_COUNT(Map)==15,"Exact source bookkeeping role roster");
    for(const auto& M:Map)
    {FDDSOriginalLiveRole R;R.NativeNodeObjectPath=FString::Printf(TEXT("%s:SimpleConstructionScript_0.SCS_Node_%d"),PublisherClass,M.Node);
     R.Role=M.Role;R.Index=M.Index;R.TargetComponentName=FName(*(FString(TEXT("DDSDiag_"))+M.Name));R.SuppliedPartId=M.Name;Inputs.Original15Roles.Add(R);}
}
}

namespace DDSCityCalibrationDiagnostic
{
struct FJsonNumbers
{
    bool bFinite=true;
    TSharedPtr<FJsonValue> Value(double N)
    {if(FMath::IsFinite(N))return MakeShared<FJsonValueNumber>(N);bFinite=false;return MakeShared<FJsonValueNull>();}
    void Number(const TSharedRef<FJsonObject>& J,const TCHAR* Key,double N)
    {J->SetField(Key,Value(N));}
    void Vector(const TSharedRef<FJsonObject>& J,const TCHAR* Key,const FVector& V)
    {J->SetArrayField(Key,{Value(V.X),Value(V.Y),Value(V.Z)});}
    void Pose(const TSharedRef<FJsonObject>& J,const TCHAR* Key,const FTransform& T)
    {const auto P=T.GetLocation();const auto Q=T.GetRotation();const auto S=T.GetScale3D();
     J->SetArrayField(Key,{Value(P.X),Value(P.Y),Value(P.Z),Value(Q.X),Value(Q.Y),Value(Q.Z),Value(Q.W),Value(S.X),Value(S.Y),Value(S.Z)});}
    void Axes(const TSharedRef<FJsonObject>& J,const TCHAR* Key,const FQuat& Q)
    {J->SetArrayField(Key,{Value(Q.X),Value(Q.Y),Value(Q.Z),Value(Q.W)});}
};
void Strings(const TSharedRef<FJsonObject>& J,const TCHAR* Key,const TArray<FString>& Values)
{TArray<TSharedPtr<FJsonValue>> A;for(const auto& V:Values)A.Add(MakeShared<FJsonValueString>(V));J->SetArrayField(Key,A);}
TSharedRef<FJsonObject> BodyObservation(const FDDSOriginalRobotNativeBodyObservation& B,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetBoolField(TEXT("available"),B.bAvailable);J->SetStringField(TEXT("unavailableReason"),B.UnavailableReason);
    J->SetStringField(TEXT("component"),B.ComponentObjectPath);J->SetStringField(TEXT("componentBodySetup"),B.ComponentBodySetupObjectPath);
    J->SetStringField(TEXT("meshBodySetup"),B.MeshBodySetupObjectPath);
    if(B.bAvailable)J->SetStringField(TEXT("componentBodySetupGuid"),B.ComponentBodySetupGuid.ToString());else J->SetField(TEXT("componentBodySetupGuid"),MakeShared<FJsonValueNull>());
    if(B.bAvailable)
    {N.Number(J,TEXT("nativeSimulationShapeCount"),B.NativeSimulationShapeCount);N.Number(J,TEXT("nativeMassKg"),B.NativeMassKg);
     N.Vector(J,TEXT("nativePrincipalInertiaKgM2"),B.NativePrincipalInertiaKgM2);N.Pose(J,TEXT("nativeMassSpaceBodyLocalMeters"),B.NativeMassSpaceBodyLocalMeters);
     N.Pose(J,TEXT("nativeActorWorldCm"),B.NativeActorWorldCm);N.Vector(J,TEXT("nativeVelocityCmps"),B.NativeVelocityCmps);N.Vector(J,TEXT("nativeOmegaRadps"),B.NativeOmegaRadps);}
    else for(const TCHAR* Key:{TEXT("nativeSimulationShapeCount"),TEXT("nativeMassKg"),TEXT("nativePrincipalInertiaKgM2"),TEXT("nativeMassSpaceBodyLocalMeters"),TEXT("nativeActorWorldCm"),TEXT("nativeVelocityCmps"),TEXT("nativeOmegaRadps")})
        J->SetField(Key,MakeShared<FJsonValueNull>());
    if(B.DeclaredGameMassKg>0)N.Number(J,TEXT("declaredGameMassKg"),B.DeclaredGameMassKg);else J->SetField(TEXT("declaredGameMassKg"),MakeShared<FJsonValueNull>());
    if(B.bAvailable && B.DeclaredGameMassKg>0)
    {N.Number(J,TEXT("gameToNativeMassRatio"),B.GameToNativeMassRatio);N.Vector(J,TEXT("declaredGamePrincipalInertiaKgM2"),B.DeclaredGamePrincipalInertiaKgM2);}
    else {J->SetField(TEXT("gameToNativeMassRatio"),MakeShared<FJsonValueNull>());J->SetField(TEXT("declaredGamePrincipalInertiaKgM2"),MakeShared<FJsonValueNull>());}
    J->SetStringField(TEXT("model"),TEXT("native engine shape/default/contact model versus separate GAME same-shape density choice; not supplier specifications"));return J;
}
TSharedRef<FJsonObject> SupportObservation(UWorld& World,const FHitResult& Hit,const FVector& Start,const FVector& End,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("trace"),TEXT("actual City LineTraceSingleByObjectType WorldStatic, bTraceComplex=false; support is not streaming/collision readiness"));
    J->SetBoolField(TEXT("blockingHit"),Hit.bBlockingHit);J->SetBoolField(TEXT("startPenetrating"),Hit.bStartPenetrating);
    N.Vector(J,TEXT("startCm"),Start);N.Vector(J,TEXT("endCm"),End);N.Vector(J,TEXT("impactPointCm"),Hit.ImpactPoint);N.Vector(J,TEXT("impactNormal"),Hit.ImpactNormal);
    auto* C=Hit.GetComponent();auto* Actor=Hit.GetActor();auto* MeshC=Cast<UStaticMeshComponent>(C);auto* Setup=C?C->GetBodySetup():nullptr;
    J->SetStringField(TEXT("actor"),GetPathNameSafe(Actor));J->SetStringField(TEXT("component"),GetPathNameSafe(C));J->SetStringField(TEXT("componentClass"),C?C->GetClass()->GetPathName():FString());
    J->SetBoolField(TEXT("registered"),C && C->IsRegistered());J->SetBoolField(TEXT("sameWorld"),C && C->GetWorld()==&World);
    J->SetBoolField(TEXT("physicsStateCreated"),C && C->IsPhysicsStateCreated());J->SetBoolField(TEXT("staticMeshComponent"),MeshC!=nullptr);
    if(C){N.Number(J,TEXT("collisionEnabledEnum"),int32(C->GetCollisionEnabled()));N.Number(J,TEXT("collisionObjectTypeEnum"),int32(C->GetCollisionObjectType()));N.Pose(J,TEXT("componentWorldCmQuaternionScale"),C->GetComponentTransform());}
    J->SetStringField(TEXT("mesh"),MeshC?GetPathNameSafe(MeshC->GetStaticMesh().Get()):FString());J->SetStringField(TEXT("bodySetup"),GetPathNameSafe(Setup));
    J->SetBoolField(TEXT("setupIsMeshOwned"),MeshC && MeshC->GetStaticMesh() && Setup==MeshC->GetStaticMesh()->GetBodySetup());
    if(Setup)
    {
        J->SetStringField(TEXT("bodySetupGuid"),Setup->BodySetupGuid.ToString());N.Number(J,TEXT("resolvedCollisionTraceFlagEnum"),int32(Setup->GetCollisionTraceFlag()));
        N.Number(J,TEXT("rawCollisionTraceFlagEnum"),int32(Setup->CollisionTraceFlag));N.Vector(J,TEXT("collisionBuildScale"),Setup->BuildScale3D);
        const auto& A=Setup->AggGeom;auto Shapes=MakeShared<FJsonObject>();
        N.Number(Shapes,TEXT("boxes"),A.BoxElems.Num());N.Number(Shapes,TEXT("spheres"),A.SphereElems.Num());N.Number(Shapes,TEXT("capsules"),A.SphylElems.Num());
        N.Number(Shapes,TEXT("convex"),A.ConvexElems.Num());N.Number(Shapes,TEXT("taperedCapsules"),A.TaperedCapsuleElems.Num());
        N.Number(Shapes,TEXT("levelSets"),A.LevelSetElems.Num());N.Number(Shapes,TEXT("skinnedLevelSets"),A.SkinnedLevelSetElems.Num());N.Number(Shapes,TEXT("totalSimple"),A.GetElementCount());J->SetObjectField(TEXT("nativeSimpleShapeRoster"),Shapes);
    }
    J->SetStringField(TEXT("compatibility"),TEXT("Producer accepts exact registered mesh-owned convex-only support at preserved uniform scale; no alternate floor/model selection"));return J;
}
TSharedRef<FJsonObject> DraftObservation(const FDDSOriginalRobotMeasurementDraft& D,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("scope"),D.Scope);J->SetStringField(TEXT("sourceActor"),GetPathNameSafe(D.SourceActor.Get()));
    J->SetStringField(TEXT("nativeGeometryRecordSha256"),D.SourceGeometry.NativeGeometryRecordSha256);J->SetStringField(TEXT("worldPlacementRecordSha256"),D.SourceGeometry.WorldPlacementRecordSha256);
    auto Admission=MakeShared<FJsonObject>();Admission->SetBoolField(TEXT("nineBodyTopologyVerified"),D.UnadmittedBinding.bNineBodyTopologyVerified);
    Admission->SetBoolField(TEXT("mountsMeasured"),D.UnadmittedBinding.bMountsMeasured);Admission->SetBoolField(TEXT("framesAreScaledPhysicalSI"),D.UnadmittedBinding.bFramesAreScaledPhysicalSI);
    Admission->SetBoolField(TEXT("mechanicsFrozen"),D.UnadmittedBinding.Mechanics.bFrozen);Admission->SetBoolField(TEXT("serviceFrozen"),D.DeclaredGameProfile.Service.bFrozen);
    Admission->SetStringField(TEXT("bindingSha256"),D.UnadmittedBinding.BindingSha256);Admission->SetStringField(TEXT("a02CertificateSha256"),D.UnadmittedBinding.A02MeasurementCertificateSha256);
    Admission->SetBoolField(TEXT("parcelBound"),D.UnadmittedBinding.Parcel.ActualPart!=nullptr);J->SetObjectField(TEXT("unadmittedFlags"),Admission);
    TArray<TSharedPtr<FJsonValue>> Bodies,Suspension,Joints,Mounts,Sources;
    for(int32 I=0;I<9;++I)
    {const auto& B=D.UnadmittedBinding.Bodies[I];auto Row=BodyObservation(D.Bodies[I],N);N.Number(Row,TEXT("index"),I);
     Row->SetStringField(TEXT("partId"),B.SuppliedPartId);Row->SetBoolField(TEXT("measured"),B.bMeasured);Row->SetBoolField(TEXT("simpleCollisionMeasured"),B.bSimpleCollisionMeasured);
     Row->SetBoolField(TEXT("ccdMeasuredRequired"),B.bCCDMeasuredRequired);N.Number(Row,TEXT("admittedMotorForwardSign"),B.MeasuredMotorForwardSign);
     Row->SetStringField(TEXT("measurementRecordSha256"),B.MeasurementRecordSha256);Row->SetStringField(TEXT("simpleCollisionSha256"),B.SimpleCollisionSha256);
     N.Pose(Row,TEXT("observedRestInChassisMeters"),B.RestInChassisMeters);N.Vector(Row,TEXT("preservedSupplierScale"),B.SupplierScale);N.Number(Row,TEXT("componentCollisionSnapshotByteCount"),D.ComponentConvexContent[I].Num());Bodies.Add(MakeShared<FJsonValueObject>(Row));}
    for(int32 I=0;I<6;++I)
    {const auto& S=D.UnadmittedBinding.SuspensionParts[I];auto Row=MakeShared<FJsonObject>();N.Number(Row,TEXT("index"),I);Row->SetStringField(TEXT("partId"),S.SuppliedPartId);
     Row->SetBoolField(TEXT("measured"),S.bMeasured);Row->SetBoolField(TEXT("rigidMotionAdmitted"),S.bRigidRelativeOwnerThroughoutMotion);Row->SetBoolField(TEXT("ownerShapeInclusionAdmitted"),S.bCollisionAlreadyIncludedInOwnerShape);
     Row->SetStringField(TEXT("geometryAndMassMapSha256"),S.GeometryAndMassMapSha256);Suspension.Add(MakeShared<FJsonValueObject>(Row));}
    for(int32 I=0;I<8;++I)
    {const auto& S=D.Joints[I];auto Row=MakeShared<FJsonObject>();N.Number(Row,TEXT("index"),I);Row->SetBoolField(TEXT("actualNativeSourceAvailable"),S.bAvailable);Row->SetStringField(TEXT("source"),S.SourceObjectPath);
     Row->SetBoolField(TEXT("measuredAdmissionFlag"),D.UnadmittedBinding.Joints[I].bMeasured);
     if(S.bAvailable){N.Pose(Row,TEXT("nativeInChassisMeters"),S.NativeInChassisMeters);N.Pose(Row,TEXT("nativeInPartMeters"),S.NativeInPartMeters);N.Number(Row,TEXT("nativeLinearLimitM"),S.NativeLinearLimitM);}
     Joints.Add(MakeShared<FJsonValueObject>(Row));}
    for(int32 I=0;I<4;++I)
    {auto Row=MakeShared<FJsonObject>();N.Number(Row,TEXT("index"),I);Row->SetBoolField(TEXT("actualSourceAvailable"),D.bMountSourceAvailable[I]);Row->SetStringField(TEXT("source"),D.MountSourceObjectPaths[I]);
     if(D.bMountSourceAvailable[I])N.Pose(Row,TEXT("nativeInChassisMeters"),D.MountsInChassisMeters[I]);else Row->SetField(TEXT("nativeInChassisMeters"),MakeShared<FJsonValueNull>());Mounts.Add(MakeShared<FJsonValueObject>(Row));}
    for(const auto& P:D.SourceGeometry.Parts)
    {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("source"),P.ExactVisualSourceObjectPath);Row->SetStringField(TEXT("mesh"),P.ExactMeshObjectPath);Row->SetStringField(TEXT("meshPackageSha256"),P.MeshPackageSha256);
     N.Number(Row,TEXT("role"),int32(P.Role));N.Number(Row,TEXT("index"),P.Index);Row->SetStringField(TEXT("partId"),P.SuppliedPartId);N.Pose(Row,TEXT("actualSourceWorldCm"),P.MeasuredWorldCm);Sources.Add(MakeShared<FJsonValueObject>(Row));}
    J->SetArrayField(TEXT("bodies"),Bodies);J->SetArrayField(TEXT("suspension"),Suspension);J->SetArrayField(TEXT("nativeJointSources"),Joints);J->SetArrayField(TEXT("nativeMountSources"),Mounts);J->SetArrayField(TEXT("observedSource16"),Sources);
    Strings(J,TEXT("missingEvidence"),D.MissingEvidence);return J;
}
TSharedRef<FJsonObject> NativeObservation(const FDDSOriginalRobotNativeCalibrationDraft& D,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("scope"),D.Scope);J->SetBoolField(TEXT("boundedDiagnosticRunCompleted"),D.bBoundedRunCompleted);
    N.Number(J,TEXT("constructedBodyCount"),D.ConstructedBodyCount);N.Number(J,TEXT("constructedJointCount"),D.ConstructedJointCount);
    const bool HasReadback=D.bBoundedRunCompleted || D.ConstructedBodyCount>0 || D.ObservedWorldGravityCmps2!=0 || D.ObservedSolverAsyncDtS!=0;
    if(HasReadback){N.Number(J,TEXT("observedWorldGravityCmps2"),D.ObservedWorldGravityCmps2);N.Number(J,TEXT("observedSolverAsyncDtS"),D.ObservedSolverAsyncDtS);}
    else {J->SetField(TEXT("observedWorldGravityCmps2"),MakeShared<FJsonValueNull>());J->SetField(TEXT("observedSolverAsyncDtS"),MakeShared<FJsonValueNull>());}
    J->SetStringField(TEXT("readbackAvailability"),TEXT("Producer has no independent availability flag; zero defaults stay null unless completion, a body or a nonzero readback establishes observation"));
    TArray<TSharedPtr<FJsonValue>> Reference,Declared,Signs,Suspension,Steps;
    for(int32 I=0;I<9;++I){Reference.Add(MakeShared<FJsonValueObject>(BodyObservation(D.NativeReferenceBodies[I],N)));Declared.Add(MakeShared<FJsonValueObject>(BodyObservation(D.DeclaredMassEngineBodies[I],N)));}
    for(int32 I=0;I<6;++I)
    {Signs.Add(N.Value(D.CandidateMotorForwardSign[I]));auto Row=MakeShared<FJsonObject>();N.Number(Row,TEXT("index"),I);
     N.Number(Row,TEXT("rawProducerMaximumRelativePositionErrorM"),D.ObservedMaximumSuspensionRelativePositionErrorM[I]);N.Number(Row,TEXT("rawProducerMaximumRelativeAngleErrorRad"),D.ObservedMaximumSuspensionRelativeAngleErrorRad[I]);
     Row->SetBoolField(TEXT("completeDiagnosticCoverage"),D.bBoundedRunCompleted);Row->SetStringField(TEXT("availability"),TEXT("Raw accumulator: partial attempt may retain constructor zero; no rigidity or error budget admission"));Suspension.Add(MakeShared<FJsonValueObject>(Row));}
    for(const auto& S:D.Steps)
    {
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("sequence"),LexToString(S.Sequence));N.Number(Row,TEXT("callbackStartS"),S.CallbackStartS);
        N.Number(Row,TEXT("callbackDtS"),S.CallbackDtS);N.Number(Row,TEXT("previousCallbackDtS"),S.PreviousCallbackDtS);N.Number(Row,TEXT("configuredSolverAsyncDtS"),S.ConfiguredSolverAsyncDtS);
        Row->SetBoolField(TEXT("previousCompletedStateDiagnostic"),S.bPreviousCompletedState);N.Number(Row,TEXT("nativeDynamicBodyCount"),S.NativeDynamicBodyCount);N.Number(Row,TEXT("nativeMatchingJointCount"),S.NativeMatchingJointCount);
        N.Number(Row,TEXT("appliedPulseWheel"),S.AppliedPulseWheel);N.Number(Row,TEXT("previousPulseWheel"),S.PreviousPulseWheel);N.Vector(Row,TEXT("actualGravityCmps2"),S.ActualGravityCmps2);
        TArray<TSharedPtr<FJsonValue>> Bodies,Wheels,Lids;
        for(const auto& B:S.Bodies)
        {auto Body=MakeShared<FJsonObject>();N.Number(Body,TEXT("particleId"),B.ParticleId);Body->SetBoolField(TEXT("live"),B.bLive);Body->SetBoolField(TEXT("sleeping"),B.bSleeping);
         N.Pose(Body,TEXT("worldCm"),B.WorldCm);N.Vector(Body,TEXT("velocityCmps"),B.VelocityCmps);N.Vector(Body,TEXT("angularVelocityRadps"),B.AngularVelocityRadps);
         Body->SetField(TEXT("accelerationCmps2"),MakeShared<FJsonValueNull>()); // Producer does not observe this field; default zero is not a measurement.
         N.Number(Body,TEXT("massKg"),B.MassKg);N.Vector(Body,TEXT("comLocalCm"),B.COMLocalCm);N.Vector(Body,TEXT("principalInertiaKgCm2"),B.InertiaKgCm2);N.Axes(Body,TEXT("principalAxesInBody"),B.PrincipalAxesInBody);Bodies.Add(MakeShared<FJsonValueObject>(Body));}
        for(const auto& W:S.Wheels)
        {auto Wheel=MakeShared<FJsonObject>();Wheel->SetBoolField(TEXT("solvedContactObserved"),W.bSolvedContactObserved);N.Number(Wheel,TEXT("relativeAxleOmegaRadps"),W.RelativeAxleOmegaRadps);
         if(W.bSolvedContactObserved)
         {N.Number(Wheel,TEXT("supportParticleId"),W.SupportParticleId);N.Vector(Wheel,TEXT("contactPointCm"),W.ContactPointCm);N.Vector(Wheel,TEXT("contactNormal"),W.ContactNormal);
          N.Number(Wheel,TEXT("rawDetectionPhiCm"),W.RawDetectionPhiCm);N.Number(Wheel,TEXT("currentSeparationCm"),W.CurrentSeparationCm);N.Number(Wheel,TEXT("rawNormalLoadImpulseKgCmps"),W.RawNormalLoadImpulseKgCmps);
          N.Number(Wheel,TEXT("angularContactRollingForwardCmps"),W.AngularContactRollingForwardCmps);N.Number(Wheel,TEXT("wheelCOMForwardRelativeToSupportCmps"),W.WheelCOMForwardRelativeToSupportCmps);}
         else for(const TCHAR* Key:{TEXT("supportParticleId"),TEXT("contactPointCm"),TEXT("contactNormal"),TEXT("rawDetectionPhiCm"),TEXT("currentSeparationCm"),TEXT("rawNormalLoadImpulseKgCmps"),TEXT("angularContactRollingForwardCmps"),TEXT("wheelCOMForwardRelativeToSupportCmps")})
             Wheel->SetField(Key,MakeShared<FJsonValueNull>());
         Wheels.Add(MakeShared<FJsonValueObject>(Wheel));}
        for(int32 L=0;L<2;++L){auto Lid=MakeShared<FJsonObject>();N.Number(Lid,TEXT("angleRad"),S.ClosedLidAngleRad[L]);N.Number(Lid,TEXT("omegaRadps"),S.ClosedLidOmegaRadps[L]);Lids.Add(MakeShared<FJsonValueObject>(Lid));}
        Row->SetArrayField(TEXT("bodies"),Bodies);Row->SetArrayField(TEXT("wheels"),Wheels);Row->SetArrayField(TEXT("closedLids"),Lids);Steps.Add(MakeShared<FJsonValueObject>(Row));
    }
    J->SetArrayField(TEXT("nativeReferenceBodies"),Reference);J->SetArrayField(TEXT("declaredMassEngineBodies"),Declared);J->SetArrayField(TEXT("candidateMotorForwardSignsOnly"),Signs);
    J->SetArrayField(TEXT("suspensionRelativeDiagnostic"),Suspension);J->SetArrayField(TEXT("steps"),Steps);Strings(J,TEXT("remainingEvidence"),D.RemainingEvidence);
    J->SetStringField(TEXT("dtTruth"),TEXT("previousCallbackDtS is previous callback delta, not independent evolution timestep"));return J;
}
// Caller-only opt-in wiring. Declared GAME choices are separate from observations.
constexpr TCHAR MeasurementChoice[]=TEXT("DriveMeasurement20261002v1");
constexpr TCHAR MeasurementRecord[]=TEXT("{\"id\":\"DriveMeasurement20261002v1\",\"scope\":\"declared GAME engineering acceptance criteria; no certificate\",\"settleSteps\":32,\"pulseStepsPerDirection\":12,\"coastStepsPerDirection\":4,\"pulseTorqueNm\":0.5,\"directionsPerWheel\":2,\"wheelCount\":6,\"drainSteps\":2,\"expectedFrames\":226,\"slipDomainSpeedMps\":0.05,\"sleepingAnchorToleranceM\":0.005,\"sleepingSpeedToleranceMps\":0.02,\"sleepingOmegaToleranceRadps\":0.02,\"minSupportUpDot\":0.8,\"minLoadedImpulseNs\":0.01,\"minAxleResponseRadps\":0.001,\"minRollingForwardMps\":0.001,\"minCOMForwardMps\":0.001,\"minLoadedSamplesPerDirection\":2,\"maxContactSeparationM\":0.01,\"closedLidAngleToleranceRad\":0.01,\"closedLidOmegaToleranceRadps\":0.02}");
bool ConfigureMeasurement(FDDSOriginalRobotCalibrationRecipe& R,bool& Enabled,FString& Digest,FString& Error)
{
    Enabled=false;Digest.Reset();FString Choice;
    if(!FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationMeasurement="),Choice))return true;
    if(Choice!=MeasurementChoice){Error=TEXT("Unknown explicit DDSCalibrationMeasurement choice; no diagnostic recipe guessed");return false;}
    FTCHARToUTF8 U(MeasurementRecord);TArray<uint8> Bytes;Bytes.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length());
    if(!HashBytes(Bytes,Digest)){Error=TEXT("Actual declared measurement GAME recipe digest unavailable");return false;}
    Enabled=true;R.bExplicitBidirectionalTrials=true;R.bExplicitGameAcceptanceLimits=true;
    R.SettleSteps=32;R.PulseStepsPerWheel=12;R.CoastStepsPerWheel=4;
    R.GameSlipDomainSpeedMps=0.05;R.GameSleepingAnchorToleranceM=0.005;R.GameSleepingSpeedToleranceMps=0.02;R.GameSleepingOmegaToleranceRadps=0.02;
    R.GameMinSupportUpDot=0.8;R.GameMinLoadedImpulseNs=0.01;R.GameMinAxleResponseRadps=0.001;R.GameMinRollingForwardMps=0.001;R.GameMinCOMForwardMps=0.001;
    R.GameMinLoadedSamplesPerDirection=2;R.GameMaxContactSeparationM=0.01;R.GameClosedLidAngleToleranceRad=0.01;R.GameClosedLidOmegaToleranceRadps=0.02;
    R.HypothesisDescription+=FString(TEXT("\n"))+MeasurementRecord;return true;
}
bool DeclareGeometryGameMounts(UDDSRobotLeasedMeshComponent& C,FDDSOriginalRobotGameMountDeclarations& Design,
    const TSharedRef<FJsonObject>& Record,FJsonNumbers& N,FString& Error)
{
    auto* Setup=C.GetBodySetup();const FTransform Pose=C.GetComponentTransform();
    if(!Live(&C) || !C.IsRegistered() || C.IsSimulatingPhysics() || !C.PreparedOriginalChassisCompoundUnchanged() || !Live(Setup)
       || !Setup->BuildScale3D.Equals(FVector::OneVector,1.e-9))
    {Error=TEXT("Declared GAME mounts require actual unchanged component-owned original compound and supported unit collision build scale");return false;}
    const FGuid Guid=Setup->BodySetupGuid;const FVector Scale=C.GetComponentScale();
    const auto& A=Setup->AggGeom;
    if(A.ConvexElems.Num()<1 || A.ConvexElems.Num()>128 || A.GetElementCount()!=A.ConvexElems.Num()
       || Scale.ContainsNaN() || Scale.X<=0 || Scale.Y<=0 || Scale.Z<=0)
    {Error=TEXT("Declared GAME mounts require bounded original convex compound with finite preserved positive supplier scale");return false;}
    FBox Physical(ForceInit);int32 Vertices=0;
    for(const auto& Hull:A.ConvexElems)
    {
        if(Hull.VertexData.Num()<4 || Hull.VertexData.Num()>4096 || Hull.GetTransform().ContainsNaN())
        {Error=TEXT("Actual component convex hull is outside mount derivation bounds");return false;}
        for(const FVector& V:Hull.VertexData)
        {
            if(++Vertices>65536 || V.ContainsNaN()){Error=TEXT("Actual hull mount derivation vertex budget/nonfinite refusal");return false;}
            const FVector PhysicalM=Hull.GetTransform().TransformPosition(V)*Scale*0.01;
            if(PhysicalM.ContainsNaN()){Error=TEXT("Scaled original collision vertex is nonfinite");return false;}Physical+=PhysicalM;
        }
    }
    if(!Physical.IsValid || Physical.Min.ContainsNaN() || Physical.Max.ContainsNaN() || Physical.GetSize().GetMin()<=0
       || C.GetBodySetup()!=Setup || Setup->BodySetupGuid!=Guid || !C.GetComponentTransform().Equals(Pose,0) || !C.PreparedOriginalChassisCompoundUnchanged())
    {Error=TEXT("Physical collision hull bounds unavailable or chassis receipt changed while declaring GAME mounts");return false;}
    const FVector Mid=Physical.GetCenter();const FVector Offset[4]={FVector(0.05,0,0.05),FVector(-1.25,0,0.60),FVector(0,0.03,0.02),FVector(0,0,0.03)};
    const FVector Anchor[4]={FVector(Physical.Max.X,Mid.Y,Physical.Max.Z),FVector(Physical.Min.X,Mid.Y,Physical.Max.Z),FVector(Mid.X,Physical.Max.Y,Physical.Max.Z),FVector(Mid.X,Mid.Y,Physical.Max.Z)};
    const TCHAR* Names[]={TEXT("sensor"),TEXT("chase"),TEXT("service"),TEXT("cargo")};
    const TCHAR* Formula[]={TEXT("[maxX,midY,maxZ]+[0.05,0,0.05] m"),TEXT("[minX,midY,maxZ]+[-1.25,0,0.60] m"),TEXT("[midX,maxY,maxZ]+[0,0.03,0.02] m"),TEXT("[midX,midY,maxZ]+[0,0,0.03] m")};
    Design.bExplicitGameDesign=true;Design.Provenance=TEXT("DriveMeasurement20261002v1 intentional GAME sensor/chase/service/cargo mount frames: actual component-owned original compound hull vertices transformed by hull pose and preserved supplier scale exactly once, cm to m; identity rotation in chassis X-forward frame; recorded fixed GAME offsets. No supplier mount assertion, parcel, render bounds or mass derivation.");
    Record->SetStringField(TEXT("provenance"),Design.Provenance);Record->SetStringField(TEXT("sourceComponent"),C.GetPathName());Record->SetStringField(TEXT("componentBodySetup"),Setup->GetPathName());
    Record->SetStringField(TEXT("componentBodySetupGuid"),Guid.ToString());N.Vector(Record,TEXT("collisionBuildScale"),Setup->BuildScale3D);N.Vector(Record,TEXT("preservedSupplierScale"),Scale);
    N.Number(Record,TEXT("convexHullCount"),A.ConvexElems.Num());N.Number(Record,TEXT("vertexCount"),Vertices);N.Vector(Record,TEXT("physicalCollisionMinMeters"),Physical.Min);N.Vector(Record,TEXT("physicalCollisionMaxMeters"),Physical.Max);
    Record->SetStringField(TEXT("vertexFormula"),TEXT("Hull.GetTransform().TransformPosition(VertexData)*ComponentScale*0.01; collision BuildScale3D required unit; no second inherited attachment scale"));
    TArray<TSharedPtr<FJsonValue>> Rows;
    for(int32 I=0;I<4;++I){Design.InChassisMeters[I]=FTransform(FQuat::Identity,Anchor[I]+Offset[I],FVector::OneVector);auto Row=MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("role"),Names[I]);Row->SetStringField(TEXT("authoredFormula"),Formula[I]);N.Vector(Row,TEXT("declaredGameOffsetMeters"),Offset[I]);N.Pose(Row,TEXT("declaredGameInChassisMeters"),Design.InChassisMeters[I]);Rows.Add(MakeShared<FJsonValueObject>(Row));}
    Record->SetArrayField(TEXT("declaredFrames"),Rows);return N.bFinite;
}
void NullFields(const TSharedRef<FJsonObject>& J,std::initializer_list<const TCHAR*> Keys)
{for(const TCHAR* Key:Keys)J->SetField(Key,MakeShared<FJsonValueNull>());}
void Identity(const TSharedRef<FJsonObject>& J,const TCHAR* Key,uint64 Id,bool Available)
{if(Available && Id)J->SetStringField(Key,LexToString(Id));else J->SetField(Key,MakeShared<FJsonValueNull>());}
TSharedRef<FJsonObject> JointIncrement(const FDDSOriginalRobotJointSourceObservation& O,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetBoolField(TEXT("available"),O.bAvailable);
    if(!O.SourceObjectPath.IsEmpty())J->SetStringField(TEXT("SourceObjectPath"),O.SourceObjectPath);else NullFields(J,{TEXT("SourceObjectPath")});
    Identity(J,TEXT("NativeConstraintIdentity"),O.NativeConstraintIdentity,O.bAvailable);
    J->SetStringField(TEXT("identityMeaning"),TEXT("run-local observed address only, no external dereference; created external constraint and per-callback internal constraint are distinct identities"));
    if(O.bAvailable)
    {
        N.Pose(J,TEXT("NativeInChassisMeters"),O.NativeInChassisMeters);N.Pose(J,TEXT("NativeInPartMeters"),O.NativeInPartMeters);N.Number(J,TEXT("NativeLinearLimitM"),O.NativeLinearLimitM);
        J->SetBoolField(TEXT("bNativeAdjacentCollisionDisabled"),O.bNativeAdjacentCollisionDisabled);J->SetBoolField(TEXT("bNativeParticleIdsAvailable"),O.bNativeParticleIdsAvailable);
        if(O.bNativeParticleIdsAvailable){N.Number(J,TEXT("ChassisParticleId"),O.ChassisParticleId);N.Number(J,TEXT("PartParticleId"),O.PartParticleId);}else NullFields(J,{TEXT("ChassisParticleId"),TEXT("PartParticleId")});
        TArray<TSharedPtr<FJsonValue>> LM,AM,PD,VD;
        for(int32 I=0;I<3;++I){LM.Add(N.Value(O.NativeLinearMotion[I]));AM.Add(N.Value(O.NativeAngularMotion[I]));PD.Add(MakeShared<FJsonValueBoolean>(O.NativeLinearPositionDriveEnabled[I]));VD.Add(MakeShared<FJsonValueBoolean>(O.NativeLinearVelocityDriveEnabled[I]));}
        J->SetArrayField(TEXT("NativeLinearMotion"),LM);J->SetArrayField(TEXT("NativeAngularMotion"),AM);J->SetArrayField(TEXT("NativeLinearPositionDriveEnabled"),PD);J->SetArrayField(TEXT("NativeLinearVelocityDriveEnabled"),VD);
        N.Vector(J,TEXT("NativeAngularLimitsRad"),O.NativeAngularLimitsRad);N.Number(J,TEXT("NativeLinearDriveForceMode"),O.NativeLinearDriveForceMode);
        N.Vector(J,TEXT("NativeLinearDriveTargetMeters"),O.NativeLinearDriveTargetMeters);N.Vector(J,TEXT("NativeLinearDriveVelocityTargetMps"),O.NativeLinearDriveVelocityTargetMps);
        N.Vector(J,TEXT("NativeLinearDriveStiffnessNpm"),O.NativeLinearDriveStiffnessNpm);N.Vector(J,TEXT("NativeLinearDriveDampingNspm"),O.NativeLinearDriveDampingNspm);N.Vector(J,TEXT("NativeLinearDriveMaxForceN"),O.NativeLinearDriveMaxForceN);
    }
    else NullFields(J,{TEXT("NativeInChassisMeters"),TEXT("NativeInPartMeters"),TEXT("NativeLinearLimitM"),TEXT("bNativeAdjacentCollisionDisabled"),TEXT("bNativeParticleIdsAvailable"),TEXT("ChassisParticleId"),TEXT("PartParticleId"),TEXT("NativeLinearMotion"),TEXT("NativeAngularMotion"),TEXT("NativeAngularLimitsRad"),TEXT("NativeLinearDriveForceMode"),TEXT("NativeLinearPositionDriveEnabled"),TEXT("NativeLinearVelocityDriveEnabled"),TEXT("NativeLinearDriveTargetMeters"),TEXT("NativeLinearDriveVelocityTargetMps"),TEXT("NativeLinearDriveStiffnessNpm"),TEXT("NativeLinearDriveDampingNspm"),TEXT("NativeLinearDriveMaxForceN")});
    return J;
}
TSharedRef<FJsonObject> HullOrigin(const FDDSOriginalCompoundHullOrigin& O,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("OriginalPartId"),O.OriginalPartId);J->SetStringField(TEXT("OriginalMeshObjectPath"),O.OriginalMeshObjectPath);
    J->SetStringField(TEXT("OriginalMeshPackageSha256"),O.OriginalMeshPackageSha256);J->SetStringField(TEXT("OriginalBodySetupGuid"),O.OriginalBodySetupGuid.ToString());
    N.Number(J,TEXT("SourceHullIndex"),O.SourceHullIndex);N.Pose(J,TEXT("SourceHullToChassisCm"),O.SourceHullToChassisCm);return J;
}
TSharedRef<FJsonObject> WheelIncrement(const FDDSOriginalRobotCalibrationStepSample& S,int32 I,FJsonNumbers& N)
{
    const auto& O=S.Wheels[I];auto J=MakeShared<FJsonObject>();const bool Current=O.bSolvedContactObserved || O.bSleepingAnchorTransportObserved;
    const bool State=S.bPreviousCompletedState;const bool Axle=S.Bodies[0].bLive && S.Bodies[I+1].bLive;
    if(State){J->SetBoolField(TEXT("bSolvedContactObserved"),O.bSolvedContactObserved);J->SetBoolField(TEXT("bSleepingAnchorTransportObserved"),O.bSleepingAnchorTransportObserved);}else NullFields(J,{TEXT("bSolvedContactObserved"),TEXT("bSleepingAnchorTransportObserved")});
    J->SetStringField(TEXT("solveTruth"),TEXT("Transported sleeping anchor uses current points/velocities but raw solve/impulse/time remain the last observed solve; not a fresh solve"));
    if(Axle)N.Number(J,TEXT("RelativeAxleOmegaRadps"),O.RelativeAxleOmegaRadps);else NullFields(J,{TEXT("RelativeAxleOmegaRadps")});
    if(Current)
    {
        N.Number(J,TEXT("WheelParticleId"),O.WheelParticleId);N.Number(J,TEXT("SupportParticleId"),O.SupportParticleId);
        Identity(J,TEXT("WheelShapeId"),O.WheelShapeId,true);Identity(J,TEXT("SupportShapeId"),O.SupportShapeId,true);Identity(J,TEXT("WheelGeometryId"),O.WheelGeometryId,true);Identity(J,TEXT("SupportGeometryId"),O.SupportGeometryId,true);
#define DDS_DIAG_VECTOR(Member) N.Vector(J,TEXT(#Member),O.Member)
#define DDS_DIAG_NUMBER(Member) N.Number(J,TEXT(#Member),O.Member)
        DDS_DIAG_VECTOR(ContactPointCm);DDS_DIAG_VECTOR(ContactNormal);DDS_DIAG_VECTOR(WheelLocalAnchorCm);DDS_DIAG_VECTOR(SupportLocalAnchorCm);DDS_DIAG_VECTOR(NormalInSupport);DDS_DIAG_VECTOR(SupportPointCm);
        DDS_DIAG_VECTOR(RawSolveWheelPointCm);DDS_DIAG_VECTOR(RawSolveOtherPointCm);DDS_DIAG_VECTOR(RawSolveNormal);DDS_DIAG_VECTOR(SolvedImpulseKgCmps);DDS_DIAG_VECTOR(SolvedPushOutKgCm);
        DDS_DIAG_VECTOR(SupportContactVelocityCmps);DDS_DIAG_VECTOR(WheelContactVelocityCmps);
        DDS_DIAG_NUMBER(RawDetectionPhiCm);DDS_DIAG_NUMBER(CurrentSeparationCm);DDS_DIAG_NUMBER(RawNormalLoadImpulseKgCmps);DDS_DIAG_NUMBER(AngularContactRollingForwardCmps);
        DDS_DIAG_NUMBER(WheelCOMForwardRelativeToSupportCmps);DDS_DIAG_NUMBER(GroundFrameForwardSpeedCmps);DDS_DIAG_NUMBER(LastSolvedCallbackTimeS);
        DDS_DIAG_NUMBER(WheelSpeedCmps);DDS_DIAG_NUMBER(WheelOmegaRadps);
#undef DDS_DIAG_VECTOR
#undef DDS_DIAG_NUMBER
        // Producer reads support speed/omega for rigid or static support, never unknown kinematic defaults.
        if(O.bSupportStatic || O.bSupportSleeping){N.Number(J,TEXT("SupportSpeedCmps"),O.SupportSpeedCmps);N.Number(J,TEXT("SupportOmegaRadps"),O.SupportOmegaRadps);}else NullFields(J,{TEXT("SupportSpeedCmps"),TEXT("SupportOmegaRadps")});
        J->SetBoolField(TEXT("bWheelSleeping"),O.bWheelSleeping);J->SetBoolField(TEXT("bSupportSleeping"),O.bSupportSleeping);J->SetBoolField(TEXT("bSupportStatic"),O.bSupportStatic);J->SetBoolField(TEXT("bSlipDefined"),O.bSlipDefined);
        if(O.bSlipDefined)N.Number(J,TEXT("LongitudinalSlip"),O.LongitudinalSlip);else NullFields(J,{TEXT("LongitudinalSlip")});
    }
    else NullFields(J,{TEXT("WheelParticleId"),TEXT("SupportParticleId"),TEXT("WheelShapeId"),TEXT("SupportShapeId"),TEXT("WheelGeometryId"),TEXT("SupportGeometryId"),TEXT("ContactPointCm"),TEXT("ContactNormal"),TEXT("WheelLocalAnchorCm"),TEXT("SupportLocalAnchorCm"),TEXT("NormalInSupport"),TEXT("SupportPointCm"),TEXT("RawSolveWheelPointCm"),TEXT("RawSolveOtherPointCm"),TEXT("RawSolveNormal"),TEXT("SolvedImpulseKgCmps"),TEXT("SolvedPushOutKgCm"),TEXT("SupportContactVelocityCmps"),TEXT("WheelContactVelocityCmps"),TEXT("RawDetectionPhiCm"),TEXT("CurrentSeparationCm"),TEXT("RawNormalLoadImpulseKgCmps"),TEXT("AngularContactRollingForwardCmps"),TEXT("WheelCOMForwardRelativeToSupportCmps"),TEXT("GroundFrameForwardSpeedCmps"),TEXT("LastSolvedCallbackTimeS"),TEXT("WheelSpeedCmps"),TEXT("WheelOmegaRadps"),TEXT("SupportSpeedCmps"),TEXT("SupportOmegaRadps"),TEXT("bWheelSleeping"),TEXT("bSupportSleeping"),TEXT("bSupportStatic"),TEXT("bSlipDefined"),TEXT("LongitudinalSlip")});
    if(O.bSleepingAnchorTransportObserved){N.Number(J,TEXT("SleepingAnchorDistanceCm"),O.SleepingAnchorDistanceCm);J->SetBoolField(TEXT("bSleepingWitnessWithinDeclaredGameLimits"),O.bSleepingWitnessWithinDeclaredGameLimits);}else NullFields(J,{TEXT("SleepingAnchorDistanceCm"),TEXT("bSleepingWitnessWithinDeclaredGameLimits")});
    if(O.bSolvedContactObserved && State)J->SetBoolField(TEXT("bContactWithinDeclaredGameLimits"),O.bContactWithinDeclaredGameLimits);else NullFields(J,{TEXT("bContactWithinDeclaredGameLimits")});
    if(State && S.PreviousTrialWheel==I)J->SetBoolField(TEXT("bConsecutiveLoadedResponseAvailable"),O.bConsecutiveLoadedResponseAvailable);else NullFields(J,{TEXT("bConsecutiveLoadedResponseAvailable")});
    if(O.bConsecutiveLoadedResponseAvailable){N.Number(J,TEXT("AxleOmegaDeltaRadps"),O.AxleOmegaDeltaRadps);N.Number(J,TEXT("COMForwardVelocityDeltaCmps"),O.COMForwardVelocityDeltaCmps);}else NullFields(J,{TEXT("AxleOmegaDeltaRadps"),TEXT("COMForwardVelocityDeltaCmps")});return J;
}
TSharedRef<FJsonObject> StepIncrement(const FDDSOriginalRobotCalibrationStepSample& S,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("Sequence"),LexToString(S.Sequence));J->SetBoolField(TEXT("bPreviousCompletedState"),S.bPreviousCompletedState);
    N.Number(J,TEXT("CallbackStartS"),S.CallbackStartS);N.Number(J,TEXT("CallbackDtS"),S.CallbackDtS);N.Number(J,TEXT("ConfiguredSolverAsyncDtS"),S.ConfiguredSolverAsyncDtS);
    N.Number(J,TEXT("NativeDynamicBodyCount"),S.NativeDynamicBodyCount);N.Number(J,TEXT("NativeMatchingJointCount"),S.NativeMatchingJointCount);
    N.Number(J,TEXT("AppliedTrialWheel"),S.AppliedTrialWheel);N.Number(J,TEXT("AppliedTrialPhase"),int32(S.AppliedTrialPhase));N.Number(J,TEXT("AppliedPulseWheel"),S.AppliedPulseWheel);N.Number(J,TEXT("AppliedPulseNm"),S.AppliedPulseNm);
    if(S.bPreviousCompletedState){N.Number(J,TEXT("PreviousCallbackDtS"),S.PreviousCallbackDtS);N.Number(J,TEXT("PreviousTrialWheel"),S.PreviousTrialWheel);N.Number(J,TEXT("PreviousTrialPhase"),int32(S.PreviousTrialPhase));N.Number(J,TEXT("PreviousPulseWheel"),S.PreviousPulseWheel);N.Number(J,TEXT("PreviousPulseNm"),S.PreviousPulseNm);}else NullFields(J,{TEXT("PreviousCallbackDtS"),TEXT("PreviousTrialWheel"),TEXT("PreviousTrialPhase"),TEXT("PreviousPulseWheel"),TEXT("PreviousPulseNm")});
    if(S.Bodies[0].bLive)N.Vector(J,TEXT("ActualGravityCmps2"),S.ActualGravityCmps2);else NullFields(J,{TEXT("ActualGravityCmps2")});
    TArray<TSharedPtr<FJsonValue>> Bodies,Wheel,Joint,Lid;
    for(int32 I=0;I<9;++I){const auto& O=S.Bodies[I];auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("bLive"),O.bLive);
        if(O.bLive){N.Number(B,TEXT("ParticleId"),O.ParticleId);B->SetBoolField(TEXT("bSleeping"),O.bSleeping);B->SetBoolField(TEXT("NativeCCDEnabled"),S.NativeBodyCCDEnabled[I]);N.Pose(B,TEXT("WorldCm"),O.WorldCm);N.Vector(B,TEXT("VelocityCmps"),O.VelocityCmps);N.Vector(B,TEXT("AngularVelocityRadps"),O.AngularVelocityRadps);N.Number(B,TEXT("MassKg"),O.MassKg);N.Vector(B,TEXT("COMLocalCm"),O.COMLocalCm);N.Vector(B,TEXT("InertiaKgCm2"),O.InertiaKgCm2);N.Axes(B,TEXT("PrincipalAxesInBody"),O.PrincipalAxesInBody);}
        else NullFields(B,{TEXT("ParticleId"),TEXT("bSleeping"),TEXT("NativeCCDEnabled"),TEXT("WorldCm"),TEXT("VelocityCmps"),TEXT("AngularVelocityRadps"),TEXT("MassKg"),TEXT("COMLocalCm"),TEXT("InertiaKgCm2"),TEXT("PrincipalAxesInBody")});NullFields(B,{TEXT("AccelerationCmps2")});Bodies.Add(MakeShared<FJsonValueObject>(B));}
    for(int32 I=0;I<6;++I)Wheel.Add(MakeShared<FJsonValueObject>(WheelIncrement(S,I,N)));
    for(const auto& O:S.NativeJoints)Joint.Add(MakeShared<FJsonValueObject>(JointIncrement(O,N)));
    for(int32 I=0;I<2;++I){auto L=MakeShared<FJsonObject>();const bool Available=S.Bodies[0].bLive && S.Bodies[I+7].bLive;
        L->SetBoolField(TEXT("nativeBodyStateAvailable"),Available);if(Available){N.Number(L,TEXT("ClosedLidAngleRad"),S.ClosedLidAngleRad[I]);N.Number(L,TEXT("ClosedLidOmegaRadps"),S.ClosedLidOmegaRadps[I]);N.Number(L,TEXT("ClosedLidFullAngularErrorRad"),S.ClosedLidFullAngularErrorRad[I]);N.Number(L,TEXT("ClosedLidRelativeOmegaMagnitudeRadps"),S.ClosedLidRelativeOmegaMagnitudeRadps[I]);}else NullFields(L,{TEXT("ClosedLidAngleRad"),TEXT("ClosedLidOmegaRadps"),TEXT("ClosedLidFullAngularErrorRad"),TEXT("ClosedLidRelativeOmegaMagnitudeRadps")});Lid.Add(MakeShared<FJsonValueObject>(L));}
    J->SetArrayField(TEXT("Bodies"),Bodies);J->SetArrayField(TEXT("Wheels"),Wheel);J->SetArrayField(TEXT("NativeJoints"),Joint);J->SetArrayField(TEXT("ClosedLids"),Lid);return J;
}
TSharedRef<FJsonObject> SuppliedRecipeObservation(const FDDSOriginalRobotCalibrationRecipe& R,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("meaning"),TEXT("Retained authored GAME recipe inputs, not native physical constants or measured admission"));
    J->SetBoolField(TEXT("bExplicitGameFixtureHypotheses"),R.bExplicitGameFixtureHypotheses);J->SetStringField(TEXT("HypothesisDescription"),R.HypothesisDescription);
    J->SetBoolField(TEXT("bExplicitBidirectionalTrials"),R.bExplicitBidirectionalTrials);J->SetBoolField(TEXT("bExplicitGameAcceptanceLimits"),R.bExplicitGameAcceptanceLimits);
    auto* Support=R.OriginalSupportSource.Get();if(Live(Support))J->SetStringField(TEXT("OriginalSupportSource"),Support->GetPathName());else NullFields(J,{TEXT("OriginalSupportSource")});
    N.Pose(J,TEXT("FixtureChassisWorldCm"),R.FixtureChassisWorldCm);N.Pose(J,TEXT("FixtureSupportWorldCm"),R.FixtureSupportWorldCm);N.Vector(J,TEXT("DeclaredForwardInChassis"),R.DeclaredForwardInChassis);
#define DDS_DIAG_RECIPE(Member) N.Number(J,TEXT(#Member),R.Member)
    DDS_DIAG_RECIPE(PulseTorqueNm);DDS_DIAG_RECIPE(SettleSteps);DDS_DIAG_RECIPE(PulseStepsPerWheel);DDS_DIAG_RECIPE(CoastStepsPerWheel);
    DDS_DIAG_RECIPE(GameSlipDomainSpeedMps);DDS_DIAG_RECIPE(GameSleepingAnchorToleranceM);DDS_DIAG_RECIPE(GameSleepingSpeedToleranceMps);DDS_DIAG_RECIPE(GameSleepingOmegaToleranceRadps);
    DDS_DIAG_RECIPE(GameMinSupportUpDot);DDS_DIAG_RECIPE(GameMinLoadedImpulseNs);DDS_DIAG_RECIPE(GameMinAxleResponseRadps);DDS_DIAG_RECIPE(GameMinRollingForwardMps);DDS_DIAG_RECIPE(GameMinCOMForwardMps);
    DDS_DIAG_RECIPE(GameMinLoadedSamplesPerDirection);DDS_DIAG_RECIPE(GameMaxContactSeparationM);DDS_DIAG_RECIPE(GameClosedLidAngleToleranceRad);DDS_DIAG_RECIPE(GameClosedLidOmegaToleranceRadps);
#undef DDS_DIAG_RECIPE
    TArray<TSharedPtr<FJsonValue>> Shares,Joints;for(double Share:R.DeclaredSuspensionMassAllocationKg)Shares.Add(N.Value(Share));J->SetArrayField(TEXT("DeclaredSuspensionMassAllocationKg"),Shares);
    for(const auto& H:R.JointHypotheses){auto O=MakeShared<FJsonObject>();N.Pose(O,TEXT("InChassisMeters"),H.InChassisMeters);N.Pose(O,TEXT("InPartMeters"),H.InPartMeters);N.Number(O,TEXT("SuspensionHalfTravelM"),H.SuspensionHalfTravelM);N.Number(O,TEXT("SuspensionRestTargetM"),H.SuspensionRestTargetM);N.Number(O,TEXT("LidOpenLimitRad"),H.LidOpenLimitRad);O->SetBoolField(TEXT("bAdjacentCollisionExclusionJustified"),H.bAdjacentCollisionExclusionJustified);O->SetBoolField(TEXT("bMeasured"),H.bMeasured);Joints.Add(MakeShared<FJsonValueObject>(O));}
    J->SetArrayField(TEXT("JointHypotheses"),Joints);return J;
}
TSharedRef<FJsonObject> MeasurementSummary(const FDDSOriginalRobotNativeCalibrationDraft& D,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("scope"),TEXT("actual retained measurement increment draft; no admission/binding/certificate; empty datasets are empty, unavailable native values are null"));
    J->SetStringField(TEXT("producerScope"),D.Scope);J->SetBoolField(TEXT("bBoundedRunCompleted"),D.bBoundedRunCompleted);
    N.Number(J,TEXT("ConstructedBodyCount"),D.ConstructedBodyCount);N.Number(J,TEXT("ConstructedJointCount"),D.ConstructedJointCount);
    const bool HasReadback=D.bBoundedRunCompleted || D.ConstructedBodyCount>0 || D.ObservedWorldGravityCmps2!=0 || D.ObservedSolverAsyncDtS!=0;
    if(HasReadback){N.Number(J,TEXT("ObservedWorldGravityCmps2"),D.ObservedWorldGravityCmps2);N.Number(J,TEXT("ObservedSolverAsyncDtS"),D.ObservedSolverAsyncDtS);}else NullFields(J,{TEXT("ObservedWorldGravityCmps2"),TEXT("ObservedSolverAsyncDtS")});
    J->SetStringField(TEXT("readbackAvailability"),TEXT("Existing producer has no independent scene-readback availability flag; zero constructor defaults stay null unless nonzero readback/body/completion establishes observation"));
    Strings(J,TEXT("RemainingEvidence"),D.RemainingEvidence);
    if(!D.FixtureWorldObjectPath.IsEmpty())J->SetStringField(TEXT("FixtureWorldObjectPath"),D.FixtureWorldObjectPath);else NullFields(J,{TEXT("FixtureWorldObjectPath")});
    if(!D.SourceActorObjectPath.IsEmpty())J->SetStringField(TEXT("SourceActorObjectPath"),D.SourceActorObjectPath);else NullFields(J,{TEXT("SourceActorObjectPath")});
    Identity(J,TEXT("FixtureSceneIdentity"),D.FixtureSceneIdentity,true);Identity(J,TEXT("FixtureSolverIdentity"),D.FixtureSolverIdentity,true);
    J->SetBoolField(TEXT("bSuppliedDeclaredRecipeAvailable"),D.bSuppliedDeclaredRecipeAvailable);J->SetStringField(TEXT("trialPhaseEnum"),TEXT("0 Settle,1 PositivePulse,2 CoastAfterPositive,3 NegativePulse,4 CoastAfterNegative,5 Drain"));
    if(D.bSuppliedDeclaredRecipeAvailable)J->SetObjectField(TEXT("SuppliedDeclaredRecipe"),SuppliedRecipeObservation(D.SuppliedDeclaredRecipe,N));else NullFields(J,{TEXT("SuppliedDeclaredRecipe")});
    if(D.FixtureSceneIdentity)J->SetBoolField(TEXT("bBidirectionalTrialsRequested"),D.bBidirectionalTrialsRequested);else NullFields(J,{TEXT("bBidirectionalTrialsRequested")});
    N.Number(J,TEXT("retainedStepCount"),D.Steps.Num());N.Number(J,TEXT("retainedPostEndFrameCount"),D.PostEndFrameSuspensionMotion.Num());N.Number(J,TEXT("expectedDeclaredFrameCount"),226);
    const bool Evaluated=D.CompletedGameLimitSamples>0;J->SetBoolField(TEXT("actualDeclaredLimitSamplesAvailable"),Evaluated);
    if(Evaluated){N.Number(J,TEXT("CompletedGameLimitSamples"),D.CompletedGameLimitSamples);N.Number(J,TEXT("LoadedContactSamples"),D.LoadedContactSamples);N.Number(J,TEXT("ContactOutsideDeclaredGameLimits"),D.ContactOutsideDeclaredGameLimits);N.Number(J,TEXT("SleepingAnchorSamples"),D.SleepingAnchorSamples);N.Number(J,TEXT("SleepingAnchorOutsideDeclaredGameLimits"),D.SleepingAnchorOutsideDeclaredGameLimits);N.Number(J,TEXT("DefinedSlipSamples"),D.DefinedSlipSamples);N.Number(J,TEXT("ClosedLidOutsideDeclaredGameLimits"),D.ClosedLidOutsideDeclaredGameLimits);}else NullFields(J,{TEXT("CompletedGameLimitSamples"),TEXT("LoadedContactSamples"),TEXT("ContactOutsideDeclaredGameLimits"),TEXT("SleepingAnchorSamples"),TEXT("SleepingAnchorOutsideDeclaredGameLimits"),TEXT("DefinedSlipSamples"),TEXT("ClosedLidOutsideDeclaredGameLimits")});
    TArray<TSharedPtr<FJsonValue>> Reference,Declared,Joint,Hulls,Constituents,Trials,Motion;
    for(int32 I=0;I<9;++I){auto R=BodyObservation(D.NativeReferenceBodies[I],N),M=BodyObservation(D.DeclaredMassEngineBodies[I],N);
        if(D.NativeReferenceBodies[I].bAvailable)R->SetStringField(TEXT("NativeComponentCollisionProfile"),D.NativeReferenceBodies[I].NativeComponentCollisionProfile.ToString());else NullFields(R,{TEXT("NativeComponentCollisionProfile")});
        if(D.DeclaredMassEngineBodies[I].bAvailable)M->SetStringField(TEXT("NativeComponentCollisionProfile"),D.DeclaredMassEngineBodies[I].NativeComponentCollisionProfile.ToString());else NullFields(M,{TEXT("NativeComponentCollisionProfile")});Reference.Add(MakeShared<FJsonValueObject>(R));Declared.Add(MakeShared<FJsonValueObject>(M));}
    for(const auto& O:D.NativeCreatedJoints)Joint.Add(MakeShared<FJsonValueObject>(JointIncrement(O,N)));
    for(const auto& O:D.ChassisCompoundHullOrigins)Hulls.Add(MakeShared<FJsonValueObject>(HullOrigin(O,N)));
    for(int32 I=0;I<6;++I){const auto& O=D.SuspensionConstituents[I];auto C=MakeShared<FJsonObject>();const bool Available=!O.FixtureComponentPath.IsEmpty() && O.ObservedOwnerBodyIndex!=INDEX_NONE;C->SetBoolField(TEXT("nativeConstituentAvailable"),Available);
        if(Available){C->SetStringField(TEXT("OriginalPartId"),O.OriginalPartId);C->SetStringField(TEXT("OriginalMeshObjectPath"),O.OriginalMeshObjectPath);C->SetStringField(TEXT("OriginalMeshPackageSha256"),O.OriginalMeshPackageSha256);C->SetStringField(TEXT("FixtureComponentPath"),O.FixtureComponentPath);C->SetStringField(TEXT("OriginalBodySetupGuid"),O.OriginalBodySetupGuid.ToString());N.Number(C,TEXT("ObservedOwnerBodyIndex"),O.ObservedOwnerBodyIndex);C->SetBoolField(TEXT("bObservedNoIndependentSimulation"),O.bObservedNoIndependentSimulation);C->SetBoolField(TEXT("bObservedNoIndependentCollision"),O.bObservedNoIndependentCollision);N.Number(C,TEXT("DeclaredIncludedOwnerMassShareKg"),O.DeclaredIncludedOwnerMassShareKg);N.Number(C,TEXT("ObservedOwnerNativeMassKg"),O.ObservedOwnerNativeMassKg);N.Pose(C,TEXT("ObservedRestInOwnerMeters"),O.ObservedRestInOwnerMeters);}
        else NullFields(C,{TEXT("OriginalPartId"),TEXT("OriginalMeshObjectPath"),TEXT("OriginalMeshPackageSha256"),TEXT("FixtureComponentPath"),TEXT("OriginalBodySetupGuid"),TEXT("ObservedOwnerBodyIndex"),TEXT("bObservedNoIndependentSimulation"),TEXT("bObservedNoIndependentCollision"),TEXT("DeclaredIncludedOwnerMassShareKg"),TEXT("ObservedOwnerNativeMassKg"),TEXT("ObservedRestInOwnerMeters")});
        TArray<TSharedPtr<FJsonValue>> Included;for(const auto& H:O.IncludedCompoundHullOrigins)Included.Add(MakeShared<FJsonValueObject>(HullOrigin(H,N)));C->SetArrayField(TEXT("IncludedCompoundHullOrigins"),Included);Constituents.Add(MakeShared<FJsonValueObject>(C));
        const auto& T=D.BidirectionalWheels[I];auto Trial=MakeShared<FJsonObject>();const bool Observed=D.bBidirectionalTrialsRequested && D.Steps.ContainsByPredicate([I](const FDDSOriginalRobotCalibrationStepSample& S){return S.bPreviousCompletedState && S.PreviousTrialWheel==I;});Trial->SetBoolField(TEXT("actualTrialSamplesAvailable"),Observed);
        if(!D.PostEndFrameSuspensionMotion.IsEmpty()){N.Number(C,TEXT("ObservedMaximumSuspensionRelativePositionErrorM"),D.ObservedMaximumSuspensionRelativePositionErrorM[I]);N.Number(C,TEXT("ObservedMaximumSuspensionRelativeAngleErrorRad"),D.ObservedMaximumSuspensionRelativeAngleErrorRad[I]);}else NullFields(C,{TEXT("ObservedMaximumSuspensionRelativePositionErrorM"),TEXT("ObservedMaximumSuspensionRelativeAngleErrorRad")});
        if(Observed){N.Number(Trial,TEXT("PositiveLoadedResponseSamples"),T.PositiveLoadedResponseSamples);N.Number(Trial,TEXT("NegativeLoadedResponseSamples"),T.NegativeLoadedResponseSamples);N.Number(Trial,TEXT("CoastSamples"),T.CoastSamples);N.Number(Trial,TEXT("PositiveCoastSamples"),T.PositiveCoastSamples);N.Number(Trial,TEXT("NegativeCoastSamples"),T.NegativeCoastSamples);Trial->SetBoolField(TEXT("bContradictoryResponseObserved"),T.bContradictoryResponseObserved);
            if(D.bBoundedRunCompleted)Trial->SetBoolField(TEXT("bBothLoadedDirectionsObserved"),T.bBothLoadedDirectionsObserved);else NullFields(Trial,{TEXT("bBothLoadedDirectionsObserved")});
            if(T.CoherentForwardSignCandidate!=0)N.Number(Trial,TEXT("CoherentForwardSignCandidateOnly"),T.CoherentForwardSignCandidate);else NullFields(Trial,{TEXT("CoherentForwardSignCandidateOnly")});
            if(D.CandidateMotorForwardSign[I]!=0)N.Number(Trial,TEXT("CandidateMotorForwardSignOnly"),D.CandidateMotorForwardSign[I]);else NullFields(Trial,{TEXT("CandidateMotorForwardSignOnly")});}
        else NullFields(Trial,{TEXT("PositiveLoadedResponseSamples"),TEXT("NegativeLoadedResponseSamples"),TEXT("CoastSamples"),TEXT("PositiveCoastSamples"),TEXT("NegativeCoastSamples"),TEXT("bContradictoryResponseObserved"),TEXT("bBothLoadedDirectionsObserved"),TEXT("CoherentForwardSignCandidateOnly"),TEXT("CandidateMotorForwardSignOnly")});Trials.Add(MakeShared<FJsonValueObject>(Trial));}
    for(const auto& O:D.PostEndFrameSuspensionMotion){auto M=MakeShared<FJsonObject>();N.Number(M,TEXT("NativeFrameIndex"),O.NativeFrameIndex);M->SetStringField(TEXT("ObservedCallbackSequenceThrough"),LexToString(O.ObservedCallbackSequenceThrough));N.Number(M,TEXT("ChassisParticleId"),O.ChassisParticleId);N.Pose(M,TEXT("NativeChassisWorldCm"),O.NativeChassisWorldCm);N.Pose(M,TEXT("ComponentChassisWorldCm"),O.ComponentChassisWorldCm);TArray<TSharedPtr<FJsonValue>> Parts;
        for(int32 I=0;I<6;++I){auto P=MakeShared<FJsonObject>();N.Pose(P,TEXT("ComponentWorldCm"),O.ComponentWorldCm[I]);N.Pose(P,TEXT("RelativeToOwnerMeters"),O.RelativeToOwnerMeters[I]);Parts.Add(MakeShared<FJsonValueObject>(P));}M->SetArrayField(TEXT("suspension"),Parts);Motion.Add(MakeShared<FJsonValueObject>(M));}
    J->SetArrayField(TEXT("NativeReferenceBodies"),Reference);J->SetArrayField(TEXT("DeclaredMassEngineBodies"),Declared);J->SetArrayField(TEXT("NativeCreatedJoints"),Joint);J->SetArrayField(TEXT("ChassisCompoundHullOrigins"),Hulls);J->SetArrayField(TEXT("SuspensionConstituents"),Constituents);J->SetArrayField(TEXT("BidirectionalWheels"),Trials);J->SetArrayField(TEXT("PostEndFrameSuspensionMotion"),Motion);return J;
}
// Explicit support adapter selection; default remains legacy convex/uniform.
constexpr TCHAR SupportChoice[]=TEXT("OriginalSingleBox20261002v1");
bool ConfigureSupport(FDDSOriginalRobotCalibrationRecipe& R,bool& Enabled,FString& Error)
{
    Enabled=false;FString Choice;
    if(!FParse::Value(FCommandLine::Get(),TEXT("DDSCalibrationSupport="),Choice))return true;
    if(Choice!=SupportChoice){Error=TEXT("Unknown explicit DDSCalibrationSupport choice; no support adapter guessed");return false;}
    Enabled=true;R.bExplicitOriginalSingleBoxSupportAdapter=true;return true;
}
TSharedRef<FJsonObject> SupportBodyReceipt(const FDDSOriginalRobotSupportNativeBodyObservation& O,FJsonNumbers& N)
{
    auto J=MakeShared<FJsonObject>();J->SetBoolField(TEXT("available"),O.bAvailable);
    if(O.bAvailable)
    {J->SetBoolField(TEXT("static"),O.bStatic);Identity(J,TEXT("sceneIdentity"),O.SceneIdentity,true);Identity(J,TEXT("solverIdentity"),O.SolverIdentity,true);
     Identity(J,TEXT("actorIdentity"),O.ActorIdentity,true);N.Number(J,TEXT("particleId"),O.ParticleId);N.Number(J,TEXT("ownerObjectId"),O.OwnerObjectId);N.Number(J,TEXT("componentObjectId"),O.ComponentObjectId);N.Pose(J,TEXT("nativeActorWorldCm"),O.NativeActorWorldCm);}
    else NullFields(J,{TEXT("static"),TEXT("sceneIdentity"),TEXT("solverIdentity"),TEXT("actorIdentity"),TEXT("particleId"),TEXT("ownerObjectId"),TEXT("componentObjectId"),TEXT("nativeActorWorldCm")});
    TArray<TSharedPtr<FJsonValue>> Shapes;
    for(const auto& S:O.Shapes)
    {
        auto R=MakeShared<FJsonObject>();N.Number(R,TEXT("shapeIndex"),S.ShapeIndex);N.Number(R,TEXT("geometryType"),S.GeometryType);N.Number(R,TEXT("collisionShapeType"),S.CollisionShapeType);
        Identity(R,TEXT("shapeIdentity"),S.ShapeIdentity,S.ShapeIdentity!=0);Identity(R,TEXT("geometryIdentity"),S.GeometryIdentity,S.GeometryIdentity!=0);Identity(R,TEXT("assetQueryGeometryIdentity"),S.AssetQueryGeometryIdentity,S.AssetQueryGeometryIdentity!=0);
        R->SetBoolField(TEXT("simulation"),S.bSimulation);R->SetBoolField(TEXT("query"),S.bQuery);R->SetBoolField(TEXT("probe"),S.bProbe);
        N.Number(R,TEXT("collisionTraceType"),S.CollisionTraceType);N.Number(R,TEXT("nativeMaterialCount"),S.NativeMaterialCount);
        TArray<TSharedPtr<FJsonValue>> Q,F;for(int32 I=0;I<4;++I){Q.Add(N.Value(S.QueryFilter[I]));F.Add(N.Value(S.SimulationFilter[I]));}R->SetArrayField(TEXT("queryFilterWords"),Q);R->SetArrayField(TEXT("simulationFilterWords"),F);
        N.Vector(R,TEXT("nativeBoundsMinCm"),S.NativeBoundsMinCm);N.Vector(R,TEXT("nativeBoundsMaxCm"),S.NativeBoundsMaxCm);
        if(S.CollisionShapeType==int32(ECollisionShapeType::Box))
        {N.Vector(R,TEXT("nativeBoxMinCm"),S.NativeBoxMinCm);N.Vector(R,TEXT("nativeBoxMaxCm"),S.NativeBoxMaxCm);N.Number(R,TEXT("nativeBoxMarginCm"),S.NativeBoxMarginCm);}
        else NullFields(R,{TEXT("nativeBoxMinCm"),TEXT("nativeBoxMaxCm"),TEXT("nativeBoxMarginCm")});
        N.Number(R,TEXT("exactGeometryByteCount"),S.GeometryContent.Num());N.Number(R,TEXT("exactMaterialByteCount"),S.MaterialContent.Num());
        R->SetBoolField(TEXT("nativeBodyAvailable"),O.bAvailable);Shapes.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetArrayField(TEXT("shapes"),Shapes);return J;
}
TSharedRef<FJsonObject> SupportAdapterReceipt(const FDDSOriginalRobotNativeCalibrationDraft& D,FJsonNumbers& N)
{
    const auto& O=D.SupportAdapterObservation;auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("DDSCity.OriginalSingleBoxSupportReceipt.v1"));
    J->SetStringField(TEXT("adapterTag"),O.AdapterTag);J->SetBoolField(TEXT("rawSourceAvailable"),O.bRawSourceAvailable);
    if(O.SourceNative.bAvailable && O.FixtureNative.bAvailable)J->SetBoolField(TEXT("nativeEquivalent"),O.bNativeEquivalent);else NullFields(J,{TEXT("nativeEquivalent")});
    if(O.bRawSourceAvailable)
    {J->SetStringField(TEXT("sourceComponent"),O.SourceComponentPath);J->SetStringField(TEXT("sourceOwner"),O.SourceOwnerPath);J->SetStringField(TEXT("sourceWorld"),O.SourceWorldPath);
     J->SetStringField(TEXT("originalMesh"),O.OriginalMeshPath);J->SetStringField(TEXT("originalSetup"),O.OriginalSetupPath);J->SetStringField(TEXT("originalSetupGuid"),O.OriginalSetupGuid.ToString());
     N.Pose(J,TEXT("originalComponentWorldCm"),O.OriginalComponentWorldCm);N.Vector(J,TEXT("buildScaleProvenance"),O.BuildScale3D);N.Vector(J,TEXT("rawBoxDimensionsCm"),O.BoxDimensionsCm);N.Vector(J,TEXT("rawBoxCenterCm"),O.BoxCenterCm);
     N.Vector(J,TEXT("rawBoxRotationPitchYawRollDeg"),FVector(O.BoxRotation.Pitch,O.BoxRotation.Yaw,O.BoxRotation.Roll));N.Number(J,TEXT("collisionTraceFlag"),O.CollisionTraceFlag);N.Number(J,TEXT("boxCollisionEnabled"),O.BoxCollisionEnabled);J->SetStringField(TEXT("componentCollisionProfile"),O.ComponentCollisionProfile.ToString());}
    else NullFields(J,{TEXT("sourceComponent"),TEXT("sourceOwner"),TEXT("sourceWorld"),TEXT("originalMesh"),TEXT("originalSetup"),TEXT("originalSetupGuid"),TEXT("originalComponentWorldCm"),TEXT("buildScaleProvenance"),TEXT("rawBoxDimensionsCm"),TEXT("rawBoxCenterCm"),TEXT("rawBoxRotationPitchYawRollDeg"),TEXT("collisionTraceFlag"),TEXT("boxCollisionEnabled"),TEXT("componentCollisionProfile")});
    J->SetObjectField(TEXT("sourceNative"),SupportBodyReceipt(O.SourceNative,N));J->SetObjectField(TEXT("fixtureNative"),SupportBodyReceipt(O.FixtureNative,N));
    J->SetStringField(TEXT("scope"),TEXT("Diagnostic original single axis-aligned static unwelded query-and-physics box only; no binding/readiness or general affine/curb admission"));return J;
}
TSharedRef<FJsonObject> RecipePredicateReceipt(const FDDSOriginalRobotNativeCalibrationDraft& D)
{
    static const TCHAR* Names[]={TEXT("RECIPE_SETTLE_STEP_BOUNDS"),TEXT("RECIPE_PULSE_STEP_BOUNDS"),TEXT("RECIPE_COAST_STEP_BOUNDS"),TEXT("RECIPE_POSITIVE_FINITE_TORQUE"),TEXT("RECIPE_FINITE_NATIVE_TORQUE"),TEXT("RECIPE_TORQUE_ABOVE_GAME_LIMIT"),TEXT("RECIPE_RIGID_CHASSIS_POSE"),TEXT("RECIPE_VALID_SUPPORT_POSE"),TEXT("RECIPE_SUPPORT_SCALE_ADAPTER"),TEXT("RECIPE_NORMALIZED_FORWARD")};
    auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("firstFailedPredicate"),D.FirstFailedPredicate);TArray<TSharedPtr<FJsonValue>> Rows;
    for(const TCHAR* Name:Names)
    {
        auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),Name);const FString Pass=FString(Name)+TEXT("=FALSE"),Fail=FString(Name)+TEXT("=TRUE");
        if(D.EvaluatedRecipePredicates.Contains(Pass)){R->SetStringField(TEXT("state"),TEXT("EVALUATED"));R->SetBoolField(TEXT("refuses"),false);}
        else if(D.EvaluatedRecipePredicates.Contains(Fail)){R->SetStringField(TEXT("state"),TEXT("EVALUATED"));R->SetBoolField(TEXT("refuses"),true);}
        else {R->SetStringField(TEXT("state"),TEXT("NOT_EVALUATED"));NullFields(R,{TEXT("refuses")});}
        Rows.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetArrayField(TEXT("orderedRecipePredicates"),Rows);Strings(J,TEXT("rawEvaluatedRecipePredicates"),D.EvaluatedRecipePredicates);return J;
}
bool WriteSupportRaw(FOutput& Output,const FDDSOriginalRobotNativeCalibrationDraft& D,const TSharedRef<FJsonObject>& Summary,FJsonNumbers& N,TFunctionRef<bool()> StillExact,FString& Error)
{
    const auto& O=D.SupportAdapterObservation;TArray<TSharedPtr<FJsonValue>> Files;
    const auto BytesField=[&](const TSharedRef<FJsonObject>& J,const TCHAR* Key,const TArray<uint8>& Bytes)
    {if(Bytes.IsEmpty()){NullFields(J,{Key});return true;}if(Bytes.Num()>1024*1024){Error=TEXT("Retained support content exceeds exact1MiB bound");return false;}
     J->SetStringField(Key,FBase64::Encode(Bytes));return true;};
    if(!O.AggregateGeometryContent.IsEmpty() || !O.SetupSettingsContent.IsEmpty() || !O.ComponentBodySettingsContent.IsEmpty())
    {
        if(!StillExact())return false;auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("DDSCity.ExactOriginalSupportRawBytes.v1"));J->SetStringField(TEXT("nonce"),Output.Nonce);J->SetStringField(TEXT("sourceComponent"),O.SourceComponentPath);J->SetBoolField(TEXT("rawSourceAvailable"),O.bRawSourceAvailable);
        if(!BytesField(J,TEXT("aggregateGeometryBase64"),O.AggregateGeometryContent) || !BytesField(J,TEXT("setupSettingsBase64"),O.SetupSettingsContent) || !BytesField(J,TEXT("componentBodySettingsBase64"),O.ComponentBodySettingsContent))return false;
        if(!Output.Write(TEXT("support-adapter-raw"),J,Error) || !StillExact())return false;Files.Add(MakeShared<FJsonValueString>(TEXT("support-adapter-raw")));
    }
    for(int32 Kind=0;Kind<2;++Kind)
    {
        const auto& B=Kind?O.FixtureNative:O.SourceNative;if(B.Shapes.Num()>8){Error=TEXT("Support native roster exceeds8shape bound");return false;}
        for(int32 I=0;I<B.Shapes.Num();++I)
        {
            if(!StillExact())return false;const auto& S=B.Shapes[I];auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("DDSCity.ExactOriginalSupportNativeShapeBytes.v1"));J->SetStringField(TEXT("nonce"),Output.Nonce);
            J->SetBoolField(TEXT("nativeBodyAvailable"),B.bAvailable);N.Number(J,TEXT("shapeArrayIndex"),I);N.Number(J,TEXT("shapeIndex"),S.ShapeIndex);
            Identity(J,TEXT("shapeIdentity"),S.ShapeIdentity,S.ShapeIdentity!=0);Identity(J,TEXT("geometryIdentity"),S.GeometryIdentity,S.GeometryIdentity!=0);
            if(!BytesField(J,TEXT("geometryBase64"),S.GeometryContent) || !BytesField(J,TEXT("materialBase64"),S.MaterialContent))return false;
            const FString Stage=FString::Printf(TEXT("support-adapter-%s-shape-%d"),Kind?TEXT("fixture"):TEXT("source"),I);
            if(!Output.Write(*Stage,J,Error) || !StillExact())return false;Files.Add(MakeShared<FJsonValueString>(Stage));
        }
    }
    Summary->SetArrayField(TEXT("exactSupportSidecarStages"),Files);Summary->SetStringField(TEXT("encoding"),TEXT("Exact bounded producer byte snapshots in Base64 sidecars; not a measured certificate"));return true;
}

bool WriteMeasurementRaw(FOutput& Output,const FDDSOriginalRobotMeasurementDraft& Draft,const FDDSOriginalRobotNativeCalibrationDraft& Native,
    const TSharedRef<FJsonObject>& Summary,FJsonNumbers& N,TFunctionRef<bool()> StillExact,FString& Error)
{
    constexpr int32 ChunkSteps=16,MaximumRetainedSteps=4096,MaximumShapeBytes=4*1024*1024;
    if(Native.Steps.Num()>MaximumRetainedSteps){Error=TEXT("Retained native step count exceeds producer finite source bound");return false;}
    TArray<TSharedPtr<FJsonValue>> ShapeFiles,StepFiles;
    for(int32 Kind=0;Kind<2;++Kind)for(int32 I=0;I<9;++I)
    {
        if(!StillExact())return false;const auto& Bytes=Kind?Native.NativeFixtureConvexContent[I]:Draft.ComponentConvexContent[I];
        if(Bytes.IsEmpty())continue;if(Bytes.Num()>MaximumShapeBytes){Error=TEXT("Retained exact convex content exceeds supported4MiB source bound");return false;}
        const FString Stage=FString::Printf(TEXT("measurement-%s-shape-%d"),Kind?TEXT("native"):TEXT("source"),I);auto J=MakeShared<FJsonObject>();
        J->SetStringField(TEXT("schema"),TEXT("DDSCity.ExactObservedOriginalCollisionBytes.v1"));J->SetStringField(TEXT("nonce"),Output.Nonce);J->SetStringField(TEXT("stage"),Stage);J->SetStringField(TEXT("kind"),Kind?TEXT("native fixture component-owned convex content"):TEXT("CaptureDraft original City component-owned convex content"));
        N.Number(J,TEXT("bodyIndex"),I);N.Number(J,TEXT("exactByteCount"),Bytes.Num());J->SetStringField(TEXT("encoding"),TEXT("base64 of exact producer ConvexContent snapshot; not a collision-map/certificate hash"));J->SetStringField(TEXT("exactConvexContentBase64"),FBase64::Encode(Bytes));
        const auto& B=Kind?Native.NativeReferenceBodies[I]:Draft.Bodies[I];J->SetStringField(TEXT("component"),B.ComponentObjectPath);J->SetStringField(TEXT("componentBodySetup"),B.ComponentBodySetupObjectPath);
        if(B.bAvailable)J->SetStringField(TEXT("componentBodySetupGuid"),B.ComponentBodySetupGuid.ToString());else NullFields(J,{TEXT("componentBodySetupGuid")});
        if(!Output.Write(*Stage,J,Error) || !StillExact())return false;ShapeFiles.Add(MakeShared<FJsonValueString>(Stage));
    }
    for(int32 First=0;First<Native.Steps.Num();First+=ChunkSteps)
    {
        if(!StillExact())return false;auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("schema"),TEXT("DDSCity.NativeMeasurementStepChunk.v1"));J->SetStringField(TEXT("nonce"),Output.Nonce);
        N.Number(J,TEXT("firstRetainedArrayIndex"),First);TArray<TSharedPtr<FJsonValue>> Rows;
        for(int32 I=First;I<FMath::Min(First+ChunkSteps,Native.Steps.Num());++I)Rows.Add(MakeShared<FJsonValueObject>(StepIncrement(Native.Steps[I],N)));
        J->SetArrayField(TEXT("steps"),Rows);J->SetBoolField(TEXT("finiteNumericFields"),N.bFinite);const FString Stage=FString::Printf(TEXT("measurement-steps-%d"),First/ChunkSteps);
        if(!Output.Write(*Stage,J,Error) || !StillExact())return false;StepFiles.Add(MakeShared<FJsonValueString>(Stage));
    }
    Summary->SetArrayField(TEXT("exactShapeSidecarStages"),ShapeFiles);Summary->SetArrayField(TEXT("nativeStepSidecarStages"),StepFiles);Summary->SetStringField(TEXT("sidecarNaming"),TEXT("engine-owned-city-calibration-{nonce}-{stage}.json; exclusive writes obey unchanged8MiB report/256MiB root/5s context guards"));return true;
}

}

bool UDDSOriginalCityCalibrationDiagnosticSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
    return false;
#else
    const auto* World=Cast<UWorld>(Outer);
    // Excludes the producer's separate transient Game world, preventing recursion.
    return World && World->WorldType==EWorldType::Game && World->GetOutermost()->GetName()==DDSCityCalibrationDiagnostic::CityMap
        && FParse::Param(FCommandLine::Get(),TEXT("DDSOriginalCityCalibrationDiagnostic")) && Super::ShouldCreateSubsystem(Outer);
#endif
}
void UDDSOriginalCityCalibrationDiagnosticSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{Super::Initialize(Collection);StartedRealS=FPlatformTime::Seconds();}
bool UDDSOriginalCityCalibrationDiagnosticSubsystem::IsTickable() const
{return !IsTemplate() && IsInitialized() && !bFinished && !bRunning;}
TStatId UDDSOriginalCityCalibrationDiagnosticSubsystem::GetStatId() const
{RETURN_QUICK_DECLARE_CYCLE_STAT(UDDSOriginalCityCalibrationDiagnosticSubsystem,STATGROUP_Tickables);}
bool UDDSOriginalCityCalibrationDiagnosticSubsystem::StartupContextMatches(FString& Error) const
{
    using namespace DDSCityCalibrationDiagnostic;
    const double Now=FPlatformTime::Seconds();
    if(!FMath::IsFinite(Now) || !FMath::IsFinite(StartedRealS) || Now<StartedRealS || Now-StartedRealS>=120.0)
    {Error=TEXT("Original City/source startup wait exceeded120s or monotonic clock invalid; no timed diagnostic BEGIN");return false;}
    auto* World=StartupWorld.Get();auto* Mode=StartupMode.Get();auto* PC=StartupController.Get();auto* Pawn=StartupPawn.Get();
    if(!IsInGameThread() || !IsInitialized() || !Live(this) || !Live(World) || GetWorld()!=World || World->bIsTearingDown
       || World->WorldType!=EWorldType::Game || !World->HasBegunPlay() || !World->PersistentLevel
       || World->PersistentLevel->GetOutermost()->GetName()!=CityMap || !StartupSceneIdentity || World->GetPhysicsScene()!=StartupSceneIdentity
       || !Live(Mode) || World->GetAuthGameMode()!=Mode || Mode->GetClass()->GetPathName()!=CityMode
       || !Live(PC) || !PC->IsLocalController() || PC->GetWorld()!=World || World->GetFirstPlayerController()!=PC
       || !Live(Pawn) || Pawn->IsActorBeingDestroyed() || !Pawn->HasActorBegunPlay() || Pawn->GetWorld()!=World
       || PC->GetPawn()!=Pawn || Pawn->GetController()!=PC || PC->GetViewTarget()!=StartupView.Get()
       || (bStartupHadView && (!Live(StartupView.Get()) || StartupView->IsActorBeingDestroyed() || StartupView->GetWorld()!=World)))
    {Error=TEXT("Exact original City startup world/mode/controller/pawn/view/scene context changed; no timed diagnostic BEGIN");return false;}
    Error.Reset();return true;
}
void UDDSOriginalCityCalibrationDiagnosticSubsystem::ClearStartupSources()
{
    PreloadedPublisherClass=nullptr;PreloadedCargoMesh=nullptr;
    StartupWorld.Reset();StartupMode.Reset();StartupController.Reset();StartupPawn.Reset();StartupView.Reset();StartupSceneIdentity=nullptr;
}
void UDDSOriginalCityCalibrationDiagnosticSubsystem::Tick(float DeltaSeconds)
{
    using namespace DDSCityCalibrationDiagnostic;
    Super::Tick(DeltaSeconds);if(bFinished || bRunning || bStartupLoading)return;
    const auto RefuseStartup=[&](const FString& Reason)
    {bFinished=true;UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_STARTUP_REFUSED: %s; no timed diagnostic execution"),*Reason);ClearStartupSources();};
    const double Now=FPlatformTime::Seconds();
    if(!FMath::IsFinite(Now) || !FMath::IsFinite(StartedRealS) || Now<StartedRealS || Now-StartedRealS>=120.0)
    {RefuseStartup(TEXT("Actual City/controller/original source startup wait exceeded120s or clock invalid"));return;}
    auto* World=GetWorld();auto* PC=World?World->GetFirstPlayerController():nullptr;
    if(!bStartupSourcesLoaded)
    {
        if(!World || !World->HasBegunPlay() || !PC || !PC->IsLocalController() || !PC->GetPawn() || !PC->GetPawn()->HasActorBegunPlay())return;
        StartupWorld=World;StartupMode=World->GetAuthGameMode();StartupController=PC;StartupPawn=PC->GetPawn();StartupView=PC->GetViewTarget();
        bStartupHadView=PC->GetViewTarget()!=nullptr;StartupSceneIdentity=World->GetPhysicsScene();
        FString Error;if(!StartupContextMatches(Error)){RefuseStartup(Error);return;}
        TGuardValue<bool> LoadingGuard(bStartupLoading,true);TStrongObjectPtr<UDDSOriginalCityCalibrationDiagnosticSubsystem> SelfPin(this);
        bStartupSourcesLoaded=true; // One attempt, including refusal; no per-tick reload.
        PreloadedPublisherClass=LoadClass<AActor>(nullptr,PublisherClass);
        if(!StartupContextMatches(Error)){RefuseStartup(Error);return;}
        PreloadedCargoMesh=LoadObject<UStaticMesh>(nullptr,CargoMeshPath);
        if(!StartupContextMatches(Error)){RefuseStartup(Error);return;}
    }
    FString Error;if(!StartupContextMatches(Error)){RefuseStartup(Error);return;}
    auto* Class=PreloadedPublisherClass.Get();auto* CargoMesh=PreloadedCargoMesh.Get();
    if(!Live(Class) || Class->GetPathName()!=PublisherClass || Class->IsChildOf(APawn::StaticClass())
       || !Live(CargoMesh) || CargoMesh->GetPathName()!=CargoMeshPath)
    {RefuseStartup(TEXT("Exact live non-pawn supplied publisher/original BurgerBox source required; no timed reload or replacement"));return;}
    if(CargoMesh->IsCompiling())
    {
        if(!bStartupWaitingLogged){bStartupWaitingLogged=true;UE_LOG(LogDDSCityCalibrationDiagnostic,Display,TEXT("CITY_CALIBRATION_SOURCE_STARTUP_WAIT cargoIsCompiling=1; returning to ordinary engine ticks before BEGIN"));}
        return; // LaunchEngineLoop processes async asset tasks normally; no forced finish/task pumping.
    }
    if(!StartupContextMatches(Error)){RefuseStartup(Error);return;}
    UE_LOG(LogDDSCityCalibrationDiagnostic,Display,TEXT("CITY_CALIBRATION_SOURCE_STARTUP_READY elapsedStartup=%.6fs cargoIsCompiling=0; source readiness only"),FPlatformTime::Seconds()-StartedRealS);
    bFinished=true;RunOnce();
}
bool UDDSOriginalCityCalibrationDiagnosticSubsystem::ReleaseOwned(FString& Error)
{
    using namespace DDSCityCalibrationDiagnostic;
    Error.Reset();if(!IsInGameThread() || bRunning){Error=TEXT("Owned diagnostic cleanup deferred while caller active or wrong thread");return false;}
    if(bOwnedGameMountLease)
    {
        if(!FDDSOriginalRobotMeasurementProducer::ReleaseDeclaredGameMountSources(OwnedGameMounts,Error))return false;
        bOwnedGameMountLease=false;
    }
    if(OwnedOwner)
    {
        if(!Live(OwnedOwner.Get()) || OwnedOwner->GetOwner()!=OwnedCarrier.Get())
        {Error=TEXT("Owned diagnostic owner lifetime changed; actor teardown cannot bypass its release fence");return false;}
        if(!OwnedOwner->ReleaseOriginalAssembly(Error))return false;
    }
    // The owner must permit component release before any carrier/source actor is
    // destroyed. A foreign claim/fence refusal keeps these exact objects retained.
    const auto Destroy=[&](AActor* Actor)
    {return !Actor || Actor->IsActorBeingDestroyed() || (Live(Actor) && Actor->Destroy());};
    if(!Destroy(OwnedCarrier.Get()) || !Destroy(OwnedPublisher.Get()) || !Destroy(OwnedCargoHolder.Get()))
    {Error=TEXT("Owned diagnostic actor destruction refused; exact remaining references retained");return false;}
    OwnedOwner=nullptr;OwnedCarrier=nullptr;OwnedPublisher=nullptr;OwnedCargoHolder=nullptr;return true;
}
void UDDSOriginalCityCalibrationDiagnosticSubsystem::Deinitialize()
{
    bFinished=true;FString Error;
    if(!ReleaseOwned(Error)){UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_CLEANUP_PENDING: %s; no foreign/global world destroyed"),*Error);}
    ClearStartupSources();
    Super::Deinitialize();
}
void UDDSOriginalCityCalibrationDiagnosticSubsystem::RunOnce()
{
    using namespace DDSCityCalibrationDiagnostic;
    static bool bAnyCallerActive=false;
    if(!IsInGameThread() || bRunning || bAnyCallerActive)
    {UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_REFUSED: wrong thread/reentry"));return;}
    TGuardValue<bool> AnyGuard(bAnyCallerActive,true);bRunning=true;
    ON_SCOPE_EXIT {bRunning=false;ClearStartupSources();};
    FOutput Output;FString Error;
    if(!Output.Configure(Error)){UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_REFUSED configuration=%s; no files created"),*Error);return;}
    if(!bStartupSourcesLoaded || !StartupContextMatches(Error) || !Live(PreloadedPublisherClass.Get())
       || PreloadedPublisherClass->GetPathName()!=PublisherClass || PreloadedPublisherClass->IsChildOf(APawn::StaticClass())
       || !Live(PreloadedCargoMesh.Get()) || PreloadedCargoMesh->GetPathName()!=CargoMeshPath || PreloadedCargoMesh->IsCompiling())
    {UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_STARTUP_REFUSED before BEGIN: retained exact original assets/context not ready reason=%s"),*Error);return;}
    const double Begin=FPlatformTime::Seconds();const double Deadline=Begin+CallerBudgetS;
    auto Report=MakeShared<FJsonObject>();FJsonNumbers Numbers;
    Report->SetStringField(TEXT("schema"),TEXT("DDSCity.ActualCityCalibrationDiagnostic.v1"));Report->SetStringField(TEXT("nonce"),Output.Nonce);
    Report->SetStringField(TEXT("utc"),FDateTime::UtcNow().ToIso8601());Report->SetBoolField(TEXT("gameReady"),false);Report->SetBoolField(TEXT("drivingEnabled"),false);
    Report->SetBoolField(TEXT("calibrationCertificateIssued"),false);Report->SetBoolField(TEXT("serviceOrRecoveryGranted"),false);
    Report->SetStringField(TEXT("guardContract"),TEXT("RootNoReparse5s256MiB: existing fresh direct child, no overwrite/reparse/foreign file; outer process watchdog must preempt stuck synchronous native API"));
    Report->SetStringField(TEXT("oldNativeRecordMeaning"),TEXT("CombinedNativeRecord is source graph/unstepped independent engine report; never assembled-motion proof"));
    UE_LOG(LogDDSCityCalibrationDiagnostic,Display,TEXT("CITY_CALIBRATION_DIAGNOSTIC_BEGIN nonce=%s outerWatchdog=5s rootBudget=256MiB mode=oneDiagnosticNoDrive"),*Output.Nonce);
    UWorld* const World=GetWorld();auto* Mode=World?World->GetAuthGameMode():nullptr;auto* PC=World?World->GetFirstPlayerController():nullptr;
    APawn* const CityPawn=PC?PC->GetPawn():nullptr;AActor* const CityView=PC?PC->GetViewTarget():nullptr;
    auto* const CityScene=World?World->GetPhysicsScene():nullptr;
    TArray<TStrongObjectPtr<UObject>> Lifetime;Lifetime.Emplace(this);
    if(World)Lifetime.Emplace(World);if(Mode)Lifetime.Emplace(Mode);if(PC)Lifetime.Emplace(PC);if(CityPawn)Lifetime.Emplace(CityPawn);if(CityView)Lifetime.Emplace(CityView);
    const auto Context=[&]()
    {
        if(FPlatformTime::Seconds()>=Deadline){if(Error.IsEmpty())Error=TEXT("Caller exceeded5s diagnostic budget; preserve first/raw output and outer watchdog evidence");return false;}
        if(!bRunning || !Live(this) || !Live(World) || World->bIsTearingDown || GetWorld()!=World || World->WorldType!=EWorldType::Game || !World->HasBegunPlay()
           || !World->PersistentLevel || World->PersistentLevel->GetOutermost()->GetName()!=CityMap || World->GetPhysicsScene()!=CityScene || !CityScene
           || !Live(Mode) || World->GetAuthGameMode()!=Mode || Mode->GetClass()->GetPathName()!=CityMode
           || !Live(PC) || !PC->IsLocalController() || PC->GetWorld()!=World || World->GetFirstPlayerController()!=PC
           || !Live(CityPawn) || CityPawn->IsActorBeingDestroyed() || PC->GetPawn()!=CityPawn || CityPawn->GetController()!=PC || CityPawn->GetWorld()!=World
           || PC->GetViewTarget()!=CityView || !Output.Guarded())
        {if(Error.IsEmpty())Error=TEXT("Actual City/controller/pawn/view/scene/output context revoked; no readiness inferred");return false;}
        return true;
    };
    Report->SetStringField(TEXT("world"),GetPathNameSafe(World));Report->SetStringField(TEXT("originalCityMode"),GetPathNameSafe(Mode));
    Report->SetStringField(TEXT("originalCityController"),GetPathNameSafe(PC));Report->SetStringField(TEXT("originalCityPawn"),GetPathNameSafe(CityPawn));
    Report->SetStringField(TEXT("borrowedCitySceneIdentity"),FString::Printf(TEXT("%p"),CityScene));
    FDDSOriginalRobotMeasurementDraft Draft;FDDSOriginalRobotNativeCalibrationDraft Native;
    bool bDraftCaptured=false,bRunCalled=false,bProducerCompleted=false,bMeasurementEnabled=false,bSupportEnabled=false;FString MeasurementDigest;
    const auto Work=[&]()
    {
        if(!Context())return false;
        FDDSOriginalLiveIntakeInputs Intake;FDDSOriginalRobotDraftInputs Inputs;FDDSOriginalRobotCalibrationRecipe Recipe;
        if(!ReadPinned(TEXT("CombinedNativeRecord="),NativeSha,Intake.NativeGeometryJsonBytes,Error) || !Context()
           || !ReadPinned(TEXT("CombinedOriginalLedger="),LedgerSha,Intake.OriginalIntegrityLedgerBytes,Error) || !Context()
           || !ReadPinned(TEXT("CombinedPackagePins="),PackagePinsSha,Intake.PackagePinsJsonBytes,Error) || !Context())return false;
        FString SuppliedPinsHash;if(!FParse::Value(FCommandLine::Get(),TEXT("CombinedPackagePinsSha="),SuppliedPinsHash) || SuppliedPinsHash!=PackagePinsSha)
        {Error=TEXT("Explicit current root CombinedPackagePinsSha token mismatch");return false;}
        if(!Hypotheses(Inputs,Recipe,Error) || !Context())return false;
        if(!ConfigureMeasurement(Recipe,bMeasurementEnabled,MeasurementDigest,Error) || !Context())return false;
        if(!ConfigureSupport(Recipe,bSupportEnabled,Error) || !Context())return false;
        if(bSupportEnabled){Report->SetStringField(TEXT("supportOption"),SupportChoice);Report->SetStringField(TEXT("supportTruth"),TEXT("Explicit original single-box diagnostic adapter; no source mutation, general affine support or readiness"));}
        if(bMeasurementEnabled)
        {Report->SetStringField(TEXT("declaredMeasurementGameRecipeRecord"),MeasurementRecord);Report->SetStringField(TEXT("declaredMeasurementGameRecipeRecordSha256"),MeasurementDigest);
         Report->SetStringField(TEXT("measurementOption"),MeasurementChoice);Report->SetStringField(TEXT("measurementTruth"),TEXT("Explicit GAME mount/engineering inputs, actual native outcomes separate; no measured binding flags or certificate"));}
        Report->SetStringField(TEXT("declaredGameHypothesisRecord"),HypothesisRecord);Report->SetStringField(TEXT("declaredGameProfileRecordSha256"),Inputs.DeclaredGameProfile.ProfileSha256);
        Report->SetStringField(TEXT("declaredGameProvenance"),Inputs.DeclaredGameProfile.Provenance);
        // One actual City WorldStatic downtrace. No synthetic support, streaming
        // completion promise, retries on alternate geometry or original asset edits.
        const FVector Seed=CityPawn->GetActorLocation();const FVector Start=Seed+FVector(0,0,2000),End=Seed-FVector(0,0,5000);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(DDSActualCityCalibrationSupport),false);Query.AddIgnoredActor(CityPawn);
        FHitResult Hit;const bool HitFound=World->LineTraceSingleByObjectType(Hit,Start,End,FCollisionObjectQueryParams(ECC_WorldStatic),Query);
        if(!Context())return false;
        auto Support=SupportObservation(*World,Hit,Start,End,Numbers);Report->SetObjectField(TEXT("actualCityDowntraceSupport"),Support);
        auto Preflight=MakeShared<FJsonObject>();Preflight->SetStringField(TEXT("schema"),TEXT("DDSCity.ActualCityCalibrationSupport.v1"));Preflight->SetStringField(TEXT("nonce"),Output.Nonce);
        Preflight->SetObjectField(TEXT("actualSupport"),Support);Preflight->SetBoolField(TEXT("finiteNumericFields"),Numbers.bFinite);
        if(!Output.Write(TEXT("support"),Preflight,Error) || !Context())return false;
        UE_LOG(LogDDSCityCalibrationDiagnostic,Display,TEXT("CITY_CALIBRATION_SUPPORT_OBSERVED nonce=%s actor=%s component=%s; exact native support roster written before RunCalibration"),*Output.Nonce,*GetPathNameSafe(Hit.GetActor()),*GetPathNameSafe(Hit.GetComponent()));
        if(!HitFound || !Hit.bBlockingHit || Hit.bStartPenetrating || !Live(Hit.GetComponent()) || !Hit.GetComponent()->IsRegistered()
           || Hit.GetComponent()->GetWorld()!=World || Hit.ImpactPoint.ContainsNaN() || Hit.ImpactNormal.ContainsNaN())
        {Error=TEXT("Actual City downtrace has no coherent registered blocking support; no floor/proxy generated");return false;}
        auto* SupportMeshComponent=Cast<UStaticMeshComponent>(Hit.GetComponent());
        if(!SupportMeshComponent){Error=TEXT("Actual support is not a static mesh component; producer has no adapter for this exact recorded component class");return false;}
        Lifetime.Emplace(SupportMeshComponent);Lifetime.Emplace(SupportMeshComponent->GetOwner());
        if(SupportMeshComponent->GetStaticMesh())Lifetime.Emplace(SupportMeshComponent->GetStaticMesh().Get());if(SupportMeshComponent->GetBodySetup())Lifetime.Emplace(SupportMeshComponent->GetBodySetup());
        const FTransform SupportPose=SupportMeshComponent->GetComponentTransform();UBodySetup* const SupportSetup=SupportMeshComponent->GetBodySetup();
        const FGuid SupportGuid=SupportSetup?SupportSetup->BodySetupGuid:FGuid();
        UClass* Class=PreloadedPublisherClass.Get();UStaticMesh* CargoMesh=PreloadedCargoMesh.Get();
        // Read-only GT settings/world/configured-solver getters of the borrowed
        // original City. These are not PT evolution dt or PT gravity observations.
        auto PhysicsReadbacks=MakeShared<FJsonObject>();
        PhysicsReadbacks->SetStringField(TEXT("scope"),TEXT("Original City GT settings/world/configured-solver getters only; no physics-thread step/gravity observation"));
        PhysicsReadbacks->SetStringField(TEXT("borrowedWorldIdentity"),FString::Printf(TEXT("%p"),World));
        PhysicsReadbacks->SetStringField(TEXT("borrowedSceneIdentity"),FString::Printf(TEXT("%p"),CityScene));
        const bool bPhysicsContextBefore=Context();
        const UPhysicsSettings* Settings=bPhysicsContextBefore?GetDefault<UPhysicsSettings>():nullptr;
        const bool bSettingsLive=Live(Settings);auto* BorrowedSolver=bPhysicsContextBefore?CityScene->GetSolver():nullptr;
        PhysicsReadbacks->SetBoolField(TEXT("contextBeforeReadbacks"),bPhysicsContextBefore);
        PhysicsReadbacks->SetBoolField(TEXT("settingsAvailable"),bSettingsLive);
        PhysicsReadbacks->SetBoolField(TEXT("configuredSolverAvailable"),BorrowedSolver!=nullptr);
        PhysicsReadbacks->SetStringField(TEXT("borrowedSolverIdentity"),FString::Printf(TEXT("%p"),BorrowedSolver));
        if(bSettingsLive)
        {PhysicsReadbacks->SetStringField(TEXT("settingsObjectPath"),Settings->GetPathName());PhysicsReadbacks->SetBoolField(TEXT("settingsTickPhysicsAsync"),Settings->bTickPhysicsAsync);
         Numbers.Number(PhysicsReadbacks,TEXT("settingsAsyncFixedTimeStepSizeS"),Settings->AsyncFixedTimeStepSize);Numbers.Number(PhysicsReadbacks,TEXT("settingsDefaultGravityCmps2"),Settings->DefaultGravityZ);}
        else
        {PhysicsReadbacks->SetField(TEXT("settingsObjectPath"),MakeShared<FJsonValueNull>());PhysicsReadbacks->SetField(TEXT("settingsTickPhysicsAsync"),MakeShared<FJsonValueNull>());
         PhysicsReadbacks->SetField(TEXT("settingsAsyncFixedTimeStepSizeS"),MakeShared<FJsonValueNull>());PhysicsReadbacks->SetField(TEXT("settingsDefaultGravityCmps2"),MakeShared<FJsonValueNull>());}
        if(bPhysicsContextBefore)Numbers.Number(PhysicsReadbacks,TEXT("worldGravityGetterCmps2"),World->GetGravityZ());
        else PhysicsReadbacks->SetField(TEXT("worldGravityGetterCmps2"),MakeShared<FJsonValueNull>());
        if(BorrowedSolver)Numbers.Number(PhysicsReadbacks,TEXT("configuredSolverAsyncDeltaTimeS"),BorrowedSolver->GetAsyncDeltaTime());
        else PhysicsReadbacks->SetField(TEXT("configuredSolverAsyncDeltaTimeS"),MakeShared<FJsonValueNull>());
        const bool bPhysicsContextAfter=Context();
        PhysicsReadbacks->SetBoolField(TEXT("contextAfterReadbacks"),bPhysicsContextAfter);
        if(BorrowedSolver && bPhysicsContextAfter)PhysicsReadbacks->SetBoolField(TEXT("borrowedSolverIdentityRetained"),CityScene->GetSolver()==BorrowedSolver);
        else PhysicsReadbacks->SetField(TEXT("borrowedSolverIdentityRetained"),MakeShared<FJsonValueNull>());
        Report->SetObjectField(TEXT("originalCityPhysicsReadbacks"),PhysicsReadbacks);
        // Preserve the exact seven OR predicates and their short-circuit order.
        // Later predicates remain unknown, including Pawn ancestry and compilation.
        auto SourceLoad=MakeShared<FJsonObject>();
        SourceLoad->SetStringField(TEXT("schema"),TEXT("DDSCity.OriginalSourceLoadPredicates.v1"));
        SourceLoad->SetStringField(TEXT("expectedPublisherClassPath"),PublisherClass);
        SourceLoad->SetStringField(TEXT("expectedCargoMeshPath"),CargoMeshPath);
        SourceLoad->SetField(TEXT("actualPublisherClassPath"),MakeShared<FJsonValueNull>());
        SourceLoad->SetField(TEXT("actualCargoMeshPath"),MakeShared<FJsonValueNull>());
        SourceLoad->SetField(TEXT("pawnAncestryClassPath"),MakeShared<FJsonValueNull>());
        const TCHAR* PredicateNames[]={TEXT("context"),TEXT("publisherClassLive"),TEXT("publisherExactPath"),TEXT("publisherIsPawnDescendant"),TEXT("cargoMeshLive"),TEXT("cargoExactPath"),TEXT("cargoIsCompiling")};
        const TCHAR* PredicateExpressions[]={TEXT("!Context()"),TEXT("!Live(Class)"),TEXT("Class->GetPathName()!=PublisherClass"),TEXT("Class->IsChildOf(APawn::StaticClass())"),TEXT("!Live(CargoMesh)"),TEXT("CargoMesh->GetPathName()!=CargoMeshPath"),TEXT("CargoMesh->IsCompiling()")};
        TArray<TSharedRef<FJsonObject>> PredicateRows;TArray<TSharedPtr<FJsonValue>> PredicateValues;
        for(int32 I=0;I<7;++I)
        {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),PredicateNames[I]);Row->SetStringField(TEXT("refusalExpression"),PredicateExpressions[I]);
         Row->SetBoolField(TEXT("evaluated"),false);Row->SetStringField(TEXT("state"),TEXT("UNKNOWN_NOT_EVALUATED"));Row->SetField(TEXT("refuses"),MakeShared<FJsonValueNull>());
         PredicateRows.Add(Row);PredicateValues.Add(MakeShared<FJsonValueObject>(Row));}
        SourceLoad->SetArrayField(TEXT("orderedGuardPredicates"),PredicateValues);
        Report->SetObjectField(TEXT("originalSourceLoadPredicates"),SourceLoad);
        FString ActualPublisherPath,ActualCargoPath,FirstSourcePredicate,FirstSourceReason;
        const auto ObservePredicate=[&](int32 I,bool bRefuses)
        {
            auto Row=PredicateRows[I];Row->SetBoolField(TEXT("evaluated"),true);Row->SetStringField(TEXT("state"),TEXT("EVALUATED"));Row->SetBoolField(TEXT("refuses"),bRefuses);
            if(bRefuses)
            {
                FirstSourcePredicate=PredicateNames[I];
                switch(I)
                {
                case 0:FirstSourceReason=Error;break;
                case 1:FirstSourceReason=TEXT("Exact supplied publisher class is unavailable/not live; no replacement");break;
                case 2:FirstSourceReason=FString::Printf(TEXT("Exact supplied publisher class path mismatch: actual=%s expected=%s; no replacement"),*ActualPublisherPath,PublisherClass);break;
                case 3:FirstSourceReason=TEXT("Exact supplied publisher class is an APawn descendant; original caller non-pawn source guard refuses it; no replacement");break;
                case 4:FirstSourceReason=TEXT("Exact supplied original BurgerBox mesh is unavailable/not live; no replacement");break;
                case 5:FirstSourceReason=FString::Printf(TEXT("Exact supplied original BurgerBox mesh path mismatch: actual=%s expected=%s; no replacement"),*ActualCargoPath,CargoMeshPath);break;
                case 6:FirstSourceReason=TEXT("Exact supplied original BurgerBox mesh IsCompiling=true; no eager compile/wait/retry or replacement");break;
                }
            }
            return bRefuses;
        };
        const auto PublisherPathMismatch=[&]()
        {ActualPublisherPath=Class->GetPathName();SourceLoad->SetStringField(TEXT("actualPublisherClassPath"),ActualPublisherPath);return ActualPublisherPath!=PublisherClass;};
        const auto PublisherPawnAncestry=[&]()
        {UClass* PawnClass=APawn::StaticClass();SourceLoad->SetStringField(TEXT("pawnAncestryClassPath"),PawnClass->GetPathName());return Class->IsChildOf(PawnClass);};
        const auto CargoPathMismatch=[&]()
        {ActualCargoPath=CargoMesh->GetPathName();SourceLoad->SetStringField(TEXT("actualCargoMeshPath"),ActualCargoPath);return ActualCargoPath!=CargoMeshPath;};
        if(ObservePredicate(0,!Context()) || ObservePredicate(1,!Live(Class)) || ObservePredicate(2,PublisherPathMismatch()) || ObservePredicate(3,PublisherPawnAncestry())
           || ObservePredicate(4,!Live(CargoMesh)) || ObservePredicate(5,CargoPathMismatch()) || ObservePredicate(6,CargoMesh->IsCompiling()))
        {SourceLoad->SetStringField(TEXT("firstFailedPredicate"),FirstSourcePredicate);SourceLoad->SetStringField(TEXT("firstFailedPredicateReason"),FirstSourceReason);
         if(Error.IsEmpty())Error=FirstSourceReason;return false;}
        SourceLoad->SetStringField(TEXT("firstFailedPredicate"),FString());SourceLoad->SetStringField(TEXT("firstFailedPredicateReason"),FString());
        Lifetime.Emplace(Class);Lifetime.Emplace(CargoMesh);
        const FTransform SourcePose(FRotator(0,CityPawn->GetActorRotation().Yaw,0),Hit.ImpactPoint+FVector(0,0,45),FVector::OneVector);
        FActorSpawnParameters Spawn;Spawn.ObjectFlags=RF_Transient;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        OwnedPublisher=World->SpawnActor<AActor>(Class,SourcePose,Spawn);OwnedCargoHolder=World->SpawnActor<AActor>(AActor::StaticClass(),SourcePose,Spawn);
        if(!Context() || !Live(OwnedPublisher.Get()) || !Live(OwnedCargoHolder.Get())){if(Error.IsEmpty())Error=TEXT("Owned original diagnostic source actor creation failed");return false;}
        Lifetime.Emplace(OwnedPublisher.Get());Lifetime.Emplace(OwnedCargoHolder.Get());
        OwnedPublisher->SetActorTickEnabled(false);OwnedPublisher->SetActorHiddenInGame(true);OwnedCargoHolder->SetActorTickEnabled(false);OwnedCargoHolder->SetActorHiddenInGame(true);
        TArray<UStaticMeshComponent*> SourceComponents;OwnedPublisher->GetComponents(SourceComponents);
        if(SourceComponents.Num()!=15){Error=TEXT("Actual exact publisher15 source mesh components required");return false;}
        for(auto* C:SourceComponents)
        {if(!Live(C) || !C->IsRegistered() || C->GetOwner()!=OwnedPublisher.Get() || C->GetWorld()!=World){Error=TEXT("Owned publisher actual source component context unavailable");return false;}
         Lifetime.Emplace(C);C->SetSimulatePhysics(false);C->SetEnableGravity(false);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetComponentTickEnabled(false);}
        auto* Cargo=NewObject<UStaticMeshComponent>(OwnedCargoHolder.Get(),NAME_None,RF_Transient);if(!Live(Cargo)){Error=TEXT("Owned excluded original source parcel component allocation failed");return false;}
        Lifetime.Emplace(Cargo);OwnedCargoHolder->AddInstanceComponent(Cargo);Cargo->SetCollisionEnabled(ECollisionEnabled::NoCollision);Cargo->SetEnableGravity(false);
        if(!Cargo->SetStaticMesh(CargoMesh)){Error=TEXT("Exact original source-only BurgerBox mesh installation failed");return false;}
        Cargo->SetWorldTransform(SourcePose);Cargo->SetVisibility(false);Cargo->SetHiddenInGame(true);Cargo->RegisterComponent();
        if(!Context())return false;
        Intake.ExpectedNativeGeometrySha256=NativeSha;Intake.ExpectedPackagePinsSha256=PackagePinsSha;Intake.ExactCargoObjectPath=CargoMeshPath;
        Intake.ExpectedWorldPackageName=World->GetOutermost()->GetName();Intake.CargoTargetComponentName=TEXT("DDSDiag_ExcludedSourceCargo");Intake.ActualCargoPartId=TEXT("OriginalCityBurgerBoxSourceOnlyExcluded");Roles(Intake);
        FDDSOriginalRobotNativeGeometry Geometry;FString Placement;
        if(!FDDSOriginalLiveIntake::Observe(*OwnedPublisher,*Cargo,Intake,Geometry,Placement,Error) || !Context())return false;
        Report->SetStringField(TEXT("actualWorldPlacementRecordJson"),Placement);Report->SetStringField(TEXT("sourcePublisher"),OwnedPublisher->GetPathName());Report->SetStringField(TEXT("sourceOnlyCargoHolder"),OwnedCargoHolder->GetPathName());
        Spawn.bDeferConstruction=true;OwnedCarrier=World->SpawnActor<ADDSCityRollingPawn>(ADDSCityRollingPawn::StaticClass(),SourcePose,Spawn);
        if(!Live(OwnedCarrier.Get())){Error=TEXT("Deferred unpossessed diagnostic rolling carrier allocation failed");return false;}Lifetime.Emplace(OwnedCarrier.Get());
        // Disable auto-possession BEFORE FinishSpawning/PreInitializeComponents.
        OwnedCarrier->AutoPossessPlayer=EAutoReceiveInput::Disabled;OwnedCarrier->AutoPossessAI=EAutoPossessAI::Disabled;OwnedCarrier->AIControllerClass=nullptr;
        OwnedCarrier->AutoReceiveInput=EAutoReceiveInput::Disabled;
        OwnedCarrier->SetActorHiddenInGame(true);OwnedCarrier->SetActorTickEnabled(false);OwnedCarrier->PrimaryActorTick.bStartWithTickEnabled=false;
        TArray<UActorComponent*> CarrierComponents;OwnedCarrier->GetComponents(CarrierComponents);
        for(auto* C:CarrierComponents){C->PrimaryComponentTick.bStartWithTickEnabled=false;C->SetComponentTickEnabled(false);}
        OwnedCarrier->FinishSpawning(SourcePose);OwnedCarrier->SetActorTickEnabled(false);
        auto* Rig=SoleOwnedRig(OwnedCarrier.Get(),World);
        if(!Context() || OwnedCarrier->GetController()!=nullptr || OwnedCarrier->AutoPossessPlayer!=EAutoReceiveInput::Disabled || OwnedCarrier->AutoPossessAI!=EAutoPossessAI::Disabled
           || !Rig || Rig->GetRigState()!=EDDSRobotRigState::Unbound)
        {if(Error.IsEmpty())Error=TEXT("Diagnostic carrier possession or unbound rig context changed");return false;}
        const uint64 Generation=Rig->GetGeneration(),Revision=Rig->GetReadinessRevision();
        Lifetime.Emplace(Rig);
        const auto CarrierStillExact=[&]()
        {
            if(!Context() || !Live(OwnedCarrier.Get()) || OwnedCarrier->IsActorBeingDestroyed() || OwnedCarrier->GetWorld()!=World
               || OwnedCarrier->GetController()!=nullptr || SoleOwnedRig(OwnedCarrier.Get(),World)!=Rig || !Live(Rig) || Rig->GetOwner()!=OwnedCarrier.Get() || Rig->GetWorld()!=World
               || Rig->GetRigState()!=EDDSRobotRigState::Unbound || Rig->GetGeneration()!=Generation || Rig->GetReadinessRevision()!=Revision
               || !Live(OwnedOwner.Get()) || !OwnedOwner->IsRegistered() || OwnedOwner->GetOwner()!=OwnedCarrier.Get() || OwnedOwner->GetWorld()!=World)
            {if(Error.IsEmpty())Error=TEXT("Owned unpossessed carrier/selected owner/rig lifetime or generation/readiness identity revoked");return false;}
            return true;
        };
        Report->SetStringField(TEXT("sourceCarrier"),OwnedCarrier->GetPathName());Report->SetStringField(TEXT("rigGenerationBefore"),LexToString(Generation));Report->SetStringField(TEXT("readinessRevisionBefore"),LexToString(Revision));
        OwnedOwner=NewObject<UDDSOriginalRobotRuntimeOwner>(OwnedCarrier.Get(),NAME_None,RF_Transient);
        if(!Live(OwnedOwner.Get())){Error=TEXT("Closed-lid runtime owner allocation failed");return false;}Lifetime.Emplace(OwnedOwner.Get());OwnedCarrier->AddInstanceComponent(OwnedOwner.Get());OwnedOwner->RegisterComponent();
        if(!CarrierStillExact() || !OwnedOwner->CreateOriginalAssembly(Geometry,Error,EDDSOriginalAssemblyScope::ClosedLidDrive) || !CarrierStillExact())return false;
        TArray<UDDSRobotLeasedMeshComponent*> Managed;OwnedCarrier->GetComponents(Managed);
        if(Geometry.Parts.Num()!=16 || Managed.Num()!=15){Error=TEXT("Observed16 source record versus exact15 created leased robot roster mismatch");return false;}
        for(const auto& Part:Geometry.Parts)if(Part.Role==EDDSOriginalRobotPartRole::Parcel)
        {for(auto* C:Managed)if(C->GetFName()==Part.ComponentName){Error=TEXT("Excluded parcel appeared in ClosedLidDrive ownership");return false;}}
        Report->SetStringField(TEXT("runtimeOwnerDiagnostic"),OwnedOwner->GetOriginalRobotRuntimeDiagnostic());
        FDDSOriginalRobotGameMountDeclarations MountDesign;UDDSRobotLeasedMeshComponent* MountChassis=nullptr;bool bMountsCreated=false;
        const auto MountSourcesStillExact=[&]()
        {
            if(!bMeasurementEnabled)return true;
            if(!bMountsCreated || !CarrierStillExact() || !Live(MountChassis) || !MountChassis->IsRegistered()
               || MountChassis->GetOwner()!=OwnedCarrier.Get() || MountChassis->GetWorld()!=World || !MountChassis->PreparedOriginalChassisCompoundUnchanged())
            {if(Error.IsEmpty())Error=TEXT("Declared GAME mount/chassis owned context revoked");return false;}
            const FName Names[]={TEXT("DDSGameSensorMount"),TEXT("DDSGameChaseMount"),TEXT("DDSGameServiceMount"),TEXT("DDSGameCargoMount")};
            for(int32 I=0;I<4;++I)
            {auto* C=Inputs.MountSources[I].Get();const FTransform Expected=DDSPhysicalCm(MountDesign.InChassisMeters[I])*DDSRigidComponentPose(MountChassis->GetComponentTransform());
             if(!Live(C) || !C->IsRegistered() || C->GetOwner()!=OwnedCarrier.Get() || C->GetOuter()!=OwnedCarrier.Get() || C->GetWorld()!=World
                || C->GetAttachParent()!=MountChassis || C->GetFName()!=Names[I] || !C->GetComponentTransform().Equals(Expected,1.e-6))
             {if(Error.IsEmpty())Error=TEXT("Actual registered GAME mount identity/attachment/frame changed");return false;}}
            return true;
        };
        if(bMeasurementEnabled)
        {
            FName ChassisName=NAME_None;int32 SourceChassisCount=0;
            for(const auto& Part:Geometry.Parts)if(Part.Role==EDDSOriginalRobotPartRole::Body && Part.Index==0){ChassisName=Part.ComponentName;++SourceChassisCount;}
            int32 MatchingChassis=0;for(auto* C:Managed)if(Live(C) && C->GetFName()==ChassisName){MountChassis=C;++MatchingChassis;}
            if(SourceChassisCount!=1 || MatchingChassis!=1 || !CarrierStillExact() || !MountChassis)
            {if(Error.IsEmpty())Error=TEXT("Exact source Body0 to actual managed chassis required for GAME mount derivation");return false;}
            Lifetime.Emplace(MountChassis);auto MountRecord=MakeShared<FJsonObject>();Report->SetObjectField(TEXT("declaredGameMountDesign"),MountRecord);
            if(!DeclareGeometryGameMounts(*MountChassis,MountDesign,MountRecord,Numbers,Error) || !CarrierStillExact())return false;
            bOwnedGameMountLease=true; // Set BEFORE creation: partial returned lease must release before owner generation retires.
            if(!FDDSOriginalRobotMeasurementProducer::CreateDeclaredGameMountSources(*OwnedCarrier,*MountChassis,MountDesign,Inputs,OwnedGameMounts,Error) || !CarrierStillExact())return false;
            bMountsCreated=true;for(const auto& Weak:Inputs.MountSources)Lifetime.Emplace(Weak.Get());
            if(!MountSourcesStillExact())return false;
        }
        if(!FDDSOriginalRobotMeasurementProducer::CaptureDraft(*OwnedCarrier,Geometry,Inputs,Draft,Error) || !CarrierStillExact())return false;
        if(!MountSourcesStillExact())return false;
        if(bMeasurementEnabled)
        {TArray<TSharedPtr<FJsonValue>> Readbacks;
         for(int32 I=0;I<4;++I)
         {auto Row=MakeShared<FJsonObject>();Numbers.Number(Row,TEXT("index"),I);Row->SetBoolField(TEXT("actualRegisteredSourceAvailable"),Draft.bMountSourceAvailable[I]);
          Row->SetStringField(TEXT("actualRegisteredSource"),Draft.MountSourceObjectPaths[I]);Numbers.Pose(Row,TEXT("declaredGameInChassisMeters"),MountDesign.InChassisMeters[I]);
          if(Draft.bMountSourceAvailable[I])Numbers.Pose(Row,TEXT("observedRegisteredInChassisMeters"),Draft.MountsInChassisMeters[I]);else NullFields(Row,{TEXT("observedRegisteredInChassisMeters")});Readbacks.Add(MakeShared<FJsonValueObject>(Row));}
         Report->SetArrayField(TEXT("actualRegisteredGameMountReadbacks"),Readbacks);
         for(int32 I=0;I<4;++I)if(!Draft.bMountSourceAvailable[I] || !Draft.MountsInChassisMeters[I].Equals(MountDesign.InChassisMeters[I],1.e-6))
         {Error=TEXT("Declared GAME mount differs from actual registered CaptureDraft readback; no mount admission");return false;}}
        bDraftCaptured=true;Report->SetObjectField(TEXT("captureDraft"),DraftObservation(Draft,Numbers));
        // Only explicit GAME candidate frames are populated in Recipe, never in
        // Draft.UnadmittedBinding.Joints or its topology/mount/sign flags.
        const FQuat DeclaredAxes=FRotationMatrix::MakeFromXZ(FVector(0,1,0),FVector(0,0,1)).ToQuat();
        TArray<TSharedPtr<FJsonValue>> Frames;
        for(int32 I=0;I<8;++I)
        {const auto& Rest=Draft.UnadmittedBinding.Bodies[I+1].RestInChassisMeters;auto& H=Recipe.JointHypotheses[I];
         H.InPartMeters=FTransform(Rest.GetRotation().Inverse()*DeclaredAxes,FVector::ZeroVector,FVector::OneVector);H.InChassisMeters=H.InPartMeters*Rest;
         H.SuspensionHalfTravelM=I<6?0.02:0.0;H.SuspensionRestTargetM=0.0;H.LidOpenLimitRad=0.0;H.bAdjacentCollisionExclusionJustified=true;
         if(H.bMeasured || !(H.InPartMeters*Rest).Equals(H.InChassisMeters,1.e-6)){Error=TEXT("Explicit GAME joint frames failed actual observed rest meeting; no measured flags assigned");return false;}
         auto Frame=MakeShared<FJsonObject>();Numbers.Number(Frame,TEXT("index"),I);Numbers.Pose(Frame,TEXT("gameInPartMeters"),H.InPartMeters);Numbers.Pose(Frame,TEXT("gameInChassisMeters"),H.InChassisMeters);
         Numbers.Number(Frame,TEXT("gameHalfTravelM"),H.SuspensionHalfTravelM);Frame->SetBoolField(TEXT("measuredAdmission"),H.bMeasured);Frames.Add(MakeShared<FJsonValueObject>(Frame));}
        Report->SetArrayField(TEXT("declaredGameCandidateJointFrames"),Frames);
        Recipe.OriginalSupportSource=SupportMeshComponent;Recipe.FixtureSupportWorldCm=SupportPose;
        Recipe.FixtureChassisWorldCm=DDSRigidComponentPose(Draft.UnadmittedBinding.Bodies[0].ActualPart->GetComponentTransform());
        if(!CarrierStillExact() || !Live(SupportMeshComponent) || !SupportMeshComponent->IsRegistered() || SupportMeshComponent->GetWorld()!=World
           || SupportMeshComponent->GetBodySetup()!=SupportSetup || (SupportSetup && SupportSetup->BodySetupGuid!=SupportGuid) || !SupportMeshComponent->GetComponentTransform().Equals(SupportPose,0)
           || Rig->GetRigState()!=EDDSRobotRigState::Unbound || Rig->GetGeneration()!=Generation || Rig->GetReadinessRevision()!=Revision)
        {if(Error.IsEmpty())Error=TEXT("Actual support/City source carrier/rig generation changed before native diagnostic");return false;}
        auto Captured=MakeShared<FJsonObject>();Captured->SetStringField(TEXT("schema"),TEXT("DDSCity.ActualCityCalibrationCapturedDraft.v1"));Captured->SetStringField(TEXT("nonce"),Output.Nonce);
        Captured->SetObjectField(TEXT("draft"),Report->GetObjectField(TEXT("captureDraft")).ToSharedRef());Captured->SetArrayField(TEXT("gameCandidateJointFrames"),Frames);
        Captured->SetStringField(TEXT("gameHypothesisRecord"),HypothesisRecord);
        if(bMeasurementEnabled)
        {Captured->SetStringField(TEXT("declaredMeasurementGameRecipeRecord"),MeasurementRecord);Captured->SetStringField(TEXT("declaredMeasurementGameRecipeRecordSha256"),MeasurementDigest);
         Captured->SetObjectField(TEXT("declaredGameMountDesign"),Report->GetObjectField(TEXT("declaredGameMountDesign")).ToSharedRef());Captured->SetArrayField(TEXT("actualRegisteredGameMountReadbacks"),Report->GetArrayField(TEXT("actualRegisteredGameMountReadbacks")));
         TArray<TSharedPtr<FJsonValue>> Profiles;for(const auto& B:Draft.Bodies){auto P=MakeShared<FJsonObject>();P->SetBoolField(TEXT("available"),B.bAvailable);if(B.bAvailable)P->SetStringField(TEXT("NativeComponentCollisionProfile"),B.NativeComponentCollisionProfile.ToString());else NullFields(P,{TEXT("NativeComponentCollisionProfile")});Profiles.Add(MakeShared<FJsonValueObject>(P));}Captured->SetArrayField(TEXT("actualSourceBodyCollisionProfiles"),Profiles);}
        if(!MountSourcesStillExact())return false;
        if(!Output.Write(TEXT("draft"),Captured,Error) || !CarrierStillExact())return false;
        if(!MountSourcesStillExact())return false;
        bRunCalled=true;bProducerCompleted=FDDSOriginalRobotMeasurementProducer::RunCalibration(Draft,Recipe,Native,Error);
        Report->SetStringField(TEXT("producerFirstRefusal"),bProducerCompleted?FString():Error);
        Report->SetObjectField(TEXT("producerPredicateDiagnostic"),RecipePredicateReceipt(Native));
        if(bSupportEnabled)
        {
            if(!CarrierStillExact() || !MountSourcesStillExact())return false;
            auto SupportReceipt=SupportAdapterReceipt(Native,Numbers);Report->SetObjectField(TEXT("originalSingleBoxSupportDiagnostic"),SupportReceipt);
            const auto SupportWriteContext=[&]()
            {
                if(!CarrierStillExact() || !MountSourcesStillExact() || !Live(SupportMeshComponent) || !SupportMeshComponent->IsRegistered()
                   || SupportMeshComponent->GetWorld()!=World || SupportMeshComponent->GetBodySetup()!=SupportSetup
                   || (SupportSetup && SupportSetup->BodySetupGuid!=SupportGuid) || !SupportMeshComponent->GetComponentTransform().Equals(SupportPose,0))
                {Error=TEXT("Support source context revoked before/during retained diagnostic writes");return false;}
                return !Native.SupportAdapterObservation.bRawSourceAvailable
                    || FDDSOriginalRobotMeasurementProducer::ConfirmOriginalSupportDiagnosticReceipt(*SupportMeshComponent,Native.SupportAdapterObservation,Error);
            };
            if(!WriteSupportRaw(Output,Native,SupportReceipt,Numbers,SupportWriteContext,Error) || !SupportWriteContext())return false;
        }
        if(!bMeasurementEnabled)Report->SetObjectField(TEXT("nativeCalibrationDiagnostic"),NativeObservation(Native,Numbers));
        if(bMeasurementEnabled)
        {
            if(!MountSourcesStillExact())return false;
            auto Increment=MeasurementSummary(Native,Numbers);Report->SetObjectField(TEXT("measurementIncrementDiagnostic"),Increment);
            // Save actual partial/refused native data as well as completed runs.
            const auto RawStillExact=[&](){return CarrierStillExact() && MountSourcesStillExact();};
            if(!WriteMeasurementRaw(Output,Draft,Native,Increment,Numbers,RawStillExact,Error) || !RawStillExact())return false;
        }
        if(!CarrierStillExact())return false;
        Report->SetStringField(TEXT("rigGenerationAfter"),LexToString(Rig->GetGeneration()));Report->SetStringField(TEXT("readinessRevisionAfter"),LexToString(Rig->GetReadinessRevision()));
        if(!Context())return false;
        if(Rig->GetRigState()!=EDDSRobotRigState::Unbound || Rig->GetGeneration()!=Generation || Rig->GetReadinessRevision()!=Revision || OwnedCarrier->GetController()!=nullptr)
        {Error=TEXT("Diagnostic attempted production rig/possession transition; no success claim");return false;}
        if(!Numbers.bFinite){Error=TEXT("Nonfinite raw fields observed; JSON uses null, no diagnostic numeric acceptance");return false;}
        return bProducerCompleted;
    };
    const bool Completed=Work();
    Report->SetBoolField(TEXT("draftCaptured"),bDraftCaptured);Report->SetBoolField(TEXT("runCalibrationCalled"),bRunCalled);
    Report->SetBoolField(TEXT("producerBoundedDiagnosticCompleted"),bProducerCompleted);
    Report->SetBoolField(TEXT("finiteNumericFields"),Numbers.bFinite);Report->SetStringField(TEXT("firstRefusal"),Completed?FString():Error);
    bRunning=false;FString Cleanup;const bool Released=ReleaseOwned(Cleanup);bRunning=true;
    Report->SetBoolField(TEXT("ownedCleanupCompleted"),Released);Report->SetStringField(TEXT("ownedCleanupRefusal"),Cleanup);
    Numbers.Number(Report,TEXT("actualCallerElapsedRealS"),FPlatformTime::Seconds()-Begin);
    const bool InBudget=FPlatformTime::Seconds()<Deadline;Report->SetBoolField(TEXT("callerWithin5sBeforeFinalWrite"),InBudget);
    Report->SetBoolField(TEXT("callerWorkAndCleanupCompletedBeforeFinalWrite"),Completed && InBudget && Released);
    Report->SetStringField(TEXT("outputAcceptance"),TEXT("Candidate JSON; root must confirm fresh nonce END marker and actual wall watchdog/file guard. A file cannot observe elapsed time after its own final write."));
    if(!InBudget && Completed)Report->SetStringField(TEXT("firstRefusal"),TEXT("Caller exceeded5s budget; raw producer completion is not caller acceptance"));
    else if(!Released && Completed)Report->SetStringField(TEXT("firstRefusal"),Cleanup);
    FString OutputError;const bool Written=Output.Write(TEXT("result"),Report,OutputError);
    if(!Written){UE_LOG(LogDDSCityCalibrationDiagnostic,Warning,TEXT("CITY_CALIBRATION_OUTPUT_REFUSED nonce=%s reason=%s; preserved prior evidence"),*Output.Nonce,*OutputError);}
    const bool EndInBudget=FPlatformTime::Seconds()<Deadline;
    UE_LOG(LogDDSCityCalibrationDiagnostic,Display,TEXT("CITY_CALIBRATION_DIAGNOSTIC_END nonce=%s callerCompleted=%d producerCompleted=%d cleanup=%d elapsed=%.6fs firstRefusal=%s; NO_BINDING_NO_POSSESSION_NO_READINESS"),
        *Output.Nonce,int32(Completed && EndInBudget && Released && Written),int32(bProducerCompleted),int32(Released),FPlatformTime::Seconds()-Begin,*Report->GetStringField(TEXT("firstRefusal")));
}
