#ifndef HIKVISIONBACKUP_H
#define HIKVISIONBACKUP_H

#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <QVariantList>

class QThread;

// Backs up the configuration of Hikvision cameras, one camera at a time, each reached on its own address
// (a camera behind a recorder is found through the recorder's channel list, but the recorder itself is never
// backed up and is not contacted for the export). A backup is the camera's own configuration export
// (/ISAPI/System/configurationData), saved to <config dir>/backups/ with a small .json beside it.
// The export needs a key; a fixed one built into the program is used for every camera, so there is nothing
// to type or remember, and it must never change or older backups stop restoring.
class HikvisionBackup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool ruleBusy READ ruleBusy NOTIFY ruleBusyChanged)

public:
    explicit HikvisionBackup(QObject *parent = nullptr);
    ~HikvisionBackup() override;

    bool busy() const { return m_busy; }
    bool ruleBusy() const { return m_ruleBusy; }

    // Works out the camera list from the saved recorders (looking up each recorder's channels on a worker
    // thread) and emits camerasLoaded(): a list of maps with name, ip, login ("own" or "recorder") and user.
    Q_INVOKABLE void loadCameras(const QString &recordersJson, const QString &configPath);
    // Backs up one camera of the loaded list. A non-empty user and password are used for this camera instead of
    // its saved login, and are remembered for it once the camera has accepted them. One login attempt only.
    Q_INVOKABLE void backupCamera(const QString &ip, const QString &user, const QString &password);
    // Backs up every camera of the loaded list, one after the other.
    Q_INVOKABLE void backupAll();
    // The backups saved for one camera (matched by address), newest first: a list of maps with file and label.
    Q_INVOKABLE QVariantList listBackups(const QString &ip) const;
    // Puts a backup back on the camera it came from (never another camera: the serial number must match).
    // Takes a fresh backup of the camera first, uploads the file, waits for the camera to restart and checks it
    // answers again. A non-empty user and password are used instead of the saved login. One login attempt only.
    Q_INVOKABLE void restoreCamera(const QString &ip, const QString &file, const QString &user, const QString &password);
    // The folder the backups are saved in.
    Q_INVOKABLE QString backupDir() const;
    // The camera's own switch for drawing the VCA rule lines and targets on the video stream (web page: VCA > Smart
    // Event > General Settings > Metadata > Enable Stream Rule / Sub-Stream). Both work on the camera's own address,
    // with the camera's saved login, one login attempt only. checkRuleLines only reads. setRuleLines reads first,
    // changes only "enabled" and "streamType", and reads it back. The answer comes in ruleLinesResult().
    Q_INVOKABLE void checkRuleLines(const QString &ip);
    Q_INVOKABLE void setRuleLines(const QString &ip, bool on);
    // Reads the camera's switch and sets it the other way (the right-click menu). The answer comes in ruleLinesResult().
    Q_INVOKABLE void toggleRuleLines(const QString &ip);
    // The camera's "Environmental Noise Filtering" switch (<noisereduce> in the two-way audio settings). Read only, or
    // read and then set the other way, with a read back. The answer comes in noiseFilterResult().
    Q_INVOKABLE void checkNoiseFilter(const QString &ip);
    Q_INVOKABLE void toggleNoiseFilter(const QString &ip);
    // The camera's built-in flashing light. checkStrobe only asks whether the camera has one; start sends the
    // camera's own trigger (it stops by itself after its own time). The answer comes in strobeResult().
    Q_INVOKABLE void checkStrobe(const QString &ip);
    Q_INVOKABLE void startStrobe(const QString &ip);
    // The camera's warning message (a sound the camera plays: the sound, volume and number of times are the ones set on
    // the camera). Same idea as the flashing light. The answer comes in warningResult().
    Q_INVOKABLE void checkWarning(const QString &ip);
    Q_INVOKABLE void startWarning(const QString &ip);
    // Read only: the camera's clock against this computer's, and its firmware version. The answer comes in
    // healthResult(); checkHealthAll() goes through every camera and reports in healthAllFinished().
    Q_INVOKABLE void checkHealth(const QString &ip);
    Q_INVOKABLE void checkHealthAll();
    Q_INVOKABLE int cameraCount() const { return m_cams.size(); }
    // The same for every camera of the loaded list, one after the other. A camera without the switch is skipped and
    // counted; a login that one camera refused is not sent to another. The result comes in ruleLinesAllFinished().
    Q_INVOKABLE void checkRuleLinesAll();
    Q_INVOKABLE void setRuleLinesAll(bool on);

signals:
    void camerasLoaded(const QVariantList &cameras);
    void progress(const QString &label, const QString &text);
    // summary is a short report; ok is false when something failed.
    void finished(const QString &summary, bool ok);
    void busyChanged();
    void ruleBusyChanged();
    // state is "on", "off", "unavailable" (the camera has no such switch) or "error"; message says what happened.
    void ruleLinesResult(const QString &ip, const QString &state, const QString &message);
    // state is "on", "off", "unavailable" (the camera has no such switch) or "error"; message says what happened.
    void noiseFilterResult(const QString &ip, const QString &state, const QString &message);
    // state is "available", "unavailable" (no flashing light), "started" or "error".
    void strobeResult(const QString &ip, const QString &state, const QString &message);
    // state is "available", "unavailable" (no warning message), "started" or "error".
    void warningResult(const QString &ip, const QString &state, const QString &message);
    // state is "ok", "warn" (for example a clock that is off), "unavailable" or "error"; message is one or more lines.
    void healthResult(const QString &ip, const QString &state, const QString &message);
    void healthAllProgress(const QString &name, int done, int total);
    // One camera of checkHealthAll() has answered (state as in healthResult()).
    void healthAllItem(const QString &ip, const QString &state, const QString &message);
    void healthAllFinished(const QString &summary, bool ok);
    void ruleLinesAllProgress(const QString &name, int done, int total);
    void ruleLinesAllFinished(const QString &summary, bool ok);

public:
    struct Cam {
        QString name;
        QString ip;
        int httpPort = 80;
        QString user;
        QString pass;
        bool ownLogin = false; // a login saved for this camera, not the recorder's
    };
    struct Result {
        enum Kind { Saved, Skipped, Failed } kind = Failed;
        QString message;
    };

private:
    void runRuleLines(const QString &ip, int mode);
    void runNoiseFilter(const QString &ip, int mode);
    void runStrobe(const QString &ip, int mode, bool siren = false);
    void runRuleLinesAll(int mode);
    void startLoad(const QByteArray &json, const QString &configPath);
    void start(const QList<Cam> &cams, bool single, const QString &overrideUser, const QString &overridePass);
    static Result backupOne(const Cam &cam, const QString &dir);
    static Result restoreOne(const Cam &cam, const QString &dir, const QString &fileName,
                             const std::function<void(const QString &)> &say);

    QList<Cam> m_cams;
    QString m_configPath;
    bool m_busy = false;
    QThread *m_worker = nullptr;
    QThread *m_loader = nullptr;
    QThread *m_ruleWorker = nullptr;
    bool m_ruleBusy = false;
    // A load asked for while another is running waits here (only the newest is kept), so it is never lost.
    bool m_hasPending = false;
    QByteArray m_pendingJson;
    QString m_pendingPath;
};

#endif // HIKVISIONBACKUP_H
