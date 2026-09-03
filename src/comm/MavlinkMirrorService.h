#ifndef MAVLINKMIRRORSERVICE_H
#define MAVLINKMIRRORSERVICE_H

#include "MavlinkMirrorOutput.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

/*
 * Mission Planner 10 MAVLink Mirror core (ExtLibs MAVLinkInterface.ProcessMirrorStream
 * + ViewModels/SerialPassThroughViewModel.cs), without UI and without LinkManager.
 *
 * One session at a time. start() pins the SOURCE as an immutable physical link id;
 * the session never follows later target changes and ends when that link is
 * forgotten. Complete framed vehicle->GCS bytes arrive through observeFrame() and
 * are queued FIFO; the FIFO plus the output's not yet transmitted bytes never
 * exceed PendingLimitBytes (the newest frame is dropped instead); partial writes keep the remainder
 * at the head so byte order is preserved. Raw peer bytes are counted and, while
 * allowWriteBack is on, handed unmodified to the RawWriter (the owner maps the
 * link id to LinkInterface::writeBytes; no LinkInterface pointer lives here).
 *
 * Everything runs on the GUI thread; nothing blocks. Draining is re-entrancy safe:
 * an output that signals bytesWritten synchronously from inside write() only
 * schedules another pass.
 */
class MavlinkMirrorService final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Listening,   // output open, no peer yet (TCP client / UDP datagram pending)
        Mirroring,
        Failed
    };
    Q_ENUM(State)

    struct Status
    {
        State state = State::Stopped;
        QString text;                 // MP10 status line (+ drop / write-back notes)
        QString selection;            // port selection of the current/last session
        int linkId = -1;              // pinned source link, -1 when stopped
        QString linkName;
        bool allowWriteBack = false;
        bool writeBackFailed = false;
        quint64 txBytes = 0;          // bytes accepted by the output (MP10 TxBytes)
        quint64 rxBytes = 0;          // raw bytes received from the peer (MP10 RxBytes)
        quint64 droppedBytes = 0;     // frames dropped because the queue was full
        quint64 droppedFrames = 0;
        qint64 pendingBytes = 0;      // queued in the service, not yet given to the output
    };

    using RawWriter = std::function<bool(int linkId, const QByteArray &bytes)>;
    using OutputFactory = std::function<std::unique_ptr<MavlinkMirrorOutput>(
        const MavlinkMirrorSettings &settings, QString *error)>;
    // Return false (with a reason) to refuse a UDP host bind, e.g. because the
    // primary UDP link already owns the port. Consulted before any socket exists.
    using UdpPortGuard = std::function<bool(quint16 port, QString *reason)>;

    // Hard bound on mirror-owned memory: service FIFO + output->pendingBytes().
    static constexpr qint64 PendingLimitBytes = 256 * 1024;

    static OutputFactory ProductionOutputFactory();
    static QStringList Selections(const QStringList &serialPorts)
    {
        return MavlinkMirrorOutput::Selections(serialPorts);
    }
    static QString StoppedText();                                  // "Stopped."
    static QString PickPortText();                                 // "Pick a port first."
    static QString ErrorConnectingText(const QString &reason);     // "Error connecting: <reason>"
    static QString SourceRemovedText(const QString &linkName);
    static QString WriteBackFailedText();

    explicit MavlinkMirrorService(RawWriter rawWriter,
                                  OutputFactory outputFactory = ProductionOutputFactory(),
                                  QObject *parent = nullptr);
    ~MavlinkMirrorService() override;

    void setUdpPortGuard(UdpPortGuard guard);

    // Starts (or restarts) the single session. Returns false and leaves nothing
    // open when the selection is empty, the guard refuses, or the output cannot
    // be created/opened; *error and status().text carry the reason.
    bool start(int linkId, const QString &linkName, const MavlinkMirrorSettings &settings,
               QString *error = nullptr);
    void stop();
    void setAllowWriteBack(bool enabled);

    bool isRunning() const { return m_output != nullptr; }
    State state() const { return m_state; }
    Status status() const;
    int linkId() const { return m_linkId; }

public slots:
    // Complete framed bytes of one MAVLink packet received on linkId.
    void observeFrame(int linkId, const QByteArray &frame);
    // The physical link is gone: a session pinned to it stops.
    void forgetLink(int linkId);
    // Application shutdown: stop and release everything.
    void clear();

signals:
    void statusChanged();

private:
    void drain();
    void refreshState();
    void setStopped(const QString &text);
    void fail(const QString &text);
    void releaseOutput();
    void resetCounters();
    void handlePeerBytes(const QByteArray &bytes);

    RawWriter m_rawWriter;
    OutputFactory m_outputFactory;
    UdpPortGuard m_udpPortGuard;
    std::unique_ptr<MavlinkMirrorOutput> m_output;
    State m_state = State::Stopped;
    QString m_text;
    MavlinkMirrorSettings m_settings;
    int m_linkId = -1;
    QString m_linkName;
    bool m_allowWriteBack = false;
    bool m_writeBackFailed = false;
    quint64 m_txBytes = 0;
    quint64 m_rxBytes = 0;
    quint64 m_droppedBytes = 0;
    quint64 m_droppedFrames = 0;
    QList<QByteArray> m_queue;
    qint64 m_pendingBytes = 0;
    bool m_draining = false;
    bool m_drainRequested = false;
};

#endif // MAVLINKMIRRORSERVICE_H
