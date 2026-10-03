// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "UObject/StrongObjectPtr.h"

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
    World->DestroyWorld(false);
    return true;
}
#endif
