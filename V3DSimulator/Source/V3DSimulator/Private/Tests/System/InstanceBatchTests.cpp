#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Model/InstancedMeshActor.h"
#include "Model/V3DHISMChunkPolicy.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DInstanceBatchTest,
    "V3DSimulator.Streaming.Regression.InstanceBatchRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DInstanceBatchTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("World"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Actor = World->SpawnActor<AInstancedMeshActor>();
    if (!TestNotNull(TEXT("Actor"), Actor)) return false;
    FInstancedMeshGroupInitData Definitions;
    Definitions.ReferencedMeshNames.Add(TEXT("Cube")); Definitions.MaxNodeScale = 1;
    TArray<FWorldPreparedInstance> Rows;
    for (int32 Index = 0; Index < 600; ++Index)
    {
        FWorldPreparedInstance& Row = Rows.AddDefaulted_GetRef();
        Row.NodeName = FName(*FString::Printf(TEXT("Node%d"), Index));
        Row.LocalTransform.SetLocation(FVector(Index < 300 ? Index * 10.0 : 200000.0 + Index * 10.0, 0, 0));
        Row.WorldChunk = V3DHISMChunkPolicy::Key(Row.LocalTransform.GetLocation()); Row.bValid = true;
        FModelNodeData Node; Node.MeshName = TEXT("Cube"); Node.Transform = Row.LocalTransform;
        Definitions.Nodes.Add(Row.NodeName, Node); Definitions.NodeBounds += Node.Transform.GetLocation();
    }
    if (!TestTrue(TEXT("Definitions"), Actor->InitializeGroup(TEXT("Cubes"), MoveTemp(Definitions)))) return false;
    auto* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Assign mesh"), Actor->AssignStaticMesh(Mesh))) return false;
    Actor->SetInstanceCullDistances(85000, 100000);
    TArray<FName> Added;
    TestTrue(TEXT("Batched transforms registered"), Actor->AddNodeInstances(Rows, Added));
    TestEqual(TEXT("All transforms registered once"), Added.Num(), 600);
    TestTrue(TEXT("Duplicate request is idempotent"), Actor->AddNodeInstances(Rows, Added) && Added.IsEmpty());
    TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
    Actor->GetComponents(Components);
    int32 Total = 0;
    for (auto* Component : Components)
    {
        TestTrue(TEXT("Every page bounded to 256 instances"), Component->GetNumInstances() <= 256);
        TestTrue(TEXT("Culling inherited by pages"), Component->InstanceEndCullDistance == 100000);
        Total += Component->GetNumInstances();
    }
    TestEqual(TEXT("Component count agrees with slot bookkeeping"), Total, 600);
    for (int32 Index = 0; Index < Rows.Num(); Index += 3)
        TestTrue(TEXT("Removal updates moved slot indices"), Actor->RemoveNodeInstance(Rows[Index].NodeName));
    TSet<FName> Loaded; Actor->GetLoadedNodeSnapshot(Loaded);
    TestEqual(TEXT("Remaining records correct"), Loaded.Num(), 400);
    TestTrue(TEXT("Removed instances can be appended again"), Actor->AddNodeInstances(Rows, Added));
    TestEqual(TEXT("Only missing transforms are re-added"), Added.Num(), 200);
    Rows[0].bValid = false;
    TestFalse(TEXT("Invalid batch is rejected before mutation"), Actor->AddNodeInstances(Rows, Added));
    Actor->GetLoadedNodeSnapshot(Loaded); TestEqual(TEXT("Rejected batch preserves existing state"), Loaded.Num(), 600);
    Actor->ReleaseRuntimeResources();
    TestFalse(TEXT("Late batch cannot repopulate a released scene"), Actor->AddNodeInstances(Rows, Added));
    return true;
}
#endif
