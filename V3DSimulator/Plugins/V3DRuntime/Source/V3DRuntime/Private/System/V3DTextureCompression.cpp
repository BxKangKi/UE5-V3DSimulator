// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DTextureCompression.h"
#include "Async/ParallelFor.h"
#include "PixelFormat.h"

namespace V3DTextureCompressionPrivate
{
    FORCEINLINE uint16 PackRGB565(const int32 R, const int32 G, const int32 B)
    {
        return static_cast<uint16>(
            ((FMath::Clamp(R, 0, 255) >> 3) << 11)
            | ((FMath::Clamp(G, 0, 255) >> 2) << 5)
            | (FMath::Clamp(B, 0, 255) >> 3));
    }

    FORCEINLINE void UnpackRGB565(const uint16 C, int32& OutR, int32& OutG, int32& OutB)
    {
        const int32 R5 = (C >> 11) & 31;
        const int32 G6 = (C >> 5) & 63;
        const int32 B5 = C & 31;
        OutR = (R5 << 3) | (R5 >> 2);
        OutG = (G6 << 2) | (G6 >> 4);
        OutB = (B5 << 3) | (B5 >> 2);
    }

    // Encodes a 4x4 RGBA block's color channels into 8 bytes (BC1 / BC3 color block, 4-color opaque mode).
    void EncodeBCColorBlock(const uint8 BlockRGBA[64], uint8 OutBlock[8])
    {
        int32 MinR = 255, MinG = 255, MinB = 255;
        int32 MaxR = 0, MaxG = 0, MaxB = 0;

        for (int32 I = 0; I < 16; ++I)
        {
            const int32 R = BlockRGBA[I * 4 + 0];
            const int32 G = BlockRGBA[I * 4 + 1];
            const int32 B = BlockRGBA[I * 4 + 2];
            if (R < MinR) MinR = R;
            if (G < MinG) MinG = G;
            if (B < MinB) MinB = B;
            if (R > MaxR) MaxR = R;
            if (G > MaxG) MaxG = G;
            if (B > MaxB) MaxB = B;
        }

        const int32 InsetR = (MaxR - MinR) >> 4;
        const int32 InsetG = (MaxG - MinG) >> 4;
        const int32 InsetB = (MaxB - MinB) >> 4;
        MinR = FMath::Min(255, MinR + InsetR);
        MinG = FMath::Min(255, MinG + InsetG);
        MinB = FMath::Min(255, MinB + InsetB);
        MaxR = FMath::Max(0, MaxR - InsetR);
        MaxG = FMath::Max(0, MaxG - InsetG);
        MaxB = FMath::Max(0, MaxB - InsetB);

        uint16 C0 = PackRGB565(MaxR, MaxG, MaxB);
        uint16 C1 = PackRGB565(MinR, MinG, MinB);

        if (C0 < C1)
        {
            Swap(C0, C1);
        }

        OutBlock[0] = static_cast<uint8>(C0 & 0xFFu);
        OutBlock[1] = static_cast<uint8>((C0 >> 8) & 0xFFu);
        OutBlock[2] = static_cast<uint8>(C1 & 0xFFu);
        OutBlock[3] = static_cast<uint8>((C1 >> 8) & 0xFFu);

        if (C0 == C1)
        {
            OutBlock[4] = 0; OutBlock[5] = 0; OutBlock[6] = 0; OutBlock[7] = 0;
            return;
        }

        int32 PalR[4], PalG[4], PalB[4];
        UnpackRGB565(C0, PalR[0], PalG[0], PalB[0]);
        UnpackRGB565(C1, PalR[1], PalG[1], PalB[1]);
        PalR[2] = (2 * PalR[0] + PalR[1] + 1) / 3;
        PalG[2] = (2 * PalG[0] + PalG[1] + 1) / 3;
        PalB[2] = (2 * PalB[0] + PalB[1] + 1) / 3;
        PalR[3] = (PalR[0] + 2 * PalR[1] + 1) / 3;
        PalG[3] = (PalG[0] + 2 * PalG[1] + 1) / 3;
        PalB[3] = (PalB[0] + 2 * PalB[1] + 1) / 3;

        uint32 Indices = 0;
        for (int32 I = 0; I < 16; ++I)
        {
            const int32 R = BlockRGBA[I * 4 + 0];
            const int32 G = BlockRGBA[I * 4 + 1];
            const int32 B = BlockRGBA[I * 4 + 2];

            int32 BestDist = MAX_int32;
            uint32 BestIdx = 0;
            for (uint32 P = 0; P < 4; ++P)
            {
                const int32 DR = R - PalR[P];
                const int32 DG = G - PalG[P];
                const int32 DB = B - PalB[P];
                const int32 Dist = DR * DR + DG * DG + DB * DB;
                if (Dist < BestDist)
                {
                    BestDist = Dist;
                    BestIdx = P;
                }
            }
            Indices |= (BestIdx << (I * 2));
        }

        OutBlock[4] = static_cast<uint8>(Indices & 0xFFu);
        OutBlock[5] = static_cast<uint8>((Indices >> 8) & 0xFFu);
        OutBlock[6] = static_cast<uint8>((Indices >> 16) & 0xFFu);
        OutBlock[7] = static_cast<uint8>((Indices >> 24) & 0xFFu);
    }

    // Encodes a 4x4 RGBA block's alpha channel into 8 bytes (BC3 / DXT5 interpolated alpha block).
    void EncodeBC3AlphaBlock(const uint8 BlockRGBA[64], uint8 OutBlock[8])
    {
        uint8 MinA = 255;
        uint8 MaxA = 0;
        for (int32 I = 0; I < 16; ++I)
        {
            const uint8 A = BlockRGBA[I * 4 + 3];
            if (A < MinA) MinA = A;
            if (A > MaxA) MaxA = A;
        }

        OutBlock[0] = MaxA;
        OutBlock[1] = MinA;

        if (MaxA == MinA)
        {
            FMemory::Memzero(OutBlock + 2, 6);
            return;
        }

        int32 PalA[8];
        PalA[0] = MaxA;
        PalA[1] = MinA;
        for (int32 K = 1; K <= 6; ++K)
        {
            PalA[K + 1] = ((7 - K) * static_cast<int32>(MaxA) + K * static_cast<int32>(MinA) + 3) / 7;
        }

        uint64 Bits = 0;
        for (int32 I = 0; I < 16; ++I)
        {
            const int32 A = BlockRGBA[I * 4 + 3];
            int32 BestDist = MAX_int32;
            uint64 BestIdx = 0;
            for (uint64 P = 0; P < 8; ++P)
            {
                const int32 Dist = FMath::Abs(A - PalA[P]);
                if (Dist < BestDist)
                {
                    BestDist = Dist;
                    BestIdx = P;
                    if (Dist == 0) break;
                }
            }
            Bits |= (BestIdx << (I * 3));
        }

        for (int32 ByteIdx = 0; ByteIdx < 6; ++ByteIdx)
        {
            OutBlock[2 + ByteIdx] = static_cast<uint8>((Bits >> (ByteIdx * 8)) & 0xFFu);
        }
    }

    // Encodes a 4x4 RGBA block's color channels into 8 bytes of ETC2 RGB8 (Individual/Differential mode).
    void EncodeETC2RGBBlock(const uint8 BlockRGBA[64], uint8 OutBlock[8])
    {
        static constexpr int32 ModTables[8][4] = {
            {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
            {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}
        };

        auto EvaluatePartition = [&](const bool bFlip, int32& OutError, uint8 Candidate[8])
        {
            int32 SumR[2] = {0, 0}, SumG[2] = {0, 0}, SumB[2] = {0, 0};
            for (int32 X = 0; X < 4; ++X)
            {
                for (int32 Y = 0; Y < 4; ++Y)
                {
                    const int32 Sub = bFlip ? (Y >= 2 ? 1 : 0) : (X >= 2 ? 1 : 0);
                    const int32 Src = (Y * 4 + X) * 4;
                    SumR[Sub] += BlockRGBA[Src + 0];
                    SumG[Sub] += BlockRGBA[Src + 1];
                    SumB[Sub] += BlockRGBA[Src + 2];
                }
            }

            // Try Differential mode (5-bit base + 3-bit signed delta) if delta fits in [-4, 3].
            const int32 R5_0 = FMath::Clamp((SumR[0] / 8 + 4) >> 3, 0, 31);
            const int32 G5_0 = FMath::Clamp((SumG[0] / 8 + 4) >> 3, 0, 31);
            const int32 B5_0 = FMath::Clamp((SumB[0] / 8 + 4) >> 3, 0, 31);
            const int32 R5_1 = FMath::Clamp((SumR[1] / 8 + 4) >> 3, 0, 31);
            const int32 G5_1 = FMath::Clamp((SumG[1] / 8 + 4) >> 3, 0, 31);
            const int32 B5_1 = FMath::Clamp((SumB[1] / 8 + 4) >> 3, 0, 31);

            const int32 DR = R5_1 - R5_0;
            const int32 DG = G5_1 - G5_0;
            const int32 DB = B5_1 - B5_0;
            const bool bUseDiff = (DR >= -4 && DR <= 3 && DG >= -4 && DG <= 3 && DB >= -4 && DB <= 3);

            int32 BaseR[2], BaseG[2], BaseB[2];
            if (bUseDiff)
            {
                BaseR[0] = (R5_0 << 3) | (R5_0 >> 2);
                BaseG[0] = (G5_0 << 3) | (G5_0 >> 2);
                BaseB[0] = (B5_0 << 3) | (B5_0 >> 2);
                BaseR[1] = (R5_1 << 3) | (R5_1 >> 2);
                BaseG[1] = (G5_1 << 3) | (G5_1 >> 2);
                BaseB[1] = (B5_1 << 3) | (B5_1 >> 2);
                Candidate[0] = static_cast<uint8>((R5_0 << 3) | (DR & 7));
                Candidate[1] = static_cast<uint8>((G5_0 << 3) | (DG & 7));
                Candidate[2] = static_cast<uint8>((B5_0 << 3) | (DB & 7));
            }
            else
            {
                const int32 R4_0 = FMath::Clamp((SumR[0] / 8 + 8) >> 4, 0, 15);
                const int32 G4_0 = FMath::Clamp((SumG[0] / 8 + 8) >> 4, 0, 15);
                const int32 B4_0 = FMath::Clamp((SumB[0] / 8 + 8) >> 4, 0, 15);
                const int32 R4_1 = FMath::Clamp((SumR[1] / 8 + 8) >> 4, 0, 15);
                const int32 G4_1 = FMath::Clamp((SumG[1] / 8 + 8) >> 4, 0, 15);
                const int32 B4_1 = FMath::Clamp((SumB[1] / 8 + 8) >> 4, 0, 15);
                BaseR[0] = (R4_0 << 4) | R4_0;
                BaseG[0] = (G4_0 << 4) | G4_0;
                BaseB[0] = (B4_0 << 4) | B4_0;
                BaseR[1] = (R4_1 << 4) | R4_1;
                BaseG[1] = (G4_1 << 4) | G4_1;
                BaseB[1] = (B4_1 << 4) | B4_1;
                Candidate[0] = static_cast<uint8>((R4_0 << 4) | R4_1);
                Candidate[1] = static_cast<uint8>((G4_0 << 4) | G4_1);
                Candidate[2] = static_cast<uint8>((B4_0 << 4) | B4_1);
            }

            int32 BestTbl[2] = {0, 0};
            int32 BestSubErr[2] = {MAX_int32, MAX_int32};
            uint16 SubMsb[2] = {0, 0};
            uint16 SubLsb[2] = {0, 0};

            for (int32 Sub = 0; Sub < 2; ++Sub)
            {
                for (int32 T = 0; T < 8; ++T)
                {
                    int32 Err = 0;
                    uint16 Msb = 0, Lsb = 0;
                    for (int32 X = 0; X < 4; ++X)
                    {
                        for (int32 Y = 0; Y < 4; ++Y)
                        {
                            if ((bFlip ? (Y >= 2 ? 1 : 0) : (X >= 2 ? 1 : 0)) != Sub) continue;
                            const int32 Src = (Y * 4 + X) * 4;
                            const int32 PR = BlockRGBA[Src + 0];
                            const int32 PG = BlockRGBA[Src + 1];
                            const int32 PB = BlockRGBA[Src + 2];

                            int32 BestPixErr = MAX_int32;
                            int32 BestSel = 0;
                            for (int32 Sel = 0; Sel < 4; ++Sel)
                            {
                                const int32 D = ModTables[T][Sel];
                                const int32 DRP = PR - FMath::Clamp(BaseR[Sub] + D, 0, 255);
                                const int32 DGP = PG - FMath::Clamp(BaseG[Sub] + D, 0, 255);
                                const int32 DBP = PB - FMath::Clamp(BaseB[Sub] + D, 0, 255);
                                const int32 PixErr = DRP * DRP + DGP * DGP + DBP * DBP;
                                if (PixErr < BestPixErr)
                                {
                                    BestPixErr = PixErr;
                                    BestSel = Sel;
                                }
                            }
                            Err += BestPixErr;
                            const int32 Bit = X * 4 + Y;
                            if (BestSel & 2) Msb |= static_cast<uint16>(1u << Bit);
                            if (BestSel & 1) Lsb |= static_cast<uint16>(1u << Bit);
                        }
                    }
                    if (Err < BestSubErr[Sub])
                    {
                        BestSubErr[Sub] = Err;
                        BestTbl[Sub] = T;
                        SubMsb[Sub] = Msb;
                        SubLsb[Sub] = Lsb;
                    }
                }
            }

            OutError = BestSubErr[0] + BestSubErr[1];
            const uint16 MsbAll = SubMsb[0] | SubMsb[1];
            const uint16 LsbAll = SubLsb[0] | SubLsb[1];
            Candidate[3] = static_cast<uint8>((BestTbl[0] << 5) | (BestTbl[1] << 2) | (bUseDiff ? 2 : 0) | (bFlip ? 1 : 0));
            Candidate[4] = static_cast<uint8>(MsbAll >> 8);
            Candidate[5] = static_cast<uint8>(MsbAll & 0xFFu);
            Candidate[6] = static_cast<uint8>(LsbAll >> 8);
            Candidate[7] = static_cast<uint8>(LsbAll & 0xFFu);
        };

        int32 Err0 = 0, Err1 = 0;
        uint8 Cand0[8], Cand1[8];
        EvaluatePartition(false, Err0, Cand0);
        EvaluatePartition(true, Err1, Cand1);
        FMemory::Memcpy(OutBlock, Err0 <= Err1 ? Cand0 : Cand1, 8);
    }

    // Encodes a 4x4 RGBA block's alpha channel into 8 bytes of ETC2 EAC Alpha.
    void EncodeEACAlphaBlock(const uint8 BlockRGBA[64], uint8 OutBlock[8])
    {
        static constexpr int32 Mod[16][8] = {
            {-3,-6,-9,-15,2,5,8,14}, {-3,-7,-10,-13,2,6,9,12}, {-2,-5,-8,-13,1,4,7,12}, {-2,-4,-6,-13,1,3,5,12},
            {-3,-6,-8,-12,2,5,7,11}, {-3,-7,-9,-11,2,6,8,10}, {-4,-7,-8,-11,3,6,7,10}, {-3,-5,-8,-11,2,4,7,10},
            {-2,-6,-8,-10,1,5,7,9},  {-2,-5,-8,-10,1,4,7,9},  {-2,-4,-8,-10,1,3,7,9},  {-2,-5,-7,-10,1,4,6,9},
            {-3,-4,-7,-10,2,3,6,9},  {-1,-2,-3,-10,0,1,2,9},  {-4,-6,-8,-9,3,5,7,8},   {-3,-5,-7,-9,2,4,6,8}
        };

        int32 MinA = 255, MaxA = 0;
        for (int32 I = 0; I < 16; ++I)
        {
            const int32 A = BlockRGBA[I * 4 + 3];
            if (A < MinA) MinA = A;
            if (A > MaxA) MaxA = A;
        }

        if (MinA == MaxA)
        {
            OutBlock[0] = static_cast<uint8>(MinA);
            OutBlock[1] = 0;
            FMemory::Memzero(OutBlock + 2, 6);
            return;
        }

        const int32 Base = FMath::Clamp((MinA + MaxA) >> 1, 0, 255);
        const int32 Span = MaxA - MinA;
        const int32 Mult = FMath::Clamp((Span + 14) / 20, 1, 15);

        int32 BestTbl = 0;
        int32 BestErr = MAX_int32;
        uint64 BestBits = 0;

        for (int32 T = 0; T < 16; ++T)
        {
            int32 Err = 0;
            uint64 Bits = 0;
            for (int32 X = 0; X < 4; ++X)
            {
                for (int32 Y = 0; Y < 4; ++Y)
                {
                    const int32 A = BlockRGBA[(Y * 4 + X) * 4 + 3];
                    int32 BestPixErr = MAX_int32;
                    int32 BestIdx = 0;
                    for (int32 K = 0; K < 8; ++K)
                    {
                        const int32 CandA = FMath::Clamp(Base + Mod[T][K] * Mult, 0, 255);
                        const int32 D = FMath::Abs(A - CandA);
                        if (D < BestPixErr)
                        {
                            BestPixErr = D;
                            BestIdx = K;
                            if (D == 0) break;
                        }
                    }
                    Err += BestPixErr;
                    const int32 P = X * 4 + Y;
                    Bits |= (static_cast<uint64>(BestIdx) << (45 - P * 3));
                }
            }
            if (Err < BestErr)
            {
                BestErr = Err;
                BestTbl = T;
                BestBits = Bits;
            }
        }

        OutBlock[0] = static_cast<uint8>(Base);
        OutBlock[1] = static_cast<uint8>((Mult << 4) | BestTbl);
        for (int32 B = 0; B < 6; ++B)
        {
            OutBlock[2 + B] = static_cast<uint8>((BestBits >> ((5 - B) * 8)) & 0xFFu);
        }
    }

    void DecodeColorBlockBGRA(const uint8 SrcBlock[8], const bool bIsBC1, uint8 OutBGRA[64])
    {
        const uint16 C0 = static_cast<uint16>(SrcBlock[0]) | (static_cast<uint16>(SrcBlock[1]) << 8);
        const uint16 C1 = static_cast<uint16>(SrcBlock[2]) | (static_cast<uint16>(SrcBlock[3]) << 8);
        int32 R[4], G[4], B[4], A[4];
        UnpackRGB565(C0, R[0], G[0], B[0]);
        UnpackRGB565(C1, R[1], G[1], B[1]);
        A[0] = 255; A[1] = 255; A[2] = 255; A[3] = 255;

        if (!bIsBC1 || C0 > C1)
        {
            R[2] = (2 * R[0] + R[1] + 1) / 3;
            G[2] = (2 * G[0] + G[1] + 1) / 3;
            B[2] = (2 * B[0] + B[1] + 1) / 3;
            R[3] = (R[0] + 2 * R[1] + 1) / 3;
            G[3] = (G[0] + 2 * G[1] + 1) / 3;
            B[3] = (B[0] + 2 * B[1] + 1) / 3;
        }
        else
        {
            R[2] = (R[0] + R[1]) / 2;
            G[2] = (G[0] + G[1]) / 2;
            B[2] = (B[0] + B[1]) / 2;
            R[3] = 0; G[3] = 0; B[3] = 0; A[3] = 0;
        }

        const uint32 Indices = static_cast<uint32>(SrcBlock[4])
            | (static_cast<uint32>(SrcBlock[5]) << 8)
            | (static_cast<uint32>(SrcBlock[6]) << 16)
            | (static_cast<uint32>(SrcBlock[7]) << 24);

        for (int32 I = 0; I < 16; ++I)
        {
            const uint32 Idx = (Indices >> (I * 2)) & 3u;
            OutBGRA[I * 4 + 0] = static_cast<uint8>(B[Idx]);
            OutBGRA[I * 4 + 1] = static_cast<uint8>(G[Idx]);
            OutBGRA[I * 4 + 2] = static_cast<uint8>(R[Idx]);
            OutBGRA[I * 4 + 3] = static_cast<uint8>(A[Idx]);
        }
    }

    void DecodeAlphaBlockBGRA(const uint8 SrcBlock[8], uint8 OutBGRA[64])
    {
        const int32 A0 = SrcBlock[0];
        const int32 A1 = SrcBlock[1];
        int32 PalA[8];
        PalA[0] = A0;
        PalA[1] = A1;
        if (A0 > A1)
        {
            for (int32 K = 1; K <= 6; ++K)
                PalA[K + 1] = ((7 - K) * A0 + K * A1 + 3) / 7;
        }
        else
        {
            for (int32 K = 1; K <= 4; ++K)
                PalA[K + 1] = ((5 - K) * A0 + K * A1 + 2) / 5;
            PalA[6] = 0;
            PalA[7] = 255;
        }

        uint64 Bits = 0;
        for (int32 B = 0; B < 6; ++B)
        {
            Bits |= static_cast<uint64>(SrcBlock[2 + B]) << (B * 8);
        }

        for (int32 I = 0; I < 16; ++I)
        {
            const uint32 Idx = static_cast<uint32>((Bits >> (I * 3)) & 7ull);
            OutBGRA[I * 4 + 3] = static_cast<uint8>(PalA[Idx]);
        }
    }

    bool CanBlockCompressTexture(const FGWorldBakedTexture& Texture)
    {
        if (Texture.PixelFormat != PF_B8G8R8A8 && Texture.PixelFormat != PF_R8G8B8A8)
        {
            return false;
        }
        if (Texture.SizeX < 4 || Texture.SizeY < 4 || Texture.NumLayers <= 0 || Texture.Mips.IsEmpty())
        {
            return false;
        }
        for (const FGWorldBakedTextureMip& Mip : Texture.Mips)
        {
            if (Mip.SizeX <= 0 || Mip.SizeY <= 0 || Mip.SizeZ != Texture.NumLayers)
            {
                return false;
            }
            if ((Mip.SizeX >= 4 && (Mip.SizeX & 3) != 0) || (Mip.SizeY >= 4 && (Mip.SizeY & 3) != 0))
            {
                return false;
            }
            const int64 ExpectedUncompressed = static_cast<int64>(Mip.SizeX) * Mip.SizeY * Mip.SizeZ * 4;
            if (Mip.Bytes.Num() != ExpectedUncompressed)
            {
                return false;
            }
        }
        return true;
    }

    bool DetectTextureHasAlpha(const FGWorldBakedTexture& Texture)
    {
        const FGWorldBakedTextureMip& TopMip = Texture.Mips[0];
        const uint8* Data = TopMip.Bytes.GetData();
        const int32 NumPixels = TopMip.Bytes.Num() / 4;
        for (int32 I = 0; I < NumPixels; ++I)
        {
            if (Data[I * 4 + 3] < 252)
            {
                return true;
            }
        }
        return false;
    }

    void CompressSingleTexture(FGWorldBakedTexture& Texture)
    {
        if (!CanBlockCompressTexture(Texture))
        {
            return;
        }

        const bool bBGRA = (Texture.PixelFormat == PF_B8G8R8A8);
        const bool bHasAlpha = DetectTextureHasAlpha(Texture);
        const int32 BlockBytes = bHasAlpha ? 16 : 8;
        const int32 PrimaryFormat = bHasAlpha ? static_cast<int32>(PF_DXT5) : static_cast<int32>(PF_DXT1);
        const int32 AltFormat = bHasAlpha ? static_cast<int32>(PF_ETC2_RGBA) : static_cast<int32>(PF_ETC2_RGB);

        Texture.AltMips.Reset();
        Texture.AltMips.SetNum(Texture.Mips.Num());

        for (int32 MipIdx = 0; MipIdx < Texture.Mips.Num(); ++MipIdx)
        {
            FGWorldBakedTextureMip& Mip = Texture.Mips[MipIdx];
            FGWorldBakedTextureMip& AltMip = Texture.AltMips[MipIdx];

            const int32 W = Mip.SizeX;
            const int32 H = Mip.SizeY;
            const int32 Layers = Mip.SizeZ;
            const int32 BlocksX = (W + 3) / 4;
            const int32 BlocksY = (H + 3) / 4;
            const int64 LayerSrcBytes = static_cast<int64>(W) * H * 4;
            const int64 LayerDstBytes = static_cast<int64>(BlocksX) * BlocksY * BlockBytes;

            TArray<uint8> CompressedBC;
            CompressedBC.SetNumUninitialized(static_cast<int32>(LayerDstBytes * Layers));
            TArray<uint8> CompressedETC2;
            CompressedETC2.SetNumUninitialized(static_cast<int32>(LayerDstBytes * Layers));

            const uint8* SrcAll = Mip.Bytes.GetData();
            uint8* DstBCAll = CompressedBC.GetData();
            uint8* DstETC2All = CompressedETC2.GetData();
            const int32 TotalBlockRows = Layers * BlocksY;

            ParallelFor(TotalBlockRows, [&](const int32 RowIndex)
            {
                const int32 Layer = RowIndex / BlocksY;
                const int32 BY = RowIndex % BlocksY;
                const uint8* SrcLayer = SrcAll + static_cast<int64>(Layer) * LayerSrcBytes;
                uint8* DstBCRow = DstBCAll + static_cast<int64>(Layer) * LayerDstBytes
                    + static_cast<int64>(BY) * BlocksX * BlockBytes;
                uint8* DstETC2Row = DstETC2All + static_cast<int64>(Layer) * LayerDstBytes
                    + static_cast<int64>(BY) * BlocksX * BlockBytes;

                uint8 BlockRGBA[64];
                for (int32 BX = 0; BX < BlocksX; ++BX)
                {
                    for (int32 PY = 0; PY < 4; ++PY)
                    {
                        const int32 SY = FMath::Min(BY * 4 + PY, H - 1);
                        const uint8* SrcScan = SrcLayer + static_cast<int64>(SY) * W * 4;
                        for (int32 PX = 0; PX < 4; ++PX)
                        {
                            const int32 SX = FMath::Min(BX * 4 + PX, W - 1);
                            const uint8* P = SrcScan + SX * 4;
                            uint8* OutP = BlockRGBA + (PY * 4 + PX) * 4;
                            if (bBGRA)
                            {
                                OutP[0] = P[2]; OutP[1] = P[1]; OutP[2] = P[0]; OutP[3] = P[3];
                            }
                            else
                            {
                                OutP[0] = P[0]; OutP[1] = P[1]; OutP[2] = P[2]; OutP[3] = P[3];
                            }
                        }
                    }

                    uint8* DstBCBlock = DstBCRow + static_cast<int64>(BX) * BlockBytes;
                    uint8* DstETC2Block = DstETC2Row + static_cast<int64>(BX) * BlockBytes;
                    if (bHasAlpha)
                    {
                        EncodeBC3AlphaBlock(BlockRGBA, DstBCBlock);
                        EncodeBCColorBlock(BlockRGBA, DstBCBlock + 8);
                        EncodeEACAlphaBlock(BlockRGBA, DstETC2Block);
                        EncodeETC2RGBBlock(BlockRGBA, DstETC2Block + 8);
                    }
                    else
                    {
                        EncodeBCColorBlock(BlockRGBA, DstBCBlock);
                        EncodeETC2RGBBlock(BlockRGBA, DstETC2Block);
                    }
                }
            });

            Mip.Bytes = MoveTemp(CompressedBC);
            AltMip.SizeX = W;
            AltMip.SizeY = H;
            AltMip.SizeZ = Layers;
            AltMip.Bytes = MoveTemp(CompressedETC2);
        }

        Texture.PixelFormat = PrimaryFormat;
        Texture.AltPixelFormat = AltFormat;
    }
}

void V3DTextureCompression::CompressModelTextures(FGWorldBakedModel& Model)
{
    using namespace V3DTextureCompressionPrivate;

    struct FPreparedTexture
    {
        int32 TextureIndex = INDEX_NONE;
        bool bBGRA = false;
        bool bHasAlpha = false;
        int32 PrimaryFormat = 0;
        int32 AltFormat = 0;
        TArray<TArray<uint8>> BCOutputs;
        TArray<FGWorldBakedTextureMip> AltOutputs;
    };

    TArray<FPreparedTexture> Prepared;
    Prepared.Reserve(Model.Textures.Num());

    // Step 1: Identify eligible textures and detect alpha in parallel across textures.
    TArray<int32> EligibleIndices;
    EligibleIndices.Reserve(Model.Textures.Num());
    for (int32 TexIdx = 0; TexIdx < Model.Textures.Num(); ++TexIdx)
    {
        if (CanBlockCompressTexture(Model.Textures[TexIdx]))
        {
            EligibleIndices.Add(TexIdx);
        }
    }
    if (EligibleIndices.IsEmpty())
    {
        return;
    }

    Prepared.SetNum(EligibleIndices.Num());
    ParallelFor(EligibleIndices.Num(), [&](const int32 ItemIdx)
    {
        const int32 TexIdx = EligibleIndices[ItemIdx];
        const FGWorldBakedTexture& Texture = Model.Textures[TexIdx];
        FPreparedTexture& P = Prepared[ItemIdx];
        P.TextureIndex = TexIdx;
        P.bBGRA = (Texture.PixelFormat == PF_B8G8R8A8);
        P.bHasAlpha = DetectTextureHasAlpha(Texture);
        P.PrimaryFormat = P.bHasAlpha ? static_cast<int32>(PF_DXT5) : static_cast<int32>(PF_DXT1);
        P.AltFormat = P.bHasAlpha ? static_cast<int32>(PF_ETC2_RGBA) : static_cast<int32>(PF_ETC2_RGB);
    });

    // Step 2: Allocate destination mip buffers and flatten all (texture, mip, layer, block-row) slices
    // into a single lock-free work queue so every CPU core stays 100% saturated across small & large textures alike.
    struct FBlockRowBatchJob
    {
        const uint8* SrcLayer = nullptr;
        uint8* DstBCRow = nullptr;
        uint8* DstETC2Row = nullptr;
        int32 W = 0;
        int32 H = 0;
        int32 BlocksX = 0;
        int32 StartBY = 0;
        int32 EndBY = 0;
        int32 BlockBytes = 8;
        bool bBGRA = false;
        bool bHasAlpha = false;
    };

    TArray<FBlockRowBatchJob> Jobs;
    for (FPreparedTexture& P : Prepared)
    {
        const FGWorldBakedTexture& Texture = Model.Textures[P.TextureIndex];
        const int32 BlockBytes = P.bHasAlpha ? 16 : 8;
        const int32 NumMips = Texture.Mips.Num();
        P.BCOutputs.SetNum(NumMips);
        P.AltOutputs.SetNum(NumMips);

        for (int32 MipIdx = 0; MipIdx < NumMips; ++MipIdx)
        {
            const FGWorldBakedTextureMip& Mip = Texture.Mips[MipIdx];
            const int32 W = Mip.SizeX;
            const int32 H = Mip.SizeY;
            const int32 Layers = Mip.SizeZ;
            const int32 BlocksX = (W + 3) / 4;
            const int32 BlocksY = (H + 3) / 4;
            const int64 LayerSrcBytes = static_cast<int64>(W) * H * 4;
            const int64 LayerDstBytes = static_cast<int64>(BlocksX) * BlocksY * BlockBytes;

            P.BCOutputs[MipIdx].SetNumUninitialized(static_cast<int32>(LayerDstBytes * Layers));
            FGWorldBakedTextureMip& AltMip = P.AltOutputs[MipIdx];
            AltMip.SizeX = W;
            AltMip.SizeY = H;
            AltMip.SizeZ = Layers;
            AltMip.Bytes.SetNumUninitialized(static_cast<int32>(LayerDstBytes * Layers));

            const uint8* SrcAll = Mip.Bytes.GetData();
            uint8* DstBCAll = P.BCOutputs[MipIdx].GetData();
            uint8* DstETC2All = AltMip.Bytes.GetData();

            const int32 RowsPerBatch = FMath::Max(1, 256 / FMath::Max(1, BlocksX));
            for (int32 Layer = 0; Layer < Layers; ++Layer)
            {
                const uint8* SrcLayer = SrcAll + static_cast<int64>(Layer) * LayerSrcBytes;
                uint8* DstBCLayer = DstBCAll + static_cast<int64>(Layer) * LayerDstBytes;
                uint8* DstETC2Layer = DstETC2All + static_cast<int64>(Layer) * LayerDstBytes;

                for (int32 BY = 0; BY < BlocksY; BY += RowsPerBatch)
                {
                    FBlockRowBatchJob Job;
                    Job.SrcLayer = SrcLayer;
                    Job.DstBCRow = DstBCLayer + static_cast<int64>(BY) * BlocksX * BlockBytes;
                    Job.DstETC2Row = DstETC2Layer + static_cast<int64>(BY) * BlocksX * BlockBytes;
                    Job.W = W;
                    Job.H = H;
                    Job.BlocksX = BlocksX;
                    Job.StartBY = BY;
                    Job.EndBY = FMath::Min(BY + RowsPerBatch, BlocksY);
                    Job.BlockBytes = BlockBytes;
                    Job.bBGRA = P.bBGRA;
                    Job.bHasAlpha = P.bHasAlpha;
                    Jobs.Add(Job);
                }
            }
        }
    }

    // Step 3: Encode all BC1/BC3 + ETC2 blocks across all textures & mips in parallel.
    ParallelFor(Jobs.Num(), [&](const int32 JobIdx)
    {
        const FBlockRowBatchJob& Job = Jobs[JobIdx];
        uint8 BlockRGBA[64];
        const int64 RowStrideBytes = static_cast<int64>(Job.BlocksX) * Job.BlockBytes;

        for (int32 BY = Job.StartBY; BY < Job.EndBY; ++BY)
        {
            const int32 LocalRow = BY - Job.StartBY;
            uint8* DstBCRow = Job.DstBCRow + static_cast<int64>(LocalRow) * RowStrideBytes;
            uint8* DstETC2Row = Job.DstETC2Row + static_cast<int64>(LocalRow) * RowStrideBytes;

            for (int32 BX = 0; BX < Job.BlocksX; ++BX)
            {
                for (int32 PY = 0; PY < 4; ++PY)
                {
                    const int32 SY = FMath::Min(BY * 4 + PY, Job.H - 1);
                    const uint8* SrcScan = Job.SrcLayer + static_cast<int64>(SY) * Job.W * 4;
                    for (int32 PX = 0; PX < 4; ++PX)
                    {
                        const int32 SX = FMath::Min(BX * 4 + PX, Job.W - 1);
                        const uint8* P = SrcScan + SX * 4;
                        uint8* OutP = BlockRGBA + (PY * 4 + PX) * 4;
                        if (Job.bBGRA)
                        {
                            OutP[0] = P[2]; OutP[1] = P[1]; OutP[2] = P[0]; OutP[3] = P[3];
                        }
                        else
                        {
                            OutP[0] = P[0]; OutP[1] = P[1]; OutP[2] = P[2]; OutP[3] = P[3];
                        }
                    }
                }

                uint8* DstBCBlock = DstBCRow + static_cast<int64>(BX) * Job.BlockBytes;
                uint8* DstETC2Block = DstETC2Row + static_cast<int64>(BX) * Job.BlockBytes;
                if (Job.bHasAlpha)
                {
                    EncodeBC3AlphaBlock(BlockRGBA, DstBCBlock);
                    EncodeBCColorBlock(BlockRGBA, DstBCBlock + 8);
                    EncodeEACAlphaBlock(BlockRGBA, DstETC2Block);
                    EncodeETC2RGBBlock(BlockRGBA, DstETC2Block + 8);
                }
                else
                {
                    EncodeBCColorBlock(BlockRGBA, DstBCBlock);
                    EncodeETC2RGBBlock(BlockRGBA, DstETC2Block);
                }
            }
        }
    });

    // Step 4: Commit compressed buffers back to Model.Textures.
    for (FPreparedTexture& P : Prepared)
    {
        FGWorldBakedTexture& Texture = Model.Textures[P.TextureIndex];
        for (int32 MipIdx = 0; MipIdx < Texture.Mips.Num(); ++MipIdx)
        {
            Texture.Mips[MipIdx].Bytes = MoveTemp(P.BCOutputs[MipIdx]);
        }
        Texture.AltMips = MoveTemp(P.AltOutputs);
        Texture.PixelFormat = P.PrimaryFormat;
        Texture.AltPixelFormat = P.AltFormat;
    }
}

bool V3DTextureCompression::DecompressToBGRA(FGWorldBakedTexture& Texture)
{
    if (Texture.PixelFormat != PF_DXT1 && Texture.PixelFormat != PF_DXT5)
    {
        return false;
    }

    const bool bIsBC1 = (Texture.PixelFormat == PF_DXT1);
    const int32 BlockBytes = bIsBC1 ? 8 : 16;

    for (FGWorldBakedTextureMip& Mip : Texture.Mips)
    {
        const int32 W = Mip.SizeX;
        const int32 H = Mip.SizeY;
        const int32 Layers = Mip.SizeZ;
        if (W <= 0 || H <= 0 || Layers <= 0) return false;

        const int32 BlocksX = (W + 3) / 4;
        const int32 BlocksY = (H + 3) / 4;
        const int64 LayerSrcBytes = static_cast<int64>(BlocksX) * BlocksY * BlockBytes;
        const int64 LayerDstBytes = static_cast<int64>(W) * H * 4;
        if (Mip.Bytes.Num() != LayerSrcBytes * Layers)
        {
            return false;
        }

        TArray<uint8> Decoded;
        Decoded.SetNumUninitialized(static_cast<int32>(LayerDstBytes * Layers));

        const uint8* SrcAll = Mip.Bytes.GetData();
        uint8* DstAll = Decoded.GetData();
        const int32 TotalBlockRows = Layers * BlocksY;

        ParallelFor(TotalBlockRows, [&](const int32 RowIndex)
        {
            const int32 Layer = RowIndex / BlocksY;
            const int32 BY = RowIndex % BlocksY;
            const uint8* SrcRow = SrcAll + static_cast<int64>(Layer) * LayerSrcBytes
                + static_cast<int64>(BY) * BlocksX * BlockBytes;
            uint8* DstLayer = DstAll + static_cast<int64>(Layer) * LayerDstBytes;

            uint8 BlockBGRA[64];
            for (int32 BX = 0; BX < BlocksX; ++BX)
            {
                const uint8* SrcBlock = SrcRow + static_cast<int64>(BX) * BlockBytes;
                if (bIsBC1)
                {
                    V3DTextureCompressionPrivate::DecodeColorBlockBGRA(SrcBlock, true, BlockBGRA);
                }
                else
                {
                    V3DTextureCompressionPrivate::DecodeColorBlockBGRA(SrcBlock + 8, false, BlockBGRA);
                    V3DTextureCompressionPrivate::DecodeAlphaBlockBGRA(SrcBlock, BlockBGRA);
                }

                for (int32 PY = 0; PY < 4; ++PY)
                {
                    const int32 DY = BY * 4 + PY;
                    if (DY >= H) break;
                    uint8* DstScan = DstLayer + static_cast<int64>(DY) * W * 4;
                    for (int32 PX = 0; PX < 4; ++PX)
                    {
                        const int32 DX = BX * 4 + PX;
                        if (DX >= W) break;
                        const uint8* P = BlockBGRA + (PY * 4 + PX) * 4;
                        uint8* OutP = DstScan + DX * 4;
                        OutP[0] = P[0]; OutP[1] = P[1]; OutP[2] = P[2]; OutP[3] = P[3];
                    }
                }
            }
        });

        Mip.Bytes = MoveTemp(Decoded);
    }

    Texture.PixelFormat = PF_B8G8R8A8;
    Texture.AltPixelFormat = PF_Unknown;
    Texture.AltMips.Empty();
    return true;
}
