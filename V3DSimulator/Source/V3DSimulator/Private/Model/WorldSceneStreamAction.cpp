// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldSceneStreamAction.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/WorldSceneStreamAction.h"
#include "System/WorldSurfaceSubsystem.h"
#include "Model/WorldSceneStreamingSubsystem.h"
#include "System/V3DStreamingPolicy.h"
#include "System/V3DStreamingBudget.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Setting/GameSettings.h"
#include "Model/StaticActor.h"
#include "Model/InstancedMeshActor.h"
#include "Model/V3DHISMChunkPolicy.h"
#include "Async/ParallelFor.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "System/SafeFileIO.h"
#include "System/WorldBakedModelAsset.h"
#include "Misc/ScopeExit.h"
#include "System/V3DRuntimeSafety.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "World/WaterActor.h"

struct FWorldSceneStreamPlan
{
    TMap<FName, FModelNodeData> NodeMap;
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;
    TMap<FName, FModelMeshData> MeshMap;
    TSet<FName> LoadedWaterNodes;
    TArray<FName> InvalidWaterNodes;
    TArray<FName> PendingLoadNodes;
    TArray<FName> PendingUnloadNodes;
    TArray<FName> PendingLoadWaterNodes;
    TArray<FName> PendingUnloadWaterNodes;
    TArray<FWorldSceneUnavailableRegion> UnavailableRegions;
    int32 OriginalNodeWorkCount = 0;
};

namespace
{
    // The baked facade builds detached runtime LODs on a worker and uses the guarded finalizer.
    // HISMC cannot select authored LODs if the shared UStaticMesh contains only LOD0.
    constexpr int32 DefaultRuntimeMultiLOD = 1;

    TAutoConsoleVariable<int32> CVarEnableRuntimeMultiLOD(
        TEXT("v3d.Streaming.EnableRuntimeMultiLOD"),
        DefaultRuntimeMultiLOD,
        TEXT("Enables glTFRuntime multi-LOD mesh assembly. 0 loads one validated LOD (prefers LOD0); 1 enables LOD1-LOD3."),
        ECVF_Default);

    /** Every UObject/component mutation in this action is intentionally game-thread-only. */
    bool EnsureStreamActionGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    bool ConfigureGeneratedMeshCollision(
        UInstancedStaticMeshComponent* Component,
        const FMeshData* MeshData,
        const bool bRenderOnly)
    {
        if (!IsValid(Component))
        {
            return false;
        }

        const bool bEnableCollision =
            !bRenderOnly && MeshData && (MeshData->bComplexCollision || MeshData->bSimpleCollision);

        Component->SetGenerateOverlapEvents(false);
        Component->SetCanEverAffectNavigation(bEnableCollision);
        if (!bEnableCollision)
        {
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetCollisionResponseToAllChannels(ECR_Ignore);
            return false;
        }

        Component->SetCollisionProfileName(TEXT("BlockAll"));
        Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Component->SetCollisionObjectType(ECC_WorldStatic);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        return true;
    }

    void BuildNativeStreamPlan(
        FWorldSceneStreamPlan& Plan,
        const TSet<FName>& LoadedMeshNodes,
        const FTransform& OwnerWorldTransform,
        const TArray<FVector>& ObserverLocations,
        const float Distance,
        const double MaxDistanceCm,
        const float UnloadDistanceMultiplier,
        const bool bRenderOnly,
        const TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe>& Cancellation)
    {
        uint32 Examined = 0;
        const auto IsCancelled = [&Cancellation, &Examined]()
        {
            return (Examined++ & 255) == 0 && Cancellation.IsValid() && Cancellation->GetValue() != 0;
        };
        // Everything in this routine is detached native data. It is intentionally safe to execute
        // on a worker and must never dereference a UObject or Actor.
        Plan.InvalidWaterNodes.Reset();
        for (auto It = Plan.WaterNodeMap.CreateIterator(); It; ++It)
        {
            if (IsCancelled()) return;
            const FWaterStreamNodeData& Water = It.Value();
            if (It.Key().IsNone() || Water.Transform.ContainsNaN()
                || !FMath::IsFinite(Water.StreamRadius) || Water.StreamRadius <= 0.0f)
            {
                Plan.InvalidWaterNodes.Add(It.Key());
                Plan.LoadedWaterNodes.Remove(It.Key());
                It.RemoveCurrent();
            }
        }
        for (auto It = Plan.LoadedWaterNodes.CreateIterator(); It; ++It)
        {
            if (IsCancelled()) return;
            if (!Plan.WaterNodeMap.Contains(*It)) It.RemoveCurrent();
        }

        TSet<FName> ReferencedMeshes;
        ReferencedMeshes.Reserve(Plan.MeshMap.Num());
        for (auto It = Plan.NodeMap.CreateIterator(); It; ++It)
        {
            if (IsCancelled()) return;
            const FModelNodeData& Node = It.Value();
            if (It.Key().IsNone() || Node.MeshName.IsNone() || Node.Transform.ContainsNaN())
            {
                It.RemoveCurrent();
                continue;
            }
            ReferencedMeshes.Add(Node.MeshName);
        }

        for (auto It = Plan.MeshMap.CreateIterator(); It; ++It)
        {
            if (IsCancelled()) return;
            const FModelMeshData& Mesh = It.Value();
            const bool bHasRuntimeMesh = Mesh.LOD0 != INDEX_NONE || Mesh.LOD1 != INDEX_NONE
                || Mesh.LOD2 != INDEX_NONE || Mesh.LOD3 != INDEX_NONE;
            if (!ReferencedMeshes.Contains(It.Key()) || !bHasRuntimeMesh || Mesh.Size.ContainsNaN())
            {
                It.RemoveCurrent();
            }
        }
        for (auto It = Plan.NodeMap.CreateIterator(); It; ++It)
        {
            if (IsCancelled()) return;
            if (!Plan.MeshMap.Contains(It.Value().MeshName)) It.RemoveCurrent();
        }

        Plan.PendingLoadNodes.Reset();
        Plan.PendingUnloadNodes.Reset();
        Plan.PendingLoadWaterNodes.Reset();
        Plan.PendingUnloadWaterNodes.Reset();
        Plan.UnavailableRegions.Reset();
        Plan.PendingLoadNodes.Reserve(FMath::Min(Plan.NodeMap.Num(), 256));
        Plan.PendingUnloadNodes.Reserve(LoadedMeshNodes.Num());
        Plan.UnavailableRegions.Reserve(FMath::Min(Plan.NodeMap.Num(), 256));
        TMap<FName, double> NodeDistanceSqCache;
        NodeDistanceSqCache.Reserve(FMath::Min(Plan.NodeMap.Num(), 256));
        TMap<FName, double> WaterDistanceSqCache;
        WaterDistanceSqCache.Reserve(Plan.WaterNodeMap.Num());

        const float SafeUnloadMultiplier = FMath::Max(1.0f, UnloadDistanceMultiplier);
        const auto NearestObserverDistanceSq = [&ObserverLocations](const FVector& WorldLocation)
        {
            double BestDistanceSq = TNumericLimits<double>::Max();
            for (const FVector& Observer : ObserverLocations)
            {
                BestDistanceSq = FMath::Min(
                    BestDistanceSq, static_cast<double>(FVector::DistSquared(Observer, WorldLocation)));
            }
            return BestDistanceSq == TNumericLimits<double>::Max()
                ? TNumericLimits<double>::Max() : BestDistanceSq;
        };
        struct FChunkSelection
        {
            FBox Bounds = FBox(ForceInit);
            double Radius = 0.0;
            bool bResident = false;
            bool bSelected = false;
            double DistanceSq = 0.0;
        };
        TMap<FIntVector, FChunkSelection> Chunks;
        for (const auto& Pair : Plan.NodeMap)
        {
            if (IsCancelled()) return;
            const FModelNodeData& Node = Pair.Value;
            const FModelMeshData* Mesh = Plan.MeshMap.Find(Node.MeshName);
            if (!Mesh) continue;
            const FTransform Transform = Node.Transform * OwnerWorldTransform;
            const FIntVector Key = V3DHISMChunkPolicy::Key(Transform.GetLocation());
            FChunkSelection& Chunk = Chunks.FindOrAdd(Key);
            Chunk.Bounds += V3DHISMChunkPolicy::CellBounds(Key);
            const FVector Half = Mesh->Size.GetAbs() * 0.5;
            Chunk.Bounds += FBox(-Half, Half).TransformBy(Transform);
            Chunk.Radius = FMath::Max(Chunk.Radius, FMath::Max(
                FMath::Min(MaxDistanceCm, V3DHISMChunkPolicy::ChunkSizeCm),
                V3DStreamingPolicy::LoadRadius(Mesh->Size, Transform.GetScale3D().GetAbs().GetMax(), Distance, MaxDistanceCm)));
            if (Node.bAlwaysLoaded) Chunk.Radius = FMath::Max(Chunk.Radius, MaxDistanceCm);
            Chunk.bResident |= LoadedMeshNodes.Contains(Pair.Key);
        }
        for (auto& Pair : Chunks)
        {
            if (IsCancelled()) return;
            FChunkSelection& Chunk = Pair.Value;
            Chunk.bSelected = V3DHISMChunkPolicy::InRange(Chunk.Bounds, ObserverLocations,
                FMath::Min(MaxDistanceCm, Chunk.Radius * (Chunk.bResident ? SafeUnloadMultiplier : 1.0f)));
            Chunk.DistanceSq = NearestObserverDistanceSq(Chunk.Bounds.GetCenter());
        }
        for (const TPair<FName, FModelNodeData>& NodePair : Plan.NodeMap)
        {
            if (IsCancelled()) return;
            const FModelNodeData& Info = NodePair.Value;
            const FModelMeshData* MeshPtr = Plan.MeshMap.Find(Info.MeshName);
            if (!MeshPtr) continue;

            const FTransform WorldTransform = Info.Transform * OwnerWorldTransform;
            const bool bIsLoaded = LoadedMeshNodes.Contains(NodePair.Key);
            const FChunkSelection& Chunk = Chunks.FindChecked(V3DHISMChunkPolicy::Key(WorldTransform.GetLocation()));
            const bool bChunkInRange = Chunk.bSelected;
            // Residency is chosen for the whole 1024 m chunk. At the outer boundary keep the
            // requested hard cap: a mesh whose entire bounds are outside it must never spawn.
            bool bInRange = false;
            if (bChunkInRange)
                for (const FVector& Observer : ObserverLocations)
                    if (V3DStreamingPolicy::BoundsInRenderRange(FVector::ZeroVector, MeshPtr->Size,
                        WorldTransform, Observer, MaxDistanceCm)) { bInRange = true; break; }
            if (bIsLoaded != bInRange)
                NodeDistanceSqCache.Add(NodePair.Key, NearestObserverDistanceSq(WorldTransform.GetLocation()));

            if (bIsLoaded)
            {
                if (!bInRange)
                    Plan.PendingUnloadNodes.Add(NodePair.Key);
                continue;
            }

            if (!bRenderOnly && (MeshPtr->Data.bComplexCollision || MeshPtr->Data.bSimpleCollision))
            {
                FWorldSceneUnavailableRegion& Region = Plan.UnavailableRegions.AddDefaulted_GetRef();
                Region.NodeName = NodePair.Key;
                const FVector Extent = (MeshPtr->Size * 0.5f + BOX_BUFFER_SIZE).GetAbs();
                Region.WorldBounds = FBox(-Extent, Extent).TransformBy(WorldTransform);
            }
            if (bInRange)
                Plan.PendingLoadNodes.Add(NodePair.Key);
        }

        const float WaterDistanceRadius = FMath::Max(Distance * 1024.0f, 2048.0f);
        for (const TPair<FName, FWaterStreamNodeData>& WaterPair : Plan.WaterNodeMap)
        {
            if (IsCancelled()) return;
            const FVector WaterWorldLocation =
                (WaterPair.Value.Transform * OwnerWorldTransform).GetLocation();
            const double CurrentDist = NearestObserverDistanceSq(WaterWorldLocation);
            WaterDistanceSqCache.Add(WaterPair.Key, CurrentDist);
            const float LoadRadius = FMath::Max(WaterPair.Value.StreamRadius, WaterDistanceRadius);
            const float LoadRadiusSq = FMath::Square(LoadRadius);
            const float UnloadRadiusSq = LoadRadiusSq * FMath::Square(SafeUnloadMultiplier);
            const double RenderRadius = MaxDistanceCm
                + WaterPair.Value.StreamRadius * OwnerWorldTransform.GetScale3D().GetAbs().GetMax();
            const bool bInRenderRange = CurrentDist <= FMath::Square(RenderRadius);
            if (Plan.LoadedWaterNodes.Contains(WaterPair.Key))
            {
                if (!bInRenderRange || CurrentDist > UnloadRadiusSq) Plan.PendingUnloadWaterNodes.Add(WaterPair.Key);
            }
            else if (bInRenderRange && CurrentDist <= LoadRadiusSq)
            {
                Plan.PendingLoadWaterNodes.Add(WaterPair.Key);
            }
        }
        if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
        const auto ChunkOrder = [&Plan, &Chunks, &OwnerWorldTransform, &NodeDistanceSqCache](FName A, FName B, bool bUnload)
        {
            const FIntVector CA = V3DHISMChunkPolicy::Key((Plan.NodeMap.FindChecked(A).Transform * OwnerWorldTransform).GetLocation());
            const FIntVector CB = V3DHISMChunkPolicy::Key((Plan.NodeMap.FindChecked(B).Transform * OwnerWorldTransform).GetLocation());
            if (CA != CB)
            {
                const double DA = Chunks.FindChecked(CA).DistanceSq;
                const double DB = Chunks.FindChecked(CB).DistanceSq;
                if (DA != DB) return bUnload ? DA > DB : DA < DB;
                if (CA.X != CB.X) return CA.X < CB.X;
                if (CA.Y != CB.Y) return CA.Y < CB.Y;
                return CA.Z < CB.Z;
            }
            const double DA = NodeDistanceSqCache.FindChecked(A), DB = NodeDistanceSqCache.FindChecked(B);
            return DA != DB ? (bUnload ? DA > DB : DA < DB) : A.LexicalLess(B);
        };
        Plan.PendingLoadNodes.Sort([&ChunkOrder](FName A, FName B) { return ChunkOrder(A, B, false); });
        Plan.PendingUnloadNodes.Sort([&ChunkOrder](FName A, FName B) { return ChunkOrder(A, B, true); });
        Plan.PendingLoadWaterNodes.Sort([&WaterDistanceSqCache](const FName A, const FName B) { return WaterDistanceSqCache.FindChecked(A) < WaterDistanceSqCache.FindChecked(B); });
        Plan.PendingUnloadWaterNodes.Sort([&WaterDistanceSqCache](const FName A, const FName B) { return WaterDistanceSqCache.FindChecked(A) > WaterDistanceSqCache.FindChecked(B); });
    }
}

UWorldSceneStreamAction *UWorldSceneStreamAction::StreamAsync(
    UObject *WorldContextObject,
    AStaticActor *Actor,
    AInstancedMeshActor *InMeshActor,
    const FVector &InPlayerLocation,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    int32 InChunkSize,
    bool bInRenderOnly,
    bool bInWaterGroup,
    float InUnloadDistanceMultiplier)
{
    TArray<FVector> Observers;
    Observers.Add(InPlayerLocation);
    return StreamAsyncForObservers(
        WorldContextObject, Actor, InMeshActor, Observers, StaticMeshConfig,
        InChunkSize, bInRenderOnly, bInWaterGroup, InUnloadDistanceMultiplier);
}

UWorldSceneStreamAction* UWorldSceneStreamAction::StreamAsyncForObservers(
    UObject* WorldContextObject,
    AStaticActor* Actor,
    AInstancedMeshActor* InMeshActor,
    const TArray<FVector>& InObserverLocations,
    const FglTFRuntimeStaticMeshConfig& StaticMeshConfig,
    int32 InChunkSize,
    bool bInRenderOnly,
    bool bInWaterGroup,
    float InUnloadDistanceMultiplier)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::StreamAsyncForObservers")))
    {
        return nullptr;
    }

    TArray<FVector> ValidObservers;
    ValidObservers.Reserve(InObserverLocations.Num());
    for (const FVector& Observer : InObserverLocations)
    {
        if (!Observer.ContainsNaN()
            && FMath::IsFinite(Observer.X) && FMath::IsFinite(Observer.Y) && FMath::IsFinite(Observer.Z))
        {
            ValidObservers.Add(Observer);
        }
    }

    if (!IsValid(Actor)
        || (bInWaterGroup ? IsValid(InMeshActor) : !IsValid(InMeshActor))
        || (!bInWaterGroup && InMeshActor->GetOwner() != Actor)
        || ValidObservers.IsEmpty()
        || InChunkSize <= 0
        || !FMath::IsFinite(InUnloadDistanceMultiplier) || InUnloadDistanceMultiplier < 1.0f
        || Actor->GetActorTransform().ContainsNaN())
    {
        return nullptr;
    }

    const float InDistance = V3DStreamingPolicy::GetScreenSizeDistance(Actor);

    auto* Action = NewObject<UWorldSceneStreamAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->OwnerActor = Actor;
    Action->MeshActor = InMeshActor;
    Action->DecalLight = Actor->GetDecalLight();
    Action->Asset = Actor->GetBakedAsset();
    Action->WaterClass = Actor->GetWaterClass();
    Action->bWaterGroup = bInWaterGroup;
    Action->ObserverLocations = ValidObservers;
    Action->MaxRenderDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(Actor);
    if (bInWaterGroup)
    {
        Action->GroupName = NAME_None;
        Action->WaterNodeMap = Actor->GetWaterNodeMapRef();
        Action->LoadedWaterNodes = Actor->GetLoadedWaterNodesRef();
        Action->WaterActorMap = Actor->GetWaterActorMapRef();
    }
    else
    {
        Action->GroupName = InMeshActor->GetGroupName();
        float MaxWorldRadius = static_cast<float>(FMath::Min(Action->MaxRenderDistanceCm, V3DHISMChunkPolicy::ChunkSizeCm));
        for (const FName MeshName : InMeshActor->GetReferencedMeshNames())
        {
            const FModelMeshData* MeshData = Actor->GetAllMeshMapRef().Find(MeshName);
            if (!MeshData) return nullptr;
            Action->MeshMap.Add(MeshName, *MeshData);
            MaxWorldRadius = FMath::Max(MaxWorldRadius, float(V3DStreamingPolicy::LoadRadius(
                MeshData->Size, InMeshActor->GetMaxNodeScale()
                    * Actor->GetActorScale3D().GetAbs().GetMax(), InDistance, Action->MaxRenderDistanceCm)));
        }

        // Snapshot construction and full-cell node copying happen on the planning worker.
        Action->PlanningWorldRadius = MaxWorldRadius * FMath::Clamp(InUnloadDistanceMultiplier, 1.0f, 2.0f);
    }
    Action->Distance = InDistance;
    Action->UnloadDistanceMultiplier = FMath::Clamp(InUnloadDistanceMultiplier, 1.0f, 2.0f);
    Action->ChunkSize = InChunkSize;
    Action->StaticMeshConfig = StaticMeshConfig;
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Action->MaterialReferenceGuard = GameManager->AcquireMaterialDefaultReferenceGuard();
        if (const UGameSettings* Settings = GameManager->GetGameSettings()) Action->FrameTimeBudgetMs = Settings->GetStreamingFrameTimeBudgetMs();
    }
    Action->FrameTimeBudgetMs = FMath::Clamp(Action->FrameTimeBudgetMs, 0.25f, 4.0f);
    Action->bRenderOnly = bInRenderOnly;
    Action->AddToRoot();
    return Action;
}


void UWorldSceneStreamAction::CancelAndRelease()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::CancelAndRelease")))
    {
        return;
    }

    AbortAndRelease();
}

void UWorldSceneStreamAction::AbortAndRelease(UStaticMesh* OrphanedMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::AbortAndRelease")))
    {
        return;
    }

    bAbortRequested = true;
    // Water actions own a partial spawn/unload result until completion. Preserve that result
    // for a live retry; during teardown destroy it now so newly spawned water cannot be orphaned.
    if (bWaterGroup && TotalOperationCount > 0)
    {
        FWorldSceneStreamResult WaterState;
        WaterState.WaterNodeMap = MoveTemp(WaterNodeMap);
        WaterState.LoadedWaterNodes = MoveTemp(LoadedWaterNodes);
        WaterState.WaterActorMap = MoveTemp(WaterActorMap);
        if (!IsValid(OwnerActor)
            || !OwnerActor->PreserveAbortedWaterState(this, MoveTemp(WaterState)))
            for (auto& Pair : WaterState.WaterActorMap)
                if (IsValid(Pair.Value)) Pair.Value->Destroy();
    }
    bPreparationInFlight = false;
    if (PreparationCancellation.IsValid()) PreparationCancellation->Increment();
    PreparationCancellation.Reset();
    ++PreparationSerial;

    UWorld* World = nullptr;
    if (IsValid(OwnerActor))
    {
        World = OwnerActor->GetWorld();
    }
    else if (IsValid(WorldContextObject))
    {
        World = WorldContextObject->GetWorld();
    }

    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    Progress.Clear();
    Completed.Clear();
    if (IsValid(GeneratedMeshWorldContext)) GeneratedMeshWorldContext->CancelBuild();

    // Planning rows and actor references are no longer needed by a cancelled terminal callback.
    // Release them immediately even if an admitted read/native build still needs its own pins.
    NodeMap.Empty();
    WaterNodeMap.Empty();
    MeshMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    PendingUnavailableRegions.Empty();
    PreparedInstances.Empty();
    PreparedInstanceOffset = 0;
    bInstancePreparationInFlight = false;
    PendingLoadNodes.Empty();
    PendingUnloadNodes.Empty();
    PendingLoadWaterNodes.Empty();
    PendingUnloadWaterNodes.Empty();
    bRetryMeshLoadPending = false;
    MeshLoadRecovery.Reset();
    MeshActor = nullptr;
    DecalLight = nullptr;
    ObserverLocations.Empty();
    CurrentRenderObservers.Empty();

    // Do not release the facade/action while its .dat read or mesh finalizer is still active.
    // SetStaticMesh receives the terminal callback and completes deferred cleanup.
    if (bStaticMeshLoadInFlight && OrphanedMesh == nullptr)
    {
        OwnerActor = nullptr;
        WorldContextObject = nullptr;
        return;
    }

    bStaticMeshLoadInFlight = false;
    if (IsValid(OrphanedMesh) && !OrphanedMesh->IsAsset())
    {
        OrphanedMesh->ClearFlags(RF_Public | RF_Standalone);
    }

    CurrentUnavailableRegionIndex = 0;
    TotalOperationCount = 0;
    TotalSkippedOperationCount = 0;
    CurrentSkippedOperationIndex = 0;
    SkippedProgressChunkSize = 1;

    bWaitingForMeshCallback = false;
    CurrentRenderObservers.Empty();
    ObserverLocations.Empty();
    Asset = nullptr;
    OwnerActor = nullptr;
    MeshActor = nullptr;
    WorldContextObject = nullptr;
    MaterialReferenceGuard = nullptr;
    if (IsValid(GeneratedMeshWorldContext))
    {
        GeneratedMeshWorldContext->ReleaseWorldPin();
    }
    GeneratedMeshWorldContext = nullptr;
    DecalLight = nullptr;
    WaterClass = nullptr;
    StaticMeshConfig = FglTFRuntimeStaticMeshConfig();
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bRenderOnly = false;
    ObserverLocations.Empty();
    if (IsRooted())
    {
        RemoveFromRoot();
    }
    FV3DStreamingBudget::NotifyUnloaded();
}

void UWorldSceneStreamAction::Activate()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::Activate"))) return;
    if (bActivated || bAbortRequested) return;
    bActivated = true;

    bAbortRequested = false;
    bGroupFailed = false;
    bStaticMeshLoadInFlight = false;
    if (!IsValid(OwnerActor) || !IsValid(WorldContextObject)
        || (!bWaterGroup && !IsValid(MeshActor)))
    {
        AbortAndRelease();
        return;
    }

    StartStreamPlanAsync();
}

void UWorldSceneStreamAction::StartStreamPlanAsync()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::StartStreamPlanAsync"))) return;
    if (bAbortRequested || bPreparationInFlight || !IsValid(OwnerActor)
        || (!bWaterGroup && !IsValid(MeshActor)))
    {
        AbortAndRelease();
        return;
    }

    FWorldSceneStreamPlan Plan;
    Plan.OriginalNodeWorkCount = NodeMap.Num() + WaterNodeMap.Num();
    Plan.NodeMap = MoveTemp(NodeMap);
    Plan.WaterNodeMap = MoveTemp(WaterNodeMap);
    Plan.MeshMap = MoveTemp(MeshMap);
    Plan.LoadedWaterNodes = MoveTemp(LoadedWaterNodes);

    const auto Definitions = IsValid(MeshActor) ? MeshActor->CaptureDefinitions()
        : TSharedPtr<const FInstancedMeshGroupInitData, ESPMode::ThreadSafe>();
    const auto LoadedMeshNodes = IsValid(MeshActor) ? MeshActor->CaptureLoadedNodes()
        : TSharedPtr<const TSet<FName>, ESPMode::ThreadSafe>();
    const float PlanningRadius = PlanningWorldRadius;
    const FTransform OwnerTransform = OwnerActor->GetActorTransform();
    const TArray<FVector> PlanningObserverLocations = ObserverLocations;
    const float PlanningDistance = Distance;
    const double PlanningMaxDistanceCm = MaxRenderDistanceCm;
    const float PlanningUnloadMultiplier = UnloadDistanceMultiplier;
    const bool bPlanningRenderOnly = bRenderOnly;
    const uint32 Serial = ++PreparationSerial;
    PreparationCancellation = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
    const auto Cancellation = PreparationCancellation;
    bPreparationInFlight = true;
    if (!Definitions.IsValid() && Plan.NodeMap.IsEmpty() && Plan.WaterNodeMap.IsEmpty())
    {
        // No I/O/UObject work exists for this group. Do not spend a worker dispatch and a
        // whole frame occupying a planning slot just to return an empty plan.
        ApplyPreparedPlan(MoveTemp(Plan), Serial);
        return;
    }
    TWeakObjectPtr<UWorldSceneStreamAction> WeakThis(this);

    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Serial, Plan = MoveTemp(Plan), Definitions, LoadedMeshNodes, PlanningRadius,
            OwnerTransform, PlanningObserverLocations, PlanningDistance, PlanningMaxDistanceCm, PlanningUnloadMultiplier, bPlanningRenderOnly, Cancellation]() mutable
        {
            if (Cancellation->GetValue() != 0) return;
            const TSet<FName> EmptyLoaded;
            const TSet<FName>& Loaded = LoadedMeshNodes.IsValid() ? *LoadedMeshNodes : EmptyLoaded;
            if (Definitions.IsValid())
            {
                TMap<FName, FModelNodeData> ObserverNodes;
                for (const FVector& Observer : PlanningObserverLocations)
                {
                    if (Cancellation->GetValue() != 0) return;
                    AInstancedMeshActor::BuildNativeNodeSnapshot(*Definitions, Loaded, Observer,
                        OwnerTransform, PlanningRadius, ObserverNodes, PlanningMaxDistanceCm);
                    Plan.NodeMap.Append(ObserverNodes);
                }
            }
            Plan.OriginalNodeWorkCount = Plan.NodeMap.Num() + Plan.WaterNodeMap.Num();
            BuildNativeStreamPlan(Plan, Loaded, OwnerTransform, PlanningObserverLocations,
                PlanningDistance, PlanningMaxDistanceCm, PlanningUnloadMultiplier, bPlanningRenderOnly, Cancellation);
            if (Cancellation->GetValue() != 0) return;
            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Serial, Plan = MoveTemp(Plan)]() mutable
                {
                    UWorldSceneStreamAction* Action = WeakThis.Get();
                    if (!IsValid(Action)) return;
                    Action->ApplyPreparedPlan(MoveTemp(Plan), Serial);
                });
        });

    if (!bQueued)
    {
        bPreparationInFlight = false;
        AbortAndRelease();
    }
}

void UWorldSceneStreamAction::ApplyPreparedPlan(FWorldSceneStreamPlan&& Plan, const uint32 Serial)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ApplyPreparedPlan"))) return;
    if (Serial != PreparationSerial || bAbortRequested || !bPreparationInFlight) return;
    if (!IsValid(OwnerActor) || !IsValid(OwnerActor->GetWorld())
        || (!bWaterGroup && !IsValid(MeshActor)))
    {
        // A planning worker can finish after its actor/world has gone away. This action is rooted
        // until its terminal path runs, so silently discarding the current plan leaks the root.
        AbortAndRelease();
        return;
    }
    bPreparationInFlight = false;

    NodeMap = MoveTemp(Plan.NodeMap);
    WaterNodeMap = MoveTemp(Plan.WaterNodeMap);
    MeshMap = MoveTemp(Plan.MeshMap);
    LoadedWaterNodes = MoveTemp(Plan.LoadedWaterNodes);
    PendingLoadNodes = MoveTemp(Plan.PendingLoadNodes);
    PendingUnloadNodes = MoveTemp(Plan.PendingUnloadNodes);
    PendingLoadWaterNodes = MoveTemp(Plan.PendingLoadWaterNodes);
    PendingUnloadWaterNodes = MoveTemp(Plan.PendingUnloadWaterNodes);

    for (const FName InvalidWaterName : Plan.InvalidWaterNodes)
    {
        if (TObjectPtr<AWaterActor>* WaterPtr = WaterActorMap.Find(InvalidWaterName))
        {
            if (IsValid(WaterPtr->Get())) WaterPtr->Get()->Destroy();
        }
        WaterActorMap.Remove(InvalidWaterName);
    }

    PendingUnavailableRegions = MoveTemp(Plan.UnavailableRegions);
    CurrentUnavailableRegionIndex = 0;

    ChunkSize = FMath::Max(1, FMath::Min(
        FMath::Max(1, NodeMap.Num() + WaterNodeMap.Num()), ChunkSize));
    CurrentLoadIndex = 0;
    CurrentUnloadIndex = 0;
    CurrentLoadWaterIndex = 0;
    CurrentUnloadWaterIndex = 0;
    const int32 PendingOperationCount = PendingLoadNodes.Num() + PendingUnloadNodes.Num()
        + PendingLoadWaterNodes.Num() + PendingUnloadWaterNodes.Num();
    TotalOperationCount = Plan.OriginalNodeWorkCount;
    TotalSkippedOperationCount = FMath::Max(0, TotalOperationCount - PendingOperationCount);
    // No-op nodes were already evaluated by the worker. UI smoothing must not hold readiness
    // or a pipeline slot for 16 extra frames when there is no remaining resource work.
    CurrentSkippedOperationIndex = TotalSkippedOperationCount;
    SkippedProgressChunkSize = 1;
    bIsLoading = false;
    BroadcastProgress();
    ProcessChunk();
}

void UWorldSceneStreamAction::ReleaseActionReferences()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ReleaseActionReferences")))
    {
        return;
    }

    bPreparationInFlight = false;
    if (PreparationCancellation.IsValid()) PreparationCancellation->Increment();
    PreparationCancellation.Reset();
    ++PreparationSerial;
    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    bWaitingForMeshCallback = false;
    CurrentRenderObservers.Empty();
    ObserverLocations.Empty();
    Asset = nullptr;
    OwnerActor = nullptr;
    MeshActor = nullptr;
    WorldContextObject = nullptr;
    MaterialReferenceGuard = nullptr;
    if (IsValid(GeneratedMeshWorldContext))
    {
        GeneratedMeshWorldContext->ReleaseWorldPin();
    }
    GeneratedMeshWorldContext = nullptr;
    PendingUnavailableRegions.Empty();
    CurrentUnavailableRegionIndex = 0;
    bRetryMeshLoadPending = false;
    MeshLoadRecovery.Reset();
    PreparedInstances.Empty();
    PreparedInstanceOffset = 0;
    bInstancePreparationInFlight = false;
    PendingLoadNodes.Empty();
    PendingUnloadNodes.Empty();
    PendingLoadWaterNodes.Empty();
    PendingUnloadWaterNodes.Empty();
    TotalOperationCount = 0;
    TotalSkippedOperationCount = 0;
    CurrentSkippedOperationIndex = 0;
    SkippedProgressChunkSize = 1;
    NodeMap.Empty();
    WaterNodeMap.Empty();
    MeshMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    DecalLight = nullptr;
    WaterClass = nullptr;
    StaticMeshConfig = FglTFRuntimeStaticMeshConfig();
    Progress.Clear();
    Completed.Clear();
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    bAbortRequested = true;
    bRenderOnly = false;
    if (IsRooted())
    {
        RemoveFromRoot();
    }
}

void UWorldSceneStreamAction::StartInstanceBatchPreparation()
{
    check(IsInGameThread());
    if (bInstancePreparationInFlight || bAbortRequested || !IsValid(MeshActor)) return;
    // At most 2048 detached rows per active group; never duplicate a million-transform scene.
    const int32 Count = FMath::Min(2048, PendingLoadNodes.Num() - CurrentLoadIndex);
    if (Count <= 0) return;
    TArray<FWorldPreparedInstance> Rows; Rows.SetNum(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        auto& Row = Rows[Index]; Row.NodeName = PendingLoadNodes[CurrentLoadIndex + Index];
        if (const auto* Node = NodeMap.Find(Row.NodeName))
        { Row.LocalTransform = Node->Transform; Row.bValid = !Node->MeshName.IsNone() && MeshMap.Contains(Node->MeshName); }
    }
    const FTransform ActorTransform = MeshActor->GetActorTransform();
    const int32 Begin = CurrentLoadIndex;
    const uint32 Serial = PreparationSerial;
    const auto Cancellation = PreparationCancellation;
    TWeakObjectPtr<UWorldSceneStreamAction> WeakThis(this);
    bInstancePreparationInFlight = true;
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [Rows = MoveTemp(Rows), ActorTransform, Begin, Serial, Cancellation, WeakThis]() mutable
        {
            if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
            // A bounded four-lane job; each lane writes disjoint rows, and reads only native data.
            const int32 Lanes = Rows.Num() >= 256 ? 4 : 1;
            ParallelFor(Lanes, [&](int32 Lane)
            {
                for (int32 Index = Lane; Index < Rows.Num(); Index += Lanes)
                {
                    if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
                    auto& Row = Rows[Index];
                    Row.bValid &= !ActorTransform.ContainsNaN() && !Row.LocalTransform.ContainsNaN()
                        && Row.LocalTransform.GetRotation().IsNormalized();
                    if (Row.bValid) Row.WorldChunk = V3DHISMChunkPolicy::Key(ActorTransform.TransformPosition(Row.LocalTransform.GetLocation()));
                }
            });
            if (Cancellation.IsValid() && Cancellation->GetValue() != 0) return;
            FSafeFileIO::DispatchTrackedGameThread([Rows = MoveTemp(Rows), ActorTransform, Begin, Serial, WeakThis]() mutable
            {
                auto* Action = WeakThis.Get();
                if (!IsValid(Action) || Action->bAbortRequested || Action->PreparationSerial != Serial) return;
                Action->bInstancePreparationInFlight = false;
                if (Action->CurrentLoadIndex != Begin || !IsValid(Action->OwnerActor) || !IsValid(Action->MeshActor))
                { Action->AbortAndRelease(); return; }
                Action->PreparedInstances = MoveTemp(Rows); Action->PreparedInstanceOffset = 0;
                Action->PreparedActorTransform = ActorTransform;
                if (UWorld* World = Action->OwnerActor->GetWorld())
                    Action->ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                        FTimerDelegate::CreateUObject(Action, &UWorldSceneStreamAction::ProcessChunk));
                else Action->AbortAndRelease();
            });
        });
    if (!bQueued) { bInstancePreparationInFlight = false; AbortAndRelease(); }
}

int32 UWorldSceneStreamAction::ApplyInstanceBatch(int32 MaxCount)
{
    check(IsInGameThread());
    const int32 Count = FMath::Min(MaxCount, PreparedInstances.Num() - PreparedInstanceOffset);
    if (Count <= 0 || !IsValid(MeshActor) || bAbortRequested) return 0;
    if (!PendingLoadNodes.IsValidIndex(CurrentLoadIndex)
        || PreparedInstances[PreparedInstanceOffset].NodeName != PendingLoadNodes[CurrentLoadIndex])
    { PreparedInstances.Reset(); PreparedInstanceOffset = 0; return 0; }
    TArray<FWorldPreparedInstance> Visible; Visible.Reserve(Count);
    const FTransform ActorTransform = MeshActor->GetActorTransform();
    const bool bOwnerMoved = !ActorTransform.Equals(PreparedActorTransform);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        auto Row = PreparedInstances[PreparedInstanceOffset + Index];
        if (!Row.bValid) { FailGroup(TEXT("InstancePreparation"), TEXT("Invalid instance transform"), Row.NodeName); return 0; }
        if (MeshActor->IsNodeLoaded(Row.NodeName)) continue;
        if (!IsNodeWithinCurrentRenderRange(Row.NodeName)) { OwnerActor->RequestStreamingRefresh(); continue; }
        if (bOwnerMoved) Row.WorldChunk = V3DHISMChunkPolicy::Key(ActorTransform.TransformPosition(Row.LocalTransform.GetLocation()));
        Visible.Add(MoveTemp(Row));
    }
    TArray<FName> Added;
    const bool bAdded = MeshActor->AddNodeInstances(Visible, Added);
    for (const FName Name : Added)
    {
        if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(MeshActor)) return 0;
        const FModelNodeData& Node = NodeMap.FindChecked(Name);
        if (const auto* MeshData = MeshMap.Find(Node.MeshName)) SpawnStreamComponents(Name, Node, MeshData->Data);
        if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(MeshActor)) return 0;
        if (UWorld* World = OwnerActor->GetWorld())
            if (auto* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                Gate->SetModelRegionAvailable(MeshActor.Get(), Name, FBox(ForceInit), true);
    }
    if (!bAdded) { FailGroup(TEXT("AddInstances"), TEXT("Batch registration failed; completed pages retained")); return 0; }
    CurrentLoadIndex += Count; PreparedInstanceOffset += Count;
    if (PreparedInstanceOffset == PreparedInstances.Num()) { PreparedInstances.Reset(); PreparedInstanceOffset = 0; }
    return Count;
}

void UWorldSceneStreamAction::ProcessChunk()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessChunk"))) return;
    if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(OwnerActor->GetWorld()))
    { AbortAndRelease(); return; }
    const double SliceStartSeconds = FPlatformTime::Seconds();
    const double SliceBudgetSeconds = FMath::Min(
        static_cast<double>(FMath::Clamp(FrameTimeBudgetMs, 0.25f, 4.0f)) * 0.001,
        FV3DStreamingBudget::RemainingSceneSeconds());
    if (SliceBudgetSeconds <= 0.0)
    {
        if (UWorld* World = OwnerActor->GetWorld())
            ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &UWorldSceneStreamAction::ProcessChunk));
        return;
    }
    ON_SCOPE_EXIT { FV3DStreamingBudget::ChargeSceneSeconds(FPlatformTime::Seconds() - SliceStartSeconds); };
    RefreshRenderRangeSnapshot();
    int32 OperationsThisSlice = 0;
    bool bProgressChanged = false;
    const auto HasSliceBudget = [&]()
    {
        return OperationsThisSlice < ChunkSize && (OperationsThisSlice == 0 || (FPlatformTime::Seconds() - SliceStartSeconds) < SliceBudgetSeconds);
    };
    // Large chunks also have many gate regions. Register these within the same frame budget
    // instead of an unbounded game-thread loop in ApplyPreparedPlan.
    while (CurrentUnavailableRegionIndex < PendingUnavailableRegions.Num() && HasSliceBudget())
    {
        const auto& Region = PendingUnavailableRegions[CurrentUnavailableRegionIndex++];
        if (UWorld* World = OwnerActor->GetWorld())
            if (auto* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                Gate->SetModelRegionAvailable(MeshActor.Get(), Region.NodeName, Region.WorldBounds, false);
        ++OperationsThisSlice;
    }
    if (CurrentSkippedOperationIndex < TotalSkippedOperationCount)
    {
        CurrentSkippedOperationIndex = FMath::Min(CurrentSkippedOperationIndex + SkippedProgressChunkSize, TotalSkippedOperationCount);
        bProgressChanged = true;
    }
    const bool bHasPendingLoads = CurrentLoadIndex < PendingLoadNodes.Num() || CurrentLoadWaterIndex < PendingLoadWaterNodes.Num();
    const int32 UnloadQuota = bHasPendingLoads ? FMath::Max(1, ChunkSize / 2) : ChunkSize;
    int32 UnloadsThisSlice = 0;
    while (CurrentUnloadWaterIndex < PendingUnloadWaterNodes.Num() && UnloadsThisSlice < UnloadQuota && HasSliceBudget())
    { ProcessUnloadWaterNode(PendingUnloadWaterNodes[CurrentUnloadWaterIndex++]); ++OperationsThisSlice; ++UnloadsThisSlice; bProgressChanged = true; }
    while (CurrentUnloadIndex < PendingUnloadNodes.Num() && UnloadsThisSlice < UnloadQuota && HasSliceBudget())
    {
        if (IsValid(MeshActor) && !MeshActor->CanMutateNode(PendingUnloadNodes[CurrentUnloadIndex])) break;
        ProcessUnloadNode(PendingUnloadNodes[CurrentUnloadIndex++]); ++OperationsThisSlice; ++UnloadsThisSlice; bProgressChanged = true;
    }
    // A fallback starts on a later slice, after the previous native callback has unwound
    // and released its world pin. Never submit a second build from inside that callback.
    if (bRetryMeshLoadPending && HasSliceBudget())
    {
        bRetryMeshLoadPending = false;
        if (!bGroupFailed && IsNodeWithinCurrentRenderRange(CurrentLoadingNode))
            LoadStaticMeshAsync(CurrentLoadingMesh);
        else
        {
            OwnerActor->RequestStreamingRefresh();
            ResetLoadState();
        }
        ++OperationsThisSlice;
    }
    while (CurrentUnavailableRegionIndex >= PendingUnavailableRegions.Num()
        && !bIsLoading && CurrentLoadIndex < PendingLoadNodes.Num() && HasSliceBudget())
    {
        if (IsValid(MeshActor) && IsValid(MeshActor->GetMeshComponent())
            && IsValid(MeshActor->GetMeshComponent()->GetStaticMesh()))
        {
            if (bInstancePreparationInFlight) break;
            if (PreparedInstanceOffset >= PreparedInstances.Num())
            {
                StartInstanceBatchPreparation();
                break;
            }
            const int32 Applied = ApplyInstanceBatch(FMath::Min(256, ChunkSize - OperationsThisSlice));
            OperationsThisSlice += Applied; bProgressChanged |= Applied > 0;
            if (Applied == 0 || bGroupFailed) break;
            continue;
        }
        const FName TargetNode = PendingLoadNodes[CurrentLoadIndex++]; ++OperationsThisSlice; bProgressChanged = true;
        if (IsValid(MeshActor) && MeshActor->IsNodeLoaded(TargetNode)) continue;
        if (ProcessLoadNode(TargetNode)) break;
    }
    while (!bIsLoading && CurrentLoadWaterIndex < PendingLoadWaterNodes.Num() && HasSliceBudget())
    { ProcessLoadWaterNode(PendingLoadWaterNodes[CurrentLoadWaterIndex++]); ++OperationsThisSlice; bProgressChanged = true; }
    while (CurrentUnloadWaterIndex < PendingUnloadWaterNodes.Num() && HasSliceBudget())
    { ProcessUnloadWaterNode(PendingUnloadWaterNodes[CurrentUnloadWaterIndex++]); ++OperationsThisSlice; bProgressChanged = true; }
    while (CurrentUnloadIndex < PendingUnloadNodes.Num() && HasSliceBudget())
    {
        if (IsValid(MeshActor) && !MeshActor->CanMutateNode(PendingUnloadNodes[CurrentUnloadIndex])) break;
        ProcessUnloadNode(PendingUnloadNodes[CurrentUnloadIndex++]); ++OperationsThisSlice; bProgressChanged = true;
    }
    const bool bNodeWorkFinished = CurrentLoadIndex >= PendingLoadNodes.Num()
        && CurrentUnloadIndex >= PendingUnloadNodes.Num() && !bIsLoading;
    if (IsValid(MeshActor)) MeshActor->FlushInstanceRenderUpdates(bNodeWorkFinished);
    if (bProgressChanged) BroadcastProgress();
    // Blueprint progress handlers may cancel this action or tear down the owning scene. Never
    // dereference references or reschedule the action after a user callback invalidates them.
    if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(OwnerActor->GetWorld()))
    { AbortAndRelease(); return; }
    if (CurrentUnavailableRegionIndex >= PendingUnavailableRegions.Num()
        && CurrentSkippedOperationIndex >= TotalSkippedOperationCount
        && CurrentLoadIndex >= PendingLoadNodes.Num() && CurrentUnloadIndex >= PendingUnloadNodes.Num()
        && CurrentLoadWaterIndex >= PendingLoadWaterNodes.Num() && CurrentUnloadWaterIndex >= PendingUnloadWaterNodes.Num() && !bIsLoading
        && (!IsValid(MeshActor) || MeshActor->AreChunkTreesReady()))
    {
        UWorld* World = OwnerActor->GetWorld();
        if (IsValid(World)) World->GetTimerManager().ClearTimer(ProcessTimerHandle);
        FWorldSceneStreamResult Result;
        Result.GroupName = GroupName;
        Result.bWaterGroup = bWaterGroup;
        Result.bGroupFailed = bGroupFailed;
        Result.bMeshPayloadUnavailable = bMeshPayloadUnavailable;
        Result.FailureStage = FailureStage;
        Result.FailureReason = FailureReason;
        Result.FailedNode = FailedNode;
        Result.FailedMesh = FailedMesh;
        Result.WaterNodeMap = MoveTemp(WaterNodeMap);
        Result.LoadedWaterNodes = MoveTemp(LoadedWaterNodes);
        Result.WaterActorMap = MoveTemp(WaterActorMap);
        Progress.Broadcast(GroupName, 1.0f);
        Completed.Broadcast(Result);
        ReleaseActionReferences();
        return;
    }
    if (bInstancePreparationInFlight && CurrentUnloadIndex >= PendingUnloadNodes.Num()
        && CurrentUnloadWaterIndex >= PendingUnloadWaterNodes.Num()) return; // Worker callback resumes the action.
    if (bStaticMeshLoadInFlight && bIsLoading
        && CurrentUnloadIndex >= PendingUnloadNodes.Num()
        && CurrentUnloadWaterIndex >= PendingUnloadWaterNodes.Num())
    {
        bWaitingForMeshCallback = true;
        return;
    }
    if (UWorld* World = OwnerActor->GetWorld())
    {
        ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UWorldSceneStreamAction::ProcessChunk));
    }
}


void UWorldSceneStreamAction::ProcessLoadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessLoadWaterNode")))
    {
        return;
    }

    if (bAbortRequested || LoadedWaterNodes.Contains(Name) || !IsValid(OwnerActor.Get()))
    {
        return;
    }

    const FWaterStreamNodeData* WaterInfo = WaterNodeMap.Find(Name);
    if (!WaterInfo) return;
    if (!IsWaterWithinCurrentRenderRange(*WaterInfo))
    {
        OwnerActor->RequestStreamingRefresh();
        return;
    }

    UWorld* World = OwnerActor->GetWorld();
    if (!World)
    {
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = OwnerActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    UClass* WaterSpawnClass = WaterClass ? WaterClass.Get() : AWaterActor::StaticClass();
    AWaterActor* WaterActor = World->SpawnActor<AWaterActor>(WaterSpawnClass, WaterInfo->Transform, Params);
    if (!IsValid(WaterActor) && WaterSpawnClass != AWaterActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured water actor class failed for streamed ;WATER node; retrying native AWaterActor. Class=%s Node=%s"),
            *GetNameSafe(WaterSpawnClass), *Name.ToString());
        WaterActor = World->SpawnActor<AWaterActor>(AWaterActor::StaticClass(), WaterInfo->Transform, Params);
    }
    if (IsValid(WaterActor))
    {
        if (bRenderOnly)
        {
            WaterActor->SetActorEnableCollision(false);
        }
        WaterActorMap.Emplace(Name, WaterActor);
        LoadedWaterNodes.Emplace(Name);
    }
}

void UWorldSceneStreamAction::ProcessUnloadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessUnloadWaterNode")))
    {
        return;
    }

    if (TObjectPtr<AWaterActor>* WaterPtr = WaterActorMap.Find(Name))
    {
        if (IsValid(WaterPtr->Get()))
        {
            WaterPtr->Get()->Destroy();
        }
    }

    WaterActorMap.Remove(Name);
    LoadedWaterNodes.Remove(Name);
}

bool UWorldSceneStreamAction::ProcessLoadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessLoadNode")))
    {
        return false;
    }

    if (bAbortRequested)
    {
        return false;
    }

    if (FModelNodeData *Info = NodeMap.Find(Name))
    {
        if (IsValid(MeshActor) && MeshActor->IsNodeLoaded(Name))
        {
            return false;
        }

        if (Info->MeshName.IsNone() || Info->Transform.ContainsNaN() || !MeshMap.Contains(Info->MeshName))
        {
            NodeMap.Remove(Name);
            return false;
        }

        if (!IsNodeWithinCurrentRenderRange(Name))
        {
            OwnerActor->RequestStreamingRefresh();
            return false;
        }

        CurrentLoadingNode = Name;
        CurrentLoadingMesh = Info->MeshName;
        bIsLoading = true;

        UInstancedStaticMeshComponent* ISMC = IsValid(MeshActor)
            ? MeshActor->GetMeshComponent()
            : nullptr;
        if (IsValid(ISMC) && IsValid(ISMC->GetStaticMesh()))
        {
            AddTransform(Name);
            // No native load was submitted. Keep walking this chunk so one action can add many
            // instances of its already-loaded shared mesh in the same frame.
            return false;
        }

        LoadStaticMeshAsync(CurrentLoadingMesh);
        return true;
    }

    return false;
}

void UWorldSceneStreamAction::RefreshRenderRangeSnapshot()
{
    check(IsInGameThread());
    CurrentRenderObservers.Reset();
    if (!IsValid(OwnerActor)) return;
    if (UWorldSceneStreamingSubsystem* Streamer = UWorldSceneStreamingSubsystem::Get(OwnerActor.Get()))
        Streamer->GetStreamingObserverLocations(CurrentRenderObservers);
    if (CurrentRenderObservers.IsEmpty()) CurrentRenderObservers = ObserverLocations;
    CurrentRenderOwnerTransform = OwnerActor->GetActorTransform();
    CurrentMaxRenderDistanceCm = V3DStreamingPolicy::GetMaxRenderDistanceCm(OwnerActor.Get());
    if (IsValid(MeshActor) && IsValid(MeshActor->GetMeshComponent()) && IsValid(MeshActor->GetMeshComponent()->GetStaticMesh()))
    {
        const double End = FMath::Min(CurrentMaxRenderDistanceCm, FMath::Max(2000.0, double(PlanningWorldRadius)));
        MeshActor->SetInstanceCullDistances(FMath::FloorToInt(End * 0.85), FMath::FloorToInt(End));
    }
}

bool UWorldSceneStreamAction::IsNodeWithinCurrentRenderRange(const FName& Name) const
{
    check(IsInGameThread());
    const FModelNodeData* Node = NodeMap.Find(Name);
    const FModelMeshData* Mesh = Node ? MeshMap.Find(Node->MeshName) : nullptr;
    if (!Mesh || !IsValid(OwnerActor)) return false;
    const FTransform Transform = Node->Transform * CurrentRenderOwnerTransform;
    // The worker admitted this node using the aggregate bounds/radius of its entire cell.
    // Replacing that decision with this node's smaller radius rejects valid peers forever.
    // Only enforce the current hard render cap here; movement/settings invalidate the pass,
    // and the next worker evaluation updates cell residency using the complete group.
    for (const FVector& Observer : CurrentRenderObservers)
        if (V3DStreamingPolicy::BoundsInRenderRange(FVector::ZeroVector, Mesh->Size,
            Transform, Observer, CurrentMaxRenderDistanceCm)) return true;
    return false;
}

bool UWorldSceneStreamAction::IsWaterWithinCurrentRenderRange(const FWaterStreamNodeData& Water) const
{
    check(IsInGameThread());
    if (!IsValid(OwnerActor)) return false;
    const FVector Center = (Water.Transform * CurrentRenderOwnerTransform).GetLocation();
    const double Radius = CurrentMaxRenderDistanceCm
        + Water.StreamRadius * CurrentRenderOwnerTransform.GetScale3D().GetAbs().GetMax();
    for (const FVector& Observer : CurrentRenderObservers)
        if (FVector::DistSquared(Center, Observer) <= FMath::Square(Radius)) return true;
    return false;
}

void UWorldSceneStreamAction::ProcessUnloadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessUnloadNode")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()) || !IsValid(MeshActor.Get()))
    {
        return;
    }

    FModelNodeData *Info = NodeMap.Find(Name);
    if (!Info)
        return;

    if (!MeshActor->RemoveNodeInstance(Name))
    {
        FailGroup(TEXT("RemoveInstance"), TEXT("Instance removal was rejected; existing colliders were retained"), Name, Info->MeshName);
        return;
    }
    DestroyStreamComponents(Name);
    if (bRenderOnly)
    {
        return;
    }

    if (const FModelMeshData *Mesh = MeshMap.Find(Info->MeshName))
    {
        if (UWorld* World = OwnerActor->GetWorld())
        {
            if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
            {
                const FTransform WorldTransform = Info->Transform * OwnerActor->GetActorTransform();
                const FVector Extent = (Mesh->Size * 0.5f + BOX_BUFFER_SIZE).GetAbs();
                Gate->SetModelRegionAvailable(MeshActor.Get(), Name,
                    FBox(-Extent, Extent).TransformBy(WorldTransform), false);
            }
        }
    }
}

void UWorldSceneStreamAction::SetStaticMesh(UStaticMesh *StaticMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::SetStaticMesh")))
    {
        return;
    }

    bStaticMeshLoadInFlight = false;
    ON_SCOPE_EXIT
    {
        // Drop the strong world pin only after all callback-side component work is complete.
        if (IsValid(GeneratedMeshWorldContext))
        {
            GeneratedMeshWorldContext->ReleaseWorldPin();
        }
        if (bWaitingForMeshCallback)
        {
            bWaitingForMeshCallback = false;
            if (!bAbortRequested && IsValid(OwnerActor))
                if (UWorld* World = OwnerActor->GetWorld())
                    ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                        FTimerDelegate::CreateUObject(this, &UWorldSceneStreamAction::ProcessChunk));
        }
    };
    if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(MeshActor))
    {
        AbortAndRelease(StaticMesh);
        return;
    }
    // Async builds may finish after the camera or setting changed. A stale/cancelled request
    // must not consume a payload-failure attempt or restart an obsolete mesh build.
    RefreshRenderRangeSnapshot();
    if (bGroupFailed || !IsNodeWithinCurrentRenderRange(CurrentLoadingNode))
    {
        OwnerActor->RequestStreamingRefresh();
        ResetLoadState();
        return;
    }
    if (!IsValid(StaticMesh))
    {
        const bool bCanRecover = IsValid(Asset) && MeshLoadRecovery.IsInitialized()
            && !FSafeFileIO::IsShuttingDown() && !FV3DRuntimeSafety::IsCircuitOpen()
            && (!IsValid(GeneratedMeshWorldContext) || !GeneratedMeshWorldContext->IsBuildCancelled());
        if (bCanRecover && MeshLoadRecovery.AdvanceAfterFailure())
        {
            bRetryMeshLoadPending = true;
            UE_LOG(LogTemp, Warning,
                TEXT("Mesh load failed; retrying one authored LOD. Group=%s Node=%s Mesh=%s Index=%d"),
                *GroupName.ToString(), *CurrentLoadingNode.ToString(), *CurrentLoadingMesh.ToString(),
                MeshLoadRecovery.GetMeshIndex(0));
            return;
        }
        // The payload/finalizer already logged the detailed reason. Do not count it twice in
        // source quarantine; report only the exhausted pass to the scene's bounded recovery.
        FailGroup(TEXT("MeshLoad"), TEXT("No usable mesh after the available LOD requests; see V3DStreaming Stage/Reason"),
            NAME_None, NAME_None, bCanRecover);
        ResetLoadState();
        return;
    }
    if (MeshLoadRecovery.IsFallback())
        UE_LOG(LogTemp, Warning, TEXT("Recovered streamed mesh using one LOD. Group=%s Mesh=%s Index=%d"),
            *GroupName.ToString(), *CurrentLoadingMesh.ToString(), MeshLoadRecovery.GetMeshIndex(0));

    const FModelMeshData* ModelMeshData = MeshMap.Find(CurrentLoadingMesh);

    // Every instance in this group receives the same mesh object created from its streamed bundle.
    UInstancedStaticMeshComponent* ISMC = MeshActor->AssignStaticMesh(StaticMesh);
    if (IsValid(ISMC))
    {
        ISMC->SetRenderCustomDepth(true);
        ISMC->SetCustomDepthStencilValue(1);
        const double End = FMath::Min(CurrentMaxRenderDistanceCm, FMath::Max(2000.0, double(PlanningWorldRadius)));
        MeshActor->SetInstanceCullDistances(FMath::FloorToInt(End * 0.85), FMath::FloorToInt(End));
    }

    ConfigureGeneratedMeshCollision(
        ISMC,
        ModelMeshData ? &ModelMeshData->Data : nullptr,
        bRenderOnly);

    if (IsValid(ISMC))
    {
        // AddInstance creates the per-instance body. Avoid a second explicit physics-state rebuild;
        // it can overlap Chaos scene insertion while the generated mesh is being finalized.
        AddTransform(CurrentLoadingNode);
        if (!bAbortRequested && IsValid(MeshActor)) MeshActor->FlushInstanceRenderUpdates();
    }
    else
    {
        FailGroup(TEXT("AssignMesh"), TEXT("Shared mesh assignment was rejected"));
        ResetLoadState();
    }
}

FORCEINLINE float CalculateLODScreenSize(int32 i, int32 N)
{
    if (N <= 1) return 1.0f;
    // LOD0 -> 1.0, LOD1 -> 0.35, LOD2 -> 0.1225, LOD3 -> 0.042875.
    return i <= 0 ? 1.0f : FMath::Pow(0.35f, static_cast<float>(i));
}

void UWorldSceneStreamAction::LoadStaticMeshAsync(const FName &MeshName)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::LoadStaticMeshAsync")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(Asset))
    {
        AbortAndRelease();
        return;
    }

    UWorldBakedModelAsset* MeshSourceAsset = Asset.Get();
    if (!IsValid(MeshSourceAsset))
    {
        FailGroup(TEXT("MeshSource"), TEXT("Baked mesh source is no longer valid"));
        ResetLoadState();
        return;
    }

    FModelMeshData* Mesh = MeshMap.Find(MeshName);
    if (!Mesh)
    {
        FailGroup(TEXT("MeshMetadata"), TEXT("Requested mesh metadata is absent"));
        ResetLoadState();
        return;
    }

    const int32 RuntimeMeshCount = MeshSourceAsset->GetNumMeshes();
    TArray<int32> LocalIndices;
    LocalIndices.Reserve(4);

    const auto AddValidLOD = [this, &LocalIndices, RuntimeMeshCount, MeshName](
        const int32 MeshIndex,
        const TCHAR* LODLabel)
    {
        if (MeshIndex == INDEX_NONE)
        {
            return;
        }
        if (MeshIndex < 0 || MeshIndex >= RuntimeMeshCount)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Ignoring invalid runtime mesh index. Mesh=%s Node=%s LOD=%s Index=%d MeshCount=%d"),
                *MeshName.ToString(),
                *CurrentLoadingNode.ToString(),
                LODLabel,
                MeshIndex,
                RuntimeMeshCount);
            return;
        }
        LocalIndices.AddUnique(MeshIndex);
    };

    if (!MeshLoadRecovery.IsInitialized())
    {
        AddValidLOD(Mesh->LOD0, TEXT("LOD0"));
        AddValidLOD(Mesh->LOD1, TEXT("LOD1"));
        AddValidLOD(Mesh->LOD2, TEXT("LOD2"));
        AddValidLOD(Mesh->LOD3, TEXT("LOD3"));
        MeshLoadRecovery.Begin(LocalIndices.GetData(), LocalIndices.Num(),
            CVarEnableRuntimeMultiLOD.GetValueOnGameThread() != 0);
    }
    LocalIndices.Reset();
    for (int32 Index = 0; Index < MeshLoadRecovery.GetRequestCount(); ++Index)
        LocalIndices.Add(MeshLoadRecovery.GetMeshIndex(Index));

    const int32 Count = LocalIndices.Num();
    if (Count <= 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Runtime mesh request skipped because no valid mesh index remains. Mesh=%s Node=%s MeshCount=%d"),
            *MeshName.ToString(),
            *CurrentLoadingNode.ToString(),
            RuntimeMeshCount);
        FailGroup(TEXT("MeshIndices"), TEXT("No valid runtime mesh index remains"),
            NAME_None, NAME_None, true);
        ResetLoadState();
        return;
    }

    TMap<int32, float> LODScreenSize;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        LODScreenSize.Add(Index, CalculateLODScreenSize(Index, Count));
    }

    const bool bBuildComplexCollision = !bRenderOnly && Mesh->Data.bComplexCollision;
    const bool bBuildSimpleCollision = !bRenderOnly && Mesh->Data.bSimpleCollision;
    const bool bNeedsCollision = bBuildComplexCollision || bBuildSimpleCollision;

    if (bNeedsCollision && !IsValid(GeneratedMeshWorldContext))
    {
        UWorld* RuntimeWorld = IsValid(OwnerActor.Get()) ? OwnerActor->GetWorld() : nullptr;
        if (IsValid(RuntimeWorld) && RuntimeWorld->IsGameWorld())
        {
            UWorldGeneratedStaticMeshContext* MeshWorldContext =
                NewObject<UWorldGeneratedStaticMeshContext>(GetTransientPackage(), NAME_None, RF_Transient);
            if (IsValid(MeshWorldContext))
            {
                MeshWorldContext->Initialize(RuntimeWorld);
                GeneratedMeshWorldContext = MeshWorldContext;
            }
        }
    }

    UWorld* CollisionWorld = IsValid(GeneratedMeshWorldContext)
        ? GeneratedMeshWorldContext->GetWorld()
        : nullptr;
    if (bNeedsCollision && (!IsValid(CollisionWorld) || !CollisionWorld->IsGameWorld()))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Runtime mesh collision build rejected because no active game-world outer is available. Mesh=%s Node=%s"),
            *MeshName.ToString(),
            *CurrentLoadingNode.ToString());
        FailGroup(TEXT("CollisionWorld"), TEXT("No active game-world outer for collision build"));
        ResetLoadState();
        return;
    }

    FglTFRuntimeStaticMeshConfig Config = StaticMeshConfig;
    // Visual-only world geometry is immutable and safe to share across every placement using the
    // same baked facade. Using the facade as Outer explicitly opts into the weak completed-mesh
    // cache + in-flight request coalescing. Collision meshes keep a world-scoped outer.
    Config.Outer = !bNeedsCollision
        ? static_cast<UObject*>(MeshSourceAsset)
        : static_cast<UObject*>(GeneratedMeshWorldContext.Get());
    Config.bBuildComplexCollision = bBuildComplexCollision;
    Config.bBuildSimpleCollision = bBuildSimpleCollision;
    // Avoid CPU copies and nav data for visual-only groups. Complex collision needs CPU vertex
    // access while it is built; simple collision does not require retaining the render vertices.
    Config.bBuildNavCollision = !bRenderOnly && bNeedsCollision && Config.bBuildNavCollision;
    Config.bAllowCPUAccess = !bRenderOnly && bBuildComplexCollision;
    Config.bGenerateStaticMeshDescription = false;
    Config.CollisionComplexity = bBuildComplexCollision
        ? ECollisionTraceFlag::CTF_UseComplexAsSimple
        : ECollisionTraceFlag::CTF_UseDefault;
    Config.LODScreenSize = LODScreenSize;
    Config.LODScreenSizeMultiplier = 1.0f;

    const TArray<int32> RequestedIndices = LocalIndices;
    const FglTFRuntimeStaticMeshConfig RequestedConfig = Config;
    const bool bRequestedCollision =
        RequestedConfig.bBuildComplexCollision || RequestedConfig.bBuildSimpleCollision;
    if (bRequestedCollision && (!IsValid(GeneratedMeshWorldContext)
        || !GeneratedMeshWorldContext->PinWorldForBuild()))
    {
        FailGroup(TEXT("WorldPin"), TEXT("Collision world pin was rejected or cancelled"));
        ResetLoadState();
        return;
    }
    bStaticMeshLoadInFlight = true;
    FglTFRuntimeStaticMeshAsync Callback;
    Callback.BindDynamic(this, &UWorldSceneStreamAction::SetStaticMesh);
    if (RequestedIndices.Num() == 1)
        MeshSourceAsset->LoadStaticMeshAsync(RequestedIndices[0], Callback, RequestedConfig);
    else
        MeshSourceAsset->LoadStaticMeshLODsAsync(RequestedIndices, Callback, RequestedConfig);
}

void UWorldSceneStreamAction::AddTransform(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::AddTransform")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()) || !IsValid(MeshActor.Get()))
    {
        ResetLoadState();
        return;
    }

    if (!IsNodeWithinCurrentRenderRange(Name))
    {
        OwnerActor->RequestStreamingRefresh();
        ResetLoadState();
        return;
    }

    if (FModelNodeData *NodeInfo = NodeMap.Find(Name))
    {
        if (NodeInfo->Transform.ContainsNaN() || NodeInfo->MeshName.IsNone()
            || !MeshActor->AddNodeInstance(Name, NodeInfo->Transform))
        {
            FailGroup(TEXT("AddInstance"), TEXT("Instance transform, mesh, or node placement was rejected"), Name, NodeInfo->MeshName);
            ResetLoadState();
            return;
        }
        if (const FModelMeshData *MeshData = MeshMap.Find(NodeInfo->MeshName))
        {
            SpawnStreamComponents(Name, *NodeInfo, MeshData->Data);
        }

        if (UWorld* World = OwnerActor->GetWorld())
        {
            if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                Gate->SetModelRegionAvailable(MeshActor.Get(), Name, FBox(ForceInit), true);
        }
    }
    ResetLoadState();
}

void UWorldSceneStreamAction::SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::SpawnStreamComponents")))
    {
        return;
    }

    if (!IsValid(MeshActor) || MeshActor->HasDynamicComponents(NodeName))
    {
        return;
    }

    FComponentGroup Group;
    UWorld *World = MeshActor->GetWorld();
    if (!World)
        return;

    if (!bRenderOnly && Data.bSimpleCollision)
    {
        for (const FModelCollider &ColliderData : Data.Colliders)
        {
            UShapeComponent *NewShape = nullptr;
            FTransform ComponentLocalTransform = NodeInfo.Transform;
            // Collider centers in model JSON are expressed in the mesh node's local space.
            // Apply the offset through the node transform so rotation/scale remain consistent.
            ComponentLocalTransform.SetLocation(NodeInfo.Transform.TransformPosition(ColliderData.Center));

            if (ColliderData.Collider == EColliderType::Box)
            {
                UBoxComponent *BoxComp = NewObject<UBoxComponent>(MeshActor);
                if (BoxComp)
                {
                    BoxComp->SetBoxExtent(ColliderData.Size);
                    NewShape = BoxComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Sphere)
            {
                USphereComponent *SphereComp = NewObject<USphereComponent>(MeshActor);
                if (SphereComp)
                {
                    SphereComp->SetSphereRadius(ColliderData.Size.X);
                    NewShape = SphereComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Capsule)
            {
                UCapsuleComponent *CapsuleComp = NewObject<UCapsuleComponent>(MeshActor);
                if (CapsuleComp)
                {
                    CapsuleComp->SetCapsuleSize(ColliderData.Size.X, ColliderData.Size.Y);
                    NewShape = CapsuleComp;
                }
            }

            if (NewShape)
            {
                MeshActor->AddInstanceComponent(NewShape);
                NewShape->AttachToComponent(MeshActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
                NewShape->SetRelativeTransform(ComponentLocalTransform);
                NewShape->SetCollisionProfileName(TEXT("BlockAll"));
                if (UWorldSurfaceSubsystem* Surfaces = UWorldSurfaceSubsystem::Get(OwnerActor.Get()))
                    if (UPhysicalMaterial* Physical = Surfaces->GetSimplePhysicalMaterial(MeshActor->GetMeshComponent()))
                        NewShape->SetPhysMaterialOverride(Physical);
                NewShape->RegisterComponent();
                Group.Colliders.Add(NewShape);
            }
        }
    }

    for (const FLightData &LightData : Data.Lights)
    {
        UDynamicPointLightComponent *PointLight = NewObject<UDynamicPointLightComponent>(MeshActor);
        if (PointLight)
        {
            MeshActor->AddInstanceComponent(PointLight);
            PointLight->AttachToComponent(MeshActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            const FVector LightLocalLocation = NodeInfo.Transform.TransformPosition(LightData.Location);
            PointLight->SetRelativeLocation(LightLocalLocation);
            PointLight->SetSourceRadius(LightData.SourceRadius);
            PointLight->SetSoftSourceRadius(LightData.SoftSourceRadius);
            PointLight->SetSourceLength(LightData.Length);
            PointLight->SetAttenuationRadius(LightData.AttenuationRadius);
            PointLight->SetIntensityUnits(LightData.Unit);
            PointLight->SetIntensity(LightData.Intensity);
            PointLight->SetLightDecal(DecalLight);
            PointLight->RegisterComponent();
            Group.Lights.Add(PointLight);
        }
    }

    if (Group.Colliders.Num() > 0 || Group.Lights.Num() > 0)
    {
        MeshActor->StoreDynamicComponents(NodeName, MoveTemp(Group));
    }
}

void UWorldSceneStreamAction::DestroyStreamComponents(const FName &NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::DestroyStreamComponents")))
    {
        return;
    }

    if (IsValid(MeshActor))
    {
        MeshActor->DestroyDynamicComponents(NodeName);
    }
}

void UWorldSceneStreamAction::FailGroup(const TCHAR* Stage, const FString& Reason, const FName Node,
    const FName Mesh, const bool bPayloadUnavailable)
{
    check(IsInGameThread());
    if (!bGroupFailed)
    {
        bMeshPayloadUnavailable = bPayloadUnavailable;
        FailureStage = Stage;
        FailureReason = Reason.Left(2048);
        FailedNode = Node.IsNone() ? CurrentLoadingNode : Node;
        FailedMesh = Mesh.IsNone() ? CurrentLoadingMesh : Mesh;
    }
    bGroupFailed = true;
    if (PreparationCancellation.IsValid()) PreparationCancellation->Increment();
    ++PreparationSerial;
    bInstancePreparationInFlight = false;
    PreparedInstances.Reset(); PreparedInstanceOffset = 0;
    CurrentLoadIndex = PendingLoadNodes.Num();
    NodeMap.Empty();
    // An unload failure may happen while a mesh load is outstanding. Its native callback still
    // owns the action/world pins; FailGroup must not reset bStaticMeshLoadInFlight here.
}

void UWorldSceneStreamAction::ResetLoadState()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ResetLoadState")))
    {
        return;
    }

    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    bRetryMeshLoadPending = false;
    MeshLoadRecovery.Reset();
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    BroadcastProgress();
}

void UWorldSceneStreamAction::BroadcastProgress()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::BroadcastProgress")))
    {
        return;
    }

    if (TotalOperationCount <= 0)
    {
        Progress.Broadcast(GroupName, 1.0f);
        return;
    }

    const int32 CompletedLoadNodes = FMath::Max(0, CurrentLoadIndex - (bIsLoading ? 1 : 0));
    const int32 CompletedOperations = FMath::Clamp(
        CurrentSkippedOperationIndex +
        CurrentUnloadIndex +
        CompletedLoadNodes +
        CurrentUnloadWaterIndex +
        CurrentLoadWaterIndex,
        0,
        TotalOperationCount);
    Progress.Broadcast(GroupName, FMath::Clamp(
        static_cast<float>(CompletedOperations) / static_cast<float>(TotalOperationCount),
        0.0f,
        1.0f));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSceneActionLifetimeTest,
    "V3DSimulator.Streaming.Memory.SceneActionLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSceneActionLifetimeTest::RunTest(const FString&)
{
    TStrongObjectPtr<UWorldSceneStreamAction> Action(NewObject<UWorldSceneStreamAction>());
    Action->AddToRoot();
    Action->bPreparationInFlight = true;
    Action->PreparationCancellation = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
    const auto PlanningCancellation = Action->PreparationCancellation;
    Action->PreparationSerial = 1;
    FWorldSceneStreamPlan Plan;
    Action->ApplyPreparedPlan(MoveTemp(Plan), 1);
    TestFalse(TEXT("Planning completion after owner loss releases its root"), Action->IsRooted());
    TestFalse(TEXT("Invalid current plan no longer occupies a planning slot"), Action->bPreparationInFlight);
    TestTrue(TEXT("A lost owner cancels the detached planning token"), PlanningCancellation->GetValue() != 0);

    TStrongObjectPtr<UWorldSceneStreamAction> InFlight(NewObject<UWorldSceneStreamAction>());
    InFlight->AddToRoot();
    InFlight->bActivated = true;
    InFlight->bStaticMeshLoadInFlight = true;
    InFlight->GeneratedMeshWorldContext = NewObject<UWorldGeneratedStaticMeshContext>();
    InFlight->GeneratedMeshWorldContext->Initialize(nullptr);
    const auto Cancellation = InFlight->GeneratedMeshWorldContext->GetBuildCancellationToken();
    InFlight->NodeMap.Add(TEXT("DeferredNode"), FModelNodeData());
    InFlight->PendingLoadNodes.Add(TEXT("DeferredNode"));
    InFlight->Activate();
    TestTrue(TEXT("Duplicate activation preserves native build lifetime"), InFlight->bStaticMeshLoadInFlight);
    InFlight->CancelAndRelease();
    TestTrue(TEXT("Cancellation waits for the native callback"), InFlight->IsRooted());
    TestTrue(TEXT("Queued/read work observes detached cancellation"), Cancellation.IsValid() && Cancellation->GetValue() != 0);
    TestTrue(TEXT("Cancelled native wait releases scene rows immediately"), InFlight->NodeMap.IsEmpty() && InFlight->PendingLoadNodes.IsEmpty());
    TestFalse(TEXT("A cancelled context cannot start a later build"), InFlight->GeneratedMeshWorldContext->PinWorldForBuild());
    InFlight->Activate();
    TestTrue(TEXT("A cancelled action cannot be revived"), InFlight->bAbortRequested);
    InFlight->SetStaticMesh(nullptr);
    TestFalse(TEXT("Terminal callback releases the cancelled action"), InFlight->IsRooted());

    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Water lifetime world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Scene = World->SpawnActor<AStaticActor>();
    AWaterActor* Water = World->SpawnActor<AWaterActor>();
    if (!TestNotNull(TEXT("Scene"), Scene) || !TestNotNull(TEXT("Water"), Water)) return false;
    TStrongObjectPtr<UWorldSceneStreamAction> WaterAction(NewObject<UWorldSceneStreamAction>());
    WaterAction->OwnerActor = Scene;
    WaterAction->bWaterGroup = true;
    WaterAction->TotalOperationCount = 1;
    WaterAction->WaterNodeMap.Add(TEXT("Water"), FWaterStreamNodeData());
    WaterAction->LoadedWaterNodes.Add(TEXT("Water"));
    WaterAction->WaterActorMap.Add(TEXT("Water"), Water);
    Scene->ActiveStreamActions.Add(NAME_None, WaterAction.Get());
    WaterAction->CancelAndRelease();
    TestTrue(TEXT("Retry preserves partial water residency"),
        Scene->GetLoadedWaterNodesRef().Contains(TEXT("Water"))
        && Scene->GetWaterActorMapRef().FindRef(TEXT("Water")).Get() == Water);
    Scene->ReleaseRuntimeResourcesForWorldExit();
    TestTrue(TEXT("World exit destroys water retained by a cancelled pass"), Water->IsActorBeingDestroyed());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSceneCellAdmissionTest,
    "V3DSimulator.Streaming.Regression.CellAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSceneCellAdmissionTest::RunTest(const FString&)
{
    FWorldSceneStreamPlan Plan;
    FModelMeshData Mesh;
    Mesh.LOD0 = 0;
    Mesh.Size = FVector(100.0);
    Mesh.Extent = Mesh.Size * 0.5;
    Plan.MeshMap.Add(TEXT("Mesh"), Mesh);
    FModelNodeData Anchor;
    Anchor.MeshName = TEXT("Mesh");
    Anchor.Transform.SetLocation(FVector(100, 100, 100));
    Anchor.bAlwaysLoaded = true;
    Plan.NodeMap.Add(TEXT("Anchor"), Anchor);
    FModelNodeData Peer = Anchor;
    Peer.bAlwaysLoaded = false;
    Peer.Transform.SetLocation(FVector(1000, 100, 100));
    Plan.NodeMap.Add(TEXT("Peer"), Peer);
    TArray<FVector> Observers;
    Observers.Add(FVector(300000, 100, 100));
    BuildNativeStreamPlan(Plan, TSet<FName>(), FTransform::Identity, Observers,
        19.0f, 819200.0, 1.1f, true, nullptr);
    TestTrue(TEXT("Worker admits the ordinary peer of a selected cell"),
        Plan.PendingLoadNodes.Contains(TEXT("Peer")));
    const FBox PeerCell = V3DHISMChunkPolicy::CellBounds(V3DHISMChunkPolicy::Key(Peer.Transform.GetLocation()));
    TestFalse(TEXT("Fixture would fail the old per-node callback radius check"),
        V3DHISMChunkPolicy::InRange(PeerCell, Observers, V3DHISMChunkPolicy::ChunkSizeCm * 1.1));

    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Cell admission world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Scene = World->SpawnActor<AStaticActor>();
    if (!TestNotNull(TEXT("Cell admission owner"), Scene)) return false;
    TStrongObjectPtr<UWorldSceneStreamAction> Action(NewObject<UWorldSceneStreamAction>());
    Action->OwnerActor = Scene;
    Action->NodeMap = Plan.NodeMap;
    Action->MeshMap = Plan.MeshMap;
    Action->CurrentRenderObservers = Observers;
    Action->CurrentRenderOwnerTransform = FTransform::Identity;
    Action->CurrentMaxRenderDistanceCm = 819200.0;
    TestTrue(TEXT("Callback preserves the worker's whole-cell admission"),
        Action->IsNodeWithinCurrentRenderRange(TEXT("Peer")));
    Action->CurrentMaxRenderDistanceCm = 10000.0;
    TestFalse(TEXT("Reduced hard cap still rejects a stale asynchronous result"),
        Action->IsNodeWithinCurrentRenderRange(TEXT("Peer")));
    Action->CurrentRenderObservers.Add(Peer.Transform.GetLocation());
    TestTrue(TEXT("Any current observer can keep the node visible"),
        Action->IsNodeWithinCurrentRenderRange(TEXT("Peer")));
    Action->CurrentRenderObservers = Observers;
    Action->CurrentRenderOwnerTransform.SetLocation(Observers[0]);
    TestTrue(TEXT("Callback applies the current scene transform"),
        Action->IsNodeWithinCurrentRenderRange(TEXT("Peer")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSceneFailureDiagnosticTest,
    "V3DSimulator.Streaming.Regression.FailureDiagnosticLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSceneFailureDiagnosticTest::RunTest(const FString&)
{
    TStrongObjectPtr<UWorldSceneStreamAction> Action(NewObject<UWorldSceneStreamAction>());
    Action->CurrentLoadingNode = TEXT("Tree_01");
    Action->CurrentLoadingMesh = TEXT("Tree");
    Action->NodeMap.Add(Action->CurrentLoadingNode, FModelNodeData());
    Action->bStaticMeshLoadInFlight = true;
    Action->FailGroup(TEXT("RemoveInstance"), TEXT("injected removal failure"), TEXT("Tree_02"), TEXT("Tree"));
    TestTrue(TEXT("Failure does not release an outstanding native callback"), Action->bStaticMeshLoadInFlight);
    TestTrue(TEXT("Failed plan releases temporary placements"), Action->NodeMap.IsEmpty());
    Action->FailGroup(TEXT("MeshLoad"), TEXT("later error"));
    TestEqual(TEXT("The first failure is retained"), Action->FailureStage, FString(TEXT("RemoveInstance")));
    TestEqual(TEXT("The failing node survives per-node reset"), Action->FailedNode, FName(TEXT("Tree_02")));
    Action->ResetLoadState();
    TestTrue(TEXT("Load state is released"), Action->CurrentLoadingNode.IsNone());
    TestEqual(TEXT("Error details survive until completion"), Action->FailureReason, FString(TEXT("injected removal failure")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSceneMeshFallbackTest,
    "V3DSimulator.Streaming.Regression.DeferredMeshLODFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSceneMeshFallbackTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Fallback world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AStaticActor* Scene = World->SpawnActor<AStaticActor>();
    AInstancedMeshActor* Group = World->SpawnActor<AInstancedMeshActor>();
    if (!TestNotNull(TEXT("Scene"), Scene) || !TestNotNull(TEXT("Group"), Group)) return false;
    const int Indices[] = {7, 2};
    const auto MakeAction = [&]()
    {
        auto* Action = NewObject<UWorldSceneStreamAction>();
        Action->OwnerActor = Scene;
        Action->MeshActor = Group;
        Action->WorldContextObject = Scene;
        Action->Asset = NewObject<UWorldBakedModelAsset>();
        Action->ObserverLocations.Add(FVector::ZeroVector);
        Action->CurrentLoadingNode = TEXT("Node");
        Action->CurrentLoadingMesh = TEXT("Mesh");
        FModelNodeData Node;
        Node.MeshName = Action->CurrentLoadingMesh;
        Action->NodeMap.Add(Action->CurrentLoadingNode, Node);
        FModelMeshData Mesh;
        Mesh.LOD0 = 7; Mesh.LOD1 = 2; Mesh.Size = FVector(100);
        Action->MeshMap.Add(Action->CurrentLoadingMesh, Mesh);
        Action->MeshLoadRecovery.Begin(Indices, 2, true);
        Action->bIsLoading = true;
        Action->bStaticMeshLoadInFlight = true;
        Action->bWaitingForMeshCallback = true;
        return Action;
    };
    TStrongObjectPtr<UWorldSceneStreamAction> Action(MakeAction());
    // Inject real terminal callback results without starting a glTFRuntime job in this fixture.
    Action->SetStaticMesh(nullptr);
    TestTrue(TEXT("Combined failure queues a later slice, not a reentrant native build"),
        Action->bRetryMeshLoadPending && Action->bIsLoading && !Action->bStaticMeshLoadInFlight);
    TestFalse(TEXT("A recoverable callback does not fail the whole group"), Action->bGroupFailed);
    TestEqual(TEXT("The first fallback keeps authored LOD0"), Action->MeshLoadRecovery.GetMeshIndex(0), 7);
    TestTrue(TEXT("Async completion wakes the action"), World->GetTimerManager().TimerExists(Action->ProcessTimerHandle));
    Action->bRetryMeshLoadPending = false;
    Action->bStaticMeshLoadInFlight = true;
    Action->SetStaticMesh(nullptr);
    TestEqual(TEXT("Failed LOD0 advances to the next authored LOD"), Action->MeshLoadRecovery.GetMeshIndex(0), 2);
    TestFalse(TEXT("Remaining LODs keep the group alive"), Action->bGroupFailed);
    Action->bRetryMeshLoadPending = false;
    Action->bStaticMeshLoadInFlight = true;
    Action->SetStaticMesh(nullptr);
    TestTrue(TEXT("Only an exhausted payload reports a terminal group attempt"),
        Action->bGroupFailed && Action->bMeshPayloadUnavailable && !Action->bRetryMeshLoadPending);
    TestEqual(TEXT("Failure retains the node identity"), Action->FailedNode, FName(TEXT("Node")));
    Action->CancelAndRelease();

    TStrongObjectPtr<UWorldSceneStreamAction> Cancelled(MakeAction());
    Cancelled->SetStaticMesh(nullptr);
    const FTimerHandle RetryTimer = Cancelled->ProcessTimerHandle;
    Cancelled->CancelAndRelease();
    TestFalse(TEXT("Cancellation removes the scheduled fallback"), World->GetTimerManager().TimerExists(RetryTimer));
    TestTrue(TEXT("Cancelled fallback releases its action state"),
        Cancelled->IsFinished() && !Cancelled->bRetryMeshLoadPending && !Cancelled->MeshLoadRecovery.IsInitialized());
    return true;
}

#endif
