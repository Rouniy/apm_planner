#include <QtTest>

#include "comm/MicrodroneDownlinkService.h"

#include <QPointer>
#include <functional>
#include <limits>
#include <memory>

namespace {
struct OutputEvidence {
    int opens = 0, closes = 0, destructions = 0, writes = 0;
    QByteArray accepted;
    qint64 largestRequest = 0;
};

class FakeOutput final : public MavlinkMirrorOutput {
public:
    explicit FakeOutput(std::shared_ptr<OutputEvidence> evidence) : evidence(std::move(evidence)) {}
    ~FakeOutput() override { ++evidence->destructions; }
    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return QStringLiteral("fakeMicrodrone"); }
    bool open(QString *error) override {
        const auto retained = evidence;
        ++retained->opens; opened = openResult;
        const bool result = openResult;
        if (!result && error) *error = QStringLiteral("Injected open failure.");
        const auto callback = onOpen;
        if (callback) callback();
        return result;
    }
    void close() override {
        const auto retained = evidence;
        ++retained->closes; opened = false;
        const auto callback = onClose;
        if (callback) callback();
    }
    bool isOpen() const override { return opened; }
    bool hasPeer() const override { return opened; }
    qint64 pendingBytes() const override { return pending; }
    QString statusText() const override { return QStringLiteral("Fake serial output."); }
    qint64 write(const QByteArray &bytes) override {
        const auto retained = evidence;
        ++retained->writes;
        retained->largestRequest = qMax(retained->largestRequest, qint64(bytes.size()));
        const qint64 limit = script.isEmpty() ? acceptPerWrite : script.takeFirst();
        const qint64 accepted = limit < 0 ? limit : qMin(limit, qint64(bytes.size()));
        if (accepted > 0) retained->accepted.append(bytes.left(int(accepted)));
        const bool notify = synchronousBytesWritten;
        const auto callback = onWrite;
        QPointer<FakeOutput> guard(this);
        if (callback) callback();
        if (guard && notify && accepted > 0) emit bytesWritten(accepted);
        return accepted;
    }
    void drainSignal() { emit bytesWritten(1); }
    void fail(const QString &text) { emit errorOccurred(text); }

    std::shared_ptr<OutputEvidence> evidence;
    bool opened = false, openResult = true, synchronousBytesWritten = false;
    qint64 pending = 0, acceptPerWrite = std::numeric_limits<qint64>::max();
    QList<qint64> script;
    std::function<void()> onOpen, onClose, onWrite;
};

MicrodroneSource selectedSource() {
    MicrodroneSource result;
    result.selection.endpoint.linkId = 7;
    result.selection.endpoint.systemId = 42;
    result.selection.endpoint.componentId = 1;
    result.selection.generation = 13;
    result.instance.endpoint = result.selection.endpoint;
    result.instance.linkSessionEpoch = 21;
    result.instance.instanceEpoch = 31;
    result.linkName = QStringLiteral("Selected physical telemetry link");
    return result;
}

struct Harness {
    MicrodroneSource selected = selectedSource();
    QDateTime now = QDateTime(QDate(2026, 9, 6), QTime(12, 34, 56), Qt::UTC);
    std::shared_ptr<OutputEvidence> evidence = std::make_shared<OutputEvidence>();
    QPointer<FakeOutput> output;
    std::function<void(FakeOutput *)> configure;
    std::function<void()> onFactory, onResolver, onClock, onValidate;
    QString portError;
    bool factoryFails = false;
    int factories = 0;
    MicrodroneOutputSettings lastSettings;
    std::unique_ptr<MicrodroneDownlinkService> service;

    Harness() {
        MicrodroneDownlinkService::Dependencies dependencies;
        dependencies.resolveSource = [this] {
            const auto callback = onResolver;
            if (callback) callback();
            return selected;
        };
        dependencies.validatePort = [this](const QString &) {
            const auto callback = onValidate;
            if (callback) callback();
            return portError;
        };
        dependencies.clock = [this] {
            const auto callback = onClock;
            if (callback) callback();
            return now;
        };
        dependencies.outputFactory = [this](const MicrodroneOutputSettings &settings, QString *error)
            -> std::unique_ptr<MavlinkMirrorOutput> {
            ++factories; lastSettings = settings;
            if (factoryFails) {
                if (error) *error = QStringLiteral("Injected factory failure.");
                return {};
            }
            auto result = std::make_unique<FakeOutput>(evidence);
            output = result.get();
            if (configure) configure(result.get());
            const auto callback = onFactory;
            if (callback) callback();
            return result;
        };
        service = std::make_unique<MicrodroneDownlinkService>(std::move(dependencies));
    }
    MicrodroneOutputSettings settings() const {
        MicrodroneOutputSettings result; result.port = QStringLiteral("fakeMicrodrone"); return result;
    }
};

mavlink_message_t position(int systemId = 42, int componentId = 1, qint32 latitude = 515000000) {
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(systemId, componentId, &message, 0,
        latitude, 134000000, 70000, 43000, 0, 0, 0, 0);
    return message;
}

bool sevenValidRecords(const QByteArray &frame) {
    const QList<QByteArray> records = frame.split('\n');
    if (records.size() != 8 || !records.last().isEmpty()) return false;
    const QList<QByteArray> names = {"#1,", "#4,", "#5,", "#6,", "#7,", "#8,", "#9,"};
    for (int i = 0; i < 7; ++i) {
        QByteArray record = records.at(i);
        if (!record.startsWith(names.at(i)) || !record.endsWith('\r')) return false;
        record.chop(1);
        const int comma = record.lastIndexOf(',');
        if (comma < 0) return false;
        const QByteArray payload = record.left(comma + 1);
        quint8 checksum = 0;
        for (const char byte : payload) checksum = quint8(checksum + quint8(byte));
        bool ok = false;
        const int actual = record.mid(comma + 1).toInt(&ok);
        if (!ok || actual != int(quint8(checksum ^ 0xff))) return false;
    }
    return true;
}

void retireOutputs() {
    // The service deliberately keeps signal senders alive through the Qt
    // emission stack and retires them with deleteLater().
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
}

class MicrodroneDownlinkServiceTest final : public QObject {
    Q_OBJECT
private slots:
    void defaultsAndAdmission() {
        QCOMPARE(MicrodroneOutputSettings{}.baud, 57600);
        QCOMPARE(MicrodroneDownlinkService::Bauds(), QList<int>({4800,9600,14400,19200,28800,38400,57600,115200}));
        Harness h;
        QVERIFY(!h.service->isRunning()); QVERIFY(!h.service->busy());
        QVERIFY(h.service->lastLine().isEmpty()); QCOMPARE(h.service->framesSubmitted(), quint64(0));
        auto settings = h.settings(); QString error;
        settings.port.clear(); QVERIFY(!h.service->start(settings, &error)); QVERIFY(!error.isEmpty());
        settings = h.settings(); settings.baud = 12345;
        QVERIFY(!h.service->start(settings, &error)); QVERIFY(!error.isEmpty());
        h.portError = QStringLiteral("Output is already used by telemetry.");
        QVERIFY(!h.service->start(h.settings(), &error)); QVERIFY(error.contains("already used"));
        h.portError.clear(); h.selected = {};
        QVERIFY(!h.service->start(h.settings(), &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(h.factories, 0); QCOMPARE(h.evidence->writes, 0);
        MicrodroneDownlinkService missing(MicrodroneDownlinkService::Dependencies{});
        QVERIFY(!missing.start(h.settings(), &error)); QVERIFY(!error.isEmpty());
    }

    void factoryAndOpenFailure() {
        Harness h; QString error;
        h.factoryFails = true;
        QVERIFY(!h.service->start(h.settings(), &error)); QVERIFY(error.contains("factory"));
        h.factoryFails = false; h.configure = [](FakeOutput *output) { output->openResult = false; };
        QVERIFY(!h.service->start(h.settings(), &error)); QVERIFY(error.contains("open"));
        QVERIFY(!h.service->isRunning()); QCOMPARE(h.evidence->writes, 0);
        retireOutputs();
        QVERIFY(h.evidence->closes <= 1); QCOMPARE(h.evidence->destructions, 1);
    }

    void callerSettingsArePinnedBeforeValidationCallbacks() {
        Harness h;
        auto settings = h.settings();
        bool mutated = false;
        QObject::connect(h.service.get(), &MicrodroneDownlinkService::changed, h.service.get(), [&] {
            if (!mutated && h.service->state() == MicrodroneDownlinkService::State::Opening) {
                mutated = true;
                settings.port = QStringLiteral("unvalidated-primary-telemetry-port");
                settings.baud = 12345;
            }
        });
        QVERIFY(h.service->start(settings)); QVERIFY(mutated);
        QCOMPARE(h.lastSettings.port, QStringLiteral("fakeMicrodrone"));
        QCOMPARE(h.lastSettings.baud, 57600);
    }

    void idleCacheHasExactEndpointAndEpoch() {
        Harness h; h.service->synchronizeSource();
        h.service->observeMessage(8, 21, position());
        h.service->observeMessage(7, 22, position());
        h.service->observeMessage(7, 21, position(43, 1));
        h.service->observeMessage(7, 21, position(42, 2));
        QCOMPARE(h.service->telemetry().latitude, 0.0);
        h.service->observeMessage(7, 21, position());
        QCOMPARE(h.service->telemetry().latitude, 51.5);
        QVERIFY(!h.service->isRunning()); QCOMPARE(h.factories, 0);
        ++h.selected.selection.generation; h.service->synchronizeSource();
        QCOMPARE(h.service->telemetry().latitude, 0.0);
        h.service->observeMessage(7, 21, position(42, 1, 520000000));
        QCOMPARE(h.service->telemetry().latitude, 52.0);
    }

    void idleClockRetargetCannotPopulateSuccessorCache() {
        Harness h; h.service->synchronizeSource();
        bool switched = false;
        h.onClock = [&] {
            if (switched) return;
            switched = true;
            ++h.selected.selection.generation;
            h.selected.selection.endpoint.systemId = 43;
            h.selected.instance.endpoint = h.selected.selection.endpoint;
            ++h.selected.instance.instanceEpoch;
            h.service->synchronizeSource();
        };
        h.service->observeMessage(7, 21, position());
        QVERIFY(switched); QVERIFY(!h.service->isRunning());
        QCOMPARE(h.service->source().selection.endpoint.systemId, 43);
        QCOMPARE(h.service->telemetry().latitude, 0.0);
        h.service->observeMessage(7, 21, position(43, 1, 520000000));
        QCOMPARE(h.service->telemetry().latitude, 52.0);
        QCOMPARE(h.factories, 0);
    }

    void idleNestedResolverCannotRestoreOlderSource() {
        MicrodroneSource selected = selectedSource();
        bool armed = false, nested = false;
        std::unique_ptr<MicrodroneDownlinkService> service;
        MicrodroneDownlinkService::Dependencies dependencies;
        dependencies.resolveSource = [&] {
            const auto oldSnapshot = selected;
            if (armed && !nested) {
                nested = true;
                ++selected.selection.generation;
                selected.selection.endpoint.systemId = 43;
                selected.instance.endpoint = selected.selection.endpoint;
                ++selected.instance.instanceEpoch;
                service->synchronizeSource();
            }
            return oldSnapshot;
        };
        service = std::make_unique<MicrodroneDownlinkService>(std::move(dependencies));
        service->synchronizeSource();
        QCOMPARE(service->source().selection.endpoint.systemId, 42);
        armed = true; service->synchronizeSource();
        QVERIFY(nested); QVERIFY(service->source().sameSource(selected));
        QCOMPARE(service->source().selection.endpoint.systemId, 43);
    }

    void initialManualAndTimedSevenRecordFrames() {
        Harness h; h.service->synchronizeSource();
        h.service->observeMessage(7, 21, position());
        const auto snapshot = h.service->telemetry();
        QString error;
        QVERIFY2(h.service->start(h.settings(), &error), qPrintable(error));
        QCOMPARE(h.evidence->opens, 1); QCOMPARE(h.lastSettings.port, h.settings().port);
        QCOMPARE(h.lastSettings.baud, 57600); QVERIFY(h.service->isRunning());
        QCOMPARE(h.service->framesSubmitted(), quint64(1));
        const auto first = h.evidence->accepted;
        QVERIFY(sevenValidRecords(first));
        QCOMPARE(first, MicrodroneDownlinkEncoder::EncodeFrame(snapshot, h.now, 0));
        QCOMPARE(h.service->lastLine(), QString::fromLatin1(first.trimmed().split('\n').last().trimmed()));
        h.now = h.now.addSecs(1); h.service->emitNow();
        QCOMPARE(h.service->framesSubmitted(), quint64(2));
        const auto second = h.evidence->accepted.mid(first.size());
        QVERIFY(sevenValidRecords(second));
        QCOMPARE(second, MicrodroneDownlinkEncoder::EncodeFrame(snapshot, h.now, 1));
        // Exercise real event-driven periodic delivery, without assuming
        // QTimer is a QObject child or exposing implementation-private state.
        QTRY_VERIFY_WITH_TIMEOUT(h.service->framesSubmitted() >= quint64(3), 1000);
        h.service->stop();
        const auto bytes = h.evidence->accepted;
        h.service->emitNow(); QCOMPARE(h.evidence->accepted, bytes);
        retireOutputs();
        QCOMPARE(h.evidence->closes, 1); QCOMPARE(h.evidence->destructions, 1);
    }

    void sourceChangesStopWithoutRetargeting_data() {
        QTest::addColumn<int>("mutation");
        QTest::newRow("generation-ABA") << 0; QTest::newRow("link") << 1;
        QTest::newRow("system") << 2; QTest::newRow("component") << 3;
        QTest::newRow("instance") << 4; QTest::newRow("physical-epoch") << 5;
        QTest::newRow("disappeared") << 6;
    }
    void sourceChangesStopWithoutRetargeting() {
        QFETCH(int, mutation); Harness h;
        QVERIFY(h.service->start(h.settings()));
        const QByteArray before = h.evidence->accepted;
        switch (mutation) {
        case 0: h.selected.selection.generation += 2; break;
        case 1: ++h.selected.selection.endpoint.linkId; h.selected.instance.endpoint = h.selected.selection.endpoint; break;
        case 2: ++h.selected.selection.endpoint.systemId; h.selected.instance.endpoint = h.selected.selection.endpoint; break;
        case 3: ++h.selected.selection.endpoint.componentId; h.selected.instance.endpoint = h.selected.selection.endpoint; break;
        case 4: ++h.selected.instance.instanceEpoch; break;
        case 5: ++h.selected.instance.linkSessionEpoch; break;
        case 6: h.selected = {}; break;
        }
        // The output loop must fence itself, without depending on UI refresh.
        h.service->emitNow(); QVERIFY(!h.service->isRunning());
        QCOMPARE(h.evidence->accepted, before); QCOMPARE(h.evidence->closes, 1);
        h.service->synchronizeSource(); h.service->emitNow();
        QCOMPARE(h.evidence->accepted, before); QCOMPARE(h.evidence->closes, 1);
    }

    void metadataAndUnrelatedRetirementDoNotInvalidate() {
        Harness h; QVERIFY(h.service->start(h.settings()));
        h.selected.linkName = QStringLiteral("Renamed metadata only");
        h.selected.selection.endpoint.componentName = QStringLiteral("Renamed component");
        h.service->synchronizeSource(); QVERIFY(h.service->isRunning());
        h.service->forgetLink(8, 21); h.service->forgetLink(7, 99);
        QVERIFY(h.service->isRunning());
        h.service->forgetLink(7, 21); QVERIFY(!h.service->isRunning());
        const auto before = h.evidence->accepted;
        h.service->emitNow(); QCOMPARE(h.evidence->accepted, before);
    }

    void partialAndZeroWritesKeepOneBoundedFrame() {
        Harness h;
        h.configure = [](FakeOutput *output) { output->script = {7}; output->acceptPerWrite = 0; };
        QVERIFY(h.service->start(h.settings())); QVERIFY(h.output);
        const QByteArray expected = MicrodroneDownlinkEncoder::EncodeFrame(h.service->telemetry(), h.now, 0);
        QCOMPARE(h.evidence->accepted, expected.left(7));
        QCOMPARE(h.service->framesSubmitted(), quint64(0)); QVERIFY(h.service->lastLine().isEmpty());
        const quint64 beforeDrops = h.service->droppedFrames();
        for (int i = 0; i < 1000; ++i) h.service->emitNow();
        QCOMPARE(h.service->droppedFrames(), beforeDrops + 1000);
        QCOMPARE(h.evidence->accepted, expected.left(7));
        QVERIFY(h.evidence->largestRequest <= expected.size());
        h.output->acceptPerWrite = std::numeric_limits<qint64>::max();
        h.output->drainSignal();
        QCOMPARE(h.service->framesSubmitted(), quint64(1));
        QCOMPARE(h.evidence->accepted, expected); QVERIFY(!h.service->lastLine().isEmpty());
        h.service->stop(); QCOMPARE(h.evidence->closes, 1);
    }

    void transportPendingDropsFreshFramesWithoutGrowth() {
        Harness h; h.configure = [](FakeOutput *output) { output->pending = 11; };
        QVERIFY(h.service->start(h.settings())); QVERIFY(h.output);
        QCOMPARE(h.service->framesSubmitted(), quint64(0)); QCOMPARE(h.evidence->writes, 0);
        const auto drops = h.service->droppedFrames();
        for (int i = 0; i < 1000; ++i) h.service->emitNow();
        QCOMPARE(h.service->droppedFrames(), drops + 1000); QCOMPARE(h.evidence->writes, 0);
        QVERIFY(h.service->lastLine().isEmpty());
        h.output->pending = 0; h.service->emitNow();
        QCOMPARE(h.service->framesSubmitted(), quint64(1)); QVERIFY(sevenValidRecords(h.evidence->accepted));
    }

    void lastCompletedLineSurvivesPartialAndSourceLossStopsDrain() {
        Harness h; QVERIFY(h.service->start(h.settings()));
        const auto completedLine = h.service->lastLine();
        const auto completedFrame = h.evidence->accepted;
        h.output->script = {7}; h.output->acceptPerWrite = 0;
        h.service->emitNow();
        QCOMPARE(h.service->framesSubmitted(), quint64(1));
        QCOMPARE(h.service->lastLine(), completedLine);
        QCOMPARE(h.evidence->accepted.size(), completedFrame.size() + 7);
        const auto partial = h.evidence->accepted;
        ++h.selected.instance.instanceEpoch;
        h.output->acceptPerWrite = std::numeric_limits<qint64>::max();
        h.output->drainSignal();
        QVERIFY(!h.service->isRunning()); QCOMPARE(h.evidence->accepted, partial);
        QCOMPARE(h.service->framesSubmitted(), quint64(1));
        QCOMPARE(h.service->lastLine(), completedLine);
    }

    void synchronousDrainAndErrors() {
        Harness h; h.configure = [](FakeOutput *output) { output->synchronousBytesWritten = true; };
        QVERIFY(h.service->start(h.settings()));
        QCOMPARE(h.service->framesSubmitted(), quint64(1)); QCOMPARE(h.evidence->writes, 1);
        const auto before = h.evidence->accepted;
        h.output->acceptPerWrite = -1; h.service->emitNow();
        QCOMPARE(h.service->state(), MicrodroneDownlinkService::State::Failed);
        QCOMPARE(h.evidence->accepted, before); QCOMPARE(h.service->framesSubmitted(), quint64(1));
        retireOutputs();
        QCOMPARE(h.evidence->closes, 1); QCOMPARE(h.evidence->destructions, 1);
        Harness signalled; QVERIFY(signalled.service->start(signalled.settings()));
        signalled.output->fail(QStringLiteral("Injected serial error."));
        QCOMPARE(signalled.service->state(), MicrodroneDownlinkService::State::Failed);
        QVERIFY(signalled.service->statusText().contains("Injected serial error"));
        QCOMPARE(signalled.evidence->closes, 1);
    }

    void badClocksFailWithoutPublishing_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("invalid-date") << 0; QTest::newRow("before-gps-epoch") << 1;
    }
    void badClocksFailWithoutPublishing() {
        QFETCH(int, failure); Harness h; QVERIFY(h.service->start(h.settings()));
        const auto before = h.evidence->accepted;
        if (failure == 0) h.now = {};
        else h.now = QDateTime(QDate(1979,1,1), QTime(0,0), Qt::UTC);
        h.service->emitNow();
        QCOMPARE(h.service->state(), MicrodroneDownlinkService::State::Failed);
        QCOMPARE(h.evidence->accepted, before); QVERIFY(!h.service->statusText().isEmpty());
        QCOMPARE(h.evidence->closes, 1);
    }

    void callbackStopAndDeletion_data() {
        QTest::addColumn<QString>("phase"); QTest::addColumn<bool>("destroy");
        for (const QString &phase : {QStringLiteral("open"), QStringLiteral("write"),
             QStringLiteral("opening-changed"), QStringLiteral("emitting-changed"),
             QStringLiteral("factory"), QStringLiteral("clock"), QStringLiteral("resolver"), QStringLiteral("validator")}) {
            QTest::newRow(qPrintable(phase + "-stop")) << phase << false;
            QTest::newRow(qPrintable(phase + "-delete")) << phase << true;
        }
    }
    void callbackStopAndDeletion() {
        QFETCH(QString, phase); QFETCH(bool, destroy); Harness h;
        bool fired = false;
        const auto interrupt = [&] {
            if (fired) return;
            fired = true;
            if (destroy) h.service.reset(); else h.service->stop();
        };
        if (phase == "open") h.configure = [&](FakeOutput *output) { output->onOpen = interrupt; };
        if (phase == "write") h.configure = [&](FakeOutput *output) { output->onWrite = interrupt; };
        if (phase == "factory") h.onFactory = interrupt;
        if (phase == "clock") h.onClock = interrupt;
        if (phase == "resolver") h.onResolver = interrupt;
        if (phase == "validator") h.onValidate = interrupt;
        if (phase.endsWith("-changed")) {
            QObject::connect(h.service.get(), &MicrodroneDownlinkService::changed, h.service.get(), [&] {
                const auto wanted = phase == "opening-changed" ? MicrodroneDownlinkService::State::Opening
                    : MicrodroneDownlinkService::State::Emitting;
                if (h.service && h.service->state() == wanted) interrupt();
            });
        }
        QPointer<MicrodroneDownlinkService> observer(h.service.get());
        auto *raw = h.service.get();
        const bool started = raw->start(h.settings());
        QVERIFY(fired); QVERIFY(!started);
        if (destroy) QVERIFY(observer.isNull());
        else { QVERIFY(h.service); QVERIFY(!h.service->isRunning()); QCOMPARE(h.service->framesSubmitted(), quint64(0)); }
        if (phase != "write") QCOMPARE(h.evidence->writes, 0);
        // Cancellation inside open() can require a second idempotent close:
        // a transport may complete opening only after the cancellation callback.
        const int maximumCloses = phase == "open" ? 2 : 1;
        QVERIFY(h.evidence->closes <= maximumCloses); QVERIFY(h.evidence->destructions <= 1);
        if (h.service) h.service->stop();
        retireOutputs();
        QVERIFY(h.evidence->closes <= maximumCloses); QVERIFY(h.output.isNull());
    }

    void closeCallbackAndRestartCannotCloseReplacement() {
        Harness h; QVERIFY(h.service->start(h.settings()));
        const auto old = h.evidence;
        bool closed = false;
        h.output->onClose = [&] {
            if (closed) return;
            closed = true; h.service->stop();
        };
        h.service->stop(); QVERIFY(closed);
        retireOutputs();
        QCOMPARE(old->closes, 1); QCOMPARE(old->destructions, 1);
        h.evidence = std::make_shared<OutputEvidence>();
        QVERIFY(h.service->start(h.settings()));
        QVERIFY(h.service->isRunning()); QCOMPARE(h.evidence->opens, 1);
        h.service.reset();
        retireOutputs();
        QCOMPARE(h.evidence->closes, 1); QCOMPARE(h.evidence->destructions, 1);
        QCOMPARE(old->closes, 1);
    }
};

QTEST_GUILESS_MAIN(MicrodroneDownlinkServiceTest)
#include "test_microdronedownlinkservice.moc"
