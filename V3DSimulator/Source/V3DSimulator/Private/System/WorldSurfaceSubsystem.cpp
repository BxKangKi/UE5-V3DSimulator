// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldSurfaceSubsystem.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "System/WorldArchive.h"
#include "TimerManager.h"

UWorldSurfaceSubsystem* UWorldSurfaceSubsystem::Get(const UObject* Context)
{
    UWorld* World = IsValid(Context) ? Context->GetWorld() : nullptr;
    return World ? World->GetSubsystem<UWorldSurfaceSubsystem>() : nullptr;
}

void UWorldSurfaceSubsystem::Reset()
{
    check(IsInGameThread());
    ++Generation;
    if (UWorld* World = GetWorld()) World->GetTimerManager().ClearAllTimersForObject(this);
    for (UAudioComponent* Sound : ActiveSounds) if (IsValid(Sound)) Sound->Stop();
    ActiveSounds.Empty();
    PendingSounds.Empty();
    PcmCache.Empty();
    MaterialCache.Empty();
    PhysicalMaterials.Empty();
    LastSoundIndex.Empty();
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
    UGameInstance* Instance = GetWorld()->GetGameInstance();
    UModelDatabaseSubsystem* Database = Instance ? Instance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    TArray<FSoundDefinition> Sounds;
    if (Database) Database->GetSoundDefinitions(Sounds);
    TSet<FGuid> Available;
    for (const auto& Sound : Sounds) Available.Add(Sound.UUID);
    if (!Parsed.ValidateSounds(Available, Error)) return false;
    Reset();
    Config = MoveTemp(Parsed);
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
    check(IsInGameThread());
    if (!IsValid(Component) || PhysicalMaterials.IsEmpty()) return;
    // Weak cached wrappers share unchanged shader/texture parameters without editing a shared
    // facade material (facades can be shared across worlds, including multiple PIE sessions).
    if ((++MaterialApplications % 128) == 0)
    {
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
        if (IsValid(Base) && IsValid(Physical))
        {
            auto& Cache = MaterialCache.FindOrAdd(TWeakObjectPtr<UMaterialInterface>(Base));
            UMaterialInstanceDynamic* Material = Cache.FindRef(Surface).Get();
            if (!IsValid(Material))
            {
                UMaterialInterface* Parent = Base;
                while (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Parent))
                    Parent = Dynamic->Parent;
                if (!IsValid(Parent)) continue;
                Material = UMaterialInstanceDynamic::Create(Parent, this);
                Material->CopyMaterialUniformParameters(Base);
                Material->PhysMaterial = Physical;
                Material->bOverridePhysMaterial = true; // UE 5.8
                Cache.Add(Surface, Material);
            }
            Component->SetMaterial(Index, Material);
        }
        else Physical = Base ? Base->GetPhysicalMaterial() : nullptr;
        if (Index == 0) Uniform = Physical;
        else bUniform &= Uniform == Physical;
    }
    // A component override would erase all per-triangle materials on a mixed-material mesh.
    if (bUniform && !FindSurface(Uniform).IsNone()) Component->SetPhysMaterialOverride(Uniform);
}

UPhysicalMaterial* UWorldSurfaceSubsystem::GetSimplePhysicalMaterial(UPrimitiveComponent* Component) const
{
    // A primitive collider has only one material: use slot 0, then the explicit world default.
    if (IsValid(Component))
        if (UMaterialInterface* Material = Component->GetMaterial(0))
            if (UPhysicalMaterial* Physical = Material->GetPhysicalMaterial(); !FindSurface(Physical).IsNone()) return Physical;
    return PhysicalMaterials.FindRef(Config.DefaultPhysicalMaterial);
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

bool UWorldSurfaceSubsystem::PlayFootstep(const FHitResult& Hit)
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
    const FV3DFootstepDefinition* Definition = Config.FootstepMappings.Find(Surface);
    if (!Definition) return false;
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
    if (ActiveSounds.Num() >= 16) return true;
    USoundWaveProcedural* Sound = NewObject<USoundWaveProcedural>(this, NAME_None, RF_Transient);
    Sound->NumChannels = Pcm->NumChannels;
    Sound->SetSampleRate(Pcm->SampleRate);
    Sound->SetNumFrames(Pcm->NumFrames);
    Sound->SampleByteSize = 2;
    Sound->Duration = static_cast<float>(Pcm->NumFrames) / Pcm->SampleRate;
    Sound->QueueAudio(Pcm->Bytes.GetData(), Pcm->Bytes.Num());
    const float Pitch = FMath::FRandRange(Definition->PitchMin, Definition->PitchMax);
    UAudioComponent* Audio = UGameplayStatics::SpawnSoundAtLocation(this, Sound, Hit.ImpactPoint,
        FRotator::ZeroRotator, Definition->Volume, Pitch, 0.0f, Attenuation, nullptr, true);
    if (IsValid(Audio))
    {
        ActiveSounds.Add(Audio);
        // Procedural sources can keep asking for silence after their queue ends. Explicitly
        // stop at the end; use one independent queue per voice so rapid steps cannot consume it.
        FTimerHandle StopTimer;
        const TWeakObjectPtr<UAudioComponent> WeakAudio(Audio);
        GetWorld()->GetTimerManager().SetTimer(StopTimer, FTimerDelegate::CreateWeakLambda(this,
            [WeakAudio]() { if (auto* Component = WeakAudio.Get()) Component->Stop(); }), Sound->Duration / Pitch + 0.1f, false);
    }
    return true;
}
