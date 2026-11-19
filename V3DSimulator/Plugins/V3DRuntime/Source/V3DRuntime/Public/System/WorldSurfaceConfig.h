// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct V3DRUNTIME_API FV3DSurfaceDefinition
{
    float Friction = 0.7f;
    float StaticFriction = 0.8f;
    float Restitution = 0.1f;
    float Density = 1.0f;
    uint8 FrictionCombine = 0; // Average, Min, Multiply, Max (EFrictionCombineMode).
    uint8 RestitutionCombine = 0;
};

struct V3DRUNTIME_API FV3DSurfaceMaterialRule
{
    FString Material;
    FName PhysicalMaterial;
};

struct V3DRUNTIME_API FV3DSoundListDefinition { TArray<FString> Sounds; bool bLoop = false; };
struct V3DRUNTIME_API FV3DSoundEventDefinition
{
    FName SoundList;
    float Volume = 1.f, PitchMin = 1.f, PitchMax = 1.f;
    bool bSpatial = true;
    bool bLoop = false;
};
struct V3DRUNTIME_API FV3DFootstepDefinition
{
    TArray<FString> Sounds;
    FName Event;
    FName SoundList;
    bool bLoop = false;
    float Volume = 0.7f;
    float PitchMin = 0.94f;
    float PitchMax = 1.06f;
};

/** Native, immutable config; no UObject creation, disk writes or assets loaded while parsing. */
struct V3DRUNTIME_API FV3DWorldSurfaceConfig
{
    TMap<FName, FV3DSurfaceDefinition> PhysicalMaterials;
    TArray<FV3DSurfaceMaterialRule> MaterialPhysicalMappings;
    TMap<FName, FV3DFootstepDefinition> FootstepMappings;
    FName DefaultPhysicalMaterial;
    TMap<FName, FV3DSoundListDefinition> SoundLists;
    TMap<FName, FV3DSoundEventDefinition> Events;

    static bool Parse(const TSharedPtr<FJsonObject>& Root, FV3DWorldSurfaceConfig& Out, FString& Error);
    bool ValidateSounds(const TSet<FGuid>& Available, FString& Error) const;
    FName ResolveMaterial(const FString& SlotName) const;
    static FString SourceMaterialName(const FString& SlotName);
};
