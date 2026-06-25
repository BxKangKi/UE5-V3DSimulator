// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldStartupQueueTests.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/V3DRuntimeSafety.h"
#include "UObject/StrongObjectPtr.h"
#include "glTFRuntimeAsset.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldStartupQueueWaitTest,
    "V3DSimulator.World.Startup.NativeGateWaits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStartupQueueWaitTest::RunTest(const FString& Parameters)
{
    // Run in an idle editor. These tickets exercise scheduling only, never plugin decoding.
    if (FV3DRuntimeSafety::GetPendingOperationCount() != 0 || FV3DRuntimeSafety::IsCircuitOpen())
    {
        AddError(TEXT("Run this test without an active world build or native circuit failure."));
        return false;
    }
    TStrongObjectPtr<UObject> Owner(NewObject<UObject>());
    TStrongObjectPtr<UglTFRuntimeAsset> Asset(NewObject<UglTFRuntimeAsset>());
    bool bFirstStarted = false;
    bool bSecondStarted = false;
    const uint64 First = FV3DRuntimeSafety::EnqueueOperation(Owner.Get(), Asset.Get(), TEXT("test hold"),
        [&bFirstStarted](uint64) { bFirstStarted = true; });
    const uint64 Second = FV3DRuntimeSafety::EnqueueOperation(Owner.Get(), Asset.Get(), TEXT("test wait"),
        [&bSecondStarted](uint64 Ticket)
        {
            bSecondStarted = true;
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        });
    TestTrue(TEXT("first request acquired gate"), First != 0 && bFirstStarted);
    TestTrue(TEXT("busy gate retains second request"), Second != 0 && !bSecondStarted);
    FV3DRuntimeSafety::CompleteOperation(First);
    TestTrue(TEXT("waiting request runs after release"), bSecondStarted);
    TestEqual(TEXT("all tickets returned"), FV3DRuntimeSafety::GetPendingOperationCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldStartupQueueCancelTest,
    "V3DSimulator.World.Startup.CancelQueuedOwner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStartupQueueCancelTest::RunTest(const FString& Parameters)
{
    if (FV3DRuntimeSafety::GetPendingOperationCount() != 0 || FV3DRuntimeSafety::IsCircuitOpen())
    {
        AddError(TEXT("Run this test in an idle editor."));
        return false;
    }
    TStrongObjectPtr<UObject> ActiveOwner(NewObject<UObject>());
    TStrongObjectPtr<UObject> CancelledOwner(NewObject<UObject>());
    TStrongObjectPtr<UglTFRuntimeAsset> Asset(NewObject<UglTFRuntimeAsset>());
    const uint64 Active = FV3DRuntimeSafety::EnqueueOperation(ActiveOwner.Get(), Asset.Get(), TEXT("test hold"), [](uint64) {});
    bool bCancelledStarted = false;
    int32 Rejections = 0;
    FV3DRuntimeSafety::EnqueueOperation(CancelledOwner.Get(), Asset.Get(), TEXT("test cancel"),
        [&bCancelledStarted](uint64 Ticket)
        {
            bCancelledStarted = true;
            FV3DRuntimeSafety::CompleteOperation(Ticket);
        }, [&Rejections](const FString&) { ++Rejections; });
    FV3DRuntimeSafety::CancelQueuedOperations(CancelledOwner.Get());
    FV3DRuntimeSafety::CompleteOperation(Active);
    TestFalse(TEXT("cancelled build did not execute"), bCancelledStarted);
    TestEqual(TEXT("cancel notified exactly once"), Rejections, 1);
    TestEqual(TEXT("queue released"), FV3DRuntimeSafety::GetPendingOperationCount(), 0);
    return true;
}
#endif
