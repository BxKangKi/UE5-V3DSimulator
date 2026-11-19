// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "System/RuntimeSoundSubsystem.h"
#include "TimerManager.h"
class UAudioComponent;
class USoundBase;
class USoundWaveProcedural;

struct FV3DFootstepLoopVoice
{
    TWeakObjectPtr<UAudioComponent> Audio;
    FString Key;
    FTimerHandle StopTimer;
};
using FV3DFootstepLoops = TMap<TWeakObjectPtr<UObject>, FV3DFootstepLoopVoice>;

namespace V3DSoundPlayback
{
    USoundWaveProcedural* CreatePcm(UObject* Outer, FV3DPcmPtr Data, bool bLoop);
    /** Private runtime cue/wave-player state; cooked sound assets are never edited. */
    USoundBase* CreateAsset(UObject* Outer, USoundBase* Source, bool bLoop);
    void StopOneShot(UObject* Owner, UAudioComponent* Audio, float Duration, float Pitch);
    /** A looping footstep is one voice per emitter, renewed by footsteps and stopped after 0.75 s idle. */
    bool RefreshFootstep(FV3DFootstepLoops& Loops, UObject* Owner, UObject* Emitter,
        const FString& Key, const FVector& Location);
    void TrackFootstep(FV3DFootstepLoops& Loops, UObject* Owner, UObject* Emitter,
        const FString& Key, UAudioComponent* Audio, const FVector& Location);
}
