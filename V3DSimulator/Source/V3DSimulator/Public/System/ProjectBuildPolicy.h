// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Simulator/ModelDefinitionTypes.h"
#include "System/ProjectTypes.h"

namespace V3DSimulatorProjectBuildPolicy
{
    V3DSIMULATOR_API bool IsAssetProject(EV3DSimulatorProjectType Type);
    V3DSIMULATOR_API EModelDefinitionType DefaultModelType(EV3DSimulatorProjectType Type);
    V3DSIMULATOR_API bool AllowsModelType(EV3DSimulatorProjectType Type, EModelDefinitionType ModelType);
}
