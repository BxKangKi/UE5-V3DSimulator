// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include <algorithm>

// Native policy shared by settings, the UI and renderer integration. Distances are metres.
namespace V3DShadowDistance
{
    constexpr int Minimum = 256;
    constexpr int Maximum = 8192;
    constexpr int Step = 256;
    constexpr int Default = 2048;

    inline int Normalize(int Metres)
    {
        const int Clamped = std::max(Minimum, std::min(Maximum, Metres));
        return std::min(Maximum, ((Clamped + Step / 2) / Step) * Step);
    }

    inline int Profile(int Quality)
    {
        return 512 << std::max(0, std::min(3, Quality));
    }

    inline float EffectiveCentimetres(int Metres, int RenderDistanceMetres)
    {
        return static_cast<float>(std::min(Normalize(Metres), std::max(0, RenderDistanceMetres))) * 100.0f;
    }

    inline int LastClipmapLevel(float Centimetres)
    {
        int Level = 6;
        // A VSM clipmap has a radius of 2^Level cm. Include the requested distance.
        while (Level < 22 && static_cast<float>(1 << Level) < Centimetres) ++Level;
        return Level;
    }
}
