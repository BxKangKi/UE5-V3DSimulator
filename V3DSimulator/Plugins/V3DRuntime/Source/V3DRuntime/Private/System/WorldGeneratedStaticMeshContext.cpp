// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/WorldGeneratedStaticMeshContext.h"
#include "Engine/World.h"

void UWorldGeneratedStaticMeshContext::Initialize(UWorld* InWorld)
{
    CancelBuild();
    BuildCancellation = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
    PinnedWorld = nullptr;
    World = InWorld;
}

bool UWorldGeneratedStaticMeshContext::PinWorldForBuild()
{
    if (IsBuildCancelled()) return false;
    UWorld* RuntimeWorld = World.Get();
    if (!IsValid(RuntimeWorld) || !RuntimeWorld->IsGameWorld())
    {
        PinnedWorld = nullptr;
        return false;
    }

    PinnedWorld = RuntimeWorld;
    return true;
}

void UWorldGeneratedStaticMeshContext::ReleaseWorldPin()
{
    PinnedWorld = nullptr;
}

void UWorldGeneratedStaticMeshContext::CancelBuild()
{
    check(IsInGameThread());
    if (BuildCancellation.IsValid() && BuildCancellation->GetValue() == 0) BuildCancellation->Increment();
}

bool UWorldGeneratedStaticMeshContext::IsBuildCancelled() const
{
    check(IsInGameThread());
    return BuildCancellation.IsValid() && BuildCancellation->GetValue() != 0;
}

UWorld* UWorldGeneratedStaticMeshContext::GetWorld() const
{
    if (UWorld* ActiveWorld = PinnedWorld.Get())
    {
        return ActiveWorld;
    }

    return World.Get();
}

