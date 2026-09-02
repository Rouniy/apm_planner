#ifndef CONFIGHWBTSERIALSERVICE_H
#define CONFIGHWBTSERIALSERVICE_H

#include <QObject>
#include <QStringList>

#include <memory>

class QThread;
struct ConfigHWBTSerialOperationGate;

class ConfigHWBTSerialService final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigHWBTSerialService(QObject *parent = nullptr);
    ~ConfigHWBTSerialService() override;

    static QStringList availablePorts();

public slots:
    void program(quint64 generation, const QString &portName,
                 const QString &name, const QString &selectedBaud,
                 const QString &pin);
    void cancel(quint64 generation = 0);

signals:
    void progress(quint64 generation, const QString &line);
    void finished(quint64 generation, bool success, bool canceled);

private:
    static bool portIsInUse(const QString &portName);

    QThread *m_thread = nullptr;
    std::shared_ptr<ConfigHWBTSerialOperationGate> m_operationGate;
    quint64 m_activeGeneration = 0;
};

#endif
