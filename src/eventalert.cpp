#include "eventalert.h"

#include <QHash>
#include <QRegularExpression>

namespace {
QString vcaKind(const QString &type)
{
    static const QHash<QString, QString> kinds = {
        { QStringLiteral("linedetection"), QStringLiteral("Line crossing") },
        { QStringLiteral("fielddetection"), QStringLiteral("Intrusion") },
        { QStringLiteral("regionEntrance"), QStringLiteral("Region entrance") },
        { QStringLiteral("regionExiting"), QStringLiteral("Region exit") },
        { QStringLiteral("loitering"), QStringLiteral("Loitering") },
        { QStringLiteral("group"), QStringLiteral("People gathering") },
        { QStringLiteral("rapidMove"), QStringLiteral("Rapid movement") },
        { QStringLiteral("parking"), QStringLiteral("Parking") },
        { QStringLiteral("unattendedBaggage"), QStringLiteral("Unattended object") },
        { QStringLiteral("attendedBaggage"), QStringLiteral("Object removed") },
    };
    return kinds.value(type);
}

QString vcaKey(const QString &type)
{
    if (type == QLatin1String("linedetection"))
        return QStringLiteral("line");
    if (type == QLatin1String("fielddetection"))
        return QStringLiteral("intrusion");
    if (type == QLatin1String("regionEntrance"))
        return QStringLiteral("enter");
    if (type == QLatin1String("regionExiting"))
        return QStringLiteral("exit");
    return QStringLiteral("other");
}

QString vcaShort(const QString &type)
{
    if (type == QLatin1String("linedetection"))
        return QStringLiteral("LINE");
    if (type == QLatin1String("fielddetection"))
        return QStringLiteral("INTRUSION");
    if (type == QLatin1String("regionEntrance"))
        return QStringLiteral("ENTER");
    if (type == QLatin1String("regionExiting"))
        return QStringLiteral("EXIT");
    return QStringLiteral("SMART");
}

QString tagText(const QString &body, const QString &tag)
{
    const QRegularExpression rx(QStringLiteral("<%1>([^<]*)</%1>").arg(tag));
    return rx.match(body).captured(1);
}

QString normalizedTarget(const QString &raw)
{
    const QString t = raw.trimmed().toLower();
    if (t.isEmpty())
        return QString();
    if (t == QLatin1String("human") || t == QLatin1String("vehicle") || t == QLatin1String("animal"))
        return t;
    return QStringLiteral("other");
}
}

QVector<EventAlertSpan> splitEventAlerts(const QString &text)
{
    static const QRegularExpression eventRx(
        QStringLiteral("<EventNotificationAlert.*?</EventNotificationAlert>"),
        QRegularExpression::DotMatchesEverythingOption);
    QVector<EventAlertSpan> spans;
    QRegularExpressionMatchIterator it = eventRx.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        EventAlertSpan span;
        span.start = static_cast<int>(m.capturedStart());
        span.end = static_cast<int>(m.capturedEnd());
        span.body = m.captured(0);
        spans.append(span);
    }
    return spans;
}

bool eventReportIsLate(const QString &cameraTime, const QDateTime &now)
{
    const QDateTime eventTime = QDateTime::fromString(cameraTime, Qt::ISODate);
    if (!eventTime.isValid() || !now.isValid())
        return false;
    return eventTime.toLocalTime().secsTo(now) >= 10;
}

EventAlert classifyEventAlert(const QString &body, const QSet<QString> &kinds, const QSet<QString> &targets,
                              bool frameMotion, const QDateTime &now)
{
    EventAlert alert;
    alert.type = tagText(body, QStringLiteral("eventType"));
    alert.cameraTime = tagText(body, QStringLiteral("dateTime"));
    alert.channel = tagText(body, QStringLiteral("channelID")).toInt();
    alert.state = tagText(body, QStringLiteral("eventState"));
    alert.active = alert.state == QLatin1String("active");
    const bool late = eventReportIsLate(alert.cameraTime, now);

    if (alert.type == QLatin1String("facedetection")) {
        alert.action = EventAlert::Face;
        alert.logKind = QStringLiteral("Face detection");
        alert.shortLabel = QStringLiteral("FACE");
        return alert;
    }

    if (alert.type != QLatin1String("VMD")) {
        alert.logKind = vcaKind(alert.type);
        if (alert.logKind.isEmpty()) {
            alert.action = EventAlert::Other;
            return alert;
        }
        alert.action = EventAlert::Smart;
        alert.shortLabel = vcaShort(alert.type);
        alert.target = normalizedTarget(tagText(body, QStringLiteral("detectionTarget")));
        // An empty target is logged as empty and shown as "other", matching the previous loop.
        const QString shown = alert.target.isEmpty() ? QStringLiteral("other") : alert.target;
        if (alert.active && kinds.contains(vcaKey(alert.type)) && targets.contains(shown) && !late)
            alert.frame = EventAlert::Flash;
        return alert;
    }

    const QString rawTarget = tagText(body, QStringLiteral("targetType")).trimmed();
    if (rawTarget.isEmpty()) {
        alert.action = EventAlert::PlainMotion;
        alert.logKind = QStringLiteral("Motion");
        alert.shortLabel = QStringLiteral("MOTION");
        alert.target = QStringLiteral("other");
        if (kinds.contains(QStringLiteral("motion")) && !late) {
            if (frameMotion && targets.contains(QStringLiteral("other")))
                alert.frame = EventAlert::Flash;
            else
                alert.frame = EventAlert::Keep;
        }
        return alert;
    }

    alert.action = EventAlert::LabelledMotion;
    alert.logKind = QStringLiteral("Motion");
    alert.shortLabel = QStringLiteral("MOTION");
    alert.target = normalizedTarget(rawTarget);
    if (kinds.contains(QStringLiteral("motion")) && targets.contains(alert.target) && !late)
        alert.frame = EventAlert::Flash;
    return alert;
}
