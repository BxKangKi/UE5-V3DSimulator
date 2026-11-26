// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "ProjectTypes.generated.h"

/** Domain-level project category: World (.v3d) or Module (.v3dm). */
UENUM(BlueprintType)
enum class EV3DSimulatorProjectType : uint8
{
    World UMETA(DisplayName="World"),
    Module UMETA(DisplayName="Module")
};

namespace V3DSimulatorProjectTypes
{
    V3DRUNTIME_API FString ToString(EV3DSimulatorProjectType ProjectType);
    V3DRUNTIME_API bool TryParse(const FString& Value, EV3DSimulatorProjectType& OutType);
}
