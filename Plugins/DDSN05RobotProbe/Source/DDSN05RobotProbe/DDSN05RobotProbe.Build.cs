using UnrealBuildTool;

public class DDSN05RobotProbe : ModuleRules
{
    public DDSN05RobotProbe(ReadOnlyTargetRules Target) : base(Target)
    {
        if (Target.Type != TargetType.Editor || Target.Platform != UnrealTargetPlatform.Win64)
            throw new BuildException("DDSN05RobotProbe supports the pinned Win64 Editor target only.");
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateIncludePaths.Add(System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Source", "DDSCity")));
        PrivateDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "AssetRegistry", "Json",
            "MeshDescription", "StaticMeshDescription", "PhysicsCore", "Chaos", "DDSCity"
        });
    }
}
