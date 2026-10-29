// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/MacroLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterCrouchRegressionTest,
    "V3DSimulator.Character.Crouch.PoseAndBlockedRelease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCharacterCrouchRegressionTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;

    ACharacterController* Character = World->SpawnActor<ACharacterController>();
    if (!TestNotNull(TEXT("Native character"), Character))
    {
        World->DestroyWorld(false);
        return false;
    }
    Character->DispatchBeginPlay();
    UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
    UCharacterComponent* State = Character->GetCharacterComponent();
    USkeletalMeshComponent* Mesh = Character->GetMesh();
    const FVector StandingLocation = Mesh->GetRelativeLocation();
    const FQuat StandingRotation = Mesh->GetRelativeRotation().Quaternion();
    TestTrue(TEXT("Standing offset exists before the first gameplay audit"),
        StandingLocation.Equals(FVector(0, 0, -90)));
    TestTrue(TEXT("Native mesh faces the expected direction"),
        StandingRotation.Equals(FRotator(0, 270, 0).Quaternion()));
    Movement->SetMovementMode(MOVE_Walking);

    // A press/release before the movement tick must cancel pending intent.
    State->UpdateComponent(1.0f / 60.0f, FVector::ZeroVector, STATE_CROUCH, -10000.0f);
    TestTrue(TEXT("Crouch intent queued"), bool(Movement->bWantsToCrouch));
    State->ResetMovementState();
    TestFalse(TEXT("Release cancels an unprocessed crouch"), bool(Movement->bWantsToCrouch));

    for (int32 Cycle = 0; Cycle < 20; ++Cycle)
    {
        Character->SetActorRotation(FRotator(0, Cycle * 37.0f, 0));
        State->UpdateComponent(1.0f / 60.0f, FVector::ZeroVector, STATE_CROUCH, -10000.0f);
        Movement->Crouch(false);
        TestTrue(TEXT("Capsule commits crouch"), Character->bIsCrouched);
        TestTrue(TEXT("Crouch preserves mesh rotation"), Mesh->GetRelativeRotation().Quaternion().Equals(StandingRotation));
        const FVector CrouchedLocation = Mesh->GetRelativeLocation();
        TestTrue(TEXT("Mesh compensates for capsule shrink"), CrouchedLocation.Z > StandingLocation.Z);

        AActor* Ceiling = World->SpawnActor<AActor>();
        UBoxComponent* Box = NewObject<UBoxComponent>(Ceiling);
        Ceiling->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(300, 300, 10));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Box->SetCollisionResponseToAllChannels(ECR_Block);
        Box->RegisterComponent();
        UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
        Ceiling->SetActorLocation(Capsule->GetComponentLocation()
            + FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight() + 11));

        State->UpdateComponent(1.0f / 60.0f, FVector::ZeroVector, 0, -10000.0f);
        TestTrue(TEXT("Release does not move the mesh before capsule acceptance"),
            Mesh->GetRelativeLocation().Equals(CrouchedLocation));
        Movement->UnCrouch(false);
        TestTrue(TEXT("Low ceiling keeps the character crouched"), Character->bIsCrouched);
        TestTrue(TEXT("Blocked release preserves crouched mesh height"), Mesh->GetRelativeLocation().Equals(CrouchedLocation));
        Ceiling->Destroy();
        Movement->UnCrouch(false);
        TestFalse(TEXT("Cleared headroom permits standing"), Character->bIsCrouched);
        TestTrue(TEXT("No accumulated crouch offset"), Mesh->GetRelativeLocation().Equals(StandingLocation));
        TestTrue(TEXT("No accumulated mesh rotation"), Mesh->GetRelativeRotation().Quaternion().Equals(StandingRotation));
    }
    World->DestroyWorld(false);
    return true;
}
#endif
