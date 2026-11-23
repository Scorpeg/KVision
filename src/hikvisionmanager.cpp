#include "hikvisionmanager.h"
#include "context.h"
#include "loginguard.h"
#include <QDebug>
#include <QDateTime>
#include <QTemporaryFile>
#include <cstring>
#include <QProcess>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QThread>
#include <QSet>
#include <algorithm>

HikvisionManager* HikvisionManager::m_instance = nullptr;

HikvisionManager::HikvisionManager(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
{
    m_instance = this;

    std::thread initThread([this]() {
        if (NET_DVR_Init()) {
            m_initialized = true;
            if (!Context::enableLogs()) {
                NET_DVR_SetLogPrint(FALSE);
            }
            qDebug() << "[Hikvision] HCNetSDK Initialized successfully in background.";
        } else {
            qWarning() << "[Hikvision] Failed to initialize HCNetSDK.";
        }
        {
            std::lock_guard<std::mutex> lock(m_initMutex);
            m_initCompleted = true;
        }
        m_initCond.notify_all();
    });
    initThread.detach();

    // Start background worker thread for PTZ commands
    m_ptzWorker = std::thread(&HikvisionManager::ptzWorkerLoop, this);
}

HikvisionManager::~HikvisionManager()
{
    // Wait for all active async discovery threads to finish
    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        for (QThread *thread : m_discoveryThreads) {
            thread->disconnect(this); // Disconnect all signals connected to this
            thread->wait();
        }
        m_discoveryThreads.clear();
    }

    // Stop the worker thread cleanly first
    {
        std::lock_guard<std::mutex> lock(m_ptzMutex);
        m_ptzWorkerStop = true;
    }
    m_ptzCond.notify_all();
    if (m_ptzWorker.joinable()) {
        m_ptzWorker.join();
    }

    ensureInitialized();

    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);

    // Logout all active sessions
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        NET_DVR_Logout(it.value().lUserID);
    }
    m_sessions.clear();

    for (auto it = m_sharedSessions.begin(); it != m_sharedSessions.end(); ++it) {
        NET_DVR_Logout(it.value().lUserID);
    }
    m_sharedSessions.clear();

    if (m_initialized) {
        NET_DVR_Cleanup();
        qDebug() << "[Hikvision] HCNetSDK Cleaned up.";
    }
    m_instance = nullptr;
}

HikvisionManager* HikvisionManager::instance()
{
    return m_instance;
}

struct CurlAnswer {
    QByteArray body;
    int status = 0;
};

QString curlQuoted(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return s;
}

// The password goes to curl on stdin, so it does not appear in the process list.
// status 0 means curl did not get an HTTP answer.
CurlAnswer curlExchange(const QString &url, const QString &username, const QString &password,
                        const QString &method, const QString &bodyFile)
{
    CurlAnswer answer;
    QString cfg = QStringLiteral("user = \"%1\"\n").arg(curlQuoted(username + QLatin1Char(':') + password));
    if (!bodyFile.isEmpty()) {
        cfg += QStringLiteral("data-binary = \"@%1\"\n").arg(curlQuoted(bodyFile));
        cfg += QStringLiteral("header = \"Content-Type: application/xml\"\n");
    }
    QStringList args = { QStringLiteral("-s"), QStringLiteral("-k"), QStringLiteral("--anyauth"),
                         QStringLiteral("-K"), QStringLiteral("-"), QStringLiteral("--max-time"), QStringLiteral("4"),
                         QStringLiteral("-w"), QStringLiteral("\n__HTTP__%{http_code}") };
    if (!method.isEmpty() && method != QLatin1String("GET")) {
        args << QStringLiteral("-X") << method;
    }
    args << url;
    QProcess process;
    process.start(QStringLiteral("curl"), args);
    if (!process.waitForStarted(2000))
        return answer;
    process.write(cfg.toUtf8());
    process.closeWriteChannel();
    if (!process.waitForFinished(5000))
        return answer;
    const QByteArray out = process.readAllStandardOutput();
    const int at = out.lastIndexOf("\n__HTTP__");
    if (at < 0)
        return answer;
    answer.body = out.left(at);
    answer.status = out.mid(at + 9).trimmed().toInt();
    return answer;
}

void noteHttpRefusal(const QString &ip, const QString &username, const QString &password, int status)
{
    if (LoginGuard::isHttpAuthRefusal(status))
        LoginGuard::instance().refuse(ip, username, password);
}

static QByteArray fetchUrl(const QString &ip, int port, const QString &username, const QString &password, const QString &path) {
    if (LoginGuard::instance().isRefused(ip, username, password))
        return QByteArray();
    const QString httpUrl = QStringLiteral("http://%1:%2%3").arg(ip).arg(port).arg(path);
    qDebug() << "[Hikvision ISAPI] Querying:" << httpUrl;
    const CurlAnswer http = curlExchange(httpUrl, username, password, QStringLiteral("GET"), QString());
    // A refused login is final. Trying HTTPS as well would send the password a second time.
    if (http.status == 401 || http.status == 403) {
        noteHttpRefusal(ip, username, password, http.status);
        return QByteArray();
    }
    if (!http.body.isEmpty())
        return http.body;

    const QString httpsUrl = QStringLiteral("https://%1:%2%3").arg(ip).arg(port).arg(path);
    const CurlAnswer https = curlExchange(httpsUrl, username, password, QStringLiteral("GET"), QString());
    noteHttpRefusal(ip, username, password, https.status);
    return https.body;
}

// PUT an XML body to an ISAPI endpoint on the camera. Same auth/timeout/fallback
// semantics as fetchUrl(). Returns the response body (usually a ResponseStatus XML).
static QByteArray putUrl(const QString &ip, int port, const QString &username, const QString &password,
                         const QString &path, const QByteArray &body)
{
    const QString tmpFile = QStringLiteral("/tmp/kvision-put-XXXXXX.xml");
    QTemporaryFile tf(tmpFile);
    if (!tf.open()) {
        qWarning() << "[Hikvision ISAPI] Cannot create temp file for PUT body";
        return QByteArray();
    }
    tf.write(body);
    tf.close();
    const QString tfPath = tf.fileName();

    const QString httpUrl = QStringLiteral("http://%1:%2%3").arg(ip).arg(port).arg(path);
    qDebug() << "[Hikvision ISAPI] PUT:" << httpUrl;
    if (LoginGuard::instance().isRefused(ip, username, password)) {
        tf.remove();
        return QByteArray();
    }
    const CurlAnswer http = curlExchange(httpUrl, username, password, QStringLiteral("PUT"), tfPath);
    if (http.status == 401 || http.status == 403) {
        noteHttpRefusal(ip, username, password, http.status);
        tf.remove();
        return QByteArray();
    }
    QByteArray res = http.body;
    if (res.isEmpty()) {
        const QString httpsUrl = QStringLiteral("https://%1:%2%3").arg(ip).arg(port).arg(path);
        const CurlAnswer https = curlExchange(httpsUrl, username, password, QStringLiteral("PUT"), tfPath);
        noteHttpRefusal(ip, username, password, https.status);
        res = https.body;
    }

    tf.remove();
    return res;
}

// Unstick a camera whose LAN event-push dispatcher has wedged. For every event trigger of the
// given channel (motion, line crossing, intrusion, tamper, ...) that has "Notify Surveillance
// Center" (the LAN push) enabled, toggles that notification off and back
// on, restoring the original config. Works for standalone cameras and NVR channels.
// Returns "ok..." or an error/warning string.
QString HikvisionManager::unstickEventDetection(const QString &ip, int httpPort, const QString &username,
                                                const QString &password, int channelId, const QString &label)
{
    const QString cleanIp = ip.trimmed();
    if (cleanIp.isEmpty())
        return QStringLiteral("error: empty ip");

    const int port = httpPort > 0 ? httpPort : 80;
    const int channel = channelId > 0 ? channelId : 1;

    emit unstickProgress(label, QStringLiteral("Reading event triggers..."));

    // 1. Find the triggers that belong to this channel
    QStringList triggerIds;
    const QByteArray listXml = fetchUrl(cleanIp, port, username, password, QStringLiteral("/ISAPI/Event/triggers"));
    if (!listXml.isEmpty()) {
        const QString text = QString::fromUtf8(listXml);
        QRegularExpression blockRx(QStringLiteral("<EventTrigger(?:\\s[^>]*)?>([\\s\\S]*?)</EventTrigger>"));
        QRegularExpression idRx(QStringLiteral("<id>([^<]+)</id>"));
        QRegularExpression chanRx(QStringLiteral("<(?:dynVideoInputChannelID|videoInputChannelID)>(\\d+)</"));
        auto it = blockRx.globalMatch(text);
        while (it.hasNext()) {
            const QString block = it.next().captured(1);
            const QString id = idRx.match(block).captured(1).trimmed();
            if (id.isEmpty())
                continue;
            bool onChannel = false;
            auto cit = chanRx.globalMatch(block);
            while (cit.hasNext()) {
                if (cit.next().captured(1).toInt() == channel)
                    onChannel = true;
            }
            if (onChannel)
                triggerIds << id;
        }
    }
    if (triggerIds.isEmpty()) // older firmware without a trigger list: fall back to motion detection
        triggerIds << QStringLiteral("VMD-%1").arg(channel);

    QRegularExpression centerBlock(
        QStringLiteral("<EventTriggerNotification>\\s*<id>center</id>.*?</EventTriggerNotification>"),
        QRegularExpression::DotMatchesEverythingOption);
    emit unstickProgress(label, QStringLiteral("Checking %1 trigger(s)...").arg(triggerIds.size()));
    auto accepted = [](const QByteArray &r) {
        return r.contains("<statusCode>1</statusCode>") || r.contains("<statusString>OK</statusString>");
    };

    int fixed = 0;
    QStringList problems;
    for (const QString &id : triggerIds) {
        const QString path = QStringLiteral("/ISAPI/Event/triggers/") + id;
        emit unstickProgress(label, QStringLiteral("Checking %1...").arg(id));

        // 2. Read the trigger's current config; skip triggers that don't have Notify Surveillance Center
        const QByteArray original = fetchUrl(cleanIp, port, username, password, path);
        if (original.isEmpty())
            continue;
        QString centerOffXml = QString::fromUtf8(original);
        if (!centerBlock.match(centerOffXml).hasMatch())
            continue;
        centerOffXml.remove(centerBlock);

        // 3. PUT Notify Surveillance Center OFF
        emit unstickProgress(label, QStringLiteral("Resetting %1...").arg(id));
        const QByteArray putOff = putUrl(cleanIp, port, username, password, path, centerOffXml.toUtf8());
        if (putOff.isEmpty() || !accepted(putOff)) {
            problems << QStringLiteral("%1: Notify Surveillance Center OFF %2").arg(id, putOff.isEmpty() ? QStringLiteral("failed (no response)")
                                                                                   : QStringLiteral("rejected"));
            continue;
        }

        // 4. PUT original back (Notify Surveillance Center ON - restores the user's config)
        const QByteArray putOn = putUrl(cleanIp, port, username, password, path, original);
        if (putOn.isEmpty() || !accepted(putOn)) {
            problems << QStringLiteral("%1: Notify Surveillance Center left OFF, restore failed - run again").arg(id);
            continue;
        }
        ++fixed;
    }

    if (!problems.isEmpty())
        return QStringLiteral("warning: ") + problems.join(QStringLiteral("; "));
    if (fixed == 0)
        return QStringLiteral("error: no event triggers with Notify Surveillance Center found for channel %1 (check credentials/connectivity)").arg(channel);
    return QStringLiteral("ok (%1 trigger(s) reset)").arg(fixed);
}

void HikvisionManager::unstickEventDetectionAsync(const QString &ip, int httpPort, const QString &username,
                                                  const QString &password, int channelId, const QString &label)
{
    QThread *thread = QThread::create([this, ip, httpPort, username, password, channelId, label]() {
        const QString result = unstickEventDetection(ip, httpPort, username, password, channelId, label);
        emit unstickFinished(label, result);
    });

    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.append(thread);
    }
    connect(thread, &QThread::finished, this, [this, thread]() {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.removeOne(thread);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

QString HikvisionManager::fetchStreamingChannels(const QString &ip, int httpPort, const QString &username, const QString &password)
{
    const QString cleanIp = cleanHost(ip);
    if (cleanIp.isEmpty())
        return QString();
    if (httpPort <= 0)
        httpPort = 80;
    QByteArray xml = fetchUrl(cleanIp, httpPort, username, password, QStringLiteral("/ISAPI/Streaming/channels"));
    if (xml.isEmpty() && httpPort != 80)
        xml = fetchUrl(cleanIp, 80, username, password, QStringLiteral("/ISAPI/Streaming/channels"));
    return QString::fromUtf8(xml);
}

void HikvisionManager::fetchStreamingChannelsAsync(const QString &ip, int httpPort, const QString &username, const QString &password)
{
    const QString host = cleanHost(ip);
    if (host.isEmpty() || username.isEmpty() || password.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString cached;
    bool haveCached = false;
    {
        std::lock_guard<std::mutex> lock(m_streamMutex);
        if (m_streamXml.contains(host)) {
            cached = m_streamXml.value(host);
            haveCached = true;
        } else if (m_streamFetching.contains(host)) {
            return;
        } else if (m_streamFailedAt.contains(host) && now - m_streamFailedAt.value(host) < 15000) {
            return;
        } else {
            m_streamFetching.insert(host);
        }
    }
    if (haveCached) {
        QMetaObject::invokeMethod(this, [this, host, cached]() {
            emit streamingChannelsReady(host, cached);
        }, Qt::QueuedConnection);
        return;
    }
    const int port = httpPort > 0 ? httpPort : 80;
    startWorker([this, host, port, username, password]() {
        const QString xml = fetchStreamingChannels(host, port, username, password);
        {
            std::lock_guard<std::mutex> lock(m_streamMutex);
            m_streamFetching.remove(host);
            if (xml.isEmpty())
                m_streamFailedAt.insert(host, QDateTime::currentMSecsSinceEpoch());
            else {
                m_streamXml.insert(host, xml);
                m_streamFailedAt.remove(host);
            }
        }
        emit streamingChannelsReady(host, xml);
    });
}

QVariantMap HikvisionManager::proxyCameraCached(const QString &nvrIp, int httpPort, const QString &username, const QString &password, int channelId)
{
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    if (httpPort <= 0)
        httpPort = 80;
    if (m_proxyCache.contains(cleanIp))
        return m_proxyCache.value(cleanIp).value(channelId);
    // The window thread must not wait on curl. The list arrives later through proxyChannelsReady().
    prefetchProxyChannels(cleanIp, httpPort, username, password);
    return QVariantMap();
}

bool HikvisionManager::proxyListKnown(const QString &nvrIp) const
{
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    return m_proxyCache.contains(cleanIp);
}

bool HikvisionManager::proxyFetchPending(const QString &nvrIp) const
{
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    return m_proxyFetching.contains(cleanIp);
}

QVariantMap HikvisionManager::proxyCameraIfCached(const QString &nvrIp, int channelId) const
{
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    return m_proxyCache.value(cleanIp).value(channelId);
}

bool HikvisionManager::isBehindKnownRecorder(const QString &cameraIp) const
{
    const QString ip = cameraIp.trimmed();
    if (ip.isEmpty())
        return false;
    for (auto rec = m_proxyCache.constBegin(); rec != m_proxyCache.constEnd(); ++rec) {
        for (auto ch = rec.value().constBegin(); ch != rec.value().constEnd(); ++ch) {
            if (ch.value().value(QStringLiteral("ip")).toString() == ip)
                return true;
        }
    }
    return false;
}

void HikvisionManager::prefetchProxyChannels(const QString &nvrIp, int httpPort, const QString &username, const QString &password)
{
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    if (cleanIp.isEmpty() || username.isEmpty() || password.isEmpty() || m_proxyCache.contains(cleanIp) || m_proxyFetching.contains(cleanIp))
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_proxyAttemptAt.contains(cleanIp) && now - m_proxyAttemptAt.value(cleanIp) < 15000)
        return;
    m_proxyAttemptAt.insert(cleanIp, now);
    m_proxyFetching.insert(cleanIp);
    if (httpPort <= 0)
        httpPort = 80;

    QThread *thread = QThread::create([this, cleanIp, httpPort, username, password]() {
        const QHash<int, QVariantMap> channels = fetchProxyChannels(cleanIp, httpPort, username, password);
        QMetaObject::invokeMethod(this, [this, cleanIp, channels]() {
            m_proxyFetching.remove(cleanIp);
            if (!channels.isEmpty()) {
                m_proxyCache.insert(cleanIp, channels);
                m_proxyFailedAt.remove(cleanIp);
            }
            emit proxyChannelsReady(cleanIp, !channels.isEmpty());
        }, Qt::QueuedConnection);
    });
    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.append(thread);
    }
    connect(thread, &QThread::finished, this, [this, thread]() {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.removeOne(thread);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

QHash<int, QVariantMap> HikvisionManager::fetchProxyChannels(const QString &nvrIp, int httpPort, const QString &username, const QString &password)
{
    QHash<int, QVariantMap> channels;
    QString cleanIp = nvrIp.trimmed();
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    if (httpPort <= 0)
        httpPort = 80;
    const QByteArray xml = fetchUrl(cleanIp, httpPort, username, password, QStringLiteral("/ISAPI/ContentMgmt/InputProxy/channels"));
    if (xml.isEmpty())
        return channels;
    const QString text = QString::fromUtf8(xml);
    QRegularExpression blockRx(QStringLiteral("<InputProxyChannel[^>]*>([\\s\\S]*?)</InputProxyChannel>"));
    QRegularExpression idRx(QStringLiteral("<id>(\\d+)</id>"));
    QRegularExpression ipRx(QStringLiteral("<ipAddress>([^<]+)</ipAddress>"));
    QRegularExpression srcRx(QStringLiteral("<srcInputPort>(\\d+)</srcInputPort>"));
    QRegularExpression mgmtPortRx(QStringLiteral("<managePortNo>(\\d+)</managePortNo>"));
    auto it = blockRx.globalMatch(text);
    while (it.hasNext()) {
        const QString block = it.next().captured(1);
        const int id = idRx.match(block).captured(1).toInt();
        if (id <= 0)
            continue;
        int sourceChannel = srcRx.match(block).captured(1).toInt();
        if (sourceChannel <= 0)
            sourceChannel = 1;
        const int mgmtPort = mgmtPortRx.match(block).captured(1).toInt();
        QVariantMap cam;
        cam.insert(QStringLiteral("ip"), ipRx.match(block).captured(1).trimmed());
        cam.insert(QStringLiteral("channel"), sourceChannel);
        cam.insert(QStringLiteral("sdkPort"), mgmtPort > 0 ? mgmtPort : 8000);
        channels.insert(id, cam);
    }
    return channels;
}

QVariantMap HikvisionManager::proxyCamera(const QString &nvrIp, int httpPort, const QString &username, const QString &password, int channelId)
{
    return proxyCameraCached(nvrIp, httpPort, username, password, channelId);
}

QVariantList HikvisionManager::discoverCameras(const QString &ip, int port, int httpPort, const QString &username, const QString &password)
{
    QVariantList cameraList;
    QSet<int> seenChannels;

    QString cleanIp = ip.trimmed();
    if (cleanIp.startsWith("http://", Qt::CaseInsensitive)) cleanIp = cleanIp.mid(7);
    if (cleanIp.startsWith("https://", Qt::CaseInsensitive)) cleanIp = cleanIp.mid(8);
    if (cleanIp.contains(":")) cleanIp = cleanIp.section(':', 0, 0);

    // Try ISAPI HTTP discovery first to support real NVRs/DVRs
    if (httpPort <= 0) {
        httpPort = 80;
    }
    if (port <= 0) {
        port = 8000;
    }

    qDebug() << "[Hikvision] Attempting real NVR HTTP ISAPI discovery on" << cleanIp << "port" << httpPort;

    // 1. Try IP Proxy Channels (/ISAPI/ContentMgmt/InputProxy/channels) - Standard for Hikvision IP NVRs
    QByteArray xmlIP = fetchUrl(cleanIp, httpPort, username, password, "/ISAPI/ContentMgmt/InputProxy/channels");
    if (xmlIP.isEmpty() && httpPort != 80) {
        xmlIP = fetchUrl(cleanIp, 80, username, password, "/ISAPI/ContentMgmt/InputProxy/channels");
        if (!xmlIP.isEmpty()) {
            httpPort = 80;
        }
    }
    if (!xmlIP.isEmpty()) {
        qDebug() << "[Hikvision ISAPI] Found InputProxy channels XML. Parsing...";
        QRegularExpression rxIP("<InputProxyChannel[^>]*>([\\s\\S]*?)</InputProxyChannel>");
        QRegularExpressionMatchIterator iIP = rxIP.globalMatch(QString::fromUtf8(xmlIP));
        while (iIP.hasNext()) {
            QRegularExpressionMatch match = iIP.next();
            QString channelXml = match.captured(1);
            
            QRegularExpression rxId("<id>(\\d+)</id>");
            QRegularExpression rxName("<name>(.*?)</name>");
            QRegularExpression rxChanName("<channelName>(.*?)</channelName>");
            
            int id = rxId.match(channelXml).captured(1).toInt();
            QString name = rxName.match(channelXml).captured(1);
            if (name.isEmpty()) {
                name = rxChanName.match(channelXml).captured(1);
            }
            
            if (id > 0 && !seenChannels.contains(id)) {
                seenChannels.insert(id);
                QVariantMap cam;
                cam.insert("channelId", id);
                cam.insert("name", name.isEmpty() ? QString("Camera %1").arg(id) : name);
                cam.insert("recorderIp", cleanIp);
                cameraList.append(cam);
                qDebug() << "[Hikvision ISAPI] Discovered proxy IP channel:" << id << "name:" << name;
            }
        }
    }

    // 2. Try Analog/TVI Video Inputs (/ISAPI/System/Video/inputs/channels) - For DVRs / Hybrid NVRs
    QByteArray xmlAnalog = fetchUrl(cleanIp, httpPort, username, password, "/ISAPI/System/Video/inputs/channels");
    if (xmlAnalog.isEmpty() && httpPort != 80 && cameraList.isEmpty()) {
        xmlAnalog = fetchUrl(cleanIp, 80, username, password, "/ISAPI/System/Video/inputs/channels");
        if (!xmlAnalog.isEmpty()) {
            httpPort = 80;
        }
    }
    if (!xmlAnalog.isEmpty()) {
        qDebug() << "[Hikvision ISAPI] Found Video inputs channels XML. Parsing...";
        QRegularExpression rxChan("<VideoInputChannel[^>]*>([\\s\\S]*?)</VideoInputChannel>");
        QRegularExpressionMatchIterator iChan = rxChan.globalMatch(QString::fromUtf8(xmlAnalog));
        while (iChan.hasNext()) {
            QRegularExpressionMatch match = iChan.next();
            QString channelXml = match.captured(1);
            
            QRegularExpression rxId("<id>(\\d+)</id>");
            QRegularExpression rxName("<name>(.*?)</name>");
            QRegularExpression rxChanName("<channelName>(.*?)</channelName>");
            QRegularExpression rxEnabled("<videoInputEnabled>(.*?)</videoInputEnabled>");
            
            int id = rxId.match(channelXml).captured(1).toInt();
            QString name = rxName.match(channelXml).captured(1);
            if (name.isEmpty()) {
                name = rxChanName.match(channelXml).captured(1);
            }
            QString enabled = rxEnabled.match(channelXml).captured(1);
            
            if (id > 0 && enabled != "false" && !seenChannels.contains(id)) {
                seenChannels.insert(id);
                QVariantMap cam;
                cam.insert("channelId", id);
                cam.insert("name", name.isEmpty() ? QString("Camera %1").arg(id) : name);
                cam.insert("recorderIp", cleanIp);
                cameraList.append(cam);
                qDebug() << "[Hikvision ISAPI] Discovered analog channel:" << id << "name:" << name;
            }
        }
    }

    // 3. Try Streaming Channels (/ISAPI/Streaming/channels) - Universal fallback on all Hikvision devices
    if (cameraList.isEmpty()) {
        QByteArray xmlStreaming = fetchUrl(cleanIp, httpPort, username, password, "/ISAPI/Streaming/channels");
        if (xmlStreaming.isEmpty() && httpPort != 80) {
            xmlStreaming = fetchUrl(cleanIp, 80, username, password, "/ISAPI/Streaming/channels");
        }
        if (!xmlStreaming.isEmpty()) {
            qDebug() << "[Hikvision ISAPI] Found Streaming channels XML. Parsing...";
            QRegularExpression rxStream("<StreamingChannel[^>]*>([\\s\\S]*?)</StreamingChannel>");
            QRegularExpressionMatchIterator iStream = rxStream.globalMatch(QString::fromUtf8(xmlStreaming));
            while (iStream.hasNext()) {
                QRegularExpressionMatch match = iStream.next();
                QString channelXml = match.captured(1);
                
                QRegularExpression rxId("<id>(\\d+)</id>");
                QRegularExpression rxName("<channelName>(.*?)</channelName>");
                QRegularExpression rxEnabled("<enabled>(.*?)</enabled>");
                
                int rawId = rxId.match(channelXml).captured(1).toInt();
                QString name = rxName.match(channelXml).captured(1);
                QString enabled = rxEnabled.match(channelXml).captured(1);
                
                if (rawId > 0 && enabled != "false") {
                    int baseChanId = (rawId >= 100) ? (rawId / 100) : rawId;
                    if (!seenChannels.contains(baseChanId)) {
                        seenChannels.insert(baseChanId);
                        QVariantMap cam;
                        cam.insert("channelId", baseChanId);
                        cam.insert("name", name.isEmpty() ? QString("Camera %1").arg(baseChanId) : name);
                        cam.insert("recorderIp", cleanIp);
                        cameraList.append(cam);
                        qDebug() << "[Hikvision ISAPI] Discovered streaming channel:" << baseChanId << "(raw ID: " << rawId << ") name:" << name;
                    }
                }
            }
        }
    }

    if (!cameraList.isEmpty()) {
        return cameraList;
    }

    qDebug() << "[Hikvision] Real NVR HTTP ISAPI discovery returned no channels. Falling back to SDK/Mock...";

    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    const LONG lUserID = getSession(cleanIp, port, username, password);
    if (lUserID < 0) {
        qWarning() << "[Hikvision] Login failed for IP:" << cleanIp;
        return cameraList;
    }
    NET_DVR_DEVICEINFO_V40 deviceInfo;
    if (!getDeviceInfo(cleanIp, deviceInfo))
        return cameraList;

    // Discover real cameras via NET_DVR_GET_IPPARACFG_V40
    NET_DVR_IPPARACFG_V40 ipParaCfg;
    std::memset(&ipParaCfg, 0, sizeof(NET_DVR_IPPARACFG_V40));
    DWORD bytesReturned = 0;
    
    BOOL getCfgRet = NET_DVR_GetDVRConfig(lUserID, NET_DVR_GET_IPPARACFG_V40, 0, &ipParaCfg, sizeof(NET_DVR_IPPARACFG_V40), &bytesReturned);
    
    if (getCfgRet) {
        DWORD startDChan = ipParaCfg.dwStartDChan;
        DWORD ipChanNum = ipParaCfg.dwDChanNum;
        if (ipChanNum == 0) ipChanNum = 64; // Max support
        
        for (DWORD i = 0; i < ipChanNum; ++i) {
            if (ipParaCfg.struIPDevInfo[i].byEnable == 1) {
                int chanId = static_cast<int>(startDChan + i);
                QVariantMap cam;
                cam.insert("channelId", chanId);
                cam.insert("name", QString("Camera %1").arg(i + 1));
                cam.insert("recorderIp", cleanIp);
                cameraList.append(cam);
                qDebug() << "[Hikvision] Discovered active IP camera on channel:" << chanId;
            }
        }
    }
    
    // Fallback if no active IP cameras found or config query failed
    if (cameraList.isEmpty()) {
        int startChan = deviceInfo.struDeviceV30.byStartChan;
        int numChan = deviceInfo.struDeviceV30.byIPChanNum;
        
        if (numChan == 0) {
            startChan = deviceInfo.struDeviceV30.byStartChan;
            numChan = deviceInfo.struDeviceV30.byChanNum;
        }
 
        if (numChan == 0) {
            numChan = 4;
            startChan = 1;
        }

        for (int i = 0; i < numChan; ++i) {
            int chanId = startChan + i;
            QVariantMap cam;
            cam.insert("channelId", chanId);
            cam.insert("name", QString("Camera %1").arg(i + 1));
            cam.insert("recorderIp", cleanIp);
            cameraList.append(cam);
        }
    }

    return cameraList;
}

void HikvisionManager::discoverCamerasAsync(const QString &ip, int port, int httpPort, const QString &username, const QString &password)
{
    QThread *thread = QThread::create([this, ip, port, httpPort, username, password]() {
        QVariantList cameras = discoverCameras(ip, port, httpPort, username, password);
        bool success = !cameras.isEmpty();
        QString errorMsg = success ? "" : tr("LNG_00099");
        emit discoveryFinished(ip, cameras, success, errorMsg);
    });

    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.append(thread);
    }

    connect(thread, &QThread::finished, this, [this, thread]() {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.removeOne(thread);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

QString HikvisionManager::cleanHost(const QString &ip)
{
    QString cleanIp = ip.trimmed();
    if (cleanIp.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        cleanIp = cleanIp.mid(7);
    if (cleanIp.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        cleanIp = cleanIp.mid(8);
    if (cleanIp.contains(QLatin1Char(':')))
        cleanIp = cleanIp.section(QLatin1Char(':'), 0, 0);
    return cleanIp;
}

void HikvisionManager::startWorker(const std::function<void()> &fn)
{
    QThread *thread = QThread::create(fn);
    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.append(thread);
    }
    connect(thread, &QThread::finished, this, [this, thread]() {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_discoveryThreads.removeOne(thread);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void HikvisionManager::logoutBlocking(const QString &ip)
{
    const QString host = cleanHost(ip);
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    LONG lUserID = -1;
    bool wasLogged = false;
    bool isLoggedNow = false;
    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        wasLogged = isLoggedInternal(host);
        if (m_sessions.contains(host))
            lUserID = m_sessions.take(host).lUserID;
        isLoggedNow = isLoggedInternal(host);
    }
    if (lUserID >= 0) {
        ensureInitialized();
        NET_DVR_Logout(lUserID);
        qDebug() << "[Hikvision] Logged out from IP:" << host << "UserID:" << lUserID;
    }
    if (wasLogged != isLoggedNow)
        emit sessionStatusChanged(host, isLoggedNow);
}

void HikvisionManager::logout(const QString &ip)
{
    // The window calls this. The SDK logout can wait on the device, so it runs in the background.
    // A caller that is already off the window thread (camera discovery) logs out directly.
    if (QThread::currentThread() == thread()) {
        startWorker([this, ip]() { logoutBlocking(ip); });
        return;
    }
    logoutBlocking(ip);
}

bool HikvisionManager::isLogged(const QString &ip)
{
    std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
    return isLoggedInternal(cleanHost(ip));
}

bool HikvisionManager::isLoggedInternal(const QString &ip) const
{
    return m_sessions.contains(ip) || m_sharedSessions.contains(ip);
}

bool HikvisionManager::getDeviceInfo(const QString &ip, NET_DVR_DEVICEINFO_V40 &deviceInfo)
{
    const QString host = cleanHost(ip);
    std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
    if (m_sessions.contains(host)) {
        deviceInfo = m_sessions.value(host).deviceInfo;
        return true;
    }
    if (m_sharedSessions.contains(host)) {
        deviceInfo = m_sharedSessions.value(host).deviceInfo;
        return true;
    }
    return false;
}

void HikvisionManager::withSdkLock(const std::function<void()> &fn)
{
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    fn();
}

LONG HikvisionManager::getSession(const QString &ip, int port, const QString &username, const QString &password)
{
    const QString host = cleanHost(ip);
    if (host.isEmpty() || username.isEmpty() || password.isEmpty())
        return -1;
    if (LoginGuard::instance().isRefused(host, username, password)) {
        static QSet<QString> logged;
        static std::mutex loggedMutex;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(loggedMutex);
            first = !logged.contains(host);
            if (first)
                logged.insert(host);
        }
        if (first)
            qWarning() << "[Hikvision] getSession skipped, login already refused for" << host;
        return -1;
    }

    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);

    bool wasLogged = false;
    bool isLoggedNow = false;
    LONG stale = -1;

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        wasLogged = isLoggedInternal(host);
        if (m_sessions.contains(host)) {
            const SessionInfo session = m_sessions.value(host);
            if (session.port == port && session.username == username && session.password == password)
                return session.lUserID;
            stale = m_sessions.take(host).lUserID;
        } else if (m_sharedSessions.contains(host)) {
            const SharedSession session = m_sharedSessions.value(host);
            if (session.port == port && session.username == username && session.password == password)
                return session.lUserID;
            // The saved password changed. Drop the old login, including one playback is holding,
            // so the next call uses the new password.
            stale = m_sharedSessions.take(host).lUserID;
        }
    }

    ensureInitialized();
    if (stale >= 0) {
        NET_DVR_Logout(stale);
        qDebug() << "[Hikvision] getSession dropped a login for" << host << "because the password changed";
    }
    if (!m_initialized) {
        qWarning() << "[Hikvision] Cannot get session: SDK not initialized.";
        return -1;
    }

    NET_DVR_USER_LOGIN_INFO loginInfo;
    std::memset(&loginInfo, 0, sizeof(NET_DVR_USER_LOGIN_INFO));
    std::strncpy(loginInfo.sDeviceAddress, host.toUtf8().constData(), sizeof(loginInfo.sDeviceAddress) - 1);
    loginInfo.wPort = static_cast<WORD>(port);
    std::strncpy(loginInfo.sUserName, username.toUtf8().constData(), sizeof(loginInfo.sUserName) - 1);
    std::strncpy(loginInfo.sPassword, password.toUtf8().constData(), sizeof(loginInfo.sPassword) - 1);
    loginInfo.bUseAsynLogin = FALSE;
    loginInfo.byLoginMode = 0;

    NET_DVR_DEVICEINFO_V40 deviceInfo;
    std::memset(&deviceInfo, 0, sizeof(NET_DVR_DEVICEINFO_V40));

    const LONG lUserID = NET_DVR_Login_V40(&loginInfo, &deviceInfo);
    if (lUserID < 0) {
        const DWORD err = NET_DVR_GetLastError();
        if (LoginGuard::isSdkAuthFailure(err))
            LoginGuard::instance().refuse(host, username, password);
        qWarning() << "[Hikvision] getSession login FAILED for IP:" << host << "Error:" << err;
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        SessionInfo session;
        session.lUserID = lUserID;
        session.deviceInfo = deviceInfo;
        session.port = port;
        session.username = username;
        session.password = password;
        m_sessions.insert(host, session);
        isLoggedNow = isLoggedInternal(host);
    }

    qDebug() << "[Hikvision] getSession login SUCCESS for IP:" << host << "UserID:" << lUserID;
    if (wasLogged != isLoggedNow)
        emit sessionStatusChanged(host, isLoggedNow);
    return lUserID;
}

qint64 HikvisionManager::cachedDeviceTimeMs(const QString &ip)
{
    const QString host = cleanHost(ip);
    std::lock_guard<std::mutex> lock(m_clockMutex);
    const auto it = m_clock.constFind(host);
    if (it == m_clock.cend() || it->deviceMs <= 0)
        return -1;
    return it->deviceMs + (QDateTime::currentMSecsSinceEpoch() - it->localMs);
}

void HikvisionManager::refreshDeviceTime(const QString &ip, int port, const QString &username, const QString &password, bool forSnapshot)
{
    const QString host = cleanHost(ip);
    if (host.isEmpty() || username.isEmpty() || password.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 freshFor = forSnapshot ? 30 * 1000 : 3 * 60 * 1000;
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        if (m_clockFetching.contains(host))
            return;
        const auto it = m_clock.constFind(host);
        if (it != m_clock.cend() && now - it->localMs < freshFor)
            return;
        m_clockFetching.insert(host);
    }
    const int sdkPort = port > 0 ? port : 8000;
    startWorker([this, host, sdkPort, username, password]() {
        const qint64 ms = deviceTimeMs(host, sdkPort, username, password);
        {
            std::lock_guard<std::mutex> lock(m_clockMutex);
            m_clockFetching.remove(host);
            if (ms > 0)
                m_clock.insert(host, ClockSample{ms, QDateTime::currentMSecsSinceEpoch()});
        }
        if (ms > 0)
            emit deviceTimeReady(host, ms);
    });
}

qint64 HikvisionManager::deviceTimeMs(const QString &ip, int port, const QString &username, const QString &password)
{
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    LONG lUserID = getSession(ip, port, username, password);
    if (lUserID < 0) {
        qWarning() << "[Hikvision] deviceTimeMs failed to get session for IP:" << ip;
        return -1;
    }
    NET_DVR_TIME t;
    std::memset(&t, 0, sizeof(t));
    DWORD returned = 0;
    if (!NET_DVR_GetDVRConfig(lUserID, NET_DVR_GET_TIMECFG, 0, &t, sizeof(t), &returned)) {
        qWarning() << "[Hikvision] NET_DVR_GET_TIMECFG failed for" << ip << "error" << NET_DVR_GetLastError();
        return -1;
    }
    QDateTime dt(QDate(static_cast<int>(t.dwYear), static_cast<int>(t.dwMonth), static_cast<int>(t.dwDay)),
                 QTime(static_cast<int>(t.dwHour), static_cast<int>(t.dwMinute), static_cast<int>(t.dwSecond)),
                 Qt::LocalTime);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : -1;
}

void HikvisionManager::forceIFrameAsync(const QString &ip, int port, const QString &username, const QString &password, int channelId, bool subStream)
{
    const QString host = cleanHost(ip);
    if (host.isEmpty() || username.isEmpty() || password.isEmpty() || channelId <= 0)
        return;
    const int sdkPort = port > 0 ? port : 8000;
    startWorker([this, host, sdkPort, username, password, channelId, subStream]() {
        forceIFrame(host, sdkPort, username, password, channelId, subStream);
    });
}

bool HikvisionManager::forceIFrame(const QString &ip, int port, const QString &username, const QString &password, int channelId, bool subStream)
{
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    LONG lUserID = getSession(ip, port, username, password);
    if (lUserID < 0) {
        qDebug() << "[Hikvision] forceIFrame skipped, no session for" << ip;
        return false;
    }

    NET_DVR_DEVICEINFO_V40 deviceInfo;
    int targetChannel = channelId;
    if (getDeviceInfo(ip, deviceInfo)) {
        int startDChan = deviceInfo.struDeviceV30.byStartDChan;
        int chanNum = deviceInfo.struDeviceV30.byChanNum;
        int startChan = deviceInfo.struDeviceV30.byStartChan;
        if (startDChan > 0 && channelId < startDChan) {
            if (channelId <= chanNum) {
                targetChannel = startChan + channelId - 1;
            } else {
                targetChannel = startDChan + (channelId - chanNum) - 1;
            }
            qDebug() << "[Hikvision] Translated logical channel" << channelId << "to SDK channel ID" << targetChannel << "for MakeKeyFrame";
        }
    }

    // The channel id is already the SDK channel. Sub and main do not fall back onto each other.
    const BOOL ret = subStream
        ? NET_DVR_MakeKeyFrameSub(lUserID, static_cast<LONG>(targetChannel))
        : NET_DVR_MakeKeyFrame(lUserID, static_cast<LONG>(targetChannel));
    const char *api = subStream ? "NET_DVR_MakeKeyFrameSub" : "NET_DVR_MakeKeyFrame";
    if (!ret) {
        qWarning() << "[Hikvision]" << api << "failed for IP:" << ip << "Channel:" << targetChannel << "Error:" << NET_DVR_GetLastError();
        return false;
    }

    qDebug() << "[Hikvision]" << api << "succeeded for IP:" << ip << "Channel:" << targetChannel;
    return true;
}

bool HikvisionManager::ptzZoom(const QString &ip, int port, const QString &username, const QString &password, int channelId, int command, bool stop)
{
    {
        std::lock_guard<std::mutex> lock(m_ptzMutex);
        m_ptzQueue.push({ip, port, username, password, channelId, command, stop, 0});
    }
    m_ptzCond.notify_one();
    return true;
}

bool HikvisionManager::ptzPreset(const QString &ip, int port, const QString &username, const QString &password, int channelId, int command, int presetIndex)
{
    {
        std::lock_guard<std::mutex> lock(m_ptzMutex);
        m_ptzQueue.push({ip, port, username, password, channelId, command, false, presetIndex});
    }
    m_ptzCond.notify_one();
    return true;
}

void HikvisionManager::ptzWorkerLoop()
{
    while (true) {
        PtzCommand cmd;
        {
            std::unique_lock<std::mutex> lock(m_ptzMutex);
            m_ptzCond.wait(lock, [this]() { return m_ptzWorkerStop || !m_ptzQueue.empty(); });
            if (m_ptzWorkerStop && m_ptzQueue.empty()) {
                break;
            }
            cmd = m_ptzQueue.front();
            m_ptzQueue.pop();
        }

        // Process the command asynchronously. The lock covers the SDK call that follows getSession.
        std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
        LONG lUserID = getSession(cmd.ip, cmd.port, cmd.username, cmd.password);
        if (lUserID < 0) {
            qWarning() << "[Hikvision PTZ Worker] Failed to get session for IP:" << cmd.ip;
            continue;
        }

        NET_DVR_DEVICEINFO_V40 deviceInfo;
        int targetChannel = cmd.channelId;
        if (getDeviceInfo(cmd.ip, deviceInfo)) {
            int startDChan = deviceInfo.struDeviceV30.byStartDChan;
            int chanNum = deviceInfo.struDeviceV30.byChanNum;
            int startChan = deviceInfo.struDeviceV30.byStartChan;
            if (startDChan > 0 && cmd.channelId < startDChan) {
                if (cmd.channelId <= chanNum) {
                    targetChannel = startChan + cmd.channelId - 1;
                } else {
                    targetChannel = startDChan + (cmd.channelId - chanNum) - 1;
                }
                qDebug() << "[Hikvision PTZ Worker] Translated logical channel" << cmd.channelId << "to SDK channel ID" << targetChannel;
            }
        }

        BOOL ret = FALSE;
        if (cmd.presetIndex > 0) {
            // Preset command (SET_PRESET=8, GOTO_PRESET=39, CLE_PRESET=9) — acts on a preset slot
            ret = NET_DVR_PTZPreset_Other(lUserID, static_cast<LONG>(targetChannel), static_cast<DWORD>(cmd.command), static_cast<DWORD>(cmd.presetIndex));
        } else {
            DWORD dwStop = cmd.stop ? 1 : 0;
            ret = NET_DVR_PTZControl_Other(lUserID, static_cast<LONG>(targetChannel), static_cast<DWORD>(cmd.command), dwStop);
        }
        if (!ret) {
            DWORD err = NET_DVR_GetLastError();
            qWarning() << "[Hikvision PTZ Worker] PTZ Control failed for IP:" << cmd.ip << "Channel:" << targetChannel 
                       << "Command:" << cmd.command << "Preset:" << cmd.presetIndex << "Stop:" << cmd.stop << "Error:" << err;
        } else {
            qDebug() << "[Hikvision PTZ Worker] PTZ Control succeeded for IP:" << cmd.ip << "Channel:" << targetChannel 
                     << "Command:" << cmd.command << "Preset:" << cmd.presetIndex << "Stop:" << cmd.stop;
        }
    }
}

LONG HikvisionManager::loginShared(const QString &ip, int port, const QString &username, const QString &password, NET_DVR_DEVICEINFO_V40 &deviceInfo)
{
    const QString host = cleanHost(ip);
    if (host.isEmpty() || username.isEmpty() || password.isEmpty())
        return -1;
    if (LoginGuard::instance().isRefused(host, username, password)) {
        qWarning() << "[Hikvision Shared] Login skipped, already refused for" << host;
        return -1;
    }

    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    bool wasLogged = false;
    bool isLoggedNow = false;
    LONG lUserID = -1;
    LONG stale = -1;

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        if (m_sharedSessions.contains(host)) {
            SharedSession &session = m_sharedSessions[host];
            if (session.port == port && session.username == username && session.password == password) {
                session.refCount++;
                deviceInfo = session.deviceInfo;
                qDebug() << "[Hikvision Shared] Reusing session for IP:" << host << "UserID:" << session.lUserID << "RefCount:" << session.refCount;
                return session.lUserID;
            }
            stale = session.lUserID;
            m_sharedSessions.remove(host);
        }
        wasLogged = isLoggedInternal(host);
    }
    if (stale >= 0)
        NET_DVR_Logout(stale);

    qDebug() << "[Hikvision Shared] Creating NEW session for IP:" << ip << ":" << port;

    ensureInitialized();

    if (!m_initialized) {
        qWarning() << "[Hikvision Shared] Cannot login: SDK not initialized.";
        return -1;
    }

    NET_DVR_USER_LOGIN_INFO loginInfo;
    std::memset(&loginInfo, 0, sizeof(NET_DVR_USER_LOGIN_INFO));
    std::strncpy(loginInfo.sDeviceAddress, host.toUtf8().constData(), sizeof(loginInfo.sDeviceAddress) - 1);
    loginInfo.wPort = static_cast<WORD>(port);
    std::strncpy(loginInfo.sUserName, username.toUtf8().constData(), sizeof(loginInfo.sUserName) - 1);
    std::strncpy(loginInfo.sPassword, password.toUtf8().constData(), sizeof(loginInfo.sPassword) - 1);
    loginInfo.bUseAsynLogin = FALSE;
    loginInfo.byLoginMode = 0;

    NET_DVR_DEVICEINFO_V40 devInfo;
    std::memset(&devInfo, 0, sizeof(NET_DVR_DEVICEINFO_V40));

    lUserID = NET_DVR_Login_V40(&loginInfo, &devInfo);
    if (lUserID < 0) {
        const DWORD err = NET_DVR_GetLastError();
        if (LoginGuard::isSdkAuthFailure(err))
            LoginGuard::instance().refuse(host, username, password);
        qWarning() << "[Hikvision Shared] Login FAILED for IP:" << host << "Error:" << err;
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        SharedSession session;
        session.lUserID = lUserID;
        session.deviceInfo = devInfo;
        session.refCount = 1;
        session.port = port;
        session.username = username;
        session.password = password;

        m_sharedSessions.insert(host, session);
        deviceInfo = devInfo;
        isLoggedNow = isLoggedInternal(host);
    }

    qDebug() << "[Hikvision Shared] Login SUCCESS for IP:" << host << "UserID:" << lUserID;
    if (wasLogged != isLoggedNow)
        emit sessionStatusChanged(host, isLoggedNow);
    return lUserID;
}

void HikvisionManager::logoutShared(const QString &ip)
{
    const QString host = cleanHost(ip);
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    bool wasLogged = false;
    bool isLoggedNow = false;
    LONG lUserIDToLogout = -1;

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        wasLogged = isLoggedInternal(host);
        if (m_sharedSessions.contains(host)) {
            auto &session = m_sharedSessions[host];
            session.refCount--;
            qDebug() << "[Hikvision Shared] Decremented refCount for IP:" << host << "New RefCount:" << session.refCount;
            if (session.refCount <= 0) {
                lUserIDToLogout = session.lUserID;
                m_sharedSessions.remove(host);
            }
        }
        isLoggedNow = isLoggedInternal(host);
    }

    if (lUserIDToLogout >= 0) {
        ensureInitialized();
        NET_DVR_Logout(lUserIDToLogout);
        qDebug() << "[Hikvision Shared] Logged out session for IP:" << host << "UserID:" << lUserIDToLogout;
    }

    if (wasLogged != isLoggedNow)
        emit sessionStatusChanged(host, isLoggedNow);
}

void HikvisionManager::forceLogoutShared(const QString &ip)
{
    const QString host = cleanHost(ip);
    std::lock_guard<std::recursive_mutex> sdkLock(m_sdkMutex);
    bool wasLogged = false;
    bool isLoggedNow = false;
    LONG lUserIDToLogout = -1;

    {
        std::lock_guard<std::mutex> lock(m_sharedSessionsMutex);
        wasLogged = isLoggedInternal(host);
        if (m_sharedSessions.contains(host)) {
            auto &session = m_sharedSessions[host];
            lUserIDToLogout = session.lUserID;
            m_sharedSessions.remove(host);
            qDebug() << "[Hikvision Shared] Force logout requested. Removing session for IP:" << host << "UserID:" << lUserIDToLogout;
        }
        isLoggedNow = isLoggedInternal(host);
    }

    if (lUserIDToLogout >= 0) {
        ensureInitialized();
        NET_DVR_Logout(lUserIDToLogout);
        qDebug() << "[Hikvision Shared] Logged out session for IP (force):" << host << "UserID:" << lUserIDToLogout;
    }

    if (wasLogged != isLoggedNow)
        emit sessionStatusChanged(host, isLoggedNow);
}

void HikvisionManager::ensureInitialized() const
{
    std::unique_lock<std::mutex> lock(m_initMutex);
    m_initCond.wait(lock, [this]() { return m_initCompleted; });
}
