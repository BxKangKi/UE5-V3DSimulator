// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#ifndef SHADER_LIBRARY_HLSL
#define SHADER_LIBRARY_HLSL
#pragma once
#if POST_PROCESS_MATERIAL
#include "/Plugin/ShaderLibrary/Underwater.ush"
#endif
// /Plugin/ShaderLibrary/ShaderLibrary.hlsl

float InvLerp(float a, float b, float v)
{
    const float denom = b - a;
    return (abs(denom) > 1.0e-6f) ? ((v - a) / denom) : 0.0f;
}

float3 NormalToTangent(float3 NormalMap)
{
    return normalize(NormalMap * 2.0f - 1.0f);
}

float2 ParticlePosToScreenUV(float3 World, float3 View, float FlipY)
{
    const float3 TranslatedWorld = World + LWCHackToFloat(ResolvedView.PreViewTranslation);
    const float4 clipPos = mul(float4(TranslatedWorld, 1.0f), ResolvedView.TranslatedWorldToClip);

    const float w = (clipPos.w < 0.0f ? -1.0f : 1.0f) * max(abs(clipPos.w), 1e-6f);
    const float3 ndc = clipPos.xyz / w;

    const float2 uvScale = float2(0.5f, FlipY ? -0.5f : 0.5f);
    const float2 uvBias = float2(0.5f, FlipY ? 1.0f : 0.0f);

    return saturate(ndc.xy * uvScale + uvBias);
}

float3 WorldToTangent(float3 TangentWS, float3 NormalWS, float3 NormalMap)
{
    TangentWS = normalize(TangentWS);
    NormalWS = normalize(NormalWS);
    const float3 BinormalWS = normalize(cross(NormalWS, TangentWS));
    const float3 TangentNormal = normalize(NormalMap * 2.0f - 1.0f);

    return normalize(
        TangentWS * TangentNormal.x +
        BinormalWS * TangentNormal.y +
        NormalWS * TangentNormal.z);
}

float3x3 TransformMatrix(float3 X, float3 Y, float3 Z)
{
    return float3x3(X.x, Y.x, Z.x, X.y, Y.y, Z.y, X.z, Y.z, Z.z);
}

float4 NormalFromBase(float3 L, float3 R, float3 T, float3 B, float3 C)
{
    float3x3 ColorLRT = TransformMatrix(L, R, T);
    float3x3 ColorBC = TransformMatrix(B, C, 0);

    const float3 Weight = float3(0.299f, 0.587f, 0.114f);
    const float3 heightsLRT = mul(ColorLRT, Weight);
    const float3 heightsBC = mul(ColorBC, Weight);

    const float dX = heightsLRT[1] - heightsLRT[0];
    const float dY = heightsLRT[2] - heightsBC[0];
    const float HeightCenter = heightsBC[1];

    float3 normal = normalize(float3(-dX, -dY, 1.0f));
    normal.z = saturate(normal.z);

    const float3 normalMap = normal * 0.5f + 0.5f;
    return float4(HeightCenter, normalMap.g, -normalMap.b, normalMap.r);
}

float CausticsFadeOut(float Z, float MinZ, float MaxZ, float3 ObjPos)
{
    Z = abs(ObjPos.z - Z);
    return saturate(InvLerp(MaxZ, MinZ, Z)) * saturate(InvLerp(0.0f, MinZ, Z));
}

float2 AltasUV(float GridSize, float Index, float2 UV)
{
    const float SafeGrid = max(GridSize, 1.0f);
    const float CellSize = rcp(SafeGrid);
    const float SafeIndex = floor(Index + 0.5f);

    float2 Offset;
    Offset.x = fmod(SafeIndex, SafeGrid);
    Offset.y = floor(SafeIndex * CellSize);

    const float2 TiledUV = frac(UV);
    const float Inset = 0.01f;

    const float2 CellMin = Offset * CellSize + Inset;
    const float2 CellMax = (Offset + 1.0f) * CellSize - Inset;

    return lerp(CellMin, CellMax, TiledUV);
}

#endif