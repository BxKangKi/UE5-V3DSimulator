// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// The wire format belongs to Format/v3d.hpp. This file only translates Unreal values.
#include "System/WorldArchiveCodec.h"
#include "System/V3DTerrain.h"
#include "Format/v3d.hpp"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"
#include "PixelFormat.h"
#include "Engine/Texture.h"
#include <type_traits>

namespace WorldArchiveCodecPrivate
{
    std::string ToPortable(const FString& Value)
    {
        FTCHARToUTF8 Utf8(*Value);
        return std::string(Utf8.Get(), static_cast<size_t>(Utf8.Length()));
    }
    FString ToUnreal(const std::string& Value)
    {
        if (Value.empty()) return FString();
        const FUTF8ToTCHAR Text(Value.data(), static_cast<int32>(Value.size()));
        return FString(Text.Length(), Text.Get());
    }
    v3d::vec2 ToPortable(const FVector2f& V) { return {V.X, V.Y}; }
    v3d::vec3 ToPortable(const FVector3f& V) { return {V.X, V.Y, V.Z}; }
    v3d::vec4 ToPortable(const FVector4f& V) { return {V.X, V.Y, V.Z, V.W}; }
    v3d::vec4 ToPortable(const FLinearColor& V) { return {V.R, V.G, V.B, V.A}; }
    v3d::dvec3 ToPortable(const FVector& V) { return {V.X, V.Y, V.Z}; }
    v3d::transform ToPortable(const FTransform& V)
    {
        const FQuat Q = V.GetRotation();
        return {{Q.X, Q.Y, Q.Z, Q.W}, ToPortable(V.GetLocation()), ToPortable(V.GetScale3D())};
    }
    FVector2f ToUnreal(const v3d::vec2& V) { return FVector2f(V.x, V.y); }
    FVector3f ToUnreal(const v3d::vec3& V) { return FVector3f(V.x, V.y, V.z); }
    FVector4f ToUnreal(const v3d::vec4& V) { return FVector4f(V.x, V.y, V.z, V.w); }
    FVector ToUnreal(const v3d::dvec3& V) { return FVector(V.x, V.y, V.z); }
    FTransform ToUnreal(const v3d::transform& V)
    {
        return FTransform(FQuat(V.rotation.x, V.rotation.y, V.rotation.z, V.rotation.w),
            ToUnreal(V.translation), ToUnreal(V.scale));
    }
    v3d::joint4 ToPortable(const FGWorldBakedJoint4& V) { return {V.X, V.Y, V.Z, V.W}; }
    FGWorldBakedJoint4 ToUnreal(const v3d::joint4& V)
    {
        FGWorldBakedJoint4 Out; Out.X = V.x; Out.Y = V.y; Out.Z = V.z; Out.W = V.w; return Out;
    }

    // Declare non-vector conversions before the generic array adapters (two-phase lookup).
    v3d::primitive ToPortable(const FGWorldBakedPrimitive& V);
    FGWorldBakedPrimitive ToUnreal(v3d::primitive& V);

    // Only detached vertex streams are eligible. In particular, neither FString/FName/map
    // conversion nor the recursive primitive adapter runs inside this ParallelFor. Exclude bool:
    // std::vector<bool> packs different elements into the same word and cannot use disjoint writes.
    template<typename T>
    constexpr bool IsPlainVertexStreamElement =
        (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
        || std::is_same_v<T, FVector2f> || std::is_same_v<T, FVector3f>
        || std::is_same_v<T, FVector4f> || std::is_same_v<T, FLinearColor>
        || std::is_same_v<T, FGWorldBakedJoint4>
        || std::is_same_v<T, v3d::vec2> || std::is_same_v<T, v3d::vec3>
        || std::is_same_v<T, v3d::vec4> || std::is_same_v<T, v3d::joint4>;

    template<typename ConvertIndex>
    void ConvertLargeVertexStream(const int32 Count, ConvertIndex&& ConvertOne)
    {
        // Bundle reads already shard across a few archive members; keep nested conversion fan-out
        // bounded as well. Lanes write contiguous ranges for locality and never resize a container.
        const int32 Lanes = FMath::Clamp(FPlatformMisc::NumberOfCores() - 1, 1, 4);
        ParallelFor(Lanes, [Count, Lanes, &ConvertOne](const int32 Lane)
        {
            const int32 Begin = static_cast<int32>(static_cast<int64>(Count) * Lane / Lanes);
            const int32 End = static_cast<int32>(static_cast<int64>(Count) * (Lane + 1) / Lanes);
            for (int32 Index = Begin; Index < End; ++Index) ConvertOne(Index);
        });
    }

    template<typename Src, typename Dst, typename Convert>
    void CopyToPortable(const TArray<Src>& In, std::vector<Dst>& Out, Convert&& ConvertOne)
    {
        if constexpr (IsPlainVertexStreamElement<Src> && IsPlainVertexStreamElement<Dst>)
        {
            if (In.Num() >= 65536 && Out.empty())
            {
                Out.resize(static_cast<size_t>(In.Num()));
                ConvertLargeVertexStream(In.Num(), [&In, &Out, &ConvertOne](const int32 Index)
                {
                    Out[static_cast<size_t>(Index)] = ConvertOne(In[Index]);
                });
                return;
            }
        }
        Out.reserve(static_cast<size_t>(In.Num()));
        for (const Src& V : In) Out.push_back(ConvertOne(V));
    }
    template<typename Src, typename Dst, typename Convert>
    void CopyToUnreal(std::vector<Src>& In, TArray<Dst>& Out, Convert&& ConvertOne)
    {
        if constexpr (IsPlainVertexStreamElement<Src> && IsPlainVertexStreamElement<Dst>)
        {
            if (In.size() >= 65536 && Out.IsEmpty())
            {
                const int32 Count = static_cast<int32>(In.size());
                Out.SetNumUninitialized(Count);
                ConvertLargeVertexStream(Count, [&In, &Out, &ConvertOne](const int32 Index)
                {
                    Out[Index] = ConvertOne(In[static_cast<size_t>(Index)]);
                });
                std::vector<Src>().swap(In);
                return;
            }
        }
        Out.Reserve(static_cast<int32>(In.size()));
        for (Src& V : In) Out.Add(ConvertOne(V));
        // Release each portable stream as its Unreal counterpart is filled. Do not retain
        // a second complete mesh/pixel payload through resource finalization.
        std::vector<Src>().swap(In);
    }
    template<typename Src, typename Dst>
    void CopyToPortable(const TArray<Src>& In, std::vector<Dst>& Out)
    {
        CopyToPortable(In, Out, [](const Src& V) { return ToPortable(V); });
    }
    template<typename Src, typename Dst>
    void CopyToUnreal(std::vector<Src>& In, TArray<Dst>& Out)
    {
        CopyToUnreal(In, Out, [](Src& V) { return ToUnreal(V); });
    }

    v3d::primitive ToPortable(const FGWorldBakedPrimitive& V)
    {
        v3d::primitive Out;
        CopyToPortable(V.Positions, Out.positions);
        CopyToPortable(V.Normals, Out.normals);
        CopyToPortable(V.Tangents, Out.tangents);
        Out.uvs.resize(V.UVs.Num());
        for (int32 I = 0; I < V.UVs.Num(); ++I) CopyToPortable(V.UVs[I], Out.uvs[I]);
        CopyToPortable(V.Indices, Out.indices, [](uint32 I) { return I; });
        Out.joints.resize(V.Joints.Num());
        for (int32 I = 0; I < V.Joints.Num(); ++I) CopyToPortable(V.Joints[I], Out.joints[I]);
        Out.weights.resize(V.Weights.Num());
        for (int32 I = 0; I < V.Weights.Num(); ++I) CopyToPortable(V.Weights[I], Out.weights[I]);
        CopyToPortable(V.Colors, Out.colors);
        for (const FGWorldBakedMorphTarget& M : V.MorphTargets)
        {
            v3d::morph_target Target;
            Target.name = ToPortable(M.Name);
            CopyToPortable(M.Positions, Target.positions);
            CopyToPortable(M.Normals, Target.normals);
            Out.morph_targets.push_back(std::move(Target));
        }
        for (const auto& Pair : V.BoneMap) Out.bone_map.emplace(Pair.Key, ToPortable(Pair.Value.ToString()));
        for (const auto& Pair : V.WeightMaps)
            CopyToPortable(Pair.Value, Out.weight_maps[ToPortable(Pair.Key)], [](float F) { return F; });
        Out.material_name = ToPortable(V.MaterialName);
        Out.material_id = V.MaterialId; Out.mode = V.Mode;
        Out.has_material = V.bHasMaterial; Out.high_precision_uvs = V.bHighPrecisionUVs;
        Out.high_precision_weights = V.bHighPrecisionWeights; Out.disable_shadows = V.bDisableShadows;
        Out.has_indices = V.bHasIndices;
        return Out;
    }
    FGWorldBakedPrimitive ToUnreal(v3d::primitive& V)
    {
        FGWorldBakedPrimitive Out;
        CopyToUnreal(V.positions, Out.Positions);
        CopyToUnreal(V.normals, Out.Normals);
        CopyToUnreal(V.tangents, Out.Tangents);
        Out.UVs.SetNum(static_cast<int32>(V.uvs.size()));
        for (int32 I = 0; I < Out.UVs.Num(); ++I) CopyToUnreal(V.uvs[I], Out.UVs[I]);
        CopyToUnreal(V.indices, Out.Indices, [](uint32 I) { return I; });
        Out.Joints.SetNum(static_cast<int32>(V.joints.size()));
        for (int32 I = 0; I < Out.Joints.Num(); ++I) CopyToUnreal(V.joints[I], Out.Joints[I]);
        Out.Weights.SetNum(static_cast<int32>(V.weights.size()));
        for (int32 I = 0; I < Out.Weights.Num(); ++I) CopyToUnreal(V.weights[I], Out.Weights[I]);
        CopyToUnreal(V.colors, Out.Colors);
        for (v3d::morph_target& M : V.morph_targets)
        {
            FGWorldBakedMorphTarget& Target = Out.MorphTargets.AddDefaulted_GetRef();
            Target.Name = ToUnreal(M.name);
            CopyToUnreal(M.positions, Target.Positions);
            CopyToUnreal(M.normals, Target.Normals);
        }
        for (const auto& Pair : V.bone_map) Out.BoneMap.Add(Pair.first, FName(*ToUnreal(Pair.second)));
        for (auto& Pair : V.weight_maps)
            CopyToUnreal(Pair.second, Out.WeightMaps.Add(ToUnreal(Pair.first)), [](float F) { return F; });
        Out.MaterialName = ToUnreal(V.material_name);
        Out.MaterialId = V.material_id; Out.Mode = V.mode;
        Out.bHasMaterial = V.has_material; Out.bHighPrecisionUVs = V.high_precision_uvs;
        Out.bHighPrecisionWeights = V.high_precision_weights; Out.bDisableShadows = V.disable_shadows;
        Out.bHasIndices = V.has_indices;
        return Out;
    }
    v3d::mesh ToPortable(const FGWorldBakedMesh& V)
    {
        v3d::mesh Out;
        Out.mesh_index = V.MeshIndex; Out.name = ToPortable(V.Name);
        CopyToPortable(V.Primitives, Out.primitives);
        CopyToPortable(V.AdditionalTransforms, Out.additional_transforms);
        Out.has_normals = V.bHasNormals; Out.has_tangents = V.bHasTangents;
        Out.has_uv = V.bHasUV; Out.has_vertex_colors = V.bHasVertexColors;
        return Out;
    }
    FGWorldBakedMesh ToUnreal(v3d::mesh& V)
    {
        FGWorldBakedMesh Out;
        Out.MeshIndex = V.mesh_index; Out.Name = ToUnreal(V.name);
        CopyToUnreal(V.primitives, Out.Primitives);
        CopyToUnreal(V.additional_transforms, Out.AdditionalTransforms);
        Out.bHasNormals = V.has_normals; Out.bHasTangents = V.has_tangents;
        Out.bHasUV = V.has_uv; Out.bHasVertexColors = V.has_vertex_colors;
        return Out;
    }
    v3d::skin ToPortable(const FGWorldBakedSkin& V)
    {
        v3d::skin Out; Out.skin_index = V.SkinIndex;
        for (const FGWorldBakedBone& B : V.Bones)
            Out.bones.push_back({ToPortable(B.Name), B.ParentIndex, ToPortable(B.Transform)});
        for (const auto& Pair : V.JointBoneMap)
            Out.joint_bone_map.emplace(Pair.Key, ToPortable(Pair.Value.ToString()));
        return Out;
    }
    FGWorldBakedSkin ToUnreal(const v3d::skin& V)
    {
        FGWorldBakedSkin Out; Out.SkinIndex = V.skin_index;
        for (const v3d::bone& B : V.bones)
        {
            FGWorldBakedBone& Bone = Out.Bones.AddDefaulted_GetRef();
            Bone.Name = ToUnreal(B.name); Bone.ParentIndex = B.parent_index;
            Bone.Transform = ToUnreal(B.local_transform);
        }
        for (const auto& Pair : V.joint_bone_map)
            Out.JointBoneMap.Add(Pair.first, FName(*ToUnreal(Pair.second)));
        return Out;
    }
    v3d::material ToPortable(const FGWorldBakedMaterial& V)
    {
        v3d::material Out; Out.material_id = V.MaterialId; Out.name = ToPortable(V.Name);
        Out.base_material_path = ToPortable(V.BaseMaterialPath); Out.material_type = V.MaterialType;
        Out.unlit = V.bUnlit;
        for (const auto& P : V.Scalars) Out.scalars.push_back({ToPortable(P.Name), P.Value});
        for (const auto& P : V.Vectors) Out.vectors.push_back({ToPortable(P.Name), ToPortable(P.Value)});
        for (const auto& P : V.Textures)
            Out.textures.push_back({ToPortable(P.Name), P.TextureId, ToPortable(P.AssetPath)});
        return Out;
    }
    FString DefaultUnrealMaterialPath(const v3d::material& V)
    {
        // Portable producers describe material behavior and need not know Unreal asset paths.
        for (const auto& P : V.scalars)
            if (P.name == "V3D_Terrain" && P.value == 1.0f)
                return V3DTerrain::DefaultMaterialPath;

        // Keep the same family order and assets used by FglTFRuntimeParser's constructor.
        static const TCHAR* const LitParents[] = {
            TEXT("/glTFRuntime/M_glTFRuntimeBase.M_glTFRuntimeBase"),
            TEXT("/glTFRuntime/M_glTFRuntimeTranslucent_Inst.M_glTFRuntimeTranslucent_Inst"),
            TEXT("/glTFRuntime/M_glTFRuntimeTwoSided_Inst.M_glTFRuntimeTwoSided_Inst"),
            TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedTranslucent_Inst.M_glTFRuntimeTwoSidedTranslucent_Inst"),
            TEXT("/glTFRuntime/M_glTFRuntimeMasked_Inst.M_glTFRuntimeMasked_Inst"),
            TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedMasked_Inst.M_glTFRuntimeTwoSidedMasked_Inst")
        };
        static const TCHAR* const UnlitParents[] = {
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeBase.M_Unlit_glTFRuntimeBase"),
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeTranslucent_Inst.M_Unlit_glTFRuntimeTranslucent_Inst"),
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeTwoSided_Inst.M_Unlit_glTFRuntimeTwoSided_Inst"),
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeTwoSidedTranslucent_Inst.M_Unlit_glTFRuntimeTwoSidedTranslucent_Inst"),
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeMasked_Inst.M_Unlit_glTFRuntimeMasked_Inst"),
            TEXT("/glTFRuntime/M_Unlit_glTFRuntimeTwoSidedMasked_Inst.M_Unlit_glTFRuntimeTwoSidedMasked_Inst")
        };
        // decode() validates the family; keep this helper bounded when called directly as well.
        const uint8 Family = V.material_type <= 5 ? V.material_type : 0;
        return V.unlit ? UnlitParents[Family] : LitParents[Family];
    }
    FGWorldBakedMaterial ToUnreal(const v3d::material& V)
    {
        FGWorldBakedMaterial Out; Out.MaterialId = V.material_id; Out.Name = ToUnreal(V.name);
        Out.BaseMaterialPath = V.base_material_path.empty()
            ? DefaultUnrealMaterialPath(V) : ToUnreal(V.base_material_path);
        Out.MaterialType = V.material_type;
        Out.bUnlit = V.unlit;
        for (const auto& P : V.scalars)
        {
            auto& Parameter = Out.Scalars.AddDefaulted_GetRef();
            Parameter.Name = ToUnreal(P.name); Parameter.Value = P.value;
        }
        for (const auto& P : V.vectors)
        {
            auto& Parameter = Out.Vectors.AddDefaulted_GetRef();
            Parameter.Name = ToUnreal(P.name);
            Parameter.Value = FLinearColor(P.value.x, P.value.y, P.value.z, P.value.w);
        }
        for (const auto& P : V.textures)
        {
            auto& Parameter = Out.Textures.AddDefaulted_GetRef();
            Parameter.Name = ToUnreal(P.name); Parameter.TextureId = P.texture_id;
            Parameter.AssetPath = ToUnreal(P.asset_path);
        }
        return Out;
    }

    // Values on disk are v3d::texture_format, never a cast of Unreal's EPixelFormat.
    bool ToPortablePixelFormat(const int32 In, v3d::texture_format& Out)
    {
        switch (In)
        {
#define V3D_PIXEL_TO_PORTABLE(UE, Portable) case UE: Out = v3d::texture_format::Portable; return true
            V3D_PIXEL_TO_PORTABLE(PF_R8G8B8A8, rgba8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_B8G8R8A8, bgra8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_G8, r8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_R8G8, rg8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_G16, r16_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_FloatRGBA, rgba16_float);
            V3D_PIXEL_TO_PORTABLE(PF_R16F, r16_float);
            V3D_PIXEL_TO_PORTABLE(PF_R32_FLOAT, r32_float);
            V3D_PIXEL_TO_PORTABLE(PF_A32B32G32R32F, rgba32_float);
            V3D_PIXEL_TO_PORTABLE(PF_DXT1, bc1_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_DXT3, bc2_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_DXT5, bc3_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_BC4, bc4_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_BC5, bc5_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_BC6H, bc6h_ufloat);
            V3D_PIXEL_TO_PORTABLE(PF_BC7, bc7_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ASTC_4x4, astc_4x4_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ASTC_6x6, astc_6x6_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ASTC_8x8, astc_8x8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ASTC_10x10, astc_10x10_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ASTC_12x12, astc_12x12_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ETC2_RGB, etc2_rgb8_unorm);
            V3D_PIXEL_TO_PORTABLE(PF_ETC2_RGBA, etc2_rgba8_unorm);
#undef V3D_PIXEL_TO_PORTABLE
            default: return false;
        }
    }
    bool ToUnrealPixelFormat(const v3d::texture_format In, int32& Out)
    {
        switch (In)
        {
#define V3D_PIXEL_TO_UNREAL(Portable, UE) case v3d::texture_format::Portable: Out = UE; return true
            V3D_PIXEL_TO_UNREAL(rgba8_unorm, PF_R8G8B8A8);
            V3D_PIXEL_TO_UNREAL(bgra8_unorm, PF_B8G8R8A8);
            V3D_PIXEL_TO_UNREAL(r8_unorm, PF_G8);
            V3D_PIXEL_TO_UNREAL(rg8_unorm, PF_R8G8);
            V3D_PIXEL_TO_UNREAL(r16_unorm, PF_G16);
            V3D_PIXEL_TO_UNREAL(rgba16_float, PF_FloatRGBA);
            V3D_PIXEL_TO_UNREAL(r16_float, PF_R16F);
            V3D_PIXEL_TO_UNREAL(r32_float, PF_R32_FLOAT);
            V3D_PIXEL_TO_UNREAL(rgba32_float, PF_A32B32G32R32F);
            V3D_PIXEL_TO_UNREAL(bc1_unorm, PF_DXT1);
            V3D_PIXEL_TO_UNREAL(bc2_unorm, PF_DXT3);
            V3D_PIXEL_TO_UNREAL(bc3_unorm, PF_DXT5);
            V3D_PIXEL_TO_UNREAL(bc4_unorm, PF_BC4);
            V3D_PIXEL_TO_UNREAL(bc5_unorm, PF_BC5);
            V3D_PIXEL_TO_UNREAL(bc6h_ufloat, PF_BC6H);
            V3D_PIXEL_TO_UNREAL(bc7_unorm, PF_BC7);
            V3D_PIXEL_TO_UNREAL(astc_4x4_unorm, PF_ASTC_4x4);
            V3D_PIXEL_TO_UNREAL(astc_6x6_unorm, PF_ASTC_6x6);
            V3D_PIXEL_TO_UNREAL(astc_8x8_unorm, PF_ASTC_8x8);
            V3D_PIXEL_TO_UNREAL(astc_10x10_unorm, PF_ASTC_10x10);
            V3D_PIXEL_TO_UNREAL(astc_12x12_unorm, PF_ASTC_12x12);
            V3D_PIXEL_TO_UNREAL(etc2_rgb8_unorm, PF_ETC2_RGB);
            V3D_PIXEL_TO_UNREAL(etc2_rgba8_unorm, PF_ETC2_RGBA);
#undef V3D_PIXEL_TO_UNREAL
            default: return false;
        }
    }
    bool ToPortable(const FGWorldBakedTexture& V, v3d::texture& Out, FString& Error)
    {
        if (!ToPortablePixelFormat(V.PixelFormat, Out.pixel_format))
        {
            Error = FString::Printf(TEXT("Texture '%s' uses unsupported EPixelFormat %d; no portable V3D mapping exists"),
                *V.Name, V.PixelFormat); return false;
        }
        Out.texture_id = V.TextureId; Out.name = ToPortable(V.Name);
        Out.width = V.SizeX; Out.height = V.SizeY; Out.layers = V.NumLayers;
        Out.is_array = V.bTextureArray;
        // The public enum values have explicit mappings even when today's UE values coincide.
        switch (V.AddressX)
        {
            case TA_Wrap: Out.address_x = uint8(v3d::address_mode::wrap); break;
            case TA_Clamp: Out.address_x = uint8(v3d::address_mode::clamp); break;
            case TA_Mirror: Out.address_x = uint8(v3d::address_mode::mirror); break;
            default: Error = TEXT("Unsupported texture AddressX"); return false;
        }
        switch (V.AddressY)
        {
            case TA_Wrap: Out.address_y = uint8(v3d::address_mode::wrap); break;
            case TA_Clamp: Out.address_y = uint8(v3d::address_mode::clamp); break;
            case TA_Mirror: Out.address_y = uint8(v3d::address_mode::mirror); break;
            default: Error = TEXT("Unsupported texture AddressY"); return false;
        }
        switch (V.Filter)
        {
            case TF_Nearest: Out.filter = uint8(v3d::filter_mode::nearest); break;
            case TF_Bilinear: Out.filter = uint8(v3d::filter_mode::bilinear); break;
            case TF_Trilinear: Out.filter = uint8(v3d::filter_mode::trilinear); break;
            case TF_Default: Out.filter = uint8(v3d::filter_mode::default_filter); break;
            default: Error = TEXT("Unsupported texture filter"); return false;
        }
        Out.lod_group = V.LODGroup; Out.srgb = V.bSRGB;
        for (const auto& M : V.Mips)
        {
            v3d::mip Mip; Mip.width = M.SizeX; Mip.height = M.SizeY; Mip.layers = M.SizeZ;
            // Pixel blocks require no value conversion. A bulk copy avoids per-byte adapter work
            // and additional task fan-out while independent archive members already run in parallel.
            if (!M.Bytes.IsEmpty()) Mip.bytes.assign(M.Bytes.GetData(), M.Bytes.GetData() + M.Bytes.Num());
            Out.mips.push_back(std::move(Mip));
        }
        return true;
    }
    bool ToUnreal(v3d::texture& V, FGWorldBakedTexture& Out, FString& Error)
    {
        if (V.legacy_unreal_pixel_format >= 0)
        {
            // Only the legacy TEXR/v1 reader can set this field. It is never written by v6.
            if (V.legacy_unreal_pixel_format <= PF_Unknown || V.legacy_unreal_pixel_format >= PF_MAX)
            { Error = TEXT("Legacy texture has an invalid Unreal pixel format"); return false; }
            Out.PixelFormat = V.legacy_unreal_pixel_format;
        }
        else if (!ToUnrealPixelFormat(V.pixel_format, Out.PixelFormat))
        { Error = TEXT("V3D texture format has no Unreal mapping"); return false; }
        Out.TextureId = V.texture_id; Out.Name = ToUnreal(V.name);
        Out.SizeX = V.width; Out.SizeY = V.height; Out.NumLayers = V.layers;
        Out.bTextureArray = V.is_array;
        switch (v3d::address_mode(V.address_x))
        {
            case v3d::address_mode::wrap: Out.AddressX = TA_Wrap; break;
            case v3d::address_mode::clamp: Out.AddressX = TA_Clamp; break;
            case v3d::address_mode::mirror: Out.AddressX = TA_Mirror; break;
            default: Error = TEXT("V3D texture has an invalid AddressX"); return false;
        }
        switch (v3d::address_mode(V.address_y))
        {
            case v3d::address_mode::wrap: Out.AddressY = TA_Wrap; break;
            case v3d::address_mode::clamp: Out.AddressY = TA_Clamp; break;
            case v3d::address_mode::mirror: Out.AddressY = TA_Mirror; break;
            default: Error = TEXT("V3D texture has an invalid AddressY"); return false;
        }
        switch (v3d::filter_mode(V.filter))
        {
            case v3d::filter_mode::nearest: Out.Filter = TF_Nearest; break;
            case v3d::filter_mode::bilinear: Out.Filter = TF_Bilinear; break;
            case v3d::filter_mode::trilinear: Out.Filter = TF_Trilinear; break;
            case v3d::filter_mode::default_filter: Out.Filter = TF_Default; break;
            default: Error = TEXT("V3D texture has an invalid filter"); return false;
        }
        Out.LODGroup = V.lod_group; Out.bSRGB = V.srgb;
        // lod_group is an optional engine hint, not part of the texture interpretation.
        if (Out.LODGroup >= TEXTUREGROUP_MAX) Out.LODGroup = TEXTUREGROUP_World;
        for (auto& M : V.mips)
        {
            auto& Mip = Out.Mips.AddDefaulted_GetRef();
            Mip.SizeX = M.width; Mip.SizeY = M.height; Mip.SizeZ = M.layers;
            if (!M.bytes.empty()) Mip.Bytes.Append(M.bytes.data(), static_cast<int32>(M.bytes.size()));
            std::vector<uint8_t>().swap(M.bytes);
        }
        return true;
    }
    v3d::range ToPortable(const FGWorldArchiveRange& V)
    {
        return {ToPortable(V.Name), V.Offset, V.StoredSize, V.UncompressedSize, V.Crc, V.Codec};
    }
    FGWorldArchiveRange ToUnreal(const v3d::range& V)
    {
        FGWorldArchiveRange Out; Out.Name = ToUnreal(V.name); Out.Offset = V.offset;
        Out.StoredSize = V.stored_size; Out.UncompressedSize = V.uncompressed_size;
        Out.Crc = V.crc; Out.Codec = V.codec; return Out;
    }
    v3d::manifest ToPortable(const FGWorldModelManifest& V)
    {
        v3d::manifest Out;
        for (const auto& P : V.MeshRanges) Out.mesh_ranges.emplace(P.Key, ToPortable(P.Value));
        for (const auto& P : V.SkinRanges) Out.skin_ranges.emplace(P.Key, ToPortable(P.Value));
        for (const auto& P : V.MaterialRanges) Out.material_ranges.emplace(P.Key, ToPortable(P.Value));
        for (const auto& P : V.TextureRanges) Out.texture_ranges.emplace(P.Key, ToPortable(P.Value));
        for (const auto& P : V.MeshNames) Out.mesh_names.emplace(P.Key, ToPortable(P.Value));
        return Out;
    }
    FGWorldModelManifest ToUnreal(const v3d::manifest& V)
    {
        FGWorldModelManifest Out;
        for (const auto& P : V.mesh_ranges) Out.MeshRanges.Add(P.first, ToUnreal(P.second));
        for (const auto& P : V.skin_ranges) Out.SkinRanges.Add(P.first, ToUnreal(P.second));
        for (const auto& P : V.material_ranges) Out.MaterialRanges.Add(P.first, ToUnreal(P.second));
        for (const auto& P : V.texture_ranges) Out.TextureRanges.Add(P.first, ToUnreal(P.second));
        for (const auto& P : V.mesh_names) Out.MeshNames.Add(P.first, ToUnreal(P.second));
        return Out;
    }
    template<typename T>
    bool Encode(const T& Value, TArray<uint8>& Out, FString& Error)
    {
        Out.Reset(); std::vector<uint8_t> Bytes; std::string Detail;
        if (!v3d::encode(Value, Bytes, Detail)) { Error = ToUnreal(Detail); return false; }
        if (Bytes.size() > size_t(FGWorldArchive::MaxDatMemberBytes))
        { Error = TEXT("Portable V3D member exceeds Unreal's TArray limit"); return false; }
        if (!Bytes.empty()) Out.Append(Bytes.data(), static_cast<int32>(Bytes.size()));
        Error.Reset(); return true;
    }
}

namespace WorldArchiveCodec
{
    using namespace WorldArchiveCodecPrivate;
    bool SerializeMesh(const FGWorldBakedMesh& V, TArray<uint8>& Out, FString& Error)
    { return Encode(ToPortable(V), Out, Error); }
    bool DeserializeMesh(const TArray<uint8>& Bytes, FGWorldBakedMesh& Out, FString& Error, const bool bDependenciesOnly)
    {
        Out = {}; v3d::mesh Value; std::string Detail;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Detail, bDependenciesOnly))
        { Error = ToUnreal(Detail); return false; }
        Out = ToUnreal(Value);
        if (!bDependenciesOnly)
        {
            // Keep engine-specific checks (e.g. FName semantics) after portable validation.
            const int32 MeshIndex = Out.MeshIndex; Out.MeshIndex = 0;
            FGWorldBakedModel Check; Check.Meshes.Add(MoveTemp(Out));
            if (!Check.IsSane(&Error)) { Out = {}; return false; }
            Out = MoveTemp(Check.Meshes[0]); Out.MeshIndex = MeshIndex;
        }
        Error.Reset(); return true;
    }
    bool SerializeSkin(const FGWorldBakedSkin& V, TArray<uint8>& Out, FString& Error)
    { return Encode(ToPortable(V), Out, Error); }
    bool DeserializeSkin(const TArray<uint8>& Bytes, FGWorldBakedSkin& Out, FString& Error)
    {
        Out = {}; v3d::skin Value; std::string Detail;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Detail))
        { Error = ToUnreal(Detail); return false; }
        FGWorldBakedModel Check; Check.Skins.Add(ToUnreal(Value));
        if (!Check.IsSane(&Error)) return false;
        Out = MoveTemp(Check.Skins[0]); Error.Reset(); return true;
    }
    bool SerializeMaterial(const FGWorldBakedMaterial& V, TArray<uint8>& Out, FString& Error)
    { return Encode(ToPortable(V), Out, Error); }
    bool DeserializeMaterial(const TArray<uint8>& Bytes, FGWorldBakedMaterial& Out, FString& Error)
    {
        Out = {}; v3d::material Value; std::string Detail;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Detail))
        { Error = ToUnreal(Detail); return false; }
        Out = ToUnreal(Value); Error.Reset(); return true;
    }
    bool SerializeTexture(const FGWorldBakedTexture& V, TArray<uint8>& Out, FString& Error)
    {
        Out.Reset(); v3d::texture Value;
        if (!ToPortable(V, Value, Error)) return false;
        return Encode(Value, Out, Error);
    }
    bool DeserializeTexture(const TArray<uint8>& Bytes, FGWorldBakedTexture& Out, FString& Error, const int32 MaxResolution)
    {
        Out = {}; v3d::texture Value; std::string Detail;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Detail, static_cast<uint32>(FMath::Max(0, MaxResolution))))
        { Error = ToUnreal(Detail); return false; }
        if (!ToUnreal(Value, Out, Error)) { Out = {}; return false; }
        Error.Reset(); return true;
    }
    bool SerializeManifest(const FGWorldModelManifest& V, TArray<uint8>& Out, FString& Error)
    { return Encode(ToPortable(V), Out, Error); }
    bool DeserializeManifest(const TArray<uint8>& Bytes, FGWorldModelManifest& Out, FString& Error)
    {
        Out = {}; v3d::manifest Value; std::string Detail;
        if (!v3d::decode(Bytes.GetData(), Bytes.Num(), Value, Detail))
        { Error = ToUnreal(Detail); return false; }
        Out = ToUnreal(Value); Error.Reset(); return true;
    }
}
