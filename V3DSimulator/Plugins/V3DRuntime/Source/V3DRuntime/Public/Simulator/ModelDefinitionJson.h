/**
 * @file ModelDefinitionJson.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
class FJsonObject;
#include "Gravity/GravityFieldTypes.h"
#include "Simulator/AssetDefinitionTypes.h"
#include "Simulator/ModelDefinitionTypes.h"
#include "System/ProjectTypes.h"
#include "Format/v3d.h"

/** Strict, immutable view of one author-owned model JSON document. */
struct V3DRUNTIME_API FModelDefinition
{
    EAssetDefinitionType AssetType = EAssetDefinitionType::Model;
    FGuid UUID;
    FString Name;
    FString DisplayName;
    /** Stores the source GLB, glTF, OBJ, FBX or STL path. */
    FString GlbPath;
    FString JsonPath;
    EModelDefinitionType ModelType = EModelDefinitionType::Invalid;
    EModelEntityType EntityType = EModelEntityType::None;
    EModelItemType ItemType = EModelItemType::None;
    bool bToon = false;
    uint8 MaterialMode = 0; // Stable wire values: 0 Auto, 1 legacy Opaque alias, 3 Translucent, 5 Opaque (Masked shader). Sidedness comes from the source.
    // These authoring values travel with the model JSON, never with settings.json or the GPU.
    int32 BuildTextureResolution = v3d::build_texture_resolution;
    int32 BuildTerrainTextureResolution = v3d::build_terrain_texture_resolution;
    bool bBuildTerrainFlipNormalGreen = false;
    TMap<FString, FString> Bones;
    FGravityFieldSettings GravityField;

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
 * scanned recursively, and a source model is paired only with the JSON that has the
 * same base filename in the same directory.
 */
namespace ModelDefinitionJson
{
    /**
     * Creates missing sibling JSON for every recursively discovered supported model format.
     * New metadata is persisted to disk before validation, and the default ModelType comes
     * from ProjectBuildPolicy. Existing author-owned fields are preserved.
     * OutDiscoveredModelFiles receives the exact normalized, de-duplicated source snapshot.
     *
     * This function performs file-system work only and is safe to call from a
     * worker thread. It never touches UObjects or world state.
     */
    V3DRUNTIME_API bool EnsureDefinitions(
        const FString& ModelRootDirectory,
        EV3DSimulatorProjectType ProjectType,
        TArray<FString>* OutUpdatedJsonFiles = nullptr,
        TArray<FString>* OutDiscoveredModelFiles = nullptr);

    /**
     * Strictly validates a canonical definition. Missing required metadata, unknown types,
     * invalid UUIDs, mismatched subtypes and a character without a Bones object are rejected.
     * OutCanonicalJson receives the normalized snapshot of the exact DOM that passed validation.
     */
    V3DRUNTIME_API bool ValidateDefinition(const TSharedPtr<FJsonObject>& Root,
        const FString& JsonPath, const FString& ExpectedSourcePath, FModelDefinition& OutDefinition,
        FString& OutError, FString* OutCanonicalJson = nullptr);

    V3DRUNTIME_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedSourcePath,
        FModelDefinition& OutDefinition,
        FString& OutError,
        FString* OutCanonicalJson = nullptr);

}
