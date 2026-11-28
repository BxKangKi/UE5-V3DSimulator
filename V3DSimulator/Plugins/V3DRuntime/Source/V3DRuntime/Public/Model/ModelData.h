// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ModelData.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Scene.h"
#include "ModelData.generated.h"

class UShapeComponent;
class ULightComponent;

UENUM(BlueprintType)
enum class EColliderType : uint8
{
    None UMETA(DisplayName = "None"),
    Sphere UMETA(DisplayName = "Sphere"),
    Capsule UMETA(DisplayName = "Capsule"),
    Box UMETA(DisplayName = "Box"),
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FModelCollider
{
    GENERATED_BODY()

    UPROPERTY()
    EColliderType Collider = EColliderType::None;
    UPROPERTY()
    FVector Center = FVector::ZeroVector;
    UPROPERTY()
    FVector Size = FVector::ZeroVector;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

UENUM(BlueprintType)
enum class ELightType : uint8
{
    Point = 0,
    Spot = 1,
    Rect = 2
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FLightData
{
    GENERATED_BODY()

    UPROPERTY()
    ELightType Type = ELightType::Point;
    UPROPERTY()
    FVector Location = FVector::ZeroVector;
    UPROPERTY()
    FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY()
    FLinearColor Color = FLinearColor::White;
    UPROPERTY()
    float Intensity = 10.0f;
    UPROPERTY()
    float SourceRadius = 10.0f;
    UPROPERTY()
    float SoftSourceRadius = 10.0f;
    UPROPERTY()
    float AttenuationRadius = 1000.0f;
    UPROPERTY()
    float Length = 10.0f;
    UPROPERTY()
    float InnerConeAngle = 22.0f;
    UPROPERTY()
    float OuterConeAngle = 44.0f;
    UPROPERTY()
    float SourceWidth = 64.0f;
    UPROPERTY()
    float SourceHeight = 64.0f;
    UPROPERTY()
    float BarnDoorAngle = 88.0f;
    UPROPERTY()
    float BarnDoorLength = 20.0f;
    UPROPERTY()
    ELightUnits Unit = ELightUnits::Unitless;

    TSharedRef<FJsonObject>
    Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FMeshData
{
    GENERATED_BODY()

    UPROPERTY()
    bool bComplexCollision = true;

    UPROPERTY()
    bool bSimpleCollision = false;

    UPROPERTY()
    bool bIsEntity = false;

    UPROPERTY()
    bool bGenerateImpostor = true;

    UPROPERTY()
    TArray<FModelCollider> Colliders;

    UPROPERTY()
    TArray<FLightData> Lights;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FModelData
{
    GENERATED_BODY()

    /** Build-derived model center stored in the .v3d directory, never in author JSON. */
    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    /** Build-derived full model size stored in the .v3d directory, never in author JSON. */
    UPROPERTY()
    FVector Size = FVector::ZeroVector;

    /** User-authored read-only JSON settings, keyed by base mesh name. */
    UPROPERTY()
    TMap<FName, FMeshData> MeshData;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FModelMeshData
{
    GENERATED_BODY()

    UPROPERTY()
    int32 LOD0 = INDEX_NONE;

    UPROPERTY()
    int32 LOD1 = INDEX_NONE;

    UPROPERTY()
    int32 LOD2 = INDEX_NONE;

    UPROPERTY()
    int32 LOD3 = INDEX_NONE;

    UPROPERTY()
    FMeshData Data;

    /** Conservative unscaled extent about the mesh pivot, enclosing every authored LOD. */
    UPROPERTY()
    FVector Extent = FVector::ZeroVector;

    /** Twice Extent; no local bounds centre is stored in the .v3d metadata format. */
    UPROPERTY()
    FVector Size = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct V3DRUNTIME_API FModelNodeData
{
    GENERATED_BODY()

    UPROPERTY()
    FName MeshName = NAME_None;

    UPROPERTY()
    FTransform Transform = FTransform::Identity;

    /** 8192 m model-space coarse chunk. */
    UPROPERTY()
    FIntVector CoarseChunk = FIntVector::ZeroValue;

    /** 512 m child chunk, stored as 0..15 coordinates relative to CoarseChunk. */
    UPROPERTY()
    FIntVector FineChunk = FIntVector::ZeroValue;

    /** Oversized mesh bypasses spatial buckets/screen-size culling, but never the maximum render distance. */
    UPROPERTY()
    bool bAlwaysLoaded = false;
};

/** Native 1024 m residency chunk. Bounds include mesh extents, in the scene's local space. */
struct V3DRUNTIME_API FInstancedMeshStreamChunk
{
    FBox LocalBounds = FBox(ForceInit);
    TArray<FName> Nodes;
    bool bHasAlwaysLoadedNodes = false;
};

/** Pure native instanced-group payload prepared on a worker before UObject publication. */
struct V3DRUNTIME_API FInstancedMeshGroupInitData
{
    FBox NodeBounds = FBox(ForceInit);
    double MaxNodeScale = 0.0;
    TMap<FName, FModelNodeData> Nodes;
    TArray<FName> AlwaysLoadedNodeNames;
    TArray<FName> ReferencedMeshNames;
    TMap<FIntVector, FInstancedMeshStreamChunk> StreamChunks;

    bool IsValid() const
    {
        return !Nodes.IsEmpty() && NodeBounds.IsValid && !NodeBounds.Min.ContainsNaN()
            && !NodeBounds.Max.ContainsNaN() && FMath::IsFinite(MaxNodeScale) && MaxNodeScale >= 0.0;
    }
};


USTRUCT(BlueprintType)
struct V3DRUNTIME_API FWaterStreamNodeData
{
    GENERATED_BODY()

    UPROPERTY()
    FTransform Transform = FTransform::Identity;

    UPROPERTY()
    float StreamRadius = 65536.0f;
};

/** Render-oriented scene tables decoded from one model's metadata.dat member. */
USTRUCT(BlueprintType)
struct V3DRUNTIME_API FGWorldSceneRenderData
{
    GENERATED_BODY()

    /** True only when definition parsing and the complete bounded node scan finished. */
    UPROPERTY()
    bool bSuccess = false;

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;

    UPROPERTY()
    FModelData ModelData;
};

