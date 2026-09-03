#include <QtTest>

#include "comm/NmeaOutputService.h"

#include <QSignalSpy>

#include <memory>

namespace
{
struct FakeOutputState
{
    MavlinkMirrorOutput::Kind kind = MavlinkMirrorOutput::Kind::Serial;
    bool open = false;
    bool peer = true;
    bool openSucceeds = true;
    qint64 pending = 0;
    qint64 nextWriteResult = -2;
    QList<QByteArray> writes;
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

    Kind kind() const override { return m_state->kind; }
    QString selection() const override { return QStringLiteral("ttyTEST"); }
    bool open(QString *error) override
    {
        if (!m_state->openSucceeds) {
            if (error) {
                *error = QStringLiteral("open failed");
            }
            return false;
        }
        m_state->open = true;
        return true;
    }
    void close() override { m_state->open = false; }
    bool isOpen() const override { return m_state->open; }
    bool hasPeer() const override { return m_state->open && m_state->peer; }
    qint64 write(const QByteArray &bytes) override
    {
        const qint64 result = m_state->nextWriteResult == -2
            ? bytes.size() : m_state->nextWriteResult;
        m_state->nextWriteResult = -2;
        if (result > 0) {
            m_state->writes.append(bytes.left(static_cast<int>(result)));
        }
        return result;
    }
    qint64 pendingBytes() const override { return m_state->pending; }
    QString statusText() const override { return QStringLiteral("fake"); }

    void setPeer(bool peer)
    {
        if (m_state->peer == peer) {
            return;
        }
        m_state->peer = peer;
        emit peerChanged();
    }
    void injectError(const QString &text) { emit errorOccurred(text); }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

VehicleEndpoint endpoint()
{
    VehicleEndpoint value;
    value.linkId = 42;
    value.systemId = 7;
    value.componentId = 1;
    value.linkName = QStringLiteral("Vehicle Link");
    return value;
}

mavlink_message_t globalPosition(int sysid = 7, int compid = 1)
{
    mavlink_global_position_int_t position{};
    position.lat = 481173000;
    position.lon = 115166667;
    position.alt = 545400;
    position.relative_alt = 100000;
    position.vx = 300;
    position.vy = 400;
    position.hdg = 8440;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<uint8_t>(sysid), static_cast<uint8_t>(compid),
        &message, &position);
    return message;
}

struct Fixture
{
    std::shared_ptr<FakeOutputState> output =
        std::make_shared<FakeOutputState>();

    NmeaOutputService::OutputFactory factory()
    {
        return [state = output](const NmeaOutputSettings &, QString *) {
            return std::unique_ptr<MavlinkMirrorOutput>(new FakeOutput(state));
        };
    }

    NmeaOutputService::Clock clock() const
    {
        return []() {
            return QDateTime(QDate(1994, 3, 23), QTime(12, 35, 19, 120),
                             Qt::UTC);
        };
    }
};
}

class NmeaOutputServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void selectionsAndDefaultsMatchMp10();
    void pinsExactEndpointAndEmitsFiveCompleteLines();
    void noPeerRateChangeAndUdpNoPeerFollowMp10();
    void boundsDropsErrorsAndLinkRemovalAreExplicit();
    void guardsAndOpenFailuresLeaveNothingRunning();
};

void NmeaOutputServiceTest::selectionsAndDefaultsMatchMp10()
{
    QCOMPARE(NmeaOutputService::Selections({QStringLiteral("ttyA"),
                                             QStringLiteral("ttyA"),
                                             QStringLiteral("ttyB")}),
             QStringList({QStringLiteral("ttyA"), QStringLiteral("ttyB"),
                          QStringLiteral("TCP Host - 14551"),
                          QStringLiteral("UDP Host - 14551")}));
    QCOMPARE(NmeaOutputService::DefaultHostPort, quint16(14551));
    QCOMPARE(NmeaOutputService::StoppedText(), QStringLiteral("Stopped."));
    QCOMPARE(NmeaOutputService::PickPortText(),
             QStringLiteral("Pick a port first."));
}

void NmeaOutputServiceTest::pinsExactEndpointAndEmitsFiveCompleteLines()
{
    Fixture fixture;
    NmeaOutputService service(fixture.factory(), fixture.clock());
    NmeaOutputSettings settings;
    settings.portSelection = QStringLiteral("ttyTEST");
    QVERIFY(service.start(endpoint(), QStringLiteral("Vehicle Link"), settings));
    QCOMPARE(service.endpoint(), endpoint());
    QCOMPARE(service.status().text,
             QStringLiteral("Emitting NMEA on ttyTEST."));

    service.observeMessage(41, globalPosition());
    service.observeMessage(42, globalPosition(8, 1));
    service.observeMessage(42, globalPosition(7, 2));
    service.emitNow();
    QVERIFY(fixture.output->writes.isEmpty());

    service.observeMessage(42, globalPosition());
    service.emitNow();
    QCOMPARE(fixture.output->writes.size(), 5);
    QVERIFY(fixture.output->writes.at(0).startsWith("$GPGGA,"));
    QVERIFY(fixture.output->writes.at(1).startsWith("$GPGLL,"));
    QVERIFY(fixture.output->writes.at(2).startsWith("$GPHDG,"));
    QVERIFY(fixture.output->writes.at(3).startsWith("$GPVTG,"));
    QVERIFY(fixture.output->writes.at(4).startsWith("$GPRMC,"));
    for (const QByteArray &line : fixture.output->writes) {
        QVERIFY(line.endsWith("\r\n"));
        QVERIFY(line.contains('*'));
    }
    QCOMPARE(service.status().sentencesSent, quint64(5));
    QCOMPARE(service.status().ticks, quint64(1));
    QCOMPARE(service.status().lastSentence,
             QString::fromLatin1(fixture.output->writes.last()).trimmed());
}

void NmeaOutputServiceTest::noPeerRateChangeAndUdpNoPeerFollowMp10()
{
    Fixture tcp;
    tcp.output->kind = MavlinkMirrorOutput::Kind::TcpHost;
    tcp.output->peer = false;
    NmeaOutputService tcpService(tcp.factory(), tcp.clock());
    NmeaOutputSettings tcpSettings;
    tcpSettings.portSelection = NmeaOutputService::TcpHostSelection();
    QVERIFY(tcpService.start(endpoint(), QStringLiteral("Link"), tcpSettings));
    tcpService.observeMessage(42, globalPosition());
    tcpService.emitNow();
    QVERIFY(tcp.output->writes.isEmpty());
    QVERIFY(tcpService.status().lastSentence.isEmpty());
    tcpService.setRateHz(1.0);
    QCOMPARE(tcpService.timerIntervalMs(), 1000);

    Fixture udp;
    udp.output->kind = MavlinkMirrorOutput::Kind::UdpHost;
    udp.output->peer = false;
    NmeaOutputService udpService(udp.factory(), udp.clock());
    NmeaOutputSettings udpSettings;
    udpSettings.portSelection = NmeaOutputService::UdpHostSelection();
    QVERIFY(udpService.start(endpoint(), QStringLiteral("Link"), udpSettings));
    udpService.observeMessage(42, globalPosition());
    udpService.emitNow();
    QVERIFY(udp.output->writes.isEmpty());
    QVERIFY(udpService.status().lastSentence.startsWith(QStringLiteral("$GPRMC,")));
    QCOMPARE(udpService.status().droppedSentences, quint64(0));
}

void NmeaOutputServiceTest::boundsDropsErrorsAndLinkRemovalAreExplicit()
{
    Fixture fixture;
    NmeaOutputService service(fixture.factory(), fixture.clock());
    NmeaOutputSettings settings;
    settings.portSelection = QStringLiteral("ttyTEST");
    QVERIFY(service.start(endpoint(), QStringLiteral("Vehicle Link"), settings));
    service.observeMessage(42, globalPosition());

    fixture.output->pending = MavlinkMirrorOutput::OutputPendingLimitBytes;
    service.emitNow();
    QCOMPARE(service.status().droppedSentences, quint64(5));
    QVERIFY(service.status().text.contains(QStringLiteral("dropped 5")));
    fixture.output->pending = 0;

    fixture.output->nextWriteResult = 1;
    service.emitNow();
    QCOMPARE(service.state(), NmeaOutputService::State::Failed);
    QVERIFY(service.status().text.startsWith(QStringLiteral("Error connecting:")));

    Fixture removed;
    NmeaOutputService other(removed.factory(), removed.clock());
    QVERIFY(other.start(endpoint(), QStringLiteral("Vehicle Link"), settings));
    other.forgetLink(9);
    QVERIFY(other.isRunning());
    other.forgetLink(42);
    QVERIFY(!other.isRunning());
    QCOMPARE(other.status().text,
             QStringLiteral("Stopped: source link Vehicle Link was removed."));
}

void NmeaOutputServiceTest::guardsAndOpenFailuresLeaveNothingRunning()
{
    Fixture fixture;
    NmeaOutputService service(fixture.factory(), fixture.clock());
    quint16 guardedPort = 0;
    service.setUdpPortGuard([&guardedPort](quint16 port, QString *reason) {
        guardedPort = port;
        *reason = QStringLiteral("busy");
        return false;
    });
    NmeaOutputSettings settings;
    settings.portSelection = NmeaOutputService::UdpHostSelection();
    QVERIFY(!service.start(endpoint(), QStringLiteral("Link"), settings));
    QCOMPARE(guardedPort, quint16(14551));
    QVERIFY(!service.isRunning());
    QCOMPARE(service.status().text,
             QStringLiteral("Error connecting: busy"));

    Fixture failure;
    failure.output->openSucceeds = false;
    NmeaOutputService failed(failure.factory(), failure.clock());
    settings.portSelection = QStringLiteral("ttyTEST");
    QVERIFY(!failed.start(endpoint(), QStringLiteral("Link"), settings));
    QCOMPARE(failed.state(), NmeaOutputService::State::Failed);
    QCOMPARE(failed.status().text,
             QStringLiteral("Error connecting: open failed"));
}

QTEST_MAIN(NmeaOutputServiceTest)
#include "test_nmeaoutputservice.moc"
