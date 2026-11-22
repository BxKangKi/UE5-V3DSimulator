// Stable, world-anchored multi-octave slope normals for T_Water_H with quality scaling.
#ifndef V3D_WATER_SURFACE_HLSL
#define V3D_WATER_SURFACE_HLSL

float2 V3DSampleWaterSlope(Texture2D WaveTexture, SamplerState WaveSampler, float2 UV)
{
    return Texture2DSample(WaveTexture, WaveSampler, UV).rg - 0.5f;
}

float3 V3DWaterSurfaceNormalQuality(Texture2D WaveTexture, SamplerState WaveSampler,
    float2 Position, float Time, float Speed, float NearScale, float FarScale,
    float NearStrength, float FarStrength, float ViewDepth, float BlendStart, float BlendLength,
    float QualityLevel)
{
    const float safeNearScale = max(abs(NearScale), 100.0f);
    const float safeFarScale = max(abs(FarScale), 100.0f);
    const float2 nearUV = Position / safeNearScale;
    const float2 farUV = Position / safeFarScale;
    const float t = Time * clamp(Speed, -4.0f, 4.0f);

    // Primary cross-scrolling ocean waves (all quality tiers).
    const float2 nearA = V3DSampleWaterSlope(WaveTexture, WaveSampler, nearUV + t * float2(0.042f, 0.024f));
    const float2 rotUV1 = float2(-nearUV.y * 0.82f + nearUV.x * 0.57f, nearUV.x * 0.82f + nearUV.y * 0.57f) * 1.18f;
    float2 nearB = V3DSampleWaterSlope(WaveTexture, WaveSampler, rotUV1 + t * float2(-0.031f, 0.039f));
    nearB = float2(nearB.x * 0.57f + nearB.y * 0.82f, -nearB.x * 0.82f + nearB.y * 0.57f);

    float2 nearSlope = (nearA + nearB) * 0.55f;

    // Medium+ (QualityLevel >= 1): 3rd swell octave breaks tiling and restores organic wave rolling.
    [branch] if (QualityLevel >= 0.5f)
    {
        const float2 swellUV = float2(nearUV.x * 0.61f - nearUV.y * 0.43f, nearUV.x * 0.43f + nearUV.y * 0.61f);
        float2 swell = V3DSampleWaterSlope(WaveTexture, WaveSampler, swellUV + t * float2(0.018f, -0.027f));
        nearSlope = nearSlope * 0.78f + swell * 0.42f;
    }

    // High+ (QualityLevel >= 2): 4th fine wind ripple octave for crisp sunlight glints & realistic motion.
    [branch] if (QualityLevel >= 1.5f)
    {
        const float2 rippleUV = float2(nearUV.x * 1.92f + nearUV.y * 1.31f, -nearUV.x * 1.31f + nearUV.y * 1.92f);
        const float2 ripple = V3DSampleWaterSlope(WaveTexture, WaveSampler, rippleUV + t * float2(-0.072f, -0.054f));
        const float rippleFade = 1.0f - saturate(max(ViewDepth, 0.0f) / 3200.0f);
        nearSlope += ripple * (0.28f * rippleFade);
    }

    // Epic (QualityLevel >= 3): subtle high-frequency capillary micro-ripples up close.
    [branch] if (QualityLevel >= 2.5f)
    {
        const float2 microUV = nearUV * 3.65f;
        const float2 micro = V3DSampleWaterSlope(WaveTexture, WaveSampler, microUV + t * float2(0.115f, -0.095f));
        const float microFade = 1.0f - saturate(max(ViewDepth, 0.0f) / 1400.0f);
        nearSlope += micro * (0.16f * microFade);
    }

    // Distant ocean swells (2 counter-drifting layers on High+ to prevent flat/static horizon bands).
    float2 farSlope = V3DSampleWaterSlope(WaveTexture, WaveSampler, farUV + t * float2(0.015f, -0.011f));
    [branch] if (QualityLevel >= 1.5f)
    {
        const float2 farUV2 = float2(-farUV.y, farUV.x) * 1.42f;
        float2 farB = V3DSampleWaterSlope(WaveTexture, WaveSampler, farUV2 + t * float2(-0.012f, 0.014f));
        farB = float2(farB.y, -farB.x);
        farSlope = (farSlope + farB) * 0.62f;
    }

    float blend = saturate((max(ViewDepth, 0.0f) - max(BlendStart, 0.0f)) / max(BlendLength, 100.0f));
    blend = blend * blend * (3.0f - 2.0f * blend);

    float2 slope = lerp(
        nearSlope * (1.85f * clamp(NearStrength, 0.0f, 1.5f)),
        farSlope * (2.10f * clamp(FarStrength, 0.0f, 1.5f)),
        blend);

    if (!all(isfinite(slope))) return float3(0.0f, 0.0f, 1.0f);
    slope = clamp(slope, -0.65f, 0.65f);
    return float3(slope, 1.0f) * rsqrt(1.0f + dot(slope, slope));
}

#endif
