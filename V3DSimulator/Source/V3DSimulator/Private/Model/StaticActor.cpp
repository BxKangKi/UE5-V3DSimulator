// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StaticActor.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/StaticActor.h"
#include "System/V3DStreamingPolicy.h"
#include "System/V3DStreamingBudget.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformTime.h"
#include "Gravity/GravityFieldComponent.h"

#include "Async/ParallelFor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Model/InstancedMeshActor.h"
#include "Model/V3DHISMChunkPolicy.h"
#include "Model/WorldSceneStreamAction.h"
#include "Model/WorldSceneStreamingSubsystem.h"
#include "Model/V3DMaterialOverrideUtils.h"
#include "Setting/GameSettings.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "Simulator/ModelDefinitionJson.h"
#include "Simulator/ModelDefinitionTypes.h"
#include "Simulator/RuntimeModelResolver.h"
#include "System/ActorHelper.h"
#include "System/SimulatorFileServices.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/SafeFileIO.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedModelAsset.h"
#include "HAL/PlatformFileManager.h"
#include "System/V3DTerrain.h"
#include "System/WorldArchiveCodec.h"
#include "System/WorldSurfaceSubsystem.h"
#include "System/V3DRuntimeSafety.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "TimerManager.h"
#include "World/WaterActor.h"
#include "UObject/UObjectGlobals.h"

namespace StaticActorPrivate
{
    TArray<FString> ExtractTerrainLayerNamesFromArchive(
        const FString& ArchivePath,
        const FGWorldModelManifest& Manifest)
    {
        TArray<FString> LayerNames;
        if (ArchivePath.IsEmpty() || Manifest.MaterialRanges.IsEmpty()) return LayerNames;

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*ArchivePath));
        if (!Handle) return LayerNames;

        TMap<int32, FString> NamesById;
        NamesById.Reserve(Manifest.MaterialRanges.Num());
        TArray<FString> FallbackTerrainNames;
        TMap<int32, int32> PaletteSourceByLayer;
        bool bHasPalette = false;
        TArray<uint8> Payload;

        for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.MaterialRanges)
        {
            const FGWorldArchiveRange& Range = Pair.Value;
            if (Range.Codec != 0 || Range.StoredSize == 0
                || Range.StoredSize != Range.UncompressedSize
                || Range.StoredSize > 16ull * 1024ull * 1024ull)
            {
                continue;
            }
            if (!Handle->Seek(static_cast<int64>(Range.Offset))) continue;
            Payload.SetNumUninitialized(static_cast<int32>(Range.StoredSize), EAllowShrinking::No);
            if (!Handle->Read(Payload.GetData(), static_cast<int64>(Range.StoredSize))) continue;

            FGWorldBakedMaterial Mat;
            FString Err;
            if (!WorldArchiveCodec::DeserializeMaterial(Payload, Mat, Err)) continue;

            NamesById.Add(Mat.MaterialId, Mat.Name);
            if (V3DTerrain::IsTerrainName(Mat.Name))
            {
                FallbackTerrainNames.AddUnique(Mat.Name);
            }
            if (V3DTerrain::IsPaletteMaterial(Mat)
                || Mat.Name.Equals(TEXT("V3D.TerrainPalette"), ESearchCase::IgnoreCase))
            {
                bHasPalette = true;
                for (const FGWorldBakedScalarParameter& Scalar : Mat.Scalars)
                {
                    if (Scalar.Name.StartsWith(TEXT("V3D_SourceMaterial")))
                    {
                        const FString Suffix = Scalar.Name.Mid(18);
                        if (Suffix.IsNumeric())
                        {
                            const int32 LayerIdx = FCString::Atoi(*Suffix);
                            if (LayerIdx >= 0 && LayerIdx < 4)
                            {
                                PaletteSourceByLayer.Add(LayerIdx, FMath::RoundToInt(Scalar.Value));
                            }
                        }
                    }
                }
            }
        }

        if (!bHasPalette && FallbackTerrainNames.IsEmpty()) return LayerNames;

        if (!PaletteSourceByLayer.IsEmpty())
        {
            LayerNames.SetNum(4);
            int32 HighestValid = -1;
            for (int32 LayerIdx = 0; LayerIdx < 4; ++LayerIdx)
            {
                if (const int32* SrcId = PaletteSourceByLayer.Find(LayerIdx))
                {
                    if (const FString* SrcName = NamesById.Find(*SrcId))
                    {
                        LayerNames[LayerIdx] = *SrcName;
                        HighestValid = LayerIdx;
                    }
                }
            }
            if (HighestValid >= 0)
            {
                LayerNames.SetNum(HighestValid + 1);
                return LayerNames;
            }
            LayerNames.Reset();
        }

        FallbackTerrainNames.Sort();
        if (FallbackTerrainNames.Num() > 4) FallbackTerrainNames.SetNum(4);
        return FallbackTerrainNames;
    }

    struct FStaticPreparedGroup
    {
        FInstancedMeshGroupInitData Data;
        TSet<FName> MeshNames;
    };

    struct FStaticGroupingWork
    {
        TMap<FName, FModelNodeData> Nodes;
        TMap<FName, FName> MeshToGroup;
        TMap<FName, FVector> MeshSizes;
        FTransform OwnerTransform = FTransform::Identity;
        TMap<FName, FInstancedMeshGroupInitData> Groups;
        int32 InvalidNodeCount = 0;
        TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> Cancellation;
    };

    void BuildStaticGroups(FStaticGroupingWork& Work)
    {
        TMap<FName, FStaticPreparedGroup> PreparedGroups;
        PreparedGroups.Reserve(Work.MeshToGroup.Num());
        Work.Groups.Reset();
        Work.InvalidNodeCount = 0;

        const FTransform InverseOwnerTransform = Work.OwnerTransform.Inverse();
        uint32 Examined = 0;
        for (TPair<FName, FModelNodeData>& Pair : Work.Nodes)
        {
            if ((Examined++ & 255) == 0 && Work.Cancellation.IsValid()
                && Work.Cancellation->GetValue() != 0)
            {
                // Partial groups are worker-owned and die here; nothing is published to a new world.
                Work.Groups.Empty();
                return;
            }
            const FModelNodeData& Node = Pair.Value;
            const FName* GroupName = Work.MeshToGroup.Find(Node.MeshName);
            if (Pair.Key.IsNone()
                || Node.MeshName.IsNone()
                || Node.Transform.ContainsNaN()
                || !GroupName)
            {
                ++Work.InvalidNodeCount;
                continue;
            }

            FStaticPreparedGroup& Group = PreparedGroups.FindOrAdd(*GroupName);
            Group.MeshNames.Add(Node.MeshName);
            Group.Data.NodeBounds += Node.Transform.GetLocation();
            Group.Data.MaxNodeScale = FMath::Max(Group.Data.MaxNodeScale,
                static_cast<double>(Node.Transform.GetScale3D().GetAbs().GetMax()));
            if (Node.bAlwaysLoaded) Group.Data.AlwaysLoadedNodeNames.Add(Pair.Key);
            const FIntVector ChunkKey = V3DHISMChunkPolicy::Key(
                Work.OwnerTransform.TransformPosition(Node.Transform.GetLocation()));
            FInstancedMeshStreamChunk& Chunk = Group.Data.StreamChunks.FindOrAdd(ChunkKey);
            Chunk.Nodes.Add(Pair.Key);
            Chunk.bHasAlwaysLoadedNodes |= Node.bAlwaysLoaded;
            // Include the full world-space cell, transformed back for the actor's immutable index.
            Chunk.LocalBounds += V3DHISMChunkPolicy::CellBounds(ChunkKey).TransformBy(InverseOwnerTransform);
            const FVector Half = Work.MeshSizes.FindChecked(Node.MeshName).GetAbs() * 0.5;
            Chunk.LocalBounds += FBox(-Half, Half).TransformBy(Node.Transform);
            Group.Data.Nodes.Add(Pair.Key, MoveTemp(Pair.Value));
        }

        Work.Groups.Reserve(PreparedGroups.Num());
        for (TPair<FName, FStaticPreparedGroup>& Pair : PreparedGroups)
        {
            FInstancedMeshGroupInitData& Data = Pair.Value.Data;
            Data.ReferencedMeshNames = Pair.Value.MeshNames.Array();
            Data.ReferencedMeshNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
            Data.AlwaysLoadedNodeNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
            Work.Groups.Add(Pair.Key, MoveTemp(Data));
        }
        Work.Nodes.Empty();
        Work.MeshToGroup.Empty();
        Work.MeshSizes.Empty();
    }

    bool EnsureGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(
            IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    FName MakeMeshGroupName(const FModelMeshData& Mesh)
    {
        return FName(*FString::Printf(
            TEXT("Mesh_%d_%d_%d_%d_C%d_S%d"),
            Mesh.LOD0,
            Mesh.LOD1,
            Mesh.LOD2,
            Mesh.LOD3,
            Mesh.Data.bComplexCollision ? 1 : 0,
            Mesh.Data.bSimpleCollision ? 1 : 0));
    }

    FString NormalizeReference(const FString& Reference)
    {
        FGuid UUID;
        return FGWorldArchive::ParseModelReference(Reference, UUID)
            ? FGWorldArchive::MakeModelReference(UUID)
            : FString();
    }

    bool ResolveScene(
        UObject* Context,
        const FString& Reference,
        FGuid& OutUUID,
        FString& OutCanonicalReference,
        FString& OutReason)
    {
        const UWorld* World = IsValid(Context) ? Context->GetWorld() : nullptr;
        const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
        const UModelDatabaseSubsystem* Database = GameInstance
            ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>()
            : nullptr;
        FModelDefinition Definition;
        if (!Database
            || !Database->IsBuiltWorld()
            || !Database->FindUUIDForReference(Reference, OutUUID)
            || !Database->ResolveLoadable(
                OutUUID, Definition, OutCanonicalReference))
        {
            OutReason = TEXT("model is absent from the verified .v3d directory");
            return false;
        }
        if (Definition.ModelType != EModelDefinitionType::Static)
        {
            OutReason = FString::Printf(
                TEXT("world scene requires ModelType=Static; got %s"),
                *V3DSimulatorModelTypes::ToString(Definition.ModelType));
            return false;
        }
        if (NormalizeReference(OutCanonicalReference).IsEmpty())
        {
            OutReason = TEXT("model database returned a non-v3d runtime reference");
            return false;
        }
        return true;
    }
}

AStaticActor::AStaticActor()
{
    GravityField = CreateDefaultSubobject<UGravityFieldComponent>(TEXT("GravityField"));
    PrimaryActorTick.bCanEverTick = false;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);
}

void AStaticActor::Init(const FString& InModelReference)
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::Init")))
    {
        return;
    }

    const bool bRestart = HasActorBegunPlay();
    if (bRestart)
    {
        ReleaseRuntimeResourcesForWorldExit();
    }

    ModelReference = StaticActorPrivate::NormalizeReference(
        InModelReference);
    bRuntimeResourcesReleased = false;
    bIsDestroyed = false;
    StreamingRetry.Reset();
    bReloadMetadataOnRetry = false;
    if (bRestart)
    {
        bIsLoaded = false;
        bAsyncLoading = false;
        LoadingStatus = 0.0f;
        StartBuiltLoad();
    }
}


bool AStaticActor::LoadStatic(const FString& InModelReference, const FString& InObjectName)
{
    check(IsInGameThread());
    FResolvedRuntimeModel Resolved;
    FString Error;
    if (!FRuntimeModelResolver::Resolve(this, InModelReference, Resolved, Error)
        || Resolved.Definition.ModelType != EModelDefinitionType::Static)
    {
        UE_LOG(LogTemp, Error, TEXT("StaticActor rejected model reference '%s': %s"),
            *InModelReference, Error.IsEmpty() ? TEXT("ModelType must be Static") : *Error);
        return false;
    }

    ObjectName = InObjectName;
    BaseName = Resolved.Definition.Name;
    Init(Resolved.Reference);
    return !ModelReference.IsEmpty();
}
void AStaticActor::SetRenderOnlyStreaming(const bool bRenderOnly)
{
    if (StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::SetRenderOnlyStreaming")))
    {
        bRenderOnlyStreaming = bRenderOnly;
    }
}

void AStaticActor::RequestStreamingRefresh()
{
    if (!StaticActorPrivate::EnsureGameThread(TEXT("AStaticActor::RequestStreamingRefresh"))
        || bIsDestroyed
        || !bHasModelMetadata
        || !IsValid(BakedAsset))
    {
        // A newly spawned coarse scene may still be reading metadata/preparing its facade. Its
        // existing bIsLoaded=false state already keeps destination readiness blocked; do not run
        // StartStreamingStep early and accidentally treat an unfinished empty group set as ready.
        return;
    }
    StreamingEvaluation.Invalidate();
    bIsLoaded = false;
    NextStreamingUpdateSeconds = 0.0;
    StartStreamingStep();
}

bool AStaticActor::IsLocationStreamingReady(const FVector& WorldLocation) const
{
    check(IsInGameThread());
    if (bIsDestroyed || !bHasModelMetadata || !IsValid(BakedAsset)
        || bReloadMetadataOnRetry || !PendingInstancedGroupNames.IsEmpty()
        || (bAsyncLoading && !StreamingEvaluation.HasSnapshot())
        || WorldLocation.ContainsNaN())
    {
        return false;
    }

    const float EffectiveStreamDistance = V3DStreamingPolicy::GetScreenSizeDistance(this);
    const double MaxDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(this);
    const FTransform OwnerTransform = GetActorTransform();

    for (const TPair<FName, TObjectPtr<AInstancedMeshActor>>& GroupPair : OwnedInstancedMeshActors)
    {
        const AInstancedMeshActor* Group = GroupPair.Value.Get();
        if (!IsValid(Group) || !Group->AreChunkTreesReady()) return false;
        if (IsMeshGroupUnavailable(GroupPair.Key)) continue;

        float MaxWorldRadius = static_cast<float>(FMath::Min(MaxDistanceCm, V3DHISMChunkPolicy::ChunkSizeCm));
        for (const FName MeshName : Group->GetReferencedMeshNames())
        {
            const FModelMeshData* MeshData = AllMeshMap.Find(MeshName);
            if (!MeshData) return false;
            MaxWorldRadius = FMath::Max(MaxWorldRadius, float(V3DStreamingPolicy::LoadRadius(
                MeshData->Size, Group->GetMaxNodeScale()
                    * OwnerTransform.GetScale3D().GetAbs().GetMax(), EffectiveStreamDistance, MaxDistanceCm)));
        }

        const auto Definitions = Group->CaptureDefinitions();
        if (!Definitions.IsValid()) return false;
        // Read immutable rows in place: destination readiness must not copy dense chunk tables.
        for (const auto& ChunkPair : Definitions->StreamChunks)
        {
            const FBox Bounds = ChunkPair.Value.LocalBounds.TransformBy(OwnerTransform);
            const double QueryRadius = ChunkPair.Value.bHasAlwaysLoadedNodes
                ? FMath::Max(double(MaxWorldRadius), MaxDistanceCm) : double(MaxWorldRadius);
            if (Bounds.ComputeSquaredDistanceToPoint(WorldLocation) > FMath::Square(QueryRadius)) continue;
            double ChunkRadius = FMath::Min(MaxDistanceCm, V3DHISMChunkPolicy::ChunkSizeCm);
            for (const FName NodeName : ChunkPair.Value.Nodes)
            {
                const FModelNodeData& Node = Definitions->Nodes.FindChecked(NodeName);
                const FModelMeshData* Mesh = AllMeshMap.Find(Node.MeshName);
                if (!Mesh) return false;
                const FTransform Transform = Node.Transform * OwnerTransform;
                ChunkRadius = FMath::Max(ChunkRadius, Node.bAlwaysLoaded ? MaxDistanceCm
                    : V3DStreamingPolicy::LoadRadius(Mesh->Size, Transform.GetScale3D().GetAbs().GetMax(),
                        EffectiveStreamDistance, MaxDistanceCm));
            }
            if (Bounds.ComputeSquaredDistanceToPoint(WorldLocation) > FMath::Square(FMath::Min(MaxDistanceCm, ChunkRadius))) continue;
            for (const FName NodeName : ChunkPair.Value.Nodes)
            {
                if (Group->IsNodeLoaded(NodeName)) continue;
                const FModelNodeData& Node = Definitions->Nodes.FindChecked(NodeName);
                const FModelMeshData* Mesh = AllMeshMap.Find(Node.MeshName);
                if (!Mesh || V3DStreamingPolicy::BoundsInRenderRange(FVector::ZeroVector, Mesh->Size,
                    Node.Transform * OwnerTransform, WorldLocation, MaxDistanceCm)) return false;
            }
        }
    }

    const float WaterDistanceRadius = FMath::Max(EffectiveStreamDistance * 1024.0f, 2048.0f);
    for (const TPair<FName, FWaterStreamNodeData>& WaterPair : WaterNodeMap)
    {
        const FVector WaterWorldLocation =
            (WaterPair.Value.Transform * OwnerTransform).GetLocation();
        const float LoadRadius = FMath::Max(WaterPair.Value.StreamRadius, WaterDistanceRadius);
        const double RenderRadius = MaxDistanceCm + WaterPair.Value.StreamRadius * OwnerTransform.GetScale3D().GetAbs().GetMax();
        if (FVector::DistSquared(WorldLocation, WaterWorldLocation) <= FMath::Square(FMath::Min(double(LoadRadius), RenderRadius))
            && !LoadedWaterNodes.Contains(WaterPair.Key))
        {
            return false;
        }
    }
    return true;
}

void AStaticActor::BeginPlay()
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::BeginPlay")))
    {
        return;
    }
    Super::BeginPlay();

    bRuntimeResourcesReleased = false;
    bIsDestroyed = false;
    StreamingRetry.Reset();
    bReloadMetadataOnRetry = false;
    bIsLoaded = false;
    bAsyncLoading = false;
    bHasModelMetadata = false;
    LoadingStatus = 0.0f;
    GameUpdateTickHandle = INDEX_NONE;
    MetadataRequestSerial = 0;
    ActiveStreamActions.Empty();
    StreamGroupProgress.Empty();
    StreamGroupProgressSum = 0.0;
    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    LoadedWaterNodes.Empty();
    OwnedInstancedMeshActors.Empty();
    MeshGroupRecovery.Empty();
    WaterActorMap.Empty();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    ModelMetadata = FModelData();

    if (UV3DSimulatorAssetRegistry* Registry =
            UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        if (!IsValid(DecalLight))
        {
            DecalLight = Registry->StaticDecalLightMaterial.LoadSynchronous();
        }
        if (!WaterClass)
        {
            UClass* ResolvedWaterClass = Registry->WaterActorClass.LoadSynchronous();
            if (IsValid(ResolvedWaterClass) && ResolvedWaterClass->IsChildOf(AWaterActor::StaticClass())
                && !ResolvedWaterClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                WaterClass = ResolvedWaterClass;
            }
        }
    }
    if (!WaterClass)
    {
        WaterClass = AWaterActor::StaticClass();
    }

    StartBuiltLoad();
}

void AStaticActor::StartBuiltLoad()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        return;
    }

    if (WorkCancellation.IsValid()) WorkCancellation->Increment();
    WorkCancellation = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
    StreamingEvaluation.Reset();
    NextStreamingUpdateSeconds = 0.0;
    StreamingRetry.BeginAttempt();
    bReloadMetadataOnRetry = false;
    CancelActiveStreamActions();
    ReleaseStreamingResources();
    BakedAsset = nullptr;
    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;
    bHasModelMetadata = false;
    bIsLoaded = false;
    ModelReference = StaticActorPrivate::NormalizeReference(ModelReference);
    FGuid UUID;
    FString CanonicalReference;
    FString Reason;
    if (ModelReference.IsEmpty()
        || !StaticActorPrivate::ResolveScene(
            this, ModelReference, UUID, CanonicalReference, Reason))
    {
        RetryMetadataLoad();
        WriteLogAsync(FString::Printf(
            TEXT("Static load rejected. Reference=%s Reason=%s"),
            *ModelReference,
            Reason.IsEmpty() ? TEXT("invalid v3d reference") : *Reason));
        return;
    }

    ModelReference = CanonicalReference;
    LoadBuiltMetadataAsync(UUID);
}

void AStaticActor::EndPlay(
    const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::EndPlay(EndPlayReason);
}

void AStaticActor::Destroyed()
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::Destroyed();
}

void AStaticActor::ReleaseRuntimeResourcesForWorldExit()
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::ReleaseRuntimeResourcesForWorldExit"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;
    if (WorkCancellation.IsValid()) WorkCancellation->Increment();
    WorkCancellation.Reset();
    bStreamingBatchQueued = false;

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearAllTimersForObject(this);
        if (UStreamingMovementGateSubsystem* Gate =
                World->GetSubsystem<UStreamingMovementGateSubsystem>())
        {
            Gate->ClearModelRegions(this);
        }
    }

    StreamingEvaluation.Reset();
    StreamingRetry.Reset();
    bReloadMetadataOnRetry = false;
    bIsDestroyed = true;
    bAsyncLoading = false;
    bIsLoaded = false;
    LoadingStatus = 1.0f;
    ++MetadataRequestSerial;
    UnregisterGameUpdate();
    CancelActiveStreamActions();
    ReleaseStreamingResources();
    BakedAsset = nullptr;
    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    ModelMetadata = FModelData();
    bHasModelMetadata = false;
    ModelReference.Reset();
    StreamGroupProgress.Empty();
    StreamGroupProgressSum = 0.0;
}

void AStaticActor::LoadBuiltMetadataAsync(const FGuid& UUID)
{
    check(IsInGameThread());
    const UGameInstance* GameInstance = GetGameInstance();
    const UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>()
        : nullptr;
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
        Database ? Database->GetArchiveReaderForModel(UUID) : nullptr;
    FGWorldModelRecord FieldRecord;
    if (Reader.IsValid() && Reader->FindRecord(UUID, FieldRecord))
        GravityField->ApplyModelSettings(FieldRecord.Definition.GravityField);
    const uint64 RequestSerial = ++MetadataRequestSerial;
    const FString ExpectedReference = ModelReference;
    bAsyncLoading = true;
    LoadingStatus = FMath::Max(LoadingStatus, 0.05f);

    TWeakObjectPtr<AStaticActor> WeakThis(this);
    const auto Cancellation = WorkCancellation;
    const bool bQueued = Reader.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, Reader, UUID, RequestSerial, ExpectedReference, Cancellation]() mutable
            {
                if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
                FGWorldModelMetadata Metadata;
                FGWorldModelManifest Manifest;
                FString Errors[2];
                bool Results[2] = { false, false };

                // Metadata and the manifest occupy independent immutable archive members. Reading
                // them concurrently uses private file handles and overlaps decompression/CRC work
                // without ever sharing a seek cursor or touching a UObject from a worker.
                ParallelFor(2, [&](const int32 Index)
                {
                    if (Index == 0)
                    {
                        Results[0] = Reader->ReadModelMetadata(UUID, Metadata, Errors[0]);
                    }
                    else
                    {
                        Results[1] = Reader->ReadModelManifest(UUID, Manifest, Errors[1]);
                    }
                });

                if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
                const bool bSuccess = Results[0] && Results[1];
                TArray<FString> TerrainLayerNames;
                if (bSuccess && Reader.IsValid())
                {
                    TerrainLayerNames = StaticActorPrivate::ExtractTerrainLayerNamesFromArchive(
                        Reader->GetArchivePath(), Manifest);
                }
                FString Error;
                if (!bSuccess)
                {
                    Error = !Results[0] ? MoveTemp(Errors[0]) : MoveTemp(Errors[1]);
                    if (Error.IsEmpty())
                    {
                        Error = TEXT("failed to read prepared .v3d model tables");
                    }
                }

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis,
                        Reader,
                        UUID,
                        RequestSerial,
                        ExpectedReference,
                        bSuccess,
                        Metadata = MoveTemp(Metadata),
                        Manifest = MoveTemp(Manifest),
                        TerrainLayerNames = MoveTemp(TerrainLayerNames),
                        Error = MoveTemp(Error)]() mutable
                    {
                        if (AStaticActor* StrongThis = WeakThis.Get())
                        {
                            StrongThis->OnBuiltMetadataLoaded(
                                RequestSerial,
                                ExpectedReference,
                                UUID,
                                Reader,
                                bSuccess,
                                MoveTemp(Metadata),
                                MoveTemp(Manifest),
                                MoveTemp(Error));
                            if (bSuccess && !TerrainLayerNames.IsEmpty() && IsValid(StrongThis->GetBakedAsset()))
                            {
                                if (UWorldSurfaceSubsystem* Surfaces = UWorldSurfaceSubsystem::Get(StrongThis))
                                {
                                    Surfaces->RegisterTerrainLayers(StrongThis->GetBakedAsset(), TerrainLayerNames);
                                }
                            }
                        }
                    });
            });

    if (!bQueued)
    {
        OnBuiltMetadataLoaded(
            RequestSerial,
            ExpectedReference,
            UUID,
            Reader,
            false,
            FGWorldModelMetadata(),
            FGWorldModelManifest(),
            TEXT(".v3d metadata worker queue is shutting down"));
    }
}

void AStaticActor::OnBuiltMetadataLoaded(
    const uint64 RequestSerial,
    const FString& ExpectedReference,
    const FGuid& UUID,
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& Reader,
    const bool bSuccess,
    FGWorldModelMetadata&& Metadata,
    FGWorldModelManifest&& Manifest,
    FString&& Error)
{
    check(IsInGameThread());
    if (bIsDestroyed
        || RequestSerial != MetadataRequestSerial
        || ModelReference != ExpectedReference)
    {
        return;
    }
    if (!bSuccess || !Reader.IsValid())
    {
        RetryMetadataLoad();
        WriteLogAsync(FString::Printf(
            TEXT("Static metadata range rejected. Reference=%s Reason=%s"),
            *ModelReference, *Error));
        return;
    }

    // Complete the admitted scene's metadata even if observers moved during the read. The outer
    // streamer owns coarse eviction; dropping these tables here leaves an empty actor in its
    // unload hysteresis band that can never load when an observer returns.
    ModelMetadata = Metadata.SceneData.ModelData;
    bHasModelMetadata = true;
    LoadingStatus = 0.5f;

    // Publish the tables that the worker already read. This deliberately avoids Resolve() here:
    // static world streaming does not need definition JSON/bone aliases, and synchronously loading
    // those rows plus metadata/manifest again was a major game-thread startup hitch.
    FString ResolveError;
    BakedAsset = FRuntimeModelResolver::LoadAssetFromPreparedTables(
        UUID,
        ModelReference,
        Reader,
        MoveTemp(Metadata),
        MoveTemp(Manifest),
        ResolveError);
    if (!IsValid(BakedAsset))
    {
        RetryMetadataLoad();
        WriteLogAsync(FString::Printf(
            TEXT("Baked scene facade initialization failed: %s"),
            *ResolveError));
        return;
    }


    // The facade only consumes the complete node-transform/manifest view. Transfer the scene
    // placement maps afterwards so validation sees the exact table read from disk and no second
    // copy of these potentially huge maps is retained.
    AllNodeMap = MoveTemp(Metadata.SceneData.NodeMap);
    WaterNodeMap = MoveTemp(Metadata.SceneData.WaterNodeMap);
    AllMeshMap = MoveTemp(Metadata.SceneData.MeshMap);
    ModelMetadata = MoveTemp(Metadata.SceneData.ModelData);

    BeginBuildInstancedMeshActors();
}

void AStaticActor::CancelActiveStreamActions()
{
    check(IsInGameThread());
    for (TPair<FName, TObjectPtr<UWorldSceneStreamAction>>& Pair :
        ActiveStreamActions)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->CancelAndRelease();
        }
    }
    ActiveStreamActions.Empty();
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
}

void AStaticActor::ReleaseStreamingResources()
{
    check(IsInGameThread());
    ReleaseInstancedMeshActors();
    for (TPair<FName, TObjectPtr<AWaterActor>>& Pair : WaterActorMap)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->Destroy();
        }
    }
    WaterActorMap.Empty();
    LoadedWaterNodes.Empty();
}

void AStaticActor::BeginBuildInstancedMeshActors()
{
    check(IsInGameThread());
    ReleaseInstancedMeshActors();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;

    // Grouping can touch hundreds of thousands of node rows but does not need a UObject. Move the
    // scene-wide node map into detached worker-owned storage instead of copying it, and leave only a
    // compact mesh->group lookup prepared on the game thread.
    TSharedRef<StaticActorPrivate::FStaticGroupingWork, ESPMode::ThreadSafe> Work =
        MakeShared<StaticActorPrivate::FStaticGroupingWork, ESPMode::ThreadSafe>();
    Work->Nodes = MoveTemp(AllNodeMap);
    Work->Cancellation = WorkCancellation;
    Work->OwnerTransform = GetActorTransform();
    Work->MeshToGroup.Reserve(AllMeshMap.Num());
    for (const TPair<FName, FModelMeshData>& Pair : AllMeshMap)
    {
        const FModelMeshData& Mesh = Pair.Value;
        const bool bHasRuntimeMesh = Mesh.LOD0 != INDEX_NONE
            || Mesh.LOD1 != INDEX_NONE
            || Mesh.LOD2 != INDEX_NONE
            || Mesh.LOD3 != INDEX_NONE;
        if (!Pair.Key.IsNone() && bHasRuntimeMesh && !Mesh.Size.ContainsNaN())
        {
            Work->MeshToGroup.Add(Pair.Key, StaticActorPrivate::MakeMeshGroupName(Mesh));
            Work->MeshSizes.Add(Pair.Key, Mesh.Size);
        }
    }

    const uint64 RequestSerial = MetadataRequestSerial;
    const FString ExpectedReference = ModelReference;
    TWeakObjectPtr<AStaticActor> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Work, RequestSerial, ExpectedReference]() mutable
        {
            if (Work->Cancellation.IsValid() && Work->Cancellation->GetValue() != 0) return;
            StaticActorPrivate::BuildStaticGroups(*Work);
            if (Work->Cancellation.IsValid() && Work->Cancellation->GetValue() != 0) return;
            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Work, RequestSerial, ExpectedReference]() mutable
                {
                    if (AStaticActor* StrongThis = WeakThis.Get())
                    {
                        StrongThis->OnInstancedMeshGroupsPrepared(
                            RequestSerial,
                            ExpectedReference,
                            MoveTemp(Work->Groups),
                            Work->InvalidNodeCount);
                    }
                });
        });

    if (!bQueued)
    {
        // Shutdown/drain refuses new worker tasks. Do not move an O(N) grouping/index build back to
        // GameThread; invalidate this request and let teardown release the detached native work.
        ++MetadataRequestSerial;
        RetryMetadataLoad();
        UE_LOG(LogTemp, Verbose, TEXT("Static grouping deferred because the worker queue is shutting down."));
    }
}

void AStaticActor::OnInstancedMeshGroupsPrepared(
    const uint64 RequestSerial,
    const FString& ExpectedReference,
    TMap<FName, FInstancedMeshGroupInitData>&& Groups,
    const int32 InvalidNodeCount)
{
    check(IsInGameThread());
    if (bIsDestroyed
        || RequestSerial != MetadataRequestSerial
        || ModelReference != ExpectedReference)
    {
        return;
    }

    PendingInstancedGroups = MoveTemp(Groups);
    PendingInstancedGroupNames.Reset();
    PendingInstancedGroups.GetKeys(PendingInstancedGroupNames);
    PendingInstancedGroupNames.Sort([](const FName A, const FName B)
    {
        return A.LexicalLess(B);
    });
    PendingInstancedGroupIndex = 0;

    if (InvalidNodeCount > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Removed %d invalid static node(s) while grouping. Static=%s"),
            InvalidNodeCount, *ModelReference);
    }

    if (PendingInstancedGroupNames.IsEmpty())
    {
        FinishBuildInstancedMeshActors();
        return;
    }

    LoadingStatus = FMath::Max(LoadingStatus, 0.50f);
    BuildInstancedMeshActorsStep();
}

void AStaticActor::BuildInstancedMeshActorsStep()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        PendingInstancedGroups.Empty();
        PendingInstancedGroupNames.Empty();
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        RetryMetadataLoad();
        return;
    }

    // These are lightweight group actors; payload concurrency is governed separately.
    // Fill a time slice instead of delaying every few cheap groups by an entire frame.
    const double SliceStart = FPlatformTime::Seconds();
    const double SliceBudget = FMath::Min(0.002, FV3DStreamingBudget::RemainingSceneSeconds());
    if (SliceBudget <= 0.0)
    {
        World->GetTimerManager().SetTimerForNextTick(
            this, &AStaticActor::BuildInstancedMeshActorsStep);
        return;
    }
    {
        ON_SCOPE_EXIT { FV3DStreamingBudget::ChargeSceneSeconds(FPlatformTime::Seconds() - SliceStart); };
        const uint64 RequestSerial = MetadataRequestSerial;
        const double Deadline = SliceStart + SliceBudget;
        const int32 BeginIndex = PendingInstancedGroupIndex;
        const int32 EndIndex = FMath::Min(PendingInstancedGroupIndex + 32, PendingInstancedGroupNames.Num());
        for (; PendingInstancedGroupIndex < EndIndex
            && (PendingInstancedGroupIndex == BeginIndex || FPlatformTime::Seconds() < Deadline);
            ++PendingInstancedGroupIndex)
        {
            const FName GroupName = PendingInstancedGroupNames[PendingInstancedGroupIndex];
            FInstancedMeshGroupInitData* GroupData = PendingInstancedGroups.Find(GroupName);
            if (!GroupData)
            {
                continue;
            }

            FInstancedMeshGroupInitData PreparedGroup = MoveTemp(*GroupData);

            FActorSpawnParameters Parameters;
            Parameters.Owner = this;
            Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AInstancedMeshActor* InstancedActor =
                FActorHelper::SpawnActorDeferred<AInstancedMeshActor>(
                    World, AInstancedMeshActor::StaticClass(), GetActorTransform(), Parameters);
            const bool bInitialized = IsValid(InstancedActor)
                && InstancedActor->InitializeGroup(GroupName, MoveTemp(PreparedGroup));
            if (IsValid(InstancedActor))
            {
                InstancedActor->FinishSpawning(GetActorTransform());
            }
            // Native construction/BeginPlay may re-enter world teardown or Init().
            if (bIsDestroyed || MetadataRequestSerial != RequestSerial)
            {
                if (IsValid(InstancedActor)) InstancedActor->Destroy();
                return;
            }
            if (!bInitialized
                || !IsValid(InstancedActor)
                || !InstancedActor->AttachToActor(
                    this, FAttachmentTransformRules::KeepWorldTransform))
            {
                // InitializeGroup transfers ownership. Recover that immutable data before
                // destroying a partially constructed actor; never silently skip its placements.
                if (IsValid(InstancedActor))
                {
                    const auto Definitions = InstancedActor->CaptureDefinitions();
                    if (Definitions.IsValid()) PreparedGroup = *Definitions;
                    InstancedActor->Destroy();
                }
                if (PreparedGroup.IsValid())
                {
                    PendingInstancedGroups.Add(GroupName, MoveTemp(PreparedGroup));
                    bAsyncLoading = false;
                    RecordStreamingFailure();
                    RegisterGameUpdate();
                }
                else RetryMetadataLoad();
                WriteLogAsync(FString::Printf(
                    TEXT("Static group construction deferred. Static=%s Group=%s"),
                    *ModelReference, *GroupName.ToString()));
                return;
            }
            PendingInstancedGroups.Remove(GroupName);
            OwnedInstancedMeshActors.Add(GroupName, InstancedActor);
        }
    }

    const float GroupFraction = PendingInstancedGroupNames.IsEmpty()
        ? 1.0f
        : static_cast<float>(PendingInstancedGroupIndex)
            / static_cast<float>(PendingInstancedGroupNames.Num());
    LoadingStatus = FMath::Max(LoadingStatus, FMath::Lerp(0.50f, 0.60f, GroupFraction));

    if (PendingInstancedGroupIndex < PendingInstancedGroupNames.Num())
    {
        World->GetTimerManager().SetTimerForNextTick(
            this, &AStaticActor::BuildInstancedMeshActorsStep);
        return;
    }
    FinishBuildInstancedMeshActors();
}

void AStaticActor::FinishBuildInstancedMeshActors()
{
    check(IsInGameThread());
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;

    // Node rows are now owned by their mesh-group actors. Discard the duplicate scene-wide map.
    AllNodeMap.Empty();
    PruneUnreferencedMeshMetadata();

    if (!OwnedInstancedMeshActors.IsEmpty() || !WaterNodeMap.IsEmpty())
    {
        bAsyncLoading = false;
        StartStreaming();
    }
    else
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
    }
}

void AStaticActor::ReleaseInstancedMeshActors()
{
    check(IsInGameThread());
    for (TPair<FName, TObjectPtr<AInstancedMeshActor>>& Pair :
        OwnedInstancedMeshActors)
    {
        if (AInstancedMeshActor* Actor = Pair.Value.Get(); IsValid(Actor))
        {
            if (UWorld* World = GetWorld())
                if (auto* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                    Gate->ClearModelRegions(Actor);
            Actor->ReleaseRuntimeResources();
            Actor->Destroy();
        }
    }
    OwnedInstancedMeshActors.Empty();
    MeshGroupRecovery.Empty();
}

bool AStaticActor::IsMeshGroupUnavailable(const FName GroupName) const
{
    const FV3DMeshGroupRecovery* Recovery = MeshGroupRecovery.Find(GroupName);
    return Recovery && Recovery->IsUnavailable();
}

int32 AStaticActor::GetUnavailableMeshGroupCount() const
{
    int32 Count = 0;
    for (const auto& Pair : MeshGroupRecovery)
        if (Pair.Value.IsUnavailable()) ++Count;
    return Count;
}

void AStaticActor::PruneUnreferencedMeshMetadata()
{
    check(IsInGameThread());
    // Group identity is a pure function of FModelMeshData. Avoid re-walking every node stored by
    // every instanced actor after grouping; dense scenes can contain orders of magnitude more nodes
    // than mesh rows. Keeping an unused mesh row that shares a live group's signature is harmless,
    // while removing rows whose group no longer exists is sufficient for streaming correctness.
    for (auto It = AllMeshMap.CreateIterator(); It; ++It)
    {
        if (!OwnedInstancedMeshActors.Contains(
                StaticActorPrivate::MakeMeshGroupName(It.Value())))
        {
            It.RemoveCurrent();
        }
    }
}

void AStaticActor::SetStreamGroupProgress(const FName GroupName, const float Progress)
{
    float& Previous = StreamGroupProgress.FindOrAdd(GroupName);
    const float Current = FMath::Clamp(Progress, 0.0f, 1.0f);
    StreamGroupProgressSum += static_cast<double>(Current) - Previous;
    Previous = Current;
    const float BatchProgress = StreamGroupProgress.IsEmpty() ? 1.0f
        : static_cast<float>(StreamGroupProgressSum / StreamGroupProgress.Num());
    LoadingStatus = FMath::Max(LoadingStatus,
        FMath::Clamp(0.5f + BatchProgress * 0.5f, 0.5f, 1.0f));
    if (StreamingRetry.HasFailedAttempt()) LoadingStatus = FMath::Min(LoadingStatus, 0.99f);
}

void AStaticActor::OnStreamProgress(const FName GroupName, const float Progress)
{
    check(IsInGameThread());
    SetStreamGroupProgress(GroupName, Progress);
}

void AStaticActor::WriteLogAsync(const FString& Message) const
{
    FSimulatorFileServices::WriteLogAsync(
        TEXT("StaticActor"), Message);
}

void AStaticActor::StartStreaming()
{
    check(IsInGameThread());
    if (OwnedInstancedMeshActors.IsEmpty() && WaterNodeMap.IsEmpty())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }
    RegisterGameUpdate();
    StartStreamingStep();
}

void AStaticActor::RegisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<AStaticActor>(this)](float DeltaSeconds)
            {
                if (AStaticActor* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateStreaming(DeltaSeconds);
                }
            },
            15);
    }
}

void AStaticActor::UnregisterGameUpdate()
{
    check(IsInGameThread());
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AStaticActor::UpdateStreaming(float /*DeltaSeconds*/)
{
    check(IsInGameThread());
    StartStreamingStep();
}

FglTFRuntimeStaticMeshConfig AStaticActor::BuildStreamingMeshConfig()
{
    check(IsInGameThread());
    FglTFRuntimeStaticMeshConfig Config;
    // The RuntimeLOD finalizer receives fully reconstructed materials and textures from .dat.
    // Parser caches would only retain transient build products beyond their streaming lifetime.
    Config.CacheMode = EglTFRuntimeCacheMode::None;
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::None;
    Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;
    if (UGameManagerSubSystem* Manager =
            UGameManagerSubSystem::GetSubSystem(this))
    {
        V3DMaterialOverrideUtils::ApplyOverrides(
            Manager->GetMaterialDefaultReferences(), Config.MaterialsConfig);
    }
    Config.MaterialsConfig.bGeneratesMipMaps = false;
    Config.MaterialsConfig.bLoadMipMaps = true;
    Config.MaterialsConfig.ImagesConfig.bStreaming = false;
    Config.MaterialsConfig.SpecularFactor = 0.0f;
    const int32 TextureLimit = UGameSettings::ResolveMaxTextureResolution(this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    Config.Outer = nullptr; // The stream action supplies a world-aware transient outer.
    // Runtime world streaming should not retain a CPU vertex copy for every visual mesh. The
    // stream action enables CPU access only for groups that actually request complex collision.
    Config.bAllowCPUAccess = false;
    Config.bGenerateStaticMeshDescription = false;
    // glTFRuntime supplies six bounds-aligned cards per LOD. Keep this compact representation
    // for Lumen's surface cache; screen traces alone cannot cover off-screen world geometry.
    // Mesh builds remain shared and streaming-concurrency limited.
    Config.bBuildLumenCards = true;
    Config.bBuildNavCollision = false;
    if (bRenderOnlyStreaming)
    {
        Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;
        Config.bBuildComplexCollision = false;
        Config.bBuildSimpleCollision = false;
    }
    return Config;
}

FV3DStreamingEvaluationInputs AStaticActor::GetStreamingEvaluationInputs() const
{
    FV3DStreamingEvaluationInputs Inputs;
    if (UWorldSceneStreamingSubsystem* Streamer = UWorldSceneStreamingSubsystem::Get(GetWorld()))
        Streamer->GetStreamingObserverLocations(Inputs.Observers);
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        if (Inputs.Observers.IsEmpty()) Inputs.Observers.Add(Manager->GetPlayerLocation());
        if (const UGameSettings* Settings = Manager->GetGameSettings())
            Inputs.UnloadMultiplier = Settings->GetStreamingUnloadDistanceMultiplier();
    }
    if (Inputs.Observers.IsEmpty()) Inputs.Observers.Add(FVector::ZeroVector);
    Inputs.OwnerTransform = GetActorTransform();
    Inputs.ScreenDistance = V3DStreamingPolicy::GetScreenSizeDistance(this);
    Inputs.MaxRenderDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(this);
    Inputs.bRenderOnly = bRenderOnlyStreaming;
    return Inputs;
}

void AStaticActor::RecordStreamingFailure()
{
    check(IsInGameThread());
    StreamingRetry.Fail(FPlatformTime::Seconds());
    StreamingEvaluation.Invalidate();
    bIsLoaded = false;
    LoadingStatus = FMath::Min(LoadingStatus, 0.99f);
}

void AStaticActor::RetryMetadataLoad()
{
    check(IsInGameThread());
    bReloadMetadataOnRetry = true;
    bAsyncLoading = false;
    RecordStreamingFailure();
    RegisterGameUpdate();
}

void AStaticActor::StartStreamingStep()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        UnregisterGameUpdate();
        return;
    }
    // A failed admission/planning path may terminate without Completed. Do not let its
    // stale slot lock this scene forever. Keep definitions and retry after a short cooldown.
    bool bAbortedAction = false;
    for (auto It = ActiveStreamActions.CreateIterator(); It; ++It)
        if (!IsValid(It.Value()) || It.Value()->IsFinished())
        {
            const auto* Group = OwnedInstancedMeshActors.Find(It.Key());
            if (!It.Key().IsNone() && (!Group || !IsValid(Group->Get())))
                bReloadMetadataOnRetry = true;
            It.RemoveCurrent();
            bAbortedAction = true;
        }
    if (bAbortedAction)
    {
        RecordStreamingFailure();
        if (ActiveStreamActions.IsEmpty()
            && PendingStreamGroupIndex >= PendingStreamGroupNames.Num() && !bPendingWaterStream)
            FinishStreamingCycle();
        else QueueStreamingBatch();
    }
    // Metadata/group construction and active payload work own this cycle until completion.
    if (bAsyncLoading) return;
    const double Now = FPlatformTime::Seconds();
    if (!StreamingRetry.CanAttempt(Now)) return;
    if (bReloadMetadataOnRetry)
    {
        StartBuiltLoad();
        return;
    }
    if (PendingInstancedGroupIndex < PendingInstancedGroupNames.Num())
    {
        StreamingRetry.BeginAttempt();
        bAsyncLoading = true;
        BuildInstancedMeshActorsStep();
        return;
    }
    if (!IsValid(BakedAsset))
    {
        StreamingRetry.BeginAttempt();
        RetryMetadataLoad();
        WriteLogAsync(FString::Printf(
            TEXT("Baked scene facade became invalid: %s"), *ModelReference));
        return;
    }
    if (OwnedInstancedMeshActors.IsEmpty() && WaterNodeMap.IsEmpty())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        UnregisterGameUpdate();
        return;
    }

    if (Now < NextStreamingUpdateSeconds) return;
    double UpdateInterval = 0.08;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
        if (const UGameSettings* Settings = Manager->GetGameSettings())
            UpdateInterval = Settings->GetStreamingUpdateIntervalSeconds();
    NextStreamingUpdateSeconds = Now + UpdateInterval;

    FV3DStreamingEvaluationInputs Inputs = GetStreamingEvaluationInputs();
    if (!StreamingEvaluation.NeedsEvaluation(Inputs)) return;
    StreamingEvaluation.Capture(MoveTemp(Inputs));
    StreamingRetry.BeginAttempt();

    // Keep completed scenes ready while other scenes load. An unchanged view cannot gain new
    // candidates in immutable metadata, so re-queuing every mesh group would only delay startup.
    bAsyncLoading = true;
    bIsLoaded = false;
    PendingStreamGroupNames.Empty();
    OwnedInstancedMeshActors.GetKeys(PendingStreamGroupNames);
    const TArray<FVector>& PriorityObservers = StreamingEvaluation.GetInputs().Observers;
    TMap<FName, double> GroupDistances;
    for (const FName Name : PendingStreamGroupNames)
    {
        const auto* Group = OwnedInstancedMeshActors.FindChecked(Name).Get();
        double Best = TNumericLimits<double>::Max();
        if (IsValid(Group))
        {
            if (Group->HasAlwaysLoadedNodes()) Best = 0.0;
            else if (Group->GetNodeBounds().IsValid)
            {
                const FBox WorldBounds = Group->GetNodeBounds().TransformBy(GetActorTransform());
                for (const FVector& Observer : PriorityObservers)
                    Best = FMath::Min(Best, WorldBounds.ComputeSquaredDistanceToPoint(Observer));
            }
        }
        GroupDistances.Add(Name, Best);
    }
    PendingStreamGroupNames.Sort([&GroupDistances](const FName A, const FName B)
    {
        const double DA = GroupDistances.FindChecked(A), DB = GroupDistances.FindChecked(B);
        return DA == DB ? A.LexicalLess(B) : DA < DB;
    });
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = !WaterNodeMap.IsEmpty();
    StreamGroupProgress.Empty();
    StreamGroupProgressSum = 0.0;
    for (const FName GroupName : PendingStreamGroupNames)
    {
        StreamGroupProgress.Add(GroupName, 0.0f);
    }
    if (bPendingWaterStream)
    {
        StreamGroupProgress.Add(NAME_None, 0.0f);
    }
    LaunchNextStreamingBatch();
}

void AStaticActor::QueueStreamingBatch()
{
    check(IsInGameThread());
    if (bStreamingBatchQueued || bIsDestroyed || !bAsyncLoading) return;
    if (UWorld* World = GetWorld())
    {
        bStreamingBatchQueued = true;
        StreamingBatchTimerHandle = World->GetTimerManager().SetTimerForNextTick(
            this, &AStaticActor::LaunchNextStreamingBatch);
    }
}

void AStaticActor::LaunchNextStreamingBatch()
{
    check(IsInGameThread());
    if (bStreamingBatchQueued)
    {
        bStreamingBatchQueued = false;
        if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(StreamingBatchTimerHandle);
    }
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        PendingStreamGroupNames.Empty();
        PendingStreamGroupIndex = 0;
        bPendingWaterStream = false;
        return;
    }
    if (!bAsyncLoading)
    {
        return;
    }

    // Batches can observe A -> B -> A during a long pass. Remember the intermediate
    // change even when B only skipped/unloaded groups and no mesh callback was involved.
    if (StreamingEvaluation.NeedsEvaluation(GetStreamingEvaluationInputs()))
        StreamingEvaluation.Invalidate();

    TArray<FVector> ObserverLocations;
    int32 SafeChunkSize = FMath::Max(1, ChunkSize);
    // Actions hold planning metadata while waiting. Actual payloads have a global byte/count
    // admission limit; tying planning slots to native builds leaves disk workers idle.
    int32 GroupBudget = 12;
    float UnloadDistanceMultiplier = 1.10f;
    if (UWorldSceneStreamingSubsystem* Streamer = UWorldSceneStreamingSubsystem::Get(this))
    {
        Streamer->GetStreamingObserverLocations(ObserverLocations);
    }
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        if (ObserverLocations.IsEmpty())
        {
            ObserverLocations.Add(Manager->GetPlayerLocation());
        }
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            SafeChunkSize = FMath::Min(
                SafeChunkSize, Settings->GetStreamingNodeBudgetPerFrame());
            GroupBudget = FMath::Min(
                GroupBudget, Settings->GetStreamingMeshGroupConcurrency());
            UnloadDistanceMultiplier = Settings->GetStreamingUnloadDistanceMultiplier();
        }
    }
    if (ObserverLocations.IsEmpty()) ObserverLocations.Add(FVector::ZeroVector);
    GroupBudget = FMath::Clamp(GroupBudget, 1, 16);
    const int32 AvailableSlots = FMath::Max(
        0, GroupBudget - ActiveStreamActions.Num());
    if (AvailableSlots == 0) return; // The next completion will queue one backfill pump.

    // Both readiness and stream planning use the same bounded, world-scale-aware policy.
    const float EffectiveStreamDistance = V3DStreamingPolicy::GetScreenSizeDistance(this);
    const double MaxDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(this);
    const FglTFRuntimeStaticMeshConfig MeshConfig = BuildStreamingMeshConfig();

    const auto StartGroup =
        [this, &ObserverLocations, &MeshConfig, SafeChunkSize, EffectiveStreamDistance, UnloadDistanceMultiplier, MaxDistanceCm](
            AInstancedMeshActor* InstancedActor,
            const bool bWaterGroup)
        {
            const FName GroupName = bWaterGroup
                ? NAME_None : InstancedActor->GetGroupName();
            if (!bWaterGroup && IsMeshGroupUnavailable(GroupName))
            {
                SetStreamGroupProgress(GroupName, 1.0f);
                return true;
            }
            // Unloaded groups outside a conservative bound need no action/snapshot/worker.
            // Loaded groups must still run so instances leaving the range can be released.
            if (!bWaterGroup && !InstancedActor->HasLoadedNodes()
                && !InstancedActor->HasAlwaysLoadedNodes()
                && InstancedActor->GetNodeBounds().IsValid)
            {
                const FTransform OwnerTransform = GetActorTransform();
                const double MaxScale = InstancedActor->GetMaxNodeScale()
                    * OwnerTransform.GetScale3D().GetAbs().GetMax();
                double Radius = 0.0;
                for (const FName MeshName : InstancedActor->GetReferencedMeshNames())
                {
                    if (const FModelMeshData* Mesh = AllMeshMap.Find(MeshName))
                    {
                        const double MeshRadius = FMath::Max(
                            FMath::Min(MaxDistanceCm, V3DHISMChunkPolicy::ChunkSizeCm),
                            V3DStreamingPolicy::LoadRadius(Mesh->Size, MaxScale, EffectiveStreamDistance, MaxDistanceCm))
                            + FMath::Sqrt(3.0) * V3DHISMChunkPolicy::ChunkSizeCm;
                        Radius = FMath::Max(Radius, MeshRadius);
                    }
                }
                const FBox WorldBounds = InstancedActor->GetNodeBounds().TransformBy(OwnerTransform);
                bool bInRange = false;
                for (const FVector& Observer : ObserverLocations)
                    if (WorldBounds.ComputeSquaredDistanceToPoint(Observer) <= FMath::Square(Radius))
                    { bInRange = true; break; }
                if (!bInRange)
                {
                    SetStreamGroupProgress(GroupName, 1.0f);
                    return true;
                }
            }
            UWorldSceneStreamAction* Action =
                UWorldSceneStreamAction::StreamAsyncForObservers(
                    this,
                    this,
                    InstancedActor,
                    ObserverLocations,
                    MeshConfig,
                    SafeChunkSize,
                    bRenderOnlyStreaming,
                    bWaterGroup,
                    UnloadDistanceMultiplier);
            if (!IsValid(Action))
            {
                return false;
            }
            ActiveStreamActions.Add(GroupName, Action);
            SetStreamGroupProgress(GroupName, 0.0f);
            Action->Completed.AddDynamic(
                this, &AStaticActor::OnStreamCompleted);
            Action->Progress.AddDynamic(
                this, &AStaticActor::OnStreamProgress);
            Action->Activate();
            return true;
        };

    int32 LaunchedThisBatch = 0;
    int32 ExaminedThisBatch = 0;
    const double LaunchStart = FPlatformTime::Seconds();
    const double LaunchBudget = FMath::Min(0.0015, FV3DStreamingBudget::RemainingSceneSeconds());
    if (LaunchBudget <= 0.0) { QueueStreamingBatch(); return; }
    ON_SCOPE_EXIT { FV3DStreamingBudget::ChargeSceneSeconds(FPlatformTime::Seconds() - LaunchStart); };
    const uint64 RequestSerial = MetadataRequestSerial;
    const double LaunchDeadline = LaunchStart + LaunchBudget;
    while (LaunchedThisBatch < AvailableSlots && ExaminedThisBatch++ < 64
        && FPlatformTime::Seconds() < LaunchDeadline
        && PendingStreamGroupIndex < PendingStreamGroupNames.Num())
    {
        const FName GroupName = PendingStreamGroupNames[PendingStreamGroupIndex++];
        TObjectPtr<AInstancedMeshActor>* Found = OwnedInstancedMeshActors.Find(GroupName);
        AInstancedMeshActor* InstancedActor = Found ? Found->Get() : nullptr;
        if (!IsValid(InstancedActor) || InstancedActor->GetOwner() != this)
        {
            // The immutable definitions disappeared with the actor. Recover from archive tables
            // after the remaining actions finish, instead of deleting the group's mesh metadata.
            bReloadMetadataOnRetry = true;
            RecordStreamingFailure();
            SetStreamGroupProgress(GroupName, 1.0f);
            continue;
        }
        if (!StartGroup(InstancedActor, false))
        {
            RecordStreamingFailure();
            SetStreamGroupProgress(GroupName, 1.0f);
            continue;
        }
        if (bIsDestroyed || MetadataRequestSerial != RequestSerial) return;
        if (ActiveStreamActions.Contains(GroupName)) ++LaunchedThisBatch;
    }
    if (LaunchedThisBatch < AvailableSlots && bPendingWaterStream)
    {
        bPendingWaterStream = false;
        if (!StartGroup(nullptr, true))
        {
            RecordStreamingFailure();
            SetStreamGroupProgress(NAME_None, 1.0f);
        }
        else
        {
            ++LaunchedThisBatch;
        }
    }

    if (bIsDestroyed || MetadataRequestSerial != RequestSerial) return;
    const bool bHasPendingGroups = PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream;
    if (bHasPendingGroups && ActiveStreamActions.Num() < GroupBudget)
    {
        // A time-sliced launch may have started only one group. Keep filling free slots on the
        // next frame while that group reads/builds, instead of accidentally serializing the wave.
        QueueStreamingBatch();
    }
    else if (!bHasPendingGroups && ActiveStreamActions.IsEmpty())
    {
        FinishStreamingCycle();
    }
}

void AStaticActor::FinishStreamingCycle()
{
    check(IsInGameThread());
    bStreamingBatchQueued = false;
    if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(StreamingBatchTimerHandle);
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    // A settings/focus request or late callback rejection may invalidate an in-flight pass.
    // Do not publish readiness until the current observer set has actually been evaluated.
    bIsLoaded = !bReloadMetadataOnRetry && !StreamingRetry.HasFailedAttempt()
        && !StreamingEvaluation.NeedsEvaluation(GetStreamingEvaluationInputs());
    if (!bIsLoaded) NextStreamingUpdateSeconds = 0.0;
    else StreamingRetry.Reset();
    bAsyncLoading = false;
    LoadingStatus = bIsLoaded ? 1.0f : FMath::Min(LoadingStatus, 0.99f);
    StreamGroupProgress.Empty();
    StreamGroupProgressSum = 0.0;
}

bool AStaticActor::PreserveAbortedWaterState(
    const UWorldSceneStreamAction* Action, FWorldSceneStreamResult&& State)
{
    check(IsInGameThread());
    const auto* Current = ActiveStreamActions.Find(NAME_None);
    if (bIsDestroyed || !Current || Current->Get() != Action) return false;
    WaterNodeMap = MoveTemp(State.WaterNodeMap);
    LoadedWaterNodes = MoveTemp(State.LoadedWaterNodes);
    WaterActorMap = MoveTemp(State.WaterActorMap);
    return true;
}

void AStaticActor::OnStreamCompleted(
    const FWorldSceneStreamResult& Result)
{
    check(IsInGameThread());
    ActiveStreamActions.Remove(Result.GroupName);
    SetStreamGroupProgress(Result.GroupName, 1.0f);
    if (bIsDestroyed)
    {
        return;
    }
    if (Result.bWaterGroup)
    {
        WaterNodeMap = Result.WaterNodeMap;
        LoadedWaterNodes = Result.LoadedWaterNodes;
        WaterActorMap = Result.WaterActorMap;
    }
    if (Result.bGroupFailed)
    {
        const bool bUnavailable = Result.bMeshPayloadUnavailable && !Result.bWaterGroup
            && MeshGroupRecovery.FindOrAdd(Result.GroupName).RecordFailedPass();
        if (bUnavailable)
        {
            // A completed pass exhausted every valid LOD three times. Keep definitions for a
            // fresh world/metadata load, but stop letting this one unusable group freeze the world.
            // Region ownership is per group so another group's pending collision stays protected.
            if (const auto* Group = OwnedInstancedMeshActors.Find(Result.GroupName))
                if (UWorld* World = GetWorld())
                    if (auto* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                        Gate->ClearModelRegions(Group->Get());
            const FString Detail = FString::Printf(
                TEXT("Mesh group unavailable after %d exhausted passes; continuing with missing geometry/collision until scene reload. Static=%s Group=%s Node=%s Mesh=%s Stage=%s Reason=%s"),
                FV3DMeshGroupRecovery::MaxFailedPasses, *ModelReference, *Result.GroupName.ToString(),
                *Result.FailedNode.ToString(), *Result.FailedMesh.ToString(), *Result.FailureStage, *Result.FailureReason);
            WriteLogAsync(Detail);
            UE_LOG(LogTemp, Warning, TEXT("%s"), *Detail);
        }
        else
        {
            RecordStreamingFailure();
            WriteLogAsync(FString::Printf(
                TEXT("Mesh group retained for retry. Static=%s Group=%s Node=%s Mesh=%s Stage=%s Reason=%s RetryIn=%.2fs"),
                *ModelReference, *Result.GroupName.ToString(), *Result.FailedNode.ToString(),
                *Result.FailedMesh.ToString(), *Result.FailureStage, *Result.FailureReason,
                FMath::Max(0.0, StreamingRetry.GetRetryAfterSeconds() - FPlatformTime::Seconds())));
        }
    }
    else
    {
        MeshGroupRecovery.Remove(Result.GroupName);
    }
    const bool bHasPendingGroups =
        PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream;
    if (bHasPendingGroups)
    {
        // Back-fill a completed slot on the next game-thread tick instead of waiting for every
        // action in the current wave. Deferring one tick avoids re-entrant Activate()/Completed()
        // chains for empty groups while keeping archive I/O and native finalizers continuously fed.
        QueueStreamingBatch();
        return;
    }
    if (ActiveStreamActions.IsEmpty())
    {
        FinishStreamingCycle();
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStaticStreamingSchedulingTest,
    "V3DSimulator.Streaming.Memory.StaticSchedulingLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStaticStreamingSchedulingTest::RunTest(const FString&)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Transient streaming test world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Actor = World->SpawnActor<AStaticActor>();
    if (!TestNotNull(TEXT("Transient scene actor"), Actor)) return false;
    Actor->bAsyncLoading = true;
    // Keep one long-running group active while more than one launch slice can examine waits.
    // The next pump must fill spare slots without waiting for this held group to complete.
    Actor->ActiveStreamActions.Add(TEXT("HeldBuild"), NewObject<UWorldSceneStreamAction>(Actor));
    for (int32 Index = 0; Index < 128; ++Index)
        Actor->PendingStreamGroupNames.Add(FName(*FString::Printf(TEXT("Pending_%d"), Index)));
    Actor->LaunchNextStreamingBatch();
    TestTrue(TEXT("A partial launch backfills spare slots while another group remains active"),
        Actor->bStreamingBatchQueued && Actor->ActiveStreamActions.Num() == 1
        && Actor->PendingStreamGroupIndex < Actor->PendingStreamGroupNames.Num());
    const FTimerHandle First = Actor->StreamingBatchTimerHandle;
    for (int32 Completion = 0; Completion < 16; ++Completion) Actor->QueueStreamingBatch();
    TestTrue(TEXT("A wave of completions queues one next-frame pump"),
        Actor->bStreamingBatchQueued && First == Actor->StreamingBatchTimerHandle);
    // A planner that terminates without broadcasting Completed must release its slot.
    auto* HeldAction = Actor->ActiveStreamActions.FindChecked(TEXT("HeldBuild")).Get();
    HeldAction->CancelAndRelease();
    Actor->StartStreamingStep();
    TestTrue(TEXT("Aborted action does not permanently occupy a streaming slot"), Actor->ActiveStreamActions.IsEmpty());
    TestTrue(TEXT("Aborted cycle schedules a bounded retry"), Actor->StreamingRetry.GetRetryAfterSeconds() > FPlatformTime::Seconds());
    TestTrue(TEXT("Aborted cycle leaves readiness invalidated"), !Actor->StreamingEvaluation.HasSnapshot());
    Actor->WorkCancellation = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
    const auto DetachedWorkerToken = Actor->WorkCancellation;
    Actor->ReleaseRuntimeResourcesForWorldExit();
    TestFalse(TEXT("World exit removes the queued pump"), World->GetTimerManager().TimerExists(First));
    TestFalse(TEXT("World exit clears scheduling state"), Actor->bStreamingBatchQueued);
    TestTrue(TEXT("Detached metadata/group workers can stop without touching the actor"),
        DetachedWorkerToken->GetValue() != 0);
    Actor->QueueStreamingBatch();
    TestFalse(TEXT("Teardown cannot schedule a new pump"), Actor->bStreamingBatchQueued);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStaticStreamingRecoveryTest,
    "V3DSimulator.Streaming.Regression.FailureRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStaticStreamingRecoveryTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Recovery world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Scene = World->SpawnActor<AStaticActor>();
    AInstancedMeshActor* Group = World->SpawnActor<AInstancedMeshActor>();
    if (!TestNotNull(TEXT("Scene"), Scene) || !TestNotNull(TEXT("Group"), Group)) return false;
    FModelMeshData Mesh;
    Mesh.LOD0 = 0;
    Mesh.Size = FVector(100);
    const FName GroupName = StaticActorPrivate::MakeMeshGroupName(Mesh);
    FInstancedMeshGroupInitData Definitions;
    FModelNodeData Node;
    Node.MeshName = TEXT("Mesh");
    Definitions.Nodes.Add(TEXT("Node"), Node);
    Definitions.ReferencedMeshNames.Add(Node.MeshName);
    Definitions.NodeBounds = FBox::BuildAABB(FVector::ZeroVector, FVector(50));
    if (!TestTrue(TEXT("Group initializes"), Group->InitializeGroup(GroupName, MoveTemp(Definitions)))) return false;
    Group->SetOwner(Scene);
    Scene->OwnedInstancedMeshActors.Add(GroupName, Group);
    Scene->AllMeshMap.Add(Node.MeshName, Mesh);
    Scene->BakedAsset = NewObject<UWorldBakedModelAsset>();
    Scene->bHasModelMetadata = true;
    Scene->bAsyncLoading = true;
    Scene->StreamingEvaluation.Capture(Scene->GetStreamingEvaluationInputs());
    FWorldSceneStreamResult Failed;
    Failed.GroupName = GroupName;
    Failed.bGroupFailed = true;
    Scene->OnStreamCompleted(Failed);
    TestTrue(TEXT("Failure retains the group actor and immutable placements"),
        IsValid(Group) && Scene->OwnedInstancedMeshActors.FindRef(GroupName).Get() == Group
        && Group->CaptureDefinitions().IsValid() && Group->CaptureDefinitions()->Nodes.Num() == 1);
    TestTrue(TEXT("Failure retains the mesh payload lookup"), Scene->AllMeshMap.Contains(Node.MeshName));
    TestFalse(TEXT("Failure cannot publish a successful scene"), Scene->GetIsLoaded());
    TestTrue(TEXT("Stationary observers are eligible for a new evaluation"),
        Scene->StreamingEvaluation.NeedsEvaluation(Scene->GetStreamingEvaluationInputs()));
    const double Deadline = Scene->StreamingRetry.GetRetryAfterSeconds();
    Scene->StartStreamingStep();
    TestTrue(TEXT("Cooldown prevents an immediate worker burst"),
        Scene->ActiveStreamActions.IsEmpty() && !Scene->bAsyncLoading);
    Scene->OnStreamCompleted(Failed);
    TestEqual(TEXT("Failure wave does not indefinitely extend retry time"),
        Scene->StreamingRetry.GetRetryAfterSeconds(), Deadline);

    Scene->StreamingRetry.BeginAttempt();
    Scene->StreamingEvaluation.Capture(Scene->GetStreamingEvaluationInputs());
    FWorldSceneStreamResult Success;
    Success.GroupName = GroupName;
    Scene->OnStreamCompleted(Success);
    TestTrue(TEXT("Successful retry restores readiness"), Scene->GetIsLoaded());
    TestFalse(TEXT("Successful retry clears backoff"), Scene->StreamingRetry.HasFailedAttempt());

    Scene->MetadataRequestSerial = 7;
    Scene->ModelReference = TEXT("v3d://test-metadata-retry");
    Scene->bAsyncLoading = true;
    Scene->OnBuiltMetadataLoaded(7, Scene->ModelReference, FGuid(), nullptr, false,
        FGWorldModelMetadata(), FGWorldModelManifest(), TEXT("injected metadata read failure"));
    TestFalse(TEXT("Metadata failure never marks an empty scene loaded"), Scene->GetIsLoaded());
    TestTrue(TEXT("Metadata failure schedules a new read"), Scene->bReloadMetadataOnRetry);
    TestFalse(TEXT("Metadata retry releases the active-pass lock"), Scene->bAsyncLoading);
    Scene->ReleaseRuntimeResourcesForWorldExit();
    TestFalse(TEXT("Teardown cancels retries"), Scene->bReloadMetadataOnRetry);
    TestFalse(TEXT("Teardown resets retry timing"), Scene->StreamingRetry.HasFailedAttempt());
    Scene->OnBuiltMetadataLoaded(7, Scene->ModelReference, FGuid(), nullptr, false,
        FGWorldModelMetadata(), FGWorldModelManifest(), TEXT("late callback"));
    TestFalse(TEXT("Late metadata callback cannot restart a released scene"), Scene->bReloadMetadataOnRetry);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStaticMeshFailureIsolationTest,
    "V3DSimulator.Streaming.Regression.TerminalMeshFailureIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStaticMeshFailureIsolationTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Failure isolation world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Scene = World->SpawnActor<AStaticActor>();
    AInstancedMeshActor* Group = World->SpawnActor<AInstancedMeshActor>();
    ACharacter* Character = World->SpawnActor<ACharacter>();
    auto* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>();
    if (!TestNotNull(TEXT("Scene"), Scene) || !TestNotNull(TEXT("Group"), Group)
        || !TestNotNull(TEXT("Character"), Character) || !TestNotNull(TEXT("Gate"), Gate)) return false;
    const FName GroupName(TEXT("BrokenMesh")), NodeName(TEXT("Node")), MeshName(TEXT("Mesh"));
    FInstancedMeshGroupInitData Definitions;
    FModelNodeData Node;
    Node.MeshName = MeshName;
    Definitions.Nodes.Add(NodeName, Node);
    Definitions.NodeBounds = FBox::BuildAABB(FVector::ZeroVector, FVector(50));
    Definitions.MaxNodeScale = 1.0;
    Definitions.ReferencedMeshNames.Add(MeshName);
    auto& Chunk = Definitions.StreamChunks.Add(FIntVector::ZeroValue);
    Chunk.LocalBounds = Definitions.NodeBounds;
    Chunk.Nodes.Add(NodeName);
    if (!TestTrue(TEXT("Group initializes"), Group->InitializeGroup(GroupName, MoveTemp(Definitions)))) return false;
    Group->SetOwner(Scene);
    Scene->OwnedInstancedMeshActors.Add(GroupName, Group);
    FModelMeshData Mesh;
    Mesh.LOD0 = 0; Mesh.Size = FVector(100);
    Scene->AllMeshMap.Add(MeshName, Mesh);
    Scene->BakedAsset = NewObject<UWorldBakedModelAsset>();
    Scene->bHasModelMetadata = true;
    TestFalse(TEXT("Missing in-range geometry initially blocks destination readiness"),
        Scene->IsLocationStreamingReady(FVector::ZeroVector));

    Character->DispatchBeginPlay();
    auto* Movement = Character->GetCharacterMovement();
    const FVector Velocity(120, 30, 0);
    Movement->SetMovementMode(MOVE_Custom, 7);
    Movement->Velocity = Velocity;
    const FBox Bounds(FVector(-1000), FVector(1000));
    TStrongObjectPtr<UObject> OtherGroup(NewObject<UObject>());
    Gate->SetModelRegionAvailable(Group, NodeName, Bounds, false);
    Gate->SetModelRegionAvailable(OtherGroup.Get(), NodeName, Bounds, false);
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("Pending collision pauses movement"), Movement->MovementMode == MOVE_None);

    FWorldSceneStreamResult Failed;
    Failed.GroupName = GroupName;
    Failed.bGroupFailed = true;
    Failed.bMeshPayloadUnavailable = true;
    Failed.FailureStage = TEXT("MeshLoad");
    Failed.FailureReason = TEXT("injected exhausted LOD pass");
    Scene->StreamingEvaluation.Capture(Scene->GetStreamingEvaluationInputs());
    Scene->bAsyncLoading = true;
    Scene->OnStreamCompleted(Failed);
    Scene->StreamingRetry.BeginAttempt();
    Scene->StreamingEvaluation.Capture(Scene->GetStreamingEvaluationInputs());
    FWorldSceneStreamResult Recovered;
    Recovered.GroupName = GroupName;
    Scene->OnStreamCompleted(Recovered);
    TestFalse(TEXT("A successful pass clears this group's earlier failure history"),
        Scene->MeshGroupRecovery.Contains(GroupName));

    for (int32 Pass = 1; Pass <= FV3DMeshGroupRecovery::MaxFailedPasses; ++Pass)
    {
        Scene->StreamingRetry.BeginAttempt();
        Scene->StreamingEvaluation.Capture(Scene->GetStreamingEvaluationInputs());
        Scene->bAsyncLoading = true;
        Scene->OnStreamCompleted(Failed);
        if (Pass < FV3DMeshGroupRecovery::MaxFailedPasses)
        {
            TestFalse(TEXT("Transient failure retains the retry and collision barrier"), Scene->GetIsLoaded());
            TestEqual(TEXT("Transient failure is not unavailable"), Scene->GetUnavailableMeshGroupCount(), 0);
        }
    }
    TestTrue(TEXT("Exhausted geometry no longer blocks scene completion"), Scene->GetIsLoaded());
    TestEqual(TEXT("Degraded state is observable"), Scene->GetUnavailableMeshGroupCount(), 1);
    TestTrue(TEXT("Exhausted geometry no longer blocks destination readiness"),
        Scene->IsLocationStreamingReady(FVector::ZeroVector));
    TestTrue(TEXT("Immutable definitions and mesh metadata are retained for scene reload"),
        Group->CaptureDefinitions().IsValid() && Scene->AllMeshMap.Contains(MeshName));
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("Another group's pending collision is still protected"), Movement->MovementMode == MOVE_None);
    Gate->ClearModelRegions(OtherGroup.Get());
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("Unavailable group's barrier is gone and movement resumes"),
        Movement->MovementMode == MOVE_Custom && Movement->CustomMovementMode == 7);
    TestTrue(TEXT("Resuming restores the original velocity"), Movement->Velocity.Equals(Velocity));

    Scene->StreamingRetry.Reset();
    Scene->StreamingEvaluation.Invalidate();
    Scene->NextStreamingUpdateSeconds = 0;
    Scene->StartStreamingStep();
    TestTrue(TEXT("Stationary refresh skips terminal payloads without launching another action"),
        Scene->GetIsLoaded() && Scene->ActiveStreamActions.IsEmpty());
    Scene->ReleaseRuntimeResourcesForWorldExit();
    TestEqual(TEXT("World teardown resets the per-generation failure state"), Scene->GetUnavailableMeshGroupCount(), 0);
    return true;
}

#endif
