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
                "V3DRuntime",
                "ShaderLibrary",
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
                // Optional glTFRuntime format extensions are discovered at runtime; their modules
                // are not hard dependencies, so an uninstalled importer cannot break the build.
                "Projects",
                "HTTP", // Match glTFRuntime's local image-URI decoding during source preflight.
                // MoviePlayer renders a pure-Slate loading screen while blocking map loads run.
                "MoviePlayer",
                "PropertyPath"
            });


    }
}
