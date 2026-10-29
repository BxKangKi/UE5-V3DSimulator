// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "System/V3DStreamingPolicy.h"

namespace V3DHISMChunkPolicy
{
    constexpr double ChunkSizeCm = 1024.0 * 100.0;
    // Bound each UObject mutation/tree snapshot even in an exceptionally dense chunk.
    constexpr int32 MaxInstancesPerComponent = 256;
    inline FIntVector Key(const FVector& WorldPosition)
    {
        const auto Cell = [](double Value)
        {
            return static_cast<int32>(FMath::Clamp<double>(
                FMath::FloorToDouble(Value / ChunkSizeCm), MIN_int32, MAX_int32));
        };
        return FIntVector(Cell(WorldPosition.X), Cell(WorldPosition.Y), Cell(WorldPosition.Z));
    }
    inline FBox CellBounds(const FIntVector& Cell)
    {
        const FVector Min(double(Cell.X) * ChunkSizeCm, double(Cell.Y) * ChunkSizeCm,
            double(Cell.Z) * ChunkSizeCm);
        return FBox(Min, Min + FVector(ChunkSizeCm));
    }
    inline bool InRange(const FBox& Bounds, const TArray<FVector>& Observers, double DistanceCm)
    {
        if (!Bounds.IsValid || !FMath::IsFinite(DistanceCm) || DistanceCm < 0.0) return false;
        for (const FVector& Observer : Observers)
            if (Bounds.ComputeSquaredDistanceToPoint(Observer) <= FMath::Square(DistanceCm)) return true;
        return false;
    }
    // Coarse scene admission contains the far corner of every selected child cell.
    inline bool SceneInRange(const FVector& Center, const FVector& Size, const FTransform& Transform,
        const FVector& Observer, double Distance, double Hysteresis = 1.0,
        double MaxDistanceCm = V3DStreamingPolicy::DefaultMaxRenderDistanceCm)
    {
        const FVector Half = Size.GetAbs() * 0.5;
        const FBox Bounds = FBox(Center - Half, Center + Half).TransformBy(Transform);
        const double Radius = FMath::Max(FMath::Min(MaxDistanceCm, ChunkSizeCm),
            V3DStreamingPolicy::LoadRadius(Size, Transform.GetScale3D().GetAbs().GetMax(), Distance, MaxDistanceCm))
            * FMath::Clamp(Hysteresis, 1.0, 2.0) + 1.7320508075688772 * ChunkSizeCm;
        return Bounds.ComputeSquaredDistanceToPoint(Observer) <= FMath::Square(FMath::Min(MaxDistanceCm, Radius));
    }

}
