// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Velocity-based streaming barrier; no synthetic collision geometry is created.

/**
 * @file StreamingMovementGateSubsystem.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "StreamingMovementGateSubsystem.generated.h"

class UPrimitiveComponent;
class USkeletalMeshComponent;
class UBodySetupCore;

/**
 * Stops a player or physics object before it enters unavailable chunk/mesh data. Each machine
 * evaluates its own readiness: the authority controls authoritative motion, while a slow client
 * may pause only its local presentation. Original linear and angular velocities are restored once
 * both the local chunk and every intersecting model region are ready.
 */
UCLASS()
class V3DSIMULATOR_API UStreamingMovementGateSubsystem final : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return !bDeinitializing && Super::IsTickable(); }

    void RegisterMovable(AActor* Actor);
    void UnregisterMovable(AActor* Actor);
    void SetModelRegionAvailable(const UObject* Owner, FName RegionName, const FBox& WorldBounds, bool bAvailable);
    void ClearModelRegions(const UObject* Owner);

private:
    struct FRegionKey
    {
        TWeakObjectPtr<UObject> Owner;
        FName Name;
        bool operator==(const FRegionKey& Other) const { return Owner == Other.Owner && Name == Other.Name; }
        friend uint32 GetTypeHash(const FRegionKey& Key)
        {
            return HashCombine(GetTypeHash(Key.Owner), GetTypeHash(Key.Name));
        }
    };

    struct FFrozenBody
    {
        FName Bone = NAME_None;
        FTransform Transform;
        FVector LinearVelocity = FVector::ZeroVector;
        FVector AngularVelocity = FVector::ZeroVector;
        TWeakObjectPtr<UPrimitiveComponent> Primitive;
        TWeakObjectPtr<UBodySetupCore> Setup;
        bool bWasAwake = true;
    };
    struct FFrozenMesh
    {
        TWeakObjectPtr<USkeletalMeshComponent> Mesh;
        bool bPauseAnims = false;
        bool bNoSkeletonUpdate = false;
    };
    struct FFrozenState
    {
        TArray<FFrozenBody> Bodies;
        TArray<FFrozenMesh> Meshes;
        FVector LinearVelocity = FVector::ZeroVector;
        uint8 MovementMode = 0;
        uint8 CustomMovementMode = 0;
        bool bCharacterMovement = false;
        bool bRagdoll = false;
    };

    bool bDeinitializing = false;
    bool bUpdating = false;
    TSet<TWeakObjectPtr<AActor>> RegisteredMovables;
    TMap<FRegionKey, FBox> UnavailableRegions;
    TMap<TWeakObjectPtr<AActor>, FFrozenState> FrozenActors;

    bool IsDestinationAvailable(const FVector& Destination) const;
    void Freeze(AActor* Actor);
    void Resume(AActor* Actor, const FFrozenState& State);
};
