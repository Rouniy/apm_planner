#include "ui/ConnectionOptionsViewModel.h"
#include "ui/ConnectionOptionsWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

class ConnectionOptionsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndCanonicalPrecedence();
    void legacyFallbacksAreMigratable();
    void applyPersistsAndDefendsSystemId();
    void windowMatchesReferenceContract();
    void saveDoesNotCloseAndCloseDoesNotSave();
    void openWindowBridgesApplyToRuntimeOwner();
};

class RuntimeOwner final : public QWidget
{
    Q_OBJECT
public slots:
    void applyConnectionOptions(int newBaud, bool newHeartbeat,
                                int newSystemId)
    {
        baud = newBaud;
        heartbeat = newHeartbeat;
        systemId = newSystemId;
        ++applyCount;
    }

public:
    int baud = 0;
    bool heartbeat = false;
    int systemId = 0;
    int applyCount = 0;
};

void ConnectionOptionsWindowTest::defaultsAndCanonicalPrecedence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);

    ConnectionOptionsViewModel defaults(&settings);
    QCOMPARE(defaults.Bauds(),
             QList<int>({9600, 19200, 38400, 57600, 115200, 230400,
                         460800, 921600}));
    QCOMPARE(defaults.SelectedBaud(), 115200);
    QCOMPARE(defaults.GcsSysid(), 255);
    QVERIFY(defaults.SendGcsHeartbeat());

    settings.setValue(QStringLiteral("baudrate"), 230400);
    settings.setValue(QStringLiteral("CHK_GCSheartbeat"), false);
    settings.setValue(QStringLiteral("gcsid"), 42);
    settings.setValue(QStringLiteral("GCS_sysid"), 43);
    settings.setValue(QStringLiteral("GLOBAL_SETTINGS/MAVLINK_ID"), 44);
    settings.sync();

    ConnectionOptionsViewModel canonical(&settings);
    QCOMPARE(canonical.SelectedBaud(), 230400);
    QCOMPARE(canonical.GcsSysid(), 42);
    QVERIFY(!canonical.SendGcsHeartbeat());

    settings.setValue(QStringLiteral("gcsid"), 0);
    settings.setValue(QStringLiteral("baudrate"), 0);
    settings.sync();
    ConnectionOptionsViewModel invalidCanonical(&settings);
    QCOMPARE(invalidCanonical.GcsSysid(), 255);
    QCOMPARE(invalidCanonical.SelectedBaud(), 115200);
}

void ConnectionOptionsWindowTest::legacyFallbacksAreMigratable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("SERIALLINK_COMM_BAUD"), 57600);
    settings.setValue(
        QStringLiteral("QGC_MAINWINDOW/HEARTBEATS_ENABLED"), false);
    settings.setValue(QStringLiteral("GCS_sysid"), 17);
    settings.sync();

    ConnectionOptionsViewModel model(&settings);
    QCOMPARE(model.SelectedBaud(), 57600);
    QCOMPARE(model.GcsSysid(), 17);
    QVERIFY(!model.SendGcsHeartbeat());

    settings.remove(QStringLiteral("GCS_sysid"));
    settings.setValue(QStringLiteral("GLOBAL_SETTINGS/MAVLINK_ID"), 31);
    settings.sync();
    ConnectionOptionsViewModel apmLegacy(&settings);
    QCOMPARE(apmLegacy.GcsSysid(), 31);
}

void ConnectionOptionsWindowTest::applyPersistsAndDefendsSystemId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    ConnectionOptionsViewModel model(&settings);
    model.setSelectedBaud(460800);
    model.setSendGcsHeartbeat(false);
    model.setGcsSysid(0);
    QSignalSpy applied(&model,
                       &ConnectionOptionsViewModel::settingsApplied);

    QVERIFY(model.Apply());
    QCOMPARE(model.GcsSysid(), 255);
    QCOMPARE(model.Status(), QStringLiteral("Saved."));
    QCOMPARE(applied.count(), 1);
    QCOMPARE(applied.takeFirst(),
             QVariantList({460800, false, 255}));

    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 460800);
    QVERIFY(!settings.value(
        QStringLiteral("CHK_GCSheartbeat")).toBool());
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 255);
    QCOMPARE(settings.value(
        QStringLiteral("GLOBAL_SETTINGS/MAVLINK_ID")).toInt(), 255);
    QVERIFY(!settings.value(
        QStringLiteral("QGC_MAINWINDOW/HEARTBEATS_ENABLED")).toBool());
}

void ConnectionOptionsWindowTest::windowMatchesReferenceContract()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    ConnectionOptionsWindow window(&settings);

    QCOMPARE(window.objectName(), QStringLiteral("ConnectionOptionsWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Connection Options"));
    QCOMPARE(window.size(), QSize(340, 230));
    QCOMPARE(window.minimumSize(), QSize(340, 230));
    QCOMPARE(window.maximumSize(), QSize(340, 230));
    QVERIFY(!window.isModal());

    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("SelectedBaud"));
    auto *systemId = window.findChild<QSpinBox *>(
        QStringLiteral("GcsSysid"));
    auto *heartbeat = window.findChild<QCheckBox *>(
        QStringLiteral("SendGcsHeartbeat"));
    auto *baudLabel = window.findChild<QLabel *>(
        QStringLiteral("DefaultBaudRateLabel"));
    auto *systemIdLabel = window.findChild<QLabel *>(
        QStringLiteral("GcsSystemIdLabel"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("Status"));
    auto *save = window.findChild<QPushButton *>(
        QStringLiteral("ApplyCommand"));
    auto *close = window.findChild<QPushButton *>(QStringLiteral("OnClose"));
    QVERIFY(baud);
    QVERIFY(systemId);
    QVERIFY(heartbeat);
    QVERIFY(baudLabel);
    QVERIFY(systemIdLabel);
    QVERIFY(status);
    QVERIFY(save);
    QVERIFY(close);
    QCOMPARE(baud->count(), 8);
    QCOMPARE(baud->itemData(0).toInt(), 9600);
    QCOMPARE(baud->itemData(7).toInt(), 921600);
    QCOMPARE(baud->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
    QCOMPARE(baudLabel->text(), QStringLiteral("Default baud rate"));
    QCOMPARE(systemIdLabel->text(), QStringLiteral("GCS system id"));
    QCOMPARE(systemId->minimum(), 1);
    QCOMPARE(systemId->maximum(), 255);
    QCOMPARE(systemId->singleStep(), 1);
    QCOMPARE(heartbeat->text(), QStringLiteral("Send GCS heartbeat"));
    QCOMPARE(save->text(), QStringLiteral("Save"));
    QCOMPARE(close->text(), QStringLiteral("Close"));
    QVERIFY(save->minimumWidth() >= 80);
    QVERIFY(close->minimumWidth() >= 80);
}

void ConnectionOptionsWindowTest::saveDoesNotCloseAndCloseDoesNotSave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("baudrate"), 115200);
    settings.sync();

    ConnectionOptionsWindow window(&settings);
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("SelectedBaud"));
    auto *systemId = window.findChild<QSpinBox *>(
        QStringLiteral("GcsSysid"));
    auto *save = window.findChild<QPushButton *>(
        QStringLiteral("ApplyCommand"));
    auto *close = window.findChild<QPushButton *>(QStringLiteral("OnClose"));
    QVERIFY(baud);
    QVERIFY(systemId);
    QVERIFY(save);
    QVERIFY(close);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    baud->setCurrentIndex(baud->findData(230400));
    systemId->setValue(77);
    QSignalSpy applied(&window, &ConnectionOptionsWindow::settingsApplied);
    QTest::mouseClick(save, Qt::LeftButton);
    QVERIFY(window.isVisible());
    QCOMPARE(applied.count(), 1);
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 230400);
    QCOMPARE(settings.value(QStringLiteral("gcsid")).toInt(), 77);

    baud->setCurrentIndex(baud->findData(921600));
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(!window.isVisible());
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("baudrate")).toInt(), 230400);
}

void ConnectionOptionsWindowTest::openWindowBridgesApplyToRuntimeOwner()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    RuntimeOwner owner;
    QPointer<ConnectionOptionsWindow> window(
        ConnectionOptionsWindow::OpenWindow(&owner, &settings));
    QVERIFY(window);

    auto *baud = window->findChild<QComboBox *>(
        QStringLiteral("SelectedBaud"));
    auto *systemId = window->findChild<QSpinBox *>(
        QStringLiteral("GcsSysid"));
    auto *heartbeat = window->findChild<QCheckBox *>(
        QStringLiteral("SendGcsHeartbeat"));
    auto *save = window->findChild<QPushButton *>(
        QStringLiteral("ApplyCommand"));
    QVERIFY(baud);
    QVERIFY(systemId);
    QVERIFY(heartbeat);
    QVERIFY(save);

    baud->setCurrentIndex(baud->findData(921600));
    systemId->setValue(88);
    heartbeat->setChecked(true);
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(owner.applyCount, 1);
    QCOMPARE(owner.baud, 921600);
    QVERIFY(owner.heartbeat);
    QCOMPARE(owner.systemId, 88);

    window->close();
    QTRY_VERIFY(window.isNull());
}

QTEST_MAIN(ConnectionOptionsWindowTest)
#include "test_connectionoptionswindow.moc"
