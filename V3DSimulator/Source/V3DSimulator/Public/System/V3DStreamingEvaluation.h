// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

/** Inputs that can change instance selection for immutable built-world scene metadata. */
struct FV3DStreamingEvaluationInputs
{
    TArray<FVector> Observers;
    FTransform OwnerTransform = FTransform::Identity;
    double ScreenDistance = 0.0;
    double MaxRenderDistanceCm = 0.0;
    float UnloadMultiplier = 1.10f;
    bool bRenderOnly = false;

    bool Matches(const FV3DStreamingEvaluationInputs& Other) const
    {
        if (Observers.Num() != Other.Observers.Num()
            || !OwnerTransform.Equals(Other.OwnerTransform)
            || ScreenDistance != Other.ScreenDistance
            || MaxRenderDistanceCm != Other.MaxRenderDistanceCm
            || UnloadMultiplier != Other.UnloadMultiplier
            || bRenderOnly != Other.bRenderOnly) return false;
        for (int32 Index = 0; Index < Observers.Num(); ++Index)
            if (!Observers[Index].Equals(Other.Observers[Index], 0.01)) return false;
        return true;
    }
};

/** An unchanged, completed view needs no further worker pass. Invalidation survives completion. */
class FV3DStreamingEvaluation
{
public:
    bool NeedsEvaluation(const FV3DStreamingEvaluationInputs& Inputs) const
    {
        return !bValid || !LastInputs.Matches(Inputs);
    }
    void Capture(FV3DStreamingEvaluationInputs&& Inputs)
    {
        LastInputs = MoveTemp(Inputs);
        bValid = true;
    }
    void Invalidate() { bValid = false; }
    void Reset() { LastInputs = FV3DStreamingEvaluationInputs(); bValid = false; }
    bool HasSnapshot() const { return bValid; }
    const FV3DStreamingEvaluationInputs& GetInputs() const { return LastInputs; }

private:
    FV3DStreamingEvaluationInputs LastInputs;
    bool bValid = false;
};
