// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHairPhysicsMassAndIsolationTest,
    "V3DSimulator.Character.Hair.MassAndIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHairPhysicsMassAndIsolationTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
    TStrongObjectPtr<UPhysicsAsset> Asset(NewObject<UPhysicsAsset>());
    FReferenceSkeleton Reference;
    {
        FReferenceSkeletonModifier Modifier(Reference, nullptr);
        Modifier.Add(FMeshBoneInfo(TEXT("Root"), TEXT("Root"), INDEX_NONE), FTransform::Identity);
        Modifier.Add(FMeshBoneInfo(TEXT("head"), TEXT("head"), 0), FTransform(FVector(0, 0, 20)));
        Modifier.Add(FMeshBoneInfo(TEXT("rig:Hair_Root"), TEXT("rig:Hair_Root"), 1), FTransform(FVector(0, 0, 5)));
        Modifier.Add(FMeshBoneInfo(TEXT("segment01"), TEXT("segment01"), 2), FTransform(FVector(0, 0, -10)));
        Modifier.Add(FMeshBoneInfo(TEXT("segment02"), TEXT("segment02"), 3), FTransform(FVector(0, 0, -10)));
    }
    Mesh->SetRefSkeleton(Reference);
    USkeletalBodySetup* Head = NewObject<USkeletalBodySetup>(Asset.Get());
    Head->BoneName = TEXT("head");
    Head->DefaultInstance.SetMassOverride(8.0f, true);
    Asset->SkeletalBodySetups.Add(Head);
    UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(Asset.Get(), Mesh.Get(), TEXT("hairRoot"));

    TestEqual(TEXT("Head plus three hair bodies"), Asset->SkeletalBodySetups.Num(), 4);
    for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
    {
        if (Body == Head) continue;
        TestTrue(TEXT("Hair has an explicit mass override"), bool(Body->DefaultInstance.bOverrideMass));
        TestTrue(TEXT("Each hair body is one gram"),
            FMath::IsNearlyEqual(Body->DefaultInstance.GetMassOverride(), 0.001f, UE_SMALL_NUMBER));
        TestEqual(TEXT("Linear drag is applied to every generated hair segment"),
            Body->DefaultInstance.LinearDamping, 0.75f);
        TestEqual(TEXT("Angular drag damps repeated swaying"),
            Body->DefaultInstance.AngularDamping, 3.0f);
        TestEqual(TEXT("Hair ignores physical world contacts"),
            Body->DefaultInstance.GetResponseToChannel(ECC_WorldStatic), ECR_Ignore);
        TestEqual(TEXT("Hair ignores character contacts"),
            Body->DefaultInstance.GetResponseToChannel(ECC_Pawn), ECR_Ignore);
    }
    TestEqual(TEXT("Authored head mass remains intact"), Head->DefaultInstance.GetMassOverride(), 8.0f);
    TestEqual(TEXT("Every hair segment including holder has a joint"), Asset->ConstraintSetup.Num(), 3);
    for (const UPhysicsConstraintTemplate* Constraint : Asset->ConstraintSetup)
    {
        TestEqual(TEXT("Swing 1 stays within the reduced envelope"), Constraint->DefaultInstance.GetAngularSwing1Limit(), 30.0f);
        TestEqual(TEXT("Swing 2 stays within the reduced envelope"), Constraint->DefaultInstance.GetAngularSwing2Limit(), 30.0f);
        TestEqual(TEXT("Twist stays within the reduced envelope"), Constraint->DefaultInstance.GetAngularTwistLimit(), 18.0f);
        TestTrue(TEXT("Child hair cannot drive its parent"), bool(Constraint->DefaultInstance.ProfileInstance.bParentDominates));
        TestEqual(TEXT("No contact force transfer to the head"),
            Constraint->DefaultInstance.ProfileInstance.ContactTransferScale, 0.0f);
    }

    // Repeated setup must not duplicate bodies or constraints on a mesh reload.
    UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(Asset.Get(), Mesh.Get(), TEXT("hairRoot"));
    TestEqual(TEXT("No duplicate bodies after repeated setup"), Asset->SkeletalBodySetups.Num(), 4);
    TestEqual(TEXT("No duplicate joints after repeated setup"), Asset->ConstraintSetup.Num(), 3);
    return true;
}
#endif
