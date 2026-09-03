#include <QtTest>

#include "comm/MavlinkMirrorService.h"

#include <QPointer>
#include <QSignalSpy>

#include <memory>

namespace {

/** Scriptable output: controls peer presence, per-write acceptance and drain signalling. */
class FakeOutput final : public MavlinkMirrorOutput
{
public:
    explicit FakeOutput(const QString &selection = QStringLiteral("fake"))
        : m_selection(selection)
    {
    }
    ~FakeOutput() override
    {
        if (destroyed) {
            *destroyed = true;
        }
    }

    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return m_selection; }
    bool open(QString *error) override
    {
        if (!openResult && error) {
            *error = QStringLiteral("boom");
        }
        m_open = openResult;
        return m_open;
    }
    void close() override { m_open = false; ++closeCalls; }
    bool isOpen() const override { return m_open; }
    bool hasPeer() const override { return m_open && peer; }
    qint64 write(const QByteArray &bytes) override
    {
        ++writeCalls;
        if (failWrites) {
            return -1;
        }
        const qint64 accepted = qMin<qint64>(acceptPerWrite, bytes.size());
        if (accepted <= 0) {
            return 0;
        }
        written.append(bytes.left(static_cast<int>(accepted)));
        chunks.append(bytes.left(static_cast<int>(accepted)));
        if (syncBytesWritten) {
            emit bytesWritten(accepted);   // synchronous re-entrancy into the service
        }
        return accepted;
    }
    qint64 pendingBytes() const override { return pending; }
    QString statusText() const override
    {
        if (!m_open) {
            return QStringLiteral("Stopped.");
        }
        return peer ? QStringLiteral("Mirroring on %1.").arg(m_selection)
                    : QStringLiteral("Listening on %1 — waiting for a client…").arg(m_selection);
    }

    void setPeer(bool present)
    {
        if (peer != present) {
            peer = present;
            emit peerChanged();
        }
    }
    void feedPeer(const QByteArray &bytes) { emit peerBytesReceived(bytes); }
    void flush() { emit bytesWritten(1); }
    void raiseError(const QString &text) { emit errorOccurred(text); }

    bool openResult = true;
    bool peer = true;
    bool failWrites = false;
    bool syncBytesWritten = false;
    qint64 acceptPerWrite = 1 << 30;
    qint64 pending = 0;   // reported as not yet transmitted output bytes
    int writeCalls = 0;
    int closeCalls = 0;
    QByteArray written;
    QList<QByteArray> chunks;
    bool *destroyed = nullptr;

private:
    QString m_selection;
    bool m_open = false;
};

struct Harness
{
    QList<QPair<int, QByteArray>> writes;
    bool writerResult = true;
    QPointer<FakeOutput> output;   // last output handed to the service
    std::function<void(FakeOutput *)> configure;
    std::unique_ptr<MavlinkMirrorService> service;

    Harness()
    {
        service = std::make_unique<MavlinkMirrorService>(
            [this](int linkId, const QByteArray &bytes) {
                writes.append(qMakePair(linkId, bytes));
                return writerResult;
            },
            [this](const MavlinkMirrorSettings &settings, QString *error) {
                Q_UNUSED(error)
                auto created = std::make_unique<FakeOutput>(settings.portSelection);
                if (configure) {
                    configure(created.get());
                }
                output = created.get();
                return std::unique_ptr<MavlinkMirrorOutput>(std::move(created));
            });
    }

    MavlinkMirrorSettings settings(const QString &selection = QStringLiteral("ttyMIRROR"),
                                   bool writeBack = false) const
    {
        MavlinkMirrorSettings s;
        s.portSelection = selection;
        s.baud = 57600;
        s.allowWriteBack = writeBack;
        return s;
    }
};

QByteArray frame(char fill, int size = 20)
{
    QByteArray bytes(size, fill);
    bytes[0] = static_cast<char>(0xFD);
    return bytes;
}

} // namespace

class MavlinkMirrorServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void startStopStateAndTexts();
    void refusalsLeaveNothingOpen();
    void pinnedLinkFilteringKeepsOrder();
    void writeBackOffOnLiveAndWriterFailure();
    void partialWritesPreserveOrder();
    void boundDropsNewestFrames();
    void boundIncludesOutputPendingBytes();
    void synchronousBytesWrittenIsReentrancySafe();
    void sourceRemovalStopsOnlyThePinnedLink();
    void outputErrorFailsTheSession();
    void clearRestartAndDestruction();
};

void MavlinkMirrorServiceTest::startStopStateAndTexts()
{
    Harness h;
    QSignalSpy changed(h.service.get(), &MavlinkMirrorService::statusChanged);
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Stopped);
    QCOMPARE(h.service->status().text, QStringLiteral("Stopped."));
    QVERIFY(!h.service->isRunning());
    QCOMPARE(MavlinkMirrorService::Selections({}).size(), 2);

    h.configure = [](FakeOutput *o) { o->peer = false; };
    QString error;
    QVERIFY(h.service->start(3, QStringLiteral("COM7"), h.settings(), &error));
    QVERIFY(error.isEmpty());
    QVERIFY(h.service->isRunning());
    QCOMPARE(h.service->linkId(), 3);
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Listening);
    QCOMPARE(h.service->status().text, QStringLiteral("Listening on ttyMIRROR — waiting for a client…"));
    QCOMPARE(h.service->status().linkName, QStringLiteral("COM7"));
    QCOMPARE(h.service->status().selection, QStringLiteral("ttyMIRROR"));
    QCOMPARE(changed.count(), 1);

    h.output->setPeer(true);
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Mirroring);
    QCOMPARE(h.service->status().text, QStringLiteral("Mirroring on ttyMIRROR."));
    QCOMPARE(changed.count(), 2);

    h.service->stop();
    QVERIFY(!h.service->isRunning());
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Stopped);
    QCOMPARE(h.service->status().text, QStringLiteral("Stopped."));
    QCOMPARE(h.service->status().linkId, -1);
    QCOMPARE(changed.count(), 3);
    QVERIFY(h.output);   // released with deleteLater, still alive until the loop runs
    QCOMPARE(h.output->closeCalls, 1);
    h.service->stop();   // idempotent: no extra notification
    QCOMPARE(changed.count(), 3);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(h.output.isNull());
}

void MavlinkMirrorServiceTest::refusalsLeaveNothingOpen()
{
    Harness h;
    int factoryCalls = 0;
    h.configure = [&factoryCalls](FakeOutput *) { ++factoryCalls; };
    QString error;

    QVERIFY(!h.service->start(3, QStringLiteral("COM7"), h.settings(QStringLiteral("  ")), &error));
    QCOMPARE(error, QStringLiteral("Pick a port first."));
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Stopped);
    QCOMPARE(h.service->status().text, QStringLiteral("Pick a port first."));
    QCOMPARE(factoryCalls, 0);

    QVERIFY(!h.service->start(-1, QString(), h.settings(), &error));
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Failed);
    QVERIFY2(error.startsWith(QStringLiteral("Error connecting: No current link")), qPrintable(error));
    QCOMPARE(factoryCalls, 0);

    // UDP guard refuses before any output exists.
    int guardCalls = 0;
    h.service->setUdpPortGuard([&guardCalls](quint16 port, QString *reason) {
        ++guardCalls;
        *reason = QStringLiteral("UDP port %1 is used by the active UDP link").arg(port);
        return false;
    });
    QVERIFY(!h.service->start(3, QStringLiteral("UDP"), h.settings(MavlinkMirrorOutput::UdpHostSelection()), &error));
    QCOMPARE(guardCalls, 1);
    QCOMPARE(factoryCalls, 0);
    QCOMPARE(error, QStringLiteral("Error connecting: UDP port 14550 is used by the active UDP link"));
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Failed);
    QVERIFY(!h.service->isRunning());
    // The guard is not consulted for other selections.
    QVERIFY(h.service->start(3, QStringLiteral("COM7"), h.settings(MavlinkMirrorOutput::TcpHostSelection()), &error));
    QCOMPARE(guardCalls, 1);
    QCOMPARE(factoryCalls, 1);
    h.service->stop();

    // Open failure: closed, released, Failed with the reason.
    h.configure = [&factoryCalls](FakeOutput *o) { ++factoryCalls; o->openResult = false; };
    QVERIFY(!h.service->start(3, QStringLiteral("COM7"), h.settings(), &error));
    QCOMPARE(error, QStringLiteral("Error connecting: boom"));
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Failed);
    QVERIFY(!h.service->isRunning());
    QCOMPARE(h.service->status().linkId, -1);
    h.service->observeFrame(3, frame('a'));   // ignored while not running
    QCOMPARE(h.service->status().txBytes, quint64(0));

    // Factory failure.
    MavlinkMirrorService failing([](int, const QByteArray &) { return true; },
                                 [](const MavlinkMirrorSettings &, QString *reason) {
                                     *reason = QStringLiteral("no such transport");
                                     return std::unique_ptr<MavlinkMirrorOutput>();
                                 });
    QVERIFY(!failing.start(1, QStringLiteral("L"), h.settings(), &error));
    QCOMPARE(error, QStringLiteral("Error connecting: no such transport"));
}

void MavlinkMirrorServiceTest::pinnedLinkFilteringKeepsOrder()
{
    Harness h;
    QVERIFY(h.service->start(5, QStringLiteral("udp:14550"), h.settings()));
    const QByteArray a = frame('a', 12);
    const QByteArray b = frame('b', 30);
    const QByteArray c = frame('c', 7);
    h.service->observeFrame(5, a);
    h.service->observeFrame(4, frame('x'));   // another physical link: ignored
    h.service->observeFrame(6, frame('y'));
    h.service->observeFrame(5, b);
    h.service->observeFrame(5, QByteArray());  // empty: ignored
    h.service->observeFrame(5, c);
    QCOMPARE(h.output->written, a + b + c);
    QCOMPARE(h.output->chunks.size(), 3);
    QCOMPARE(h.service->status().txBytes, quint64(a.size() + b.size() + c.size()));
    QCOMPARE(h.service->status().pendingBytes, qint64(0));
    QCOMPARE(h.service->status().droppedBytes, quint64(0));

    // No peer: frames are discarded, not queued, not counted as drops.
    h.output->setPeer(false);
    h.service->observeFrame(5, frame('n'));
    QCOMPARE(h.service->status().pendingBytes, qint64(0));
    QCOMPARE(h.service->status().droppedBytes, quint64(0));
    QCOMPARE(h.output->written, a + b + c);
}

void MavlinkMirrorServiceTest::writeBackOffOnLiveAndWriterFailure()
{
    Harness h;
    QVERIFY(h.service->start(9, QStringLiteral("COM3"), h.settings()));
    QSignalSpy changed(h.service.get(), &MavlinkMirrorService::statusChanged);

    h.output->feedPeer(QByteArray("peer1"));
    QCOMPARE(h.service->status().rxBytes, quint64(5));
    QVERIFY(h.writes.isEmpty());   // write back off: counted, not forwarded

    h.service->setAllowWriteBack(true);   // live toggle
    QCOMPARE(changed.count(), 1);
    QVERIFY(h.service->status().allowWriteBack);
    h.output->feedPeer(QByteArray("peer2"));
    QCOMPARE(h.writes.size(), 1);
    QCOMPARE(h.writes.first().first, 9);
    QCOMPARE(h.writes.first().second, QByteArray("peer2"));
    QCOMPARE(h.service->status().rxBytes, quint64(10));

    h.service->setAllowWriteBack(false);
    h.output->feedPeer(QByteArray("peer3"));
    QCOMPARE(h.writes.size(), 1);
    QCOMPARE(h.service->status().rxBytes, quint64(15));

    // Writer failure disables write back, keeps mirroring, and says so.
    h.service->setAllowWriteBack(true);
    h.writerResult = false;
    h.output->feedPeer(QByteArray("peer4"));
    QCOMPARE(h.writes.size(), 2);
    QVERIFY(!h.service->status().allowWriteBack);
    QVERIFY(h.service->status().writeBackFailed);
    QVERIFY(h.service->status().text.contains(QStringLiteral("Write back failed")));
    QVERIFY(h.service->isRunning());
    h.output->feedPeer(QByteArray("peer5"));
    QCOMPARE(h.writes.size(), 2);   // stays disabled
    h.service->observeFrame(9, frame('z'));
    QCOMPARE(h.output->written, frame('z'));   // vehicle -> peer still flows

    // Re-enabling clears the failure note.
    h.writerResult = true;
    h.service->setAllowWriteBack(true);
    QVERIFY(!h.service->status().writeBackFailed);

    // Settings can start with write back already on.
    Harness on;
    QVERIFY(on.service->start(2, QStringLiteral("L"), on.settings(QStringLiteral("p"), true)));
    on.output->feedPeer(QByteArray("x"));
    QCOMPARE(on.writes.size(), 1);
}

void MavlinkMirrorServiceTest::partialWritesPreserveOrder()
{
    Harness h;
    h.configure = [](FakeOutput *o) { o->acceptPerWrite = 5; };
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    const QByteArray a = frame('a', 12);
    const QByteArray b = frame('b', 8);
    h.service->observeFrame(1, a);
    // First write takes 5 bytes, then the service waits for bytesWritten.
    QCOMPARE(h.output->written, a.left(5));
    QCOMPARE(h.service->status().pendingBytes, qint64(7));
    QCOMPARE(h.service->status().txBytes, quint64(5));
    h.service->observeFrame(1, b);   // queued behind the remainder; arrival retries the head
    QCOMPARE(h.output->written, a.left(10));
    QCOMPARE(h.service->status().pendingBytes, qint64(10));
    QCOMPARE(h.service->status().txBytes, quint64(10));

    h.output->flush();   // 2 of a complete it, then 5 of b in the same pass
    QCOMPARE(h.output->written, a + b.left(5));
    QCOMPARE(h.service->status().pendingBytes, qint64(3));
    h.output->flush();
    QCOMPARE(h.output->written, a + b);
    QCOMPARE(h.service->status().pendingBytes, qint64(0));
    QCOMPARE(h.service->status().txBytes, quint64(20));
    h.output->flush();   // nothing pending: harmless
    QCOMPARE(h.output->written, a + b);
}

void MavlinkMirrorServiceTest::boundDropsNewestFrames()
{
    Harness h;
    h.configure = [](FakeOutput *o) { o->acceptPerWrite = 0; };   // peer present but blocked
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    const QByteArray big(1024, 'q');
    const int fits = static_cast<int>(MavlinkMirrorService::PendingLimitBytes / big.size());
    for (int i = 0; i < fits + 5; ++i) {
        h.service->observeFrame(1, big);
    }
    QCOMPARE(h.service->status().pendingBytes, MavlinkMirrorService::PendingLimitBytes);
    QCOMPARE(h.service->status().droppedFrames, quint64(5));
    QCOMPARE(h.service->status().droppedBytes, quint64(5 * big.size()));
    QVERIFY(h.service->status().text.endsWith(QStringLiteral("(dropped %1 bytes)").arg(5 * big.size())));
    QByteArray marker = frame('M', 10);
    h.service->observeFrame(1, marker);   // still full: newest dropped, oldest intact
    QCOMPARE(h.service->status().droppedFrames, quint64(6));

    h.output->acceptPerWrite = 1 << 30;
    h.output->flush();
    QCOMPARE(h.service->status().pendingBytes, qint64(0));
    QCOMPARE(h.output->written.size(), static_cast<int>(MavlinkMirrorService::PendingLimitBytes));
    QVERIFY(!h.output->written.contains('M'));
    QCOMPARE(h.service->status().txBytes, quint64(MavlinkMirrorService::PendingLimitBytes));
    // Room again: new frames flow.
    h.service->observeFrame(1, marker);
    QVERIFY(h.output->written.endsWith(marker));
}

void MavlinkMirrorServiceTest::boundIncludesOutputPendingBytes()
{
    Harness h;
    const qint64 outputPending = MavlinkMirrorOutput::OutputPendingLimitBytes;   // 64 KiB
    h.configure = [outputPending](FakeOutput *o) {
        o->acceptPerWrite = 0;
        o->pending = outputPending;
    };
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    const QByteArray big(1024, 'q');
    const int fits = static_cast<int>((MavlinkMirrorService::PendingLimitBytes - outputPending) / big.size());
    for (int i = 0; i < fits; ++i) {
        h.service->observeFrame(1, big);
    }
    // Exactly at the bound: accepted, nothing dropped.
    QCOMPARE(h.service->status().pendingBytes, MavlinkMirrorService::PendingLimitBytes - outputPending);
    QCOMPARE(h.service->status().droppedFrames, quint64(0));
    // One more byte over the bound: dropped.
    h.service->observeFrame(1, frame('x', 1));
    QCOMPARE(h.service->status().droppedFrames, quint64(1));
    QCOMPARE(h.service->status().droppedBytes, quint64(1));
    QCOMPARE(h.service->status().pendingBytes, MavlinkMirrorService::PendingLimitBytes - outputPending);
    // The output drained its own buffer: the same frame now fits.
    h.output->pending = 0;
    h.service->observeFrame(1, frame('x', 1));
    QCOMPARE(h.service->status().droppedFrames, quint64(1));
    QCOMPARE(h.service->status().pendingBytes, MavlinkMirrorService::PendingLimitBytes - outputPending + 1);
}

void MavlinkMirrorServiceTest::synchronousBytesWrittenIsReentrancySafe()
{
    Harness h;
    h.configure = [](FakeOutput *o) { o->syncBytesWritten = true; o->acceptPerWrite = 4; };
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    const QByteArray a = frame('a', 10);
    const QByteArray b = frame('b', 6);
    h.service->observeFrame(1, a);
    h.service->observeFrame(1, b);
    // Every write re-enters through bytesWritten; the drain must neither recurse
    // unboundedly nor duplicate or reorder bytes.
    QCOMPARE(h.output->written, a + b);
    QCOMPARE(h.service->status().pendingBytes, qint64(0));
    QCOMPARE(h.service->status().txBytes, quint64(16));
    QCOMPARE(h.output->writeCalls, 5);   // a: 4+4+2, b: 4+2 -> five writes, no duplicates
}

void MavlinkMirrorServiceTest::sourceRemovalStopsOnlyThePinnedLink()
{
    Harness h;
    QVERIFY(h.service->start(7, QStringLiteral("COM9"), h.settings()));
    QSignalSpy changed(h.service.get(), &MavlinkMirrorService::statusChanged);
    h.service->forgetLink(8);
    QVERIFY(h.service->isRunning());
    QCOMPARE(changed.count(), 0);

    h.service->forgetLink(7);
    QVERIFY(!h.service->isRunning());
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Stopped);
    QCOMPARE(h.service->status().text, QStringLiteral("Stopped: source link COM9 was removed."));
    QCOMPARE(h.service->status().linkId, -1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(h.output->closeCalls, 1);
    h.service->observeFrame(7, frame('a'));
    QCOMPARE(h.service->status().txBytes, quint64(0));
    h.service->forgetLink(7);   // already stopped: silent
    QCOMPARE(changed.count(), 1);
}

void MavlinkMirrorServiceTest::outputErrorFailsTheSession()
{
    Harness h;
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    h.output->raiseError(QStringLiteral("Serial port ttyMIRROR: device removed"));
    QVERIFY(!h.service->isRunning());
    QCOMPARE(h.service->state(), MavlinkMirrorService::State::Failed);
    QCOMPARE(h.service->status().text,
             QStringLiteral("Error connecting: Serial port ttyMIRROR: device removed"));
    QCOMPARE(h.output->closeCalls, 1);

    // A failing write does the same and drops the queue.
    Harness w;
    w.configure = [](FakeOutput *o) { o->failWrites = true; };
    QVERIFY(w.service->start(1, QStringLiteral("L"), w.settings()));
    w.service->observeFrame(1, frame('a'));
    QCOMPARE(w.service->state(), MavlinkMirrorService::State::Failed);
    QCOMPARE(w.service->status().text, QStringLiteral("Error connecting: cannot write to ttyMIRROR"));
    QCOMPARE(w.service->status().pendingBytes, qint64(0));
}

void MavlinkMirrorServiceTest::clearRestartAndDestruction()
{
    Harness h;
    QVERIFY(h.service->start(1, QStringLiteral("L"), h.settings()));
    h.service->observeFrame(1, frame('a'));
    h.output->feedPeer(QByteArray("rx"));
    QCOMPARE(h.service->status().txBytes, quint64(20));
    QCOMPARE(h.service->status().rxBytes, quint64(2));
    QPointer<FakeOutput> first = h.output;

    h.service->clear();   // shutdown path
    QVERIFY(!h.service->isRunning());
    QCOMPARE(h.service->status().text, QStringLiteral("Stopped."));
    QCOMPARE(first->closeCalls, 1);

    // Restart resets counters and gets a fresh output; a running restart stops the old one first.
    QVERIFY(h.service->start(2, QStringLiteral("M"), h.settings(QStringLiteral("other"))));
    QVERIFY(h.output != first);
    QCOMPARE(h.service->status().txBytes, quint64(0));
    QCOMPARE(h.service->status().rxBytes, quint64(0));
    QCOMPARE(h.service->status().linkId, 2);
    QPointer<FakeOutput> second = h.output;
    QVERIFY(h.service->start(3, QStringLiteral("N"), h.settings(QStringLiteral("third"))));
    QCOMPARE(second->closeCalls, 1);
    QCOMPARE(h.service->status().linkId, 3);

    // Destroying a running service closes and destroys the output at once:
    // the output is parented to the service, no event loop is needed.
    bool destroyed = false;
    h.output->destroyed = &destroyed;
    QPointer<FakeOutput> third = h.output;
    QCOMPARE(third->parent(), h.service.get());
    h.service.reset();
    QVERIFY(destroyed);
    QVERIFY(third.isNull());

    // stop() followed by immediate destruction (window closed before the loop
    // runs): the deleteLater'd output is still reclaimed through its parent.
    Harness late;
    QVERIFY(late.service->start(1, QStringLiteral("L"), late.settings()));
    bool lateDestroyed = false;
    late.output->destroyed = &lateDestroyed;
    QPointer<FakeOutput> lateOutput = late.output;
    late.service->stop();
    QVERIFY(lateOutput);   // pending deleteLater
    late.service.reset();
    QVERIFY(lateDestroyed);
    QVERIFY(lateOutput.isNull());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // stale event: harmless
}

QTEST_GUILESS_MAIN(MavlinkMirrorServiceTest)
#include "test_mavlinkmirrorservice.moc"
