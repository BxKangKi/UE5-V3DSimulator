// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

/** Native retry timing. A failed pass remains retryable even when the observer does not move. */
class FV3DStreamingRetrySchedule
{
public:
    void BeginAttempt() { bAttemptFailed = false; }

    void Fail(const double Now)
    {
        // A wave may contain many failed groups. Advance backoff once per attempt, not per node.
        if (bAttemptFailed) return;
        bAttemptFailed = true;
        RetryAfterSeconds = Now + NextDelaySeconds;
        NextDelaySeconds = NextDelaySeconds < 8.0 ? NextDelaySeconds * 2.0 : 8.0;
    }

    bool CanAttempt(const double Now) const { return Now >= RetryAfterSeconds; }
    bool HasFailedAttempt() const { return bAttemptFailed; }
    double GetRetryAfterSeconds() const { return RetryAfterSeconds; }
    void Reset() { *this = FV3DStreamingRetrySchedule(); }

private:
    double RetryAfterSeconds = 0.0;
    double NextDelaySeconds = 0.5;
    bool bAttemptFailed = false;
};
