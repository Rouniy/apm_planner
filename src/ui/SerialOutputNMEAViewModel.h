#ifndef SERIALOUTPUTNMEAVIEWMODEL_H
#define SERIALOUTPUTNMEAVIEWMODEL_H

#include "comm/NmeaOutputService.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

struct NmeaOutputSource
{
    VehicleEndpoint endpoint;
    QString linkName;

    bool isValid() const { return endpoint.isValid(); }
};

/** UI state for Mission Planner 10's SerialOutputNMEAView. */
class SerialOutputNMEAViewModel final : public QObject
{
    Q_OBJECT

public:
    using PortEnumerator = std::function<QStringList()>;
    using SourceResolver = std::function<NmeaOutputSource()>;
    using SelectionValidator = std::function<QString(const QString &selection)>;

    struct Dependencies
    {
        PortEnumerator enumeratePorts;
        SourceResolver resolveSource;
        SelectionValidator validateSelection;
    };

    explicit SerialOutputNMEAViewModel(
        NmeaOutputService *service, Dependencies dependencies,
        QObject *parent = nullptr);
    ~SerialOutputNMEAViewModel() override;

    QStringList ports() const { return m_ports; }
    QList<int> bauds() const { return m_bauds; }
    QList<double> rates() const { return m_rates; }
    QString selectedPort() const { return m_selectedPort; }
    int selectedBaud() const { return m_selectedBaud; }
    double selectedRateHz() const { return m_selectedRateHz; }
    QString connectButtonText() const;
    QString statusText() const { return m_statusText; }
    QString lastSentence() const { return m_lastSentence; }
    bool isRunning() const;

public slots:
    void refreshPorts();
    void setSelectedPort(const QString &selection);
    void setSelectedBaud(int baud);
    void setSelectedRateHz(double rateHz);
    void toggleConnection();
    void stop();
    void refreshStatus();

signals:
    void changed();

private:
    void setLocalStatus(const QString &text);

    QPointer<NmeaOutputService> m_service;
    Dependencies m_dependencies;
    QStringList m_ports;
    const QList<int> m_bauds = {
        4800, 9600, 19200, 38400, 57600, 115200
    };
    const QList<double> m_rates = {1.0, 2.0, 5.0, 10.0};
    QString m_selectedPort;
    int m_selectedBaud = 4800;
    double m_selectedRateHz = 5.0;
    QString m_localStatus;
    QString m_statusText;
    QString m_lastSentence;
};

#endif // SERIALOUTPUTNMEAVIEWMODEL_H
