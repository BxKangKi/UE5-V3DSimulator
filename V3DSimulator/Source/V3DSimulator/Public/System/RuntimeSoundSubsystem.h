// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RuntimeSoundSubsystem.generated.h"

class USoundWaveProcedural;

/** Reusable native PCM. Each playback must create a fresh procedural wave/queue. */
struct FV3DDecodedPcm16
{
    TArray<uint8> Bytes;
    int32 SampleRate = 0;
    int32 NumChannels = 0;
    int32 NumFrames = 0;
    FString Error;
    bool IsValid() const { return Error.IsEmpty() && !Bytes.IsEmpty() && SampleRate > 0 && NumChannels > 0 && NumFrames > 0; }
};
using FV3DPcmPtr = TSharedPtr<const FV3DDecodedPcm16, ESPMode::ThreadSafe>;
using FV3DPcmCompletion = TFunction<void(FV3DPcmPtr, const FString&)>;

DECLARE_DYNAMIC_DELEGATE_TwoParams(
    FRuntimeSoundLoadCompleted,
    USoundWaveProcedural*, Sound,
    const FString&, Error);

/**
 * Runtime loader for AssetType=Sound entries stored in .v3d/.v3d archives.
 *
 * Archive range I/O and WAV decoding run on a tracked worker. Only USoundWaveProcedural creation
 * and PCM publication run on GameThread. No platform codec or source-project path is used at runtime.
 */
UCLASS()
class V3DSIMULATOR_API URuntimeSoundSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category="Sound", meta=(WorldContext="WorldContextObject"))
    static URuntimeSoundSubsystem* Get(const UObject* WorldContextObject);

    /** Loads an exact gworld://sound/<UUID> reference asynchronously. */
    UFUNCTION(BlueprintCallable, Category="Sound")
    void LoadSoundAsync(const FString& SoundReference, FRuntimeSoundLoadCompleted Completion);

    /** Bounded native decode for cached short effects. Completion runs on GameThread. */
    void LoadPcmAsync(const FString& SoundReference, FV3DPcmCompletion Completion,
        int64 MaxBytes = 512ll * 1024ll * 1024ll);

private:
    uint64 RequestGeneration = 1;
};
