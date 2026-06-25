// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file V3DSimulator.Target.cs
 // UE 5.8 target configuration for V3DSimulator.
 // UE 5.8 target configuration for V3DSimulator.
 */

using UnrealBuildTool;
using System.Collections.Generic;

public class V3DSimulatorTarget : TargetRules
{
    public V3DSimulatorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange( new string[] { "V3DSimulator"} );
    }
}
