// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include <algorithm>
#include <cmath>
namespace V3DAcousticsMath
{
    struct FRoomEstimate { float DecaySeconds = .1f; float Wet = 0; };
    /** Opposing probe distances in metres, hit bits +X,-X,+Y,-Y,+Z,-Z.
     * A Sabine room-box approximation, not a wave solver or diffraction simulation. */
    inline FRoomEstimate Estimate(const float* Distances, unsigned HitMask, float Absorption)
    {
        FRoomEstimate Result;
        if (!Distances || !std::isfinite(Absorption) || Absorption <= 0) return Result;
        for (int I = 0; I < 6; ++I) if (!std::isfinite(Distances[I]) || Distances[I] < 0) return Result;
        const double X = std::max(1., double(Distances[0]) + Distances[1]);
        const double Y = std::max(1., double(Distances[2]) + Distances[3]);
        const double Z = std::max(1., double(Distances[4]) + Distances[5]);
        Result.DecaySeconds = float(std::clamp(.161 * X * Y * Z / (2 * (X*Y + X*Z + Y*Z) * std::clamp(double(Absorption), .02, 1.)), .1, 8.));
        int Hits = 0; for (int I = 0; I < 6; ++I) if (HitMask & (1u << I)) ++Hits;
        if ((HitMask & (1u << 4)) && Hits >= 4) Result.Wet = float(Hits * Hits) / 36.f * .65f;
        return Result;
    }
}
