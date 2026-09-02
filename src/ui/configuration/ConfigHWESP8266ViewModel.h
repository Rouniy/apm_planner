#ifndef CONFIGHWESP8266VIEWMODEL_H
#define CONFIGHWESP8266VIEWMODEL_H

#include "comm/Esp8266ParameterClient.h"
#include "ui/configuration/Esp8266Settings.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

/*
 * Mission Planner 10 SETUP > ESP8266 Setup page state
 * (ViewModels/GCSViews/ConfigurationView/ConfigHWESP8266ViewModel.cs).
 *
 * Owns the editable fields, the MP10 status texts and the load/save/reset
 * workflow on top of a caller-owned Esp8266ParameterClient (exact link,
 * primary-target lease, component 240) and the pure Esp8266SettingsCodec.
 * Loaded values are applied only when they belong to the lease generation
 * the load was started for and the page is still connected.
 */
class ConfigHWESP8266ViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigHWESP8266ViewModel(QObject *parent = nullptr);
    ~ConfigHWESP8266ViewModel() override;

    // --- MP10 texts ---
    static QString Title();                 // "ESP8266"
    static QString NotConnectedStatus();    // "Not connected."
    static QString RequestingStatus();      // "Requesting ESP8266 parameters…"
    static QString TargetChangedStatus();   // "The selected device changed before ESP8266 parameters were loaded."
    static QString SavingStatus();          // "Saving…"
    static QString ProgrammedOkStatus();    // "Programmed OK."
    static QString ErrorSettingStatus();    // "Error setting parameter."
    static QString ResettingStatus();       // "Resetting to defaults…"
    static QString ProgrammedRefreshStatus(); // "Programmed OK. Refreshing parameters…"

    Esp8266ParameterClient *client() const { return m_client.data(); }
    void setClient(Esp8266ParameterClient *client);

    // --- state ---
    QString ssid() const { return m_ssid; }
    QString password() const { return m_password; }
    QString baud() const { return m_baud; }
    QString channel() const { return m_channel; }
    bool staMode() const { return m_staMode; }
    QString ipSta() const { return m_ipSta; }
    QString gatewaySta() const { return m_gatewaySta; }
    QString subnetSta() const { return m_subnetSta; }
    QString details() const { return m_details; }
    QString status() const { return m_status; }
    bool isLoaded() const { return m_loaded; }
    bool isConnected() const { return m_connected; }
    bool isActive() const { return m_active; }
    // True while the client runs an operation this page started.
    bool isBusy() const;
    // Connected and bound to a current primary lease.
    bool isTransportAvailable() const;

    QStringList channelOptions() const { return Esp8266SettingsCodec::ChannelOptions(); }
    QStringList baudOptions() const { return Esp8266SettingsCodec::BaudOptions(); }

    // The page inputs as MP10 Save() sees them.
    Esp8266SaveRequest saveRequest() const;

public slots:
    void setSsid(const QString &value);
    void setPassword(const QString &value);
    void setBaud(const QString &value);
    void setChannel(const QString &value);
    void setStaMode(bool value);
    void setIpSta(const QString &value);
    void setGatewaySta(const QString &value);
    void setSubnetSta(const QString &value);

    void setConnected(bool connected);

    // MP10 Activate: "Not connected." or a fresh parameter load (a running
    // load is replaced; a running save/reset is left alone).
    void activate();
    // MP10 Deactivate: cancels the running load only.
    void deactivate();
    // The selected vehicle changed: cancel everything this page started.
    void parameterTargetChanged();

    // MP10 Save(): validation, 22 writes, storage, reboot.
    bool save();
    // MP10 ResetDefaults(): erase, save, then refresh on success.
    bool resetDefaults();
    // Cancels whatever operation this page started.
    void cancelOperation();

signals:
    void ssidChanged(const QString &value);
    void passwordChanged(const QString &value);
    void baudChanged(const QString &value);
    void channelChanged(const QString &value);
    void staModeChanged(bool value);
    void ipStaChanged(const QString &value);
    void gatewayStaChanged(const QString &value);
    void subnetStaChanged(const QString &value);
    void detailsChanged(const QString &value);
    void statusChanged(const QString &status);
    void loadedChanged(bool loaded);
    void connectedChanged(bool connected);
    void busyChanged(bool busy);

private:
    enum class Owned
    {
        None,
        Load,
        Save,
        Reset
    };

    bool startLoad();
    void setStatus(const QString &status);
    void setLoaded(bool loaded);
    void setDetails(const QString &details);
    void applySettings(const Esp8266Settings &settings);
    bool ownsGeneration(Owned owned, qulonglong generation) const;
    void finishOwned();
    QString failureStatus(Esp8266ParameterClient::Operation operation,
                          const QString &reason, bool cancelled) const;

    void onParametersLoaded(qulonglong generation, const Esp8266RawParameters &values);
    void onLoadFailed(qulonglong generation, const QString &reason,
                      const QStringList &missing);
    void onSaveCompleted(qulonglong generation);
    void onResetCompleted(qulonglong generation);
    void onOperationFailed(Esp8266ParameterClient::Operation operation,
                           qulonglong generation, const QString &reason, bool cancelled);
    void onLeaseInvalidated(qulonglong generation);

    QPointer<Esp8266ParameterClient> m_client;
    QString m_ssid;
    QString m_password;
    QString m_baud;
    QString m_channel;
    bool m_staMode = false;
    QString m_ipSta;
    QString m_gatewaySta;
    QString m_subnetSta;
    QString m_details;
    QString m_status;
    bool m_loaded = false;
    bool m_connected = false;
    bool m_active = false;
    Owned m_owned = Owned::None;          // operation this page started
    qulonglong m_ownedGeneration = 0;     // lease generation it was started for
    bool m_cancelling = false; // set around our own cancel() so the status stays honest
};

#endif
