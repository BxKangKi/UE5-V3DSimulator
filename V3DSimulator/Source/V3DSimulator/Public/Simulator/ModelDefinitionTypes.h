// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

enum class EModelDefinitionType : uint8 { Invalid, Static, Dynamic, Character };
enum class EModelEntityType : uint8 { None, Vehicle, Prop, Animal };
enum class EModelItemType : uint8 { None, Weapon, Tool, Misc };

namespace V3DSimulatorModelTypes
{
    inline FString ToString(const EModelDefinitionType Type)
    {
        switch (Type)
        {
        case EModelDefinitionType::Static: return TEXT("Static");
        case EModelDefinitionType::Dynamic: return TEXT("Dynamic");
        case EModelDefinitionType::Character: return TEXT("Character");
        default: return TEXT("Invalid");
        }
    }

    inline bool TryParse(const FString& Value, EModelDefinitionType& Out)
    {
        if (Value == TEXT("Static")) { Out = EModelDefinitionType::Static; return true; }
        if (Value == TEXT("Dynamic")) { Out = EModelDefinitionType::Dynamic; return true; }
        if (Value == TEXT("Character")) { Out = EModelDefinitionType::Character; return true; }
        Out = EModelDefinitionType::Invalid;
        return false;
    }
}
