// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file WaterActor.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/WaterActor.h"
#include "World/PhysicalMaterialProfile.h"
#include "Materials/Material.h"
#include "World/WaterExclusionBoxComponent.h"
#include "World/WaterQuerySubsystem.h"
#include "World/BuoyancyComponent.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "Character/CharacterController.h"
#include "Interface/WaterInteract.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/PostProcessComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/DecalComponent.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"

namespace
{
    constexpr int32 MaxWaterRenderExclusionBoxes = 8;

    FName MakeIndexedWaterParameterName(const TCHAR* Prefix, const int32 Index)
    {
        return FName(*FString::Printf(TEXT("%s_%d"), Prefix, Index));
    }

    void ApplyPhysicalWaterSurfaceParameters(UMaterialInstanceDynamic* MID)
    {
        if (!IsValid(MID)) return;
        const bool bSingleLayer = MID->GetShadingModels().HasShadingModel(MSM_SingleLayerWater);
        for (const auto& Parameter : V3DPhysicalMaterial::WaterScalars)
        {
            float Current = 0.0f;
            const FName Name(Parameter.Name);
            if (!MID->GetScalarParameterValue(Name, Current)) continue;
            const bool bOpacity = Name == TEXT("Opacity") || Name == TEXT("Water Opacity");
            // Zero opacity is correct for SingleLayerWater's surface/volume mix, but would
            // erase an ordinary translucent custom water material entirely.
            const float Value = bOpacity && !bSingleLayer ? 0.68f : Parameter.Value;
            if (!FMath::IsNearlyEqual(Current, Value, 1.e-7f)) MID->SetScalarParameterValue(Name, Value);
        }
        for (const auto& Parameter : V3DPhysicalMaterial::WaterVectors)
        {
            FLinearColor Current;
            const FName Name(Parameter.Name);
            if (MID->GetVectorParameterValue(Name, Current) && !Current.Equals(Parameter.Value, 1.e-8f))
                MID->SetVectorParameterValue(Name, Parameter.Value);
        }
    }

    void ApplyPhysicalLocalUnderwaterParameters(UMaterialInstanceDynamic* PostProcessMID)
    {
        if (!IsValid(PostProcessMID))
        {
            return;
        }
        PostProcessMID->SetVectorParameterValue(TEXT("Fog Ambient Color"), FLinearColor(0.012f, 0.036f, 0.082f, 1.0f));
        PostProcessMID->SetVectorParameterValue(TEXT("Fog Scatter Color"), FLinearColor(0.018f, 0.052f, 0.115f, 1.0f));
        PostProcessMID->SetVectorParameterValue(TEXT("Band Color"), FLinearColor(0.010f, 0.028f, 0.064f, 1.0f));
        PostProcessMID->SetVectorParameterValue(TEXT("ShaftColor"), FLinearColor(0.06f, 0.14f, 0.24f, 1.0f));
        PostProcessMID->SetScalarParameterValue(TEXT("Scattering Aniso"), 0.58f);
        PostProcessMID->SetScalarParameterValue(TEXT("DistortionIntensity"), 0.12f);
    }

}

AWaterActor::AWaterActor()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Decal = CreateDefaultSubobject<UDecalComponent>(TEXT("StaticMesh"));
    Decal->SetupAttachment(RootComponent);
    Decal->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
    Decal->DecalSize = FVector(1.0f, 1.0f, 1.0f);
    Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
    Collision->SetupAttachment(RootComponent);
    Collision->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
    Collision->SetBoxExtent(FVector(1.0f, 1.0f, 1.0f));
    Collision->SetEnableGravity(false);
    Collision->SetVisibility(false);
    // Water is a trigger/query volume, never a solid physics wall. The old default primitive
    // response could block capsules and ragdoll bodies depending on the project collision preset.
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
    Collision->SetGenerateOverlapEvents(true);
    Collision->SetCanEverAffectNavigation(false);
    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProces"));
    PostProcess->SetupAttachment(Collision);
    PostProcess->bUnbound = false;
    PostProcess->bEnabled = true;
    PostProcess->BlendRadius = 5.0f;
    StaticMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WaterStaticMesh"));
    StaticMesh->SetupAttachment(RootComponent);
    StaticMesh->SetEnableGravity(false);
    StaticMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    StaticMesh->SetGenerateOverlapEvents(false);
    StaticMesh->SetCastShadow(false);
    StaticMesh->SetVisibleInRayTracing(false); // Prevent SingleLayerWater RT self-intersection blocking Lumen reflections.
    StaticMesh->SetRenderCustomDepth(false); // Shaft occlusion uses solid geometry only.
    StaticMesh->SetVisibility(true, true);
    StaticMesh->SetHiddenInGame(false, true);
}

void AWaterActor::BeginPlay()
{
    Super::BeginPlay();

    // Re-assert native water collision at runtime as Blueprint subclasses may carry older serialized
    // component presets that override constructor defaults. Water must overlap, never block.
    if (IsValid(Collision.Get()))
    {
        Collision->SetCollisionEnabled(bGlobalOcean ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
        Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
        Collision->SetGenerateOverlapEvents(!bGlobalOcean);
        Collision->SetCanEverAffectNavigation(false);
    }
    if (IsValid(StaticMesh.Get()))
    {
        StaticMesh->SetRenderCustomDepth(false);
        StaticMesh->SetVisibleInRayTracing(false);
        StaticMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        StaticMesh->SetGenerateOverlapEvents(false);
        StaticMesh->SetVisibility(true, true);
        StaticMesh->SetHiddenInGame(false, true);
    }

    if (UV3DSimulatorAssetRegistry* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        // Empty registry entries must not erase values authored on BP_Water or another derived class.
        if (!Registry->WaterDecalMaterial.IsNull())
        {
            if (UMaterialInterface* RegistryDecal = Registry->WaterDecalMaterial.LoadSynchronous())
            {
                DecalMaterial = RegistryDecal;
            }
        }
        if (!bGlobalOcean && !Registry->UnderWaterMaterial.IsNull())
        {
            if (UMaterialInterface* RegistryUnderWater = Registry->UnderWaterMaterial.LoadSynchronous())
            {
                UnderWaterMaterial = RegistryUnderWater;
            }
        }
        if (IsValid(StaticMesh) && !Registry->WaterMesh.IsNull())
        {
            if (UStaticMesh* RegistryWaterMesh = Registry->WaterMesh.LoadSynchronous())
            {
                StaticMesh->SetStaticMesh(RegistryWaterMesh);
            }
        }
    }

    if (!IsValid(StaticMesh.Get()) || !IsValid(StaticMesh->GetStaticMesh()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Water actor has no render mesh. Configure AssetRegistry.WaterMesh or a derived WaterActor class."));
    }

    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->RegisterWaterActor(this);
    }
    SetCurrentLevel();
    if (!bGlobalOcean)
    {
        bAuthoredPostProcessEnabled = IsValid(PostProcess.Get()) ? PostProcess->bEnabled : true;
    }
    EnsureWaterMaterialInstances();
    ApplyWaterExclusionRenderParameters();
    UMaterialInstanceDynamic *DecalMID = IsValid(DecalMaterial) ? UMaterialInstanceDynamic::Create(DecalMaterial, this) : nullptr;
    if (DecalMID && IsValid(Decal.Get()))
    {
        DecalMID->SetScalarParameterValue(TEXT("WaterLevel"), Level);
        Decal->SetDecalMaterial(DecalMID);
    }
    // The global ocean no longer owns an underwater PP material. WorldEnvManager's single
    // unbound underwater MID receives WaterLevel from the current ocean data.
    UMaterialInstanceDynamic *PostProcessMID = !bGlobalOcean && IsValid(UnderWaterMaterial)
        ? UMaterialInstanceDynamic::Create(UnderWaterMaterial, this)
        : nullptr;
    if (PostProcessMID && IsValid(PostProcess.Get()))
    {
        PostProcess->AddOrUpdateBlendable(PostProcessMID, 1.0f);
        PostProcessMID->SetScalarParameterValue(FName("WaterLevel"), Level);
        ApplyPhysicalLocalUnderwaterParameters(PostProcessMID);
    }
}

void AWaterActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->UnregisterWaterActor(this);
    }
    WaterMaterialInstances.Reset();
    Super::EndPlay(EndPlayReason);
}

void AWaterActor::SetGlobalOcean(const bool bInGlobalOcean)
{
    bGlobalOcean = bInGlobalOcean;
    SetCurrentLevel();

    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->SetGlobalOceanActor(this, bGlobalOcean);
    }

    // The global ocean is an infinite XY half-space below Level. Its collision box is rendering
    // scaffolding only and must never determine water presence. Local/streamed water actors retain
    // their normal overlap volume behaviour.
    if (IsValid(Collision.Get()))
    {
        Collision->SetCollisionEnabled(bGlobalOcean ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
        Collision->SetGenerateOverlapEvents(!bGlobalOcean);
        if (!bGlobalOcean)
        {
            Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
        }
    }

    if (IsValid(PostProcess.Get()))
    {
        // Global-ocean underwater rendering is owned exclusively by WorldEnvManager's unbound
        // post process. Keep this actor's finite PP only for local/streamed water volumes.
        PostProcess->bUnbound = false;
        PostProcess->bEnabled = bGlobalOcean ? false : bAuthoredPostProcessEnabled;
    }
}

void AWaterActor::SetGlobalOceanRenderRadius(const float RadiusCm)
{
    if (!bGlobalOcean || !FMath::IsFinite(RadiusCm) || RadiusCm <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    // This radius is visual only. Global-ocean water presence remains an infinite XY half-space
    // resolved from OceanHeightCm, so reducing the mesh footprint never changes buoyancy/swimming.
    const float SafeRadiusCm = FMath::Max(100.0f, RadiusCm);
    if (FMath::IsNearlyEqual(GlobalOceanRenderRadiusCm, SafeRadiusCm, 1.0f))
    {
        return;
    }

    if (!IsValid(StaticMesh.Get()) || !IsValid(StaticMesh->GetStaticMesh()))
    {
        return;
    }

    const FBoxSphereBounds LocalBounds = StaticMesh->GetStaticMesh()->GetBounds();
    const FVector RelativeScale = StaticMesh->GetRelativeScale3D().GetAbs();
    const double BaseHalfExtentX = static_cast<double>(LocalBounds.BoxExtent.X)
        * static_cast<double>(FMath::Max(RelativeScale.X, UE_SMALL_NUMBER));
    const double BaseHalfExtentY = static_cast<double>(LocalBounds.BoxExtent.Y)
        * static_cast<double>(FMath::Max(RelativeScale.Y, UE_SMALL_NUMBER));
    const double BaseHalfExtent = FMath::Min(BaseHalfExtentX, BaseHalfExtentY);
    if (!FMath::IsFinite(BaseHalfExtent) || BaseHalfExtent <= UE_SMALL_NUMBER)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Global ocean mesh has invalid XY bounds; render-radius sizing skipped. Mesh=%s Bounds=%s"),
            *GetNameSafe(StaticMesh->GetStaticMesh()),
            *LocalBounds.BoxExtent.ToCompactString());
        return;
    }

    // Use one uniform actor scale so the camera-following water surface covers the requested
    // circular view radius and the attached underwater decal also receives a sufficiently deep
    // projection volume. Global-ocean collision/underwater detection are height-based and do not
    // depend on this actor scale, so enlarging Z is visual-only for the global ocean.
    const double RequiredScale = static_cast<double>(SafeRadiusCm) / BaseHalfExtent;
    if (!FMath::IsFinite(RequiredScale) || RequiredScale <= UE_SMALL_NUMBER)
    {
        return;
    }

    FVector NewActorScale = GetActorScale3D();
    NewActorScale.X = RequiredScale;
    NewActorScale.Y = RequiredScale;
    NewActorScale.Z = RequiredScale;

    if (!NewActorScale.Equals(GetActorScale3D(), 0.001))
    {
        SetActorScale3D(NewActorScale);
    }
    GlobalOceanRenderRadiusCm = SafeRadiusCm;

    UE_LOG(LogTemp, Display,
        TEXT("Global ocean visual radius configured. Radius=%.1f m Mesh=%s ActorScaleXYZ=%.3f"),
        SafeRadiusCm / 100.0f,
        *GetNameSafe(StaticMesh->GetStaticMesh()),
        NewActorScale.X);
}

void AWaterActor::WaterTrigger(AActor *OtherActor, bool InWater)
{
    if (!IsValid(this) || IsActorBeingDestroyed() || !IsValid(OtherActor) || OtherActor->IsActorBeingDestroyed())
    {
        return;
    }

    const TWeakObjectPtr<AWaterActor> WeakSelf(this);
    const TWeakObjectPtr<AActor> WeakOther(OtherActor);
    const auto StillAlive = [WeakSelf, WeakOther]()
    {
        const AWaterActor* Self = WeakSelf.Get();
        const AActor* Other = WeakOther.Get();
        return Self && Other && !Self->IsActorBeingDestroyed() && !Other->IsActorBeingDestroyed();
    };

    // WaterInteraction and Buoyancy are intentionally independent switches. Generic swimming /
    // splash state sees an exclusion as dry, while UBuoyancyComponent keeps the underlying water
    // overlap latched and suppresses force per sample. Keeping that overlap latched is important:
    // an object can leave an exclusion box without ever leaving the large ocean overlap and must
    // resume buoyancy immediately without waiting for a new BeginOverlap event.
    const bool bInteractionExcluded = InWater && UWaterExclusionBoxComponent::IsLocationExcluded(
        this, OtherActor->GetActorLocation(), EWaterExclusionFeature::WaterInteraction);
    const bool bInteractionInWater = InWater && !bInteractionExcluded;

    if (IWaterInteract* ActorWaterInteract = Cast<IWaterInteract>(OtherActor))
    {
        if (bInteractionInWater)
        {
            ActorWaterInteract->EnterWater(Level);
        }
        else
        {
            ActorWaterInteract->ExitWater(Level);
        }
    }

    // An interface callback may destroy either actor, or remove sibling components.
    if (!StillAlive()) return;
    TInlineComponentArray<UActorComponent*, 16> Components;
    OtherActor->GetComponents(Components);
    TArray<TWeakObjectPtr<UActorComponent>, TInlineAllocator<16>> Snapshot;
    for (UActorComponent* Component : Components) Snapshot.Add(Component);
    for (const auto& WeakComponent : Snapshot)
    {
        if (!StillAlive()) return;
        UActorComponent* Component = WeakComponent.Get();
        if (!Component || Component->IsBeingDestroyed()) continue;
        if (IWaterInteract* ComponentWaterInteract = Cast<IWaterInteract>(Component))
        {
            // Global-ocean buoyancy is height-driven inside UBuoyancyComponent and must never be
            // latched by a synthetic overlap. Local water keeps overlap state, while exclusions are
            // still applied per buoyancy sample.
            if (bGlobalOcean && Cast<UBuoyancyComponent>(Component))
            {
                continue;
            }

            const bool bComponentInWater = Cast<UBuoyancyComponent>(Component)
                ? InWater
                : bInteractionInWater;
            if (bComponentInWater)
            {
                ComponentWaterInteract->EnterWater(Level);
            }
            else
            {
                ComponentWaterInteract->ExitWater(Level);
            }
        }
    }
}

void AWaterActor::NotifyActorBeginOverlap(AActor* OtherActor)
{
    if (!bGlobalOcean)
    {
        WaterTrigger(OtherActor, true);
    }
}

void AWaterActor::NotifyActorEndOverlap(AActor* OtherActor)
{
    if (!bGlobalOcean)
    {
        WaterTrigger(OtherActor, false);
    }
}

void AWaterActor::SetCurrentLevel()
{
    FVector Location = GetActorLocation();
    Level = Location.Z;
}

void AWaterActor::CheckOverlappingWater(AActor *Target)
{
    if (!IsValid(Target))
    {
        return;
    }

    // Preserve local-water overlap compatibility, then resolve the height-driven global ocean
    // through the common query service. New gameplay code should query UWaterQuerySubsystem directly.
    TArray<AActor*> OverlappingActors;
    Target->GetOverlappingActors(OverlappingActors, AWaterActor::StaticClass());
    const TWeakObjectPtr<AActor> WeakTarget(Target);
    TArray<TWeakObjectPtr<AActor>, TInlineAllocator<16>> Snapshot;
    for (AActor* Actor : OverlappingActors) Snapshot.Add(Actor);
    for (const auto& WeakActor : Snapshot)
    {
        Target = WeakTarget.Get();
        if (!Target || Target->IsActorBeingDestroyed()) return;
        if (AWaterActor* WaterActor = Cast<AWaterActor>(WeakActor.Get());
            IsValid(WaterActor) && !WaterActor->IsGlobalOcean())
        {
            WaterActor->WaterTrigger(Target, true);
        }
    }

    Target = WeakTarget.Get();
    if (!Target || Target->IsActorBeingDestroyed()) return;
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(Target))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryActorLocation(
                Target,
                EWaterQueryMode::Strict,
                EWaterQueryPurpose::Interaction,
                Result)
            && Result.bGlobalOcean
            && IsValid(Result.WaterActor))
        {
            Result.WaterActor->WaterTrigger(Target, true);
        }
    }
}

bool AWaterActor::FindGlobalOceanLevel(const UObject* WorldContextObject, float& OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        return WaterQuery->GetGlobalOceanLevel(OutLevel);
    }
    return false;
}

bool AWaterActor::FindWaterLevelAtLocationStrict(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryWaterAtLocation(
                WorldLocation,
                EWaterQueryMode::Strict,
                EWaterQueryPurpose::Interaction,
                Result))
        {
            OutLevel = Result.SurfaceZ;
            return true;
        }
    }
    return false;
}

bool AWaterActor::FindWaterLevelAtLocation(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryWaterAtLocation(
                WorldLocation,
                EWaterQueryMode::Relaxed,
                EWaterQueryPurpose::Interaction,
                Result))
        {
            OutLevel = Result.SurfaceZ;
            return true;
        }
    }
    return false;
}

void AWaterActor::EnsureWaterMaterialInstances()
{
    if (!IsValid(StaticMesh.Get()))
    {
        return;
    }

    const int32 MaterialCount = StaticMesh->GetNumMaterials();
    if (MaterialCount <= 0)
    {
        WaterMaterialInstances.Reset();
        return;
    }

    WaterMaterialInstances.SetNum(MaterialCount);
    for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
    {
        UMaterialInterface* SlotMaterial = StaticMesh->GetMaterial(MaterialIndex);
        UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(SlotMaterial);
        if (IsValid(MID) && WaterMaterialInstances[MaterialIndex] == MID) continue;
        // A mesh asset/BP can share a MID between actors. Own a copy before assigning
        // actor-specific water/exclusion parameters; never mutate another water body's MID.
        auto* SourceMID = Cast<UMaterialInstanceDynamic>(SlotMaterial);
        UMaterialInterface* Parent = SourceMID ? SourceMID->Parent.Get() : SlotMaterial;
        MID = IsValid(Parent) ? UMaterialInstanceDynamic::Create(Parent, this) : nullptr;
        if (IsValid(MID))
        {
            if (SourceMID) MID->CopyParameterOverrides(SourceMID);
            ApplyPhysicalWaterSurfaceParameters(MID);
            StaticMesh->SetMaterial(MaterialIndex, MID);
        }
        WaterMaterialInstances[MaterialIndex] = MID;
    }
}

void AWaterActor::ApplyWaterExclusionRenderParameters()
{
    EnsureWaterMaterialInstances();

    TArray<FWaterExclusionNativeBox> Boxes;
    UWaterExclusionBoxComponent::GatherNativeBoxes(
        this,
        EWaterExclusionFeature::Rendering,
        Boxes,
        MaxWaterRenderExclusionBoxes);

    for (UMaterialInstanceDynamic* MID : WaterMaterialInstances)
    {
        if (!IsValid(MID))
        {
            continue;
        }

        MID->SetScalarParameterValue(TEXT("WaterExclusionCount"), static_cast<float>(Boxes.Num()));
        for (int32 Index = 0; Index < Boxes.Num(); ++Index)
        {
            const FWaterExclusionNativeBox& Box = Boxes[Index];
            const FTransform& Transform = Box.WorldTransform;
            const FVector Scale = Transform.GetScale3D().GetAbs();
            const FVector WorldExtent = Box.LocalExtent * Scale;
            const FQuat Rotation = Transform.GetRotation().GetNormalized();

            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionCenter"), Index),
                FLinearColor(Transform.GetLocation().X, Transform.GetLocation().Y, Transform.GetLocation().Z, 1.0f));
            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionExtent"), Index),
                FLinearColor(WorldExtent.X, WorldExtent.Y, WorldExtent.Z, 0.0f));
            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionRotation"), Index),
                FLinearColor(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
        }
    }
}

void AWaterActor::UpdateUnderwaterPostProcessForView(const FVector& ViewLocation, const bool bExcluded)
{
    if (!IsValid(PostProcess.Get()))
    {
        return;
    }

    if (bGlobalOcean)
    {
        // Global-ocean PP is evaluated by the unified WorldEnvManager material through MPC values.
        PostProcess->bEnabled = false;
        return;
    }

    // Local water keeps its authored finite PostProcessComponent bounds. Exclusions can still turn
    // that effect off while the camera is inside an exclusion box.
    PostProcess->bEnabled = bAuthoredPostProcessEnabled && !bExcluded;
}

void AWaterActor::RefreshWaterExclusionRendering(const UObject* WorldContextObject)
{
    UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject);
    if (!WaterQuery)
    {
        return;
    }

    WaterQuery->RequestExclusionRenderRefresh();
}

void AWaterActor::UpdateLocalViewWaterEffects(const UObject* WorldContextObject, const FVector& ViewLocation)
{
    UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject);
    if (!WaterQuery)
    {
        return;
    }

    const bool bSuppressPostProcess = UWaterExclusionBoxComponent::IsLocationExcluded(
        WorldContextObject,
        ViewLocation,
        EWaterExclusionFeature::UnderwaterPostProcess);

    TArray<AWaterActor*> WaterActors;
    WaterQuery->GetRegisteredWaterActors(WaterActors);
    for (AWaterActor* WaterActor : WaterActors)
    {
        if (IsValid(WaterActor) && !WaterActor->IsGlobalOcean())
        {
            WaterActor->UpdateUnderwaterPostProcessForView(ViewLocation, bSuppressPostProcess);
        }
    }
}

