// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicLightSubsystem.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements quality-scaled dynamic light budgeting, shadow capping,
 * hysteresis distance culling, and strictly throttled (1-2 per tick) light/decal transitions.
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Components/DecalComponent.h"
#include "DynamicLightSubsystem.generated.h"

class UDynamicPointLightComponent;
class UGameUpdateSubSystem;

/** Compact state used to batch distance culling and avoid redundant render-state changes. */
struct FLightOptimizationData
{
    FVector Position = FVector::ZeroVector;
    float CullingDistanceSq = 0.0f;
    float DecalTransitionDistanceSq = 0.0f;
    float LastDistanceSq = FLT_MAX;

    TWeakObjectPtr<UDynamicPointLightComponent> LightComponent;
    TWeakObjectPtr<UDecalComponent> DecalComponent;
    TWeakObjectPtr<UMaterialInterface> TargetDecalMaterial;

    // Authored shadow preference preserved across dynamic quality-tier capping.
    bool bAuthoredCastShadows = false;
    bool bTargetCastShadows = false;
    bool bCurrentCastShadows = false;

    // Desired visibility calculated during the current update.
    bool bTargetLightVisibility = false;
    bool bTargetDecalVisibility = false;

    // Cached visibility state used to avoid redundant SetVisibility calls.
    bool bCurrentLightVisibility = false;
    bool bCurrentDecalVisibility = false;
};

UCLASS()
class V3DSIMULATOR_API UDynamicLightSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;

    void RegisterLight(UDynamicPointLightComponent *InLight);
    void UnregisterLight(UDynamicPointLightComponent *InLight);

private:
    TArray<FLightOptimizationData> ManagedLights;
    TArray<TPair<float, int32>> ScratchRankedLights;
    TArray<TPair<float, int32>> ScratchDecalCandidates;
    TArray<TPair<float, int32>> ScratchPendingTransitions;
    int32 GameUpdateHandle = INDEX_NONE;
    float CullingAccumulator = .2f;

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void CompactManagedLights();
    void UpdateLightsFromGameUpdate(float DeltaTime);

    // Internal helper for lazy decal component creation.
    UDecalComponent *CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material);
};
