#include "loginguard.h"

#include <QHash>
#include <mutex>

namespace {
struct Store {
    std::mutex mutex;
    // "host\nuser" -> the password that was refused. A new password does not match.
    QHash<QString, QString> refusedPassword;
};

Store &store()
{
    static Store s;
    return s;
}
}

LoginGuard &LoginGuard::instance()
{
    static LoginGuard guard;
    return guard;
}

QString LoginGuard::hostKey(const QString &host)
{
    QString h = host.trimmed();
    if (h.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        h = h.mid(7);
    if (h.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        h = h.mid(8);
    const int slash = h.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        h = h.left(slash);
    const int colon = h.indexOf(QLatin1Char(':'));
    if (colon >= 0)
        h = h.left(colon);
    return h;
}

QString LoginGuard::mapKey(const QString &host, const QString &user)
{
    return hostKey(host) + QLatin1Char('\n') + user.trimmed();
}

bool LoginGuard::isRefused(const QString &host, const QString &user, const QString &password) const
{
    if (hostKey(host).isEmpty() || user.trimmed().isEmpty() || password.isEmpty())
        return false;
    std::lock_guard<std::mutex> lock(store().mutex);
    const auto it = store().refusedPassword.constFind(mapKey(host, user));
    return it != store().refusedPassword.cend() && it.value() == password;
}

void LoginGuard::refuse(const QString &host, const QString &user, const QString &password)
{
    if (hostKey(host).isEmpty() || user.trimmed().isEmpty() || password.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(store().mutex);
    store().refusedPassword.insert(mapKey(host, user), password);
}

bool LoginGuard::isHttpAuthRefusal(int status)
{
    return status == 401;
}

bool LoginGuard::isSdkAuthFailure(unsigned long error)
{
    // NET_DVR_PASSWORD_ERROR, NET_DVR_PASSWORD_FORMAT_ERROR, NET_DVR_USER_LOCKED,
    // NET_DVR_IPC_PASSWORD_ERROR. A locked user is included so a later call does not
    // extend the lock.
    return error == 1 || error == 26 || error == 153 || error == 2193;
}

void LoginGuard::clear()
{
    std::lock_guard<std::mutex> lock(store().mutex);
    store().refusedPassword.clear();
}
