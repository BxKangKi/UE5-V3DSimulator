// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "System/WorldBakedData.h"

/**
 * Cross-platform (Windows DX12/Vulkan, macOS Apple Silicon Metal, Linux Vulkan)
 * build-time BC1 (DXT1) / BC3 (DXT5) block compression for V3D baked textures.
 * Compresses 8-bit RGBA/BGRA 2D textures and Texture2DArray palettes/atlases
 * directly into hardware-sampled GPU block formats (4x-8x smaller .v3d & VRAM).
 */
namespace V3DTextureCompression
{
    /**
     * Compresses eligible 8-bit RGBA/BGRA textures (including Texture2DArray slices and full mip chains)
     * in-place to PF_DXT1 (opaque, 8x smaller) or PF_DXT5 (alpha-bearing, 4x smaller).
     * Textures with non-4-aligned dimensions, HDR float formats, or already-compressed formats are untouched.
     */
    V3DRUNTIME_API void CompressModelTextures(FGWorldBakedModel& Model);

    /**
     * Decodes a BC1 (PF_DXT1) or BC3 (PF_DXT5) texture (including 2D arrays and mip chains)
     * in-place back to PF_B8G8R8A8 for mobile GPUs (iOS/Android) that lack hardware DXT support.
     */
    V3DRUNTIME_API bool DecompressToBGRA(FGWorldBakedTexture& Texture);
}
