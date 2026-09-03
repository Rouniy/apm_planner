#ifndef DEVICEOPERATIONSVIEWMODEL_H
#define DEVICEOPERATIONSVIEWMODEL_H

#include "comm/DeviceOperationService.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

/** UI state for Mission Planner 10's DeviceOperationsView. */
class DeviceOperationsViewModel final : public QObject
{
    Q_OBJECT

public:
    using TargetResolver = std::function<VehicleTargetLease()>;
    using Confirmation = std::function<bool(const QString &title,
                                             const QString &message)>;

    struct Dependencies
    {
        TargetResolver resolveTarget;
        Confirmation confirm;
    };

    explicit DeviceOperationsViewModel(
        DeviceOperationService *service, Dependencies dependencies,
        QObject *parent = nullptr);
    ~DeviceOperationsViewModel() override;

    QStringList busTypes() const;
    int systemId() const { return m_systemId; }
    int componentId() const { return m_componentId; }
    QString busType() const { return m_busType; }
    QString busName() const { return m_busName; }
    int busNumber() const { return m_busNumber; }
    int address() const { return m_address; }
    int registerStart() const { return m_registerStart; }
    int count() const { return m_count; }
    bool isSpi() const;
    bool isBusy() const;
    bool requiresTargetRebind() const;
    bool canOperate() const;
    bool canTest() const;
    QString output() const { return m_output; }

    static QString FormatStatus(quint8 status);
    static QString FormatResult(quint8 status, quint8 registerStart,
                                const QByteArray &data,
                                bool timedOut = false);

public slots:
    void setSystemId(int value);
    void setComponentId(int value);
    void setBusType(const QString &value);
    void setBusName(const QString &value);
    void setBusNumber(int value);
    void setAddress(int value);
    void setRegisterStart(int value);
    void setCount(int value);
    void useActiveTarget();
    void readRegisters();
    void testIcm20948();
    void cancel();
    void shutdown();

signals:
    void changed();

private:
    void bindActiveTarget(bool initial);
    DeviceOperationService::Request request() const;
    QString validate(bool requireSpi = false,
                     bool requireDisarmed = false) const;
    void applyStartFailure(DeviceOperationService::StartResult result,
                           const QString &operation);
    void operationFinished(
        const DeviceOperationService::OperationResult &result);
    void setOutput(const QString &text);

    QPointer<DeviceOperationService> m_service;
    Dependencies m_dependencies;
    int m_systemId = 1;
    int m_componentId = 1;
    QString m_busType = QStringLiteral("SPI");
    QString m_busName = QStringLiteral("icm20948_ext");
    int m_busNumber = 0;
    int m_address = 0;
    int m_registerStart = 255;
    int m_count = 1;
    QString m_output = QStringLiteral(
        "DEVICE_OP directly accesses a flight-controller peripheral bus. "
        "Use only with known hardware.");
    bool m_shuttingDown = false;
};

#endif // DEVICEOPERATIONSVIEWMODEL_H
