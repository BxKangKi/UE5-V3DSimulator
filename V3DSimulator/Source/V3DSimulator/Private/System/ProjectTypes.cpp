// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/ProjectTypes.h"

FString V3DSimulatorProjectTypes::ToString(const EV3DSimulatorProjectType ProjectType)
{
    switch (ProjectType)
    {
    case EV3DSimulatorProjectType::Prefab: return TEXT("Prefab");
    case EV3DSimulatorProjectType::Character: return TEXT("Character");
    case EV3DSimulatorProjectType::Dynamic: return TEXT("Dynamic");
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
    if (Normalized.Equals(TEXT("Prefab"), ESearchCase::IgnoreCase))
    {
        OutType = EV3DSimulatorProjectType::Prefab;
        return true;
    }
    if (Normalized.Equals(TEXT("Character"), ESearchCase::IgnoreCase))
    {
        OutType = EV3DSimulatorProjectType::Character;
        return true;
    }
    if (Normalized.Equals(TEXT("Dynamic"), ESearchCase::IgnoreCase))
    {
        OutType = EV3DSimulatorProjectType::Dynamic;
        return true;
    }
    return false;
}
