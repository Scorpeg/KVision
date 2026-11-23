#ifndef STALLWATCH_H
#define STALLWATCH_H

#include <QObject>
#include <QtGlobal>

// QMediaPlayer::MediaStatus values from Qt 5. Kept as ints so the decision does not
// depend on Qt Multimedia, and the tests can call it without a window.
constexpr int kStallStatusLoading = 2;
constexpr int kStallStatusEnd = 7;
constexpr int kStallStatusInvalid = 8;

// A stream that has shown a picture and then gone quiet for this long is restarted.
constexpr int kStallQuietAfterFrameMs = 8000;
// A stream that has never shown a picture is given this long before the first restart.
constexpr int kStallFirstFrameMs = 15000;

// Returns 0 when the tile should be left alone. Otherwise returns the backoff, in
// milliseconds, to store for the next attempt (5s, then 10, 20, and 30). The caller
// restarts the stream when the result is greater than zero.
int evaluateStall(bool enabled, int status, qint64 lastFrameAtMs, qint64 sourceAssignedAtMs,
                  qint64 nowMs, int backoffMs, qint64 lastAttemptAtMs);

// QML calls this. The numbers are the same as evaluateStall.
class StallWatch : public QObject
{
    Q_OBJECT
public:
    explicit StallWatch(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int evaluate(bool enabled, int status, double lastFrameAtMs, double sourceAssignedAtMs,
                             double nowMs, int backoffMs, double lastAttemptAtMs) const
    {
        return evaluateStall(enabled, status, static_cast<qint64>(lastFrameAtMs),
                             static_cast<qint64>(sourceAssignedAtMs), static_cast<qint64>(nowMs),
                             backoffMs, static_cast<qint64>(lastAttemptAtMs));
    }
};

#endif
