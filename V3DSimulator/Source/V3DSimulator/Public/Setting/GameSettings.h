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
#include "InputCoreTypes.h"
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

    /** Camera input multiplier. 1.0 preserves the controller's configured look speed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Input", meta=(ClampMin="0.05", ClampMax="5.0", UIMin="0.05", UIMax="5.0"))
    float MouseSensitivity = 1.0f;

    /** Also validates values assigned by Blueprint or hand-edited settings.json files. */
    UFUNCTION(BlueprintPure, Category="Settings|Input")
    float GetClampedMouseSensitivity() const;

    /** Persistent keybinding overrides: logical action ID (e.g. "Jump", "Chat") -> FKey name. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Input")
    TMap<FName, FName> KeyMappings;

    /** Persists Project Editor dark mode preference across sessions. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Editor")
    bool bEditorDarkMode = false;

    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Graphics")
    bool bVSyncEnabled = false;

    /** 0 = Unlimited, otherwise target FPS cap (30, 60, 90, 120, 144, 165, 240). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Graphics")
    int32 FrameRateLimit = 0;

    UFUNCTION(BlueprintPure, Category="Settings|Input")
    FKey GetKeyMappingOverride(FName ActionId) const;

    UFUNCTION(BlueprintCallable, Category="Settings|Input")
    void SetKeyMappingOverride(FName ActionId, FKey NewKey);

    UFUNCTION(BlueprintCallable, Category="Settings|Input")
    void ResetKeyMappingsToDefaults();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomIntensity = 0.675f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomThreshold = -1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float AmbientOcclusionIntensity = 0.5f;

    /** -11 = Basic Auto Exposure. -10..20 = fixed manual EV100. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess", meta=(ClampMin="-11.0", ClampMax="20.0"))
    float Exposure = -11.0f;

    /** Minimum EV100 limit when Auto Exposure is enabled. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess", meta=(ClampMin="-10.0", ClampMax="10.0"))
    float AutoExposureMinEV = -1.5f;

    /** Maximum EV100 limit when Auto Exposure is enabled. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess", meta=(ClampMin="-10.0", ClampMax="20.0"))
    float AutoExposureMaxEV = 13.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering")
    // Requests hardware Lumen and RT shadows; unsupported renderers use raster shadows.
    bool bRayTracing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bHeightFog = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bCloud = true;


    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadowQuality = 2;

    /** Shadow coverage from the camera; bounded by MaxRenderDistanceMeters. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="256", ClampMax="8192", UIMin="256", UIMax="8192", Delta="256", Units="m"))
    int32 ShadowDistanceMeters = 2048;

    UFUNCTION(BlueprintPure, Category="Settings|Rendering")
    int32 GetClampedShadowDistanceMeters() const;

    UFUNCTION(BlueprintPure, Category="Settings|Rendering")
    float GetEffectiveShadowDistanceCentimeters() const;

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
