// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Canonical user-data paths. No caller should concatenate V3DSimulator/Projects/Resources/V3Ds manually. */
namespace V3DSimulatorPaths
{
    V3DRUNTIME_API FString UserRoot();
    V3DRUNTIME_API FString ProjectsRoot();
    V3DRUNTIME_API FString ResourcesRoot();
    /** Runtime world archive directory: V3DSimulator/V3Ds. Does not migrate older directories. */
    V3DRUNTIME_API FString WorldsRoot();
    V3DRUNTIME_API FString DataRoot();
    V3DRUNTIME_API FString LogsRoot();
    V3DRUNTIME_API FString SettingsPath();
}
