#include "hikvisionisapi.h"
#include <QNetworkRequest>
#include <QUrl>
#include <QXmlStreamReader>
#include <QDebug>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryFile>
#include <QDir>
#include <QImage>
#include <algorithm>
#include <cstring>
#include "hikvisionmanager.h"

HikvisionISAPI::HikvisionISAPI(QObject *parent)
    : QObject(parent)
{
    m_netManager = new QNetworkAccessManager(this);
    connect(m_netManager, &QNetworkAccessManager::authenticationRequired,
            this, &HikvisionISAPI::onAuthenticationRequired);
}

void HikvisionISAPI::searchRecordings(const QVariantMap &recorderInfo, int channelId, const QDateTime &start, const QDateTime &end)
{
    QString sessionId = QUuid::createUuid().toString();
    HikvisionSearchSession session;
    session.recorderInfo = recorderInfo;
    session.channelId = channelId;
    session.startTime = start;
    session.endTime = end;
    session.isMonthSearch = false;
    session.currentPosition = 1;
    m_sessions[sessionId] = session;

    doSearchRequest(sessionId);
}

// What the recorder says a saved event was, for the colour on the timeline. Motion is its own group; the smart
// events carry a minor type (line crossing, field detection = intrusion, region entrance, region exit...).
static QString eventKind(WORD major, WORD minor)
{
    if (major == EVENT_MOT_DET)
        return QStringLiteral("motion");
    if (major == EVENT_VCA_DETECTION) {
        switch (minor) {
        case EVENT_VCA_TRAVERSE_PLANE: return QStringLiteral("line");
        case EVENT_FIELD_DETECTION: return QStringLiteral("intrusion");
        case EVENT_ENTER_REGION: return QStringLiteral("enter");
        case EVENT_EXIT_REGION: return QStringLiteral("exit");
        default: break;
        }
    }
    return QStringLiteral("other");
}

void HikvisionISAPI::searchEvents(const QVariantMap &recorderInfo, int channelId, const QDateTime &start, const QDateTime &end)
{
    HikvisionSearchSession session;
    session.recorderInfo = recorderInfo;
    session.channelId = channelId;
    session.startTime = start;
    session.endTime = end;
    session.isMonthSearch = false;

    QThread *worker = QThread::create([this, session]() {
        const QString ip = session.recorderInfo.value("ip").toString();
        const int port = session.recorderInfo.value("sdkPort").toInt() > 0
            ? session.recorderInfo.value("sdkPort").toInt()
            : session.recorderInfo.value("port").toInt();
        const QString username = session.recorderInfo.value("username").toString();
        const QString password = session.recorderInfo.value("password").toString();

        NET_DVR_DEVICEINFO_V40 deviceInfo;
        std::memset(&deviceInfo, 0, sizeof(deviceInfo));
        const LONG userId = HikvisionManager::instance()->loginShared(ip, port > 0 ? port : 8000, username, password, deviceInfo);
        QVariantList segments;
        if (userId >= 0) {
            int channel = session.channelId;
            const auto &info = deviceInfo.struDeviceV30;
            if (info.byChanNum == 0 && info.byIPChanNum > 0)
                channel = session.channelId + info.byStartDChan - 1;
            else if (info.byChanNum > 0 && session.channelId > info.byChanNum)
                channel = info.byStartDChan + (session.channelId - info.byChanNum) - 1;
            else if (info.byChanNum > 0)
                channel = session.channelId + info.byStartChan - 1;

            // Continuous files do not carry the red smart marks. Those live in the event log.
            const WORD majorTypes[] = {EVENT_MOT_DET, EVENT_VCA_DETECTION, EVENT_VCA_BEHAVIOR};
            for (WORD majorType : majorTypes) {
                NET_DVR_SEARCH_EVENT_PARAM_V40 param;
                std::memset(&param, 0, sizeof(param));
                param.wMajorType = majorType;
                param.wMinorType = 0xffff;
                param.byLockType = 0xff;
                fillSdkTime(param.struStartTime, session.startTime);
                fillSdkTime(param.struEndTime, session.endTime);
                if (majorType == EVENT_MOT_DET) {
                    param.uSeniorParam.struMotionParam.wMotDetChanNo[0] = static_cast<WORD>(channel);
                } else if (majorType == EVENT_VCA_BEHAVIOR) {
                    param.uSeniorParam.struVcaParam.wChanNo[0] = static_cast<WORD>(channel);
                    param.uSeniorParam.struVcaParam.byRuleID = 0xff;
                } else {
                    param.uSeniorParam.struVCADetect.wChanNo[0] = static_cast<WORD>(channel);
                }

                const LONG findHandle = NET_DVR_FindFileByEvent_V40(userId, &param);
                if (findHandle < 0) {
                    qDebug() << "[ISAPI] Event search failed for channel" << channel
                               << "major" << majorType << "error" << NET_DVR_GetLastError();
                    continue;
                }
                int accepted = 0;
                NET_DVR_SEARCH_EVENT_RET_V40 event;
                int guard = 0;
                while (guard++ < 4000) {
                    const LONG state = NET_DVR_FindNextEvent_V40(findHandle, &event);
                    if (state == 1000) {
                        const NET_DVR_TIME &rawStart = event.struStartTime;
                        const NET_DVR_TIME &rawEnd = event.struEndTime;
                        QDateTime eventStart(QDate(rawStart.dwYear, rawStart.dwMonth, rawStart.dwDay),
                                             QTime(rawStart.dwHour, rawStart.dwMinute, rawStart.dwSecond));
                        QDateTime eventEnd(QDate(rawEnd.dwYear, rawEnd.dwMonth, rawEnd.dwDay),
                                           QTime(rawEnd.dwHour, rawEnd.dwMinute, rawEnd.dwSecond));
                        if (!eventStart.isValid())
                            continue;
                        if (!eventEnd.isValid() || eventEnd <= eventStart)
                            eventEnd = eventStart.addSecs(10);
                        if (accepted < 5) {
                            qDebug() << "[ISAPI] Event channel" << channel
                                     << "major" << event.wMajorType << "minor" << event.wMinorType
                                     << eventStart.toString(Qt::ISODate) << eventEnd.toString(Qt::ISODate);
                        }
                        QVariantMap segment;
                        segment["startTime"] = eventStart.toMSecsSinceEpoch();
                        segment["endTime"] = eventEnd.toMSecsSinceEpoch();
                        segment["timeIsLocal"] = true;
                        segment["event"] = true;
                        segment["kind"] = eventKind(event.wMajorType, event.wMinorType);
                        segments.append(segment);
                        accepted++;
                    } else if (state == 1002) {
                        QThread::msleep(10);
                    } else {
                        break;
                    }
                }
                NET_DVR_FindClose_V30(findHandle);
                qDebug() << "[ISAPI] Event search channel" << channel << "major" << majorType << "accepted" << accepted;
            }
            qDebug() << "[ISAPI] Event search finished for channel" << channel << "events" << segments.count();
        }

        QMetaObject::invokeMethod(this, [this, session, segments]() {
            emit eventsFinished(session.recorderInfo.value("ip").toString(), session.channelId, session.startTime, segments);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    worker->start();
}

void HikvisionISAPI::searchMonthAvailability(const QVariantMap &recorderInfo, int channelId, int year, int month)
{
    QString sessionId = QUuid::createUuid().toString();
    HikvisionSearchSession session;
    session.recorderInfo = recorderInfo;
    session.channelId = channelId;
    
    QDate firstDay(year, month, 1);
    QDate lastDay(year, month, firstDay.daysInMonth());
    
    session.startTime = QDateTime(firstDay, QTime(0, 0, 0), Qt::LocalTime);
    session.endTime = QDateTime(lastDay, QTime(23, 59, 59), Qt::LocalTime);
    session.isMonthSearch = true;
    session.year = year;
    session.month = month;
    session.currentPosition = 1;
    m_sessions[sessionId] = session;

    doSearchRequest(sessionId);
}

void HikvisionISAPI::doSearchRequest(const QString &sessionId)
{
    if (!m_sessions.contains(sessionId)) return;
    const HikvisionSearchSession &session = m_sessions[sessionId];

    QString ip = session.recorderInfo["ip"].toString();
    int port = session.recorderInfo.contains("httpPort") ? session.recorderInfo["httpPort"].toInt() : 80;
    if (port <= 0) port = 80; // fallback if invalid
    m_currentUser = session.recorderInfo["username"].toString();
    m_currentPassword = session.recorderInfo["password"].toString();

    QUrl url(QString("http://%1:%2/ISAPI/ContentMgmt/search").arg(ip).arg(port));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml");

    QString startTimeStr = session.startTime.toUTC().toString("yyyy-MM-ddThh:mm:ss") + "Z";
    QString endTimeStr = session.endTime.toUTC().toString("yyyy-MM-ddThh:mm:ss") + "Z";

    qDebug() << "[ISAPI] doSearchRequest session:" << sessionId
             << "channelId:" << session.channelId
             << "startTime (orig):" << session.startTime.toString(Qt::ISODate)
             << "startTime (local):" << session.startTime.toLocalTime().toString(Qt::ISODate)
             << "startTimeStr:" << startTimeStr
             << "endTimeStr:" << endTimeStr
             << "pos:" << session.currentPosition;

    QString xmlPayload = QString(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<CMSearchDescription xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">\n"
        "  <searchID>%1</searchID>\n"
        "  <trackList>\n"
        "    <trackID>%2</trackID>\n"
        "  </trackList>\n"
        "  <timeSpanList>\n"
        "    <timeSpan>\n"
        "      <startTime>%3</startTime>\n"
        "      <endTime>%4</endTime>\n"
        "    </timeSpan>\n"
        "  </timeSpanList>\n"
        "  <maxResults>1000</maxResults>\n"
        "  <searchResultPostion>%5</searchResultPostion>\n"
        "  <metadataList>\n"
        "    <metadataDescriptor>//recordType.meta.std-cgi.com</metadataDescriptor>\n"
        "  </metadataList>\n"
        "</CMSearchDescription>\n"
    ).arg(sessionId).arg(session.channelId * 100 + 1).arg(startTimeStr).arg(endTimeStr).arg(session.currentPosition);

    QNetworkReply *reply = m_netManager->post(request, xmlPayload.toUtf8());
    reply->setProperty("sessionId", sessionId);
    reply->setProperty("username", m_currentUser);
    reply->setProperty("password", m_currentPassword);
    m_activeReplies[sessionId] = reply;
    connect(reply, &QNetworkReply::finished, this, &HikvisionISAPI::onReplyFinished);

    const bool standaloneCamera = session.channelId <= 1;
    if (standaloneCamera) {
        qWarning() << "[ISAPI] Standalone camera; using SDK recording search";
        startSdkSearch(sessionId);
        return;
    }
    QTimer::singleShot(8000, this, [this, sessionId]() {
        if (!m_sessions.contains(sessionId) || m_sessions[sessionId].sdkFallbackStarted)
            return;
        qWarning() << "[ISAPI] Search timed out; falling back to SDK recording search";
        if (m_activeReplies.contains(sessionId) && m_activeReplies[sessionId]) {
            m_activeReplies[sessionId]->abort();
        } else {
            startSdkSearch(sessionId);
        }
    });
}

void HikvisionISAPI::onAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
    if (reply) {
        QString user = reply->property("username").toString();
        QString pass = reply->property("password").toString();
        if (!user.isEmpty()) {
            authenticator->setUser(user);
            authenticator->setPassword(pass);
            return;
        }
    }
    authenticator->setUser(m_currentUser);
    authenticator->setPassword(m_currentPassword);
}

void HikvisionISAPI::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    QString sessionId = reply->property("sessionId").toString();
    m_activeReplies.remove(sessionId);
    reply->deleteLater();

    if (!m_sessions.contains(sessionId)) {
        return; // Session cancelled or already finished
    }
    
    HikvisionSearchSession &session = m_sessions[sessionId];
    QString recorderIp = session.recorderInfo["ip"].toString();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[ISAPI] Search failed:" << reply->errorString();
        if (session.isMonthSearch) {
            QVariantList daysList;
            for (int d : session.accumulatedDays) daysList.append(d);
            emit monthAvailabilityFinished(recorderIp, session.channelId, session.year, session.month, daysList, false);
        } else {
            if (session.accumulatedSegments.count() > 0) {
                emit searchFinished(recorderIp, session.channelId, session.startTime, session.accumulatedSegments);
            } else {
                emit searchFailed(recorderIp, session.channelId, session.startTime, reply->errorString());
            }
        }
        if (!session.sdkFallbackStarted) {
            startSdkSearch(sessionId);
            return;
        }
        m_sessions.remove(sessionId);
        return;
    }

    QByteArray response = reply->readAll();
    QXmlStreamReader xml(response);

    bool inMatchList = false;
    bool inMatchItem = false;
    QString responseStatusStrg;
    
    QDateTime currentStart;
    QDateTime currentEnd;
    
    int matchItemsCount = 0;

    while (!xml.atEnd() && !xml.hasError()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            QString name = xml.name().toString();
            if (name == "responseStatusStrg") {
                responseStatusStrg = xml.readElementText().trimmed();
            } else if (name == "matchList") {
                inMatchList = true;
            } else if (inMatchList && name == "searchMatchItem") {
                inMatchItem = true;
                matchItemsCount++;
            } else if (inMatchItem && name == "startTime") {
                QString val = xml.readElementText();
                currentStart = QDateTime::fromString(val, Qt::ISODate).toLocalTime();
            } else if (inMatchItem && name == "endTime") {
                QString val = xml.readElementText();
                currentEnd = QDateTime::fromString(val, Qt::ISODate).toLocalTime();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            QString name = xml.name().toString();
            if (name == "matchList") {
                inMatchList = false;
            } else if (name == "searchMatchItem") {
                inMatchItem = false;
                if (currentStart.isValid() && currentEnd.isValid()) {
                    if (session.isMonthSearch) {
                        int day = currentStart.date().day();
                        if (!session.accumulatedDays.contains(day)) {
                            session.accumulatedDays.append(day);
                        }
                    } else {
                        QVariantMap segment;
                        segment["startTime"] = currentStart.toMSecsSinceEpoch();
                        segment["endTime"] = currentEnd.toMSecsSinceEpoch();
                        session.accumulatedSegments.append(segment);
                    }
                }
                currentStart = QDateTime();
                currentEnd = QDateTime();
            }
        }
    }

    if (xml.hasError()) {
        qWarning() << "[ISAPI] XML parsing error:" << xml.errorString();
        if (session.isMonthSearch) {
            QVariantList daysList;
            for (int d : session.accumulatedDays) daysList.append(d);
            emit monthAvailabilityFinished(recorderIp, session.channelId, session.year, session.month, daysList, false);
        } else {
            emit searchFailed(recorderIp, session.channelId, session.startTime, xml.errorString());
        }
        m_sessions.remove(sessionId);
        return;
    }

    if (matchItemsCount == 0) {
        // Done paginating
        if (session.isMonthSearch) {
            QVariantList daysList;
            for (int d : session.accumulatedDays) {
                daysList.append(d);
            }
            qDebug() << "[ISAPI] Month availability finished (no more matches) for channel:" << session.channelId << "days:" << daysList;
            emit monthAvailabilityFinished(recorderIp, session.channelId, session.year, session.month, daysList, true);
        } else {
            qDebug() << "[ISAPI] Day search finished (no more matches) for channel:" << session.channelId << "segments:" << session.accumulatedSegments.count();
            emit searchFinished(recorderIp, session.channelId, session.startTime, session.accumulatedSegments);
        }
        m_sessions.remove(sessionId);
        return;
    }

    // If responseStatusStrg is MORE, there are more pages (limit to 100000 results to avoid infinite loop)
    if (responseStatusStrg == "MORE" && session.currentPosition < 100000) {
        qDebug() << "[ISAPI] Session" << sessionId << "status MORE; paginating next page from:" << (session.currentPosition + matchItemsCount);
        session.currentPosition += matchItemsCount;
        doSearchRequest(sessionId); // Fetch next page
    } else {
        // Done paginating
        if (session.isMonthSearch) {
            QVariantList daysList;
            for (int d : session.accumulatedDays) {
                daysList.append(d);
            }
            qDebug() << "[ISAPI] Month availability finished for channel:" << session.channelId << "days:" << daysList;
            emit monthAvailabilityFinished(recorderIp, session.channelId, session.year, session.month, daysList, true);
        } else {
            qDebug() << "[ISAPI] Day search finished for channel:" << session.channelId << "segments:" << session.accumulatedSegments.count();
            emit searchFinished(recorderIp, session.channelId, session.startTime, session.accumulatedSegments);
        }
        m_sessions.remove(sessionId);
    }
}

void HikvisionISAPI::fillSdkTime(NET_DVR_TIME &out, const QDateTime &value)
{
    out.dwYear = value.date().year();
    out.dwMonth = value.date().month();
    out.dwDay = value.date().day();
    out.dwHour = value.time().hour();
    out.dwMinute = value.time().minute();
    out.dwSecond = value.time().second();
}

QDateTime HikvisionISAPI::fromSdkTime(const NET_DVR_TIME_SEARCH &value)
{
    return QDateTime(QDate(value.wYear, value.byMonth, value.byDay),
                     QTime(value.byHour, value.byMinute, value.bySecond));
}

void HikvisionISAPI::startSdkSearch(const QString &sessionId)
{
    if (!m_sessions.contains(sessionId))
        return;
    HikvisionSearchSession session = m_sessions.take(sessionId);
    if (session.sdkFallbackStarted)
        return;
    session.sdkFallbackStarted = true;

    QThread *worker = QThread::create([this, session]() {
        const QString ip = session.recorderInfo.value("ip").toString();
        const int port = session.recorderInfo.value("sdkPort").toInt() > 0
            ? session.recorderInfo.value("sdkPort").toInt()
            : session.recorderInfo.value("port").toInt();
        const QString username = session.recorderInfo.value("username").toString();
        const QString password = session.recorderInfo.value("password").toString();

        NET_DVR_DEVICEINFO_V40 deviceInfo;
        std::memset(&deviceInfo, 0, sizeof(deviceInfo));
        LONG userId = HikvisionManager::instance()->loginShared(ip, port > 0 ? port : 8000, username, password, deviceInfo);
        if (userId < 0) {
            QMetaObject::invokeMethod(this, [this, session]() {
                emit searchFailed(session.recorderInfo.value("ip").toString(), session.channelId, session.startTime, tr("SDK login failed"));
            }, Qt::QueuedConnection);
            return;
        }

        int channel = session.channelId;
        const auto &info = deviceInfo.struDeviceV30;
        if (info.byChanNum == 0 && info.byIPChanNum > 0)
            channel = session.channelId + info.byStartDChan - 1;
        else if (info.byChanNum > 0 && session.channelId > info.byChanNum)
            channel = info.byStartDChan + (session.channelId - info.byChanNum) - 1;
        else if (info.byChanNum > 0)
            channel = session.channelId + info.byStartChan - 1;

        NET_DVR_FILECOND_V40 cond;
        std::memset(&cond, 0, sizeof(cond));
        cond.lChannel = channel;
        cond.dwFileType = 0xFF;
        cond.dwIsLocked = 0xFF;
        fillSdkTime(cond.struStartTime, session.startTime);
        fillSdkTime(cond.struStopTime, session.endTime);

        QVariantList segments;
        QList<int> days;
        LONG findHandle = NET_DVR_FindFile_V40(userId, &cond);
        if (findHandle >= 0) {
            NET_DVR_FINDDATA_V50 data;
            while (true) {
                int state = NET_DVR_FindNextFile_V50(findHandle, &data);
                if (state == 1000) {
                    QDateTime start = fromSdkTime(data.struStartTime);
                    QDateTime end = fromSdkTime(data.struStopTime);
                    if (start.isValid() && end.isValid() && start < end) {
                        if (session.isMonthSearch) {
                            if (!days.contains(start.date().day()))
                                days.append(start.date().day());
                        } else {
                            QVariantMap segment;
                            segment["startTime"] = start.toMSecsSinceEpoch();
                            segment["endTime"] = end.toMSecsSinceEpoch();
                            segment["timeIsLocal"] = true;
                            segments.append(segment);
                        }
                    }
                } else if (state == 1002) {
                    QThread::msleep(10);
                } else {
                    break;
                }
            }
            NET_DVR_FindClose_V30(findHandle);
        }
        HikvisionManager::instance()->logoutShared(ip);

        QMetaObject::invokeMethod(this, [this, session, segments, days]() {
            const QString ip = session.recorderInfo.value("ip").toString();
            if (session.isMonthSearch) {
                QVariantList dayList;
                for (int day : days)
                    dayList.append(day);
                emit monthAvailabilityFinished(ip, session.channelId, session.year, session.month, dayList, true);
            } else {
                emit searchFinished(ip, session.channelId, session.startTime, segments);
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    worker->start();
}

namespace {

QByteArray curlExchange(const QString &user, const QString &pass, const QString &url, const QByteArray &post, int timeoutSec, const QByteArray &contentType = QByteArray("application/json"))
{
    QProcess process;
    QStringList args;
    args << QStringLiteral("-sS") << QStringLiteral("-k") << QStringLiteral("--anyauth")
         << QStringLiteral("-u") << (user + QLatin1Char(':') + pass)
         << QStringLiteral("--max-time") << QString::number(timeoutSec);
    QTemporaryFile bodyFile;
    if (!post.isEmpty()) {
        if (!bodyFile.open())
            return QByteArray();
        bodyFile.write(post);
        bodyFile.close();
        args << QStringLiteral("-H") << (QByteArray("Content-Type: ") + contentType)
             << QStringLiteral("--data-binary") << (QLatin1Char('@') + bodyFile.fileName());
    }
    args << url;
    process.start(QStringLiteral("curl"), args);
    if (!process.waitForFinished((timeoutSec + 3) * 1000)) {
        process.kill();
        process.waitForFinished(1000);
        return QByteArray();
    }
    return process.readAllStandardOutput();
}

QString saveSmartCrop(const QByteArray &jpeg, double x, double y, double w, double h, const QString &dir, int index)
{
    QImage img;
    if (!img.loadFromData(jpeg) || img.isNull())
        return QString();
    if (w > 0.0 && h > 0.0) {
        double sx = x, sy = y, sw = w, sh = h;
        if (sx > 1.5 || sy > 1.5 || sw > 1.5 || sh > 1.5) {
            sx /= 10000.0;
            sy /= 10000.0;
            sw /= 10000.0;
            sh /= 10000.0;
        }
        const QRect crop(int(sx * img.width()), int(sy * img.height()),
                         qMax(1, int(sw * img.width())), qMax(1, int(sh * img.height())));
        const QRect fitted = crop.intersected(img.rect());
        if (fitted.width() >= 8 && fitted.height() >= 8)
            img = img.copy(fitted);
    }
    const QString path = dir + QStringLiteral("/smart-%1.jpg").arg(index);
    if (!img.save(path, "JPEG", 85))
        return QString();
    return path;
}

QString kindLabel(const QString &kind)
{
    if (kind == QLatin1String("human"))
        return QStringLiteral("Human");
    if (kind == QLatin1String("face"))
        return QStringLiteral("Face");
    if (kind == QLatin1String("event"))
        return QStringLiteral("Event");
    return QStringLiteral("Vehicle");
}

} // namespace

void HikvisionISAPI::searchSmartTargets(const QVariantMap &recorderInfo, const QVariantList &channels,
                                        const QDateTime &start, const QDateTime &end,
                                        bool human, bool vehicle, bool face, int token)
{
    QThread *worker = QThread::create([this, recorderInfo, channels, start, end, human, vehicle, face, token]() {
        QVariantList hits;
        QString note;
        const QString ip = recorderInfo.value(QStringLiteral("ip")).toString();
        int httpPort = recorderInfo.value(QStringLiteral("httpPort")).toInt();
        if (httpPort <= 0)
            httpPort = 80;
        const QString user = recorderInfo.value(QStringLiteral("username")).toString();
        const QString pass = recorderInfo.value(QStringLiteral("password")).toString();
        const QString dirPath = QDir::tempPath() + QStringLiteral("/kvision-smart");
        QDir dir(dirPath);
        if (dir.exists())
            dir.removeRecursively();
        QDir().mkpath(dirPath);

        auto cameraNameFor = [&](int channel) {
            for (const QVariant &item : channels) {
                const QVariantMap cam = item.toMap();
                if (cam.value(QStringLiteral("channelId")).toInt() == channel) {
                    const QString name = cam.value(QStringLiteral("name")).toString();
                    if (!name.isEmpty())
                        return name;
                }
            }
            return QString();
        };

        auto addHit = [&](const QString &image, const QDateTime &when, const QString &kind, int channel, const QString &cameraName) {
            if (!when.isValid() || (image.isEmpty() && kind != QLatin1String("event")))
                return;
            const QString shownName = !cameraName.isEmpty() ? cameraName : cameraNameFor(channel);
            QVariantMap hit;
            hit.insert(QStringLiteral("image"), image);
            hit.insert(QStringLiteral("timeMs"), when.toMSecsSinceEpoch());
            hit.insert(QStringLiteral("kind"), kind);
            hit.insert(QStringLiteral("channelId"), channel);
            hit.insert(QStringLiteral("cameraName"), shownName);
            const QString who = shownName.isEmpty() ? kindLabel(kind) : (shownName + QStringLiteral("  ") + kindLabel(kind));
            hit.insert(QStringLiteral("label"), who + QLatin1Char(' ') + when.toLocalTime().toString(QStringLiteral("h:mm:ss AP")));
            hits.append(hit);
        };

        // A chosen camera is sent as choiceChannel. The whole recorder is one search, and a channel
        // is sent for each camera only when that recorder refuses an all-camera search.
        enum class Ask { None, Answered, Unsupported };
        bool sawSupport = false;
        bool sawUnsupported = false;
        auto searchVehicles = [&](int onlyChannel) -> Ask {
            const QString searchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            int position = 0;
            bool answered = false;
            while (hits.size() < 48) {
                QJsonObject body{
                    {QStringLiteral("searchID"), searchId},
                    {QStringLiteral("searchResultPosition"), position},
                    {QStringLiteral("maxResults"), 24},
                    {QStringLiteral("startTime"), start.toOffsetFromUtc(start.offsetFromUtc()).toString(Qt::ISODate)},
                    {QStringLiteral("endTime"), end.toOffsetFromUtc(end.offsetFromUtc()).toString(Qt::ISODate)},
                    {QStringLiteral("targetType"), QStringLiteral("vehicle")}
                };
                if (onlyChannel > 0) {
                    body.insert(QStringLiteral("choiceChannel"),
                                QJsonArray{QJsonObject{{QStringLiteral("channelID"), QString::number(onlyChannel)}}});
                }
                const QByteArray raw = curlExchange(user, pass,
                    QStringLiteral("http://%1:%2/ISAPI/SDT/Management/IntelligentSearch?format=json").arg(ip).arg(httpPort),
                    QJsonDocument(body).toJson(QJsonDocument::Compact), 25);
                const QJsonObject doc = QJsonDocument::fromJson(raw).object();
                if (doc.isEmpty() || !doc.contains(QStringLiteral("responseStatusStrg"))) {
                    const QString body = QString::fromUtf8(raw);
                    if (body.contains(QLatin1String("notSupport"), Qt::CaseInsensitive)
                            || body.contains(QLatin1String("Not Found"))
                            || body.contains(QLatin1String("Invalid Operation")))
                        return Ask::Unsupported;
                    break;
                }
                answered = true;
                const QString status = doc.value(QStringLiteral("responseStatusStrg")).toString();
                const QJsonArray infos = doc.value(QStringLiteral("targetInfo")).toArray();
                for (const QJsonValue &value : infos) {
                    if (hits.size() >= 48)
                        break;
                    const QJsonObject info = value.toObject();
                    const QString picUrl = info.value(QStringLiteral("picUrl")).toString();
                    if (picUrl.isEmpty())
                        continue;
                    const QByteArray jpeg = curlExchange(user, pass, picUrl, QByteArray(), 15);
                    const QJsonObject rect = info.value(QStringLiteral("targetRect")).toObject();
                    const QString path = saveSmartCrop(jpeg,
                        rect.value(QStringLiteral("x")).toDouble(),
                        rect.value(QStringLiteral("y")).toDouble(),
                        rect.value(QStringLiteral("width")).toDouble(),
                        rect.value(QStringLiteral("height")).toDouble(),
                        dirPath, hits.size());
                    const QDateTime when = QDateTime::fromString(info.value(QStringLiteral("captureTime")).toString(), Qt::ISODate);
                    const int channel = info.value(QStringLiteral("channelID")).toString().toInt();
                    if (onlyChannel > 0 && channel > 0 && channel != onlyChannel)
                        continue;
                    addHit(path, when, QStringLiteral("vehicle"), channel > 0 ? channel : onlyChannel,
                           info.value(QStringLiteral("channelName")).toString());
                }
                const int got = doc.value(QStringLiteral("numOfMatches")).toInt();
                position += qMax(1, got);
                if (status != QLatin1String("MORE") || infos.isEmpty())
                    break;
            }
            return answered ? Ask::Answered : Ask::None;
        };

        auto take = [&](Ask answer) {
            if (answer == Ask::Answered)
                sawSupport = true;
            else if (answer == Ask::Unsupported)
                sawUnsupported = true;
            return answer == Ask::Answered;
        };

        if (vehicle && !ip.isEmpty()) {
            QList<int> wanted;
            for (const QVariant &item : channels) {
                const int channel = item.toMap().value(QStringLiteral("channelId")).toInt();
                if (channel > 0)
                    wanted.append(channel);
            }
            if (wanted.size() == 1) {
                take(searchVehicles(wanted.first()));
            } else if (!take(searchVehicles(-1))) {
                for (int channel : wanted) {
                    if (hits.size() >= 48)
                        break;
                    if (take(searchVehicles(channel)))
                        sawSupport = true;
                }
            }
            if (!sawSupport && !sawUnsupported)
                note = QStringLiteral("The recorder did not answer the vehicle search.");
        }

        if ((human || face) && !ip.isEmpty()) {
            const int port = recorderInfo.value(QStringLiteral("sdkPort")).toInt() > 0
                ? recorderInfo.value(QStringLiteral("sdkPort")).toInt()
                : recorderInfo.value(QStringLiteral("port")).toInt();
            NET_DVR_DEVICEINFO_V40 deviceInfo;
            std::memset(&deviceInfo, 0, sizeof(deviceInfo));
            const LONG userId = HikvisionManager::instance()->loginShared(ip, port > 0 ? port : 8000, user, pass, deviceInfo);
            if (userId < 0) {
                if (note.isEmpty())
                    note = QStringLiteral("Could not sign in to the recorder for people and faces.");
            } else {
                const auto &info = deviceInfo.struDeviceV30;
                const QDateTime startLocal = start.toLocalTime();
                const QDateTime endLocal = end.toLocalTime();
                int kept = 0;
                const int kinds[2] = {human ? 3 : 0, face ? 1 : 0};
                for (int kindCode : kinds) {
                if (kindCode == 0)
                    continue;
                for (const QVariant &item : channels) {
                    if (kept >= 40)
                        break;
                    const int logical = item.toMap().value(QStringLiteral("channelId")).toInt();
                    if (logical <= 0)
                        continue;
                    int channel = logical;
                    if (info.byChanNum == 0 && info.byIPChanNum > 0)
                        channel = logical + info.byStartDChan - 1;
                    else if (info.byChanNum > 0 && logical > info.byChanNum)
                        channel = info.byStartDChan + (logical - info.byChanNum) - 1;
                    else if (info.byChanNum > 0)
                        channel = logical + info.byStartChan - 1;

                    NET_DVR_SMART_SEARCH_PIC_PARA para;
                    std::memset(&para, 0, sizeof(para));
                    para.dwChanNo = static_cast<DWORD>(channel);
                    // Behaviour search filtered to a person. This recorder stores people here;
                    // the vehicle search does not return them, and the face-attribute search is empty.
                    para.wSearchType = static_cast<WORD>(kindCode);
                    if (kindCode == 3)
                        para.uSmartSearchCond.struBehaviourCond.byHumanMisinfo = 1;
                    para.struStartTime.wYear = static_cast<WORD>(startLocal.date().year());
                    para.struStartTime.byMonth = static_cast<BYTE>(startLocal.date().month());
                    para.struStartTime.byDay = static_cast<BYTE>(startLocal.date().day());
                    para.struStartTime.byHour = static_cast<BYTE>(startLocal.time().hour());
                    para.struStartTime.byMinute = static_cast<BYTE>(startLocal.time().minute());
                    para.struStartTime.bySecond = static_cast<BYTE>(startLocal.time().second());
                    para.struEndTime.wYear = static_cast<WORD>(endLocal.date().year());
                    para.struEndTime.byMonth = static_cast<BYTE>(endLocal.date().month());
                    para.struEndTime.byDay = static_cast<BYTE>(endLocal.date().day());
                    para.struEndTime.byHour = static_cast<BYTE>(endLocal.time().hour());
                    para.struEndTime.byMinute = static_cast<BYTE>(endLocal.time().minute());
                    para.struEndTime.bySecond = static_cast<BYTE>(endLocal.time().second());

                    const LONG find = NET_DVR_SmartSearchPicture(userId, &para);
                    if (find < 0) {
                        if (NET_DVR_GetLastError() == NET_DVR_NOSUPPORT)
                            sawUnsupported = true;
                        continue;
                    }
                    sawSupport = true;
                    int guard = 0;
                    while (guard++ < 40 && kept < 40) {
                        NET_DVR_SMART_SEARCH_PIC_RET ret;
                        std::memset(&ret, 0, sizeof(ret));
                        const LONG state = NET_DVR_FindNextSmartPicture(find, &ret);
                        if (state == NET_DVR_ISFINDING)
                            continue;
                        if (state != NET_DVR_FILE_SUCCESS)
                            break;
                        const bool person = kindCode == 3;
                        const QString kind = person ? QStringLiteral("human") : QStringLiteral("face");
                        if (ret.dwFileSize == 0 || ret.dwFileSize > 8 * 1024 * 1024)
                            continue;
                        QByteArray jpeg(int(ret.dwFileSize) + 64, 0);
                        DWORD got = 0;
                        if (!NET_DVR_GetPicture_V30(userId, ret.sFileName, jpeg.data(), DWORD(jpeg.size()), &got) || got < 32)
                            continue;
                        jpeg.truncate(int(got));
                        double cropX = 0, cropY = 0, cropW = 0, cropH = 0;
                        const NET_VCA_RECT rect = person ? ret.uPicFeature.struBehavior.struVcaRect
                                                        : ret.uPicFeature.struFaceSubInfo.struVcaRect;
                        cropX = rect.fX;
                        cropY = rect.fY;
                        cropW = rect.fWidth;
                        cropH = rect.fHeight;
                        const QString path = saveSmartCrop(jpeg, cropX, cropY, cropW, cropH, dirPath, 1000 + hits.size());
                        QDateTime when(QDate(ret.struTime.wYear, ret.struTime.byMonth, ret.struTime.byDay),
                                       QTime(ret.struTime.byHour, ret.struTime.byMinute, ret.struTime.bySecond),
                                       Qt::LocalTime);
                        addHit(path, when, kind, logical, QString());
                        ++kept;
                    }
                    NET_DVR_CloseSmartSearchPicture(find);
                }
                }
            }
        }

        if (hits.isEmpty() && !sawSupport && sawUnsupported) {
            const QString windowStart = start.toOffsetFromUtc(start.offsetFromUtc()).toString(Qt::ISODate);
            const QString windowEnd = end.toOffsetFromUtc(end.offsetFromUtc()).toString(Qt::ISODate);
            for (const QVariant &item : channels) {
                if (hits.size() >= 80)
                    break;
                const QVariantMap cam = item.toMap();
                const int logical = cam.value(QStringLiteral("channelId")).toInt();
                if (logical <= 0)
                    continue;
                const QString searchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                int position = 0;
                for (int page = 0; page < 8 && hits.size() < 80; ++page) {
                    const QByteArray xml = QString(
                        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                        "<CMSearchDescription xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
                        "<searchID>%1</searchID>"
                        "<trackList><trackID>%2</trackID></trackList>"
                        "<timeSpanList><timeSpan><startTime>%3</startTime><endTime>%4</endTime></timeSpan></timeSpanList>"
                        "<maxResults>40</maxResults>"
                        "<searchResultPostion>%5</searchResultPostion>"
                        "<metadataList><metadataDescriptor>//recordType.meta.hikvision.com/AllEvent</metadataDescriptor></metadataList>"
                        "</CMSearchDescription>")
                        .arg(searchId)
                        .arg(logical * 100 + 1)
                        .arg(windowStart, windowEnd)
                        .arg(position)
                        .toUtf8();
                    const QByteArray raw = curlExchange(user, pass,
                        QStringLiteral("http://%1:%2/ISAPI/ContentMgmt/search").arg(ip).arg(httpPort),
                        xml, 25, QByteArray("application/xml"));
                    QXmlStreamReader reader(raw);
                    QString status;
                    int matched = 0;
                    int added = 0;
                    while (!reader.atEnd()) {
                        reader.readNext();
                        if (!reader.isStartElement())
                            continue;
                        if (reader.name() == QLatin1String("responseStatusStrg")) {
                            status = reader.readElementText();
                        } else if (reader.name() == QLatin1String("numOfMatches")) {
                            matched = reader.readElementText().toInt();
                        } else if (reader.name() == QLatin1String("searchMatchItem")) {
                            QString startText;
                            while (!reader.atEnd() && !(reader.isEndElement() && reader.name() == QLatin1String("searchMatchItem"))) {
                                reader.readNext();
                                if (reader.isStartElement() && reader.name() == QLatin1String("startTime") && startText.isEmpty())
                                    startText = reader.readElementText();
                            }
                            const QDateTime when = QDateTime::fromString(startText, Qt::ISODate);
                            if (!when.isValid() || hits.size() >= 80)
                                continue;
                            addHit(QString(), when, QStringLiteral("event"), logical, cam.value(QStringLiteral("name")).toString());
                            ++added;
                        }
                    }
                    if (added == 0)
                        break;
                    position += qMax(1, matched > 0 ? matched : added);
                    if (status != QLatin1String("MORE"))
                        break;
                }
            }
            std::sort(hits.begin(), hits.end(), [](const QVariant &a, const QVariant &b) {
                return a.toMap().value(QStringLiteral("timeMs")).toLongLong()
                     > b.toMap().value(QStringLiteral("timeMs")).toLongLong();
            });
            if (hits.isEmpty())
                note = QStringLiteral("Smart-search pictures are not stored on this camera, and it has no event recordings for this day. KVision's Detection events may have the properly tagged event for the video timestamp.");
            else
                note = QStringLiteral("Smart-search pictures are not stored on this camera. %1 event recordings. Click one to play from that time. KVision's Detection events may have the properly tagged event for the video timestamp.").arg(hits.size());
        } else if (note.isEmpty()) {
            if (hits.isEmpty())
                note = QStringLiteral("No pictures for this day.");
            else
                note = QStringLiteral("%1 pictures. Click one to play from that time.").arg(hits.size());
        }

        QMetaObject::invokeMethod(this, [this, token, hits, note]() {
            emit smartSearchFinished(token, hits, note);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    worker->start();
}

void HikvisionISAPI::cancelAllSearches()
{
    qDebug() << "[ISAPI] cancelAllSearches called. Cancelling" << m_sessions.size() << "active search sessions.";
    m_sessions.clear();
    
    // Copy the replies to a local list first to avoid iterator invalidation when they are aborted/deleted
    QList<QNetworkReply*> repliesToAbort = m_activeReplies.values();
    m_activeReplies.clear();

    for (auto* reply : repliesToAbort) {
        if (reply) {
            // Disconnect all signals to prevent onReplyFinished or authentication requests
            // from being triggered synchronously during reply->abort()
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
}

