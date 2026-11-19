// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchiveCodec.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "System/WorldArchive.h"

/** Unreal adapters for the portable single-header V3D member codecs in Format/v3d.hpp. */
namespace WorldArchiveCodec
{
    V3DRUNTIME_API bool SerializeMesh(const FGWorldBakedMesh& Value, TArray<uint8>& Out, FString& Error);
    // Dependency-only mode walks bounded records without allocating vertex arrays. Its output
    // is metadata only; the normal payload read still validates every geometry value.
    V3DRUNTIME_API bool DeserializeMesh(const TArray<uint8>& Bytes, FGWorldBakedMesh& Out, FString& Error, bool bDependenciesOnly = false);
    V3DRUNTIME_API bool SerializeSkin(const FGWorldBakedSkin& Value, TArray<uint8>& Out, FString& Error);
    V3DRUNTIME_API bool DeserializeSkin(const TArray<uint8>& Bytes, FGWorldBakedSkin& Out, FString& Error);
    V3DRUNTIME_API bool SerializeMaterial(const FGWorldBakedMaterial& Value, TArray<uint8>& Out, FString& Error);
    V3DRUNTIME_API bool DeserializeMaterial(const TArray<uint8>& Bytes, FGWorldBakedMaterial& Out, FString& Error);
    V3DRUNTIME_API bool SerializeTexture(const FGWorldBakedTexture& Value, TArray<uint8>& Out, FString& Error);
    V3DRUNTIME_API bool DeserializeTexture(const TArray<uint8>& Bytes, FGWorldBakedTexture& Out, FString& Error, int32 MaxResolution = 0);
    V3DRUNTIME_API bool SerializeManifest(const FGWorldModelManifest& Value, TArray<uint8>& Out, FString& Error);
    V3DRUNTIME_API bool DeserializeManifest(const TArray<uint8>& Bytes, FGWorldModelManifest& Out, FString& Error);
}
