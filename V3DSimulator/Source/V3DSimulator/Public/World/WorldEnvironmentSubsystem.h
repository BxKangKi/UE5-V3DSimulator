// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "World/WorldEnvironmentConfig.h"
#include "System/RuntimeSoundSubsystem.h"
#include "TimerManager.h"
#include "WorldEnvironmentSubsystem.generated.h"
class USoundBase;
class UAudioComponent;
class UReverbEffect;
class USoundAttenuation;
struct FStreamableHandle;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FV3DBiomeChanged, FName, Biome, bool, bInside);

/** Bounded geometry acoustics, default audio and named spatial variables. No per-frame scans. */
UCLASS()
class V3DSIMULATOR_API UWorldEnvironmentSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Deinitialize() override;
    bool Configure(const TSharedPtr<FJsonObject>& Root, FString& Error);
    void Reset();
    UFUNCTION(BlueprintPure, Category="World|Biomes") bool IsInBiome(FName Name) const;
    UFUNCTION(BlueprintPure, Category="World|Biomes") bool IsPointInBiome(FName Name, FVector Point) const;
    UFUNCTION(BlueprintPure, Category="World|Biomes") FName GetActiveBiome() const { return ActiveBiome; }
    UPROPERTY(BlueprintAssignable, Category="World|Biomes") FV3DBiomeChanged OnBiomeChanged;
    void PlayDefaultFootstep(const FVector& Location);
    void ApplySpatialAudio(UAudioComponent* Audio) const;
    USoundAttenuation* GetSpatialAttenuation() const { return SpatialAttenuation; }
private:
    FV3DEnvironmentConfig Config;
    FTimerHandle Timer;
    TSet<FName> Inside;
    FName ActiveBiome;
    uint64 Generation = 0;
    TSharedPtr<FStreamableHandle> AssetLoad;
    UPROPERTY(Transient) TArray<TObjectPtr<UObject>> LoadedAssets;
    UPROPERTY(Transient) TMap<FString, TObjectPtr<USoundBase>> Defaults;
    UPROPERTY(Transient) TArray<TObjectPtr<UAudioComponent>> Effects;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> WaterAudio;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> UnderwaterAudio;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> MusicAudio;
    UPROPERTY(Transient) TArray<TObjectPtr<UAudioComponent>> FadingMusic;
    UPROPERTY(Transient) TObjectPtr<USoundAttenuation> SpatialAttenuation;
    UPROPERTY(Transient) TObjectPtr<UReverbEffect> RoomReverb;
    TMap<FString, FV3DPcmPtr> Pcm;
    TSet<FString> Loading, Failed;
    TArray<FString> CacheOrder;
    int64 CachedBytes = 0;
    FString MusicKey;
    bool bAssetsReady = false;
    int32 LastReverbBucket = -1;
    float LastWet = -1;
    void Poll();
    void ProbeRoom(const FVector& Listener, AActor* Pawn);
    void RequestPcm(const FString& Reference);
    USoundBase* ResolveSound(const FString& Reference, const FString& DefaultKey, bool bLoop);
    void UpdateLoop(TObjectPtr<UAudioComponent>& Component, const FString& Reference,
        const FString& DefaultKey, float Volume);
};
