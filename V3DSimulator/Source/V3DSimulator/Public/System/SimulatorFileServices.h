// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Narrow C++ file service used by runtime settings and diagnostics. */
class V3DSIMULATOR_API FSimulatorFileServices
{
public:
    static void WriteLogAsync(const FString& Category, const FString& Message);
    static TSharedPtr<FJsonObject> LoadJson(const FString& Path);
    static void SaveJsonAsync(TSharedRef<FJsonObject> Json, const FString& Path);
};
