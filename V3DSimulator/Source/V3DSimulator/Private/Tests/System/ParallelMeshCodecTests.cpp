// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldArchiveCodec.h"
#include "Format/v3d.hpp"
#include "Misc/AutomationTest.h"
#include "PixelFormat.h"
#include "Engine/Texture.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DLargeMeshCodecOrderTest,
    "V3DSimulator.Format.LargeMeshCodecOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DLargeMeshCodecOrderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // More than the conversion threshold, not divisible by four: verifies every lane boundary
    // and the tail, with enough vertices for a valid triangle list.
    constexpr int32 Count = 65541;
    FGWorldBakedMesh Input;
    Input.MeshIndex = 0;
    Input.Name = TEXT("Large vertex streams");
    Input.bHasNormals = Input.bHasTangents = Input.bHasUV = Input.bHasVertexColors = true;
    FGWorldBakedPrimitive& Primitive = Input.Primitives.AddDefaulted_GetRef();
    Primitive.bHasIndices = true;
    Primitive.bHighPrecisionUVs = Primitive.bHighPrecisionWeights = true;
    Primitive.UVs.SetNum(1);
    Primitive.Joints.SetNum(1);
    Primitive.Weights.SetNum(1);
    FGWorldBakedMorphTarget& Morph = Primitive.MorphTargets.AddDefaulted_GetRef();
    Morph.Name = TEXT("Shape");
    TArray<float>& WeightMap = Primitive.WeightMaps.Add(TEXT("Pin"));

    v3d::mesh Canonical;
    Canonical.mesh_index = 0;
    Canonical.name = "Large vertex streams";
    Canonical.has_normals = Canonical.has_tangents = Canonical.has_uv = Canonical.has_vertex_colors = true;
    v3d::primitive& Expected = Canonical.primitives.emplace_back();
    Expected.has_indices = Expected.high_precision_uvs = Expected.high_precision_weights = true;
    Expected.uvs.resize(1);
    Expected.joints.resize(1);
    Expected.weights.resize(1);
    v3d::morph_target& ExpectedMorph = Expected.morph_targets.emplace_back();
    ExpectedMorph.name = "Shape";

    for (int32 Index = 0; Index < Count; ++Index)
    {
        const float X = static_cast<float>(Index);
        const float Y = static_cast<float>(Index % 17);
        const float Z = -static_cast<float>(Index % 23);
        const uint16 Joint = static_cast<uint16>(Index % 8);
        Primitive.Positions.Add(FVector3f(X, Y, Z));
        Primitive.Normals.Add(FVector3f(0, 0, 1));
        Primitive.Tangents.Add(FVector4f(1, 0, 0, -1));
        Primitive.UVs[0].Add(FVector2f(Y, Z));
        Primitive.Indices.Add(static_cast<uint32>(Index));
        FGWorldBakedJoint4 Joints;
        Joints.X = Joint;
        Primitive.Joints[0].Add(Joints);
        Primitive.Weights[0].Add(FVector4f(1, 0, 0, 0));
        Primitive.Colors.Add(FVector4f(Y / 16.f, 0.5f, 1, 1));
        Morph.Positions.Add(FVector3f(0, Y, 0));
        Morph.Normals.Add(FVector3f(0, 0, 0));
        WeightMap.Add(Y / 16.f);

        Expected.positions.push_back({X, Y, Z});
        Expected.normals.push_back({0, 0, 1});
        Expected.tangents.push_back({1, 0, 0, -1});
        Expected.uvs[0].push_back({Y, Z});
        Expected.indices.push_back(static_cast<uint32>(Index));
        Expected.joints[0].push_back({Joint, 0, 0, 0});
        Expected.weights[0].push_back({1, 0, 0, 0});
        Expected.colors.push_back({Y / 16.f, 0.5f, 1, 1});
        ExpectedMorph.positions.push_back({0, Y, 0});
        ExpectedMorph.normals.push_back({0, 0, 0});
        Expected.weight_maps["Pin"].push_back(Y / 16.f);
    }

    FString Error;
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Large Unreal mesh encodes"), WorldArchiveCodec::SerializeMesh(Input, Bytes, Error)))
    { AddError(Error); return false; }
    v3d::bytes ExpectedBytes;
    std::string Detail;
    if (!TestTrue(TEXT("Independent serial portable API encodes"), v3d::encode(Canonical, ExpectedBytes, Detail)))
        return false;
    TestEqual(TEXT("Wire size matches independent portable producer"), Bytes.Num(), static_cast<int32>(ExpectedBytes.size()));
    TestTrue(TEXT("Parallel adapter produces canonical bytes in the original order"),
        Bytes.Num() == static_cast<int32>(ExpectedBytes.size())
        && FMemory::Memcmp(Bytes.GetData(), ExpectedBytes.data(), Bytes.Num()) == 0);

    FGWorldBakedMesh Output;
    if (!TestTrue(TEXT("Large Unreal mesh decodes"), WorldArchiveCodec::DeserializeMesh(Bytes, Output, Error)))
    { AddError(Error); return false; }
    if (!TestEqual(TEXT("One primitive restored"), Output.Primitives.Num(), 1)) return false;
    const FGWorldBakedPrimitive& Restored = Output.Primitives[0];
    const TArray<float>* RestoredWeights = Restored.WeightMaps.Find(TEXT("Pin"));
    bool bAllStreamsMatch = RestoredWeights && Restored.Positions == Primitive.Positions
        && Restored.Normals == Primitive.Normals && Restored.Tangents == Primitive.Tangents
        && Restored.UVs == Primitive.UVs && Restored.Indices == Primitive.Indices
        && Restored.Weights == Primitive.Weights && Restored.Colors == Primitive.Colors
        && *RestoredWeights == WeightMap;
    if (Restored.Joints.Num() != 1 || Restored.Joints[0].Num() != Count
        || Restored.MorphTargets.Num() != 1) bAllStreamsMatch = false;
    if (bAllStreamsMatch)
    {
        bAllStreamsMatch = Restored.MorphTargets[0].Positions == Morph.Positions
            && Restored.MorphTargets[0].Normals == Morph.Normals;
        for (int32 Index = 0; Index < Count && bAllStreamsMatch; ++Index)
        {
            const FGWorldBakedJoint4& Actual = Restored.Joints[0][Index];
            bAllStreamsMatch = Actual.X == static_cast<uint16>(Index % 8)
                && Actual.Y == 0 && Actual.Z == 0 && Actual.W == 0;
        }
    }
    TestTrue(TEXT("All vertex attributes, indices, joints, morphs and weight maps preserve order"), bAllStreamsMatch);
    TArray<uint8> RepeatedBytes;
    TestTrue(TEXT("Decoded result reencodes"), WorldArchiveCodec::SerializeMesh(Output, RepeatedBytes, Error));
    TestTrue(TEXT("Roundtrip is deterministic"), Bytes == RepeatedBytes);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DLargeTextureCodecCopyTest,
    "V3DSimulator.Format.LargeTextureCodecCopy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DLargeTextureCodecCopyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FGWorldBakedTexture Input;
    Input.TextureId = 0;
    Input.Name = TEXT("Bulk texture bytes");
    Input.SizeX = Input.SizeY = 256;
    Input.PixelFormat = PF_B8G8R8A8;
    Input.AddressX = Input.AddressY = TA_Clamp;
    Input.Filter = TF_Trilinear;
    FGWorldBakedTextureMip& Mip = Input.Mips.AddDefaulted_GetRef();
    Mip.SizeX = Mip.SizeY = 256;
    Mip.Bytes.SetNumUninitialized(256 * 256 * 4);
    for (int32 Index = 0; Index < Mip.Bytes.Num(); ++Index)
        Mip.Bytes[Index] = static_cast<uint8>((Index * 37 + Index / 251) & 255);
    FString Error;
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Texture bulk copy encodes"), WorldArchiveCodec::SerializeTexture(Input, Bytes, Error)))
    { AddError(Error); return false; }
    FGWorldBakedTexture Output;
    if (!TestTrue(TEXT("Texture bulk copy decodes"), WorldArchiveCodec::DeserializeTexture(Bytes, Output, Error)))
    { AddError(Error); return false; }
    if (!TestEqual(TEXT("One mip restored"), Output.Mips.Num(), 1)) return false;
    TestTrue(TEXT("Every raw pixel byte is conserved"), Output.Mips[0].Bytes == Mip.Bytes);
    return true;
}
#endif
