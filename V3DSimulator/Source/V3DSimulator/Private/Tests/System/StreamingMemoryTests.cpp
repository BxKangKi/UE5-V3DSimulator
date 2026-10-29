// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DStreamingPolicy.h"
#include "../../System/WorldArchiveCodec.h"
#include "Misc/AutomationTest.h"
#include "PixelFormat.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DTextureMipSelectionTest,
    "V3DSimulator.Streaming.Memory.TextureMipSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DTextureMipSelectionTest::RunTest(const FString&)
{
    FGWorldBakedTexture Source;
    Source.TextureId = 3;
    Source.Name = TEXT("MipSelection");
    Source.SizeX = Source.SizeY = 4;
    Source.PixelFormat = PF_B8G8R8A8;
    for (int32 Size = 4; Size > 0; Size >>= 1)
    {
        auto& Mip = Source.Mips.AddDefaulted_GetRef();
        Mip.SizeX = Mip.SizeY = Size;
        Mip.Bytes.Init(uint8(Size), Size * Size * 4);
    }
    TArray<uint8> Bytes;
    FString Error;
    TestTrue(TEXT("Encode full mip chain"), WorldArchiveCodec::SerializeTexture(Source, Bytes, Error));
    FGWorldBakedTexture Decoded;
    TestTrue(TEXT("Decode only selected mips"), WorldArchiveCodec::DeserializeTexture(Bytes, Decoded, Error, 2));
    TestEqual(TEXT("Top mip is capped"), Decoded.SizeX, 2);
    TestEqual(TEXT("Only two mips are allocated"), Decoded.Mips.Num(), 2);
    if (Decoded.Mips.Num() == 2)
    {
        TestEqual(TEXT("Selected mip content preserved"), Decoded.Mips[0].Bytes[0], uint8(2));
        TestEqual(TEXT("Resident selected pixels total 20 bytes, not 84"),
            Decoded.Mips[0].Bytes.Num() + Decoded.Mips[1].Bytes.Num(), 20);
    }
    TestTrue(TEXT("Build-time decode still keeps every mip"),
        WorldArchiveCodec::DeserializeTexture(Bytes, Decoded, Error));
    TestEqual(TEXT("Uncapped decode has three mips"), Decoded.Mips.Num(), 3);
    Bytes.Pop();
    TestFalse(TEXT("Truncated retained mip is rejected"),
        WorldArchiveCodec::DeserializeTexture(Bytes, Decoded, Error, 2));
    Source.Mips[0].SizeX = 3; // This mip is skipped by the cap but must still be validated.
    TestTrue(TEXT("Encode malformed fixture"), WorldArchiveCodec::SerializeTexture(Source, Bytes, Error));
    TestFalse(TEXT("Malformed skipped mip is rejected"),
        WorldArchiveCodec::DeserializeTexture(Bytes, Decoded, Error, 2));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStreamingDistancePolicyTest,
    "V3DSimulator.Streaming.Memory.DistancePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStreamingDistancePolicyTest::RunTest(const FString&)
{
    using namespace V3DStreamingPolicy;
    const double Distance = ScreenSizeDistance(16.0 / 9.0);
    const FVector Size(200, 300, 400);
    const double Radius = Size.Size() * 0.5;
    const double Load = LoadRadius(Size, 1.0, Distance);
    TestTrue(TEXT("Boundary is a 5% projected sphere diameter"),
        FMath::IsNearlyEqual((16.0 / 9.0) * Radius / Load, 0.05, 1.e-8));
    TestTrue(TEXT("Zooming in doubles the visible range"),
        FMath::IsNearlyEqual(LoadRadius(Size, 1.0, ScreenSizeDistance(32.0 / 9.0)), 2.0 * Load, 1.e-6));
    TestEqual(TEXT("Mirrored authored bounds keep the same range"), LoadRadius(-Size, 1.0, Distance), Load);
    TestTrue(TEXT("Tiny props retain the local collision neighbourhood"), LoadRadius(FVector(1), 1.0, Distance) >= 2000.0);
    TestTrue(TEXT("Large visible meshes are not capped at 512m"), LoadRadius(Size, 10.0, Distance) > 51200.0);
    const double BoxEdge = Size.X * 0.5;
    TestFalse(TEXT("Beyond coarse scene range unloads"),
        SceneInRange(FVector::ZeroVector, Size, FTransform::Identity, FVector(BoxEdge + Load * 1.2, 0, 0), Distance));
    TestTrue(TEXT("Unload hysteresis retains the boundary"),
        SceneInRange(FVector::ZeroVector, Size, FTransform::Identity, FVector(BoxEdge + Load * 1.05, 0, 0), Distance, 1.10));
    return true;
}
#endif
