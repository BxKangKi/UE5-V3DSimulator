// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Owns the canonical Projects authoring layout and top-level built Resources repository. */
namespace V3DSimulatorProjectWorkspace
{
    V3DSIMULATOR_API bool NormalizeProjectName(const FString& Input, FString& OutSafeName);
    V3DSIMULATOR_API FString ProjectsRoot();
    V3DSIMULATOR_API FString ProjectRoot(const FString& ProjectName);
    V3DSIMULATOR_API FString ProjectResourcesRoot(const FString& ProjectName);
    V3DSIMULATOR_API FString ConfigPath(const FString& ProjectName);
    V3DSIMULATOR_API FString AssetArchivePath(const FString& ProjectName);
    V3DSIMULATOR_API bool EnsureWorkspaceRoots();
    V3DSIMULATOR_API bool EnsureProjectDirectories(const FString& ProjectName);
    V3DSIMULATOR_API FString WorldArchivePath(const FString& ProjectName);
    V3DSIMULATOR_API void InvalidateBuiltArtifacts(const FString& ProjectName);
}
