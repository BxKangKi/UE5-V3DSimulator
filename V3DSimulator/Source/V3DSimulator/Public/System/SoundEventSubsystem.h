// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "System/WorldSurfaceConfig.h"
#include "System/RuntimeSoundSubsystem.h"
#include "SoundEventSubsystem.generated.h"
class UAudioComponent;
class USoundBase;
USTRUCT()
struct FV3DSoundEventVoice
{
    GENERATED_BODY()
    UPROPERTY(Transient) FName Event;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> Audio;
};
/** Each trigger owns a fresh voice. Triggering B never invalidates A's pending load. */
UCLASS()
class V3DSIMULATOR_API USoundEventSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void Reset();
    FString PickReference(FName Event) const;
    bool IsLooping(FName Event) const;
    const FV3DSoundEventDefinition* GetSettings(FName Event) const { return Definitions.Find(Event); }
    UFUNCTION(BlueprintCallable, Category="Audio|Events") void StopEvent(FName Event);
    UFUNCTION(BlueprintCallable, Category="Audio|Events") void StopAllEvents();
    void Configure(const FV3DWorldSurfaceConfig& Config);
    virtual void Deinitialize() override;
    UFUNCTION(BlueprintCallable, Category="Audio|Events")
    bool TriggerEvent(FName Event, FVector Location);
    UFUNCTION(BlueprintCallable, Category="Audio|Events")
    int32 TriggerEvents(const TArray<FName>& Events, FVector Location);
    UFUNCTION(BlueprintPure, Category="Audio|Events") bool HasEvent(FName Event) const { return Definitions.Contains(Event); }
private:
    friend class FV3DSoundEventPcmCacheTest;
    bool PlayLoadedVoice(FName Event, const FV3DSoundEventDefinition& Settings, FVector Location, FV3DPcmPtr Data);
    void CachePcm(const FString& Reference, FV3DPcmPtr Data);
    // Immutable decoded samples are shared; procedural queues and loop cursors remain per voice.
    TMap<FString, FV3DPcmPtr> PcmCache;
    TArray<FString> PcmLru;
    int64 CachedPcmBytes = 0;
    TMap<FName, FV3DSoundEventDefinition> Definitions;
    TMap<FName, FV3DSoundListDefinition> Lists;
    uint64 Generation = 0;
    int32 Pending = 0;
    TMap<FName, uint64> EventGenerations;
    UPROPERTY(Transient) TArray<FV3DSoundEventVoice> Voices;
};
