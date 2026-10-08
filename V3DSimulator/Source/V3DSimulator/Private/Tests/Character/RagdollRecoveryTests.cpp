// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/RagdollRecoveryMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollRebaseWorldPoseTest,
    "V3DSimulator.Character.Ragdoll.RebasePreservesWorldPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRagdollRebaseWorldPoseTest::RunTest(const FString& Parameters)
{
    for (double Distance : {0.0, 1000000.0, -1000000.0})
    {
        const FTransform OldFrame(FRotator(10, 55, 25), FVector(Distance, -420, -800), FVector(1.2));
        const FTransform NewFrame(FRotator(0, 145, 0), FVector(Distance + 110, -390, -900), FVector(1.2));
        const FTransform Root(FRotator(15, -20, 4), FVector(35, -12, 80));
        const FTransform Child(FRotator(5, 0, 2), FVector(0, 0, 24));
        const FTransform Rebased = RagdollRecoveryMath::RebaseRoot(Root, OldFrame, NewFrame);
        TestTrue(TEXT("Root world pose survives frame change"), (Rebased * NewFrame).Equals(Root * OldFrame, 0.001));
        TestTrue(TEXT("Descendant world pose survives frame change"),
            (Child * Rebased * NewFrame).Equals(Child * Root * OldFrame, 0.001));
        // The native recovery proxy starts at the saved world pose even if the capsule
        // has moved/turned to its standing frame. It ends exactly at the evaluated animation.
        const FTransform Animation(FRotator(0, 0, 0), FVector(0, 0, 0));
        FTransform FirstOutput;
        FirstOutput.Blend(Animation, Rebased, 1.0f);
        TestTrue(TEXT("First native recovery frame cannot jump with the capsule"),
            (FirstOutput * NewFrame).Equals(Root * OldFrame, 0.001));
        FTransform LastOutput;
        LastOutput.Blend(Animation, Rebased, 0.0f);
        TestTrue(TEXT("Completed recovery is exactly the evaluated animation"), LastOutput.Equals(Animation, 0.001));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollRecoveryPlacementTest,
    "V3DSimulator.Character.Ragdoll.SlopeClearanceAndWaterBlend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRagdollRecoveryPlacementTest::RunTest(const FString&)
{
    const FVector Up = FVector::UpVector;
    const FVector Normal = FVector(0.7, 0.0, 0.7).GetSafeNormal();
    const FVector Center(0, 0, 96);
    const double Lift = RagdollRecoveryMath::CapsulePlaneLift(Center, FVector::ZeroVector, Normal, Up, 96, 42, 6);
    TestTrue(TEXT("Uphill capsule hemisphere clears a 45 degree slope"), Lift > 20.0);
    const FVector BottomSphere = Center + Up * Lift - Up * (96.0 - 42.0);
    TestTrue(TEXT("Entire bottom sphere lies above the slope"), FVector::DotProduct(BottomSphere, Normal) >= 42.0);
    TestEqual(TEXT("Water blend midpoint uses a normalized pose alpha"), RagdollRecoveryMath::RecoveryAlpha(1.5f, 3.0f), 0.5f);
    TestEqual(TEXT("Water blend starts without a jump"), RagdollRecoveryMath::RecoveryAlpha(0.0f, 3.0f), 0.0f);
    TestEqual(TEXT("Water blend reaches the exact final pose"), RagdollRecoveryMath::RecoveryAlpha(3.0f, 3.0f), 1.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollRecoveryNoExcursionTest,
    "V3DSimulator.Character.Ragdoll.RecoveryDoesNotOrbitOldOrigin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRagdollRecoveryNoExcursionTest::RunTest(const FString&)
{
    // Root -> hips -> chest, plus a second branch attached directly to Root.
    // The ragdoll has travelled 20 metres away from its detached component origin.
    const TArray<int32> Parents = {INDEX_NONE, 0, 1, 0};
    const TArray<FTransform> Reference = {
        FTransform::Identity, FTransform(FVector(0, 0, 90)),
        FTransform(FVector(0, 0, 35)), FTransform(FVector(0, 20, 0))};
    const TArray<FTransform> Saved = {
        FTransform::Identity, FTransform(FVector(2000, 0, 90)),
        Reference[2], FTransform(FVector(2000, 20, 0))};

    for (double Distance : {0.0, 1000000.0, -1000000.0})
    for (double Yaw : {0.0, 90.0, 179.0, -179.0})
    for (double Tilt : {0.0, 70.0})
    for (double Scale : {0.5, 1.0, 1.2})
    {
        const FTransform OldFrame(FRotator(Tilt, 0, 0), FVector(Distance, -420, -800), FVector(Scale));
        const FTransform NewFrame(
            (OldFrame.GetRotation() * FRotator(0, Yaw, 0).Quaternion()).GetNormalized(),
            OldFrame.TransformPosition(FVector(2000, 0, 0)), FVector(Scale));
        TArray<FTransform> Rebased = Saved;
        TestTrue(TEXT("Recovery pose rebases"), RagdollRecoveryMath::RebasePose(
            Rebased, Parents, Reference, 1, OldFrame, NewFrame));
        TestTrue(TEXT("Root uses the animation frame"), Rebased[0].Equals(Reference[0], 0.001));
        TestTrue(TEXT("Pelvis world transform is preserved"),
            (Rebased[1] * Rebased[0] * NewFrame).Equals(Saved[1] * Saved[0] * OldFrame, 0.001));
        TestTrue(TEXT("Chest world transform is preserved"),
            (Rebased[2] * Rebased[1] * Rebased[0] * NewFrame)
                .Equals(Saved[2] * Saved[1] * Saved[0] * OldFrame, 0.001));
        TestTrue(TEXT("Sibling branch world transform is preserved"),
            (Rebased[3] * Rebased[0] * NewFrame).Equals(Saved[3] * Saved[0] * OldFrame, 0.001));

        // Check the entire land and water blend, not only the endpoints. Both endpoints
        // have the same pelvis position, so no intermediate sample may leave that point.
        for (float Duration : {0.35f, 3.0f})
        for (int32 Sample = 0; Sample <= 120; ++Sample)
        {
            const float Weight = 1.0f - RagdollRecoveryMath::RecoveryAlpha(Duration * Sample / 120.0f, Duration);
            FTransform Root, Hips;
            Root.Blend(Reference[0], Rebased[0], Weight);
            Hips.Blend(Reference[1], Rebased[1], Weight);
            TestTrue(TEXT("Pelvis stays at the release position throughout recovery"),
                (Hips * Root * NewFrame).GetLocation().Equals(
                    (Saved[1] * Saved[0] * OldFrame).GetLocation(), 0.001));
        }
    }

    // A skeleton whose pelvis is itself the root must keep its physical root pose.
    TArray<FTransform> RootPelvis = {Saved[1]};
    TestTrue(TEXT("Root-pelvis skeleton is supported"), RagdollRecoveryMath::RebasePose(
        RootPelvis, {INDEX_NONE}, {Reference[1]}, 0, FTransform::Identity, FTransform::Identity));
    TestTrue(TEXT("Physical root is not replaced by reference pose"), RootPelvis[0].Equals(Saved[1]));
    return true;
}

#endif
