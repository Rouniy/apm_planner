#ifndef CONNECTIONOPTIONSVIEWMODEL_H
#define CONNECTIONOPTIONSVIEWMODEL_H

#include <QList>
#include <QObject>
#include <QStringList>

class ConnectionOptionsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList Connections READ Connections CONSTANT)
    Q_PROPERTY(QList<int> Bauds READ Bauds CONSTANT)
    Q_PROPERTY(QString SelectedConnection READ SelectedConnection
               WRITE setSelectedConnection NOTIFY SelectedConnectionChanged)
    Q_PROPERTY(int SelectedBaud READ SelectedBaud WRITE setSelectedBaud
               NOTIFY SelectedBaudChanged)
    Q_PROPERTY(bool BaudEnabled READ BaudEnabled NOTIFY BaudEnabledChanged)

public:
    explicit ConnectionOptionsViewModel(QObject *parent = nullptr);
    explicit ConnectionOptionsViewModel(const QStringList &serialPorts,
                                        QObject *parent = nullptr);

    static QList<int> availableBaudRates();
    static QStringList availableConnections(const QStringList &serialPorts);
    static bool isNetworkConnection(const QString &connection);

    QStringList Connections() const;
    QList<int> Bauds() const;
    QString SelectedConnection() const;
    int SelectedBaud() const;
    bool BaudEnabled() const;

public slots:
    void setSelectedConnection(const QString &connection);
    void setSelectedBaud(int baud);
    bool Connect();

signals:
    void SelectedConnectionChanged(const QString &connection);
    void SelectedBaudChanged(int baud);
    void BaudEnabledChanged(bool enabled);
    void connectRequested(const QString &connection, int baud);

private:
    QStringList m_connections;
    QString m_selectedConnection;
    int m_selectedBaud = 115200;
};

#endif
