// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"

/** Admission covers disk reads, decoded payloads AND native finalization, not just workers. */
class V3DSIMULATOR_API FV3DStreamingBudget
{
public:
    struct FLease
    {
        explicit FLease(int64 InBytes);
        ~FLease();
        FLease(const FLease&) = delete;
        FLease& operator=(const FLease&) = delete;
        const int64 Bytes;
    };
    using FPermit = TSharedPtr<FLease, ESPMode::ThreadSafe>;

    /** GT only. Queued entries contain metadata/callbacks only; no decoded assets or strong owner. */
    static void Enqueue(UObject* Owner, TFunction<int64()> EstimateBytes,
        TFunction<void(FPermit)> Start, TFunction<void()> Reject);
    static void Shutdown();
    /** Ask the engine for a throttled, deferred GC after streaming releases strong references. */
    static void NotifyUnloaded();

    /** Aggregate time used by all scene actions in this frame. GT only. */
    static double RemainingSceneSeconds();
    static void ChargeSceneSeconds(double Seconds);
};
