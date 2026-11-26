// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DImpostor.h"
#include "System/V3DTerrain.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"

namespace V3DImpostorPrivate
{
    inline float SRGBToLinearChannel(const uint8 Value)
    {
        const float C = static_cast<float>(Value) / 255.0f;
        return C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
    }

    inline uint8 LinearToSRGBByte(const float Linear)
    {
        const float C = FMath::Clamp(Linear, 0.0f, 1.0f);
        const float S = C <= 0.0031308f ? C * 12.92f : 1.055f * FMath::Pow(C, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(S * 255.0f), 0, 255));
    }

    inline uint8 FloatToByte(const float Value)
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
    }

    // Matches V3D_HemiOctahedronToDir in v3d.hlsl verbatim.
    inline FVector3f HemiOctahedronToDir(const float U, const float V)
    {
        const float OctX = U * 2.0f - 1.0f;
        const float OctY = V * 2.0f - 1.0f;
        const float TempX = (OctX + OctY) * 0.5f;
        const float TempY = (OctX - OctY) * 0.5f;
        const float Z = 1.0f - FMath::Abs(TempX) - FMath::Abs(TempY);
        FVector3f Dir(TempX, TempY, FMath::Max(Z, 0.0f));
        return Dir.GetSafeNormal(1.0e-6f, FVector3f(0.0f, 0.0f, 1.0f));
    }

    inline FLinearColor SampleBakedTextureBilinear(const FGWorldBakedTexture* Tex, const float U, const float V)
    {
        if (!Tex || Tex->Mips.IsEmpty()) return FLinearColor::White;
        const FGWorldBakedTextureMip& Mip = Tex->Mips[0];
        if (Mip.SizeX <= 0 || Mip.SizeY <= 0) return FLinearColor::White;
        const int64 ExpectedBytes = static_cast<int64>(Mip.SizeX) * Mip.SizeY * 4;
        if (Mip.Bytes.Num() < ExpectedBytes) return FLinearColor::White;
        const EPixelFormat Format = static_cast<EPixelFormat>(Tex->PixelFormat);
        const bool bBGRA = (Format == PF_B8G8R8A8);
        const bool bRGBA = (Format == PF_R8G8B8A8);
        if (!bBGRA && !bRGBA) return FLinearColor::White;

        const float SafeU = FMath::IsFinite(U) ? U : 0.0f;
        const float SafeV = FMath::IsFinite(V) ? V : 0.0f;
        const float WrappedU = SafeU - FMath::FloorToFloat(SafeU);
        const float WrappedV = SafeV - FMath::FloorToFloat(SafeV);
        const int32 X = FMath::Clamp(FMath::FloorToInt(WrappedU * Mip.SizeX), 0, Mip.SizeX - 1);
        const int32 Y = FMath::Clamp(FMath::FloorToInt(WrappedV * Mip.SizeY), 0, Mip.SizeY - 1);
        const uint8* Pixel = Mip.Bytes.GetData() + (static_cast<int64>(Y) * Mip.SizeX + X) * 4;

        const uint8 RByte = bBGRA ? Pixel[2] : Pixel[0];
        const uint8 GByte = Pixel[1];
        const uint8 BByte = bBGRA ? Pixel[0] : Pixel[2];
        const uint8 AByte = Pixel[3];

        return FLinearColor(
            Tex->bSRGB ? SRGBToLinearChannel(RByte) : RByte / 255.0f,
            Tex->bSRGB ? SRGBToLinearChannel(GByte) : GByte / 255.0f,
            Tex->bSRGB ? SRGBToLinearChannel(BByte) : BByte / 255.0f,
            AByte / 255.0f);
    }

    struct FResolvedPrimitiveSurface
    {
        const FGWorldBakedTexture* BaseColorTexture = nullptr;
        FLinearColor BaseColorFactor = FLinearColor::White;
        float RoughnessFactor = 0.75f;
        bool bHasAlphaMask = false;
    };

    FResolvedPrimitiveSurface ResolveSurface(
        const FGWorldBakedPrimitive& Primitive,
        const TMap<int32, const FGWorldBakedMaterial*>& MaterialMap,
        const TMap<int32, const FGWorldBakedTexture*>& TextureMap)
    {
        FResolvedPrimitiveSurface Surface;
        const FGWorldBakedMaterial* const* FoundMat = MaterialMap.Find(Primitive.MaterialId);
        if (!FoundMat || !(*FoundMat)) return Surface;
        const FGWorldBakedMaterial& Mat = **FoundMat;
        Surface.bHasAlphaMask = (Mat.MaterialType != 0);

        for (const FGWorldBakedVectorParameter& Vec : Mat.Vectors)
        {
            if (Vec.Name.Equals(TEXT("baseColorFactor"), ESearchCase::IgnoreCase))
            {
                Surface.BaseColorFactor = Vec.Value;
            }
        }
        for (const FGWorldBakedScalarParameter& Sc : Mat.Scalars)
        {
            if (Sc.Name.Equals(TEXT("roughnessFactor"), ESearchCase::IgnoreCase))
            {
                Surface.RoughnessFactor = FMath::Clamp(Sc.Value, 0.04f, 1.0f);
            }
        }
        for (const FGWorldBakedTextureParameter& TP : Mat.Textures)
        {
            if (TP.Name.Equals(TEXT("baseColorTexture"), ESearchCase::IgnoreCase))
            {
                if (const FGWorldBakedTexture* const* FoundTex = TextureMap.Find(TP.TextureId))
                {
                    Surface.BaseColorTexture = *FoundTex;
                }
                break;
            }
        }
        return Surface;
    }

    void BuildMipChainBGRA(
        const int32 BaseSize,
        const int32 NumLayers,
        const TArray<uint8>& TopLevelBytes,
        const bool bSRGB,
        TArray<FGWorldBakedTextureMip>& OutMips)
    {
        OutMips.Reset();
        FGWorldBakedTextureMip Top;
        Top.SizeX = BaseSize;
        Top.SizeY = BaseSize;
        Top.SizeZ = NumLayers;
        Top.Bytes = TopLevelBytes;
        OutMips.Add(MoveTemp(Top));

        int32 CurSize = BaseSize;
        while (CurSize > 1)
        {
            const FGWorldBakedTextureMip& Prev = OutMips.Last();
            const int32 NextSize = FMath::Max(1, CurSize >> 1);
            FGWorldBakedTextureMip Next;
            Next.SizeX = NextSize;
            Next.SizeY = NextSize;
            Next.SizeZ = NumLayers;
            Next.Bytes.SetNumZeroed(static_cast<int64>(NextSize) * NextSize * NumLayers * 4);

            const int64 PrevSliceStride = static_cast<int64>(Prev.SizeX) * Prev.SizeY * 4;
            const int64 NextSliceStride = static_cast<int64>(NextSize) * NextSize * 4;

            for (int32 Layer = 0; Layer < NumLayers; ++Layer)
            {
                const uint8* SrcSlice = Prev.Bytes.GetData() + Layer * PrevSliceStride;
                uint8* DstSlice = Next.Bytes.GetData() + Layer * NextSliceStride;
                for (int32 Y = 0; Y < NextSize; ++Y)
                {
                    for (int32 X = 0; X < NextSize; ++X)
                    {
                        float SumR = 0.0f, SumG = 0.0f, SumB = 0.0f, SumA = 0.0f;
                        for (int32 DY = 0; DY < 2; ++DY)
                        {
                            const int32 SY = FMath::Min(Y * 2 + DY, Prev.SizeY - 1);
                            for (int32 DX = 0; DX < 2; ++DX)
                            {
                                const int32 SX = FMath::Min(X * 2 + DX, Prev.SizeX - 1);
                                const uint8* P = SrcSlice + (static_cast<int64>(SY) * Prev.SizeX + SX) * 4;
                                SumB += bSRGB ? SRGBToLinearChannel(P[0]) : P[0] / 255.0f;
                                SumG += bSRGB ? SRGBToLinearChannel(P[1]) : P[1] / 255.0f;
                                SumR += bSRGB ? SRGBToLinearChannel(P[2]) : P[2] / 255.0f;
                                SumA += P[3] / 255.0f;
                            }
                        }
                        uint8* OutP = DstSlice + (static_cast<int64>(Y) * NextSize + X) * 4;
                        OutP[0] = bSRGB ? LinearToSRGBByte(SumB * 0.25f) : FloatToByte(SumB * 0.25f);
                        OutP[1] = bSRGB ? LinearToSRGBByte(SumG * 0.25f) : FloatToByte(SumG * 0.25f);
                        OutP[2] = bSRGB ? LinearToSRGBByte(SumR * 0.25f) : FloatToByte(SumR * 0.25f);
                        OutP[3] = FloatToByte(SumA * 0.25f);
                    }
                }
            }
            OutMips.Add(MoveTemp(Next));
            CurSize = NextSize;
        }
    }

    /**
     * Captures an N x N hemi-octahedral impostor atlas for one baked mesh on CPU.
     * Slice 0 (ColorSlice): BGRA8 linear BaseColor (RGB) + Alpha mask (A).
     * Slice 1 (NormalSlice): BGRA8 linear encoded Object-space Normal (RGB in [0,1]) + Roughness (A).
     */
    bool CaptureOctahedralAtlas(
        const FGWorldBakedMesh& SourceMesh,
        const TMap<int32, const FGWorldBakedMaterial*>& MaterialMap,
        const TMap<int32, const FGWorldBakedTexture*>& TextureMap,
        const FVector3f& Center,
        const float Radius,
        const int32 GridFrames,
        const int32 FrameRes,
        TArray<uint8>& OutColorSlice,
        TArray<uint8>& OutNormalSlice)
    {
        const int32 AtlasSize = GridFrames * FrameRes;
        if (AtlasSize <= 0 || Radius <= 1.0e-4f) return false;

        OutColorSlice.SetNumZeroed(static_cast<int64>(AtlasSize) * AtlasSize * 4);
        OutNormalSlice.SetNumZeroed(static_cast<int64>(AtlasSize) * AtlasSize * 4);

        // Pre-resolve surfaces per primitive so ParallelFor cells remain lock-free.
        TArray<FResolvedPrimitiveSurface> Surfaces;
        Surfaces.Reserve(SourceMesh.Primitives.Num());
        for (const FGWorldBakedPrimitive& Prim : SourceMesh.Primitives)
        {
            Surfaces.Add(ResolveSurface(Prim, MaterialMap, TextureMap));
        }

        const int32 TotalCells = GridFrames * GridFrames;
        const float InvRadius = 1.0f / Radius;

        ParallelFor(TotalCells, [&](const int32 CellIndex)
        {
            const int32 CellX = CellIndex % GridFrames;
            const int32 CellY = CellIndex / GridFrames;
            const float OctU = GridFrames > 1 ? static_cast<float>(CellX) / static_cast<float>(GridFrames - 1) : 0.5f;
            const float OctV = GridFrames > 1 ? static_cast<float>(CellY) / static_cast<float>(GridFrames - 1) : 0.5f;

            const FVector3f Forward = HemiOctahedronToDir(OctU, OctV);
            const FVector3f WorldUp = FMath::Abs(Forward.Z) < 0.995f
                ? FVector3f(0.0f, 0.0f, 1.0f)
                : FVector3f(0.0f, 1.0f, 0.0f);
            const FVector3f Right = FVector3f::CrossProduct(WorldUp, Forward).GetSafeNormal();
            const FVector3f Up = FVector3f::CrossProduct(Forward, Right).GetSafeNormal();

            TArray<float> DepthBuffer;
            DepthBuffer.Init(-FLT_MAX, FrameRes * FrameRes);

            const int32 OffsetX = CellX * FrameRes;
            const int32 OffsetY = CellY * FrameRes;

            const bool bHasPrimTransforms = SourceMesh.AdditionalTransforms.Num() == SourceMesh.Primitives.Num();
            for (int32 PrimIdx = 0; PrimIdx < SourceMesh.Primitives.Num(); ++PrimIdx)
            {
                const FGWorldBakedPrimitive& Prim = SourceMesh.Primitives[PrimIdx];
                if (Prim.Mode != 4 || Prim.Positions.Num() < 3) continue;
                const FTransform* PrimXform = bHasPrimTransforms ? &SourceMesh.AdditionalTransforms[PrimIdx] : nullptr;
                const FResolvedPrimitiveSurface& Surface = Surfaces[PrimIdx];
                const bool bHasUV0 = !Prim.UVs.IsEmpty() && Prim.UVs[0].Num() == Prim.Positions.Num();
                const bool bHasNormals = Prim.Normals.Num() == Prim.Positions.Num();
                const bool bIndexed = Prim.bHasIndices && Prim.Indices.Num() >= 3;
                const int32 TriCount = bIndexed ? (Prim.Indices.Num() / 3) : (Prim.Positions.Num() / 3);

                for (int32 Tri = 0; Tri < TriCount; ++Tri)
                {
                    const uint32 I0 = bIndexed ? Prim.Indices[Tri * 3 + 0] : static_cast<uint32>(Tri * 3 + 0);
                    const uint32 I1 = bIndexed ? Prim.Indices[Tri * 3 + 1] : static_cast<uint32>(Tri * 3 + 1);
                    const uint32 I2 = bIndexed ? Prim.Indices[Tri * 3 + 2] : static_cast<uint32>(Tri * 3 + 2);
                    if (I0 >= static_cast<uint32>(Prim.Positions.Num())
                        || I1 >= static_cast<uint32>(Prim.Positions.Num())
                        || I2 >= static_cast<uint32>(Prim.Positions.Num()))
                    {
                        continue;
                    }

                    const auto XformPos = [PrimXform, &Center](const FVector3f& Raw) -> FVector3f
                    {
                        const FVector3f P = PrimXform
                            ? FVector3f(PrimXform->TransformPosition(FVector(Raw)))
                            : Raw;
                        return P - Center;
                    };
                    const FVector3f P0 = XformPos(Prim.Positions[I0]);
                    const FVector3f P1 = XformPos(Prim.Positions[I1]);
                    const FVector3f P2 = XformPos(Prim.Positions[I2]);

                    const auto Project = [&](const FVector3f& P) -> FVector3f
                    {
                        const float SX = (FVector3f::DotProduct(P, Right) * InvRadius * 0.5f + 0.5f) * (FrameRes - 1);
                        const float SY = (-FVector3f::DotProduct(P, Up) * InvRadius * 0.5f + 0.5f) * (FrameRes - 1);
                        const float SZ = FVector3f::DotProduct(P, Forward);
                        return FVector3f(SX, SY, SZ);
                    };

                    const FVector3f V0 = Project(P0);
                    const FVector3f V1 = Project(P1);
                    const FVector3f V2 = Project(P2);

                    const float Area = (V1.X - V0.X) * (V2.Y - V0.Y) - (V2.X - V0.X) * (V1.Y - V0.Y);
                    if (FMath::Abs(Area) < 1.0e-5f) continue;
                    const float InvArea = 1.0f / Area;

                    const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(V0.X, V1.X, V2.X)), 0, FrameRes - 1);
                    const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(V0.X, V1.X, V2.X)), 0, FrameRes - 1);
                    const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(V0.Y, V1.Y, V2.Y)), 0, FrameRes - 1);
                    const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(V0.Y, V1.Y, V2.Y)), 0, FrameRes - 1);

                    FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal(1.0e-6f, Forward);
                    const auto XformNorm = [PrimXform, &FaceNormal](const FVector3f& Raw) -> FVector3f
                    {
                        const FVector3f N = PrimXform
                            ? FVector3f(PrimXform->TransformVectorNoScale(FVector(Raw)))
                            : Raw;
                        return N.GetSafeNormal(1.0e-6f, FaceNormal);
                    };
                    const FVector3f N0 = bHasNormals ? XformNorm(Prim.Normals[I0]) : FaceNormal;
                    const FVector3f N1 = bHasNormals ? XformNorm(Prim.Normals[I1]) : FaceNormal;
                    const FVector3f N2 = bHasNormals ? XformNorm(Prim.Normals[I2]) : FaceNormal;

                    const FVector2f UV0 = bHasUV0 ? Prim.UVs[0][I0] : FVector2f::ZeroVector;
                    const FVector2f UV1 = bHasUV0 ? Prim.UVs[0][I1] : FVector2f::ZeroVector;
                    const FVector2f UV2 = bHasUV0 ? Prim.UVs[0][I2] : FVector2f::ZeroVector;

                    for (int32 PY = MinY; PY <= MaxY; ++PY)
                    {
                        const float SampleY = static_cast<float>(PY) + 0.5f;
                        for (int32 PX = MinX; PX <= MaxX; ++PX)
                        {
                            const float SampleX = static_cast<float>(PX) + 0.5f;
                            const float W0 = ((V1.X - SampleX) * (V2.Y - SampleY) - (V2.X - SampleX) * (V1.Y - SampleY)) * InvArea;
                            const float W1 = ((V2.X - SampleX) * (V0.Y - SampleY) - (V0.X - SampleX) * (V2.Y - SampleY)) * InvArea;
                            const float W2 = 1.0f - W0 - W1;
                            if (W0 < -1.0e-4f || W1 < -1.0e-4f || W2 < -1.0e-4f) continue;

                            const float Depth = W0 * V0.Z + W1 * V1.Z + W2 * V2.Z;
                            const int32 LocalPixelIdx = PY * FrameRes + PX;
                            if (Depth <= DepthBuffer[LocalPixelIdx]) continue;

                            FLinearColor Color = Surface.BaseColorFactor;
                            if (Surface.BaseColorTexture)
                            {
                                const float U = W0 * UV0.X + W1 * UV1.X + W2 * UV2.X;
                                const float V = W0 * UV0.Y + W1 * UV1.Y + W2 * UV2.Y;
                                Color *= SampleBakedTextureBilinear(Surface.BaseColorTexture, U, V);
                            }
                            if (Surface.bHasAlphaMask && Color.A < 0.35f) continue;

                            DepthBuffer[LocalPixelIdx] = Depth;
                            FVector3f InterpNormal = (N0 * W0 + N1 * W1 + N2 * W2).GetSafeNormal(1.0e-6f, Forward);

                            const int64 AtlasPixelOffset = (static_cast<int64>(OffsetY + PY) * AtlasSize + (OffsetX + PX)) * 4;
                            uint8* ColorDst = OutColorSlice.GetData() + AtlasPixelOffset;
                            ColorDst[0] = FloatToByte(Color.B);
                            ColorDst[1] = FloatToByte(Color.G);
                            ColorDst[2] = FloatToByte(Color.R);
                            ColorDst[3] = 255;

                            uint8* NormalDst = OutNormalSlice.GetData() + AtlasPixelOffset;
                            NormalDst[0] = FloatToByte(InterpNormal.Z * 0.5f + 0.5f);
                            NormalDst[1] = FloatToByte(InterpNormal.Y * 0.5f + 0.5f);
                            NormalDst[2] = FloatToByte(InterpNormal.X * 0.5f + 0.5f);
                            NormalDst[3] = FloatToByte(Surface.RoughnessFactor);
                        }
                    }
                }
            }
            // 1-pixel alpha edge dilation prevents dark bilinear fringes along impostor silhouettes.
            for (int32 PY = 0; PY < FrameRes; ++PY)
            {
                for (int32 PX = 0; PX < FrameRes; ++PX)
                {
                    if (DepthBuffer[PY * FrameRes + PX] > -FLT_MAX * 0.5f) continue;
                    int32 SumR = 0, SumG = 0, SumB = 0, SumNX = 0, SumNY = 0, SumNZ = 0, SumRough = 0, Hits = 0;
                    for (int32 DY = -1; DY <= 1; ++DY)
                    {
                        const int32 NY = PY + DY;
                        if (NY < 0 || NY >= FrameRes) continue;
                        for (int32 DX = -1; DX <= 1; ++DX)
                        {
                            const int32 NX = PX + DX;
                            if (NX < 0 || NX >= FrameRes || (DX == 0 && DY == 0)) continue;
                            if (DepthBuffer[NY * FrameRes + NX] <= -FLT_MAX * 0.5f) continue;
                            const int64 NOff = (static_cast<int64>(OffsetY + NY) * AtlasSize + (OffsetX + NX)) * 4;
                            SumB += OutColorSlice[NOff + 0]; SumG += OutColorSlice[NOff + 1]; SumR += OutColorSlice[NOff + 2];
                            SumNZ += OutNormalSlice[NOff + 0]; SumNY += OutNormalSlice[NOff + 1]; SumNX += OutNormalSlice[NOff + 2];
                            SumRough += OutNormalSlice[NOff + 3];
                            ++Hits;
                        }
                    }
                    if (Hits > 0)
                    {
                        const int64 Off = (static_cast<int64>(OffsetY + PY) * AtlasSize + (OffsetX + PX)) * 4;
                        OutColorSlice[Off + 0] = static_cast<uint8>(SumB / Hits);
                        OutColorSlice[Off + 1] = static_cast<uint8>(SumG / Hits);
                        OutColorSlice[Off + 2] = static_cast<uint8>(SumR / Hits);
                        OutNormalSlice[Off + 0] = static_cast<uint8>(SumNZ / Hits);
                        OutNormalSlice[Off + 1] = static_cast<uint8>(SumNY / Hits);
                        OutNormalSlice[Off + 2] = static_cast<uint8>(SumNX / Hits);
                        OutNormalSlice[Off + 3] = static_cast<uint8>(SumRough / Hits);
                    }
                }
            }
        });

        return true;
    }

    FGWorldBakedMesh BuildBillboardQuadMesh(
        const int32 MeshIndex,
        const FString& MeshName,
        const int32 MaterialId,
        const FString& MaterialName,
        const FVector3f& Center,
        const float Radius)
    {
        FGWorldBakedMesh Quad;
        Quad.MeshIndex = MeshIndex;
        Quad.Name = MeshName;
        Quad.bHasNormals = true;
        Quad.bHasTangents = true;
        Quad.bHasUV = true;

        FGWorldBakedPrimitive& Prim = Quad.Primitives.AddDefaulted_GetRef();
        Prim.Mode = 4;
        Prim.MaterialId = MaterialId;
        Prim.MaterialName = MaterialName;
        Prim.bHasMaterial = true;
        Prim.bHasIndices = true;

        // Three orthogonal crossed quads (XZ, YZ, XY) centered at Center with identical UV0.
        // In rasterization, V3D_ImpostorWPO_VS collapses all vertices onto the camera-facing billboard;
        // in Hardware Ray Tracing (static BLAS without per-ray WPO), the 3 orthogonal cards guarantee
        // solid 360-degree silhouette coverage from every ray direction (sides, shadows, water reflections).
        Prim.Positions = {
            // Card 0: XZ plane (normal -Y)
            Center + FVector3f(-Radius, 0.0f,  Radius),
            Center + FVector3f( Radius, 0.0f,  Radius),
            Center + FVector3f( Radius, 0.0f, -Radius),
            Center + FVector3f(-Radius, 0.0f, -Radius),
            // Card 1: YZ plane (normal +X)
            Center + FVector3f(0.0f, -Radius,  Radius),
            Center + FVector3f(0.0f,  Radius,  Radius),
            Center + FVector3f(0.0f,  Radius, -Radius),
            Center + FVector3f(0.0f, -Radius, -Radius),
            // Card 2: XY plane (normal +Z)
            Center + FVector3f(-Radius,  Radius, 0.0f),
            Center + FVector3f( Radius,  Radius, 0.0f),
            Center + FVector3f( Radius, -Radius, 0.0f),
            Center + FVector3f(-Radius, -Radius, 0.0f)
        };
        Prim.Normals = {
            FVector3f(0.0f, -1.0f, 0.0f), FVector3f(0.0f, -1.0f, 0.0f), FVector3f(0.0f, -1.0f, 0.0f), FVector3f(0.0f, -1.0f, 0.0f),
            FVector3f(1.0f,  0.0f, 0.0f), FVector3f(1.0f,  0.0f, 0.0f), FVector3f(1.0f,  0.0f, 0.0f), FVector3f(1.0f,  0.0f, 0.0f),
            FVector3f(0.0f,  0.0f, 1.0f), FVector3f(0.0f,  0.0f, 1.0f), FVector3f(0.0f,  0.0f, 1.0f), FVector3f(0.0f,  0.0f, 1.0f)
        };
        Prim.Tangents = {
            FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f),
            FVector4f(0.0f, 1.0f, 0.0f, 1.0f), FVector4f(0.0f, 1.0f, 0.0f, 1.0f), FVector4f(0.0f, 1.0f, 0.0f, 1.0f), FVector4f(0.0f, 1.0f, 0.0f, 1.0f),
            FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f), FVector4f(1.0f, 0.0f, 0.0f, 1.0f)
        };
        Prim.UVs.Add({
            FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(1.0f, 1.0f), FVector2f(0.0f, 1.0f),
            FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(1.0f, 1.0f), FVector2f(0.0f, 1.0f),
            FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(1.0f, 1.0f), FVector2f(0.0f, 1.0f)
        });
        Prim.Indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11
        };
        return Quad;
    }
}

bool V3DImpostor::IsImpostorMeshName(const FString& Name)
{
    return Name.EndsWith(MeshNameSuffix, ESearchCase::IgnoreCase);
}

bool V3DImpostor::IsImpostorMaterial(const FGWorldBakedMaterial& Material)
{
    if (Material.Name.EndsWith(MaterialNameSuffix, ESearchCase::IgnoreCase))
    {
        return true;
    }
    for (const FGWorldBakedScalarParameter& Scalar : Material.Scalars)
    {
        if (Scalar.Name.Equals(FramesParamName, ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}

FString V3DImpostor::ResolveMaterialPath(const UObject* WorldContextObject)
{
    (void)WorldContextObject;
    return FString(DefaultMaterialPath);
}

UMaterialInterface* V3DImpostor::ResolveMaterial(
    const UObject* WorldContextObject,
    const FString& ArchiveMaterialPath)
{
    (void)WorldContextObject;
    check(IsInGameThread());
    static TWeakObjectPtr<UMaterialInterface> CachedDefault;
    static bool bProbedDefault = false;
    if (!ArchiveMaterialPath.IsEmpty() && ArchiveMaterialPath != DefaultMaterialPath)
    {
        if (UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(nullptr, *ArchiveMaterialPath))
        {
            return Loaded;
        }
    }
    if (UMaterialInterface* Cached = CachedDefault.Get())
    {
        return Cached;
    }
    if (CachedDefault.IsStale())
    {
        CachedDefault.Reset();
        bProbedDefault = false;
    }
    if (!bProbedDefault)
    {
        bProbedDefault = true;
        if (UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(nullptr, DefaultMaterialPath))
        {
            CachedDefault = Loaded;
            return Loaded;
        }
    }
    return nullptr;
}

bool V3DImpostor::Bake(
    FGWorldBakedModel& Model,
    FGWorldModelMetadata& Metadata,
    FString& OutError,
    const UObject* WorldContextObject,
    const int32 GridFrames,
    const int32 FrameResolution)
{
    OutError.Reset();
    if (Model.Meshes.IsEmpty() || Metadata.SceneData.MeshMap.IsEmpty())
    {
        return true;
    }

    const int32 SafeFrames = FMath::Clamp(GridFrames, 4, 16);
    const int32 SafeFrameRes = FMath::Clamp(FrameResolution, 32, 128);
    const int32 AtlasSize = SafeFrames * SafeFrameRes;

    // Snapshot lookup maps for existing source meshes/materials/textures before appending impostors.
    TMap<int32, int32> MeshIndexToArraySlot;
    for (int32 I = 0; I < Model.Meshes.Num(); ++I)
    {
        MeshIndexToArraySlot.Add(Model.Meshes[I].MeshIndex, I);
    }
    TMap<int32, const FGWorldBakedMaterial*> MaterialMap;
    for (const FGWorldBakedMaterial& Mat : Model.Materials)
    {
        MaterialMap.Add(Mat.MaterialId, &Mat);
    }
    TMap<int32, const FGWorldBakedTexture*> TextureMap;
    for (const FGWorldBakedTexture& Tex : Model.Textures)
    {
        TextureMap.Add(Tex.TextureId, &Tex);
    }

    int32 NextMeshId = 0;
    for (const FGWorldBakedMesh& M : Model.Meshes) NextMeshId = FMath::Max(NextMeshId, M.MeshIndex + 1);
    int32 NextMaterialId = 0;
    for (const FGWorldBakedMaterial& M : Model.Materials) NextMaterialId = FMath::Max(NextMaterialId, M.MaterialId + 1);
    int32 NextTextureId = 0;
    for (const FGWorldBakedTexture& T : Model.Textures) NextTextureId = FMath::Max(NextTextureId, T.TextureId + 1);

    const FString ImpostorMatPath = ResolveMaterialPath(WorldContextObject);

    TArray<FGWorldBakedMesh> NewMeshes;
    TArray<FGWorldBakedMaterial> NewMaterials;
    TArray<FGWorldBakedTexture> NewTextures;

    for (TPair<FName, FModelMeshData>& Pair : Metadata.SceneData.MeshMap)
    {
        FModelMeshData& MeshData = Pair.Value;
        // Pick the finest available LOD as capture source for highest impostor visual quality.
        const int32 SourceLodIndex = MeshData.LOD0 != INDEX_NONE ? MeshData.LOD0
            : (MeshData.LOD1 != INDEX_NONE ? MeshData.LOD1
                : (MeshData.LOD2 != INDEX_NONE ? MeshData.LOD2 : MeshData.LOD3));
        const int32* SourceSlot = MeshIndexToArraySlot.Find(SourceLodIndex);
        if (!SourceSlot || !Model.Meshes.IsValidIndex(*SourceSlot)) continue;

        const FGWorldBakedMesh& SourceMesh = Model.Meshes[*SourceSlot];
        if (IsImpostorMeshName(SourceMesh.Name)) continue;

        // Skip terrain meshes (terrain uses continuous chunked geometry, not billboard impostors).
        bool bIsTerrain = false;
        for (const FGWorldBakedPrimitive& Prim : SourceMesh.Primitives)
        {
            if (const FGWorldBakedMaterial* const* Mat = MaterialMap.Find(Prim.MaterialId);
                Mat && *Mat && V3DTerrain::IsPaletteMaterial(**Mat))
            {
                bIsTerrain = true;
                break;
            }
        }
        if (bIsTerrain) continue;

        // Compute tight bounding sphere from source vertices (honoring per-primitive AdditionalTransforms).
        const bool bHasPrimTransforms = SourceMesh.AdditionalTransforms.Num() == SourceMesh.Primitives.Num();
        FBox3f Bounds(ForceInit);
        int32 VertexCount = 0;
        for (int32 PrimIdx = 0; PrimIdx < SourceMesh.Primitives.Num(); ++PrimIdx)
        {
            const FGWorldBakedPrimitive& Prim = SourceMesh.Primitives[PrimIdx];
            const FTransform* PrimXform = bHasPrimTransforms ? &SourceMesh.AdditionalTransforms[PrimIdx] : nullptr;
            for (const FVector3f& RawPos : Prim.Positions)
            {
                const FVector3f Pos = PrimXform
                    ? FVector3f(PrimXform->TransformPosition(FVector(RawPos)))
                    : RawPos;
                if (!FMath::IsFinite(Pos.X) || !FMath::IsFinite(Pos.Y) || !FMath::IsFinite(Pos.Z)) continue;
                Bounds += Pos;
                ++VertexCount;
            }
        }
        if (VertexCount < 3 || !Bounds.IsValid) continue;

        const FVector3f BoxSize = Bounds.GetSize();
        const float MaxAxis = BoxSize.GetMax();
        const float MinAxis = FMath::Max(BoxSize.GetMin(), 1.0f);
        // Skip ultra-flat/elongated slabs (roads, walls, ground sheets) that distort as spherical billboards.
        if (MaxAxis / MinAxis > 12.0f) continue;

        const FVector3f Center = Bounds.GetCenter();
        float RadiusSq = 0.0f;
        for (int32 PrimIdx = 0; PrimIdx < SourceMesh.Primitives.Num(); ++PrimIdx)
        {
            const FGWorldBakedPrimitive& Prim = SourceMesh.Primitives[PrimIdx];
            const FTransform* PrimXform = bHasPrimTransforms ? &SourceMesh.AdditionalTransforms[PrimIdx] : nullptr;
            for (const FVector3f& RawPos : Prim.Positions)
            {
                const FVector3f Pos = PrimXform
                    ? FVector3f(PrimXform->TransformPosition(FVector(RawPos)))
                    : RawPos;
                if (!FMath::IsFinite(Pos.X) || !FMath::IsFinite(Pos.Y) || !FMath::IsFinite(Pos.Z)) continue;
                RadiusSq = FMath::Max(RadiusSq, FVector3f::DistSquared(Pos, Center));
            }
        }
        const float Radius = FMath::Sqrt(RadiusSq) * 1.02f;
        if (!FMath::IsFinite(Radius) || Radius < 5.0f) continue;

        TArray<uint8> ColorSlice;
        TArray<uint8> NormalSlice;
        if (!V3DImpostorPrivate::CaptureOctahedralAtlas(
            SourceMesh, MaterialMap, TextureMap, Center, Radius, SafeFrames, SafeFrameRes, ColorSlice, NormalSlice))
        {
            continue;
        }

        const int32 ImpostorMatId = NextMaterialId++;
        const int32 ImpostorMeshId = NextMeshId++;
        const FString BaseLabel = Pair.Key.ToString();

        FGWorldBakedMaterial ImpostorMat;
        ImpostorMat.MaterialId = ImpostorMatId;
        ImpostorMat.Name = BaseLabel + MaterialNameSuffix;
        ImpostorMat.BaseMaterialPath = ImpostorMatPath;
        ImpostorMat.MaterialType = 1; // Masked
        ImpostorMat.Scalars.Add({ FramesParamName, static_cast<float>(SafeFrames) });
        ImpostorMat.Scalars.Add({ RadiusParamName, Radius });
        ImpostorMat.Vectors.Add({ CenterParamName, FLinearColor(Center.X, Center.Y, Center.Z, 1.0f) });

        // Pack Slice 0 (BaseColor+Alpha) and Slice 1 (Normal+Roughness) into one 2-slice Texture2DArray.
        const int32 AtlasTexId = NextTextureId++;
        TArray<uint8> CombinedSlices;
        CombinedSlices.Reserve(ColorSlice.Num() + NormalSlice.Num());
        CombinedSlices.Append(ColorSlice);
        CombinedSlices.Append(NormalSlice);

        FGWorldBakedTexture AtlasTex;
        AtlasTex.TextureId = AtlasTexId;
        AtlasTex.Name = BaseLabel + TEXT("_ImpostorAtlasArray");
        AtlasTex.SizeX = AtlasSize;
        AtlasTex.SizeY = AtlasSize;
        AtlasTex.NumLayers = 2;
        AtlasTex.bTextureArray = true;
        AtlasTex.PixelFormat = static_cast<int32>(PF_B8G8R8A8);
        AtlasTex.AddressX = static_cast<uint8>(TA_Clamp);
        AtlasTex.AddressY = static_cast<uint8>(TA_Clamp);
        AtlasTex.Filter = static_cast<uint8>(TF_Trilinear);
        AtlasTex.LODGroup = static_cast<uint8>(TEXTUREGROUP_World);
        AtlasTex.bSRGB = false;
        V3DImpostorPrivate::BuildMipChainBGRA(AtlasSize, 2, CombinedSlices, false, AtlasTex.Mips);

        NewTextures.Add(MoveTemp(AtlasTex));
        ImpostorMat.Textures.Add({ AtlasArrayParamName, AtlasTexId });

        FGWorldBakedMesh ImpostorQuad = V3DImpostorPrivate::BuildBillboardQuadMesh(
            ImpostorMeshId, BaseLabel + MeshNameSuffix, ImpostorMatId, ImpostorMat.Name, Center, Radius);

        NewMaterials.Add(MoveTemp(ImpostorMat));
        NewMeshes.Add(MoveTemp(ImpostorQuad));

        // Attach the baked impostor to the LAST LOD slot of this mesh group.
        if (MeshData.LOD1 == INDEX_NONE)
        {
            MeshData.LOD1 = ImpostorMeshId;
        }
        else if (MeshData.LOD2 == INDEX_NONE)
        {
            MeshData.LOD2 = ImpostorMeshId;
        }
        else
        {
            MeshData.LOD3 = ImpostorMeshId;
        }
    }

    if (!NewMeshes.IsEmpty())
    {
        Model.Textures.Append(MoveTemp(NewTextures));
        Model.Materials.Append(MoveTemp(NewMaterials));
        Model.Meshes.Append(MoveTemp(NewMeshes));
    }
    Metadata.SourceMeshCount = Model.Meshes.Num();
    Metadata.SourceMaterialCount = Model.Materials.Num();
    Metadata.SourceTextureCount = Model.Textures.Num();
    return true;
}
