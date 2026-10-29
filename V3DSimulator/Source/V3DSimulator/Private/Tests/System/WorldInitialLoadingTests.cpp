// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/V3DStreamingEvaluation.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DInitialLoadingStableViewTest,
    "V3DSimulator.World.Startup.StableView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DInitialLoadingStableViewTest::RunTest(const FString&)
{
    FV3DStreamingEvaluationInputs View;
    View.Observers.Add(FVector::ZeroVector);
    View.ScreenDistance = 30.0;
    View.MaxRenderDistanceCm = 819200.0;
    FV3DStreamingEvaluation Evaluation;
    TestTrue(TEXT("New scene requires its first pass"), Evaluation.NeedsEvaluation(View));
    Evaluation.Capture(FV3DStreamingEvaluationInputs(View));
    for (int32 Poll = 0; Poll < 1000; ++Poll)
        if (Evaluation.NeedsEvaluation(View))
        {
            AddError(TEXT("Stationary loading screen restarted a completed scene"));
            return false;
        }
    auto Changed = View;
    Changed.Observers[0].X += 100.0;
    TestTrue(TEXT("Movement still streams new candidates"), Evaluation.NeedsEvaluation(Changed));
    Changed = View;
    Changed.Observers.Add(FVector(100000.0, 0.0, 0.0));
    TestTrue(TEXT("Teleport destination is additive"), Evaluation.NeedsEvaluation(Changed));
    Changed = View; Changed.ScreenDistance *= 2.0;
    TestTrue(TEXT("FOV change reevaluates screen threshold"), Evaluation.NeedsEvaluation(Changed));
    Changed = View; Changed.MaxRenderDistanceCm = 102400.0;
    TestTrue(TEXT("Distance setting still culls loaded instances"), Evaluation.NeedsEvaluation(Changed));
    Changed = View; Changed.UnloadMultiplier = 1.5f;
    TestTrue(TEXT("Unload hysteresis changes invalidate"), Evaluation.NeedsEvaluation(Changed));
    Changed = View; Changed.bRenderOnly = true;
    TestTrue(TEXT("Streaming mode is part of the key"), Evaluation.NeedsEvaluation(Changed));
    Changed = View; Changed.OwnerTransform = FTransform(FVector(1, 0, 0));
    TestTrue(TEXT("Moving the scene also invalidates"), Evaluation.NeedsEvaluation(Changed));
    // Camera A -> B -> A while a mesh is being built can skip an instance at B. The final input
    // values alone are insufficient: the callback's explicit invalidation must survive at A.
    Evaluation.Invalidate();
    TestTrue(TEXT("A late skipped callback must retry even after returning to the old view"), Evaluation.NeedsEvaluation(View));
    Evaluation.Capture(FV3DStreamingEvaluationInputs(View));
    TestFalse(TEXT("New completed evaluation can be reused"), Evaluation.NeedsEvaluation(View));
    Evaluation.Reset();
    TestTrue(TEXT("Changing world/model starts a fresh pass"), Evaluation.NeedsEvaluation(View));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DInitialLoadingStaggeredScenesTest,
    "V3DSimulator.World.Startup.StaggeredScenes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DInitialLoadingStaggeredScenesTest::RunTest(const FString&)
{
    FV3DStreamingEvaluationInputs View;
    View.Observers.Add(FVector::ZeroVector);
    FV3DStreamingEvaluation Scenes[32];
    bool Active[32] = {};
    bool Ready[32] = {};
    int32 Starts = 0;
    // Scenes complete on different frames. Completed scenes must not compete with the last
    // scene for worker/time budgets or make the all-scenes readiness condition oscillate.
    for (int32 Frame = 0; Frame < 300; ++Frame)
    {
        for (int32 Scene = 0; Scene < 32; ++Scene)
        {
            if (!Active[Scene] && Scenes[Scene].NeedsEvaluation(View))
            {
                Scenes[Scene].Capture(FV3DStreamingEvaluationInputs(View));
                Active[Scene] = true;
                Ready[Scene] = false;
                ++Starts;
            }
            if (Active[Scene] && Frame >= Scene + 1)
            {
                Active[Scene] = false;
                Ready[Scene] = !Scenes[Scene].NeedsEvaluation(View);
            }
        }
        if (Frame >= 32)
            for (const bool bReady : Ready)
                if (!bReady) { AddError(TEXT("A completed scene stopped being ready")); return false; }
    }
    TestEqual(TEXT("Only one pass per scene for the stationary spawn view"), Starts, 32);
    return true;
}
#endif
