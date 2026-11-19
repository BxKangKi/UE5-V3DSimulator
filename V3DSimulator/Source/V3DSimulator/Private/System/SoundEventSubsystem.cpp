// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SoundEventSubsystem.h"
#include "System/RuntimeSoundSubsystem.h"
#include "System/V3DSoundPlayback.h"
#include "World/WorldEnvironmentSubsystem.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

void USoundEventSubsystem::Reset()
{
    StopAllEvents(); Definitions.Empty(); Lists.Empty();
    PcmCache.Empty(); PcmLru.Empty(); CachedPcmBytes = 0;
}
void USoundEventSubsystem::StopAllEvents()
{
    ++Generation; Pending = 0;
    for (const auto& Voice : Voices) if (IsValid(Voice.Audio)) Voice.Audio->Stop();
    Voices.Empty(); EventGenerations.Empty();
    if (GetWorld()) GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
}
void USoundEventSubsystem::StopEvent(FName Event)
{
    if (!Definitions.Contains(Event)) return;
    ++EventGenerations.FindOrAdd(Event); // Pending loads for this event cannot start afterward.
    for (const auto& Voice : Voices) if (Voice.Event == Event && IsValid(Voice.Audio)) Voice.Audio->Stop();
    Voices.RemoveAll([Event](const auto& Voice) { return Voice.Event == Event || !IsValid(Voice.Audio) || !Voice.Audio->IsPlaying(); });
}
bool USoundEventSubsystem::IsLooping(FName Event) const
{
    const auto* Definition = Definitions.Find(Event);
    return Definition && Definition->bLoop;
}

void USoundEventSubsystem::Configure(const FV3DWorldSurfaceConfig& Config)
{
    Reset(); Definitions = Config.Events; Lists = Config.SoundLists;
}
void USoundEventSubsystem::Deinitialize() { Reset(); Super::Deinitialize(); }
FString USoundEventSubsystem::PickReference(FName Event) const
{
    const auto* Definition = Definitions.Find(Event);
    const auto* List = Definition ? Lists.Find(Definition->SoundList) : nullptr;
    return List && !List->Sounds.IsEmpty() ? List->Sounds[FMath::RandHelper(List->Sounds.Num())] : FString();
}
int32 USoundEventSubsystem::TriggerEvents(const TArray<FName>& Events, FVector Location)
{
    int32 Count = 0;
    for (FName Event : Events) Count += TriggerEvent(Event, Location) ? 1 : 0;
    return Count;
}
bool USoundEventSubsystem::TriggerEvent(FName Event, FVector Location)
{
    check(IsInGameThread());
    if (!GetWorld() || GetWorld()->GetNetMode() == NM_DedicatedServer || Location.ContainsNaN()) return false;
    const auto* Definition = Definitions.Find(Event);
    const auto* List = Definition ? Lists.Find(Definition->SoundList) : nullptr;
    if (!List || List->Sounds.IsEmpty()) return false;
    if (Definition->Volume <= 0.f) return true;
    Voices.RemoveAll([](const auto& Voice) { return !IsValid(Voice.Audio) || !Voice.Audio->IsPlaying(); });
    if (Voices.Num() + Pending >= 64) return false;
    auto* Loader = URuntimeSoundSubsystem::Get(this);
    if (!Loader) return false;
    const auto Settings = *Definition;
    const FString Reference = List->Sounds[FMath::RandHelper(List->Sounds.Num())];
    if (const FV3DPcmPtr* Cached = PcmCache.Find(Reference))
    {
        PcmLru.Remove(Reference); PcmLru.Add(Reference);
        return PlayLoadedVoice(Event, Settings, Location, *Cached);
    }
    const uint64 Token = Generation;
    const uint64 EventToken = EventGenerations.FindOrAdd(Event);
    const TWeakObjectPtr<USoundEventSubsystem> Weak(this);
    ++Pending;
    Loader->LoadPcmAsync(Reference, [Weak, Token, EventToken, Event, Settings, Location, Reference](FV3DPcmPtr Data, const FString& Error)
    {
        auto* Self = Weak.Get();
        if (!Self || Self->Generation != Token) return;
        --Self->Pending;
        if (Self->EventGenerations.FindRef(Event) != EventToken) return;
        if (!Data || !Data->IsValid())
        { UE_LOG(LogTemp, Warning, TEXT("Sound event could not load %s: %s"), *Reference, *Error); return; }
        Self->CachePcm(Reference, Data);
        Self->PlayLoadedVoice(Event, Settings, Location, MoveTemp(Data));
    }, 16ll * 1024 * 1024);
    return true;
}

void USoundEventSubsystem::CachePcm(const FString& Reference, FV3DPcmPtr Data)
{
    constexpr int64 MaxCacheBytes = 32ll * 1024 * 1024;
    if (!Data || !Data->IsValid() || Data->Bytes.Num() > MaxCacheBytes) return;
    if (PcmCache.Contains(Reference)) return; // Concurrent initial requests may finish together.
    while (!PcmLru.IsEmpty() && (CachedPcmBytes + Data->Bytes.Num() > MaxCacheBytes || PcmCache.Num() >= 64))
    {
        const FString Oldest = PcmLru[0];
        if (const auto* Existing = PcmCache.Find(Oldest)) CachedPcmBytes -= (*Existing)->Bytes.Num();
        PcmCache.Remove(Oldest); PcmLru.RemoveAt(0);
    }
    CachedPcmBytes += Data->Bytes.Num();
    PcmCache.Add(Reference, MoveTemp(Data)); PcmLru.Add(Reference);
}

bool USoundEventSubsystem::PlayLoadedVoice(FName Event, const FV3DSoundEventDefinition& Settings, FVector Location, FV3DPcmPtr Data)
{
    if (!GetWorld()) return false;
    auto* Wave = V3DSoundPlayback::CreatePcm(this, Data, Settings.bLoop);
    if (!Wave) return false;
    const float Pitch = FMath::FRandRange(Settings.PitchMin, Settings.PitchMax);
    auto* Environment = GetWorld()->GetSubsystem<UWorldEnvironmentSubsystem>();
    auto* Voice = Settings.bSpatial
        ? UGameplayStatics::SpawnSoundAtLocation(this, Wave, Location, FRotator::ZeroRotator, Settings.Volume, Pitch,
            0, Environment ? Environment->GetSpatialAttenuation() : nullptr)
        : UGameplayStatics::SpawnSound2D(this, Wave, Settings.Volume, Pitch);
    if (!Voice) return false;
    auto& Entry = Voices.AddDefaulted_GetRef(); Entry.Event = Event; Entry.Audio = Voice;
    if (Settings.bSpatial && Environment) Environment->ApplySpatialAudio(Voice);
    if (!Settings.bLoop) V3DSoundPlayback::StopOneShot(this, Voice, Wave->Duration, Pitch);
    return true;
}
