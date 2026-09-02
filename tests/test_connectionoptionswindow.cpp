#include "ui/ConnectionOptionsViewModel.h"
#include "ui/ConnectionOptionsWindow.h"

#include <QComboBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

class ConnectionOptionsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void catalogMatchesReferenceAndDeduplicatesPorts();
    void viewModelTracksTransportAndBaud();
    void windowMatchesReferenceContract();
    void connectEmitsRequestAndKeepsWindowOpen();
    void openWindowBridgesRequestToRuntimeOwner();
};

class RuntimeOwner final : public QWidget
{
    Q_OBJECT
public slots:
    void openAdditionalConnection(const QString &newConnection, int newBaud)
    {
        connection = newConnection;
        baud = newBaud;
        ++requestCount;
    }

public:
    QString connection;
    int baud = 0;
    int requestCount = 0;
};

void ConnectionOptionsWindowTest::catalogMatchesReferenceAndDeduplicatesPorts()
{
    const QStringList connections =
        ConnectionOptionsViewModel::availableConnections(
            {QStringLiteral("ttyUSB0"), QStringLiteral(" COM9 "),
             QStringLiteral("ttyUSB0"), QString()});
    QCOMPARE(connections,
             QStringList({QStringLiteral("ttyUSB0"), QStringLiteral("COM9"),
                          QStringLiteral("TCP"), QStringLiteral("UDP"),
                          QStringLiteral("UDPCl"), QStringLiteral("WS")}));
    QCOMPARE(ConnectionOptionsViewModel::availableBaudRates(),
             QList<int>({1200, 2400, 4800, 9600, 19200, 28800, 38400,
                         57600, 111100, 115200, 230400, 460800, 500000,
                         625000, 921600, 1000000, 1500000}));
}

void ConnectionOptionsWindowTest::viewModelTracksTransportAndBaud()
{
    ConnectionOptionsViewModel model({QStringLiteral("ttyACM0")});
    QCOMPARE(model.SelectedConnection(), QStringLiteral("ttyACM0"));
    QCOMPARE(model.SelectedBaud(), 115200);
    QVERIFY(model.BaudEnabled());

    QSignalSpy baudEnabledChanged(
        &model, &ConnectionOptionsViewModel::BaudEnabledChanged);
    model.setSelectedConnection(QStringLiteral("TCP"));
    QVERIFY(!model.BaudEnabled());
    QCOMPARE(baudEnabledChanged.count(), 1);

    model.setSelectedConnection(QStringLiteral("UDPCl"));
    QCOMPARE(baudEnabledChanged.count(), 1);
    model.setSelectedConnection(QStringLiteral("ttyACM0"));
    QVERIFY(model.BaudEnabled());
    QCOMPARE(baudEnabledChanged.count(), 2);

    model.setSelectedBaud(921600);
    QCOMPARE(model.SelectedBaud(), 921600);
    model.setSelectedBaud(12345);
    QCOMPARE(model.SelectedBaud(), 921600);
}

void ConnectionOptionsWindowTest::windowMatchesReferenceContract()
{
    ConnectionOptionsWindow window(
        {QStringLiteral("ttyUSB0"), QStringLiteral("ttyACM0")});

    QCOMPARE(window.objectName(), QStringLiteral("ConnectionOptions"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Connections"));
    QCOMPARE(window.size(), QSize(228, 75));
    QCOMPARE(window.minimumSize(), QSize(228, 75));
    QCOMPARE(window.maximumSize(), QSize(228, 75));
    QVERIFY(!window.isModal());

    auto *connection = window.findChild<QComboBox *>(
        QStringLiteral("CMB_serialport"));
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("CMB_baudrate"));
    auto *connectButton = window.findChild<QPushButton *>(
        QStringLiteral("BUT_connect"));
    QVERIFY(connection);
    QVERIFY(baud);
    QVERIFY(connectButton);
    QCOMPARE(connection->geometry(), QRect(13, 13, 121, 21));
    QCOMPARE(baud->geometry(), QRect(13, 40, 121, 21));
    QCOMPARE(connectButton->geometry(), QRect(140, 13, 75, 23));
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QCOMPARE(connection->count(), 6);
    QCOMPARE(connection->itemText(0), QStringLiteral("ttyUSB0"));
    QCOMPARE(connection->itemText(2), QStringLiteral("TCP"));
    QCOMPARE(connection->itemText(5), QStringLiteral("WS"));
    QCOMPARE(baud->count(), 17);
    QCOMPARE(baud->itemData(0).toInt(), 1200);
    QCOMPARE(baud->itemData(16).toInt(), 1500000);
    QCOMPARE(baud->currentData().toInt(), 115200);
    QVERIFY(baud->isEnabled());

    connection->setCurrentText(QStringLiteral("UDP"));
    QVERIFY(!baud->isEnabled());
    connection->setCurrentText(QStringLiteral("ttyACM0"));
    QVERIFY(baud->isEnabled());
}

void ConnectionOptionsWindowTest::connectEmitsRequestAndKeepsWindowOpen()
{
    ConnectionOptionsWindow window({QStringLiteral("ttyUSB0")});
    auto *connection = window.findChild<QComboBox *>(
        QStringLiteral("CMB_serialport"));
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("CMB_baudrate"));
    auto *connectButton = window.findChild<QPushButton *>(
        QStringLiteral("BUT_connect"));
    QVERIFY(connection);
    QVERIFY(baud);
    QVERIFY(connectButton);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    connection->setCurrentText(QStringLiteral("ttyUSB0"));
    baud->setCurrentIndex(baud->findData(460800));
    QSignalSpy requested(&window, &ConnectionOptionsWindow::connectRequested);
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.takeFirst(),
             QVariantList({QStringLiteral("ttyUSB0"), 460800}));
    QVERIFY(window.isVisible());
}

void ConnectionOptionsWindowTest::openWindowBridgesRequestToRuntimeOwner()
{
    RuntimeOwner owner;
    QPointer<ConnectionOptionsWindow> window(
        ConnectionOptionsWindow::OpenWindow(&owner));
    QVERIFY(window);
    auto *connection = window->findChild<QComboBox *>(
        QStringLiteral("CMB_serialport"));
    auto *connectButton = window->findChild<QPushButton *>(
        QStringLiteral("BUT_connect"));
    QVERIFY(connection);
    QVERIFY(connectButton);

    connection->setCurrentText(QStringLiteral("TCP"));
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(owner.requestCount, 1);
    QCOMPARE(owner.connection, QStringLiteral("TCP"));
    QCOMPARE(owner.baud, 115200);

    window->close();
    QTRY_VERIFY(window.isNull());
}

QTEST_MAIN(ConnectionOptionsWindowTest)
#include "test_connectionoptionswindow.moc"
