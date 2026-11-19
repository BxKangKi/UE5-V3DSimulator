// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "WorldEnvironmentConfig.generated.h"

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DWorldAudioSettings
{
    GENERATED_BODY()
    TOptional<bool> DefaultFootstepLoop, WaterLoop, UnderwaterLoop, EnvironmentMusicLoop;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") bool bGlobalAcoustics = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") bool bEnvironmentMusic = true;
    /** Empty uses AssetRegistry. "none" explicitly mutes. Otherwise v3d://sound/UUID. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") FString DefaultFootstep;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") FString Water;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") FString Underwater;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio") FString EnvironmentMusic;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio", meta=(ClampMin="0", ClampMax="2")) float MusicVolume = 0.4f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio", meta=(ClampMin="0.02", ClampMax="1")) float Absorption = 0.25f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Audio", meta=(ClampMin="100", ClampMax="20000")) float ProbeDistanceCm = 3000;
};

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DBiomeDefinition
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FName Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") bool bSphere = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FVector Min = FVector(-1000);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FVector Max = FVector(1000);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FVector Center = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") float Radius = 1000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") int32 Priority = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FString Music;
    TOptional<bool> MusicLoop;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FName EnterCommand;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Biome") FName ExitCommand;
    bool Contains(const FVector& Point) const;
};

struct V3DSIMULATOR_API FV3DEnvironmentConfig
{
    FV3DWorldAudioSettings Audio;
    TArray<FV3DBiomeDefinition> Biomes;
    static bool Parse(const TSharedPtr<FJsonObject>& Root, FV3DEnvironmentConfig& Out, FString& Error);
};
