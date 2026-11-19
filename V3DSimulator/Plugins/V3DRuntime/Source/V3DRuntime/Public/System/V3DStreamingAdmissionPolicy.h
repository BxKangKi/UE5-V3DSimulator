// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

namespace V3DStreamingAdmissionPolicy
{
    // Bounded backfill keeps small loads moving without starving the oldest large request.
    template<typename EstimateAt>
    int32 FindRequest(const int32 Count, const int32 Active, const int32 Limit,
        const int64 Used, const int64 Budget, const int32 HeadBypasses, EstimateAt&& Estimate)
    {
        if (Count <= 0 || Active >= Limit) return INDEX_NONE;
        if (Active == 0) return 0; // An oversized request is permitted only in isolation.
        const int32 ScanCount = HeadBypasses >= 8 ? 1 : FMath::Min(Count, 64);
        for (int32 I = 0; I < ScanCount; ++I)
        {
            const int64 Bytes = Estimate(I);
            if (Bytes <= Budget && Used <= Budget - Bytes) return I;
        }
        return INDEX_NONE;
    }
}
