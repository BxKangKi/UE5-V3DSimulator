// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Simulator/AssetDefinitionTypes.h"
#include "Simulator/ModelDefinitionTypes.h"

/** Upgrades one sibling asset JSON in place. Existing author fields are preserved. */
namespace V3DSimulatorJsonMetadataNormalizer
{
    V3DSIMULATOR_API bool EnsureAssetJson(
        const FString& JsonPath,
        const FString& BaseName,
        EAssetDefinitionType ExpectedAssetType,
        EModelDefinitionType DefaultModelType,
        bool& bOutChanged,
        FString& OutError);
}
