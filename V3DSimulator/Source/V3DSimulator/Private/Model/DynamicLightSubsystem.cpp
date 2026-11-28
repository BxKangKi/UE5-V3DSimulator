// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicLightSubsystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Quality-scaled active dynamic light & shadow budgets,
 * hysteresis distance culling, and throttled (1-2 per tick) light/decal transitions.
 */

#include "Model/DynamicLightSubsystem.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
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
    struct FDynamicLightQualityBudget
    {
        int32 MaxActiveLights = 64;
        int32 MaxShadowLights = 4;
        int32 MaxActiveDecals = 24;
        float DistanceScaleSq = 1.0f;
        int32 MaxTransitionsPerTick = 2;
        int32 MaxDecalCreationsPerTick = 1;
    };

    FDynamicLightQualityBudget ResolveLightQualityBudget()
    {
        int32 Quality = 2; // Default High
        if (GEngine)
        {
            if (const UGameUserSettings* Settings = GEngine->GetGameUserSettings())
            {
                Quality = FMath::Clamp(Settings->GetShadingQuality(), 0, 4);
            }
        }

        FDynamicLightQualityBudget Budget;
        switch (Quality)
        {
        case 0: // Low
            Budget.MaxActiveLights = 16;
            Budget.MaxShadowLights = 0;
            Budget.MaxActiveDecals = 8;
            Budget.DistanceScaleSq = 0.65f * 0.65f;
            Budget.MaxTransitionsPerTick = 1;
            Budget.MaxDecalCreationsPerTick = 1;
            break;
        case 1: // Medium
            Budget.MaxActiveLights = 32;
            Budget.MaxShadowLights = 2;
            Budget.MaxActiveDecals = 16;
            Budget.DistanceScaleSq = 0.85f * 0.85f;
            Budget.MaxTransitionsPerTick = 2;
            Budget.MaxDecalCreationsPerTick = 1;
            break;
        case 2: // High
            Budget.MaxActiveLights = 64;
            Budget.MaxShadowLights = 4;
            Budget.MaxActiveDecals = 24;
            Budget.DistanceScaleSq = 1.0f;
            Budget.MaxTransitionsPerTick = 2;
            Budget.MaxDecalCreationsPerTick = 1;
            break;
        case 3: // Epic
            Budget.MaxActiveLights = 96;
            Budget.MaxShadowLights = 6;
            Budget.MaxActiveDecals = 32;
            Budget.DistanceScaleSq = 1.25f * 1.25f;
            Budget.MaxTransitionsPerTick = 2;
            Budget.MaxDecalCreationsPerTick = 1;
            break;
        case 4: // Cinematic
        default:
            Budget.MaxActiveLights = 128;
            Budget.MaxShadowLights = 8;
            Budget.MaxActiveDecals = 48;
            Budget.DistanceScaleSq = 1.50f * 1.50f;
            Budget.MaxTransitionsPerTick = 2;
            Budget.MaxDecalCreationsPerTick = 1;
            break;
        }
#if PLATFORM_ANDROID || PLATFORM_IOS
        Budget.MaxActiveLights = FMath::Max(8, Budget.MaxActiveLights / 2);
        Budget.MaxShadowLights = Budget.MaxShadowLights / 2;
        Budget.MaxActiveDecals = FMath::Max(4, Budget.MaxActiveDecals / 2);
#endif
        return Budget;
    }

    void DestroyManagedDecal(UDecalComponent* Decal)
    {
        if (!IsValid(Decal))
        {
            return;
        }

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
            if (UMaterialInterface* Loaded = Registry->StaticDecalLightMaterial.Get(); IsValid(Loaded))
            {
                return Loaded;
            }
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
    ScratchRankedLights.Empty();
    ScratchDecalCandidates.Empty();
    ScratchPendingTransitions.Empty();
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

    const FDynamicLightQualityBudget Budget = ResolveLightQualityBudget();

    FLightOptimizationData NewData;
    NewData.Position = InLight->GetComponentLocation();
    NewData.CullingDistanceSq = FMath::Square(CullingDist);
    NewData.DecalTransitionDistanceSq = EffectiveDecalDistSq;
    NewData.LightComponent = InLight;
    NewData.TargetDecalMaterial = bCanUseDecalFallback ? ResolvedMaterial : nullptr;
    NewData.bAuthoredCastShadows = InLight->CastShadows != 0;

    // Start newly streamed lights disabled when already at/above the active light cap so
    // streaming a dense chunk of streetlights never causes a 1-frame GPU lighting/VSM spike.
    if (ManagedLights.Num() >= Budget.MaxActiveLights)
    {
        if (InLight->IsVisible())
        {
            InLight->SetVisibility(false);
        }
        if (InLight->CastShadows != 0)
        {
            InLight->SetCastShadows(false);
        }
        NewData.bCurrentLightVisibility = false;
        NewData.bCurrentCastShadows = false;
    }
    else
    {
        NewData.bCurrentLightVisibility = InLight->IsVisible();
        NewData.bCurrentCastShadows = InLight->CastShadows != 0;
    }

    ManagedLights.Add(NewData);
    RegisterGameUpdate();
}

void UDynamicLightSubsystem::UnregisterLight(UDynamicPointLightComponent *InLight)
{
    for (int32 i = ManagedLights.Num() - 1; i >= 0; --i)
    {
        if (ManagedLights[i].LightComponent.Get() == InLight)
        {
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

    APlayerController *PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!IsValid(PC))
    {
        return;
    }

    FVector CameraLocation;
    FRotator CameraRotation;
    PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

    const FDynamicLightQualityBudget Budget = ResolveLightQualityBudget();

    // Pass 1: Measure distances and collect candidates within the quality-scaled culling range
    // (with 10% distance hysteresis to stop boundary flickering).
    ScratchRankedLights.Reset();
    for (int32 Index = 0; Index < ManagedLights.Num(); ++Index)
    {
        FLightOptimizationData& Data = ManagedLights[Index];
        UDynamicPointLightComponent* LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
        {
            continue;
        }

        Data.Position = LightComp->GetComponentLocation();
        const float DistSq = static_cast<float>(FVector::DistSquared(CameraLocation, Data.Position));
        Data.LastDistanceSq = DistSq;

        const float EffectiveCullSq = Data.CullingDistanceSq * Budget.DistanceScaleSq;
        const float HysteresisThresholdSq = Data.bCurrentLightVisibility ? EffectiveCullSq : (EffectiveCullSq * 0.81f);

        Data.bTargetLightVisibility = false;
        Data.bTargetCastShadows = false;
        Data.bTargetDecalVisibility = false;

        if (DistSq < HysteresisThresholdSq)
        {
            // Small distance bonus for already-active lights prevents rank thrashing between equal-distance pairs.
            const float SortKey = Data.bCurrentLightVisibility ? (DistSq * 0.90f) : DistSq;
            ScratchRankedLights.Emplace(SortKey, Index);
        }
    }

    // Pass 2: Pick only the closest MaxActiveLights for real point lights,
    // and only the closest MaxShadowLights among them for shadow casting.
    if (ScratchRankedLights.Num() > 1)
    {
        ScratchRankedLights.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
        {
            return A.Key < B.Key;
        });
    }

    const int32 ActiveBudget = FMath::Min(ScratchRankedLights.Num(), Budget.MaxActiveLights);
    int32 AssignedShadows = 0;
    for (int32 Rank = 0; Rank < ActiveBudget; ++Rank)
    {
        FLightOptimizationData& Data = ManagedLights[ScratchRankedLights[Rank].Value];
        Data.bTargetLightVisibility = true;
        if (Data.bAuthoredCastShadows && AssignedShadows < Budget.MaxShadowLights)
        {
            Data.bTargetCastShadows = true;
            ++AssignedShadows;
        }
    }

    // Pass 3A: Rank all eligible far-LOD decal candidates by distance from camera so decals
    // follow the player evenly across the entire world instead of biasing toward early-streamed chunks.
    ScratchDecalCandidates.Reset();
    const bool bLightBudgetSaturated = (ScratchRankedLights.Num() > Budget.MaxActiveLights);
    for (int32 Index = 0; Index < ManagedLights.Num(); ++Index)
    {
        FLightOptimizationData& Data = ManagedLights[Index];
        if (!Data.LightComponent.IsValid() || Data.bTargetLightVisibility)
        {
            continue;
        }

        const float EffectiveCullSq = Data.CullingDistanceSq * Budget.DistanceScaleSq;
        const bool bIsFarLodBand = (Data.LastDistanceSq >= EffectiveCullSq * 0.85f) || bLightBudgetSaturated;
        const bool bWithinDecalRange = (Data.DecalTransitionDistanceSq == 0.0f)
            || (Data.DecalTransitionDistanceSq > Data.CullingDistanceSq
                && Data.LastDistanceSq < Data.DecalTransitionDistanceSq);
        if (bIsFarLodBand
            && Data.TargetDecalMaterial.IsValid()
            && Data.DecalTransitionDistanceSq >= 0.0f
            && bWithinDecalRange)
        {
            const float SortKey = Data.bCurrentDecalVisibility ? (Data.LastDistanceSq * 0.90f) : Data.LastDistanceSq;
            ScratchDecalCandidates.Emplace(SortKey, Index);
        }
    }

    if (ScratchDecalCandidates.Num() > 1)
    {
        ScratchDecalCandidates.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
        {
            return A.Key < B.Key;
        });
    }

    const int32 DecalBudget = FMath::Min(ScratchDecalCandidates.Num(), Budget.MaxActiveDecals);
    for (int32 Rank = 0; Rank < DecalBudget; ++Rank)
    {
        ManagedLights[ScratchDecalCandidates[Rank].Value].bTargetDecalVisibility = true;
    }

    // Pass 3B: Collect pending state transitions sorted nearest-first so we throttle strictly to 1-2 per tick.
    ScratchPendingTransitions.Reset();
    for (int32 Index = 0; Index < ManagedLights.Num(); ++Index)
    {
        FLightOptimizationData& Data = ManagedLights[Index];
        if (!Data.LightComponent.IsValid())
        {
            continue;
        }

        const bool bNeedsTransition = (Data.bTargetLightVisibility != Data.bCurrentLightVisibility)
            || (Data.bTargetDecalVisibility != Data.bCurrentDecalVisibility)
            || (Data.bTargetLightVisibility && Data.bTargetCastShadows != Data.bCurrentCastShadows);

        if (bNeedsTransition)
        {
            ScratchPendingTransitions.Emplace(Data.LastDistanceSq, Index);
        }
        else if (Data.bCurrentDecalVisibility)
        {
            // Keep already-visible decal synced with moving dynamic lights.
            if (UDecalComponent* DecalComp = Data.DecalComponent.Get(); IsValid(DecalComp))
            {
                if (!DecalComp->GetComponentLocation().Equals(Data.Position, 1.0f))
                {
                    DecalComp->SetWorldLocationAndRotation(Data.Position, FRotator(-90.0f, 0.0f, 0.0f));
                }
            }
        }
    }

    if (ScratchPendingTransitions.Num() > 1)
    {
        ScratchPendingTransitions.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
        {
            return A.Key < B.Key;
        });
    }

    // Pass 4: Execute at most MaxTransitionsPerTick (1-2) light/decal switches per update
    // to eliminate render-state / VSM invalidation hitches.
    int32 TransitionsApplied = 0;
    int32 DecalCreationsApplied = 0;
    for (const TPair<float, int32>& Entry : ScratchPendingTransitions)
    {
        if (TransitionsApplied >= Budget.MaxTransitionsPerTick)
        {
            break;
        }

        FLightOptimizationData& Data = ManagedLights[Entry.Value];
        UDynamicPointLightComponent* LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
        {
            continue;
        }

        // Handle decal creation cap before switching a light off -> decal on.
        if (Data.bTargetDecalVisibility && !Data.bCurrentDecalVisibility)
        {
            UDecalComponent* DecalComp = Data.DecalComponent.Get();
            if (!IsValid(DecalComp) && Data.TargetDecalMaterial.IsValid())
            {
                if (DecalCreationsApplied >= Budget.MaxDecalCreationsPerTick)
                {
                    continue; // Defer creation to next tick.
                }
                DecalComp = CreateDecalComponent(LightComp, Data.TargetDecalMaterial.Get());
                Data.DecalComponent = DecalComp;
                ++DecalCreationsApplied;
            }
            Data.bCurrentDecalVisibility = true;
            if (IsValid(DecalComp))
            {
                DecalComp->SetWorldLocationAndRotation(Data.Position, FRotator(-90.0f, 0.0f, 0.0f));
                DecalComp->SetVisibility(true);
            }
        }
        else if (!Data.bTargetDecalVisibility && Data.bCurrentDecalVisibility)
        {
            Data.bCurrentDecalVisibility = false;
            // Release unused decal & MID memory promptly instead of retaining dormant far-LOD decals.
            DestroyManagedDecal(Data.DecalComponent.Get());
            Data.DecalComponent.Reset();
        }

        // Apply shadow casting change before or alongside light visibility.
        const bool bDesiredShadow = Data.bTargetLightVisibility && Data.bTargetCastShadows;
        if (bDesiredShadow != Data.bCurrentCastShadows)
        {
            Data.bCurrentCastShadows = bDesiredShadow;
            LightComp->SetCastShadows(bDesiredShadow);
        }

        if (Data.bTargetLightVisibility != Data.bCurrentLightVisibility)
        {
            Data.bCurrentLightVisibility = Data.bTargetLightVisibility;
            LightComp->SetVisibility(Data.bCurrentLightVisibility);
        }

        ++TransitionsApplied;
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
    NewDecal->SetupAttachment(RootComponent);
    NewDecal->SetWorldLocationAndRotation(LightComp->GetComponentLocation(), FRotator(-90.0f, 0.0f, 0.0f));

    const float LightRadius = FMath::Clamp(LightComp->AttenuationRadius, 1.0f, LightComp->GetMaxLightDecalSize());
    NewDecal->DecalSize = FVector(LightRadius, LightRadius, LightRadius);

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
