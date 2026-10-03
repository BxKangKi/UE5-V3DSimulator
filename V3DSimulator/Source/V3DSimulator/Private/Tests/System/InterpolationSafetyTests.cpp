// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/PhysicsTransformInterpolationSubSystem.h"
#include "Components/SceneComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInterpolationReentrantMutationTest,
    "V3DSimulator.System.Interpolation.ReentrantMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInterpolationReentrantMutationTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
    TStrongObjectPtr<UPhysicsTransformInterpolationSubSystem> Interpolator(
        NewObject<UPhysicsTransformInterpolationSubSystem>(Instance.Get()));
    const auto MakeComponent = [World]()
    {
        AActor* Actor = World->SpawnActor<AActor>();
        USceneComponent* Component = NewObject<USceneComponent>(Actor);
        Actor->SetRootComponent(Component);
        Component->RegisterComponent();
        return Component;
    };
    USceneComponent* First = MakeComponent();
    USceneComponent* Second = MakeComponent();
    const FTransform FirstTarget(FQuat::Identity, FVector(100, 0, 0), FVector(2));
    const FTransform SecondTarget(FQuat::Identity, FVector(200, 0, 0), FVector(3));
    const FTransform Replacement(FQuat::Identity, FVector(900, 0, 0), FVector(4));
    Interpolator->SubmitTransform(First, FirstTarget, 0, 0, true);
    Interpolator->SubmitTransform(Second, SecondTarget, 0, 0, true);
    bool bCallbackRan = false;
    const FDelegateHandle Handle = First->TransformUpdated.AddLambda(
        [&](USceneComponent*, EUpdateTransformFlags, ETeleportType)
        {
            if (bCallbackRan) return;
            bCallbackRan = true;
            // Removing First swaps Second into its index. Resubmitting invalidates
            // Second's old work; additions then force the backing array to grow.
            Interpolator->ClearComponent(First);
            Interpolator->SubmitTransform(Second, Replacement, 0, 0, true);
            for (int32 I = 0; I < 80; ++I)
            {
                Interpolator->SubmitTransform(MakeComponent(), FTransform(FVector(I + 1, 10, 0)), 0);
            }
            Interpolator->UpdateFromGameUpdate(1.0f / 60.0f); // Re-entry is ignored.
        });
    Interpolator->UpdateFromGameUpdate(1.0f / 60.0f);
    TestTrue(TEXT("Transform callback ran"), bCallbackRan);
    TestTrue(TEXT("Removed work cannot apply its scale"), First->GetComponentScale().Equals(FVector::OneVector));
    TestTrue(TEXT("Replaced work cannot apply an obsolete target"), Second->GetComponentLocation().IsNearlyZero());
    First->TransformUpdated.Remove(Handle);
    Interpolator->UpdateFromGameUpdate(1.0f / 60.0f);
    TestTrue(TEXT("New target survives callbacks and reaches its position"), Second->GetComponentTransform().Equals(Replacement));

    // Destruction during a setter must not leave a component pointer in use.
    USceneComponent* Doomed = MakeComponent();
    const FDelegateHandle DestroyHandle = Doomed->TransformUpdated.AddLambda(
        [Doomed](USceneComponent*, EUpdateTransformFlags, ETeleportType) { Doomed->DestroyComponent(); });
    Interpolator->SubmitTransform(Doomed, FirstTarget, 0, 0, true);
    Interpolator->UpdateFromGameUpdate(1.0f / 60.0f);
    TestFalse(TEXT("Destroyed component was unregistered"), Doomed->IsRegistered());
    Doomed->TransformUpdated.Remove(DestroyHandle);
    Interpolator->Deinitialize();
    World->DestroyWorld(false);
    return true;
}
#endif
