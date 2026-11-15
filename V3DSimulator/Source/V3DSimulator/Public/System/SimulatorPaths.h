// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Canonical user-data paths. No caller should concatenate V3DSimulator/Projects/Resources/V3Ds manually. */
namespace V3DSimulatorPaths
{
    V3DSIMULATOR_API FString UserRoot();
    V3DSIMULATOR_API FString ProjectsRoot();
    V3DSIMULATOR_API FString ResourcesRoot();
    /** Runtime world archive directory: V3DSimulator/V3Ds. Does not migrate older directories. */
    V3DSIMULATOR_API FString WorldsRoot();
    V3DSIMULATOR_API FString DataRoot();
    V3DSIMULATOR_API FString LogsRoot();
    V3DSIMULATOR_API FString SettingsPath();
}
