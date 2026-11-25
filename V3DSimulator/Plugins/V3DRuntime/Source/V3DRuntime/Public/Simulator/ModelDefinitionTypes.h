// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

enum class EModelDefinitionType : uint8 { Invalid, Static, Dynamic };
enum class EModelDynamicType : uint8 { None, Humanoid, Vehicle, Prop, Creature, Item };
enum class EModelItemType : uint8 { None, Weapon, Tool, Misc };
enum class EModelVehicleType : uint8 { None, Car, Train };

namespace V3DSimulatorModelTypes
{
    inline FString ToString(const EModelDefinitionType Type)
    {
        switch (Type)
        {
        case EModelDefinitionType::Static: return TEXT("Static");
        case EModelDefinitionType::Dynamic: return TEXT("Dynamic");
        default: return TEXT("Invalid");
        }
    }

    inline bool TryParse(const FString& Value, EModelDefinitionType& Out)
    {
        if (Value == TEXT("Static")) { Out = EModelDefinitionType::Static; return true; }
        if (Value == TEXT("Dynamic")) { Out = EModelDefinitionType::Dynamic; return true; }
        Out = EModelDefinitionType::Invalid;
        return false;
    }

    inline FString ToString(const EModelDynamicType Type)
    {
        switch (Type)
        {
        case EModelDynamicType::Humanoid: return TEXT("Humanoid");
        case EModelDynamicType::Vehicle: return TEXT("Vehicle");
        case EModelDynamicType::Prop: return TEXT("Prop");
        case EModelDynamicType::Creature: return TEXT("Creature");
        case EModelDynamicType::Item: return TEXT("Item");
        default: return TEXT("None");
        }
    }

    inline bool TryParseDynamicType(const FString& Value, EModelDynamicType& Out)
    {
        if (Value == TEXT("Humanoid")) { Out = EModelDynamicType::Humanoid; return true; }
        if (Value == TEXT("Vehicle")) { Out = EModelDynamicType::Vehicle; return true; }
        if (Value == TEXT("Prop")) { Out = EModelDynamicType::Prop; return true; }
        if (Value == TEXT("Creature")) { Out = EModelDynamicType::Creature; return true; }
        if (Value == TEXT("Item")) { Out = EModelDynamicType::Item; return true; }
        Out = EModelDynamicType::None;
        return false;
    }

    inline FString ToString(const EModelItemType Type)
    {
        switch (Type)
        {
        case EModelItemType::Weapon: return TEXT("Weapon");
        case EModelItemType::Tool: return TEXT("Tool");
        case EModelItemType::Misc: return TEXT("Misc");
        default: return TEXT("None");
        }
    }

    inline bool TryParseItemType(const FString& Value, EModelItemType& Out)
    {
        if (Value == TEXT("Weapon")) { Out = EModelItemType::Weapon; return true; }
        if (Value == TEXT("Tool")) { Out = EModelItemType::Tool; return true; }
        if (Value == TEXT("Misc")) { Out = EModelItemType::Misc; return true; }
        Out = EModelItemType::None;
        return false;
    }

    inline FString ToString(const EModelVehicleType Type)
    {
        switch (Type)
        {
        case EModelVehicleType::Car: return TEXT("Car");
        case EModelVehicleType::Train: return TEXT("Train");
        default: return TEXT("None");
        }
    }

    inline bool TryParseVehicleType(const FString& Value, EModelVehicleType& Out)
    {
        if (Value == TEXT("Car")) { Out = EModelVehicleType::Car; return true; }
        if (Value == TEXT("Train")) { Out = EModelVehicleType::Train; return true; }
        Out = EModelVehicleType::None;
        return false;
    }
}
