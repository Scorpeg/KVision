#include "hikvisionvcaoverlay.h"
#include "hikvisionmanager.h"

#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <cmath>
#include <QPointer>
#include <QThread>
#include <QCoreApplication>
#include <QDebug>
#include <thread>

extern std::atomic<int> g_sdkStreams; // defined in qmlavdemuxer.cpp: open SDK sessions, for the statistics panel
extern std::atomic<qint64> g_networkBytesAccumulator; // network statistics

namespace {
constexpr int kLiveHoldMs = 1200;   // a box with no newer report fades out after this (live reports are ~4/s)
constexpr int kRetryMs = 15000;     // after a failed connection
constexpr int kStreamSub = 1;       // the sub-stream carries the same smart data at a fraction of the bytes
}

// One SDK live connection. The SDK's data callback holds a raw pointer to it, so it is kept alive (and
// told it is closed) until NET_DVR_StopRealPlay has returned; only then is the last reference dropped.
struct HikvisionVcaOverlay::Link {
    HikVcaDecoder decoder;
    std::atomic<bool> closed{false};
    std::atomic<bool> notifyPending{false};
    std::mutex ownerMutex;               // owner is cleared on the GUI thread and read in the SDK callback
    QPointer<HikvisionVcaOverlay> owner;
    QString ip;
    std::atomic<LONG> userId{-1};
    std::atomic<LONG> handle{-1};
    std::mutex stopMutex; // the connect and stop threads both may close the connection; only one does
};

HikvisionVcaOverlay::HikvisionVcaOverlay(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, [this]() { if (m_active && !m_link) start(); });
    m_frame.setInterval(40);
    connect(&m_frame, &QTimer::timeout, this, [this]() {
        update();
        // The box only moves while it glides to the newest report; after that nothing changes until the next
        // report (or until it is dropped, which a single-shot repaint below takes care of).
        if (!m_link || m_sinceReport.elapsed() > m_reportGapMs) {
            m_frame.stop();
            const int left = kLiveHoldMs - int(m_sinceReport.elapsed());
            if (m_link && left > 0)
                QTimer::singleShot(left + 20, this, [this]() { update(); });
        }
    });
}

HikvisionVcaOverlay::~HikvisionVcaOverlay()
{
    stop();
}

void HikvisionVcaOverlay::setRecorderIp(const QString &v) { if (m_ip != v) { m_ip = v; emit settingsChanged(); restart(); } }
void HikvisionVcaOverlay::setPort(int v) { if (m_port != v) { m_port = v; emit settingsChanged(); restart(); } }
void HikvisionVcaOverlay::setUsername(const QString &v) { if (m_user != v) { m_user = v; emit settingsChanged(); restart(); } }
void HikvisionVcaOverlay::setPassword(const QString &v) { if (m_pass != v) { m_pass = v; emit settingsChanged(); restart(); } }
void HikvisionVcaOverlay::setChannelId(int v) { if (m_channel != v) { m_channel = v; emit settingsChanged(); restart(); } }

void HikvisionVcaOverlay::setActive(bool v)
{
    if (m_active == v)
        return;
    m_active = v;
    emit activeChanged();
    restart();
    update();
}

void HikvisionVcaOverlay::setShowRules(bool v) { if (m_showRules != v) { m_showRules = v; emit drawChanged(); update(); } }
void HikvisionVcaOverlay::setShowTargets(bool v) { if (m_showTargets != v) { m_showTargets = v; emit drawChanged(); update(); } }
void HikvisionVcaOverlay::setVideoRect(const QRectF &r) { if (m_videoRect != r) { m_videoRect = r; emit drawChanged(); update(); } }

void HikvisionVcaOverlay::setStatus(const QString &s)
{
    if (m_status != s) {
        m_status = s;
        emit statusChanged();
    }
}

void HikvisionVcaOverlay::restart()
{
    stop();
    m_retry.stop();
    if (m_active)
        start();
}

void HikvisionVcaOverlay::closeLink(const std::shared_ptr<Link> &link)
{
    std::lock_guard<std::mutex> lock(link->stopMutex);
    const LONG handle = link->handle.exchange(-1);
    if (handle >= 0) {
        NET_DVR_StopRealPlay(handle); // waits for a callback that is running
        g_sdkStreams.fetch_sub(1, std::memory_order_relaxed);
    }
    if (link->userId.exchange(-1) >= 0) {
        if (HikvisionManager *mgr = HikvisionManager::instance())
            mgr->logoutShared(link->ip);
    }
}

void HikvisionVcaOverlay::start()
{
    // A blank password counts as a failed login and can lock a camera, so it is never sent.
    if (m_link || m_ip.isEmpty())
        return;
    if (m_user.isEmpty() || m_pass.isEmpty()) {
        qInfo() << "[VcaLive]" << m_ip << "no saved login for target boxes";
        setStatus(QStringLiteral("error"));
        return;
    }
    qInfo() << "[VcaLive]" << m_ip << "channel" << m_channel << "connecting for target boxes";
    auto link = std::make_shared<Link>();
    link->owner = this;
    link->ip = m_ip;
    m_link = link;
    m_lastReportMs = -1;
    setStatus(QStringLiteral("connecting"));

    // Login and NET_DVR_RealPlay_V40 can take seconds; they run off the GUI thread.
    const QString ip = m_ip, user = m_user, pass = m_pass;
    const int port = m_port > 0 ? m_port : 8000;
    const int channel = m_channel;
    std::thread([link, ip, port, user, pass, channel]() {
        NET_DVR_DEVICEINFO_V40 info;
        std::memset(&info, 0, sizeof(info));
        HikvisionManager *mgr = HikvisionManager::instance();
        bool ok = false;
        const LONG userId = mgr ? mgr->loginShared(ip, port, user, pass, info) : -1;
        if (userId >= 0) {
            link->userId = userId;
            // The logical channel to the SDK channel, as the archive player does.
            int sdkChannel = channel;
            const auto &d = info.struDeviceV30;
            if (d.byChanNum == 0 && d.byIPChanNum > 0)
                sdkChannel = channel + d.byStartDChan - 1;
            else if (d.byChanNum > 0 && channel > d.byChanNum)
                sdkChannel = d.byStartDChan + (channel - d.byChanNum) - 1;
            else if (d.byChanNum > 0)
                sdkChannel = channel + d.byStartChan - 1;
            if (!link->closed.load()) {
                NET_DVR_PREVIEWINFO preview;
                std::memset(&preview, 0, sizeof(preview));
                preview.lChannel = sdkChannel;
                preview.dwStreamType = kStreamSub;
                preview.dwLinkMode = 0; // TCP
                preview.hPlayWnd = 0;
                preview.bBlocked = FALSE;
                const LONG handle = NET_DVR_RealPlay_V40(userId, &preview, &HikvisionVcaOverlay::dataCallBack, link.get());
                if (handle >= 0) {
                    g_sdkStreams.fetch_add(1, std::memory_order_relaxed);
                    link->handle = handle;
                    ok = true;
                } else {
                    qWarning() << "[VcaLive]" << ip << "live smart data failed, SDK error" << NET_DVR_GetLastError();
                }
            }
        } else {
            qWarning() << "[VcaLive]" << ip << "login failed for live smart data";
        }
        if (!ok || link->closed.load())
            closeLink(link); // failed, or switched off while connecting
        QPointer<HikvisionVcaOverlay> owner;
        {
            std::lock_guard<std::mutex> lock(link->ownerMutex);
            owner = link->owner;
        }
        if (!owner)
            return;
        QMetaObject::invokeMethod(owner.data(), [owner, link, ok]() {
            HikvisionVcaOverlay *self = owner.data();
            if (!self || self->m_link != link)
                return;
            if (ok) {
                qInfo() << "[VcaLive]" << self->m_ip << "channel" << self->m_channel << "target boxes connected";
                self->setStatus(QStringLiteral("on"));
            } else {
                self->m_link.reset();
                self->setStatus(QStringLiteral("error"));
                if (self->m_active)
                    self->m_retry.start();
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void HikvisionVcaOverlay::stop()
{
    m_frame.stop();
    if (!m_link)
        return;
    std::shared_ptr<Link> link = std::move(m_link);
    m_link.reset();
    link->closed = true;
    {
        std::lock_guard<std::mutex> lock(link->ownerMutex);
        link->owner = nullptr;
    }
    setStatus(QString());
    // NET_DVR_StopRealPlay waits for a running callback, so it is done off the GUI thread. If the connect
    // thread is still connecting, it closes the connection itself when it sees `closed`.
    std::thread([link]() { closeLink(link); }).detach();
    update();
}

void CALLBACK HikvisionVcaOverlay::dataCallBack(LONG handle, DWORD type, BYTE *buffer, DWORD size, void *user)
{
    Q_UNUSED(handle);
    auto *link = static_cast<Link *>(user);
    if (!link || link->closed.load())
        return;
    g_networkBytesAccumulator.fetch_add(size, std::memory_order_relaxed);
    if (type == NET_DVR_SYSHEAD) {
        link->decoder.reset();
        return;
    }
    if (type != NET_DVR_STREAMDATA || !buffer || size == 0)
        return;
    const qint64 before = link->decoder.latestMs();
    link->decoder.feed(buffer, static_cast<int>(size));
    if (link->decoder.latestMs() == before || link->notifyPending.exchange(true))
        return;
    // The callback cannot outlive the link: stop() keeps it until NET_DVR_StopRealPlay has returned. The
    // repaint request is queued on the overlay itself (dropped by Qt if the overlay is gone), and it only
    // touches the link while it is still the overlay's current one, which the overlay keeps alive.
    QPointer<HikvisionVcaOverlay> owner;
    {
        std::lock_guard<std::mutex> lock(link->ownerMutex);
        owner = link->owner;
    }
    if (!owner)
        return;
    QMetaObject::invokeMethod(owner.data(), [owner, link]() {
        if (owner && owner->m_link.get() == link) {
            link->notifyPending = false;
            owner->onData();
        }
    }, Qt::QueuedConnection);
}

void HikvisionVcaOverlay::onData()
{
    if (!m_link)
        return;
    const qint64 newest = m_link->decoder.latestMs();
    if (newest != m_lastReportMs) {
        if (m_lastReportMs >= 0 && newest > m_lastReportMs)
            m_reportGapMs = qBound<qint64>(60, newest - m_lastReportMs, 1000);
        m_lastReportMs = newest;
        m_sinceReport.restart();
        if (!m_frame.isActive())
            m_frame.start();
    }
    update();
}

// Builds one outline per box (and per rule, if drawn) as GPU line geometry. A box is a closed
// strip of thin quads (a rectangle frame), coloured with a flat-colour material.
namespace {
QSGGeometryNode *frameNode(const QVector<QPointF> &points, bool closed, qreal width, const QColor &color)
{
    const int n = points.size();
    const int segments = closed ? n : n - 1;
    if (segments < 1)
        return nullptr;
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), segments * 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    QSGGeometry::Point2D *v = geometry->vertexDataAsPoint2D();
    const qreal h = width / 2.0;
    for (int i = 0; i < segments; ++i) {
        const QPointF a = points[i];
        const QPointF b = points[(i + 1) % n];
        QPointF d = b - a;
        const qreal len = std::hypot(d.x(), d.y());
        if (len <= 0.0)
            d = QPointF(1, 0);
        else
            d /= len;
        // extend each segment by half the width so the corners close, and offset to both sides
        const QPointF e = d * h;
        const QPointF nrm(-d.y() * h, d.x() * h);
        const QPointF p1 = a - e + nrm, p2 = a - e - nrm, p3 = b + e + nrm, p4 = b + e - nrm;
        v[i * 6 + 0].set(float(p1.x()), float(p1.y()));
        v[i * 6 + 1].set(float(p2.x()), float(p2.y()));
        v[i * 6 + 2].set(float(p3.x()), float(p3.y()));
        v[i * 6 + 3].set(float(p3.x()), float(p3.y()));
        v[i * 6 + 4].set(float(p2.x()), float(p2.y()));
        v[i * 6 + 5].set(float(p4.x()), float(p4.y()));
    }
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new QSGFlatColorMaterial;
    material->setColor(color);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}
}

QSGNode *HikvisionVcaOverlay::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // The node tree is small (a few outlines), so it is rebuilt on every update.
    delete oldNode;
    if (!m_active || !m_link || (!m_showRules && !m_showTargets))
        return nullptr;
    const QRectF video = m_videoRect.isValid() ? m_videoRect : boundingRect();
    QVector<HikVcaDecoder::Target> targets;
    QVector<quint32> alarmRules;
    if (m_showTargets && m_lastReportMs >= 0 && m_sinceReport.isValid()) {
        // Stream time now: the newest report's time plus the wall time since it arrived.
        const qint64 now = m_lastReportMs + m_sinceReport.elapsed();
        const auto list = m_link->decoder.tracks(now, kLiveHoldMs, &alarmRules);
        for (const HikVcaDecoder::Track &tr : list) {
            HikVcaDecoder::Target target = tr.target;
            // Glide from the target's previous box to its newest one over the time between its own reports.
            if (tr.previousMs >= 0 && tr.lastMs > tr.previousMs) {
                const qreal gap = qreal(qBound<qint64>(40, tr.lastMs - tr.previousMs, 1000));
                const qreal t = qBound<qreal>(0.0, qreal(now - tr.lastMs) / gap, 1.0);
                const QRectF a = tr.previous;
                const QRectF b = tr.target.rect;
                target.rect = QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                                     a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t);
            }
            targets.append(target);
        }
    }
    auto toView = [&video](const QPointF &pt) {
        return QPointF(video.x() + pt.x() * video.width(), video.y() + pt.y() * video.height());
    };
    const qreal lineWidth = std::max<qreal>(1.5, video.height() / 540.0);
    auto *root = new QSGNode;
    if (m_showRules) {
        for (const HikVcaDecoder::Rule &rule : m_link->decoder.rules()) {
            QVector<QPointF> pts;
            for (const QPointF &pt : rule.points)
                pts << toView(pt);
            const bool red = alarmRules.contains(rule.id);
            if (QSGGeometryNode *n = frameNode(pts, rule.polygon && pts.size() >= 3, lineWidth, red ? QColor(255, 0, 0) : QColor(0, 0, 255)))
                root->appendChildNode(n);
        }
    }
    for (const HikVcaDecoder::Target &t : targets) {
        const QRectF r(toView(t.rect.topLeft()), toView(t.rect.bottomRight()));
        const QVector<QPointF> corners = { r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft() };
        if (QSGGeometryNode *n = frameNode(corners, true, lineWidth, t.alarm ? QColor(255, 0, 0) : QColor(0, 255, 0)))
            root->appendChildNode(n);
    }
    if (!root->childCount()) {
        delete root;
        return nullptr;
    }
    return root;
}
