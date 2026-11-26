// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectTypes.h"

FString V3DSimulatorProjectTypes::ToString(const EV3DSimulatorProjectType ProjectType)
{
    switch (ProjectType)
    {
    case EV3DSimulatorProjectType::Module: return TEXT("Module");
    case EV3DSimulatorProjectType::World:
    default: return TEXT("World");
    }
}

bool V3DSimulatorProjectTypes::TryParse(
    const FString& Value,
    EV3DSimulatorProjectType& OutType)
{
    FString Normalized = Value;
    Normalized.TrimStartAndEndInline();
    if (Normalized.Equals(TEXT("World"), ESearchCase::IgnoreCase))
    {
        OutType = EV3DSimulatorProjectType::World;
        return true;
    }
    if (Normalized.Equals(TEXT("Module"), ESearchCase::IgnoreCase))
    {
        OutType = EV3DSimulatorProjectType::Module;
        return true;
    }
    return false;
}
