#ifndef CONFIGHWBTVIEWMODEL_H
#define CONFIGHWBTVIEWMODEL_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class ConfigHWBTViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Instructions READ Instructions CONSTANT)
    Q_PROPERTY(QStringList Ports READ Ports NOTIFY portsChanged)
    Q_PROPERTY(QStringList Bauds READ Bauds CONSTANT)
    Q_PROPERTY(QString SelectedPort READ SelectedPort
               WRITE setSelectedPort NOTIFY settingsChanged)
    Q_PROPERTY(QString Name READ Name WRITE setName NOTIFY settingsChanged)
    Q_PROPERTY(QString SelectedBaud READ SelectedBaud
               WRITE setSelectedBaud NOTIFY settingsChanged)
    Q_PROPERTY(QString Pin READ Pin WRITE setPin NOTIFY settingsChanged)
    Q_PROPERTY(QString Output READ Output NOTIFY outputChanged)
    Q_PROPERTY(bool IsBusy READ IsBusy NOTIFY stateChanged)
    Q_PROPERTY(bool IsCanceling READ IsCanceling NOTIFY stateChanged)
    Q_PROPERTY(bool IsConnected READ IsConnected NOTIFY stateChanged)

public:
    explicit ConfigHWBTViewModel(QObject *parent = nullptr);

    QString Title() const;
    QString Instructions() const;
    QStringList Ports() const { return m_ports; }
    QStringList Bauds() const;
    QString SelectedPort() const { return m_selectedPort; }
    QString Name() const { return m_name; }
    QString SelectedBaud() const { return m_selectedBaud; }
    QString Pin() const { return m_pin; }
    QString Output() const { return m_output; }
    bool IsBusy() const { return m_isBusy; }
    bool IsCanceling() const { return m_isCanceling; }
    bool IsConnected() const { return m_mainLinkConnected; }
    quint64 Generation() const { return m_generation; }

    static QHash<int, int> BaudMap();
    static QList<int> ProbeBauds();
    static QList<QByteArray> BuildCommands(
        const QString &name, const QString &selectedBaud,
        const QString &pin);

    void setPorts(const QStringList &ports,
                  const QString &preferredPort = QString());
    void setSelectedPort(const QString &port);
    void setName(const QString &name);
    void setSelectedBaud(const QString &baud);
    void setPin(const QString &pin);
    void setMainLinkConnected(bool connected);

    bool Write();
    void Cancel();

public slots:
    void operationProgress(quint64 generation, const QString &line);
    void operationFinished(quint64 generation, bool success,
                           bool canceled);

signals:
    void portsChanged();
    void settingsChanged();
    void outputChanged();
    void stateChanged();
    void programRequested(quint64 generation, const QString &portName,
                          const QString &name,
                          const QString &selectedBaud,
                          const QString &pin);
    void cancelRequested(quint64 generation);

private:
    void append(const QString &line);
    static bool isPrintableAscii(const QString &value, int maximumLength);

    QStringList m_ports;
    QString m_selectedPort;
    QString m_name;
    QString m_selectedBaud = QStringLiteral("57600");
    QString m_pin = QStringLiteral("1234");
    QString m_output;
    quint64 m_generation = 0;
    bool m_isBusy = false;
    bool m_isCanceling = false;
    bool m_mainLinkConnected = false;
};

#endif
