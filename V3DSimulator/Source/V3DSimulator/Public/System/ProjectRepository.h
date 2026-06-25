// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "System/ProjectConfig.h"

/** File-system repository for Projects/. UI/game systems must not manipulate project files directly. */
namespace V3DSimulatorProjectRepository
{
    V3DSIMULATOR_API bool ListProjects(TArray<FV3DSimulatorProjectSummary>& OutProjects, FString& OutError);
    V3DSIMULATOR_API bool CreateProject(
        const FString& ProjectName,
        EV3DSimulatorProjectType ProjectType,
        FString& OutError);
    V3DSIMULATOR_API bool LoadProject(
        const FString& ProjectName,
        FV3DSimulatorProjectConfig& OutConfig,
        TSharedPtr<FJsonObject>* OutJson,
        FString& OutError);
    V3DSIMULATOR_API bool SetProjectType(
        const FString& ProjectName,
        EV3DSimulatorProjectType ProjectType,
        FString& OutError);
    V3DSIMULATOR_API bool SetWorldProjectAssetsAllowed(
        const FString& ProjectName,
        bool bAllowed,
        FString& OutError);
}
