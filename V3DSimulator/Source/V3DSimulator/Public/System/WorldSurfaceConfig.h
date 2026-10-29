// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FV3DSurfaceDefinition
{
    float Friction = 0.7f;
    float StaticFriction = 0.8f;
    float Restitution = 0.1f;
    float Density = 1.0f;
    uint8 FrictionCombine = 0; // Average, Min, Multiply, Max (EFrictionCombineMode).
    uint8 RestitutionCombine = 0;
};

struct FV3DSurfaceMaterialRule
{
    FString Material;
    FName PhysicalMaterial;
};

struct FV3DFootstepDefinition
{
    TArray<FString> Sounds;
    float Volume = 0.7f;
    float PitchMin = 0.94f;
    float PitchMax = 1.06f;
};

/** Native, immutable config; no UObject creation, disk writes or assets loaded while parsing. */
struct V3DSIMULATOR_API FV3DWorldSurfaceConfig
{
    TMap<FName, FV3DSurfaceDefinition> PhysicalMaterials;
    TArray<FV3DSurfaceMaterialRule> MaterialPhysicalMappings;
    TMap<FName, FV3DFootstepDefinition> FootstepMappings;
    FName DefaultPhysicalMaterial;

    static bool Parse(const TSharedPtr<FJsonObject>& Root, FV3DWorldSurfaceConfig& Out, FString& Error);
    bool ValidateSounds(const TSet<FGuid>& Available, FString& Error) const;
    FName ResolveMaterial(const FString& SlotName) const;
    static FString SourceMaterialName(const FString& SlotName);
};
