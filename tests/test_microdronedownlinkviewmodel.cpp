#include <QtTest>

#include "ui/MicrodroneDownlinkViewModel.h"

#include <QPointer>

#include <memory>

namespace
{
struct OutputState
{
    bool open = false;
    int opens = 0;
    int closes = 0;
    QList<QByteArray> writes;
    MicrodroneOutputSettings settings;
};

class FakeOutput final : public MavlinkMirrorOutput
{
public:
    explicit FakeOutput(const std::shared_ptr<OutputState> &state)
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
    std::shared_ptr<OutputState> m_state;
};

MicrodroneSource source(int linkId = 7, int systemId = 42,
                        quint64 generation = 3,
                        quint64 instanceEpoch = 5)
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
    result.instance.instanceEpoch = instanceEpoch;
    result.linkName = QStringLiteral("Telemetry A");
    return result;
}

struct Fixture
{
    std::shared_ptr<OutputState> output = std::make_shared<OutputState>();
    std::shared_ptr<MicrodroneSource> current =
        std::make_shared<MicrodroneSource>(source());
    QStringList ports{QStringLiteral("ttyB"), QStringLiteral("ttyA"),
                      QStringLiteral("ttyB")};
    bool rejectPort = false;
    MicrodroneDownlinkService service;
    MicrodroneDownlinkViewModel model;

    Fixture()
        : service(dependencies())
        , model(&service, [this]() { return ports; })
    {
    }

    MicrodroneDownlinkService::Dependencies dependencies()
    {
        MicrodroneDownlinkService::Dependencies result;
        result.outputFactory = [state = output](
            const MicrodroneOutputSettings &settings, QString *) {
            state->settings = settings;
            return std::unique_ptr<MavlinkMirrorOutput>(new FakeOutput(state));
        };
        result.resolveSource = [selected = current]() { return *selected; };
        result.validatePort = [this](const QString &) {
            return rejectPort ? QStringLiteral("Port is already in use.")
                              : QString();
        };
        result.clock = []() {
            return QDateTime(QDate(2026, 9, 6), QTime(12, 0), Qt::UTC);
        };
        return result;
    }
};
} // namespace

class MicrodroneDownlinkViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialStateAndPortRefreshMatchReference();
    void connectionUsesFrozenSelectionAndMirrorsService();
    void failuresAndSourceChangesRemainTruthful();
    void callbacksMayDeleteViewModel();
};

void MicrodroneDownlinkViewModelTest::initialStateAndPortRefreshMatchReference()
{
    Fixture fixture;
    QCOMPARE(fixture.model.bauds(), QList<int>({
        4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200}));
    QCOMPARE(fixture.model.selectedBaud(), 57600);
    QCOMPARE(fixture.model.ports(),
             QStringList({QStringLiteral("ttyA"), QStringLiteral("ttyB")}));
    QCOMPARE(fixture.model.selectedPort(), QStringLiteral("ttyA"));
    QVERIFY(!fixture.model.busy());
    QVERIFY(!fixture.model.isRunning());
    QVERIFY(fixture.model.canEditSettings());
    QCOMPARE(fixture.model.connectButtonText(), QStringLiteral("Connect"));
    QVERIFY(fixture.model.sourceDescription().contains(QStringLiteral("42")));

    fixture.model.setSelectedPort(QStringLiteral("ttyB"));
    fixture.ports = QStringList{QStringLiteral("ttyC"), QStringLiteral("ttyB")};
    fixture.model.refreshPorts();
    QCOMPARE(fixture.model.ports(),
             QStringList({QStringLiteral("ttyB"), QStringLiteral("ttyC")}));
    QCOMPARE(fixture.model.selectedPort(), QStringLiteral("ttyB"));
    fixture.ports = QStringList{QStringLiteral("ttyD")};
    fixture.model.refreshPorts();
    QCOMPARE(fixture.model.selectedPort(), QStringLiteral("ttyD"));
}

void MicrodroneDownlinkViewModelTest::connectionUsesFrozenSelectionAndMirrorsService()
{
    Fixture fixture;
    fixture.model.setSelectedPort(QStringLiteral("ttyB"));
    fixture.model.setSelectedBaud(115200);
    fixture.model.toggleConnection();
    QVERIFY(fixture.service.isRunning());
    QVERIFY(fixture.output->open);
    QCOMPARE(fixture.output->settings.port, QStringLiteral("ttyB"));
    QCOMPARE(fixture.output->settings.baud, 115200);
    QCOMPARE(fixture.model.connectButtonText(), QStringLiteral("Stop"));
    QVERIFY(!fixture.model.canEditSettings());
    QVERIFY(fixture.model.statusText().contains(
        QStringLiteral("Emitting"), Qt::CaseInsensitive));

    fixture.service.emitNow();
    QVERIFY(!fixture.output->writes.isEmpty());
    QVERIFY(fixture.output->writes.last().contains("#1,"));
    QVERIFY(fixture.output->writes.last().contains("#9,"));
    QVERIFY(fixture.model.lastLine().startsWith(QStringLiteral("#9,")));

    fixture.model.toggleConnection();
    QVERIFY(!fixture.service.isRunning());
    QVERIFY(!fixture.output->open);
    QCOMPARE(fixture.output->closes, 1);
    QCOMPARE(fixture.model.connectButtonText(), QStringLiteral("Connect"));
    QCOMPARE(fixture.model.statusText(), QStringLiteral("Stopped."));
}

void MicrodroneDownlinkViewModelTest::failuresAndSourceChangesRemainTruthful()
{
    Fixture fixture;
    fixture.rejectPort = true;
    fixture.model.toggleConnection();
    QVERIFY(!fixture.service.isRunning());
    QVERIFY(fixture.model.statusText().contains(
        QStringLiteral("already in use"), Qt::CaseInsensitive));
    QCOMPARE(fixture.output->opens, 0);

    fixture.rejectPort = false;
    fixture.model.toggleConnection();
    QVERIFY(fixture.service.isRunning());
    *fixture.current = source(8, 43, 4, 6);
    fixture.service.synchronizeSource();
    QVERIFY(!fixture.service.isRunning());
    QVERIFY(fixture.model.statusText().contains(
        QStringLiteral("changed"), Qt::CaseInsensitive));
    QCOMPARE(fixture.output->closes, 1);

    fixture.ports.clear();
    fixture.model.refreshPorts();
    QCOMPARE(fixture.model.selectedPort(), QString());
    fixture.model.toggleConnection();
    QCOMPARE(fixture.model.statusText(),
             QStringLiteral("Select a serial output port first."));
}

void MicrodroneDownlinkViewModelTest::callbacksMayDeleteViewModel()
{
    auto output = std::make_shared<OutputState>();
    auto current = std::make_shared<MicrodroneSource>(source());
    MicrodroneDownlinkService::Dependencies dependencies;
    dependencies.outputFactory = [output](
        const MicrodroneOutputSettings &settings, QString *) {
        output->settings = settings;
        return std::unique_ptr<MavlinkMirrorOutput>(new FakeOutput(output));
    };
    dependencies.resolveSource = [current]() { return *current; };
    dependencies.validatePort = [](const QString &) { return QString(); };
    dependencies.clock = []() { return QDateTime::currentDateTimeUtc(); };
    MicrodroneDownlinkService service(dependencies);
    QPointer<MicrodroneDownlinkViewModel> model =
        new MicrodroneDownlinkViewModel(
            &service, []() { return QStringList{QStringLiteral("ttyTEST")}; });
    connect(model, &MicrodroneDownlinkViewModel::changed,
            model, [model]() {
        if (model && model->isRunning())
            delete model;
    }, Qt::DirectConnection);
    model->toggleConnection();
    QVERIFY(model.isNull());
    QVERIFY(!service.isRunning());
    QVERIFY(!output->open);
}

QTEST_MAIN(MicrodroneDownlinkViewModelTest)
#include "test_microdronedownlinkviewmodel.moc"
