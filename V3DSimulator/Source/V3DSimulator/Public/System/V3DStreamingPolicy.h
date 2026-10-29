// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
class UObject;

/** Pure geometry shared by stream selection and readiness; sizes are full local dimensions. */
namespace V3DStreamingPolicy
{
    constexpr double MinScreenDiameter = 0.05;
    constexpr int32 RenderDistanceStepMeters = 1024;
    constexpr int32 MinRenderDistanceMeters = 1024;
    constexpr int32 MaxRenderDistanceMeters = 32768;
    constexpr int32 DefaultMaxRenderDistanceMeters = 8192;
    constexpr double DefaultMaxRenderDistanceCm = DefaultMaxRenderDistanceMeters * 100.0;

    inline int32 NormalizeMaxRenderDistanceMeters(const int32 Meters)
    {
        const int32 Clamped = FMath::Clamp(Meters, MinRenderDistanceMeters, MaxRenderDistanceMeters);
        return ((Clamped + RenderDistanceStepMeters / 2) / RenderDistanceStepMeters) * RenderDistanceStepMeters;
    }

    // Game-thread settings snapshot. Never query UObjects from a stream-planning worker.
    V3DSIMULATOR_API double GetMaxRenderDistanceCm(const UObject* WorldContext);

    inline bool BoundsInRenderRange(const FVector& Center, const FVector& Size,
        const FTransform& Transform, const FVector& Observer, const double MaxDistanceCm)
    {
        const FVector Half = Size.GetAbs() * 0.5;
        const FBox Bounds = FBox(Center - Half, Center + Half).TransformBy(Transform);
        // Distance to the nearest bound, not to the pivot: intersecting large meshes stay visible.
        return Bounds.ComputeSquaredDistanceToPoint(Observer) <= FMath::Square(MaxDistanceCm);
    }

    // Same projected bounding-sphere diameter convention as UE's mesh screen size.
    inline double ScreenSizeDistance(const double ProjectionScale)
    {
        return FMath::Max(0.0, ProjectionScale) / MinScreenDiameter - 1.0;
    }

    // Game-thread camera snapshot. Workers receive only the returned scalar.
    V3DSIMULATOR_API float GetScreenSizeDistance(const UObject* WorldContext);

    inline double LoadRadius(const FVector& Size, const double MaxWorldScale, const double Distance,
        const double MaxDistanceCm = DefaultMaxRenderDistanceCm)
    {
        const double BoundRadius = 0.5 * Size.GetAbs().Size() * FMath::Max(0.0, MaxWorldScale);
        // Broad-phase bucket radius includes the bounds. The exact bound check below also
        // caps always-loaded meshes and unload hysteresis at the configured maximum.
        return FMath::Min(MaxDistanceCm + BoundRadius,
            FMath::Max(BoundRadius + 2000.0, BoundRadius * FMath::Max(1.0, Distance + 1.0)));
    }

    inline bool MeshInRange(const FVector& Size, const FTransform& Transform,
        const FVector& Observer, const double Distance, const double MaxDistanceCm,
        const bool bAlwaysLoaded = false, const double Hysteresis = 1.0)
    {
        if (!BoundsInRenderRange(FVector::ZeroVector, Size, Transform, Observer, MaxDistanceCm))
            return false;
        const double Radius = LoadRadius(Size, Transform.GetScale3D().GetAbs().GetMax(), Distance, MaxDistanceCm);
        return bAlwaysLoaded || FVector::DistSquared(Observer, Transform.GetLocation())
            <= FMath::Square(Radius * FMath::Clamp(Hysteresis, 1.0, 2.0));
    }
    inline bool SceneInRange(const FVector& Center, const FVector& Size, const FTransform& Transform,
        const FVector& Observer, const double Distance, const double Hysteresis = 1.0,
        const double MaxDistanceCm = DefaultMaxRenderDistanceCm)
    {
        const FVector Half = Size.GetAbs() * 0.5;
        const FBox Bounds = FBox(Center - Half, Center + Half).TransformBy(Transform);
        if (Bounds.ComputeSquaredDistanceToPoint(Observer) > FMath::Square(MaxDistanceCm))
            return false;
        const double Radius = LoadRadius(Size, Transform.GetScale3D().GetAbs().GetMax(), Distance, MaxDistanceCm);
        // Coarse scene selection must contain every child mesh's screen-size range.
        return Bounds.ComputeSquaredDistanceToPoint(Observer)
            <= FMath::Square(Radius * FMath::Clamp(Hysteresis, 1.0, 2.0));
    }
}
