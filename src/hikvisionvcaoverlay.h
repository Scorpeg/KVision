#ifndef HIKVISIONVCAOVERLAY_H
#define HIKVISIONVCAOVERLAY_H

#include <QQuickItem>
#include <QElapsedTimer>
#include <QTimer>
#include <QSizeF>
#include <atomic>
#include <memory>
#include "hcnetsdk_compat.h"
#include "hikvisionvca.h"

// Live smart (VCA) overlay for a Hikvision camera tile, as iVMS-4200 draws it: the rule lines/zones and every
// tracked target's box (green, red while it sets off a rule; the rule it set off turns red too).
// The picture comes from the tile's own player; this item only opens a light SDK sub-stream connection to read
// the camera's smart data (no video is decoded) and draws over the tile. It connects only while it is active.
// Live boxes arrive a few times a second, so a box glides from one report to the next.
// The outlines are drawn by the GPU as scene graph line nodes (no full-tile picture is painted and uploaded),
// so several tiles with several targets each stay cheap.
class HikvisionVcaOverlay : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString recorderIp READ recorderIp WRITE setRecorderIp NOTIFY settingsChanged)
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY settingsChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY settingsChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY settingsChanged)
    Q_PROPERTY(int channelId READ channelId WRITE setChannelId NOTIFY settingsChanged)
    // Connect and draw only while true (the tile is visible and one of the two switches below is on).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool showRules READ showRules WRITE setShowRules NOTIFY drawChanged)
    Q_PROPERTY(bool showTargets READ showTargets WRITE setShowTargets NOTIFY drawChanged)
    // Where the picture is drawn inside this item (the tile's fitted video rectangle).
    Q_PROPERTY(QRectF videoRect READ videoRect WRITE setVideoRect NOTIFY drawChanged)
    // "", "connecting", "on" or "error"; for a tooltip or a status line.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit HikvisionVcaOverlay(QQuickItem *parent = nullptr);
    ~HikvisionVcaOverlay() override;

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

    QString recorderIp() const { return m_ip; }
    void setRecorderIp(const QString &v);
    int port() const { return m_port; }
    void setPort(int v);
    QString username() const { return m_user; }
    void setUsername(const QString &v);
    QString password() const { return m_pass; }
    void setPassword(const QString &v);
    int channelId() const { return m_channel; }
    void setChannelId(int v);
    bool active() const { return m_active; }
    void setActive(bool v);
    bool showRules() const { return m_showRules; }
    void setShowRules(bool v);
    bool showTargets() const { return m_showTargets; }
    void setShowTargets(bool v);
    QRectF videoRect() const { return m_videoRect; }
    void setVideoRect(const QRectF &r);
    QString status() const { return m_status; }

signals:
    void settingsChanged();
    void activeChanged();
    void drawChanged();
    void statusChanged();

private:
    struct Link; // one SDK live connection; shared with its data callback, which can outlive this item
    static void CALLBACK dataCallBack(LONG handle, DWORD type, BYTE *buffer, DWORD size, void *user);
    static void closeLink(const std::shared_ptr<Link> &link); // stops the connection once; any thread
    void restart();
    void start();
    void stop();
    void setStatus(const QString &s);
    void onData(); // called on the GUI thread when new smart data arrived
    void syncReportClock(); // follow the newest target report, including after the clock jumped ahead of it

    QString m_ip;
    int m_port = 8000;
    QString m_user;
    QString m_pass;
    int m_channel = 1;
    bool m_active = false;
    bool m_showRules = true;
    bool m_showTargets = true;
    QRectF m_videoRect;
    QString m_status;

    std::shared_ptr<Link> m_link;
    QTimer m_retry;      // reconnects after a failure
    QTimer m_frame;      // repaints while a box is gliding (stops when it has arrived)
    QTimer m_follow;     // picks up target reports if a repaint request was missed
    // Smoothing: the box moves from the previous report to the newest over the time between them.
    qint64 m_lastReportMs = -1;   // stream time of the newest report seen
    qint64 m_reportGapMs = 250;   // time between the last two reports
    QElapsedTimer m_sinceReport;  // wall time since the newest report arrived
};

#endif // HIKVISIONVCAOVERLAY_H
