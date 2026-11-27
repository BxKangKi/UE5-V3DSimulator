// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicPointLightComponent.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/DynamicPointLightComponent.h"
#include "Model/DynamicLightSubsystem.h"
#include "Engine/World.h"

UDynamicPointLightComponent::UDynamicPointLightComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // Disable ticking to remove scheduling overhead.
    bCastVolumetricShadow = false;
    bAffectTranslucentLighting = false;
    CastStaticShadows = false;
    MaxDrawDistance = 12000.0f;
    MaxDistanceFadeRange = 2000.0f;
}

void UDynamicPointLightComponent::NotifySubsystemRefresh()
{
    if (UWorld* World = GetWorld(); IsValid(World) && World->IsGameWorld())
    {
        if (UDynamicLightSubsystem* Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->RegisterLight(this);
        }
    }
}

void UDynamicPointLightComponent::SetLightDecal(UMaterialInterface* InDecal)
{
    LightDecal = InDecal;
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::SetLightDecalFallbackEnabled(const bool bEnable)
{
    bEnableLightDecalFallback = bEnable;
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::SetCullingDistance(const float InDistance)
{
    CullingDistance = FMath::Max(0.0f, InDistance);
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::SetDecalTransitionDistance(const float InDistance)
{
    DecalTransitionDistance = FMath::Max(0.0f, InDistance);
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::OnRegister()
{
    Super::OnRegister();
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::OnUnregister()
{
    if (UWorld* World = GetWorld(); IsValid(World) && World->IsGameWorld())
    {
        if (UDynamicLightSubsystem* Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->UnregisterLight(this);
        }
    }
    Super::OnUnregister();
}

void UDynamicPointLightComponent::BeginPlay()
{
    Super::BeginPlay();
    NotifySubsystemRefresh();
}

void UDynamicPointLightComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        if (UDynamicLightSubsystem* Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->UnregisterLight(this);
        }
    }
    Super::EndPlay(EndPlayReason);
}