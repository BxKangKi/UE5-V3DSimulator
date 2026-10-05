// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "CharacterControllerMovementComponent.generated.h"

class APhysicsVolume;

/**
 * Character movement implementation used by V3DSimulator.
 *
 * The stock MOVE_Swimming implementation assumes that the character is inside an
 * APhysicsVolume whose bWaterVolume flag is set. V3DSimulator deliberately keeps
 * global/local water gameplay queries independent from PhysicsVolume so the same
 * UWaterQuerySubsystem can be used by characters, ragdolls, vehicles and props.
 *
 * When gameplay has already selected MOVE_Swimming, this component therefore uses
 * collision-safe 3D flying physics while keeping MovementMode == MOVE_Swimming.
 * MaxSwimSpeed / BrakingDecelerationSwimming / acceleration are still selected from
 * the normal CharacterMovement swimming settings because the movement mode remains
 * MOVE_Swimming throughout the physics step.
 */
UCLASS(ClassGroup=(Movement), meta=(BlueprintSpawnableComponent))
class V3DSIMULATOR_API UCharacterControllerMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()

public:
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
    virtual float GetGravityZ() const override;
    /** Also called before each predicted/server move, not only once per render frame. */
    void RefreshGravity();
protected:
    virtual void PerformMovement(float DeltaSeconds) override;
    virtual void SimulateMovement(float DeltaSeconds) override;
    virtual void PhysicsRotation(float DeltaTime) override;
private:
    void AlignWithGravity();
    double FieldGravityMagnitude = 0.0;
    bool bInGravityField = false;
protected:
    virtual void PhysSwimming(float DeltaTime, int32 Iterations) override;
    virtual void PhysicsVolumeChanged(APhysicsVolume* NewVolume) override;
};
