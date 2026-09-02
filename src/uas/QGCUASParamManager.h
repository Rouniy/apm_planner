#ifndef QGCUASPARAMMANAGER_H
#define QGCUASPARAMMANAGER_H

#include "comm/VehicleEndpoint.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

class ParameterService;
class ParameterStore;
class VehicleTargetManager;

/**
 * Compatibility facade for parameter consumers that are still being ported.
 *
 * The facade owns neither transport state nor a second parameter cache. All
 * reads come from ParameterStore's committed snapshot and all requests go
 * through ParameterService using the current exact endpoint lease. LinkManager
 * owns one instance for the entire application; widgets only observe it.
 */
class QGCUASParamManager final : public QObject
{
    Q_OBJECT

public:
    enum ParamFileType
    {
        TabSeperatedValues,
        CommaSeperatedValues
    };
    Q_ENUM(ParamFileType)

    explicit QGCUASParamManager(ParameterService *service,
                                VehicleTargetManager *targetManager,
                                QObject *parent = nullptr);

    bool parameterListReady() const { return m_parameterListReady; }
    bool parameterListInProgress() const { return m_parameterListInProgress; }
    int parameterListReceivedCount() const { return m_parameterListReceivedCount; }
    int parameterListReportedCount() const { return m_parameterListReportedCount; }

    QList<int> getComponentIds() const;
    QList<QString> getParameterNames(int component) const;
    QList<QVariant> getParameterValues(int component) const;
    bool getParameterValue(int component, const QString &parameter,
                           QVariant &value) const;
    QVariant getParameterValue(int component,
                               const QString &parameter) const;

    bool isParamMinKnown(const QString &param) const;
    bool isParamMaxKnown(const QString &param) const;
    bool isParamDefaultKnown(const QString &param) const;
    double getParamMin(const QString &param) const;
    double getParamMax(const QString &param) const;
    double getParamDefault(const QString &param) const;
    QString getParamInfo(const QString &param) const;
    void setParamInfo(const QMap<QString, QString> &param);
    void setParamMetadata(const QMap<QString, double> &minimum,
                          const QMap<QString, double> &maximum,
                          const QMap<QString, double> &defaults,
                          const QMap<QString, QString> &tooltips);

    ParameterStore *store() const { return m_store; }
    qulonglong writeParameters(int component,
                               const QVariantList &changes,
                               bool force = false);

signals:
    void parameterChanged(int component, QString parameter, QVariant value);
    void parameterChanged(int component, int parameterIndex, QVariant value);
    void parameterValueReceived(int component, int parameterCount,
                                int parameterIndex, QString parameter,
                                QVariant value, int type);
    void parameterWriteAcknowledged(int component, QString parameter,
                                    QVariant value, int type);
    void parameterWriteStarted(qulonglong transactionId, qulonglong batchId,
                               int component, QString parameter,
                               QVariant value, int type);
    void parameterWriteRetried(qulonglong transactionId, int attempt);
    void parameterWriteFailed(qulonglong transactionId, qulonglong batchId,
                              int component, QString parameter,
                              int reason, QString message);
    void parameterWriteCancelled(qulonglong transactionId, qulonglong batchId,
                                 int component, QString parameter);
    void parameterBatchStarted(qulonglong batchId, int total);
    void parameterBatchProgress(qulonglong batchId,
                                int completed, int total,
                                int succeeded, int failed);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void parameterTargetChanged();
    void parameterSnapshotAboutToChange();
    void parameterListUpToDate(int component);
    void parameterListLoadStarted();
    void parameterListReadyChanged(bool ready);
    void parameterListProgressChanged(int received, int reported, int percent);
    void parameterListLoadFailed(const QString &reason);
    void parameterListLoadCanceled();

public slots:
    void setParameter(int component, QString parameterName, QVariant value);
    void requestParameterList();
    void cancelParameterList();
    void requestParameterListUpdate(int component = 0);
    void requestParameterUpdate(int component, const QString &parameter);

private:
    bool matchesCurrentTarget(qulonglong generation,
                              int linkId, int systemId,
                              int componentId) const;
    bool matchesCurrentTransaction(qulonglong generation,
                                   int linkId, int systemId,
                                   int componentId) const;
    void setParameterListReady(bool ready);
    bool replayCommittedSnapshot(const VehicleTargetLease &target);
    void handleTargetChanged(qulonglong generation);
    void deferParameterListRequest(const VehicleTargetLease &target);
    void dispatchDeferredParameterListRequest();
    void deferParameterReadRequest(
        const VehicleTargetLease &target, const QString &name);
    void dispatchDeferredParameterReadRequests();
    void handleListStarted(qulonglong generation,
                           int linkId, int systemId, int componentId);
    void handleListCompleted(qulonglong generation,
                             int linkId, int systemId, int componentId);
    void handleListFailed(qulonglong generation,
                          int linkId, int systemId, int componentId,
                          const QString &reason);
    void handleListCancelled(qulonglong generation,
                             int linkId, int systemId, int componentId);
    void handleParameterValue(qulonglong generation,
                              int linkId, int systemId, int componentId,
                              int parameterCount, int parameterIndex,
                              const QString &name, const QVariant &value,
                              int type);
    void handleParameterWriteAcknowledged(
        qulonglong generation,
        int linkId, int systemId, int componentId,
        const QString &name, const QVariant &value, int type);
    void handleProgress(int received, int reported, int percent);

    ParameterService *const m_service;
    VehicleTargetManager *const m_targetManager;
    ParameterStore *const m_store;
    bool m_parameterListReady = false;
    bool m_parameterListInProgress = false;
    bool m_parameterListRequestDispatching = false;
    bool m_parameterListRequestFailedThisTurn = false;
    VehicleTargetLease m_failedParameterListTarget;
    VehicleTargetLease m_dispatchedParameterListTarget;
    VehicleTargetLease m_deferredParameterListTarget;
    bool m_deferredParameterListScheduled = false;
    struct DeferredParameterRead
    {
        VehicleTargetLease target;
        QString name;
    };
    QList<DeferredParameterRead> m_deferredParameterReads;
    bool m_deferredParameterReadsScheduled = false;
    int m_parameterListReceivedCount = 0;
    int m_parameterListReportedCount = 0;
    quint64 m_listGeneration = 0;
    quint64 m_lastHandledTargetGeneration = 0;
    VehicleEndpoint m_listEndpoint;
    QHash<qulonglong, VehicleTargetLease> m_parameterBatchTargets;

    QMap<QString, QString> m_paramToolTips;
    QMap<QString, double> m_paramMin;
    QMap<QString, double> m_paramDefault;
    QMap<QString, double> m_paramMax;
};

#endif // QGCUASPARAMMANAGER_H
