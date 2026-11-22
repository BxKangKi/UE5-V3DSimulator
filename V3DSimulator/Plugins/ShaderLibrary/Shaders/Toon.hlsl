// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#ifndef V3D_TOON_HLSL
#define V3D_TOON_HLSL
#pragma once
// /Plugin/ShaderLibrary/Toon.hlsl

// Core exposure-matched Emissive Toon shading.
// BaseColor on M_Toon is kept pure black (0,0,0) with Metallic=0, Specular=0, Roughness=1
// so PBR indirect specular/occlusion never produces dark metallic sheen in daytime.
// All stylized shading is output through EmissiveColor calibrated to real directional
// + sky ambient luminance (1/PI diffuse normalization) so AutoExposure stays stable day & night,
// while native DistanceToNearestSurface samples along the primary light direction receive soft
// cast shadows from surrounding meshes.
float3 V3DToonComputeShading(
    FMaterialPixelParameters Parameters,
    float3 Albedo,
    float3 ShadeTint,
    float3 WorldNormalInput,
    float ShadowThreshold,
    float ShadowSmoothness,
    float AmbientWeight,
    float ShadowReceiveStrength,
    float DistNear,
    float DistMid,
    float DistFar,
    float3 ExtraEmissive)
{
    const float3 SafeAlbedo = max(all(isfinite(Albedo)) ? Albedo : float3(0.5f, 0.5f, 0.5f), 0.0f);
    const float3 SafeShadeTint = saturate(all(isfinite(ShadeTint)) ? ShadeTint : float3(0.55f, 0.60f, 0.72f));
    const float3 SafeEmissive = max(all(isfinite(ExtraEmissive)) ? ExtraEmissive : float3(0.0f, 0.0f, 0.0f), 0.0f);

    // Resolve world-space shading normal safely.
    const float NormalLenSq = dot(WorldNormalInput, WorldNormalInput);
    const float3 FallbackNormal = Parameters.WorldNormal;
    const float FallbackLenSq = dot(FallbackNormal, FallbackNormal);
    const float3 N = (isfinite(NormalLenSq) && NormalLenSq > 1.0e-6f)
        ? (WorldNormalInput * rsqrt(NormalLenSq))
        : ((isfinite(FallbackLenSq) && FallbackLenSq > 1.0e-6f)
            ? (FallbackNormal * rsqrt(FallbackLenSq))
            : float3(0.0f, 0.0f, 1.0f));

    // Read real-time Sun (index 0) and Moon (index 1) directions & linear light colors.
    const float3 SunDirRaw = ResolvedView.AtmosphereLightDirection[0].xyz;
    const float SunDirLenSq = dot(SunDirRaw, SunDirRaw);
    const float3 SunDir = (isfinite(SunDirLenSq) && SunDirLenSq > 1.0e-6f)
        ? (SunDirRaw * rsqrt(SunDirLenSq))
        : float3(0.0f, 0.0f, 1.0f);
    const float3 SunColor = max(all(isfinite(ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[0].rgb))
        ? ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[0].rgb : float3(0.0f, 0.0f, 0.0f), 0.0f);
    const float SunHorizon = smoothstep(-0.06f, 0.14f, SunDir.z);
    const float3 ActiveSunLight = SunColor * SunHorizon;

    const float3 MoonDirRaw = ResolvedView.AtmosphereLightDirection[1].xyz;
    const float MoonDirLenSq = dot(MoonDirRaw, MoonDirRaw);
    const float3 MoonDir = (isfinite(MoonDirLenSq) && MoonDirLenSq > 1.0e-6f)
        ? (MoonDirRaw * rsqrt(MoonDirLenSq))
        : float3(0.0f, 0.0f, 1.0f);
    const float3 MoonColor = max(all(isfinite(ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[1].rgb))
        ? ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[1].rgb : float3(0.0f, 0.0f, 0.0f), 0.0f);
    const float MoonHorizon = smoothstep(-0.04f, 0.16f, MoonDir.z) * (1.0f - SunHorizon);
    const float3 ActiveMoonLight = MoonColor * MoonHorizon;

    const float SunLuma = dot(ActiveSunLight, float3(0.2126f, 0.7152f, 0.0722f));
    const float MoonLuma = dot(ActiveMoonLight, float3(0.2126f, 0.7152f, 0.0722f));
    const bool bUseSun = SunLuma >= MoonLuma;
    const float3 PrimaryDir = bUseSun ? SunDir : MoonDir;
    const float3 PrimaryLight = bUseSun ? ActiveSunLight : ActiveMoonLight;

    // 1) Self-shadow cel band from primary directional light.
    const float Threshold = clamp(isfinite(ShadowThreshold) ? ShadowThreshold : 0.08f, -0.5f, 0.8f);
    const float Smoothness = clamp(isfinite(ShadowSmoothness) ? ShadowSmoothness : 0.06f, 0.005f, 0.5f);
    const float NdotL = dot(N, PrimaryDir);
    const float SelfLit = smoothstep(Threshold - Smoothness, Threshold + Smoothness, NdotL);

    // 2) Cast shadow reception from surrounding geometry via 3-tap DistanceToNearestSurface.
    // If Distance Fields are disabled or unavailable (all samples <= 0), default to unshadowed (1.0).
    float CastShadow = 1.0f;
    const float SafeReceive = saturate(isfinite(ShadowReceiveStrength) ? ShadowReceiveStrength : 1.0f);
    if (SafeReceive > 0.001f && isfinite(DistNear) && isfinite(DistMid) && isfinite(DistFar))
    {
        const float MaxDist = max(DistNear, max(DistMid, DistFar));
        if (MaxDist > 0.01f)
        {
            const float OccNear = saturate((DistNear - 4.0f) / 22.0f);
            const float OccMid = saturate((DistMid - 8.0f) / 60.0f);
            const float OccFar = saturate((DistFar - 14.0f) / 135.0f);
            const float RawShadow = min(OccNear, min(OccMid, OccFar));
            const float ToonCastShadow = smoothstep(0.18f, 0.65f, RawShadow);
            CastShadow = lerp(1.0f, ToonCastShadow, SafeReceive);
        }
    }

    const float LitMask = SelfLit * CastShadow;

    // 3) Exposure-matched physical irradiance scaling (1 / PI Lambertian normalization).
    // Matches surrounding DefaultLit PBR surfaces so AutoExposure stays completely neutral.
    const float InvPi = 0.318309886f;
    const float SafeAmbient = clamp(isfinite(AmbientWeight) ? AmbientWeight : 0.34f, 0.05f, 0.85f);

    // Sky hemisphere ambient proportional to active world lighting (no artificial glow in dark scenes).
    const float Hemi = 0.5f + 0.5f * N.z;
    const float3 DaySkyAmbient = lerp(float3(0.24f, 0.27f, 0.32f), float3(0.42f, 0.50f, 0.64f), Hemi) * ActiveSunLight;
    const float3 NightSkyAmbient = lerp(float3(0.18f, 0.22f, 0.30f), float3(0.32f, 0.40f, 0.56f), Hemi) * ActiveMoonLight;
    const float3 AmbientIrradiance = (DaySkyAmbient + NightSkyAmbient) * (SafeAmbient * InvPi);
    const float3 DirectIrradiance = PrimaryLight * InvPi;

    const float3 ShadowColor = SafeAlbedo * SafeShadeTint * (AmbientIrradiance + DirectIrradiance * (SafeAmbient * 0.45f));
    const float3 LitColor = SafeAlbedo * (AmbientIrradiance + DirectIrradiance);

    const float3 FinalColor = lerp(ShadowColor, LitColor, LitMask) + SafeEmissive;
    return max(all(isfinite(FinalColor)) ? FinalColor : float3(0.0f, 0.0f, 0.0f), 0.0f);
}

#endif
