// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StreamingMovementGateSubsystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/StreamingMovementGateSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Templates/UnrealTemplate.h"
#include "Character/CharacterComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "System/WorldObjectStreamingSubsystem.h"

namespace
{
    UPrimitiveComponent* FindSimulatingPrimitive(AActor* Actor)
    {
        if (!IsValid(Actor)) return nullptr;
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
        for (UPrimitiveComponent* Primitive : Primitives)
            if (IsValid(Primitive) && Primitive->IsSimulatingPhysics()) return Primitive;
        return nullptr;
    }
}

void UStreamingMovementGateSubsystem::Deinitialize()
{
    check(IsInGameThread());
    bDeinitializing = true;
    auto PendingResume = MoveTemp(FrozenActors);
    FrozenActors.Empty();
    for (const TPair<TWeakObjectPtr<AActor>, FFrozenState>& Pair : PendingResume)
        if (AActor* Actor = Pair.Key.Get()) Resume(Actor, Pair.Value);
    RegisteredMovables.Empty();
    UnavailableRegions.Empty();
    Super::Deinitialize();
}

TStatId UStreamingMovementGateSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UStreamingMovementGateSubsystem, STATGROUP_Tickables);
}

void UStreamingMovementGateSubsystem::RegisterMovable(AActor* Actor)
{
    check(IsInGameThread());
    if (!bDeinitializing && IsValid(Actor) && !Actor->IsActorBeingDestroyed()) RegisteredMovables.Add(Actor);
}

void UStreamingMovementGateSubsystem::UnregisterMovable(AActor* Actor)
{
    check(IsInGameThread());
    if (!Actor) return;
    if (FFrozenState State; FrozenActors.RemoveAndCopyValue(Actor, State)) Resume(Actor, State);
    RegisteredMovables.Remove(Actor);
}

void UStreamingMovementGateSubsystem::SetModelRegionAvailable(
    const UObject* Owner, const FName RegionName, const FBox& WorldBounds, const bool bAvailable)
{
    check(IsInGameThread());
    if (bDeinitializing || !IsValid(Owner) || RegionName.IsNone()) return;
    const FRegionKey Key{const_cast<UObject*>(Owner), RegionName};
    if (bAvailable || !WorldBounds.IsValid || WorldBounds.Min.ContainsNaN() || WorldBounds.Max.ContainsNaN()) UnavailableRegions.Remove(Key);
    else UnavailableRegions.Add(Key, WorldBounds);
}

void UStreamingMovementGateSubsystem::ClearModelRegions(const UObject* Owner)
{
    check(IsInGameThread());
    for (auto It = UnavailableRegions.CreateIterator(); It; ++It)
        if (!It.Key().Owner.IsValid() || It.Key().Owner.Get() == Owner) It.RemoveCurrent();
}

bool UStreamingMovementGateSubsystem::IsDestinationAvailable(const FVector& Destination) const
{
    UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
    if (Chunks && Chunks->IsRunning() && !Chunks->IsLocationLoaded(Destination))
    {
        Chunks->EnsureLocationLoaded(Destination);
        return false;
    }
    for (const TPair<FRegionKey, FBox>& Pair : UnavailableRegions)
        if (Pair.Key.Owner.IsValid() && Pair.Value.IsInsideOrOn(Destination)) return false;
    return true;
}

void UStreamingMovementGateSubsystem::Freeze(AActor* Actor)
{
    if (bDeinitializing || !IsValid(Actor) || Actor->IsActorBeingDestroyed() || FrozenActors.Contains(Actor)) return;
    FFrozenState State;
    // Simulated bodies take priority, including a ragdoll mesh below an ACharacter root capsule.
    // Storing both velocity vectors before disabling simulation prevents solver drift at the gate.
    if (UPrimitiveComponent* Primitive = FindSimulatingPrimitive(Actor))
    {
        State.Primitive = Primitive;
        State.bWasSimulatingPhysics = true;
        State.LinearVelocity = Primitive->GetPhysicsLinearVelocity();
        State.AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
    }
    else if (ACharacter* Character = Cast<ACharacter>(Actor))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            State.bCharacterMovement = true;
            State.MovementMode = static_cast<uint8>(Movement->MovementMode);
            State.CustomMovementMode = Movement->CustomMovementMode;
            State.LinearVelocity = Movement->Velocity;
        }
    }
    if (!State.bCharacterMovement && !State.bWasSimulatingPhysics) return;
    FrozenActors.Add(Actor, State);
    if (UCharacterComponent* CharacterState = Actor->FindComponentByClass<UCharacterComponent>())
        CharacterState->SetStreamingMovementSuspended(true);
    // Publish restoration data before a setter can emit an overlap/movement callback.
    if (UPrimitiveComponent* Primitive = State.Primitive.Get())
    {
        Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
        Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
        Primitive->SetSimulatePhysics(false);
    }
    else if (ACharacter* Character = Cast<ACharacter>(Actor))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            Movement->StopMovementImmediately();
            if (IsValid(Movement) && FrozenActors.Contains(Actor)) Movement->DisableMovement();
        }
    }
}

void UStreamingMovementGateSubsystem::Resume(AActor* Actor, const FFrozenState& State)
{
    if (!IsValid(Actor) || Actor->IsActorBeingDestroyed()) return;
    if (State.bCharacterMovement)
    {
        if (ACharacter* Character = Cast<ACharacter>(Actor))
            if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
            {
                Movement->SetMovementMode(static_cast<EMovementMode>(State.MovementMode), State.CustomMovementMode);
                if (IsValid(Movement)) Movement->Velocity = State.LinearVelocity;
            }
    }
    else if (UPrimitiveComponent* Primitive = State.Primitive.Get(); IsValid(Primitive) && State.bWasSimulatingPhysics)
    {
        Primitive->SetSimulatePhysics(true);
        if (IsValid(Primitive)) Primitive->SetPhysicsLinearVelocity(State.LinearVelocity);
        if (IsValid(Primitive)) Primitive->SetPhysicsAngularVelocityInRadians(State.AngularVelocity);
    }
    // Restore velocity first, then make that velocity the new impact baseline. This also handles
    // resume during unregistration or subsystem teardown, not just the ordinary tick path.
    if (IsValid(Actor))
        if (UCharacterComponent* CharacterState = Actor->FindComponentByClass<UCharacterComponent>())
            CharacterState->SetStreamingMovementSuspended(false);
}

void UStreamingMovementGateSubsystem::Tick(const float DeltaTime)
{
    check(IsInGameThread());
    if (bDeinitializing || bUpdating || !GetWorld() || !FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0f) return;
    TGuardValue<bool> UpdateGuard(bUpdating, true);
    for (auto It = UnavailableRegions.CreateIterator(); It; ++It)
        if (!It.Key().Owner.IsValid()) It.RemoveCurrent();
    UWorldObjectStreamingSubsystem* Chunks = GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>();
    // Idle/main-menu worlds need neither a full pawn scan nor destination queries.
    if ((!Chunks || !Chunks->IsRunning()) && UnavailableRegions.IsEmpty() && FrozenActors.IsEmpty()) return;
    for (TActorIterator<APawn> It(GetWorld()); It; ++It) RegisteredMovables.Add(*It);

    // Loading, freezing and resuming may unregister/destroy actors through callbacks.
    const TArray<TWeakObjectPtr<AActor>> Snapshot = RegisteredMovables.Array();
    for (const TWeakObjectPtr<AActor>& WeakActor : Snapshot)
    {
        if (bDeinitializing) return;
        if (!RegisteredMovables.Contains(WeakActor)) continue;
        AActor* Actor = WeakActor.Get();
        if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
        {
            FrozenActors.Remove(WeakActor);
            RegisteredMovables.Remove(WeakActor);
            continue;
        }
        const FFrozenState* Frozen = FrozenActors.Find(WeakActor);
        const FVector Velocity = Frozen ? Frozen->LinearVelocity : Actor->GetVelocity();
        const FVector Destination = Actor->GetActorLocation() + Velocity * FMath::Clamp(DeltaTime, 0.0f, 0.25f);
        if (Destination.ContainsNaN()) continue;
        const bool bAvailable = IsDestinationAvailable(Destination);
        // EnsureLocationLoaded can synchronously change world/actor ownership.
        if (bDeinitializing || !WeakActor.IsValid() || !RegisteredMovables.Contains(WeakActor)) continue;
        if (!bAvailable) Freeze(Actor);
        else if (FFrozenState State; FrozenActors.RemoveAndCopyValue(WeakActor, State)) Resume(Actor, State);
    }
}
