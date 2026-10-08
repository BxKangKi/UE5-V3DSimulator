// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/V3DStreamingBudget.h"
#include "System/V3DStreamingAdmissionPolicy.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include <atomic>

namespace V3DStreamingBudgetPrivate
{
    constexpr int64 MiB = 1024ll * 1024ll;
    TAutoConsoleVariable<int32> MaxRequests(TEXT("gltfsim.Streaming.MaxResidentBuilds"), 0,
        TEXT("End-to-end baked load concurrency: 0 = cores - 1 (2..8), explicit 1..16. Memory budget still applies."), ECVF_Default);
    TAutoConsoleVariable<int32> BudgetMiB(TEXT("gltfsim.Streaming.BuildBudgetMiB"), 256,
        TEXT("Estimated transient CPU admission budget. A single oversized request runs alone."), ECVF_Default);
    TAutoConsoleVariable<float> SceneMs(TEXT("gltfsim.Streaming.SceneFrameBudgetMs"), 3.0f,
        TEXT("Aggregate scene instance work per frame, across all actors (0.25..8 ms)."), ECVF_Default);

    struct FPending
    {
        TWeakObjectPtr<UObject> Owner;
        TFunction<int64()> Estimate;
        TFunction<void(FV3DStreamingBudget::FPermit)> Start;
        TFunction<void()> Reject;
        int32 Bypasses = 0;
    };
    struct FState
    {
        TArray<FPending> Queue;
        std::atomic<int32> Active{0};
        std::atomic<int64> Bytes{0};
        FTSTicker::FDelegateHandle Ticker;
        bool bShutdown = false;
        uint64 Frame = MAX_uint64;
        double SceneSeconds = 0.0;
    };
    FState& State() { static FState Value; return Value; }

    bool Tick(float)
    {
        check(IsInGameThread());
        FState& S = State();
        if (S.bShutdown) return false;
        const int32 Configured = MaxRequests.GetValueOnGameThread();
        const int32 Limit = Configured > 0 ? FMath::Clamp(Configured, 1, 16)
            : FMath::Clamp(FPlatformMisc::NumberOfCores() - 1, 2, 8);
        const int64 Budget = int64(FMath::Clamp(BudgetMiB.GetValueOnGameThread(), 32, 4096)) * MiB;
        const double Deadline = FPlatformTime::Seconds() + 0.001;
        int32 Examined = 0;
        // Fit ready work into spare memory, then drain for the oldest request after 8 bypasses.
        while (!S.bShutdown && !S.Queue.IsEmpty() && Examined++ < 64 && FPlatformTime::Seconds() < Deadline)
        {
            if (!S.Queue[0].Owner.IsValid())
            {
                FPending Dead = MoveTemp(S.Queue[0]);
                S.Queue.RemoveAt(0, 1, EAllowShrinking::No);
                if (Dead.Reject) Dead.Reject();
                continue;
            }
            const int32 Active = S.Active.load();
            const int64 Used = S.Bytes.load();
            const int32 Index = V3DStreamingAdmissionPolicy::FindRequest(
                S.Queue.Num(), Active, Limit, Used, Budget, S.Queue[0].Bypasses,
                [&S, Deadline](const int32 I)
                {
                    if (FPlatformTime::Seconds() >= Deadline) return MAX_int64;
                    return S.Queue[I].Owner.IsValid() ? FMath::Max<int64>(MiB, S.Queue[I].Estimate()) : MiB;
                });
            if (Index == INDEX_NONE) break;
            if (Index > 0) ++S.Queue[0].Bypasses;
            FPending Next = MoveTemp(S.Queue[Index]);
            S.Queue.RemoveAt(Index, 1, EAllowShrinking::No);
            if (!Next.Owner.IsValid()) { if (Next.Reject) Next.Reject(); continue; }
            const int64 Estimate = FMath::Max<int64>(MiB, Next.Estimate());
            Next.Start(MakeShared<FV3DStreamingBudget::FLease, ESPMode::ThreadSafe>(Estimate));
        }
        if (S.Queue.IsEmpty())
        {
            S.Queue.Empty();
            S.Ticker.Reset();
            return false;
        }
        return true;
    }
    FAutoConsoleCommand Dump(TEXT("gltfsim.Streaming.DumpBudget"),
        TEXT("Print queued/active baked loads and their estimated transient CPU reservation."),
        FConsoleCommandDelegate::CreateLambda([]()
        {
            check(IsInGameThread());
            UE_LOG(LogTemp, Display, TEXT("V3D streaming: queued=%d active=%d reserved=%.1f MiB"),
                State().Queue.Num(), State().Active.load(), double(State().Bytes.load()) / MiB);
        }));
}

FV3DStreamingBudget::FLease::FLease(const int64 InBytes) : Bytes(InBytes)
{
    V3DStreamingBudgetPrivate::State().Active.fetch_add(1);
    V3DStreamingBudgetPrivate::State().Bytes.fetch_add(Bytes);
}
FV3DStreamingBudget::FLease::~FLease()
{
    // Last callback capture may be destroyed on a worker. Never touch UObjects/ticker here.
    V3DStreamingBudgetPrivate::State().Bytes.fetch_sub(Bytes);
    V3DStreamingBudgetPrivate::State().Active.fetch_sub(1);
}

void FV3DStreamingBudget::Enqueue(UObject* Owner, TFunction<int64()> EstimateBytes,
    TFunction<void(FPermit)> Start, TFunction<void()> Reject)
{
    check(IsInGameThread());
    using namespace V3DStreamingBudgetPrivate;
    FState& S = State();
    if (S.bShutdown || !IsValid(Owner) || !EstimateBytes || !Start || S.Queue.Num() >= 4096)
    {
        if (Reject) Reject();
        return;
    }
    FPending Entry;
    Entry.Owner = Owner;
    Entry.Estimate = MoveTemp(EstimateBytes);
    Entry.Start = MoveTemp(Start);
    Entry.Reject = MoveTemp(Reject);
    S.Queue.Add(MoveTemp(Entry));
    // A ticker, rather than an inline recursive pump, lets native callbacks fully unwind first.
    if (!S.Ticker.IsValid())
        S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
}

void FV3DStreamingBudget::Shutdown()
{
    check(IsInGameThread());
    auto& S = V3DStreamingBudgetPrivate::State();
    S.bShutdown = true;
    if (S.Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(S.Ticker);
    S.Ticker.Reset();
    auto Rejected = MoveTemp(S.Queue);
    S.Queue.Empty();
    for (auto& Entry : Rejected) if (Entry.Reject) Entry.Reject();
}

double FV3DStreamingBudget::RemainingSceneSeconds()
{
    check(IsInGameThread());
    auto& S = V3DStreamingBudgetPrivate::State();
    if (S.Frame != GFrameCounter) { S.Frame = GFrameCounter; S.SceneSeconds = 0.0; }
    return FMath::Max(0.0, double(FMath::Clamp(
        V3DStreamingBudgetPrivate::SceneMs.GetValueOnGameThread(), 0.25f, 8.0f)) * 0.001 - S.SceneSeconds);
}
void FV3DStreamingBudget::ChargeSceneSeconds(const double Seconds)
{
    RemainingSceneSeconds();
    V3DStreamingBudgetPrivate::State().SceneSeconds += FMath::Max(0.0, Seconds);
}

void FV3DStreamingBudget::NotifyUnloaded()
{
    check(IsInGameThread());
    static double LastRequestSeconds = -5.0;
    const double Now = FPlatformTime::Seconds();
    if (!V3DStreamingBudgetPrivate::State().bShutdown && GEngine && Now - LastRequestSeconds >= 5.0)
    {
        LastRequestSeconds = Now;
        // Only schedules engine GC. Never collect recursively inside a mesh/plugin callback,
        // force a full purge, or flush the rendering thread from the unload loop.
        GEngine->ForceGarbageCollection(false);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DBakedAdmissionTest,
    "V3DSimulator.Streaming.Memory.AdmissionLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DBakedAdmissionTest::RunTest(const FString&)
{
    using namespace V3DStreamingBudgetPrivate;
    auto& S = State();
    if (S.bShutdown || S.Active.load() != 0 || !S.Queue.IsEmpty())
    {
        AddError(TEXT("Run admission test in an idle editor before world streaming."));
        return false;
    }
    const int32 OldMax = MaxRequests.GetValueOnGameThread();
    const int32 OldBudget = BudgetMiB.GetValueOnGameThread();
    MaxRequests.AsVariable()->Set(2, ECVF_SetByCode);
    BudgetMiB.AsVariable()->Set(32, ECVF_SetByCode);
    TStrongObjectPtr<UObject> Owner(NewObject<UObject>());
    TArray<FV3DStreamingBudget::FPermit> Held;
    ON_SCOPE_EXIT
    {
        Held.Empty();
        S.Queue.Empty();
        if (S.Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(S.Ticker);
        S.Ticker.Reset();
        MaxRequests.AsVariable()->Set(OldMax, ECVF_SetByCode);
        BudgetMiB.AsVariable()->Set(OldBudget, ECVF_SetByCode);
    };
    const auto Submit = [&](const int64 Bytes)
    {
        FV3DStreamingBudget::Enqueue(Owner.Get(), [Bytes]() { return Bytes; },
            [&Held](FV3DStreamingBudget::FPermit Permit) { Held.Add(MoveTemp(Permit)); },
            [this]() { AddError(TEXT("Unexpected admission rejection")); });
    };
    Submit(16 * MiB); Submit(16 * MiB); Submit(64 * MiB);
    // Detach the registered ticker before manually stepping the same production pump.
    FTSTicker::GetCoreTicker().RemoveTicker(S.Ticker);
    S.Ticker.Reset();
    Tick(0);
    TestEqual(TEXT("Two independent loads overlap"), Held.Num(), 2);
    TestEqual(TEXT("Large load waits before allocating any payload"), S.Queue.Num(), 1);
    FV3DStreamingBudget::FPermit NativeCopy = Held[0];
    Held.RemoveAt(0);
    TestEqual(TEXT("Native callback capture keeps its end-to-end reservation"), S.Active.load(), 2);
    NativeCopy.Reset();
    Tick(0);
    TestEqual(TEXT("Oversized load cannot overlap one remaining load"), S.Queue.Num(), 1);
    Held.Empty();
    Tick(0);
    TestEqual(TEXT("Oversized request runs alone without deadlock"), Held.Num(), 1);
    TestEqual(TEXT("Oversized request is fully charged"), S.Bytes.load(), int64(64 * MiB));
    Held.Empty();
    TestEqual(TEXT("Every reservation is released"), S.Bytes.load(), int64(0));
    return true;
}
#endif
