// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "Components/PrimitiveComponent.h"

namespace V3DMeshRendering
{
    // Streamed static geometry is created after lighting was built and uses movable ISMs.
    // Both raster shadows and RT visibility must survive serialized Blueprint defaults.
    inline void ConfigureWorldMesh(UPrimitiveComponent* Component)
    {
        if (!IsValid(Component)) return;
        Component->SetCastShadow(true);
        Component->SetVisibleInRayTracing(true);
        if (!Component->bCastDynamicShadow || !Component->bCastStaticShadow)
        {
            Component->bCastDynamicShadow = true;
            Component->bCastStaticShadow = true;
            Component->MarkRenderStateDirty();
        }
    }
}
