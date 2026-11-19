// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "World/WorldEnvironmentSubsystem.h"
#include "System/SoundEventSubsystem.h"
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
    if (GetWorld()) GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
    for (auto* Audio : {WaterAudio.Get(), UnderwaterAudio.Get(), MusicAudio.Get()}) if (IsValid(Audio)) Audio->Stop();
    for (auto& Audio : Effects) if (IsValid(Audio)) Audio->Stop();
    for (auto& Audio : FadingMusic) if (IsValid(Audio)) Audio->Stop();
    UGameplayStatics::DeactivateReverbEffect(this, TEXT("V3DGeometry"));
    WaterAudio = UnderwaterAudio = MusicAudio = nullptr;
    Effects.Empty(); FadingMusic.Empty(); Defaults.Empty(); LoadedAssets.Empty();
    EventChoices.Empty(); StartedChannels.Empty(); FootstepLoops.Empty(); MusicContext.Reset();
    Pcm.Empty(); Loading.Empty(); Failed.Empty(); CacheOrder.Empty(); CachedBytes = 0;
    Inside.Empty(); ActiveBiome = NAME_None; MusicKey.Reset(); bAssetsReady = false;
    LastReverbBucket = -1; LastWet = -1; SpatialAttenuation = nullptr; RoomReverb = nullptr;
}
void UWorldEnvironmentSubsystem::Deinitialize() { Reset(); Super::Deinitialize(); }

bool UWorldEnvironmentSubsystem::Configure(const TSharedPtr<FJsonObject>& Root, FString& Error)
{
    FV3DEnvironmentConfig Parsed;
    if (!FV3DEnvironmentConfig::Parse(Root, Parsed, Error)) return false;
    auto* Events = GetWorld()->GetSubsystem<USoundEventSubsystem>();
    auto ValidateEvent = [Events, &Error](const FString& Reference)
    {
        if (Reference.StartsWith(TEXT("event://")) && !Events->HasEvent(FName(Reference.Mid(8))))
        { Error = TEXT("Unknown audio event: ") + Reference; return false; }
        return true;
    };
    for (const FString* Reference : {&Parsed.Audio.DefaultFootstep, &Parsed.Audio.Water, &Parsed.Audio.Underwater, &Parsed.Audio.EnvironmentMusic})
        if (!ValidateEvent(*Reference)) return false;
    for (const auto& Biome : Parsed.Biomes) if (!ValidateEvent(Biome.Music)) return false;
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
            for (const auto* Settings : {&Registry->AudioDefaults.Footstep, &Registry->AudioDefaults.Water,
                &Registry->AudioDefaults.Underwater, &Registry->AudioDefaults.EnvironmentMusic})
                for (const auto& Soft : Settings->Sounds) if (!Soft.IsNull()) Paths.AddUnique(Soft.ToSoftObjectPath());
            for (const auto& Binding : Registry->AudioDefaults.Footsteps)
            {
                for (const auto& Sound : Binding.Sounds) if (!Sound.IsNull()) Paths.AddUnique(Sound.ToSoftObjectPath());
                if (!Binding.Effect.IsNull()) Paths.AddUnique(Binding.Effect.ToSoftObjectPath());
            }
            const uint64 Token = Generation;
            auto Ready = [this, Token, Paths]()
            {
                if (Token != Generation) return;
                for (const auto& Path : Paths) if (auto* Object = Path.ResolveObject()) LoadedAssets.Add(Object);
                if (auto* R = UV3DSimulatorGameInstance::GetAssetRegistryFromContext(this))
                {
                    auto Add = [this](const TCHAR* Key, const FV3DDefaultSoundSettings& Settings)
                    {
                        auto& List = Defaults.FindOrAdd(Key); List.bLoop = Settings.bLoop;
                        for (const auto& Source : Settings.Sounds) if (Source.Get()) List.Sounds.Add(Source.Get());
                    };
                    Add(TEXT("Footstep"), R->AudioDefaults.Footstep);
                    Add(TEXT("Water"), R->AudioDefaults.Water);
                    Add(TEXT("Underwater"), R->AudioDefaults.Underwater);
                    Add(TEXT("Music"), R->AudioDefaults.EnvironmentMusic);
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
    if (Reference.StartsWith(TEXT("event://"))) return;
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
    if (Reference.StartsWith(TEXT("event://")))
    {
        auto* Events = GetWorld()->GetSubsystem<USoundEventSubsystem>();
        FString& Selected = EventChoices.FindOrAdd(Reference);
        if (Selected.IsEmpty()) Selected = Events->PickReference(FName(Reference.Mid(8)));
        if (Selected.IsEmpty()) return nullptr;
        auto* Sound = ResolveSound(Selected, DefaultKey, bLoop);
        if (Sound) EventChoices.Remove(Reference);
        return Sound;
    }
    if (!Reference.IsEmpty())
    {
        RequestPcm(Reference);
        if (const auto* Found = Pcm.Find(Reference))
        {
            return V3DSoundPlayback::CreatePcm(this, *Found, bLoop);
        }
        if (!Failed.Contains(Reference)) return nullptr;
    }
    const auto* FoundDefault = Defaults.Find(DefaultKey);
    return FoundDefault && !FoundDefault->Sounds.IsEmpty()
        ? V3DSoundPlayback::CreateAsset(this, FoundDefault->Sounds[FMath::RandHelper(FoundDefault->Sounds.Num())].Get(), bLoop) : nullptr;
}

bool UWorldEnvironmentSubsystem::ResolveLoop(const FString& Reference, const FString& Key, TOptional<bool> Override) const
{
    if (Override.IsSet()) return Override.GetValue();
    if (Reference.StartsWith(TEXT("event://")))
        return GetWorld()->GetSubsystem<USoundEventSubsystem>()->IsLooping(FName(Reference.Mid(8)));
    if (const auto* Default = Defaults.Find(Key)) return Default->bLoop;
    return Key != TEXT("Footstep");
}

void UWorldEnvironmentSubsystem::UpdateLoop(TObjectPtr<UAudioComponent>& Audio, const FString& Reference,
    const FString& Key, float Volume, TOptional<bool> LoopOverride)
{
    if (IsValid(Audio) && !Audio->IsPlaying()) Audio = nullptr;
    if (Volume <= .001f || Reference == TEXT("none"))
    {
        StartedChannels.Remove(Key); // A later activation may play a non-looping sound once again.
        if (Audio) { Audio->FadeOut(.25f, 0); FadingMusic.Add(Audio); Audio = nullptr; }
        return;
    }
    const bool bLoop = ResolveLoop(Reference, Key, LoopOverride);
    if (!Audio && (bLoop || !StartedChannels.Contains(Key)))
        if (auto* Sound = ResolveSound(Reference, Key, bLoop))
        {
            Audio = UGameplayStatics::CreateSound2D(this, Sound, Volume, 1, 0, nullptr, false, true);
            if (Audio)
            {
                StartedChannels.Add(Key); Audio->FadeIn(.4f, Volume);
                if (!bLoop && Cast<USoundWaveProcedural>(Sound)) V3DSoundPlayback::StopOneShot(this, Audio, Sound->Duration, 1.f);
            }
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
    FString DesiredContext = TEXT("Environment");
    TOptional<bool> MusicLoop = Config.Audio.EnvironmentMusicLoop;
    ActiveBiome = NAME_None;
    bool bChosenMusic = false;
    for (const auto& Biome : Config.Biomes)
    {
        const bool bHere = Biome.Contains(Position);
        if (bHere)
        {
            NewInside.Add(Biome.Name);
            if (ActiveBiome.IsNone()) ActiveBiome = Biome.Name;
            if (!bChosenMusic && !Biome.Music.IsEmpty())
            {
                DesiredMusic = Biome.Music; DesiredContext = Biome.Name.ToString();
                MusicLoop = Biome.MusicLoop; bChosenMusic = true;
            }
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
    UpdateLoop(WaterAudio, Config.Audio.Water, TEXT("Water"), Near.bInWater && !Wet.bInWater ? .5f : 0, Config.Audio.WaterLoop);
    UpdateLoop(UnderwaterAudio, Config.Audio.Underwater, TEXT("Underwater"), Wet.bInWater ? .7f : 0, Config.Audio.UnderwaterLoop);
    if (!Config.Audio.bEnvironmentMusic) DesiredMusic = TEXT("none");
    FadingMusic.RemoveAll([](const auto& C) { return !IsValid(C) || !C->IsPlaying(); });
    if (MusicKey != DesiredMusic || MusicContext != DesiredContext)
    {
        MusicKey = DesiredMusic; MusicContext = DesiredContext; StartedChannels.Remove(TEXT("Music"));
        if (MusicAudio)
        {
            MusicAudio->FadeOut(.6f, 0); FadingMusic.Add(MusicAudio); MusicAudio = nullptr;
            if (FadingMusic.Num() > 2) { if (FadingMusic[0]) FadingMusic[0]->Stop(); FadingMusic.RemoveAt(0); }
        }
    }
    UpdateLoop(MusicAudio, MusicKey, TEXT("Music"), Config.Audio.MusicVolume, MusicLoop);
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
void UWorldEnvironmentSubsystem::PlayDefaultFootstep(const FVector& Location, UObject* Emitter)
{
    if (!GetWorld() || GetWorld()->GetNetMode() == NM_DedicatedServer) return;
    const bool bLoop = ResolveLoop(Config.Audio.DefaultFootstep, TEXT("Footstep"), Config.Audio.DefaultFootstepLoop);
    if (V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter,
        bLoop ? TEXT("Default") : FString(), Location)) return;
    Effects.RemoveAll([](const auto& C) { return !IsValid(C) || !C->IsPlaying(); });
    if (Effects.Num() >= 16) return;
    float Volume = .7f, Pitch = 1.f;
    if (Config.Audio.DefaultFootstep.StartsWith(TEXT("event://")))
        if (const auto* Event = GetWorld()->GetSubsystem<USoundEventSubsystem>()->GetSettings(FName(Config.Audio.DefaultFootstep.Mid(8))))
        { Volume = Event->Volume; Pitch = FMath::FRandRange(Event->PitchMin, Event->PitchMax); }
    PlayFootstepSound(ResolveSound(Config.Audio.DefaultFootstep, TEXT("Footstep"), bLoop), Location, Volume, bLoop, Emitter, nullptr, Pitch);
}

void UWorldEnvironmentSubsystem::PlayFootstepSound(USoundBase* Sound, const FVector& Location, float Volume,
    bool bLoop, UObject* Emitter, UObject* LoopIdentity, float Pitch)
{
    UWorld* World = GetWorld();
    if (!IsValid(World) || World->GetNetMode() == NM_DedicatedServer || !IsValid(Sound)
        || Location.ContainsNaN() || !FMath::IsFinite(Volume) || Volume <= 0.f || !FMath::IsFinite(Pitch) || Pitch <= 0) return;
    const FString Key = bLoop ? (LoopIdentity ? LoopIdentity->GetPathName() : TEXT("Default")) : FString();
    if (V3DSoundPlayback::RefreshFootstep(FootstepLoops, this, Emitter, Key, Location)) return;
    Effects.RemoveAll([](const auto& C) { return !IsValid(C) || !C->IsPlaying(); });
#if PLATFORM_ANDROID || PLATFORM_IOS
    constexpr int32 MaxVoices = 8;
#else
    constexpr int32 MaxVoices = 16;
#endif
    if (Effects.Num() >= MaxVoices) return;
    // ResolveSound already returns a private cue/PCM. Cooked bindings need a private loop wrapper.
    USoundBase* VoiceSound = Sound->GetOuter() == this ? Sound : V3DSoundPlayback::CreateAsset(this, Sound, bLoop);
    if (!VoiceSound) return;
    auto* Audio = UGameplayStatics::SpawnSoundAtLocation(this, VoiceSound, Location,
        FRotator::ZeroRotator, Volume, Pitch, 0, SpatialAttenuation);
    if (!IsValid(Audio)) return;
    Effects.Add(Audio); ApplySpatialAudio(Audio);
    if (bLoop) V3DSoundPlayback::TrackFootstep(FootstepLoops, this, Emitter, Key, Audio, Location);
    else if (Cast<USoundWaveProcedural>(VoiceSound)) V3DSoundPlayback::StopOneShot(this, Audio, VoiceSound->Duration, Pitch);
}
