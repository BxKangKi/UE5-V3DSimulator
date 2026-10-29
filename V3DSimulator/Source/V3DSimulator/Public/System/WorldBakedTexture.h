// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureAllMipDataProviderFactory.h"
#include "Streaming/StreamableRenderResourceState.h"
#include "System/WorldArchive.h"
#include "WorldBakedTexture.generated.h"

/**
 * Archive-backed mip source attached to an ordinary UTexture2D as asset user data.
 * UTexture2D's MinimalAPI virtuals are not exported by the installed Mac Engine.
 * Use the engine's mip-source interface instead of deriving a project texture.
 */
UCLASS(Transient)
class V3DSIMULATOR_API UWorldBakedTextureMipProvider final : public UTextureAllMipDataProviderFactory
{
    GENERATED_BODY()
public:
    // Baked has passed the facade's pixel-format/byte-count validation. Consumes mips.
    bool InitializeArchiveSource(const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& InReader,
        const FGWorldArchiveRange& InRange, FGWorldBakedTexture& Baked,
        int32 FirstMip, int32 InResolution);

    virtual bool GetInitialMipData(int32 FirstMipToLoad, TArrayView<void*> OutMipData,
        TArrayView<int64> OutMipSize, FStringView DebugContext) override;
    virtual FStreamableRenderResourceState GetResourcePostInitState(
        const UTexture* Owner, bool bAllowStreaming) override;

    // Selected mips stay resident until the mesh/material unloads. No cooked-file streamer.
    virtual FTextureMipDataProvider* AllocateMipDataProvider(UTexture* Asset) override { return nullptr; }
    virtual bool WillProvideMipDataWithoutDisk() const override { return false; }
    virtual bool ShouldAllowPlatformTiling(const UTexture* Owner) const override { return false; }

    int64 GetRetainedMipBytes() const;

private:
    struct FMipLayout
    {
        int32 SizeX = 0;
        int32 SizeY = 0;
        int64 Bytes = 0;
    };
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader;
    FGWorldArchiveRange Range;
    int32 TextureId = INDEX_NONE;
    int32 Resolution = 0;
    int32 PixelFormat = 0;
    TArray<FMipLayout> MipLayout;
    // Only survives until the first GetInitialMipData. PlatformData bulk stores no pixels.
    TArray<FGWorldBakedTextureMip> PreparedMips;
};
