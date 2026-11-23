#include "hikvisionptzaim.h"
#include "loginguard.h"

#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {
QString quoted(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return s;
}

int to255(double fraction)
{
    return std::max(0, std::min(255, int(std::lround(fraction * 255.0))));
}

bool tagIsTrue(const QString &xml, const QString &tag)
{
    const QRegularExpression rx(QStringLiteral("<%1>\\s*true\\s*</%1>").arg(tag), QRegularExpression::CaseInsensitiveOption);
    return rx.match(xml).hasMatch();
}
}

HikvisionPtzAim::HikvisionPtzAim(QObject *parent)
    : QObject(parent)
{
}

// Runs curl with its settings on stdin, so the password never shows in the process list. A body, when given, is sent as
// a PUT data line in the same settings. done gets the HTTP status (0 when curl could not connect) and the answer.
void HikvisionPtzAim::request(const QString &method, const QString &url, const QString &user, const QString &pass,
                              const QByteArray &body, const Done &done)
{
    QString cfg = QStringLiteral("user = \"%1\"\n").arg(quoted(user + QLatin1Char(':') + pass));
    if (!body.isEmpty()) {
        cfg += QStringLiteral("data-binary = \"%1\"\n").arg(quoted(QString::fromUtf8(body)));
        cfg += QStringLiteral("header = \"Content-Type: application/xml\"\n");
    }
    const QStringList args = { QStringLiteral("-s"), QStringLiteral("-k"), QStringLiteral("--anyauth"), QStringLiteral("-K"), QStringLiteral("-"),
                               QStringLiteral("--max-time"), QStringLiteral("10"), QStringLiteral("-X"), method,
                               QStringLiteral("-w"), QStringLiteral("\n__HTTP__%{http_code}"), url };
    auto *p = new QProcess(this);
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            done(0, QByteArray());
            p->deleteLater();
        }
    });
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [p, done](int, QProcess::ExitStatus) {
        const QByteArray out = p->readAllStandardOutput();
        const int at = out.lastIndexOf("\n__HTTP__");
        done(at >= 0 ? out.mid(at + 9).trimmed().toInt() : 0, at >= 0 ? out.left(at) : out);
        p->deleteLater();
    });
    p->start(QStringLiteral("curl"), args);
    p->write(cfg.toUtf8());
    p->closeWriteChannel();
}

void HikvisionPtzAim::checkWideLens(const QString &ip, int httpPort, const QString &user, const QString &pass, int channel)
{
    if (ip.isEmpty() || user.isEmpty() || pass.isEmpty() || channel <= 0) {
        emit lensChecked(ip, channel, false, false, QStringLiteral("no login saved for this camera"));
        return;
    }
    if (LoginGuard::instance().isRefused(ip, user, pass)) {
        emit lensChecked(ip, channel, false, false, QStringLiteral("the camera refused the login earlier (not tried again)"));
        return;
    }
    const QString url = QStringLiteral("http://%1:%2/ISAPI/PTZCtrl/channels/%3/capabilities").arg(ip).arg(httpPort > 0 ? httpPort : 80).arg(channel);
    QPointer<HikvisionPtzAim> self(this);
    request(QStringLiteral("GET"), url, user, pass, QByteArray(), [self, ip, channel, user, pass](int code, const QByteArray &body) {
        if (!self)
            return;
        if (code == 401) {
            LoginGuard::instance().refuse(ip, user, pass);
            emit self->lensChecked(ip, channel, false, false, QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)"));
            return;
        }
        if (code != 200) {
            emit self->lensChecked(ip, channel, false, false, code == 0 ? QStringLiteral("could not connect") : QStringLiteral("the camera answered HTTP %1").arg(code));
            return;
        }
        // Any PTZ lens that lists 3D positioning takes a box. Only the wide lens of a two-lens PTZ also lists manual tracking.
        const QString xml = QString::fromUtf8(body);
        const bool ok = tagIsTrue(xml, QStringLiteral("isSupportPosition3D"));
        const bool wide = ok && tagIsTrue(xml, QStringLiteral("isSupportManualTrack"));
        emit self->lensChecked(ip, channel, ok, wide, ok ? QString() : QStringLiteral("this camera does not offer 3D positioning"));
    });
}

void HikvisionPtzAim::aimAt(const QString &ip, int httpPort, const QString &user, const QString &pass, int channel,
                            double x1, double y1, double x2, double y2)
{
    if (ip.isEmpty() || user.isEmpty() || pass.isEmpty() || channel <= 0) {
        emit aimResult(ip, false, QStringLiteral("no login saved for this camera"));
        return;
    }
    if (LoginGuard::instance().isRefused(ip, user, pass)) {
        emit aimResult(ip, false, QStringLiteral("the camera refused the login earlier (not tried again)"));
        return;
    }
    // The camera counts the vertical axis from the bottom of the picture, a tile from the top.
    const int sx = to255(std::min(x1, x2));
    const int ex = to255(std::max(x1, x2));
    const int sy = to255(1.0 - std::max(y1, y2));
    const int ey = to255(1.0 - std::min(y1, y2));
    if (ex - sx < 4 || ey - sy < 4) {
        emit aimResult(ip, false, QStringLiteral("the box is too small"));
        return;
    }
    const QByteArray xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Position3D><StartPoint><positionX>%1</positionX><positionY>%2</positionY></StartPoint>"
        "<EndPoint><positionX>%3</positionX><positionY>%4</positionY></EndPoint></Position3D>").arg(sx).arg(sy).arg(ex).arg(ey).toUtf8();
    const QString url = QStringLiteral("http://%1:%2/ISAPI/PTZCtrl/channels/%3/position3D").arg(ip).arg(httpPort > 0 ? httpPort : 80).arg(channel);
    QPointer<HikvisionPtzAim> self(this);
    request(QStringLiteral("PUT"), url, user, pass, xml, [self, ip, user, pass](int code, const QByteArray &) {
        if (!self)
            return;
        if (code == 200) {
            emit self->aimResult(ip, true, QString());
        } else if (code == 401) {
            LoginGuard::instance().refuse(ip, user, pass);
            emit self->aimResult(ip, false, QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)"));
        } else {
            emit self->aimResult(ip, false, code == 0 ? QStringLiteral("could not connect") : QStringLiteral("the camera answered HTTP %1").arg(code));
        }
    });
}
