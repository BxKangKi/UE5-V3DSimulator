// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Model/InstancedMeshActor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamedMeshShadowRestoreTest,
    "V3DSimulator.Rendering.StreamedMeshRestoresShadows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamedMeshShadowRestoreTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false)
        .CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    AInstancedMeshActor* Actor = World->SpawnActor<AInstancedMeshActor>();
    if (!TestNotNull(TEXT("Streamed mesh actor"), Actor)) return false;
    UInstancedStaticMeshComponent* Component = Actor->GetMeshComponent();
    if (!TestNotNull(TEXT("World instance component"), Component)) return false;
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr,
        TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Built-in test mesh"), Mesh)) return false;

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        // Simulate serialized Blueprint defaults and then a streamed group reload.
        Component->SetCastShadow(false);
        Component->bCastDynamicShadow = false;
        Component->bCastStaticShadow = false;
        Component->SetVisibleInRayTracing(false);
        TestNotNull(TEXT("Assign render mesh"), Actor->AssignStaticMesh(Mesh));
        TestTrue(TEXT("Shadow casting restored"), bool(Component->CastShadow));
        TestTrue(TEXT("Movable instance shadows restored"), bool(Component->bCastDynamicShadow));
        TestTrue(TEXT("Static shadows restored"), bool(Component->bCastStaticShadow));
        TestTrue(TEXT("RT scene visibility restored"), bool(Component->bVisibleInRayTracing));
        Component->SetStaticMesh(nullptr);
    }
    return true;
}
#endif
