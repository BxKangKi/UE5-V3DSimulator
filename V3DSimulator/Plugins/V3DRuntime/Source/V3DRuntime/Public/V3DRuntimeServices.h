#pragma once
#include "CoreMinimal.h"
/** Optional application policy. The plugin works independently with safe defaults. Game-thread only. */
struct V3DRUNTIME_API FV3DRuntimeServices
{
    static TFunction<int32(const UObject*)> TextureLimit;
    static TFunction<FString(const UObject*)> TerrainPath;
    static int32 MaxTextureResolution(const UObject* Context) { return TextureLimit ? TextureLimit(Context) : 2048; }
    static FString TerrainMaterialPath(const UObject* Context) { return TerrainPath ? TerrainPath(Context) : FString(); }
};
