// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldEnvManager.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/WorldEnvManager.h"
#include "World/UnderwaterRenderPolicy.h"
#include "World/UnderwaterShaftOcclusionComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/DynamicActor.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/GameUpdateSubSystem.h"
#include "System/MathHelper.h"
#include "TimerManager.h"
#include "World/WorldData.h"

namespace WorldEnvManagerPrivate
{
    void ConfigureLightShadows(UDirectionalLightComponent* Light)
    {
        if (!IsValid(Light)) return;
        Light->SetCastShadows(true);
        if (!Light->CastDynamicShadows)
        {
            Light->CastDynamicShadows = true;
            Light->MarkRenderStateDirty();
        }
        Light->SetCastRaytracedShadows(ECastRayTracedShadow::UseProjectSetting);
    }

    struct FSkyLightRotations
    {
        FRotator Sun = FRotator::ZeroRotator;
        FRotator Moon = FRotator::ZeroRotator;
    };

    /**
     * Calculates a guarded solar direction directly from immutable values already read on the
     * game thread. Returns the DirectionalLight propagation rotation (Forward = -ToSun),
     * so positive solar altitude yields a downward-shining light (negative Pitch) and
     * SkyAtmosphere / underwater shafts (-Sun->GetForwardVector()) align with the actual sun.
     */
    FRotator CalculateSunRotation(const UWorldData* WorldData)
    {
        if (!IsValid(WorldData)
            || !FMath::IsFinite(WorldData->WorldTime)
            || !FMath::IsFinite(WorldData->OneDayTime)
            || !FMath::IsFinite(WorldData->OneYearDays)
            || !FMath::IsFinite(WorldData->AxialTilt)
            || !FMath::IsFinite(WorldData->Latitude)
            || !FMath::IsFinite(WorldData->Longitude)
            || WorldData->OneDayTime <= UE_SMALL_NUMBER
            || WorldData->OneYearDays <= UE_SMALL_NUMBER)
        {
            return FRotator(-35.0f, -30.0f, 0.0f);
        }

        float TotalSeconds = FMath::Fmod(
            WorldData->WorldTime, WorldData->OneDayTime);
        if (TotalSeconds < 0.0f)
        {
            TotalSeconds += WorldData->OneDayTime;
        }

        double DayOfYear = FMath::Fmod(
            static_cast<double>(WorldData->WorldTime)
                / static_cast<double>(WorldData->OneDayTime),
            static_cast<double>(WorldData->OneYearDays));
        if (DayOfYear < 0.0)
        {
            DayOfYear += WorldData->OneYearDays;
        }
        const float AxialTiltRadians = Deg2Rad * WorldData->AxialTilt;
        const float LatitudeRadians = Deg2Rad * WorldData->Latitude;
        const float SunDeclination = AxialTiltRadians * FMath::Sin(
            2.0 * __PI__ * (DayOfYear / WorldData->OneYearDays));
        const float TimeInHours =
            (TotalSeconds / WorldData->OneDayTime) * 24.0f;
        // WorldTime is the world's local solar clock. Longitude rotates the compass orientation
        // below; multiplying longitude by 15 here previously pushed the default sun below the
        // horizon even at noon and made a healthy procedural atmosphere look unrendered.
        const float HourAngle = 15.0f * (TimeInHours - 12.0f);
        const float HourAngleRadians = Deg2Rad * HourAngle;

        const float Altitude = FMath::Asin(FMath::Clamp(
            FMath::Sin(LatitudeRadians) * FMath::Sin(SunDeclination)
                + FMath::Cos(LatitudeRadians) * FMath::Cos(SunDeclination)
                    * FMath::Cos(HourAngleRadians),
            -1.0f,
            1.0f));
        const float Azimuth = FMath::Atan2(
            -FMath::Cos(SunDeclination) * FMath::Sin(HourAngleRadians),
            FMath::Cos(LatitudeRadians) * FMath::Sin(SunDeclination)
                - FMath::Sin(LatitudeRadians) * FMath::Cos(SunDeclination)
                    * FMath::Cos(HourAngleRadians));
        const float OrientedAzimuth = Azimuth + Deg2Rad * WorldData->Longitude;

        // Vector from observer TOWARD the sun in the sky (+Z when Altitude > 0).
        const FVector ToSunDirection(
            FMath::Cos(Altitude) * FMath::Cos(OrientedAzimuth),
            FMath::Cos(Altitude) * FMath::Sin(OrientedAzimuth),
            FMath::Sin(Altitude));
        if (ToSunDirection.IsNearlyZero())
        {
            return FRotator(-35.0f, -30.0f, 0.0f);
        }

        // Unreal DirectionalLight Forward (+X) is the direction light rays TRAVEL (from sun to ground).
        const FVector LightPropagationDir = (-ToSunDirection).GetSafeNormal();
        FRotator Rotation =
            FRotationMatrix::MakeFromX(LightPropagationDir).Rotator();
        Rotation.Normalize();
        return Rotation;
    }

    FSkyLightRotations CalculateLightRotations(const UWorldData* WorldData)
    {
        FSkyLightRotations Result;
        Result.Sun = CalculateSunRotation(WorldData);
        Result.Moon = FRotator(
            -Result.Sun.Pitch,
            Result.Sun.Yaw + 180.0f,
            -Result.Sun.Roll);
        Result.Moon.Normalize();
        return Result;
    }
}

AWorldEnvManager::AWorldEnvManager()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);

    Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
    Sun->SetupAttachment(RootComponent);
    Sun->SetMobility(EComponentMobility::Movable);
    WorldEnvManagerPrivate::ConfigureLightShadows(Sun);
    // SkyAtmosphere ignores an ordinary directional light when generating the sun disk and
    // aerial perspective. Explicitly reserve atmosphere-light slot zero for the sun.
    Sun->SetAtmosphereSunLight(true);
    Sun->SetAtmosphereSunLightIndex(0);
    Sun->bUseTemperature = true;
    Sun->SetEnableLightShaftOcclusion(true);
    Sun->SetEnableLightShaftBloom(true);
    Sun->SetBloomScale(0.0001f);
    Sun->SetIntensity(20.0f);
    Sun->SetLightSourceAngle(0.53f);
    Sun->SetTemperature(5700.0f);
    Sun->SetVolumetricScatteringIntensity(2.5f);
    Sun->bCastShadowsOnClouds = true;
    Sun->bCastShadowsOnAtmosphere = true;
    Sun->SetRelativeRotation(FRotator(-35.0f, -30.0f, 0.0f));

    Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
    Moon->SetupAttachment(RootComponent);
    Moon->SetMobility(EComponentMobility::Movable);
    WorldEnvManagerPrivate::ConfigureLightShadows(Moon);
    // Slot one lets SkyAtmosphere render a separate moon disk without replacing the sun.
    Moon->SetAtmosphereSunLight(true);
    Moon->SetAtmosphereSunLightIndex(1);
    Moon->SetIntensity(0.005f);
    Moon->SetUseTemperature(true);
    Moon->SetTemperature(4100.0f);
    Moon->SetEnableLightShaftOcclusion(true);
    Moon->SetEnableLightShaftBloom(true);
    Moon->SetBloomScale(0.0001f);
    Moon->bCastShadowsOnClouds = true;
    Moon->bCastShadowsOnAtmosphere = true;
    Moon->SetForwardShadingPriority(1);
    Moon->SetRelativeRotation(FRotator(35.0f, 150.0f, 0.0f));

    UnderwaterShaftOcclusion = CreateDefaultSubobject<UUnderwaterShaftOcclusionComponent>(TEXT("UnderwaterShaftOcclusion"));

    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    PostProcess->SetupAttachment(RootComponent);
    PostProcess->bUnbound = true;

    SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
    SkyAtmosphere->SetupAttachment(RootComponent);
    SkyAtmosphere->SetMobility(EComponentMobility::Movable);
    SkyAtmosphere->SetGroundAlbedo(FColor::White);
    SkyAtmosphere->SetAtmosphereHeight(200.0f);
    SkyAtmosphere->SetMultiScatteringFactor(1.0f);
    SkyAtmosphere->SetRayleighScattering(FLinearColor(0.0058f, 0.0180f, 0.0331f));
    SkyAtmosphere->SetRayleighScatteringScale(0.35f);
    SkyAtmosphere->SetRayleighExponentialDistribution (8.0f);
    SkyAtmosphere->SetMieScatteringScale(0.012f);
    SkyAtmosphere->SetMieAbsorption(FLinearColor(0.00044f, 0.00044f, 0.00044f));
    SkyAtmosphere->SetMieAbsorptionScale(0.0004f);
    SkyAtmosphere->SetMieAnisotropy(0.85f);
    SkyAtmosphere->SetSkyLuminanceFactor(FLinearColor(1.0f, 1.0f, 1.0f));

    // Keep the original named component so existing Blueprint defaults retain their SM_Skybox
    // assignment after recompilation. No hard-coded content path is used by native code.
    Skybox = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Skybox"));
    Skybox->SetupAttachment(RootComponent);
    Skybox->SetWorldScale3D(FVector(8192.0f, 8192.0f, 8192.0f));
    Skybox->SetEnableGravity(false);
    Skybox->SetCastShadow(false);
    Skybox->SetVisibleInRayTracing(false);

    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
    SkyLight->SetupAttachment(RootComponent);
    SkyLight->SetMobility(EComponentMobility::Movable);
    SkyLight->SetRealTimeCapture(true);
    SkyLight->SetIntensity(0.5f);
    SkyLight->SetCastDeepShadow(true);
    SkyLight->bTransmission = true;
    SkyLight->bAffectTranslucentLighting = true;
}

void AWorldEnvManager::BeginPlay()
{
    Super::BeginPlay();
    // Reapply after Blueprint defaults/construction scripts have been loaded.
    WorldEnvManagerPrivate::ConfigureLightShadows(Sun);
    WorldEnvManagerPrivate::ConfigureLightShadows(Moon);
    if (IsValid(Skybox))
    {
        Skybox->SetCastShadow(false);
        Skybox->SetVisibleInRayTracing(false);
    }

    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    if (UGameManagerSubSystem* GameManager = SubSystem.Get())
    {
        // This is the one project-wide post-process component. GameSettings and the unified
        // cel/ocean material both target this component rather than maintaining parallel PP actors.
        GameManager->SetPostProcess(PostProcess);
    }

    PrepareForWorldLoading();

    // Start package I/O before settings/world initialization so shader/sky asset loading overlaps
    // the remaining startup work instead of sitting behind it on the critical path.
    QueueEnvironmentAssetLoad();
    ConfigureRenderingSettings();

    // Placed WorldEnvManager actors can begin rendering once GameManagerSubSystem has already loaded world data.
    if (UGameManagerSubSystem* GameManager = SubSystem.Get(); IsValid(GameManager) && IsValid(GameManager->GetWorldData()))
    {
        InitializeRendering(GameManager->GetWorldData());
    }
}

void AWorldEnvManager::PrepareForWorldLoading()
{
    if (!ensureMsgf(IsInGameThread(),
            TEXT("AWorldEnvManager::PrepareForWorldLoading must run on the game thread")))
    {
        return;
    }

    // The procedural atmosphere is the loading-state sky as well as the normal runtime sky. It
    // must not wait for config.json or the dynamic .dat state to become available.
    SetActorHiddenInGame(false);
    EnsureCoreSkyVisible();
}

void AWorldEnvManager::EnsureCoreSkyVisible()
{
    const auto EnableSceneComponent = [](USceneComponent* Component)
    {
        if (!IsValid(Component))
        {
            return;
        }

        // Blueprint subclasses may have serialized a disabled/hidden component. Restore the
        // native rendering contract every time this actor is adopted for a new world session.
        Component->SetActive(true, true);
        Component->SetVisibility(true, true);
        Component->SetHiddenInGame(false, true);
        Component->MarkRenderStateDirty();
    };

    EnableSceneComponent(SkyAtmosphere);
    EnableSceneComponent(Skybox);
    EnableSceneComponent(Sun);
    EnableSceneComponent(Moon);
    EnableSceneComponent(SkyLight);
    EnableSceneComponent(PostProcess);

    if (IsValid(Sun))
    {
        Sun->SetAtmosphereSunLight(true);
        Sun->SetAtmosphereSunLightIndex(0);
        if (!bRenderingActive)
        {
            Sun->SetIntensity(20.0f);
            Sun->SetRelativeRotation(FRotator(-35.0f, -30.0f, 0.0f));
        }
    }
    if (IsValid(Moon))
    {
        Moon->SetAtmosphereSunLight(true);
        Moon->SetAtmosphereSunLightIndex(1);
        if (!bRenderingActive)
        {
            Moon->SetIntensity(0.005f);
            Moon->SetRelativeRotation(FRotator(35.0f, 150.0f, 0.0f));
        }
    }
    if (IsValid(SkyLight))
    {
        // A movable real-time skylight captures both the procedural fallback atmosphere and the
        // optional authored SM_Skybox dome without requiring a separate cooked cube map.
        SkyLight->SetRealTimeCapture(true);
        if (!bRenderingActive)
        {
            SkyLight->SetIntensity(0.5f);
        }
    }
}

void AWorldEnvManager::QueueEnvironmentAssetLoad()
{
    check(IsInGameThread());

    UV3DSimulatorAssetRegistry* Registry =
        UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        return;
    }

    TArray<FSoftObjectPath> AssetsToLoad;
    AssetsToLoad.Reserve(4);
    const auto AddSoftPath = [&AssetsToLoad](const FSoftObjectPath& Path)
    {
        if (Path.IsValid())
        {
            AssetsToLoad.AddUnique(Path);
        }
    };

    AddSoftPath(Registry->CloudMaterial.ToSoftObjectPath());
    AddSoftPath(Registry->SkyboxMesh.ToSoftObjectPath());
    AddSoftPath(Registry->OceanPostProcessMaterial.ToSoftObjectPath());
    AddSoftPath(Registry->ShaderLibraryMPC.ToSoftObjectPath());

    if (AssetsToLoad.IsEmpty())
    {
        HandleEnvironmentAssetsReady();
        return;
    }

    EnvironmentAssetLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
        AssetsToLoad,
        FStreamableDelegate::CreateUObject(this, &AWorldEnvManager::HandleEnvironmentAssetsReady));

    if (!EnvironmentAssetLoadHandle.IsValid())
    {
        // RequestAsyncLoad can fail during teardown. Do not fall back to blocking loads here:
        // the procedural atmosphere remains a valid dependency-free fallback.
        UE_LOG(LogTemp, Warning,
            TEXT("WorldEnvManager could not queue environment shader assets for async loading."));
    }
}

void AWorldEnvManager::HandleEnvironmentAssetsReady()
{
    check(IsInGameThread());

    UV3DSimulatorAssetRegistry* Registry =
        UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        EnvironmentAssetLoadHandle.Reset();
        return;
    }

    if (!Registry->CloudMaterial.IsNull())
    {
        CloudMaterial = Registry->CloudMaterial.Get();
    }

    if (IsValid(Skybox) && !Registry->SkyboxMesh.IsNull())
    {
        if (UStaticMesh* LoadedSkybox = Registry->SkyboxMesh.Get())
        {
            Skybox->SetStaticMesh(LoadedSkybox);
        }
    }

    if (IsValid(Registry->OceanPostProcessMaterial.Get()))
    {
        OceanPostProcessMaterial = UMaterialInstanceDynamic::Create(Registry->OceanPostProcessMaterial.Get(), this);
        LastAppliedWaterSunElevation = -999.0f;
        if (IsValid(PostProcess) && IsValid(OceanPostProcessMaterial))
        {
            // The underwater-only material has no cel shading or stencil dependency.
            PostProcess->Settings.WeightedBlendables.Array.Reset();
            PostProcess->AddOrUpdateBlendable(OceanPostProcessMaterial, 1.0f);
        }
    }

    if (!Registry->ShaderLibraryMPC.IsNull())
    {
        ShaderLibraryMPC = Registry->ShaderLibraryMPC.Get();
    }

    // Components/UProperties above now own strong references to every asset we need. Releasing the
    // streamable handle here cannot make the just-loaded shader assets disappear at the next GC.
    EnvironmentAssetLoadHandle.Reset();

    ApplyCloudSettings();
    RefreshShaderLibraryParameters();
    if (IsValid(Sun))
    {
        const float SunElevationSin = FMath::Clamp(static_cast<float>(-Sun->GetForwardVector().Z), -1.0f, 1.0f);
        UpdatePhysicalUnderwaterColors(SunElevationSin, true);
    }
}

void AWorldEnvManager::RefreshShaderLibraryParameters()
{
    check(IsInGameThread());

    UpdateUnderwaterLightShafts();
    const bool bOceanEnabled = IsValid(Data) && Data->bOcean && bRenderingActive;
    const float OceanHeight = bOceanEnabled && FMath::IsFinite(Data->OceanHeightCm)
        ? static_cast<float>(Data->OceanHeightCm) : 0.0f;
    if (IsValid(OceanPostProcessMaterial) && IsValid(PostProcess))
    {
        OceanPostProcessMaterial->SetScalarParameterValue(TEXT("WaterLevel"), OceanHeight);
        PostProcess->AddOrUpdateBlendable(OceanPostProcessMaterial, bOceanEnabled ? 1.0f : 0.0f);
    }

    if (!IsValid(ShaderLibraryMPC))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UMaterialParameterCollectionInstance* Parameters =
        World->GetParameterCollectionInstance(ShaderLibraryMPC);
    if (!IsValid(Parameters))
    {
        return;
    }

    float MaxDistanceCm = GetDefault<UGameSettings>()->GetMaxRenderDistanceCentimeters();
    if (UGameManagerSubSystem* GameManager = SubSystem.Get())
    {
        if (const UGameSettings* Settings = GameManager->GetGameSettings())
        {
            MaxDistanceCm = Settings->GetMaxRenderDistanceCentimeters();
        }
    }

    Parameters->SetScalarParameterValue(TEXT("OceanHeight"), OceanHeight);
    Parameters->SetScalarParameterValue(TEXT("OceanEnabled"), bOceanEnabled ? 1.0f : 0.0f);
    Parameters->SetScalarParameterValue(TEXT("MaxRenderDistance"), MaxDistanceCm);
}

void AWorldEnvManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (EnvironmentAssetLoadHandle.IsValid())
    {
        EnvironmentAssetLoadHandle->CancelHandle();
        EnvironmentAssetLoadHandle.Reset();
    }
    StopRendering();
    if (IsValid(PostProcess))
    {
        PostProcess->Settings.WeightedBlendables.Array.Reset();
    }
    OceanPostProcessMaterial = nullptr;
    CloudMaterial = nullptr;
    ShaderLibraryMPC = nullptr;
    LastConfiguredShaftMaterial.Reset();
    SubSystem.Reset();
    Super::EndPlay(EndPlayReason);
}

void AWorldEnvManager::InitializeRendering(UWorldData* InWorldData)
{
    if (!IsValid(InWorldData))
    {
        StopRendering();
        return;
    }

    Data = InWorldData;
    bRenderingActive = true;
    LastAppliedWaterSunElevation = -999.0f;
    PrepareForWorldLoading();
    ConfigureRenderingSettings();
    ApplyCloudSettings();
    RefreshShaderLibraryParameters();
    RegisterGameUpdate();
    UpdateSkyLighting();
    UpdateFoliageInteractors();
    UpdateUnderwaterLightShafts();
}

void AWorldEnvManager::StopRendering()
{
    bRenderingActive = false;
    UnregisterGameUpdate();
    ReleaseDynamicRenderingResources();
    Data = nullptr;
    // Clear world-dependent MPC state as soon as rendering stops so a persistent/placed
    // environment manager cannot leave underwater shading enabled between world loads.
    RefreshShaderLibraryParameters();
}

void AWorldEnvManager::ConfigureRenderingSettings()
{
    UGameManagerSubSystem* GameManager = SubSystem.Get();
    if (!IsValid(GameManager))
    {
        GameManager = UGameManagerSubSystem::GetSubSystem(this);
        SubSystem = GameManager;
    }

    if (!IsValid(GameManager) || !IsValid(PostProcess))
    {
        return;
    }

    RefreshRuntimeSettings();
    // Apply quality/post-process/streaming settings through the shared manager. The manager's
    // environment refresh calls back into RefreshRuntimeSettings(), which is intentionally a
    // leaf operation and therefore cannot recurse into UpdateSettings().
    GameManager->UpdateSettings();
}

void AWorldEnvManager::RefreshRuntimeSettings()
{
    check(IsInGameThread());
    UGameManagerSubSystem* GameManager = SubSystem.Get();
    if (!IsValid(GameManager))
    {
        GameManager = UGameManagerSubSystem::GetSubSystem(this);
        SubSystem = GameManager;
    }
    if (!IsValid(GameManager))
    {
        return;
    }

    UGameSettings* Setting = GameManager->GetGameSettings();
    if (!IsValid(Setting))
    {
        return;
    }

    // Apply to both lights on creation and every settings change; Blueprint defaults must
    // not restore the engine's short 200 m shadow range after the user has set a longer one.
    for (UDirectionalLightComponent* Light : {Sun.Get(), Moon.Get()})
    {
        if (!IsValid(Light)) continue;
        WorldEnvManagerPrivate::ConfigureLightShadows(Light);
        Light->SetCastShadows(Setting->ShadowQuality > 0);
        const float ShadowRange = Setting->GetEffectiveShadowDistanceCentimeters();
        Light->SetDynamicShadowDistanceMovableLight(ShadowRange);
        Light->SetDynamicShadowDistanceStationaryLight(ShadowRange);
#if PLATFORM_ANDROID || PLATFORM_IOS
        Light->SetDynamicShadowCascades(2);
#else
        Light->SetDynamicShadowCascades(Setting->ShadowQuality >= 2 ? 4 : 3);
#endif
        Light->SetCascadeDistributionExponent(3.0f);
        Light->SetCascadeTransitionFraction(0.15f);
        Light->SetShadowDistanceFadeoutFraction(0.15f);
    }

    if (Setting->bHeightFog)
    {
        if (!IsValid(Fog))
        {
            Fog = NewObject<UExponentialHeightFogComponent>(this, TEXT("RuntimeHeightFog"));
            if (IsValid(Fog))
            {
                AddInstanceComponent(Fog);
                Fog->SetVolumetricFog(true);
                Fog->SetVolumetricFogScatteringDistribution(0.25f);
                Fog->SetVolumetricFogExtinctionScale(1.2f);
                Fog->SetupAttachment(GetRootComponent());
                Fog->SetFogDensity(0.02f);
                Fog->SetFogHeightFalloff(0.2f);
                Fog->SetSecondFogDensity(0.0f);
                Fog->RegisterComponent();
            }
        }
    }
    else
    {
        DestroyFogComponent();
    }

    const bool bLevelAllowsCloud = IsValid(Data) && Data->Cloud.bEnabled;
    if (Setting->bCloud && bLevelAllowsCloud)
    {
        if (!IsValid(Cloud))
        {
            Cloud = NewObject<UVolumetricCloudComponent>(this, TEXT("RuntimeVolumetricCloud"));
            if (IsValid(Cloud))
            {
                AddInstanceComponent(Cloud);
                Cloud->SetupAttachment(GetRootComponent());
                Cloud->RegisterComponent();
            }
        }
    }
    else
    {
        DestroyCloudComponent();
    }

    ApplyCloudSettings();
    RefreshShaderLibraryParameters();
}

void AWorldEnvManager::ApplyCloudSettings()
{
    if (!IsValid(Data))
    {
        return;
    }

    UGameSettings* Setting = nullptr;
    if (UGameManagerSubSystem* GameManager = SubSystem.Get())
    {
        Setting = GameManager->GetGameSettings();
    }

    const bool bCloudEnabled = Data->Cloud.bEnabled && (!IsValid(Setting) || Setting->bCloud);
    if (!bCloudEnabled)
    {
        DestroyCloudComponent();
        return;
    }

    if (!IsValid(Cloud))
    {
        Cloud = NewObject<UVolumetricCloudComponent>(this, TEXT("RuntimeVolumetricCloud"));
        if (!IsValid(Cloud))
        {
            return;
        }

        AddInstanceComponent(Cloud);
        Cloud->SetupAttachment(GetRootComponent());
        Cloud->RegisterComponent();
    }

    if (!IsValid(CloudMID))
    {
        UMaterialInterface* SourceMaterial = CloudMaterial.Get();
        if (!IsValid(SourceMaterial))
        {
            SourceMaterial = Cloud->GetMaterial();
        }
        if (IsValid(SourceMaterial))
        {
            CloudMID = UMaterialInstanceDynamic::Create(SourceMaterial, this);
            if (IsValid(CloudMID))
            {
                Cloud->SetMaterial(CloudMID);
            }
        }
    }

    if (IsValid(CloudMID))
    {
        // The parameter names are intentionally generic so different cloud materials can opt in.
        CloudMID->SetScalarParameterValue(TEXT("Coverage"), Data->Cloud.Coverage);
        CloudMID->SetScalarParameterValue(TEXT("Density"), Data->Cloud.Density);
        CloudMID->SetScalarParameterValue(TEXT("Opacity"), Data->Cloud.Opacity);
        CloudMID->SetScalarParameterValue(TEXT("WindSpeed"), Data->Cloud.WindSpeed);
        CloudMID->SetVectorParameterValue(TEXT("Tint"), Data->Cloud.Tint);
    }
}

void AWorldEnvManager::DestroyCloudComponent()
{
    // Runtime-created components are stored in the actor's InstanceComponents array. Remove the
    // ownership entry before destruction so a stopped/placed manager cannot retain a dead component
    // or its material graph until the actor itself is collected.
    if (IsValid(Cloud))
    {
        Cloud->SetMaterial(nullptr);
        RemoveInstanceComponent(Cloud);
        Cloud->UnregisterComponent();
        Cloud->DestroyComponent();
    }

    Cloud = nullptr;
    CloudMID = nullptr;
}

void AWorldEnvManager::DestroyFogComponent()
{
    if (IsValid(Fog))
    {
        RemoveInstanceComponent(Fog);
        Fog->UnregisterComponent();
        Fog->DestroyComponent();
    }

    Fog = nullptr;
}

void AWorldEnvManager::ReleaseDynamicRenderingResources()
{
    DestroyCloudComponent();
    DestroyFogComponent();
}

void AWorldEnvManager::RegisterGameUpdate()
{
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<AWorldEnvManager> WeakThis(this);
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (AWorldEnvManager* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            35);
    }
}

void AWorldEnvManager::UnregisterGameUpdate()
{
    if (GameUpdateTickHandle == INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AWorldEnvManager::UpdateFromGameUpdate(float /*DeltaSeconds*/)
{
    // Cloud settings are applied when rendering/settings change, not redundantly every frame.
    UpdateSkyLighting();
    UpdateUnderwaterLightShafts();
}

void AWorldEnvManager::UpdateSkyLighting()
{
    if (!bRenderingActive || !IsValid(Data))
    {
        return;
    }

    const WorldEnvManagerPrivate::FSkyLightRotations Result =
        WorldEnvManagerPrivate::CalculateLightRotations(Data);

    float SunElevationSin = 0.5f;
    if (IsValid(Sun))
    {
        Sun->SetWorldRotation(Result.Sun);
        SunElevationSin = FMath::Clamp(static_cast<float>(-Sun->GetForwardVector().Z), -1.0f, 1.0f);
        // Smooth physical sunrise/sunset attenuation and color temperature shift.
        const float SunDayFactor = FMath::SmoothStep(-0.04f, 0.18f, SunElevationSin);
        Sun->SetIntensity(20.0f * SunDayFactor);
        const float TempAlpha = FMath::SmoothStep(0.0f, 0.45f, SunElevationSin);
        Sun->SetTemperature(FMath::Lerp(2600.0f, 5800.0f, TempAlpha));
    }

    if (IsValid(Moon))
    {
        Moon->SetWorldRotation(Result.Moon);
        const float MoonElevationSin = FMath::Clamp(static_cast<float>(-Moon->GetForwardVector().Z), -1.0f, 1.0f);
        const float NightFactor = 1.0f - FMath::SmoothStep(-0.04f, 0.12f, SunElevationSin);
        // Subtle realistic moonlight intensity so nights stay naturally dark.
        Moon->SetIntensity(0.008f * FMath::SmoothStep(0.0f, 0.20f, MoonElevationSin) * NightFactor);
    }

    UpdatePhysicalUnderwaterColors(SunElevationSin, false);
}

void AWorldEnvManager::UpdatePhysicalUnderwaterColors(const float SunElevationSin, const bool bForceUpdate)
{
    if (!IsValid(OceanPostProcessMaterial))
    {
        return;
    }
    if (!bForceUpdate && FMath::Abs(SunElevationSin - LastAppliedWaterSunElevation) < 0.005f)
    {
        return;
    }
    LastAppliedWaterSunElevation = SunElevationSin;

    // Physically-based clear ocean equilibrium colors derived from Jerlov Type I spectral
    // absorption/scattering modulated by solar elevation and air-water Fresnel transmission.
    const float Daylight = FMath::SmoothStep(-0.06f, 0.25f, SunElevationSin);
    const float CosAir = FMath::Clamp(SunElevationSin, 0.0f, 1.0f);
    const float OneMinusCos = 1.0f - CosAir;
    const float Fresnel = 0.0204f + (1.0f - 0.0204f) * FMath::Pow(OneMinusCos, 5.0f);
    const float SurfaceTrans = 1.0f - Fresnel;
    const float LightScale = 0.03f + 0.97f * Daylight * SurfaceTrans;

    // Darker deep-ocean ambient equilibrium matched to Beer-Lambert water surface optics.
    const FLinearColor PhysicalFogAmbient(
        0.0015f * LightScale,
        0.016f * LightScale,
        0.042f * LightScale,
        1.0f);
    // Single-scattering marine in-scatter color (dark ocean navy-teal, never bright sky blue).
    const FLinearColor PhysicalFogScatter(
        0.006f * LightScale,
        0.055f * LightScale,
        0.125f * LightScale,
        1.0f);
    // Waterline meniscus refraction band.
    const FLinearColor PhysicalBandColor(
        0.009f * LightScale,
        0.068f * LightScale,
        0.140f * LightScale,
        1.0f);
    // Neutral-warm shaft base tint (Beer-Lambert spectral extinction in Underwater.ush supplies depth color).
    const float WarmShift = FMath::SmoothStep(0.0f, 0.35f, SunElevationSin);
    const FLinearColor PhysicalShaftColor(
        FMath::Lerp(0.95f, 0.80f, WarmShift),
        FMath::Lerp(0.76f, 0.94f, WarmShift),
        FMath::Lerp(0.55f, 1.00f, WarmShift),
        1.0f);

    OceanPostProcessMaterial->SetVectorParameterValue(TEXT("Fog Ambient Color"), PhysicalFogAmbient);
    OceanPostProcessMaterial->SetVectorParameterValue(TEXT("Fog Scatter Color"), PhysicalFogScatter);
    OceanPostProcessMaterial->SetVectorParameterValue(TEXT("Band Color"), PhysicalBandColor);
    OceanPostProcessMaterial->SetVectorParameterValue(TEXT("ShaftColor"), PhysicalShaftColor);
    OceanPostProcessMaterial->SetScalarParameterValue(TEXT("Scattering Aniso"), 0.75f);
}

void AWorldEnvManager::UpdateFoliageInteractors()
{
    if (!IsValid(ShaderLibraryMPC))
    {
        return;
    }
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    UMaterialParameterCollectionInstance* Parameters = World->GetParameterCollectionInstance(ShaderLibraryMPC);
    if (!IsValid(Parameters))
    {
        return;
    }

    static const FName InteractorNames[4] = {
        TEXT("FoliageInteractor0"),
        TEXT("FoliageInteractor1"),
        TEXT("FoliageInteractor2"),
        TEXT("FoliageInteractor3")
    };

    FLinearColor Slots[4] = {
        FLinearColor(0.f, 0.f, 0.f, 0.f),
        FLinearColor(0.f, 0.f, 0.f, 0.f),
        FLinearColor(0.f, 0.f, 0.f, 0.f),
        FLinearColor(0.f, 0.f, 0.f, 0.f)
    };
    int32 SlotIndex = 0;

    FVector ReferenceLocation = FVector::ZeroVector;
    if (APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0); IsValid(PlayerPawn))
    {
        ReferenceLocation = PlayerPawn->GetActorLocation();
        Slots[SlotIndex++] = FLinearColor(ReferenceLocation.X, ReferenceLocation.Y, ReferenceLocation.Z, 115.0f);
    }

    // Populate up to 3 nearest moving dynamic actors around the player for foliage bending.
    struct FMovingCandidate
    {
        double DistSq;
        FLinearColor Packed;
    };
    TArray<FMovingCandidate> Candidates;
    constexpr double MaxTrackDistSq = 2500.0 * 2500.0;
    for (TActorIterator<ADynamicActor> It(World); It; ++It)
    {
        ADynamicActor* Dyn = *It;
        if (!IsValid(Dyn)) continue;
        const FVector Vel = Dyn->GetVelocity();
        if (Vel.SizeSquared() < 100.0) continue; // Only moving objects bend foliage dynamically.
        const FVector Loc = Dyn->GetActorLocation();
        const double DistSq = FVector::DistSquared(Loc, ReferenceLocation);
        if (DistSq > MaxTrackDistSq) continue;
        const FBox Bounds = Dyn->GetComponentsBoundingBox(true);
        const float Radius = FMath::Clamp(static_cast<float>(Bounds.GetExtent().Size2D()) + 45.0f, 65.0f, 320.0f);
        Candidates.Add({DistSq, FLinearColor(Loc.X, Loc.Y, Loc.Z, Radius)});
    }
    Candidates.Sort([](const FMovingCandidate& A, const FMovingCandidate& B) { return A.DistSq < B.DistSq; });
    for (const FMovingCandidate& C : Candidates)
    {
        if (SlotIndex >= 4) break;
        Slots[SlotIndex++] = C.Packed;
    }

    for (int32 i = 0; i < 4; ++i)
    {
        Parameters->SetVectorParameterValue(InteractorNames[i], Slots[i]);
    }
}

void AWorldEnvManager::UpdateUnderwaterLightShafts()
{
    check(IsInGameThread());
    if (!bShaftDefaultsCaptured)
    {
        // Capture after Blueprint defaults have been applied, before the first water transition.
        bAboveWaterSunOcclusion = IsValid(Sun) && Sun->bEnableLightShaftOcclusion;
        bAboveWaterSunBloom = IsValid(Sun) && Sun->bEnableLightShaftBloom;
        bAboveWaterMoonOcclusion = IsValid(Moon) && Moon->bEnableLightShaftOcclusion;
        bAboveWaterMoonBloom = IsValid(Moon) && Moon->bEnableLightShaftBloom;
        bShaftDefaultsCaptured = true;
    }
    const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
    const bool bOceanActive = bRenderingActive && IsValid(Data) && Data->bOcean && IsValid(Camera);
    const float OceanHeight = IsValid(Data) && FMath::IsFinite(Data->OceanHeightCm)
        ? static_cast<float>(Data->OceanHeightCm) : 0.0f;
    const bool bSuppress = V3DUnderwaterRender::SuppressScreenShafts(bOceanActive,
        IsValid(OceanPostProcessMaterial), IsValid(Camera) ? Camera->GetCameraLocation().Z : 0.0,
        OceanHeight, bUnderwaterScreenShaftsSuppressed);

    if (IsValid(UnderwaterShaftOcclusion))
    {
        const bool bShaftConfigChanged = (LastConfiguredShaftMaterial.Get() != OceanPostProcessMaterial.Get())
            || (bLastShaftOceanActive != bOceanActive)
            || !FMath::IsNearlyEqual(LastShaftOceanHeight, OceanHeight, 0.1f);
        if (bShaftConfigChanged)
        {
            UnderwaterShaftOcclusion->Configure(OceanPostProcessMaterial, Sun, bOceanActive, OceanHeight);
            LastConfiguredShaftMaterial = OceanPostProcessMaterial;
            bLastShaftOceanActive = bOceanActive;
            LastShaftOceanHeight = OceanHeight;
        }
    }
    if (bSuppress == bUnderwaterScreenShaftsSuppressed) return;
    bUnderwaterScreenShaftsSuppressed = bSuppress;
    // UE's radial SceneDepth occlusion cannot distinguish the water plane from a wall. It
    // would apply a second, shifted silhouette over the new water volume integration.
    if (IsValid(Sun))
    {
        Sun->SetEnableLightShaftOcclusion(!bSuppress && bAboveWaterSunOcclusion);
        Sun->SetEnableLightShaftBloom(!bSuppress && bAboveWaterSunBloom);
    }
    if (IsValid(Moon))
    {
        Moon->SetEnableLightShaftOcclusion(!bSuppress && bAboveWaterMoonOcclusion);
        Moon->SetEnableLightShaftBloom(!bSuppress && bAboveWaterMoonBloom);
    }
}
