#ifndef HIKVISIONPTZAIM_H
#define HIKVISIONPTZAIM_H

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

// "Point the camera here" for a two-lens PTZ camera (a close-up PTZ lens plus a fixed wide lens).
// A box drawn on the wide lens's picture is sent to the camera's 3D positioning call
//   PUT /ISAPI/PTZCtrl/channels/<wide lens>/position3D
// and the camera turns and zooms the PTZ lens to it. Checked on a real camera:
//  * both axes run 0 to 255; the vertical axis starts at the BOTTOM of the picture, the horizontal at the left;
//  * the call goes to the channel of the lens the box was drawn on (the camera's own channel number, not the recorder's). On the wide lens of
//    a two-lens PTZ it aims the close-up lens (tested). On the close-up lens it re-centers and zooms that lens (tested on the Cabin PTZ).
// Nothing is stored on the camera. A refused login is never retried: a wrong password can lock a camera.
class HikvisionPtzAim : public QObject
{
    Q_OBJECT

public:
    explicit HikvisionPtzAim(QObject *parent = nullptr);

    // Asks the camera whether this channel can take a drawn box: its PTZ capabilities list isSupportPosition3D. wide says it is the wide
    // lens of a two-lens PTZ, which also lists isSupportManualTrack (the close-up lens and a single-lens PTZ list only the first).
    // Answers with lensChecked().
    Q_INVOKABLE void checkWideLens(const QString &ip, int httpPort, const QString &user, const QString &pass, int channel);

    // Sends one box. x1..y2 are fractions (0 to 1) of the picture the box was drawn on, with the origin at its top left.
    // Answers with aimResult().
    Q_INVOKABLE void aimAt(const QString &ip, int httpPort, const QString &user, const QString &pass, int channel,
                           double x1, double y1, double x2, double y2);

signals:
    // ok is true when this channel offers 3D positioning; wide is true when it is the wide lens of a two-lens PTZ (a box then picks a spot in
    // the whole scene), false for a close-up or single-lens PTZ (a box re-centers and zooms on what that lens shows). message says why not.
    void lensChecked(const QString &ip, int channel, bool ok, bool wide, const QString &message);
    void aimResult(const QString &ip, bool ok, const QString &message);

private:
    using Done = std::function<void(int code, const QByteArray &body)>;
    void request(const QString &method, const QString &url, const QString &user, const QString &pass,
                 const QByteArray &body, const Done &done);

    QSet<QString> m_refused; // addresses whose login was refused once
};

#endif // HIKVISIONPTZAIM_H
