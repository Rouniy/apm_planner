#include "comm/MovingBaseInputTransport.h"
#include "comm/MovingBasePositionStore.h"
#include "comm/MovingBaseService.h"
#include "comm/NmeaGgaParser.h"
#include "comm/VehicleTargetManager.h"
#include "ui/MovingBaseWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

namespace
{
VehicleEndpoint endpoint(int linkId, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Test modem");
    result.componentName = QStringLiteral("AUTOPILOT1");
    return result;
}

QByteArray ggaSentence()
{
    const QString sentence = QStringLiteral(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    return (sentence + QLatin1Char('*')
            + NmeaGgaParser::checksum(sentence)
            + QStringLiteral("\r\n")).toLatin1();
}

quint16 unusedUdpPort()
{
    QUdpSocket reservation;
    if (!reservation.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        return 0;
    }
    return reservation.localPort();
}

struct WindowRig
{
    VehicleTargetManager targets;
    MovingBasePositionStore store;
    MovingBaseService service;
    QVariantMap settings;
    QTemporaryDir logDirectory;
    int writeCount = 0;
    int transportCount = 0;
    QString udpConflict;
    MovingBaseInputTransport::Settings transportSettings;

    WindowRig()
        : store(&targets)
        , service(&targets, &store)
    {
        targets.observeEndpoint(endpoint(7), true);
    }

    MovingBaseWindow::Dependencies dependencies()
    {
        MovingBaseWindow::Dependencies result;
        result.targetManager = &targets;
        result.service = &service;
        result.transportFactory = [this](
            const MovingBaseInputTransport::Settings &settings,
            QObject *parent) {
            ++transportCount;
            transportSettings = settings;
            return new MovingBaseInputTransport(settings, parent);
        };
        result.enumeratePorts = []() {
            return QStringList{QStringLiteral("TEST-NMEA")};
        };
        result.validateSerial = [](const QString &) { return QString(); };
        result.validateUdpHostPort = [this](quint16) {
            return udpConflict;
        };
        result.readSetting = [this](
            const QString &key, const QVariant &fallback) {
            return settings.value(key, fallback);
        };
        result.writeSettings = [this](
            const QVariantMap &values, QString *) {
            ++writeCount;
            for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
                settings.insert(it.key(), it.value());
            }
            return true;
        };
        result.rawLogPath = logDirectory.filePath(
            QStringLiteral("MovingBase.txt"));
        return result;
    }
};

QPushButton *toggle(MovingBaseWindow *window)
{
    return window->findChild<QPushButton *>(
        QStringLiteral("ToggleMovingBaseButton"));
}

void selectUdpHost(MovingBaseWindow *window, quint16 port)
{
    auto *input = window->findChild<QComboBox *>(
        QStringLiteral("movingBaseInput"));
    auto *portSpin = window->findChild<QSpinBox *>(
        QStringLiteral("movingBasePort"));
    QVERIFY(input);
    QVERIFY(portSpin);
    const int index = input->findText(QStringLiteral("UDP Host"));
    QVERIFY(index >= 0);
    input->setCurrentIndex(index);
    portSpin->setValue(port);
}
}

class MovingBaseWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void rendersCompleteModelessSurface();
    void udpFixUpdatesStorePersistsAndTargetChangeStops();
    void udpHostPortConflictFailsBeforeReservationAndTransportOpen();
    void sharedServiceMakesSecondWindowTruthfullyBusy();
};

void MovingBaseWindowTest::rendersCompleteModelessSurface()
{
    WindowRig rig;
    QWidget owner;
    auto *window = new MovingBaseWindow(rig.dependencies(), &owner);
    QPointer<MovingBaseWindow> guard(window);

    QCOMPARE(window->objectName(), QStringLiteral("MovingBaseWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("Moving Base"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->parentWidget(), &owner);
    QVERIFY(window->isWindow());
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(window->size(), QSize(MovingBaseWindow::WindowWidth,
                                   MovingBaseWindow::WindowHeight));
    QCOMPARE(window->minimumSize(),
             QSize(MovingBaseWindow::MinimumWindowWidth,
                   MovingBaseWindow::MinimumWindowHeight));
    QCOMPARE(window->statusText(), QStringLiteral("Stopped."));
    QCOMPARE(window->locationText(),
             QStringLiteral("No moving-base fix received."));

    const QStringList controls = {
        QStringLiteral("movingBaseHeader"),
        QStringLiteral("movingBaseDescription"),
        QStringLiteral("movingBaseTargetDescription"),
        QStringLiteral("movingBaseInput"),
        QStringLiteral("movingBaseRefreshInputs"),
        QStringLiteral("movingBaseBaud"),
        QStringLiteral("movingBaseHost"),
        QStringLiteral("movingBasePort"),
        QStringLiteral("movingBaseRate"),
        QStringLiteral("movingBaseRelativeAltitude"),
        QStringLiteral("movingBaseUpdateRally"),
        QStringLiteral("ToggleMovingBaseButton"),
        QStringLiteral("movingBaseStatusPanel"),
        QStringLiteral("movingBaseStatus"),
        QStringLiteral("movingBaseLocation")
    };
    for (const QString &name : controls) {
        QVERIFY2(window->findChild<QWidget *>(name), qPrintable(name));
    }
    auto *relative = window->findChild<QCheckBox *>(
        QStringLiteral("movingBaseRelativeAltitude"));
    auto *rally = window->findChild<QCheckBox *>(
        QStringLiteral("movingBaseUpdateRally"));
    QVERIFY(relative && !relative->isEnabled());
    QVERIFY(rally && !rally->isEnabled());
    QVERIFY(!relative->toolTip().isEmpty());
    QVERIFY(!rally->toolTip().isEmpty());

    window->show();
    QTRY_VERIFY(window->isVisible());
    window->close();
    QTRY_VERIFY(guard.isNull());
}

void MovingBaseWindowTest::
udpFixUpdatesStorePersistsAndTargetChangeStops()
{
    WindowRig rig;
    QVERIFY(rig.logDirectory.isValid());
    QVERIFY(rig.targets.observeEndpoint(endpoint(9)));
    const quint16 port = unusedUdpPort();
    QVERIFY(port != 0);
    auto *window = new MovingBaseWindow(rig.dependencies());
    QPointer<MovingBaseWindow> guard(window);
    selectUdpHost(window, port);

    toggle(window)->click();
    QVERIFY(window->isRunning());
    QCOMPARE(rig.transportSettings.host, QStringLiteral("0.0.0.0"));
    QCOMPARE(toggle(window)->text(), QStringLiteral("Stop"));
    QTRY_COMPARE(rig.writeCount, 1);
    QCOMPARE(rig.settings.value(QStringLiteral("MovingBaseInput")).toString(),
             QStringLiteral("UDP Host"));
    QCOMPARE(rig.settings.value(QStringLiteral("MovingBasePort")).toInt(),
             static_cast<int>(port));
    QCOMPARE(rig.settings.value(
                 QStringLiteral("MovingBaseUpdateRally")).toBool(), false);

    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(
                 ggaSentence(), QHostAddress::LocalHost, port),
             qint64(ggaSentence().size()));
    QTRY_VERIFY(rig.store.currentSnapshot().isValid());
    const MovingBasePositionSnapshot snapshot = rig.store.currentSnapshot();
    QCOMPARE(snapshot.fix.latitudeDegrees, 48.1173);
    QCOMPARE(snapshot.fix.longitudeDegrees, 11.516666666666667);
    QCOMPARE(snapshot.fix.altitudeAmslMetres, 545.4);
    QVERIFY(window->locationText().contains(QStringLiteral("545.4 m AMSL")));

    QVERIFY(rig.targets.selectTarget(9, 42, 1));
    QTRY_VERIFY(!window->isRunning());
    QVERIFY(!rig.store.currentSnapshot().isValid());
    QCOMPARE(toggle(window)->text(), QStringLiteral("Connect"));
    QVERIFY(window->statusText().contains(QStringLiteral("stopped"),
                                          Qt::CaseInsensitive));

    QFile log(rig.logDirectory.filePath(QStringLiteral("MovingBase.txt")));
    QVERIFY(log.open(QIODevice::ReadOnly));
    QVERIFY(log.readAll().contains(QByteArray("$GPGGA")));
    window->close();
    QTRY_VERIFY(guard.isNull());
}

void MovingBaseWindowTest::
udpHostPortConflictFailsBeforeReservationAndTransportOpen()
{
    WindowRig rig;
    rig.udpConflict = QStringLiteral(
        "UDP port is used by an active vehicle link.");
    auto *window = new MovingBaseWindow(rig.dependencies());
    QPointer<MovingBaseWindow> guard(window);
    selectUdpHost(window, unusedUdpPort());

    toggle(window)->click();

    QVERIFY(!window->isRunning());
    QCOMPARE(rig.service.state(), MovingBaseService::State::Idle);
    QCOMPARE(rig.transportCount, 0);
    QCOMPARE(rig.writeCount, 0);
    QCOMPARE(window->statusText(), rig.udpConflict);
    window->close();
    QTRY_VERIFY(guard.isNull());
}

void MovingBaseWindowTest::sharedServiceMakesSecondWindowTruthfullyBusy()
{
    WindowRig rig;
    const quint16 firstPort = unusedUdpPort();
    const quint16 secondPort = unusedUdpPort();
    QVERIFY(firstPort != 0);
    QVERIFY(secondPort != 0);
    auto *first = new MovingBaseWindow(rig.dependencies());
    auto *second = new MovingBaseWindow(rig.dependencies());
    selectUdpHost(first, firstPort);
    selectUdpHost(second, secondPort);

    toggle(first)->click();
    QVERIFY(first->isRunning());
    toggle(second)->click();
    QVERIFY(!second->isRunning());
    QVERIFY(second->statusText().contains(QStringLiteral("another window"),
                                          Qt::CaseInsensitive));

    toggle(first)->click();
    QVERIFY(!first->isRunning());
    toggle(second)->click();
    QVERIFY(second->isRunning());
    toggle(second)->click();
    first->close();
    second->close();
}

QTEST_MAIN(MovingBaseWindowTest)

#include "test_movingbasewindow.moc"
