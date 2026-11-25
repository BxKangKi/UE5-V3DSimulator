// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldSurfaceSubsystem.h"
#include "System/SoundEventSubsystem.h"
#include "Components/AudioComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Model/StaticActor.h"
#include "Model/V3DMaterialAssetReferences.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "StaticMeshResources.h"
#include "System/V3DTerrain.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedModelAsset.h"
#include "World/WorldEnvironmentSubsystem.h"
#include "TimerManager.h"

namespace WorldSurfaceTerrainPrivate
{
    static bool SampleTerrainVertexWeights(
        const FHitResult& Hit,
        UStaticMeshComponent* StaticComp,
        float OutWeights[4])
    {
        OutWeights[0] = OutWeights[1] = OutWeights[2] = OutWeights[3] = 0.0f;
        if (!IsValid(StaticComp) || Hit.FaceIndex < 0)
        {
            return false;
        }
        UStaticMesh* StaticMesh = StaticComp->GetStaticMesh();
        if (!IsValid(StaticMesh))
        {
            return false;
        }
        const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
        if (!RenderData || RenderData->LODResources.IsEmpty())
        {
            return false;
        }
        const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
        const FIndexArrayView Indices = LOD0.IndexBuffer.GetArrayView();
        const int32 BaseIdx = Hit.FaceIndex * 3;
        if (BaseIdx < 0 || BaseIdx + 2 >= Indices.Num())
        {
            return false;
        }

        const uint32 I0 = Indices[BaseIdx];
        const uint32 I1 = Indices[BaseIdx + 1];
        const uint32 I2 = Indices[BaseIdx + 2];
        const uint32 MaxIdx = FMath::Max3(I0, I1, I2);

        const FPositionVertexBuffer& PosBuf = LOD0.VertexBuffers.PositionVertexBuffer;
        const FColorVertexBuffer& ColorBuf = LOD0.VertexBuffers.ColorVertexBuffer;
        if (PosBuf.GetNumVertices() <= MaxIdx || ColorBuf.GetNumVertices() <= MaxIdx)
        {
            return false;
        }

        FVector LocalPoint = FVector::ZeroVector;
        bool bHasLocalPoint = false;
        if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(StaticComp);
            IsValid(ISM) && Hit.Item != INDEX_NONE)
        {
            FTransform InstanceWorld;
            if (ISM->GetInstanceTransform(Hit.Item, InstanceWorld, /*bWorldSpace=*/true))
            {
                LocalPoint = InstanceWorld.InverseTransformPosition(Hit.ImpactPoint);
                bHasLocalPoint = true;
            }
        }
        if (!bHasLocalPoint)
        {
            LocalPoint = StaticComp->GetComponentTransform().InverseTransformPosition(Hit.ImpactPoint);
        }

        const FVector P0(PosBuf.VertexPosition(I0));
        const FVector P1(PosBuf.VertexPosition(I1));
        const FVector P2(PosBuf.VertexPosition(I2));
        const FVector Bary = FMath::ComputeBaryCentric2D(LocalPoint, P0, P1, P2);

        float U = 1.0f / 3.0f;
        float V = 1.0f / 3.0f;
        float W = 1.0f / 3.0f;
        if (!Bary.ContainsNaN())
        {
            const float RawU = FMath::Clamp(static_cast<float>(Bary.X), 0.0f, 1.0f);
            const float RawV = FMath::Clamp(static_cast<float>(Bary.Y), 0.0f, 1.0f);
            const float RawW = FMath::Clamp(static_cast<float>(Bary.Z), 0.0f, 1.0f);
            const float BarySum = RawU + RawV + RawW;
            if (BarySum > KINDA_SMALL_NUMBER)
            {
                U = RawU / BarySum;
                V = RawV / BarySum;
                W = RawW / BarySum;
            }
        }

        const FColor& C0 = ColorBuf.VertexColor(I0);
        const FColor& C1 = ColorBuf.VertexColor(I1);
        const FColor& C2 = ColorBuf.VertexColor(I2);
        constexpr float Inv255 = 1.0f / 255.0f;

        OutWeights[0] = (U * C0.R + V * C1.R + W * C2.R) * Inv255;
        OutWeights[1] = (U * C0.G + V * C1.G + W * C2.G) * Inv255;
        OutWeights[2] = (U * C0.B + V * C1.B + W * C2.B) * Inv255;
        OutWeights[3] = (U * C0.A + V * C1.A + W * C2.A) * Inv255;

        const float WeightSum = OutWeights[0] + OutWeights[1] + OutWeights[2] + OutWeights[3];
        if (WeightSum <= KINDA_SMALL_NUMBER)
        {
            return false;
        }
        for (int32 Layer = 0; Layer < 4; ++Layer)
        {
            OutWeights[Layer] /= WeightSum;
        }
        return true;
    }

    static bool IsTerrainPaletteMaterial(UMaterialInterface* Material)
    {
        if (!IsValid(Material)) return false;
        float Flag = 0.0f;
        return Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("V3D_Terrain")), Flag) && Flag > 0.5f;
    }
}

UWorldSurfaceSubsystem* UWorldSurfaceSubsystem::Get(const UObject* Context)
{
    UWorld* World = IsValid(Context) ? Context->GetWorld() : nullptr;
    return World ? World->GetSubsystem<UWorldSurfaceSubsystem>() : nullptr;
}

void UWorldSurfaceSubsystem::Reset()
{
    check(IsInGameThread());
    if (UWorld* World = GetWorld())
        if (auto* Events = World->GetSubsystem<USoundEventSubsystem>()) Events->Reset();
    ++Generation;
    if (UWorld* World = GetWorld()) World->GetTimerManager().ClearAllTimersForObject(this);
    for (UAudioComponent* Sound : ActiveSounds) if (IsValid(Sound)) Sound->Stop();
    ActiveSounds.Empty();
    PendingSounds.Empty();
    PcmCache.Empty();
    for (const auto& Pair : UniformComponents)
        if (UPrimitiveComponent* Component = Pair.Key.Get(); IsValid(Component)
            && Component->GetPhysicsMaterialOverride() == Pair.Value.Get())
            Component->SetPhysMaterialOverride(nullptr);
    UniformComponents.Empty();
    TerrainLayersByAsset.Empty();
    TerrainAssetByComponent.Empty();
    for (const auto& Source : MaterialCache)
        for (const auto& Entry : Source.Value)
            if (auto* Material = Entry.Value.Get()) { Material->PhysMaterial = nullptr; Material->bOverridePhysMaterial = false; }
    MaterialCache.Empty();
    PhysicalMaterials.Empty();
    DefaultPhysicalMaterial = nullptr;
    LastSoundIndex.Empty(); FootstepLoops.Empty();
    Attenuation = nullptr;
    Config = FV3DWorldSurfaceConfig();
    InFlight = 0;
    CachedBytes = 0;
    MaterialApplications = 0;
}

void UWorldSurfaceSubsystem::Deinitialize()
{
    Reset();
    Super::Deinitialize();
}

void UWorldSurfaceSubsystem::RegisterTerrainLayers(const UObject* BakedAsset, const TArray<FString>& LayerNames)
{
    check(IsInGameThread());
    if (!IsValid(BakedAsset) || LayerNames.IsEmpty()) return;
    TerrainLayersByAsset.Add(BakedAsset, LayerNames);
}

bool UWorldSurfaceSubsystem::Configure(const TSharedPtr<FJsonObject>& Json, FString& Error)
{
    check(IsInGameThread());
    FV3DWorldSurfaceConfig Parsed;
    if (!FV3DWorldSurfaceConfig::Parse(Json, Parsed, Error)) return false;
    return Configure(MoveTemp(Parsed), Error);
}

bool UWorldSurfaceSubsystem::Configure(FV3DWorldSurfaceConfig Parsed, FString& Error)
{
    check(IsInGameThread());
    UWorld* World = GetWorld();
    if (!IsValid(World) || World->bIsTearingDown)
    { Error = TEXT("Surface configuration requires an active world."); return false; }
    UGameInstance* Instance = World->GetGameInstance();
    UModelDatabaseSubsystem* Database = Instance ? Instance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    TArray<FSoundDefinition> Sounds;
    if (Database) Database->GetSoundDefinitions(Sounds);
    TSet<FGuid> Available;
    for (const auto& Sound : Sounds) Available.Add(Sound.UUID);
    if (!Parsed.ValidateSounds(Available, Error)) return false;
    Reset();
    Config = MoveTemp(Parsed);
    if (const auto* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        DefaultPhysicalMaterial = Registry->ResolveDefaultPhysicalMaterial();
        if (!Registry->DefaultPhysicalMaterial.IsNull() && !IsValid(DefaultPhysicalMaterial))
            UE_LOG(LogTemp, Warning, TEXT("AssetRegistry.DefaultPhysicalMaterial could not be loaded: %s"),
                *Registry->DefaultPhysicalMaterial.ToSoftObjectPath().ToString());
    }
    if (auto* Events = World->GetSubsystem<USoundEventSubsystem>()) Events->Configure(Config);
    for (const auto& Pair : Config.PhysicalMaterials)
    {
        UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(this, NAME_None, RF_Transient);
        const FV3DSurfaceDefinition& Definition = Pair.Value;
        Material->Friction = Definition.Friction;
        Material->StaticFriction = Definition.StaticFriction;
        Material->Restitution = Definition.Restitution;
        Material->Density = Definition.Density;
        Material->bOverrideFrictionCombineMode = true;
        Material->bOverrideRestitutionCombineMode = true;
        Material->FrictionCombineMode = static_cast<EFrictionCombineMode::Type>(Definition.FrictionCombine);
        Material->RestitutionCombineMode = static_cast<EFrictionCombineMode::Type>(Definition.RestitutionCombine);
        PhysicalMaterials.Add(Pair.Key, Material);
    }
    for (const auto& Pair : Config.FootstepMappings)
        for (const FString& Reference : Pair.Value.Sounds) PendingSounds.AddUnique(Reference);
    if (PendingSounds.IsEmpty()) return true;
    PendingSounds.Sort();
    Attenuation = NewObject<USoundAttenuation>(this, NAME_None, RF_Transient);
    Attenuation->Attenuation.bAttenuate = true;
    Attenuation->Attenuation.bSpatialize = true;
    Attenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
    Attenuation->Attenuation.AttenuationShapeExtents = FVector(100.0);
    Attenuation->Attenuation.FalloffDistance = 1800.0f;
    if (GetWorld()->GetNetMode() != NM_DedicatedServer) PumpSoundLoads();
    return true;
}

void UWorldSurfaceSubsystem::ApplyToStaticMesh(UStaticMeshComponent* Component)
{
    ApplyToMesh(Component);
}

void UWorldSurfaceSubsystem::ApplyToMesh(UMeshComponent* Component)
{
    check(IsInGameThread());
    if (!IsValid(Component) || (PhysicalMaterials.IsEmpty() && !IsValid(DefaultPhysicalMaterial))) return;
    // Weak cached wrappers share unchanged shader/texture parameters without editing a shared
    // facade material (facades can be shared across worlds, including multiple PIE sessions).
    if ((++MaterialApplications % 128) == 0)
    {
        for (auto It = UniformComponents.CreateIterator(); It; ++It)
            if (!It.Key().IsValid() || !It.Value().IsValid()) It.RemoveCurrent();
        for (auto It = TerrainAssetByComponent.CreateIterator(); It; ++It)
            if (!It.Key().IsValid() || !It.Value().IsValid()) It.RemoveCurrent();
        for (auto It = TerrainLayersByAsset.CreateIterator(); It; ++It)
            if (!It.Key().IsValid()) It.RemoveCurrent();
        for (auto It = MaterialCache.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid()) { It.RemoveCurrent(); continue; }
            for (auto Surface = It.Value().CreateIterator(); Surface; ++Surface)
                if (!Surface.Value().IsValid()) Surface.RemoveCurrent();
            if (It.Value().IsEmpty()) It.RemoveCurrent();
        }
    }
    const TArray<FName> Slots = Component->GetMaterialSlotNames();
    UPhysicalMaterial* Uniform = nullptr;
    bool bUniform = Component->GetNumMaterials() > 0;
    for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
    {
        UMaterialInterface* Base = Component->GetMaterial(Index);
        const FString SlotStr = Slots.IsValidIndex(Index) ? Slots[Index].ToString() : FString();
        const FString SourceMatName = FV3DWorldSurfaceConfig::SourceMaterialName(SlotStr);

        if (IsValid(Base)
            && (WorldSurfaceTerrainPrivate::IsTerrainPaletteMaterial(Base)
                || SourceMatName.Equals(TEXT("V3D.TerrainPalette"), ESearchCase::IgnoreCase)))
        {
            if (const UObject* AssetOuter = Base->GetOuter(); IsValid(AssetOuter) && AssetOuter != this)
            {
                TerrainAssetByComponent.Add(Component, AssetOuter);
            }
        }

        FName Surface = Config.ResolveMaterial(SlotStr);
        if (Surface == Config.DefaultPhysicalMaterial)
        {
            const FString NormalizedName = FV3DMaterialAssetReferences::NormalizeMaterialName(SourceMatName);
            if (NormalizedName == TEXT("foliage") || NormalizedName.EndsWith(TEXT(".foliage")))
            {
                const FName FoliageSurface = Config.ResolveMaterial(TEXT("foliage"));
                if (!FoliageSurface.IsNone())
                {
                    Surface = FoliageSurface;
                }
            }
        }

        UPhysicalMaterial* Physical = PhysicalMaterials.FindRef(Surface);
        UPhysicalMaterial* Authored = IsValid(Base) ? Base->GetPhysicalMaterial() : nullptr;
        // JSON mappings/defaults win. A specifically authored material keeps its own surface;
        // only an unassigned/engine-default slot receives the registry fallback.
        if (!IsValid(Physical) && IsValid(DefaultPhysicalMaterial)
            && (!IsValid(Authored) || (GEngine && Authored == GEngine->DefaultPhysMaterial)))
            Physical = DefaultPhysicalMaterial;
        if (IsValid(Base) && IsValid(Physical))
        {
            // Applying a surface again must preserve the existing wrapper. Keying another
            // wrapper by that MID would create one more material on every refresh, defeat
            // sharing between components and retain redundant parameter/texture references.
            if (Base->GetPhysicalMaterial() != Physical)
            {
                auto& Cache = MaterialCache.FindOrAdd(TWeakObjectPtr<UMaterialInterface>(Base));
                UMaterialInstanceDynamic* Material = Cache.FindRef(Surface).Get();
                if (!IsValid(Material))
                {
                    UMaterialInterface* Parent = Base;
                    while (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Parent))
                    {
                        Parent = Dynamic->Parent;
                    }
                    if (!IsValid(Parent)) continue;
                    Material = UMaterialInstanceDynamic::Create(Parent, this,
                        MakeUniqueObjectName(this, UMaterialInstanceDynamic::StaticClass(), FName(TEXT("V3D_Surface"))));
                    Material->CopyMaterialUniformParameters(Base);
                    Material->PhysMaterial = Physical;
                    Material->bOverridePhysMaterial = true; // UE 5.8
                    Cache.Add(Surface, Material);
                }
                Component->SetMaterial(Index, Material);
            }
        }
        else if (!IsValid(Physical)) Physical = Authored;
        if (Index == 0) Uniform = Physical;
        else bUniform &= Uniform == Physical;
    }
    // A component override would erase all per-triangle materials on a mixed-material mesh.
    const TWeakObjectPtr<UPrimitiveComponent> ComponentKey(Component);
    if (Component->GetNumMaterials() == 0)
    {
        Uniform = PhysicalMaterials.FindRef(Config.DefaultPhysicalMaterial);
        if (!IsValid(Uniform)) Uniform = DefaultPhysicalMaterial;
        bUniform = IsValid(Uniform);
    }
    // Preserve a deliberate component override (for example vehicle low-friction bodies).
    UPhysicalMaterial* ExistingOverride = Component->GetPhysicsMaterialOverride();
    const auto* OwnedOverride = UniformComponents.Find(ComponentKey);
    const bool bOwnsOverride = OwnedOverride && OwnedOverride->Get() == ExistingOverride;
    if (IsValid(ExistingOverride) && !bOwnsOverride)
    {
        UniformComponents.Remove(ComponentKey);
        return;
    }
    if (bUniform && IsValid(Uniform)
        && (Uniform == DefaultPhysicalMaterial || !FindSurface(Uniform).IsNone()))
    {
        if (ExistingOverride != Uniform) Component->SetPhysMaterialOverride(Uniform);
        UniformComponents.Add(ComponentKey, Uniform);
    }
    else if (UniformComponents.Remove(ComponentKey)) Component->SetPhysMaterialOverride(nullptr);
}

void UWorldSurfaceSubsystem::ApplyToPrimitive(UPrimitiveComponent* Component)
{
    check(IsInGameThread());
    if (!IsValid(Component)) return;
    if (UMeshComponent* Mesh = Cast<UMeshComponent>(Component)) { ApplyToMesh(Mesh); return; }
    const TWeakObjectPtr<UPrimitiveComponent> Key(Component);
    UPhysicalMaterial* Existing = Component->GetPhysicsMaterialOverride();
    const auto* Owned = UniformComponents.Find(Key);
    if (IsValid(Existing) && (!Owned || Owned->Get() != Existing)) return;
    UPhysicalMaterial* Physical = PhysicalMaterials.FindRef(Config.DefaultPhysicalMaterial);
    if (!IsValid(Physical)) Physical = DefaultPhysicalMaterial;
    if (!IsValid(Physical)) return;
    if (Existing != Physical) Component->SetPhysMaterialOverride(Physical);
    UniformComponents.Add(Key, Physical);
}

UPhysicalMaterial* UWorldSurfaceSubsystem::GetSimplePhysicalMaterial(UPrimitiveComponent* Component) const
{
    // A primitive collider has one material. Use an explicit override/slot before defaults.
    if (IsValid(Component))
    {
        if (UPhysicalMaterial* Override = Component->GetPhysicsMaterialOverride()) return Override;
        if (UMaterialInterface* Material = Component->GetMaterial(0))
            if (UPhysicalMaterial* Physical = Material->GetPhysicalMaterial(); IsValid(Physical)
                && (!GEngine || Physical != GEngine->DefaultPhysMaterial)) return Physical;
    }
    if (UPhysicalMaterial* Physical = PhysicalMaterials.FindRef(Config.DefaultPhysicalMaterial)) return Physical;
    return DefaultPhysicalMaterial;
}

FName UWorldSurfaceSubsystem::FindSurface(const UPhysicalMaterial* Material) const
{
    if (Material)
        for (const auto& Pair : PhysicalMaterials) if (Pair.Value.Get() == Material) return Pair.Key;
    return NAME_None;
}

FName UWorldSurfaceSubsystem::ResolveTerrainLayerSurface(const FString& LayerName) const
{
    if (LayerName.IsEmpty()) return Config.DefaultPhysicalMaterial;

    const FName Direct = Config.ResolveMaterial(LayerName);
    if (!Direct.IsNone() && Direct != Config.DefaultPhysicalMaterial)
    {
        return Direct;
    }

    if (V3DTerrain::IsTerrainName(LayerName))
    {
        const FString Stripped = LayerName.LeftChop(8); // strip ".terrain"
        if (!Stripped.IsEmpty())
        {
            const FName StrippedSurface = Config.ResolveMaterial(Stripped);
            if (!StrippedSurface.IsNone() && StrippedSurface != Config.DefaultPhysicalMaterial)
            {
                return StrippedSurface;
            }
            const FName DirectFootstepName(*Stripped);
            if (Config.FootstepMappings.Contains(DirectFootstepName))
            {
                return DirectFootstepName;
            }
        }
    }

    const FName LayerAsSurface(*LayerName);
    if (Config.FootstepMappings.Contains(LayerAsSurface))
    {
        return LayerAsSurface;
    }

    return Direct;
}

const TArray<FString>* UWorldSurfaceSubsystem::FindTerrainLayersForHit(
    const FHitResult& Hit,
    UMeshComponent* MeshComp,
    UMaterialInterface* HitMaterial) const
{
    if (IsValid(MeshComp))
    {
        if (const TWeakObjectPtr<const UObject>* AssetPtr = TerrainAssetByComponent.Find(MeshComp))
        {
            if (const UObject* Asset = AssetPtr->Get(); IsValid(Asset))
            {
                if (const TArray<FString>* Found = TerrainLayersByAsset.Find(Asset))
                {
                    return Found;
                }
            }
        }
    }

    if (IsValid(HitMaterial))
    {
        if (const UObject* Outer = HitMaterial->GetOuter(); IsValid(Outer))
        {
            if (const TArray<FString>* Found = TerrainLayersByAsset.Find(Outer))
            {
                return Found;
            }
        }
    }

    // Walk owner / attach-parent hierarchy to find AStaticActor -> BakedAsset
    for (AActor* Actor = Hit.GetActor(); IsValid(Actor); )
    {
        if (const AStaticActor* StaticOwner = Cast<AStaticActor>(Actor))
        {
            if (const UWorldBakedModelAsset* Baked = StaticOwner->GetBakedAsset(); IsValid(Baked))
            {
                if (const TArray<FString>* Found = TerrainLayersByAsset.Find(Baked))
                {
                    return Found;
                }
            }
            break;
        }
        AActor* Next = Actor->GetAttachParentActor();
        if (!IsValid(Next)) Next = Actor->GetOwner();
        if (Next == Actor) break;
        Actor = Next;
    }

    return nullptr;
}

void UWorldSurfaceSubsystem::PumpSoundLoads()
{
    constexpr int64 MaxCacheBytes = 8ll * 1024ll * 1024ll;
    constexpr int64 MaxClipBytes = 256ll * 1024ll;
    URuntimeSoundSubsystem* Loader = URuntimeSoundSubsystem::Get(this);
    if (!Loader) return;
    while (InFlight < 2 && !PendingSounds.IsEmpty())
    {
        const FString Reference = PendingSounds.Pop(EAllowShrinking::No);
        const uint64 RequestGeneration = Generation;
        const TWeakObjectPtr<UWorldSurfaceSubsystem> WeakThis(this);
        ++InFlight;
        Loader->LoadPcmAsync(Reference, [WeakThis, RequestGeneration, Reference](FV3DPcmPtr Pcm, const FString& Error)
        {
            UWorldSurfaceSubsystem* Self = WeakThis.Get();
            if (!IsValid(Self) || Self->Generation != RequestGeneration) return;
            --Self->InFlight;
            if (Pcm.IsValid() && Pcm->NumChannels == 1 && Pcm->NumFrames <= Pcm->SampleRate * 2
                && Self->CachedBytes + Pcm->Bytes.Num() <= MaxCacheBytes)
            {
                Self->CachedBytes += Pcm->Bytes.Num();
                Self->PcmCache.Add(Reference, MoveTemp(Pcm));
            }
            else UE_LOG(LogTemp, Warning, TEXT("Footstep preload skipped (mono, <=2 s, bounded PCM required): %s %s"), *Reference, *Error);
            // Deferral also avoids recursive completions when a reference is rejected immediately.
            if (UWorld* World = Self->GetWorld()) World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateWeakLambda(Self, [WeakThis]() { if (auto* Current = WeakThis.Get()) Current->PumpSoundLoads(); }));
        }, MaxClipBytes);
    }
}

bool UWorldSurfaceSubsystem::PlaySurfaceFootstep(
    FName Surface,
    const FVector& Location,
    UObject* Emitter,
    const float VolumeScale,
    const bool bAllowLoopTracking)
{
    const FV3DFootstepDefinition* Definition = Config.FootstepMappings.Find(Surface);
    if (!Definition)
    {
        if (bAllowLoopTracking)
        {
            V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter, FString(), Location);
        }
        return false;
    }

    const float EffectiveVolume = Definition->Volume * FMath::Clamp(VolumeScale, 0.0f, 1.0f);
    if (bAllowLoopTracking)
    {
        const FString LoopKey = Definition->bLoop && EffectiveVolume > 0.0f ? Surface.ToString() : FString();
        if (V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter, LoopKey, Location)) return true;
    }
    if (EffectiveVolume <= 0.01f) return true;

    TArray<int32, TInlineAllocator<16>> Ready;
    for (int32 Index = 0; Index < Definition->Sounds.Num(); ++Index)
        if (PcmCache.Contains(Definition->Sounds[Index])) Ready.Add(Index);
    if (Ready.IsEmpty()) return true; // Never replay a stale step after I/O finishes.

    const int32* Previous = LastSoundIndex.Find(Surface);
    if (Ready.Num() > 1 && Previous) Ready.Remove(*Previous);
    const int32 Index = Ready[FMath::RandHelper(Ready.Num())];
    LastSoundIndex.Add(Surface, Index);
    const FV3DPcmPtr Pcm = PcmCache.FindChecked(Definition->Sounds[Index]);

    ActiveSounds.RemoveAll([](const TObjectPtr<UAudioComponent>& Component) { return !IsValid(Component.Get()) || !Component->IsPlaying(); });
#if PLATFORM_ANDROID || PLATFORM_IOS
    if (ActiveSounds.Num() >= 8) return true;
#else
    if (ActiveSounds.Num() >= 16) return true;
#endif

    const bool bShouldLoop = Definition->bLoop && bAllowLoopTracking;
    USoundWaveProcedural* Sound = V3DSoundPlayback::CreatePcm(this, Pcm, bShouldLoop);
    if (!Sound) return true;

    const float Pitch = FMath::FRandRange(Definition->PitchMin, Definition->PitchMax);
    UAudioComponent* Audio = UGameplayStatics::SpawnSoundAtLocation(this, Sound, Location,
        FRotator::ZeroRotator, EffectiveVolume, Pitch, 0.0f, Attenuation, nullptr, true);
    if (IsValid(Audio))
    {
        ActiveSounds.Add(Audio);
        if (auto* Environment = GetWorld()->GetSubsystem<UWorldEnvironmentSubsystem>()) Environment->ApplySpatialAudio(Audio);
        if (bShouldLoop)
        {
            V3DSoundPlayback::TrackFootstep(FootstepLoops, this, Emitter, Surface.ToString(), Audio, Location);
        }
        else
        {
            V3DSoundPlayback::StopOneShot(this, Audio, Sound->Duration, Pitch);
        }
    }
    return true;
}

bool UWorldSurfaceSubsystem::TryPlayTerrainFootstep(
    const FHitResult& Hit,
    UObject* Emitter,
    UMeshComponent* MeshComp,
    UMaterialInterface* HitMaterial)
{
    UStaticMeshComponent* StaticComp = Cast<UStaticMeshComponent>(MeshComp);
    if (!IsValid(StaticComp)) return false;

    const TArray<FString>* LayerNames = FindTerrainLayersForHit(Hit, MeshComp, HitMaterial);
    if (!LayerNames || LayerNames->IsEmpty()) return false;

    float LayerWeights[4] = {0.f, 0.f, 0.f, 0.f};
    if (!WorldSurfaceTerrainPrivate::SampleTerrainVertexWeights(Hit, StaticComp, LayerWeights))
    {
        return false;
    }

    // Aggregate blend weights per resolved surface (in case multiple layers share a surface)
    TMap<FName, float> SurfaceWeights;
    const int32 LayerCount = FMath::Min(LayerNames->Num(), 4);
    for (int32 LayerIdx = 0; LayerIdx < LayerCount; ++LayerIdx)
    {
        const float Weight = LayerWeights[LayerIdx];
        if (Weight <= 0.02f) continue;

        const FName LayerSurface = ResolveTerrainLayerSurface((*LayerNames)[LayerIdx]);
        if (LayerSurface.IsNone() || !Config.FootstepMappings.Contains(LayerSurface)) continue;

        SurfaceWeights.FindOrAdd(LayerSurface) += Weight;
    }

    if (SurfaceWeights.IsEmpty()) return false;

    TArray<TPair<FName, float>> Ranked;
    Ranked.Reserve(SurfaceWeights.Num());
    for (const TPair<FName, float>& Pair : SurfaceWeights)
    {
        Ranked.Add(Pair);
    }
    Ranked.Sort([](const TPair<FName, float>& A, const TPair<FName, float>& B)
    {
        return A.Value > B.Value;
    });

    // Single dominant texture or negligible secondary blend
    if (Ranked.Num() == 1 || Ranked[1].Value < 0.05f * (Ranked[0].Value + Ranked[1].Value))
    {
        return PlaySurfaceFootstep(Ranked[0].Key, Hit.ImpactPoint, Emitter, 1.0f, true);
    }

    // Blend zone: mix the top two terrain texture footsteps using equal-power crossfade
    const float SumTopTwo = Ranked[0].Value + Ranked[1].Value;
    const float RatioA = FMath::Clamp(Ranked[0].Value / SumTopTwo, 0.0f, 1.0f);
    const float RatioB = FMath::Clamp(Ranked[1].Value / SumTopTwo, 0.0f, 1.0f);
    const float GainA = FMath::Sqrt(RatioA);
    const float GainB = FMath::Sqrt(RatioB);

    const bool bPlayedA = PlaySurfaceFootstep(Ranked[0].Key, Hit.ImpactPoint, Emitter, GainA, true);
    const bool bPlayedB = PlaySurfaceFootstep(Ranked[1].Key, Hit.ImpactPoint, Emitter, GainB, false);
    return bPlayedA || bPlayedB;
}

bool UWorldSurfaceSubsystem::PlayFootstep(const FHitResult& Hit, UObject* Emitter)
{
    check(IsInGameThread());
    if (!Hit.bBlockingHit || GetWorld()->GetNetMode() == NM_DedicatedServer) return false;

    UPrimitiveComponent* Component = Hit.GetComponent();
    UMeshComponent* MeshComp = Cast<UMeshComponent>(Component);
    UMaterialInterface* HitMaterial = nullptr;
    if (IsValid(Component))
    {
        int32 Section = INDEX_NONE;
        if (Hit.FaceIndex != INDEX_NONE)
        {
            HitMaterial = Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section);
        }
        if (!IsValid(HitMaterial))
        {
            HitMaterial = Component->GetMaterial(0);
        }
    }

    FName Surface = FindSurface(Hit.PhysMaterial.Get());
    if (Surface.IsNone() && IsValid(HitMaterial))
    {
        Surface = FindSurface(HitMaterial->GetPhysicalMaterial());
    }

    const bool bIsTerrainHit = WorldSurfaceTerrainPrivate::IsTerrainPaletteMaterial(HitMaterial)
        || (IsValid(MeshComp) && TerrainAssetByComponent.Contains(MeshComp))
        || (!Surface.IsNone() && Surface.ToString().Contains(TEXT("Terrain"), ESearchCase::IgnoreCase))
        || (Hit.PhysMaterial.IsValid() && Hit.PhysMaterial->GetName().Contains(TEXT("Terrain"), ESearchCase::IgnoreCase));

    if (bIsTerrainHit && TryPlayTerrainFootstep(Hit, Emitter, MeshComp, HitMaterial))
    {
        return true;
    }

    if (Surface.IsNone()) Surface = Config.DefaultPhysicalMaterial;
    return PlaySurfaceFootstep(Surface, Hit.ImpactPoint, Emitter, 1.0f, true);
}
