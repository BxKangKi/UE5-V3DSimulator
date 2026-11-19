// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
class FJsonObject;
#include "Simulator/AssetDefinitionTypes.h"

/** Strict immutable view of one Sound JSON + same-basename WAV pair. */
struct V3DRUNTIME_API FSoundDefinition
{
    EAssetDefinitionType AssetType = EAssetDefinitionType::Sound;
    FGuid UUID;
    FString Name;
    FString DisplayName;
    FString WavPath;
    FString JsonPath;

    bool IsLoadable() const
    {
        return AssetType == EAssetDefinitionType::Sound && UUID.IsValid()
            && !Name.IsEmpty() && !WavPath.IsEmpty();
    }
};

/** Disk-side helpers for Sound assets. A Sound is exactly <base>.json + <base>.wav. */
namespace SoundDefinitionJson
{
    /** Recursively discovers WAV files and creates or upgrades each same-basename Sound JSON. */
    V3DRUNTIME_API bool EnsureDefinitions(
        const FString& AssetRootDirectory,
        TArray<FString>* OutUpdatedJsonFiles = nullptr,
        TArray<FString>* OutDiscoveredWavFiles = nullptr);

    /** Strictly validates AssetType=Sound and the exact same-basename WAV pairing. */
    V3DRUNTIME_API bool ValidateDefinition(const TSharedPtr<FJsonObject>& Root,
        const FString& JsonPath, const FString& ExpectedWavPath, FSoundDefinition& OutDefinition,
        FString& OutError, FString* OutCanonicalJson = nullptr);

    V3DRUNTIME_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedWavPath,
        FSoundDefinition& OutDefinition,
        FString& OutError,
        FString* OutCanonicalJson = nullptr);
}
