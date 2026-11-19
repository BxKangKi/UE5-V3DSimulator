// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/InstancedMeshActor.h"
#include "Model/V3DHISMChunkPolicy.h"
#include "System/V3DStreamingReclaimSchedule.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStreamingCellSnapshotTest,
    "V3DSimulator.Streaming.Regression.CellSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStreamingCellSnapshotTest::RunTest(const FString&)
{
    FInstancedMeshGroupInitData Definitions;
    const auto AddNode = [&](FName Name, double X, bool bAlways)
    {
        FModelNodeData Node;
        Node.MeshName = TEXT("Mesh");
        Node.Transform.SetLocation(FVector(X, 100, 100));
        Node.bAlwaysLoaded = bAlways;
        Definitions.Nodes.Add(Name, Node);
        Definitions.NodeBounds += Node.Transform.GetLocation();
        auto& Cell = Definitions.StreamChunks.FindOrAdd(V3DHISMChunkPolicy::Key(Node.Transform.GetLocation()));
        Cell.LocalBounds = V3DHISMChunkPolicy::CellBounds(V3DHISMChunkPolicy::Key(Node.Transform.GetLocation()));
        Cell.Nodes.Add(Name);
        Cell.bHasAlwaysLoadedNodes |= bAlways;
        if (bAlways) Definitions.AlwaysLoadedNodeNames.Add(Name);
    };
    Definitions.MaxNodeScale = 1.0;
    Definitions.ReferencedMeshNames.Add(TEXT("Mesh"));
    AddNode(TEXT("Near"), 100.0, false);
    AddNode(TEXT("Far"), 500000.0, false);
    AddNode(TEXT("Always"), 500100.0, true);
    AddNode(TEXT("OutsideCap"), 1200000.0, true);
    TSet<FName> Loaded;
    TMap<FName, FModelNodeData> Snapshot;
    const auto Capture = [&](const FVector& Observer, const FTransform& Transform, double MaxCm)
    {
        AInstancedMeshActor::BuildNativeNodeSnapshot(Definitions, Loaded, Observer,
            Transform, 102400.0f, Snapshot, MaxCm);
    };
    Capture(FVector::ZeroVector, FTransform::Identity, 819200.0);
    TestTrue(TEXT("Nearby cell is selected"), Snapshot.Contains(TEXT("Near")));
    TestTrue(TEXT("Always-loaded cell survives the broad phase"), Snapshot.Contains(TEXT("Always")));
    TestTrue(TEXT("Whole-cell residency includes regular peers"), Snapshot.Contains(TEXT("Far")));
    TestFalse(TEXT("A distant always-loaded cell still respects the maximum"), Snapshot.Contains(TEXT("OutsideCap")));
    Capture(FVector::ZeroVector, FTransform::Identity, 204800.0);
    TestFalse(TEXT("Distance setting can exclude the special cell"), Snapshot.Contains(TEXT("Always")));
    Loaded.Add(TEXT("OutsideCap"));
    Capture(FVector::ZeroVector, FTransform::Identity, 204800.0);
    TestTrue(TEXT("Resident rows remain available to the unload planner"), Snapshot.Contains(TEXT("OutsideCap")));
    Loaded.Empty();
    Capture(FVector(500000, 0, 0), FTransform::Identity, 204800.0);
    TestTrue(TEXT("Movement selects the new area"), Snapshot.Contains(TEXT("Far")));
    TestFalse(TEXT("Movement drops unloaded old-area rows"), Snapshot.Contains(TEXT("Near")));
    Capture(FVector::ZeroVector, FTransform::Identity, 204800.0);
    TestTrue(TEXT("Returning selects the original area again"), Snapshot.Contains(TEXT("Near")));
    const FTransform Shift(FVector(2000000, 0, 0));
    Capture(FVector(2000000, 0, 0), Shift, 819200.0);
    TestTrue(TEXT("Translated scene uses world-space observer distance"), Snapshot.Contains(TEXT("Always")));

    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None,
        nullptr, true, ERHIFeatureLevel::Num, &Values);
    if (!TestNotNull(TEXT("World"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AInstancedMeshActor* Group = World->SpawnActor<AInstancedMeshActor>();
    if (!TestNotNull(TEXT("Group"), Group)) return false;
    if (!TestTrue(TEXT("Initialize native definitions"), Group->InitializeGroup(TEXT("Group"), MoveTemp(Definitions)))) return false;
    const auto WorkerSnapshot = Group->CaptureDefinitions();
    if (!WorkerSnapshot.IsValid()) return false;
    TestTrue(TEXT("Immutable worker snapshot retains special-node metadata"),
        WorkerSnapshot.IsValid() && WorkerSnapshot->AlwaysLoadedNodeNames.Num() == 2);
    Group->ReleaseRuntimeResources();
    TestFalse(TEXT("Released actor drops its definition pin immediately"), Group->CaptureDefinitions().IsValid());
    TestEqual(TEXT("Outstanding worker snapshot remains valid after teardown"), WorkerSnapshot->Nodes.Num(), 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DStreamingTrailingReclaimTest,
    "V3DSimulator.Streaming.Memory.TrailingReclaim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DStreamingTrailingReclaimTest::RunTest(const FString&)
{
    FV3DStreamingReclaimSchedule Schedule;
    TestFalse(TEXT("No idle collection"), Schedule.ConsumeIfDue(10.0));
    Schedule.Request(10.0);
    TestTrue(TEXT("First unload schedules collection"), Schedule.ConsumeIfDue(10.0));
    Schedule.Request(10.1);
    TestFalse(TEXT("Cooldown coalesces rather than collecting per node"), Schedule.ConsumeIfDue(10.9));
    Schedule.Request(10.99);
    TestTrue(TEXT("Last unload is collected even when streaming stops"), Schedule.ConsumeIfDue(11.0));
    TestFalse(TEXT("Consumed request is not repeated"), Schedule.ConsumeIfDue(20.0));
    Schedule.Request(20.0);
    Schedule.Reset();
    TestFalse(TEXT("Shutdown cancels pending collection"), Schedule.IsPending());
    return true;
}
#endif
