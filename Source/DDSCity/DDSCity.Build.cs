using UnrealBuildTool;

public class DDSCity : ModuleRules
{
    public DDSCity(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });
        PublicDependencyModuleNames.AddRange(new string[] { "CitySample", "CinematicCamera" });
        PublicDependencyModuleNames.Add("MassEntity");
        PrivateDependencyModuleNames.Add("MassCommon");
        PrivateDependencyModuleNames.Add("MassActors");
        PrivateDependencyModuleNames.Add("MassRepresentation");
        PrivateDependencyModuleNames.AddRange(new string[] { "ZoneGraph", "MassCrowd" }); // explicit read-only block survey APIs
        PrivateDependencyModuleNames.Add("ChaosVehicles"); // exported original native vehicle base header
        PrivateDependencyModuleNames.AddRange(new string[] { "MassSimulation", "MassMovement", "MassNavigation", "MassTraffic" });
        PrivateDependencyModuleNames.Add("EnhancedInput");
        PrivateDependencyModuleNames.Add("RenderCore"); // full-map canvas batch white texture
        PrivateDependencyModuleNames.AddRange(new string[] { "MoviePlayer", "MovieSceneCapture", "ImageWriteQueue", "ImageWrapper" });
        PublicIncludePaths.Add(System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "..", "CitySample")));
        PrivateDependencyModuleNames.AddRange(new string[] { "UMG", "Slate", "SlateCore", "Json", "PhysicsCore", "Chaos", "ChaosSolverEngine" });
    }
}
