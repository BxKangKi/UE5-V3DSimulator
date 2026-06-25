// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedData.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class USkeleton;
class UTexture2D;
class UglTFRuntimeAsset;
struct FglTFRuntimeMeshLOD;

/** Four joint indices as stored by glTF's JOINTS_n attributes. */
struct V3DSIMULATOR_API FGWorldBakedJoint4
{
    uint16 X = 0;
    uint16 Y = 0;
    uint16 Z = 0;
    uint16 W = 0;
};

/** One morph-target delta stream. Values are already converted to Unreal coordinates. */
struct V3DSIMULATOR_API FGWorldBakedMorphTarget
{
    FString Name;
    TArray<FVector3f> Positions;
    TArray<FVector3f> Normals;
};

/**
 * One render primitive captured after glTFRuntime decoded the source GLB.
 *
 * The archive intentionally stores decoded arrays instead of accessors/bufferViews. Runtime code
 * can therefore create an Unreal mesh without reopening or parsing the source GLB.
 */
struct V3DSIMULATOR_API FGWorldBakedPrimitive
{
    TArray<FVector3f> Positions;
    TArray<FVector3f> Normals;
    TArray<FVector4f> Tangents;
    TArray<TArray<FVector2f>> UVs;
    TArray<uint32> Indices;
    TArray<TArray<FGWorldBakedJoint4>> Joints;
    TArray<TArray<FVector4f>> Weights;
    TArray<FVector4f> Colors;
    TArray<FGWorldBakedMorphTarget> MorphTargets;
    TMap<int32, FName> BoneMap;
    TMap<FString, TArray<float>> WeightMaps;
    FString MaterialName;
    int32 MaterialId = INDEX_NONE;
    int32 Mode = 4;
    bool bHasMaterial = false;
    bool bHighPrecisionUVs = false;
    bool bHighPrecisionWeights = false;
    bool bDisableShadows = false;
    bool bHasIndices = false;
};

/** One source mesh. Static LOD0..LOD3 rows refer to these indices. */
struct V3DSIMULATOR_API FGWorldBakedMesh
{
    int32 MeshIndex = INDEX_NONE;
    FString Name;
    TArray<FGWorldBakedPrimitive> Primitives;
    TArray<FTransform> AdditionalTransforms;
    bool bHasNormals = false;
    bool bHasTangents = false;
    bool bHasUV = false;
    bool bHasVertexColors = false;
};

/** One bone in parent-before-child reference-pose order. */
struct V3DSIMULATOR_API FGWorldBakedBone
{
    FString Name;
    int32 ParentIndex = INDEX_NONE;
    FTransform Transform = FTransform::Identity;
};

/** Skeleton and joint-index mapping needed to rebuild a skinned mesh without a glTF skin object. */
struct V3DSIMULATOR_API FGWorldBakedSkin
{
    int32 SkinIndex = INDEX_NONE;
    TArray<FGWorldBakedBone> Bones;
    TMap<int32, FName> JointBoneMap;
};

struct V3DSIMULATOR_API FGWorldBakedScalarParameter
{
    FString Name;
    float Value = 0.0f;
};

struct V3DSIMULATOR_API FGWorldBakedVectorParameter
{
    FString Name;
    FLinearColor Value = FLinearColor::Black;
};

/** A texture parameter either references a baked texture member or a cooked project asset. */
struct V3DSIMULATOR_API FGWorldBakedTextureParameter
{
    FString Name;
    int32 TextureId = INDEX_NONE;
    FString AssetPath;
};

/** Material shader identity plus the resolved parameters of the runtime material instance. */
struct V3DSIMULATOR_API FGWorldBakedMaterial
{
    int32 MaterialId = INDEX_NONE;
    FString Name;
    FString BaseMaterialPath;
    /** glTFRuntime material family used to select an optional runtime override shader. */
    uint8 MaterialType = 0;
    bool bUnlit = false;
    TArray<FGWorldBakedScalarParameter> Scalars;
    TArray<FGWorldBakedVectorParameter> Vectors;
    TArray<FGWorldBakedTextureParameter> Textures;
};

struct V3DSIMULATOR_API FGWorldBakedTextureMip
{
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 1;
    TArray<uint8> Bytes;
};

/** Final platform texture data copied from the in-memory texture produced by glTFRuntime. */
struct V3DSIMULATOR_API FGWorldBakedTexture
{
    int32 TextureId = INDEX_NONE;
    FString Name;
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 PixelFormat = 0;
    uint8 AddressX = 0;
    uint8 AddressY = 0;
    uint8 Filter = 0;
    uint8 LODGroup = 0;
    bool bSRGB = false;
    TArray<FGWorldBakedTextureMip> Mips;
};

/** Complete build-time result for one GLB. Members are split when written to .v3d. */
struct V3DSIMULATOR_API FGWorldBakedModel
{
    TArray<FGWorldBakedMesh> Meshes;
    TArray<FGWorldBakedSkin> Skins;
    TArray<FGWorldBakedMaterial> Materials;
    TArray<FGWorldBakedTexture> Textures;

    void Reset();
    bool IsSane(FString* OutError = nullptr) const;
};

/** Only the members needed for one requested mesh (or LOD set) are read into this bundle. */
struct V3DSIMULATOR_API FGWorldBakedAssetBundle
{
    TArray<FGWorldBakedMesh> Meshes;
    TArray<FGWorldBakedSkin> Skins;
    TArray<FGWorldBakedMaterial> Materials;
    TArray<FGWorldBakedTexture> Textures;

    void Reset();
};

/** Build-time conversion from glTFRuntime's decoded in-memory representation. */
class V3DSIMULATOR_API FGWorldBakedDataCapture
{
public:
    static bool CaptureMesh(
        int32 MeshIndex,
        const FString& MeshName,
        const FglTFRuntimeMeshLOD& RuntimeLOD,
        FGWorldBakedModel& InOutModel,
        TMap<TWeakObjectPtr<UMaterialInterface>, int32>& InOutMaterialIds,
        TMap<TWeakObjectPtr<UTexture2D>, int32>& InOutTextureIds,
        FString& OutError);

    static bool CaptureSkin(
        UglTFRuntimeAsset* SourceAsset,
        int32 SkinIndex,
        const TSet<int32>& MeshIndicesUsingSkin,
        const TMap<FString, FString>& BoneAliases,
        USkeleton* TargetSkeleton,
        FGWorldBakedModel& InOutModel,
        FString& OutError);
};
