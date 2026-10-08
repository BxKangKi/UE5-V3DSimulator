// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

namespace SwimmingSurfaceMath
{
    // A stroke moves the head even when the capsule is stationary. Average the
    // evaluated bone offset, then hold the reference across small pose changes.
    // Sustained changes (idle -> swimming/sprint, or a different rig) still settle.
    struct FHeadReference
    {
        bool bInitialized = false;
        float SmoothedOffset = 0.0f;
        float Offset = 0.0f;

        void Reset() { *this = FHeadReference(); }

        void Update(const float HeadOffset, const float DeltaSeconds)
        {
            if (!FMath::IsFinite(HeadOffset) || !FMath::IsFinite(DeltaSeconds)
                || DeltaSeconds <= 0.0f) return;
            if (!bInitialized)
            {
                SmoothedOffset = Offset = HeadOffset;
                bInitialized = true;
                return;
            }
            const float Alpha = 1.0f - FMath::Exp(-2.0f * FMath::Min(DeltaSeconds, 0.05f));
            SmoothedOffset += (HeadOffset - SmoothedOffset) * Alpha;
            if (FMath::Abs(SmoothedOffset - Offset) > 3.0f) Offset = SmoothedOffset;
        }
    };

    inline float FilterVerticalInput(const float Input, const bool bSurfaceHeld)
    {
        // Only locomotion intent is filtered. Physical velocity is never a ceiling.
        return bSurfaceHeld ? FMath::Min(Input, 0.0f) : Input;
    }

    inline bool CanCorrectHeight(const float BeforeVelocityZ, const float AfterVelocityZ,
        const double PhysicsDeltaZ)
    {
        // A force, impulse, root motion or collision response owns its displacement.
        // Do not pull the capsule back to the waterline while it is moving vertically.
        return FMath::Abs(BeforeVelocityZ) <= 0.1f
            && FMath::Abs(AfterVelocityZ) <= 0.1f
            && FMath::Abs(PhysicsDeltaZ) <= 0.001;
    }

    // Depth is measured from the evaluated head bone to the horizontal water plane.
    inline bool UpdateLock(const float HeadDepth, const float CaptureDepth,
        const bool bWantsDown, bool& bLocked)
    {
        if (!FMath::IsFinite(HeadDepth) || bWantsDown)
        {
            bLocked = false;
            return false;
        }
        const float Band = FMath::Max(1.0f, CaptureDepth);
        // A swimming-pose change can lower the head after capture. Give that pose
        // room to settle, but do not pull a deeply submerged swimmer to the surface.
        bLocked = HeadDepth <= (bLocked ? Band * 2.0f : Band);
        return bLocked;
    }

    inline float CorrectionDeltaZ(const float HeadDepth, const float Clearance,
        const float DeltaSeconds)
    {
        if (!FMath::IsFinite(HeadDepth) || !FMath::IsFinite(Clearance)
            || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return 0.0f;
        const float Error = HeadDepth + FMath::Max(0.0f, Clearance);
        if (FMath::Abs(Error) <= 0.25f) return 0.0f;
        // Limit hitches as well as normal frames. The movement component sweeps this
        // displacement after swimming physics; braking cannot erase passive lift.
        const float StepTime = FMath::Min(DeltaSeconds, 0.05f);
        const float Alpha = 1.0f - FMath::Exp(-6.0f * StepTime);
        const float MaxStep = 80.0f * StepTime;
        return FMath::Clamp(Error * Alpha, -MaxStep, MaxStep);
    }
}
