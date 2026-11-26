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
    constexpr float WaterSurfaceOpacity = 0.28f;
    inline const FScalar WaterScalars[] = {
        {TEXT("Water Opacity"), WaterSurfaceOpacity}, {TEXT("Opacity"), WaterSurfaceOpacity},
        {TEXT("Water Specular"), 0.50f}, {TEXT("Water Fresnel Specular"), 0.62f},
        {TEXT("Water Roughness"), 0.03f}, {TEXT("Water Fresnel Roughness"), 0.015f},
        {TEXT("Water Fresnel  Exponenth"), 5.0f}, {TEXT("PhaseG"), 0.52f},
        // PixelNormalOffset subtracts 1: zero would produce a full reverse offset.
        {TEXT("Refraction"), 1.015f}, {TEXT("Refraction Far"), 1.0f},
        // Bottom Amount is a blend weight, while Lip Refraction is an actual PNO value.
        {TEXT("Refraction Bottom Amount"), 0.0f}, {TEXT("Lip Refraction"), 1.0f},
        // Slightly tighter/softer Snell window keeps looking up from underwater realistic rather than air-clear.
        {TEXT("Critical Angle Dot"), 0.74f}, {TEXT("Critical Angle Width"), 0.07f},
        {TEXT("CausticsMaxIntensity"), 0.85f},
        {TEXT("Default Near Water Scale"), 900.0f}, {TEXT("Default Distant Water Scale"), 3600.0f},
        {TEXT("Default Near Normal Strength"), 0.20f},
        {TEXT("Default Distant Normal Strength"), 0.14f}, {TEXT("Default Distant Normal StrengthB"), 0.14f},
        {TEXT("Water Distant Normal Offset"), 2400.0f}, {TEXT("Water Distant Normal Length"), 10000.0f}
    };
    // Native SingleLayerWater coefficients in inverse centimeters (UE world units).
    // Strong red & moderate green absorption prevent sunlit emerald glow while hiding deep seabed naturally.
    inline const FLinearColor WaterAbsorption(0.0055f, 0.0028f, 0.00120f, 1.0f);
    inline const FLinearColor WaterScattering(0.000020f, 0.000045f, 0.000115f, 1.0f);
    inline const FVectorParameter WaterVectors[] = {
        {TEXT("AbsorptionCoefficients"), WaterAbsorption}, {TEXT("Absorption"), WaterAbsorption},
        {TEXT("ScatteringCoefficients"), WaterScattering}, {TEXT("Scattering"), WaterScattering},
        {TEXT("Water Albedo"), FLinearColor(0.006f, 0.014f, 0.036f, 1.0f)},
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
