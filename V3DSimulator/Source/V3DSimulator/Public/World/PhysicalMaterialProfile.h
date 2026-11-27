#pragma once

#include "CoreMinimal.h"

// Shared by editor graph migration and runtime MIDs, so cooked and PIE values agree.
// Values are rendering defaults; imported material textures/factors remain authoritative.
namespace V3DPhysicalMaterial
{
    struct FScalar { const TCHAR* Name; float Value; };
    struct FVectorParameter { const TCHAR* Name; FLinearColor Value; };

    // Balanced natural water optics for both Raster and Lumen Hardware Ray Tracing:
    // shallow floor stays visible up to ~2-4m, deeper water fades smoothly into deep natural blue (no emerald wash).
    constexpr float WaterSurfaceOpacity = 0.20f;
    inline const FScalar WaterScalars[] = {
        {TEXT("Water Opacity"), WaterSurfaceOpacity}, {TEXT("Opacity"), WaterSurfaceOpacity},
        {TEXT("Water Specular"), 0.50f}, {TEXT("Water Fresnel Specular"), 0.60f},
        {TEXT("Water Roughness"), 0.03f}, {TEXT("Water Fresnel Roughness"), 0.015f},
        {TEXT("Water Fresnel  Exponenth"), 5.0f}, {TEXT("PhaseG"), 0.58f},
        // PixelNormalOffset subtracts 1: zero would produce a full reverse offset.
        {TEXT("Refraction"), 1.015f}, {TEXT("Refraction Far"), 1.0f},
        // Bottom Amount is a blend weight, while Lip Refraction is an actual PNO value.
        {TEXT("Refraction Bottom Amount"), 0.0f}, {TEXT("Lip Refraction"), 1.0f},
        // Physical Snell critical angle for water IOR 1.3333 (cos(asin(1/1.3333)) = 0.6614) with smooth blend.
        {TEXT("Critical Angle Dot"), 0.6814166f}, {TEXT("Critical Angle Width"), 0.06f},
        {TEXT("CausticsMaxIntensity"), 0.90f},
        {TEXT("Default Near Water Scale"), 900.0f}, {TEXT("Default Distant Water Scale"), 3600.0f},
        {TEXT("Default Near Normal Strength"), 0.16f},
        {TEXT("Default Distant Normal Strength"), 0.11f}, {TEXT("Default Distant Normal StrengthB"), 0.11f},
        {TEXT("Water Distant Normal Offset"), 2400.0f}, {TEXT("Water Distant Normal Length"), 10000.0f}
    };
    // Native SingleLayerWater coefficients in inverse centimeters (matched with Underwater.ush Jerlov IB/II).
    inline const FLinearColor WaterAbsorption(0.00320f, 0.00110f, 0.00065f, 1.0f);
    inline const FLinearColor WaterScattering(0.000032f, 0.000075f, 0.000135f, 1.0f);
    inline const FVectorParameter WaterVectors[] = {
        {TEXT("AbsorptionCoefficients"), WaterAbsorption}, {TEXT("Absorption"), WaterAbsorption},
        {TEXT("ScatteringCoefficients"), WaterScattering}, {TEXT("Scattering"), WaterScattering},
        {TEXT("Water Albedo"), FLinearColor(0.012f, 0.036f, 0.072f, 1.0f)},
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
