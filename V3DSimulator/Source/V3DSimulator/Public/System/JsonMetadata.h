// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Shared canonical metadata keys used by config.json and authored asset JSON. */
namespace V3DSimulatorJsonMetadata
{
    inline constexpr TCHAR UUID[] = TEXT("UUID");
    inline constexpr TCHAR Name[] = TEXT("Name");
    inline constexpr TCHAR DisplayName[] = TEXT("DisplayName");
    inline constexpr TCHAR Version[] = TEXT("Version");
    inline constexpr TCHAR ProjectType[] = TEXT("ProjectType");
    inline constexpr TCHAR AssetType[] = TEXT("AssetType");
    inline constexpr TCHAR ModelType[] = TEXT("ModelType");
    inline constexpr TCHAR AllowProjectAssets[] = TEXT("bAllowProjectAssets");
    inline constexpr TCHAR WorldName[] = TEXT("WorldName");
    inline constexpr TCHAR SchemaVersion[] = TEXT("1.0.0");
}
