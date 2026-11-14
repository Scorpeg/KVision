#include "overlaytext.h"

#include "overlayentry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

OverlayText::OverlayText(QObject *parent)
    : QObject(parent)
{
}

QString OverlayText::formatText(double celsius, const QString &templ, const QString &unit, int decimals)
{
    return OverlayEntry::formatText(celsius, templ, unit, decimals);
}

double OverlayText::parseValue(const QString &payload, const QString &field, bool *ok)
{
    return OverlayEntry::parseValue(payload, field, ok);
}

QString OverlayText::entryKey(const QVariantMap &e)
{
    const int ch = e.value(QStringLiteral("camChannel")).toInt();
    return QStringLiteral("%1|%2|%3").arg(e.value(QStringLiteral("camera")).toString().trimmed()).arg(ch > 0 ? ch : 1).arg(e.value(QStringLiteral("slot")).toInt());
}

QString OverlayText::migrate(const QString &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    QJsonObject out;
    out.insert(QStringLiteral("version"), 2);
    out.insert(QStringLiteral("whenClosed"), o.value(QStringLiteral("whenClosed")).toString(QStringLiteral("keep")));
    QJsonArray list;
    if (o.contains(QStringLiteral("entries"))) {
        list = o.value(QStringLiteral("entries")).toArray();
    } else if (!o.value(QStringLiteral("camera")).toString().isEmpty() || !o.value(QStringLiteral("topic")).toString().isEmpty()) {
        // The earlier single setup becomes the first entry.
        QJsonObject e = o;
        e.remove(QStringLiteral("whenClosed"));
        e.remove(QStringLiteral("version"));
        list.append(e);
    }
    out.insert(QStringLiteral("entries"), list);
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QVariantList OverlayText::entriesOf(const QString &json)
{
    return QJsonDocument::fromJson(migrate(json).toUtf8()).object().value(QStringLiteral("entries")).toArray().toVariantList();
}

QString OverlayText::status() const
{
    int shown = 0;
    QString last;
    for (auto it = m_statuses.constBegin(); it != m_statuses.constEnd(); ++it) {
        if (!it.value().toString().isEmpty()) {
            ++shown;
            last = it.value().toString();
        }
    }
    if (shown == 0)
        return QString();
    return shown == 1 ? last : tr("%1 entries running").arg(shown);
}

OverlayEntry *OverlayText::makeEntry(const QString &key)
{
    auto *e = new OverlayEntry(this);
    connect(e, &OverlayEntry::statusChanged, this, [this, e, key]() { entryStatus(key, e->status()); });
    return e;
}

void OverlayText::entryStatus(const QString &key, const QString &status)
{
    if (status.isEmpty())
        m_statuses.remove(key);
    else
        m_statuses.insert(key, status);
    emit statusesChanged();
}

void OverlayText::apply(const QString &json)
{
    const QVariantList list = entriesOf(json);
    QSet<QString> wanted;
    int started = 0;
    for (const QVariant &v : list) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("enabled")).toBool())
            continue;
        const QString key = entryKey(e);
        if (wanted.contains(key))
            continue; // the same camera and slot twice: the first one wins
        wanted.insert(key);
        OverlayEntry *run = m_entries.value(key);
        if (run && run->settings() == e)
            continue; // unchanged: leave it running
        if (!run) {
            run = makeEntry(key);
            m_entries.insert(key, run);
        }
        // Entries start a couple of seconds apart, so a recorder with several cameras is not asked everything at once.
        run->apply(e, 3000 + 2000 * started++);
    }
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->apply(QVariantMap());
        it.value()->deleteLater();
        m_statuses.remove(it.key());
        it = m_entries.erase(it);
    }
    emit statusesChanged();
}

void OverlayText::stop()
{
    apply(QString());
}

void OverlayText::clearSlot(const QVariantMap &entry)
{
    const QString key = entryKey(entry);
    // The updates of this entry stop first, so nothing writes the text back.
    if (OverlayEntry *run = m_entries.take(key)) {
        run->apply(QVariantMap());
        run->deleteLater();
    }
    m_statuses.remove(key);
    emit statusesChanged();
    auto *e = new OverlayEntry(this);
    connect(e, &OverlayEntry::clearDone, this, [this, e, key](bool ok, const QString &message) {
        emit clearDone(key, ok, message);
        e->deleteLater();
    });
    e->clearSlot(entry);
}

void OverlayText::clearAll(const QString &json)
{
    stop();
    QSet<QString> done;
    for (const QVariant &v : entriesOf(json)) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("enabled")).toBool() || done.contains(entryKey(e)))
            continue;
        done.insert(entryKey(e));
        clearSlot(e);
    }
}

void OverlayText::staleAll(const QString &json)
{
    stop();
    QSet<QString> done;
    for (const QVariant &v : entriesOf(json)) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("enabled")).toBool() || done.contains(entryKey(e)))
            continue;
        done.insert(entryKey(e));
        auto *run = new OverlayEntry(this);
        connect(run, &OverlayEntry::statusChanged, this, [this, run]() {
            // The marker is written once; the entry is not needed afterwards. A short wait lets the read-back finish.
            QTimer::singleShot(20000, run, &QObject::deleteLater);
        });
        run->showStale(e);
    }
}

void OverlayText::test(const QVariantMap &entry)
{
    auto *e = new OverlayEntry(this);
    connect(e, &OverlayEntry::testDone, this, [this, e](bool ok, const QString &text, const QString &message) {
        emit testDone(ok, text, message);
        e->deleteLater();
    });
    e->test(entry);
}
