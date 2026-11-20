#ifndef HIKVISIONISAPI_H
#define HIKVISIONISAPI_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QVariantMap>
#include <QDateTime>
#include <QMap>
#include <QTimer>
#include <QUuid>
#include "hcnetsdk_compat.h"

struct PlaybackSegment {
    QDateTime startTime;
    QDateTime endTime;
};

struct HikvisionSearchSession {
    QVariantMap recorderInfo;
    int channelId;
    QDateTime startTime;
    QDateTime endTime;
    bool isMonthSearch;
    int year;
    int month;
    int currentPosition;
    QVariantList accumulatedSegments;
    QList<int> accumulatedDays;
    bool sdkFallbackStarted = false;
};

class HikvisionISAPI : public QObject
{
    Q_OBJECT
public:
    explicit HikvisionISAPI(QObject *parent = nullptr);

    // Queries the NVR for available recordings
    // start, end: the date range to query
    Q_INVOKABLE void searchRecordings(const QVariantMap &recorderInfo, int channelId, const QDateTime &start, const QDateTime &end);
    Q_INVOKABLE void searchEvents(const QVariantMap &recorderInfo, int channelId, const QDateTime &start, const QDateTime &end);
    // Pictures of people, vehicles and faces the recorder stored for one camera and time span.
    // token is returned with the result so a newer search can ignore an older one.
    // channels: every camera on the recorder, each {channelId, name}. The search is not limited to one camera.
    Q_INVOKABLE void searchSmartTargets(const QVariantMap &recorderInfo, const QVariantList &channels,
                                        const QDateTime &start, const QDateTime &end,
                                        bool human, bool vehicle, bool face, bool other,
                                        bool line, bool intrusion, bool entrance, int token);
    Q_INVOKABLE void searchMonthAvailability(const QVariantMap &recorderInfo, int channelId, int year, int month);
    Q_INVOKABLE void cancelAllSearches();

signals:
    void searchFinished(const QString &recorderIp, int channelId, const QDateTime &startTime, const QVariantList &segments);
    void eventsFinished(const QString &recorderIp, int channelId, const QDateTime &startTime, const QVariantList &segments);
    // Each hit has image (a local file), timeMs, kind ("human", "vehicle" or "face") and label.
    void smartSearchFinished(int token, const QVariantList &hits, const QString &note);
    void searchFailed(const QString &recorderIp, int channelId, const QDateTime &startTime, const QString &error);
    void monthAvailabilityFinished(const QString &recorderIp, int channelId, int year, int month, const QVariantList &daysWithRecords, bool success);

private slots:
    void onAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator);
    void onReplyFinished();

private:
    void doSearchRequest(const QString &sessionId);
    void startSdkSearch(const QString &sessionId);
    static void fillSdkTime(NET_DVR_TIME &out, const QDateTime &value);
    static QDateTime fromSdkTime(const NET_DVR_TIME_SEARCH &value);

    QNetworkAccessManager *m_netManager;
    QString m_currentUser;
    QString m_currentPassword;
    QMap<QString, HikvisionSearchSession> m_sessions;
    QMap<QString, QNetworkReply*> m_activeReplies;
};

#endif // HIKVISIONISAPI_H
