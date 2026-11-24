// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "System/WorldBakedData.h"

/**
 * Unified Texture2DArray material texture packer for V3DRuntime.
 *
 * Every non-terrain, non-impostor V3D material binds a single 4-slice linear PF_B8G8R8A8
 * UTexture2DArray parameter named "V3D_MaterialArray" accessed via compile-time slice constants:
 *   Slice 0: BaseColor (RGB, linear) + Opacity (A)
 *   Slice 1: Tangent-space Normal (RGB, UNORM [0,1])
 *   Slice 2: ORM (R=Occlusion, G=Roughness, B=Metallic)
 *   Slice 3: Emissive (RGB, linear)
 *
 * Separate Texture2D PBR slots are replaced and unreferenced source Texture2D entries are pruned
 * during project build so .v3d archives store no duplicate textures.
 */
namespace V3DMaterialTextureArray
{
    inline constexpr const TCHAR* ArrayTextureParamName = TEXT("V3D_MaterialArray");

    inline constexpr int32 SliceBaseColor = 0;
    inline constexpr int32 SliceNormal = 1;
    inline constexpr int32 SliceORM = 2;
    inline constexpr int32 SliceEmissive = 3;
    inline constexpr int32 SliceCount = 4;

    /**
     * Build-time pass: converts every standard PBR material's textures into a single 4-slice
     * linear Texture2DArray ("V3D_MaterialArray") and prunes unreferenced separate Texture2D entries.
     */
    /**
     * Unreal load-time pass: reads separate Texture2D entries from a loaded .v3d bundle and
     * assembles each standard PBR material's 4-slice linear Texture2DArray ("V3D_MaterialArray")
     * in memory for Unreal rendering, while keeping the .v3d archive format itself portable
     * with separate textures intact.
     */
    V3DRUNTIME_API void PackBundle(FGWorldBakedAssetBundle& Bundle);

    /** Optional build-time packer (if a caller explicitly wants pre-packed arrays in an archive). */
    V3DRUNTIME_API bool PackMaterials(
        FGWorldBakedModel& Model,
        FString& OutError,
        int32 MaxResolution = 1024);
}
