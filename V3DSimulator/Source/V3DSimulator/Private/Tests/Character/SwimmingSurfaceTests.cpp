// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/SwimmingSurfaceMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwimmingHeadSurfaceTest,
    "V3DSimulator.Character.Swimming.StableSurfaceControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSwimmingHeadSurfaceTest::RunTest(const FString&)
{
    // Resolve different rigs and sustained changes of pose without using the
    // standing-head reference. Test large world coordinates before float casts.
    for (int32 FPS : {30, 60, 120})
    for (double Level : {0.0, 1000000.0, -1000000.0})
    for (double InitialHeadOffset : {70.0, 30.0, -10.0})
    for (float Clearance : {4.0f, 10.0f})
    {
        const float Dt = 1.0f / FPS;
        double HeadOffset = InitialHeadOffset;
        double CapsuleZ = Level - HeadOffset - 30.0;
        bool bLocked = false;
        SwimmingSurfaceMath::FHeadReference Reference;
        for (int32 Frame = 0; Frame < FPS * 8; ++Frame)
        {
            if (Frame == FPS) HeadOffset -= 30.0; // Sustained lower swim/sprint pose.
            const float Depth = static_cast<float>(Level - (CapsuleZ + HeadOffset));
            TestTrue(TEXT("Surface hold survives pose changes without Up input"),
                SwimmingSurfaceMath::UpdateLock(Depth, 38.5f, false, bLocked));
            Reference.Update(static_cast<float>(HeadOffset), Dt);
            const float ReferenceDepth = static_cast<float>(Level - CapsuleZ - Reference.Offset);
            const float Step = SwimmingSurfaceMath::CorrectionDeltaZ(ReferenceDepth, Clearance, Dt);
            TestTrue(TEXT("Surface correction has a bounded speed"), FMath::Abs(Step) <= 80.0f * Dt + 0.0001f);
            CapsuleZ += Step;
        }
        const double HeadAboveWater = CapsuleZ + HeadOffset - Level;
        TestTrue(TEXT("Settled head is above water without raising the body excessively"),
            HeadAboveWater > 0.5 && FMath::Abs(HeadAboveWater - Clearance) <= 3.251);
    }

    // Stroke animation must not be converted into repeated whole-body vertical
    // movement, including after a moving swimmer returns to the idle pose.
    for (int32 FPS : {30, 60, 120})
    for (float PoseOffset : {-10.0f, 30.0f, 70.0f})
    {
        const float Dt = 1.0f / FPS;
        SwimmingSurfaceMath::FHeadReference Reference;
        Reference.Update(PoseOffset, Dt);
        double CapsuleZ = 4.0 - PoseOffset;
        const double InitialZ = CapsuleZ;
        for (int32 Frame = 0; Frame < FPS * 10; ++Frame)
        {
            // +/- 2.5 cm triangular head bob at two strokes/second.
            const float Phase = static_cast<float>(Frame % (FPS / 2)) / (FPS / 2);
            const float Bob = 2.5f * (1.0f - 4.0f * FMath::Abs(Phase - 0.5f));
            Reference.Update(PoseOffset + Bob, Dt);
            CapsuleZ += SwimmingSurfaceMath::CorrectionDeltaZ(
                static_cast<float>(-CapsuleZ - Reference.Offset), 4.0f, Dt);
            TestTrue(TEXT("Stroke bob does not move the capsule"), FMath::Abs(CapsuleZ - InitialZ) < 0.001);
        }
        // A higher idle head should lower the body again, not keep the sprint height.
        for (int32 Frame = 0; Frame < FPS * 5; ++Frame)
        {
            Reference.Update(PoseOffset + 20.0f, Dt);
            CapsuleZ += SwimmingSurfaceMath::CorrectionDeltaZ(
                static_cast<float>(-CapsuleZ - Reference.Offset), 4.0f, Dt);
        }
        TestTrue(TEXT("Returning to idle removes the excess swimming height"),
            FMath::Abs(CapsuleZ - (InitialZ - 20.0)) <= 3.251);
        Reference.Reset();
        Reference.Update(-25.0f, Dt);
        TestEqual(TEXT("Re-entry or mesh change discards the previous pose reference"), Reference.Offset, -25.0f);
    }

    bool bLocked = false;
    for (float Depth : {30.0f, 10.0f, 0.0f, -4.0f, -20.0f})
    {
        TestTrue(TEXT("Capture holds before the exact head target is reached"),
            SwimmingSurfaceMath::UpdateLock(Depth, 38.5f, false, bLocked));
        for (float UpInput : {0.1f, 0.5f, 1.0f})
            TestEqual(TEXT("Up input is ignored throughout the surface band"),
                SwimmingSurfaceMath::FilterVerticalInput(UpInput, bLocked), 0.0f);
    }
    TestEqual(TEXT("No vertical intent while idle"), SwimmingSurfaceMath::FilterVerticalInput(0.0f, true), 0.0f);
    TestEqual(TEXT("A deliberate dive is never filtered out"), SwimmingSurfaceMath::FilterVerticalInput(-1.0f, true), -1.0f);
    TestFalse(TEXT("Dive releases surface hold immediately"),
        SwimmingSurfaceMath::UpdateLock(-4.0f, 38.5f, true, bLocked));
    TestFalse(TEXT("No surface pull in deep water"),
        SwimmingSurfaceMath::UpdateLock(200.0f, 38.5f, false, bLocked));
    TestEqual(TEXT("Underwater ascent still works"), SwimmingSurfaceMath::FilterVerticalInput(1.0f, bLocked), 1.0f);

    for (float PhysicalSpeed : {-300.0f, -10.0f, 0.5f, 10.0f, 300.0f})
    {
        TestFalse(TEXT("External vertical momentum prevents waterline correction"),
            SwimmingSurfaceMath::CanCorrectHeight(PhysicalSpeed, PhysicalSpeed * 0.9f, PhysicalSpeed / 60.0));
        TestFalse(TEXT("A force applied during physics prevents waterline correction"),
            SwimmingSurfaceMath::CanCorrectHeight(0.0f, PhysicalSpeed, 0.0));
        TestFalse(TEXT("A force stopped by a collision is not pulled back in the same frame"),
            SwimmingSurfaceMath::CanCorrectHeight(PhysicalSpeed, 0.0f, 0.0));
    }
    TestFalse(TEXT("Collision/root-motion displacement is not undone"),
        SwimmingSurfaceMath::CanCorrectHeight(0.0f, 0.0f, 1.0));
    TestTrue(TEXT("Quiet surface resumes passive height hold"),
        SwimmingSurfaceMath::CanCorrectHeight(0.0f, 0.0f, 0.0));
    TestTrue(TEXT("Hitches cannot cause a large correction"),
        FMath::Abs(SwimmingSurfaceMath::CorrectionDeltaZ(70.0f, 4.0f, 1.0f)) <= 4.0f);
    TestEqual(TEXT("No movement at the target head height"),
        SwimmingSurfaceMath::CorrectionDeltaZ(-4.0f, 4.0f, 1.0f / 60.0f), 0.0f);
    TestEqual(TEXT("No movement for an invalid timestep"),
        SwimmingSurfaceMath::CorrectionDeltaZ(30.0f, 4.0f, -1.0f), 0.0f);
    TestTrue(TEXT("An overly high head settles downward"),
        SwimmingSurfaceMath::CorrectionDeltaZ(-25.0f, 4.0f, 1.0f / 60.0f) < 0.0f);
    return true;
}
#endif
