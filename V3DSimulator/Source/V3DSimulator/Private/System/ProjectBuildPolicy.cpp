// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectBuildPolicy.h"

bool V3DSimulatorProjectBuildPolicy::IsAssetProject(const EV3DSimulatorProjectType Type)
{
    return Type == EV3DSimulatorProjectType::Prefab
        || Type == EV3DSimulatorProjectType::Character
        || Type == EV3DSimulatorProjectType::Dynamic;
}

EModelDefinitionType V3DSimulatorProjectBuildPolicy::DefaultModelType(const EV3DSimulatorProjectType Type)
{
    switch (Type)
    {
    case EV3DSimulatorProjectType::Character: return EModelDefinitionType::Character;
    case EV3DSimulatorProjectType::Dynamic: return EModelDefinitionType::Dynamic;
    case EV3DSimulatorProjectType::Prefab:
    case EV3DSimulatorProjectType::World:
    default: return EModelDefinitionType::Static;
    }
}

bool V3DSimulatorProjectBuildPolicy::AllowsModelType(
    const EV3DSimulatorProjectType Type,
    const EModelDefinitionType ModelType)
{
    if (Type == EV3DSimulatorProjectType::World)
    {
        return ModelType == EModelDefinitionType::Static
            || ModelType == EModelDefinitionType::Dynamic
            || ModelType == EModelDefinitionType::Character;
    }
    return ModelType == DefaultModelType(Type);
}
