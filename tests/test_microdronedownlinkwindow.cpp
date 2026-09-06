#include <QtTest>

#include "ui/MicrodroneDownlinkWindow.h"

#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>

#include <memory>

namespace
{
struct WindowOutputState
{
    bool open = false;
    int opens = 0;
    int closes = 0;
    QList<QByteArray> writes;
    MicrodroneOutputSettings settings;
};

class WindowFakeOutput final : public MavlinkMirrorOutput
{
public:
    explicit WindowFakeOutput(
        const std::shared_ptr<WindowOutputState> &state)
        : m_state(state) {}
    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return m_state->settings.port; }
    bool open(QString *) override
    {
        m_state->open = true;
        ++m_state->opens;
        return true;
    }
    void close() override
    {
        if (m_state->open)
            ++m_state->closes;
        m_state->open = false;
    }
    bool isOpen() const override { return m_state->open; }
    bool hasPeer() const override { return m_state->open; }
    qint64 write(const QByteArray &bytes) override
    {
        if (!m_state->open)
            return -1;
        m_state->writes.append(bytes);
        return bytes.size();
    }
    qint64 pendingBytes() const override { return 0; }
    QString statusText() const override { return QStringLiteral("fake serial"); }

private:
    std::shared_ptr<WindowOutputState> m_state;
};

MicrodroneSource windowSource(int linkId = 7, int systemId = 42,
                              quint64 generation = 3)
{
    MicrodroneSource result;
    result.selection.endpoint.linkId = linkId;
    result.selection.endpoint.systemId = systemId;
    result.selection.endpoint.componentId = 1;
    result.selection.endpoint.linkName = QStringLiteral("Telemetry A");
    result.selection.endpoint.componentName = QStringLiteral("Autopilot");
    result.selection.generation = generation;
    result.instance.endpoint = result.selection.endpoint;
    result.instance.linkSessionEpoch = 91;
    result.instance.instanceEpoch = generation + 10;
    result.linkName = QStringLiteral("Telemetry A");
    return result;
}

struct WindowFixture
{
    std::shared_ptr<WindowOutputState> output =
        std::make_shared<WindowOutputState>();
    std::shared_ptr<MicrodroneSource> current =
        std::make_shared<MicrodroneSource>(windowSource());
    std::shared_ptr<QStringList> ports = std::make_shared<QStringList>(
        QStringList{QStringLiteral("ttyB"), QStringLiteral("ttyA")});
    bool rejectPort = false;

    MicrodroneDownlinkWindow::Dependencies dependencies()
    {
        MicrodroneDownlinkWindow::Dependencies result;
        result.service.outputFactory = [state = output](
            const MicrodroneOutputSettings &settings, QString *) {
            state->settings = settings;
            return std::unique_ptr<MavlinkMirrorOutput>(
                new WindowFakeOutput(state));
        };
        result.service.resolveSource = [selected = current]() {
            return *selected;
        };
        result.service.validatePort = [this](const QString &) {
            return rejectPort ? QStringLiteral("Port conflict fixture.")
                              : QString();
        };
        result.service.clock = []() {
            return QDateTime(QDate(2026, 9, 6), QTime(12, 0), Qt::UTC);
        };
        result.enumeratePorts = [values = ports]() { return *values; };
        return result;
    }
};
} // namespace

class MicrodroneDownlinkWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialUiMatchesMp10();
    void controlsRefreshConnectEmitAndStop();
    void offlineFailureAndSourceSwitchAreVisible();
    void closeStopsOwnedOutputAndWindowsAreIndependent();
    void synchronousChangedCallbackMayDeleteWindow();
};

void MicrodroneDownlinkWindowTest::initialUiMatchesMp10()
{
    WindowFixture fixture;
    MicrodroneDownlinkWindow window(fixture.dependencies());
    QCOMPARE(window.objectName(), QStringLiteral("MicrodroneDownlinkWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("MicroDrone Downlink"));
    QCOMPARE(window.size(), QSize(580, 440));
    QCOMPARE(window.minimumSize(), QSize(520, 390));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QVERIFY(window.isWindow());
    QVERIFY(window.testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(!window.isClosing());

    auto *port = window.findChild<QComboBox *>(
        QStringLiteral("microdroneDownlinkPort"));
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("microdroneDownlinkBaud"));
    auto *refresh = window.findChild<QPushButton *>(
        QStringLiteral("RefreshMicrodronePortsButton"));
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"));
    QVERIFY(port && baud && refresh && toggle);
    QCOMPARE(port->count(), 2);
    QCOMPARE(port->itemText(0), QStringLiteral("ttyA"));
    QCOMPARE(baud->count(), 8);
    QCOMPARE(baud->itemText(0), QStringLiteral("4800"));
    QCOMPARE(baud->itemText(7), QStringLiteral("115200"));
    QCOMPARE(baud->currentText(), QStringLiteral("57600"));
    QCOMPARE(toggle->text(), QStringLiteral("Connect"));
    QVERIFY(refresh->isEnabled());
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkDescription"))->text().contains(
            QStringLiteral("#1 and #4–#9")));
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkWarning"))->text().contains(
            QStringLiteral("interrupt telemetry")));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("microdroneDownlinkStatus"))->text(),
             QStringLiteral("Stopped."));
    QVERIFY(!window.findChild<QProgressBar *>(
        QStringLiteral("microdroneDownlinkBusy"))->isVisible());
}

void MicrodroneDownlinkWindowTest::controlsRefreshConnectEmitAndStop()
{
    WindowFixture fixture;
    MicrodroneDownlinkWindow window(fixture.dependencies());
    window.show();
    auto *port = window.findChild<QComboBox *>(
        QStringLiteral("microdroneDownlinkPort"));
    auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("microdroneDownlinkBaud"));
    auto *refresh = window.findChild<QPushButton *>(
        QStringLiteral("RefreshMicrodronePortsButton"));
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"));
    QVERIFY(port && baud && refresh && toggle);
    port->setCurrentText(QStringLiteral("ttyB"));
    *fixture.ports = QStringList{QStringLiteral("ttyC"), QStringLiteral("ttyB")};
    refresh->click();
    QCOMPARE(port->currentText(), QStringLiteral("ttyB"));
    QCOMPARE(port->itemText(0), QStringLiteral("ttyB"));
    QCOMPARE(port->itemText(1), QStringLiteral("ttyC"));
    baud->setCurrentText(QStringLiteral("115200"));

    toggle->click();
    QVERIFY(window.service()->isRunning());
    QCOMPARE(fixture.output->settings.port, QStringLiteral("ttyB"));
    QCOMPARE(fixture.output->settings.baud, 115200);
    QCOMPARE(toggle->text(), QStringLiteral("Stop"));
    QVERIFY(!port->isEnabled());
    QVERIFY(!baud->isEnabled());
    QVERIFY(!refresh->isEnabled());
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkSource"))->text().contains(
            QStringLiteral("42")));

    window.service()->emitNow();
    QVERIFY(!fixture.output->writes.isEmpty());
    const QString line = window.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkLastLine"))->text();
    QVERIFY(line.startsWith(QStringLiteral("#9,")));
    QVERIFY(!line.contains(QStringLiteral("\r")));

    toggle->click();
    QVERIFY(!window.service()->isRunning());
    QCOMPARE(toggle->text(), QStringLiteral("Connect"));
    QVERIFY(port->isEnabled());
    QVERIFY(refresh->isEnabled());
    QCOMPARE(fixture.output->closes, 1);
}

void MicrodroneDownlinkWindowTest::offlineFailureAndSourceSwitchAreVisible()
{
    WindowFixture fixture;
    *fixture.current = MicrodroneSource();
    MicrodroneDownlinkWindow offline(fixture.dependencies());
    offline.findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"))->click();
    QVERIFY(!offline.service()->isRunning());
    QVERIFY(offline.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkStatus"))->text().contains(
            QStringLiteral("select a vehicle"), Qt::CaseInsensitive));
    QCOMPARE(fixture.output->opens, 0);

    *fixture.current = windowSource();
    offline.viewModel()->refreshStatus();
    offline.findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"))->click();
    QVERIFY(offline.service()->isRunning());
    *fixture.current = windowSource(9, 77, 8);
    offline.service()->synchronizeSource();
    QVERIFY(!offline.service()->isRunning());
    QVERIFY(offline.findChild<QLabel *>(
        QStringLiteral("microdroneDownlinkStatus"))->text().contains(
            QStringLiteral("changed"), Qt::CaseInsensitive));
}

void MicrodroneDownlinkWindowTest::closeStopsOwnedOutputAndWindowsAreIndependent()
{
    WindowFixture firstFixture;
    WindowFixture secondFixture;
    QPointer<MicrodroneDownlinkWindow> first =
        new MicrodroneDownlinkWindow(firstFixture.dependencies());
    QPointer<MicrodroneDownlinkWindow> second =
        new MicrodroneDownlinkWindow(secondFixture.dependencies());
    first->show();
    second->show();
    first->findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"))->click();
    second->findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"))->click();
    QVERIFY(first->service()->isRunning());
    QVERIFY(second->service()->isRunning());

    first->close();
    QVERIFY(first && first->isClosing());
    QVERIFY(!firstFixture.output->open);
    QCOMPARE(firstFixture.output->closes, 1);
    auto *staleToggle = first->findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"));
    staleToggle->setEnabled(true);
    staleToggle->click();
    QVERIFY(!first->service()->isRunning());
    QVERIFY(second && second->service()->isRunning());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(first.isNull());
    QVERIFY(second && second->service()->isRunning());
    second->close();
    QTRY_VERIFY(second.isNull());
    QCOMPARE(secondFixture.output->closes, 1);
}

void MicrodroneDownlinkWindowTest::synchronousChangedCallbackMayDeleteWindow()
{
    WindowFixture fixture;
    QPointer<MicrodroneDownlinkWindow> window =
        new MicrodroneDownlinkWindow(fixture.dependencies());
    window->show();
    connect(window->viewModel(), &MicrodroneDownlinkViewModel::changed,
            window, [window]() {
        if (window && window->service()->isRunning())
            delete window;
    }, Qt::DirectConnection);
    window->findChild<QPushButton *>(
        QStringLiteral("ToggleMicrodroneOutputButton"))->click();
    QVERIFY(window.isNull());
    QVERIFY(!fixture.output->open);
    QCOMPARE(fixture.output->closes, 1);
}

QTEST_MAIN(MicrodroneDownlinkWindowTest)
#include "test_microdronedownlinkwindow.moc"
