// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file GameSettings.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Setting/GameSettings.h"
#include "Setting/ShadowDistancePolicy.h"
#include "Character/MouseLookPolicy.h"
#include "Character/PlayerCharacterController.h"
#include "EngineUtils.h"
#include "System/SafeFileIO.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "System/V3DStreamingPolicy.h"
#include "System/SimulatorFileServices.h"
#include "System/SimulatorPaths.h"
#include "System/JsonMetadata.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Components/PostProcessComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Scene.h"
#include "HAL/IConsoleManager.h"
#include "RenderUtils.h"

#define SETTING_FILE_NAME TEXT("/settings.json")

TSharedRef<FJsonObject> UGameSettings::Serialization()
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(V3DSimulatorJsonMetadata::Version, V3DSimulatorJsonMetadata::SchemaVersion);
    Json->SetNumberField(TEXT("MouseSensitivity"), GetClampedMouseSensitivity());
    Json->SetNumberField(TEXT("BloomIntensity"), BloomIntensity);
    Json->SetNumberField(TEXT("BloomThreshold"), BloomThreshold);
    Json->SetNumberField(TEXT("AmbientOcclusionIntensity"), AmbientOcclusionIntensity);
    Json->SetNumberField(TEXT("Exposure"), Exposure);
    Json->SetNumberField(TEXT("AutoExposureMinEV"), AutoExposureMinEV);
    Json->SetNumberField(TEXT("AutoExposureMaxEV"), AutoExposureMaxEV);
    Json->SetNumberField(TEXT("ShadowQuality"), ShadowQuality);
    Json->SetNumberField(TEXT("ShadowDistanceMeters"), GetClampedShadowDistanceMeters());
    Json->SetNumberField(TEXT("TextureQuality"), TextureQuality);
    Json->SetNumberField(TEXT("MaxTextureResolution"), GetClampedMaxTextureResolution());
    Json->SetNumberField(TEXT("ViewDistanceQuality"), ViewDistanceQuality);
    Json->SetNumberField(TEXT("MaxRenderDistanceMeters"), GetClampedMaxRenderDistanceMeters());
    Json->SetNumberField(TEXT("AntiAliasingQuality"), AntiAliasingQuality);
    Json->SetNumberField(TEXT("PostProcessingQuality"), PostProcessingQuality);
    Json->SetNumberField(TEXT("EffectsQuality"), EffectsQuality);
    Json->SetNumberField(TEXT("FoliageQuality"), FoliageQuality);
    Json->SetNumberField(TEXT("ShadingQuality"), ShadingQuality);
    Json->SetNumberField(TEXT("GlobalIlluminationQuality"), GlobalIlluminationQuality);
    Json->SetNumberField(TEXT("ReflectionQuality"), ReflectionQuality);
    Json->SetNumberField(TEXT("DynamicGlobalIlluminationMethod"), DynamicGlobalIlluminationMethod);
    Json->SetNumberField(TEXT("ReflectionMethod"), ReflectionMethod);
    Json->SetBoolField(TEXT("bRayTracing"), bRayTracing);
    Json->SetBoolField(TEXT("bHeightFog"), bHeightFog);
    Json->SetBoolField(TEXT("bCloud"), bCloud);
    Json->SetBoolField(TEXT("bEditorDarkMode"), bEditorDarkMode);
    Json->SetBoolField(TEXT("bVSyncEnabled"), bVSyncEnabled);
    Json->SetNumberField(TEXT("FrameRateLimit"), FrameRateLimit);
    if (!KeyMappings.IsEmpty())
    {
        TSharedRef<FJsonObject> KeyMapJson = MakeShared<FJsonObject>();
        TArray<FName> ActionIds;
        KeyMappings.GetKeys(ActionIds);
        ActionIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
        for (const FName& ActionId : ActionIds)
        {
            const FName KeyName = KeyMappings.FindChecked(ActionId);
            if (!ActionId.IsNone() && !KeyName.IsNone() && FKey(KeyName).IsValid())
            {
                KeyMapJson->SetStringField(ActionId.ToString(), KeyName.ToString());
            }
        }
        Json->SetObjectField(TEXT("KeyMappings"), KeyMapJson);
    }
    return Json;
}

bool UGameSettings::Deserialization(TSharedPtr<FJsonObject> Json)
{
    if (Json.IsValid())
    {
        // Older settings files have no input section. Retain the current/default value.
        Json->TryGetNumberField(TEXT("MouseSensitivity"), MouseSensitivity);
        MouseSensitivity = GetClampedMouseSensitivity();
        Json->TryGetNumberField(TEXT("BloomIntensity"), BloomIntensity);
        Json->TryGetNumberField(TEXT("BloomThreshold"), BloomThreshold);
        Json->TryGetNumberField(TEXT("AmbientOcclusionIntensity"), AmbientOcclusionIntensity);
        Json->TryGetNumberField(TEXT("Exposure"), Exposure);
        Exposure = Exposure <= -10.5f ? -11.0f : FMath::Clamp(Exposure, -10.0f, 20.0f);
        Json->TryGetNumberField(TEXT("AutoExposureMinEV"), AutoExposureMinEV);
        Json->TryGetNumberField(TEXT("AutoExposureMaxEV"), AutoExposureMaxEV);
        AutoExposureMinEV = FMath::IsFinite(AutoExposureMinEV) ? FMath::Clamp(AutoExposureMinEV, -10.0f, 10.0f) : -1.5f;
        AutoExposureMaxEV = FMath::IsFinite(AutoExposureMaxEV) ? FMath::Clamp(AutoExposureMaxEV, AutoExposureMinEV, 20.0f) : FMath::Max(AutoExposureMinEV, 13.0f);
        Json->TryGetNumberField(TEXT("ShadowQuality"), ShadowQuality);
        Json->TryGetNumberField(TEXT("ShadowDistanceMeters"), ShadowDistanceMeters);
        ShadowDistanceMeters = GetClampedShadowDistanceMeters();
        Json->TryGetNumberField(TEXT("TextureQuality"), TextureQuality);
        Json->TryGetNumberField(TEXT("MaxTextureResolution"), MaxTextureResolution);
        MaxTextureResolution = GetClampedMaxTextureResolution();
        Json->TryGetNumberField(TEXT("MaxRenderDistanceMeters"), MaxRenderDistanceMeters);
        MaxRenderDistanceMeters = GetClampedMaxRenderDistanceMeters();
        Json->TryGetNumberField(TEXT("ViewDistanceQuality"), ViewDistanceQuality);
        ViewDistanceQuality = FMath::Clamp(ViewDistanceQuality, 0, 3);
        Json->TryGetNumberField(TEXT("AntiAliasingQuality"), AntiAliasingQuality);
        Json->TryGetNumberField(TEXT("PostProcessingQuality"), PostProcessingQuality);
        Json->TryGetNumberField(TEXT("EffectsQuality"), EffectsQuality);
        Json->TryGetNumberField(TEXT("FoliageQuality"), FoliageQuality);
        Json->TryGetNumberField(TEXT("ShadingQuality"), ShadingQuality);
        Json->TryGetNumberField(TEXT("GlobalIlluminationQuality"), GlobalIlluminationQuality);
        Json->TryGetNumberField(TEXT("ReflectionQuality"), ReflectionQuality);
        Json->TryGetNumberField(TEXT("DynamicGlobalIlluminationMethod"), DynamicGlobalIlluminationMethod);
        Json->TryGetNumberField(TEXT("ReflectionMethod"), ReflectionMethod);
        Json->TryGetBoolField(TEXT("bRayTracing"), bRayTracing);
        Json->TryGetBoolField(TEXT("bHeightFog"), bHeightFog);
        Json->TryGetBoolField(TEXT("bCloud"), bCloud);
        Json->TryGetBoolField(TEXT("bEditorDarkMode"), bEditorDarkMode);
        Json->TryGetBoolField(TEXT("bVSyncEnabled"), bVSyncEnabled);
        int32 LoadedFps = FrameRateLimit;
        if (Json->TryGetNumberField(TEXT("FrameRateLimit"), LoadedFps))
        {
            FrameRateLimit = FMath::Clamp(LoadedFps, 0, 1000);
        }
        KeyMappings.Reset();
        const TSharedPtr<FJsonObject>* KeyMapJson = nullptr;
        if (Json->TryGetObjectField(TEXT("KeyMappings"), KeyMapJson) && KeyMapJson && KeyMapJson->IsValid())
        {
            for (const auto& Pair : (*KeyMapJson)->Values)
            {
                FString KeyStr;
                if (Pair.Value.IsValid() && Pair.Value->TryGetString(KeyStr))
                {
                    const FName ActionId(*FString(Pair.Key));
                    const FKey ParsedKey(*KeyStr);
                    if (!ActionId.IsNone() && ParsedKey.IsValid())
                    {
                        KeyMappings.Add(ActionId, ParsedKey.GetFName());
                    }
                }
            }
        }
        ShadowQuality = FMath::Clamp(ShadowQuality, 0, 3);
        TextureQuality = FMath::Clamp(TextureQuality, 0, 3);
        ViewDistanceQuality = FMath::Clamp(ViewDistanceQuality, 0, 3);
        AntiAliasingQuality = FMath::Clamp(AntiAliasingQuality, 0, 3);
        PostProcessingQuality = FMath::Clamp(PostProcessingQuality, 0, 3);
        EffectsQuality = FMath::Clamp(EffectsQuality, 0, 3);
        FoliageQuality = FMath::Clamp(FoliageQuality, 0, 3);
        ShadingQuality = FMath::Clamp(ShadingQuality, 0, 3);
        GlobalIlluminationQuality = FMath::Clamp(GlobalIlluminationQuality, 0, 3);
        ReflectionQuality = FMath::Clamp(ReflectionQuality, 0, 3);
        BloomIntensity = FMath::IsFinite(BloomIntensity) ? FMath::Clamp(BloomIntensity, 0.0f, 8.0f) : 1.0f;
        BloomThreshold = FMath::IsFinite(BloomThreshold) ? FMath::Clamp(BloomThreshold, -1.0f, 20.0f) : -1.0f;
        AmbientOcclusionIntensity = FMath::IsFinite(AmbientOcclusionIntensity) ? FMath::Clamp(AmbientOcclusionIntensity, 0.0f, 1.0f) : 1.0f;
        Exposure = FMath::IsFinite(Exposure) ? Exposure : 0.0f;
        DynamicGlobalIlluminationMethod = FMath::Clamp(DynamicGlobalIlluminationMethod, 0, 2);
        ReflectionMethod = FMath::Clamp(ReflectionMethod, 0, 2);
        return true;
    }
    return false;
}


float UGameSettings::GetClampedMouseSensitivity() const
{
    return V3DMouseLook::NormalizeSensitivity(MouseSensitivity);
}

FKey UGameSettings::GetKeyMappingOverride(const FName ActionId) const
{
    if (ActionId.IsNone()) return EKeys::Invalid;
    if (const FName* Found = KeyMappings.Find(ActionId))
    {
        const FKey Key(*Found);
        return Key.IsValid() ? Key : EKeys::Invalid;
    }
    return EKeys::Invalid;
}

void UGameSettings::SetKeyMappingOverride(const FName ActionId, const FKey NewKey)
{
    if (ActionId.IsNone()) return;
    if (!NewKey.IsValid())
    {
        KeyMappings.Remove(ActionId);
        return;
    }
    KeyMappings.Add(ActionId, NewKey.GetFName());
}

void UGameSettings::ResetKeyMappingsToDefaults()
{
    KeyMappings.Reset();
}

int32 UGameSettings::GetClampedMaxTextureResolution() const
{
    // Runtime texture decode cost grows quadratically with resolution. Keep a
    // tighter ceiling on mobile devices to prevent OOM crashes.
#if PLATFORM_ANDROID || PLATFORM_IOS
    return FMath::Clamp(MaxTextureResolution, 64, 1024);
#else
    return FMath::Clamp(MaxTextureResolution, 64, 8192);
#endif
}

namespace
{
    constexpr int32 ProfileTextureLimits[] = {256, 512, 768, 1024};
    constexpr int32 ProfileRenderDistances[] = {1024, 2048, 4096, 8192};
}

void UGameSettings::ApplyQualityProfile(const EQualitySettings Profile)
{
    const int32 Quality = FMath::Clamp(static_cast<int32>(Profile), 0, 3);
    ShadowQuality = TextureQuality = ViewDistanceQuality = AntiAliasingQuality = Quality;
    PostProcessingQuality = EffectsQuality = FoliageQuality = ShadingQuality = Quality;
    GlobalIlluminationQuality = ReflectionQuality = Quality;
    MaxTextureResolution = ProfileTextureLimits[Quality];
    MaxRenderDistanceMeters = ProfileRenderDistances[Quality];
    ShadowDistanceMeters = V3DShadowDistance::Profile(Quality);
    bRayTracing = Quality >= 2;
    bCloud = Quality >= 1;
    bHeightFog = true;
    DynamicGlobalIlluminationMethod = Quality >= 2 ? 1 : 0;
    ReflectionMethod = Quality >= 2 ? 1 : (Quality == 1 ? 2 : 0);
}

int32 UGameSettings::GetQualityProfileIndex() const
{
    const int32 Q = FMath::Clamp(ViewDistanceQuality, 0, 3);
    const bool bMatches = ViewDistanceQuality == Q && ShadowQuality == Q && TextureQuality == Q && AntiAliasingQuality == Q
        && PostProcessingQuality == Q && EffectsQuality == Q && FoliageQuality == Q
        && ShadingQuality == Q && GlobalIlluminationQuality == Q && ReflectionQuality == Q
        && GetClampedMaxTextureResolution() == ProfileTextureLimits[Q]
        && GetClampedMaxRenderDistanceMeters() == ProfileRenderDistances[Q]
        && GetClampedShadowDistanceMeters() == V3DShadowDistance::Profile(Q)
        && bRayTracing == (Q >= 2) && bCloud == (Q >= 1) && bHeightFog
        && DynamicGlobalIlluminationMethod == (Q >= 2 ? 1 : 0)
        && ReflectionMethod == (Q >= 2 ? 1 : (Q == 1 ? 2 : 0));
    return bMatches ? Q : INDEX_NONE;
}

int32 UGameSettings::GetClampedShadowDistanceMeters() const
{
    return V3DShadowDistance::Normalize(ShadowDistanceMeters);
}

float UGameSettings::GetEffectiveShadowDistanceCentimeters() const
{
    return V3DShadowDistance::EffectiveCentimetres(
        ShadowDistanceMeters, GetClampedMaxRenderDistanceMeters());
}

int32 UGameSettings::GetClampedMaxRenderDistanceMeters() const
{
    return V3DStreamingPolicy::NormalizeMaxRenderDistanceMeters(MaxRenderDistanceMeters);
}

float UGameSettings::GetMaxRenderDistanceCentimeters() const
{
    return static_cast<float>(GetClampedMaxRenderDistanceMeters()) * 100.0f;
}

float UGameSettings::GetViewDistanceScale() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 0.50f;
    case 1: return 0.75f;
    case 3: return 1.50f;
    case 2:
    default: return 1.00f;
    }
}

float UGameSettings::GetEffectiveStreamingDistanceMultiplier() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 12.0f;
    case 1: return 16.0f;
    case 3: return 28.0f;
    case 2:
    default: return 20.0f;
    }
}

float UGameSettings::GetEffectiveObjectStreamingRadiusMeters() const
{
    const float RenderMeters = static_cast<float>(GetClampedMaxRenderDistanceMeters());
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return FMath::Max(256.0f, RenderMeters);
    case 1: return FMath::Max(384.0f, RenderMeters);
    case 3: return FMath::Max(768.0f, RenderMeters);
    case 2:
    default: return FMath::Max(512.0f, RenderMeters);
    }
}

float UGameSettings::GetStreamingUnloadDistanceMultiplier() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 1.10f;
    case 1: return 1.10f;
    case 3: return 1.08f;
    case 2:
    default: return 1.08f;
    }
}

int32 UGameSettings::GetStreamingSceneSpawnBudget() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 2;
    case 1: return 4;
    case 3: return 8;
    case 2:
    default: return 6;
    }
}

int32 UGameSettings::GetStreamingNodeBudgetPerFrame() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 48;
    case 1: return 72;
    case 3: return 144;
    case 2:
    default: return 96;
    }
}

float UGameSettings::GetStreamingUpdateIntervalSeconds() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 0.12f;
    case 1: return 0.10f;
    case 3: return 0.06f;
    case 2:
    default: return 0.08f;
    }
}

float UGameSettings::GetStreamingFrameTimeBudgetMs() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 0.75f;
    case 1: return 1.00f;
    case 3: return 2.00f;
    case 2:
    default: return 1.50f;
    }
}

int32 UGameSettings::GetStreamingMeshGroupConcurrency() const
{
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 4;
    case 1: return 6;
    case 3: return 12;
    case 2:
    default: return 8;
    }
}

int32 UGameSettings::ResolveMaxTextureResolution(const UObject* WorldContextObject)
{
    if (GEngine && WorldContextObject)
    {
        if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
        {
            if (UGameInstance* GameInstance = World->GetGameInstance())
            {
                if (const UGameManagerSubSystem* Manager = GameInstance->GetSubsystem<UGameManagerSubSystem>())
                {
                    if (const UGameSettings* Settings = Manager->GetGameSettings())
                    {
                        return Settings->GetClampedMaxTextureResolution();
                    }
                }
            }
        }
    }

    const FString Path = V3DSimulatorPaths::SettingsPath();
    if (const TSharedPtr<FJsonObject> Json = FSimulatorFileServices::LoadJson(Path); Json.IsValid())
    {
        int32 SavedResolution = GetDefaultMaxTextureResolution();
        if (Json->TryGetNumberField(TEXT("MaxTextureResolution"), SavedResolution))
        {
            return FMath::Clamp(SavedResolution, 64, 8192);
        }
    }

    return GetDefaultMaxTextureResolution();
}

UGameSettings *UGameSettings::CreateSettingsData(UObject *Onwer)
{
    TObjectPtr<UGameSettings> Data = NewObject<UGameSettings>(Onwer);
    if (Data)
    {
        Data->LoadSettingsData();
    }
    return Data;
}

void UGameSettings::LoadSettingsData()
{
    FString Path = V3DSimulatorPaths::SettingsPath();
    TSharedPtr<FJsonObject> Json = FSimulatorFileServices::LoadJson(Path);
    if (!Deserialization(Json))
    {
#if PLATFORM_ANDROID || PLATFORM_IOS
        ApplyQualityProfile(EQualitySettings::Low);
        BloomIntensity = 0; AmbientOcclusionIntensity = 0;
#endif
        UE_LOG(LogTemp, Log, TEXT("Setting file doesn't exist. Generate new one."));
        SaveSettingsData();
    }
    else
    {
        LastRequestedSaveSnapshot = MakeSaveSnapshot(Serialization());
    }
}

FString UGameSettings::MakeSaveSnapshot(const TSharedRef<FJsonObject>& Json)
{
    FString Snapshot;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Snapshot);
    FJsonSerializer::Serialize(Json, Writer);
    return Snapshot;
}

void UGameSettings::SaveSettingsData()
{
    check(IsInGameThread());
    const TSharedRef<FJsonObject> Json = Serialization();
    const FString Snapshot = MakeSaveSnapshot(Json);
    if (Snapshot == LastRequestedSaveSnapshot) return;
    LastRequestedSaveSnapshot = Snapshot;
    const uint64 Revision = ++SaveRequestRevision;
    const TWeakObjectPtr<UGameSettings> WeakThis(this);
    FSafeFileIO::SaveJsonAsync(Json, V3DSimulatorPaths::SettingsPath(),
        [WeakThis, Revision](FSafeFileWriteResult Result)
        {
            if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::Superseded)
            {
                if (UGameSettings* Self = WeakThis.Get(); Self && Self->SaveRequestRevision == Revision)
                    Self->LastRequestedSaveSnapshot.Reset(); // A failed write can be retried.
                UE_LOG(LogTemp, Warning, TEXT("Settings save failed: %s"), *Result.Error);
            }
        });
}

void UGameSettings::UpdateSettings(UPostProcessComponent *PostProcess)
{
    MouseSensitivity = GetClampedMouseSensitivity();
    ShadowDistanceMeters = GetClampedShadowDistanceMeters();
    MaxRenderDistanceMeters = GetClampedMaxRenderDistanceMeters();
    if (!IsValid(GEngine))
        return;

    UGameUserSettings *Settings = GEngine->GetGameUserSettings();
    if (Settings)
    {
        // 0-3 (or 0-4): Low through Epic (or Cinematic).
        Settings->SetShadowQuality(ShadowQuality);
        Settings->SetTextureQuality(TextureQuality);
        Settings->SetViewDistanceQuality(ViewDistanceQuality);
        Settings->SetAntiAliasingQuality(AntiAliasingQuality);
        Settings->SetPostProcessingQuality(PostProcessingQuality);
        Settings->SetFoliageQuality(FoliageQuality);
        Settings->SetShadingQuality(ShadingQuality);
        Settings->SetGlobalIlluminationQuality(GlobalIlluminationQuality);
        Settings->SetReflectionQuality(ReflectionQuality);
        Settings->SetVisualEffectQuality(EffectsQuality);
        // Use this path if resolution or window mode should be changed together.
        // Settings->SetScreenResolution(FIntPoint(1920, 1080));
        // Settings->SetFullscreenMode(EWindowMode::WindowedFullscreen);

        Settings->SetVSyncEnabled(bVSyncEnabled);
        const float SafeFpsLimit = FrameRateLimit > 0 ? static_cast<float>(FMath::Clamp(FrameRateLimit, 15, 1000)) : 0.0f;
        Settings->SetFrameRateLimit(SafeFpsLimit);
        Settings->ApplySettings(false);
        Settings->SaveSettings();
    }
    if (GEngine)
    {
        GEngine->SetMaxFPS(FrameRateLimit > 0 ? static_cast<float>(FMath::Clamp(FrameRateLimit, 15, 1000)) : 0.0f);
    }

    // r.RayTracing is a startup/cook capability, not a runtime user toggle. Gate the
    // supported effects instead, keeping raster shadows available when RT is unavailable.
    if (!IsRunningCommandlet())
    {
#if PLATFORM_ANDROID || PLATFORM_IOS
        const bool bUseHardwareRayTracing = false;
#else
        const bool bUseHardwareRayTracing = bRayTracing && IsRayTracingEnabled();
#endif
        const auto SetRenderingOption = [](const TCHAR* Name, const int32 Value)
        {
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
            {
                // GameSetting has lower priority than RendererSettings. GameOverride is the
                // intended UE 5.8 user override and still respects console/command-line locks.
                if ((Variable->GetFlags() & ECVF_SetByMask) <= ECVF_SetByGameOverride
                    && Variable->GetInt() != Value)
                {
                    Variable->Set(Value, ECVF_SetByGameOverride);
                }
            }
        };
        const auto SetRenderingFloatOption = [](const TCHAR* Name, const float Value)
        {
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
            {
                if ((Variable->GetFlags() & ECVF_SetByMask) <= ECVF_SetByGameOverride
                    && !FMath::IsNearlyEqual(Variable->GetFloat(), Value, 1.e-4f))
                {
                    Variable->Set(Value, ECVF_SetByGameOverride);
                }
            }
        };
        const bool bLowQualityShading = (ShadingQuality == 0 || ShadowQuality == 0);
        // Always keep SingleLayerWater active so water stays transparent and reflective at every quality preset.
        SetRenderingOption(TEXT("r.Water.SingleLayer"), 1);
        SetRenderingOption(TEXT("r.Water.SingleLayer.Reflection"), 1);
        SetRenderingOption(TEXT("r.Water.SingleLayer.LumenReflections"), bLowQualityShading ? 0 : 1);
        SetRenderingOption(TEXT("r.Lumen.HardwareRayTracing"),
            bUseHardwareRayTracing && !bLowQualityShading ? 1 : 0);
        SetRenderingOption(TEXT("r.Lumen.Reflections.HardwareRayTracing"),
            bUseHardwareRayTracing && !bLowQualityShading ? 1 : 0);
        SetRenderingOption(TEXT("r.Lumen.Reflections.SingleLayerWater"), bLowQualityShading ? 0 : 1);
        // Keep SingleLayerWater reflections fast without paying for full-scene FrontLayer translucent RT pass.
        SetRenderingOption(TEXT("r.Lumen.TranslucencyReflections.FrontLayer.Allow"), 0);
        SetRenderingOption(TEXT("r.Lumen.TranslucencyReflections.FrontLayer.EnableForProject"), 0);
        SetRenderingOption(TEXT("r.RayTracing.Shadows"),
            bUseHardwareRayTracing && ShadowQuality > 0 ? 1 : 0);
        // Classic clean low-spec 3D game shading on Low quality: disable noisy SSAO, contact shadows,
        // Lumen GI/reflections, and SSR so characters and terrain have clean smooth diffuse lighting.
        SetRenderingOption(TEXT("r.AmbientOcclusionLevels"), bLowQualityShading ? 0 : 2);
        SetRenderingOption(TEXT("r.ContactShadows"), bLowQualityShading ? 0 : 1);
        SetRenderingOption(TEXT("r.Lumen.DiffuseIndirect.Allow"), bLowQualityShading ? 0 : 1);
        SetRenderingOption(TEXT("r.Lumen.Reflections.Allow"), bLowQualityShading ? 0 : 1);
        SetRenderingOption(TEXT("r.SSR.Quality"), bLowQualityShading ? 0 : 2);
        SetRenderingOption(TEXT("r.BloomQuality"), bLowQualityShading ? 1 : 4);

        // Apply after scalability, which otherwise silently scales the user's distance down.
        const float ShadowRange = GetEffectiveShadowDistanceCentimeters();
        SetRenderingOption(TEXT("r.Shadow.DistanceScale"), 1);
        int32 LastLevel = FMath::Min(V3DShadowDistance::LastClipmapLevel(ShadowRange), 20);
        if (const IConsoleVariable* FirstLevel = IConsoleManager::Get().FindConsoleVariable(
            TEXT("r.Shadow.Virtual.Clipmap.FirstLevel")))
            LastLevel = FMath::Max(LastLevel, FirstLevel->GetInt());
        SetRenderingOption(TEXT("r.Shadow.Virtual.Clipmap.LastLevel"), LastLevel);
        // Prevent [VSM] Non-Nanite Marking Job Queue overflow & VSM GPU stutters on dense non-Nanite HISM worlds.
        SetRenderingOption(TEXT("r.Shadow.Virtual.NonNanite.MarkJobQueueSize"), 2097152);
        SetRenderingOption(TEXT("r.Shadow.Virtual.NonNanite.IncludeInCoarsePages"), 0);
        SetRenderingOption(TEXT("r.Shadow.Virtual.MaxPhysicalPages"), 4096);
        SetRenderingOption(TEXT("r.Shadow.Virtual.Cache.StaticSeparate"), 1);
        SetRenderingOption(TEXT("r.Shadow.Virtual.AllowScreenOverflowMessages"), 0);
        // Frame-pacing & anti-stutter budgets (keep PSO compile and async streaming smooth).
        SetRenderingOption(TEXT("r.ShaderPipelineCache.Enabled"), 1);
        SetRenderingFloatOption(TEXT("r.ShaderPipelineCache.BatchTime"), 2.0f);
        SetRenderingFloatOption(TEXT("s.AsyncLoadingTimeLimit"), 4.0f);
        SetRenderingFloatOption(TEXT("s.LevelStreamingActorsUpdateTimeLimit"), 3.0f);

        if (bUseHardwareRayTracing)
        {
            // Keep Near-Field RT TLAS compact (150m-400m, 1.0 deg angle) to eliminate per-frame TLAS build stutters,
            // and use Lumen HWRT FarField + AlphaMasked so distant impostors render in RT reflections/GI cheaply.
            const float NearRayTracingRange = FMath::Clamp(ShadowRange, 15000.0f, 40000.0f);
            SetRenderingOption(TEXT("r.RayTracing.Culling"), 1);
            SetRenderingOption(TEXT("r.RayTracing.Culling.Radius"), FMath::RoundToInt(NearRayTracingRange));
            SetRenderingFloatOption(TEXT("r.RayTracing.Culling.Angle"), 1.0f);
            SetRenderingOption(TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.CullClusterRadius"),
                FMath::RoundToInt(NearRayTracingRange));
            const int32 EnableFarField = (ShadingQuality >= 3) ? 1 : 0;
            SetRenderingOption(TEXT("r.Lumen.HardwareRayTracing.FarField"), EnableFarField);
            SetRenderingOption(TEXT("r.Lumen.Reflections.HardwareRayTracing.FarField"), EnableFarField);
            SetRenderingOption(TEXT("r.Lumen.HardwareRayTracing.AlphaMasked"), 1);
            SetRenderingOption(TEXT("r.RayTracing.Shadows.EnableTwoSidedGeometry"), 1);
            SetRenderingOption(TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.EvaluateWPO"), 0);
            SetRenderingOption(TEXT("r.RayTracing.Geometry.InstancedStaticMeshes.SimulationCount"), 0);
            SetRenderingOption(TEXT("r.Lumen.HardwareRayTracing.HitLighting"), 0);
            SetRenderingFloatOption(TEXT("r.Lumen.Reflections.MaxRoughnessToTrace"), 0.35f);
        }
    }

    if (IsValid(PostProcess))
    {
        FPostProcessSettings PPSettings = PostProcess->Settings;
        PPSettings.BloomIntensity = BloomIntensity;
        PPSettings.BloomThreshold = BloomThreshold;
        const bool bLowQualityPP = (ShadingQuality == 0 || ShadowQuality == 0);
        PPSettings.AmbientOcclusionIntensity = bLowQualityPP ? 0.0f : AmbientOcclusionIntensity;
        PPSettings.bOverride_ReflectionMethod = true;
        PPSettings.ReflectionMethod = bLowQualityPP ? EReflectionMethod::None : EReflectionMethod::Lumen;

        // Exposure=-11 is the explicit Auto sentinel. Auto uses the cheaper Basic metering mode
        // (single downsampled luminance value) with an EV100 adaptation window wide enough for
        // interiors/daylight. Manual values lock Min/Max to the requested EV100 without changing
        // the rest of the post-process stack.
        const bool bAutoExposure = Exposure <= -10.5f;
        const float ManualExposureEV = FMath::Clamp(Exposure, -10.0f, 20.0f);
        PPSettings.bOverride_AutoExposureMethod = true;
        PPSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Basic;
        PPSettings.bOverride_AutoExposureMinBrightness = true;
        PPSettings.bOverride_AutoExposureMaxBrightness = true;
        PPSettings.bOverride_AutoExposureSpeedUp = true;
        PPSettings.bOverride_AutoExposureSpeedDown = true;
        // Histogram-only controls are intentionally not overridden in Basic metering mode.
        PPSettings.bOverride_HistogramLogMin = false;
        PPSettings.bOverride_HistogramLogMax = false;
        PPSettings.bOverride_AutoExposureLowPercent = false;
        PPSettings.bOverride_AutoExposureHighPercent = false;
        PPSettings.bOverride_AutoExposureBias = true;
        PPSettings.AutoExposureBias = 0.0f;
        PPSettings.bOverride_AutoExposureBiasCurve = true;
        PPSettings.AutoExposureBiasCurve = nullptr;

        if (bAutoExposure)
        {
            const float SafeMinEV = FMath::IsFinite(AutoExposureMinEV) ? FMath::Clamp(AutoExposureMinEV, -10.0f, 10.0f) : -1.5f;
            const float SafeMaxEV = FMath::IsFinite(AutoExposureMaxEV) ? FMath::Clamp(AutoExposureMaxEV, SafeMinEV, 20.0f) : FMath::Max(SafeMinEV, 13.0f);
            PPSettings.AutoExposureMinBrightness = SafeMinEV;
            PPSettings.AutoExposureMaxBrightness = SafeMaxEV;
            // F-stops/second: faster bright-scene recovery, slightly gentler dark adaptation.
            PPSettings.AutoExposureSpeedUp = 6.0f;
            PPSettings.AutoExposureSpeedDown = 4.0f;
        }
        else
        {
            PPSettings.AutoExposureMinBrightness = ManualExposureEV;
            PPSettings.AutoExposureMaxBrightness = ManualExposureEV;
            PPSettings.AutoExposureSpeedUp = 6.0f;
            PPSettings.AutoExposureSpeedDown = 4.0f;
        }

        PPSettings.bOverride_DynamicGlobalIlluminationMethod = true;
        PPSettings.bOverride_ReflectionMethod = true;
        PPSettings.DynamicGlobalIlluminationMethod = GetDynamicGlobalIlluminationMethod(DynamicGlobalIlluminationMethod);
        PPSettings.ReflectionMethod = GetReflectionMethod(ReflectionMethod);
        PostProcess->Settings = PPSettings;
    }

    if (GEngine)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!IsValid(World) || !World->IsGameWorld()) continue;
            for (TActorIterator<APlayerCharacterController> It(World); It; ++It)
            {
                if (APlayerCharacterController* PC = *It; IsValid(PC) && PC->IsLocalController())
                {
                    PC->ApplyConfiguredInputMappingContexts();
                }
            }
        }
    }
}

EDynamicGlobalIlluminationMethod::Type UGameSettings::GetDynamicGlobalIlluminationMethod(const int &Value)
{
    switch(Value)
    {
        case 0:
            return EDynamicGlobalIlluminationMethod::Type::None;
        case 1:
            return EDynamicGlobalIlluminationMethod::Type::Lumen;
        case 2:
            return EDynamicGlobalIlluminationMethod::Type::ScreenSpace;
        case 3:
            return EDynamicGlobalIlluminationMethod::Type::Plugin;
        default:
            return EDynamicGlobalIlluminationMethod::Type::None;
    }
}

EReflectionMethod::Type UGameSettings::GetReflectionMethod(const int &Value)
{
    switch (Value)
    {
        case 0:
            return EReflectionMethod::Type::None;
        case 1:
            return EReflectionMethod::Type::Lumen;
        case 2:
            return EReflectionMethod::Type::ScreenSpace;
        default:
            return EReflectionMethod::Type::None;
    }
}
