#ifndef EVENTALERT_H
#define EVENTALERT_H

#include <QDateTime>
#include <QSet>
#include <QString>
#include <QVector>

// One <EventNotificationAlert> inside a detection-stream buffer.
// start and end are indexes in the QString the caller built with an explicit length,
// so a zero byte inside a picture does not hide the next alert.
struct EventAlertSpan {
    int start = 0;
    int end = 0;
    QString body;
};

QVector<EventAlertSpan> splitEventAlerts(const QString &text);

// True when the camera's timestamp is at least 10 seconds behind `now`.
// An invalid timestamp is not late.
bool eventReportIsLate(const QString &cameraTime, const QDateTime &now);

// What one alert asks the tile and the log to do. Logging, pictures and the quiet-period
// for plain motion stay with the caller. This only reads the XML.
struct EventAlert {
    enum Action { None, Face, Other, Smart, PlainMotion, LabelledMotion };
    enum Frame { NoFrame, Keep, Flash };

    Action action = None;
    Frame frame = NoFrame;
    bool active = false;
    QString type;
    QString target;
    QString logKind;
    QString shortLabel;
    QString cameraTime;
    QString state;
    int channel = 0;
};

EventAlert classifyEventAlert(const QString &body, const QSet<QString> &kinds, const QSet<QString> &targets,
                              bool frameMotion, const QDateTime &now);

#endif
