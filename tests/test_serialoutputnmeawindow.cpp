#include <QtTest>

#include "ui/SerialOutputNMEAWindow.h"

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>

#include <memory>

namespace
{
struct WindowOutputState
{
    bool open = false;
    QList<QByteArray> writes;
};

class WindowFakeOutput final : public MavlinkMirrorOutput
{
public:
    explicit WindowFakeOutput(const std::shared_ptr<WindowOutputState> &state)
        : m_state(state) {}
    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return QStringLiteral("ttyTEST"); }
    bool open(QString *) override { return m_state->open = true; }
    void close() override { m_state->open = false; }
    bool isOpen() const override { return m_state->open; }
    bool hasPeer() const override { return m_state->open; }
    qint64 write(const QByteArray &bytes) override
    {
        m_state->writes.append(bytes);
        return bytes.size();
    }
    qint64 pendingBytes() const override { return 0; }
    QString statusText() const override { return QStringLiteral("fake"); }
private:
    std::shared_ptr<WindowOutputState> m_state;
};

struct WindowFixture
{
    QStringList ports = {QStringLiteral("ttyTEST")};
    NmeaOutputSource source;
    std::shared_ptr<WindowOutputState> output =
        std::make_shared<WindowOutputState>();

    WindowFixture()
    {
        source.endpoint.linkId = 42;
        source.endpoint.systemId = 7;
        source.endpoint.componentId = 1;
        source.endpoint.linkName = QStringLiteral("Vehicle Link");
        source.linkName = QStringLiteral("Vehicle Link");
    }

    SerialOutputNMEAWindow::Dependencies dependencies()
    {
        SerialOutputNMEAWindow::Dependencies result;
        result.outputFactory = [state = output](
            const NmeaOutputSettings &, QString *) {
            return std::unique_ptr<MavlinkMirrorOutput>(
                new WindowFakeOutput(state));
        };
        result.udpPortGuard = [](quint16, QString *) { return true; };
        result.clock = []() {
            return QDateTime(QDate(1994, 3, 23), QTime(12, 35, 19), Qt::UTC);
        };
        result.viewModel.enumeratePorts = [this]() { return ports; };
        result.viewModel.resolveSource = [this]() { return source; };
        result.viewModel.validateSelection = [](const QString &) {
            return QString();
        };
        return result;
    }
};

mavlink_message_t positionMessage()
{
    mavlink_global_position_int_t position{};
    position.lat = 481173000;
    position.lon = 115166667;
    position.alt = 545400;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(7, 1, &message, &position);
    return message;
}
}

class SerialOutputNMEAWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialUiMatchesMp10();
    void refreshRateAndEmissionAreLive();
    void failuresStayVisibleAndWindowsAreIndependent();
};

void SerialOutputNMEAWindowTest::initialUiMatchesMp10()
{
    WindowFixture fixture;
    SerialOutputNMEAWindow window(fixture.dependencies());
    QCOMPARE(window.objectName(), QStringLiteral("SerialOutputNMEAWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("NMEA Output"));
    QCOMPARE(window.size(), QSize(480, 400));
    QCOMPARE(window.minimumSize(), QSize(480, 400));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QVERIFY(window.isWindow());
    QVERIFY(qobject_cast<QDialog *>(&window) == nullptr);

    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialOutputNmeaDescription"))->text(),
             QStringLiteral("Emits NMEA-0183 GGA/GLL/HDG/VTG/RMC sentences "
                            "from the live vehicle position to the selected port."));
    auto *port = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputNmeaPort"));
    QCOMPARE(port->count(), 3);
    QCOMPARE(port->itemText(0), QStringLiteral("ttyTEST"));
    QCOMPARE(port->itemText(1), QStringLiteral("TCP Host - 14551"));
    QCOMPARE(port->itemText(2), QStringLiteral("UDP Host - 14551"));
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputNmeaBaud"));
    QCOMPARE(baud->count(), 6);
    QCOMPARE(baud->currentText(), QStringLiteral("4800"));
    auto *rate = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputNmeaRate"));
    QCOMPARE(rate->count(), 4);
    QCOMPARE(rate->currentText(), QStringLiteral("5"));
    QCOMPARE(window.findChild<QPushButton *>(
                 QStringLiteral("serialOutputNmeaToggle"))->text(),
             QStringLiteral("Connect"));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialOutputNmeaStatus"))->text(),
             QStringLiteral("Stopped."));
}

void SerialOutputNMEAWindowTest::refreshRateAndEmissionAreLive()
{
    WindowFixture fixture;
    fixture.ports = QStringList{
        QStringLiteral("ttyA"), QStringLiteral("ttyB")};
    SerialOutputNMEAWindow window(fixture.dependencies());
    auto *port = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputNmeaPort"));
    port->setCurrentText(QStringLiteral("ttyB"));
    fixture.ports = QStringList{
        QStringLiteral("ttyB"), QStringLiteral("ttyC")};
    window.findChild<QPushButton *>(
        QStringLiteral("serialOutputNmeaRefresh"))->click();
    QCOMPARE(port->currentText(), QStringLiteral("ttyB"));

    window.findChild<QPushButton *>(
        QStringLiteral("serialOutputNmeaToggle"))->click();
    QVERIFY(window.service()->isRunning());
    QCOMPARE(window.service()->endpoint(), fixture.source.endpoint);
    QCOMPARE(window.viewModel()->connectButtonText(), QStringLiteral("Stop"));

    auto *rate = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputNmeaRate"));
    rate->setCurrentText(QStringLiteral("1"));
    QCOMPARE(window.service()->timerIntervalMs(), 1000);

    window.service()->observeMessage(42, positionMessage());
    window.service()->emitNow();
    QCOMPARE(fixture.output->writes.size(), 5);
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("serialOutputNmeaLastSentence"))->text()
        .startsWith(QStringLiteral("$GPRMC,")));
}

void SerialOutputNMEAWindowTest::failuresStayVisibleAndWindowsAreIndependent()
{
    WindowFixture missing;
    missing.source = NmeaOutputSource();
    SerialOutputNMEAWindow noSource(missing.dependencies());
    noSource.findChild<QPushButton *>(
        QStringLiteral("serialOutputNmeaToggle"))->click();
    QVERIFY(!noSource.service()->isRunning());
    QVERIFY(noSource.viewModel()->statusText().contains(
        QStringLiteral("No current vehicle target")));

    WindowFixture rejected;
    auto dependencies = rejected.dependencies();
    dependencies.viewModel.validateSelection = [](const QString &) {
        return QStringLiteral("Port is busy.");
    };
    SerialOutputNMEAWindow invalid(dependencies);
    invalid.findChild<QPushButton *>(
        QStringLiteral("serialOutputNmeaToggle"))->click();
    QCOMPARE(invalid.viewModel()->statusText(),
             QStringLiteral("Error connecting: Port is busy."));

    WindowFixture firstFixture;
    WindowFixture secondFixture;
    auto *first = new SerialOutputNMEAWindow(firstFixture.dependencies());
    auto *second = new SerialOutputNMEAWindow(secondFixture.dependencies());
    QPointer<SerialOutputNMEAWindow> firstGuard(first);
    QPointer<SerialOutputNMEAWindow> secondGuard(second);
    first->show();
    second->show();
    first->findChild<QPushButton *>(
        QStringLiteral("serialOutputNmeaToggle"))->click();
    QVERIFY(first->service()->isRunning());
    QVERIFY(!second->service()->isRunning());
    first->close();
    QTRY_VERIFY(firstGuard.isNull());
    QVERIFY(!secondGuard.isNull());
    second->close();
    QTRY_VERIFY(secondGuard.isNull());
}

QTEST_MAIN(SerialOutputNMEAWindowTest)
#include "test_serialoutputnmeawindow.moc"
