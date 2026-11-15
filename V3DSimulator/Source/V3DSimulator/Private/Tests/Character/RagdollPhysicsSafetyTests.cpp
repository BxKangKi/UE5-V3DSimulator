// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Character/CharacterController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/RigidBodyIndexPair.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollCollisionExclusionRemapTest,
    "V3DSimulator.Character.Ragdoll.PreserveAuthoredCollisionExclusions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRagdollCollisionExclusionRemapTest::RunTest(const FString&)
{
    TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
    TStrongObjectPtr<UPhysicsAsset> Asset(NewObject<UPhysicsAsset>());
    FReferenceSkeleton Reference;
    {
        FReferenceSkeletonModifier Modifier(Reference, nullptr);
        Modifier.Add(FMeshBoneInfo(TEXT("hips"), TEXT("hips"), INDEX_NONE), FTransform::Identity);
        Modifier.Add(FMeshBoneInfo(TEXT("chest"), TEXT("chest"), 0), FTransform(FVector(0, 0, 30)));
        Modifier.Add(FMeshBoneInfo(TEXT("head"), TEXT("head"), 1), FTransform(FVector(0, 0, 20)));
    }
    Mesh->SetRefSkeleton(Reference);
    // Removing the missing bone and duplicate shifts every retained index.
    for (const TCHAR* Name : {TEXT("missing"), TEXT("hips"), TEXT("chest"), TEXT("head"), TEXT("hips")})
    {
        USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(Asset.Get());
        Body->BoneName = FName(Name);
        Asset->SkeletalBodySetups.Add(Body);
    }
    Asset->DisableCollision(1, 3); // hips/head are not joined directly but intentionally overlap.
    Asset->DisableCollision(0, 2); // must disappear with the missing body.
    Asset->CollisionDisableTable.Add(FRigidBodyIndexPair(0, 999), true); // stale index must be ignored.
    const auto AddConstraint = [&Asset](const TCHAR* A, const TCHAR* B)
    {
        UPhysicsConstraintTemplate* Joint = NewObject<UPhysicsConstraintTemplate>(Asset.Get());
        Joint->DefaultInstance.ConstraintBone1 = FName(A);
        Joint->DefaultInstance.ConstraintBone2 = FName(B);
        Asset->ConstraintSetup.Add(Joint);
    };
    AddConstraint(TEXT("hips"), TEXT("chest"));
    AddConstraint(TEXT("head"), TEXT("head")); // invalid self-constraint
    TestTrue(TEXT("Sanitize succeeds"), UCharacterFunctionLibrary::SanitizeRuntimePhysicsAsset(Asset.Get(), Mesh.Get()));
    TestEqual(TEXT("Three unique valid bodies survive"), Asset->SkeletalBodySetups.Num(), 3);
    TestEqual(TEXT("Self-constraint removed"), Asset->ConstraintSetup.Num(), 1);
    const auto HasExclusion = [&Asset](const TCHAR* A, const TCHAR* B)
    {
        return Asset->CollisionDisableTable.Contains(FRigidBodyIndexPair(
            Asset->FindBodyIndex(FName(A)), Asset->FindBodyIndex(FName(B))));
    };
    TestTrue(TEXT("Non-adjacent authored exclusion survives reindexing"), HasExclusion(TEXT("hips"), TEXT("head")));
    TestTrue(TEXT("Joint neighbors cannot collide"), HasExclusion(TEXT("hips"), TEXT("chest")));
    TestFalse(TEXT("Unexcluded pair stays unchanged"), HasExclusion(TEXT("chest"), TEXT("head")));
    TestEqual(TEXT("Invalid old indices removed"), Asset->CollisionDisableTable.Num(), 2);
    UCharacterFunctionLibrary::SanitizeRuntimePhysicsAsset(Asset.Get(), Mesh.Get());
    TestTrue(TEXT("Repeated sanitize preserves authored exclusion"), HasExclusion(TEXT("hips"), TEXT("head")));
    TestEqual(TEXT("Repeated sanitize is stable"), Asset->CollisionDisableTable.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollActivationGuardTest,
    "V3DSimulator.Character.Ragdoll.RejectDegenerateActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRagdollActivationGuardTest::RunTest(const FString&)
{
    // An unregistered component keeps deliberately invalid scale out of Chaos.
    TStrongObjectPtr<USkeletalMeshComponent> Mesh(NewObject<USkeletalMeshComponent>());
    Mesh->SetRelativeScale3D(FVector(1, 0, 1));
    FString Reason;
    TestFalse(TEXT("Zero-scale pose cannot enter ragdoll"), UCharacterFunctionLibrary::ValidateRagdollActivation(*Mesh, Reason));
    TestTrue(TEXT("Degenerate transform is identified"), Reason.Contains(TEXT("transform")));
    Mesh->SetRelativeScale3D(FVector::OneVector);
    TestFalse(TEXT("Missing physics bodies cannot enter ragdoll"), UCharacterFunctionLibrary::ValidateRagdollActivation(*Mesh, Reason));
    TestTrue(TEXT("Missing bodies are identified"), Reason.Contains(TEXT("bodies")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFirstPersonPhysicsPolicyTest,
    "V3DSimulator.Character.FirstPerson.VisibilityDoesNotChangePhysics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFirstPersonPhysicsPolicyTest::RunTest(const FString&)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    ACharacterController* Character = World->SpawnActor<ACharacterController>();
    if (!TestNotNull(TEXT("Character"), Character))
    {
        World->DestroyWorld(false);
        return false;
    }
    USkeletalMeshComponent* Mesh = Character->GetMesh();
    Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
    Mesh->bEnableUpdateRateOptimizations = true;
    const FTransform InitialTransform = Mesh->GetRelativeTransform();
    const auto InitialCollision = Mesh->GetCollisionEnabled();
    // The same visibility policy must preserve normal and full-ragdoll ownership.
    for (const auto Mode : {EKinematicBonesUpdateToPhysics::SkipSimulatingBones, EKinematicBonesUpdateToPhysics::SkipAllBones})
    {
        Mesh->KinematicBonesUpdateType = Mode;
        Character->SetFirstPersonEnabled(true);
        Character->SetFirstPersonEnabled(true); // idempotent: do not replace saved third-person options.
        TestTrue(TEXT("Hidden first-person mesh continues bone refresh"),
            Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
        TestFalse(TEXT("First-person pose is not update-rate skipped"), bool(Mesh->bEnableUpdateRateOptimizations));
        TestTrue(TEXT("Only owner rendering is hidden"), bool(Mesh->bOwnerNoSee));
        TestTrue(TEXT("Camera mode does not move or zero-scale physics mesh"), Mesh->GetRelativeTransform().Equals(InitialTransform));
        TestTrue(TEXT("Camera mode preserves physics ownership"), Mesh->KinematicBonesUpdateType == Mode);
        TestTrue(TEXT("Camera mode preserves collision"), Mesh->GetCollisionEnabled() == InitialCollision);
        Character->SetFirstPersonEnabled(false);
        TestTrue(TEXT("Third-person tick policy restored"),
            Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);
        TestTrue(TEXT("Third-person update-rate policy restored"), bool(Mesh->bEnableUpdateRateOptimizations));
        TestFalse(TEXT("Third-person owner can see the mesh"), bool(Mesh->bOwnerNoSee));
    }
    World->DestroyWorld(false);
    return true;
}
#endif
