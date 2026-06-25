/**
 * @file ModelDefinitionJson.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Simulator/AssetDefinitionTypes.h"
#include "Simulator/ModelDefinitionTypes.h"
#include "System/ProjectTypes.h"

/** Strict, immutable view of one author-owned model JSON document. */
struct V3DSIMULATOR_API FModelDefinition
{
    EAssetDefinitionType AssetType = EAssetDefinitionType::Model;
    FGuid UUID;
    FString Name;
    FString DisplayName;
    FString GlbPath;
    FString JsonPath;
    EModelDefinitionType ModelType = EModelDefinitionType::Invalid;
    EModelEntityType EntityType = EModelEntityType::None;
    EModelItemType ItemType = EModelItemType::None;
    TMap<FString, FString> Bones;

    bool IsLoadable() const
    {
        return AssetType == EAssetDefinitionType::Model && (ModelType == EModelDefinitionType::Static
            || ModelType == EModelDefinitionType::Dynamic
            || ModelType == EModelDefinitionType::Character);
    }
};

/**
 * Disk-side helpers for model definition JSON files.
 *
 * The model directory is intentionally category-free. Every subdirectory is
 * scanned recursively, and a GLB is paired only with the JSON that has the
 * same base filename in the same directory.
 */
namespace ModelDefinitionJson
{
    /**
     * Creates or upgrades the sibling JSON for every recursively discovered GLB.
     * Required metadata is persisted to disk before validation, and the default ModelType comes
     * from ProjectBuildPolicy. Existing author-owned fields are preserved.
     * OutDiscoveredGlbFiles receives the exact normalized, de-duplicated source snapshot.
     *
     * This function performs file-system work only and is safe to call from a
     * worker thread. It never touches UObjects or world state.
     */
    V3DSIMULATOR_API bool EnsureDefinitions(
        const FString& ModelRootDirectory,
        EV3DSimulatorProjectType ProjectType,
        TArray<FString>* OutUpdatedJsonFiles = nullptr,
        TArray<FString>* OutDiscoveredGlbFiles = nullptr);

    /**
     * Strictly validates a canonical definition. Missing required metadata, unknown types,
     * invalid UUIDs, mismatched subtypes and a character without a Bones object are rejected.
     * OutCanonicalJson receives the normalized snapshot of the exact DOM that passed validation.
     */
    V3DSIMULATOR_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedGlbPath,
        FModelDefinition& OutDefinition,
        FString& OutError,
        FString* OutCanonicalJson = nullptr);

}
