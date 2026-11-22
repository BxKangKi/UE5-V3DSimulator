// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#ifndef V3D_TOON_HLSL
#define V3D_TOON_HLSL
#pragma once
// /Plugin/ShaderLibrary/Toon.hlsl

// Shared world-normal and primary Sun/Moon directional light resolver.
float3 V3DToonResolveNormal(FMaterialPixelParameters Parameters, float3 WorldNormalInput)
{
    const float NormalLenSq = dot(WorldNormalInput, WorldNormalInput);
    const float3 FallbackNormal = Parameters.WorldNormal;
    const float FallbackLenSq = dot(FallbackNormal, FallbackNormal);
    return (isfinite(NormalLenSq) && NormalLenSq > 1.0e-6f)
        ? (WorldNormalInput * rsqrt(NormalLenSq))
        : ((isfinite(FallbackLenSq) && FallbackLenSq > 1.0e-6f)
            ? (FallbackNormal * rsqrt(FallbackLenSq))
            : float3(0.0f, 0.0f, 1.0f));
}

void V3DToonResolveLights(
    out float3 OutPrimaryDir,
    out float3 OutPrimaryLight,
    out float3 OutActiveSunLight,
    out float3 OutActiveMoonLight,
    out float OutPrimaryLuma)
{
    const float3 SunDirRaw = ResolvedView.AtmosphereLightDirection[0].xyz;
    const float SunDirLenSq = dot(SunDirRaw, SunDirRaw);
    const float3 SunDir = (isfinite(SunDirLenSq) && SunDirLenSq > 1.0e-6f)
        ? (SunDirRaw * rsqrt(SunDirLenSq))
        : float3(0.0f, 0.0f, 1.0f);
    const float3 SunColor = max(all(isfinite(ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[0].rgb))
        ? ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[0].rgb : float3(0.0f, 0.0f, 0.0f), 0.0f);
    const float SunHorizon = smoothstep(-0.06f, 0.14f, SunDir.z);
    OutActiveSunLight = SunColor * SunHorizon;

    const float3 MoonDirRaw = ResolvedView.AtmosphereLightDirection[1].xyz;
    const float MoonDirLenSq = dot(MoonDirRaw, MoonDirRaw);
    const float3 MoonDir = (isfinite(MoonDirLenSq) && MoonDirLenSq > 1.0e-6f)
        ? (MoonDirRaw * rsqrt(MoonDirLenSq))
        : float3(0.0f, 0.0f, 1.0f);
    const float3 MoonColor = max(all(isfinite(ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[1].rgb))
        ? ResolvedView.AtmosphereLightIlluminanceOnGroundPostTransmittance[1].rgb : float3(0.0f, 0.0f, 0.0f), 0.0f);
    const float MoonHorizon = smoothstep(-0.04f, 0.16f, MoonDir.z) * (1.0f - SunHorizon);
    OutActiveMoonLight = MoonColor * MoonHorizon;

    const float SunLuma = dot(OutActiveSunLight, float3(0.2126f, 0.7152f, 0.0722f));
    const float MoonLuma = dot(OutActiveMoonLight, float3(0.2126f, 0.7152f, 0.0722f));
    const bool bUseSun = SunLuma >= MoonLuma;
    OutPrimaryDir = bUseSun ? SunDir : MoonDir;
    OutPrimaryLight = bUseSun ? OutActiveSunLight : OutActiveMoonLight;
    OutPrimaryLuma = max(SunLuma, MoonLuma);
}

// Shadow-receiving DefaultLit Toon BaseColor.
// M_Toon keeps Metallic=0, Specular=0, Roughness=1 so PBR specular sheen stays zero,
// while outputting cel-banded diffuse albedo to BaseColor lets Unreal Engine's native
// shadow maps (CSM / Virtual Shadow Maps / dynamic & skeletal mesh shadows) cast real
// shadows onto Toon geometry regardless of Mesh Distance Fields.
float3 V3DToonComputeBaseColor(
    FMaterialPixelParameters Parameters,
    float3 Albedo,
    float3 ShadeTint,
    float3 WorldNormalInput,
    float ShadowThreshold,
    float ShadowSmoothness)
{
    const float3 SafeAlbedo = max(all(isfinite(Albedo)) ? Albedo : float3(0.5f, 0.5f, 0.5f), 0.0f);
    const float3 SafeShadeTint = saturate(all(isfinite(ShadeTint)) ? ShadeTint : float3(0.55f, 0.60f, 0.72f));
    const float3 N = V3DToonResolveNormal(Parameters, WorldNormalInput);

    float3 PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight;
    float PrimaryLuma;
    V3DToonResolveLights(PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight, PrimaryLuma);

    const float Threshold = clamp(isfinite(ShadowThreshold) ? ShadowThreshold : 0.08f, -0.5f, 0.8f);
    const float Smoothness = clamp(isfinite(ShadowSmoothness) ? ShadowSmoothness : 0.06f, 0.005f, 0.5f);
    const float NdotL = dot(N, PrimaryDir);
    const float SelfLit = smoothstep(Threshold - Smoothness, Threshold + Smoothness, NdotL);

    // In pitch darkness (no Sun/Moon), keep full albedo active so local point/spot lights
    // still illuminate and cast shadows from any direction.
    const float CelActive = smoothstep(0.01f, 0.15f, PrimaryLuma);
    const float EffectiveLit = lerp(1.0f, SelfLit, CelActive);

    // Flatten the lit Lambertian falloff toward a crisp cel band while staying within GBuffer [0,1].
    const float CelCompensation = lerp(1.0f, min(rcp(max(NdotL, 0.35f)), 1.65f), 0.65f * CelActive);
    const float3 BackBase = SafeAlbedo * SafeShadeTint * 0.08f;
    const float3 LitBase = SafeAlbedo * CelCompensation;
    const float3 Result = lerp(BackBase, LitBase, EffectiveLit);
    return saturate(all(isfinite(Result)) ? Result : float3(0.0f, 0.0f, 0.0f));
}

// Companion stylized shadow-tint & ambient floor for M_Toon EmissiveColor.
// Direct sunlight is handled by UE DefaultLit + real shadow maps via V3DToonComputeBaseColor;
// this output supplies only the stylized cel shadow tone + ExtraEmissive so cast-shadowed
// pixels drop cleanly into the toon shade color.
float3 V3DToonComputeShadowEmissive(
    FMaterialPixelParameters Parameters,
    float3 Albedo,
    float3 ShadeTint,
    float3 WorldNormalInput,
    float ShadowThreshold,
    float ShadowSmoothness,
    float AmbientWeight,
    float3 ExtraEmissive)
{
    const float3 SafeAlbedo = max(all(isfinite(Albedo)) ? Albedo : float3(0.5f, 0.5f, 0.5f), 0.0f);
    const float3 SafeShadeTint = saturate(all(isfinite(ShadeTint)) ? ShadeTint : float3(0.55f, 0.60f, 0.72f));
    const float3 SafeEmissive = max(all(isfinite(ExtraEmissive)) ? ExtraEmissive : float3(0.0f, 0.0f, 0.0f), 0.0f);
    const float3 N = V3DToonResolveNormal(Parameters, WorldNormalInput);

    float3 PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight;
    float PrimaryLuma;
    V3DToonResolveLights(PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight, PrimaryLuma);

    const float Threshold = clamp(isfinite(ShadowThreshold) ? ShadowThreshold : 0.08f, -0.5f, 0.8f);
    const float Smoothness = clamp(isfinite(ShadowSmoothness) ? ShadowSmoothness : 0.06f, 0.005f, 0.5f);
    const float NdotL = dot(N, PrimaryDir);
    const float SelfLit = smoothstep(Threshold - Smoothness, Threshold + Smoothness, NdotL);

    const float InvPi = 0.318309886f;
    const float SafeAmbient = clamp(isfinite(AmbientWeight) ? AmbientWeight : 0.34f, 0.05f, 0.85f);
    const float Hemi = 0.5f + 0.5f * N.z;
    const float3 DaySkyAmbient = lerp(float3(0.24f, 0.27f, 0.32f), float3(0.42f, 0.50f, 0.64f), Hemi) * ActiveSunLight;
    const float3 NightSkyAmbient = lerp(float3(0.18f, 0.22f, 0.30f), float3(0.32f, 0.40f, 0.56f), Hemi) * ActiveMoonLight;
    const float3 AmbientIrradiance = (DaySkyAmbient + NightSkyAmbient) * (SafeAmbient * InvPi);
    const float3 DirectIrradiance = PrimaryLight * InvPi;

    // Reduce emissive floor on lit-facing surfaces because BaseColor already receives SkyLight indirect diffuse;
    // when a cast shadow blocks direct light on a lit-facing surface, it drops smoothly to match the self-shadow tone.
    const float FloorScale = lerp(0.85f, 0.35f, SelfLit);
    const float3 ShadowFloor = SafeAlbedo * SafeShadeTint * (AmbientIrradiance * 0.70f + DirectIrradiance * (SafeAmbient * 0.20f)) * FloorScale;
    const float3 FinalColor = ShadowFloor + SafeEmissive;
    return max(all(isfinite(FinalColor)) ? FinalColor : float3(0.0f, 0.0f, 0.0f), 0.0f);
}

// Legacy single-output entry point kept for full compatibility with existing MF_Toon graphs.
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
    const float3 N = V3DToonResolveNormal(Parameters, WorldNormalInput);

    float3 PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight;
    float PrimaryLuma;
    V3DToonResolveLights(PrimaryDir, PrimaryLight, ActiveSunLight, ActiveMoonLight, PrimaryLuma);

    // 1) Self-shadow cel band from primary directional light.
    const float Threshold = clamp(isfinite(ShadowThreshold) ? ShadowThreshold : 0.08f, -0.5f, 0.8f);
    const float Smoothness = clamp(isfinite(ShadowSmoothness) ? ShadowSmoothness : 0.06f, 0.005f, 0.5f);
    const float NdotL = dot(N, PrimaryDir);
    const float SelfLit = smoothstep(Threshold - Smoothness, Threshold + Smoothness, NdotL);

    // 2) Cast shadow reception from surrounding geometry via 3-tap DistanceToNearestSurface.
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
    const float InvPi = 0.318309886f;
    const float SafeAmbient = clamp(isfinite(AmbientWeight) ? AmbientWeight : 0.34f, 0.05f, 0.85f);

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
