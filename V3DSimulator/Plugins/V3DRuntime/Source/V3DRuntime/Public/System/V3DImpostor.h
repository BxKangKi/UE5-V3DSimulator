// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedData.h"

class UMaterialInterface;
class UObject;

/**
 * Build-time octahedral impostor capture and runtime material resolution for V3DRuntime.
 * Captures an N x N hemi-octahedral atlas (BaseColor+Alpha, WorldNormal+Roughness) into a
 * 2-slice Texture2DArray (or 2D fallback), creates a lightweight 2-triangle billboard quad
 * mesh, and attaches it to the LAST LOD slot of each eligible static mesh group in the .v3d archive.
 */
namespace V3DImpostor
{
    inline constexpr const TCHAR* DefaultMaterialPath = TEXT("/V3DRuntime/Materials/M_V3D_Impostor.M_V3D_Impostor");
    inline constexpr const TCHAR* MeshNameSuffix = TEXT(".v3d_impostor");
    inline constexpr const TCHAR* MaterialNameSuffix = TEXT(".v3d_impostor");
    inline constexpr const TCHAR* AtlasArrayParamName = TEXT("ImpostorAtlasArray");
    inline constexpr const TCHAR* FramesParamName = TEXT("ImpostorFrames");
    inline constexpr const TCHAR* RadiusParamName = TEXT("ImpostorRadius");
    inline constexpr const TCHAR* CenterParamName = TEXT("ImpostorCenter");

    inline constexpr int32 DefaultGridFrames = 8;
    inline constexpr int32 DefaultFrameResolution = 64;

    V3DRUNTIME_API bool IsImpostorMeshName(const FString& Name);
    V3DRUNTIME_API bool IsImpostorMaterial(const FGWorldBakedMaterial& Material);

    /** Resolves the impostor base material path (archive path -> /V3DRuntime/Materials/M_V3D_Impostor). */
    V3DRUNTIME_API FString ResolveMaterialPath(const UObject* WorldContextObject = nullptr);

    /** Loads the impostor base material on the game thread, falling back cleanly if not yet generated. */
    V3DRUNTIME_API UMaterialInterface* ResolveMaterial(
        const UObject* WorldContextObject = nullptr,
        const FString& ArchiveMaterialPath = FString());

    /**
     * Captures hemi-octahedral impostors for every eligible static mesh group in Metadata.SceneData.MeshMap,
     * appends the baked impostor textures, materials, and billboard quad meshes to Model, and binds each
     * impostor mesh into the LAST LOD slot of its FModelMeshData entry.
     *
     * Runs purely on CPU arrays (thread-safe parallel software rasterizer), so it works identically
     * in Editor, Commandlet, Shipping, Windows, macOS, and Linux builds without GPU readback stalls.
     */
    V3DRUNTIME_API bool Bake(
        FGWorldBakedModel& Model,
        FGWorldModelMetadata& Metadata,
        FString& OutError,
        const UObject* WorldContextObject = nullptr,
        int32 GridFrames = DefaultGridFrames,
        int32 FrameResolution = DefaultFrameResolution);
}
