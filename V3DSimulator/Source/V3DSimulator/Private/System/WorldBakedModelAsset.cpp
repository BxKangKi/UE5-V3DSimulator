// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedModelAsset.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldBakedModelAsset.h"
#include "System/WorldBakedTexture.h"
#include "System/V3DStreamingBudget.h"
#include "Misc/ScopeExit.h"

#include "Async/ParallelFor.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"
#include "Setting/GameSettings.h"
#include "Simulator/RuntimeModelResolver.h"
#include "System/SafeFileIO.h"
#include "System/V3DRuntimeSafety.h"
#include "UObject/GCObject.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#endif

/**
 * Request-local GC bridge for transient textures/materials stored in glTFRuntime POD structs.
 * Native stack pointers and non-reflected TMaps are not GC references; this guard keeps every
 * object alive from decoded bundle conversion through the synchronous mesh finalizer call.
 */
struct FWorldBakedBuildReferenceGuard final : public FGCObject
{
    void Add(UObject* Object)
    {
        if (IsValid(Object))
        {
            // Duplicate references are harmless to GC and cheaper than an O(N) AddUnique scan for
            // every material in a large model. The guard exists for one short build request only.
            References.Add(Object);
        }
    }

    void Reserve(const int32 ExpectedReferences)
    {
        References.Reserve(References.Num() + FMath::Max(0, ExpectedReferences));
    }

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (TObjectPtr<UObject>& Reference : References)
        {
            Collector.AddReferencedObject(Reference);
        }
    }

    virtual FString GetReferencerName() const override
    {
        return TEXT("FWorldBakedBuildReferenceGuard");
    }

    void AppendTo(TArray<TObjectPtr<UObject>>& OutReferences) const
    {
        OutReferences.Append(References);
    }

private:
    TArray<TObjectPtr<UObject>> References;
};

namespace WorldBakedModelAssetPrivate
{
    constexpr int32 MaxTextureMips = 32;
    constexpr int64 MaxTextureMipBytes = 1024ll * 1024ll * 1024ll;
    int32 ResolveRequestTextureLimit(const FglTFRuntimeMaterialsConfig& Config, const UObject* Context)
    {
        // Gameplay callers already snapshot the user cap. Facades have no world, so repeatedly
        // resolving settings from them would synchronously open settings.json on the game thread.
        const int32 Width = Config.ImagesConfig.MaxWidth;
        const int32 Height = Config.ImagesConfig.MaxHeight;
        const int32 Requested = Width > 0 && Height > 0 ? FMath::Min(Width, Height) : FMath::Max(Width, Height);
        return Requested > 0 ? FMath::Clamp(Requested, 64, 8192)
            : UGameSettings::ResolveMaxTextureResolution(Context);
    }

    uint32 BuildMaterialConfigSignature(const FglTFRuntimeMaterialsConfig& Config)
    {
        uint32 Hash = 0x9e3779b9u;
        Hash = HashCombineFast(Hash, PointerHash(Config.ForceMaterial));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.MaxWidth));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.MaxHeight));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.bCompressMips));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.bStreaming));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bMaterialsOverrideMapInjectParams));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bSkipLoad));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bGeneratesMipMaps));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bLoadMipMaps));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.SpecularFactor));

        TArray<FString> NamedKeys;
        Config.MaterialsOverrideByNameMap.GenerateKeyArray(NamedKeys);
        NamedKeys.Sort();
        for (const FString& Key : NamedKeys)
        {
            UMaterialInterface* const* Material = Config.MaterialsOverrideByNameMap.Find(Key);
            Hash = HashCombineFast(Hash, GetTypeHash(Key));
            Hash = HashCombineFast(Hash, PointerHash(Material ? *Material : nullptr));
        }

        const auto HashTypedMap = [&Hash](const TMap<EglTFRuntimeMaterialType, UMaterialInterface*>& Map)
        {
            TArray<EglTFRuntimeMaterialType> Keys;
            Map.GenerateKeyArray(Keys);
            Keys.Sort([](const EglTFRuntimeMaterialType A, const EglTFRuntimeMaterialType B)
            {
                return static_cast<uint8>(A) < static_cast<uint8>(B);
            });
            for (const EglTFRuntimeMaterialType Key : Keys)
            {
                UMaterialInterface* const* Material = Map.Find(Key);
                Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(Key)));
                Hash = HashCombineFast(Hash, PointerHash(Material ? *Material : nullptr));
            }
        };
        HashTypedMap(Config.UberMaterialsOverrideMap);
        HashTypedMap(Config.UnlitOverrideMap);

        TArray<FString> ScalarKeys;
        Config.ScalarParamsOverrides.GenerateKeyArray(ScalarKeys);
        ScalarKeys.Sort();
        for (const FString& Key : ScalarKeys)
        {
            const float* Value = Config.ScalarParamsOverrides.Find(Key);
            Hash = HashCombineFast(Hash, GetTypeHash(Key));
            Hash = HashCombineFast(Hash, Value ? GetTypeHash(*Value) : 0u);
        }
        return Hash;
    }

    uint64 BuildMaterialCacheKey(
        const int32 MaterialId,
        const FglTFRuntimeMaterialsConfig& Config)
    {
        return (static_cast<uint64>(static_cast<uint32>(MaterialId)) << 32)
            | static_cast<uint64>(BuildMaterialConfigSignature(Config));
    }

    FString BuildSharedRenderMeshCacheKey(
        const TArray<int32>& MeshIndices,
        const FglTFRuntimeStaticMeshConfig& Config)
    {
        uint32 MeshConfigHash = 0x85ebca6bu;
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bReverseWinding));
        MeshConfigHash = HashCombineFast(MeshConfigHash,
            GetTypeHash(static_cast<uint8>(Config.NormalsGenerationStrategy)));
        MeshConfigHash = HashCombineFast(MeshConfigHash,
            GetTypeHash(static_cast<uint8>(Config.TangentsGenerationStrategy)));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bReverseTangents));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bUseHighPrecisionUVs));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bUseHighPrecisionTangentBasis));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bGenerateStaticMeshDescription));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bBuildLumenCards));

        FString Key = FString::Printf(
            TEXT("mat=%08x;mesh=%08x;mul=%08x;lod="),
            BuildMaterialConfigSignature(Config.MaterialsConfig),
            MeshConfigHash,
            GetTypeHash(Config.LODScreenSizeMultiplier));
        Key.Reserve(Key.Len() + MeshIndices.Num() * 16 + Config.LODScreenSize.Num() * 24);
        for (const int32 MeshIndex : MeshIndices)
        {
            Key += FString::Printf(TEXT("%d,"), MeshIndex);
        }
        Key += TEXT(";screen=");
        TArray<int32> ScreenKeys;
        Config.LODScreenSize.GenerateKeyArray(ScreenKeys);
        ScreenKeys.Sort();
        for (const int32 ScreenKey : ScreenKeys)
        {
            const float* ScreenSize = Config.LODScreenSize.Find(ScreenKey);
            Key += FString::Printf(
                TEXT("%d:%08x,"), ScreenKey, ScreenSize ? GetTypeHash(*ScreenSize) : 0u);
        }
        return Key;
    }

    UMaterialInterface* SelectBaseMaterial(
        const FGWorldBakedMaterial& Baked,
        const FglTFRuntimeMaterialsConfig& Config)
    {
        if (IsValid(Config.ForceMaterial))
        {
            return Config.ForceMaterial;
        }
        if (UMaterialInterface* const* Named =
                Config.MaterialsOverrideByNameMap.Find(Baked.Name))
        {
            if (IsValid(*Named))
            {
                return *Named;
            }
        }

        const EglTFRuntimeMaterialType Type =
            static_cast<EglTFRuntimeMaterialType>(Baked.MaterialType);
        const TMap<EglTFRuntimeMaterialType, UMaterialInterface*>& Overrides =
            Baked.bUnlit ? Config.UnlitOverrideMap : Config.UberMaterialsOverrideMap;
        if (UMaterialInterface* const* Typed = Overrides.Find(Type))
        {
            if (IsValid(*Typed))
            {
                return *Typed;
            }
        }

        // The archive stores the shader asset identity, not a transient MID pointer. Project and
        // plugin master materials must consequently remain cooked just as they did for glTFRuntime.
        return LoadObject<UMaterialInterface>(nullptr, *Baked.BaseMaterialPath);
    }

    bool CheckedMultiply(const int64 A, const int64 B, int64& Out)
    {
        if (A <= 0 || B <= 0 || A > MAX_int64 / B)
        {
            Out = 0;
            return false;
        }
        Out = A * B;
        return true;
    }

    /**
     * Validates every field that is consumed by FTexturePlatformData/RHI. A CRC only proves that a
     * member matches its directory entry; it does not make malicious dimensions or enums safe.
     */
    bool ValidateTextureLayout(const FGWorldBakedTexture& Baked, FString& OutError)
    {
        if (Baked.SizeX <= 0 || Baked.SizeY <= 0
            || Baked.PixelFormat <= PF_Unknown || Baked.PixelFormat >= PF_MAX
            || Baked.AddressX >= static_cast<uint8>(TA_MAX)
            || Baked.AddressY >= static_cast<uint8>(TA_MAX)
            || Baked.Filter >= static_cast<uint8>(TF_MAX)
            || Baked.LODGroup >= static_cast<uint8>(TEXTUREGROUP_MAX)
            || Baked.Mips.IsEmpty() || Baked.Mips.Num() > MaxTextureMips)
        {
            OutError = FString::Printf(
                TEXT("Unsupported baked texture metadata: %s"), *Baked.Name);
            return false;
        }

        const FPixelFormatInfo& Format = GPixelFormats[Baked.PixelFormat];
        if (!Format.Supported || Format.BlockSizeX <= 0 || Format.BlockSizeY <= 0
            || Format.BlockSizeZ <= 0 || Format.BlockBytes <= 0)
        {
            OutError = FString::Printf(
                TEXT("Baked texture pixel format is unsupported on this platform: %s"),
                *Baked.Name);
            return false;
        }

        int32 ExpectedSizeX = Baked.SizeX;
        int32 ExpectedSizeY = Baked.SizeY;
        for (const FGWorldBakedTextureMip& Mip : Baked.Mips)
        {
            if (Mip.SizeX != ExpectedSizeX || Mip.SizeY != ExpectedSizeY
                || Mip.SizeZ != 1 || Mip.Bytes.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("Baked texture has an invalid mip chain: %s"), *Baked.Name);
                return false;
            }

            const int64 BlocksX =
                (static_cast<int64>(Mip.SizeX) + Format.BlockSizeX - 1) / Format.BlockSizeX;
            const int64 BlocksY =
                (static_cast<int64>(Mip.SizeY) + Format.BlockSizeY - 1) / Format.BlockSizeY;
            const int64 BlocksZ =
                (static_cast<int64>(Mip.SizeZ) + Format.BlockSizeZ - 1) / Format.BlockSizeZ;
            int64 BlockCountXY = 0;
            int64 BlockCount = 0;
            int64 ExpectedBytes = 0;
            if (!CheckedMultiply(BlocksX, BlocksY, BlockCountXY)
                || !CheckedMultiply(BlockCountXY, BlocksZ, BlockCount)
                || !CheckedMultiply(BlockCount, Format.BlockBytes, ExpectedBytes)
                || ExpectedBytes > MaxTextureMipBytes
                || ExpectedBytes != static_cast<int64>(Mip.Bytes.Num()))
            {
                OutError = FString::Printf(
                    TEXT("Baked texture mip byte count is invalid: %s"), *Baked.Name);
                return false;
            }

            ExpectedSizeX = FMath::Max(1, ExpectedSizeX >> 1);
            ExpectedSizeY = FMath::Max(1, ExpectedSizeY >> 1);
        }
        return true;
    }

    /**
     * Attaches the data-free parser required by glTFRuntime's RuntimeLOD finalizers. The caller
     * must already own FV3DRuntimeSafety's synchronous native gate because parser construction and
     * SetParser touch the same third-party mutable state as mesh creation.
     */
    bool InitializeEmptyMeshBuilder(UglTFRuntimeAsset* Builder)
    {
        if (!IsValid(Builder))
        {
            return false;
        }
        const TSharedRef<FJsonObject> EmptyRoot = MakeShared<FJsonObject>();
        const TSharedRef<FglTFRuntimeParser> EmptyParser =
            MakeShared<FglTFRuntimeParser>(EmptyRoot, FMatrix::Identity, 1.0f);
        return Builder->SetParser(EmptyParser);
    }

    /**
     * Converts archive POD arrays into glTFRuntime RuntimeLODs without touching any UObject.
     * This function is intentionally worker-safe and is the expensive half of reconstruction.
     */
    bool BuildRuntimeLODsDetached(
        FGWorldBakedAssetBundle& Bundle,
        const int32 SkinIndex,
        const bool bLoadMaterials,
        TArray<FglTFRuntimeMeshLOD>& OutLODs,
        TArray<TArray<int32>>& OutMaterialIds,
        FString& OutError)
    {
        OutLODs.Reset();
        OutMaterialIds.Reset();
        OutError.Reset();

        const FGWorldBakedSkin* Skin = nullptr;
        if (SkinIndex != INDEX_NONE)
        {
            for (const FGWorldBakedSkin& Candidate : Bundle.Skins)
            {
                if (Candidate.SkinIndex == SkinIndex)
                {
                    Skin = &Candidate;
                    break;
                }
            }
            if (!Skin)
            {
                OutError = TEXT("The requested baked skin is absent");
                return false;
            }
        }

        OutLODs.SetNum(Bundle.Meshes.Num());
        OutMaterialIds.SetNum(Bundle.Meshes.Num());
        TArray<TPair<int32, int32>> PrimitiveTasks;
        for (int32 MeshArrayIndex = 0; MeshArrayIndex < Bundle.Meshes.Num(); ++MeshArrayIndex)
        {
            FGWorldBakedMesh& BakedMesh = Bundle.Meshes[MeshArrayIndex];
            FglTFRuntimeMeshLOD& LOD = OutLODs[MeshArrayIndex];
            LOD.bHasNormals = BakedMesh.bHasNormals;
            LOD.bHasTangents = BakedMesh.bHasTangents;
            LOD.bHasUV = BakedMesh.bHasUV;
            LOD.bHasVertexColors = BakedMesh.bHasVertexColors;
            LOD.AdditionalTransforms = MoveTemp(BakedMesh.AdditionalTransforms);

            if (Skin)
            {
                LOD.Skeleton.Reserve(Skin->Bones.Num());
                for (const FGWorldBakedBone& BakedBone : Skin->Bones)
                {
                    FglTFRuntimeBone& Bone = LOD.Skeleton.AddDefaulted_GetRef();
                    Bone.BoneName = BakedBone.Name;
                    Bone.ParentIndex = BakedBone.ParentIndex;
                    Bone.Transform = BakedBone.Transform;
                }
            }

            LOD.Primitives.SetNum(BakedMesh.Primitives.Num());
            OutMaterialIds[MeshArrayIndex].SetNum(BakedMesh.Primitives.Num());
            PrimitiveTasks.Reserve(PrimitiveTasks.Num() + BakedMesh.Primitives.Num());
            for (int32 PrimitiveIndex = 0; PrimitiveIndex < BakedMesh.Primitives.Num(); ++PrimitiveIndex)
            {
                PrimitiveTasks.Emplace(MeshArrayIndex, PrimitiveIndex);
            }
        }

        ParallelFor(PrimitiveTasks.Num(),
            [&Bundle, Skin, bLoadMaterials, &OutLODs, &OutMaterialIds, &PrimitiveTasks](const int32 TaskIndex)
            {
                const int32 MeshArrayIndex = PrimitiveTasks[TaskIndex].Key;
                const int32 PrimitiveIndex = PrimitiveTasks[TaskIndex].Value;
                FGWorldBakedPrimitive& Baked =
                    Bundle.Meshes[MeshArrayIndex].Primitives[PrimitiveIndex];
                FglTFRuntimePrimitive& Primitive =
                    OutLODs[MeshArrayIndex].Primitives[PrimitiveIndex];

                Primitive.Positions.Reserve(Baked.Positions.Num());
                for (const FVector3f& Value : Baked.Positions) Primitive.Positions.Add(FVector(Value));
                Baked.Positions.Empty();
                Primitive.Normals.Reserve(Baked.Normals.Num());
                for (const FVector3f& Value : Baked.Normals) Primitive.Normals.Add(FVector(Value));
                Baked.Normals.Empty();
                Primitive.Tangents.Reserve(Baked.Tangents.Num());
                for (const FVector4f& Value : Baked.Tangents) Primitive.Tangents.Add(FVector4(Value));
                Baked.Tangents.Empty();

                Primitive.UVs.SetNum(Baked.UVs.Num());
                for (int32 Channel = 0; Channel < Baked.UVs.Num(); ++Channel)
                {
                    Primitive.UVs[Channel].Reserve(Baked.UVs[Channel].Num());
                    for (const FVector2f& Value : Baked.UVs[Channel])
                    {
                        Primitive.UVs[Channel].Add(FVector2D(Value));
                    }
                }

                Primitive.Indices = MoveTemp(Baked.Indices);
                Primitive.Joints.SetNum(Baked.Joints.Num());
                for (int32 SetIndex = 0; SetIndex < Baked.Joints.Num(); ++SetIndex)
                {
                    Primitive.Joints[SetIndex].Reserve(Baked.Joints[SetIndex].Num());
                    for (const FGWorldBakedJoint4& Value : Baked.Joints[SetIndex])
                    {
                        FglTFRuntimeUInt16Vector4& Joint =
                            Primitive.Joints[SetIndex].AddDefaulted_GetRef();
                        Joint.X = Value.X;
                        Joint.Y = Value.Y;
                        Joint.Z = Value.Z;
                        Joint.W = Value.W;
                    }
                }

                Primitive.Weights.SetNum(Baked.Weights.Num());
                for (int32 SetIndex = 0; SetIndex < Baked.Weights.Num(); ++SetIndex)
                {
                    Primitive.Weights[SetIndex].Reserve(Baked.Weights[SetIndex].Num());
                    for (const FVector4f& Value : Baked.Weights[SetIndex])
                    {
                        Primitive.Weights[SetIndex].Add(FVector4(Value));
                    }
                }

                Primitive.Colors.Reserve(Baked.Colors.Num());
                for (const FVector4f& Value : Baked.Colors) Primitive.Colors.Add(FVector4(Value));
                Baked.Colors.Empty();

                Primitive.MorphTargets.Reserve(Baked.MorphTargets.Num());
                for (const FGWorldBakedMorphTarget& BakedMorph : Baked.MorphTargets)
                {
                    FglTFRuntimeMorphTarget& Morph = Primitive.MorphTargets.AddDefaulted_GetRef();
                    Morph.Name = BakedMorph.Name;
                    Morph.Positions.Reserve(BakedMorph.Positions.Num());
                    for (const FVector3f& Value : BakedMorph.Positions) Morph.Positions.Add(FVector(Value));
                    Morph.Normals.Reserve(BakedMorph.Normals.Num());
                    for (const FVector3f& Value : BakedMorph.Normals) Morph.Normals.Add(FVector(Value));
                }

                Primitive.OverrideBoneMap = !Baked.BoneMap.IsEmpty()
                    ? MoveTemp(Baked.BoneMap)
                    : (Skin ? Skin->JointBoneMap : TMap<int32, FName>());
                Primitive.WeightMaps = MoveTemp(Baked.WeightMaps);
                Primitive.MaterialName = MoveTemp(Baked.MaterialName);
                Primitive.Mode = Baked.Mode;
                Primitive.bHasMaterial = bLoadMaterials && Baked.bHasMaterial;
                Primitive.bHighPrecisionUVs = Baked.bHighPrecisionUVs;
                Primitive.bHighPrecisionWeights = Baked.bHighPrecisionWeights;
                Primitive.bDisableShadows = false;
                Primitive.bHasIndices = Baked.bHasIndices;
                OutMaterialIds[MeshArrayIndex][PrimitiveIndex] = Baked.MaterialId;
                Baked = FGWorldBakedPrimitive(); // Parallel tasks own distinct elements.
            });

        if (OutLODs.IsEmpty())
        {
            OutError = TEXT("The requested baked mesh set is empty");
            return false;
        }
        return true;
    }
}

bool UWorldBakedModelAsset::Initialize(const FResolvedRuntimeModel& Model, FString& OutError)
{
    check(IsInGameThread());
    if (!Model.IsValid())
    {
        OutError = TEXT("Cannot initialize a baked asset from an invalid model record");
        return false;
    }

    FGWorldModelMetadata Metadata;
    FGWorldModelManifest LoadedManifest;
    if (!Model.ArchiveReader->ReadModelMetadata(Model.UUID, Metadata, OutError)
        || !Model.ArchiveReader->ReadModelManifest(Model.UUID, LoadedManifest, OutError))
    {
        return false;
    }

    return InitializePrepared(
        Model.UUID,
        Model.Reference,
        Model.ArchiveReader,
        MoveTemp(Metadata),
        MoveTemp(LoadedManifest),
        OutError);
}

bool UWorldBakedModelAsset::InitializePrepared(
    const FGuid& InUUID,
    const FString& InReference,
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& InReader,
    FGWorldModelMetadata&& Metadata,
    FGWorldModelManifest&& InManifest,
    FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();

    // A facade is immutable after initialization. Reusing it while queued requests capture its
    // old tables/cache keys would violate request pinning even when no native build has started.
    if (Reader.IsValid())
    {
        OutError = TEXT("A baked facade cannot be reinitialized; create a new facade");
        return false;
    }
    UUID.Invalidate();
    Reference.Reset();
    Reader.Reset();
    Manifest.Reset();
    Nodes.Reset();
    WeakTextureCache.Reset();
    WeakMaterialCache.Reset();
    MeshMaterialDependencies.Reset();
    MaterialTextureDependencies.Reset();
    AsyncBuildReferences.Reset();
    ActiveFinalizeRequests.Reset();
    SharedRenderMeshCache.Reset();
    PendingSharedRenderMeshBuilds.Reset();

    FGuid ParsedUUID;
    if (!InUUID.IsValid()
        || !InReader.IsValid()
        || !FGWorldArchive::ParseModelReference(InReference, ParsedUUID)
        || ParsedUUID != InUUID)
    {
        OutError = TEXT("Cannot initialize a baked asset from invalid prepared archive tables");
        return false;
    }

    FGWorldModelSummary Summary;
    if (!InReader->GetSummary(InUUID, Summary)
        || Metadata.SourceNodeCount != Summary.SourceNodeCount
        || Metadata.SourceMeshCount != Summary.SourceMeshCount
        || Metadata.SourceMaterialCount != Summary.SourceMaterialCount
        || Metadata.SourceTextureCount != Summary.SourceTextureCount
        || Metadata.NodeTransforms.Num() != Summary.SourceNodeCount
        || Metadata.SceneData.ModelData.Center != Summary.Center
        || Metadata.SceneData.ModelData.Size != Summary.Size
        || InManifest.MeshRanges.Num() != Summary.SourceMeshCount
        || InManifest.MaterialRanges.Num() != Summary.SourceMaterialCount
        || InManifest.TextureRanges.Num() != Summary.SourceTextureCount
        || InManifest.MeshNames.Num() != Summary.SourceMeshCount)
    {
        // ReadModelMetadata/ReadModelManifest already performed the expensive structural/range
        // validation on the worker that produced these tables. Repeat only constant-time identity
        // checks here so publication cannot turn that validation into a new game-thread hitch.
        OutError = TEXT("Prepared baked tables do not match their immutable archive summary");
        return false;
    }

    UUID = InUUID;
    Reference = InReference;
    Reader = InReader;
    Manifest = MakeShared<FGWorldModelManifest, ESPMode::ThreadSafe>(
        MoveTemp(InManifest));

    // Build the glTFRuntime-compatible node view from baked metadata only. No parser or GLB bytes
    // participate in this path.
    Nodes.Reserve(Metadata.NodeTransforms.Num());
    TMap<int32, int32> NodeToArray;
    NodeToArray.Reserve(Metadata.NodeTransforms.Num());
    for (const FGWorldNodeTransform& Source : Metadata.NodeTransforms)
    {
        FglTFRuntimeNode& Target = Nodes.AddDefaulted_GetRef();
        Target.Index = Source.NodeIndex;
        Target.ParentIndex = Source.ParentIndex;
        Target.MeshIndex = Source.MeshIndex;
        Target.SkinIndex = Source.SkinIndex;
        Target.Name = Source.Name;
        Target.Transform = Source.LocalTransform;
        NodeToArray.Add(Target.Index, Nodes.Num() - 1);
    }
    for (FglTFRuntimeNode& Node : Nodes)
    {
        if (const int32* ParentArrayIndex = NodeToArray.Find(Node.ParentIndex))
        {
            Nodes[*ParentArrayIndex].ChildrenIndices.Add(Node.Index);
        }
    }

    OutError.Reset();
    return true;
}

FString UWorldBakedModelAsset::GetMeshName(const int32 MeshIndex) const
{
    if (!Manifest.IsValid())
    {
        return FString();
    }
    const FString* Name = Manifest->MeshNames.Find(MeshIndex);
    return Name ? *Name : FString();
}

void UWorldBakedModelAsset::CollectCachedDependencies(
    const TArray<int32>& MeshIndices, const FglTFRuntimeMaterialsConfig& Config,
    TSet<int32>& OutTextureIds, TSet<int32>& OutMaterialIds,
    FWorldBakedBuildReferenceGuard& Guard, const int32 TextureLimit)
{
    check(IsInGameThread());
    OutTextureIds.Reset();
    OutMaterialIds.Reset();
    const uint32 Signature = WorldBakedModelAssetPrivate::BuildMaterialConfigSignature(Config);
    TSet<int32> RequiredMaterials;
    for (const int32 MeshIndex : MeshIndices)
    {
        if (const auto* Ids = MeshMaterialDependencies.Find(MeshIndex))
            for (const int32 Id : *Ids) RequiredMaterials.Add(Id);
    }
    // Unknown meshes read their dependencies normally. Pinning every live cache entry allowed
    // overlapping loads to keep handing off unrelated textures indefinitely while travelling.
    for (const int32 MaterialId : RequiredMaterials)
    {
        const uint64 Key = (uint64(uint32(MaterialId)) << 32) | Signature;
        const auto* Cached = WeakMaterialCache.Find(Key);
        if (Cached && Cached->IsValid())
        {
            Guard.Add(Cached->Get());
            OutMaterialIds.Add(MaterialId); // The MID already retains its own textures.
            continue;
        }
        if (const auto* Ids = MaterialTextureDependencies.Find(MaterialId))
            for (const int32 TextureId : *Ids)
                if (!OutTextureIds.Contains(TextureId) && FindCachedTexture(TextureId, Guard, TextureLimit))
                    OutTextureIds.Add(TextureId);
    }
}

void UWorldBakedModelAsset::RememberDependencies(const FGWorldBakedAssetBundle& Bundle)
{
    check(IsInGameThread());
    for (const auto& Pair : Bundle.MeshMaterialDependencies)
        if (!MeshMaterialDependencies.Contains(Pair.Key)) MeshMaterialDependencies.Add(Pair.Key, Pair.Value);
    for (const auto& Material : Bundle.Materials)
    {
        if (MaterialTextureDependencies.Contains(Material.MaterialId)) continue;
        TSet<int32> Ids;
        for (const auto& Parameter : Material.Textures)
            if (Parameter.TextureId != INDEX_NONE) Ids.Add(Parameter.TextureId);
        MaterialTextureDependencies.FindOrAdd(Material.MaterialId) = Ids.Array();
    }
    // Lookup prunes used keys; also discard dead, unused keys when the cache is large.
    if (WeakTextureCache.Num() > 2048)
        for (auto It = WeakTextureCache.CreateIterator(); It; ++It)
            if (!It.Value().IsValid()) It.RemoveCurrent();
    if (WeakMaterialCache.Num() > 2048)
        for (auto It = WeakMaterialCache.CreateIterator(); It; ++It)
            if (!It.Value().IsValid()) It.RemoveCurrent();
}

void UWorldBakedModelAsset::PrepareDependenciesAsync(
    const TArray<int32>& MeshIndices, TFunction<void(bool)> Completion)
{
    check(IsInGameThread());
    bool bKnown = true;
    for (const int32 Index : MeshIndices)
    {
        const auto* Materials = MeshMaterialDependencies.Find(Index);
        if (!Materials) { bKnown = false; break; }
        for (const int32 Id : *Materials)
            if (!MaterialTextureDependencies.Contains(Id)) { bKnown = false; break; }
        if (!bKnown) break;
    }
    if (bKnown) { Completion(true); return; }

    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    FV3DStreamingBudget::Enqueue(this,
        [WeakThis, MeshIndices]() -> int64
        {
            const auto* Self = WeakThis.Get();
            if (!Self || !Self->Manifest.IsValid()) return 1024 * 1024;
            // Checksum/decompression buffers plus material records; the codec skips vertex arrays.
            const int64 Ceiling = MAX_int64 / 2;
            int64 Bytes = 1024 * 1024;
            for (const int32 Index : MeshIndices)
                if (const auto* Range = Self->Manifest->MeshRanges.Find(Index))
                    Bytes = FMath::Min(Ceiling, Bytes + int64(FMath::Min<uint64>(Range->UncompressedSize, uint64(Ceiling / 3))) * 3);
            for (const auto& Pair : Self->Manifest->MaterialRanges)
                Bytes = FMath::Min(Ceiling, Bytes + int64(FMath::Min<uint64>(Pair.Value.UncompressedSize, uint64(Ceiling / 3))) * 3);
            return Bytes;
        },
        [WeakThis, MeshIndices, Completion](FV3DStreamingBudget::FPermit Permit)
        {
            auto* Self = WeakThis.Get();
            if (!Self || !Self->Reader.IsValid() || !Self->Manifest.IsValid()) { Completion(false); return; }
            const auto LocalReader = Self->Reader;
            const auto LocalManifest = Self->Manifest;
            const FGuid LocalUUID = Self->UUID;
            const bool bQueued = FSafeFileIO::RunTrackedWorker(
                [WeakThis, MeshIndices, Completion, Permit, LocalReader, LocalManifest, LocalUUID]()
                {
                    FGWorldBakedAssetBundle Dependencies;
                    FString Error;
                    const bool bRead = LocalReader->ReadMeshBundle(LocalUUID, *LocalManifest,
                        MeshIndices, INDEX_NONE, true, Dependencies, Error, nullptr, nullptr, 0, true);
                    // No geometry/pixels may wait in the second admission queue. Only IDs and
                    // small material descriptions cross to GT. The lease ends after this callback.
                    Dependencies.Meshes.Empty();
                    Dependencies.Skins.Empty();
                    FSafeFileIO::DispatchTrackedGameThread(
                        [WeakThis, Completion, Permit, bRead, Dependencies = MoveTemp(Dependencies)]() mutable
                        {
                            auto* Owner = WeakThis.Get();
                            const bool bReady = Owner && bRead && !FSafeFileIO::IsShuttingDown();
                            if (bReady) Owner->RememberDependencies(Dependencies);
                            Dependencies.Reset();
                            Completion(bReady);
                        }, true);
                }, true);
            if (!bQueued) Completion(false);
        },
        [Completion]() { Completion(false); });
}

uint64 UWorldBakedModelAsset::RetainReadReferences(const FWorldBakedBuildReferenceGuard& Guard)
{
    check(IsInGameThread());
    auto& Entry = PendingReadReferences.AddDefaulted_GetRef();
    Entry.Id = NextReadId++;
    Guard.AppendTo(Entry.References);
    return Entry.Id;
}

uint64 UWorldBakedModelAsset::RetainRequestConfig(const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    auto& Entry = PendingReadReferences.AddDefaulted_GetRef();
    Entry.Id = NextReadId++;
    Entry.StaticConfig = Config;
    return Entry.Id;
}

uint64 UWorldBakedModelAsset::RetainRequestConfig(const FglTFRuntimeSkeletalMeshConfig& Config)
{
    check(IsInGameThread());
    auto& Entry = PendingReadReferences.AddDefaulted_GetRef();
    Entry.Id = NextReadId++;
    Entry.SkeletalConfig = Config;
    return Entry.Id;
}

void UWorldBakedModelAsset::ReleaseReadReferences(const uint64 Id)
{
    check(IsInGameThread());
    for (int32 Index = 0; Index < PendingReadReferences.Num(); ++Index)
        if (PendingReadReferences[Index].Id == Id)
        {
            PendingReadReferences.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            return;
        }
}

int64 UWorldBakedModelAsset::EstimateLoadBytes(
    const TArray<int32>& MeshIndices, const bool bMaterials, const int32 SkinIndex,
    const int32 TextureLimit, const uint32 MaterialSignature) const
{
    check(IsInGameThread());
    if (!Manifest.IsValid()) return 1024 * 1024;
    // A conservative reservation, not a process-RSS cap. Covers decompression, float->double
    // conversion and plugin build copies. Unknown dependencies retain a conservative fallback.
    constexpr uint64 Ceiling = uint64(MAX_int64 / 2);
    uint64 Total = 1024 * 1024;
    const auto Add = [&Total, Ceiling](const FGWorldArchiveRange& Range, const uint64 Copies)
    {
        const uint64 Charge = FMath::Min<uint64>(Range.UncompressedSize, Ceiling / Copies) * Copies;
        Total = FMath::Min<uint64>(Ceiling, Total + Charge);
    };
    for (const int32 Index : MeshIndices)
        if (const auto* Range = Manifest->MeshRanges.Find(Index)) Add(*Range, 8);
    if (const auto* Range = Manifest->SkinRanges.Find(SkinIndex)) Add(*Range, 4);
    if (bMaterials)
    {
        bool bKnown = true;
        TSet<int32> RequiredMaterials;
        TSet<int32> RequiredTextures;
        for (const int32 MeshIndex : MeshIndices)
        {
            if (const auto* Ids = MeshMaterialDependencies.Find(MeshIndex))
            {
                for (const int32 Id : *Ids) RequiredMaterials.Add(Id);
            }
            else bKnown = false;
        }
        for (const int32 MaterialId : RequiredMaterials)
        {
            const uint64 Key = (uint64(uint32(MaterialId)) << 32) | MaterialSignature;
            const auto* Cached = WeakMaterialCache.Find(Key);
            if (Cached && Cached->IsValid()) continue;
            if (const auto* Ids = MaterialTextureDependencies.Find(MaterialId))
            {
                for (const int32 Id : *Ids) RequiredTextures.Add(Id);
            }
            else bKnown = false;
            if (const auto* Range = Manifest->MaterialRanges.Find(MaterialId)) Add(*Range, 3);
        }
        const uint32 Limit = uint32(TextureLimit);
        if (bKnown)
        {
            for (const int32 TextureId : RequiredTextures)
            {
                const uint64 Key = (uint64(uint32(TextureId)) << 32) | Limit;
                const auto* Cached = WeakTextureCache.Find(Key);
                if (Cached && Cached->IsValid()) continue;
                if (const auto* Range = Manifest->TextureRanges.Find(TextureId)) Add(*Range, 3);
            }
        }
        else
        {
            for (const auto& Pair : Manifest->TextureRanges) Add(Pair.Value, 3);
            for (const auto& Pair : Manifest->MaterialRanges) Add(Pair.Value, 3);
        }
    }
    return int64(Total);
}

UMaterialInterface* UWorldBakedModelAsset::FindCachedMaterial(
    const int32 MaterialId,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    const uint64 CacheKey =
        WorldBakedModelAssetPrivate::BuildMaterialCacheKey(MaterialId, MaterialsConfig);
    if (const TWeakObjectPtr<UMaterialInterface>* Cached = WeakMaterialCache.Find(CacheKey))
    {
        if (UMaterialInterface* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakMaterialCache.Remove(CacheKey);
    }
    return nullptr;
}

UTexture2D* UWorldBakedModelAsset::FindCachedTexture(
    const int32 TextureId,
    FWorldBakedBuildReferenceGuard& ReferenceGuard, const int32 TextureLimit)
{
    check(IsInGameThread());
    const uint64 TextureCacheKey =
        (static_cast<uint64>(static_cast<uint32>(TextureId)) << 32) | TextureLimit;
    if (const TWeakObjectPtr<UTexture2D>* Cached = WeakTextureCache.Find(TextureCacheKey))
    {
        if (UTexture2D* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakTextureCache.Remove(TextureCacheKey);
    }
    return nullptr;
}

UTexture2D* UWorldBakedModelAsset::CreateTexture(
    FGWorldBakedTexture& Baked,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    FString& OutError, const int32 TextureLimit)
{
    check(IsInGameThread());
    if (!WorldBakedModelAssetPrivate::ValidateTextureLayout(Baked, OutError))
    {
        return nullptr;
    }
    const uint64 TextureCacheKey =
        (static_cast<uint64>(static_cast<uint32>(Baked.TextureId)) << 32)
        | static_cast<uint32>(TextureLimit);
    if (const TWeakObjectPtr<UTexture2D>* Cached = WeakTextureCache.Find(TextureCacheKey))
    {
        if (UTexture2D* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakTextureCache.Remove(TextureCacheKey);
    }

    // Existing .v3d archives can contain larger source mips. Select the first stored mip that
    // satisfies the current user cap instead of rebuilding/resampling pixels at load time. This
    // makes the setting effective immediately and avoids allocating/uploading discarded top mips.
    int32 FirstRuntimeMip = 0;
    while (FirstRuntimeMip + 1 < Baked.Mips.Num()
        && (Baked.Mips[FirstRuntimeMip].SizeX > TextureLimit
            || Baked.Mips[FirstRuntimeMip].SizeY > TextureLimit))
    {
        ++FirstRuntimeMip;
    }
    const FGWorldBakedTextureMip& RuntimeTopMip = Baked.Mips[FirstRuntimeMip];

    const FGWorldArchiveRange* TextureRange = Manifest.IsValid()
        ? Manifest->TextureRanges.Find(Baked.TextureId) : nullptr;
    if (!Reader.IsValid() || !TextureRange)
    {
        OutError = TEXT("Runtime texture has no immutable backing range");
        return nullptr;
    }
    UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!IsValid(Texture))
    {
        OutError = TEXT("Could not allocate a runtime texture");
        return nullptr;
    }
    // Register immediately: UpdateResource and later material creation may enter engine code that
    // can collect an otherwise unreflected runtime texture.
    ReferenceGuard.Add(Texture);

    UWorldBakedTextureMipProvider* Provider =
        NewObject<UWorldBakedTextureMipProvider>(Texture, NAME_None, RF_Transient);
    if (!IsValid(Provider))
    {
        OutError = TEXT("Could not allocate the archive mip provider");
        return nullptr;
    }
    ReferenceGuard.Add(Provider);

    // PlatformData describes the normal UTexture2D resource but holds no duplicate CPU pixels.
    // The engine requests initial/recreated pixels through its all-mip provider interface.
    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = RuntimeTopMip.SizeX;
    PlatformData->SizeY = RuntimeTopMip.SizeY;
    PlatformData->PixelFormat = static_cast<EPixelFormat>(Baked.PixelFormat);
    for (int32 MipIndex = FirstRuntimeMip; MipIndex < Baked.Mips.Num(); ++MipIndex)
    {
        const FGWorldBakedTextureMip& Source = Baked.Mips[MipIndex];
        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        Mip->SizeX = Source.SizeX;
        Mip->SizeY = Source.SizeY;
        Mip->SizeZ = 0; // Native 2D layout; archive validation uses normalized depth=1.
        PlatformData->Mips.Add(Mip);
    }
    Texture->SetPlatformData(PlatformData);
    if (!Provider->InitializeArchiveSource(Reader, *TextureRange, Baked, FirstRuntimeMip, TextureLimit))
    {
        OutError = TEXT("Could not initialize the archive mip provider");
        return nullptr;
    }
    Texture->AddAssetUserData(Provider); // Reflected ownership lasts exactly as long as the texture.
    Texture->SRGB = Baked.bSRGB;
    Texture->AddressX = static_cast<TextureAddress>(Baked.AddressX);
    Texture->AddressY = static_cast<TextureAddress>(Baked.AddressY);
    Texture->Filter = static_cast<TextureFilter>(Baked.Filter);
    Texture->LODGroup = static_cast<TextureGroup>(Baked.LODGroup);

    // Streaming happens at the archive-member/mesh level. Each material dependency is loaded only
    // when requested, so Unreal's separate texture streamer must not seek nonexistent cooked bulk.
    Texture->NeverStream = true;
    Texture->UpdateResource();
    WeakTextureCache.Add(TextureCacheKey, Texture);

    return Texture;
}

UMaterialInterface* UWorldBakedModelAsset::CreateMaterial(
    const FGWorldBakedMaterial& Baked,
    const TMap<int32, UTexture2D*>& Textures,
    const FglTFRuntimeMaterialsConfig& Config,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    FString& OutError)
{
    check(IsInGameThread());
    if (UMaterialInterface* Cached =
            FindCachedMaterial(Baked.MaterialId, Config, ReferenceGuard))
    {
        return Cached;
    }

    UMaterialInterface* Base =
        WorldBakedModelAssetPrivate::SelectBaseMaterial(Baked, Config);
    if (!IsValid(Base))
    {
        OutError = FString::Printf(
            TEXT("Baked material shader is unavailable: %s"), *Baked.BaseMaterialPath);
        return nullptr;
    }
    ReferenceGuard.Add(Base);

    UMaterialInstanceDynamic* Material =
        UMaterialInstanceDynamic::Create(Base, this);
    if (!IsValid(Material))
    {
        OutError = FString::Printf(
            TEXT("Could not create material instance: %s"), *Baked.Name);
        return nullptr;
    }
    ReferenceGuard.Add(Material);

    for (const FGWorldBakedScalarParameter& Parameter : Baked.Scalars)
    {
        Material->SetScalarParameterValue(FName(*Parameter.Name), Parameter.Value);
    }
    for (const TPair<FString, float>& Override : Config.ScalarParamsOverrides)
    {
        Material->SetScalarParameterValue(FName(*Override.Key), Override.Value);
    }
    for (const FGWorldBakedVectorParameter& Parameter : Baked.Vectors)
    {
        Material->SetVectorParameterValue(FName(*Parameter.Name), Parameter.Value);
    }
    for (const FGWorldBakedTextureParameter& Parameter : Baked.Textures)
    {
        UTexture* Texture = nullptr;
        if (Parameter.TextureId != INDEX_NONE)
        {
            UTexture2D* const* Found = Textures.Find(Parameter.TextureId);
            Texture = Found ? *Found : nullptr;
        }
        else if (!Parameter.AssetPath.IsEmpty())
        {
            Texture = LoadObject<UTexture>(nullptr, *Parameter.AssetPath);
        }

        if (!IsValid(Texture))
        {
            OutError = FString::Printf(
                TEXT("Material %s has an unavailable texture parameter %s"),
                *Baked.Name, *Parameter.Name);
            return nullptr;
        }
        // Asset-path parameters do not pass through the baked-texture map, so retain them here
        // before material mutation can enter engine code and expose a collection point.
        ReferenceGuard.Add(Texture);
        Material->SetTextureParameterValue(FName(*Parameter.Name), Texture);
    }

    const uint64 MaterialCacheKey =
        WorldBakedModelAssetPrivate::BuildMaterialCacheKey(Baked.MaterialId, Config);
    WeakMaterialCache.Add(MaterialCacheKey, Material);
    return Material;
}

bool UWorldBakedModelAsset::AttachRuntimeMaterials(
    FGWorldBakedAssetBundle& Bundle,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    const TArray<TArray<int32>>& MaterialIds,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    TArray<FglTFRuntimeMeshLOD>& InOutLODs,
    FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    ReferenceGuard.Add(this);
    RememberDependencies(Bundle);

    if (MaterialIds.Num() != InOutLODs.Num())
    {
        OutError = TEXT("Prepared RuntimeLOD material layout is inconsistent");
        return false;
    }
    if (MaterialsConfig.bSkipLoad)
    {
        return true;
    }

    TMap<int32, UTexture2D*> Textures;
    TMap<int32, UMaterialInterface*> Materials;
    ReferenceGuard.Reserve(Bundle.Textures.Num() + Bundle.Materials.Num() * 2 + 16);

    // Material.dat can be omitted entirely by the worker when a matching immutable MID is in the
    // request-pinned weak cache. Recover those dependencies before creating any missing entries.
    for (const TArray<int32>& LODMaterialIds : MaterialIds)
    {
        for (const int32 MaterialId : LODMaterialIds)
        {
            if (MaterialId == INDEX_NONE || Materials.Contains(MaterialId)) continue;
            if (UMaterialInterface* Cached =
                    FindCachedMaterial(MaterialId, MaterialsConfig, ReferenceGuard))
            {
                Materials.Add(MaterialId, Cached);
            }
        }
    }
    Textures.Reserve(Bundle.Textures.Num());
    for (FGWorldBakedTexture& Baked : Bundle.Textures)
    {
        UTexture2D* Texture = CreateTexture(Baked, ReferenceGuard, OutError, Bundle.MaxTextureResolution);
        Baked.Mips.Empty(); // Also release decoded bytes when another request populated the cache.
        if (!Texture) return false;
        Textures.Add(Baked.TextureId, Texture);
    }

    for (const FGWorldBakedMaterial& BakedMaterial : Bundle.Materials)
    {
        for (const FGWorldBakedTextureParameter& Parameter : BakedMaterial.Textures)
        {
            if (Parameter.TextureId == INDEX_NONE || Textures.Contains(Parameter.TextureId)) continue;
            UTexture2D* CachedTexture = FindCachedTexture(Parameter.TextureId, ReferenceGuard, Bundle.MaxTextureResolution);
            if (!IsValid(CachedTexture))
            {
                OutError = FString::Printf(
                    TEXT("Texture %d was omitted from archive I/O but is no longer resident"),
                    Parameter.TextureId);
                return false;
            }
            Textures.Add(Parameter.TextureId, CachedTexture);
        }
    }

    Materials.Reserve(Bundle.Materials.Num());
    for (const FGWorldBakedMaterial& Baked : Bundle.Materials)
    {
        UMaterialInterface* Material =
            CreateMaterial(Baked, Textures, MaterialsConfig, ReferenceGuard, OutError);
        if (!Material) return false;
        Materials.Add(Baked.MaterialId, Material);
    }

    for (int32 MeshArrayIndex = 0; MeshArrayIndex < InOutLODs.Num(); ++MeshArrayIndex)
    {
        FglTFRuntimeMeshLOD& LOD = InOutLODs[MeshArrayIndex];
        if (!MaterialIds.IsValidIndex(MeshArrayIndex)
            || MaterialIds[MeshArrayIndex].Num() != LOD.Primitives.Num())
        {
            OutError = TEXT("Prepared RuntimeLOD primitive/material layout is inconsistent");
            return false;
        }
        for (int32 PrimitiveIndex = 0; PrimitiveIndex < LOD.Primitives.Num(); ++PrimitiveIndex)
        {
            const int32 MaterialId = MaterialIds[MeshArrayIndex][PrimitiveIndex];
            if (MaterialId == INDEX_NONE) continue;
            UMaterialInterface* const* Found = Materials.Find(MaterialId);
            if (!Found || !IsValid(*Found))
            {
                OutError = TEXT("A mesh references a missing baked material");
                return false;
            }
            LOD.Primitives[PrimitiveIndex].Material = *Found;
        }
    }
    return true;
}

bool UWorldBakedModelAsset::BuildRuntimeLODs(
    FGWorldBakedAssetBundle& Bundle,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    const int32 SkinIndex,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    TArray<FglTFRuntimeMeshLOD>& OutLODs,
    FString& OutError)
{
    TArray<TArray<int32>> MaterialIds;
    if (!WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
            Bundle, SkinIndex, !MaterialsConfig.bSkipLoad, OutLODs, MaterialIds, OutError))
    {
        return false;
    }
    return AttachRuntimeMaterials(
        Bundle, MaterialsConfig, MaterialIds, ReferenceGuard, OutLODs, OutError);
}

UglTFRuntimeAsset* UWorldBakedModelAsset::CreateMeshBuilder(FString& OutError)
{
    check(IsInGameThread());
    UglTFRuntimeAsset* Builder =
        NewObject<UglTFRuntimeAsset>(this, NAME_None, RF_Transient);
    if (!IsValid(Builder))
    {
        OutError = TEXT("Could not allocate the decoded-data mesh finalizer");
        return nullptr;
    }

    // Parser attachment is deferred into the request's native operation. Each request owns a distinct
    // builder/parser, so baked RuntimeLOD finalizers can use the bounded-parallel gate safely.
    return Builder;
}

void UWorldBakedModelAsset::RetainAsyncBuildReferences(
    UglTFRuntimeAsset* Builder,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    if (!IsValid(Builder)) return;
    FGWorldBakedAsyncBuildReferences& Entry = AsyncBuildReferences.AddDefaulted_GetRef();
    Entry.Builder = Builder;
    ReferenceGuard.AppendTo(Entry.References);
}

void UWorldBakedModelAsset::ReleaseAsyncBuildReferences(UglTFRuntimeAsset* Builder)
{
    check(IsInGameThread());
    for (int32 Index = AsyncBuildReferences.Num() - 1; Index >= 0; --Index)
    {
        if (AsyncBuildReferences[Index].Builder.Get() == Builder)
        {
            AsyncBuildReferences.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            return;
        }
    }
}

void UWorldBakedMeshFinalizeRequest::InitializeStatic(
    UWorldBakedModelAsset* InOwner,
    UglTFRuntimeAsset* InBuilder,
    FString InSourceReference,
    FGWorldBakedStaticMeshNativeCallback InCallback)
{
    check(IsInGameThread());
    Owner = InOwner;
    Builder = InBuilder;
    SourceReference = MoveTemp(InSourceReference);
    StaticCallback = MoveTemp(InCallback);
    SkeletalCallback = FGWorldBakedSkeletalMeshNativeCallback();
}

void UWorldBakedMeshFinalizeRequest::InitializeSkeletal(
    UWorldBakedModelAsset* InOwner,
    UglTFRuntimeAsset* InBuilder,
    FString InSourceReference,
    FGWorldBakedSkeletalMeshNativeCallback InCallback)
{
    check(IsInGameThread());
    Owner = InOwner;
    Builder = InBuilder;
    SourceReference = MoveTemp(InSourceReference);
    SkeletalCallback = MoveTemp(InCallback);
    StaticCallback = FGWorldBakedStaticMeshNativeCallback();
}

void UWorldBakedMeshFinalizeRequest::Cleanup()
{
    check(IsInGameThread());
    FV3DRuntimeSafety::RequestAssetRelease(Builder.Get());
    if (UWorldBakedModelAsset* StrongOwner = Owner.Get())
    {
        StrongOwner->ReleaseAsyncBuildReferences(Builder.Get());
        StrongOwner->RemoveFinalizeRequest(this);
    }
    Builder = nullptr;
    Owner = nullptr;
}

void UWorldBakedMeshFinalizeRequest::FailStarted(const FString& Reason)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    FGWorldBakedStaticMeshNativeCallback LocalStatic = MoveTemp(StaticCallback);
    FGWorldBakedSkeletalMeshNativeCallback LocalSkeletal = MoveTemp(SkeletalCallback);
    Ticket = 0;
    FV3DRuntimeSafety::ReportRecoverableFailure(SourceReference, Reason);
    Cleanup();
    if (LocalStatic) LocalStatic(nullptr);
    if (LocalSkeletal) LocalSkeletal(nullptr);
    FV3DRuntimeSafety::CompleteOperation(LocalTicket);
}

void UWorldBakedMeshFinalizeRequest::RejectBeforeStart(const FString& Reason)
{
    check(IsInGameThread());
    FGWorldBakedStaticMeshNativeCallback LocalStatic = MoveTemp(StaticCallback);
    FGWorldBakedSkeletalMeshNativeCallback LocalSkeletal = MoveTemp(SkeletalCallback);
    // Queue cancellation/shutdown is not evidence that the immutable model is corrupt.
    UE_LOG(LogTemp, Verbose, TEXT("Baked finalizer rejected before start: %s"), *Reason);
    Cleanup();
    if (LocalStatic) LocalStatic(nullptr);
    if (LocalSkeletal) LocalSkeletal(nullptr);
}

void UWorldBakedMeshFinalizeRequest::BeginStatic(
    const uint64 InTicket,
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    Ticket = InTicket;
    if (!IsValid(Owner.Get()) || !IsValid(Builder.Get()))
    {
        FailStarted(TEXT("The baked model facade or mesh builder expired before static finalization"));
        return;
    }
    if (!WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder.Get()))
    {
        FailStarted(TEXT("Could not initialize the decoded-data static mesh finalizer"));
        return;
    }
    // Keep UStaticMesh's RT support default. glTFRuntime must initialize the RT
    // representation before InitResources; changing the flag after completion is too late.
    FglTFRuntimeStaticMeshAsync NativeCallback;
    NativeCallback.BindDynamic(this, &UWorldBakedMeshFinalizeRequest::HandleStaticMeshFinalized);
    Builder->LoadStaticMeshFromRuntimeLODsAsync(LODs, NativeCallback, Config);
}

void UWorldBakedMeshFinalizeRequest::BeginSkeletal(
    const uint64 InTicket,
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    check(IsInGameThread());
    Ticket = InTicket;
    if (!IsValid(Owner.Get()) || !IsValid(Builder.Get()))
    {
        FailStarted(TEXT("The baked model facade or mesh builder expired before skeletal finalization"));
        return;
    }
    if (!WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder.Get()))
    {
        FailStarted(TEXT("Could not initialize the decoded-data skeletal mesh finalizer"));
        return;
    }
    FglTFRuntimeSkeletalMeshAsync NativeCallback;
    NativeCallback.BindDynamic(this, &UWorldBakedMeshFinalizeRequest::HandleSkeletalMeshFinalized);
    Builder->LoadSkeletalMeshFromRuntimeLODsAsync(LODs, INDEX_NONE, NativeCallback, Config);
}

void UWorldBakedMeshFinalizeRequest::HandleStaticMeshFinalized(UStaticMesh* StaticMesh)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    const FString LocalReference = SourceReference;
    FGWorldBakedStaticMeshNativeCallback Callback = MoveTemp(StaticCallback);
    Ticket = 0;
    Cleanup();
    if (!IsValid(StaticMesh))
    {
        FV3DRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("Streamed decoded-data static mesh build failed"));
    }
    if (Callback) Callback(StaticMesh);
    FV3DRuntimeSafety::CompleteOperationAfterCallback(LocalTicket);
}

void UWorldBakedMeshFinalizeRequest::HandleSkeletalMeshFinalized(USkeletalMesh* SkeletalMesh)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    const FString LocalReference = SourceReference;
    FGWorldBakedSkeletalMeshNativeCallback Callback = MoveTemp(SkeletalCallback);
    Ticket = 0;
    Cleanup();
    if (!IsValid(SkeletalMesh))
    {
        FV3DRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("Streamed decoded-data skeletal mesh build failed"));
    }
    if (Callback) Callback(SkeletalMesh);
    FV3DRuntimeSafety::CompleteOperationAfterCallback(LocalTicket);
}

void UWorldBakedModelAsset::RemoveFinalizeRequest(UWorldBakedMeshFinalizeRequest* Request)
{
    check(IsInGameThread());
    ActiveFinalizeRequests.RemoveSingleSwap(Request, EAllowShrinking::No);
}

void UWorldBakedModelAsset::QueueStaticRuntimeLODFinalizer(
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeStaticMeshConfig& Config,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FString& SourceReference,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    FString Error;
    UglTFRuntimeAsset* Builder = CreateMeshBuilder(Error);
    if (!IsValid(Builder))
    {
        FV3DRuntimeSafety::ReportRecoverableFailure(SourceReference, Error);
        if (Callback) Callback(nullptr);
        return;
    }
    RetainAsyncBuildReferences(Builder, ReferenceGuard);

    UWorldBakedMeshFinalizeRequest* Request = NewObject<UWorldBakedMeshFinalizeRequest>(this);
    if (!IsValid(Request))
    {
        ReleaseAsyncBuildReferences(Builder);
        FV3DRuntimeSafety::ReportRecoverableFailure(SourceReference, TEXT("Could not allocate static finalizer request"));
        if (Callback) Callback(nullptr);
        return;
    }
    Request->InitializeStatic(this, Builder, SourceReference, MoveTemp(Callback));
    ActiveFinalizeRequests.Add(Request);

    TWeakObjectPtr<UWorldBakedMeshFinalizeRequest> WeakRequest(Request);
    FV3DRuntimeSafety::EnqueueOperation(
        this,
        Builder,
        TEXT("Finalize streamed baked static mesh asynchronously"),
        [WeakRequest, LODs = MoveTemp(LODs), Config,
            WeakOuter = TWeakObjectPtr<UObject>(Config.Outer), bHadOuter = Config.Outer != nullptr]
            (const uint64 Ticket) mutable
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                if (bHadOuter && !WeakOuter.IsValid())
                {
                    StrongRequest->RejectBeforeStart(TEXT("Mesh outer was destroyed while queued"));
                    FV3DRuntimeSafety::CompleteOperation(Ticket);
                    return;
                }
                StrongRequest->BeginStatic(Ticket, MoveTemp(LODs), Config);
                return;
            }
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakRequest](const FString& Reason)
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->RejectBeforeStart(Reason);
            }
        });
}

void UWorldBakedModelAsset::QueueSkeletalRuntimeLODFinalizer(
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeSkeletalMeshConfig& Config,
    FGWorldBakedSkeletalMeshNativeCallback Callback,
    const FString& SourceReference,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    FString Error;
    UglTFRuntimeAsset* Builder = CreateMeshBuilder(Error);
    if (!IsValid(Builder))
    {
        FV3DRuntimeSafety::ReportRecoverableFailure(SourceReference, Error);
        if (Callback) Callback(nullptr);
        return;
    }
    RetainAsyncBuildReferences(Builder, ReferenceGuard);

    UWorldBakedMeshFinalizeRequest* Request = NewObject<UWorldBakedMeshFinalizeRequest>(this);
    if (!IsValid(Request))
    {
        ReleaseAsyncBuildReferences(Builder);
        FV3DRuntimeSafety::ReportRecoverableFailure(SourceReference, TEXT("Could not allocate skeletal finalizer request"));
        if (Callback) Callback(nullptr);
        return;
    }
    Request->InitializeSkeletal(this, Builder, SourceReference, MoveTemp(Callback));
    ActiveFinalizeRequests.Add(Request);

    TWeakObjectPtr<UWorldBakedMeshFinalizeRequest> WeakRequest(Request);
    FV3DRuntimeSafety::EnqueueOperation(
        this,
        Builder,
        TEXT("Finalize streamed baked skeletal mesh asynchronously"),
        [WeakRequest, LODs = MoveTemp(LODs), Config,
            WeakOuter = TWeakObjectPtr<UObject>(Config.Outer), bHadOuter = Config.Outer != nullptr]
            (const uint64 Ticket) mutable
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                if (bHadOuter && !WeakOuter.IsValid())
                {
                    StrongRequest->RejectBeforeStart(TEXT("Mesh outer was destroyed while queued"));
                    FV3DRuntimeSafety::CompleteOperation(Ticket);
                    return;
                }
                StrongRequest->BeginSkeletal(Ticket, MoveTemp(LODs), Config);
                return;
            }
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakRequest](const FString& Reason)
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->RejectBeforeStart(Reason);
            }
        });
}

bool UWorldBakedModelAsset::TryBeginSharedRenderMeshRequest(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshConfig& Config,
    FGWorldBakedStaticMeshNativeCallback& InOutCallback,
    FString& OutCacheKey)
{
    check(IsInGameThread());
    OutCacheKey.Reset();

    // Explicit opt-in contract: WorldSceneStreamAction sets this facade as Outer only for visual
    // static geometry. Physics/nav/CPU-access meshes keep their world context and never enter this
    // cache, avoiding accidental cross-world sharing of runtime collision state.
    if (Config.Outer != this
        || Config.bBuildComplexCollision
        || Config.bBuildSimpleCollision
        || Config.bBuildNavCollision
        || Config.bAllowCPUAccess
        || MeshIndices.IsEmpty())
    {
        return false;
    }

    OutCacheKey = WorldBakedModelAssetPrivate::BuildSharedRenderMeshCacheKey(MeshIndices, Config);
    if (TWeakObjectPtr<UStaticMesh>* Cached = SharedRenderMeshCache.Find(OutCacheKey))
    {
        if (UStaticMesh* Existing = Cached->Get(); IsValid(Existing))
        {
            FGWorldBakedStaticMeshNativeCallback Callback = MoveTemp(InOutCallback);
            if (Callback) Callback(Existing);
            return true;
        }
        SharedRenderMeshCache.Remove(OutCacheKey);
    }

    if (TArray<FGWorldBakedStaticMeshNativeCallback>* Waiters =
            PendingSharedRenderMeshBuilds.Find(OutCacheKey))
    {
        Waiters->Add(MoveTemp(InOutCallback));
        return true;
    }

    TArray<FGWorldBakedStaticMeshNativeCallback>& Waiters =
        PendingSharedRenderMeshBuilds.Add(OutCacheKey);
    Waiters.Add(MoveTemp(InOutCallback));

    // The first caller is the build leader. Replace its callback with one fan-out callback that
    // publishes the result to every request that arrived while I/O/native finalization was active.
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    const FString CacheKeyCopy = OutCacheKey;
    InOutCallback = [WeakThis, CacheKeyCopy](UStaticMesh* Mesh)
    {
        if (UWorldBakedModelAsset* Self = WeakThis.Get())
        {
            Self->CompleteSharedRenderMeshRequest(CacheKeyCopy, Mesh);
        }
    };
    return false;
}

void UWorldBakedModelAsset::CompleteSharedRenderMeshRequest(
    const FString& CacheKey,
    UStaticMesh* Mesh)
{
    check(IsInGameThread());
    TArray<FGWorldBakedStaticMeshNativeCallback> Waiters;
    if (TArray<FGWorldBakedStaticMeshNativeCallback>* Pending =
            PendingSharedRenderMeshBuilds.Find(CacheKey))
    {
        Waiters = MoveTemp(*Pending);
        PendingSharedRenderMeshBuilds.Remove(CacheKey);
    }

    if (IsValid(Mesh))
    {
        SharedRenderMeshCache.Add(CacheKey, Mesh);
        // Weak entries cost little but can accumulate during long editor rebuild sessions.
        if (SharedRenderMeshCache.Num() > 2048)
        {
            for (auto It = SharedRenderMeshCache.CreateIterator(); It; ++It)
            {
                if (!It.Value().IsValid()) It.RemoveCurrent();
            }
        }
    }

    for (FGWorldBakedStaticMeshNativeCallback& Callback : Waiters)
    {
        if (Callback) Callback(Mesh);
    }
}

UStaticMesh* UWorldBakedModelAsset::LoadStaticMesh(
    const int32 MeshIndex,
    const FglTFRuntimeStaticMeshConfig& Config,
    FString* OutError)
{
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    return LoadStaticMeshLODs(MeshIndices, Config, OutError);
}

UStaticMesh* UWorldBakedModelAsset::LoadStaticMeshLODs(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshConfig& InConfig,
    FString* OutError)
{
    check(IsInGameThread());
    FglTFRuntimeStaticMeshConfig Config = InConfig;
    const int32 TextureLimit = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    FString Error;
    FGWorldBakedAssetBundle Bundle;
    FWorldBakedBuildReferenceGuard CacheGuard;
    CacheGuard.Add(this);
    CacheGuard.Add(Config.Outer);
    const uint64 ConfigId = RetainRequestConfig(Config);
    ON_SCOPE_EXIT { ReleaseReadReferences(ConfigId); };
    const int32 TextureResolution = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedDependencies(MeshIndices, Config.MaterialsConfig,
            CachedTextureIds, CachedMaterialIds, CacheGuard, TextureResolution);
    }
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, INDEX_NONE,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
            &CachedTextureIds, &CachedMaterialIds, TextureResolution))
    {
        if (Error.IsEmpty())
        {
            Error = TEXT("The baked model reader is not initialized");
        }
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }

    FWorldBakedBuildReferenceGuard ReferenceGuard;
    TArray<FglTFRuntimeMeshLOD> LODs;
    if (!BuildRuntimeLODs(
            Bundle, Config.MaterialsConfig, INDEX_NONE,
            ReferenceGuard, LODs, Error))
    {
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }
    Bundle.Reset(); // Release serialized float/mip arrays before native mesh finalization peaks.

    // NewObject alone is not a stack GC reference. Keep the transient finalizer strongly reachable
    // across any nested engine work performed by glTFRuntime, then release it with this call frame.
    TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(CreateMeshBuilder(Error));
    UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
    UStaticMesh* Result = nullptr;
    bool bBuilderInitialized = false;
    const bool bExecuted = Builder
        && FV3DRuntimeSafety::ExecuteSynchronousOperation(
            TEXT("Build baked static mesh"),
            [&Result, &bBuilderInitialized, Builder, &LODs, &Config, &ReferenceGuard]()
            {
                bBuilderInitialized =
                    WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder);
                if (bBuilderInitialized)
                {
                    Result = Builder->LoadStaticMeshFromRuntimeLODs(LODs, Config);
                    // ExecuteSynchronousOperation may pump another queued native job before it
                    // returns, so protect the newly created mesh during that re-entrant window.
                    ReferenceGuard.Add(Result);
                }
            });
    if (!bExecuted && Error.IsEmpty())
    {
        Error = TEXT("The glTFRuntime mesh finalizer is busy or shutting down");
    }
    if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
    {
        Error = TEXT("Could not initialize the decoded-data mesh finalizer");
    }
    if (!Result && Error.IsEmpty())
    {
        Error = TEXT("Decoded-data static mesh finalization failed");
    }
    if (OutError)
    {
        *OutError = Error;
    }
    return Result;
}

void UWorldBakedModelAsset::LoadStaticMeshAsync(
    const int32 MeshIndex,
    const FglTFRuntimeStaticMeshAsync& Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    FglTFRuntimeStaticMeshAsync DynamicCallback = Callback;
    LoadStaticMeshAsyncNative(
        MeshIndex,
        [DynamicCallback](UStaticMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAsync(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshAsync& Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    FglTFRuntimeStaticMeshAsync DynamicCallback = Callback;
    LoadStaticMeshLODsAsyncNative(
        MeshIndices,
        [DynamicCallback](UStaticMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadStaticMeshAsyncNative(
    const int32 MeshIndex,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    LoadStaticMeshLODsAsyncNative(MeshIndices, MoveTemp(Callback), Config);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAsyncNative(
    const TArray<int32>& MeshIndices,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FglTFRuntimeStaticMeshConfig& InConfig)
{
    check(IsInGameThread());
    FglTFRuntimeStaticMeshConfig Config = InConfig;
    const int32 TextureLimit = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    FString SharedRenderMeshCacheKey;
    if (TryBeginSharedRenderMeshRequest(
            MeshIndices, Config, Callback, SharedRenderMeshCacheKey))
    {
        return;
    }

    TWeakObjectPtr<UWorldBakedModelAsset> WeakOwner(this);
    // A native lambda capture is invisible to GC. Pin the complete reflected config (including
    // material overrides, skeleton, physics asset and custom objects) before the admission wait.
    const uint64 ConfigId = RetainRequestConfig(Config);
    Callback = [WeakOwner, ConfigId, Completion = MoveTemp(Callback)](UStaticMesh* Mesh)
    {
        ON_SCOPE_EXIT { if (auto* Owner = WeakOwner.Get()) Owner->ReleaseReadReferences(ConfigId); };
        if (Completion) Completion(Mesh);
    };
    const uint32 MaterialSignature = WorldBakedModelAssetPrivate::BuildMaterialConfigSignature(Config.MaterialsConfig);
    const auto SubmitPayload = [WeakOwner, MeshIndices, TextureLimit, MaterialSignature, Config, Callback](const bool bReady)
    {
        UWorldBakedModelAsset* Owner = WeakOwner.Get();
        if (!bReady || !Owner) { if (Callback) Callback(nullptr); return; }
        FV3DStreamingBudget::Enqueue(Owner,
            [WeakOwner, MeshIndices, TextureLimit, MaterialSignature, bMaterials = !Config.MaterialsConfig.bSkipLoad]()
            { return WeakOwner.IsValid() ? WeakOwner->EstimateLoadBytes(MeshIndices, bMaterials, INDEX_NONE, TextureLimit, MaterialSignature) : int64(0); },
            [WeakOwner, MeshIndices, Config, Callback,
                WeakOuter = TWeakObjectPtr<UObject>(Config.Outer), bHadOuter = Config.Outer != nullptr]
                (FV3DStreamingBudget::FPermit Permit) mutable
            {
                if (bHadOuter && !WeakOuter.IsValid())
                {
                    if (Callback) Callback(nullptr);
                    return;
                }
                if (auto* Self = WeakOwner.Get())
                    Self->LoadStaticMeshLODsAdmitted(MeshIndices,
                        [Permit, Callback](UStaticMesh* Mesh) { if (Callback) Callback(Mesh); }, Config);
                else if (Callback) Callback(nullptr);
            },
            [Callback]() { if (Callback) Callback(nullptr); });
    };
    if (Config.MaterialsConfig.bSkipLoad) SubmitPayload(true);
    else PrepareDependenciesAsync(MeshIndices, SubmitPayload);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAdmitted(
    const TArray<int32>& MeshIndices, FGWorldBakedStaticMeshNativeCallback Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    FWorldBakedBuildReferenceGuard CacheGuard;
    CacheGuard.Add(Config.Outer);
    const int32 TextureResolution = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedDependencies(MeshIndices, Config.MaterialsConfig,
            CachedTextureIds, CachedMaterialIds, CacheGuard, TextureResolution);
    }

    const uint64 ReadId = RetainReadReferences(CacheGuard);
    const TWeakObjectPtr<UObject> WeakOuter(Config.Outer);
    const bool bHadOuter = Config.Outer != nullptr;
    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference, WeakOuter, bHadOuter,
                MeshIndices, Callback, Config, CachedTextureIds, CachedMaterialIds, TextureResolution, ReadId]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                TArray<FglTFRuntimeMeshLOD> LODs;
                TArray<TArray<int32>> MaterialIds;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, INDEX_NONE,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
                    &CachedTextureIds, &CachedMaterialIds, TextureResolution);
                const bool bPrepared = bRead
                    && WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
                        Bundle, INDEX_NONE, !Config.MaterialsConfig.bSkipLoad,
                        LODs, MaterialIds, Error);

                // Geometry/joints/morphs are now fully detached in RuntimeLODs. Do not carry the
                // duplicate baked mesh arrays back to GT; only material/texture payloads remain.
                if (bPrepared)
                {
                    Bundle.Meshes.Empty();
                    Bundle.Skins.Empty();
                }

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, Callback, Config, bPrepared, ReadId, WeakOuter, bHadOuter,
                        Bundle = MoveTemp(Bundle), LODs = MoveTemp(LODs),
                        MaterialIds = MoveTemp(MaterialIds), Error = MoveTemp(Error)]() mutable
                    {
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        ON_SCOPE_EXIT { if (auto* Owner = WeakThis.Get()) Owner->ReleaseReadReferences(ReadId); };
                        if (!IsValid(Self) || (bHadOuter && !WeakOuter.IsValid()) || FSafeFileIO::IsShuttingDown())
                        {
                            if (Callback) Callback(nullptr);
                            return;
                        }
                        if (bPrepared)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            ReferenceGuard.Add(Config.Outer);
                            if (Self->AttachRuntimeMaterials(
                                    Bundle, Config.MaterialsConfig, MaterialIds,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                Self->QueueStaticRuntimeLODFinalizer(
                                    MoveTemp(LODs), Config, Callback,
                                    LocalReference, ReferenceGuard);
                                return;
                            }
                        }
                        if (Error.IsEmpty()) Error = TEXT("Streamed baked static mesh preparation failed");
                        FV3DRuntimeSafety::ReportRecoverableFailure(LocalReference, Error);
                        if (Callback) Callback(nullptr);
                    }, true); // Terminal callback releases pins even during module shutdown.
            }, true);

    if (!bQueued)
    {
        ReleaseReadReferences(ReadId);
        UE_LOG(LogTemp, Verbose, TEXT("Baked mesh I/O was rejected during shutdown"));
        if (Callback) Callback(nullptr);
    }
}

USkeletalMesh* UWorldBakedModelAsset::LoadSkeletalMesh(
    const int32 MeshIndex,
    const int32 SkinIndex,
    const FglTFRuntimeSkeletalMeshConfig& InConfig,
    FString* OutError)
{
    check(IsInGameThread());
    FglTFRuntimeSkeletalMeshConfig Config = InConfig;
    const int32 TextureLimit = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    FString Error;
    FGWorldBakedAssetBundle Bundle;
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    FWorldBakedBuildReferenceGuard CacheGuard;
    CacheGuard.Add(this);
    CacheGuard.Add(Config.Outer);
    const uint64 ConfigId = RetainRequestConfig(Config);
    ON_SCOPE_EXIT { ReleaseReadReferences(ConfigId); };
    const int32 TextureResolution = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedDependencies(MeshIndices, Config.MaterialsConfig,
            CachedTextureIds, CachedMaterialIds, CacheGuard, TextureResolution);
    }
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, SkinIndex,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
            &CachedTextureIds, &CachedMaterialIds, TextureResolution))
    {
        if (Error.IsEmpty())
        {
            Error = TEXT("The baked model reader is not initialized");
        }
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }

    FWorldBakedBuildReferenceGuard ReferenceGuard;
    TArray<FglTFRuntimeMeshLOD> LODs;
    if (!BuildRuntimeLODs(
            Bundle, Config.MaterialsConfig, SkinIndex,
            ReferenceGuard, LODs, Error))
    {
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }
    Bundle.Reset();

    // The reference skeleton and joint map are already present in RuntimeLODs. Telling the plugin
    // to ignore parser skins guarantees it never looks for a GLB skin object on the empty parser.
    FglTFRuntimeSkeletalMeshConfig LocalConfig = Config;
    LocalConfig.bIgnoreSkin = true;
    LocalConfig.OverrideSkinIndex = INDEX_NONE;

    // Protect the transient empty-parser finalizer from nested GC during skeletal construction.
    TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(CreateMeshBuilder(Error));
    UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
    USkeletalMesh* Result = nullptr;
    bool bBuilderInitialized = false;
    const bool bExecuted = Builder
        && FV3DRuntimeSafety::ExecuteSynchronousOperation(
            TEXT("Build baked skeletal mesh"),
            [&Result, &bBuilderInitialized, Builder, &LODs, &LocalConfig, &ReferenceGuard]()
            {
                bBuilderInitialized =
                    WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder);
                if (bBuilderInitialized)
                {
                    Result = Builder->LoadSkeletalMeshFromRuntimeLODs(
                        LODs, INDEX_NONE, LocalConfig);
                    // Hold the result across the coordinator's post-operation queue pump.
                    ReferenceGuard.Add(Result);
                }
            });
    if (!bExecuted && Error.IsEmpty())
    {
        Error = TEXT("The glTFRuntime skeletal finalizer is busy or shutting down");
    }
    if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
    {
        Error = TEXT("Could not initialize the decoded-data skeletal mesh finalizer");
    }
    if (!Result && Error.IsEmpty())
    {
        Error = TEXT("Decoded-data skeletal mesh finalization failed");
    }
    if (OutError)
    {
        *OutError = Error;
    }
    return Result;
}

void UWorldBakedModelAsset::LoadSkeletalMeshAsync(
    const int32 MeshIndex,
    const int32 SkinIndex,
    const FglTFRuntimeSkeletalMeshAsync& Callback,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    FglTFRuntimeSkeletalMeshAsync DynamicCallback = Callback;
    LoadSkeletalMeshAsyncNative(
        MeshIndex, SkinIndex,
        [DynamicCallback](USkeletalMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadSkeletalMeshAsyncNative(
    const int32 MeshIndex,
    const int32 SkinIndex,
    FGWorldBakedSkeletalMeshNativeCallback Callback,
    const FglTFRuntimeSkeletalMeshConfig& InConfig)
{
    check(IsInGameThread());
    FglTFRuntimeSkeletalMeshConfig Config = InConfig;
    const int32 TextureLimit = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakOwner(this);
    // A native lambda capture is invisible to GC. Pin the complete reflected config (including
    // material overrides, skeleton, physics asset and custom objects) before the admission wait.
    const uint64 ConfigId = RetainRequestConfig(Config);
    Callback = [WeakOwner, ConfigId, Completion = MoveTemp(Callback)](USkeletalMesh* Mesh)
    {
        ON_SCOPE_EXIT { if (auto* Owner = WeakOwner.Get()) Owner->ReleaseReadReferences(ConfigId); };
        if (Completion) Completion(Mesh);
    };
    const uint32 MaterialSignature = WorldBakedModelAssetPrivate::BuildMaterialConfigSignature(Config.MaterialsConfig);
    const auto SubmitPayload = [WeakOwner, MeshIndex, SkinIndex, TextureLimit, MaterialSignature, Config, Callback](const bool bReady)
    {
        UWorldBakedModelAsset* Owner = WeakOwner.Get();
        if (!bReady || !Owner) { if (Callback) Callback(nullptr); return; }
        FV3DStreamingBudget::Enqueue(Owner,
            [WeakOwner, MeshIndex, SkinIndex, TextureLimit, MaterialSignature, bMaterials = !Config.MaterialsConfig.bSkipLoad]()
            { return WeakOwner.IsValid() ? WeakOwner->EstimateLoadBytes({MeshIndex}, bMaterials, SkinIndex, TextureLimit, MaterialSignature) : int64(0); },
            [WeakOwner, MeshIndex, SkinIndex, Config, Callback,
                WeakOuter = TWeakObjectPtr<UObject>(Config.Outer), bHadOuter = Config.Outer != nullptr]
                (FV3DStreamingBudget::FPermit Permit) mutable
            {
                if (bHadOuter && !WeakOuter.IsValid())
                {
                    if (Callback) Callback(nullptr);
                    return;
                }
                if (auto* Self = WeakOwner.Get())
                    Self->LoadSkeletalMeshAdmitted(MeshIndex, SkinIndex,
                        [Permit, Callback](USkeletalMesh* Mesh) { if (Callback) Callback(Mesh); }, Config);
                else if (Callback) Callback(nullptr);
            },
            [Callback]() { if (Callback) Callback(nullptr); });
    };
    if (Config.MaterialsConfig.bSkipLoad) SubmitPayload(true);
    else PrepareDependenciesAsync({MeshIndex}, SubmitPayload);
}

void UWorldBakedModelAsset::LoadSkeletalMeshAdmitted(
    const int32 MeshIndex, const int32 SkinIndex, FGWorldBakedSkeletalMeshNativeCallback Callback,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    check(IsInGameThread());
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    FWorldBakedBuildReferenceGuard CacheGuard;
    CacheGuard.Add(Config.Outer);
    const int32 TextureResolution = WorldBakedModelAssetPrivate::ResolveRequestTextureLimit(Config.MaterialsConfig, this);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedDependencies(MeshIndices, Config.MaterialsConfig,
            CachedTextureIds, CachedMaterialIds, CacheGuard, TextureResolution);
    }

    const uint64 ReadId = RetainReadReferences(CacheGuard);
    const TWeakObjectPtr<UObject> WeakOuter(Config.Outer);
    const bool bHadOuter = Config.Outer != nullptr;
    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference, WeakOuter, bHadOuter,
                MeshIndices, SkinIndex, Callback, Config, CachedTextureIds, CachedMaterialIds, TextureResolution, ReadId]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                TArray<FglTFRuntimeMeshLOD> LODs;
                TArray<TArray<int32>> MaterialIds;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, SkinIndex,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
                    &CachedTextureIds, &CachedMaterialIds, TextureResolution);
                const bool bPrepared = bRead
                    && WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
                        Bundle, SkinIndex, !Config.MaterialsConfig.bSkipLoad,
                        LODs, MaterialIds, Error);
                if (bPrepared)
                {
                    Bundle.Meshes.Empty();
                    Bundle.Skins.Empty();
                }

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, Callback, Config, bPrepared, ReadId, WeakOuter, bHadOuter,
                        Bundle = MoveTemp(Bundle), LODs = MoveTemp(LODs),
                        MaterialIds = MoveTemp(MaterialIds), Error = MoveTemp(Error)]() mutable
                    {
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        ON_SCOPE_EXIT { if (auto* Owner = WeakThis.Get()) Owner->ReleaseReadReferences(ReadId); };
                        if (!IsValid(Self) || (bHadOuter && !WeakOuter.IsValid()) || FSafeFileIO::IsShuttingDown())
                        {
                            if (Callback) Callback(nullptr);
                            return;
                        }
                        if (bPrepared)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            ReferenceGuard.Add(Config.Outer);
                            if (Self->AttachRuntimeMaterials(
                                    Bundle, Config.MaterialsConfig, MaterialIds,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                FglTFRuntimeSkeletalMeshConfig LocalConfig = Config;
                                // Skeleton + authoritative joint maps are embedded in RuntimeLODs.
                                // Never re-read a parser skin during final reconstruction.
                                LocalConfig.bIgnoreSkin = true;
                                LocalConfig.OverrideSkinIndex = INDEX_NONE;
                                Self->QueueSkeletalRuntimeLODFinalizer(
                                    MoveTemp(LODs), LocalConfig, Callback,
                                    LocalReference, ReferenceGuard);
                                return;
                            }
                        }
                        if (Error.IsEmpty()) Error = TEXT("Streamed baked skeletal mesh preparation failed");
                        FV3DRuntimeSafety::ReportRecoverableFailure(LocalReference, Error);
                        if (Callback) Callback(nullptr);
                    }, true); // Terminal callback releases pins even during module shutdown.
            }, true);

    if (!bQueued)
    {
        ReleaseReadReferences(ReadId);
        UE_LOG(LogTemp, Verbose, TEXT("Baked skeletal I/O was rejected during shutdown"));
        if (Callback) Callback(nullptr);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DBakedRequestReferencesTest,
    "V3DSimulator.Streaming.Memory.RequestReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DBakedRequestReferencesTest::RunTest(const FString&)
{
    // This test performs full GC; run in an idle editor alongside the admission/shutdown tests.
    TStrongObjectPtr<UWorldBakedModelAsset> Asset(NewObject<UWorldBakedModelAsset>());
    FglTFRuntimeStaticMeshConfig StaticConfig;
    StaticConfig.Outer = NewObject<UObject>();
    StaticConfig.MaterialsConfig.ForceMaterial = NewObject<UMaterialInstanceDynamic>();
    const TWeakObjectPtr<UObject> Outer(StaticConfig.Outer);
    const TWeakObjectPtr<UMaterialInterface> Override(StaticConfig.MaterialsConfig.ForceMaterial);
    const uint64 StaticId = Asset->RetainRequestConfig(StaticConfig);
    StaticConfig.Outer = nullptr;
    StaticConfig.MaterialsConfig.ForceMaterial = nullptr;
    FglTFRuntimeSkeletalMeshConfig SkeletalConfig;
    SkeletalConfig.Skeleton = NewObject<USkeleton>();
    const TWeakObjectPtr<USkeleton> Skeleton(SkeletalConfig.Skeleton);
    const uint64 SkeletalId = Asset->RetainRequestConfig(SkeletalConfig);
    SkeletalConfig.Skeleton = nullptr;
    CollectGarbage(RF_NoFlags);
    TestTrue(TEXT("Admission wait retains outer"), Outer.IsValid());
    TestTrue(TEXT("Admission wait retains material overrides"), Override.IsValid());
    TestTrue(TEXT("Admission wait retains skeleton"), Skeleton.IsValid());
    Asset->ReleaseReadReferences(StaticId);
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Completed request releases outer"), Outer.IsValid());
    TestFalse(TEXT("Completed request releases overrides"), Override.IsValid());
    TestTrue(TEXT("Another request remains pinned"), Skeleton.IsValid());
    Asset->ReleaseReadReferences(SkeletalId);
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Completed skeletal request releases skeleton"), Skeleton.IsValid());

    constexpr int32 Limit = 512;
    const auto TextureKey = [](const int32 Id) { return (uint64(uint32(Id)) << 32) | uint64(512); };
    TWeakObjectPtr<UTexture2D> Used(NewObject<UTexture2D>());
    TWeakObjectPtr<UTexture2D> Unrelated(NewObject<UTexture2D>());
    Asset->WeakTextureCache.Add(TextureKey(11), Used);
    Asset->WeakTextureCache.Add(TextureKey(22), Unrelated);
    Asset->MeshMaterialDependencies.Add(7, TArray<int32>{3});
    Asset->MaterialTextureDependencies.Add(3, TArray<int32>{11});
    Asset->MaterialTextureDependencies.Add(4, TArray<int32>{22});
    uint64 ReadId = 0;
    {
        FWorldBakedBuildReferenceGuard Guard;
        TSet<int32> TextureIds, MaterialIds;
        Asset->CollectCachedDependencies({7}, StaticConfig.MaterialsConfig,
            TextureIds, MaterialIds, Guard, Limit);
        TestEqual(TEXT("Only requested texture is skipped by I/O"), TextureIds.Num(), 1);
        TestTrue(TEXT("Requested dependency is retained"), TextureIds.Contains(11));
        TestFalse(TEXT("Unrelated cached dependency is excluded"), TextureIds.Contains(22));
        ReadId = Asset->RetainReadReferences(Guard);
    }
    CollectGarbage(RF_NoFlags);
    TestTrue(TEXT("Skipped texture survives while read is pending"), Used.IsValid());
    TestFalse(TEXT("Overlapping reads cannot retain unrelated textures"), Unrelated.IsValid());
    Asset->ReleaseReadReferences(ReadId);
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Texture pin ends with its own read"), Used.IsValid());

    Asset->Manifest = MakeShared<FGWorldModelManifest, ESPMode::ThreadSafe>();
    FGWorldArchiveRange Small, Large;
    Small.UncompressedSize = 1024 * 1024;
    Large.UncompressedSize = 512ull * 1024 * 1024;
    Asset->Manifest->MeshRanges.Add(7, FGWorldArchiveRange());
    Asset->Manifest->MaterialRanges.Add(3, FGWorldArchiveRange());
    Asset->Manifest->TextureRanges.Add(11, Small);
    Asset->Manifest->TextureRanges.Add(22, Large);
    const uint32 Signature = WorldBakedModelAssetPrivate::BuildMaterialConfigSignature(StaticConfig.MaterialsConfig);
    TestTrue(TEXT("Known mesh budget excludes unrelated 512 MiB texture"),
        Asset->EstimateLoadBytes({7}, true, INDEX_NONE, Limit, Signature) < 8ll * 1024 * 1024);
    TestTrue(TEXT("Unknown mesh keeps conservative budget until decoded"),
        Asset->EstimateLoadBytes({8}, true, INDEX_NONE, Limit, Signature) >= 512ll * 1024 * 1024);
    TestEqual(TEXT("All temporary request pins released"), Asset->PendingReadReferences.Num(), 0);
    return true;
}
#endif
