#include "layoutbackups.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

LayoutBackups::LayoutBackups(const QString &configDir, QObject *parent)
    : QObject(parent)
    , m_dir(QDir::fromNativeSeparators(configDir) + QStringLiteral("/layout-backups"))
{
}

bool LayoutBackups::validUid(const QString &uid)
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9]{1,32}$"));
    return rx.match(uid).hasMatch();
}

bool LayoutBackups::validId(const QString &id)
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9]{1,32}_[0-9]{13}(-[0-9]+)?$"));
    return rx.match(id).hasMatch();
}

QString LayoutBackups::makeUid()
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    QString s;
    for (int i = 0; i < 12; ++i)
        s += QLatin1Char(letters[QRandomGenerator::global()->bounded(int(sizeof(letters) - 1))]);
    return s;
}

QStringList LayoutBackups::filesFor(const QString &uid) const
{
    if (!validUid(uid))
        return {};
    // The file names hold the time in 13 digits, so sorting the names sorts by time.
    QStringList names = QDir(m_dir).entryList({uid + QStringLiteral("_*.json")}, QDir::Files, QDir::Name);
    std::reverse(names.begin(), names.end());
    return names;
}

QString LayoutBackups::save(const QString &uid, const QString &layoutJson, const QString &note, bool deleted)
{
    if (!validUid(uid))
        return QString();
    const QJsonDocument layoutDoc = QJsonDocument::fromJson(layoutJson.toUtf8());
    if (!layoutDoc.isObject())
        return QString();
    const QJsonObject layout = layoutDoc.object();

    QDir().mkpath(m_dir);
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    int cameras = 0;
    for (const QJsonValue &v : layout.value(QStringLiteral("items")).toArray()) {
        if (!v.toObject().value(QStringLiteral("url")).toString().isEmpty())
            ++cameras;
    }
    const QJsonObject size = layout.value(QStringLiteral("size")).toObject();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QString id = QStringLiteral("%1_%2").arg(uid).arg(now, 13, 10, QLatin1Char('0'));
    for (int n = 1; QFile::exists(m_dir + QLatin1Char('/') + id + QStringLiteral(".json")); ++n)
        id = QStringLiteral("%1_%2-%3").arg(uid).arg(now, 13, 10, QLatin1Char('0')).arg(n);

    const QJsonObject o{{QStringLiteral("version"), 1},
                        {QStringLiteral("uid"), uid},
                        {QStringLiteral("whenMs"), double(now)},
                        {QStringLiteral("note"), note.left(200)},
                        {QStringLiteral("deleted"), deleted},
                        {QStringLiteral("name"), layout.value(QStringLiteral("name")).toString()},
                        {QStringLiteral("columns"), size.value(QStringLiteral("width")).toInt()},
                        {QStringLiteral("rows"), size.value(QStringLiteral("height")).toInt()},
                        {QStringLiteral("cameras"), cameras},
                        {QStringLiteral("nvrIp"), layout.value(QStringLiteral("nvrIp")).toString()},
                        {QStringLiteral("layout"), layout}};

    QSaveFile f(m_dir + QLatin1Char('/') + id + QStringLiteral(".json"));
    f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (!f.commit())
        return QString();
    QFile::setPermissions(f.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // Keep the newest kMaxPerLayout; the one just written is the newest.
    const QStringList all = filesFor(uid);
    for (int i = kMaxPerLayout; i < all.size(); ++i)
        QFile::remove(m_dir + QLatin1Char('/') + all.at(i));

    emit changed();
    return id;
}

QVariantMap LayoutBackups::describe(const QString &file) const
{
    QFile f(m_dir + QLatin1Char('/') + file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return {};
    const QJsonObject o = doc.object();
    const qint64 ms = qint64(o.value(QStringLiteral("whenMs")).toDouble());
    QVariantMap m;
    m[QStringLiteral("id")] = QFileInfo(file).completeBaseName();
    m[QStringLiteral("uid")] = o.value(QStringLiteral("uid")).toString();
    m[QStringLiteral("whenMs")] = ms;
    m[QStringLiteral("when")] = QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(ms), QStringLiteral("d MMM yyyy HH:mm"));
    m[QStringLiteral("note")] = o.value(QStringLiteral("note")).toString();
    m[QStringLiteral("name")] = o.value(QStringLiteral("name")).toString();
    m[QStringLiteral("columns")] = o.value(QStringLiteral("columns")).toInt();
    m[QStringLiteral("rows")] = o.value(QStringLiteral("rows")).toInt();
    m[QStringLiteral("cameras")] = o.value(QStringLiteral("cameras")).toInt();
    m[QStringLiteral("nvrIp")] = o.value(QStringLiteral("nvrIp")).toString();
    m[QStringLiteral("deleted")] = o.value(QStringLiteral("deleted")).toBool();
    return m;
}

QVariantList LayoutBackups::list(const QString &uid) const
{
    QVariantList out;
    for (const QString &f : filesFor(uid)) {
        const QVariantMap m = describe(f);
        if (!m.isEmpty())
            out.append(m);
    }
    return out;
}

QVariantList LayoutBackups::deletedLayouts(const QStringList &liveUids) const
{
    QVariantList out;
    QSet<QString> seen;
    QStringList names = QDir(m_dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.section(QLatin1Char('_'), 1) > b.section(QLatin1Char('_'), 1); // newest first, whatever the layout
    });
    for (const QString &f : names) {
        const QString uid = f.section(QLatin1Char('_'), 0, 0);
        if (!validUid(uid) || liveUids.contains(uid) || seen.contains(uid))
            continue;
        const QVariantMap m = describe(f);
        if (m.isEmpty())
            continue;
        seen.insert(uid);
        out.append(m);
    }
    return out;
}

QString LayoutBackups::load(const QString &id) const
{
    if (!validId(id))
        return QString();
    QFile f(m_dir + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject() || !doc.object().value(QStringLiteral("layout")).isObject())
        return QString();
    return QString::fromUtf8(QJsonDocument(doc.object().value(QStringLiteral("layout")).toObject()).toJson(QJsonDocument::Compact));
}

bool LayoutBackups::remove(const QString &id)
{
    if (!validId(id))
        return false;
    const bool ok = QFile::remove(m_dir + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (ok)
        emit changed();
    return ok;
}

int LayoutBackups::removeAll(const QString &uid)
{
    int n = 0;
    for (const QString &f : filesFor(uid)) {
        if (QFile::remove(m_dir + QLatin1Char('/') + f))
            ++n;
    }
    if (n > 0)
        emit changed();
    return n;
}

int LayoutBackups::count(const QString &uid) const
{
    return filesFor(uid).size();
}

int LayoutBackups::autoBackup(const QString &layoutsJson, int minHours)
{
    const QJsonDocument doc = QJsonDocument::fromJson(layoutsJson.toUtf8());
    if (!doc.isArray())
        return 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int saved = 0;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject layout = v.toObject();
        const QString uid = layout.value(QStringLiteral("uid")).toString();
        if (!validUid(uid))
            continue;
        int cameras = 0;
        for (const QJsonValue &it : layout.value(QStringLiteral("items")).toArray()) {
            if (!it.toObject().value(QStringLiteral("url")).toString().isEmpty())
                ++cameras;
        }
        if (cameras == 0)
            continue; // an empty layout has nothing to protect
        const QStringList files = filesFor(uid);
        if (!files.isEmpty()) {
            const QVariantMap newest = describe(files.first());
            if (!newest.isEmpty() && now - newest.value(QStringLiteral("whenMs")).toLongLong() < qint64(minHours) * 3600 * 1000)
                continue;
            if (load(QFileInfo(files.first()).completeBaseName()) ==
                QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact)))
                continue; // nothing has changed since the newest backup
        }
        if (!save(uid, QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact)), QStringLiteral("Automatic"), false).isEmpty())
            ++saved;
    }
    return saved;
}
