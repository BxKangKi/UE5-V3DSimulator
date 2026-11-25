// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "World/RailTrackSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Rendering/PositionVertexBuffer.h"
#include "StaticMeshResources.h"

namespace RailTrackPrivate
{
    constexpr float VoxelCellSizeCm = 220.0f;
    constexpr float MinTrackLengthCm = 50.0f;
    constexpr float SegmentStitchMaxDistCm = 220.0f;
    constexpr float SegmentStitchMinDot = 0.65f;

    static FIntVector QuantizeCell(const FVector& P, const float CellSize)
    {
        return FIntVector(
            FMath::FloorToInt(P.X / CellSize),
            FMath::FloorToInt(P.Y / CellSize),
            FMath::FloorToInt(P.Z / CellSize));
    }

    struct FVoxelNode
    {
        FIntVector Coord = FIntVector::ZeroValue;
        FVector SumPos = FVector::ZeroVector;
        double MaxZ = -DBL_MAX;
        int32 Count = 0;
        TArray<int32, TInlineAllocator<12>> Neighbors;

        FVector Centroid() const
        {
            return Count > 0 ? (SumPos / static_cast<double>(Count)) : FVector::ZeroVector;
        }
    };

    static void RunBFS(
        const TArray<FVoxelNode>& Nodes,
        const int32 StartIdx,
        TArray<int32>& OutDist,
        int32& OutFarthestIdx,
        int32& OutMaxDist)
    {
        OutDist.Init(INDEX_NONE, Nodes.Num());
        OutFarthestIdx = StartIdx;
        OutMaxDist = 0;
        if (!Nodes.IsValidIndex(StartIdx)) return;

        TArray<int32> Queue;
        Queue.Reserve(Nodes.Num());
        Queue.Add(StartIdx);
        OutDist[StartIdx] = 0;

        int32 Head = 0;
        while (Head < Queue.Num())
        {
            const int32 Cur = Queue[Head++];
            const int32 CurD = OutDist[Cur];
            if (CurD > OutMaxDist)
            {
                OutMaxDist = CurD;
                OutFarthestIdx = Cur;
            }
            for (const int32 Next : Nodes[Cur].Neighbors)
            {
                if (OutDist[Next] == INDEX_NONE)
                {
                    OutDist[Next] = CurD + 1;
                    Queue.Add(Next);
                }
            }
        }
    }
}

URailTrackSubsystem* URailTrackSubsystem::Get(const UObject* Context)
{
    UWorld* World = IsValid(Context) ? Context->GetWorld() : nullptr;
    return World ? World->GetSubsystem<URailTrackSubsystem>() : nullptr;
}

void URailTrackSubsystem::Deinitialize()
{
    Tracks.Empty();
    Super::Deinitialize();
}

bool URailTrackSubsystem::ExtractCenterlineFromMesh(
    UStaticMesh* Mesh,
    const FTransform& WorldTransform,
    TArray<FVector>& OutPoints,
    bool& OutClosedLoop)
{
    OutPoints.Reset();
    OutClosedLoop = false;

    if (!IsValid(Mesh)) return false;
    const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!RenderData || RenderData->LODResources.IsEmpty()) return false;

    const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
    const FPositionVertexBuffer& PosBuf = LOD0.VertexBuffers.PositionVertexBuffer;
    const uint32 NumVerts = PosBuf.GetNumVertices();
    if (NumVerts < 2) return false;

    // 1. Transform vertices to world space & compute bounds
    TArray<FVector> WorldVerts;
    WorldVerts.Reserve(NumVerts);
    FBox Bounds(ForceInit);
    const uint32 Stride = NumVerts > 65536 ? (NumVerts / 65536) : 1;
    for (uint32 I = 0; I < NumVerts; I += Stride)
    {
        const FVector WPos = WorldTransform.TransformPosition(FVector(PosBuf.VertexPosition(I)));
        if (!WPos.ContainsNaN())
        {
            WorldVerts.Add(WPos);
            Bounds += WPos;
        }
    }
    if (WorldVerts.Num() < 2 || !Bounds.IsValid) return false;

    const FVector Extent = Bounds.GetSize();
    const float LongestSpan = static_cast<float>(Extent.GetMax());
    if (LongestSpan < RailTrackPrivate::MinTrackLengthCm) return false;

    // 2. Coarse spatial voxelization so disconnected left/right rail bars merge into one cross-section
    const float CellSize = FMath::Clamp(RailTrackPrivate::VoxelCellSizeCm, 120.0f, FMath::Max(160.0f, LongestSpan * 0.25f));
    TMap<FIntVector, int32> CellToIndex;
    TArray<RailTrackPrivate::FVoxelNode> Voxels;
    CellToIndex.Reserve(FMath::Min(WorldVerts.Num(), 4096));

    for (const FVector& P : WorldVerts)
    {
        const FIntVector Coord = RailTrackPrivate::QuantizeCell(P, CellSize);
        int32* Found = CellToIndex.Find(Coord);
        int32 Idx = INDEX_NONE;
        if (Found)
        {
            Idx = *Found;
        }
        else
        {
            Idx = Voxels.AddDefaulted();
            Voxels[Idx].Coord = Coord;
            CellToIndex.Add(Coord, Idx);
        }
        RailTrackPrivate::FVoxelNode& Node = Voxels[Idx];
        Node.SumPos += P;
        Node.MaxZ = FMath::Max(Node.MaxZ, P.Z);
        ++Node.Count;
    }

    // Degenerate / very short straight rail: use principal axis projection
    if (Voxels.Num() <= 2)
    {
        const FVector Center = Bounds.GetCenter();
        FVector Axis = FVector::ForwardVector;
        if (Extent.X >= Extent.Y && Extent.X >= Extent.Z) Axis = FVector::ForwardVector;
        else if (Extent.Y >= Extent.X && Extent.Y >= Extent.Z) Axis = FVector::RightVector;
        else Axis = FVector::UpVector;

        double MinProj = DBL_MAX, MaxProj = -DBL_MAX;
        double RailTopZ = Bounds.Max.Z;
        for (const FVector& P : WorldVerts)
        {
            const double Proj = FVector::DotProduct(P - Center, Axis);
            MinProj = FMath::Min(MinProj, Proj);
            MaxProj = FMath::Max(MaxProj, Proj);
        }
        FVector Start = Center + Axis * MinProj;
        FVector End = Center + Axis * MaxProj;
        Start.Z = RailTopZ;
        End.Z = RailTopZ;
        OutPoints.Add(Start);
        OutPoints.Add(End);
        return true;
    }

    // Build 26-neighbor adjacency on occupied voxels
    for (int32 Idx = 0; Idx < Voxels.Num(); ++Idx)
    {
        const FIntVector C = Voxels[Idx].Coord;
        for (int32 dx = -1; dx <= 1; ++dx)
        {
            for (int32 dy = -1; dy <= 1; ++dy)
            {
                for (int32 dz = -1; dz <= 1; ++dz)
                {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    if (const int32* Neighbor = CellToIndex.Find(FIntVector(C.X + dx, C.Y + dy, C.Z + dz)))
                    {
                        Voxels[Idx].Neighbors.Add(*Neighbor);
                    }
                }
            }
        }
    }

    // 3. Find largest connected component & 2-pass BFS geodesic diameter
    TArray<int32> Dist0, Dist1;
    int32 FarA = 0, MaxD0 = 0;
    RailTrackPrivate::RunBFS(Voxels, 0, Dist0, FarA, MaxD0);

    int32 FarB = FarA, MaxD1 = 0;
    RailTrackPrivate::RunBFS(Voxels, FarA, Dist1, FarB, MaxD1);
    if (MaxD1 < 1) return false;

    // 4. Detect closed loop: check if mid-distance ring splits into two far-apart clusters
    const int32 MidDist = MaxD1 / 2;
    TArray<int32> MidRing;
    for (int32 I = 0; I < Voxels.Num(); ++I)
    {
        if (Dist1[I] == MidDist) MidRing.Add(I);
    }
    double MaxMidSpreadSq = 0.0;
    for (int32 A = 0; A < MidRing.Num(); ++A)
    {
        const FVector PA = Voxels[MidRing[A]].Centroid();
        for (int32 B = A + 1; B < MidRing.Num(); ++B)
        {
            MaxMidSpreadSq = FMath::Max(MaxMidSpreadSq, FVector::DistSquared(PA, Voxels[MidRing[B]].Centroid()));
        }
    }
    const bool bLooksLikeLoop = MaxD1 >= 6 && MaxMidSpreadSq > FMath::Square(static_cast<double>(CellSize * 3.0f));

    if (bLooksLikeLoop)
    {
        // Momentum-guided loop walk around the voxel graph
        TArray<bool> Visited;
        Visited.Init(false, Voxels.Num());
        TArray<FVector> LoopPts;
        int32 Cur = FarA;
        Visited[Cur] = true;
        FVector CurPos = Voxels[Cur].Centroid();
        CurPos.Z = Voxels[Cur].MaxZ;
        LoopPts.Add(CurPos);
        FVector PrevDir = FVector::ZeroVector;

        const int32 MaxSteps = Voxels.Num() + 1;
        for (int32 Step = 0; Step < MaxSteps; ++Step)
        {
            int32 BestNext = INDEX_NONE;
            double BestScore = -DBL_MAX;
            for (const int32 N : Voxels[Cur].Neighbors)
            {
                if (Visited[N]) continue;
                const FVector NPos = Voxels[N].Centroid();
                const FVector Dir = (NPos - CurPos).GetSafeNormal();
                const double Score = PrevDir.IsNearlyZero() ? 1.0 : FVector::DotProduct(PrevDir, Dir);
                if (Score > BestScore)
                {
                    BestScore = Score;
                    BestNext = N;
                }
            }
            if (BestNext == INDEX_NONE) break;
            Visited[BestNext] = true;
            // Mark immediate sideways neighbors of Cur as visited so we don't zig-zag across track width
            for (const int32 Side : Voxels[Cur].Neighbors)
            {
                if (Side != BestNext && FVector::DistSquared(Voxels[Side].Centroid(), CurPos) < FMath::Square(CellSize * 1.45f))
                {
                    Visited[Side] = true;
                }
            }
            FVector NextPos = Voxels[BestNext].Centroid();
            NextPos.Z = Voxels[BestNext].MaxZ;
            const FVector StepDir = (NextPos - CurPos).GetSafeNormal();
            if (!StepDir.IsNearlyZero()) PrevDir = StepDir;
            LoopPts.Add(NextPos);
            Cur = BestNext;
            CurPos = NextPos;
        }

        if (LoopPts.Num() >= 4 && FVector::Dist(LoopPts[0], LoopPts.Last()) <= CellSize * 3.5f)
        {
            OutPoints = MoveTemp(LoopPts);
            OutClosedLoop = true;
        }
    }

    // Open track (or loop fallback): group voxels by geodesic distance ring from FarA
    if (OutPoints.IsEmpty())
    {
        struct FRingAccum
        {
            FVector Sum = FVector::ZeroVector;
            double MaxZ = -DBL_MAX;
            int32 Weight = 0;
        };
        TArray<FRingAccum> Rings;
        Rings.SetNum(MaxD1 + 1);
        for (int32 I = 0; I < Voxels.Num(); ++I)
        {
            const int32 D = Dist1[I];
            if (D < 0 || D > MaxD1) continue;
            Rings[D].Sum += Voxels[I].SumPos;
            Rings[D].Weight += Voxels[I].Count;
            Rings[D].MaxZ = FMath::Max(Rings[D].MaxZ, Voxels[I].MaxZ);
        }

        OutPoints.Reserve(Rings.Num());
        for (const FRingAccum& R : Rings)
        {
            if (R.Weight <= 0) continue;
            FVector Pt = R.Sum / static_cast<double>(R.Weight);
            Pt.Z = R.MaxZ; // Rail head top surface
            OutPoints.Add(Pt);
        }
    }

    if (OutPoints.Num() < 2) return false;

    // 5. Light Laplacian smoothing (2 passes) to eliminate voxel quantization jitter
    const int32 N = OutPoints.Num();
    if (N >= 3)
    {
        TArray<FVector> Temp;
        Temp.SetNum(N);
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            for (int32 I = 0; I < N; ++I)
            {
                if (!OutClosedLoop && (I == 0 || I == N - 1))
                {
                    Temp[I] = OutPoints[I];
                }
                else
                {
                    const int32 Prev = (I - 1 + N) % N;
                    const int32 Next = (I + 1) % N;
                    Temp[I] = OutPoints[I] * 0.5 + (OutPoints[Prev] + OutPoints[Next]) * 0.25;
                }
            }
            OutPoints = Temp;
        }
    }

    // 6. Prune near-duplicate points
    TArray<FVector> Cleaned;
    Cleaned.Reserve(OutPoints.Num());
    Cleaned.Add(OutPoints[0]);
    for (int32 I = 1; I < OutPoints.Num(); ++I)
    {
        if (FVector::DistSquared(Cleaned.Last(), OutPoints[I]) >= 100.0) // >= 10 cm
        {
            Cleaned.Add(OutPoints[I]);
        }
    }
    if (Cleaned.Num() < 2) return false;
    OutPoints = MoveTemp(Cleaned);
    return true;
}

void URailTrackSubsystem::BuildTrackKinematics(FV3DRailTrack& Track)
{
    const int32 N = Track.Points.Num();
    Track.ArcLengths.SetNum(N);
    Track.Tangents.SetNum(N);
    Track.UpVectors.SetNum(N);
    Track.WorldBounds = FBox(ForceInit);
    Track.TotalLength = 0.0f;

    if (N < 2) return;

    Track.ArcLengths[0] = 0.0f;
    Track.WorldBounds += Track.Points[0];
    for (int32 I = 1; I < N; ++I)
    {
        Track.TotalLength += static_cast<float>(FVector::Dist(Track.Points[I - 1], Track.Points[I]));
        Track.ArcLengths[I] = Track.TotalLength;
        Track.WorldBounds += Track.Points[I];
    }
    if (Track.bClosedLoop)
    {
        Track.TotalLength += static_cast<float>(FVector::Dist(Track.Points[N - 1], Track.Points[0]));
    }

    for (int32 I = 0; I < N; ++I)
    {
        FVector Tan;
        if (Track.bClosedLoop)
        {
            const int32 Prev = (I - 1 + N) % N;
            const int32 Next = (I + 1) % N;
            Tan = (Track.Points[Next] - Track.Points[Prev]).GetSafeNormal();
        }
        else if (I == 0)
        {
            Tan = (Track.Points[1] - Track.Points[0]).GetSafeNormal();
        }
        else if (I == N - 1)
        {
            Tan = (Track.Points[N - 1] - Track.Points[N - 2]).GetSafeNormal();
        }
        else
        {
            Tan = (Track.Points[I + 1] - Track.Points[I - 1]).GetSafeNormal();
        }
        if (Tan.IsNearlyZero()) Tan = FVector::ForwardVector;
        Track.Tangents[I] = Tan;

        const FVector Right = FVector::CrossProduct(FVector::UpVector, Tan).GetSafeNormal();
        const FVector Up = Right.IsNearlyZero() ? FVector::UpVector : FVector::CrossProduct(Tan, Right).GetSafeNormal();
        Track.UpVectors[I] = Up;
    }
}

int32 URailTrackSubsystem::RegisterRailMeshInstance(
    const AActor* OwnerActor,
    const FName NodeName,
    UStaticMesh* Mesh,
    const FTransform& InstanceWorldTransform)
{
    check(IsInGameThread());
    if (!IsValid(OwnerActor) || !IsValid(Mesh)) return INDEX_NONE;

    // Avoid duplicate registration for the same owner + node
    for (const TPair<int32, FV3DRailTrack>& Pair : Tracks)
    {
        if (Pair.Value.OwnerActor.Get() == OwnerActor && Pair.Value.NodeName == NodeName)
        {
            return Pair.Key;
        }
    }

    TArray<FVector> Centerline;
    bool bClosedLoop = false;
    if (!ExtractCenterlineFromMesh(Mesh, InstanceWorldTransform, Centerline, bClosedLoop))
    {
        return INDEX_NONE;
    }

    const int32 AssignedId = NextTrackId++;
    FV3DRailTrack& Track = Tracks.Add(AssignedId);
    Track.TrackId = AssignedId;
    Track.OwnerActor = OwnerActor;
    Track.NodeName = NodeName;
    Track.Points = MoveTemp(Centerline);
    Track.bClosedLoop = bClosedLoop;
    BuildTrackKinematics(Track);

    if (Track.TotalLength < RailTrackPrivate::MinTrackLengthCm)
    {
        Tracks.Remove(AssignedId);
        return INDEX_NONE;
    }
    return AssignedId;
}

void URailTrackSubsystem::UnregisterRailNode(const AActor* OwnerActor, const FName NodeName)
{
    check(IsInGameThread());
    for (auto It = Tracks.CreateIterator(); It; ++It)
    {
        if (It.Value().OwnerActor.Get() == OwnerActor && It.Value().NodeName == NodeName)
        {
            It.RemoveCurrent();
            return;
        }
    }
}

void URailTrackSubsystem::UnregisterRailsForActor(const AActor* OwnerActor)
{
    check(IsInGameThread());
    for (auto It = Tracks.CreateIterator(); It; ++It)
    {
        if (!It.Value().OwnerActor.IsValid() || It.Value().OwnerActor.Get() == OwnerActor)
        {
            It.RemoveCurrent();
        }
    }
}

void URailTrackSubsystem::SampleSingleTrackClamped(
    const FV3DRailTrack& Track,
    const float DistanceCm,
    FVector& OutLocation,
    FVector& OutTangent,
    FVector& OutUp)
{
    const int32 N = Track.Points.Num();
    if (N < 2 || Track.TotalLength <= KINDA_SMALL_NUMBER)
    {
        OutLocation = N > 0 ? Track.Points[0] : FVector::ZeroVector;
        OutTangent = FVector::ForwardVector;
        OutUp = FVector::UpVector;
        return;
    }

    float D = DistanceCm;
    if (Track.bClosedLoop)
    {
        D = FMath::Fmod(D, Track.TotalLength);
        if (D < 0.0f) D += Track.TotalLength;
    }
    else
    {
        D = FMath::Clamp(D, 0.0f, Track.TotalLength);
    }

    // Handle closing segment on closed loops
    if (Track.bClosedLoop && D >= Track.ArcLengths[N - 1])
    {
        const float SegLen = Track.TotalLength - Track.ArcLengths[N - 1];
        const float Alpha = SegLen > KINDA_SMALL_NUMBER ? FMath::Clamp((D - Track.ArcLengths[N - 1]) / SegLen, 0.0f, 1.0f) : 0.0f;
        OutLocation = FMath::Lerp(Track.Points[N - 1], Track.Points[0], Alpha);
        OutTangent = FMath::Lerp(Track.Tangents[N - 1], Track.Tangents[0], Alpha).GetSafeNormal();
        OutUp = FMath::Lerp(Track.UpVectors[N - 1], Track.UpVectors[0], Alpha).GetSafeNormal();
        if (OutTangent.IsNearlyZero()) OutTangent = Track.Tangents[N - 1];
        if (OutUp.IsNearlyZero()) OutUp = FVector::UpVector;
        return;
    }

    // Binary search for segment
    int32 Lo = 0;
    int32 Hi = N - 1;
    while (Lo + 1 < Hi)
    {
        const int32 Mid = (Lo + Hi) / 2;
        if (Track.ArcLengths[Mid] <= D) Lo = Mid;
        else Hi = Mid;
    }

    const float SegLen = Track.ArcLengths[Lo + 1] - Track.ArcLengths[Lo];
    const float Alpha = SegLen > KINDA_SMALL_NUMBER ? FMath::Clamp((D - Track.ArcLengths[Lo]) / SegLen, 0.0f, 1.0f) : 0.0f;
    OutLocation = FMath::Lerp(Track.Points[Lo], Track.Points[Lo + 1], Alpha);
    OutTangent = FMath::Lerp(Track.Tangents[Lo], Track.Tangents[Lo + 1], Alpha).GetSafeNormal();
    OutUp = FMath::Lerp(Track.UpVectors[Lo], Track.UpVectors[Lo + 1], Alpha).GetSafeNormal();
    if (OutTangent.IsNearlyZero()) OutTangent = Track.Tangents[Lo];
    if (OutUp.IsNearlyZero()) OutUp = FVector::UpVector;
}

bool URailTrackSubsystem::FindConnectedSegment(
    const int32 CurrentTrackId,
    const FVector& ExitPoint,
    const FVector& ExitDirection,
    int32& OutNextTrackId,
    bool& OutEnterAtStart) const
{
    OutNextTrackId = INDEX_NONE;
    OutEnterAtStart = true;

    const double MaxDistSq = FMath::Square(static_cast<double>(RailTrackPrivate::SegmentStitchMaxDistCm));
    double BestScore = -DBL_MAX;

    for (const TPair<int32, FV3DRailTrack>& Pair : Tracks)
    {
        if (Pair.Key == CurrentTrackId) continue;
        const FV3DRailTrack& Candidate = Pair.Value;
        if (Candidate.Points.Num() < 2) continue;

        // Check entering at start (point 0, moving forward along Tangents[0])
        const double DistStartSq = FVector::DistSquared(ExitPoint, Candidate.Points[0]);
        if (DistStartSq <= MaxDistSq)
        {
            const double Dot = FVector::DotProduct(ExitDirection, Candidate.Tangents[0]);
            if (Dot >= RailTrackPrivate::SegmentStitchMinDot)
            {
                const double Score = Dot * 1000.0 - FMath::Sqrt(DistStartSq);
                if (Score > BestScore)
                {
                    BestScore = Score;
                    OutNextTrackId = Pair.Key;
                    OutEnterAtStart = true;
                }
            }
        }

        // Check entering at end (point N-1, moving backward along -Tangents[N-1])
        const int32 LastIdx = Candidate.Points.Num() - 1;
        const double DistEndSq = FVector::DistSquared(ExitPoint, Candidate.Points[LastIdx]);
        if (DistEndSq <= MaxDistSq)
        {
            const double Dot = FVector::DotProduct(ExitDirection, -Candidate.Tangents[LastIdx]);
            if (Dot >= RailTrackPrivate::SegmentStitchMinDot)
            {
                const double Score = Dot * 1000.0 - FMath::Sqrt(DistEndSq);
                if (Score > BestScore)
                {
                    BestScore = Score;
                    OutNextTrackId = Pair.Key;
                    OutEnterAtStart = false;
                }
            }
        }
    }
    return OutNextTrackId != INDEX_NONE;
}

bool URailTrackSubsystem::FindNearestTrack(
    const FVector& WorldLocation,
    const float MaxSearchRadiusCm,
    int32& OutTrackId,
    float& OutDistanceCm,
    FVector& OutLocation,
    FVector& OutTangent,
    FVector& OutUp) const
{
    OutTrackId = INDEX_NONE;
    OutDistanceCm = 0.0f;
    double BestDistSq = FMath::Square(static_cast<double>(FMath::Max(10.0f, MaxSearchRadiusCm)));

    for (const TPair<int32, FV3DRailTrack>& Pair : Tracks)
    {
        const FV3DRailTrack& Track = Pair.Value;
        if (Track.Points.Num() < 2) continue;
        if (Track.WorldBounds.ComputeSquaredDistanceToPoint(WorldLocation) > BestDistSq) continue;

        const int32 SegCount = Track.bClosedLoop ? Track.Points.Num() : (Track.Points.Num() - 1);
        for (int32 I = 0; I < SegCount; ++I)
        {
            const int32 Next = (I + 1) % Track.Points.Num();
            const FVector& A = Track.Points[I];
            const FVector& B = Track.Points[Next];
            const FVector AB = B - A;
            const double LenSq = AB.SizeSquared();
            const double T = LenSq > KINDA_SMALL_NUMBER
                ? FMath::Clamp(FVector::DotProduct(WorldLocation - A, AB) / LenSq, 0.0, 1.0)
                : 0.0;
            const FVector Closest = A + AB * T;
            const double DistSq = FVector::DistSquared(WorldLocation, Closest);
            if (DistSq < BestDistSq)
            {
                BestDistSq = DistSq;
                OutTrackId = Pair.Key;
                OutDistanceCm = Track.ArcLengths[I] + static_cast<float>(FMath::Sqrt(LenSq) * T);
            }
        }
    }

    if (OutTrackId == INDEX_NONE) return false;
    const FV3DRailTrack& Winner = Tracks.FindChecked(OutTrackId);
    SampleSingleTrackClamped(Winner, OutDistanceCm, OutLocation, OutTangent, OutUp);
    return true;
}

bool URailTrackSubsystem::SampleTrackAtDistance(
    int32& InOutTrackId,
    float& InOutDistanceCm,
    float& InOutDirectionSign,
    FVector& OutLocation,
    FVector& OutTangent,
    FVector& OutUp,
    bool& OutReachedOpenEnd) const
{
    OutReachedOpenEnd = false;
    const FV3DRailTrack* Track = Tracks.Find(InOutTrackId);
    if (!Track || Track->Points.Num() < 2) return false;

    for (int32 Hops = 0; Hops < 4; ++Hops)
    {
        if (Track->bClosedLoop)
        {
            InOutDistanceCm = FMath::Fmod(InOutDistanceCm, Track->TotalLength);
            if (InOutDistanceCm < 0.0f) InOutDistanceCm += Track->TotalLength;
            SampleSingleTrackClamped(*Track, InOutDistanceCm, OutLocation, OutTangent, OutUp);
            return true;
        }

        if (InOutDistanceCm > Track->TotalLength)
        {
            const float Overflow = InOutDistanceCm - Track->TotalLength;
            int32 NextId = INDEX_NONE;
            bool bEnterStart = true;
            if (FindConnectedSegment(InOutTrackId, Track->Points.Last(), Track->Tangents.Last(), NextId, bEnterStart))
            {
                const FV3DRailTrack* NextTrack = Tracks.Find(NextId);
                if (NextTrack)
                {
                    InOutTrackId = NextId;
                    Track = NextTrack;
                    if (bEnterStart)
                    {
                        InOutDistanceCm = Overflow;
                    }
                    else
                    {
                        InOutDistanceCm = NextTrack->TotalLength - Overflow;
                        InOutDirectionSign = -InOutDirectionSign;
                    }
                    continue;
                }
            }
            InOutDistanceCm = Track->TotalLength;
            OutReachedOpenEnd = true;
            break;
        }
        else if (InOutDistanceCm < 0.0f)
        {
            const float Underflow = -InOutDistanceCm;
            int32 NextId = INDEX_NONE;
            bool bEnterStart = true;
            if (FindConnectedSegment(InOutTrackId, Track->Points[0], -Track->Tangents[0], NextId, bEnterStart))
            {
                const FV3DRailTrack* NextTrack = Tracks.Find(NextId);
                if (NextTrack)
                {
                    InOutTrackId = NextId;
                    Track = NextTrack;
                    if (bEnterStart)
                    {
                        InOutDistanceCm = Underflow;
                        InOutDirectionSign = -InOutDirectionSign;
                    }
                    else
                    {
                        InOutDistanceCm = NextTrack->TotalLength - Underflow;
                    }
                    continue;
                }
            }
            InOutDistanceCm = 0.0f;
            OutReachedOpenEnd = true;
            break;
        }
        else
        {
            break;
        }
    }

    SampleSingleTrackClamped(*Track, InOutDistanceCm, OutLocation, OutTangent, OutUp);
    return true;
}

bool URailTrackSubsystem::SampleTrackOffset(
    const int32 TrackId,
    const float BaseDistanceCm,
    const float OffsetCm,
    FVector& OutLocation,
    FVector& OutTangent,
    FVector& OutUp) const
{
    int32 TempTrackId = TrackId;
    float TempDist = BaseDistanceCm + OffsetCm;
    float TempDir = 1.0f;
    bool bReachedEnd = false;
    return SampleTrackAtDistance(TempTrackId, TempDist, TempDir, OutLocation, OutTangent, OutUp, bReachedEnd);
}

float URailTrackSubsystem::GetTrackLength(const int32 TrackId) const
{
    const FV3DRailTrack* Track = Tracks.Find(TrackId);
    return Track ? Track->TotalLength : 0.0f;
}
