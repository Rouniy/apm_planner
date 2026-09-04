#ifndef FOLLOWMEWINDOW_H
#define FOLLOWMEWINDOW_H

#include "comm/FollowMeGpsInput.h"
#include "comm/GuidedTargetService.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <functional>

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QTimer;
class VehicleTargetManager;

/** Mission Planner 10 TOOLS > Follow Me modeless window. */
class FollowMeWindow final : public QWidget
{
    Q_OBJECT

public:
    using GpsInputFactory =
        std::function<FollowMeGpsInput *(QObject *parent)>;
    using PortEnumerator = std::function<QStringList()>;
    using SerialValidator = std::function<QString(const QString &portName)>;
    using DangerousConfirmation = std::function<bool(
        QWidget *parent, const QString &title, const QString &message)>;
    using MonotonicClock = std::function<qint64()>;

    struct Dependencies
    {
        VehicleTargetManager *targetManager = nullptr;
        GuidedTargetService *guidedService = nullptr;
        GpsInputFactory gpsInputFactory;
        PortEnumerator enumeratePorts;
        SerialValidator validateSerial;
        DangerousConfirmation confirmStart;
        MonotonicClock monotonicMs;
    };

    static constexpr int WindowWidth = 480;
    static constexpr int WindowHeight = 420;
    static constexpr int DefaultBaud = 4800;
    static constexpr double DefaultRateHz = 0.5;
    static constexpr double DefaultRelativeAltitudeM = 100.0;

    explicit FollowMeWindow(QWidget *owner = nullptr);
    FollowMeWindow(Dependencies dependencies, QWidget *owner = nullptr);
    ~FollowMeWindow() override;

    /** Creates a fresh independent modeless window on every call. */
    static FollowMeWindow *OpenWindow(QWidget *owner = nullptr);

    bool isRunning() const noexcept { return m_session.isValid(); }
    bool isStarting() const noexcept { return m_starting; }
    QString statusText() const;
    QString locationText() const;

    static int updateIntervalMs(double rateHz);
    static qint64 maximumFixAgeMs(double rateHz);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectDependencies();
    void syncUi();
    void refreshPorts();
    void refreshTargetDescription();
    void toggleSession();
    void beginStart();
    void startManualTarget();
    void startSerialInput();
    void handleGpsFix(quint64 generation, const NmeaGgaFix &fix);
    void handleNoPositionFix(quint64 generation);
    void handleGpsError(quint64 generation, const QString &error);
    void sendCurrentTarget();
    void requestStop(const QString &status);
    void finishLocalSession(const QString &status);
    void abortStart(const QString &status);
    void releaseGpsInput();
    void shutdown();
    bool sessionMatches(
        const GuidedTargetService::SessionToken &session) const;
    bool leaseIsCurrent(const VehicleTargetLease &lease) const;
    double selectedRateHz() const;
    int selectedBaud() const;
    QString targetDescription(const VehicleTargetLease &target) const;
    QString startFailureText(GuidedTargetService::RequestResult result) const;
    static bool validCoordinates(double latitude, double longitude);
    static QString targetText(const GuidedTargetService::Target &target,
                              const NmeaGgaFix *fix = nullptr);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<GuidedTargetService> m_guidedService;
    GpsInputFactory m_gpsInputFactory;
    PortEnumerator m_enumeratePorts;
    SerialValidator m_validateSerial;
    DangerousConfirmation m_confirmStart;
    MonotonicClock m_monotonicMs;
    QElapsedTimer m_defaultClock;
    QPointer<FollowMeGpsInput> m_gpsInput;
    quint64 m_operationGeneration = 0;
    bool m_starting = false;
    bool m_stopping = false;
    bool m_closing = false;
    bool m_serialSession = false;
    bool m_hasLatestFix = false;
    qint64 m_latestFixReceivedMs = 0;
    double m_activeRateHz = DefaultRateHz;
    double m_activeRelativeAltitudeM = DefaultRelativeAltitudeM;
    NmeaGgaFix m_latestFix;
    GuidedTargetService::Target m_manualTarget;
    VehicleTargetLease m_pendingLease;
    GuidedTargetService::SessionToken m_session;
    QTimer *m_updateTimer = nullptr;
    QRadioButton *m_manualSource = nullptr;
    QRadioButton *m_serialSource = nullptr;
    QDoubleSpinBox *m_manualLatitude = nullptr;
    QDoubleSpinBox *m_manualLongitude = nullptr;
    QDoubleSpinBox *m_relativeAltitude = nullptr;
    QComboBox *m_serialPort = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_rate = nullptr;
    QPushButton *m_useManual = nullptr;
    QPushButton *m_refreshPorts = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_target = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_location = nullptr;
};

#endif // FOLLOWMEWINDOW_H
