#ifndef CAMERASTATUSTEXT_H
#define CAMERASTATUSTEXT_H

#include <QList>
#include <QLocale>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// What a camera's status page says (GET /ISAPI/System/status), turned into rows of words with a state. One place for the wording and
// the limits, used by the Camera settings window and the right-click "Camera status" card, so they always agree. Pure text work: no
// network and nothing is ever written to a camera.
//
// The run-time counters (cameraRunTotalTime and the three temperature ranges) count MINUTES. The camera does not say so; measured on
// 15 Nov 2026: over 125 seconds they rose by 3 (a G3) and 2 (a PTZ), and a PTZ made in Jan 2023 reads 1,939,340 = 3.7 years.
// Most fixed cameras read 0 for the restart count and run time: they do not keep them.
namespace CameraStatusText {

struct Item {
    QString state;     // ok, note, warning or problem
    QString label;
    QString detail;
    QString shortText; // a few words for the card's summary line; empty when nothing needs a look
    QString cell;      // the figure alone, for a table cell ("58%"); empty when the row has none
};

struct Result {
    QList<Item> items;
    QString upShort, loadShort;                 // "Up 5 days 18 h", "CPU 58%, memory 88%"
    QString upCell, cpuCell, memCell;           // "5 d 18 h", "58%", "88% (26 MB)"
    QString upState, cpuState, memState;        // ok, note or warning
};

inline QString tagOf(const QString &xml, const char *name)
{
    const QRegularExpression rx(QStringLiteral("<%1>([^<]*)</%1>").arg(QLatin1String(name)));
    return rx.match(xml).captured(1).trimmed();
}

inline QString groupedNumber(qint64 n)
{
    return QLocale(QLocale::English).toString(n);
}

inline QString durationText(qint64 s)
{
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    if (s < 3600)
        return QStringLiteral("%1 min").arg(s / 60);
    if (s < 86400)
        return QStringLiteral("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
    return QStringLiteral("%1 days %2 h").arg(s / 86400).arg((s % 86400) / 3600);
}

inline QString shortDuration(qint64 s)
{
    if (s < 3600)
        return QStringLiteral("%1 m").arg(s / 60);
    if (s < 86400)
        return QStringLiteral("%1 h %2 m").arg(s / 3600).arg((s % 3600) / 60);
    return QStringLiteral("%1 d %2 h").arg(s / 86400).arg((s % 86400) / 3600);
}

inline QString minutesSpanText(qint64 minutes)
{
    const double days = double(minutes) / 1440.0;
    if (days >= 365.0)
        return QStringLiteral("%1 years").arg(days / 365.0, 0, 'f', 1);
    if (days >= 2.0)
        return QStringLiteral("%1 days").arg(qint64(days));
    return QStringLiteral("%1 hours").arg(double(minutes) / 60.0, 0, 'f', 1);
}

inline Result build(const QString &xml)
{
    Result r;
    auto num = [&](const char *name) -> qint64 { return tagOf(xml, name).toLongLong(); };
    auto has = [&](const char *name) { return !tagOf(xml, name).isEmpty(); };
    auto add = [&](const QString &state, const QString &label, const QString &detail, const QString &shortText = QString(), const QString &cell = QString()) {
        r.items.append(Item{state, label, detail, shortText, cell});
    };

    // Up since the last restart. A restart in the last hour is a warning, in the last day a note.
    if (has("deviceUpTime")) {
        const qint64 up = num("deviceUpTime");
        r.upShort = QStringLiteral("Up %1").arg(durationText(up));
        r.upCell = shortDuration(up);
        if (up < 3600) {
            r.upState = QStringLiteral("warning");
            add(r.upState, QStringLiteral("Uptime"), QStringLiteral("restarted %1 ago").arg(durationText(up)), QStringLiteral("Restarted %1 ago").arg(durationText(up)), r.upCell);
        } else if (up < 86400) {
            r.upState = QStringLiteral("note");
            add(r.upState, QStringLiteral("Uptime"), QStringLiteral("up %1: it restarted within the last day").arg(durationText(up)), QString(), r.upCell);
        } else {
            r.upState = QStringLiteral("ok");
            add(r.upState, QStringLiteral("Uptime"), QStringLiteral("up %1 since its last restart").arg(durationText(up)), QString(), r.upCell);
        }
    }
    if (has("cpuUtilization")) {
        const qint64 cpu = num("cpuUtilization");
        r.loadShort = QStringLiteral("CPU %1%").arg(cpu);
        r.cpuCell = QStringLiteral("%1%").arg(cpu);
        const QString detail = QStringLiteral("%1% busy at this moment (it varies from second to second)").arg(cpu);
        r.cpuState = cpu >= 95 ? QStringLiteral("warning") : QStringLiteral("ok");
        add(r.cpuState, QStringLiteral("Processor"), detail, cpu >= 95 ? QStringLiteral("CPU very busy") : QString(), r.cpuCell);
    }
    if (has("memoryUsage")) {
        const qint64 mem = num("memoryUsage");
        const qint64 freeKb = num("memoryAvailable");
        r.loadShort += QStringLiteral("%1memory %2%").arg(r.loadShort.isEmpty() ? QString() : QStringLiteral(", ")).arg(mem);
        r.memCell = has("memoryAvailable") ? QStringLiteral("%1% (%2 MB)").arg(mem).arg(freeKb / 1024) : QStringLiteral("%1%").arg(mem);
        const QString detail = QStringLiteral("%1% used%2").arg(mem).arg(has("memoryAvailable") ? QStringLiteral(", %1 MB free").arg(freeKb / 1024) : QString());
        const bool full = mem >= 97 || (has("memoryAvailable") && freeKb < 12288);
        r.memState = full ? QStringLiteral("warning") : QStringLiteral("ok");
        add(r.memState, QStringLiteral("Memory"), full ? detail + QStringLiteral(" (nearly full)") : detail, full ? QStringLiteral("Memory nearly full") : QString(), r.memCell);
    }

    // Restarts and running time, only for a camera that keeps them (most fixed cameras read 0).
    const qint64 reboots = num("totalRebootCount");
    const qint64 runMin = num("cameraRunTotalTime");
    if (has("totalRebootCount") && (reboots > 0 || runMin > 0)) {
        add(QStringLiteral("ok"), QStringLiteral("Restarts and running time"),
            QStringLiteral("%1 restart(s) in its life, running %2 in total (the camera counts it in minutes: %3)")
                .arg(reboots).arg(minutesSpanText(runMin), groupedNumber(runMin)));
    } else if (has("totalRebootCount")) {
        add(QStringLiteral("note"), QStringLiteral("Restarts and running time"), QStringLiteral("this camera does not keep a restart count or running time (it reads 0)"));
    }

    // Wear counters: a lens that moves (zoom, focus, iris, pan and tilt) and the day/night filter. Shown only when something counted.
    QStringList wear;
    const struct { const char *tag; const char *text; } parts[] = {
        {"zoomReverseTimes", "zoom reversals"}, {"focusReverseTimes", "focus reversals"}, {"irisShiftTimes", "iris shifts"},
        {"icrShiftTimes", "day/night filter shifts"}, {"panTotalRounds", "pan rounds"}, {"tiltTotalRounds", "tilt rounds"}};
    for (const auto &p : parts) {
        if (num(p.tag) > 0)
            wear << QStringLiteral("%1 %2").arg(groupedNumber(num(p.tag)), QLatin1String(p.text));
    }
    if (!wear.isEmpty())
        add(QStringLiteral("ok"), QStringLiteral("Moving parts"), wear.join(QStringLiteral(", ")));

    // How long it has run in each temperature range (same unit as the running time), only when it counted any.
    const qint64 cold = num("runTimeUnderNegativetwenty"), mild = num("runTimeBetweenNtwentyPforty"), hot = num("runtimeOverPositiveforty");
    if (cold + mild + hot > 0) {
        add(QStringLiteral("ok"), QStringLiteral("Temperature range"),
            QStringLiteral("below -20 C: %1, -20 to 40 C: %2, above 40 C: %3").arg(minutesSpanText(cold), minutesSpanText(mild), minutesSpanText(hot)));
    }
    if (num("fanState") == 1)
        add(QStringLiteral("note"), QStringLiteral("Fan"), QStringLiteral("the camera's fan is running"), QStringLiteral("Fan running"));
    if (num("heatState") == 1)
        add(QStringLiteral("note"), QStringLiteral("Heater"), QStringLiteral("the camera's heater is on"), QStringLiteral("Heater on"));
    return r;
}

} // namespace CameraStatusText

#endif // CAMERASTATUSTEXT_H
