// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectBuildPolicy.h"

bool V3DSimulatorProjectBuildPolicy::IsAssetProject(const EV3DSimulatorProjectType Type)
{
    return Type == EV3DSimulatorProjectType::Module;
}

EModelDefinitionType V3DSimulatorProjectBuildPolicy::DefaultModelType(const EV3DSimulatorProjectType Type)
{
    (void)Type;
    return EModelDefinitionType::Static;
}

bool V3DSimulatorProjectBuildPolicy::AllowsModelType(
    const EV3DSimulatorProjectType Type,
    const EModelDefinitionType ModelType)
{
    (void)Type;
    return ModelType == EModelDefinitionType::Static
        || ModelType == EModelDefinitionType::Dynamic;
}
