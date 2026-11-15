#include "hikvisionbackup.h"

#include "hikvisionmanager.h"
#include "offlinerecorders.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QThread>

namespace {
// The key every export uses. Never change it: a backup can only be read back with the key that made it.
const char *kBackupKey = "KVisionBackup1";
const char *kLoginGroup = "CameraLogins";

QString hostOf(const QString &address)
{
    return address.section(QLatin1Char(':'), 0, 0).trimmed();
}

QString safeName(const QString &s)
{
    QString r = s;
    r.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    return r.left(60);
}

// GET a URL with curl (the same way the rest of the program talks to cameras). The body goes to outFile when
// given, otherwise it is returned in *body. Returns the HTTP status (0 when curl could not connect at all).
int curlGet(const QString &url, const QString &user, const QString &pass, int maxSeconds, const QString &outFile,
            QByteArray *body)
{
    QStringList args;
    args << QStringLiteral("-s") << QStringLiteral("-k") << QStringLiteral("--anyauth") << QStringLiteral("-u")
         << QStringLiteral("%1:%2").arg(user, pass) << QStringLiteral("--max-time") << QString::number(maxSeconds)
         << QStringLiteral("-w") << QStringLiteral("%{http_code}");
    if (!outFile.isEmpty())
        args << QStringLiteral("-o") << outFile;
    args << url;
    QProcess p;
    p.start(QStringLiteral("curl"), args);
    if (!p.waitForFinished((maxSeconds + 5) * 1000)) {
        p.kill();
        return 0;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (outFile.isEmpty()) {
        // The status code is the last three characters; everything before it is the body.
        if (body)
            *body = out.left(qMax(0, int(out.size()) - 3));
        return out.right(3).toInt();
    }
    return out.trimmed().toInt();
}

// PUT a small JSON text to a camera with curl. Returns the HTTP status (0 when it could not connect).
int curlPutJson(const QString &url, const QString &user, const QString &pass, int maxSeconds, const QByteArray &json,
                QByteArray *body, const QString &contentType = QStringLiteral("application/json"))
{
    QStringList args;
    args << QStringLiteral("-s") << QStringLiteral("-k") << QStringLiteral("--anyauth") << QStringLiteral("-u")
         << QStringLiteral("%1:%2").arg(user, pass) << QStringLiteral("--max-time") << QString::number(maxSeconds)
         << QStringLiteral("-X") << QStringLiteral("PUT") << QStringLiteral("-d") << QString::fromUtf8(json)
         << QStringLiteral("-H") << QStringLiteral("Content-Type: %1").arg(contentType)
         << QStringLiteral("-w") << QStringLiteral("%{http_code}") << url;
    QProcess p;
    p.start(QStringLiteral("curl"), args);
    if (!p.waitForFinished((maxSeconds + 5) * 1000)) {
        p.kill();
        return 0;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (body)
        *body = out.left(qMax(0, int(out.size()) - 3));
    return out.right(3).toInt();
}

struct RuleLinesAnswer {
    QString state;   // "on", "off", "unavailable" or "error"
    QString message;
    bool refused = false; // the camera refused the login
};

// The camera's own switch for drawing the VCA rule lines and targets on the video stream (the web page's
// VCA > Smart Event > General Settings > Metadata > "Enable Stream Rule" and "Sub-Stream", which work as a pair).
// Read: GET /ISAPI/Streaming/channels/1/smartOverlap?format=json
//   {"SmartOverlap": {"enabled": bool, "streamType": 0 or 2, "bkgImageOverlapEnabled": bool}}
// On is enabled=true with streamType=2; off is enabled=false with streamType=0. bkgImageOverlapEnabled is a different
// switch (the rule frame on the picture a camera saves) and is always written back exactly as it was read.
// mode: 0 only read, 1 turn on, 2 turn off, 3 turn it the other way (read first). A refused login is never retried.
RuleLinesAnswer ruleLinesOne(const HikvisionBackup::Cam &cam, int mode)
{
    RuleLinesAnswer a;
    a.state = QStringLiteral("error");
    if (cam.user.isEmpty() || cam.pass.isEmpty()) {
        a.message = QStringLiteral("no login saved for this camera (a blank password is never sent)");
        return a;
    }
    const QString url = QStringLiteral("http://%1:%2/ISAPI/Streaming/channels/1/smartOverlap?format=json").arg(cam.ip).arg(cam.httpPort);
    auto explain = [&](int code) -> bool {
        if (code == 0)
            a.message = QStringLiteral("could not connect");
        else if (code == 401) {
            a.refused = true;
            a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
        }
        else if (code == 404 || code == 403 || code == 501) {
            a.state = QStringLiteral("unavailable");
            a.message = QStringLiteral("this camera has no switch for rule lines on the stream");
        } else
            a.message = QStringLiteral("the camera answered HTTP %1").arg(code);
        return code != 200;
    };
    auto readState = [&](QJsonObject *so, QString *why) -> bool {
        QByteArray body;
        const int code = curlGet(url, cam.user, cam.pass, 8, QString(), &body);
        if (explain(code)) {
            *why = a.message;
            return false;
        }
        const QJsonObject o = QJsonDocument::fromJson(body).object().value(QStringLiteral("SmartOverlap")).toObject();
        if (!o.contains(QStringLiteral("enabled"))) {
            a.state = QStringLiteral("error");
            *why = QStringLiteral("the camera's answer was not understood");
            return false;
        }
        *so = o;
        return true;
    };

    QJsonObject so;
    QString why;
    if (!readState(&so, &why)) {
        a.message = why;
        return a;
    }
    // Some cameras (panoramic and dual-lens ones, for example) answer a read but list the setting as "notSupport" in their
    // capabilities and refuse any change. They have no such switch, so say that instead of trying to change it.
    {
        QByteArray capBody;
        const int capCode = curlGet(QStringLiteral("http://%1:%2/ISAPI/Streaming/channels/1/smartOverlap/capabilities?format=json")
                                        .arg(cam.ip).arg(cam.httpPort), cam.user, cam.pass, 8, QString(), &capBody);
        if (capCode == 403 || capCode == 404 || capCode == 501
            || (capCode == 200 && !QJsonDocument::fromJson(capBody).object().contains(QStringLiteral("SmartOverlapCap")))) {
            a.state = QStringLiteral("unavailable");
            a.message = QStringLiteral("this camera has no switch for rule lines on the stream");
            return a;
        }
    }
    const bool nowOn = so.value(QStringLiteral("enabled")).toBool();
    auto stateText = [](bool on) { return on ? QStringLiteral("on") : QStringLiteral("off"); };
    if (mode == 0) {
        a.state = stateText(nowOn);
        a.message = QStringLiteral("rule lines on the stream are %1").arg(a.state);
        return a;
    }
    if (mode == 3)
        mode = nowOn ? 2 : 1;
    const bool want = (mode == 1);
    const int wantType = want ? 2 : 0;
    if (nowOn == want && so.value(QStringLiteral("streamType")).toInt() == wantType) {
        a.state = stateText(want);
        a.message = QStringLiteral("already %1, nothing changed").arg(a.state);
        return a;
    }
    QJsonObject out = so; // every other field goes back as it was read
    out.insert(QStringLiteral("enabled"), want);
    out.insert(QStringLiteral("streamType"), wantType);
    QByteArray reply;
    const int putCode = curlPutJson(url, cam.user, cam.pass, 10,
                                    QJsonDocument(QJsonObject{{QStringLiteral("SmartOverlap"), out}}).toJson(QJsonDocument::Compact), &reply);
    if (putCode != 200 || QJsonDocument::fromJson(reply).object().value(QStringLiteral("statusCode")).toInt() != 1) {
        a.state = QStringLiteral("error");
        a.message = putCode == 200 ? QStringLiteral("the camera did not accept the change")
                                   : (explain(putCode), a.message);
        return a;
    }
    // Read it back: only report success for what the camera now says.
    QJsonObject after;
    if (!readState(&after, &why)) {
        a.state = QStringLiteral("error");
        a.message = QStringLiteral("changed, but could not read it back: %1").arg(why);
        return a;
    }
    const bool isOn = after.value(QStringLiteral("enabled")).toBool();
    a.state = stateText(isOn);
    a.message = isOn == want ? QStringLiteral("rule lines on the stream are now %1").arg(a.state)
                             : QStringLiteral("the camera still says %1").arg(a.state);
    return a;
}

// The camera's "Environmental Noise Filtering" switch (web page: Configuration > Video/Audio > Audio). It is the
// <noisereduce> field of the two-way audio channel, so it sits next to the talk-back settings but it cleans the live
// sound as well (heard on a real camera). There are no levels on the camera: the 0-5 levels in Hik-Connect are a
// filter in that app's own player.
// Read: GET /ISAPI/System/TwoWayAudio/channels/1 (XML). Change: the same text goes back with only the value of
// <noisereduce> replaced, so every other setting is sent exactly as the camera gave it.
// mode: 0 only read, 1 turn on, 2 turn off, 3 turn it the other way (read first). A refused login is never retried.
RuleLinesAnswer noiseFilterOne(const HikvisionBackup::Cam &cam, int mode)
{
    RuleLinesAnswer a;
    a.state = QStringLiteral("error");
    if (cam.user.isEmpty() || cam.pass.isEmpty()) {
        a.message = QStringLiteral("no login saved for this camera (a blank password is never sent)");
        return a;
    }
    const QString url = QStringLiteral("http://%1:%2/ISAPI/System/TwoWayAudio/channels/1").arg(cam.ip).arg(cam.httpPort);
    auto explain = [&](int code) -> bool {
        if (code == 0)
            a.message = QStringLiteral("could not connect");
        else if (code == 401) {
            a.refused = true;
            a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
        } else if (code == 404 || code == 403 || code == 501) {
            a.state = QStringLiteral("unavailable");
            a.message = QStringLiteral("this camera has no noise filter switch");
        } else
            a.message = QStringLiteral("the camera answered HTTP %1").arg(code);
        return code != 200;
    };
    const QRegularExpression rx(QStringLiteral("<noisereduce>\\s*(true|false)\\s*</noisereduce>"));
    auto readState = [&](QString *xml, bool *on, QString *why) -> bool {
        QByteArray body;
        const int code = curlGet(url, cam.user, cam.pass, 8, QString(), &body);
        if (explain(code)) {
            *why = a.message;
            return false;
        }
        const QString text = QString::fromUtf8(body);
        const QRegularExpressionMatch m = rx.match(text);
        if (!m.hasMatch()) {
            a.state = QStringLiteral("unavailable");
            *why = QStringLiteral("this camera has no noise filter switch");
            return false;
        }
        *xml = text;
        *on = m.captured(1) == QLatin1String("true");
        return true;
    };

    QString xml, why;
    bool nowOn = false;
    if (!readState(&xml, &nowOn, &why)) {
        a.message = why;
        return a;
    }
    auto stateText = [](bool on) { return on ? QStringLiteral("on") : QStringLiteral("off"); };
    if (mode == 0) {
        a.state = stateText(nowOn);
        a.message = QStringLiteral("noise filter is %1").arg(a.state);
        return a;
    }
    if (mode == 3)
        mode = nowOn ? 2 : 1;
    const bool want = (mode == 1);
    if (nowOn == want) {
        a.state = stateText(want);
        a.message = QStringLiteral("already %1, nothing changed").arg(a.state);
        return a;
    }
    QString out = xml;
    out.replace(rx, QStringLiteral("<noisereduce>%1</noisereduce>").arg(want ? QStringLiteral("true") : QStringLiteral("false")));
    QByteArray reply;
    const int putCode = curlPutJson(url, cam.user, cam.pass, 10, out.toUtf8(), &reply, QStringLiteral("application/xml"));
    if (putCode != 200 || !QString::fromUtf8(reply).contains(QStringLiteral("<statusCode>1</statusCode>"))) {
        a.state = QStringLiteral("error");
        a.message = putCode == 200 ? QStringLiteral("the camera did not accept the change") : (explain(putCode), a.message);
        return a;
    }
    // Read it back: only report success for what the camera now says.
    QString xml2;
    bool isOn = false;
    if (!readState(&xml2, &isOn, &why)) {
        a.state = QStringLiteral("error");
        a.message = QStringLiteral("changed, but could not read it back: %1").arg(why);
        return a;
    }
    a.state = stateText(isOn);
    a.message = isOn == want ? QStringLiteral("noise filter is now %1").arg(a.state)
                             : QStringLiteral("the camera still says %1").arg(a.state);
    return a;
}

// The camera's built-in flashing light (the web page's white light alarm). The camera has it when
//   GET /ISAPI/Event/triggers/notifications/whiteLightAlarm/capabilities?format=json
// answers with a "WhiteLightAlarmCap". Start and stop (checked on a real camera, where it flashed):
//   PUT /ISAPI/Event/triggers/notifications/whiteLightAlarm/trigger?format=json
//   {"WhiteLightAlarmTrigger":{"triggerType":"start"}} or "stop"
// The camera stops the flashing by itself after its own duration (1 to 60 seconds, set on the camera), so there is no
// stop command here. mode: 0 only check that the camera has it, 1 start. A refused login is never retried.
RuleLinesAnswer strobeOne(const HikvisionBackup::Cam &cam, int mode, bool siren = false)
{
    RuleLinesAnswer a;
    a.state = QStringLiteral("error");
    if (cam.user.isEmpty() || cam.pass.isEmpty()) {
        a.message = QStringLiteral("no login saved for this camera (a blank password is never sent)");
        return a;
    }
    // siren: the camera's warning message (web page: Event > Audio Alarm) instead of the flashing light. Same shape:
    //   GET  .../notifications/AudioAlarm/capabilities?format=json   has "AudioAlarmCap" when the camera can play it
    //   PUT  .../notifications/AudioAlarm/trigger?format=json         {"AudioAlarmTrigger":{"triggerType":"start"}}
    // (the body is the one Hik-Connect sends). It plays the sound, volume and number of times already set on the camera.
    const QString base = QStringLiteral("http://%1:%2/ISAPI/Event/triggers/notifications/%3").arg(cam.ip).arg(cam.httpPort)
                             .arg(siren ? QStringLiteral("AudioAlarm") : QStringLiteral("whiteLightAlarm"));
    const QString capKey = siren ? QStringLiteral("AudioAlarmCap") : QStringLiteral("WhiteLightAlarmCap");
    const QString trigKey = siren ? QStringLiteral("AudioAlarmTrigger") : QStringLiteral("WhiteLightAlarmTrigger");
    const QString what = siren ? QStringLiteral("warning message") : QStringLiteral("flashing light");
    QByteArray body;
    const int code = curlGet(base + QStringLiteral("/capabilities?format=json"), cam.user, cam.pass, 8, QString(), &body);
    if (code == 0) {
        a.message = QStringLiteral("could not connect");
        return a;
    }
    if (code == 401) {
        a.refused = true;
        a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
        return a;
    }
    if (code != 200 || !QJsonDocument::fromJson(body).object().contains(capKey)) {
        a.state = QStringLiteral("unavailable");
        a.message = QStringLiteral("this camera has no %1").arg(what);
        return a;
    }
    if (mode == 0) {
        a.state = QStringLiteral("available");
        a.message = QStringLiteral("this camera has a %1").arg(what);
        return a;
    }
    // The web page's own "Test" button plays the camera's saved sound with
    //   PUT .../AudioAlarm/<id>/test?format=json   (no body)
    // where <id> is the chosen alert sound (audioClass "alertAudio": alertAudioID), 12 for a prompt sound, or 12 plus
    // the custom sound number (15 when that is above 3). Cameras that list isSupportAudioTest answer this call even
    // when they ignore the app's AudioAlarm/trigger (seen on a DS-2CD2387G3). The trigger call below is the fallback.
    if (siren) {
        const QJsonObject cap = QJsonDocument::fromJson(body).object().value(QStringLiteral("AudioAlarmCap")).toObject();
        if (cap.value(QStringLiteral("isSupportAudioTest")).toBool()) {
            QByteArray setBody;
            const int sc = curlGet(base + QStringLiteral("?format=json"), cam.user, cam.pass, 8, QString(), &setBody);
            if (sc == 401) {
                a.refused = true;
                a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
                return a;
            }
            const QJsonObject s = QJsonDocument::fromJson(setBody).object().value(QStringLiteral("AudioAlarm")).toObject();
            if (sc == 200 && !s.isEmpty()) {
                const QString cls = s.value(QStringLiteral("audioClass")).toString();
                int id = s.value(QStringLiteral("audioID")).toInt(0);
                if (cls == QLatin1String("alertAudio"))
                    id = s.value(QStringLiteral("alertAudioID")).toInt(id);
                else if (cls == QLatin1String("promptAudio"))
                    id = 12;
                else if (cls == QLatin1String("customAudio")) {
                    const int n = s.value(QStringLiteral("customAudioID")).toInt(0);
                    id = n > 3 ? 15 : 12 + n;
                }
                if (id > 0) {
                    QByteArray testReply;
                    const int tc = curlPutJson(base + QStringLiteral("/%1/test?format=json").arg(id), cam.user, cam.pass, 10,
                                               QByteArray(), &testReply);
                    if (tc == 401) {
                        a.refused = true;
                        a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
                        return a;
                    }
                    if (tc == 200 && QJsonDocument::fromJson(testReply).object().value(QStringLiteral("statusCode")).toInt() == 1) {
                        a.state = QStringLiteral("started");
                        a.message = QStringLiteral("warning message started (the camera plays the sound set on the camera)");
                        return a;
                    }
                }
            }
        }
    }
    const QJsonObject trig{{trigKey, QJsonObject{{QStringLiteral("triggerType"), QStringLiteral("start")}}}};
    QByteArray reply;
    const int putCode = curlPutJson(base + QStringLiteral("/trigger?format=json"), cam.user, cam.pass, 10,
                                    QJsonDocument(trig).toJson(QJsonDocument::Compact), &reply);
    if (putCode == 401) {
        a.refused = true;
        a.message = QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)");
        return a;
    }
    if (putCode != 200 || QJsonDocument::fromJson(reply).object().value(QStringLiteral("statusCode")).toInt() != 1) {
        a.message = putCode == 200 ? QStringLiteral("the camera did not accept the command")
                                   : QStringLiteral("the camera answered HTTP %1").arg(putCode);
        return a;
    }
    a.state = QStringLiteral("started");
    a.message = siren ? QStringLiteral("warning message started (the camera plays the sound set on the camera)")
                      : QStringLiteral("flashing light command accepted by the camera (it stops by itself after its own time)");
    return a;
}

QString tag(const QString &xml, const QString &name)
{
    const QRegularExpression rx(QStringLiteral("<%1>([^<]*)</%1>").arg(name));
    return rx.match(xml).captured(1).trimmed();
}

struct HealthAnswer {
    bool refused = false;
    QString message;      // the lines as plain text, for the log
    QVariantMap result;   // one card in the shape the setup check's list draws (see healthCard)
};

const char *kNewestFirmwareGroup = "NewestFirmware";
// The day each note was typed in and saved (ISO text), under the same key. A note saved before this existed has no day.
const char *kNewestFirmwareDateGroup = "NewestFirmwareDate";

// "14 Nov 2026" from an ISO day (always English, like the rest of the check's text), or "" when there is none.
QString niceDay(const QString &iso)
{
    const QDate d = QDate::fromString(iso, Qt::ISODate);
    return d.isValid() ? QLocale::c().toString(d, QStringLiteral("d MMM yyyy")) : QString();
}

// The model name has a "/" in it (DS-2CD2387G3-LIS2UY/SL), which QSettings would read as a sub-group.
QString newestKey(const QString &model)
{
    QString k = model.trimmed();
    k.replace(QLatin1Char('/'), QLatin1Char('|'));
    return k;
}

// The first dotted version in a text ("V5.8.40" or "V5.8.40 build 260911" both give 5, 8, 40), or nothing.
QList<int> versionParts(const QString &text)
{
    QList<int> parts;
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("(\\d+(?:\\.\\d+)+)")).match(text);
    if (!m.hasMatch())
        return parts;
    for (const QString &p : m.captured(1).split(QLatin1Char('.')))
        parts << p.toInt();
    return parts;
}

// -1 when a is older than b, 0 when equal, 1 when newer, 2 when either has no dotted version.
int compareVersions(const QString &a, const QString &b)
{
    QList<int> x = versionParts(a), y = versionParts(b);
    if (x.isEmpty() || y.isEmpty())
        return 2;
    while (x.size() < y.size())
        x << 0;
    while (y.size() < x.size())
        y << 0;
    for (int i = 0; i < x.size(); ++i) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

QVariantMap healthItem(const QString &state, const QString &label, const QString &detail)
{
    return QVariantMap{{QStringLiteral("state"), state}, {QStringLiteral("label"), label}, {QStringLiteral("detail"), detail}};
}

// One card for SetupCheckList.qml (the same shape the detection setup check gives): key, name, problems, warnings, summary,
// time, firmware, camIp, camPort and the list of items (state ok / problem / warning / note, label, detail). shorts are the
// few words for each thing that needs a look; they become the card's summary.
QVariantMap healthCard(const HikvisionBackup::Cam &cam, const QVariantList &items, const QStringList &shorts,
                       const QString &model, const QString &firmware)
{
    int problems = 0, warnings = 0;
    for (const QVariant &v : items) {
        const QString s = v.toMap().value(QStringLiteral("state")).toString();
        if (s == QLatin1String("problem"))
            ++problems;
        else if (s == QLatin1String("warning"))
            ++warnings;
    }
    QVariantMap r;
    r.insert(QStringLiteral("key"), cam.ip);
    r.insert(QStringLiteral("name"), cam.name.isEmpty() ? cam.ip : cam.name);
    r.insert(QStringLiteral("camIp"), cam.ip);
    r.insert(QStringLiteral("camPort"), cam.httpPort);
    r.insert(QStringLiteral("problems"), problems);
    r.insert(QStringLiteral("warnings"), warnings);
    r.insert(QStringLiteral("summary"), shorts.isEmpty() ? QStringLiteral("Clock fine") : shorts.join(QStringLiteral(", ")));
    r.insert(QStringLiteral("time"), QLocale::c().toString(QDateTime::currentDateTime(), QStringLiteral("d MMM yyyy hh:mm:ss")));
    r.insert(QStringLiteral("firmware"), firmware);
    r.insert(QStringLiteral("model"), model);
    r.insert(QStringLiteral("items"), items);
    return r;
}

// Read only. The camera's clock against this computer's, and its firmware:
//   GET /ISAPI/System/time        <localTime>2026-11-14T22:41:09-09:00</localTime> <timeMode>NTP|manual</timeMode>
//   GET /ISAPI/System/deviceInfo  <model> <firmwareVersion> <firmwareReleasedDate>
//   GET /ISAPI/System/time/ntpServers  (only to name the time server when the camera uses NTP)
// A clock that is more than two minutes off is "warn": it makes event times wrong and can break the login of some
// cameras. Nothing is ever written. A refused login is never retried.
HealthAnswer healthOne(const HikvisionBackup::Cam &cam, const QMap<QString, QString> &newest, const QMap<QString, QString> &newestDay)
{
    HealthAnswer a;
    // A card with a single line for a camera that could not be read at all.
    auto bail = [&](const QString &itemState, const QString &detail, const QString &shortText) {
        a.message = detail;
        a.result = healthCard(cam, {healthItem(itemState, QStringLiteral("Clock and firmware"), detail)}, {shortText}, QString(), QString());
        return a;
    };
    if (cam.user.isEmpty() || cam.pass.isEmpty())
        return bail(QStringLiteral("problem"), QStringLiteral("no login saved for this camera (a blank password is never sent)"), QStringLiteral("No login saved"));
    const QString base = QStringLiteral("http://%1:%2/ISAPI/System/").arg(cam.ip).arg(cam.httpPort);
    auto field = [](const QString &xml, const char *name) {
        const QRegularExpression rx(QStringLiteral("<%1>([^<]*)</%1>").arg(QLatin1String(name)));
        return rx.match(xml).captured(1).trimmed();
    };
    QByteArray timeBody;
    const QDateTime before = QDateTime::currentDateTimeUtc();
    const int tc = curlGet(base + QStringLiteral("time"), cam.user, cam.pass, 8, QString(), &timeBody);
    const QDateTime after = QDateTime::currentDateTimeUtc();
    if (tc == 0)
        return bail(QStringLiteral("problem"), QStringLiteral("could not connect"), QStringLiteral("Could not connect"));
    if (tc == 401) {
        a.refused = true;
        return bail(QStringLiteral("problem"), QStringLiteral("the camera refused the login (not tried again, so it cannot lock the camera)"),
                    QStringLiteral("Login refused"));
    }
    if (tc != 200) {
        const bool none = (tc == 403 || tc == 404 || tc == 501);
        return bail(none ? QStringLiteral("warning") : QStringLiteral("problem"),
                    QStringLiteral("the camera answered HTTP %1 for its clock").arg(tc),
                    none ? QStringLiteral("No clock to read") : QStringLiteral("Could not be read"));
    }
    const QString timeXml = QString::fromUtf8(timeBody);
    const QDateTime camTime = QDateTime::fromString(field(timeXml, "localTime"), Qt::ISODate);
    const QString mode = field(timeXml, "timeMode");
    QVariantList items;
    QStringList shorts;
    QStringList plain;
    QString okShort; // the card's summary when nothing needs a look: says what the firmware was compared with
    const QString checkedOn = QLocale::c().toString(QDate::currentDate(), QStringLiteral("d MMM yyyy"));
    auto add = [&](const QString &state, const QString &label, const QString &detail, const QString &shortText = QString()) {
        items << healthItem(state, label, detail);
        plain << QStringLiteral("%1: %2").arg(label, detail);
        if (!shortText.isEmpty())
            shorts << shortText;
    };
    if (!camTime.isValid()) {
        add(QStringLiteral("warning"), QStringLiteral("Clock"), QStringLiteral("the camera's time was not understood"), QStringLiteral("Clock not understood"));
    } else {
        // The camera answered about halfway through the request, so compare with the middle of it.
        const QDateTime mid = before.addMSecs(before.msecsTo(after) / 2);
        const qint64 diff = mid.secsTo(camTime.toUTC());
        const qint64 ad = diff < 0 ? -diff : diff;
        bool off = false;
        QString how;
        if (ad < 5)
            how = QStringLiteral("matches this computer");
        else if (ad < 120)
            how = QStringLiteral("%1 seconds %2 this computer").arg(ad).arg(diff > 0 ? QStringLiteral("ahead of") : QStringLiteral("behind"));
        else {
            const qint64 mins = ad / 60;
            const QString amount = mins >= 120 ? QStringLiteral("%1 hours").arg(mins / 60) : QStringLiteral("%1 minutes").arg(mins);
            how = QStringLiteral("%1 %2 this computer (more than 2 minutes off)")
                      .arg(amount, diff > 0 ? QStringLiteral("ahead of") : QStringLiteral("behind"));
            off = true;
        }
        QString src = mode == QLatin1String("NTP") ? QStringLiteral("NTP") : (mode.isEmpty() ? QStringLiteral("unknown source") : mode);
        if (mode == QLatin1String("NTP")) {
            QByteArray ntp;
            if (curlGet(base + QStringLiteral("time/ntpServers"), cam.user, cam.pass, 6, QString(), &ntp) == 200) {
                const QString x = QString::fromUtf8(ntp);
                const QString host = !field(x, "hostName").isEmpty() ? field(x, "hostName") : field(x, "ipAddress");
                if (!host.isEmpty())
                    src = QStringLiteral("NTP: %1").arg(host);
            }
        } else if (mode == QLatin1String("manual")) {
            src = QStringLiteral("set by hand, so it drifts");
        }
        add(off ? QStringLiteral("warning") : QStringLiteral("ok"), QStringLiteral("Clock"),
            QStringLiteral("%1, %2 (%3)").arg(camTime.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")), how, src),
            off ? QStringLiteral("Clock is off") : QString());
    }
    QString model, fw;
    QByteArray infoBody;
    const int ic = curlGet(base + QStringLiteral("deviceInfo"), cam.user, cam.pass, 8, QString(), &infoBody);
    if (ic == 200) {
        const QString x = QString::fromUtf8(infoBody);
        fw = field(x, "firmwareVersion");
        model = field(x, "model");
        const QString built = field(x, "firmwareReleasedDate");
        add(QStringLiteral("ok"), QStringLiteral("Firmware"),
            QStringLiteral("%1%2%3").arg(fw.isEmpty() ? QStringLiteral("not given") : fw)
                .arg(built.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(built))
                .arg(model.isEmpty() ? QString() : QStringLiteral(", model %1").arg(model)));
        // The newest version for this model is typed in by the user (from Hikvision's own page): the cameras cannot say
        // and Hikvision's site cannot be read by a program. So this says "may be", never "is". No note at all is a
        // "problem" (red): the firmware cannot be judged.
        if (!model.isEmpty() && !fw.isEmpty()) {
            const QString note = newest.value(newestKey(model));
            const QString label = QStringLiteral("Newest firmware");
            const QString noted = niceDay(newestDay.value(newestKey(model)));
            // What the note is: typed in by hand, and when. It is the only source, so say so.
            const QString how = noted.isEmpty() ? QStringLiteral("typed in by hand (day not recorded)")
                                                : QStringLiteral("typed in by hand on %1").arg(noted);
            if (note.isEmpty()) {
                add(QStringLiteral("problem"), label, QStringLiteral("none noted for this model yet (type it in Settings > Camera Tools)"),
                    QStringLiteral("No newest firmware noted"));
            } else {
                const int cmp = compareVersions(fw, note);
                if (cmp == -1) {
                    add(QStringLiteral("warning"), label,
                        QStringLiteral("update may be available: the newest noted for this model is %1 (%2), this camera has %3. Checked %4.").arg(note, how, fw, checkedOn),
                        QStringLiteral("Update may be available"));
                } else if (cmp == 0) {
                    add(QStringLiteral("ok"), label,
                        QStringLiteral("matches %1, the newest noted for this model (%2). That is your own note, not a check with Hikvision. Checked %3.").arg(note, how, checkedOn));
                    okShort = noted.isEmpty() ? QStringLiteral("Clock fine, firmware matches your note")
                                              : QStringLiteral("Clock fine, firmware matches your note of %1").arg(noted);
                } else if (cmp == 1) {
                    add(QStringLiteral("note"), label,
                        QStringLiteral("this camera is newer than the newest noted (%1, %2): update the note. Checked %3.").arg(note, how, checkedOn));
                    okShort = QStringLiteral("Clock fine, firmware newer than your note");
                } else {
                    add(QStringLiteral("note"), label,
                        QStringLiteral("the newest noted (%1, %2) could not be compared with %3. Checked %4.").arg(note, how, fw, checkedOn));
                    okShort = QStringLiteral("Clock fine, firmware not compared");
                }
            }
        }
    } else {
        add(QStringLiteral("warning"), QStringLiteral("Firmware"), QStringLiteral("could not be read (HTTP %1)").arg(ic), QStringLiteral("Firmware not read"));
    }
    if (shorts.isEmpty() && !okShort.isEmpty())
        shorts << okShort;
    a.message = plain.join(QLatin1Char('\n'));
    a.result = healthCard(cam, items, shorts, model, fw);
    return a;
}
}

HikvisionBackup::HikvisionBackup(QObject *parent) : QObject(parent) {}

HikvisionBackup::~HikvisionBackup()
{
    for (QThread *t : {m_worker, m_loader, m_ruleWorker}) {
        if (t) {
            t->disconnect(this);
            t->wait();
        }
    }
}

QString HikvisionBackup::backupDir() const
{
    const QString base = m_configPath.isEmpty() ? QSettings().fileName() : m_configPath;
    return QDir::fromNativeSeparators(QFileInfo(base).absolutePath()) + QStringLiteral("/backups");
}

void HikvisionBackup::loadCameras(const QString &recordersJson, const QString &configPath)
{
    m_configPath = configPath;
    // A recorder the user set Offline is not contacted at all, so its cameras are not listed.
    const QByteArray json = OfflineRecorders::withoutOffline(recordersJson, OfflineRecorders::readNames(configPath)).toUtf8();
    if (m_loader && m_loader->isRunning()) {
        m_hasPending = true; // the answer being worked out may already be out of date: run again when it is done
        m_pendingJson = json;
        m_pendingPath = configPath;
        return;
    }
    startLoad(json, configPath);
}

void HikvisionBackup::startLoad(const QByteArray &json, const QString &configPath)
{
    if (m_loader) {
        m_loader->wait();
        m_loader->deleteLater();
    }
    m_loader = QThread::create([this, json, configPath]() {
        QSettings saved(configPath, QSettings::IniFormat);
        saved.beginGroup(QLatin1String(kLoginGroup));
        auto login = [&](const QString &ip, QString &user, QString &pass) {
            const QJsonObject o = QJsonDocument::fromJson(saved.value(ip).toString().toUtf8()).object();
            if (o.value(QStringLiteral("user")).toString().isEmpty() || o.value(QStringLiteral("pass")).toString().isEmpty())
                return false;
            user = o.value(QStringLiteral("user")).toString();
            pass = o.value(QStringLiteral("pass")).toString();
            return true;
        };

        QMap<QString, Cam> byIp; // one entry per device, in address order
        const QJsonArray list = QJsonDocument::fromJson(json).array();
        for (const QJsonValue &rv : list) {
            const QJsonObject rec = rv.toObject();
            const QString recIp = hostOf(rec.value(QStringLiteral("ip")).toString());
            const QString recUser = rec.value(QStringLiteral("username")).toString();
            const QString recPass = rec.value(QStringLiteral("password")).toString();
            int http = rec.value(QStringLiteral("httpPort")).toVariant().toInt();
            if (http <= 0)
                http = 80;
            QString recName = rec.value(QStringLiteral("name")).toString();
            if (recName.isEmpty())
                recName = recIp;
            const QJsonArray cams = rec.value(QStringLiteral("cameras")).toArray();
            if (recIp.isEmpty())
                continue;

            auto add = [&](const QString &ip, int port, const QString &name) {
                if (ip.isEmpty() || byIp.contains(ip))
                    return;
                Cam c;
                c.name = name;
                c.ip = ip;
                c.httpPort = port;
                c.user = recUser;
                c.pass = recPass;
                c.ownLogin = login(ip, c.user, c.pass);
                byIp.insert(ip, c);
            };

            if (cams.size() <= 2) {
                add(recIp, http, recName); // a camera (or a dual-lens one): the recorder entry is the device
                continue;
            }
            // A recorder: each channel is a camera on its own address. The recorder is only asked for the list.
            const QHash<int, QVariantMap> proxy = HikvisionManager::fetchProxyChannels(recIp, http, recUser, recPass);
            for (const QJsonValue &cv : cams) {
                const QJsonObject cam = cv.toObject();
                const int ch = cam.value(QStringLiteral("channelId")).toVariant().toInt();
                const QString ip = proxy.value(ch).value(QStringLiteral("ip")).toString();
                QString nm = cam.value(QStringLiteral("customName")).toString();
                if (nm.isEmpty())
                    nm = cam.value(QStringLiteral("name")).toString();
                if (nm.isEmpty())
                    nm = QStringLiteral("Camera %1").arg(ch);
                add(ip, 80, QStringLiteral("%1 Ch.%2 %3").arg(recName).arg(ch).arg(nm));
            }
        }

        const QList<Cam> found = byIp.values();
        QVariantList out;
        for (const Cam &c : found) {
            out.append(QVariantMap{{QStringLiteral("name"), c.name},
                                   {QStringLiteral("ip"), c.ip},
                                   {QStringLiteral("login"), c.ownLogin ? QStringLiteral("own") : QStringLiteral("recorder")},
                                   {QStringLiteral("user"), c.user}});
        }
        QMetaObject::invokeMethod(this, [this, found, out]() {
            qInfo() << "[Backup] found" << found.size() << "camera(s)";
            m_cams = found;
            emit camerasLoaded(out);
        }, Qt::QueuedConnection);
    });
    connect(m_loader, &QThread::finished, this, [this]() {
        if (!m_hasPending)
            return;
        m_hasPending = false;
        startLoad(m_pendingJson, m_pendingPath);
    });
    m_loader->start();
}

// Backs up one camera. Never retries a login: a refused login is reported and left alone.
HikvisionBackup::Result HikvisionBackup::backupOne(const Cam &cam, const QString &dir)
{
    Result r;
    if (cam.user.isEmpty() || cam.pass.isEmpty()) {
        r.message = QStringLiteral("no login saved for this camera (a blank password is never sent)");
        return r;
    }
    const QString base = QStringLiteral("http://%1:%2").arg(cam.ip).arg(cam.httpPort);

    // 1. Who is this? Also the one place a refused login shows up, so it is asked only once.
    QByteArray info;
    const int infoCode = curlGet(base + QStringLiteral("/ISAPI/System/deviceInfo"), cam.user, cam.pass, 8, QString(), &info);
    if (infoCode == 401) {
        r.message = QStringLiteral("login refused: enter this camera's own username and password");
        return r;
    }
    if (infoCode == 0) {
        r.message = QStringLiteral("cannot reach the camera");
        return r;
    }
    const QString infoXml = QString::fromUtf8(info);
    const QString model = tag(infoXml, QStringLiteral("model"));
    const QString firmware = tag(infoXml, QStringLiteral("firmwareVersion"));
    const QString serial = tag(infoXml, QStringLiteral("serialNumber"));
    if (infoCode != 200 || model.isEmpty()) {
        r.kind = Result::Skipped;
        r.message = QStringLiteral("this camera does not report its details, so it cannot be backed up (HTTP %1)").arg(infoCode);
        return r;
    }

    // 2. Download into a temporary file, so a failed or partial download never looks like a backup.
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QDateTime now = QDateTime::currentDateTime();
    const QString stem = QStringLiteral("%1_%2_%3").arg(safeName(cam.name), cam.ip, now.toString(QStringLiteral("yyyy-MM-dd_HHmmss")));
    const QString tmp = dir + QLatin1Char('/') + stem + QStringLiteral(".cfg.part");
    const QString finalPath = dir + QLatin1Char('/') + stem + QStringLiteral(".cfg");
    const int code = curlGet(base + QStringLiteral("/ISAPI/System/configurationData?secretkey=") + QLatin1String(kBackupKey),
                             cam.user, cam.pass, 90, tmp, nullptr);
    QFile f(tmp);
    const qint64 size = f.size();
    if (code != 200 || size < 1024) {
        // A rejected request comes back as a small XML message instead of a file.
        QString why = QStringLiteral("the camera answered HTTP %1").arg(code);
        if (f.open(QIODevice::ReadOnly)) {
            const QString sub = tag(QString::fromUtf8(f.read(2048)), QStringLiteral("subStatusCode"));
            if (!sub.isEmpty())
                why += QStringLiteral(" (%1)").arg(sub);
            f.close();
        }
        QFile::remove(tmp);
        r.message = QStringLiteral("export failed: ") + why;
        return r;
    }
    if (!QFile::rename(tmp, finalPath)) {
        QFile::remove(tmp);
        r.message = QStringLiteral("could not save the file in ") + dir;
        return r;
    }
    QFile::setPermissions(finalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // 3. A small description next to it, so a backup can later be matched to the camera it came from.
    QFile meta(dir + QLatin1Char('/') + stem + QStringLiteral(".json"));
    if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QJsonObject o{{QStringLiteral("name"), cam.name}, {QStringLiteral("ip"), cam.ip}, {QStringLiteral("model"), model},
                            {QStringLiteral("firmware"), firmware}, {QStringLiteral("serial"), serial},
                            {QStringLiteral("file"), QFileInfo(finalPath).fileName()}, {QStringLiteral("bytes"), double(size)},
                            {QStringLiteral("saved"), now.toString(Qt::ISODate)}};
        meta.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        meta.close();
        QFile::setPermissions(meta.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    r.kind = Result::Saved;
    r.message = QStringLiteral("saved %1 KB").arg(size / 1024);
    return r;
}

void HikvisionBackup::backupCamera(const QString &ip, const QString &user, const QString &password)
{
    if (m_busy)
        return;
    for (const Cam &c : m_cams) {
        if (c.ip == ip) {
            start({c}, true, user.trimmed(), password);
            return;
        }
    }
    emit finished(QStringLiteral("That camera is not in the list. Refresh the list and try again."), false);
}

void HikvisionBackup::checkRuleLines(const QString &ip)
{
    runRuleLines(ip, 0);
}

void HikvisionBackup::setRuleLines(const QString &ip, bool on)
{
    runRuleLines(ip, on ? 1 : 2);
}

void HikvisionBackup::toggleRuleLines(const QString &ip)
{
    runRuleLines(ip, 3);
}

void HikvisionBackup::runRuleLines(const QString &ip, int mode)
{
    if (m_ruleBusy)
        return;
    Cam found;
    bool have = false;
    for (const Cam &c : m_cams) {
        if (c.ip == ip) {
            found = c;
            have = true;
            break;
        }
    }
    if (!have) {
        emit ruleLinesResult(ip, QStringLiteral("error"), QStringLiteral("That camera is not in the list. Refresh the list and try again."));
        return;
    }
    m_ruleBusy = true;
    emit ruleBusyChanged();
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    m_ruleWorker = QThread::create([this, found, mode]() {
        const RuleLinesAnswer a = ruleLinesOne(found, mode);
        QMetaObject::invokeMethod(this, [this, found, mode, a]() {
            qInfo() << "[RuleLines]" << found.ip << (mode == 0 ? "read:" : (mode == 1 ? "turn on:" : (mode == 2 ? "turn off:" : "switch over:"))) << a.state << "-" << a.message;
            m_ruleBusy = false;
            emit ruleBusyChanged();
            emit ruleLinesResult(found.ip, a.state, a.message);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

void HikvisionBackup::checkNoiseFilter(const QString &ip)
{
    runNoiseFilter(ip, 0);
}

void HikvisionBackup::toggleNoiseFilter(const QString &ip)
{
    runNoiseFilter(ip, 3);
}

void HikvisionBackup::runNoiseFilter(const QString &ip, int mode)
{
    if (m_ruleBusy)
        return;
    Cam found;
    bool have = false;
    for (const Cam &c : m_cams) {
        if (c.ip == ip) {
            found = c;
            have = true;
            break;
        }
    }
    if (!have) {
        emit noiseFilterResult(ip, QStringLiteral("error"), QStringLiteral("That camera is not in the list. Refresh the list and try again."));
        return;
    }
    m_ruleBusy = true; // shares the one-at-a-time flag with the rule lines, so the two never run together
    emit ruleBusyChanged();
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    m_ruleWorker = QThread::create([this, found, mode]() {
        const RuleLinesAnswer a = noiseFilterOne(found, mode);
        QMetaObject::invokeMethod(this, [this, found, mode, a]() {
            qInfo() << "[NoiseFilter]" << found.ip << (mode == 0 ? "read:" : "switch over:") << a.state << "-" << a.message;
            m_ruleBusy = false;
            emit ruleBusyChanged();
            emit noiseFilterResult(found.ip, a.state, a.message);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

void HikvisionBackup::checkStrobe(const QString &ip)
{
    runStrobe(ip, 0);
}

void HikvisionBackup::startStrobe(const QString &ip)
{
    runStrobe(ip, 1);
}

void HikvisionBackup::checkWarning(const QString &ip)
{
    runStrobe(ip, 0, true);
}

void HikvisionBackup::startWarning(const QString &ip)
{
    runStrobe(ip, 1, true);
}

void HikvisionBackup::runStrobe(const QString &ip, int mode, bool siren)
{
    if (m_ruleBusy)
        return;
    Cam found;
    bool have = false;
    for (const Cam &c : m_cams) {
        if (c.ip == ip) {
            found = c;
            have = true;
            break;
        }
    }
    if (!have) {
        const QString msg = QStringLiteral("That camera is not in the list. Refresh the list and try again.");
        if (siren)
            emit warningResult(ip, QStringLiteral("error"), msg);
        else
            emit strobeResult(ip, QStringLiteral("error"), msg);
        return;
    }
    m_ruleBusy = true; // shares the one-at-a-time flag with the other camera switches
    emit ruleBusyChanged();
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    m_ruleWorker = QThread::create([this, found, mode, siren]() {
        const RuleLinesAnswer a = strobeOne(found, mode, siren);
        QMetaObject::invokeMethod(this, [this, found, mode, siren, a]() {
            qInfo() << (siren ? "[Warning]" : "[Strobe]") << found.ip << (mode == 0 ? "check:" : "start:") << a.state << "-" << a.message;
            m_ruleBusy = false;
            emit ruleBusyChanged();
            if (siren)
                emit warningResult(found.ip, a.state, a.message);
            else
                emit strobeResult(found.ip, a.state, a.message);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

QMap<QString, QString> HikvisionBackup::readNewestFirmware() const
{
    QMap<QString, QString> out;
    if (m_configPath.isEmpty())
        return out;
    QSettings s(m_configPath, QSettings::IniFormat);
    s.beginGroup(QLatin1String(kNewestFirmwareGroup));
    for (const QString &k : s.childKeys())
        out.insert(k, s.value(k).toString().trimmed());
    return out;
}

QMap<QString, QString> HikvisionBackup::readNewestFirmwareDays() const
{
    QMap<QString, QString> out;
    if (m_configPath.isEmpty())
        return out;
    QSettings s(m_configPath, QSettings::IniFormat);
    s.beginGroup(QLatin1String(kNewestFirmwareDateGroup));
    for (const QString &k : s.childKeys())
        out.insert(k, s.value(k).toString().trimmed());
    return out;
}

QString HikvisionBackup::newestFirmwareDay(const QString &model) const
{
    return niceDay(readNewestFirmwareDays().value(newestKey(model)));
}

QString HikvisionBackup::newestFirmware(const QString &model) const
{
    return readNewestFirmware().value(newestKey(model));
}

void HikvisionBackup::setNewestFirmware(const QString &model, const QString &version)
{
    if (m_configPath.isEmpty() || model.trimmed().isEmpty())
        return;
    QSettings s(m_configPath, QSettings::IniFormat);
    s.beginGroup(QLatin1String(kNewestFirmwareGroup));
    s.endGroup();
    s.beginGroup(QLatin1String(kNewestFirmwareDateGroup));
    s.remove(newestKey(model));
    s.endGroup();
    s.beginGroup(QLatin1String(kNewestFirmwareGroup));
    if (version.trimmed().isEmpty()) {
        s.remove(newestKey(model));
    } else {
        // Saving again, even the same version, records today as the day it was checked by hand.
        s.setValue(newestKey(model), version.trimmed());
        s.endGroup();
        s.beginGroup(QLatin1String(kNewestFirmwareDateGroup));
        s.setValue(newestKey(model), QDate::currentDate().toString(Qt::ISODate));
    }
    s.sync();
}

void HikvisionBackup::checkHealth(const QString &ip)
{
    if (m_ruleBusy)
        return;
    Cam found;
    bool have = false;
    for (const Cam &c : m_cams) {
        if (c.ip == ip) {
            found = c;
            have = true;
            break;
        }
    }
    if (!have) {
        Cam none;
        none.ip = ip;
        emit healthResult(healthCard(none, {healthItem(QStringLiteral("problem"), QStringLiteral("Clock and firmware"),
                                                       QStringLiteral("That camera is not in the list. Refresh the list and try again."))},
                                     {QStringLiteral("Not in the list")}, QString(), QString()));
        return;
    }
    m_ruleBusy = true; // shares the one-at-a-time flag with the other camera switches
    emit ruleBusyChanged();
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    const QMap<QString, QString> newest = readNewestFirmware();
    const QMap<QString, QString> newestDay = readNewestFirmwareDays();
    m_ruleWorker = QThread::create([this, found, newest, newestDay]() {
        const HealthAnswer a = healthOne(found, newest, newestDay);
        QMetaObject::invokeMethod(this, [this, found, a]() {
            qInfo() << "[Health]" << found.ip << "-" << QString(a.message).replace(QLatin1Char('\n'), QLatin1String(" | "));
            m_ruleBusy = false;
            emit ruleBusyChanged();
            emit healthResult(a.result);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

void HikvisionBackup::checkHealthAll()
{
    if (m_ruleBusy)
        return;
    if (m_cams.isEmpty()) {
        emit healthAllFinished(QStringLiteral("No cameras found. Refresh the list first."), false);
        return;
    }
    m_ruleBusy = true;
    emit ruleBusyChanged();
    const QList<Cam> cams = m_cams;
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    const QMap<QString, QString> newest = readNewestFirmware();
    const QMap<QString, QString> newestDay = readNewestFirmwareDays();
    m_ruleWorker = QThread::create([this, cams, newest, newestDay]() {
        int good = 0, needLook = 0;
        QSet<QString> refusedLogins;
        int n = 0;
        for (const Cam &cam : cams) {
            ++n;
            const QString login = cam.user + QLatin1Char('\n') + cam.pass;
            HealthAnswer a;
            if (refusedLogins.contains(login)) {
                a.message = QStringLiteral("skipped: this login was already refused by another camera");
                a.result = healthCard(cam, {healthItem(QStringLiteral("problem"), QStringLiteral("Clock and firmware"), a.message)},
                                      {QStringLiteral("Skipped (login refused)")}, QString(), QString());
            } else {
                QMetaObject::invokeMethod(this, [this, cam, n, total = int(cams.size())]() {
                    emit healthAllProgress(cam.name, n, total);
                }, Qt::QueuedConnection);
                a = healthOne(cam, newest, newestDay);
                if (a.refused && !login.isEmpty())
                    refusedLogins.insert(login);
            }
            qInfo() << "[Health]" << cam.ip << cam.name << "-" << QString(a.message).replace(QLatin1Char('\n'), QLatin1String(" | "));
            const QVariantMap card = a.result;
            QMetaObject::invokeMethod(this, [this, card]() { emit healthAllItem(card); }, Qt::QueuedConnection);
            if (card.value(QStringLiteral("problems")).toInt() == 0 && card.value(QStringLiteral("warnings")).toInt() == 0)
                ++good;
            else
                ++needLook;
        }
        const QString summary = QStringLiteral("Clock and firmware of %1 camera(s): %2 fine, %3 to look at.").arg(cams.size()).arg(good).arg(needLook);
        QMetaObject::invokeMethod(this, [this, summary, needLook]() {
            m_ruleBusy = false;
            emit ruleBusyChanged();
            emit healthAllFinished(summary, needLook == 0);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

void HikvisionBackup::checkRuleLinesAll()
{
    runRuleLinesAll(0);
}

void HikvisionBackup::setRuleLinesAll(bool on)
{
    runRuleLinesAll(on ? 1 : 2);
}

// Goes through every camera of the loaded list, one after the other, one request set each. A camera that cannot take the
// switch (no switch, no saved login, not reachable) is reported and skipped. A login the camera refused is never sent to
// another camera either, so one wrong password cannot be tried again and again.
void HikvisionBackup::runRuleLinesAll(int mode)
{
    if (m_ruleBusy)
        return;
    if (m_cams.isEmpty()) {
        emit ruleLinesAllFinished(QStringLiteral("No cameras found. Refresh the list first."), false);
        return;
    }
    m_ruleBusy = true;
    emit ruleBusyChanged();
    const QList<Cam> cams = m_cams;
    if (m_ruleWorker) {
        m_ruleWorker->wait();
        m_ruleWorker->deleteLater();
    }
    m_ruleWorker = QThread::create([this, cams, mode]() {
        int on = 0, off = 0, unchanged = 0, unavailable = 0, failed = 0;
        QStringList problems;
        QSet<QString> refusedLogins;
        int n = 0;
        for (const Cam &cam : cams) {
            ++n;
            const QString login = cam.user + QLatin1Char('\n') + cam.pass;
            RuleLinesAnswer a;
            if (refusedLogins.contains(login)) {
                a.state = QStringLiteral("error");
                a.message = QStringLiteral("skipped: this login was already refused by another camera");
            } else {
                QMetaObject::invokeMethod(this, [this, cam, n, total = int(cams.size())]() {
                    emit ruleLinesAllProgress(cam.name, n, total);
                }, Qt::QueuedConnection);
                a = ruleLinesOne(cam, mode);
                if (a.refused && !login.isEmpty())
                    refusedLogins.insert(login);
            }
            qInfo() << "[RuleLines]" << cam.ip << cam.name << a.state << "-" << a.message;
            if (a.state == QLatin1String("unavailable"))
                ++unavailable;
            else if (a.state == QLatin1String("error")) {
                ++failed;
                problems << QStringLiteral("%1: %2").arg(cam.name, a.message);
            } else if (a.message.startsWith(QLatin1String("already")))
                ++unchanged;
            else if (a.state == QLatin1String("on"))
                ++on;
            else
                ++off;
        }
        QString summary;
        if (mode == 0) {
            summary = QStringLiteral("Read %1 camera(s): %2 on or off as listed, %3 without the switch, %4 could not be read.")
                          .arg(cams.size()).arg(on + off + unchanged).arg(unavailable).arg(failed);
        } else {
            summary = QStringLiteral("Rule lines %1: %2 camera(s) changed, %3 already that way, %4 without the switch, %5 failed.")
                          .arg(mode == 1 ? QStringLiteral("turned on") : QStringLiteral("turned off"))
                          .arg(mode == 1 ? on : off).arg(unchanged).arg(unavailable).arg(failed);
        }
        if (!problems.isEmpty())
            summary += QStringLiteral("\n") + problems.join(QLatin1Char('\n'));
        const bool ok = failed == 0;
        QMetaObject::invokeMethod(this, [this, summary, ok]() {
            m_ruleBusy = false;
            emit ruleBusyChanged();
            emit ruleLinesAllFinished(summary, ok);
        }, Qt::QueuedConnection);
    });
    m_ruleWorker->start();
}

void HikvisionBackup::backupAll()
{
    if (m_busy)
        return;
    if (m_cams.isEmpty()) {
        emit finished(QStringLiteral("No cameras found. Refresh the list first."), false);
        return;
    }
    start(m_cams, false, QString(), QString());
}

void HikvisionBackup::start(const QList<Cam> &cams, bool single, const QString &overrideUser, const QString &overridePass)
{
    m_busy = true;
    emit busyChanged();
    const QString dir = backupDir();
    const QString configPath = m_configPath;
    if (m_worker) {
        m_worker->wait();
        m_worker->deleteLater();
    }
    m_worker = QThread::create([this, cams, single, overrideUser, overridePass, dir, configPath]() {
        int saved = 0, skipped = 0, failed = 0;
        QStringList problems;
        int n = 0;
        for (Cam cam : cams) {
            ++n;
            bool usedOverride = false;
            if (single && !overrideUser.isEmpty() && !overridePass.isEmpty()) {
                cam.user = overrideUser; // this camera's own login, typed just now
                cam.pass = overridePass;
                usedOverride = true;
            }
            QMetaObject::invokeMethod(this, [this, cam, n, total = cams.size()]() {
                emit progress(cam.name, QStringLiteral("Backing up %1 of %2 (%3)...").arg(n).arg(total).arg(cam.ip));
            }, Qt::QueuedConnection);

            const Result res = backupOne(cam, dir);
            if (res.kind == Result::Saved) {
                ++saved;
                if (usedOverride) {
                    // The camera accepted this login: keep it for this camera from now on.
                    QSettings s(configPath, QSettings::IniFormat);
                    s.beginGroup(QLatin1String(kLoginGroup));
                    s.setValue(cam.ip, QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("user"), cam.user},
                                                                                  {QStringLiteral("pass"), cam.pass}}).toJson(QJsonDocument::Compact)));
                }
            } else if (res.kind == Result::Skipped) {
                ++skipped;
                problems << QStringLiteral("%1: %2").arg(cam.name, res.message);
            } else {
                ++failed;
                problems << QStringLiteral("%1: %2").arg(cam.name, res.message);
            }
            qInfo() << "[Backup]" << cam.name << cam.ip << res.message;
        }

        QString summary;
        if (single) {
            summary = problems.isEmpty() ? QStringLiteral("%1 backed up to %2").arg(cams.first().name, dir)
                                         : problems.first();
        } else {
            summary = QStringLiteral("Backed up %1 of %2 cameras to %3").arg(saved).arg(cams.size()).arg(dir);
            if (skipped > 0)
                summary += QStringLiteral("; %1 skipped").arg(skipped);
            if (failed > 0)
                summary += QStringLiteral("; %1 failed").arg(failed);
            if (!problems.isEmpty())
                summary += QStringLiteral("\n") + problems.mid(0, 6).join(QStringLiteral("\n"));
        }
        const bool ok = failed == 0 && (saved > 0 || skipped == 0);
        QMetaObject::invokeMethod(this, [this, summary, ok]() {
            m_busy = false;
            emit busyChanged();
            emit finished(summary, ok);
        }, Qt::QueuedConnection);
    });
    m_worker->start();
}

namespace {
// PUT a file to a camera with curl. Returns the HTTP status (0 when it could not connect); the answer goes to *body.
int curlPut(const QString &url, const QString &user, const QString &pass, int maxSeconds, const QString &file,
            QByteArray *body)
{
    QStringList args;
    args << QStringLiteral("-s") << QStringLiteral("-k") << QStringLiteral("--anyauth") << QStringLiteral("-u")
         << QStringLiteral("%1:%2").arg(user, pass) << QStringLiteral("--max-time") << QString::number(maxSeconds)
         << QStringLiteral("-X") << QStringLiteral("PUT") << QStringLiteral("--data-binary") << QStringLiteral("@") + file
         << QStringLiteral("-H") << QStringLiteral("Content-Type: application/octet-stream")
         << QStringLiteral("-w") << QStringLiteral("%{http_code}") << url;
    QProcess p;
    p.start(QStringLiteral("curl"), args);
    if (!p.waitForFinished((maxSeconds + 10) * 1000)) {
        p.kill();
        return 0;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (body)
        *body = out.left(qMax(0, int(out.size()) - 3));
    return out.right(3).toInt();
}

// PUT with no body (the restart request). Returns the HTTP status, 0 when it could not connect.
int curlPutEmpty(const QString &url, const QString &user, const QString &pass, int maxSeconds, QByteArray *body)
{
    QProcess p;
    p.start(QStringLiteral("curl"), {QStringLiteral("-s"), QStringLiteral("-k"), QStringLiteral("--anyauth"), QStringLiteral("-u"),
                                     QStringLiteral("%1:%2").arg(user, pass), QStringLiteral("--max-time"), QString::number(maxSeconds),
                                     QStringLiteral("-X"), QStringLiteral("PUT"), QStringLiteral("-w"), QStringLiteral("%{http_code}"), url});
    if (!p.waitForFinished((maxSeconds + 5) * 1000)) {
        p.kill();
        return 0;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (body)
        *body = out.left(qMax(0, int(out.size()) - 3));
    return out.right(3).toInt();
}

// Is the camera answering at all? No login is sent, so this can never count as a failed login. 0 = no answer;
// any HTTP status (a 401 included) means the camera's web server is up.
int probeUp(const QString &ip, int port)
{
    QProcess p;
    p.start(QStringLiteral("curl"), {QStringLiteral("-s"), QStringLiteral("-k"), QStringLiteral("--max-time"), QStringLiteral("3"),
                                     QStringLiteral("-o"), QStringLiteral("/dev/null"), QStringLiteral("-w"), QStringLiteral("%{http_code}"),
                                     QStringLiteral("http://%1:%2/ISAPI/System/deviceInfo").arg(ip).arg(port)});
    if (!p.waitForFinished(6000)) {
        p.kill();
        return 0;
    }
    return p.readAllStandardOutput().trimmed().toInt();
}
}


// Restores one camera from a backup file made earlier. Result::Saved means the camera took the file and came back.
HikvisionBackup::Result HikvisionBackup::restoreOne(const Cam &cam, const QString &dir, const QString &fileName,
                                                    const std::function<void(const QString &)> &say)
{
    Result r;
    if (cam.user.isEmpty() || cam.pass.isEmpty()) {
        r.message = QStringLiteral("no login saved for this camera (a blank password is never sent)");
        return r;
    }

    // 1. The file must be one of ours: in the backups folder, a .cfg, with its description beside it.
    const QFileInfo cfg(dir + QLatin1Char('/') + QFileInfo(fileName).fileName());
    if (cfg.suffix() != QLatin1String("cfg") || !cfg.exists()) {
        r.message = QStringLiteral("that backup file was not found in ") + dir;
        return r;
    }
    QFile metaFile(cfg.absolutePath() + QLatin1Char('/') + cfg.completeBaseName() + QStringLiteral(".json"));
    if (!metaFile.open(QIODevice::ReadOnly)) {
        r.message = QStringLiteral("the description file beside the backup is missing, so it cannot be matched to a camera");
        return r;
    }
    const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
    const QString wantSerial = meta.value(QStringLiteral("serial")).toString();
    if (wantSerial.isEmpty() || meta.value(QStringLiteral("ip")).toString() != cam.ip) {
        r.message = QStringLiteral("this backup does not belong to this camera");
        return r;
    }

    // 2. The camera in front of us must be the camera the backup came from.
    say(QStringLiteral("Checking the camera..."));
    const QString base = QStringLiteral("http://%1:%2").arg(cam.ip).arg(cam.httpPort);
    QByteArray info;
    const int infoCode = curlGet(base + QStringLiteral("/ISAPI/System/deviceInfo"), cam.user, cam.pass, 8, QString(), &info);
    if (infoCode == 401) {
        r.message = QStringLiteral("login refused: enter this camera's own username and password");
        return r;
    }
    if (infoCode != 200) {
        r.message = QStringLiteral("cannot read the camera (HTTP %1)").arg(infoCode);
        return r;
    }
    const QString haveSerial = tag(QString::fromUtf8(info), QStringLiteral("serialNumber"));
    if (haveSerial != wantSerial) {
        r.message = QStringLiteral("the camera's serial number (%1) does not match the backup (%2)").arg(haveSerial, wantSerial);
        return r;
    }

    // 3. A fresh backup of the camera as it is now, so a bad restore can be undone. If this fails, nothing is sent.
    say(QStringLiteral("Saving a safety backup of the current settings first..."));
    const Result safety = backupOne(cam, dir);
    if (safety.kind != Result::Saved) {
        r.message = QStringLiteral("could not take the safety backup first (%1); nothing was changed").arg(safety.message);
        return r;
    }

    // 4. Send the file.
    say(QStringLiteral("Uploading the backup..."));
    QByteArray answer;
    const int putCode = curlPut(base + QStringLiteral("/ISAPI/System/configurationData?secretkey=") + QLatin1String(kBackupKey),
                                cam.user, cam.pass, 120, cfg.absoluteFilePath(), &answer);
    const QString answerText = QString::fromUtf8(answer);
    const QString statusCode = tag(answerText, QStringLiteral("statusCode"));
    // 1 = OK, 7 = OK and a reboot is needed.
    const bool accepted = putCode == 200 && (statusCode.isEmpty() || statusCode == QLatin1String("1") || statusCode == QLatin1String("7"));
    const QString statusString = tag(answerText, QStringLiteral("statusString"));
    qInfo() << "[Restore]" << cam.ip << "upload answered HTTP" << putCode << "statusCode" << statusCode << statusString;
    if (!accepted) {
        QString why = QStringLiteral("the camera answered HTTP %1").arg(putCode);
        const QString sub = tag(answerText, QStringLiteral("subStatusCode"));
        if (!sub.isEmpty())
            why += QStringLiteral(" (%1)").arg(sub);
        r.message = why + QStringLiteral("; the safety backup was saved in ") + dir;
        return r;
    }

    // 5. Some cameras apply the file at once; others accept it and say a restart is needed (status 7). Then the
    // settings only take effect after a restart, so it is asked for here, as the confirmation said.
    const bool needsRestart = statusCode == QLatin1String("7");
    if (needsRestart) {
        say(QStringLiteral("The camera accepted the file and asks for a restart. Restarting it..."));
        QByteArray rebootAnswer;
        const int rebootCode = curlPutEmpty(base + QStringLiteral("/ISAPI/System/reboot"), cam.user, cam.pass, 15, &rebootAnswer);
        qInfo() << "[Restore]" << cam.ip << "restart request answered HTTP" << rebootCode
                << tag(QString::fromUtf8(rebootAnswer), QStringLiteral("statusString"));
        if (rebootCode != 200) {
            r.message = QStringLiteral("the camera accepted the file but needs a restart, and the restart request failed (HTTP %1); "
                                       "restart it by hand for the settings to apply").arg(rebootCode);
            return r;
        }
    }
    say(QStringLiteral("Uploaded. Waiting for the camera to restart..."));
    bool wentDown = false;
    qint64 waited = 0;
    for (; waited < 60 && !wentDown; waited += 2) {
        QThread::sleep(2);
        wentDown = probeUp(cam.ip, cam.httpPort) == 0;
    }
    bool back = !wentDown && probeUp(cam.ip, cam.httpPort) != 0;
    for (waited = 0; wentDown && waited < 240 && !back; waited += 3) {
        QThread::sleep(3);
        back = probeUp(cam.ip, cam.httpPort) != 0;
        if (!back && waited % 15 == 0)
            say(QStringLiteral("Waiting for the camera to restart... (%1 s)").arg(waited));
    }
    if (!back) {
        r.message = QStringLiteral("the file was sent, but the camera has not answered for 4 minutes; check it");
        return r;
    }
    QThread::sleep(3); // let its services come up

    // 6. One login to confirm it is healthy. The restored settings include the password from the backup.
    QByteArray after;
    const int afterCode = curlGet(base + QStringLiteral("/ISAPI/System/deviceInfo"), cam.user, cam.pass, 8, QString(), &after);
    r.kind = Result::Saved;
    if (afterCode == 401)
        r.message = QStringLiteral("restored and back online, but it now refuses the saved login: the backup's own password applies");
    else if (afterCode != 200)
        r.message = QStringLiteral("restored; the camera is answering but not fully up yet (HTTP %1)").arg(afterCode);
    else if (wentDown)
        r.message = needsRestart ? QStringLiteral("restored; the camera needed a restart, which was done, and it is back online")
                                 : QStringLiteral("restored and back online");
    else
        r.message = QStringLiteral("the camera accepted the file (answer: %1) and stayed online; it did not restart on its own")
                        .arg(statusString.isEmpty() ? QStringLiteral("status ") + statusCode : statusString);
    return r;
}

QVariantList HikvisionBackup::listBackups(const QString &ip) const
{
    QVariantList out;
    const QString dir = backupDir();
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QFileInfo &fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const QString cfg = o.value(QStringLiteral("file")).toString();
        if (o.value(QStringLiteral("ip")).toString() != ip || cfg.isEmpty() || !QFileInfo::exists(dir + QLatin1Char('/') + cfg))
            continue;
        const QDateTime when = QDateTime::fromString(o.value(QStringLiteral("saved")).toString(), Qt::ISODate);
        out.append(QVariantMap{{QStringLiteral("file"), cfg},
                               {QStringLiteral("label"), QStringLiteral("%1  (firmware %2, %3 KB)")
                                    .arg(QLocale::c().toString(when, QStringLiteral("d MMM yyyy HH:mm")),
                                         o.value(QStringLiteral("firmware")).toString())
                                    .arg(qint64(o.value(QStringLiteral("bytes")).toDouble()) / 1024)}});
    }
    return out;
}

void HikvisionBackup::restoreCamera(const QString &ip, const QString &file, const QString &user, const QString &password)
{
    if (m_busy)
        return;
    const Cam *found = nullptr;
    for (const Cam &c : m_cams) {
        if (c.ip == ip)
            found = &c;
    }
    if (!found) {
        emit finished(QStringLiteral("That camera is not in the list. Refresh the list and try again."), false);
        return;
    }
    Cam cam = *found;
    if (!user.trimmed().isEmpty() && !password.isEmpty()) {
        cam.user = user.trimmed();
        cam.pass = password;
    }
    m_busy = true;
    emit busyChanged();
    const QString dir = backupDir();
    if (m_worker) {
        m_worker->wait();
        m_worker->deleteLater();
    }
    m_worker = QThread::create([this, cam, file, dir]() {
        auto say = [this, cam](const QString &text) {
            QMetaObject::invokeMethod(this, [this, cam, text]() { emit progress(cam.name, text); }, Qt::QueuedConnection);
        };
        const Result res = restoreOne(cam, dir, file, say);
        qInfo() << "[Restore]" << cam.name << cam.ip << res.message;
        const bool ok = res.kind == Result::Saved;
        const QString summary = ok ? QStringLiteral("%1: %2").arg(cam.name, res.message)
                                   : QStringLiteral("%1 was NOT restored: %2").arg(cam.name, res.message);
        QMetaObject::invokeMethod(this, [this, summary, ok]() {
            m_busy = false;
            emit busyChanged();
            emit finished(summary, ok);
        }, Qt::QueuedConnection);
    });
    m_worker->start();
}
