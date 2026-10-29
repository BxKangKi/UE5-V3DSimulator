// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file GameSettings.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interface/JsonData.h"
#include "Engine/EngineTypes.h"
#include "GameSettings.generated.h"

class UPostProcessComponent;
class UActorComponent;

UENUM(BlueprintType)
enum class EQualitySettings : uint8 {
    Low UMETA(DisplayName = "Low"),
    Medium UMETA(DisplayName = "Medium"),
    High UMETA(DisplayName = "High"),
    Epic UMETA(DisplayName = "Epic")
};

UCLASS(BlueprintType)
class V3DSIMULATOR_API UGameSettings : public UObject, public IJsonData
{
    GENERATED_BODY()

public:
    virtual TSharedRef<FJsonObject> Serialization() override;
    virtual bool Deserialization(TSharedPtr<FJsonObject> Json) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomIntensity = 0.675f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomThreshold = -1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float AmbientOcclusionIntensity = 0.5f;

    /** -11 = Basic Auto Exposure. -10..20 = fixed manual EV100. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess", meta=(ClampMin="-11.0", ClampMax="20.0"))
    float Exposure = -11.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering")
    // Requests hardware Lumen and RT shadows; unsupported renderers use raster shadows.
    bool bRayTracing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bHeightFog = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bCloud = true;

    /** 0.0 = disabled, 1.0 = enabled. Written directly to ShaderLibraryMPC.CelShadingMode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CelShadingMode = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadowQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 TextureQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="64", ClampMax="8192"))
    int32 MaxTextureResolution = 768;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ViewDistanceQuality = 2;

    /** Absolute camera range in metres, independent of quality; snapped to 1024 m steps. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="1024", ClampMax="32768", UIMin="1024", UIMax="32768", Delta="1024", Units="m"))
    int32 MaxRenderDistanceMeters = 8192;

    UFUNCTION(BlueprintPure, Category="Settings|Rendering")
    int32 GetClampedMaxRenderDistanceMeters() const;

    /** Unreal world units (centimetres), also written to ShaderLibraryMPC.MaxRenderDistance. */
    UFUNCTION(BlueprintPure, Category="Settings|Rendering")
    float GetMaxRenderDistanceCentimeters() const;


    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 AntiAliasingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 PostProcessingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 EffectsQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 FoliageQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 GlobalIlluminationQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ReflectionQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0", ClampMax="3"))
    int32 DynamicGlobalIlluminationMethod = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0", ClampMax="2"))
    int32 ReflectionMethod = 1;

    /** Sets all quality fields and the texture/distance budget. UpdateSettings applies and saves them. */
    UFUNCTION(BlueprintCallable, Category="Settings|Quality")
    void ApplyQualityProfile(EQualitySettings Profile);

    /** Returns 0..3 for a matching preset, or INDEX_NONE for customized settings. */
    UFUNCTION(BlueprintPure, Category="Settings|Quality")
    int32 GetQualityProfileIndex() const;

    static UGameSettings *CreateSettingsData(UObject *Onwer = nullptr);
    static int32 GetDefaultMaxTextureResolution() { return 768; }
    int32 GetClampedMaxTextureResolution() const;

    /** Quality scale shared by engine view distance and custom archive streaming. */
    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetViewDistanceScale() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetEffectiveStreamingDistanceMultiplier() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetEffectiveObjectStreamingRadiusMeters() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetStreamingUnloadDistanceMultiplier() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    int32 GetStreamingSceneSpawnBudget() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    int32 GetStreamingNodeBudgetPerFrame() const;

    /** Internal automatic profile values derived only from ViewDistanceQuality. */
    float GetStreamingUpdateIntervalSeconds() const;
    float GetStreamingFrameTimeBudgetMs() const;
    int32 GetStreamingMeshGroupConcurrency() const;

    static int32 ResolveMaxTextureResolution(const UObject* WorldContextObject);
    UFUNCTION()
    void LoadSettingsData();
    UFUNCTION()
    void SaveSettingsData();
    UFUNCTION()
    void UpdateSettings(UPostProcessComponent *PostProcess);

private:
    FString LastRequestedSaveSnapshot;
    uint64 SaveRequestRevision = 0;
    static FString MakeSaveSnapshot(const TSharedRef<FJsonObject>& Json);
    EDynamicGlobalIlluminationMethod::Type GetDynamicGlobalIlluminationMethod(const int &Value);
    EReflectionMethod::Type GetReflectionMethod(const int &Value);
};