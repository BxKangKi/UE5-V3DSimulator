// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldEnvManager.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "World/FogProfileMath.h"
#include "WorldEnvManager.generated.h"

class UGameManagerSubSystem;
class UDirectionalLightComponent;
class UWorldData;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class UStaticMeshComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
struct FStreamableHandle;
class UExponentialHeightFogComponent;
class UGameUpdateSubSystem;
class UUnderwaterShaftOcclusionComponent;

/**
 * Rendering-only world actor.
 *
 * GameManagerSubSystem owns loading, time, saving, water, and streamed model spawning.
 * WorldEnvManager only owns sky/fog/cloud/light components and continuously reflects the current UWorldData.
 * The default sky is procedural SkyAtmosphere, so a missing or uncooked sky-dome mesh can never
 * turn the packaged world black.
 */
UCLASS(Blueprintable, BlueprintType)
class V3DSIMULATOR_API AWorldEnvManager : public AActor
{
    GENERATED_BODY()

public:
    AWorldEnvManager();

    /**
     * Makes the dependency-free procedural sky visible before config/.dat/.v3d I/O starts.
     * This is deliberately separate from InitializeRendering because archive/config startup can
     * take time and no UWorldData exists during that interval.
     */
    void PrepareForWorldLoading();

    /** Starts sky/light rendering updates from the supplied world data object. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void InitializeRendering(UWorldData* InWorldData);

    /** Stops scheduled rendering updates without touching gameplay-owned systems. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void StopRendering();

    /** Exposes the active world data for debug widgets that only read rendering state. */
    UFUNCTION(BlueprintPure, Category="World|Rendering")
    UWorldData* GetWorldData() const { return Data.Get(); }

    /**
     * Re-applies settings.json fog/cloud toggles to an already running world.
     * Public because GameManagerSubSystem and Blueprint settings UIs may refresh
     * the active environment immediately after settings are applied.
     */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void RefreshRuntimeSettings();

    /** Default angular radius of the sun disk in radians (editable in Details / Blueprint). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Celestial", meta=(ClampMin="0.001", ClampMax="0.35", UIMin="0.005", UIMax="0.2"))
    float SunAngularRadius = 0.042f;

    /** Default angular radius of the moon disk in radians (editable in Details / Blueprint). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Celestial", meta=(ClampMin="0.001", ClampMax="0.35", UIMin="0.005", UIMax="0.2"))
    float MoonAngularRadius = 0.048f;

    /** Multiplier applied to the effective sun angular radius for quick runtime/editor tuning. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Celestial", meta=(ClampMin="0.1", ClampMax="5.0", UIMin="0.25", UIMax="3.0"))
    float SunSizeScale = 1.0f;

    /** Multiplier applied to the effective moon angular radius for quick runtime/editor tuning. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Celestial", meta=(ClampMin="0.1", ClampMax="5.0", UIMin="0.25", UIMax="3.0"))
    float MoonSizeScale = 1.0f;

    /** Re-applies sun and moon angular size parameters to directional lights and sky materials. */
    UFUNCTION(BlueprintCallable, Category="World|Celestial")
    void ApplyCelestialSizes();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Runtime cloud material resolved on demand from the central Asset Registry. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UMaterialInterface> CloudMaterial;

    /**
     * Optional authored sky dome. Existing WorldEnvManager Blueprints can assign SM_Skybox here;
     * the procedural atmosphere remains behind it as the loading/error fallback.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UStaticMeshComponent> Skybox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<USkyLightComponent> SkyLight;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Sun;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Moon;

private:
    /** Re-enables the native core components even when a Blueprint default hid one of them. */
    void EnsureCoreSkyVisible();

    /** Starts one non-blocking batch for environment shader/sky assets from the AssetRegistry. */
    void QueueEnvironmentAssetLoad();
    void HandleEnvironmentAssetsReady();
    void RefreshShaderLibraryParameters();

    /** Reads current settings and creates optional fog/cloud components. */
    void ConfigureRenderingSettings();
    void ApplyCloudSettings();
    void DestroyCloudComponent();
    void DestroyFogComponent();
    void UpdateFogProfile(float DeltaSeconds);
    void ReleaseDynamicRenderingResources();

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void UpdateFromGameUpdate(float DeltaSeconds);

    /** Calculates and applies the current sun/moon rotations and physical water optical tints. */
    void UpdateSkyLighting();
    void EnsureSkyMaterialInstances();
    void UpdateSkyMaterialParameters(float SunElevationSin, float MoonElevationSin);
    void UpdatePhysicalUnderwaterColors(float SunElevationSin, bool bForceUpdate = false);
    void UpdateFoliageInteractors();
    void UpdateUnderwaterLightShafts();
    UPROPERTY(Transient)
    TObjectPtr<UUnderwaterShaftOcclusionComponent> UnderwaterShaftOcclusion;
    bool bShaftDefaultsCaptured = false;
    bool bUnderwaterScreenShaftsSuppressed = false;
    bool bAboveWaterSunOcclusion = false;
    bool bAboveWaterSunBloom = false;
    bool bAboveWaterMoonOcclusion = false;
    bool bAboveWaterMoonBloom = false;
    bool bLastShaftOceanActive = false;
    float LastShaftOceanHeight = 0.0f;
    TWeakObjectPtr<UMaterialInstanceDynamic> LastConfiguredShaftMaterial;
    float LastAppliedWaterSunElevation = -999.0f;
    float LastAppliedSunIntensity = -1.0f;
    float LastAppliedSunTemp = -1.0f;
    float LastAppliedMoonIntensity = -1.0f;
    float LastAppliedSkyLightIntensity = -1.0f;
    float LastMoonDiskVisibility = -1.0f;
    float LastSunDiskVisibility = -1.0f;
    FVector LastMoonDiskDirection = FVector::ZeroVector;
    FVector LastSunDiskDirection = FVector::ZeroVector;
    float LastStarIntensity = -1.0f;
    float LastAppliedSunRadius = -1.0f;
    float LastAppliedMoonRadius = -1.0f;
    float CurrentSunElevationSin = 0.5f;
    float CurrentMoonElevationSin = -1.0f;
    float LastFogSunElevationSin = -999.0f;
    float LastFogMoonElevationSin = -999.0f;

    float GetEffectiveSunAngularRadius() const;
    float GetEffectiveMoonAngularRadius() const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMaterialInstanceDynamic>> SkyMaterialInstances;

    UPROPERTY(Transient)
    TObjectPtr<UWorldData> Data;

    UPROPERTY(Transient)
    TObjectPtr<UVolumetricCloudComponent> Cloud;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> CloudMID;

    UPROPERTY(Transient)
    TObjectPtr<UExponentialHeightFogComponent> Fog;

    V3DFog::FTransition FogTransition;
    V3DFog::FProfile LastAppliedFogProfile;
    float FogProfilePollSeconds = 0.f;
    bool bFogProfileApplied = false;

    /** Strong refs for asynchronously resolved global shader assets. */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> OceanPostProcessMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialParameterCollection> ShaderLibraryMPC;

    TSharedPtr<FStreamableHandle> EnvironmentAssetLoadHandle;

    /** Non-owning reference: the game-instance subsystem outlives this world actor. */
    UPROPERTY(Transient)
    TWeakObjectPtr<UGameManagerSubSystem> SubSystem;

    int32 GameUpdateTickHandle = INDEX_NONE;
    bool bRenderingActive = false;
};
