// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

namespace RagdollRecoveryMath
{
    inline float RecoveryAlpha(const float Elapsed, const float Duration)
    {
        const float T = FMath::Clamp(Elapsed / FMath::Max(0.1f, Duration), 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T);
    }

    // Support of an upright capsule against a plane. Half-height alone only works on flat ground.
    inline double CapsulePlaneLift(const FVector& Center, const FVector& Point,
        const FVector& Normal, const FVector& Up, const double HalfHeight,
        const double Radius, const double Clearance)
    {
        const double Cos = FVector::DotProduct(Normal, Up);
        if (Cos <= 0.01) return 0.0;
        const double Support = FMath::Max(0.0, HalfHeight - Radius) * Cos + Radius;
        return FMath::Max(0.0, (Support - FVector::DotProduct(Center - Point, Normal)) / Cos + Clearance);
    }

    inline FTransform RebaseRoot(const FTransform& LocalRoot, const FTransform& OldMeshWorld,
        const FTransform& NewMeshWorld)
    {
        FTransform Result = (LocalRoot * OldMeshWorld).GetRelativeTransform(NewMeshWorld);
        Result.NormalizeRotation();
        return Result;
    }

    // Put the non-deforming frame above the pelvis back in the animation's reference
    // frame. A root-only rebase preserves the endpoints but leaves a large root offset
    // cancelled by the pelvis offset. Blending their rotations/translations separately
    // makes the pelvis orbit the old mesh origin between those endpoints.
    inline bool RebasePose(TArray<FTransform>& LocalPose, const TArray<int32>& Parents,
        const TArray<FTransform>& ReferencePose, const int32 PelvisBone,
        const FTransform& OldMeshWorld, const FTransform& NewMeshWorld)
    {
        const int32 Count = LocalPose.Num();
        if (Parents.Num() != Count || ReferencePose.Num() != Count
            || !LocalPose.IsValidIndex(PelvisBone)
            || OldMeshWorld.ContainsNaN() || NewMeshWorld.ContainsNaN()) return false;

        // Validate before changing the caller's pose; parents must precede children.
        for (int32 Bone = 0; Bone < Count; ++Bone)
        {
            if (Parents[Bone] < INDEX_NONE || Parents[Bone] >= Bone
                || LocalPose[Bone].ContainsNaN() || ReferencePose[Bone].ContainsNaN()) return false;
        }

        TArray<bool> IsFrameBone;
        IsFrameBone.Init(false, Count);
        for (int32 Bone = Parents[PelvisBone]; Bone != INDEX_NONE; Bone = Parents[Bone])
            IsFrameBone[Bone] = true;

        TArray<FTransform> OldComponentPose;
        TArray<FTransform> NewComponentPose;
        OldComponentPose.SetNum(Count);
        NewComponentPose.SetNum(Count);
        for (int32 Bone = 0; Bone < Count; ++Bone)
        {
            const int32 Parent = Parents[Bone];
            OldComponentPose[Bone] = Parent == INDEX_NONE ? LocalPose[Bone]
                : LocalPose[Bone] * OldComponentPose[Parent];
            if (IsFrameBone[Bone])
            {
                LocalPose[Bone] = ReferencePose[Bone];
            }
            else if (Parent == INDEX_NONE || IsFrameBone[Parent])
            {
                const FTransform NewComponent = RebaseRoot(OldComponentPose[Bone], OldMeshWorld, NewMeshWorld);
                LocalPose[Bone] = Parent == INDEX_NONE ? NewComponent
                    : NewComponent.GetRelativeTransform(NewComponentPose[Parent]);
                LocalPose[Bone].NormalizeRotation();
            }
            // Descendants keep their exact saved local transforms, including hair and fingers.
            NewComponentPose[Bone] = Parent == INDEX_NONE ? LocalPose[Bone]
                : LocalPose[Bone] * NewComponentPose[Parent];
        }
        return true;
    }
}
