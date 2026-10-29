// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file V3DSimulator.Build.cs
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 */

using UnrealBuildTool;
using System.IO;

public class V3DSimulator : ModuleRules
{
    public V3DSimulator(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // v3d.hlsl is the public shader source. Unreal shader includes use .ush;
        // keep a byte-identical alias so editing the public file is sufficient.
        string TerrainShaderDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../Shaders"));
        string TerrainHlsl = Path.Combine(TerrainShaderDirectory, "v3d.hlsl");
        string TerrainUsh = Path.Combine(TerrainShaderDirectory, "v3d.ush");
        ExternalDependencies.Add(TerrainHlsl);
        ExternalDependencies.Add(TerrainUsh);
        if (File.Exists(TerrainHlsl)
            && (!File.Exists(TerrainUsh) || File.ReadAllText(TerrainHlsl) != File.ReadAllText(TerrainUsh)))
        {
            File.Copy(TerrainHlsl, TerrainUsh, true);
        }

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "EnhancedInput",
                "InputCore",
                "Json",
                "glTFRuntime",
                "Slate",
                "UMG",
                "SlateCore"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Niagara",
                "JsonUtilities",
                "RHI",
                "RenderCore", // Shipping-safe PSO readiness diagnostics.
                "ProceduralMeshComponent",
                "PhysicsCore",
                // MoviePlayer renders a pure-Slate loading screen while blocking map loads run.
                "MoviePlayer"
            });

    }
}
