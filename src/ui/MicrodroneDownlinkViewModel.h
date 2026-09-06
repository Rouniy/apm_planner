#ifndef MICRODRONEDOWNLINKVIEWMODEL_H
#define MICRODRONEDOWNLINKVIEWMODEL_H

#include "comm/MicrodroneDownlinkService.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

/** UI state for Mission Planner 10's MicrodroneDownlinkView. */
class MicrodroneDownlinkViewModel final : public QObject
{
    Q_OBJECT

public:
    using PortEnumerator = std::function<QStringList()>;

    explicit MicrodroneDownlinkViewModel(
        MicrodroneDownlinkService *service,
        PortEnumerator enumeratePorts = {}, QObject *parent = nullptr);
    ~MicrodroneDownlinkViewModel() override;

    QStringList ports() const { return m_ports; }
    QList<int> bauds() const { return m_bauds; }
    QString selectedPort() const { return m_selectedPort; }
    int selectedBaud() const { return m_selectedBaud; }
    bool busy() const;
    bool isRunning() const;
    bool canEditSettings() const { return !busy() && !isRunning(); }
    QString connectButtonText() const;
    QString statusText() const { return m_statusText; }
    QString sourceDescription() const { return m_sourceDescription; }
    QString lastLine() const { return m_lastLine; }

public slots:
    void refreshPorts();
    void setSelectedPort(const QString &port);
    void setSelectedBaud(int baud);
    void toggleConnection();
    void stop();
    void refreshStatus();

signals:
    void changed();

private:
    void setLocalStatus(const QString &text);

    QPointer<MicrodroneDownlinkService> m_service;
    PortEnumerator m_enumeratePorts;
    QStringList m_ports;
    QList<int> m_bauds;
    QString m_selectedPort;
    int m_selectedBaud = 57600;
    QString m_localStatus;
    QString m_statusText = QStringLiteral("Stopped.");
    QString m_sourceDescription =
        QStringLiteral("No connected telemetry source.");
    QString m_lastLine;
    quint64 m_revision = 0;
};

#endif // MICRODRONEDOWNLINKVIEWMODEL_H
