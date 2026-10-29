// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Gravity/GravityFieldTypes.h"
#include "Gravity/GravityFieldComponent.h"
#include "Gravity/GravityFieldSubsystem.h"
#include "Character/CharacterController.h"
#include "Character/CharacterControllerMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DGravityMathTest, "V3DSimulator.Gravity.RadialMathAndValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DGravityMathTest::RunTest(const FString& Parameters)
{
    FGravityFieldSettings Settings;
    Settings.bEnabled = true;
    FVector Acceleration;
    double Strength, Distance;
    TestTrue(TEXT("Inside field"), V3DGravityMath::Evaluate(Settings, FVector::ZeroVector, FVector(500,0,0), Acceleration, Strength, Distance));
    TestTrue(TEXT("Attracts toward source"), Acceleration.Equals(FVector(-980,0,0), 0.001));
    TestFalse(TEXT("Outside field"), V3DGravityMath::Evaluate(Settings, FVector::ZeroVector, FVector(1001,0,0), Acceleration, Strength, Distance));
    TestTrue(TEXT("Exact centre remains an active field"), V3DGravityMath::Evaluate(Settings, FVector::ZeroVector, FVector::ZeroVector, Acceleration, Strength, Distance));
    TestTrue(TEXT("Centre has finite zero acceleration"), Acceleration.IsZero());
    Settings.Falloff = EGravityFieldFalloff::Linear;
    V3DGravityMath::Evaluate(Settings, FVector::ZeroVector, FVector(500,0,0), Acceleration, Strength, Distance);
    TestTrue(TEXT("Linear half radius gives half strength"), FMath::IsNearlyEqual(Strength, 490.0));
    for (const FVector Up : { FVector::UpVector, FVector::DownVector, FVector::RightVector, FVector::ForwardVector })
    {
        const FQuat Upright = V3DGravityMath::UprightRotation(FQuat::Identity, Up);
        TestTrue(TEXT("Upright frame follows gravity including poles"), Upright.GetUpVector().Equals(Up, 0.0001));
        TestTrue(TEXT("Forward is tangent"), FMath::Abs(FVector::DotProduct(Upright.GetForwardVector(), Up)) < 0.0001);
    }
    auto Root = MakeShared<FJsonObject>();
    auto Field = MakeShared<FJsonObject>();
    Root->SetObjectField(TEXT("GravityField"), Field);
    Field->SetBoolField(TEXT("Enabled"), true);
    Field->SetNumberField(TEXT("RadiusCm"), -1);
    FString Error;
    TestFalse(TEXT("Negative radius rejected"), FGravityFieldSettings::ReadJson(Root, Settings, Error));
    TestFalse(TEXT("Parse failure leaves safe disabled settings"), Settings.bEnabled);
    Field->SetNumberField(TEXT("RadiusCm"), 1000);
    Field->SetNumberField(TEXT("Priority"), 1.5);
    TestFalse(TEXT("Fractional priority rejected"), FGravityFieldSettings::ReadJson(Root, Settings, Error));
    Field->SetNumberField(TEXT("Priority"), 2);
    TestTrue(TEXT("Canonical settings accepted"), FGravityFieldSettings::ReadJson(Root, Settings, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DGravityCharacterTest, "V3DSimulator.Gravity.CharacterAxesCrouchAndSourceLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DGravityCharacterTest::RunTest(const FString& Parameters)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    auto* Character = World->SpawnActor<ACharacterController>();
    auto* Source = World->SpawnActor<AActor>();
    auto* SourceRoot = NewObject<USceneComponent>(Source);
    Source->SetRootComponent(SourceRoot);
    SourceRoot->RegisterComponent();
    auto* Field = NewObject<UGravityFieldComponent>(Source);
    Source->AddInstanceComponent(Field);
    Field->RegisterComponent();
    Source->DispatchBeginPlay();
    Character->DispatchBeginPlay();
    auto* Movement = CastChecked<UCharacterControllerMovementComponent>(Character->GetCharacterMovement());
    auto* Fields = World->GetSubsystem<UGravityFieldSubsystem>();
    if (!TestNotNull(TEXT("Field registry"), Fields)) { World->DestroyWorld(false); return false; }
    FGravityFieldSettings Settings;
    Settings.bEnabled = true;
    Settings.RadiusCm = 10000;
    Field->SetSettings(Settings);
    const FQuat MeshRest = Character->GetMesh()->GetRelativeRotation().Quaternion();
    for (const FVector Up : { FVector::UpVector, FVector::RightVector, FVector::DownVector, FVector::ForwardVector })
    {
        Source->SetActorLocation(-Up * 1000);
        Character->SetActorLocation(FVector::ZeroVector);
        Character->SetActorRotation(V3DGravityMath::UprightRotation(FQuat::Identity, Up));
        Movement->RefreshGravity();
        TestTrue(TEXT("Character selects radial down"), Movement->GetGravityDirection().Equals(-Up, 0.0001));
        Movement->SetMovementMode(MOVE_Walking);
        Movement->bWantsToCrouch = true;
        Movement->Crouch(false);
        TestTrue(TEXT("Crouch succeeds on all axes"), Character->bIsCrouched);
        TestTrue(TEXT("Crouch preserves imported mesh rotation"), Character->GetMesh()->GetRelativeRotation().Quaternion().Equals(MeshRest));
        Movement->bWantsToCrouch = false;
        Movement->UnCrouch(false);
        TestFalse(TEXT("Crouch releases on all axes"), Character->bIsCrouched);
    }
    Source->Destroy();
    Movement->RefreshGravity();
    TestTrue(TEXT("Destroyed field restores default down"), Movement->GetGravityDirection().Equals(FVector::DownVector));
    World->DestroyWorld(false);
    return true;
}
#endif
