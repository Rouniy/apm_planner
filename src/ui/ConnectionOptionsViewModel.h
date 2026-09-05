#ifndef CONNECTIONOPTIONSVIEWMODEL_H
#define CONNECTIONOPTIONSVIEWMODEL_H

#include <QList>
#include <QObject>
#include <QString>

class ConnectionOptionsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<int> Bauds READ Bauds CONSTANT)
    Q_PROPERTY(int SelectedBaud READ SelectedBaud WRITE setSelectedBaud
               NOTIFY SelectedBaudChanged)
    Q_PROPERTY(bool SendGcsHeartbeat READ SendGcsHeartbeat
               WRITE setSendGcsHeartbeat NOTIFY SendGcsHeartbeatChanged)
    Q_PROPERTY(int GcsSysid READ GcsSysid WRITE setGcsSysid
               NOTIFY GcsSysidChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY StatusChanged)

public:
    explicit ConnectionOptionsViewModel(QObject *parent = nullptr);

    static QList<int> availableBaudRates();

    QList<int> Bauds() const;
    int SelectedBaud() const;
    bool SendGcsHeartbeat() const;
    int GcsSysid() const;
    QString Status() const;

public slots:
    void setSelectedBaud(int baud);
    void setSendGcsHeartbeat(bool enabled);
    void setGcsSysid(int systemId);
    void Apply();

signals:
    void SelectedBaudChanged(int baud);
    void SendGcsHeartbeatChanged(bool enabled);
    void GcsSysidChanged(int systemId);
    void StatusChanged(const QString &status);
    void settingsApplied(int baud, bool sendGcsHeartbeat, int gcsSysid);

private:
    int m_selectedBaud = 115200;
    bool m_sendGcsHeartbeat = true;
    int m_gcsSysid = 255;
    QString m_status;
};

#endif
