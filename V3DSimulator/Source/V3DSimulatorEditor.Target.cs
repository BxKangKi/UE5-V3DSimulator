// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file V3DSimulatorEditor.Target.cs
 * UE 5.8 editor target configuration for V3DSimulator.
 */

using UnrealBuildTool;
using System.Collections.Generic;

public class V3DSimulatorEditorTarget : TargetRules
{
    public V3DSimulatorEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange(new string[] { "V3DSimulator", "V3DSimulatorEditor" });
    }
}
