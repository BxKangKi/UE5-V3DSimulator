// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicPointLightComponent.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Components/PointLightComponent.h"
#include "DynamicPointLightComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class V3DSIMULATOR_API UDynamicPointLightComponent : public UPointLightComponent
{
    GENERATED_BODY()

public:
    UDynamicPointLightComponent();

    UMaterialInterface *GetLightDecal() const { return LightDecal.Get(); }
    void SetLightDecal(UMaterialInterface *InDecal);

    bool IsLightDecalFallbackEnabled() const { return bEnableLightDecalFallback; }
    void SetLightDecalFallbackEnabled(bool bEnable);

    float GetCullingDistance() const { return CullingDistance; }
    void SetCullingDistance(float InDistance);

    float GetDecalTransitionDistance() const { return DecalTransitionDistance; }
    void SetDecalTransitionDistance(float InDistance);

    float GetMaxLightDecalSize() const { return MaxLightDecalSize; }
    float GetMaxLightDecalOpacity() const { return MaxLightDecalOpacity; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnRegister() override;
    virtual void OnUnregister() override;

private:
    void NotifySubsystemRefresh();

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> LightDecal;

    /** Enables lightweight decal fallback when the dynamic point light is distance-culled. */
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal")
    bool bEnableLightDecalFallback = true;

    /** Disables the dynamic point light when the camera is farther than this distance. */
    UPROPERTY(EditAnywhere, Category = "Optimization", meta = (UIMin = "0.0", ClampMin = "0.0"))
    float CullingDistance = 10000.0f;

    /** Max distance for the fallback decal. 0 keeps the decal visible until the owning actor/impostor unloads. */
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "0.0", ClampMin = "0.0"))
    float DecalTransitionDistance = 0.0f;

    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "1.0", ClampMin = "1.0"))
    float MaxLightDecalSize = 2500.0f;

    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "0.0", ClampMin = "0.0", ClampMax = "1.0"))
    float MaxLightDecalOpacity = 0.35f;
};
