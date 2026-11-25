// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RailTrackSubsystem.generated.h"

class AActor;
class UStaticMesh;

/** One world-space rail centerline extracted from a ;RAIL mesh instance. */
struct FV3DRailTrack
{
    int32 TrackId = INDEX_NONE;
    TWeakObjectPtr<const AActor> OwnerActor;
    FName NodeName = NAME_None;
    TArray<FVector> Points;
    TArray<FVector> Tangents;
    TArray<FVector> UpVectors;
    TArray<float> ArcLengths;
    FBox WorldBounds = FBox(ForceInit);
    float TotalLength = 0.0f;
    bool bClosedLoop = false;
};

/**
 * World subsystem that extracts and indexes medial centerlines from streamed ;RAIL meshes,
 * enabling Train vehicles to follow curved, sloped, looped, and multi-segment tracks.
 */
UCLASS()
class V3DSIMULATOR_API URailTrackSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    static URailTrackSubsystem* Get(const UObject* Context);

    virtual void Deinitialize() override;

    /** Extracts a world-space centerline from a ;RAIL static mesh instance and registers it. */
    int32 RegisterRailMeshInstance(
        const AActor* OwnerActor,
        FName NodeName,
        UStaticMesh* Mesh,
        const FTransform& InstanceWorldTransform);

    /** Removes a single registered rail instance by owner + node name. */
    void UnregisterRailNode(const AActor* OwnerActor, FName NodeName);

    /** Removes all rail tracks owned by the given actor (e.g. when a world chunk unloads). */
    void UnregisterRailsForActor(const AActor* OwnerActor);

    /** Finds the closest point on any registered rail track within MaxSearchRadiusCm. */
    bool FindNearestTrack(
        const FVector& WorldLocation,
        float MaxSearchRadiusCm,
        int32& OutTrackId,
        float& OutDistanceCm,
        FVector& OutLocation,
        FVector& OutTangent,
        FVector& OutUp) const;

    /**
     * Samples a track at DistanceCm. Automatically handles closed loops and seamlessly transitions
     * across connected multi-segment ;RAIL meshes when crossing an endpoint.
     */
    bool SampleTrackAtDistance(
        int32& InOutTrackId,
        float& InOutDistanceCm,
        float& InOutDirectionSign,
        FVector& OutLocation,
        FVector& OutTangent,
        FVector& OutUp,
        bool& OutReachedOpenEnd) const;

    /** Read-only sample at an offset from (TrackId, DistanceCm) without mutating caller state. */
    bool SampleTrackOffset(
        int32 TrackId,
        float BaseDistanceCm,
        float OffsetCm,
        FVector& OutLocation,
        FVector& OutTangent,
        FVector& OutUp) const;

    float GetTrackLength(int32 TrackId) const;
    bool HasTracks() const { return !Tracks.IsEmpty(); }

private:
    TMap<int32, FV3DRailTrack> Tracks;
    int32 NextTrackId = 1;

    static bool ExtractCenterlineFromMesh(
        UStaticMesh* Mesh,
        const FTransform& WorldTransform,
        TArray<FVector>& OutPoints,
        bool& OutClosedLoop);

    static void BuildTrackKinematics(FV3DRailTrack& Track);

    static void SampleSingleTrackClamped(
        const FV3DRailTrack& Track,
        float DistanceCm,
        FVector& OutLocation,
        FVector& OutTangent,
        FVector& OutUp);

    bool FindConnectedSegment(
        int32 CurrentTrackId,
        const FVector& ExitPoint,
        const FVector& ExitDirection,
        int32& OutNextTrackId,
        bool& OutEnterAtStart) const;
};
