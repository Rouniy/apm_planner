#ifndef SERIALPASSTHROUGHVIEWMODEL_H
#define SERIALPASSTHROUGHVIEWMODEL_H

#include "comm/MavlinkMirrorService.h"

#include <QObject>
#include <QStringList>

#include <functional>

struct MavlinkMirrorSource
{
    int linkId = -1;
    QString linkName;

    bool isValid() const { return linkId >= 0; }
};

/** UI state for Mission Planner 10's SerialPassThroughView. */
class SerialPassThroughViewModel final : public QObject
{
    Q_OBJECT

public:
    using PortEnumerator = std::function<QStringList()>;
    using SourceResolver = std::function<MavlinkMirrorSource()>;
    // Empty means accepted; non-empty is shown instead of opening an output.
    using SelectionValidator = std::function<QString(const QString &selection)>;

    struct Dependencies
    {
        PortEnumerator enumeratePorts;
        SourceResolver resolveSource;
        SelectionValidator validateSelection;
    };

    explicit SerialPassThroughViewModel(
        MavlinkMirrorService *service, Dependencies dependencies,
        QObject *parent = nullptr);
    ~SerialPassThroughViewModel() override;

    QStringList ports() const { return m_ports; }
    QList<int> bauds() const { return m_bauds; }
    QString selectedPort() const { return m_selectedPort; }
    int selectedBaud() const { return m_selectedBaud; }
    bool allowWriteBack() const { return m_allowWriteBack; }
    QString connectButtonText() const;
    QString statusText() const { return m_statusText; }
    quint64 txBytes() const { return m_txBytes; }
    quint64 rxBytes() const { return m_rxBytes; }
    quint64 droppedBytes() const { return m_droppedBytes; }
    bool isRunning() const;

public slots:
    void refreshPorts();
    void setSelectedPort(const QString &selection);
    void setSelectedBaud(int baud);
    void setAllowWriteBack(bool enabled);
    void toggleConnection();
    void stop();
    void refreshStatus();

signals:
    void changed();

private:
    void setLocalStatus(const QString &text);

    MavlinkMirrorService *m_service = nullptr;
    Dependencies m_dependencies;
    QStringList m_ports;
    const QList<int> m_bauds = {
        1200, 2400, 4800, 9600, 19200, 38400,
        57600, 115200, 230400, 921600
    };
    QString m_selectedPort;
    int m_selectedBaud = 115200;
    bool m_allowWriteBack = false;
    QString m_localStatus;
    QString m_statusText;
    quint64 m_txBytes = 0;
    quint64 m_rxBytes = 0;
    quint64 m_droppedBytes = 0;
};

#endif // SERIALPASSTHROUGHVIEWMODEL_H
