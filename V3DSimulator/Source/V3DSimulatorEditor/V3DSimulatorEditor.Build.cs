// Copyright © 2026 BxKangKi. Licensed under the MIT License.

using UnrealBuildTool;

public class V3DSimulatorEditor : ModuleRules
{
    public V3DSimulatorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Editor-only services, automation, and UnrealEd integrations live in this module.
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "V3DSimulator"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "glTFRuntime",
                "Json",
                "JsonUtilities",
                "RHI"
            });
    }
}
