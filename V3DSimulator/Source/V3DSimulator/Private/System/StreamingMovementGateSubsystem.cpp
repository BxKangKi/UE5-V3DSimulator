// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StreamingMovementGateSubsystem.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/StreamingMovementGateSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Templates/UnrealTemplate.h"
#include "Character/CharacterComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "System/WorldObjectStreamingSubsystem.h"

namespace
{
    template<typename Visitor>
    void VisitSimulatingBodies(AActor* Actor, Visitor&& Visit)
    {
        if (!IsValid(Actor)) return;
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            if (!IsValid(Primitive)) continue;
            if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Primitive))
            {
                // SetAllBodiesSimulatePhysics does not set the component's simulation flag.
                // Root-bone/component IsSimulatingPhysics therefore misses many ragdolls.
                for (FBodyInstance* Body : Mesh->Bodies)
                    if (Body && Body->IsValidBodyInstance() && Body->IsInstanceSimulatingPhysics()
                        && Body->BodySetup.IsValid())
                        Visit(Primitive, Body, Body->BodySetup->BoneName);
            }
            else if (FBodyInstance* Body = Primitive->GetBodyInstance())
            {
                if (Body->IsValidBodyInstance() && Body->IsInstanceSimulatingPhysics())
                    Visit(Primitive, Body, NAME_None);
            }
        }
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
    // Complete any in-flight bone evaluation before freezing its pose and individual bodies.
    TInlineComponentArray<USkeletalMeshComponent*> EvaluatingMeshes(Actor);
    for (USkeletalMeshComponent* Mesh : EvaluatingMeshes)
        if (IsValid(Mesh)) Mesh->HandleExistingParallelEvaluationTask(true, true);
    if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || FrozenActors.Contains(Actor)) return;
    FFrozenState State;
    if (UCharacterComponent* CharacterState = Actor->FindComponentByClass<UCharacterComponent>())
        State.bRagdoll = CharacterState->IsRagdollActive();
    if (ACharacter* Character = Cast<ACharacter>(Actor))
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            State.bCharacterMovement = true;
            State.MovementMode = static_cast<uint8>(Movement->MovementMode);
            State.CustomMovementMode = Movement->CustomMovementMode;
            State.LinearVelocity = Movement->Velocity;
        }

    VisitSimulatingBodies(Actor, [&State](UPrimitiveComponent* Primitive, FBodyInstance* Body, const FName Bone)
    {
        FFrozenBody& Saved = State.Bodies.AddDefaulted_GetRef();
        Saved.Primitive = Primitive;
        Saved.Setup = Body->BodySetup;
        Saved.Bone = Bone;
        Saved.Transform = Body->GetUnrealWorldTransform();
        Saved.LinearVelocity = Body->GetUnrealWorldVelocity();
        Saved.AngularVelocity = Body->GetUnrealWorldAngularVelocityInRadians();
        Saved.bWasAwake = Body->IsInstanceAwake();
    });
    if (!State.bCharacterMovement && State.Bodies.IsEmpty()) return;
    TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
    for (USkeletalMeshComponent* Mesh : Meshes)
        if (IsValid(Mesh))
        {
            FFrozenMesh& Saved = State.Meshes.AddDefaulted_GetRef();
            Saved.Mesh = Mesh;
            Saved.bPauseAnims = Mesh->bPauseAnims;
            Saved.bNoSkeletonUpdate = Mesh->bNoSkeletonUpdate;
        }

    // Publish the complete restoration state before any movement/overlap callbacks can run.
    FrozenActors.Add(Actor, State);
    if (UCharacterComponent* CharacterState = Actor->FindComponentByClass<UCharacterComponent>())
        CharacterState->SetStreamingMovementSuspended(true);
    for (const FFrozenMesh& Saved : State.Meshes)
        if (USkeletalMeshComponent* Mesh = Saved.Mesh.Get())
        {
            Mesh->bPauseAnims = true;
            Mesh->bNoSkeletonUpdate = true;
        }
    for (const FFrozenBody& Saved : State.Bodies)
        if (UPrimitiveComponent* Primitive = Saved.Primitive.Get())
            if (FBodyInstance* Body = Primitive->GetBodyInstance(Saved.Bone))
                if (Body->IsValidBodyInstance() && Body->BodySetup == Saved.Setup)
                {
                    Body->SetLinearVelocity(FVector::ZeroVector, false, false);
                    Body->SetAngularVelocityInRadians(FVector::ZeroVector, false, false);
                    Body->SetInstanceSimulatePhysics(false, true, true);
                }
    if (ACharacter* Character = Cast<ACharacter>(Actor))
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            Movement->StopMovementImmediately();
            if (IsValid(Movement) && FrozenActors.Contains(Actor)) Movement->DisableMovement();
        }
}

void UStreamingMovementGateSubsystem::Resume(AActor* Actor, const FFrozenState& State)
{
    if (!IsValid(Actor) || Actor->IsActorBeingDestroyed()) return;
    // Reacquire by weak component + bone + setup identity. Never retain raw Chaos body pointers
    // across a streaming wait, during which the actor/mesh/physics asset may be replaced.
    for (const FFrozenBody& Saved : State.Bodies)
        if (UPrimitiveComponent* Primitive = Saved.Primitive.Get())
            if (FBodyInstance* Body = Primitive->GetBodyInstance(Saved.Bone))
                if (Body->IsValidBodyInstance() && Saved.Setup.IsValid() && Body->BodySetup == Saved.Setup)
                {
                    Body->SetInstanceSimulatePhysics(true, true, true);
                    Body->SetBodyTransform(Saved.Transform, ETeleportType::TeleportPhysics, false);
                    Body->SetLinearVelocity(Saved.LinearVelocity, false, false);
                    Body->SetAngularVelocityInRadians(Saved.AngularVelocity, false, false);
                    if (Saved.bWasAwake) Body->WakeInstance();
                    else Body->PutInstanceToSleep();
                }
    for (const FFrozenMesh& Saved : State.Meshes)
        if (USkeletalMeshComponent* Mesh = Saved.Mesh.Get())
        {
            Mesh->bPauseAnims = Saved.bPauseAnims;
            Mesh->bNoSkeletonUpdate = Saved.bNoSkeletonUpdate;
        }
    if (State.bCharacterMovement)
        if (ACharacter* Character = Cast<ACharacter>(Actor))
            if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
            {
                Movement->SetMovementMode(static_cast<EMovementMode>(State.MovementMode), State.CustomMovementMode);
                if (IsValid(Movement)) Movement->Velocity = State.LinearVelocity;
            }
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
        const FFrozenState* FoundFrozen = FrozenActors.Find(WeakActor);
        const FFrozenState FrozenSnapshot = FoundFrozen ? *FoundFrozen : FFrozenState();
        const FFrozenState* Frozen = FoundFrozen ? &FrozenSnapshot : nullptr;
        const FVector Velocity = Frozen ? Frozen->LinearVelocity : Actor->GetVelocity();
        const FVector Destination = Actor->GetActorLocation() + Velocity * FMath::Clamp(DeltaTime, 0.0f, 0.25f);
        if (Destination.ContainsNaN()) continue;
        bool bAvailable = IsDestinationAvailable(Destination);
        if (bDeinitializing || !WeakActor.IsValid() || !RegisteredMovables.Contains(WeakActor)) continue;
        const UCharacterComponent* CharacterState = Actor->FindComponentByClass<UCharacterComponent>();
        const bool bProbePhysics = Frozen ? Frozen->bRagdoll || !Frozen->bCharacterMovement
            : !Cast<ACharacter>(Actor) || (CharacterState && CharacterState->IsRagdollActive());
        const float LookAhead = FMath::Clamp(DeltaTime * 2.0f, 0.05f, 0.25f);
        if (bAvailable && bProbePhysics)
        {
            if (Frozen)
            {
                // Copy before destination queries; EnsureLocationLoaded can re-enter the gate.
                const TArray<FFrozenBody> Bodies = Frozen->Bodies;
                for (const FFrozenBody& Body : Bodies)
                {
                    const FVector Next = Body.Transform.GetLocation() + Body.LinearVelocity * LookAhead;
                    if (!Next.ContainsNaN() && !IsDestinationAvailable(Next)) bAvailable = false;
                }
            }
            else
            {
                TArray<FVector, TInlineAllocator<32>> Destinations;
                VisitSimulatingBodies(Actor, [&Destinations, LookAhead](UPrimitiveComponent*, FBodyInstance* Body, FName)
                {
                    const FVector Next = Body->GetUnrealWorldTransform().GetLocation() + Body->GetUnrealWorldVelocity() * LookAhead;
                    if (!Next.ContainsNaN()) Destinations.Add(Next);
                });
                for (const FVector& Next : Destinations)
                    if (!IsDestinationAvailable(Next)) bAvailable = false;
            }
        }
        // EnsureLocationLoaded can synchronously change world/actor ownership.
        if (bDeinitializing || !WeakActor.IsValid() || !RegisteredMovables.Contains(WeakActor)) continue;
        if (!bAvailable) Freeze(Actor);
        else if (FFrozenState State; FrozenActors.RemoveAndCopyValue(WeakActor, State)) Resume(Actor, State);
    }
}
