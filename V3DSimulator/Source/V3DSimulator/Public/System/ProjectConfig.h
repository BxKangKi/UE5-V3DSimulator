// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "System/ProjectTypes.h"
#include "ProjectConfig.generated.h"

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DSimulatorProjectSummary
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Project") FString FolderName;
    UPROPERTY(BlueprintReadOnly, Category="Project") FString DisplayName;
    UPROPERTY(BlueprintReadOnly, Category="Project") FGuid UUID;
    UPROPERTY(BlueprintReadOnly, Category="Project") EV3DSimulatorProjectType ProjectType = EV3DSimulatorProjectType::World;
    /** World-only: mounts built Prefab/Character/Dynamic packs from top-level Resources/<Project>.v3d files. */
    UPROPERTY(BlueprintReadOnly, Category="Project") bool bAllowProjectAssets = false;
};

/** Canonical typed view of Projects/<Project>/config.json. Unknown project-specific fields remain in the source DOM. */
USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DSimulatorProjectConfig
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Project") FGuid UUID;
    UPROPERTY(BlueprintReadOnly, Category="Project") FString Name;
    UPROPERTY(BlueprintReadOnly, Category="Project") FString Version;
    UPROPERTY(BlueprintReadOnly, Category="Project") EV3DSimulatorProjectType ProjectType = EV3DSimulatorProjectType::World;
    UPROPERTY(BlueprintReadOnly, Category="Project") FString WorldName;
    /** World-only: mounts built Prefab/Character/Dynamic packs from top-level Resources/<Project>.v3d files. */
    UPROPERTY(BlueprintReadOnly, Category="Project") bool bAllowProjectAssets = false;

    FString GetDisplayName(const FString& FolderFallback = FString()) const;
};

namespace V3DSimulatorProjectConfig
{
    /** Parses a canonicalized DOM. Missing mandatory keys should be normalized before calling this. */
    V3DSIMULATOR_API bool Parse(
        const TSharedPtr<FJsonObject>& Json,
        const FString& FolderFallback,
        FV3DSimulatorProjectConfig& OutConfig,
        FString& OutError);

    /** Loads, upgrades and persists config.json while preserving unrelated project/world settings. */
    V3DSIMULATOR_API bool Load(
        const FString& ConfigPath,
        const FString& FolderFallback,
        FV3DSimulatorProjectConfig& OutConfig,
        TSharedPtr<FJsonObject>* OutJson,
        FString& OutError);

    /** Canonicalizes a mutable config DOM and reports whether it changed. */
    V3DSIMULATOR_API bool Normalize(
        const TSharedRef<FJsonObject>& Json,
        const FString& FolderFallback,
        bool& bOutChanged,
        FString& OutError);

}
