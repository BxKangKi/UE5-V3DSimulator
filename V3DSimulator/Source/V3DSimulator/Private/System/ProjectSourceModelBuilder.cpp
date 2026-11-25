// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file ProjectSourceModelBuilder.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/ProjectSourceModelBuilder.h"
#include "Format/v3d.h"
#include "Model/V3DModelAnimation.h"
#include "System/V3DTerrain.h"
#include "System/V3DImpostor.h"

#include "Containers/Ticker.h"
#include "Animation/Skeleton.h"
#include "Character/CharacterBoneSchema.h"
#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Model/V3DMaterialOverrideUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Simulator/NodeTokenLibrary.h"
#include "System/SimulatorFileServices.h"
#include "System/GameManagerSubSystem.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/GlbValidation.h"
#include "System/SourceModelImport.h"
#include "System/SafeFileIO.h"
#include "System/StringHelper.h"
#include "System/V3DRuntimeSafety.h"
#include "UObject/UObjectGlobals.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"

namespace ProjectSourceModelBuilderPrivate
{
    constexpr double FineChunkCm = 512.0 * 100.0;
    constexpr double CoarseChunkCm = 8192.0 * 100.0;
    constexpr int32 FinePerCoarse = 16;

    bool TryFloorChunk(const double Value, const double ChunkSize, int64& OutValue)
    {
        if (!FMath::IsFinite(Value) || !FMath::IsFinite(ChunkSize) || ChunkSize <= 0.0) return false;
        const double FloorValue = FMath::FloorToDouble(Value / ChunkSize);
        if (!FMath::IsFinite(FloorValue)
            || FloorValue < static_cast<double>(MIN_int32)
            || FloorValue > static_cast<double>(MAX_int32))
        {
            return false;
        }
        OutValue = static_cast<int64>(FloorValue);
        return true;
    }

    bool ComputeSpatialChunk(const FVector& Location, FIntVector& OutCoarse, FIntVector& OutFine)
    {
        int64 CoarseX = 0, CoarseY = 0, CoarseZ = 0;
        int64 GlobalFineX = 0, GlobalFineY = 0, GlobalFineZ = 0;
        if (!TryFloorChunk(Location.X, CoarseChunkCm, CoarseX)
            || !TryFloorChunk(Location.Y, CoarseChunkCm, CoarseY)
            || !TryFloorChunk(Location.Z, CoarseChunkCm, CoarseZ)
            || !TryFloorChunk(Location.X, FineChunkCm, GlobalFineX)
            || !TryFloorChunk(Location.Y, FineChunkCm, GlobalFineY)
            || !TryFloorChunk(Location.Z, FineChunkCm, GlobalFineZ))
        {
            OutCoarse = FIntVector::ZeroValue;
            OutFine = FIntVector::ZeroValue;
            return false;
        }

        const int64 FineX = GlobalFineX - CoarseX * FinePerCoarse;
        const int64 FineY = GlobalFineY - CoarseY * FinePerCoarse;
        const int64 FineZ = GlobalFineZ - CoarseZ * FinePerCoarse;
        if (FineX < 0 || FineX >= FinePerCoarse
            || FineY < 0 || FineY >= FinePerCoarse
            || FineZ < 0 || FineZ >= FinePerCoarse)
        {
            OutCoarse = FIntVector::ZeroValue;
            OutFine = FIntVector::ZeroValue;
            return false;
        }

        OutCoarse = FIntVector(static_cast<int32>(CoarseX), static_cast<int32>(CoarseY), static_cast<int32>(CoarseZ));
        OutFine = FIntVector(static_cast<int32>(FineX), static_cast<int32>(FineY), static_cast<int32>(FineZ));
        return true;
    }
    constexpr int32 MaxDefinitionCharacters = 16 * 1024 * 1024;

    bool IsFiniteVector(const FVector& Value)
    {
        return FMath::IsFinite(Value.X)
            && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z);
    }

    bool IsFiniteTransform(const FTransform& Value)
    {
        const FQuat Rotation = Value.GetRotation();
        return !Value.ContainsNaN()
            && IsFiniteVector(Value.GetLocation())
            && IsFiniteVector(Value.GetScale3D())
            && FMath::IsFinite(Rotation.X)
            && FMath::IsFinite(Rotation.Y)
            && FMath::IsFinite(Rotation.Z)
            && FMath::IsFinite(Rotation.W)
            && Rotation.IsNormalized();
    }

    bool IsWaterNode(const FString& Name)
    {
        return USimulatorNodeTokenLibrary::HasEffectiveToken(
            Name, FName(TEXT("WATER")));
    }

    bool FindUnsupportedNumericLodSegment(const FString& NodeName, FString& OutSegment)
    {
        OutSegment.Reset();
        int32 Delimiter = INDEX_NONE;
        if (!NodeName.FindChar(TEXT(';'), Delimiter)) return false;

        TArray<FString> Segments;
        NodeName.Mid(Delimiter + 1).ParseIntoArray(Segments, TEXT(";"), false);
        for (const FString& Raw : Segments)
        {
            const FString Token = USimulatorNodeTokenLibrary::NormalizeSegment(Raw.TrimStartAndEnd());
            if (!Token.StartsWith(TEXT("LOD"), ESearchCase::CaseSensitive) || Token.Len() <= 3) continue;

            bool bNumeric = true;
            for (int32 Index = 3; Index < Token.Len(); ++Index)
            {
                if (!FChar::IsDigit(Token[Index]))
                {
                    bNumeric = false;
                    break;
                }
            }
            if (!bNumeric) continue;
            if (Token == TEXT("LOD0") || Token == TEXT("LOD1")
                || Token == TEXT("LOD2") || Token == TEXT("LOD3")) continue;

            OutSegment = Token;
            return true;
        }
        return false;
    }

    bool SourceContainsOnlyTerrainMaterials(UglTFRuntimeAsset* Source)
    {
        if (!Source || !Source->GetParser().IsValid()) return false;
        const auto& Materials = Source->GetParser()->GetMaterials();
        if (Materials.IsEmpty()) return false;
        for (const auto& Material : Materials)
        {
            FString Name;
            // GetMaterials returns non-null TSharedRef rows, not nullable TSharedPtr rows.
            if (!Material->TryGetStringField(TEXT("name"), Name)
                || !V3DTerrain::IsTerrainName(Name)) return false;
        }
        return true;
    }

    /** Build-only material policy shared by every mesh decoded from the current source GLB. */
    FglTFRuntimeMaterialsConfig MakeMaterialsConfig(UProjectSourceModelBuilder* Builder, UglTFRuntimeAsset* Source)
    {
        FglTFRuntimeMaterialsConfig Config;
        Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;


        Config.bGeneratesMipMaps = true;
        Config.bLoadMipMaps = true;
        Config.SpecularFactor = 0.0f;
        // Terrain array construction needs CPU pixels, before platform compression.
        const bool bTerrainSource = V3DTerrain::SourceHasTerrain(Source);
        // Never bake a host-selected GPU compressor (BC on D3D vs raw/other formats on Metal).
        // Runtime texture limits are applied when reading the archive, not while authoring it.
        Config.ImagesConfig.bCompressMips = false;
        // Build-time textures are captured immediately from FTexturePlatformData on the game thread.
        // Keep glTFRuntime texture streaming disabled here. On UE 5.8, bStreaming=true attaches
        // glTFRuntime's custom mip provider before UpdateResource(); the provider can report a
        // different tiled layout from the transient texture and triggers StreamableTextureResource
        // validation. CaptureTexture() locks BulkData directly and no longer relies on the provider.
        Config.ImagesConfig.bStreaming = false;
        int32 TextureLimit = FMath::Clamp(Builder->GetBuildTextureResolution(), 64, 8192);
        if (bTerrainSource && SourceContainsOnlyTerrainMaterials(Source))
        {
            TextureLimit = FMath::Min(TextureLimit,
                FMath::Clamp(Builder->GetBuildTerrainTextureResolution(), 64, 2048));
        }
        Config.ImagesConfig.MaxWidth = TextureLimit;
        Config.ImagesConfig.MaxHeight = TextureLimit;
        return Config;
    }

    /**
     * Builds exactly the same canonical skeleton policy used by CaptureSkin().  Character geometry
     * must be decoded through glTFRuntime's skin-aware path so each primitive receives the
     * authoritative joint-index -> canonical-bone map produced while the reference skeleton is
     * generated.  Reconstructing that map later from raw glTF joint names is not equivalent when
     * ForceRootNode/helper-node filtering is involved.
     */
    bool MakeCharacterSkeletonConfig(
        const TMap<FString, FString>& BoneAliases,
        USkeleton* TargetSkeleton,
        FglTFRuntimeSkeletonConfig& OutConfig,
        FString& OutError)
    {
        OutConfig = FglTFRuntimeSkeletonConfig();
        OutConfig.CacheMode = EglTFRuntimeCacheMode::None;
        OutConfig.BonesNameMap = BoneAliases;
        OutConfig.RootBoneName = TEXT("Root");
        OutConfig.RootNodeIndex = -1;
        OutConfig.MaxNodesTreeDepth = -1;

        FString AliasError;
        if (!CharacterBoneSchema::ValidateSourceToCanonicalMap(BoneAliases, AliasError))
        {
            OutError = FString::Printf(TEXT("Character bone map rejected before mesh decode: %s"), *AliasError);
            return false;
        }
        if (!IsValid(TargetSkeleton))
        {
            OutError = TEXT("Character mesh decode requires the configured default character skeleton");
            return false;
        }

        FString SourceRootBone;
        for (const TPair<FString, FString>& Alias : BoneAliases)
        {
            if (Alias.Value.Equals(TEXT("Root"), ESearchCase::CaseSensitive))
            {
                SourceRootBone = Alias.Key;
                break;
            }
        }
        if (SourceRootBone.IsEmpty())
        {
            OutError = TEXT("Character bone map has no source bone mapped to canonical Root");
            return false;
        }

        OutConfig.ForceRootNode = SourceRootBone;
        OutConfig.bAddRootBone = false;
        OutConfig.bAddRootNodeIfMissing = false;
        // The canonical Root can sit below an Armature/helper node carrying the glTF-to-model
        // orientation. ForceRootNode intentionally prunes that helper from the UE skeleton, so fold
        // its transform into Root instead of silently losing it. The mesh decoder below uses Tree
        // for the same reason, keeping skinned geometry and the reference hierarchy in one space.
        OutConfig.bApplyParentNodesTransformsToRoot = true;
        OutConfig.bClearRotations = true;
        OutConfig.CopyRotationsFrom = TargetSkeleton;
        return true;
    }

    /** Finds one named glTF node that owns the requested skinned source mesh. */
    bool FindCharacterMeshNode(
        UglTFRuntimeAsset* SourceAsset,
        const int32 MeshIndex,
        FString& OutNodeName,
        int32& OutSkinIndex,
        FString& OutError)
    {
        OutNodeName.Reset();
        OutSkinIndex = INDEX_NONE;
        if (!IsValid(SourceAsset) || MeshIndex < 0)
        {
            OutError = TEXT("Cannot locate a skinned node for an invalid character mesh");
            return false;
        }

        const TArray<FglTFRuntimeNode>& Nodes = SourceAsset->GetNodes();
        const FglTFRuntimeNode* Selected = nullptr;
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            if (Node.MeshIndex != MeshIndex || Node.SkinIndex < 0) continue;
            if (!Selected || Node.Index < Selected->Index) Selected = &Node;
        }
        if (!Selected)
        {
            OutError = FString::Printf(TEXT("Character source mesh %d has no node with a skin"), MeshIndex);
            return false;
        }
        if (Selected->Name.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(TEXT("Character source mesh %d is attached to an unnamed glTF node"), MeshIndex);
            return false;
        }

        // LoadNodeByName is name based.  Ambiguous node names could silently pick a different
        // skin/mesh and produce a perfectly sized but semantically wrong bone map.
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            if (Node.Index != Selected->Index
                && Node.Name.Equals(Selected->Name, ESearchCase::CaseSensitive))
            {
                OutError = FString::Printf(
                    TEXT("Character skinned node name '%s' is duplicated in the GLB"), *Selected->Name);
                return false;
            }
        }

        // The recursive API is used only to obtain glTFRuntime's authoritative skin mapping.
        // Refuse a mesh node that owns another renderable descendant; otherwise that descendant
        // would be folded into this baked mesh and later duplicated when its own source mesh is read.
        TMap<int32, const FglTFRuntimeNode*> NodeByIndex;
        for (const FglTFRuntimeNode& Node : Nodes) NodeByIndex.Add(Node.Index, &Node);
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            if (Node.Index == Selected->Index || Node.MeshIndex < 0) continue;
            const FglTFRuntimeNode* Current = &Node;
            while (Current && Current->ParentIndex >= 0)
            {
                if (Current->ParentIndex == Selected->Index)
                {
                    OutError = FString::Printf(
                        TEXT("Character skinned node '%s' contains renderable child node '%s'; split nested character meshes before baking"),
                        *Selected->Name, *Node.Name);
                    return false;
                }
                const FglTFRuntimeNode* const* Parent = NodeByIndex.Find(Current->ParentIndex);
                Current = Parent ? *Parent : nullptr;
            }
        }

        OutNodeName = Selected->Name;
        OutSkinIndex = Selected->SkinIndex;
        return true;
    }

    /** Rejects the exact corruption that bIgnoreMissingBones used to hide at runtime. */
    bool ValidateAuthoritativeBoneMaps(const FglTFRuntimeMeshLOD& RuntimeLOD, FString& OutError)
    {
        for (int32 PrimitiveIndex = 0; PrimitiveIndex < RuntimeLOD.Primitives.Num(); ++PrimitiveIndex)
        {
            const FglTFRuntimePrimitive& Primitive = RuntimeLOD.Primitives[PrimitiveIndex];
            if (Primitive.Joints.IsEmpty()) continue;
            if (Primitive.Joints.Num() != Primitive.Weights.Num())
            {
                OutError = FString::Printf(TEXT("Character primitive %d has mismatched joint/weight sets"), PrimitiveIndex);
                return false;
            }

            for (int32 SetIndex = 0; SetIndex < Primitive.Joints.Num(); ++SetIndex)
            {
                const auto& Joints = Primitive.Joints[SetIndex];
                const auto& Weights = Primitive.Weights[SetIndex];
                if (Joints.Num() != Weights.Num())
                {
                    OutError = FString::Printf(TEXT("Character primitive %d skin stream %d has mismatched lengths"), PrimitiveIndex, SetIndex);
                    return false;
                }
                for (int32 VertexIndex = 0; VertexIndex < Joints.Num(); ++VertexIndex)
                {
                    const auto& Joint = Joints[VertexIndex];
                    const auto& Weight = Weights[VertexIndex];
                    const int32 JointIndices[4] = {
                        static_cast<int32>(Joint.X), static_cast<int32>(Joint.Y),
                        static_cast<int32>(Joint.Z), static_cast<int32>(Joint.W) };
                    const double JointWeights[4] = {
                        static_cast<double>(Weight.X), static_cast<double>(Weight.Y),
                        static_cast<double>(Weight.Z), static_cast<double>(Weight.W) };
                    for (int32 Influence = 0; Influence < 4; ++Influence)
                    {
                        if (FMath::Abs(JointWeights[Influence]) <= UE_SMALL_NUMBER) continue;
                        const FName* BoneName = Primitive.OverrideBoneMap.Find(JointIndices[Influence]);
                        if (!BoneName || BoneName->IsNone())
                        {
                            OutError = FString::Printf(
                                TEXT("Character primitive %d vertex %d references joint %d with weight %.6f but glTFRuntime produced no bone mapping"),
                                PrimitiveIndex, VertexIndex, JointIndices[Influence], JointWeights[Influence]);
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    bool ParseModelSettings(
        const FString& DefinitionJson,
        FModelData& OutData,
        FString& OutError)
    {
        OutData = FModelData();
        if (DefinitionJson.IsEmpty()
            || DefinitionJson.Len() > MaxDefinitionCharacters)
        {
            OutError = TEXT("validated model JSON snapshot is empty or too large");
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader =
            TJsonReaderFactory<>::Create(DefinitionJson);
        if (!FJsonSerializer::Deserialize(Reader, Root)
            || !Root.IsValid()
            || !OutData.Deserialization(Root))
        {
            OutError = TEXT("validated model JSON could not be decoded into model settings");
            return false;
        }
        return true;
    }
}

bool UProjectSourceModelBuilder::Start(
    const FModelDefinition& InDefinition,
    const FString& InDefinitionJson)
{
    check(IsInGameThread());
    if (bRunning || bCompletionSent)
    {
        return false;
    }

    const FString SourcePath = GlbValidation::NormalizePath(InDefinition.GlbPath);
    if (!InDefinition.UUID.IsValid()
        || SourcePath.IsEmpty()
        || InDefinitionJson.IsEmpty())
    {
        Complete(false, TEXT("source definition, model path, or JSON snapshot is invalid"));
        return false;
    }

    Definition = InDefinition;
    Definition.GlbPath = SourcePath;
    DefinitionJson = InDefinitionJson;
    FString ImportError;
    if (!V3DSourceModelImport::CheckAvailability(SourcePath, ImportError))
    {
        Complete(false, ImportError);
        return false;
    }
    SourceFileSize = IFileManager::Get().FileSize(*SourcePath);
    SourceTimestamp = IFileManager::Get().GetTimeStamp(*SourcePath);
    if (SourceFileSize <= 0)
    {
        Complete(false, TEXT("source model is missing or empty"));
        return false;
    }

    bRunning = true;
    bCancelled = false;
    bCompletionSent = false;
    ProgressValue = 0.01f;
    FailureReason.Reset();
    const uint64 Generation = ++RequestGeneration;
    BeginParserLoad(Generation);
    return true;
}

void UProjectSourceModelBuilder::BeginParserLoad(const uint64 Generation)
{
    check(IsInGameThread());
    const FString SourcePath = Definition.GlbPath;
    FglTFRuntimeConfig Config;
    Config.bAllowExternalFiles = true;
    Config.bAsBlob = V3DSourceModelImport::UsesBlob(V3DSourceModelImport::GetFormat(SourcePath));
    if (Definition.IsHumanoid())
    {
        // glTFRuntime's default glTF basis is correct for ordinary scene assets, but Unreal's
        // Character convention expects skeletal forward along Y. The plugin's own skeleton
        // remapping guide explicitly uses YForward for this case. Apply it at parser creation so
        // nodes, inverse bind matrices, reference poses, animations and skinned vertices all share
        // one basis; a component-level yaw cannot repair a reference-pose basis mismatch.
        Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
    }
    TWeakObjectPtr<UProjectSourceModelBuilder> WeakThis(this);

    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, SourcePath, Generation, Config]()
        {
            FString Error;
            TSharedPtr<FglTFRuntimeParser> Parser;
            // Dispatch by the real source format. Optional importers consume a glTFRuntime blob;
            // their FBX/OBJ/STL bytes must never pass through the GLB header or JSON parser.
            if (!V3DSourceModelImport::ValidateSourceFile(SourcePath, Error))
            {
                Error = FString::Printf(
                    TEXT("source model preflight failed: %s"), *Error);
            }
            else
            {
                Parser = FV3DRuntimeSafety::CreateParserSafely(SourcePath, Config, &Error);
                if (!Parser.IsValid() && Error.IsEmpty())
                {
                    Error = TEXT("glTFRuntime could not open the validated source model");
                }
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Generation, Parser, Config, Error]()
                {
                    if (UProjectSourceModelBuilder* StrongThis = WeakThis.Get())
                    {
                        StrongThis->CompleteParserLoad(
                            Generation, Parser, Config, Error);
                    }
                });
        });

    if (!bQueued)
    {
        Complete(false, TEXT("source parser worker queue is shutting down"));
    }
}

void UProjectSourceModelBuilder::CompleteParserLoad(
    const uint64 Generation,
    const TSharedPtr<FglTFRuntimeParser>& Parser,
    const FglTFRuntimeConfig& Config,
    const FString& Error)
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || Generation != RequestGeneration)
    {
        return;
    }

    // UObject allocation is illegal while GC is marking. Retain only native parser state in the
    // ticker and retry when the collection boundary has completed.
    if (IsGarbageCollecting())
    {
        TWeakObjectPtr<UProjectSourceModelBuilder> WeakThis(this);
        if (ParserRetryTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(ParserRetryTicker);
        ParserRetryTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [WeakThis, Generation, Parser, Config, Error](float)
            {
                if (FSafeFileIO::IsShuttingDown()) return false;
                if (IsGarbageCollecting()) return true;
                if (UProjectSourceModelBuilder* StrongThis = WeakThis.Get())
                {
                    StrongThis->ParserRetryTicker.Reset();
                    StrongThis->CompleteParserLoad(
                        Generation, Parser, Config, Error);
                }
                return false;
            }));
        return;
    }

    if (!Parser.IsValid())
    {
        Complete(false, Error.IsEmpty()
            ? TEXT("source parser creation failed") : Error);
        return;
    }

    SourceAsset = NewObject<UglTFRuntimeAsset>(this, NAME_None, RF_Transient);
    if (!IsValid(SourceAsset))
    {
        Complete(false, TEXT("could not allocate the build-only glTFRuntime asset"));
        return;
    }
    SourceAsset->RuntimeContextObject = Config.RuntimeContextObject;
    SourceAsset->RuntimeContextString = Config.RuntimeContextString;
    // SetParser also touches native state. Queue behind other parser/mesh operations rather
    // than racing them or treating a busy native gate as a broken GLB.
    TWeakObjectPtr<UProjectSourceModelBuilder> WeakThis(this);
    FV3DRuntimeSafety::EnqueueExclusiveOperation(this, SourceAsset, TEXT("Attach source parser"),
        [WeakThis, Generation, Parser](uint64 Ticket)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning
                && !Self->bCancelled && Self->RequestGeneration == Generation)
            {
                if (Self->SourceAsset->SetParser(Parser.ToSharedRef()))
                {
                    Self->ProgressValue = 0.05f;
                    Self->BeginDecodedCapture();
                }
                else Self->Complete(false, TEXT("glTFRuntime rejected the source parser"));
            }
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakThis](const FString& Reason)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning && !Self->bCancelled)
                Self->Complete(false, Reason);
        });
}

void UProjectSourceModelBuilder::BeginDecodedCapture()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source asset disappeared before decoding"));
        return;
    }

    BakedData.Reset();
    MaterialIds.Reset();
    TextureIds.Reset();
    SourceMeshNames.Reset();
    SourceToBakedMeshIndex.Reset();
    NodeToBakedMeshIndex.Reset();
    SourceMeshesToCapture.Reset();
    SourceMeshSizes.Reset();
    SkinMeshIndices.Reset();
    CurrentMeshIndex = 0;

    ImportSession = MakeShared<V3DSourceModelImport::FSession>(
        V3DSourceModelImport::GetFormat(Definition.GlbPath));
    FString ImportError;
    if (!ImportSession->Initialize(SourceAsset, Definition.GlbPath, ImportError))
    {
        Complete(false, ImportError);
        return;
    }
    SourceMeshNames = ImportSession->MeshNames;
    const bool bExtensionSource = V3DSourceModelImport::UsesBlob(ImportSession->Format);
    const int32 MeshCount = SourceMeshNames.Num();
    const TArray<FglTFRuntimeNode>& Nodes = ImportSession->Nodes;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        FString UnsupportedLod;
        if (ProjectSourceModelBuilderPrivate::FindUnsupportedNumericLodSegment(Node.Name, UnsupportedLod))
        {
            Complete(false, FString::Printf(
                TEXT("node '%s' uses unsupported %s; only LOD0 through LOD3 are supported"),
                *Node.Name, *UnsupportedLod));
            return;
        }
    }

    std::vector<v3d::instance_source_node> SourceNodes;
    SourceNodes.reserve(Nodes.Num());
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        const auto Parsed = USimulatorNodeTokenLibrary::ParseNodeName(Node.Name);
        v3d::instance_source_node Row;
        Row.id = Node.Index; Row.mesh = Node.MeshIndex; Row.skin = Node.SkinIndex;
        Row.base = TCHAR_TO_UTF8(*Parsed.BaseName.TrimStartAndEnd().ToLower());
        Row.instance = Parsed.HasEffectiveToken(TEXT("INST"));
        Row.explicit_lod0 = Parsed.HasEffectiveToken(TEXT("LOD0"));
        if (Parsed.HasEffectiveToken(TEXT("LOD1"))) Row.lod = 1;
        else if (Parsed.HasEffectiveToken(TEXT("LOD2"))) Row.lod = 2;
        else if (Parsed.HasEffectiveToken(TEXT("LOD3"))) Row.lod = 3;
        SourceNodes.push_back(MoveTemp(Row));
    }
    v3d::instance_capture_plan Plan;
    std::string PlanError;
    if (!v3d::build_instance_plan(SourceNodes, MeshCount, !bExtensionSource, Plan, PlanError))
    {
        Complete(false, UTF8_TO_TCHAR(PlanError.c_str()));
        return;
    }
    SourceToBakedMeshIndex.Init(INDEX_NONE, MeshCount);
    TArray<int32> CapturedSources;
    for (int32 Source : Plan.sources)
    {
        SourceToBakedMeshIndex[Source] = CapturedSources.Num();
        CapturedSources.Add(Source);
        SourceMeshesToCapture.Add(Source);
    }
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        const auto Found = Plan.node_meshes.find(Node.Index);
        if (Found == Plan.node_meshes.end()) continue;
        NodeToBakedMeshIndex.Add(Node.Index, Found->second);
        if (Node.SkinIndex >= 0) SkinMeshIndices.FindOrAdd(Node.SkinIndex).Add(Found->second);
    }

    UE_LOG(LogTemp, Display,
        TEXT("World build mesh canonicalization: source=%d baked=%d omitted-inst=%d"),
        MeshCount, CapturedSources.Num(), MeshCount - CapturedSources.Num());
    ScheduleNextMesh();
}

bool UProjectSourceModelBuilder::CaptureMeshBounds(
    const int32 MeshIndex,
    const FglTFRuntimeMeshLOD& RuntimeLOD,
    FString& OutError)
{
    using namespace ProjectSourceModelBuilderPrivate;
    FBox RawBounds(ForceInit);
    for (const FglTFRuntimePrimitive& Primitive : RuntimeLOD.Primitives)
    {
        // Large position buffers are immutable during this call. Limit temporary storage and
        // worker fan-out to four lanes; reduce deterministically after the workers join.
        const int32 LaneCount = Primitive.Positions.Num() >= 65536 ? 4 : 1;
        FBox Bounds[4] = { FBox(ForceInit), FBox(ForceInit), FBox(ForceInit), FBox(ForceInit) };
        uint8 Valid[4] = { 1, 1, 1, 1 };
        ParallelFor(LaneCount, [&Primitive, &Bounds, &Valid, LaneCount](int32 Lane)
        {
            const int32 Begin = static_cast<int32>(static_cast<int64>(Primitive.Positions.Num()) * Lane / LaneCount);
            const int32 End = static_cast<int32>(static_cast<int64>(Primitive.Positions.Num()) * (Lane + 1) / LaneCount);
            for (int32 Index = Begin; Index < End; ++Index)
            {
                const FVector& Position = Primitive.Positions[Index];
                if (!IsFiniteVector(Position)) { Valid[Lane] = 0; return; }
                Bounds[Lane] += Position;
            }
        });
        for (int32 Lane = 0; Lane < LaneCount; ++Lane)
        {
            if (!Valid[Lane])
            {
                OutError = TEXT("decoded mesh contains a non-finite position");
                return false;
            }
            if (Bounds[Lane].IsValid) RawBounds += Bounds[Lane];
        }
    }
    if (!RawBounds.IsValid)
    {
        OutError = TEXT("decoded mesh contains no bounded vertices");
        return false;
    }

    FBox FinalBounds = RawBounds;
    if (!RuntimeLOD.AdditionalTransforms.IsEmpty())
    {
        FinalBounds = FBox(ForceInit);
        for (const FTransform& Transform : RuntimeLOD.AdditionalTransforms)
        {
            if (!IsFiniteTransform(Transform))
            {
                OutError = TEXT("decoded mesh contains an invalid additional transform");
                return false;
            }
            FinalBounds += RawBounds.TransformBy(Transform);
        }
    }
    if (!FinalBounds.IsValid || !IsFiniteVector(FinalBounds.GetSize()))
    {
        OutError = TEXT("decoded mesh bounds are invalid");
        return false;
    }
    // Runtime metadata stores a pivot-centred box (Size/Extent), not a local bounds centre.
    // GetSize() alone loses offsets for meshes whose vertices are far from their pivot, causing
    // both scene and node culling to reject visible geometry. Enclose the actual local bounds.
    const FVector Extent = FinalBounds.Min.GetAbs().ComponentMax(FinalBounds.Max.GetAbs());
    const FVector Size = Extent * 2.0;
    if (!IsFiniteVector(Size))
    {
        OutError = TEXT("pivot-centred mesh bounds exceed the supported range");
        return false;
    }
    SourceMeshSizes.Add(MeshIndex, Size);
    return true;
}

void UProjectSourceModelBuilder::CaptureNextMesh()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset)) return;
    const uint64 Generation = RequestGeneration;
    TWeakObjectPtr<UProjectSourceModelBuilder> WeakThis(this);
    FV3DRuntimeSafety::EnqueueExclusiveOperation(this, SourceAsset, TEXT("Capture source mesh/metadata"),
        [WeakThis, Generation](uint64 Ticket)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning
                && !Self->bCancelled && Self->RequestGeneration == Generation)
                Self->CaptureNextMeshUnderGate();
            // Every plugin call has returned. Pending cache release is now safe.
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakThis](const FString& Reason)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning && !Self->bCancelled)
                Self->Complete(false, Reason);
        });
}

void UProjectSourceModelBuilder::CaptureNextMeshUnderGate()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source model build was cancelled during mesh decoding"));
        return;
    }

    const int32 MeshCount = SourceMeshNames.Num();
    if (CurrentMeshIndex >= MeshCount)
    {
        CaptureSkinsAndMetadata();
        return;
    }

    FglTFRuntimeMaterialsConfig MaterialsConfig =
        ProjectSourceModelBuilderPrivate::MakeMaterialsConfig(this, SourceAsset);

    // The old path intentionally yielded one engine frame after *every* source mesh. A 1,188-mesh
    // GLB therefore paid ~20 seconds of pure frame-yield latency at 60 Hz before decode/archive
    // work was counted. Keep the UI responsive, but process a bounded time slice while we already
    // own the glTFRuntime native gate. This also avoids thousands of enqueue/ticker round trips.
    constexpr int32 MaxMeshesPerNativeBatch = 32;
    constexpr double MaxNativeBatchSeconds = 0.040;
    const double BatchStartSeconds = FPlatformTime::Seconds();
    int32 CapturedThisBatch = 0;

    while (CurrentMeshIndex < MeshCount && CapturedThisBatch < MaxMeshesPerNativeBatch)
    {
        while (CurrentMeshIndex < MeshCount && !SourceMeshesToCapture.Contains(CurrentMeshIndex))
        {
            ++CurrentMeshIndex;
        }
        if (CurrentMeshIndex >= MeshCount)
        {
            CaptureSkinsAndMetadata();
            return;
        }

        const int32 MeshIndex = CurrentMeshIndex;
        const int32 BakedMeshIndex = SourceToBakedMeshIndex.IsValidIndex(MeshIndex)
            ? SourceToBakedMeshIndex[MeshIndex] : INDEX_NONE;
        if (BakedMeshIndex == INDEX_NONE)
        {
            Complete(false, TEXT("internal source-to-baked mesh map is invalid"));
            return;
        }

        FglTFRuntimeMeshLOD RuntimeLOD;
        FString Error;
        bool bDecoded = false;
        bool bCharacterMeshUsesSkin = false;
        if (Definition.IsHumanoid())
        {
            for (const TPair<int32, TSet<int32>>& SkinEntry : SkinMeshIndices)
            {
                if (SkinEntry.Value.Contains(BakedMeshIndex))
                {
                    bCharacterMeshUsesSkin = true;
                    break;
                }
            }
        }

        const bool bBlob = ImportSession.IsValid()
            && V3DSourceModelImport::UsesBlob(ImportSession->Format);
        if (bBlob)
        {
            bool bSkeletal = false;
            bDecoded = ImportSession->DecodeMesh(SourceAsset, MeshIndex, MaterialsConfig,
                RuntimeLOD, bSkeletal, Error);
            if (bDecoded && bSkeletal)
            {
                FGWorldBakedSkin Skin;
                bDecoded = ImportSession->CaptureSkin(RuntimeLOD, MeshIndex,
                    Definition.Bones, CharacterSkeletonOverride.Get(), Skin, Error)
                    && ProjectSourceModelBuilderPrivate::ValidateAuthoritativeBoneMaps(RuntimeLOD, Error);
                if (bDecoded)
                {
                    // Optional importers use their source mesh index as a build-local skin id.
                    // The .v3d reader consumes the same baked structure as ordinary glTF skins.
                    BakedData.Skins.Add(MoveTemp(Skin));
                    SkinMeshIndices.FindOrAdd(MeshIndex).Add(BakedMeshIndex);
                }
            }
            else if (bDecoded && Definition.IsHumanoid())
            {
                Error = TEXT("Character source has no skeleton; OBJ/STL are static formats, use a skinned FBX or glTF");
                bDecoded = false;
            }
        }
        else if (bCharacterMeshUsesSkin)
        {
            FString CharacterNodeName;
            int32 CharacterSkinIndex = INDEX_NONE;
            FglTFRuntimeSkeletonConfig SkeletonConfig;
            if (ProjectSourceModelBuilderPrivate::FindCharacterMeshNode(
                    SourceAsset, MeshIndex, CharacterNodeName, CharacterSkinIndex, Error)
                && ProjectSourceModelBuilderPrivate::MakeCharacterSkeletonConfig(
                    Definition.Bones, CharacterSkeletonOverride.Get(), SkeletonConfig, Error))
            {
                int32 DecodedSkinIndex = CharacterSkinIndex;
                bDecoded = SourceAsset->LoadSkinnedMeshRecursiveAsRuntimeLOD(
                    CharacterNodeName, TArray<FString>(), RuntimeLOD, MaterialsConfig, SkeletonConfig,
                    DecodedSkinIndex, CharacterSkinIndex, EglTFRuntimeRecursiveMode::Tree);
                if (bDecoded && DecodedSkinIndex != CharacterSkinIndex)
                {
                    Error = FString::Printf(
                        TEXT("Character mesh %d decoded skin %d but node declares skin %d"),
                        MeshIndex, DecodedSkinIndex, CharacterSkinIndex);
                    bDecoded = false;
                }
                if (bDecoded
                    && !ProjectSourceModelBuilderPrivate::ValidateAuthoritativeBoneMaps(RuntimeLOD, Error))
                {
                    bDecoded = false;
                }
            }
        }
        else
        {
            bDecoded = SourceAsset->LoadMeshAsRuntimeLOD(MeshIndex, RuntimeLOD, MaterialsConfig);
        }

        if (!bDecoded
            || !CaptureMeshBounds(BakedMeshIndex, RuntimeLOD, Error)
            || !FGWorldBakedDataCapture::CaptureMesh(
                BakedMeshIndex,
                SourceMeshNames.IsValidIndex(MeshIndex)
                    ? SourceMeshNames[MeshIndex] : FString(),
                RuntimeLOD,
                BakedData,
                MaterialIds,
                TextureIds,
                Error))
        {
            Complete(false, FString::Printf(
                TEXT("mesh %d decode/capture failed: %s"),
                MeshIndex,
                Error.IsEmpty() ? TEXT("glTFRuntime decode failed") : *Error));
            return;
        }

        if (Definition.IsHumanoid())
        {
            int32 BaseColorTextureBindings = 0;
            for (const FGWorldBakedMaterial& BakedMaterial : BakedData.Materials)
            {
                for (const FGWorldBakedTextureParameter& TextureParameter : BakedMaterial.Textures)
                {
                    if (TextureParameter.Name.Equals(TEXT("baseColorTexture"), ESearchCase::IgnoreCase)
                        && TextureParameter.TextureId != INDEX_NONE)
                    {
                        ++BaseColorTextureBindings;
                    }
                }
            }
            UE_LOG(LogTemp, Display,
                TEXT("Character bake capture: Mesh=%d MaterialOverride=%s Materials=%d Textures=%d BaseColorBindings=%d"),
                MeshIndex,
                TEXT("source glTF"),
                BakedData.Materials.Num(),
                BakedData.Textures.Num(),
                BaseColorTextureBindings);
        }

        ++CurrentMeshIndex;
        ++CapturedThisBatch;
        const float MeshFraction = MeshCount > 0
            ? static_cast<float>(CurrentMeshIndex) / static_cast<float>(MeshCount)
            : 1.0f;
        ProgressValue = FMath::Clamp(0.05f + MeshFraction * 0.85f, 0.05f, 0.90f);

        if (CapturedThisBatch > 0
            && FPlatformTime::Seconds() - BatchStartSeconds >= MaxNativeBatchSeconds)
        {
            break;
        }
    }

    if (CurrentMeshIndex >= MeshCount)
    {
        CaptureSkinsAndMetadata();
        return;
    }
    ScheduleNextMesh();
}

void UProjectSourceModelBuilder::ScheduleNextMesh()
{
    check(IsInGameThread());
    if (NextMeshTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(NextMeshTicker);
    const uint64 Generation = RequestGeneration;
    TWeakObjectPtr<UProjectSourceModelBuilder> WeakThis(this);
    // DispatchTrackedGameThread runs inline on GT: using it here recursively decoded every mesh
    // without presenting a frame. A retained ticker yields and can be removed during cancellation.
    NextMeshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakThis, Generation](float)
        {
            if (auto* Self = WeakThis.Get())
            {
                Self->NextMeshTicker.Reset();
                if (Self->bRunning && !Self->bCancelled && Self->RequestGeneration == Generation
                    && !FSafeFileIO::IsShuttingDown()) Self->CaptureNextMesh();
            }
            return false;
        }));
}

void UProjectSourceModelBuilder::CaptureSkinsAndMetadata()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source model build was cancelled before skin capture"));
        return;
    }

    FString Error;
    TArray<int32> SkinIndices;
    SkinMeshIndices.GetKeys(SkinIndices);
    SkinIndices.Sort();
    const bool bBlob = ImportSession.IsValid()
        && V3DSourceModelImport::UsesBlob(ImportSession->Format);
    for (const int32 SkinIndex : SkinIndices)
    {
        if (bBlob) continue; // Captured from each extension RuntimeLOD, not glTF skin JSON.
        // Called only from CaptureNextMeshUnderGate; do not acquire the same gate twice.
        const bool bCaptured = FGWorldBakedDataCapture::CaptureSkin(
            SourceAsset, SkinIndex, SkinMeshIndices.FindChecked(SkinIndex),
            Definition.Bones, CharacterSkeletonOverride.Get(), BakedData, Error);
        if (!bCaptured)
        {
            Complete(false, FString::Printf(
                TEXT("skin %d capture failed: %s"), SkinIndex, *Error));
            return;
        }
    }

    if (!bBlob && !V3DModelAnimation::Capture(SourceAsset, Definition.Bones, BakedData, Error))
    {
        Complete(false, FString(TEXT("animation capture failed: ")) + Error);
        return;
    }

    FGWorldModelMetadata Metadata;
    if (!BakedData.IsSane(&Error)
        || (!bBlob && !V3DTerrain::Bake(SourceAsset, BakedData, Error, this,
            FMath::Min(Definition.BuildTextureResolution, Definition.BuildTerrainTextureResolution),
            Definition.bBuildTerrainFlipNormalGreen))
        || !BakedData.IsSane(&Error)
        || !BuildMetadata(Metadata, Error)
        || (Definition.ModelType == EModelDefinitionType::Static
            && !V3DImpostor::Bake(BakedData, Metadata, Error, this))
        || !BakedData.IsSane(&Error)
        || !Metadata.IsSane(&Error))
    {
        Complete(false, Error.IsEmpty()
            ? TEXT("decoded model validation failed") : Error);
        return;
    }
    Metadata.SourceMeshCount = BakedData.Meshes.Num();
    Metadata.SourceMaterialCount = BakedData.Materials.Num();
    Metadata.SourceTextureCount = BakedData.Textures.Num();
    if (!Metadata.IsSane(&Error)
        || Metadata.SourceMeshCount != BakedData.Meshes.Num()
        || Metadata.SourceMaterialCount != BakedData.Materials.Num()
        || Metadata.SourceTextureCount != BakedData.Textures.Num())
    {
        Complete(false, Error.IsEmpty()
            ? TEXT("decoded model validation failed") : Error);
        return;
    }

    for (auto& Material : BakedData.Materials)
    {
        Material.Scalars.RemoveAll([](const auto& Value) { return Value.Name.StartsWith(TEXT("V3D.Internal.")); });
        Material.Scalars.Add({TEXT("V3D.Internal.Family"), Definition.bToon ? 2.f : (Definition.ModelType == EModelDefinitionType::Static ? 0.f : 1.f)});
        // Toon selection and dynamic camera fade are independent, including when reloaded without gameplay context.
        Material.Scalars.Add({TEXT("V3D.Internal.Dynamic"), Definition.ModelType == EModelDefinitionType::Static ? 0.f : 1.f});
        Material.Scalars.Add({TEXT("V3D.Internal.Mode"), float(Definition.MaterialMode)});
    }
    CompletedModel.Definition = Definition;
    CompletedModel.DefinitionJson = DefinitionJson;
    CompletedModel.Metadata = MoveTemp(Metadata);
    UE_LOG(LogTemp, Display, TEXT("[V3DBuild] Portable texture policy: %s Limit=%d TerrainLimit=%d FlipGreen=%d Meshes=%d Materials=%d Textures=%d"),
        *Definition.Name, Definition.BuildTextureResolution, Definition.BuildTerrainTextureResolution,
        int32(Definition.bBuildTerrainFlipNormalGreen), BakedData.Meshes.Num(), BakedData.Materials.Num(), BakedData.Textures.Num());
    CompletedModel.BakedData = MoveTemp(BakedData);
    CompletedModel.SourceFileSize = SourceFileSize;
    CompletedModel.SourceTimestamp = SourceTimestamp;
    ProgressValue = 0.98f;
    Complete(true);
}

bool UProjectSourceModelBuilder::BuildMetadata(
    FGWorldModelMetadata& OutMetadata,
    FString& OutError) const
{
    using namespace ProjectSourceModelBuilderPrivate;
    check(IsInGameThread());
    OutMetadata = FGWorldModelMetadata();

    FModelData AuthoredSettings;
    if (!ParseModelSettings(DefinitionJson, AuthoredSettings, OutError)) return false;

    const TArray<FglTFRuntimeNode>& Nodes = ImportSession->Nodes;
    const int32 BakedMeshCount = BakedData.Meshes.Num();
    OutMetadata.SourceNodeCount = Nodes.Num();
    OutMetadata.SourceMeshCount = BakedMeshCount;
    OutMetadata.SourceMaterialCount = BakedData.Materials.Num();
    OutMetadata.SourceTextureCount = BakedData.Textures.Num();
    OutMetadata.NodeTransforms.Reserve(Nodes.Num());

    TMap<int32, const FglTFRuntimeNode*> NodeByIndex;
    NodeByIndex.Reserve(Nodes.Num());
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (NodeByIndex.Contains(Node.Index))
        {
            OutError = FString::Printf(TEXT("duplicate glTF node index %d"), Node.Index);
            return false;
        }
        NodeByIndex.Add(Node.Index, &Node);
    }

    // Resolve model-space transforms iteratively. Parent chains are cached so large scenes remain
    // O(nodes + hierarchy edges) in the common case without risking recursion-stack overflow.
    TMap<int32, FTransform> ModelTransforms;
    ModelTransforms.Reserve(Nodes.Num());
    for (const FglTFRuntimeNode& StartNode : Nodes)
    {
        if (ModelTransforms.Contains(StartNode.Index)) continue;
        TArray<const FglTFRuntimeNode*> Chain;
        TSet<int32> ChainIds;
        const FglTFRuntimeNode* Current = &StartNode;
        FTransform ParentWorld = FTransform::Identity;
        while (Current)
        {
            if (!IsFiniteTransform(Current->Transform))
            {
                OutError = FString::Printf(TEXT("node %d has a non-finite local transform"), Current->Index);
                return false;
            }
            if (const FTransform* Cached = ModelTransforms.Find(Current->Index))
            {
                ParentWorld = *Cached;
                break;
            }
            if (ChainIds.Contains(Current->Index))
            {
                OutError = FString::Printf(TEXT("cycle detected in glTF node hierarchy at node %d"), Current->Index);
                return false;
            }
            ChainIds.Add(Current->Index);
            Chain.Add(Current);
            if (Current->ParentIndex < 0)
            {
                ParentWorld = FTransform::Identity;
                break;
            }
            const FglTFRuntimeNode* const* ParentPtr = NodeByIndex.Find(Current->ParentIndex);
            if (!ParentPtr || !*ParentPtr)
            {
                OutError = FString::Printf(TEXT("node %d references missing parent %d"), Current->Index, Current->ParentIndex);
                return false;
            }
            Current = *ParentPtr;
        }
        for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
        {
            ParentWorld = Chain[Index]->Transform * ParentWorld;
            if (!IsFiniteTransform(ParentWorld))
            {
                OutError = FString::Printf(TEXT("node %d produced an invalid model transform"), Chain[Index]->Index);
                return false;
            }
            ModelTransforms.Add(Chain[Index]->Index, ParentWorld);
        }
    }

    auto MakeUniqueStorageName = [](const FString& Preferred, int32 NodeIndex,
        const TMap<FName, FModelNodeData>& RenderMap,
        const TMap<FName, FWaterStreamNodeData>& WaterMap) -> FName
    {
        FString Base = Preferred.TrimStartAndEnd();
        if (Base.IsEmpty()) Base = FString::Printf(TEXT("Node_%d"), NodeIndex);
        FName Candidate(*Base);
        if (!RenderMap.Contains(Candidate) && !WaterMap.Contains(Candidate)) return Candidate;
        for (int32 Suffix = 0; Suffix < 1024; ++Suffix)
        {
            Candidate = FName(*FString::Printf(TEXT("%s__Node%d_%d"), *Base, NodeIndex, Suffix));
            if (!RenderMap.Contains(Candidate) && !WaterMap.Contains(Candidate)) return Candidate;
        }
        return NAME_None;
    };

    auto AssignLod = [&OutError](int32& Slot, const int32 Value, const FString& Base, const int32 Lod)
    {
        if (Slot != INDEX_NONE && Slot != Value)
        {
            OutError = FString::Printf(
                TEXT("ambiguous mesh family '%s': LOD%d resolves to multiple mesh payloads (%d/%d)"),
                *Base, Lod, Slot, Value);
            return false;
        }
        Slot = Value;
        return true;
    };

    TSet<FName> AuthoredBaseMeshNames;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        const FName BaseName(*Parsed.BaseName.TrimStartAndEnd());
        if (!BaseName.IsNone()) AuthoredBaseMeshNames.Add(BaseName);
    }

    // Explicit ;LOD0 is authoritative for the unsuffixed family name. Bare nodes with the same
    // base name are ordinary independent glTF nodes, so a distinct payload must not be forced into
    // the same LOD family. Pre-resolve explicit LOD0 rows so input node order cannot change which
    // payload keeps the canonical base name.
    TMap<FName, int32> ExplicitLod0ByBase;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        if (!RuntimeMeshPtr || *RuntimeMeshPtr < 0 || *RuntimeMeshPtr >= BakedMeshCount) continue;

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        if (Parsed.HasEffectiveToken(TEXT("INST")) || !Parsed.HasEffectiveToken(FName(TEXT("LOD0")))) continue;

        const FString BaseText = Parsed.BaseName.TrimStartAndEnd();
        const FName BaseName(*BaseText);
        if (BaseName.IsNone()) continue;

        if (const int32* Existing = ExplicitLod0ByBase.Find(BaseName))
        {
            if (*Existing != *RuntimeMeshPtr)
            {
                OutError = FString::Printf(
                    TEXT("ambiguous mesh family '%s': explicit LOD0 resolves to multiple mesh payloads (%d/%d)"),
                    *BaseText, *Existing, *RuntimeMeshPtr);
                return false;
            }
        }
        else
        {
            ExplicitLod0ByBase.Add(BaseName, *RuntimeMeshPtr);
        }
    }

    // If no explicit ;LOD0 exists, keep the lowest baked payload on the unsuffixed base name so
    // duplicate bare-node handling is deterministic even if glTF node traversal order changes.
    TMap<FName, int32> CanonicalLod0ByBase = ExplicitLod0ByBase;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        if (!RuntimeMeshPtr || *RuntimeMeshPtr < 0 || *RuntimeMeshPtr >= BakedMeshCount) continue;

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        if (Parsed.HasEffectiveToken(TEXT("INST")) || Parsed.HasEffectiveToken(FName(TEXT("LOD1")))
            || Parsed.HasEffectiveToken(FName(TEXT("LOD2")))
            || Parsed.HasEffectiveToken(FName(TEXT("LOD3"))))
        {
            continue;
        }

        const FName BaseName(*Parsed.BaseName.TrimStartAndEnd());
        if (BaseName.IsNone() || ExplicitLod0ByBase.Contains(BaseName)) continue;

        if (int32* Canonical = CanonicalLod0ByBase.Find(BaseName))
        {
            *Canonical = FMath::Min(*Canonical, *RuntimeMeshPtr);
        }
        else
        {
            CanonicalLod0ByBase.Add(BaseName, *RuntimeMeshPtr);
        }
    }

    // Generated disambiguation names inherit authored settings from their original base name.
    TMap<FName, FName> SettingsKeyByMeshFamily;

    auto MakeDistinctBareFamilyName = [&OutMetadata, &AuthoredBaseMeshNames](
        const FString& BaseText,
        const int32 RuntimeMeshIndex) -> FName
    {
        FString Stem = BaseText.TrimStartAndEnd();
        if (Stem.IsEmpty()) Stem = TEXT("Mesh");

        FName Candidate(*FString::Printf(TEXT("%s__Mesh%d"), *Stem, RuntimeMeshIndex));
        const auto CanUseCandidate = [&OutMetadata, &AuthoredBaseMeshNames, RuntimeMeshIndex](const FName Name)
        {
            if (AuthoredBaseMeshNames.Contains(Name)) return false;
            if (const FModelMeshData* Existing = OutMetadata.SceneData.MeshMap.Find(Name))
            {
                return Existing->LOD0 == INDEX_NONE || Existing->LOD0 == RuntimeMeshIndex;
            }
            return true;
        };
        if (CanUseCandidate(Candidate)) return Candidate;

        for (int32 Suffix = 1; Suffix < 1024; ++Suffix)
        {
            Candidate = FName(*FString::Printf(
                TEXT("%s__Mesh%d_%d"), *Stem, RuntimeMeshIndex, Suffix));
            if (CanUseCandidate(Candidate)) return Candidate;
        }
        return NAME_None;
    };

    TArray<const FglTFRuntimeNode*> InstancePlacements;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        const int32 RuntimeMeshIndex = RuntimeMeshPtr ? *RuntimeMeshPtr : INDEX_NONE;
        FGWorldNodeTransform& TransformRow = OutMetadata.NodeTransforms.AddDefaulted_GetRef();
        TransformRow.NodeIndex = SourceNode.Index;
        TransformRow.ParentIndex = SourceNode.ParentIndex;
        TransformRow.MeshIndex = RuntimeMeshIndex;
        TransformRow.SkinIndex = SourceNode.SkinIndex;
        TransformRow.Name = SourceNode.Name;
        TransformRow.LocalTransform = SourceNode.Transform;

        const FTransform* ModelTransform = ModelTransforms.Find(SourceNode.Index);
        if (!ModelTransform) continue;
        const FString NodeNameText = SourceNode.Name.TrimStartAndEnd();
        const bool bWater = IsWaterNode(NodeNameText);
        const bool bValidMesh = RuntimeMeshIndex >= 0 && RuntimeMeshIndex < BakedMeshCount;
        if (!bWater && !bValidMesh) continue;

        const FName StorageName = MakeUniqueStorageName(
            NodeNameText, SourceNode.Index, OutMetadata.SceneData.NodeMap, OutMetadata.SceneData.WaterNodeMap);
        if (StorageName.IsNone())
        {
            OutError = FString::Printf(TEXT("could not create a unique storage key for node %d"), SourceNode.Index);
            return false;
        }

        if (bWater)
        {
            FWaterStreamNodeData Water;
            Water.Transform = *ModelTransform;
            Water.Transform.SetScale3D(ModelTransform->GetScale3D() * 100.0f);
            const FVector AbsScale = ModelTransform->GetScale3D().GetAbs();
            Water.StreamRadius = FMath::Max(2048.0f,
                FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z) * 65536.0f);
            OutMetadata.SceneData.WaterNodeMap.Add(StorageName, MoveTemp(Water));
            continue;
        }

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(NodeNameText);
        const FString BaseText = Parsed.BaseName.TrimStartAndEnd();
        const FName BaseMeshName(*BaseText);
        if (BaseMeshName.IsNone()) continue;

        if (Parsed.HasEffectiveToken(TEXT("INST")))
        {
            InstancePlacements.Add(&SourceNode);
            continue;
        }
        FName MeshName = BaseMeshName;
        FModelMeshData* Mesh = &OutMetadata.SceneData.MeshMap.FindOrAdd(MeshName);
        SettingsKeyByMeshFamily.FindOrAdd(MeshName) = BaseMeshName;

        const bool bNoCollision = Parsed.HasEffectiveToken(FName(TEXT("NCOL")));
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD1"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD1, RuntimeMeshIndex, BaseText, 1)) return false;
            continue;
        }
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD2"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD2, RuntimeMeshIndex, BaseText, 2)) return false;
            continue;
        }
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD3"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD3, RuntimeMeshIndex, BaseText, 3)) return false;
            continue;
        }

        const FVector* MeshSize = SourceMeshSizes.Find(RuntimeMeshIndex);
        if (!MeshSize || !IsFiniteVector(*MeshSize) || MeshSize->GetMin() < 0.0) continue;

        const bool bExplicitLod0 = Parsed.HasEffectiveToken(FName(TEXT("LOD0")));
        const int32* CanonicalLod0 = CanonicalLod0ByBase.Find(BaseMeshName);
        const bool bBareUsesDistinctPayload =
            !bExplicitLod0 && CanonicalLod0 && *CanonicalLod0 != RuntimeMeshIndex;
        const bool bBareCollidesWithExisting =
            !bExplicitLod0 && Mesh->LOD0 != INDEX_NONE && Mesh->LOD0 != RuntimeMeshIndex;

        if (bBareUsesDistinctPayload || bBareCollidesWithExisting)
        {
            MeshName = MakeDistinctBareFamilyName(BaseText, RuntimeMeshIndex);
            if (MeshName.IsNone())
            {
                OutError = FString::Printf(
                    TEXT("could not disambiguate duplicate bare mesh family '%s' for payload %d"),
                    *BaseText, RuntimeMeshIndex);
                return false;
            }

            Mesh = &OutMetadata.SceneData.MeshMap.FindOrAdd(MeshName);
            SettingsKeyByMeshFamily.FindOrAdd(MeshName) = BaseMeshName;
            UE_LOG(LogTemp, Display,
                TEXT("World build split duplicate bare mesh family: base=%s node=%d payload=%d family=%s"),
                *BaseText, SourceNode.Index, RuntimeMeshIndex, *MeshName.ToString());
        }

        if (bNoCollision)
        {
            Mesh->Data.bComplexCollision = false;
            Mesh->Data.bSimpleCollision = false;
        }
        if (!AssignLod(Mesh->LOD0, RuntimeMeshIndex, MeshName.ToString(), 0)) return false;
        Mesh->Size = *MeshSize;
        Mesh->Extent = *MeshSize * 0.5f;

        FModelNodeData Node;
        Node.MeshName = MeshName;
        Node.Transform = *ModelTransform;
        OutMetadata.SceneData.NodeMap.Add(StorageName, MoveTemp(Node));
    }

    // A family with only LOD1/2/3 still has geometry. Use its most detailed available level as
    // the placement source; lower-detail siblings describe alternate geometry, not extra instances.
    TMap<FName, int32> FallbackPlacementLods;
    for (auto& Pair : OutMetadata.SceneData.MeshMap)
    {
        FModelMeshData& Mesh = Pair.Value;
        if (Mesh.LOD0 == INDEX_NONE)
        {
            const int32 Lod = Mesh.LOD1 != INDEX_NONE ? 1 : Mesh.LOD2 != INDEX_NONE ? 2 : 3;
            int32& Source = Lod == 1 ? Mesh.LOD1 : Lod == 2 ? Mesh.LOD2 : Mesh.LOD3;
            if (Source != INDEX_NONE)
            {
                FallbackPlacementLods.Add(Pair.Key, Lod);
                Mesh.LOD0 = Source;
                Source = INDEX_NONE;
            }
        }
        // Culling must enclose all selectable LODs, including off-pivot lower-detail geometry.
        for (const int32 Index : { Mesh.LOD0, Mesh.LOD1, Mesh.LOD2, Mesh.LOD3 })
            if (const FVector* Size = SourceMeshSizes.Find(Index))
                Mesh.Size = Mesh.Size.ComponentMax(*Size);
        Mesh.Extent = Mesh.Size * 0.5;
    }
    if (!FallbackPlacementLods.IsEmpty())
    {
        for (const FglTFRuntimeNode& SourceNode : Nodes)
        {
            if (IsWaterNode(SourceNode.Name)) continue;
            const auto Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
            if (Parsed.HasEffectiveToken(TEXT("INST"))) continue;
            const FName MeshName(*Parsed.BaseName.TrimStartAndEnd());
            const int32* Lod = FallbackPlacementLods.Find(MeshName);
            if (!Lod || !Parsed.HasEffectiveToken(FName(*FString::Printf(TEXT("LOD%d"), *Lod)))) continue;
            const int32* Payload = NodeToBakedMeshIndex.Find(SourceNode.Index);
            const FTransform* Transform = ModelTransforms.Find(SourceNode.Index);
            if (!Payload || !Transform || !SourceMeshSizes.Contains(*Payload)) continue;
            const FName StorageName = MakeUniqueStorageName(SourceNode.Name, SourceNode.Index,
                OutMetadata.SceneData.NodeMap, OutMetadata.SceneData.WaterNodeMap);
            if (StorageName.IsNone())
            {
                OutError = FString::Printf(TEXT("could not create a fallback placement for node %d"), SourceNode.Index);
                return false;
            }
            FModelNodeData Node;
            Node.MeshName = MeshName;
            Node.Transform = *Transform;
            OutMetadata.SceneData.NodeMap.Add(StorageName, MoveTemp(Node));
        }
    }

    // Store only an instance transform and a family reference. Instance-only families retain
    // one canonical source as LOD0; accompanying ;LOD tokens never redefine a family's LODs.
    for (const FglTFRuntimeNode* SourceNode : InstancePlacements)
    {
        const int32 RuntimeIndex = NodeToBakedMeshIndex.FindChecked(SourceNode->Index);
        const FName BaseName(*USimulatorNodeTokenLibrary::GetBaseNodeName(SourceNode->Name).TrimStartAndEnd());
        FName MeshName = BaseName;
        FModelMeshData* Mesh = OutMetadata.SceneData.MeshMap.Find(MeshName);
        if (Mesh && Mesh->LOD0 != RuntimeIndex && Mesh->LOD1 != RuntimeIndex
            && Mesh->LOD2 != RuntimeIndex && Mesh->LOD3 != RuntimeIndex)
        {
            // Opaque extension decoders/other skins cannot safely share a source payload.
            MeshName = MakeDistinctBareFamilyName(BaseName.ToString(), RuntimeIndex);
            if (MeshName.IsNone()) { OutError = TEXT("could not name an isolated instance family"); return false; }
            Mesh = OutMetadata.SceneData.MeshMap.Find(MeshName);
        }
        if (!Mesh)
        {
            Mesh = &OutMetadata.SceneData.MeshMap.Add(MeshName);
            Mesh->LOD0 = RuntimeIndex;
            Mesh->Size = SourceMeshSizes.FindChecked(RuntimeIndex);
            Mesh->Extent = Mesh->Size * 0.5f;
            SettingsKeyByMeshFamily.Add(MeshName, BaseName);
        }
        const FName StorageName = MakeUniqueStorageName(SourceNode->Name, SourceNode->Index,
            OutMetadata.SceneData.NodeMap, OutMetadata.SceneData.WaterNodeMap);
        if (StorageName.IsNone()) { OutError = TEXT("could not name an instance placement"); return false; }
        FModelNodeData Node;
        Node.MeshName = MeshName;
        Node.Transform = ModelTransforms.FindChecked(SourceNode->Index);
        OutMetadata.SceneData.NodeMap.Add(StorageName, MoveTemp(Node));
    }

    // JSON settings override only author-controlled behavior. Generated split families inherit the
    // authored settings of their original base name unless an exact generated-family key exists.
    for (auto It = OutMetadata.SceneData.MeshMap.CreateIterator(); It; ++It)
    {
        FModelMeshData& Mesh = It.Value();
        if (Mesh.LOD0 == INDEX_NONE || !IsFiniteVector(Mesh.Extent) || !IsFiniteVector(Mesh.Size)
            || Mesh.Extent.GetMin() < 0.0 || Mesh.Size.GetMin() < 0.0)
        {
            It.RemoveCurrent();
            continue;
        }

        const FMeshData* Settings = AuthoredSettings.MeshData.Find(It.Key());
        if (!Settings)
        {
            if (const FName* BaseSettingsKey = SettingsKeyByMeshFamily.Find(It.Key()))
            {
                Settings = AuthoredSettings.MeshData.Find(*BaseSettingsKey);
            }
        }
        if (Settings) Mesh.Data = *Settings;
    }
    for (auto It = OutMetadata.SceneData.NodeMap.CreateIterator(); It; ++It)
    {
        FModelNodeData& Node = It.Value();
        const FModelMeshData* Mesh = OutMetadata.SceneData.MeshMap.Find(Node.MeshName);
        if (!Mesh) { It.RemoveCurrent(); continue; }
        const bool bChunkAddressable = ComputeSpatialChunk(Node.Transform.GetLocation(), Node.CoarseChunk, Node.FineChunk);
        const FBox Bounds = FBox(-Mesh->Extent, Mesh->Extent).TransformBy(Node.Transform);
        const FVector Size = Bounds.IsValid ? Bounds.GetSize() : FVector::ZeroVector;
        Node.bAlwaysLoaded = !bChunkAddressable || !Bounds.IsValid || !IsFiniteVector(Size)
            || Size.GetMax() > CoarseChunkCm;
    }

    TSet<FName> ReferencedMeshes;
    for (const TPair<FName, FModelNodeData>& Pair : OutMetadata.SceneData.NodeMap) ReferencedMeshes.Add(Pair.Value.MeshName);
    for (auto It = OutMetadata.SceneData.MeshMap.CreateIterator(); It; ++It)
        if (!ReferencedMeshes.Contains(It.Key())) It.RemoveCurrent();

    AuthoredSettings.Center = FVector::ZeroVector;
    AuthoredSettings.Size = FVector::ZeroVector;
    FBox WorldBounds(ForceInit);
    for (const TPair<FName, FModelNodeData>& Pair : OutMetadata.SceneData.NodeMap)
    {
        const FModelMeshData* Mesh = OutMetadata.SceneData.MeshMap.Find(Pair.Value.MeshName);
        if (!Mesh) continue;
        const FVector Extent = Mesh->Extent.GetAbs();
        WorldBounds += FBox(-Extent, Extent).TransformBy(Pair.Value.Transform);
    }
    for (const TPair<FName, FWaterStreamNodeData>& Pair : OutMetadata.SceneData.WaterNodeMap)
    {
        // StreamRadius is already expressed in model-space centimetres and includes the authored
        // node scale. Transforming an extent box by Water.Transform would apply that scale a
        // second time and can inflate world bounds by orders of magnitude.
        WorldBounds += FBox::BuildAABB(Pair.Value.Transform.GetLocation(), FVector(Pair.Value.StreamRadius));
    }
    if (WorldBounds.IsValid)
    {
        AuthoredSettings.Center = WorldBounds.GetCenter();
        AuthoredSettings.Size = WorldBounds.GetSize();
    }
    // Keep complete source-node rows deterministic for byte-stable metadata and easier corruption diagnostics.
    OutMetadata.NodeTransforms.Sort([](const FGWorldNodeTransform& A, const FGWorldNodeTransform& B)
    {
        return A.NodeIndex < B.NodeIndex;
    });
    OutMetadata.SceneData.ModelData = MoveTemp(AuthoredSettings);
    OutMetadata.SceneData.bSuccess = true;
    return true;
}

void UProjectSourceModelBuilder::Complete(
    bool bSuccess,
    const FString& Error)
{
    check(IsInGameThread());
    if (bCompletionSent)
    {
        return;
    }

    if (bSuccess)
    {
        const int64 CurrentSize =
            IFileManager::Get().FileSize(*Definition.GlbPath);
        const FDateTime CurrentTimestamp =
            IFileManager::Get().GetTimeStamp(*Definition.GlbPath);
        if (CurrentSize != SourceFileSize
            || (SourceTimestamp != FDateTime()
                && CurrentTimestamp != SourceTimestamp))
        {
            bSuccess = false;
            FailureReason = TEXT("source model changed while it was being decoded");
        }
    }
    if (!bSuccess)
    {
        FailureReason = FailureReason.IsEmpty()
            ? (Error.IsEmpty() ? TEXT("source model build failed") : Error)
            : FailureReason;
        CompletedModel = FGWorldBuildModel();
        BakedData.Reset();
        FString ExistingQuarantineReason;
        if (bRunning && !FV3DRuntimeSafety::IsPathQuarantined(
                Definition.GlbPath, &ExistingQuarantineReason))
        {
            FV3DRuntimeSafety::ReportRecoverableFailure(
                Definition.GlbPath, FailureReason);
        }
        FSimulatorFileServices::WriteLogAsync(
            TEXT("ProjectSourceModelBuilder"),
            FString::Printf(TEXT("Build failed. Source=%s Reason=%s"),
                *Definition.GlbPath, *FailureReason));
    }

    if (bSuccess)
    {
        FV3DRuntimeSafety::ClearRecoverableFailure(Definition.GlbPath);
    }

    bRunning = false;
    bCompletionSent = true;
    ProgressValue = 1.0f;
    ReleaseSourceAsset();
    OnFinished.Broadcast(this, bSuccess);
}

void UProjectSourceModelBuilder::ReleaseSourceAsset()
{
    check(IsInGameThread());
    if (IsValid(SourceAsset))
    {
        FV3DRuntimeSafety::RequestAssetRelease(SourceAsset);
    }
    SourceAsset = nullptr;
    ImportSession.Reset();
    MaterialIds.Reset();
    TextureIds.Reset();
    SourceMeshNames.Reset();
    SourceToBakedMeshIndex.Reset();
    NodeToBakedMeshIndex.Reset();
    SourceMeshesToCapture.Reset();
    SourceMeshSizes.Reset();
    SkinMeshIndices.Reset();
}

FGWorldBuildModel UProjectSourceModelBuilder::TakeBuildModel()
{
    check(IsInGameThread());
    return MoveTemp(CompletedModel);
}

void UProjectSourceModelBuilder::Cancel()
{
    check(IsInGameThread());
    if (bCancelled)
    {
        return;
    }
    bCancelled = true;
    bRunning = false;
    if (ParserRetryTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ParserRetryTicker);
        ParserRetryTicker.Reset();
    }
    if (NextMeshTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(NextMeshTicker);
        NextMeshTicker.Reset();
    }
    CompletedModel = FGWorldBuildModel();
    BakedData.Reset();
    ++RequestGeneration;
    FV3DRuntimeSafety::CancelQueuedOperations(this);
    ReleaseSourceAsset();
    OnFinished.Clear();
}

void UProjectSourceModelBuilder::BeginDestroy()
{
    if (IsInGameThread())
    {
        Cancel();
    }
    Super::BeginDestroy();
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSourceStreamingMetadataTest,
    "V3DSimulator.Streaming.Regression.SourceMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSourceStreamingMetadataTest::RunTest(const FString&)
{
    TStrongObjectPtr<UProjectSourceModelBuilder> Builder(NewObject<UProjectSourceModelBuilder>());
    FString Error;
    FglTFRuntimeMeshLOD OffsetLOD;
    auto& Primitive = OffsetLOD.Primitives.AddDefaulted_GetRef();
    Primitive.Positions = { FVector(1000000, -40, -10), FVector(1000100, 80, 30) };
    if (!TestTrue(TEXT("Capture off-pivot geometry"), Builder->CaptureMeshBounds(0, OffsetLOD, Error))) return false;
    TestTrue(TEXT("Stored size encloses the pivot offset, not just the vertex span"),
        Builder->SourceMeshSizes.FindChecked(0).Equals(FVector(2000200, 160, 60)));

    FglTFRuntimeMeshLOD NegativeLOD;
    NegativeLOD.Primitives.AddDefaulted_GetRef().Positions = { FVector(-2000, -10, -5), FVector(-1000, 10, 5) };
    if (!TestTrue(TEXT("Capture negative offsets"), Builder->CaptureMeshBounds(1, NegativeLOD, Error))) return false;
    TestEqual(TEXT("Negative side is enclosed"), Builder->SourceMeshSizes.FindChecked(1).X, 4000.0);
    NegativeLOD.AdditionalTransforms.Add(FTransform(FVector(4000, 0, 0)));
    if (!TestTrue(TEXT("Capture baked additional transforms"), Builder->CaptureMeshBounds(2, NegativeLOD, Error))) return false;
    TestEqual(TEXT("Additional transform remains in the stored bounds"), Builder->SourceMeshSizes.FindChecked(2).X, 6000.0);

    Builder->DefinitionJson = TEXT("{}");
    Builder->ImportSession = MakeShared<V3DSourceModelImport::FSession>(V3DSourceModelImport::EFormat::Glb);
    Builder->BakedData.Meshes.SetNum(6);
    Builder->SourceMeshSizes.Add(3, FVector(400));
    Builder->SourceMeshSizes.Add(4, FVector(100));
    Builder->SourceMeshSizes.Add(5, FVector(1800000, 100, 100));
    const auto AddNode = [&](const TCHAR* Name, int32 Payload, const FVector& Location)
    {
        const int32 Index = Builder->ImportSession->Nodes.Num();
        auto& Node = Builder->ImportSession->Nodes.AddDefaulted_GetRef();
        Node.Index = Index;
        Node.ParentIndex = INDEX_NONE;
        Node.MeshIndex = Payload;
        Node.Name = Name;
        Node.Transform = FTransform(Location);
        Builder->NodeToBakedMeshIndex.Add(Index, Payload);
    };
    AddNode(TEXT("Offset"), 0, FVector(123, 0, 0));
    AddNode(TEXT("Fallback;LOD1"), 1, FVector(1000, 0, 0));
    AddNode(TEXT("Fallback;LOD1;INST"), 1, FVector(2000, 0, 0));
    AddNode(TEXT("Fallback;LOD2"), 3, FVector(1000, 0, 0));
    AddNode(TEXT("Full;LOD0"), 4, FVector(0, 2000, 0));
    AddNode(TEXT("Full;LOD2"), 5, FVector(0, 2000, 0));
    AddNode(TEXT("Full;INSTwhatever"), 4, FVector(500, 2000, 0));
    AddNode(TEXT("Full;LOD3;INST arbitrary"), 4, FVector(1000, 2000, 0));
    AddNode(TEXT("Twin"), 1, FVector(100, 100, 0));
    AddNode(TEXT("Twin"), 2, FVector(200, 100, 0));
    AddNode(TEXT("OnlyThird;LOD3"), 3, FVector(3000, 0, 0));
    FGWorldModelMetadata Metadata;
    if (!TestTrue(TEXT("Build streaming metadata from source rows"), Builder->BuildMetadata(Metadata, Error)))
    { AddError(Error); return false; }
    TestEqual(TEXT("All placements survive without spawning lower-detail siblings"), Metadata.SceneData.NodeMap.Num(), 9);
    const FModelMeshData* Fallback = Metadata.SceneData.MeshMap.Find(TEXT("Fallback"));
    if (!TestNotNull(TEXT("LOD1-only family is retained"), Fallback)) return false;
    TestTrue(TEXT("Best available LOD is promoted once, other LODs retained"),
        Fallback->LOD0 == 1 && Fallback->LOD1 == INDEX_NONE && Fallback->LOD2 == 3);
    TestEqual(TEXT("INST never creates a new LOD definition"), Metadata.SceneData.MeshMap.FindChecked(TEXT("Full")).LOD3, INDEX_NONE);
    TestTrue(TEXT("INST arbitrary suffix stores its own transform"), Metadata.SceneData.NodeMap.FindChecked(TEXT("Full;INSTwhatever")).Transform.GetLocation().Equals(FVector(500, 2000, 0)));
    TestEqual(TEXT("LOD3-only family is retained"), Metadata.SceneData.MeshMap.FindChecked(TEXT("OnlyThird")).LOD0, 3);
    TestTrue(TEXT("LOD0 bounds enclose lower-detail geometry too"),
        Metadata.SceneData.MeshMap.FindChecked(TEXT("Full")).Size.X == 1800000.0);
    TestTrue(TEXT("Large alternate LOD updates cell admission metadata"),
        Metadata.SceneData.NodeMap.FindChecked(TEXT("Full;LOD0")).bAlwaysLoaded);
    const FBox ModelBounds = FBox::BuildAABB(Metadata.SceneData.ModelData.Center, Metadata.SceneData.ModelData.Size * 0.5);
    for (const FVector& Position : Primitive.Positions)
        TestTrue(TEXT("Coarse scene bounds contain actual off-pivot geometry"),
            ModelBounds.IsInsideOrOn(Position + FVector(123, 0, 0)));
    TestTrue(TEXT("Bare nodes with distinct payloads still get separate families"),
        Metadata.SceneData.MeshMap.Contains(TEXT("Twin")) && Metadata.SceneData.MeshMap.Contains(TEXT("Twin__Mesh2")));

    auto& Nodes = Builder->ImportSession->Nodes;
    for (int32 Left = 0, Right = Nodes.Num() - 1; Left < Right; ++Left, --Right) Swap(Nodes[Left], Nodes[Right]);
    FGWorldModelMetadata Reversed;
    if (!TestTrue(TEXT("Reversed source order also builds"), Builder->BuildMetadata(Reversed, Error))) return false;
    TestEqual(TEXT("Fallback placements are independent of traversal order"), Reversed.SceneData.NodeMap.Num(), Metadata.SceneData.NodeMap.Num());
    TestEqual(TEXT("Fallback payload is independent of traversal order"), Reversed.SceneData.MeshMap.FindChecked(TEXT("Fallback")).LOD0, Fallback->LOD0);
    AddNode(TEXT("Full;LOD0"), 3, FVector::ZeroVector);
    TestFalse(TEXT("Conflicting explicit LOD0 payloads remain rejected"), Builder->BuildMetadata(Reversed, Error));
    TestTrue(TEXT("Conflict reports its cause"), Error.Contains(TEXT("ambiguous mesh family")));
    return true;
}
#endif
