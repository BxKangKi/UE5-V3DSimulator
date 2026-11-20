// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

/** Bounded request sequence: authored LODs together, then each LOD on its own. */
class FV3DMeshLoadRecovery
{
public:
    void Begin(const int* Indices, const int Count, const bool bAllowMultiLOD)
    {
        Reset();
        for (int I = 0; I < Count && NumLODs < MaxLODs; ++I)
        {
            if (Indices[I] < 0) continue;
            bool bDuplicate = false;
            for (int J = 0; J < NumLODs; ++J) bDuplicate |= LODs[J] == Indices[I];
            if (!bDuplicate) LODs[NumLODs++] = Indices[I];
        }
        bCombined = bAllowMultiLOD && NumLODs > 1;
    }

    bool IsInitialized() const { return NumLODs > 0; }
    int GetRequestCount() const { return bCombined ? NumLODs : (NumLODs > 0 ? 1 : 0); }
    int GetMeshIndex(const int RequestIndex) const { return LODs[bCombined ? RequestIndex : SingleLOD]; }
    bool IsFallback() const { return bFallback; }

    bool AdvanceAfterFailure()
    {
        if (!IsInitialized()) return false;
        if (bCombined) bCombined = false;
        else if (SingleLOD + 1 < NumLODs) ++SingleLOD;
        else return false;
        bFallback = true;
        return true;
    }

    void Reset() { *this = FV3DMeshLoadRecovery(); }

private:
    static constexpr int MaxLODs = 4;
    int LODs[MaxLODs] = {};
    int NumLODs = 0;
    int SingleLOD = 0;
    bool bCombined = false;
    bool bFallback = false;
};

/** Only completed, exhausted payload passes count; slow or cancelled work does not. */
class FV3DMeshGroupRecovery
{
public:
    static constexpr int MaxFailedPasses = 3;
    bool RecordFailedPass()
    {
        if (FailedPasses < MaxFailedPasses) ++FailedPasses;
        return IsUnavailable();
    }
    bool IsUnavailable() const { return FailedPasses >= MaxFailedPasses; }
    int GetFailedPasses() const { return FailedPasses; }

private:
    int FailedPasses = 0;
};
