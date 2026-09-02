#ifndef BATTERYMONITORINSTANCEMODEL_H
#define BATTERYMONITORINSTANCEMODEL_H

#include "comm/QGCMAVLink.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class BatteryMonitorInstanceModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Prefix READ Prefix CONSTANT)
    Q_PROPERTY(int BatteryId READ BatteryId CONSTANT)
    Q_PROPERTY(bool Connected READ Connected NOTIFY stateChanged)
    Q_PROPERTY(bool Available READ Available NOTIFY stateChanged)
    Q_PROPERTY(bool CanEdit READ CanEdit NOTIFY stateChanged)
    Q_PROPERTY(double LiveVoltage READ LiveVoltage NOTIFY liveValuesChanged)
    Q_PROPERTY(double LiveCurrent READ LiveCurrent NOTIFY liveValuesChanged)
    Q_PROPERTY(bool HasLiveVoltage READ HasLiveVoltage
               NOTIFY liveValuesChanged)
    Q_PROPERTY(bool HasLiveCurrent READ HasLiveCurrent
               NOTIFY liveValuesChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)

public:
    explicit BatteryMonitorInstanceModel(
        const QString &prefix, int batteryId, QObject *parent = nullptr);

    QString Prefix() const { return m_prefix; }
    int BatteryId() const { return m_batteryId; }
    bool Connected() const { return m_connected; }
    bool Available() const;
    bool CanEdit() const
    {
        return m_connected && Available() && m_pendingWrite.name.isEmpty();
    }
    bool HasPendingWrite() const { return !m_pendingWrite.name.isEmpty(); }
    double LiveVoltage() const { return m_liveVoltage; }
    double LiveCurrent() const { return m_liveCurrent; }
    bool HasLiveVoltage() const { return m_hasLiveVoltage; }
    bool HasLiveCurrent() const { return m_hasLiveCurrent; }
    QString Status() const { return m_status; }
    int ComponentId() const { return m_componentId; }

    QString MonitorParameter() const;
    QString CapacityParameter() const;
    QString VoltPinParameter() const;
    QString CurrPinParameter() const;
    QString VoltMultiplierParameter() const;
    QString AmpPerVoltParameter() const { return m_ampPerVoltParameter; }
    QString AmpOffsetParameter() const;
    QStringList ParameterNames() const;

    static QStringList AmpPerVoltCandidates(const QString &prefix);
    static QString ResolveAmpPerVoltParameter(
        const QString &prefix, const QStringList &availableParameters);
    static bool CalibratedScale(double measuredValue, double liveValue,
                                double currentScale, double *newScale);
    static bool ShouldTriggerBatteryAlert(
        double voltage, double remainingPercent,
        double warningVoltage, double warningPercent);
    static QString FormatBatteryAlert(
        const QString &messageTemplate,
        double voltage, double remainingPercent);

    bool AcceptsParameter(const QString &name) const;
    bool IsAmpPerVoltParameter(const QString &name) const;
    bool HasParameter(const QString &name) const;
    QVariant ParameterValue(const QString &name) const;

    void Reset(int componentId = MAV_COMP_ID_PRIMARY);
    void setConnected(bool connected);
    void setLiveValues(double voltage, bool hasVoltage,
                       double current, bool hasCurrent);
    void observeMavlinkMessage(const mavlink_message_t &message);

    bool setFieldValue(const QString &name, const QVariant &value);
    bool ApplyVoltageCalibration(double measuredVoltage);
    bool ApplyCurrentCalibration(double measuredCurrent);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterBatchSubmitted(int componentId, const QString &name,
                                 qulonglong batchId);
    void parameterWriteAcknowledged(int componentId, const QString &name,
                                    const QVariant &value, int type);
    void parameterWriteFailed(qulonglong transactionId,
                              qulonglong batchId, int componentId,
                              const QString &name, int reason,
                              const QString &message);
    void parameterWriteCancelled(qulonglong transactionId,
                                 qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void parameterWriteSubmissionFailed(int componentId,
                                        const QString &name,
                                        const QString &reason);

signals:
    void fieldChanged(const QString &name);
    void structureChanged();
    void liveValuesChanged();
    void stateChanged();
    void statusChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct PendingWrite
    {
        QString name;
        QVariant expectedValue;
        qulonglong batchId = 0;
        bool acknowledged = false;
        bool failed = false;
        QString failureMessage;
    };

    QString normalizedName(const QString &name) const;
    bool pendingMatches(int componentId, const QString &name,
                        qulonglong batchId) const;
    void clearPendingWrite();
    void resolveAmpPerVoltParameter();
    void setStatus(const QString &status);

    QString m_prefix;
    int m_batteryId = 0;
    int m_componentId = MAV_COMP_ID_PRIMARY;
    QHash<QString, QVariant> m_values;
    QString m_ampPerVoltParameter;
    QString m_status;
    double m_liveVoltage = 0.0;
    double m_liveCurrent = 0.0;
    bool m_connected = false;
    bool m_hasLiveVoltage = false;
    bool m_hasLiveCurrent = false;
    PendingWrite m_pendingWrite;
};

#endif // BATTERYMONITORINSTANCEMODEL_H
