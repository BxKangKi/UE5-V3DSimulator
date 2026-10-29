// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

namespace V3DVehicleSuspensionPolicy
{
    // Neutral authored geometry must not depend on the order in which wheels are assembled,
    // or include RideHeightOffset (the runtime target applies that setting once).
    inline float AuthoredRestLength(float Radius, float RestLength, float CompressionTravel,
        float DroopTravel, float VisualRestRatio)
    {
        const float Minimum = FMath::Max(3.0f, Radius * 0.30f);
        const float Compression = FMath::Clamp(CompressionTravel, 2.0f, 30.0f);
        const float Rest = FMath::Max(FMath::Max(4.0f, RestLength),
            Minimum + Compression + FMath::Clamp(DroopTravel, 4.0f, 80.0f));
        return FMath::Clamp(FMath::Max(Rest * FMath::Clamp(VisualRestRatio, 0.55f, 0.90f),
            Minimum + Compression), Minimum + 1.0f, Rest - 1.0f);
    }

    inline float BottomOutDepth(float MinimumLength, float MeasuredLength)
    {
        return FMath::Max(0.0f, MinimumLength - MeasuredLength);
    }

    inline float StepCompressionDepth(float TargetLength, float MeasuredLength)
    {
        return FMath::Max(0.0f, TargetLength - MeasuredLength - FMath::Max(1.0f, TargetLength * 0.05f));
    }
}
