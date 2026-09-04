#ifndef MOVINGBASEWINDOW_H
#define MOVINGBASEWINDOW_H

#include "comm/MovingBaseInputTransport.h"
#include "comm/MovingBaseService.h"

#include <QPointer>
#include <QVariantMap>
#include <QWidget>

#include <functional>
#include <memory>

class MovingBaseNmeaLog;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class VehicleTargetManager;

/** Mission Planner 10 TOOLS > Moving Base modeless window. */
class MovingBaseWindow final : public QWidget
{
    Q_OBJECT

public:
    using TransportFactory = std::function<MovingBaseInputTransport *(
        const MovingBaseInputTransport::Settings &, QObject *parent)>;
    using PortEnumerator = std::function<QStringList()>;
    using SerialValidator = std::function<QString(const QString &portName)>;
    using UdpPortGuard = std::function<QString(quint16 port)>;
    using SettingReader = std::function<QVariant(
        const QString &key, const QVariant &fallback)>;
    using SettingWriter = std::function<bool(
        const QVariantMap &values, QString *error)>;

    struct Dependencies
    {
        VehicleTargetManager *targetManager = nullptr;
        MovingBaseService *service = nullptr;
        TransportFactory transportFactory;
        PortEnumerator enumeratePorts;
        SerialValidator validateSerial;
        UdpPortGuard validateUdpHostPort;
        SettingReader readSetting;
        SettingWriter writeSettings;
        QString rawLogPath;
    };

    static constexpr int WindowWidth = 580;
    static constexpr int WindowHeight = 500;
    static constexpr int MinimumWindowWidth = 540;
    static constexpr int MinimumWindowHeight = 460;

    explicit MovingBaseWindow(QWidget *owner = nullptr);
    MovingBaseWindow(Dependencies dependencies, QWidget *owner = nullptr);
    ~MovingBaseWindow() override;

    /** Creates a fresh independent modeless window on every call. */
    static MovingBaseWindow *OpenWindow(QWidget *owner = nullptr);

    bool isRunning() const noexcept { return m_session.isValid(); }
    QString statusText() const;
    QString locationText() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectDependencies();
    void loadSettings();
    void persistSettingsWhenReady();
    void refreshInputs();
    void syncInputFields();
    void syncUi();
    void refreshTargetDescription();
    void toggleSession();
    void beginStart();
    void connectTransport(MovingBaseInputTransport *transport,
                          quint64 operationGeneration);
    void stopSession(const QString &status);
    void stopAfterInputFailure(const QString &status);
    void finishFromService(const QString &status);
    void releaseTransport();
    void appendRawLine(const QByteArray &line);
    void shutdown();

    MovingBaseInputTransport::Mode selectedMode() const;
    QString selectedSerialPort() const;
    double selectedRateHz() const;
    QString selectedInputSetting() const;
    bool sessionMatches(const MovingBaseService::SessionToken &session) const;
    static bool sameLease(const VehicleTargetLease &left,
                          const VehicleTargetLease &right) noexcept;
    static QString locationText(
        const MovingBasePositionSnapshot &snapshot);
    static QString stableModeSetting(MovingBaseInputTransport::Mode mode);
    static bool modeFromSetting(const QString &value,
                                MovingBaseInputTransport::Mode *mode);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<MovingBaseService> m_service;
    TransportFactory m_transportFactory;
    PortEnumerator m_enumeratePorts;
    SerialValidator m_validateSerial;
    UdpPortGuard m_validateUdpHostPort;
    SettingReader m_readSetting;
    SettingWriter m_writeSettings;
    QString m_rawLogPath;
    QPointer<MovingBaseInputTransport> m_transport;
    std::unique_ptr<MovingBaseNmeaLog> m_log;
    MovingBaseService::SessionToken m_session;
    quint64 m_operationGeneration = 0;
    bool m_stopping = false;
    bool m_closing = false;
    bool m_settingsPersisted = false;
    QString m_loadedInput;

    QComboBox *m_input = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_baudLabel = nullptr;
    QComboBox *m_baud = nullptr;
    QLabel *m_hostLabel = nullptr;
    QLineEdit *m_host = nullptr;
    QLabel *m_portLabel = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_rate = nullptr;
    QCheckBox *m_relativeAltitude = nullptr;
    QCheckBox *m_updateRally = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_target = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_location = nullptr;
};

#endif // MOVINGBASEWINDOW_H
