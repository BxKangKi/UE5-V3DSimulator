// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedMeshActor.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/ModelData.h"
#include "InstancedMeshActor.generated.h"

class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;

/**
 * One pre-created scene child actor for every unique GroupName.
 *
 * Node definitions are immutable after InitializeGroup(). Runtime instance slots and auxiliary
 * components are owned here, so concurrent stream actions never mutate another mesh group's state.
 */
UCLASS(NotBlueprintable, Transient)
class V3DSIMULATOR_API AInstancedMeshActor final : public AActor
{
    GENERATED_BODY()

public:
    AInstancedMeshActor();

    bool InitializeGroup(
        FName InGroupName,
        FInstancedMeshGroupInitData&& InData);

    FName GetGroupName() const { return GroupName; }
    double GetMaxNodeScale() const { return MaxNodeScale; }
    const FBox& GetNodeBounds() const { return NodeBounds; }
    bool HasAlwaysLoadedNodes() const { return !AlwaysLoadedNodeNames.IsEmpty(); }
    const TMap<FName, FModelNodeData>& GetNodeMapRef() const;
    const TArray<FName>& GetReferencedMeshNames() const { return ReferencedMeshNames; }
    /** Copies complete nearby 1024 m chunks and resident nodes for one stream pass. */
    void BuildStreamNodeSnapshot(
        const FVector& PlayerWorldLocation,
        const FTransform& OwnerWorldTransform,
        float WorldRadius,
        TMap<FName, FModelNodeData>& OutNodes) const;
    TSharedPtr<const FInstancedMeshGroupInitData, ESPMode::ThreadSafe> CaptureDefinitions() const { return GroupDefinitions; }
    /** One planner per group; the set is never mutated until that worker publishes its plan. */
    TSharedPtr<const TSet<FName>, ESPMode::ThreadSafe> CaptureLoadedNodes() const { return LoadedNodes; }
    static void BuildNativeNodeSnapshot(const FInstancedMeshGroupInitData& Definitions,
        const TSet<FName>& Loaded, const FVector& Observer, const FTransform& OwnerTransform,
        float Radius, TMap<FName, FModelNodeData>& OutNodes);
    bool HasLoadedNodes() const { return !LoadedNodes->IsEmpty(); }
    bool IsNodeLoaded(FName NodeName) const { return LoadedNodes->Contains(NodeName); }
    /** Game-thread snapshot used by pure worker-side stream planning. */
    void GetLoadedNodeSnapshot(TSet<FName>& OutLoadedNodes) const;
    UInstancedStaticMeshComponent* GetMeshComponent() const { return MeshComponent.Get(); }

    UInstancedStaticMeshComponent* AssignStaticMesh(UStaticMesh* Mesh);
    bool AddNodeInstance(FName NodeName, const FTransform& LocalTransform);
    bool RemoveNodeInstance(FName NodeName);
    /** Starts at most two bounded async tree builds per slice; never waits on the game thread. */
    bool FlushInstanceRenderUpdates(bool bFinalize = false);
    bool CanMutateNode(FName NodeName) const;
    bool AreChunkTreesReady() const;

    bool HasDynamicComponents(FName NodeName) const;
    void StoreDynamicComponents(FName NodeName, FComponentGroup&& Group);
    void DestroyDynamicComponents(FName NodeName);

    void ReleaseRuntimeResources();

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    double MaxNodeScale = 1.0;
    FBox NodeBounds = FBox(ForceInit);
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    /** Empty template holding the one shared mesh/material/collision configuration for all chunks. */
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UInstancedStaticMeshComponent> MeshComponent;

    // Native node tables contain no UObject references and remain alive through worker teardown.
    TSharedPtr<const FInstancedMeshGroupInitData, ESPMode::ThreadSafe> GroupDefinitions;
    TSharedPtr<TSet<FName>, ESPMode::ThreadSafe> LoadedNodes = MakeShared<TSet<FName>, ESPMode::ThreadSafe>();

    UPROPERTY(Transient)
    TMap<FName, FComponentGroup> DynamicComponentMap;

    FName GroupName = NAME_None;
    struct FInstanceSlot { FName Page; int32 Index = INDEX_NONE; };
    struct FChunkPage { FIntVector Chunk; TArray<FName> Nodes; };
    UPROPERTY(Transient)
    TMap<FName, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> ChunkComponents;
    TMap<FName, FChunkPage> ChunkPages;
    TMap<FIntVector, FName> OpenChunkPages;
    TMap<FName, FInstanceSlot> InstanceSlots;
    TSet<FName> DirtyPages;
    int32 NextPageId = 0;
    UHierarchicalInstancedStaticMeshComponent* CreateChunkComponent(const FIntVector& Chunk, FName& OutPage);
    void DestroyChunkComponent(FName Page);
    TArray<FName> AlwaysLoadedNodeNames;
    TArray<FName> ReferencedMeshNames;
    bool bRuntimeResourcesReleased = false;
};
