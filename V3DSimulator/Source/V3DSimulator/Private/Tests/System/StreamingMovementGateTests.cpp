// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/BoxComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStreamingMovementModeRestoreTest,
    "V3DSimulator.World.StreamingGate.RestoreCustomMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DStreamingMovementModeRestoreTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>();
    ACharacter* Character = World->SpawnActor<ACharacter>();
    if (!TestNotNull(TEXT("Movement gate"), Gate) || !TestNotNull(TEXT("Character"), Character))
    {
        World->DestroyWorld(false);
        return false;
    }
    Character->DispatchBeginPlay();
    UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
    const FVector Velocity(120, 30, 0);
    Movement->SetMovementMode(MOVE_Custom, 7);
    Movement->Velocity = Velocity;
    TStrongObjectPtr<UObject> RegionOwner(NewObject<UObject>());
    const FBox Bounds(FVector(-1000), FVector(1000));
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("test"), Bounds, false);
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("Unloaded region suspends movement"), Movement->MovementMode == MOVE_None);
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("test"), Bounds, true);
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("Custom movement mode restored"), Movement->MovementMode == MOVE_Custom);
    TestEqual(TEXT("Custom sub-mode restored"), Movement->CustomMovementMode, uint8(7));
    TestTrue(TEXT("Saved velocity restored"), Movement->Velocity.Equals(Velocity));
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("test"), Bounds, false);
    Gate->Tick(1.0f / 60.0f);
    Gate->UnregisterMovable(Character);
    TestTrue(TEXT("Unregistration also restores custom movement"), Movement->MovementMode == MOVE_Custom && Movement->CustomMovementMode == 7);
    Gate->ClearModelRegions(RegionOwner.Get());

    // A character's root does not simulate; its independently moving child bodies still must
    // freeze, and movement must be suspended at the same time (not an else-if alternative).
    UBoxComponent* BodyA = NewObject<UBoxComponent>(Character);
    UBoxComponent* BodyB = NewObject<UBoxComponent>(Character);
    for (UBoxComponent* Body : {BodyA, BodyB})
    {
        Character->AddInstanceComponent(Body);
        Body->SetBoxExtent(FVector(10));
        Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Body->SetCollisionResponseToAllChannels(ECR_Ignore);
        Body->RegisterComponent();
        Body->SetSimulatePhysics(true);
    }
    const FVector SpeedA(320, 15, -80), SpeedB(-40, 210, -10);
    const FVector SpinA(1, 2, 3), SpinB(3, 1, 2);
    BodyA->SetPhysicsLinearVelocity(SpeedA);
    BodyB->SetPhysicsLinearVelocity(SpeedB);
    BodyA->SetPhysicsAngularVelocityInRadians(SpinA);
    BodyB->SetPhysicsAngularVelocityInRadians(SpinB);
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("bodies"), Bounds, false);
    Gate->Tick(1.0f / 60.0f);
    TestFalse(TEXT("First body paused"), BodyA->IsSimulatingPhysics());
    TestFalse(TEXT("Second body paused"), BodyB->IsSimulatingPhysics());
    TestTrue(TEXT("Movement also paused while physics bodies exist"), Movement->MovementMode == MOVE_None);
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("bodies"), Bounds, true);
    Gate->Tick(1.0f / 60.0f);
    TestTrue(TEXT("First independent velocity restored"), BodyA->GetPhysicsLinearVelocity().Equals(SpeedA, 0.01));
    TestTrue(TEXT("Second independent velocity restored"), BodyB->GetPhysicsLinearVelocity().Equals(SpeedB, 0.01));
    TestTrue(TEXT("First independent spin restored"), BodyA->GetPhysicsAngularVelocityInRadians().Equals(SpinA, 0.01));
    TestTrue(TEXT("Second independent spin restored"), BodyB->GetPhysicsAngularVelocityInRadians().Equals(SpinB, 0.01));
    Gate->SetModelRegionAvailable(RegionOwner.Get(), TEXT("bodies"), Bounds, false);
    Gate->Tick(1.0f / 60.0f);
    BodyB->DestroyComponent();
    Gate->UnregisterMovable(Character);
    TestTrue(TEXT("Surviving body resumes after sibling destruction"), BodyA->IsSimulatingPhysics());
    Gate->ClearModelRegions(RegionOwner.Get());
    World->DestroyWorld(false);
    return true;
}
#endif
