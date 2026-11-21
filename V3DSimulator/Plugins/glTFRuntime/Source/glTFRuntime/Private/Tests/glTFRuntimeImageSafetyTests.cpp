#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../glTFRuntimeImageValidation.h"

namespace
{
    TArray64<uint8> MakeDDS(uint32 Width, uint32 Height, uint32 MipCount, bool HDR, int64 PixelBytes)
    {
        TArray64<uint8> Bytes;
        Bytes.SetNumZeroed(128 + PixelBytes);
        auto Set = [&](int32 Index, uint32 Value) { FMemory::Memcpy(Bytes.GetData() + Index * 4, &Value, 4); };
        Set(0, 0x20534444); Set(1, 124); Set(2, 0x20000); Set(3, Height); Set(4, Width);
        Set(7, MipCount); Set(19, 32); Set(20, HDR ? 4 : 0x41); Set(21, HDR ? 113 : 0); Set(22, 32);
        return Bytes;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DImageLayoutSafety,
    "V3DSimulator.Safety.Image.LayoutAndBudgets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DImageLayoutSafety::RunTest(const FString&)
{
    using namespace glTFRuntimeImageValidation;
    int64 Bytes = -1;
    TestFalse(TEXT("Negative dimensions rejected"), GetByteSize(-1, 64, PF_B8G8R8A8, Bytes));
    TestFalse(TEXT("Unknown format rejected before indexing GPixelFormats"), GetByteSize(64, 64, PF_Unknown, Bytes));
    TestFalse(TEXT("Out-of-range format rejected"), GetByteSize(64, 64, PF_MAX, Bytes));
    TestFalse(TEXT("A decompression bomb is rejected before pixel allocation"), GetByteSize(MAX_int32, MAX_int32, PF_FloatRGBA, Bytes));
    TestFalse(TEXT("A 1 GiB RGBA texture exceeds the per-resource budget"), GetByteSize(16384, 16384, PF_B8G8R8A8, Bytes));
    TestTrue(TEXT("Sub-block compressed mip has one whole block"), GetByteSize(1, 1, PF_DXT1, Bytes));
    TestEqual(TEXT("BC1 tail allocation"), Bytes, int64(8));
    TestEqual(TEXT("Limits never enlarge small textures"), FitWithin(64, 32, 1024, 1024), FIntPoint(64, 32));
    TestEqual(TEXT("Landscape image retains aspect ratio"), FitWithin(4096, 2048, 1024, 1024), FIntPoint(1024, 512));
    TArray<FglTFRuntimeMipMap> Mips;
    Mips.Emplace(-1, PF_FloatRGBA, 4, 4);
    Mips[0].Pixels.SetNumZeroed(64);
    TestFalse(TEXT("BGRA-sized bytes cannot claim to be RGBA16F"), ValidateChain(Mips));
    Mips[0].Pixels.SetNumZeroed(128);
    TestTrue(TEXT("Correct HDR layout accepted"), ValidateChain(Mips));
    Mips.Emplace(-1, PF_FloatRGBA, 3, 2);
    Mips[1].Pixels.SetNumZeroed(48);
    TestFalse(TEXT("Non-halving mip chain rejected"), ValidateChain(Mips));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DImageDecodeSafety,
    "V3DSimulator.Safety.Image.DecodeHDRAndResize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DImageDecodeSafety::RunTest(const FString&)
{
    auto Parser = MakeShared<FglTFRuntimeParser>(MakeShared<FJsonObject>(), FMatrix::Identity, 1.0f);
    FglTFRuntimeMaterialsConfig Config;
    Config.bLoadMipMaps = false; Config.bGeneratesMipMaps = true;
    TArray<FglTFRuntimeMipMap> Mips;
    const auto HDR = MakeDDS(4, 4, 1, true, 128);
    TestTrue(TEXT("HDR decoded without 8-bit reinterpretation"), Parser->LoadBlobToMips(HDR, Mips, false, Config));
    if (TestEqual(TEXT("HDR keeps a valid base mip"), Mips.Num(), 1))
    {
        TestEqual(TEXT("HDR bytes are preserved"), Mips[0].Pixels.Num(), int64(128));
        TestEqual(TEXT("HDR pixel format is preserved"), Mips[0].PixelFormat, PF_FloatRGBA);
    }
    Mips.Reset();
    Config.ImagesConfig.MaxWidth = 4; Config.ImagesConfig.MaxHeight = 4;
    auto Color = MakeDDS(8, 4, 1, false, 128);
    FMemory::Memset(Color.GetData() + 128, 255, 128);
    TestTrue(TEXT("BGRA resizes and creates complete mip chain"), Parser->LoadBlobToMips(Color, Mips, true, Config));
    if (TestEqual(TEXT("4x2, 2x1, 1x1 mip chain"), Mips.Num(), 3))
    {
        TestEqual(TEXT("Resized aspect ratio"), Mips[0].Height, 2);
        TestEqual(TEXT("Base byte count"), Mips[0].Pixels.Num(), int64(32));
        TestEqual(TEXT("Tail pixel retains white"), Mips.Last().Pixels[0], uint8(255));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DDDSRollbackSafety,
    "V3DSimulator.Safety.Image.DDSRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DDDSRollbackSafety::RunTest(const FString&)
{
    TArray<FglTFRuntimeMipMap> Mips;
    const auto Truncated = MakeDDS(4, 4, 3, false, 64); // base exists, remaining levels do not
    FglTFRuntimeDDS(Truncated).LoadMips(-1, Mips, 0, FglTFRuntimeImagesConfig());
    TestTrue(TEXT("A truncated DDS cannot publish a partial chain"), Mips.IsEmpty());
    const auto Huge = MakeDDS(MAX_uint32, MAX_uint32, 1, false, 1);
    FglTFRuntimeDDS(Huge).LoadMips(-1, Mips, 0, FglTFRuntimeImagesConfig());
    TestTrue(TEXT("Overflowing DDS dimensions rejected"), Mips.IsEmpty());
    const TArray64<uint8> Empty;
    FglTFRuntimeDDS(Empty).LoadMips(-1, Mips, 0, FglTFRuntimeImagesConfig());
    TestTrue(TEXT("Direct LoadMips validates the header"), Mips.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DCubeLayoutSafety,
    "V3DSimulator.Safety.Image.CubeFaces", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DCubeLayoutSafety::RunTest(const FString&)
{
    auto Parser = MakeShared<FglTFRuntimeParser>(MakeShared<FJsonObject>(), FMatrix::Identity, 1.0f);
    TArray<FglTFRuntimeMipMap> Square, Rectangle;
    Square.Emplace(-1, PF_B8G8R8A8, 4, 4); Square[0].Pixels.SetNumZeroed(64);
    Rectangle.Emplace(-1, PF_B8G8R8A8, 8, 2); Rectangle[0].Pixels.SetNumZeroed(64);
    TestNull(TEXT("Equal byte counts cannot hide invalid cubemap face geometry"), Parser->BuildTextureCube(
        GetTransientPackage(), Square, Rectangle, Square, Square, Square, Square, false,
        FglTFRuntimeImagesConfig(), FglTFRuntimeTextureSampler()));
    return true;
}
#endif
