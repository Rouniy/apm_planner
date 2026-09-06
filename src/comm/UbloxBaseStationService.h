#ifndef UBLOXBASESTATIONSERVICE_H
#define UBLOXBASESTATIONSERVICE_H

#include "GpsCorrectionSource.h"
#include "UbloxBaseStationProtocol.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QTimer;

/**
 * Executes the bounded Mission Planner u-blox base setup sequence on the
 * serial receiver already owned by GpsCorrectionSource.  It never opens a
 * second port and never follows a disconnected/replaced receiver session.
 *
 * Mission Planner's reference sequence does not wait for UBX ACKs.  A
 * Submitted result therefore means every command was accepted by the local
 * serial write queue, not that the receiver applied it.  ACK/NAK and NAV
 * packets are decoded and exposed as diagnostic state.
 */
class UbloxBaseStationService final : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Receiver, SurveyIn, Fixed };
    Q_ENUM(Mode)

    enum class State {
        Unavailable,
        Ready,
        Configuring,
        Configured,
        SurveyIn,
        Fixed
    };
    Q_ENUM(State)

    enum class Outcome {
        Submitted,
        Cancelled,
        Rejected,
        SourceLost
    };
    Q_ENUM(Outcome)

    struct FixedPosition
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double altitudeMeters = 0.0;
        double accuracyMeters = 0.001;

        bool isValid() const noexcept;
    };

    struct Report
    {
        quint64 operationId = 0;
        quint64 receiverSession = 0;
        Mode mode = Mode::SurveyIn;
        Outcome outcome = Outcome::Rejected;
        int totalCommands = 0;
        int submittedCommands = 0;
        int acceptedAcknowledgements = 0;
        int rejectedAcknowledgements = 0;
        QString description;

        bool isValid() const noexcept
        {
            return operationId != 0 && receiverSession != 0;
        }
    };

    static constexpr int MaximumCommands = 128;
    static constexpr qint64 MaximumSequenceBytes = 1024 * 1024;
    static constexpr quint32 MaximumSurveyDurationSeconds = 7 * 24 * 60 * 60;
    static constexpr double MinimumSurveyAccuracyMeters = 0.001;
    static constexpr double MaximumSurveyAccuracyMeters = 100.0;

    explicit UbloxBaseStationService(GpsCorrectionSource *source,
                                     QObject *parent = nullptr);
    ~UbloxBaseStationService() override;

    GpsCorrectionSource *source() const noexcept { return m_source.data(); }
    bool available() const noexcept;
    bool busy() const noexcept { return m_busy || m_finishing; }
    State state() const noexcept;
    QString status() const { return m_status; }
    quint64 currentOperationId() const noexcept { return m_operationId; }
    quint64 configuredReceiverSession() const noexcept
    {
        return m_configuredSession;
    }
    Report lastReport() const { return m_lastReport; }
    UbloxBaseStationProtocol::SurveyIn surveyStatus() const
    {
        return m_surveyStatus;
    }
    UbloxBaseStationProtocol::Position currentPosition() const
    {
        return m_position;
    }
    QString acknowledgementStatus() const { return m_acknowledgementStatus; }

    /** Runs only the Mission Planner SetupM8P/F9P baud/configuration sweep. */
    bool configureReceiver(bool m8p130Plus,
                           quint64 *operationIdOut = nullptr,
                           QString *error = nullptr);
    /** Restarts survey-in: disable/reset, SetupM8P/F9P, then TMODE3 survey. */
    bool configureSurveyIn(quint32 durationSeconds,
                           double accuracyMeters,
                           bool m8p130Plus,
                           quint64 *operationIdOut = nullptr,
                           QString *error = nullptr);
    /** Initial saved-base setup: SetupM8P/F9P, disable/reset, then fixed TMODE3. */
    bool configureFixed(const FixedPosition &position,
                        bool m8p130Plus,
                        quint64 *operationIdOut = nullptr,
                        QString *error = nullptr);
    /** Applies fixed TMODE3 and polls it without repeating baud discovery. */
    bool applyFixed(const FixedPosition &position,
                    quint64 *operationIdOut = nullptr,
                    QString *error = nullptr);

    /** Cancels only the matching in-flight local configuration sequence. */
    bool cancel(quint64 operationId, QString *error = nullptr);

    /** Stops monitoring/configuring without disconnecting the source. */
    void stop();
    void shutdown();

signals:
    void stateChanged();
    void operationFinished(UbloxBaseStationService::Report report);

private:
    enum class SequenceStep { Idle, BeforeWrite, AfterWrite };

    bool begin(Mode mode,
               QVector<UbloxBaseStationProtocol::Command> commands,
               quint64 *operationIdOut,
               QString *error);
    void scheduleCurrentCommand();
    void writeCurrentCommand();
    void handleSequenceTimer();
    void handleReceiverBytes(const QByteArray &bytes, quint64 session);
    void handleSourceStateChanged();
    void finish(Outcome outcome, const QString &description);
    void resetObservedState();
    bool operationIsCurrent(quint64 operationId,
                            quint64 receiverSession) const noexcept;

    QPointer<GpsCorrectionSource> m_source;
    QTimer *m_sequenceTimer = nullptr;
    UbloxBaseStationProtocol m_protocol;
    QVector<UbloxBaseStationProtocol::Command> m_commands;
    int m_commandIndex = 0;
    int m_submittedCommands = 0;
    int m_acceptedAcknowledgements = 0;
    int m_rejectedAcknowledgements = 0;
    quint64 m_nextOperationId = 1;
    quint64 m_operationId = 0;
    quint64 m_operationSession = 0;
    quint64 m_configuredSession = 0;
    Mode m_mode = Mode::SurveyIn;
    SequenceStep m_sequenceStep = SequenceStep::Idle;
    bool m_busy = false;
    bool m_writeInFlight = false;
    bool m_cancelRequested = false;
    bool m_sourceLostRequested = false;
    bool m_shuttingDown = false;
    bool m_finishing = false;
    QString m_status;
    QString m_acknowledgementStatus;
    UbloxBaseStationProtocol::SurveyIn m_surveyStatus;
    UbloxBaseStationProtocol::Position m_position;
    Report m_lastReport;
};

Q_DECLARE_METATYPE(UbloxBaseStationService::Mode)
Q_DECLARE_METATYPE(UbloxBaseStationService::State)
Q_DECLARE_METATYPE(UbloxBaseStationService::Outcome)
Q_DECLARE_METATYPE(UbloxBaseStationService::FixedPosition)
Q_DECLARE_METATYPE(UbloxBaseStationService::Report)

#endif // UBLOXBASESTATIONSERVICE_H
