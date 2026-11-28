// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedMeshActor.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/InstancedMeshActor.h"
#include "System/WorldSurfaceSubsystem.h"
#include "Simulator/NodeTokenLibrary.h"
#include "World/RailTrackSubsystem.h"
#include "System/V3DStreamingBudget.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Model/V3DHISMChunkPolicy.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Engine/StaticMesh.h"
#include "Model/V3DMeshRendering.h"

AInstancedMeshActor::AInstancedMeshActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SetReplicates(false);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);

    MeshComponent = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("InstancedMesh"));
    MeshComponent->SetupAttachment(Root);
    MeshComponent->SetMobility(EComponentMobility::Movable);
    V3DMeshRendering::ConfigureWorldMesh(MeshComponent);
    MeshComponent->SetGenerateOverlapEvents(false);
    MeshComponent->SetCanEverAffectNavigation(false);
    MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    MeshComponent->SetCollisionResponseToAllChannels(ECR_Ignore);
}

bool AInstancedMeshActor::InitializeGroup(
    const FName InGroupName,
    FInstancedMeshGroupInitData&& InData)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("InitializeGroup must run on the game thread"))
        || InGroupName.IsNone() || !InData.IsValid())
    {
        return false;
    }

    GroupName = InGroupName;
    NodeBounds = InData.NodeBounds;
    MaxNodeScale = InData.MaxNodeScale;
    ReferencedMeshNames = MoveTemp(InData.ReferencedMeshNames);
    GroupDefinitions = MakeShared<FInstancedMeshGroupInitData, ESPMode::ThreadSafe>(MoveTemp(InData));
    LoadedNodes = MakeShared<TSet<FName>, ESPMode::ThreadSafe>();
    InstanceSlots.Empty();
    DynamicComponentMap.Empty();
    bRuntimeResourcesReleased = false;
    return true;
}

const TMap<FName, FModelNodeData>& AInstancedMeshActor::GetNodeMapRef() const
{
    static const TMap<FName, FModelNodeData> Empty;
    return GroupDefinitions.IsValid() ? GroupDefinitions->Nodes : Empty;
}

void AInstancedMeshActor::BuildStreamNodeSnapshot(const FVector& Observer, const FTransform& OwnerTransform,
    const float Radius, TMap<FName, FModelNodeData>& OutNodes) const
{
    check(IsInGameThread());
    if (GroupDefinitions.IsValid() && LoadedNodes.IsValid()) BuildNativeNodeSnapshot(*GroupDefinitions, *LoadedNodes, Observer, OwnerTransform, Radius, OutNodes,
        V3DStreamingPolicy::GetMaxRenderDistanceCm(this));
    else OutNodes.Reset();
}

void AInstancedMeshActor::BuildNativeNodeSnapshot(const FInstancedMeshGroupInitData& Definitions,
    const TSet<FName>& Loaded, const FVector& Observer, const FTransform& OwnerTransform,
    const float Radius, TMap<FName, FModelNodeData>& OutNodes, const double MaxDistanceCm)
{
    // Detached native tables only; no UObject, world, or component access on this worker path.
    OutNodes.Reset();
    if (!FMath::IsFinite(Radius) || Radius < 0.0f) return;
    OutNodes.Reserve(Loaded.Num() + 256);
    for (const FName Name : Loaded)
        if (const FModelNodeData* Node = Definitions.Nodes.Find(Name)) OutNodes.Add(Name, *Node);
    if (OwnerTransform.ContainsNaN() || Observer.ContainsNaN()) return;
    if (Definitions.StreamChunks.IsEmpty()) { OutNodes = Definitions.Nodes; return; }
    for (const auto& Pair : Definitions.StreamChunks)
    {
        const FBox Bounds = Pair.Value.LocalBounds.TransformBy(OwnerTransform);
        const double QueryRadius = Pair.Value.bHasAlwaysLoadedNodes
            ? FMath::Max(double(Radius), MaxDistanceCm) : double(Radius);
        if (!Bounds.IsValid || Bounds.ComputeSquaredDistanceToPoint(Observer) > FMath::Square(QueryRadius)) continue;
        for (const FName Name : Pair.Value.Nodes)
            if (const FModelNodeData* Node = Definitions.Nodes.Find(Name)) OutNodes.Add(Name, *Node);
    }
}

void AInstancedMeshActor::GetLoadedNodeSnapshot(TSet<FName>& OutLoadedNodes) const
{
    check(IsInGameThread());
    if (LoadedNodes.IsValid()) OutLoadedNodes = *LoadedNodes;
    else OutLoadedNodes.Reset();
}

UInstancedStaticMeshComponent* AInstancedMeshActor::AssignStaticMesh(UStaticMesh* Mesh)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AssignStaticMesh must run on the game thread"))
        || !IsValid(MeshComponent) || !IsValid(Mesh))
    {
        return nullptr;
    }

    V3DMeshRendering::ConfigureWorldMesh(MeshComponent);
    UStaticMesh* ExistingMesh = MeshComponent->GetStaticMesh();
    if (IsValid(ExistingMesh) && ExistingMesh != Mesh && LoadedNodes.IsValid() && !LoadedNodes->IsEmpty())
    {
        return nullptr;
    }
    if (ExistingMesh != Mesh)
    {
        MeshComponent->EmptyOverrideMaterials();
        MeshComponent->SetPhysMaterialOverride(nullptr);
        MeshComponent->SetStaticMesh(Mesh);
        if (UWorldSurfaceSubsystem* Surfaces = UWorldSurfaceSubsystem::Get(this))
            Surfaces->ApplyToStaticMesh(MeshComponent);
    }
    return MeshComponent;
}

UHierarchicalInstancedStaticMeshComponent* AInstancedMeshActor::CreateChunkComponent(
    const FIntVector& Chunk, FName& OutPage)
{
    check(IsInGameThread());
    if (!IsValid(Root) || !IsValid(MeshComponent)) return nullptr;
    OutPage = FName(*FString::Printf(TEXT("HISM_%d_%d_%d_%d"), Chunk.X, Chunk.Y, Chunk.Z, NextPageId++));
    auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, OutPage, RF_Transient);
    if (!IsValid(Component)) return nullptr;
    Component->bAutoRebuildTreeOnInstanceChanges = false;
    Component->bEnableDensityScaling = false;
    Component->SetForcedLodModel(0);
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetupAttachment(Root);
    Component->SetGenerateOverlapEvents(false);
    Component->SetCullDistances(MeshComponent->InstanceStartCullDistance, MeshComponent->InstanceEndCullDistance);
    Component->SetCanEverAffectNavigation(MeshComponent->CanEverAffectNavigation());
    Component->SetStaticMesh(MeshComponent->GetStaticMesh());
    Component->SetCollisionProfileName(MeshComponent->GetCollisionProfileName());
    Component->SetCollisionEnabled(MeshComponent->GetCollisionEnabled());
    Component->SetCollisionObjectType(MeshComponent->GetCollisionObjectType());
    Component->SetCollisionResponseToChannels(MeshComponent->GetCollisionResponseToChannels());
    Component->SetRenderCustomDepth(MeshComponent->bRenderCustomDepth);
    Component->SetCustomDepthStencilValue(MeshComponent->CustomDepthStencilValue);
    V3DMeshRendering::ConfigureWorldMesh(Component);
    if (UWorldSurfaceSubsystem* Surfaces = UWorldSurfaceSubsystem::Get(this))
        Surfaces->ApplyToStaticMesh(Component);
    Component->PreAllocateInstancesMemory(V3DHISMChunkPolicy::MaxInstancesPerComponent);
    AddInstanceComponent(Component);
    Component->RegisterComponent();
    ChunkComponents.Add(OutPage, Component);
    FChunkPage& Page = ChunkPages.Add(OutPage);
    Page.Chunk = Chunk;
    Page.Nodes.Reserve(V3DHISMChunkPolicy::MaxInstancesPerComponent);
    OpenChunkPages.Add(Chunk, OutPage);
    return Component;
}

bool AInstancedMeshActor::CanMutateNode(const FName NodeName) const
{
    if (const FInstanceSlot* Slot = InstanceSlots.Find(NodeName))
        if (const auto* Component = ChunkComponents.Find(Slot->Page))
            return IsValid(Component->Get()) && !Component->Get()->IsAsyncBuilding();
    return true;
}

bool AInstancedMeshActor::AddNodeInstance(const FName NodeName, const FTransform& LocalTransform)
{
    check(IsInGameThread());
    FWorldPreparedInstance Row;
    Row.NodeName = NodeName; Row.LocalTransform = LocalTransform;
    Row.WorldChunk = V3DHISMChunkPolicy::Key(GetActorTransform().TransformPosition(LocalTransform.GetLocation()));
    Row.bValid = !LocalTransform.ContainsNaN() && LocalTransform.GetRotation().IsNormalized();
    TArray<FName> Added;
    return AddNodeInstances(MakeArrayView(&Row, 1), Added);
}

void AInstancedMeshActor::SetInstanceCullDistances(int32 Start, int32 End)
{
    check(IsInGameThread());
    if (!IsValid(MeshComponent)) return;
    Start = FMath::Max(0, Start); End = FMath::Max(Start, End);
    if (MeshComponent->InstanceStartCullDistance == Start && MeshComponent->InstanceEndCullDistance == End) return;
    MeshComponent->SetCullDistances(Start, End);
    for (const auto& Pair : ChunkComponents)
        if (IsValid(Pair.Value)) Pair.Value->SetCullDistances(Start, End);
}

bool AInstancedMeshActor::AddNodeInstances(TConstArrayView<FWorldPreparedInstance> Instances, TArray<FName>& OutAdded)
{
    check(IsInGameThread());
    OutAdded.Reset();
    if (bRuntimeResourcesReleased || !LoadedNodes.IsValid() || !IsValid(MeshComponent) || !IsValid(MeshComponent->GetStaticMesh())) return false;
    TSet<FName> Seen;
    TArray<const FWorldPreparedInstance*> Rows;
    // Reject invalid/duplicate input before mutating components or slot bookkeeping.
    for (const auto& Row : Instances)
    {
        if (!Row.bValid || Row.NodeName.IsNone() || Row.LocalTransform.ContainsNaN()
            || !Row.LocalTransform.GetRotation().IsNormalized() || !GetNodeMapRef().Contains(Row.NodeName)) return false;
        if (LoadedNodes->Contains(Row.NodeName) || Seen.Contains(Row.NodeName)) continue;
        Seen.Add(Row.NodeName); Rows.Add(&Row);
    }
    int32 Offset = 0;
    while (Offset < Rows.Num())
    {
        const FIntVector Chunk = Rows[Offset]->WorldChunk;
        FName PageName = OpenChunkPages.FindRef(Chunk);
        auto* Component = ChunkComponents.FindRef(PageName).Get();
        if (!IsValid(Component) || Component->IsAsyncBuilding()
            || Component->GetNumInstances() >= V3DHISMChunkPolicy::MaxInstancesPerComponent)
            Component = CreateChunkComponent(Chunk, PageName);
        if (!IsValid(Component)) return false;
        const int32 Capacity = V3DHISMChunkPolicy::MaxInstancesPerComponent - Component->GetNumInstances();
        TArray<FTransform> Transforms;
        Transforms.Reserve(FMath::Min(Capacity, Rows.Num() - Offset));
        while (Transforms.Num() < Capacity && Offset + Transforms.Num() < Rows.Num()
            && Rows[Offset + Transforms.Num()]->WorldChunk == Chunk)
            Transforms.Add(Rows[Offset + Transforms.Num()]->LocalTransform);
        const int32 FirstIndex = Component->GetNumInstances();
        const TArray<int32> Indices = Component->AddInstances(Transforms, true, false, false);
        bool bComplete = Indices.Num() == Transforms.Num();
        for (int32 Index = 0; bComplete && Index < Indices.Num(); ++Index) bComplete = Indices[Index] == FirstIndex + Index;
        if (!bComplete)
        {
            // HISM appends synchronously on GT. Roll back only this append, in reverse order.
            for (int32 Index = Component->GetNumInstances() - 1; Index >= FirstIndex; --Index) Component->RemoveInstance(Index);
            if (FirstIndex == 0) DestroyChunkComponent(PageName);
            else DirtyPages.Add(PageName);
            return false;
        }
        FChunkPage& Page = ChunkPages.FindChecked(PageName);
        URailTrackSubsystem* RailTracks = URailTrackSubsystem::Get(this);
        for (int32 Index = 0; Index < Indices.Num(); ++Index)
        {
            const FName Name = Rows[Offset + Index]->NodeName;
            Page.Nodes.Add(Name);
            InstanceSlots.Add(Name, FInstanceSlot{PageName, Indices[Index]});
            LoadedNodes->Add(Name); OutAdded.Add(Name);
            if (RailTracks && USimulatorNodeTokenLibrary::HasEffectiveToken(Name.ToString(), FName(TEXT("RAIL"))))
            {
                const FTransform WorldInstTransform = Rows[Offset + Index]->LocalTransform * GetActorTransform();
                RailTracks->RegisterRailMeshInstance(this, Name, MeshComponent->GetStaticMesh(), WorldInstTransform);
            }
        }
        DirtyPages.Add(PageName);
        Offset += Transforms.Num();
    }
    return true;
}

void AInstancedMeshActor::DestroyChunkComponent(const FName PageName)
{
    if (auto* Component = ChunkComponents.FindRef(PageName).Get())
    {
        // A page never exceeds 256 instances; release cost is bounded even for huge chunks.
        // Teardown may race a tree task. Destroy/unregister without mutating that task's input.
        // Normal streaming only enters here after CanMutateNode permits the removal.
        if (!Component->IsAsyncBuilding())
        {
            Component->ClearInstances();
            Component->EmptyOverrideMaterials();
            Component->SetPhysMaterialOverride(nullptr);
            Component->SetStaticMesh(nullptr);
        }
        RemoveInstanceComponent(Component);
        Component->DestroyComponent();
    }
    if (const FChunkPage* Page = ChunkPages.Find(PageName))
        if (OpenChunkPages.FindRef(Page->Chunk) == PageName) OpenChunkPages.Remove(Page->Chunk);
    ChunkComponents.Remove(PageName);
    ChunkPages.Remove(PageName);
    DirtyPages.Remove(PageName);
}

bool AInstancedMeshActor::UnloadWholeClusterPageForNode(
    const FName NodeName,
    const TSet<FName>& PendingUnloadSet,
    TArray<FName>& OutRemovedNodes)
{
    check(IsInGameThread());
    OutRemovedNodes.Reset();
    const FInstanceSlot* SlotPtr = InstanceSlots.Find(NodeName);
    if (!SlotPtr) return false;
    const FName PageName = SlotPtr->Page;
    auto* Component = ChunkComponents.FindRef(PageName).Get();
    FChunkPage* Page = ChunkPages.Find(PageName);
    if (!IsValid(Component) || !Page || Component->IsAsyncBuilding()) return false;
    // Only destroy the entire HISM component when every node in this page is unloading.
    for (const FName& PageNode : Page->Nodes)
    {
        if (!PendingUnloadSet.Contains(PageNode)) return false;
    }
    OutRemovedNodes = Page->Nodes;
    for (const FName& RemovedNode : OutRemovedNodes)
    {
        InstanceSlots.Remove(RemovedNode);
        if (LoadedNodes.IsValid()) LoadedNodes->Remove(RemovedNode);
    }
    DestroyChunkComponent(PageName);
    if (!LoadedNodes.IsValid() || LoadedNodes->IsEmpty())
    {
        if (LoadedNodes.IsValid()) LoadedNodes->Empty();
        InstanceSlots.Empty();
        ChunkComponents.Empty();
        ChunkPages.Empty();
        OpenChunkPages.Empty();
        DirtyPages.Empty();
        if (DynamicComponentMap.IsEmpty()) DynamicComponentMap.Empty();
        if (IsValid(MeshComponent))
        {
            MeshComponent->SetStaticMesh(nullptr);
            MeshComponent->EmptyOverrideMaterials();
            MeshComponent->SetPhysMaterialOverride(nullptr);
        }
        FV3DStreamingBudget::NotifyUnloaded();
    }
    return true;
}

bool AInstancedMeshActor::RemoveNodeInstance(const FName NodeName)
{
    check(IsInGameThread());
    const FInstanceSlot* SlotPtr = InstanceSlots.Find(NodeName);
    if (!SlotPtr) { if (LoadedNodes.IsValid()) LoadedNodes->Remove(NodeName); return false; }
    const FInstanceSlot Slot = *SlotPtr;
    auto* Component = ChunkComponents.FindRef(Slot.Page).Get();
    FChunkPage* Page = ChunkPages.Find(Slot.Page);
    if (!IsValid(Component) || !Page || Component->IsAsyncBuilding() || !Page->Nodes.IsValidIndex(Slot.Index)) return false;
    const bool bSwap = Component->SupportsRemoveSwap();
    if (!Component->RemoveInstance(Slot.Index)) return false;
    if (bSwap)
    {
        Page->Nodes.RemoveAtSwap(Slot.Index, 1, EAllowShrinking::No);
        if (Page->Nodes.IsValidIndex(Slot.Index))
            if (FInstanceSlot* Swapped = InstanceSlots.Find(Page->Nodes[Slot.Index])) Swapped->Index = Slot.Index;
    }
    else
    {
        Page->Nodes.RemoveAt(Slot.Index, 1, EAllowShrinking::No);
        for (int32 Index = Slot.Index; Index < Page->Nodes.Num(); ++Index)
            if (FInstanceSlot* Shifted = InstanceSlots.Find(Page->Nodes[Index])) Shifted->Index = Index;
    }
    InstanceSlots.Remove(NodeName);
    if (LoadedNodes.IsValid()) LoadedNodes->Remove(NodeName);
    if (Page->Nodes.IsEmpty()) DestroyChunkComponent(Slot.Page);
    else DirtyPages.Add(Slot.Page);
    if (!LoadedNodes.IsValid() || LoadedNodes->IsEmpty())
    {
        if (LoadedNodes.IsValid()) LoadedNodes->Empty();
        InstanceSlots.Empty();
        ChunkComponents.Empty();
        ChunkPages.Empty();
        OpenChunkPages.Empty();
        DirtyPages.Empty();
        if (DynamicComponentMap.IsEmpty()) DynamicComponentMap.Empty();
        if (IsValid(MeshComponent))
        {
            MeshComponent->SetStaticMesh(nullptr);
            MeshComponent->EmptyOverrideMaterials();
            MeshComponent->SetPhysMaterialOverride(nullptr);
        }
        FV3DStreamingBudget::NotifyUnloaded();
    }
    return true;
}

bool AInstancedMeshActor::FlushInstanceRenderUpdates(const bool bFinalize)
{
    check(IsInGameThread());
    int32 BuildsStarted = 0;
    for (auto It = DirtyPages.CreateIterator(); It; ++It)
    {
        auto* Component = ChunkComponents.FindRef(*It).Get();
        if (!IsValid(Component)) { It.RemoveCurrent(); continue; }
        if (Component->IsAsyncBuilding()) continue;
        if (!bFinalize && Component->GetNumInstances() < V3DHISMChunkPolicy::MaxInstancesPerComponent) continue;
        // No synchronous BuildTree, task waits, or per-instance automatic rebuilds.
        Component->BuildTreeIfOutdated(true, false);
        It.RemoveCurrent();
        if (++BuildsStarted >= 1) break;
    }
    return DirtyPages.IsEmpty();
}

bool AInstancedMeshActor::AreChunkTreesReady() const
{
    if (!DirtyPages.IsEmpty()) return false;
    for (const auto& Pair : ChunkComponents)
        if (IsValid(Pair.Value.Get()) && !Pair.Value->IsTreeFullyBuilt()) return false;
    return true;
}

bool AInstancedMeshActor::HasDynamicComponents(const FName NodeName) const
{
    const FComponentGroup* Group = DynamicComponentMap.Find(NodeName);
    return Group && (!Group->Colliders.IsEmpty() || !Group->Lights.IsEmpty());
}

void AInstancedMeshActor::StoreDynamicComponents(const FName NodeName, FComponentGroup&& Group)
{
    check(IsInGameThread());
    if (!NodeName.IsNone())
    {
        DynamicComponentMap.Add(NodeName, MoveTemp(Group));
    }
}

void AInstancedMeshActor::DestroyDynamicComponents(const FName NodeName)
{
    check(IsInGameThread());
    FComponentGroup* Group = DynamicComponentMap.Find(NodeName);
    if (!Group)
    {
        return;
    }

    const auto DestroyOwnedComponent = [this](UActorComponent* Component)
    {
        if (IsValid(Component))
        {
            RemoveInstanceComponent(Component);
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    };
    for (UShapeComponent* Collider : Group->Colliders)
    {
        DestroyOwnedComponent(Collider);
    }
    for (ULightComponent* Light : Group->Lights)
    {
        DestroyOwnedComponent(Light);
    }
    DynamicComponentMap.Remove(NodeName);
}

void AInstancedMeshActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("InstancedMeshActor release must run on the game thread"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;
    if (URailTrackSubsystem* RailTracks = URailTrackSubsystem::Get(this))
    {
        RailTracks->UnregisterRailsForActor(this);
    }

    TArray<FName> DynamicNodes;
    DynamicComponentMap.GetKeys(DynamicNodes);
    for (const FName NodeName : DynamicNodes)
    {
        DestroyDynamicComponents(NodeName);
    }
    TArray<FName> Pages;
    ChunkComponents.GetKeys(Pages);
    for (const FName Page : Pages) DestroyChunkComponent(Page);
    ChunkComponents.Empty();
    ChunkPages.Empty();
    DynamicComponentMap.Empty();
    OpenChunkPages.Empty();
    InstanceSlots.Empty();
    DirtyPages.Empty();
    if (IsValid(MeshComponent))
    {
        MeshComponent->ClearInstances();
        for (int32 Index = 0; Index < MeshComponent->GetNumMaterials(); ++Index)
        {
            MeshComponent->SetMaterial(Index, nullptr);
        }
        MeshComponent->SetStaticMesh(nullptr);
        MeshComponent->EmptyOverrideMaterials();
        MeshComponent->SetPhysMaterialOverride(nullptr);
        FV3DStreamingBudget::NotifyUnloaded();
    }
    GroupDefinitions.Reset();
    ReferencedMeshNames.Empty();
    LoadedNodes = MakeShared<TSet<FName>, ESPMode::ThreadSafe>();
    InstanceSlots.Empty();
    GroupName = NAME_None;
}

void AInstancedMeshActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void AInstancedMeshActor::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}
