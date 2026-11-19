// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

/** Native-only coalescing clock. A cooldown must defer the last unload, never discard it. */
class FV3DStreamingReclaimSchedule
{
public:
    void Request(double Now)
    {
        if (bPending) return; // Continuous streaming cannot keep pushing the deadline back.
        bPending = true;
        DueSeconds = Now > NextAllowedSeconds ? Now : NextAllowedSeconds;
    }
    bool ConsumeIfDue(double Now)
    {
        if (!bPending || Now < DueSeconds) return false;
        bPending = false;
        NextAllowedSeconds = Now + 1.0;
        return true;
    }
    bool IsPending() const { return bPending; }
    void Reset() { bPending = false; DueSeconds = NextAllowedSeconds = 0.0; }
private:
    bool bPending = false;
    double DueSeconds = 0.0;
    double NextAllowedSeconds = 0.0;
};
