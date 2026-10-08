// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldBakedTexture.h"
#include "Misc/ScopeExit.h"

bool UWorldBakedTextureMipProvider::InitializeArchiveSource(
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& InReader,
    const FGWorldArchiveRange& InRange, FGWorldBakedTexture& Baked,
    const int32 FirstMip, const int32 InResolution)
{
    check(IsInGameThread());
    if (Reader.IsValid() || !InReader.IsValid() || FirstMip < 0 || FirstMip >= Baked.Mips.Num()) return false;
    const int32 Count = Baked.Mips.Num() - FirstMip;
    if (Count > int32(FStreamableRenderResourceState::MAX_LOD_COUNT)) return false;
    for (int32 Index = FirstMip; Index < Baked.Mips.Num(); ++Index)
    {
        const auto& Mip = Baked.Mips[Index];
        if (Mip.SizeX <= 0 || Mip.SizeY <= 0 || Mip.SizeZ != 1 || Mip.Bytes.IsEmpty()) return false;
    }

    Reader = InReader;
    Range = InRange;
    TextureId = Baked.TextureId;
    Resolution = InResolution;
    PixelFormat = Baked.PixelFormat;
    MipLayout.Reserve(Count);
    for (int32 Index = FirstMip; Index < Baked.Mips.Num(); ++Index)
    {
        const auto& Mip = Baked.Mips[Index];
        MipLayout.Add({Mip.SizeX, Mip.SizeY, Mip.Bytes.Num()});
    }
    PreparedMips = MoveTemp(Baked.Mips);
    if (FirstMip > 0) PreparedMips.RemoveAt(0, FirstMip, EAllowShrinking::No);
    return true;
}

FStreamableRenderResourceState UWorldBakedTextureMipProvider::GetResourcePostInitState(
    const UTexture* /*Owner*/, const bool /*bAllowStreaming*/)
{
    FStreamableRenderResourceState State;
    State.Data = 0;
    if (MipLayout.IsEmpty()) return State;
    const uint8 Count = uint8(MipLayout.Num());
    State.MaxNumLODs = Count;
    State.NumNonStreamingLODs = Count;
    State.NumNonOptionalLODs = Count;
    State.NumResidentLODs = Count;
    State.NumRequestedLODs = Count;
    State.bHasPendingInitHint = true;
    // Streaming flags stay false; no external streaming task can own these arrays.
    return State;
}

bool UWorldBakedTextureMipProvider::GetInitialMipData(
    const int32 FirstMipToLoad, TArrayView<void*> OutMipData,
    TArrayView<int64> OutMipSize, FStringView /*DebugContext*/)
{
    check(IsInGameThread());
    // Successful output allocations belong to the engine, never to this UObject or archive.
    for (void*& Data : OutMipData) Data = nullptr;
    for (int64& Size : OutMipSize) Size = 0;
    if (FirstMipToLoad < 0 || FirstMipToLoad >= MipLayout.Num()) return false;
    const int32 Count = MipLayout.Num() - FirstMipToLoad;
    if (OutMipData.Num() < Count || (!OutMipSize.IsEmpty() && OutMipSize.Num() < Count)) return false;

    TArray<FGWorldBakedTextureMip> SourceMips;
    if (!PreparedMips.IsEmpty())
    {
        SourceMips = MoveTemp(PreparedMips); // Initial async decode; no extra BulkData copy.
    }
    else
    {
        // Rare resource recreation. Independent arrays/file cursors cannot overwrite memory
        // still owned by a previous render-resource initialization.
        FGWorldBakedTexture Baked;
        FString Error;
        if (!Reader.IsValid() || !Reader->ReadTextureRange(Range, Baked, Error, Resolution)
            || Baked.TextureId != TextureId || Baked.PixelFormat != PixelFormat
            || Baked.SizeX != MipLayout[0].SizeX || Baked.SizeY != MipLayout[0].SizeY)
        {
            UE_LOG(LogTemp, Error, TEXT("Cannot restore baked texture %d: %s"), TextureId, *Error);
            return false;
        }
        SourceMips = MoveTemp(Baked.Mips);
    }
    if (SourceMips.Num() != MipLayout.Num()) return false;
    // Validate every source mip before allocating/publishing any engine-owned pointer.
    for (int32 Index = 0; Index < MipLayout.Num(); ++Index)
    {
        const auto& Source = SourceMips[Index];
        const auto& Expected = MipLayout[Index];
        if (Source.SizeX != Expected.SizeX || Source.SizeY != Expected.SizeY || Source.SizeZ != 1
            || Source.Bytes.Num() != Expected.Bytes) return false;
    }

    bool bTransferred = false;
    ON_SCOPE_EXIT
    {
        if (!bTransferred)
        {
            for (void*& Data : OutMipData) { FMemory::Free(Data); Data = nullptr; }
            for (int64& Size : OutMipSize) Size = 0;
        }
    };
    for (int32 Index = 0; Index < FirstMipToLoad; ++Index) SourceMips[Index].Bytes.Empty();
    for (int32 Offset = 0; Offset < Count; ++Offset)
    {
        auto& Source = SourceMips[FirstMipToLoad + Offset];
        const int64 Bytes = Source.Bytes.Num();
        void* Data = FMemory::Malloc(SIZE_T(Bytes));
        if (!Data) return false;
        FMemory::Memcpy(Data, Source.Bytes.GetData(), SIZE_T(Bytes));
        OutMipData[Offset] = Data;
        if (!OutMipSize.IsEmpty()) OutMipSize[Offset] = Bytes;
        Source.Bytes.Empty(); // Only the engine's upload allocation remains.
    }
    bTransferred = true;
    return true;
}

int64 UWorldBakedTextureMipProvider::GetRetainedMipBytes() const
{
    int64 Bytes = 0;
    for (const auto& Mip : PreparedMips) Bytes += Mip.Bytes.Num();
    return Bytes;
}
