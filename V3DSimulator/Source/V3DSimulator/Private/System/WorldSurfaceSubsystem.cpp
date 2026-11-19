// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldSurfaceSubsystem.h"
#include "System/SoundEventSubsystem.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "System/WorldArchive.h"
#include "World/WorldEnvironmentSubsystem.h"
#include "TimerManager.h"

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
        const FName Surface = Config.ResolveMaterial(Slots.IsValidIndex(Index) ? Slots[Index].ToString() : FString());
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

bool UWorldSurfaceSubsystem::PlayFootstep(const FHitResult& Hit, UObject* Emitter)
{
    check(IsInGameThread());
    if (!Hit.bBlockingHit || GetWorld()->GetNetMode() == NM_DedicatedServer) return false;
    FName Surface = FindSurface(Hit.PhysMaterial.Get());
    if (Surface.IsNone() && Hit.FaceIndex != INDEX_NONE)
        if (UPrimitiveComponent* Component = Hit.GetComponent())
        {
            int32 Section = INDEX_NONE;
            if (UMaterialInterface* Material = Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section))
                Surface = FindSurface(Material->GetPhysicalMaterial());
        }
    if (Surface.IsNone()) Surface = Config.DefaultPhysicalMaterial;
    const FV3DFootstepDefinition* Definition = Config.FootstepMappings.Find(Surface);
    if (!Definition)
    {
        V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter, FString(), Hit.ImpactPoint);
        return false;
    }
    const FString LoopKey = Definition->bLoop && Definition->Volume > 0 ? Surface.ToString() : FString();
    if (V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter, LoopKey, Hit.ImpactPoint)) return true;
    if (Definition->Volume <= 0) return true;
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
    USoundWaveProcedural* Sound = V3DSoundPlayback::CreatePcm(this, Pcm, Definition->bLoop);
    if (!Sound) return true;
    const float Pitch = FMath::FRandRange(Definition->PitchMin, Definition->PitchMax);
    UAudioComponent* Audio = UGameplayStatics::SpawnSoundAtLocation(this, Sound, Hit.ImpactPoint,
        FRotator::ZeroRotator, Definition->Volume, Pitch, 0.0f, Attenuation, nullptr, true);
    if (IsValid(Audio))
    {
        ActiveSounds.Add(Audio);
        if (auto* Environment = GetWorld()->GetSubsystem<UWorldEnvironmentSubsystem>()) Environment->ApplySpatialAudio(Audio);
        if (Definition->bLoop) V3DSoundPlayback::TrackFootstep(FootstepLoops, this, Emitter, LoopKey, Audio, Hit.ImpactPoint);
        else V3DSoundPlayback::StopOneShot(this, Audio, Sound->Duration, Pitch);
    }
    return true;
}
