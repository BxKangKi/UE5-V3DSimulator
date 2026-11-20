#pragma once
#include "CoreMinimal.h"
#include "Model/ModelData.h"
#include "WorldSceneStreamTypes.generated.h"
class AWaterActor;
class UShapeComponent;
class ULightComponent;
/** Detached worker result. No UObject access is needed to validate/place a transform. */
struct FWorldPreparedInstance
{
    FName NodeName;
    FTransform LocalTransform = FTransform::Identity;
    FIntVector WorldChunk = FIntVector::ZeroValue;
    bool bValid = false;
};
/** Runtime collider/light components owned by one streamed scene node. */
USTRUCT()
struct V3DSIMULATOR_API FComponentGroup
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TObjectPtr<UShapeComponent>> Colliders;
    UPROPERTY()
    TArray<TObjectPtr<ULightComponent>> Lights;
};

/** Terminal result from one mesh-group or water-group .v3d streaming pass. */
USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FWorldSceneStreamResult
{
    GENERATED_BODY()

    /** Mesh name processed by this action. NAME_None identifies the water-only group. */
    UPROPERTY()
    FName GroupName;

    UPROPERTY()
    bool bWaterGroup = false;

    UPROPERTY()
    bool bGroupFailed = false;

    /** All valid LOD requests failed. Excludes cancellation and instance/owner failures. */
    UPROPERTY()
    bool bMeshPayloadUnavailable = false;

    /** First failure of this pass, captured before per-node load state is released. */
    UPROPERTY()
    FString FailureStage;
    UPROPERTY()
    FString FailureReason;
    UPROPERTY()
    FName FailedNode;
    UPROPERTY()
    FName FailedMesh;

    UPROPERTY()
    TSet<FName> LoadedWaterNodes;
    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<AWaterActor>> WaterActorMap;
};
