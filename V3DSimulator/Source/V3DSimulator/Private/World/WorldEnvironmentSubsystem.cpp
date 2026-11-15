// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "World/WorldEnvironmentSubsystem.h"
#include "World/EnvironmentAcousticsMath.h"
#include "World/WaterQuerySubsystem.h"
#include "System/V3DSimulatorAssetRegistry.h"
#include "System/V3DSimulatorGameInstance.h"
#include "System/SimulationScriptSubsystem.h"
#include "System/GameManagerSubSystem.h"
#include "NiagaraSystem.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/ReverbEffect.h"
#include "Kismet/GameplayStatics.h"

void UWorldEnvironmentSubsystem::Reset()
{
    ++Generation;
    if (AssetLoad) AssetLoad->CancelHandle();
    AssetLoad.Reset();
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(Timer);
    for (auto* Audio : {WaterAudio.Get(), UnderwaterAudio.Get(), MusicAudio.Get()}) if (IsValid(Audio)) Audio->Stop();
    for (auto& Audio : Effects) if (IsValid(Audio)) Audio->Stop();
    for (auto& Audio : FadingMusic) if (IsValid(Audio)) Audio->Stop();
    UGameplayStatics::DeactivateReverbEffect(this, TEXT("V3DGeometry"));
    WaterAudio = UnderwaterAudio = MusicAudio = nullptr;
    Effects.Empty(); FadingMusic.Empty(); Defaults.Empty(); LoadedAssets.Empty();
    Pcm.Empty(); Loading.Empty(); Failed.Empty(); CacheOrder.Empty(); CachedBytes = 0;
    Inside.Empty(); ActiveBiome = NAME_None; MusicKey.Reset(); bAssetsReady = false;
    LastReverbBucket = -1; LastWet = -1; SpatialAttenuation = nullptr; RoomReverb = nullptr;
}
void UWorldEnvironmentSubsystem::Deinitialize() { Reset(); Super::Deinitialize(); }

bool UWorldEnvironmentSubsystem::Configure(const TSharedPtr<FJsonObject>& Root, FString& Error)
{
    FV3DEnvironmentConfig Parsed;
    if (!FV3DEnvironmentConfig::Parse(Root, Parsed, Error)) return false;
    Reset(); Config = MoveTemp(Parsed);
    SpatialAttenuation = NewObject<USoundAttenuation>(this);
    auto& A = SpatialAttenuation->Attenuation;
    A.bAttenuate = true; A.bSpatialize = true;
    A.AttenuationShapeExtents = FVector(100); A.FalloffDistance = 1800;
    A.bEnableOcclusion = Config.Audio.bGlobalAcoustics;
    A.OcclusionTraceChannel = ECC_Visibility;
    A.OcclusionLowPassFilterFrequency = 1200;
    A.OcclusionVolumeAttenuation = .35f;
    A.OcclusionInterpolationTime = .2f;
    A.bEnableReverbSend = Config.Audio.bGlobalAcoustics;
    RoomReverb = NewObject<UReverbEffect>(this);
    if (GetWorld()->GetNetMode() != NM_DedicatedServer)
    {
        if (auto* Registry = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
        {
            TArray<FSoftObjectPath> Paths;
            for (const auto& Soft : {Registry->DefaultFootstepSound, Registry->DefaultWaterSound,
                Registry->DefaultUnderwaterSound, Registry->DefaultEnvironmentMusic})
                if (!Soft.IsNull()) Paths.AddUnique(Soft.ToSoftObjectPath());
            for (const auto& Binding : Registry->FootstepBindings)
            {
                if (!Binding.Sound.IsNull()) Paths.AddUnique(Binding.Sound.ToSoftObjectPath());
                if (!Binding.Effect.IsNull()) Paths.AddUnique(Binding.Effect.ToSoftObjectPath());
            }
            const uint64 Token = Generation;
            auto Ready = [this, Token, Paths]()
            {
                if (Token != Generation) return;
                for (const auto& Path : Paths) if (auto* Object = Path.ResolveObject()) LoadedAssets.Add(Object);
                if (auto* R = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
                {
                    Defaults.Add(TEXT("Footstep"), R->DefaultFootstepSound.Get());
                    Defaults.Add(TEXT("Water"), R->DefaultWaterSound.Get());
                    Defaults.Add(TEXT("Underwater"), R->DefaultUnderwaterSound.Get());
                    Defaults.Add(TEXT("Music"), R->DefaultEnvironmentMusic.Get());
                }
                bAssetsReady = true;
            };
            if (Paths.IsEmpty()) Ready();
            else
            {
                AssetLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths,
                    FStreamableDelegate::CreateWeakLambda(this, MoveTemp(Ready)));
                if (!AssetLoad) bAssetsReady = true; // Archive overrides remain usable without cooked defaults.
            }
        }
        else bAssetsReady = true;
        RequestPcm(Config.Audio.DefaultFootstep);
        RequestPcm(Config.Audio.Water); RequestPcm(Config.Audio.Underwater); RequestPcm(Config.Audio.EnvironmentMusic);
    }
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr float Interval = .4f;
#else
    constexpr float Interval = .2f;
#endif
    GetWorld()->GetTimerManager().SetTimer(Timer, this, &UWorldEnvironmentSubsystem::Poll, Interval, true);
    return true;
}

bool UWorldEnvironmentSubsystem::IsInBiome(FName Name) const { return Inside.Contains(Name); }
bool UWorldEnvironmentSubsystem::IsPointInBiome(FName Name, FVector Point) const
{
    for (const auto& Biome : Config.Biomes) if (Biome.Name == Name) return Biome.Contains(Point);
    return false;
}

void UWorldEnvironmentSubsystem::RequestPcm(const FString& Reference)
{
    if (Reference.IsEmpty() || Reference == TEXT("none") || Pcm.Contains(Reference) || Loading.Contains(Reference) || Failed.Contains(Reference)) return;
    if (Loading.Num() >= 2) return; // Poll retries keys deferred by this bounded queue.
    auto* Loader = URuntimeSoundSubsystem::Get(this);
    if (!Loader) return;
    Loading.Add(Reference);
    const uint64 Token = Generation;
    TWeakObjectPtr<UWorldEnvironmentSubsystem> Weak(this);
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr int64 SoundLimit = 8ll * 1024 * 1024;
#else
    constexpr int64 SoundLimit = 16ll * 1024 * 1024;
#endif
    Loader->LoadPcmAsync(Reference, [Weak, Token, Reference](FV3DPcmPtr Result, const FString& Error)
    {
        auto* Self = Weak.Get(); if (!Self || Self->Generation != Token) return;
        Self->Loading.Remove(Reference);
        if (!Result.IsValid())
        {
            Self->Failed.Add(Reference);
            UE_LOG(LogTemp, Warning, TEXT("Environment audio fallback: %s (%s)"), *Reference, *Error);
            return;
        }
        if (Reference == Self->Config.Audio.DefaultFootstep && (Result->NumChannels != 1 || Result->NumFrames > Result->SampleRate * 2))
        { Self->Failed.Add(Reference); return; }
#if PLATFORM_ANDROID || PLATFORM_IOS
        constexpr int64 Budget = 16ll * 1024 * 1024;
#else
        constexpr int64 Budget = 32ll * 1024 * 1024;
#endif
        while (Self->CachedBytes + Result->Bytes.Num() > Budget && !Self->CacheOrder.IsEmpty())
        {
            const FString Old = Self->CacheOrder[0]; Self->CacheOrder.RemoveAt(0);
            if (auto* Entry = Self->Pcm.Find(Old)) Self->CachedBytes -= (*Entry)->Bytes.Num();
            Self->Pcm.Remove(Old);
        }
        Self->CachedBytes += Result->Bytes.Num();
        Self->Pcm.Add(Reference, Result); Self->CacheOrder.Add(Reference);
    }, SoundLimit);
}

USoundBase* UWorldEnvironmentSubsystem::ResolveSound(const FString& Reference, const FString& DefaultKey, bool bLoop)
{
    if (Reference == TEXT("none")) return nullptr;
    if (!Reference.IsEmpty())
    {
        RequestPcm(Reference);
        if (const auto* Found = Pcm.Find(Reference))
        {
            const FV3DPcmPtr Data = *Found;
            auto* Wave = NewObject<USoundWaveProcedural>(this);
            Wave->NumChannels = Data->NumChannels; Wave->SetSampleRate(Data->SampleRate);
            Wave->SetNumFrames(Data->NumFrames); Wave->SampleByteSize = 2;
            Wave->Duration = bLoop ? INDEFINITELY_LOOPING_DURATION : float(Data->NumFrames) / Data->SampleRate;
            if (bLoop)
            {
                // Audio-thread callback owns only immutable PCM and its own cursor, never the subsystem.
                Wave->OnSoundWaveProceduralUnderflow.BindLambda([Data, Cursor = int32(0)](USoundWaveProcedural* W, int32 Samples) mutable
                {
                    int32 Remaining = FMath::Clamp(Samples, 1, 32768) * 2;
                    while (Remaining > 0)
                    {
                        const int32 Count = FMath::Min(Remaining, Data->Bytes.Num() - Cursor);
                        W->QueueAudio(Data->Bytes.GetData() + Cursor, Count);
                        Cursor = (Cursor + Count) % Data->Bytes.Num(); Remaining -= Count;
                    }
                });
            }
            else Wave->QueueAudio(Data->Bytes.GetData(), Data->Bytes.Num());
            return Wave;
        }
        if (!Failed.Contains(Reference)) return nullptr;
    }
    const auto* FoundDefault = Defaults.Find(DefaultKey);
    return FoundDefault ? FoundDefault->Get() : nullptr;
}

void UWorldEnvironmentSubsystem::UpdateLoop(TObjectPtr<UAudioComponent>& Audio, const FString& Reference,
    const FString& Key, float Volume)
{
    if (IsValid(Audio) && !Audio->IsPlaying()) Audio = nullptr;
    if (Audio && Volume <= .001f)
    {
        Audio->FadeOut(.25f, 0); FadingMusic.Add(Audio); Audio = nullptr;
        return;
    }
    if (!Audio && Volume > .001f)
        if (auto* Sound = ResolveSound(Reference, Key, true))
        {
            Audio = UGameplayStatics::CreateSound2D(this, Sound, Volume, 1, 0, nullptr, false, true);
            if (Audio) Audio->FadeIn(.4f, Volume);
        }
    if (Audio) Audio->AdjustVolume(.25f, Volume, EAudioFaderCurve::Linear);
}

void UWorldEnvironmentSubsystem::Poll()
{
    if (const auto* Manager = UGameManagerSubSystem::GetSubSystem(this); Manager && Manager->IsWorldLoading()) return;
    auto* PC = GetWorld()->GetFirstPlayerController();
    if (!PC || !PC->GetPawn()) return;
    const uint64 Token = Generation;
    const FVector Position = PC->GetPawn()->GetActorLocation();
    TSet<FName> NewInside;
    FString DesiredMusic = Config.Audio.EnvironmentMusic;
    ActiveBiome = NAME_None;
    bool bChosenMusic = false;
    for (const auto& Biome : Config.Biomes)
    {
        const bool bHere = Biome.Contains(Position);
        if (bHere)
        {
            NewInside.Add(Biome.Name);
            if (ActiveBiome.IsNone()) ActiveBiome = Biome.Name;
            if (!bChosenMusic && !Biome.Music.IsEmpty()) { DesiredMusic = Biome.Music; bChosenMusic = true; }
        }
    }
    // Publish variables before commands/events, so conditions see the new state.
    const TSet<FName> Previous = MoveTemp(Inside); Inside = MoveTemp(NewInside);
    struct FTransition { FName Name; bool bInside; FName Command; };
    TArray<FTransition, TInlineAllocator<16>> Transitions;
    for (const auto& Biome : Config.Biomes)
    {
        const bool bHere = Inside.Contains(Biome.Name);
        if (bHere != Previous.Contains(Biome.Name)) Transitions.Add({Biome.Name, bHere, bHere ? Biome.EnterCommand : Biome.ExitCommand});
    }
    for (const auto& Event : Transitions)
    {
        OnBiomeChanged.Broadcast(Event.Name, Event.bInside);
        if (Token != Generation) return;
        const FName Command = Event.Command;
        if (!Command.IsNone() && GetWorld()->GetNetMode() != NM_Client)
            if (auto* Scripts = GetWorld()->GetSubsystem<USimulationScriptSubsystem>())
            {
                FString Message;
                if (!Scripts->ExecuteNamed(PC, Command, Message, false))
                    UE_LOG(LogTemp, Warning, TEXT("Biome %s: %s"), *Event.Name.ToString(), *Message);
            }
        if (Token != Generation) return;
    }
    if (GetWorld()->GetNetMode() == NM_DedicatedServer || !bAssetsReady || !PC->PlayerCameraManager) return;
    const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
    FWaterQueryResult Wet, Near;
    if (auto* Water = UWaterQuerySubsystem::Get(this))
    {
        Water->QueryWaterAtLocation(Eye, EWaterQueryMode::Strict, EWaterQueryPurpose::Interaction, Wet);
        Water->QueryWaterAtLocation(Eye - FVector(0,0,250), EWaterQueryMode::Strict, EWaterQueryPurpose::Interaction, Near);
    }
    UpdateLoop(WaterAudio, Config.Audio.Water, TEXT("Water"), Near.bInWater && !Wet.bInWater ? .5f : 0);
    UpdateLoop(UnderwaterAudio, Config.Audio.Underwater, TEXT("Underwater"), Wet.bInWater ? .7f : 0);
    if (!Config.Audio.bEnvironmentMusic) DesiredMusic = TEXT("none");
    FadingMusic.RemoveAll([](const auto& C) { return !IsValid(C) || !C->IsPlaying(); });
    if (MusicKey != DesiredMusic)
    {
        MusicKey = DesiredMusic;
        if (MusicAudio)
        {
            MusicAudio->FadeOut(.6f, 0); FadingMusic.Add(MusicAudio); MusicAudio = nullptr;
            if (FadingMusic.Num() > 2) { if (FadingMusic[0]) FadingMusic[0]->Stop(); FadingMusic.RemoveAt(0); }
        }
    }
    UpdateLoop(MusicAudio, MusicKey, TEXT("Music"), Config.Audio.MusicVolume);
    if (MusicAudio) { MusicAudio->SetLowPassFilterEnabled(Wet.bInWater); MusicAudio->SetLowPassFilterFrequency(1200); }
    if (Config.Audio.bGlobalAcoustics) ProbeRoom(Eye, PC->GetPawn());
}

void UWorldEnvironmentSubsystem::ProbeRoom(const FVector& Eye, AActor* Pawn)
{
    const FVector Directions[] = {FVector(1,0,0),FVector(-1,0,0),FVector(0,1,0),FVector(0,-1,0),FVector(0,0,1),FVector(0,0,-1)};
    float Distances[6]; uint32 HitMask = 0;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(V3DAcoustics), false, Pawn);
    for (int32 I = 0; I < 6; ++I)
    {
        FHitResult Hit;
        const bool Blocked = GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Directions[I] * Config.Audio.ProbeDistanceCm, ECC_Visibility, Params);
        Distances[I] = Blocked ? Hit.Distance * .01f : Config.Audio.ProbeDistanceCm * .01f;
        if (Blocked) HitMask |= 1u << I;
    }
    const auto Estimate = V3DAcousticsMath::Estimate(Distances, HitMask, Config.Audio.Absorption);
    const int32 Bucket = FMath::RoundToInt(Estimate.DecaySeconds * 4);
    const float Wet = Estimate.Wet;
    if (Bucket == LastReverbBucket && FMath::IsNearlyEqual(Wet, LastWet)) return;
    LastReverbBucket = Bucket; LastWet = Wet;
    if (Wet <= 0) { UGameplayStatics::DeactivateReverbEffect(this, TEXT("V3DGeometry")); return; }
    RoomReverb->DecayTime = FMath::Max(.1f, Bucket * .25f);
    RoomReverb->Density = .8f; RoomReverb->Diffusion = .8f;
    UGameplayStatics::ActivateReverbEffect(this, RoomReverb, TEXT("V3DGeometry"), 1.f, Wet, .4f);
}

void UWorldEnvironmentSubsystem::ApplySpatialAudio(UAudioComponent* Audio) const
{
    if (Audio && SpatialAttenuation) Audio->AdjustAttenuation(SpatialAttenuation->Attenuation);
}
void UWorldEnvironmentSubsystem::PlayDefaultFootstep(const FVector& Location)
{
    if (GetWorld()->GetNetMode() == NM_DedicatedServer) return;
    Effects.RemoveAll([](const auto& C) { return !IsValid(C) || !C->IsPlaying(); });
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr int32 MaxVoices = 8;
#else
    constexpr int32 MaxVoices = 16;
#endif
    if (Effects.Num() >= MaxVoices) return;
    if (auto* Sound = ResolveSound(Config.Audio.DefaultFootstep, TEXT("Footstep"), false))
    {
        auto* Audio = UGameplayStatics::SpawnSoundAtLocation(this, Sound, Location, FRotator::ZeroRotator, .7f, 1.f, 0, SpatialAttenuation);
        if (!Audio) return;
        Effects.Add(Audio);
        if (Cast<USoundWaveProcedural>(Sound))
        {
            FTimerHandle Stop; TWeakObjectPtr<UAudioComponent> Weak(Audio);
            GetWorld()->GetTimerManager().SetTimer(Stop, FTimerDelegate::CreateWeakLambda(this,
                [Weak]() { if (auto* A = Weak.Get()) A->Stop(); }), Sound->Duration + .1f, false);
        }
    }
}
