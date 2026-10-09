// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StaticActor.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/StaticActor.h"
#include "System/V3DStreamingPolicy.h"
#include "HAL/PlatformTime.h"
#include "Gravity/GravityFieldComponent.h"

#include "Async/ParallelFor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Model/InstancedMeshActor.h"
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
#include "System/V3DRuntimeSafety.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "TimerManager.h"
#include "World/WaterActor.h"
#include "UObject/UObjectGlobals.h"

namespace StaticActorPrivate
{
    struct FStaticPreparedGroup
    {
        FInstancedMeshGroupInitData Data;
        TSet<FName> MeshNames;
    };

    struct FStaticGroupingWork
    {
        TMap<FName, FModelNodeData> Nodes;
        TMap<FName, FName> MeshToGroup;
        TMap<FName, FInstancedMeshGroupInitData> Groups;
        int32 InvalidNodeCount = 0;
    };

    void BuildStaticGroups(FStaticGroupingWork& Work)
    {
        TMap<FName, FStaticPreparedGroup> PreparedGroups;
        PreparedGroups.Reserve(Work.MeshToGroup.Num());
        Work.Groups.Reset();
        Work.InvalidNodeCount = 0;

        for (TPair<FName, FModelNodeData>& Pair : Work.Nodes)
        {
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
            if (Node.bAlwaysLoaded
                || Node.FineChunk.X < 0 || Node.FineChunk.X >= 16
                || Node.FineChunk.Y < 0 || Node.FineChunk.Y >= 16
                || Node.FineChunk.Z < 0 || Node.FineChunk.Z >= 16)
            {
                Group.Data.AlwaysLoadedNodeNames.Add(Pair.Key);
            }
            else
            {
                Group.Data.SpatialChunks.FindOrAdd(Node.CoarseChunk)
                    .FindOrAdd(Node.FineChunk).Add(Pair.Key);
            }
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
        Work.Nodes.Reset();
        Work.MeshToGroup.Reset();
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
            OutReason = TEXT("model database returned a non-gworld runtime reference");
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
        if (!IsValid(Group)) return false;

        float MaxWorldRadius = 0.0f;
        for (const FName MeshName : Group->GetReferencedMeshNames())
        {
            const FModelMeshData* MeshData = AllMeshMap.Find(MeshName);
            if (!MeshData) return false;
            MaxWorldRadius = FMath::Max(MaxWorldRadius, float(V3DStreamingPolicy::LoadRadius(
                MeshData->Size, Group->GetMaxNodeScale()
                    * OwnerTransform.GetScale3D().GetAbs().GetMax(), EffectiveStreamDistance, MaxDistanceCm)));
        }

        TMap<FName, FModelNodeData> CandidateNodes;
        Group->BuildStreamNodeSnapshot(
            WorldLocation, OwnerTransform, MaxWorldRadius, CandidateNodes);
        TSet<FName> LoadedNodes;
        Group->GetLoadedNodeSnapshot(LoadedNodes);
        for (const TPair<FName, FModelNodeData>& NodePair : CandidateNodes)
        {
            const FModelNodeData& Node = NodePair.Value;
            const FModelMeshData* MeshData = AllMeshMap.Find(Node.MeshName);
            if (!MeshData) return false;
            const FTransform NodeWorldTransform = Node.Transform * OwnerTransform;
            const bool bRequired = V3DStreamingPolicy::MeshInRange(MeshData->Size,
                NodeWorldTransform, WorldLocation, EffectiveStreamDistance, MaxDistanceCm, Node.bAlwaysLoaded);
            if (bRequired && !LoadedNodes.Contains(NodePair.Key)) return false;
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

    StreamingEvaluation.Reset();
    NextStreamingUpdateSeconds = 0.0;
    bIsLoaded = false;
    ModelReference = StaticActorPrivate::NormalizeReference(ModelReference);
    FGuid UUID;
    FString CanonicalReference;
    FString Reason;
    if (ModelReference.IsEmpty()
        || !StaticActorPrivate::ResolveScene(
            this, ModelReference, UUID, CanonicalReference, Reason))
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Static load rejected. Reference=%s Reason=%s"),
            *ModelReference,
            Reason.IsEmpty() ? TEXT("invalid gworld reference") : *Reason));
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
    const bool bQueued = Reader.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, Reader, UUID, RequestSerial, ExpectedReference]() mutable
            {
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

                const bool bSuccess = Results[0] && Results[1];
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
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Static metadata range rejected. Reference=%s Reason=%s"),
            *ModelReference, *Error));
        return;
    }

    // Keep the archive table intact until the prepared facade has validated it. Only the tiny
    // bounds row is copied here so an out-of-range actor can be rejected before any UObject setup.
    ModelMetadata = Metadata.SceneData.ModelData;
    bHasModelMetadata = true;
    LoadingStatus = 0.5f;

    // The outer streamer checks the compact archive summary first. Repeat the exact metadata
    // bounds check here because this deferred actor may have moved while its ranges were read.
    if (!IsPlayerInsideModelRange())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }

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
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
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
        }
    }

    const uint64 RequestSerial = MetadataRequestSerial;
    const FString ExpectedReference = ModelReference;
    TWeakObjectPtr<AStaticActor> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Work, RequestSerial, ExpectedReference]() mutable
        {
            StaticActorPrivate::BuildStaticGroups(*Work);
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
        bAsyncLoading = false;
        UE_LOG(LogTemp, Verbose, TEXT("Static grouping skipped because the worker queue is shutting down."));
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
        FinishBuildInstancedMeshActors();
        return;
    }

    // These are lightweight group actors; payload concurrency is governed separately.
    // Fill a time slice instead of delaying every few cheap groups by an entire frame.
    const double Deadline = FPlatformTime::Seconds() + 0.002;
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
        PendingInstancedGroups.Remove(GroupName);

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
        if (!bInitialized
            || !IsValid(InstancedActor)
            || !InstancedActor->AttachToActor(
                this, FAttachmentTransformRules::KeepWorldTransform))
        {
            if (IsValid(InstancedActor))
            {
                InstancedActor->Destroy();
            }
            continue;
        }
        OwnedInstancedMeshActors.Add(GroupName, InstancedActor);
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
            Actor->ReleaseRuntimeResources();
            Actor->Destroy();
        }
    }
    OwnedInstancedMeshActors.Empty();
}

void AStaticActor::RemoveInstancedMeshGroup(const FName GroupName)
{
    check(IsInGameThread());
    if (TObjectPtr<AInstancedMeshActor>* Found =
            OwnedInstancedMeshActors.Find(GroupName))
    {
        if (IsValid(Found->Get()))
        {
            Found->Get()->ReleaseRuntimeResources();
            Found->Get()->Destroy();
        }
    }
    OwnedInstancedMeshActors.Remove(GroupName);
    PruneUnreferencedMeshMetadata();
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
}

void AStaticActor::OnStreamProgress(const FName GroupName, const float Progress)
{
    check(IsInGameThread());
    SetStreamGroupProgress(GroupName, Progress);
}

bool AStaticActor::IsPlayerInsideModelRange() const
{
    check(IsInGameThread());
    if (GravityField->GetSettingsRef().bEnabled || !bHasModelMetadata || ModelMetadata.Size.IsNearlyZero(0.001f))
    {
        return true;
    }

    TArray<FVector> ObserverLocations;
    if (UWorldSceneStreamingSubsystem* Streamer =
            UWorldSceneStreamingSubsystem::Get(const_cast<AStaticActor*>(this)))
    {
        Streamer->GetStreamingObserverLocations(ObserverLocations);
    }
    else if (UGameManagerSubSystem* Manager =
            UGameManagerSubSystem::GetSubSystem(
                const_cast<AStaticActor*>(this)))
    {
        ObserverLocations.Add(Manager->GetPlayerLocation());
    }
    if (ObserverLocations.IsEmpty()) ObserverLocations.Add(FVector::ZeroVector);
    const float ScreenDistance = V3DStreamingPolicy::GetScreenSizeDistance(this);
    const double MaxDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(this);
    for (const FVector& Observer : ObserverLocations)
    {
        if (V3DStreamingPolicy::SceneInRange(ModelMetadata.Center, ModelMetadata.Size,
            GetActorTransform(), Observer, ScreenDistance, 1.0, MaxDistanceCm)) return true;
    }
    return false;
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
    Config.bBuildNavCollision = !bRenderOnlyStreaming;
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

void AStaticActor::StartStreamingStep()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        UnregisterGameUpdate();
        return;
    }
    // Metadata/group construction and active payload work own this cycle until completion.
    if (bAsyncLoading) return;
    if (!IsValid(BakedAsset))
    {
        bAsyncLoading = false;
        bIsLoaded = true;
        LoadingStatus = 1.0f;
        UnregisterGameUpdate();
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

    const double Now = FPlatformTime::Seconds();
    if (Now < NextStreamingUpdateSeconds) return;
    double UpdateInterval = 0.08;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
        if (const UGameSettings* Settings = Manager->GetGameSettings())
            UpdateInterval = Settings->GetStreamingUpdateIntervalSeconds();
    NextStreamingUpdateSeconds = Now + UpdateInterval;

    FV3DStreamingEvaluationInputs Inputs = GetStreamingEvaluationInputs();
    if (!StreamingEvaluation.NeedsEvaluation(Inputs)) return;
    StreamingEvaluation.Capture(MoveTemp(Inputs));

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

void AStaticActor::LaunchNextStreamingBatch()
{
    check(IsInGameThread());
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
            // Unloaded groups outside a conservative bound need no action/snapshot/worker.
            // Loaded groups must still run so instances leaving the range can be released.
            if (!bWaterGroup && !InstancedActor->HasLoadedNodes()
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
                        const double MeshRadius = InstancedActor->HasAlwaysLoadedNodes()
                            ? MaxDistanceCm + 0.5 * Mesh->Size.GetAbs().Size() * MaxScale
                            : V3DStreamingPolicy::LoadRadius(Mesh->Size, MaxScale,
                                EffectiveStreamDistance, MaxDistanceCm);
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
                    EffectiveStreamDistance,
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
    const double LaunchDeadline = FPlatformTime::Seconds() + 0.0015;
    TArray<FName> FailedGroups;
    while (LaunchedThisBatch < AvailableSlots && ExaminedThisBatch++ < 64
        && FPlatformTime::Seconds() < LaunchDeadline
        && PendingStreamGroupIndex < PendingStreamGroupNames.Num())
    {
        const FName GroupName = PendingStreamGroupNames[PendingStreamGroupIndex++];
        TObjectPtr<AInstancedMeshActor>* Found = OwnedInstancedMeshActors.Find(GroupName);
        AInstancedMeshActor* InstancedActor = Found ? Found->Get() : nullptr;
        if (!IsValid(InstancedActor)
            || InstancedActor->GetOwner() != this
            || !StartGroup(InstancedActor, false))
        {
            FailedGroups.Add(GroupName);
            SetStreamGroupProgress(GroupName, 1.0f);
            continue;
        }
        if (ActiveStreamActions.Contains(GroupName)) ++LaunchedThisBatch;
    }
    for (const FName GroupName : FailedGroups)
    {
        RemoveInstancedMeshGroup(GroupName);
    }

    if (LaunchedThisBatch < AvailableSlots && bPendingWaterStream)
    {
        bPendingWaterStream = false;
        if (!StartGroup(nullptr, true))
        {
            WaterNodeMap.Empty();
            LoadedWaterNodes.Empty();
            WaterActorMap.Empty();
            SetStreamGroupProgress(NAME_None, 1.0f);
        }
        else
        {
            ++LaunchedThisBatch;
        }
    }

    if (ActiveStreamActions.IsEmpty())
    {
        if (PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream)
        {
            if (UWorld* World = GetWorld())
            {
                World->GetTimerManager().SetTimerForNextTick(
                    this, &AStaticActor::LaunchNextStreamingBatch);
            }
            else
            {
                LaunchNextStreamingBatch();
            }
            return;
        }
        FinishStreamingCycle();
    }
}

void AStaticActor::FinishStreamingCycle()
{
    check(IsInGameThread());
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    // A settings/focus request or late callback rejection may invalidate an in-flight pass.
    // Do not publish readiness until the current observer set has actually been evaluated.
    bIsLoaded = !StreamingEvaluation.NeedsEvaluation(GetStreamingEvaluationInputs());
    if (!bIsLoaded) NextStreamingUpdateSeconds = 0.0;
    bAsyncLoading = false;
    LoadingStatus = 1.0f;
    StreamGroupProgress.Empty();
    StreamGroupProgressSum = 0.0;
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
    else if (Result.bGroupFailed)
    {
        RemoveInstancedMeshGroup(Result.GroupName);
        WriteLogAsync(FString::Printf(
            TEXT("Failed mesh group isolated. Static=%s Mesh=%s"),
            *ModelReference, *Result.GroupName.ToString()));
    }
    const bool bHasPendingGroups =
        PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream;
    if (bHasPendingGroups)
    {
        // Back-fill a completed slot on the next game-thread tick instead of waiting for every
        // action in the current wave. Deferring one tick avoids re-entrant Activate()/Completed()
        // chains for empty groups while keeping archive I/O and native finalizers continuously fed.
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimerForNextTick(
                this, &AStaticActor::LaunchNextStreamingBatch);
        }
        else
        {
            LaunchNextStreamingBatch();
        }
        return;
    }
    if (ActiveStreamActions.IsEmpty())
    {
        FinishStreamingCycle();
    }
}
