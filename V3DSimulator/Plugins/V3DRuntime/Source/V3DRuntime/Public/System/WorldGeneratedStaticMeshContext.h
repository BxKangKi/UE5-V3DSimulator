// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "WorldGeneratedStaticMeshContext.generated.h"

class UWorld;

/**
 * Stable async-build outer for runtime static meshes.
 *
 * glTFRuntime queries StaticMesh->GetWorld() while finalizing complex collision. A weak world
 * reference is retained for completed meshes, while PinWorldForBuild() temporarily holds a strong
 * reference for the exact lifetime of one native collision build. This prevents map teardown or GC
 * from nulling the world halfway through Chaos cooking without making completed meshes retain it.
 */
UCLASS(Transient)
class V3DRUNTIME_API UWorldGeneratedStaticMeshContext : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(UWorld* InWorld);
    bool PinWorldForBuild();
    void ReleaseWorldPin();
    /** Cancels queued/read work; an already-active native collision build keeps its world pin. */
    void CancelBuild();
    bool IsBuildCancelled() const;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> GetBuildCancellationToken() const { return BuildCancellation; }
    virtual UWorld* GetWorld() const override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UWorld> PinnedWorld;

    UPROPERTY(Transient)
    TWeakObjectPtr<UWorld> World;

    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> BuildCancellation;
};

