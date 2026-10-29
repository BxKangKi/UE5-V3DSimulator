// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldArchiveCodec.h"
#include "Format/v3d.hpp"
#include "Misc/AutomationTest.h"
#include "PixelFormat.h"
#include "Engine/Texture.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DPortableTextureArrayCodecTest,
    "V3DSimulator.Format.PortableTextureArrayCodec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DPortableTextureArrayCodecTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FGWorldBakedTexture In;
    In.TextureId = 0; In.Name = TEXT("Terrain array");
    In.SizeX = 2; In.SizeY = 2; In.NumLayers = 4; In.bTextureArray = true;
    In.PixelFormat = PF_B8G8R8A8; In.AddressX = TA_Clamp; In.AddressY = TA_Mirror;
    In.Filter = TF_Trilinear;
    auto& Top = In.Mips.AddDefaulted_GetRef();
    Top.SizeX = 2; Top.SizeY = 2; Top.SizeZ = 4; Top.Bytes.Init(0x80, 64);
    auto& Small = In.Mips.AddDefaulted_GetRef();
    Small.SizeX = 1; Small.SizeY = 1; Small.SizeZ = 4; Small.Bytes.Init(0x40, 16);
    FString Error; TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Unreal texture encodes"), WorldArchiveCodec::SerializeTexture(In, Bytes, Error)))
    { AddError(Error); return false; }
    v3d::texture Portable; std::string Detail;
    if (!TestTrue(TEXT("External API reads Unreal texture"),
        v3d::decode(Bytes.GetData(), Bytes.Num(), Portable, Detail))) return false;
    TestTrue(TEXT("Wire format is stable BGRA8"), Portable.pixel_format == v3d::texture_format::bgra8_unorm);
    TestEqual(TEXT("Portable array slice count"), Portable.layers, uint32(4));
    TestTrue(TEXT("Array identity preserved"), Portable.is_array);
    FGWorldBakedTexture Out;
    if (!TestTrue(TEXT("Mip-limited Unreal texture decodes"),
        WorldArchiveCodec::DeserializeTexture(Bytes, Out, Error, 1))) return false;
    TestEqual(TEXT("Oversized mip skipped"), Out.Mips.Num(), 1);
    TestEqual(TEXT("Selected width"), Out.SizeX, 1);
    TestEqual(TEXT("All slices retained"), Out.NumLayers, 4);
    TestEqual(TEXT("Mip layers do not shrink"), Out.Mips[0].SizeZ, 4);
    TestEqual(TEXT("Address mode maps back"), Out.AddressY, uint8(TA_Mirror));
    TestEqual(TEXT("Pixel enum maps back"), Out.PixelFormat, int32(PF_B8G8R8A8));
    Bytes.RemoveAt(Bytes.Num() - 1);
    TestFalse(TEXT("Truncated mip rejected"), WorldArchiveCodec::DeserializeTexture(Bytes, Out, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DPortableDependencyCodecTest,
    "V3DSimulator.Format.PortableDependencyCodec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DPortableDependencyCodecTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    v3d::mesh Mesh; Mesh.mesh_index = 7; Mesh.name = "External triangle";
    v3d::primitive P; P.positions = {{0,0,0},{100,0,0},{0,100,0}};
    P.indices = {0,1,2}; P.material_id = 3; P.material_name = "Earth.terrain";
    P.has_material = true; P.has_indices = true;
    Mesh.primitives.push_back(P);
    std::vector<uint8_t> Encoded; std::string Detail;
    if (!TestTrue(TEXT("External API writes mesh"), v3d::encode(Mesh, Encoded, Detail))) return false;
    TArray<uint8> Bytes; Bytes.Append(Encoded.data(), static_cast<int32>(Encoded.size()));
    FGWorldBakedMesh Out; FString Error;
    if (!TestTrue(TEXT("Unreal dependency read"), WorldArchiveCodec::DeserializeMesh(Bytes, Out, Error, true)))
    { AddError(Error); return false; }
    TestEqual(TEXT("Mesh identity"), Out.MeshIndex, 7);
    TestEqual(TEXT("Primitive count"), Out.Primitives.Num(), 1);
    TestEqual(TEXT("Material identity"), Out.Primitives[0].MaterialId, 3);
    TestEqual(TEXT("No geometry allocation in dependency mode"), Out.Primitives[0].Positions.Num(), 0);
    if (!TestTrue(TEXT("Unreal complete read"), WorldArchiveCodec::DeserializeMesh(Bytes, Out, Error)))
    { AddError(Error); return false; }
    TestEqual(TEXT("All vertices available"), Out.Primitives[0].Positions.Num(), 3);
    TestEqual(TEXT("Centimeter units preserved"), Out.Primitives[0].Positions[1].X, 100.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DPortableMaterialDefaultsTest,
    "V3DSimulator.Format.PortableMaterialDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DPortableMaterialDefaultsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    v3d::material Material; Material.material_id = 0; Material.name = "External material";
    const auto ReadMaterial = [this](const v3d::material& Value, FGWorldBakedMaterial& Out)
    {
        v3d::bytes Encoded; std::string Detail;
        if (!TestTrue(TEXT("External material encodes without an Unreal path"),
            v3d::encode(Value, Encoded, Detail))) return false;
        TArray<uint8> Bytes; Bytes.Append(Encoded.data(), static_cast<int32>(Encoded.size()));
        FString Error;
        if (!TestTrue(TEXT("Unreal resolves a default material parent"),
            WorldArchiveCodec::DeserializeMaterial(Bytes, Out, Error)))
        { AddError(Error); return false; }
        return true;
    };
    FGWorldBakedMaterial Out;
    if (!ReadMaterial(Material, Out)) return false;
    TestEqual(TEXT("Opaque lit parent"), Out.BaseMaterialPath,
        FString(TEXT("/glTFRuntime/M_glTFRuntimeBase.M_glTFRuntimeBase")));
    Material.unlit = true;
    if (!ReadMaterial(Material, Out)) return false;
    TestEqual(TEXT("Opaque unlit parent"), Out.BaseMaterialPath,
        FString(TEXT("/glTFRuntime/M_Unlit_glTFRuntimeBase.M_Unlit_glTFRuntimeBase")));
    Material.material_type = 5;
    if (!ReadMaterial(Material, Out)) return false;
    TestEqual(TEXT("Two-sided masked unlit parent"), Out.BaseMaterialPath,
        FString(TEXT("/glTFRuntime/M_Unlit_glTFRuntimeTwoSidedMasked_Inst.M_Unlit_glTFRuntimeTwoSidedMasked_Inst")));
    Material.scalars.push_back({"V3D_Terrain", 1.0f});
    if (!ReadMaterial(Material, Out)) return false;
    TestEqual(TEXT("Terrain palette parent"), Out.BaseMaterialPath,
        FString(TEXT("/Game/V3D/Materials/M_V3DTerrain.M_V3DTerrain")));
    Material.base_material_path = "/Game/Custom/M_Custom.M_Custom";
    if (!ReadMaterial(Material, Out)) return false;
    TestEqual(TEXT("Explicit engine hint remains authoritative"), Out.BaseMaterialPath,
        FString(TEXT("/Game/Custom/M_Custom.M_Custom")));
    return true;
}
#endif
