#ifndef CONNECTIONOPTIONSVIEWMODEL_H
#define CONNECTIONOPTIONSVIEWMODEL_H

#include <QObject>
#include <QList>
#include <QString>

class QSettings;

class ConnectionOptionsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<int> Bauds READ Bauds CONSTANT)
    Q_PROPERTY(int SelectedBaud READ SelectedBaud WRITE setSelectedBaud
               NOTIFY SelectedBaudChanged)
    Q_PROPERTY(int GcsSysid READ GcsSysid WRITE setGcsSysid
               NOTIFY GcsSysidChanged)
    Q_PROPERTY(bool SendGcsHeartbeat READ SendGcsHeartbeat
               WRITE setSendGcsHeartbeat NOTIFY SendGcsHeartbeatChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY StatusChanged)

public:
    explicit ConnectionOptionsViewModel(QObject *parent = nullptr);
    explicit ConnectionOptionsViewModel(QSettings *settings,
                                        QObject *parent = nullptr);

    static QList<int> availableBaudRates();

    QList<int> Bauds() const;
    int SelectedBaud() const;
    int GcsSysid() const;
    bool SendGcsHeartbeat() const;
    QString Status() const;

public slots:
    void setSelectedBaud(int baud);
    void setGcsSysid(int systemId);
    void setSendGcsHeartbeat(bool enabled);
    bool Apply();

signals:
    void SelectedBaudChanged(int baud);
    void GcsSysidChanged(int systemId);
    void SendGcsHeartbeatChanged(bool enabled);
    void StatusChanged(const QString &status);
    void settingsApplied(int baud, bool sendHeartbeat, int gcsSystemId);

private:
    void load();
    void setStatus(const QString &status);

    QSettings *m_settings = nullptr;
    int m_selectedBaud = 115200;
    int m_gcsSysid = 255;
    bool m_sendGcsHeartbeat = true;
    QString m_status;
};

#endif
