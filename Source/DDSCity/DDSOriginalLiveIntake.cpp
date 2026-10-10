#include "DDSOriginalLiveIntake.h"
#include "DDSStaticMeshNaniteObservation.h"
#include "DDSOriginalMaterialReceipt.h"
#include "DDSCityRollingPawn.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
namespace DDSLiveIntake
{
constexpr TCHAR Ledger[]=TEXT("88178075AEEC58333E73C576A9ABFDF3411A209E3BAB7B7ACD0445D091BEA7DA");
constexpr TCHAR PublisherClass[]=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1_C");
constexpr TCHAR MeshRoot[]=TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/");
struct FIntakeOriginalPackagePin { const TCHAR* Object; const TCHAR* Extension; const TCHAR* Sha; };
static const FIntakeOriginalPackagePin IntakeOriginal25[]={
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1"),TEXT(".uasset"),TEXT("89C958CC7DBC4922B2B26B126598DC2962600B6C5A1515959EB31AECF1B9083E")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Level/LV_DeliveryRobot_v1_Showcase.LV_DeliveryRobot_v1_Showcase"),TEXT(".umap"),TEXT("9F5911BEB8CBB40F5BC8C145BF492ADBCCAB1BE3D67F459F28C4937791DE84B2")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Level/LV_DeliveryRobot_v1_Showcase_BuiltData.LV_DeliveryRobot_v1_Showcase_BuiltData"),TEXT(".uasset"),TEXT("43029440C12FA861D05B5B03D53CE53C8C7AE3B263D2578FD884A7298BC38593")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Material/MI_DeliveryRobot_v1.MI_DeliveryRobot_v1"),TEXT(".uasset"),TEXT("75BA9CF93630AE1C9BE917788EA274D8E4CB5377F58485317267C87BA5D0FD38")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Material/MI_DeliveryRobot_v1_Dirty.MI_DeliveryRobot_v1_Dirty"),TEXT(".uasset"),TEXT("3E8F45D8F931874E4B2851F51B95201F7F4C267B8091D5C1DF0FC51586875952")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_Delivery_Robot_v1.SM_Delivery_Robot_v1"),TEXT(".uasset"),TEXT("6EA4240B2585C960D018964929E7A780754E990E494F51351CB972433CF04238")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_AllWheels.SM_DeliveryRobot_AllWheels"),TEXT(".uasset"),TEXT("B05699854DBF1805F4BB7097CD8637E6BD9FFAF8BB73D4587BBCAAD00012AFFE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Base.SM_DeliveryRobot_Base"),TEXT(".uasset"),TEXT("1A0E9EB0BAC3FFBE8B9E44537A27B52B63CFF83A3020B6DFB0654717BA514F93")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Door1.SM_DeliveryRobot_Door1"),TEXT(".uasset"),TEXT("D6542BF720540310BEB224C6B0CC185A56B0CC2B4D2CA23F44BD4DEFC4EFD91E")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Door2.SM_DeliveryRobot_Door2"),TEXT(".uasset"),TEXT("025A9465A670202DD513375BFBA5F5067F5DE336C5CBA0F841B2DC5DBE593E60")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Opened.SM_DeliveryRobot_Opened"),TEXT(".uasset"),TEXT("99D5F9E1697DA16FDFB4EA5F65B9A942E3DA21DB8CCC16220840B8E8F7724961")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Suspension.SM_DeliveryRobot_Suspension"),TEXT(".uasset"),TEXT("4158C0293B9AC62DAA65B8170CEE153C7C70B59C97FB2B3D77F832D6181474DE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_v1.SM_DeliveryRobot_v1"),TEXT(".uasset"),TEXT("3B2CF354AF948F04789666AC3815BE95C3613421808571A212B4276C433C1161")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Wheel.SM_DeliveryRobot_Wheel"),TEXT(".uasset"),TEXT("0B85380F58988A7471851BD224BFDD652A9EB4B82774E5596AE13D5081879047")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_B.T_Delivery_Robot_v1_B"),TEXT(".uasset"),TEXT("4D70A549948D74ABB647E8985C803098511C04ED5C24F5376338B19F61A7E282")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_E.T_Delivery_Robot_v1_E"),TEXT(".uasset"),TEXT("EC365A3334A6C755396C351A9C286473BDF5AECC7E5D89281F19E3F5E639CDF5")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_M.T_Delivery_Robot_v1_M"),TEXT(".uasset"),TEXT("4F3656A79A77CC97794FF833B6C48EA82E512905B3858EF8CFB3514407EB1C3B")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_N.T_Delivery_Robot_v1_N"),TEXT(".uasset"),TEXT("E3B53598862FF51087963E01BC45ED9159A1E2D59AC6BC9391EE005C2A1AFBAE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_ORM.T_Delivery_Robot_v1_ORM"),TEXT(".uasset"),TEXT("12AE9F9CD0F56D90AB2E4BD9B83711491B14885FE8451473C30A485DF6AB0060")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/MM_BLS_MasterGeneral.MM_BLS_MasterGeneral"),TEXT(".uasset"),TEXT("8C2F06EFB43AC0376E88918215C9DB9019DCC0BCF7691E9DAB5F1AA39762A836")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_Black.T_Base_Black"),TEXT(".uasset"),TEXT("360E223E5FF6024D321DF8B474E5C32DD03BFCBD3125B975B241D42D5492DD4F")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_ORM.T_Base_ORM"),TEXT(".uasset"),TEXT("E95C7BB081807FBA647898157A5960E33073F8AC6E972F891FA51862B342EC11")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_White.T_Base_White"),TEXT(".uasset"),TEXT("1AF9B4125B8290065EE2E19439C1293B860280294767C80443442AE2586928BE")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_White_Linear.T_Base_White_Linear"),TEXT(".uasset"),TEXT("F883857F62C4D9F77EB09EB32C12295323B81BB92235EC0C565FA2E16DA030A6")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_FlatNormal.T_FlatNormal"),TEXT(".uasset"),TEXT("2CBE18DB5D003EA1A9530F57441B7590BD4A439CE5E98EC28082FD48D96D4FA1")},
};
bool IntakeValidSha(const FString& V) { if(V.Len()!=64)return false;for(TCHAR C:V)if(!FChar::IsHexDigit(C))return false;return true; }
bool IntakeLoaded(const UObject* V) { return IsValid(V) && !V->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects|RF_BeginDestroyed|RF_FinishDestroyed); }
bool IntakeBoundedJson(const TArray<uint8>& B)
{
    if(B.Num()==0 || B.Num()>1024*1024)return false;int32 Depth=0,Tokens=0;bool Quoted=false,Escaped=false;
    for(uint8 C:B) { if(Quoted) { if(Escaped)Escaped=false;else if(C==92)Escaped=true;else if(C==34)Quoted=false;else if(C<32)return false; }
      else if(C==34)Quoted=true;else if(C==123 || C==91) { if(++Depth>32 || ++Tokens>16384)return false; } else if(C==125 || C==93) { if(--Depth<0)return false; } }
    return !Quoted && Depth==0;
}
bool IntakeString(const TSharedPtr<FJsonObject>& J,const TCHAR* K,FString& V) { return J && J->TryGetStringField(K,V) && !V.IsEmpty() && V.Len()<=2048; }
bool Hash(const TArray<uint8>& Bytes,FString& Out)
{
    Out.Reset(); if (Bytes.Num()==0) return false;
    void* Dll=FPlatformProcess::GetDllHandle(TEXT("bcrypt.dll")); if(!Dll) return false;
    const auto Open=reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptOpenAlgorithmProvider")));
    using FIntakeBCryptHash = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE,PUCHAR,ULONG,PUCHAR,ULONG,PUCHAR,ULONG);
    const auto Digest=reinterpret_cast<FIntakeBCryptHash>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptHash")));
    const auto Close=reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(FPlatformProcess::GetDllExport(Dll,TEXT("BCryptCloseAlgorithmProvider")));
    TArray<uint8> Mutable=Bytes; BCRYPT_ALG_HANDLE Algorithm=nullptr; uint8 Raw[32]={}; bool Good=false;
    if(Open && Digest && Close && Open(&Algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0)
    { Good=Digest(Algorithm,nullptr,0,Mutable.GetData(),Bytes.Num(),Raw,32)>=0; Close(Algorithm,0); }
    FPlatformProcess::FreeDllHandle(Dll);
    if(Good) for(uint8 B:Raw) { Out.AppendChar(TEXT("0123456789ABCDEF")[B>>4]); Out.AppendChar(TEXT("0123456789ABCDEF")[B&15]); }
    return Good;
}
bool JsonBytes(const TArray<uint8>& Bytes,const FString& Expected,TSharedPtr<FJsonObject>& Out)
{
    FString H; if(!IntakeValidSha(Expected) || !IntakeBoundedJson(Bytes) || !Hash(Bytes,H) || H!=Expected) return false;
    FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Utf8.Length(),Utf8.Get())),Out) && Out.IsValid();
}
bool Serialize(const TSharedRef<FJsonObject>& J,FString& Text,FString& Digest)
{
    Text.Reset(); auto W=TJsonWriterFactory<>::Create(&Text); if(!FJsonSerializer::Serialize(J,W)) return false;
    FTCHARToUTF8 U(*Text); TArray<uint8> Bytes; Bytes.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length()); return Hash(Bytes,Digest);
}
bool Pose(const FTransform& T)
{
    const auto S=T.GetScale3D();return !T.ContainsNaN() && T.GetRotation().IsNormalized()
        && FMath::IsFinite(S.X) && FMath::IsFinite(S.Y) && FMath::IsFinite(S.Z) && S.GetMin()>0
        && FMath::IsNearlyEqual(S.X,S.Y,1.e-6) && FMath::IsNearlyEqual(S.X,S.Z,1.e-6);
}
int32 Slot(const FDDSOriginalLiveRole& R)
{
    if(R.Role==EDDSOriginalRobotPartRole::Body && R.Index>=0 && R.Index<9) return R.Index;
    if(R.Role==EDDSOriginalRobotPartRole::Suspension && R.Index>=0 && R.Index<6) return R.Index+9;
    return INDEX_NONE;
}
TSharedRef<FJsonObject> Placement(const FDDSOriginalRobotNativePart& P)
{
    auto J=MakeShared<FJsonObject>(); J->SetStringField(TEXT("source"),P.ExactVisualSourceObjectPath);J->SetStringField(TEXT("mesh"),P.ExactMeshObjectPath);
    J->SetNumberField(TEXT("role"),int32(P.Role));J->SetNumberField(TEXT("index"),P.Index);J->SetStringField(TEXT("partId"),P.SuppliedPartId);
    const auto T=P.MeasuredWorldCm;TArray<TSharedPtr<FJsonValue>> V;
    for(double N:{T.GetTranslation().X,T.GetTranslation().Y,T.GetTranslation().Z,T.GetRotation().X,T.GetRotation().Y,T.GetRotation().Z,T.GetRotation().W,T.GetScale3D().X,T.GetScale3D().Y,T.GetScale3D().Z}) V.Add(MakeShared<FJsonValueNumber>(N));
    J->SetArrayField(TEXT("actualWorldCmQuaternionScale"),V);TArray<TSharedPtr<FJsonValue>> M;for(const auto& X:P.NativeObservedMaterialObjectPaths) M.Add(MakeShared<FJsonValueString>(X));J->SetArrayField(TEXT("actualEffectiveMaterialPaths"),M);
    const auto Paths=[&](const TCHAR* Name,const TArray<FString>& Values) { TArray<TSharedPtr<FJsonValue>> A;for(const auto& Value:Values)A.Add(MakeShared<FJsonValueString>(Value));J->SetArrayField(Name,A); };
    Paths(TEXT("actualDefaultMaterialPaths"),P.NativeObservedDefaultMaterialObjectPaths);
    Paths(TEXT("actualRawOverrideMaterialPaths"),P.NativeObservedRawOverrideMaterialObjectPaths);
    Paths(TEXT("actualSelectedRawMaterialPaths"),P.NativeObservedSelectedRawMaterialObjectPaths);
    if(P.OriginalMaterialReceipt)
    {
        const auto& Receipt=*P.OriginalMaterialReceipt;
        J->SetStringField(TEXT("naniteObservationBasis"), Receipt.NaniteObservationBasis);
        J->SetBoolField(TEXT("naniteObservationKnown"), Receipt.bNaniteObservationKnown);
        J->SetBoolField(TEXT("authoredNaniteEnabledKnown"), Receipt.bAuthoredNaniteEnabledKnown);
        if (Receipt.bAuthoredNaniteEnabledKnown) J->SetBoolField(TEXT("originalNaniteEnabled"), Receipt.bNaniteEnabled);
        else if (Receipt.bNaniteObservationKnown) J->SetBoolField(TEXT("observedCookedNaniteDataValid"), Receipt.bNaniteEnabled);
        J->SetBoolField(TEXT("useNaniteOverrideMaterials"),Receipt.bUseNaniteOverrides);
        auto Policy=MakeShared<FJsonObject>();
        Policy->SetBoolField(TEXT("disallowNanite"),Receipt.bDisallowNanite);
        Policy->SetBoolField(TEXT("forceDisableNanite"),Receipt.bForceDisableNanite);
        Policy->SetBoolField(TEXT("forceNaniteForMasked"),Receipt.bForceNaniteForMasked);
        Policy->SetBoolField(TEXT("displayNaniteFallbackMesh"),Receipt.bDisplayNaniteFallbackMesh);
        Policy->SetBoolField(TEXT("worldAllowsMasked"),Receipt.bWorldAllowsMasked);
        Policy->SetNumberField(TEXT("worldFeatureLevel"),Receipt.WorldFeatureLevel);
        Policy->SetNumberField(TEXT("settledNanitePageCount"),Receipt.NanitePageCount);
        J->SetObjectField(TEXT("originalNaniteComponentPolicy"),Policy);
        const auto CachedPath=[&](UMaterialInterface* Material)
        {
            for(const auto& Node:Receipt.Chains)if(Node.Material.Object==Material)return Node.Material.Path;
            return FString();
        };
        TArray<TSharedPtr<FJsonValue>> Chains;
        for(const auto& Node:Receipt.Chains)
        {
            auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("material"),Node.Material.Path);
            Row->SetStringField(TEXT("parent"),CachedPath(Node.Parent));
            Row->SetStringField(TEXT("naniteOverride"),CachedPath(Node.NaniteOverride));
            Row->SetBoolField(TEXT("isInstance"),Node.bInstance);Row->SetBoolField(TEXT("enableOverride"),Node.bEnableOverride);
            Chains.Add(MakeShared<FJsonValueObject>(Row));
        }
        J->SetArrayField(TEXT("cachedOriginalMaterialChain"),Chains);
    }
    return J;
}
struct FSourcePin
{
    UStaticMeshComponent* C=nullptr; UStaticMesh* Mesh=nullptr; AActor* Owner=nullptr;
    FTransform Pose; FName Name; FString SourcePath,ArchetypePath;
    TSharedPtr<FDDSOriginalMaterialReceipt> Material;
};
bool IntakeCoherentSource(const FSourcePin& Pin,AActor& Publisher,UWorld* World,bool Original)
{
    return Pin.Material && Pin.Material->Unchanged() && Pin.Material->Component==Pin.C
        && Pin.Material->Mesh==Pin.Mesh && Pin.Material->Owner==Pin.Owner
        && Pin.Material->World==World && Pin.Material->bRegistered
        && (!Original || Pin.Owner==&Publisher);
}
}
namespace DDSLiveIntake
{
// Private caller-owned lifetime collection remains alive through factory callbacks.
bool ObserveWithLifetimeLease(AActor& Publisher,UStaticMeshComponent& Cargo,const FDDSOriginalLiveIntakeInputs& Supplied,
    FDDSOriginalRobotNativeGeometry& Out,FString& PlacementJson,FString& Error,TArray<TStrongObjectPtr<UObject>>& RetainedSources)
{
    using namespace DDSLiveIntake;Out={};PlacementJson.Reset();Error.Reset(); const auto I=Supplied;
    const auto Refuse=[&](const TCHAR* Why){Error=Why;Out={};PlacementJson.Reset();return false;};
    if(!IsInGameThread() || !IsValid(&Publisher) || Publisher.GetClass()->GetPathName()!=PublisherClass
        || !Publisher.GetWorld() || Publisher.GetWorld()->GetOutermost()->GetName()!=I.ExpectedWorldPackageName
        || !IsValid(&Cargo) || Cargo.GetWorld()!=Publisher.GetWorld() || !Cargo.IsRegistered() || !IsValid(Cargo.GetOwner())
        || I.Original15Roles.Num()!=15 || I.CargoTargetComponentName.IsNone() || I.ActualCargoPartId.IsEmpty()) return Refuse(TEXT("Exact live publisher/world/cargo/explicit15 role input required"));
    static TSet<AActor*> ActiveMaterialIntakes;
    if(ActiveMaterialIntakes.Contains(&Publisher))return Refuse(TEXT("Reentrant original material intake refused"));
    ActiveMaterialIntakes.Add(&Publisher);ON_SCOPE_EXIT { ActiveMaterialIntakes.Remove(&Publisher); };
    TStrongObjectPtr<AActor> PublisherPin(&Publisher);TStrongObjectPtr<UStaticMeshComponent> CargoPin(&Cargo);UWorld* const ObservedWorld=Publisher.GetWorld();TStrongObjectPtr<UWorld> WorldPin(ObservedWorld);
    FString LedgerDigest;TSharedPtr<FJsonObject> Native,Integrity;
    if(I.OriginalIntegrityLedgerBytes.Num()>1024*1024 || !Hash(I.OriginalIntegrityLedgerBytes,LedgerDigest) || LedgerDigest!=Ledger
        || !JsonBytes(I.NativeGeometryJsonBytes,I.ExpectedNativeGeometrySha256,Native)
        || !JsonBytes(I.PackagePinsJsonBytes,I.ExpectedPackagePinsSha256,Integrity)) return Refuse(TEXT("Actual original ledger/native/package-record SHA256 mismatch"));
    FString Schema, OriginalDigest;bool GraphValid=false;
    if(!Native->TryGetStringField(TEXT("schema"),Schema) || (Schema!=TEXT("DDSCity.N05.RobotNativeProbe.v3") && Schema!=TEXT("DDSCity.N05.RobotNativeProbe.v4"))
        || !Native->TryGetBoolField(TEXT("originalLocalSCSGraphValidated"),GraphValid) || !GraphValid
        || !Integrity->TryGetStringField(TEXT("schema"),Schema) || Schema!=TEXT("DDSCity.OriginalLivePackagePins.v1")
        || !Integrity->TryGetStringField(TEXT("originalLedgerSha256"),OriginalDigest) || OriginalDigest!=Ledger) return Refuse(TEXT("Validated native graph and exact original package record schema required"));
    const TArray<TSharedPtr<FJsonValue>>* Packages=nullptr;
    if(!Integrity->TryGetArrayField(TEXT("packages"),Packages) || !Packages || Packages->Num()!=26) return Refuse(TEXT("Exact25 Bendylab packages plus explicit original City cargo pin required"));
    TMap<FString,FString> PackageHashes;int32 OriginalCount=0;int64 Total=0;
    for(const auto& V:*Packages)
    {
        TSharedPtr<FJsonObject> J=(V.IsValid() && V->Type==EJson::Object)?V->AsObject():nullptr;FString Object,Expected,Extension,Filename,Digest;
        if(!J || !IntakeString(J,TEXT("objectPath"),Object) || !FPackageName::IsValidObjectPath(Object) || !IntakeString(J,TEXT("packageSha256"),Expected) || !IntakeValidSha(Expected) || !IntakeString(J,TEXT("extension"),Extension) || PackageHashes.Contains(Object)
            || (!Object.StartsWith(TEXT("/Game/BendylabStudios/")) && Object!=I.ExactCargoObjectPath)
            || (Extension!=TEXT(".uasset") && Extension!=TEXT(".umap"))) return Refuse(TEXT("Original package pin/path invalid"));
        const FIntakeOriginalPackagePin* Exact=nullptr;for(const auto& R:IntakeOriginal25)if(Object==R.Object)Exact=&R;
        if((Object.StartsWith(TEXT("/Game/BendylabStudios/")) && (!Exact || Expected!=Exact->Sha || Extension!=Exact->Extension)) || (Object==I.ExactCargoObjectPath && Extension!=TEXT(".uasset"))
           || !FPackageName::TryConvertLongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Object),Filename,Extension))return Refuse(TEXT("Exact original25 package roster/extension/hash mismatch"));
        Filename=FPaths::ConvertRelativePathToFull(Filename);if(!FPaths::CollapseRelativeDirectories(Filename))return Refuse(TEXT("Invalid canonical package path"));
        if(!Filename.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))) return Refuse(TEXT("Package is not in original project Content"));
        const int64 Size=IFileManager::Get().FileSize(*Filename);Total+=Size;
        TArray<uint8> Bytes;if(Size<=0 || Size>128*1024*1024 || Total>512*1024*1024 || !FFileHelper::LoadFileToArray(Bytes,*Filename) || Bytes.Num()!=Size || !Hash(Bytes,Digest) || Digest!=Expected) return Refuse(TEXT("Actual package bytes/hash or finite input budget mismatch"));
        PackageHashes.Add(Object,Digest);if(Object.StartsWith(TEXT("/Game/BendylabStudios/"))) ++OriginalCount;
    }
    if(OriginalCount!=25 || !I.ExactCargoObjectPath.StartsWith(TEXT("/Game/")) || I.ExactCargoObjectPath.StartsWith(TEXT("/Game/BendylabStudios/"))) return Refuse(TEXT("Original25 and distinct original City cargo identity required"));
    const TSharedPtr<FJsonObject>* Graph=nullptr;const TArray<TSharedPtr<FJsonValue>>* Nodes=nullptr;
    if(!Native->TryGetObjectField(TEXT("originalLocalSCSGeometry"),Graph) || !Graph || !Graph->IsValid() || !(*Graph)->TryGetArrayField(TEXT("nodes"),Nodes) || !Nodes || Nodes->Num()>256) return Refuse(TEXT("Native exact SCS identity map absent"));
    TMap<FString,TSharedPtr<FJsonObject>> NativeNodes;TSet<FString> NativeVariables,NativeTemplates;int32 NativeMeshes=0;
    for(const auto& V:*Nodes) { TSharedPtr<FJsonObject> J=(V.IsValid() && V->Type==EJson::Object)?V->AsObject():nullptr;FString N,Variable,Template,Mesh;
      if(!IntakeString(J,TEXT("nodeObjectPath"),N) || !IntakeString(J,TEXT("variableName"),Variable) || !IntakeString(J,TEXT("componentTemplateObjectPath"),Template)
         || NativeNodes.Contains(N) || NativeVariables.Contains(Variable) || NativeTemplates.Contains(Template))return Refuse(TEXT("Invalid/duplicate bounded native SCS row"));
      if(J->HasField(TEXT("exactMeshObjectPath"))) { if(!IntakeString(J,TEXT("exactMeshObjectPath"),Mesh) || !Mesh.StartsWith(MeshRoot))return Refuse(TEXT("Exact original native mesh row required"));++NativeMeshes; }
      NativeNodes.Add(N,J);NativeVariables.Add(Variable);NativeTemplates.Add(Template);
    }
    if(NativeNodes.Num()!=16 || NativeMeshes!=15)return Refuse(TEXT("Exact native original16 graph/15 mesh rows required"));
    TArray<UStaticMeshComponent*> Live;Publisher.GetComponents(Live);if(Live.Num()!=15) return Refuse(TEXT("Publisher must expose exact15 original mesh components"));
    TSet<int32> Slots;TSet<FName> Names;TSet<FString> PartIds;TSet<UStaticMeshComponent*> Used;
    FDDSOriginalRobotNativeGeometry Built;Built.IntegrityLedgerSha256=LedgerDigest;Built.NativeGeometryRecordSha256=I.ExpectedNativeGeometrySha256;
    TArray<DDSLiveIntake::FSourcePin> Pins;TArray<TStrongObjectPtr<UObject>>& Strong=RetainedSources;
    for(int32 At=0;At<16;++At)
    {
        FDDSOriginalRobotNativePart P;TSharedPtr<FJsonObject> Node;UStaticMeshComponent* C=&Cargo;
        if(At<15)
        {
            const auto& R=I.Original15Roles[At];const int32 S=Slot(R);
            if(S==INDEX_NONE || Slots.Contains(S)) return Refuse(TEXT("Duplicate/invalid explicit original role index"));Slots.Add(S);
            if(const auto* Found=NativeNodes.Find(R.NativeNodeObjectPath))Node=*Found;
            if(!Node) return Refuse(TEXT("Role node not in actual native SCS record"));
            FString Variable,TemplatePath,NativeMesh;if(!IntakeString(Node,TEXT("variableName"),Variable) || !IntakeString(Node,TEXT("componentTemplateObjectPath"),TemplatePath) || !IntakeString(Node,TEXT("exactMeshObjectPath"),NativeMesh))return Refuse(TEXT("Mesh role node fields missing"));
            const FString Kind=R.Role==EDDSOriginalRobotPartRole::Suspension?TEXT("SM_DeliveryRobot_Suspension"):(R.Index==0?TEXT("SM_DeliveryRobot_Base"):(R.Index<=6?TEXT("SM_DeliveryRobot_Wheel"):TEXT("")));
            if((!Kind.IsEmpty() && NativeMesh!=FString(MeshRoot)+Kind+TEXT(".")+Kind) || (Kind.IsEmpty() && NativeMesh!=FString(MeshRoot)+TEXT("SM_DeliveryRobot_Door1.SM_DeliveryRobot_Door1") && NativeMesh!=FString(MeshRoot)+TEXT("SM_DeliveryRobot_Door2.SM_DeliveryRobot_Door2")))return Refuse(TEXT("Explicit role does not match original native mesh category"));C=nullptr;
            for(auto* Candidate:Live) if(Candidate && Candidate->GetFName()==FName(*Variable)) { if(C) return Refuse(TEXT("Duplicate live publisher variable identity"));C=Candidate; }
            if(!IntakeLoaded(C) || !IntakeLoaded(C->GetArchetype()) || C->GetArchetype()->GetPathName()!=TemplatePath) return Refuse(TEXT("Live component does not match exact original native SCS archetype"));
            P.Role=R.Role;P.Index=R.Index;P.ComponentName=R.TargetComponentName;P.SuppliedPartId=R.SuppliedPartId;
            FString RowJson;if(!Serialize(Node.ToSharedRef(),RowJson,P.NativeTemplateRecordSha256)) return Refuse(TEXT("Actual native template record hash failed"));
        }
        else { P.Role=EDDSOriginalRobotPartRole::Parcel;P.Index=0;P.ComponentName=I.CargoTargetComponentName;P.SuppliedPartId=I.ActualCargoPartId;P.NativeTemplateRecordSha256=I.ExpectedPackagePinsSha256; }
        if(!IsValid(C) || !C->IsRegistered() || C->GetWorld()!=Publisher.GetWorld() || Used.Contains(C)
            || P.ComponentName.IsNone() || Names.Contains(P.ComponentName) || P.SuppliedPartId.TrimStartAndEnd().IsEmpty() || P.SuppliedPartId.Len()>2048 || PartIds.Contains(P.SuppliedPartId)) return Refuse(TEXT("Duplicate/unregistered source part identity"));
        UStaticMesh* Mesh=C->GetStaticMesh().Get();if(!IsValid(Mesh) || Mesh->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects)) return Refuse(TEXT("Loaded original source mesh unavailable"));
        const FString Path=Mesh->GetPathName();if((At<15 && (C->GetOwner()!=&Publisher || Path!=Node->Values.FindChecked(TEXT("exactMeshObjectPath"))->AsString() || !Path.StartsWith(MeshRoot))) || (At==15 && Path!=I.ExactCargoObjectPath) || !PackageHashes.Contains(Path)) return Refuse(TEXT("Original source owner/mesh/package identity mismatch"));
        P.SuppliedMesh=Mesh;P.OriginalVisualSource=C;P.ExactMeshObjectPath=Path;P.MeshPackageSha256=PackageHashes.FindChecked(Path);P.ExactVisualSourceObjectPath=C->GetPathName();
        FSourcePin Pin;Pin.C=C;Pin.Mesh=Mesh;Pin.Owner=C->GetOwner();Pin.Name=C->GetFName();Pin.SourcePath=C->GetPathName();Pin.ArchetypePath=C->GetArchetype()->GetPathName();Pin.Pose=C->GetComponentTransform();
        // Preserve finite pose, loading and compiling refusals. Nanite enablement
        // is source state to retain, not a categorical material admission ban.
        if(!Pose(Pin.Pose) || Mesh->IsCompiling())
            return Refuse(*FString::Printf(TEXT("Finite authored source scale and settled material source required: PoseValid=%d IsCompiling=%d NaniteObservationKnown=%d NaniteValue=%d NaniteBasis=%s component=%s mesh=%s"),
                int32(Pose(Pin.Pose)),int32(Mesh->IsCompiling()),int32(DDSStaticMeshNanite::ObservationKnown(*Mesh)),
                int32(DDSStaticMeshNanite::SourceEnabledOrCookedData(*Mesh)),DDSStaticMeshNanite::ObservationBasis(),*Pin.SourcePath,*Path));
        if(!FDDSOriginalMaterialObservation::CaptureRaw(*C,Pin.Material,Error))return Refuse(*Error);
        Strong.Emplace(C);Strong.Emplace(Mesh);Strong.Emplace(Pin.Owner);Used.Add(C);Names.Add(P.ComponentName);PartIds.Add(P.SuppliedPartId);Built.Parts.Add(P);Pins.Add(Pin);
    }
    // All16 component/owner/world/mesh/raw assignments and finite Parent/Nanite
    // graphs have been retained BEFORE the first policy or material resolver.
    const auto AllSourcesUnchanged=[&]()
    {
        if(!IntakeLoaded(&Publisher) || Publisher.IsActorBeingDestroyed() || Publisher.GetWorld()!=ObservedWorld
           || Publisher.GetClass()->GetPathName()!=PublisherClass || !IntakeLoaded(ObservedWorld)
           || ObservedWorld->bIsTearingDown || ObservedWorld->GetOutermost()->GetName()!=I.ExpectedWorldPackageName
           || Pins.Num()!=16 || Built.Parts.Num()!=16)return false;
        for(int32 X=0;X<16;++X)if(!IntakeCoherentSource(Pins[X],Publisher,ObservedWorld,X<15))return false;
        return true;
    };
    for(int32 X=0;X<16;++X)
    {
        auto& P=Built.Parts[X];auto& Pin=Pins[X];
        if(!FDDSOriginalMaterialObservation::Resolve(*Pin.Material,AllSourcesUnchanged,Error))return Refuse(*Error);
        P.OriginalMaterialReceipt=Pin.Material;
        for(const auto& M:Pin.Material->Defaults)P.NativeObservedDefaultMaterialObjectPaths.Add(M.Path);
        for(const auto& M:Pin.Material->RawOverrides)P.NativeObservedRawOverrideMaterialObjectPaths.Add(M.Path);
        for(const auto& M:Pin.Material->SelectedRaw)P.NativeObservedSelectedRawMaterialObjectPaths.Add(M.Path);
        for(const auto& M:Pin.Material->Effective)P.NativeObservedMaterialObjectPaths.Add(M.Path);
        P.MeasuredWorldCm=Pin.Pose;
    }
    auto Record=MakeShared<FJsonObject>();Record->SetStringField(TEXT("schema"),TEXT("DDSCity.OriginalLiveWorldPlacement.v1"));Record->SetStringField(TEXT("publisherActor"),Publisher.GetPathName());Record->SetStringField(TEXT("worldPackage"),I.ExpectedWorldPackageName);Record->SetStringField(TEXT("nativeGeometrySha256"),I.ExpectedNativeGeometrySha256);Record->SetStringField(TEXT("packagePinRecordSha256"),I.ExpectedPackagePinsSha256);Record->SetStringField(TEXT("utc"),FDateTime::UtcNow().ToIso8601());TArray<TSharedPtr<FJsonValue>> Rows;for(const auto& P:Built.Parts)Rows.Add(MakeShared<FJsonValueObject>(Placement(P)));Record->SetArrayField(TEXT("parts"),Rows);
    if(!Serialize(Record,PlacementJson,Built.WorldPlacementRecordSha256)) return Refuse(TEXT("Actual world-placement record serialization/hash failed"));
    // Callback-free coherent source reread; never resolve GetMaterial here.
    if(!IsValid(&Publisher) || Publisher.GetClass()->GetPathName()!=PublisherClass || !Publisher.GetWorld() || Publisher.GetWorld()->GetOutermost()->GetName()!=I.ExpectedWorldPackageName) return Refuse(TEXT("Publisher lifetime/world changed"));
    if(Publisher.GetWorld()!=ObservedWorld || !IntakeLoaded(ObservedWorld) || ObservedWorld->bIsTearingDown || Publisher.IsActorBeingDestroyed())return Refuse(TEXT("Original observed world is ending/changed"));
    if(!AllSourcesUnchanged())return Refuse(TEXT("Coherent original source/material observation changed"));
    for(auto& P:Built.Parts)P.bNativeTemplateAndPoseObserved=true;Built.bActualOriginalAssetsAndTemplatesObserved=true;Out=MoveTemp(Built);return true;
}
} // DDSLiveIntake
bool FDDSOriginalLiveIntake::Observe(AActor& Publisher,UStaticMeshComponent& Cargo,const FDDSOriginalLiveIntakeInputs& Inputs,
    FDDSOriginalRobotNativeGeometry& Geometry,FString& Placement,FString& Error)
{
    TArray<TStrongObjectPtr<UObject>> DiagnosticLifetime;
    return DDSLiveIntake::ObserveWithLifetimeLease(Publisher,Cargo,Inputs,Geometry,Placement,Error,DiagnosticLifetime);
}
bool FDDSOriginalLiveIntake::CreateVisualsOnExistingPawn(ADDSCityRollingPawn& Pawn,AActor& Publisher,UStaticMeshComponent& Cargo,const FDDSOriginalLiveIntakeInputs& Inputs,UDDSOriginalRobotRuntimeOwner*& Owner,FString& Placement,FString& Error)
{
    Owner=nullptr;Placement.Reset();Error.Reset();
    // Game-thread-only scoped entry guard, local to this explicit caller.
    static TSet<ADDSCityRollingPawn*> IntakeActivePawns;
    if(!IsInGameThread() || !DDSLiveIntake::IntakeLoaded(&Pawn) || Pawn.IsActorBeingDestroyed()
       || !DDSLiveIntake::IntakeLoaded(&Publisher) || Publisher.IsActorBeingDestroyed()
       || !DDSLiveIntake::IntakeLoaded(&Cargo) || !Cargo.IsRegistered()
       || !DDSLiveIntake::IntakeLoaded(Cargo.GetOwner()) || Cargo.GetOwner()->IsActorBeingDestroyed()
       || !DDSLiveIntake::IntakeLoaded(Pawn.GetWorld()) || Pawn.GetWorld()->bIsTearingDown
       || Pawn.GetWorld()!=Publisher.GetWorld() || Pawn.GetWorld()!=Cargo.GetWorld())
    { Error=TEXT("Live exact same-world pawn/publisher/cargo required before registration");return false; }
    if(IntakeActivePawns.Contains(&Pawn)) { Error=TEXT("Reentrant original live intake for selected pawn refused");return false; }
    IntakeActivePawns.Add(&Pawn);ON_SCOPE_EXIT { IntakeActivePawns.Remove(&Pawn); };
    UWorld* const SelectedWorld=Pawn.GetWorld();AActor* const SelectedCargoOwner=Cargo.GetOwner();
    // All initial sources are retained BEFORE NewObject/AddInstance/Register callbacks.
    TArray<TStrongObjectPtr<UObject>> SourceLifetime;
    SourceLifetime.Emplace(&Pawn);SourceLifetime.Emplace(&Publisher);SourceLifetime.Emplace(&Cargo);
    SourceLifetime.Emplace(SelectedWorld);SourceLifetime.Emplace(SelectedCargoOwner);
    TArray<UDDSOriginalRobotRuntimeOwner*> Existing;Pawn.GetComponents(Existing);
    if(Existing.Num()>1) { Error=TEXT("Duplicate runtime owners; no replacement");return false; }
    UDDSOriginalRobotRuntimeOwner* const SelectedOwner=Existing.Num()?Existing[0]:NewObject<UDDSOriginalRobotRuntimeOwner>(&Pawn,NAME_None,RF_Transient);
    if(!DDSLiveIntake::IntakeLoaded(SelectedOwner)) { Error=TEXT("Runtime owner allocation unavailable");return false; }
    TStrongObjectPtr<UDDSOriginalRobotRuntimeOwner> SelectedOwnerPin(SelectedOwner);Owner=SelectedOwner;
    const auto ContextStillExact=[&]()
    {
        return Owner==SelectedOwner && DDSLiveIntake::IntakeLoaded(SelectedOwner) && SelectedOwner->IsRegistered()
            && SelectedOwner->GetOwner()==&Pawn && SelectedOwner->GetWorld()==SelectedWorld
            && DDSLiveIntake::IntakeLoaded(&Pawn) && !Pawn.IsActorBeingDestroyed() && Pawn.GetWorld()==SelectedWorld
            && DDSLiveIntake::IntakeLoaded(SelectedWorld) && !SelectedWorld->bIsTearingDown
            && DDSLiveIntake::IntakeLoaded(&Publisher) && !Publisher.IsActorBeingDestroyed()
            && Publisher.GetWorld()==SelectedWorld && Publisher.GetClass()->GetPathName()==DDSLiveIntake::PublisherClass
            && DDSLiveIntake::IntakeLoaded(&Cargo) && Cargo.IsRegistered() && Cargo.GetWorld()==SelectedWorld
            && Cargo.GetOwner()==SelectedCargoOwner && DDSLiveIntake::IntakeLoaded(SelectedCargoOwner) && !SelectedCargoOwner->IsActorBeingDestroyed();
    };
    if(!SelectedOwner->IsRegistered()) { Pawn.AddInstanceComponent(SelectedOwner);SelectedOwner->RegisterComponent(); }
    if(!ContextStillExact()) { Owner=SelectedOwner;Error=TEXT("Selected intake target/source context changed during registration");return false; }
    FDDSOriginalRobotNativeGeometry Geometry;
    const bool Observed=DDSLiveIntake::ObserveWithLifetimeLease(Publisher,Cargo,Inputs,Geometry,Placement,Error,SourceLifetime);
    if(!ContextStillExact()) { Owner=SelectedOwner;Error=TEXT("Selected intake target/source context changed during observation");return false; }
    if(!Observed)return false;
    // SourceLifetime contains all16 sources/owners/meshes/raw+effective materials,
    // world and publisher, and outlives every callback and return from this call.
    // ONLY the immutable selected owner can receive the observed geometry.
    const bool Created=SelectedOwner->CreateOriginalAssembly(Geometry,Error);
    if(!ContextStillExact()) { Owner=SelectedOwner;Error=TEXT("Selected intake target/source context changed during factory consumption; visuals retained for fenced cleanup");return false; }
    // Source originals retained. No bind, drive, possession, camera or readiness.
    return Created;
}
