#ifndef MICRODRONEDOWNLINKSERVICE_H
#define MICRODRONEDOWNLINKSERVICE_H

#include "MavlinkComponentInstanceLease.h"
#include "MavlinkMirrorOutput.h"
#include "MicrodroneTelemetryState.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

struct MicrodroneSource
{
    VehicleTargetLease selection;
    MavlinkComponentInstanceLease instance;
    QString linkName;
    bool isValid() const {
        return selection.isValid() && instance.isValid()
            && selection.endpoint == instance.endpoint;
    }
    bool sameSource(const MicrodroneSource &other) const {
        return selection.generation == other.selection.generation
            && selection.endpoint == other.selection.endpoint
            && instance == other.instance;
    }
};

struct MicrodroneOutputSettings
{
    QString port;
    int baud = 57600;
};

class MicrodroneDownlinkService final : public QObject
{
    Q_OBJECT
public:
    enum class State { Stopped, Opening, Emitting, Failed };
    Q_ENUM(State)
    using OutputFactory = std::function<std::unique_ptr<MavlinkMirrorOutput>(
        const MicrodroneOutputSettings &, QString *)>;
    using SourceResolver = std::function<MicrodroneSource()>;
    using PortValidator = std::function<QString(const QString &)>;
    using Clock = std::function<QDateTime()>;
    struct Dependencies {
        OutputFactory outputFactory;
        SourceResolver resolveSource;
        PortValidator validatePort;
        Clock clock;
    };
    explicit MicrodroneDownlinkService(Dependencies dependencies, QObject *parent = nullptr);
    ~MicrodroneDownlinkService() override;
    static OutputFactory ProductionOutputFactory();
    static QList<int> Bauds();
    static QString TargetChangedText();
    bool start(const MicrodroneOutputSettings &settings, QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_state == State::Emitting; }
    bool busy() const { return m_state == State::Opening; }
    State state() const { return m_state; }
    QString statusText() const { return m_text; }
    QString sourceDescription() const;
    QString lastLine() const { return m_lastLine; }
    quint64 framesSubmitted() const { return m_frames; }
    quint64 droppedFrames() const { return m_dropped; }
    MicrodroneSource source() const { return m_source; }
    MicrodroneTelemetry telemetry() const { return m_telemetry.snapshot(); }
public slots:
    void synchronizeSource();
    void observeMessage(int linkId, quint64 epoch, const mavlink_message_t &message);
    void forgetLink(int linkId, quint64 epoch);
    void emitNow();
signals:
    void changed();
private:
    bool resolveCurrent(MicrodroneSource *source);
    bool sourceStillCurrent(quint64 revision);
    void finish(State state, const QString &text);
    void drain();
    Dependencies m_dependencies;
    QTimer m_timer;
    State m_state = State::Stopped;
    QString m_text = QStringLiteral("Stopped."), m_lastLine;
    MicrodroneSource m_source;
    MicrodroneTelemetryState m_telemetry;
    std::shared_ptr<MavlinkMirrorOutput> m_output;
    QByteArray m_pending;
    qint64 m_offset = 0, m_counter = 0;
    quint64 m_revision = 0, m_frames = 0, m_dropped = 0;
    bool m_draining = false, m_destroying = false;
};
#endif
