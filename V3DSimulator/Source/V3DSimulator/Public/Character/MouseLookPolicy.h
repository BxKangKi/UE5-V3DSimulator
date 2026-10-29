// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include <cmath>
#include <limits>

/** Numeric camera input policy; independent of UObject/world lifetime. */
namespace V3DMouseLook
{
    constexpr float DefaultSensitivity = 1.0f;
    constexpr float MinimumSensitivity = 0.05f;
    constexpr float MaximumSensitivity = 5.0f;

    inline float NormalizeSensitivity(const float Value)
    {
        if (!std::isfinite(Value)) return DefaultSensitivity;
        return Value < MinimumSensitivity ? MinimumSensitivity
            : Value > MaximumSensitivity ? MaximumSensitivity : Value;
    }

    inline float EffectiveSensitivity(const float Base, const float Saved)
    {
        const float Effective = (std::isfinite(Base) ? Base : DefaultSensitivity) * NormalizeSensitivity(Saved);
        return std::isfinite(Effective) ? Effective : DefaultSensitivity;
    }

    /** Validate before narrowing LWC/Blueprint inputs into the float camera API. */
    inline bool TryScaleInput(const double X, const double Y, const float Sensitivity,
        float& OutYaw, float& OutPitch)
    {
        OutYaw = OutPitch = 0.0f;
        if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Sensitivity)) return false;
        const double Yaw = X * Sensitivity;
        const double Pitch = Y * Sensitivity;
        const double MaximumDelta = std::numeric_limits<float>::max();
        if (!std::isfinite(Yaw) || !std::isfinite(Pitch)
            || std::abs(Yaw) > MaximumDelta || std::abs(Pitch) > MaximumDelta) return false;
        OutYaw = static_cast<float>(Yaw);
        OutPitch = static_cast<float>(Pitch);
        return true;
    }
}
