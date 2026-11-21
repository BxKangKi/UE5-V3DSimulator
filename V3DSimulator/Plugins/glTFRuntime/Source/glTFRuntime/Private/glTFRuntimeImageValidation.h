// Validate decoded image layouts before CPU copies or render-resource publication.
#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "glTFRuntimeParser.h"

namespace glTFRuntimeImageValidation
{
    // Per-resource admission limits, not a substitute for the world's streaming budget.
    constexpr int64 MaxResourceBytes = 512ll * 1024 * 1024;
    constexpr int32 MaxDimension = 16384;

    inline bool GetByteSize(const int64 Width, const int64 Height, const EPixelFormat Format,
        int64& OutBytes, const int64 Depth = 1)
    {
        OutBytes = 0;
        if (Width <= 0 || Height <= 0 || Depth <= 0 || Width > MaxDimension
            || Height > MaxDimension || Depth > 2048 || Format <= PF_Unknown || Format >= PF_MAX)
            return false;
        const FPixelFormatInfo& Info = GPixelFormats[Format];
        if (Info.BlockSizeX == 0 || Info.BlockSizeY == 0 || Info.BlockSizeZ == 0 || Info.BlockBytes == 0)
            return false;
        // Bounded dimensions and 64-bit arithmetic prevent integer overflow before the limit check.
        const int64 BlocksX = (Width + Info.BlockSizeX - 1) / Info.BlockSizeX;
        const int64 BlocksY = (Height + Info.BlockSizeY - 1) / Info.BlockSizeY;
        const int64 BlocksZ = (Depth + Info.BlockSizeZ - 1) / Info.BlockSizeZ;
        const int64 Bytes = BlocksX * BlocksY * BlocksZ * Info.BlockBytes;
        if (Bytes <= 0 || Bytes > MaxResourceBytes) return false;
        OutBytes = Bytes;
        return true;
    }

    inline bool ValidateMip(const FglTFRuntimeMipMap& Mip, const int32 Depth = 1)
    {
        int64 Expected = 0;
        return GetByteSize(Mip.Width, Mip.Height, Mip.PixelFormat, Expected, Depth)
            && Mip.Pixels.Num() == Expected;
    }

    inline bool ValidateChain(const TArray<FglTFRuntimeMipMap>& Mips, const int32 Depth = 1)
    {
        if (Mips.IsEmpty() || !ValidateMip(Mips[0], Depth)) return false;
        const int32 MaxMips = FMath::FloorLog2(FMath::Max3(Mips[0].Width, Mips[0].Height, Depth)) + 1;
        if (Mips.Num() > MaxMips) return false;
        int64 TotalBytes = 0;
        for (int32 Index = 0; Index < Mips.Num(); ++Index)
        {
            const auto& Mip = Mips[Index];
            if (!ValidateMip(Mip, FMath::Max(1, Depth >> Index))
                || Mip.Width != FMath::Max(1, Mips[0].Width >> Index)
                || Mip.Height != FMath::Max(1, Mips[0].Height >> Index)
                || Mip.PixelFormat != Mips[0].PixelFormat) return false;
            if (Mip.Pixels.Num() > MaxResourceBytes - TotalBytes) return false;
            TotalBytes += Mip.Pixels.Num();
        }
        return true;
    }

    inline FIntPoint FitWithin(const int32 Width, const int32 Height, const int32 MaxWidth, const int32 MaxHeight)
    {
        if (Width <= 0 || Height <= 0) return FIntPoint::ZeroValue;
        double Scale = 1.0;
        if (MaxWidth > 0) Scale = FMath::Min(Scale, double(MaxWidth) / Width);
        if (MaxHeight > 0) Scale = FMath::Min(Scale, double(MaxHeight) / Height);
        return FIntPoint(FMath::Max(1, FMath::FloorToInt(Width * Scale)),
            FMath::Max(1, FMath::FloorToInt(Height * Scale)));
    }
}
