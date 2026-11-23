#ifndef LOGINGUARD_H
#define LOGINGUARD_H

#include <QString>

// One "this password was refused" flag for the whole process.
// A wrong password is tried once. A different password is tried again.
// A timeout, a dropped connection, a missing page, or HTTP 403 is not a refusal.
// HTTP 403 is how these cameras reject one address (the detection stream on a PTZ)
// while the same password still opens the live picture and the SDK login.
// Only HTTP 401 or an SDK user/password error records a refusal.
class LoginGuard
{
public:
    static LoginGuard &instance();

    // True when this host, user and password were refused earlier in this process.
    bool isRefused(const QString &host, const QString &user, const QString &password) const;

    void refuse(const QString &host, const QString &user, const QString &password);

    // SDK NET_DVR_GetLastError values that mean the account or the password was rejected.
    static bool isSdkAuthFailure(unsigned long error);

    // HTTP 401 is a rejected password. HTTP 403 is not.
    static bool isHttpAuthRefusal(int status);

    void clear();

private:
    static QString hostKey(const QString &host);
    static QString mapKey(const QString &host, const QString &user);
};

#endif
