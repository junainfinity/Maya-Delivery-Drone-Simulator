#include "DDSRobotOriginalBinding.h"
#include "DDSRobotLifecycleOwner.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
static const TCHAR* OriginalLedgerHash=TEXT("88178075AEEC58333E73C576A9ABFDF3411A209E3BAB7B7ACD0445D091BEA7DA");
struct FOriginalPackage { const TCHAR* Path; const TCHAR* Hash; };
static const FOriginalPackage OriginalPackages[]={
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Blueprint/BP_DeliveryVehicle_v1.BP_DeliveryVehicle_v1"),TEXT("89C958CC7DBC4922B2B26B126598DC2962600B6C5A1515959EB31AECF1B9083E")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Level/LV_DeliveryRobot_v1_Showcase_BuiltData.LV_DeliveryRobot_v1_Showcase_BuiltData"),TEXT("43029440C12FA861D05B5B03D53CE53C8C7AE3B263D2578FD884A7298BC38593")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Material/MI_DeliveryRobot_v1.MI_DeliveryRobot_v1"),TEXT("75BA9CF93630AE1C9BE917788EA274D8E4CB5377F58485317267C87BA5D0FD38")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Material/MI_DeliveryRobot_v1_Dirty.MI_DeliveryRobot_v1_Dirty"),TEXT("3E8F45D8F931874E4B2851F51B95201F7F4C267B8091D5C1DF0FC51586875952")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_Delivery_Robot_v1.SM_Delivery_Robot_v1"),TEXT("6EA4240B2585C960D018964929E7A780754E990E494F51351CB972433CF04238")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_AllWheels.SM_DeliveryRobot_AllWheels"),TEXT("B05699854DBF1805F4BB7097CD8637E6BD9FFAF8BB73D4587BBCAAD00012AFFE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Base.SM_DeliveryRobot_Base"),TEXT("1A0E9EB0BAC3FFBE8B9E44537A27B52B63CFF83A3020B6DFB0654717BA514F93")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Door1.SM_DeliveryRobot_Door1"),TEXT("D6542BF720540310BEB224C6B0CC185A56B0CC2B4D2CA23F44BD4DEFC4EFD91E")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Door2.SM_DeliveryRobot_Door2"),TEXT("025A9465A670202DD513375BFBA5F5067F5DE336C5CBA0F841B2DC5DBE593E60")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Opened.SM_DeliveryRobot_Opened"),TEXT("99D5F9E1697DA16FDFB4EA5F65B9A942E3DA21DB8CCC16220840B8E8F7724961")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Suspension.SM_DeliveryRobot_Suspension"),TEXT("4158C0293B9AC62DAA65B8170CEE153C7C70B59C97FB2B3D77F832D6181474DE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_v1.SM_DeliveryRobot_v1"),TEXT("3B2CF354AF948F04789666AC3815BE95C3613421808571A212B4276C433C1161")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Mesh/SM_DeliveryRobot_Wheel.SM_DeliveryRobot_Wheel"),TEXT("0B85380F58988A7471851BD224BFDD652A9EB4B82774E5596AE13D5081879047")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_B.T_Delivery_Robot_v1_B"),TEXT("4D70A549948D74ABB647E8985C803098511C04ED5C24F5376338B19F61A7E282")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_E.T_Delivery_Robot_v1_E"),TEXT("EC365A3334A6C755396C351A9C286473BDF5AECC7E5D89281F19E3F5E639CDF5")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_M.T_Delivery_Robot_v1_M"),TEXT("4F3656A79A77CC97794FF833B6C48EA82E512905B3858EF8CFB3514407EB1C3B")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_N.T_Delivery_Robot_v1_N"),TEXT("E3B53598862FF51087963E01BC45ED9159A1E2D59AC6BC9391EE005C2A1AFBAE")},
    {TEXT("/Game/BendylabStudios/BLS_DeliveryRobot_v1/Texture/T_Delivery_Robot_v1_ORM.T_Delivery_Robot_v1_ORM"),TEXT("12AE9F9CD0F56D90AB2E4BD9B83711491B14885FE8451473C30A485DF6AB0060")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/MM_BLS_MasterGeneral.MM_BLS_MasterGeneral"),TEXT("8C2F06EFB43AC0376E88918215C9DB9019DCC0BCF7691E9DAB5F1AA39762A836")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_Black.T_Base_Black"),TEXT("360E223E5FF6024D321DF8B474E5C32DD03BFCBD3125B975B241D42D5492DD4F")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_ORM.T_Base_ORM"),TEXT("E95C7BB081807FBA647898157A5960E33073F8AC6E972F891FA51862B342EC11")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_White.T_Base_White"),TEXT("1AF9B4125B8290065EE2E19439C1293B860280294767C80443442AE2586928BE")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_Base_White_Linear.T_Base_White_Linear"),TEXT("F883857F62C4D9F77EB09EB32C12295323B81BB92235EC0C565FA2E16DA030A6")},
    {TEXT("/Game/BendylabStudios/BLS_SharedResources/Material/MasterMaterial/T_FlatNormal.T_FlatNormal"),TEXT("2CBE18DB5D003EA1A9530F57441B7590BD4A439CE5E98EC28082FD48D96D4FA1")},
};
static const TCHAR* OriginalHash(const FString& Path) { for(const auto& P:OriginalPackages) if(Path==P.Path) return P.Hash; return nullptr; }

bool DDSConsumeOriginalRobotBinding(AActor& Actor,const FDDSRobotOriginalComponentMap& Names,
    const FDDSRobotMeasuredBinding& Measurement,const FDDSRobotPhysicsProfile& Profile,
    FDDSRobotMeasuredBinding& Out,FString& Error,EDDSOriginalAssemblyScope Scope)
{
    check(IsInGameThread()); Out=FDDSRobotMeasuredBinding();
    if(Scope!=EDDSOriginalAssemblyScope::FullService && Scope!=EDDSOriginalAssemblyScope::ClosedLidDrive)
    { Error=TEXT("Unsupported original assembly scope"); return false; }
    if(Scope==EDDSOriginalAssemblyScope::ClosedLidDrive
       && (!Names.Parcel.IsNone() || Measurement.Parcel.ActualPart || Measurement.Mechanics.bFrozen || Profile.Service.bFrozen))
    { Error=TEXT("ClosedLidDrive requires no mapped/bound parcel and unfrozen mechanics/service"); return false; }
    if(Names.IntegrityLedgerSha256!=OriginalLedgerHash) { Error=TEXT("Fresh original asset integrity ledger mismatch"); return false; }
    for(TCHAR H:Names.IntegrityLedgerSha256) if(!FChar::IsHexDigit(H)) return false;
    TArray<UStaticMeshComponent*> Parts; Actor.GetComponents(Parts);
    TSet<UStaticMeshComponent*> Used;
    auto Resolve=[&](FName Name)->UDDSRobotLeasedMeshComponent*
    {
        UDDSRobotLeasedMeshComponent* Found=nullptr; int32 Matches=0;
        for(auto* P:Parts) if(P && P->GetFName()==Name)
        { if(++Matches>1) return nullptr; Found=Cast<UDDSRobotLeasedMeshComponent>(P); }
        if(Name.IsNone() || !Found || !Found->IsRegistered() || Found->IsSimulatingPhysics() || Used.Contains(Found)) return nullptr;
        Used.Add(Found); return Found;
    };
    auto Candidate=Measurement;
    for(int32 I=0;I<9;++I)
    {
        auto* P=Resolve(Names.Bodies[I]);
        if(!P || !P->GetStaticMesh() || P->GetStaticMesh()->GetPathName()!=Measurement.Bodies[I].SuppliedMeshPackage
            || !OriginalHash(Measurement.Bodies[I].SuppliedMeshPackage)
            || Measurement.Bodies[I].MeshPackageSha256!=OriginalHash(Measurement.Bodies[I].SuppliedMeshPackage))
        { Error=FString::Printf(TEXT("Original body %d exact native name/mesh identity absent; A02 must supply managed original component"),I); return false; }
        Candidate.Bodies[I].ActualPart=P;
    }
    for(int32 I=0;I<6;++I)
    { Candidate.SuspensionParts[I].ActualPart=Resolve(Names.Suspension[I]);
      auto* P=Candidate.SuspensionParts[I].ActualPart;
      if(!P || !P->GetStaticMesh() || !OriginalHash(P->GetStaticMesh()->GetPathName())) return false; }
    if(Scope==EDDSOriginalAssemblyScope::FullService)
    {
        Candidate.Parcel.ActualPart=Resolve(Names.Parcel);
        if(!Candidate.Parcel.ActualPart || !Candidate.Mechanics.bFrozen || !Profile.Service.bFrozen)
        { Error=TEXT("FullService requires actual managed parcel and frozen mechanics/service"); return false; }
    }
    // Scope changes the owned parcel/service requirement only. Every applicable
    // body/joint/suspension/mount/contact/sign/profile certificate check remains.
    if(!DDSValidateMeasuredBinding(Candidate,Profile,Error)) return false;
    Out=Candidate; return true;
}
