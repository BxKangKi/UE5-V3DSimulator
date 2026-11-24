// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DMaterialTextureArray.h"
#include "System/V3DImpostor.h"
#include "System/V3DTerrain.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture.h"
#include "PixelFormat.h"

namespace V3DMaterialArrayPrivate
{
    inline float SRGBToLinear(const uint8 Byte)
    {
        const float C = static_cast<float>(Byte) / 255.0f;
        return C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
    }

    inline uint8 FloatToByte(const float V)
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(V, 0.0f, 1.0f) * 255.0f), 0, 255));
    }

    inline int32 RoundDownPowerOfTwo(const int32 Value, const int32 MinVal, const int32 MaxVal)
    {
        const int32 Clamped = FMath::Clamp(Value, MinVal, MaxVal);
        int32 P = MinVal;
        while ((P << 1) <= Clamped) P <<= 1;
        return P;
    }

    inline bool IsReplacedPBRTextureParam(const FString& Name)
    {
        return Name.Equals(TEXT("baseColorTexture"), ESearchCase::IgnoreCase)
            || Name.Equals(TEXT("normalTexture"), ESearchCase::IgnoreCase)
            || Name.Equals(TEXT("metallicRoughnessTexture"), ESearchCase::IgnoreCase)
            || Name.Equals(TEXT("occlusionTexture"), ESearchCase::IgnoreCase)
            || Name.Equals(TEXT("emissiveTexture"), ESearchCase::IgnoreCase);
    }

    void SampleSourceSlice(
        const FGWorldBakedTexture* Tex,
        const int32 DstSize,
        const bool bConvertSRGBToLinear,
        const FLinearColor& DefaultFill,
        uint8* OutSliceBGRA)
    {
        const bool bValidPixels = Tex && !Tex->bTextureArray && !Tex->Mips.IsEmpty()
            && Tex->Mips[0].SizeX > 0 && Tex->Mips[0].SizeY > 0
            && (static_cast<EPixelFormat>(Tex->PixelFormat) == PF_B8G8R8A8
                || static_cast<EPixelFormat>(Tex->PixelFormat) == PF_R8G8B8A8)
            && Tex->Mips[0].Bytes.Num() >= static_cast<int64>(Tex->Mips[0].SizeX) * Tex->Mips[0].SizeY * 4;

        if (!bValidPixels)
        {
            const uint8 B = FloatToByte(DefaultFill.B);
            const uint8 G = FloatToByte(DefaultFill.G);
            const uint8 R = FloatToByte(DefaultFill.R);
            const uint8 A = FloatToByte(DefaultFill.A);
            for (int32 I = 0; I < DstSize * DstSize; ++I)
            {
                OutSliceBGRA[I * 4 + 0] = B;
                OutSliceBGRA[I * 4 + 1] = G;
                OutSliceBGRA[I * 4 + 2] = R;
                OutSliceBGRA[I * 4 + 3] = A;
            }
            return;
        }

        const FGWorldBakedTextureMip& Src = Tex->Mips[0];
        const bool bBGRA = (static_cast<EPixelFormat>(Tex->PixelFormat) == PF_B8G8R8A8);
        const bool bDecodeSRGB = bConvertSRGBToLinear && Tex->bSRGB;

        for (int32 Y = 0; Y < DstSize; ++Y)
        {
            const int32 SY = FMath::Clamp((Y * Src.SizeY) / DstSize, 0, Src.SizeY - 1);
            for (int32 X = 0; X < DstSize; ++X)
            {
                const int32 SX = FMath::Clamp((X * Src.SizeX) / DstSize, 0, Src.SizeX - 1);
                const uint8* P = Src.Bytes.GetData() + (static_cast<int64>(SY) * Src.SizeX + SX) * 4;
                const uint8 RByte = bBGRA ? P[2] : P[0];
                const uint8 GByte = P[1];
                const uint8 BByte = bBGRA ? P[0] : P[2];
                const uint8 AByte = P[3];

                uint8* Dst = OutSliceBGRA + (static_cast<int64>(Y) * DstSize + X) * 4;
                Dst[0] = bDecodeSRGB ? FloatToByte(SRGBToLinear(BByte)) : BByte;
                Dst[1] = bDecodeSRGB ? FloatToByte(SRGBToLinear(GByte)) : GByte;
                Dst[2] = bDecodeSRGB ? FloatToByte(SRGBToLinear(RByte)) : RByte;
                Dst[3] = AByte;
            }
        }
    }

    void BuildLinearArrayMips(const int32 BaseSize, const int32 NumLayers, const TArray<uint8>& TopBytes,
        TArray<FGWorldBakedTextureMip>& OutMips, const bool bAllowParallel = true)
    {
        OutMips.Reset();
        OutMips.Reserve(12);
        FGWorldBakedTextureMip Top;
        Top.SizeX = BaseSize;
        Top.SizeY = BaseSize;
        Top.SizeZ = NumLayers;
        Top.Bytes = TopBytes;
        OutMips.Add(MoveTemp(Top));

        int32 Cur = BaseSize;
        while (Cur > 1)
        {
            const FGWorldBakedTextureMip& Prev = OutMips.Last();
            const int32 NextSize = FMath::Max(1, Cur >> 1);
            FGWorldBakedTextureMip Next;
            Next.SizeX = NextSize;
            Next.SizeY = NextSize;
            Next.SizeZ = NumLayers;
            Next.Bytes.SetNumZeroed(static_cast<int64>(NextSize) * NextSize * NumLayers * 4);

            const int64 PrevStride = static_cast<int64>(Prev.SizeX) * Prev.SizeY * 4;
            const int64 NextStride = static_cast<int64>(NextSize) * NextSize * 4;

            const auto ProcessLayer = [&](const int32 Layer)
            {
                const uint8* SrcL = Prev.Bytes.GetData() + Layer * PrevStride;
                uint8* DstL = Next.Bytes.GetData() + Layer * NextStride;
                for (int32 Y = 0; Y < NextSize; ++Y)
                {
                    for (int32 X = 0; X < NextSize; ++X)
                    {
                        uint32 Sum[4] = {0, 0, 0, 0};
                        for (int32 DY = 0; DY < 2; ++DY)
                        {
                            const int32 SY = FMath::Min(Y * 2 + DY, Prev.SizeY - 1);
                            for (int32 DX = 0; DX < 2; ++DX)
                            {
                                const int32 SX = FMath::Min(X * 2 + DX, Prev.SizeX - 1);
                                const uint8* P = SrcL + (static_cast<int64>(SY) * Prev.SizeX + SX) * 4;
                                for (int32 C = 0; C < 4; ++C) Sum[C] += P[C];
                            }
                        }
                        uint8* OutP = DstL + (static_cast<int64>(Y) * NextSize + X) * 4;
                        for (int32 C = 0; C < 4; ++C) OutP[C] = static_cast<uint8>((Sum[C] + 2) >> 2);
                    }
                }
            };
            if (bAllowParallel && NextSize >= 128)
            {
                ParallelFor(NumLayers, ProcessLayer);
            }
            else
            {
                for (int32 Layer = 0; Layer < NumLayers; ++Layer) ProcessLayer(Layer);
            }
            OutMips.Add(MoveTemp(Next));
            Cur = NextSize;
        }
    }
}

bool V3DMaterialTextureArray::PackMaterials(
    FGWorldBakedModel& Model,
    FString& OutError,
    const int32 MaxResolution)
{
    OutError.Reset();
    if (Model.Materials.IsEmpty())
    {
        return true;
    }

    TMap<int32, const FGWorldBakedTexture*> TextureById;
    int32 NextTextureId = 0;
    for (const FGWorldBakedTexture& Tex : Model.Textures)
    {
        TextureById.Add(Tex.TextureId, &Tex);
        NextTextureId = FMath::Max(NextTextureId, Tex.TextureId + 1);
    }

    const int32 CapRes = V3DMaterialArrayPrivate::RoundDownPowerOfTwo(MaxResolution, 64, 2048);
    TArray<FGWorldBakedTexture> PackedTextures;

    for (FGWorldBakedMaterial& Mat : Model.Materials)
    {
        if (V3DTerrain::IsPaletteMaterial(Mat) || V3DImpostor::IsImpostorMaterial(Mat))
        {
            continue;
        }

        const bool bAlreadyPacked = Mat.Textures.ContainsByPredicate([](const FGWorldBakedTextureParameter& P)
        {
            return P.Name.Equals(ArrayTextureParamName, ESearchCase::IgnoreCase);
        });
        if (bAlreadyPacked) continue;

        const FGWorldBakedTexture* BaseColorTex = nullptr;
        const FGWorldBakedTexture* NormalTex = nullptr;
        const FGWorldBakedTexture* ORMTex = nullptr;
        const FGWorldBakedTexture* EmissiveTex = nullptr;

        for (const FGWorldBakedTextureParameter& Param : Mat.Textures)
        {
            const FGWorldBakedTexture* const* Found = TextureById.Find(Param.TextureId);
            if (!Found || !(*Found) || (*Found)->bTextureArray) continue;

            if (Param.Name.Equals(TEXT("baseColorTexture"), ESearchCase::IgnoreCase))
                BaseColorTex = *Found;
            else if (Param.Name.Equals(TEXT("normalTexture"), ESearchCase::IgnoreCase))
                NormalTex = *Found;
            else if (Param.Name.Equals(TEXT("metallicRoughnessTexture"), ESearchCase::IgnoreCase)
                || Param.Name.Equals(TEXT("occlusionTexture"), ESearchCase::IgnoreCase))
                ORMTex = *Found;
            else if (Param.Name.Equals(TEXT("emissiveTexture"), ESearchCase::IgnoreCase))
                EmissiveTex = *Found;
        }

        int32 MaxDim = 0;
        for (const FGWorldBakedTexture* Candidate : { BaseColorTex, NormalTex, ORMTex, EmissiveTex })
        {
            if (Candidate)
            {
                MaxDim = FMath::Max(MaxDim, FMath::Max(Candidate->SizeX, Candidate->SizeY));
            }
        }
        // Solid-color materials with zero source textures still receive a compact 1x1x4 neutral array.
        const int32 TargetSize = MaxDim > 0
            ? V3DMaterialArrayPrivate::RoundDownPowerOfTwo(MaxDim, 1, CapRes)
            : 1;
        const int64 SliceBytes = static_cast<int64>(TargetSize) * TargetSize * 4;

        TArray<uint8> Combined;
        Combined.SetNumZeroed(SliceBytes * SliceCount);

        // Fixed constant slices:
        //   0: BaseColor (RGB) + Opacity (A)
        //   1: Tangent-space Normal (RGB, UNORM [0,1])
        //   2: ORM (Occlusion, Roughness, Metallic)
        //   3: Emissive (RGB)
        const FGWorldBakedTexture* SliceSources[SliceCount] = { BaseColorTex, NormalTex, ORMTex, EmissiveTex };
        const bool SliceSRGB[SliceCount] = { true, false, false, true };
        const FLinearColor SliceDefaults[SliceCount] = {
            FLinearColor::White,
            FLinearColor(0.5f, 0.5f, 1.0f, 1.0f),
            FLinearColor::White,
            FLinearColor::Black
        };
        ParallelFor(SliceCount, [&](const int32 SliceIdx)
        {
            V3DMaterialArrayPrivate::SampleSourceSlice(
                SliceSources[SliceIdx],
                TargetSize,
                SliceSRGB[SliceIdx],
                SliceDefaults[SliceIdx],
                Combined.GetData() + SliceBytes * SliceIdx);
        });

        const int32 ArrayTexId = NextTextureId++;
        FGWorldBakedTexture ArrayTex;
        ArrayTex.TextureId = ArrayTexId;
        ArrayTex.Name = Mat.Name + TEXT("_MaterialArray");
        ArrayTex.SizeX = TargetSize;
        ArrayTex.SizeY = TargetSize;
        ArrayTex.NumLayers = SliceCount;
        ArrayTex.bTextureArray = true;
        ArrayTex.PixelFormat = static_cast<int32>(PF_B8G8R8A8);
        ArrayTex.AddressX = static_cast<uint8>(TA_Wrap);
        ArrayTex.AddressY = static_cast<uint8>(TA_Wrap);
        ArrayTex.Filter = static_cast<uint8>(TF_Trilinear);
        ArrayTex.LODGroup = static_cast<uint8>(TEXTUREGROUP_World);
        ArrayTex.bSRGB = false;
        V3DMaterialArrayPrivate::BuildLinearArrayMips(TargetSize, SliceCount, Combined, ArrayTex.Mips);

        PackedTextures.Add(MoveTemp(ArrayTex));

        // Replace separate Texture2D PBR slots with the single Texture2DArray parameter.
        Mat.Textures.RemoveAll([](const FGWorldBakedTextureParameter& P)
        {
            return V3DMaterialArrayPrivate::IsReplacedPBRTextureParam(P.Name);
        });
        Mat.Textures.Add({ ArrayTextureParamName, ArrayTexId });
    }

    if (!PackedTextures.IsEmpty())
    {
        Model.Textures.Append(MoveTemp(PackedTextures));
    }

    // Prune unreferenced separate Texture2D entries and compact TextureIds cleanly.
    TSet<int32> ReferencedIds;
    for (const FGWorldBakedMaterial& Mat : Model.Materials)
    {
        for (const FGWorldBakedTextureParameter& Param : Mat.Textures)
        {
            if (Param.TextureId != INDEX_NONE)
            {
                ReferencedIds.Add(Param.TextureId);
            }
        }
    }

    TArray<FGWorldBakedTexture> CompactTextures;
    CompactTextures.Reserve(ReferencedIds.Num());
    TMap<int32, int32> OldToNewId;
    for (FGWorldBakedTexture& Tex : Model.Textures)
    {
        if (!ReferencedIds.Contains(Tex.TextureId)) continue;
        const int32 NewId = CompactTextures.Num();
        OldToNewId.Add(Tex.TextureId, NewId);
        Tex.TextureId = NewId;
        CompactTextures.Add(MoveTemp(Tex));
    }
    Model.Textures = MoveTemp(CompactTextures);

    for (FGWorldBakedMaterial& Mat : Model.Materials)
    {
        for (FGWorldBakedTextureParameter& Param : Mat.Textures)
        {
            if (const int32* Mapped = OldToNewId.Find(Param.TextureId))
            {
                Param.TextureId = *Mapped;
            }
        }
    }

    return true;
}

bool V3DMaterialTextureArray::PackBundle(FGWorldBakedAssetBundle& Bundle, TFunctionRef<bool()> ShouldCancel)
{
    if (Bundle.Materials.IsEmpty())
    {
        return !ShouldCancel();
    }

    TMap<int32, const FGWorldBakedTexture*> TextureById;
    TextureById.Reserve(Bundle.Textures.Num());
    for (const FGWorldBakedTexture& Tex : Bundle.Textures)
    {
        TextureById.Add(Tex.TextureId, &Tex);
    }

    const int32 CapRes = Bundle.MaxTextureResolution > 0
        ? V3DMaterialArrayPrivate::RoundDownPowerOfTwo(Bundle.MaxTextureResolution, 64, 2048)
        : 1024;

    TArray<FGWorldBakedTexture> PackedArrays;
    PackedArrays.Reserve(Bundle.Materials.Num());
    TSet<int32> ReplacedTextureIds;

    for (FGWorldBakedMaterial& Mat : Bundle.Materials)
    {
        if (ShouldCancel()) return false;
        if (V3DTerrain::IsPaletteMaterial(Mat) || V3DImpostor::IsImpostorMaterial(Mat))
        {
            continue;
        }
        const bool bAlreadyHasArray = Mat.Textures.ContainsByPredicate([](const FGWorldBakedTextureParameter& P)
        {
            return P.Name.Equals(ArrayTextureParamName, ESearchCase::IgnoreCase);
        });
        if (bAlreadyHasArray) continue;

        const FGWorldBakedTexture* BaseColorTex = nullptr;
        const FGWorldBakedTexture* NormalTex = nullptr;
        const FGWorldBakedTexture* ORMTex = nullptr;
        const FGWorldBakedTexture* EmissiveTex = nullptr;

        for (const FGWorldBakedTextureParameter& Param : Mat.Textures)
        {
            const FGWorldBakedTexture* const* Found = TextureById.Find(Param.TextureId);
            if (!Found || !(*Found) || (*Found)->bTextureArray) continue;

            if (Param.Name.Equals(TEXT("baseColorTexture"), ESearchCase::IgnoreCase))
                BaseColorTex = *Found;
            else if (Param.Name.Equals(TEXT("normalTexture"), ESearchCase::IgnoreCase))
                NormalTex = *Found;
            else if (Param.Name.Equals(TEXT("metallicRoughnessTexture"), ESearchCase::IgnoreCase)
                || Param.Name.Equals(TEXT("occlusionTexture"), ESearchCase::IgnoreCase))
                ORMTex = *Found;
            else if (Param.Name.Equals(TEXT("emissiveTexture"), ESearchCase::IgnoreCase))
                EmissiveTex = *Found;
        }

        int32 MaxDim = 0;
        for (const FGWorldBakedTexture* Candidate : { BaseColorTex, NormalTex, ORMTex, EmissiveTex })
        {
            if (Candidate && Candidate->SizeX > 0 && Candidate->SizeY > 0)
            {
                MaxDim = FMath::Max(MaxDim, FMath::Max(Candidate->SizeX, Candidate->SizeY));
            }
        }
        const int32 TargetSize = MaxDim > 0
            ? V3DMaterialArrayPrivate::RoundDownPowerOfTwo(MaxDim, 1, CapRes)
            : 1;
        const int64 SliceBytes = static_cast<int64>(TargetSize) * TargetSize * 4;
        if (SliceBytes <= 0 || SliceBytes * SliceCount > 128ll * 1024ll * 1024ll)
        {
            continue;
        }

        TArray<uint8> Combined;
        Combined.SetNumUninitialized(SliceBytes * SliceCount);

        const FGWorldBakedTexture* SliceSources[SliceCount] = { BaseColorTex, NormalTex, ORMTex, EmissiveTex };
        const bool SliceSRGB[SliceCount] = { true, false, false, true };
        const FLinearColor SliceDefaults[SliceCount] = {
            FLinearColor::White,
            FLinearColor(0.5f, 0.5f, 1.0f, 1.0f),
            FLinearColor::White,
            FLinearColor::Black
        };
        // Avoid nested ParallelFor oversubscription on background streaming workers so GT/RT stay smooth.
        for (int32 SliceIdx = 0; SliceIdx < SliceCount; ++SliceIdx)
        {
            if (ShouldCancel()) return false;
            V3DMaterialArrayPrivate::SampleSourceSlice(
                SliceSources[SliceIdx],
                TargetSize,
                SliceSRGB[SliceIdx],
                SliceDefaults[SliceIdx],
                Combined.GetData() + SliceBytes * SliceIdx);
        }

        if (ShouldCancel()) return false;

        // Deterministic runtime-only TextureId in upper positive range so it never collides with archive TextureIds.
        const int32 RuntimeArrayTexId = static_cast<int32>(0x40000000u | (static_cast<uint32>(Mat.MaterialId) & 0x3FFFFFFFu));
        FGWorldBakedTexture ArrayTex;
        ArrayTex.TextureId = RuntimeArrayTexId;
        ArrayTex.Name = Mat.Name + TEXT("_RuntimeMaterialArray");
        ArrayTex.SizeX = TargetSize;
        ArrayTex.SizeY = TargetSize;
        ArrayTex.NumLayers = SliceCount;
        ArrayTex.bTextureArray = true;
        ArrayTex.PixelFormat = static_cast<int32>(PF_B8G8R8A8);
        ArrayTex.AddressX = static_cast<uint8>(TA_Wrap);
        ArrayTex.AddressY = static_cast<uint8>(TA_Wrap);
        ArrayTex.Filter = static_cast<uint8>(TF_Trilinear);
        ArrayTex.LODGroup = static_cast<uint8>(TEXTUREGROUP_World);
        ArrayTex.bSRGB = false;
        V3DMaterialArrayPrivate::BuildLinearArrayMips(TargetSize, SliceCount, Combined, ArrayTex.Mips, false);
        Combined.Empty();

        PackedArrays.Add(MoveTemp(ArrayTex));

        for (const FGWorldBakedTextureParameter& P : Mat.Textures)
        {
            if (V3DMaterialArrayPrivate::IsReplacedPBRTextureParam(P.Name) && P.TextureId != INDEX_NONE)
            {
                ReplacedTextureIds.Add(P.TextureId);
            }
        }
        Mat.Textures.RemoveAll([](const FGWorldBakedTextureParameter& P)
        {
            return V3DMaterialArrayPrivate::IsReplacedPBRTextureParam(P.Name);
        });
        Mat.Textures.Add({ ArrayTextureParamName, RuntimeArrayTexId });
    }

    if (PackedArrays.IsEmpty())
    {
        return !ShouldCancel();
    }

    TSet<int32> StillReferenced;
    for (const FGWorldBakedMaterial& Mat : Bundle.Materials)
    {
        for (const FGWorldBakedTextureParameter& P : Mat.Textures)
        {
            if (P.TextureId != INDEX_NONE) StillReferenced.Add(P.TextureId);
        }
    }
    Bundle.Textures.RemoveAll([&StillReferenced, &ReplacedTextureIds](const FGWorldBakedTexture& Tex)
    {
        return ReplacedTextureIds.Contains(Tex.TextureId) && !StillReferenced.Contains(Tex.TextureId);
    });
    Bundle.Textures.Append(MoveTemp(PackedArrays));
    return !ShouldCancel();
}
