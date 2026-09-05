#include "ui/AddConnectionViewModel.h"
#include "ui/AddConnectionWindow.h"
#include "ui/ConnectionOptionsViewModel.h"
#include "ui/ConnectionOptionsWindow.h"
#include "globalobject.h"
#include "comm/serialconnection.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

#include "logging.h"

Q_LOGGING_CATEGORY(apmGeneral, "apm.general.connection-options-test")

class ConnectionOptionsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void settingsModelLoadsExactReferenceDefaultsAndFallback();
    void settingsRemainStagedUntilApply();
    void invalidSystemIdNormalizesTo255();
    void settingsWindowMatchesReferenceContract();
    void saveAppliesWithoutClosingAndCloseDiscardsStagedValues();
    void openSettingsWindowBridgesSafeRuntimeValuesToOwner();
    void deferredGcsIdSurvivesGlobalSaveAndLegacyFallback();
    void existingSerialLinkDoesNotOverwriteDefaultBaud();
    void addConnectionCatalogMatchesLegacyReference();
    void addConnectionModelTracksTransportAndBaud();
    void addConnectionWindowRemainsAvailableSeparately();
    void openAddConnectionBridgesRequestToRuntimeOwner();

private:
    QTemporaryDir m_settingsDirectory;
};

class RuntimeOwner final : public QWidget
{
    Q_OBJECT
public slots:
    void applyConnectionOptions(int newBaud, bool newHeartbeat, int newGcsId)
    {
        baud = newBaud;
        heartbeat = newHeartbeat;
        gcsId = newGcsId;
        ++settingsApplyCount;
    }

    void openAdditionalConnection(const QString &newConnection, int newBaud)
    {
        connection = newConnection;
        baud = newBaud;
        ++connectionRequestCount;
    }

public:
    QString connection;
    int baud = 0;
    bool heartbeat = false;
    int gcsId = 0;
    int settingsApplyCount = 0;
    int connectionRequestCount = 0;
};

void ConnectionOptionsWindowTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTest"));
    QCoreApplication::setApplicationName(
        QStringLiteral("ConnectionOptionsWindowTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory.path());
}

void ConnectionOptionsWindowTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void ConnectionOptionsWindowTest::settingsModelLoadsExactReferenceDefaultsAndFallback()
{
    QCOMPARE(ConnectionOptionsViewModel::availableBaudRates(),
             QList<int>({9600, 19200, 38400, 57600, 115200, 230400,
                         460800, 921600}));

    ConnectionOptionsViewModel defaults;
    QCOMPARE(defaults.SelectedBaud(), 115200);
    QVERIFY(defaults.SendGcsHeartbeat());
    QCOMPARE(defaults.GcsSysid(), 255);

    QSettings settings;
    settings.setValue(QStringLiteral("baudrate"), 57600);
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), false);
    settings.setValue(QStringLiteral("GCS_sysid"), 42);
    ConnectionOptionsViewModel legacyFallback;
    QCOMPARE(legacyFallback.SelectedBaud(), 57600);
    QVERIFY(!legacyFallback.SendGcsHeartbeat());
    QCOMPARE(legacyFallback.GcsSysid(), 42);

    settings.setValue(QStringLiteral("gcsid"), 17);
    settings.setValue(QStringLiteral("baudrate"), 12345);
    ConnectionOptionsViewModel canonical;
    QCOMPARE(canonical.SelectedBaud(), 12345);
    QCOMPARE(canonical.GcsSysid(), 17);

    settings.setValue(QStringLiteral("gcsid"), 0);
    ConnectionOptionsViewModel malformed;
    QCOMPARE(malformed.GcsSysid(), 255);
    ConnectionOptionsWindow malformedWindow;
    QCOMPARE(malformedWindow.findChild<QSpinBox *>(
                 QStringLiteral("NUM_gcsid"))->value(), 255);
}

void ConnectionOptionsWindowTest::settingsRemainStagedUntilApply()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baudrate"), 115200);
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), true);
    settings.setValue(QStringLiteral("gcsid"), 255);

    ConnectionOptionsViewModel model;
    model.setSelectedBaud(460800);
    model.setSendGcsHeartbeat(false);
    model.setGcsSysid(23);
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 115200);
    QVERIFY(settings.value(QStringLiteral("CHK_GCSheartbeat")).toBool());
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 255);

    QSignalSpy applied(&model, &ConnectionOptionsViewModel::settingsApplied);
    model.Apply();
    QCOMPARE(applied.count(), 1);
    QCOMPARE(applied.takeFirst(), QVariantList({460800, false, 23}));
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 460800);
    QVERIFY(!settings.value(QStringLiteral("CHK_GCSheartbeat")).toBool());
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 23);
    QCOMPARE(model.Status(), QStringLiteral("Saved."));
}

void ConnectionOptionsWindowTest::invalidSystemIdNormalizesTo255()
{
    QSettings().setValue(QStringLiteral("gcsid"), 32);
    ConnectionOptionsViewModel model;
    QSignalSpy applied(&model, &ConnectionOptionsViewModel::settingsApplied);
    model.setGcsSysid(0);
    model.Apply();
    QCOMPARE(model.GcsSysid(), 255);
    QCOMPARE(QSettings().value(QStringLiteral("gcsid")).toInt(), 255);
    QCOMPARE(applied.takeFirst(), QVariantList({115200, true, 255}));
}

void ConnectionOptionsWindowTest::settingsWindowMatchesReferenceContract()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baudrate"), 230400);
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), false);
    settings.setValue(QStringLiteral("gcsid"), 14);
    ConnectionOptionsWindow window;

    QCOMPARE(window.objectName(), QStringLiteral("ConnectionOptions"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Connection Options"));
    QCOMPARE(window.size(), QSize(340, 230));
    QCOMPARE(window.minimumSize(), QSize(340, 230));
    QCOMPARE(window.maximumSize(), QSize(340, 230));
    QVERIFY(!window.isModal());

    auto *baud = window.findChild<QComboBox *>(QStringLiteral("CMB_baudrate"));
    auto *gcs = window.findChild<QSpinBox *>(QStringLiteral("NUM_gcsid"));
    auto *heartbeat = window.findChild<QCheckBox *>(
        QStringLiteral("CHK_GCSheartbeat"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("LBL_status"));
    auto *restartNote = window.findChild<QLabel *>(
        QStringLiteral("LBL_gcsRestartNote"));
    auto *save = window.findChild<QPushButton *>(QStringLiteral("BUT_save"));
    auto *close = window.findChild<QPushButton *>(QStringLiteral("BUT_close"));
    QVERIFY(baud);
    QVERIFY(gcs);
    QVERIFY(heartbeat);
    QVERIFY(status);
    QVERIFY(restartNote);
    QVERIFY(save);
    QVERIFY(close);
    QCOMPARE(baud->count(), 8);
    QCOMPARE(baud->itemData(0).toInt(), 9600);
    QCOMPARE(baud->itemData(7).toInt(), 921600);
    QCOMPARE(baud->currentData().toInt(), 230400);
    QCOMPARE(gcs->minimum(), 1);
    QCOMPARE(gcs->maximum(), 255);
    QCOMPARE(gcs->value(), 14);
    QVERIFY(!heartbeat->isChecked());
    QCOMPARE(status->text(), QString());
    QCOMPARE(restartNote->text(),
             QStringLiteral("GCS system id changes apply after restart."));
    QVERIFY(restartNote->wordWrap());
    QCOMPARE(save->text(), QStringLiteral("Save"));
    QCOMPARE(close->text(), QStringLiteral("Close"));

    settings.setValue(QStringLiteral("baudrate"), 12345);
    ConnectionOptionsWindow unknownBaud;
    auto *unknownBaudCombo = unknownBaud.findChild<QComboBox *>(
        QStringLiteral("CMB_baudrate"));
    QCOMPARE(unknownBaudCombo->count(), 8);
    QCOMPARE(unknownBaudCombo->currentIndex(), -1);
    QCOMPARE(unknownBaud.viewModel()->SelectedBaud(), 12345);
}

void ConnectionOptionsWindowTest::saveAppliesWithoutClosingAndCloseDiscardsStagedValues()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baudrate"), 115200);
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), true);
    settings.setValue(QStringLiteral("gcsid"), 255);
    ConnectionOptionsWindow window;
    auto *baud = window.findChild<QComboBox *>(QStringLiteral("CMB_baudrate"));
    auto *gcs = window.findChild<QSpinBox *>(QStringLiteral("NUM_gcsid"));
    auto *heartbeat = window.findChild<QCheckBox *>(
        QStringLiteral("CHK_GCSheartbeat"));
    auto *save = window.findChild<QPushButton *>(QStringLiteral("BUT_save"));
    auto *close = window.findChild<QPushButton *>(QStringLiteral("BUT_close"));
    QVERIFY(baud && gcs && heartbeat && save && close);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    baud->setCurrentIndex(baud->findData(921600));
    gcs->setValue(31);
    heartbeat->setChecked(false);
    QSignalSpy applied(&window, &ConnectionOptionsWindow::settingsApplied);
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(applied.count(), 1);
    QVERIFY(window.isVisible());
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 921600);

    baud->setCurrentIndex(baud->findData(9600));
    gcs->setValue(50);
    heartbeat->setChecked(true);
    QTest::mouseClick(close, Qt::LeftButton);
    QVERIFY(!window.isVisible());
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 921600);
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 31);
    QVERIFY(!settings.value(QStringLiteral("CHK_GCSheartbeat")).toBool());
}

void ConnectionOptionsWindowTest::openSettingsWindowBridgesSafeRuntimeValuesToOwner()
{
    RuntimeOwner owner;
    QPointer<ConnectionOptionsWindow> first(
        ConnectionOptionsWindow::OpenWindow(&owner));
    QPointer<ConnectionOptionsWindow> second(
        ConnectionOptionsWindow::OpenWindow(&owner));
    QVERIFY(first && second && first != second);
    auto *baud = first->findChild<QComboBox *>(QStringLiteral("CMB_baudrate"));
    auto *heartbeat = first->findChild<QCheckBox *>(
        QStringLiteral("CHK_GCSheartbeat"));
    auto *save = first->findChild<QPushButton *>(QStringLiteral("BUT_save"));
    baud->setCurrentIndex(baud->findData(38400));
    heartbeat->setChecked(false);
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(owner.settingsApplyCount, 1);
    QCOMPARE(owner.baud, 38400);
    QVERIFY(!owner.heartbeat);
    QCOMPARE(owner.gcsId, 255);

    first->close();
    second->close();
    QTRY_VERIFY(first.isNull());
    QTRY_VERIFY(second.isNull());
}

void ConnectionOptionsWindowTest::deferredGcsIdSurvivesGlobalSaveAndLegacyFallback()
{
    QSettings settings;
    settings.setValue(QStringLiteral("GCS_sysid"), 41);
    GlobalObject *global = GlobalObject::sharedInstance();
    global->loadSettings();
    QCOMPARE(global->MavlinkID(), quint8(41));

    settings.remove(QStringLiteral("GCS_sysid"));
    settings.setValue(QStringLiteral("gcsid"), 255);
    global->loadSettings();
    QCOMPARE(global->MavlinkID(), quint8(255));

    ConnectionOptionsViewModel model;
    model.setGcsSysid(23);
    model.Apply();
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 23);
    QCOMPARE(global->MavlinkID(), quint8(255));

    // QGCCore performs this unrelated global save during clean shutdown.
    global->saveSettings();
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 23);
    global->loadSettings();
    QCOMPARE(global->MavlinkID(), quint8(23));
}

void ConnectionOptionsWindowTest::existingSerialLinkDoesNotOverwriteDefaultBaud()
{
    QSettings settings;
    settings.setValue(QStringLiteral("baudrate"), 57600);
    settings.setValue(QStringLiteral("SERIALLINK_COMM_PORT"),
                      QStringLiteral("ttyAPMTEST"));
    settings.setValue(QStringLiteral("SERIALLINK_COMM_PORTMAP"),
                      QStringLiteral("ttyAPMTEST:115200"));

    SerialConnection link;
    QCOMPARE(link.getPortName(), QStringLiteral("ttyAPMTEST"));
    QCOMPARE(link.getBaudRate(), 115200);
    QVERIFY(link.setBaudRate(230400));
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 57600);
    QVERIFY(settings.value(QStringLiteral("SERIALLINK_COMM_PORTMAP"))
                .toString().contains(QStringLiteral("ttyAPMTEST:230400")));
}

void ConnectionOptionsWindowTest::addConnectionCatalogMatchesLegacyReference()
{
    const QStringList connections =
        AddConnectionViewModel::availableConnections(
            {QStringLiteral("ttyUSB0"), QStringLiteral(" COM9 "),
             QStringLiteral("ttyUSB0"), QString()});
    QCOMPARE(connections,
             QStringList({QStringLiteral("ttyUSB0"), QStringLiteral("COM9"),
                          QStringLiteral("TCP"), QStringLiteral("UDP"),
                          QStringLiteral("UDPCl"), QStringLiteral("WS")}));
    QCOMPARE(AddConnectionViewModel::availableBaudRates(),
             QList<int>({1200, 2400, 4800, 9600, 19200, 28800, 38400,
                         57600, 111100, 115200, 230400, 460800, 500000,
                         625000, 921600, 1000000, 1500000}));
}

void ConnectionOptionsWindowTest::addConnectionModelTracksTransportAndBaud()
{
    AddConnectionViewModel model({QStringLiteral("ttyACM0")});
    QCOMPARE(model.SelectedConnection(), QStringLiteral("ttyACM0"));
    QCOMPARE(model.SelectedBaud(), 115200);
    QVERIFY(model.BaudEnabled());

    QSignalSpy changed(&model, &AddConnectionViewModel::BaudEnabledChanged);
    model.setSelectedConnection(QStringLiteral("TCP"));
    QVERIFY(!model.BaudEnabled());
    QCOMPARE(changed.count(), 1);
    model.setSelectedConnection(QStringLiteral("UDPCl"));
    QCOMPARE(changed.count(), 1);
    model.setSelectedConnection(QStringLiteral("ttyACM0"));
    QVERIFY(model.BaudEnabled());
    QCOMPARE(changed.count(), 2);
    model.setSelectedBaud(921600);
    QCOMPARE(model.SelectedBaud(), 921600);
    model.setSelectedBaud(12345);
    QCOMPARE(model.SelectedBaud(), 921600);
}

void ConnectionOptionsWindowTest::addConnectionWindowRemainsAvailableSeparately()
{
    AddConnectionWindow window(
        {QStringLiteral("ttyUSB0"), QStringLiteral("ttyACM0")});
    QCOMPARE(window.objectName(), QStringLiteral("AddConnection"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Add Connection"));
    QCOMPARE(window.size(), QSize(228, 75));
    QVERIFY(!window.isModal());
    auto *connection = window.findChild<QComboBox *>(
        QStringLiteral("CMB_serialport"));
    auto *baud = window.findChild<QComboBox *>(QStringLiteral("CMB_baudrate"));
    auto *connectButton = window.findChild<QPushButton *>(
        QStringLiteral("BUT_connect"));
    QVERIFY(connection && baud && connectButton);
    QCOMPARE(connection->count(), 6);
    QCOMPARE(baud->count(), 17);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    baud->setCurrentIndex(baud->findData(460800));
    QSignalSpy requested(&window, &AddConnectionWindow::connectRequested);
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(requested.takeFirst(),
             QVariantList({QStringLiteral("ttyUSB0"), 460800}));
    QVERIFY(window.isVisible());
}

void ConnectionOptionsWindowTest::openAddConnectionBridgesRequestToRuntimeOwner()
{
    RuntimeOwner owner;
    QPointer<AddConnectionWindow> window(
        AddConnectionWindow::OpenWindow(&owner));
    QVERIFY(window);
    auto *connection = window->findChild<QComboBox *>(
        QStringLiteral("CMB_serialport"));
    auto *connectButton = window->findChild<QPushButton *>(
        QStringLiteral("BUT_connect"));
    QVERIFY(connection && connectButton);
    connection->setCurrentText(QStringLiteral("TCP"));
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(owner.connectionRequestCount, 1);
    QCOMPARE(owner.connection, QStringLiteral("TCP"));
    QCOMPARE(owner.baud, 115200);
    window->close();
    QTRY_VERIFY(window.isNull());
}

QTEST_MAIN(ConnectionOptionsWindowTest)
#include "test_connectionoptionswindow.moc"
