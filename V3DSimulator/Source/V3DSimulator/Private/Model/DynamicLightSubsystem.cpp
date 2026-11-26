// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicLightSubsystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/DynamicLightSubsystem.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/GameUpdateSubSystem.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"


namespace
{
    void DestroyManagedDecal(UDecalComponent* Decal)
    {
        if (!IsValid(Decal))
        {
            return;
        }

        // The decal is dynamically added to the light owner's InstanceComponents array. Clear the
        // material first, remove the ownership entry, and only then destroy the component.
        Decal->SetDecalMaterial(nullptr);
        if (AActor* Owner = Decal->GetOwner(); IsValid(Owner))
        {
            Owner->RemoveInstanceComponent(Decal);
        }
        Decal->UnregisterComponent();
        Decal->DestroyComponent();
    }

    FLinearColor ComputeEffectiveDecalColor(const UDynamicPointLightComponent* LightComp)
    {
        if (!IsValid(LightComp))
        {
            return FLinearColor(1.0f, 1.0f, 1.0f, 0.0f);
        }

        FLinearColor Color = LightComp->GetLightColor();
        if (LightComp->bUseTemperature)
        {
            Color *= FLinearColor::MakeFromColorTemperature(LightComp->Temperature);
        }

        // Normalize intensity across light units so 1000 Lumens / ~80 Candelas / ~10 Unitless
        // produces a visible fallback decal instead of vanishing near 0.01 opacity.
        float NormalizedIntensity = 0.0f;
        const float RawIntensity = FMath::Max(0.0f, LightComp->Intensity);
        if (RawIntensity > KINDA_SMALL_NUMBER)
        {
            switch (LightComp->IntensityUnits)
            {
            case ELightUnits::Lumens:
                NormalizedIntensity = RawIntensity / 2500.0f;
                break;
            case ELightUnits::Candelas:
                NormalizedIntensity = RawIntensity / 200.0f;
                break;
            case ELightUnits::EV:
                NormalizedIntensity = FMath::Pow(2.0f, FMath::Clamp(RawIntensity, -4.0f, 12.0f)) / 64.0f;
                break;
            case ELightUnits::Unitless:
            default:
                NormalizedIntensity = RawIntensity / 16.0f;
                break;
            }
            Color.A = FMath::Clamp(NormalizedIntensity * LightComp->GetMaxLightDecalOpacity(),
                FMath::Min(0.06f, LightComp->GetMaxLightDecalOpacity()),
                LightComp->GetMaxLightDecalOpacity());
        }
        else
        {
            Color.A = 0.0f;
        }
        return Color;
    }

    UMaterialInterface* ResolveDecalMaterialForLight(
        const UObject* WorldContext,
        UDynamicPointLightComponent* InLight)
    {
        if (IsValid(InLight) && IsValid(InLight->GetLightDecal()))
        {
            return InLight->GetLightDecal();
        }
        if (UV3DSimulatorAssetRegistry* Registry =
            UV3DSimulatorGameInstance::GetAssetRegistryFromContext(WorldContext))
        {
            if (!Registry->StaticDecalLightMaterial.IsNull())
            {
                return Registry->StaticDecalLightMaterial.LoadSynchronous();
            }
        }
        return nullptr;
    }
}

void UDynamicLightSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
}

void UDynamicLightSubsystem::Deinitialize()
{
    UnregisterGameUpdate();
    for (FLightOptimizationData& Data : ManagedLights)
    {
        DestroyManagedDecal(Data.DecalComponent.Get());
        Data.DecalComponent.Reset();
        Data.TargetDecalMaterial.Reset();
    }
    ManagedLights.Empty();
    Super::Deinitialize();
}

void UDynamicLightSubsystem::RegisterLight(UDynamicPointLightComponent *InLight)
{
    if (!IsValid(InLight))
    {
        return;
    }

    CompactManagedLights();

    UMaterialInterface* ResolvedMaterial = ResolveDecalMaterialForLight(this, InLight);
    const float CullingDist = FMath::Max(0.0f, InLight->GetCullingDistance());
    const float DecalDist = FMath::Max(0.0f, InLight->GetDecalTransitionDistance());
    // DecalDist == 0 means keep the lightweight fallback decal visible until the owning actor/impostor unloads.
    const bool bInfiniteDecalRange = (DecalDist <= KINDA_SMALL_NUMBER);
    const bool bCanUseDecalFallback = InLight->IsLightDecalFallbackEnabled()
        && IsValid(ResolvedMaterial)
        && (bInfiniteDecalRange || DecalDist > CullingDist);
    const float EffectiveDecalDistSq = bCanUseDecalFallback
        ? (bInfiniteDecalRange ? 0.0f : FMath::Square(DecalDist))
        : -1.0f;

    // Refresh existing entry if re-registered after SetLightDecal / distance updates.
    for (FLightOptimizationData& Existing : ManagedLights)
    {
        if (Existing.LightComponent.Get() == InLight)
        {
            Existing.Position = InLight->GetComponentLocation();
            Existing.CullingDistanceSq = FMath::Square(CullingDist);
            Existing.DecalTransitionDistanceSq = EffectiveDecalDistSq;
            Existing.TargetDecalMaterial = bCanUseDecalFallback ? ResolvedMaterial : nullptr;
            if (!bCanUseDecalFallback)
            {
                DestroyManagedDecal(Existing.DecalComponent.Get());
                Existing.DecalComponent.Reset();
                Existing.bCurrentDecalVisibility = false;
                Existing.bTargetDecalVisibility = false;
            }
            return;
        }
    }

    FLightOptimizationData NewData;
    NewData.Position = InLight->GetComponentLocation();
    NewData.CullingDistanceSq = FMath::Square(CullingDist);
    NewData.DecalTransitionDistanceSq = EffectiveDecalDistSq;
    NewData.LightComponent = InLight;
    NewData.TargetDecalMaterial = bCanUseDecalFallback ? ResolvedMaterial : nullptr;

    // Apply the initial state.
    NewData.bCurrentLightVisibility = InLight->IsVisible();

    ManagedLights.Add(NewData);
    RegisterGameUpdate();
}

void UDynamicLightSubsystem::UnregisterLight(UDynamicPointLightComponent *InLight)
{
    for (int32 i = ManagedLights.Num() - 1; i >= 0; --i)
    {
        if (ManagedLights[i].LightComponent.Get() == InLight)
        {
            // Destroy dynamically spawned decal components, if any.
            DestroyManagedDecal(ManagedLights[i].DecalComponent.Get());
            ManagedLights[i].DecalComponent.Reset();
            ManagedLights[i].TargetDecalMaterial.Reset();
            ManagedLights.RemoveAtSwap(i, 1, EAllowShrinking::No);
            break;
        }
    }

    CompactManagedLights();
}

void UDynamicLightSubsystem::RegisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<UDynamicLightSubsystem> WeakThis(this);
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (UDynamicLightSubsystem* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateLightsFromGameUpdate(DeltaSeconds);
                }
            },
            40);
    }
}

void UDynamicLightSubsystem::UnregisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateHandle == INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;
}

void UDynamicLightSubsystem::CompactManagedLights()
{
    check(IsInGameThread());
    int32 RemovedCount = 0;
    for (int32 Index = ManagedLights.Num() - 1; Index >= 0; --Index)
    {
        if (ManagedLights[Index].LightComponent.IsValid())
        {
            continue;
        }

        DestroyManagedDecal(ManagedLights[Index].DecalComponent.Get());
        ManagedLights[Index].DecalComponent.Reset();
        ManagedLights[Index].TargetDecalMaterial.Reset();
        ManagedLights.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        ++RemovedCount;
    }

    if (ManagedLights.Num() == 0)
    {
        ManagedLights.Empty();
        UnregisterGameUpdate();
    }
    else if (RemovedCount > 0 && ManagedLights.Max() > FMath::Max(32, ManagedLights.Num() * 2))
    {
        ManagedLights.Shrink();
    }
}

void UDynamicLightSubsystem::UpdateLightsFromGameUpdate(float DeltaTime)
{
    // Visibility decisions do not need the physics/input update rate.
    CullingAccumulator += FMath::Max(0.f, DeltaTime);
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr float Interval = .2f;
#else
    constexpr float Interval = .1f;
#endif
    if (CullingAccumulator < Interval) return;
    CullingAccumulator = FMath::Fmod(CullingAccumulator, Interval);
    CompactManagedLights();
    if (ManagedLights.Num() == 0)
    {
        return;
    }

    // Read the camera location once for the whole batch.
    APlayerController *PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!IsValid(PC))
    {
        return;
    }

    FVector CameraLocation;
    FRotator CameraRotation;
    PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

    // Each light needs only one squared-distance comparison. A sequential contiguous pass is
    // cheaper than dispatching worker tasks and keeps every weak UObject access thread-safe.
    for (FLightOptimizationData &Data : ManagedLights)
    {
        UDynamicPointLightComponent *LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
        {
            continue;
        }

        // Dynamic components may move, so refresh their position before calculating visibility.
        Data.Position = LightComp->GetComponentLocation();
        const float DistanceSquared = FVector::DistSquared(CameraLocation, Data.Position);
        const bool bUseLight = DistanceSquared < Data.CullingDistanceSq;
        const bool bWithinDecalRange = (Data.DecalTransitionDistanceSq == 0.0f)
            || (Data.DecalTransitionDistanceSq > Data.CullingDistanceSq && DistanceSquared < Data.DecalTransitionDistanceSq);
        const bool bUseDecal = !bUseLight &&
            Data.TargetDecalMaterial.IsValid() &&
            Data.DecalTransitionDistanceSq >= 0.0f &&
            bWithinDecalRange;
        Data.bTargetLightVisibility = bUseLight;
        Data.bTargetDecalVisibility = bUseDecal;

        // Apply light state.
        if (Data.bTargetLightVisibility != Data.bCurrentLightVisibility)
        {
            Data.bCurrentLightVisibility = Data.bTargetLightVisibility;
            LightComp->SetVisibility(Data.bCurrentLightVisibility);
        }

        // Apply decal state.
        if (Data.bTargetDecalVisibility != Data.bCurrentDecalVisibility)
        {
            Data.bCurrentDecalVisibility = Data.bTargetDecalVisibility;

            if (Data.bCurrentDecalVisibility)
            {
                // Lazily create the decal when it is needed but not yet allocated.
                UDecalComponent *DecalComp = Data.DecalComponent.Get();
                if (!IsValid(DecalComp) && Data.TargetDecalMaterial.IsValid())
                {
                    DecalComp = CreateDecalComponent(LightComp, Data.TargetDecalMaterial.Get());
                    Data.DecalComponent = DecalComp;
                }

                if (IsValid(DecalComp))
                {
                    DecalComp->SetWorldLocationAndRotation(Data.Position, FRotator(-90.0f, 0.0f, 0.0f));
                    DecalComp->SetVisibility(true);
                }
            }
            else
            {
                if (UDecalComponent *DecalComp = Data.DecalComponent.Get())
                {
                    DecalComp->SetVisibility(false);
                }
            }
        }
        else if (Data.bCurrentDecalVisibility)
        {
            // Keep visible decal synced with moving dynamic lights.
            if (UDecalComponent* DecalComp = Data.DecalComponent.Get(); IsValid(DecalComp))
            {
                if (!DecalComp->GetComponentLocation().Equals(Data.Position, 1.0f))
                {
                    DecalComp->SetWorldLocationAndRotation(Data.Position, FRotator(-90.0f, 0.0f, 0.0f));
                }
            }
        }
    }
}

UDecalComponent *UDynamicLightSubsystem::CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material)
{
    if (!IsValid(LightComp) || !IsValid(Material))
    {
        return nullptr;
    }

    AActor *Owner = LightComp->GetOwner();
    USceneComponent* RootComponent = IsValid(Owner) ? Owner->GetRootComponent() : nullptr;
    if (!IsValid(Owner) || !IsValid(RootComponent))
    {
        return nullptr;
    }

    UDecalComponent *NewDecal = NewObject<UDecalComponent>(Owner);
    if (!IsValid(NewDecal))
    {
        return nullptr;
    }

    Owner->AddInstanceComponent(NewDecal);
    NewDecal->SetMobility(EComponentMobility::Movable);
    NewDecal->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    NewDecal->SetWorldLocationAndRotation(LightComp->GetComponentLocation(), FRotator(-90.0f, 0.0f, 0.0f)); // Default downward projection.

    // Clamp fallback decals so they cannot cover the whole scene with a white wash.
    const float LightRadius = FMath::Clamp(LightComp->AttenuationRadius, 1.0f, LightComp->GetMaxLightDecalSize());
    NewDecal->DecalSize = FVector(LightRadius, LightRadius, LightRadius);

    // Prevent UE's default FadeScreenSize (0.01) from culling distant fallback decals!
    NewDecal->FadeScreenSize = 0.0f;
    NewDecal->SetFadeOut(0.0f, 0.0f, false);
    NewDecal->bDestroyOwnerAfterFade = false;

    const FLinearColor Color = ComputeEffectiveDecalColor(LightComp);
    NewDecal->SetDecalColor(Color);

    if (UMaterialInstanceDynamic* DecalMID = UMaterialInstanceDynamic::Create(Material, NewDecal))
    {
        DecalMID->SetVectorParameterValue(TEXT("Color"), Color);
        DecalMID->SetVectorParameterValue(TEXT("LightColor"), Color);
        DecalMID->SetVectorParameterValue(TEXT("BaseColor"), Color);
        DecalMID->SetScalarParameterValue(TEXT("Opacity"), Color.A);
        DecalMID->SetScalarParameterValue(TEXT("Intensity"), Color.A);
        NewDecal->SetDecalMaterial(DecalMID);
    }
    else
    {
        NewDecal->SetDecalMaterial(Material);
    }

    NewDecal->RegisterComponent();
    return NewDecal;
}
