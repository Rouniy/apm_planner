#include "comm/GpsCorrectionSource.h"

#include <QtTest>

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QScopedPointer>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace
{
class ReadOnlyCorrectionSource final : public GpsCorrectionSource
{
public:
    using GpsCorrectionSource::GpsCorrectionSource;

    QStringList availablePorts() const override { return {}; }
    bool active() const override { return false; }
    bool connected() const override { return false; }
    bool start(const GpsCorrectionSourceSettings &) override { return false; }
    void stop() override {}
    void setGgaPosition(double, double, double, bool) override {}
};

QTimer *activeSingleShotTimer(const QObject *owner)
{
    const QList<QTimer *> timers = owner->findChildren<QTimer *>();
    for (QTimer *timer : timers) {
        if (timer->isSingleShot() && timer->isActive()) {
            return timer;
        }
    }
    return nullptr;
}

#ifdef Q_OS_UNIX
class PseudoTerminal final
{
public:
    ~PseudoTerminal()
    {
        if (master >= 0) {
            ::close(master);
        }
    }

    bool open()
    {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0 || ::grantpt(master) != 0
            || ::unlockpt(master) != 0) {
            return false;
        }
        const char *name = ::ptsname(master);
        if (!name) {
            return false;
        }
        slave = QString::fromLocal8Bit(name);
        return !slave.isEmpty();
    }

    int master = -1;
    QString slave;
};
#endif
}

class GpsCorrectionSourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void legacySubclassIsReceiverReadOnlyByDefault();
    void watchdogUsesRawSerialButValidRtcmForNtrip();
};

void GpsCorrectionSourceTest::legacySubclassIsReceiverReadOnlyByDefault()
{
    ReadOnlyCorrectionSource source;
    QCOMPARE(source.receiverSession(), quint64(0));
    QVERIFY(!source.canConfigureReceiver());
    QCOMPARE(source.receiverBaudRate(), 0);

    QString error;
    QVERIFY(!source.setReceiverBaudRate(460800, 1, &error));
    QVERIFY(error.contains(QStringLiteral("cannot change")));
    error.clear();
    QVERIFY(!source.writeReceiverData(
        QByteArray::fromHex("b562"), 1, &error));
    QVERIFY(error.contains(QStringLiteral("does not expose")));
}

void GpsCorrectionSourceTest::watchdogUsesRawSerialButValidRtcmForNtrip()
{
#ifndef Q_OS_UNIX
    QSKIP("Pseudo-terminal serial watchdog coverage requires Unix.");
#else
    PseudoTerminal terminal;
    QVERIFY(terminal.open());

    QtGpsCorrectionSource serial;
    GpsCorrectionSourceSettings serialSettings;
    serialSettings.selectedPort = terminal.slave;
    serialSettings.baudRate = 115200;
    QVERIFY(serial.start(serialSettings));
    QVERIFY(serial.connected());
    QTimer *serialWatchdog = activeSingleShotTimer(&serial);
    QVERIFY(serialWatchdog);
    serialWatchdog->start(5000);
    QSignalSpy receiverBytes(&serial, &GpsCorrectionSource::receiverBytes);
    const QByteArray navBytes = QByteArray::fromHex(
        "b562013b00003c8b"); // UBX-shaped status; deliberately not RTCM.
    QCOMPARE(::write(terminal.master, navBytes.constData(),
                     size_t(navBytes.size())), ssize_t(navBytes.size()));
    QTRY_VERIFY_WITH_TIMEOUT(receiverBytes.count() > 0, 1000);
    QVERIFY2(serialWatchdog->remainingTime() > 25000,
             "Raw serial receiver activity did not refresh the watchdog.");
    serial.stop();

    QTcpServer caster;
    QVERIFY(caster.listen(QHostAddress::LocalHost, 0));
    QtGpsCorrectionSource ntrip;
    GpsCorrectionSourceSettings ntripSettings;
    ntripSettings.selectedPort = QStringLiteral("NTRIP");
    ntripSettings.host = QStringLiteral("127.0.0.1");
    ntripSettings.casterPort = caster.serverPort();
    ntripSettings.mountPoint = QStringLiteral("BASE");
    ntripSettings.sendGga = false;
    QVERIFY(ntrip.start(ntripSettings));
    QTRY_VERIFY_WITH_TIMEOUT(caster.hasPendingConnections(), 1000);
    QScopedPointer<QTcpSocket> peer(caster.nextPendingConnection());
    QVERIFY(peer);
    QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() > 0, 1000);
    peer->readAll();
    QCOMPARE(peer->write("HTTP/1.1 200 OK\r\n\r\n"), qint64(19));
    peer->flush();
    QTRY_VERIFY_WITH_TIMEOUT(ntrip.connected(), 1000);
    QTimer *ntripWatchdog = activeSingleShotTimer(&ntrip);
    QVERIFY(ntripWatchdog);
    ntripWatchdog->start(5000);
    QSignalSpy inputBytes(&ntrip, &GpsCorrectionSource::inputBytes);
    QVERIFY(peer->write("not-rtcm") > 0);
    peer->flush();
    QTRY_VERIFY_WITH_TIMEOUT(inputBytes.count() > 0, 1000);
    // Coarse timers can round their deadline above the requested interval.
    // A real refresh calls start(30000); inspect the interval, not that rounding.
    QCOMPARE(ntripWatchdog->interval(), 5000);
    QVERIFY(ntripWatchdog->remainingTime() > 0);
    ntrip.stop();
#endif
}

QTEST_GUILESS_MAIN(GpsCorrectionSourceTest)

#include "test_gpscorrectionsource.moc"
