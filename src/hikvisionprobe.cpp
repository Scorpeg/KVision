#include "hikvisionprobe.h"

#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QUuid>
#include <QXmlStreamReader>
#include <QDebug>

namespace {
const char kMulticastGroup[] = "239.255.255.250";
const quint16 kProbePort = 37020;
const int kScanMs = 4000;

// The device type code is not a reliable NVR flag, so judge by what the device
// reports: several channels, or a recorder-style model name.
bool looksLikeRecorder(const QString &model, int analog, int digital)
{
    if (analog + digital > 1)
        return true;
    const QString m = model.toUpper();
    return m.contains(QLatin1String("NVR")) || m.contains(QLatin1String("DVR"))
        || m.startsWith(QLatin1String("DS-7")) || m.startsWith(QLatin1String("DS-8"))
        || m.startsWith(QLatin1String("DS-9")) || m.startsWith(QLatin1String("IDS-7"))
        || m.startsWith(QLatin1String("IDS-9"));
}
}

HikvisionProbe::HikvisionProbe(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kScanMs);
    connect(&m_timer, &QTimer::timeout, this, &HikvisionProbe::finish);
}

HikvisionProbe::~HikvisionProbe()
{
    stop();
}

void HikvisionProbe::start()
{
    if (m_scanning)
        return;

    m_seen.clear();
    m_count = 0;
    m_failed = 0;
    m_uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();

    const QByteArray probe = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><Probe><Uuid>%1</Uuid><Types>inquiry</Types></Probe>")
        .arg(m_uuid).toUtf8();

    // Interfaces are enumerated on every scan so a newly plugged network is picked up.
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack) || !(flags & QNetworkInterface::CanMulticast))
            continue;

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            addChannel(iface, entry, probe);
        }
    }

    m_scanning = true;
    emit scanningChanged();

    if (m_channels.isEmpty()) {
        // Nothing to scan on; report right away (queued so callers see scanning=true first).
        QTimer::singleShot(0, this, &HikvisionProbe::finish);
        return;
    }
    m_timer.start();
}

void HikvisionProbe::addChannel(const QNetworkInterface &iface, const QNetworkAddressEntry &entry, const QByteArray &probe)
{
    const QHostAddress addr = entry.ip();
    auto *socket = new QUdpSocket(this);
    // Ephemeral port bound to this interface's address: the replies are unicast back to it.
    if (!socket->bind(addr, 0, QAbstractSocket::DontShareAddress)) {
        qWarning() << "[Probe] bind failed on" << iface.name() << addr.toString() << socket->errorString();
        delete socket;
        ++m_failed;
        return;
    }
    socket->setMulticastInterface(iface);
    socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 2);

    if (socket->writeDatagram(probe, QHostAddress(QLatin1String(kMulticastGroup)), kProbePort) < 0) {
        qWarning() << "[Probe] send failed on" << iface.name() << socket->errorString();
        delete socket;
        ++m_failed;
        return;
    }

    Channel ch;
    ch.socket = socket;
    ch.name = iface.name();
    ch.ip = addr.toIPv4Address();
    ch.mask = entry.netmask().toIPv4Address();
    m_channels.append(ch);

    connect(socket, &QUdpSocket::readyRead, this, [this, socket]() {
        for (const Channel &c : qAsConst(m_channels)) {
            if (c.socket == socket) {
                readPending(c);
                return;
            }
        }
    });
}

void HikvisionProbe::stop()
{
    m_timer.stop();
    for (const Channel &c : qAsConst(m_channels)) {
        if (c.socket) {
            c.socket->close();
            c.socket->deleteLater();
        }
    }
    m_channels.clear();
    if (m_scanning) {
        m_scanning = false;
        emit scanningChanged();
    }
}

void HikvisionProbe::finish()
{
    if (!m_scanning)
        return;
    const int networks = m_channels.size();
    const int count = m_count;
    const int failed = m_failed;
    stop();
    emit scanFinished(count, networks, failed);
}

void HikvisionProbe::readPending(const Channel &channel)
{
    while (channel.socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = channel.socket->receiveDatagram();
        QVariantMap dev = parseProbeMatch(dg.data());
        if (dev.isEmpty())
            continue;

        // The same device can answer on both networks; list it once.
        const QString mac = dev.value(QStringLiteral("mac")).toString();
        const QString serial = dev.value(QStringLiteral("serial")).toString();
        const QString key = !mac.isEmpty() ? mac : (!serial.isEmpty() ? serial : dev.value(QStringLiteral("ip")).toString());
        if (m_seen.contains(key))
            continue;
        m_seen.insert(key);

        const quint32 devIp = QHostAddress(dev.value(QStringLiteral("ip")).toString()).toIPv4Address();
        dev.insert(QStringLiteral("interfaceName"), channel.name);
        dev.insert(QStringLiteral("interfaceIp"), QHostAddress(channel.ip).toString());
        dev.insert(QStringLiteral("sameSubnet"), devIp != 0 && (devIp & channel.mask) == (channel.ip & channel.mask));

        ++m_count;
        emit deviceFound(dev);
    }
}

QVariantMap HikvisionProbe::parseProbeMatch(const QByteArray &xml)
{
    QVariantMap f;
    QXmlStreamReader r(xml);
    bool isMatch = false;
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement())
            continue;
        const QString tag = r.name().toString();
        if (!isMatch) {
            if (tag != QLatin1String("ProbeMatch"))
                return QVariantMap(); // not a reply (e.g. another host's Probe)
            isMatch = true;
            continue;
        }
        f.insert(tag, r.readElementText().trimmed());
    }
    if (!isMatch)
        return QVariantMap();

    const QString ip = f.value(QStringLiteral("IPv4Address")).toString();
    if (ip.isEmpty() || QHostAddress(ip).isNull())
        return QVariantMap();

    const int analog = f.value(QStringLiteral("AnalogChannelNum")).toInt();
    const int digital = f.value(QStringLiteral("DigitalChannelNum")).toInt();
    const QString model = f.value(QStringLiteral("DeviceDescription")).toString();
    const int sdk = f.value(QStringLiteral("CommandPort")).toInt();
    const int http = f.value(QStringLiteral("HttpPort")).toInt();

    QVariantMap d;
    d.insert(QStringLiteral("ip"), ip);
    d.insert(QStringLiteral("name"), model);
    d.insert(QStringLiteral("serial"), f.value(QStringLiteral("DeviceSN")).toString());
    d.insert(QStringLiteral("mac"), f.value(QStringLiteral("MAC")).toString().toLower());
    d.insert(QStringLiteral("kind"), looksLikeRecorder(model, analog, digital) ? QStringLiteral("nvr") : QStringLiteral("camera"));
    d.insert(QStringLiteral("sdkPort"), sdk > 0 ? sdk : 8000);
    d.insert(QStringLiteral("httpPort"), http > 0 ? http : 80);
    // Older firmware omits Activated; treat that as activated rather than warn wrongly.
    d.insert(QStringLiteral("activated"), f.value(QStringLiteral("Activated")).toString().compare(QLatin1String("false"), Qt::CaseInsensitive) != 0);
    d.insert(QStringLiteral("firmware"), f.value(QStringLiteral("SoftwareVersion")).toString());
    d.insert(QStringLiteral("channels"), analog + digital);
    d.insert(QStringLiteral("subnetMask"), f.value(QStringLiteral("IPv4SubnetMask")).toString());
    d.insert(QStringLiteral("gateway"), f.value(QStringLiteral("IPv4Gateway")).toString());
    return d;
}
