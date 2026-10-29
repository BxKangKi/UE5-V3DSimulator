// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "ProjectTypes.generated.h"

/** Domain-level project category. Storage/config code consumes this type but does not own it. */
UENUM(BlueprintType)
enum class EV3DSimulatorProjectType : uint8
{
    World UMETA(DisplayName="World"),
    Prefab UMETA(DisplayName="Prefab"),
    Character UMETA(DisplayName="Character"),
    Dynamic UMETA(DisplayName="Dynamic")
};

namespace V3DSimulatorProjectTypes
{
    V3DSIMULATOR_API FString ToString(EV3DSimulatorProjectType ProjectType);
    V3DSIMULATOR_API bool TryParse(const FString& Value, EV3DSimulatorProjectType& OutType);
}
