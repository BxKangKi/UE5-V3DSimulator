// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DSoundPlayback.h"
#include "Audio/PcmLoopCursor.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWaveProcedural.h"

USoundWaveProcedural* V3DSoundPlayback::CreatePcm(UObject* Outer, FV3DPcmPtr Data, bool bLoop)
{
    if (!Data || !Data->IsValid() || Data->Bytes.Num() % (Data->NumChannels * 2) != 0) return nullptr;
    auto* Wave = NewObject<USoundWaveProcedural>(Outer, NAME_None, RF_Transient);
    Wave->NumChannels = Data->NumChannels; Wave->SetSampleRate(Data->SampleRate);
    Wave->SetNumFrames(Data->NumFrames); Wave->SampleByteSize = 2;
    Wave->Duration = bLoop ? INDEFINITELY_LOOPING_DURATION : float(Data->NumFrames) / Data->SampleRate;
    if (bLoop)
    {
        // Each audio thread queue owns immutable PCM and an independent cursor. No UObject captures.
        Wave->OnSoundWaveProceduralUnderflow.BindLambda([Data, Cursor = std::size_t(0)](USoundWaveProcedural* W, int32 Samples) mutable
        {
            V3DAudio::QueueLoopedPcm(Data->Bytes.GetData(), Data->Bytes.Num(),
                FMath::Clamp(Samples, 1, 32768) * 2, Cursor,
                [W](const std::uint8_t* Bytes, std::size_t Count) { W->QueueAudio(Bytes, static_cast<int32>(Count)); });
        });
    }
    else Wave->QueueAudio(Data->Bytes.GetData(), Data->Bytes.Num());
    return Wave;
}

USoundBase* V3DSoundPlayback::CreateAsset(UObject* Outer, USoundBase* Source, bool bLoop)
{
    if (!IsValid(Source)) return nullptr;
    if (auto* Procedural = Cast<USoundWaveProcedural>(Source)) return Procedural; // Already private, initialized by CreatePcm.
    if (auto* Wave = Cast<USoundWave>(Source))
    {
        auto* Cue = NewObject<USoundCue>(Outer, NAME_None, RF_Transient);
        auto* Player = NewObject<USoundNodeWavePlayer>(Cue, NAME_None, RF_Transient);
        Player->SetSoundWave(Wave); Player->bLooping = bLoop;
        Cue->FirstNode = Player; Cue->VolumeMultiplier = 1.f; Cue->PitchMultiplier = 1.f;
        Cue->Duration = bLoop ? INDEFINITELY_LOOPING_DURATION : Wave->Duration;
        return Cue;
    }
    if (auto* Original = Cast<USoundCue>(Source))
    {
        auto* Cue = DuplicateObject<USoundCue>(Original, Outer);
        Cue->SetFlags(RF_Transient);
        // Explicitly clone each reachable node: even external/shared source nodes stay untouched.
        TMap<USoundNode*, USoundNode*> Copies;
        TFunction<USoundNode*(USoundNode*)> Clone = [&](USoundNode* Node) -> USoundNode*
        {
            if (!Node) return nullptr;
            if (auto** Found = Copies.Find(Node)) return *Found;
            auto* Copy = DuplicateObject<USoundNode>(Node, Cue, MakeUniqueObjectName(Cue, Node->GetClass(), Node->GetFName()));
            Copies.Add(Node, Copy); Copy->SetFlags(RF_Transient);
            Copy->ChildNodes.Reset();
            for (USoundNode* Child : Node->ChildNodes) Copy->ChildNodes.Add(Clone(Child));
            if (auto* Player = Cast<USoundNodeWavePlayer>(Copy)) Player->bLooping = false;
            if (auto* Loop = Cast<USoundNodeLooping>(Copy)) { Loop->bLoopIndefinitely = false; Loop->LoopCount = 1; }
            if (auto* Envelope = Cast<USoundNodeEnveloper>(Copy)) { Envelope->bLoop = false; Envelope->bLoopIndefinitely = false; }
            return Copy;
        };
        Cue->FirstNode = Clone(Original->FirstNode);
        if (bLoop)
        {
            auto* Loop = NewObject<USoundNodeLooping>(Cue, NAME_None, RF_Transient);
            Loop->bLoopIndefinitely = true; Loop->ChildNodes.Reset(); Loop->ChildNodes.Add(Cue->FirstNode); Cue->FirstNode = Loop;
        }
        USoundNode* Root = Cue->FirstNode;
        Cue->Duration = Root ? Root->GetDuration() : 0.f; // exported base virtual interface
        return Cue;
    }
    UE_LOG(LogTemp, Warning, TEXT("Sound defaults require a SoundWave or SoundCue for per-playback Loop: %s"), *GetPathNameSafe(Source));
    return nullptr;
}

void V3DSoundPlayback::StopOneShot(UObject* Owner, UAudioComponent* Audio, float Duration, float Pitch)
{
    if (!Owner || !Owner->GetWorld() || !IsValid(Audio) || Duration <= 0 || Duration >= INDEFINITELY_LOOPING_DURATION) return;
    FTimerHandle Timer;
    const TWeakObjectPtr<UAudioComponent> Weak(Audio);
    Owner->GetWorld()->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(Owner,
        [Weak] { if (auto* Voice = Weak.Get()) Voice->Stop(); }), Duration / FMath::Max(Pitch, .01f) + .1f, false);
}

bool V3DSoundPlayback::RefreshFootstep(FV3DFootstepLoops& Loops, UObject* Owner, UObject* Emitter,
    const FString& Key, const FVector& Location)
{
    if (!Owner || !Owner->GetWorld()) return false;
    for (auto It = Loops.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || !It.Value().Audio.IsValid() || !It.Value().Audio->IsPlaying())
        {
            if (auto* Audio = It.Value().Audio.Get()) Audio->Stop();
            Owner->GetWorld()->GetTimerManager().ClearTimer(It.Value().StopTimer);
            It.RemoveCurrent();
        }
    auto* Entry = Loops.Find(TWeakObjectPtr<UObject>(Emitter ? Emitter : Owner));
    if (!Entry) return false;
    if (Entry->Key != Key)
    {
        if (auto* Audio = Entry->Audio.Get()) Audio->Stop();
        Owner->GetWorld()->GetTimerManager().ClearTimer(Entry->StopTimer);
        Loops.Remove(TWeakObjectPtr<UObject>(Emitter ? Emitter : Owner));
        return false;
    }
    Entry->Audio->SetWorldLocation(Location);
    const auto Weak = Entry->Audio;
    Owner->GetWorld()->GetTimerManager().SetTimer(Entry->StopTimer, FTimerDelegate::CreateWeakLambda(Owner,
        [Weak] { if (auto* Audio = Weak.Get()) Audio->Stop(); }), .75f, false);
    return true;
}

void V3DSoundPlayback::TrackFootstep(FV3DFootstepLoops& Loops, UObject* Owner, UObject* Emitter,
    const FString& Key, UAudioComponent* Audio, const FVector& Location)
{
    auto& Entry = Loops.FindOrAdd(TWeakObjectPtr<UObject>(Emitter ? Emitter : Owner));
    Entry.Key = Key; Entry.Audio = Audio;
    RefreshFootstep(Loops, Owner, Emitter, Key, Location);
}
