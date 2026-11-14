#ifndef OVERLAYTEXT_H
#define OVERLAYTEXT_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class OverlayEntry;

// The list of "MQTT value into a camera's text overlay" entries. Each entry is one topic going into one text overlay (slot)
// of one camera, with its own broker, topic, text, unit and update time, and runs on its own (see OverlayEntry for how a
// single entry treats the camera). The list is kept as JSON in the "overlayText" setting:
//   { "version": 2, "whenClosed": "keep" | "remove" | "stale", "entries": [ { enabled, host, port, user, pass, topic, field,
//     camera, camChannel, httpPort, camUser, camPass, slot, template, unit, decimals, minutes }, ... ] }
// "whenClosed" is one setting for all entries: what the cameras show while the KVision window is closed. The older single-entry
// form of the setting (the same fields at the top level) is converted when it is read.
class OverlayText : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap statuses READ statuses NOTIFY statusesChanged) // entry key -> what it last did
    Q_PROPERTY(QString status READ status NOTIFY statusesChanged)        // one line for all entries

public:
    explicit OverlayText(QObject *parent = nullptr);

    QVariantMap statuses() const { return m_statuses; }
    QString status() const;

    // The settings as the current JSON form, whatever form they were saved in. Empty or broken text gives an empty list.
    Q_INVOKABLE static QString migrate(const QString &json);
    // Which camera and text overlay an entry is for. Two entries with the same key would overwrite each other.
    Q_INVOKABLE static QString entryKey(const QVariantMap &entry);

    // Makes the running entries match the list: new ones start, changed ones restart, unchanged ones are left alone (their
    // timers keep running), ones no longer in the list stop. Only entries that are switched on run.
    Q_INVOKABLE void apply(const QString &json);
    // Stops every entry.
    Q_INVOKABLE void stop();
    // Removes the text overlay of one entry from its camera (switches it off and empties its text) and stops that entry.
    // The result goes to clearDone with the entry's key.
    Q_INVOKABLE void clearSlot(const QVariantMap &entry);
    // For the closed window: remove, or mark as stale ("--"), every entry that is switched on.
    void clearAll(const QString &json);
    void staleAll(const QString &json);
    // Reads the value and shows what would be written, without writing anything. Result goes to testDone.
    Q_INVOKABLE void test(const QVariantMap &entry);

    // The text for a value in Celsius: the template with {temp} and {unit} replaced.
    Q_INVOKABLE static QString formatText(double celsius, const QString &templ, const QString &unit, int decimals);
    // The number in an MQTT message: a plain number, or the field of a JSON object. ok says whether one was found.
    static double parseValue(const QString &payload, const QString &field, bool *ok = nullptr);

signals:
    void statusesChanged();
    void testDone(bool ok, const QString &text, const QString &message);
    void clearDone(const QString &key, bool ok, const QString &message);

private:
    static QVariantList entriesOf(const QString &json);
    OverlayEntry *makeEntry(const QString &key);
    void entryStatus(const QString &key, const QString &status);

    QHash<QString, OverlayEntry *> m_entries; // the running ones
    QVariantMap m_statuses;
    QList<OverlayEntry *> m_transient;        // for clearing and marking stale: they finish and delete themselves
};

#endif // OVERLAYTEXT_H
