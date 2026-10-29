// Copyright © 2026 BxKangKi. Licensed under the MIT License.
using UnrealBuildTool;

public class V3DSimulatorEditor : ModuleRules
{
    public V3DSimulatorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "UMG", "UnrealEd", "UMGEditor"
        });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "V3DSimulator", "Slate", "SlateCore", "ToolMenus", "BlueprintGraph"
        });
    }
}
