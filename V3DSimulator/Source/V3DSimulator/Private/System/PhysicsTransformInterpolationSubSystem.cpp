// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PhysicsTransformInterpolationSubSystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/PhysicsTransformInterpolationSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "Async/ParallelFor.h"
#include "Templates/UnrealTemplate.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"


namespace
{
    constexpr int32 ParallelTransformInterpolationThreshold = 64;

    struct FPhysicsInterpolationWorkItem
    {
        int32 EntryIndex = INDEX_NONE;
        uint64 Revision = 0;
        FTransform CurrentTransform = FTransform::Identity;
        FTransform TargetTransform = FTransform::Identity;
        FVector ResultLocation = FVector::ZeroVector;
        FQuat ResultRotation = FQuat::Identity;
        FVector ResultScale = FVector::OneVector;
        float InterpSpeed = 0.0f;
        float TeleportDistance = 0.0f;
        float DeltaTime = 0.0f;
        bool bApplyScale = false;
        bool bShouldApply = false;
        bool bAtTarget = false;
    };

    static void CalculatePhysicsInterpolation(FPhysicsInterpolationWorkItem& Work)
    {
        const FVector CurrentLocation = Work.CurrentTransform.GetLocation();
        const FVector TargetLocation = Work.TargetTransform.GetLocation();
        const FQuat CurrentRotation = Work.CurrentTransform.GetRotation().GetNormalized();
        const FQuat TargetRotation = Work.TargetTransform.GetRotation().GetNormalized();
        const FVector CurrentScale = Work.CurrentTransform.GetScale3D();
        const FVector TargetScale = Work.TargetTransform.GetScale3D();
        const bool bLocationAtTarget = CurrentLocation.Equals(TargetLocation, 0.01f);
        const bool bRotationAtTarget = CurrentRotation.Equals(TargetRotation, 0.0001f);
        const bool bScaleAtTarget = !Work.bApplyScale
            || Work.CurrentTransform.GetScale3D().Equals(Work.TargetTransform.GetScale3D(), 0.001f);

        if (bLocationAtTarget && bRotationAtTarget && bScaleAtTarget)
        {
            Work.ResultLocation = TargetLocation;
            Work.ResultRotation = TargetRotation;
            Work.ResultScale = TargetScale;
            Work.bShouldApply = false;
            Work.bAtTarget = true;
            return;
        }

        Work.ResultLocation = TargetLocation;
        Work.ResultRotation = TargetRotation;
        Work.ResultScale = TargetScale;
        const bool bTeleport = Work.TeleportDistance > 0.0f
            && FVector::DistSquared(CurrentLocation, TargetLocation) > FMath::Square(Work.TeleportDistance);
        if (!bTeleport && Work.InterpSpeed > KINDA_SMALL_NUMBER)
        {
            const float Alpha = FMath::Clamp(
                1.0f - FMath::Exp(-Work.InterpSpeed * Work.DeltaTime),
                0.0f,
                1.0f);
            Work.ResultLocation = FMath::Lerp(CurrentLocation, TargetLocation, Alpha);
            Work.ResultRotation = FQuat::Slerp(CurrentRotation, TargetRotation, Alpha).GetNormalized();
            if (Work.bApplyScale)
            {
                Work.ResultScale = FMath::Lerp(CurrentScale, TargetScale, Alpha);
            }
        }

        const bool bReachedLocation = Work.ResultLocation.Equals(TargetLocation, 0.01f);
        const bool bReachedRotation = Work.ResultRotation.Equals(TargetRotation, 0.0001f);
        const bool bReachedScale = !Work.bApplyScale || Work.ResultScale.Equals(TargetScale, 0.001f);
        if (bReachedLocation && bReachedRotation && bReachedScale)
        {
            Work.ResultLocation = TargetLocation;
            Work.ResultRotation = TargetRotation;
            Work.ResultScale = TargetScale;
        }
        Work.bShouldApply = true;
        Work.bAtTarget = bReachedLocation && bReachedRotation && bReachedScale;
    }
}

UPhysicsTransformInterpolationSubSystem* UPhysicsTransformInterpolationSubSystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    if (const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject))
    {
        return const_cast<UGameInstance*>(GameInstance)->GetSubsystem<UPhysicsTransformInterpolationSubSystem>();
    }

    if (const UGameInstanceSubsystem* GameInstanceSubsystem = Cast<UGameInstanceSubsystem>(WorldContextObject))
    {
        if (UGameInstance* GameInstance = GameInstanceSubsystem->GetGameInstance())
        {
            return GameInstance->GetSubsystem<UPhysicsTransformInterpolationSubSystem>();
        }
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UPhysicsTransformInterpolationSubSystem>() : nullptr;
}

void UPhysicsTransformInterpolationSubSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    // Register lazily only while at least one transform still needs interpolation.
}

void UPhysicsTransformInterpolationSubSystem::Deinitialize()
{
    UnregisterGameUpdate();
    Entries.Empty();
    Super::Deinitialize();
}

void UPhysicsTransformInterpolationSubSystem::RegisterGameUpdate()
{
    if (GameUpdateHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<UPhysicsTransformInterpolationSubSystem> WeakThis(this);
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (UPhysicsTransformInterpolationSubSystem* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            -50);
    }
}

void UPhysicsTransformInterpolationSubSystem::UnregisterGameUpdate()
{
    if (GameUpdateHandle == INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;
}

void UPhysicsTransformInterpolationSubSystem::SubmitPhysicsTransformForComponent(
    const UObject* WorldContextObject,
    USceneComponent* Component,
    const FTransform& TargetTransform,
    float InterpSpeed,
    float TeleportDistance,
    bool bApplyScale,
    bool bCanMoveSimulatingPrimitive)
{
    if (UPhysicsTransformInterpolationSubSystem* Subsystem = Get(WorldContextObject))
    {
        Subsystem->SubmitTransform(Component, TargetTransform, InterpSpeed, TeleportDistance, bApplyScale, bCanMoveSimulatingPrimitive);
    }
}

void UPhysicsTransformInterpolationSubSystem::SubmitTransform(
    USceneComponent* Component,
    const FTransform& TargetTransform,
    float InterpSpeed,
    float TeleportDistance,
    bool bApplyScale,
    bool bCanMoveSimulatingPrimitive)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("SubmitTransform must run on the game thread"))
        || !IsValid(Component) || !Component->IsRegistered() || TargetTransform.ContainsNaN()
        || !TargetTransform.GetRotation().IsNormalized()
        || !FMath::IsFinite(InterpSpeed) || !FMath::IsFinite(TeleportDistance))
    {
        return;
    }

    for (FInterpolatedTransformEntry& Entry : Entries)
    {
        if (Entry.Component.Get() == Component)
        {
            const float SafeInterpSpeed = FMath::Max(0.0f, InterpSpeed);
            const float SafeTeleportDistance = FMath::Max(0.0f, TeleportDistance);
            const bool bSettingsChanged =
                !FMath::IsNearlyEqual(Entry.InterpSpeed, SafeInterpSpeed) ||
                !FMath::IsNearlyEqual(Entry.TeleportDistance, SafeTeleportDistance) ||
                Entry.bApplyScale != bApplyScale ||
                Entry.bCanMoveSimulatingPrimitive != bCanMoveSimulatingPrimitive;
            const bool bTargetChanged = !Entry.TargetTransform.Equals(TargetTransform, 0.001f);

            Entry.Revision = NextRevision++;
            Entry.TargetTransform = TargetTransform;
            Entry.InterpSpeed = SafeInterpSpeed;
            Entry.TeleportDistance = SafeTeleportDistance;
            Entry.bApplyScale = bApplyScale;
            Entry.bCanMoveSimulatingPrimitive = bCanMoveSimulatingPrimitive;
            Entry.bAtTarget = Entry.bAtTarget && !bTargetChanged && !bSettingsChanged;
            if (!Entry.bAtTarget)
            {
                RegisterGameUpdate();
            }
            return;
        }
    }

    FInterpolatedTransformEntry NewEntry;
    NewEntry.Component = Component;
    NewEntry.Revision = NextRevision++;
    NewEntry.TargetTransform = TargetTransform;
    NewEntry.InterpSpeed = FMath::Max(0.0f, InterpSpeed);
    NewEntry.TeleportDistance = FMath::Max(0.0f, TeleportDistance);
    NewEntry.bApplyScale = bApplyScale;
    NewEntry.bCanMoveSimulatingPrimitive = bCanMoveSimulatingPrimitive;
    Entries.Add(MoveTemp(NewEntry));
    RegisterGameUpdate();
}

void UPhysicsTransformInterpolationSubSystem::ClearComponent(USceneComponent* Component)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ClearComponent must run on the game thread")) || !Component)
    {
        return;
    }

    Entries.RemoveAllSwap([Component](const FInterpolatedTransformEntry& Entry)
    {
        return !Entry.Component.IsValid() || Entry.Component.Get() == Component;
    }, EAllowShrinking::No);
    if (Entries.Num() == 0)
    {
        Entries.Empty();
        UnregisterGameUpdate();
    }
}

void UPhysicsTransformInterpolationSubSystem::ClearOwner(const UObject* Owner)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ClearOwner must run on the game thread")) || !Owner)
    {
        return;
    }

    Entries.RemoveAllSwap([Owner](const FInterpolatedTransformEntry& Entry)
    {
        const USceneComponent* Component = Entry.Component.Get();
        return !Component || Component->GetOwner() == Owner;
    }, EAllowShrinking::No);
    if (Entries.Num() == 0)
    {
        Entries.Empty();
        UnregisterGameUpdate();
    }
}

bool UPhysicsTransformInterpolationSubSystem::ShouldSkipComponent(const USceneComponent* Component, bool bCanMoveSimulatingPrimitive) const
{
    if (!IsValid(Component))
    {
        return true;
    }

    const USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component);
    if (SkeletalMesh && SkeletalMesh->IsSimulatingPhysics())
    {
        return true;
    }

    const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
    if (Primitive && Primitive->IsSimulatingPhysics() && !bCanMoveSimulatingPrimitive)
    {
        return true;
    }

    return false;
}

void UPhysicsTransformInterpolationSubSystem::UpdateFromGameUpdate(float DeltaTime)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Interpolation must run on the game thread"))
        || bIsUpdating || !FMath::IsFinite(DeltaTime))
    {
        return;
    }
    TGuardValue<bool> UpdateGuard(bIsUpdating, true);
    const float SafeDeltaTime = FMath::Clamp(DeltaTime, 0.0f, 1.0f);
    if (SafeDeltaTime <= 0.0f)
    {
        return;
    }

    // Compact first so entry indices remain stable for the immutable worker snapshots below.
    Entries.RemoveAllSwap([](const FInterpolatedTransformEntry& Entry)
    {
        return !Entry.Component.IsValid();
    }, EAllowShrinking::No);

    TArray<FPhysicsInterpolationWorkItem, TInlineAllocator<64>> WorkItems;
    WorkItems.Reserve(Entries.Num());
    for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
    {
        FInterpolatedTransformEntry& Entry = Entries[EntryIndex];
        USceneComponent* Component = Entry.Component.Get();
        if (!IsValid(Component) || ShouldSkipComponent(Component, Entry.bCanMoveSimulatingPrimitive) || Entry.bAtTarget)
        {
            continue;
        }

        const FTransform CurrentTransform = Component->GetComponentTransform();
        if (CurrentTransform.ContainsNaN() || Entry.TargetTransform.ContainsNaN())
        {
            continue;
        }

        FPhysicsInterpolationWorkItem& Work = WorkItems.AddDefaulted_GetRef();
        Work.EntryIndex = EntryIndex;
        Work.Revision = Entry.Revision;
        Work.CurrentTransform = CurrentTransform;
        Work.TargetTransform = Entry.TargetTransform;
        Work.InterpSpeed = Entry.InterpSpeed;
        Work.TeleportDistance = Entry.TeleportDistance;
        Work.DeltaTime = SafeDeltaTime;
        Work.bApplyScale = Entry.bApplyScale;
    }

    auto CalculateItem = [&WorkItems](const int32 Index)
    {
        CalculatePhysicsInterpolation(WorkItems[Index]);
    };
    if (WorkItems.Num() >= ParallelTransformInterpolationThreshold)
    {
        // Workers operate only on value snapshots and disjoint output elements. UObject access and
        // scene-transform writes remain on the game thread.
        ParallelFor(WorkItems.Num(), CalculateItem);
    }
    else
    {
        for (int32 Index = 0; Index < WorkItems.Num(); ++Index)
        {
            CalculateItem(Index);
        }
    }

    // Scene-transform setters can synchronously broadcast overlap/transform
    // callbacks. Those callbacks may remove, swap, resubmit or append Entries.
    // A valid array index alone does not identify the work that was calculated.
    const auto FindCurrentEntry = [this](const FPhysicsInterpolationWorkItem& Work) -> FInterpolatedTransformEntry*
    {
        if (Entries.IsValidIndex(Work.EntryIndex) && Entries[Work.EntryIndex].Revision == Work.Revision)
        {
            return &Entries[Work.EntryIndex];
        }
        return Entries.FindByPredicate([&Work](const FInterpolatedTransformEntry& Candidate)
        {
            return Candidate.Revision == Work.Revision;
        });
    };

    for (const FPhysicsInterpolationWorkItem& Work : WorkItems)
    {
        FInterpolatedTransformEntry* Entry = FindCurrentEntry(Work);
        if (!Entry) continue;
        const TWeakObjectPtr<USceneComponent> WeakComponent = Entry->Component;
        USceneComponent* Component = WeakComponent.Get();
        if (!IsValid(Component) || ShouldSkipComponent(Component, Entry->bCanMoveSimulatingPrimitive)) continue;

        if (Work.bShouldApply)
        {
            Component->SetWorldLocationAndRotation(
                Work.ResultLocation, Work.ResultRotation, false, nullptr, ETeleportType::TeleportPhysics);

            // Never dereference Entry or the raw component retained across a callback.
            Entry = FindCurrentEntry(Work);
            Component = WeakComponent.Get();
            if (!Entry || !IsValid(Component) || !Component->IsRegistered()
                || ShouldSkipComponent(Component, Entry->bCanMoveSimulatingPrimitive)) continue;
            if (Work.bApplyScale)
            {
                Component->SetWorldScale3D(Work.ResultScale);
            }
        }

        Entry = FindCurrentEntry(Work);
        if (Entry && WeakComponent.IsValid()) Entry->bAtTarget = Work.bAtTarget;
    }

    // Release completed/dead entries even while another target is still moving.
    Entries.RemoveAllSwap([](const FInterpolatedTransformEntry& Entry)
    {
        return !Entry.Component.IsValid() || !Entry.Component->IsRegistered() || Entry.bAtTarget;
    }, EAllowShrinking::No);
    const bool bHasPendingInterpolation = !Entries.IsEmpty();
    if (!bHasPendingInterpolation)
    {
        // Completed targets do not need to remain in a persistent GameInstance subsystem. A later
        // submission recreates its compact entry and re-registers the dispatcher on demand.
        Entries.Empty();
        UnregisterGameUpdate();
    }
}

