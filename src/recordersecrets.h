#ifndef RECORDERSECRETS_H
#define RECORDERSECRETS_H

#include <QObject>
#include <QString>

// Recorder passwords are not stored in the settings JSON. They live in a
// mode-0600 file next to the config. openRecorders puts them back for the
// running app. sealRecorders takes them out before anything is written to
// the ini file or to an export. A password that is still in an old JSON
// blob is moved across on the first open.
class RecorderSecrets : public QObject
{
    Q_OBJECT
public:
    explicit RecorderSecrets(QObject *parent = nullptr) : QObject(parent) {}

    static QString secretsFileFor(const QString &configFile);
    static QString openRecorders(const QString &stored, const QString &configFile);
    static QString sealRecorders(const QString &live, const QString &configFile);

    // Read Hikvision/recordersJson, move any inline passwords into the secret
    // file, and return the list with passwords filled in.
    static QString recordersFromConfig(const QString &configFile);

    // configFile is the settings ini. An empty string comes back when the
    // secret file could not be locked or written; the caller must leave the
    // previous settings value alone in that case.
    Q_INVOKABLE QString open(const QString &stored, const QString &configFile) const;
    Q_INVOKABLE QString seal(const QString &live, const QString &configFile) const;
};

#endif
