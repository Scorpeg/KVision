#include "stallwatch.h"

int evaluateStall(bool enabled, int status, qint64 lastFrameAtMs, qint64 sourceAssignedAtMs,
                  qint64 nowMs, int backoffMs, qint64 lastAttemptAtMs)
{
    if (!enabled || status == kStallStatusLoading)
        return 0;

    bool quiet = false;
    if (status == kStallStatusEnd || status == kStallStatusInvalid)
        quiet = true;
    else if (lastFrameAtMs > 0)
        quiet = nowMs - lastFrameAtMs >= kStallQuietAfterFrameMs;
    else if (sourceAssignedAtMs > 0)
        quiet = nowMs - sourceAssignedAtMs >= kStallFirstFrameMs;
    if (!quiet)
        return 0;

    if (lastAttemptAtMs > 0 && backoffMs > 0 && nowMs - lastAttemptAtMs < backoffMs)
        return 0;

    if (backoffMs <= 0)
        return 5000;
    const int next = backoffMs * 2;
    return next > 30000 ? 30000 : next;
}
