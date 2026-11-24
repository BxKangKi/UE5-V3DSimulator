#pragma once

#include "CoreMinimal.h"

// Shared by editor graph migration and runtime MIDs, so cooked and PIE values agree.
// Values are rendering defaults; imported material textures/factors remain authoritative.
namespace V3DPhysicalMaterial
{
    struct FScalar { const TCHAR* Name; float Value; };
    struct FVectorParameter { const TCHAR* Name; FLinearColor Value; };

    // Natural average clear-water surface opacity from above water (boosted at grazing angles via Fresnel);
    // the underside still owns its transparent Snell window and opaque TIR region.
    constexpr float WaterSurfaceOpacity = 0.36f;
    inline const FScalar WaterScalars[] = {
        {TEXT("Water Opacity"), WaterSurfaceOpacity}, {TEXT("Opacity"), WaterSurfaceOpacity},
        {TEXT("Water Specular"), 0.50f}, {TEXT("Water Fresnel Specular"), 0.50f},
        {TEXT("Water Roughness"), 0.06f}, {TEXT("Water Fresnel Roughness"), 0.04f},
        {TEXT("Water Fresnel  Exponenth"), 5.0f}, {TEXT("PhaseG"), 0.72f},
        // PixelNormalOffset subtracts 1: zero would produce a full reverse offset.
        {TEXT("Refraction"), 1.015f}, {TEXT("Refraction Far"), 1.0f},
        // Bottom Amount is a blend weight, while Lip Refraction is an actual PNO value.
        {TEXT("Refraction Bottom Amount"), 0.0f}, {TEXT("Lip Refraction"), 1.0f},
        // Legacy underside uses saturate((Dot - NoV) / Width). Centre the transition
        // at cos(asin(1 / 1.3333)) = 0.6614166 instead of the old 0.225 threshold.
        {TEXT("Critical Angle Dot"), 0.6814166f}, {TEXT("Critical Angle Width"), 0.04f},
        {TEXT("CausticsMaxIntensity"), 1.0f},
        {TEXT("Default Near Water Scale"), 1024.0f}, {TEXT("Default Distant Water Scale"), 3500.0f},
        {TEXT("Default Near Normal Strength"), 0.24f},
        {TEXT("Default Distant Normal Strength"), 0.16f}, {TEXT("Default Distant Normal StrengthB"), 0.16f},
        {TEXT("Water Distant Normal Offset"), 2500.0f}, {TEXT("Water Distant Normal Length"), 10000.0f}
    };
    // Native SingleLayerWater coefficients in inverse centimeters (UE world units).
    // Balanced average natural clear-blue water optics (neither murky dark teal nor overly stylized cyan).
    inline const FLinearColor WaterAbsorption(0.0028f, 0.00105f, 0.00058f, 1.0f);
    inline const FLinearColor WaterScattering(0.000035f, 0.00011f, 0.00021f, 1.0f);
    inline const FVectorParameter WaterVectors[] = {
        {TEXT("AbsorptionCoefficients"), WaterAbsorption}, {TEXT("Absorption"), WaterAbsorption},
        {TEXT("ScatteringCoefficients"), WaterScattering}, {TEXT("Scattering"), WaterScattering},
        {TEXT("Water Albedo"), FLinearColor(0.018f, 0.065f, 0.125f, 1.0f)},
        {TEXT("ColorScaleBehindWater"), FLinearColor::White}
    };

    constexpr float MoonIntensity = 0.65f; // Texture brightness; visibility is a separate control.
    // Enlarged default angular radii (in radians) for the sun (~4.8 deg diameter) and moon (~5.5 deg diameter).
    // Runtime actors and world config.json can override these independently via SunAngularRadius / MoonAngularRadius.
    constexpr float DefaultSunAngularRadiusRadians = 0.042f;
    constexpr float DefaultMoonAngularRadiusRadians = 0.048f;
    constexpr float MinCelestialAngularRadiusRadians = 0.001f;
    constexpr float MaxCelestialAngularRadiusRadians = 0.35f;
    constexpr float CelestialRadiusRadians = DefaultSunAngularRadiusRadians;
    constexpr float CelestialDiameterDegrees = CelestialRadiusRadians * (360.f / PI);
    constexpr float MoonRadiusRadians = DefaultMoonAngularRadiusRadians;
    constexpr float MoonDiameterDegrees = MoonRadiusRadians * (360.f / PI);
    constexpr float StarIntensity = 0.035f;
    constexpr float DielectricSpecular = 0.5f; // F0 = 0.04
    constexpr float GlassRoughness = 0.045f;
    constexpr float FoliageRoughness = 0.65f;
}
