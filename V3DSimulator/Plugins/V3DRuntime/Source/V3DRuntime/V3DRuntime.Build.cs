using UnrealBuildTool;

public class V3DRuntime : ModuleRules
{
    public V3DRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json", "glTFRuntime" });
        PrivateDependencyModuleNames.AddRange(new[] { "Projects", "RenderCore", "RHI", "PhysicsCore", "HTTP", "JsonUtilities" });
    }
}
