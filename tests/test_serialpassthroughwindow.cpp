#include <QtTest>

#include "comm/MavlinkMirrorOutput.h"
#include "ui/SerialPassThroughWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>

#include <memory>

namespace
{
struct FakeOutputState
{
    bool open = false;
    int closeCount = 0;
    QByteArray written;
    MavlinkMirrorOutput *output = nullptr;
};

class FakeOutput final : public MavlinkMirrorOutput
{
public:
    explicit FakeOutput(const std::shared_ptr<FakeOutputState> &state)
        : m_state(state)
    {
        m_state->output = this;
    }

    ~FakeOutput() override
    {
        if (m_state->output == this) {
            m_state->output = nullptr;
        }
    }

    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return QStringLiteral("ttyTEST"); }
    bool open(QString *) override
    {
        m_state->open = true;
        return true;
    }
    void close() override
    {
        if (m_state->open) {
            ++m_state->closeCount;
        }
        m_state->open = false;
    }
    bool isOpen() const override { return m_state->open; }
    bool hasPeer() const override { return m_state->open; }
    qint64 write(const QByteArray &bytes) override
    {
        m_state->written.append(bytes);
        return bytes.size();
    }
    qint64 pendingBytes() const override { return 0; }
    QString statusText() const override
    {
        return m_state->open ? QStringLiteral("Mirroring on ttyTEST.")
                             : QStringLiteral("Stopped.");
    }

    void injectPeerBytes(const QByteArray &bytes)
    {
        emit peerBytesReceived(bytes);
    }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

struct Fixture
{
    QStringList serialPorts = {QStringLiteral("ttyTEST")};
    MavlinkMirrorSource source{42, QStringLiteral("Vehicle Link")};
    std::shared_ptr<FakeOutputState> output =
        std::make_shared<FakeOutputState>();
    QList<QPair<int, QByteArray>> rawWrites;
    bool rawWriterSucceeds = true;

    SerialPassThroughWindow::Dependencies dependencies()
    {
        SerialPassThroughWindow::Dependencies result;
        result.rawWriter = [this](int linkId, const QByteArray &bytes) {
            rawWrites.append(qMakePair(linkId, bytes));
            return rawWriterSucceeds;
        };
        result.outputFactory = [state = output](
            const MavlinkMirrorSettings &, QString *) {
            return std::unique_ptr<MavlinkMirrorOutput>(new FakeOutput(state));
        };
        result.udpPortGuard = [](quint16, QString *) { return true; };
        result.viewModel.enumeratePorts = [this]() { return serialPorts; };
        result.viewModel.resolveSource = [this]() { return source; };
        result.viewModel.validateSelection = [](const QString &) {
            return QString();
        };
        return result;
    }
};
}

class SerialPassThroughWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialStateMatchesMp10();
    void refreshPreservesSelectionAndUsesExactBauds();
    void startPinsSourceAndWriteBackTogglesLive();
    void missingSourceAndValidationFailuresStayVisible();
    void writeBackFailureUnchecksTheControl();
    void closeStopsAndIndependentWindowsDoNotShareState();
};

void SerialPassThroughWindowTest::initialStateMatchesMp10()
{
    Fixture fixture;
    SerialPassThroughWindow window(fixture.dependencies());

    QCOMPARE(window.objectName(), QStringLiteral("SerialPassThroughWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Mavlink Mirror"));
    QCOMPARE(window.size(), QSize(480, 380));
    QCOMPARE(window.minimumSize(), QSize(480, 380));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QVERIFY(window.isWindow());
    QVERIFY(qobject_cast<QDialog *>(&window) == nullptr);

    const auto *description = window.findChild<QLabel *>(
        QStringLiteral("serialPassThroughDescription"));
    QVERIFY(description);
    QCOMPARE(description->text(), QStringLiteral(
        "Forwards the vehicle's MAVLink byte stream out to a second port "
        "(and optionally back). Useful for feeding a second GCS or companion tool."));

    const auto *ports = window.findChild<QComboBox *>(
        QStringLiteral("serialPassThroughPort"));
    QVERIFY(ports);
    QCOMPARE(ports->count(), 3);
    QCOMPARE(ports->itemText(0), QStringLiteral("ttyTEST"));
    QCOMPARE(ports->itemText(1), QStringLiteral("TCP Host - 14550"));
    QCOMPARE(ports->itemText(2), QStringLiteral("UDP Host - 14550"));

    const auto *baud = window.findChild<QComboBox *>(
        QStringLiteral("serialPassThroughBaud"));
    QCOMPARE(baud->count(), 10);
    QCOMPARE(baud->currentText(), QStringLiteral("115200"));
    const auto *writeBack = window.findChild<QCheckBox *>(
        QStringLiteral("serialPassThroughWriteBack"));
    QVERIFY(!writeBack->isChecked());
    QVERIFY(!writeBack->toolTip().isEmpty());
    QCOMPARE(window.findChild<QPushButton *>(
                 QStringLiteral("serialPassThroughToggle"))->text(),
             QStringLiteral("Start"));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialPassThroughStatus"))->text(),
             QStringLiteral("Stopped."));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialPassThroughTxBytes"))->text(),
             QStringLiteral("0"));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialPassThroughRxBytes"))->text(),
             QStringLiteral("0"));
}

void SerialPassThroughWindowTest::refreshPreservesSelectionAndUsesExactBauds()
{
    Fixture fixture;
    fixture.serialPorts = QStringList{
        QStringLiteral("ttyA"), QStringLiteral("ttyB")};
    SerialPassThroughWindow window(fixture.dependencies());
    auto *ports = window.findChild<QComboBox *>(
        QStringLiteral("serialPassThroughPort"));
    ports->setCurrentText(QStringLiteral("ttyB"));
    fixture.serialPorts = QStringList{
        QStringLiteral("ttyB"), QStringLiteral("ttyC")};
    window.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughRefresh"))->click();
    QCOMPARE(ports->currentText(), QStringLiteral("ttyB"));

    fixture.serialPorts = QStringList{QStringLiteral("ttyC")};
    window.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughRefresh"))->click();
    QCOMPARE(ports->currentText(), QStringLiteral("ttyC"));
    QCOMPARE(window.viewModel()->bauds(), QList<int>({
        1200, 2400, 4800, 9600, 19200, 38400,
        57600, 115200, 230400, 921600
    }));
}

void SerialPassThroughWindowTest::startPinsSourceAndWriteBackTogglesLive()
{
    Fixture fixture;
    SerialPassThroughWindow window(fixture.dependencies());
    auto *toggle = window.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughToggle"));
    toggle->click();
    QVERIFY(window.service()->isRunning());
    QCOMPARE(window.service()->linkId(), 42);
    QCOMPARE(toggle->text(), QStringLiteral("Stop"));
    QCOMPARE(window.viewModel()->statusText(),
             QStringLiteral("Mirroring on ttyTEST."));

    window.service()->observeFrame(7, QByteArray("wrong"));
    window.service()->observeFrame(42, QByteArray("frame"));
    QCOMPARE(fixture.output->written, QByteArray("frame"));
    window.viewModel()->refreshStatus();
    QCOMPARE(window.viewModel()->txBytes(), quint64(5));

    auto *writeBack = window.findChild<QCheckBox *>(
        QStringLiteral("serialPassThroughWriteBack"));
    auto *output = static_cast<FakeOutput *>(fixture.output->output);
    output->injectPeerBytes(QByteArray("discarded"));
    QVERIFY(fixture.rawWrites.isEmpty());
    writeBack->click();
    output->injectPeerBytes(QByteArray("raw"));
    QCOMPARE(fixture.rawWrites.size(), 1);
    QCOMPARE(fixture.rawWrites.first().first, 42);
    QCOMPARE(fixture.rawWrites.first().second, QByteArray("raw"));
    window.viewModel()->refreshStatus();
    QCOMPARE(window.viewModel()->rxBytes(), quint64(12));

    toggle->click();
    QVERIFY(!window.service()->isRunning());
    QCOMPARE(toggle->text(), QStringLiteral("Start"));
    QCOMPARE(window.viewModel()->statusText(), QStringLiteral("Stopped."));
}

void SerialPassThroughWindowTest::missingSourceAndValidationFailuresStayVisible()
{
    Fixture missing;
    missing.source = MavlinkMirrorSource();
    SerialPassThroughWindow noSource(missing.dependencies());
    noSource.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughToggle"))->click();
    QVERIFY(!noSource.service()->isRunning());
    QVERIFY(noSource.viewModel()->statusText().contains(
        QStringLiteral("No current link")));

    Fixture rejected;
    auto dependencies = rejected.dependencies();
    dependencies.viewModel.validateSelection = [](const QString &) {
        return QStringLiteral("Port is busy.");
    };
    SerialPassThroughWindow invalid(dependencies);
    invalid.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughToggle"))->click();
    QVERIFY(!invalid.service()->isRunning());
    QCOMPARE(invalid.viewModel()->statusText(),
             QStringLiteral("Error connecting: Port is busy."));
}

void SerialPassThroughWindowTest::writeBackFailureUnchecksTheControl()
{
    Fixture fixture;
    fixture.rawWriterSucceeds = false;
    SerialPassThroughWindow window(fixture.dependencies());
    window.findChild<QPushButton *>(
        QStringLiteral("serialPassThroughToggle"))->click();
    auto *writeBack = window.findChild<QCheckBox *>(
        QStringLiteral("serialPassThroughWriteBack"));
    writeBack->click();
    QVERIFY(writeBack->isChecked());

    auto *output = static_cast<FakeOutput *>(fixture.output->output);
    output->injectPeerBytes(QByteArray("raw"));
    window.viewModel()->refreshStatus();
    QVERIFY(!writeBack->isChecked());
    QVERIFY(window.viewModel()->statusText().contains(
        QStringLiteral("Write back failed")));
    QVERIFY(window.service()->isRunning());
}

void SerialPassThroughWindowTest::closeStopsAndIndependentWindowsDoNotShareState()
{
    Fixture firstFixture;
    Fixture secondFixture;
    auto *first = new SerialPassThroughWindow(firstFixture.dependencies());
    auto *second = new SerialPassThroughWindow(secondFixture.dependencies());
    first->show();
    second->show();
    first->findChild<QPushButton *>(
        QStringLiteral("serialPassThroughToggle"))->click();
    QVERIFY(first->service()->isRunning());
    QVERIFY(!second->service()->isRunning());
    first->service()->observeFrame(42, QByteArray("one"));
    QCOMPARE(firstFixture.output->written, QByteArray("one"));
    QVERIFY(secondFixture.output->written.isEmpty());

    QPointer<SerialPassThroughWindow> guarded(first);
    first->close();
    QTRY_VERIFY(guarded.isNull());
    QCOMPARE(firstFixture.output->closeCount, 1);
    QVERIFY(second);
    second->close();
}

QTEST_MAIN(SerialPassThroughWindowTest)

#include "test_serialpassthroughwindow.moc"
