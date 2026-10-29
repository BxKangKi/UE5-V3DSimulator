// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Top-level authored asset category stored in every non-config JSON document. */
enum class EAssetDefinitionType : uint8
{
    Model,
    Sound
};

namespace V3DSimulatorAssetTypes
{
    inline FString ToString(const EAssetDefinitionType Type)
    {
        return Type == EAssetDefinitionType::Sound ? TEXT("Sound") : TEXT("Model");
    }

    inline bool TryParse(const FString& Value, EAssetDefinitionType& OutType)
    {
        if (Value.Equals(TEXT("Model"), ESearchCase::IgnoreCase))
        {
            OutType = EAssetDefinitionType::Model;
            return true;
        }
        if (Value.Equals(TEXT("Sound"), ESearchCase::IgnoreCase))
        {
            OutType = EAssetDefinitionType::Sound;
            return true;
        }
        return false;
    }
}
