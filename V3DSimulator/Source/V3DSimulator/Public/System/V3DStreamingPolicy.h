// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
class UObject;

/** Pure geometry shared by stream selection and readiness; sizes are full local dimensions. */
namespace V3DStreamingPolicy
{
    constexpr double MinScreenDiameter = 0.05;

    // Same projected bounding-sphere diameter convention as UE's mesh screen size.
    inline double ScreenSizeDistance(const double ProjectionScale)
    {
        return FMath::Max(0.0, ProjectionScale) / MinScreenDiameter - 1.0;
    }

    // Game-thread camera snapshot. Workers receive only the returned scalar.
    V3DSIMULATOR_API float GetScreenSizeDistance(const UObject* WorldContext);

    inline double LoadRadius(const FVector& Size, const double MaxWorldScale, const double Distance)
    {
        const double BoundRadius = 0.5 * Size.GetAbs().Size() * FMath::Max(0.0, MaxWorldScale);
        // Do not cap large visible meshes at a fixed number of metres. Keep a small local
        // collision neighbourhood even for sub-pixel props and degenerate bounds.
        return FMath::Max(BoundRadius + 2000.0,
            BoundRadius * FMath::Max(1.0, Distance + 1.0));
    }
    inline bool SceneInRange(const FVector& Center, const FVector& Size, const FTransform& Transform,
        const FVector& Observer, const double Distance, const double Hysteresis = 1.0)
    {
        const FVector Half = Size.GetAbs() * 0.5;
        const FBox Bounds = FBox(Center - Half, Center + Half).TransformBy(Transform);
        const double Radius = LoadRadius(Size, Transform.GetScale3D().GetAbs().GetMax(), Distance);
        // Coarse scene selection must contain every child mesh's screen-size range.
        return Bounds.ComputeSquaredDistanceToPoint(Observer)
            <= FMath::Square(Radius * FMath::Clamp(Hysteresis, 1.0, 2.0));
    }
}
