using System.IO;
using UnrealBuildTool;

public class V3DRuntime : ModuleRules
{
    private static void SyncHlslToUsh(string ShadersDir)
    {
        if (!Directory.Exists(ShadersDir)) return;
        foreach (string HlslFile in Directory.GetFiles(ShadersDir, "*.hlsl"))
        {
            string UshFile = Path.ChangeExtension(HlslFile, ".ush");
            byte[] SourceBytes = File.ReadAllBytes(HlslFile);
            if (!File.Exists(UshFile) || !System.Linq.Enumerable.SequenceEqual(File.ReadAllBytes(UshFile), SourceBytes))
            {
                File.WriteAllBytes(UshFile, SourceBytes);
            }
        }
    }

    public V3DRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        SyncHlslToUsh(Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "Shaders")));
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Json", "glTFRuntime" });
        PrivateDependencyModuleNames.AddRange(new[] { "Projects", "RenderCore", "RHI", "PhysicsCore", "HTTP", "JsonUtilities" });
    }
}
