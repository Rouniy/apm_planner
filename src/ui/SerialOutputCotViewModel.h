#ifndef SERIALOUTPUTCOTVIEWMODEL_H
#define SERIALOUTPUTCOTVIEWMODEL_H

#include "CotIdentityModel.h"
#include "comm/CotEventSerializer.h"
#include "comm/CotOutputTransport.h"
#include "comm/VehicleEndpoint.h"

#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class CotOutputService;

struct CotOutputSource
{
    int linkId = -1;
    QString linkName;
    QList<VehicleEndpoint> endpoints;

    bool isValid() const { return linkId >= 0; }
};

/** UI state for Mission Planner 10's SerialOutputCotView. */
class SerialOutputCotViewModel final : public QObject
{
    Q_OBJECT

public:
    using PortEnumerator = std::function<QStringList()>;
    using SourceResolver = std::function<CotOutputSource()>;
    using SelectionValidator = std::function<QString(
        const CotOutputTransport::Settings &settings)>;
    using SettingsLoader = std::function<QVariantMap()>;
    using SettingsSaver = std::function<bool(
        const QVariantMap &values, QString *error)>;

    struct Dependencies
    {
        PortEnumerator enumeratePorts;
        SourceResolver resolveSource;
        SelectionValidator validateSelection;
        SettingsLoader loadSettings;
        SettingsSaver saveSettings;
    };

    explicit SerialOutputCotViewModel(
        CotOutputService *service, Dependencies dependencies,
        QObject *parent = nullptr);
    ~SerialOutputCotViewModel() override;

    static QString TakMulticastText();
    static QString UdpClientText();
    static QString UdpHostText();
    static QString TcpClientText();
    static QString TcpHostText();

    QStringList endpoints() const { return m_endpoints; }
    QList<int> bauds() const { return m_bauds; }
    QString selectedEndpoint() const { return m_selectedEndpoint; }
    QString host() const { return m_host; }
    int port() const { return m_port; }
    int baud() const { return m_baud; }
    double updateSeconds() const { return m_updateSeconds; }
    QString eventType() const { return m_eventType; }
    QString uidPrefix() const { return m_uidPrefix; }
    QString callsign() const { return m_callsign; }
    bool indentXml() const { return m_indentXml; }
    bool advancedMode() const { return m_advancedMode; }
    bool isNetworkEndpoint() const;
    bool isSerialEndpoint() const;
    bool isRunning() const;
    QString connectButtonText() const;
    QString statusText() const { return m_statusText; }
    QString lastEvent() const { return m_lastEvent; }
    CotIdentityModel *identityModel() const { return m_identityModel; }

    QVariantMap settingsSnapshot() const;
    void updateAvailableEndpoints(const QList<VehicleEndpoint> &endpoints);

public slots:
    void refreshEndpoints();
    void setSelectedEndpoint(const QString &endpoint);
    void setHost(const QString &host);
    void setPort(int port);
    void setBaud(int baud);
    void setUpdateSeconds(double seconds);
    void setEventType(const QString &eventType);
    void setUidPrefix(const QString &prefix);
    void setCallsign(const QString &callsign);
    void setIndentXml(bool enabled);
    void setAdvancedMode(bool enabled);
    void refreshIdentitySystems();
    void addIdentity();
    void removeIdentity(int row);
    void toggleConnection();
    void stop();
    void saveSettings();
    void refreshStatus();

signals:
    void changed();

private:
    static bool isFixedEndpoint(const QString &endpoint);
    static CotOutputTransport::Mode modeForEndpoint(const QString &endpoint);
    CotOutputTransport::Settings transportSettings() const;
    QHash<int, CotIdentityOverride> identityOverrides() const;
    void applyEndpointDefaults(const QString &endpoint);
    void applyLiveSettings();
    void loadSettings();
    void markIdentitiesDirty();
    void setLocalStatus(const QString &text);

    QPointer<CotOutputService> m_service;
    Dependencies m_dependencies;
    CotIdentityModel *m_identityModel = nullptr;
    QList<VehicleEndpoint> m_availableEndpoints;
    QStringList m_endpoints;
    const QList<int> m_bauds = {4800, 9600, 19200, 38400, 57600, 115200};
    QString m_selectedEndpoint;
    QString m_host = QStringLiteral("239.2.3.1");
    int m_port = 6969;
    int m_baud = 57600;
    double m_updateSeconds = 10.0;
    QString m_eventType = QStringLiteral("a-f-A-M-F-Q");
    QString m_uidPrefix = QStringLiteral("MissionPlanner");
    QString m_callsign;
    bool m_indentXml = false;
    bool m_advancedMode = true;
    bool m_identitiesDirty = false;
    QString m_localStatus;
    QString m_statusText = QStringLiteral("Stopped.");
    QString m_lastEvent;
};

#endif // SERIALOUTPUTCOTVIEWMODEL_H
