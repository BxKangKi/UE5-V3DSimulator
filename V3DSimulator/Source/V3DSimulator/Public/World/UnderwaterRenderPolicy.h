// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include <cmath>

namespace V3DUnderwaterRender
{
    inline bool SuppressScreenShafts(bool OceanActive, bool MaterialReady,
        double CameraZ, double WaterZ, bool PreviouslyUnderwater)
    {
        if (!OceanActive || !MaterialReady || !std::isfinite(CameraZ) || !std::isfinite(WaterZ)) return false;
        // Ten-centimetre hysteresis avoids toggling render state repeatedly along the waterline.
        return CameraZ - WaterZ < (PreviouslyUnderwater ? 5.0 : -5.0);
    }

    // Underwater volumetric light shafts are enabled from Medium quality (1) and above.
    inline bool AllowUnderwaterShafts(int EffectsQuality, int PostProcessingQuality)
    {
        return EffectsQuality >= 1 && PostProcessingQuality >= 1;
    }
}
