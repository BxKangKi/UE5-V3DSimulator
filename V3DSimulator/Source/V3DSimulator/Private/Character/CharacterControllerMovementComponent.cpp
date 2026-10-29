// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Character/CharacterControllerMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "Character/CharacterComponent.h"
#include "Character/SwimmingSurfaceMath.h"
#include "GameFramework/Character.h"
#include "Gravity/GravityFieldSubsystem.h"
#include "Gravity/GravityFieldTypes.h"
#include "Engine/World.h"

void UCharacterControllerMovementComponent::PhysSwimming(const float DeltaTime, const int32 Iterations)
{
    if (DeltaTime <= UE_SMALL_NUMBER)
    {
        return;
    }

    // Physics/recovery owns the capsule anchor in this interval. Keep the Swimming mode
    // for animation/water queries, but do not run a second movement integrator.
    const UCharacterComponent* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && State->IsRagdollTransitionInProgress())
    {
        StopMovementImmediately();
        ClearAccumulatedForces();
        ConsumeInputVector();
        return;
    }

    if (State && State->IsSwimmingSurfaceHeld())
    {
        // Final input gate also catches acceleration queued before surface capture.
        // Forces, impulses and root motion affect Velocity separately and survive.
        Acceleration.Z = 0.0f;
    }
    const double BeforeZ = UpdatedComponent ? UpdatedComponent->GetComponentLocation().Z : 0.0;
    const float BeforeVelocityZ = static_cast<float>(Velocity.Z);

    // UCharacterMovementComponent::PhysSwimming ultimately uses the current
    // PhysicsVolume's bWaterVolume flag (including inside Swim()) to decide that
    // the character has left water. V3DSimulator water is intentionally queried
    // through UWaterQuerySubsystem instead, so calling the stock implementation
    // would make MOVE_Swimming oscillate back to Falling every movement tick.
    //
    // PhysFlying already provides the collision-safe unrestricted 3D movement we
    // need here and does not require a water PhysicsVolume. MovementMode remains
    // MOVE_Swimming, therefore GetMaxSpeed()/GetMaxBrakingDeceleration() continue
    // to use MaxSwimSpeed and BrakingDecelerationSwimming rather than flying values.
    PhysFlying(DeltaTime, Iterations);

    float SurfaceDeltaZ = 0.0f;
    if (HasValidData() && MovementMode == MOVE_Swimming && IsValid(State)
        && SwimmingSurfaceMath::CanCorrectHeight(BeforeVelocityZ, static_cast<float>(Velocity.Z),
            UpdatedComponent->GetComponentLocation().Z - BeforeZ)
        && State->GetSwimmingSurfaceCorrection(DeltaTime, SurfaceDeltaZ))
    {
        FHitResult SurfaceHit;
        // WaterActor defines a horizontal world-Z plane even with custom gravity.
        // Sweep the capsule so surfacing cannot teleport through a low ceiling.
        SafeMoveUpdatedComponent(FVector(0.0f, 0.0f, SurfaceDeltaZ),
            UpdatedComponent->GetComponentQuat(), true, SurfaceHit, ETeleportType::None);
    }
}

void UCharacterControllerMovementComponent::PhysicsVolumeChanged(APhysicsVolume* NewVolume)
{
    if (MovementMode == MOVE_Swimming && (!NewVolume || !NewVolume->bWaterVolume))
    {
        // The authoritative exit decision is made by ACharacterController using
        // UWaterQuerySubsystem. The stock implementation would immediately switch
        // MOVE_Swimming to MOVE_Falling merely because this is not a water volume.
        return;
    }

    Super::PhysicsVolumeChanged(NewVolume);
}

void UCharacterControllerMovementComponent::RefreshGravity()
{
    UWorld* World = GetWorld();
    if (!World || !CharacterOwner || !UpdatedComponent) return;
    const auto* Fields = World->GetSubsystem<UGravityFieldSubsystem>();
    FVector GravityAcceleration;
    bInGravityField = Fields && Fields->Sample(UpdatedComponent->GetComponentLocation(), CharacterOwner, GravityAcceleration);
    const FVector OldDirection = GetGravityDirection();
    FVector Direction = World->GetGravityZ() > 0.0f ? FVector::UpVector : FVector::DownVector;
    FieldGravityMagnitude = 0.0;
    if (bInGravityField)
    {
        FieldGravityMagnitude = GravityAcceleration.Size();
        // A zero-g field/field centre has no preferred up. Retain the last valid frame.
        Direction = FieldGravityMagnitude > UE_SMALL_NUMBER ? GravityAcceleration / FieldGravityMagnitude : OldDirection;
    }
    if (!Direction.Equals(OldDirection, 1.e-6))
    {
        SetGravityDirection(Direction);
        bForceNextFloorCheck = true;
        CurrentFloor.Clear();
        // A floor that belonged to a different gravity hemisphere is no longer support.
        if (IsMovingOnGround() && FVector::DotProduct(OldDirection, Direction) < 0.9659258)
            SetMovementMode(MOVE_Falling);
    }
    // Physics interaction must use the same acceleration as locomotion after a field transition.
    const float Magnitude = FMath::Abs(GetGravityZ());
    StandingDownwardForceScale = Magnitude > UE_SMALL_NUMBER ? 1.0f : 0.0f;
}
float UCharacterControllerMovementComponent::GetGravityZ() const
{
    return bInGravityField ? -static_cast<float>(FieldGravityMagnitude) * FMath::Max(0.0f, GravityScale)
        : Super::GetGravityZ();
}
void UCharacterControllerMovementComponent::AlignWithGravity()
{
    if (!HasValidData() || !UpdatedComponent) return;
    const auto* State = CharacterOwner->FindComponentByClass<UCharacterComponent>();
    if (State && (State->IsRagdollTransitionInProgress() || State->IsStreamingMovementSuspended())) return;
    const FVector Up = -GetGravityDirection();
    const FQuat Current = UpdatedComponent->GetComponentQuat();
    const FQuat Target = (FQuat::FindBetweenNormals(Current.GetUpVector(), Up) * Current).GetNormalized();
    if (!Current.Equals(Target, 1.e-6)) MoveUpdatedComponent(FVector::ZeroVector, Target, true);
}
void UCharacterControllerMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    const UCharacterComponent* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && State->IsStreamingMovementSuspended())
    {
        StopMovementImmediately();
        ClearAccumulatedForces();
        ConsumeInputVector();
        return;
    }
    RefreshGravity();
    AlignWithGravity();
    Super::TickComponent(DeltaTime, TickType, TickFunction);
}
void UCharacterControllerMovementComponent::PerformMovement(float DeltaSeconds)
{
    const UCharacterComponent* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && State->IsStreamingMovementSuspended())
    {
        StopMovementImmediately();
        ClearAccumulatedForces();
        ConsumeInputVector();
        return;
    }
    RefreshGravity();
    AlignWithGravity();
    Super::PerformMovement(DeltaSeconds);
}
void UCharacterControllerMovementComponent::SimulateMovement(float DeltaSeconds)
{
    const UCharacterComponent* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && State->IsStreamingMovementSuspended())
    {
        StopMovementImmediately();
        ClearAccumulatedForces();
        ConsumeInputVector();
        return;
    }
    RefreshGravity();
    AlignWithGravity();
    Super::SimulateMovement(DeltaSeconds);
}
void UCharacterControllerMovementComponent::PhysicsRotation(float DeltaTime)
{
    const auto* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && (State->IsRagdollTransitionInProgress() || State->IsStreamingMovementSuspended())) return;
    Super::PhysicsRotation(DeltaTime);
    AlignWithGravity();
}
