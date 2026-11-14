#ifndef LAYOUTBACKUPS_H
#define LAYOUTBACKUPS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

// Backups of single layouts (the Backup / Restore button next to each layout in Presets). Each backup is one small file in
// <config dir>/layout-backups/, named <layout id>_<time in ms>.json, so the settings file itself is never touched. A layout
// finds its backups through its id (the "uid" kept in the layout), so renaming a layout or moving it in the list loses nothing.
// The newest kMaxPerLayout backups of a layout are kept; older ones are removed when a new one is saved.
class LayoutBackups : public QObject
{
    Q_OBJECT

public:
    static const int kMaxPerLayout = 30;

    explicit LayoutBackups(const QString &configDir, QObject *parent = nullptr);

    // A new layout id (12 letters and digits).
    Q_INVOKABLE QString newUid() const { return makeUid(); }
    static QString makeUid();
    // Saves a backup of the layout (its JSON) for the layout id and returns the backup's id, or "" when it could not be written.
    // deleted marks the last copy taken when the layout was removed with the trash button.
    Q_INVOKABLE QString save(const QString &uid, const QString &layoutJson, const QString &note, bool deleted = false);
    // The backups of one layout, newest first. Each entry: id, uid, whenMs, when, note, name, columns, rows, cameras, deleted.
    Q_INVOKABLE QVariantList list(const QString &uid) const;
    // The newest backup of every layout that is no longer in the list (its id is not in liveUids), newest first.
    Q_INVOKABLE QVariantList deletedLayouts(const QStringList &liveUids) const;
    // The layout JSON saved in a backup, or "" when the backup is missing or damaged.
    Q_INVOKABLE QString load(const QString &id) const;
    Q_INVOKABLE bool remove(const QString &id);
    // Removes every backup of a layout; returns how many were removed.
    Q_INVOKABLE int removeAll(const QString &uid);
    // How many backups a layout has (by file name only, so it is cheap).
    Q_INVOKABLE int count(const QString &uid) const;
    // Automatic backups: for every layout in the JSON list that has cameras and is different from its newest backup, saves one,
    // but not more often than once in minHours for a layout. Returns how many were saved.
    Q_INVOKABLE int autoBackup(const QString &layoutsJson, int minHours = 20);
    // Where the backups are kept.
    Q_INVOKABLE QString folder() const { return m_dir; }

    static bool validUid(const QString &uid);
    static bool validId(const QString &id);

signals:
    void changed();

private:
    QStringList filesFor(const QString &uid) const; // newest first
    QVariantMap describe(const QString &file) const;

    QString m_dir;
};

#endif // LAYOUTBACKUPS_H
