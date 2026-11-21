// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "World/FogProfileMath.h"

/** Authored scripts/environments.json snapshot. Runtime never modifies Global or these maps. */
struct V3DSIMULATOR_API FV3DWorldFogConfig
{
    bool bEnabled = true;
    bool bVolumetric = true;
    float TransitionSeconds = 3.f;
    V3DFog::FProfile Global;
    TMap<FName, V3DFog::FPatch> WeatherProfiles;
    TMap<FName, V3DFog::FPatch> BiomeProfiles;

    FV3DWorldFogConfig();
    static const TCHAR* MemberName() { return TEXT("scripts/environments.json"); }
    V3DFog::FProfile Resolve(FName Biome, FName Weather, float Intensity) const;
    TSharedRef<FJsonObject> ToJson() const;
    static TSharedRef<FJsonObject> ProfileTemplate();
    // Transactional: on invalid input, Out is untouched and Error identifies the field.
    static bool Parse(const TSharedPtr<FJsonObject>& Json, FV3DWorldFogConfig& Out, FString& Error);
    static bool ParseWorldConfig(const TSharedPtr<FJsonObject>& Json, FV3DWorldFogConfig& Out, FString& Error);
    static bool Load(const FString& ProjectRoot, FV3DWorldFogConfig& Out, FString& Error);
    /** Create the default scripts/environments.json authoring file if missing. */
    static bool CreateDefaultFile(const FString& ProjectRoot, FString& Error);
};
