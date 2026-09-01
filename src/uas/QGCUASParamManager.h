#ifndef QGCUASPARAMMANAGER_H
#define QGCUASPARAMMANAGER_H

#include <QWidget>
#include <QMap>
#include <QTimer>
#include <QVariant>

class UASInterface;

class QGCUASParamManager : public QWidget
{
    Q_OBJECT
public:
    enum ParamFileType
    {
        TabSeperatedValues,
        CommaSeperatedValues
    };
    QGCUASParamManager(UASInterface* uas, QWidget *parent = 0);

    bool parameterListReady() const { return m_parameterListReady; }
    bool parameterListInProgress() const { return transmissionListMode; }
    int parameterListReceivedCount() const {
        return m_parameterListReceivedCount;
    }
    int parameterListReportedCount() const {
        return m_parameterListReportedCount;
    }
    QList<int> getComponentIds() const { return parameters.keys(); }

    QList<QString> getParameterNames(int component) const;
    QList<QVariant> getParameterValues(int component) const;
    bool getParameterValue(int component, const QString& parameter, QVariant& value) const;
    QVariant getParameterValue(int component, const QString& parameter) const;

    virtual bool isParamMinKnown(const QString& param) = 0;
    virtual bool isParamMaxKnown(const QString& param) = 0;
    virtual bool isParamDefaultKnown(const QString& param) = 0;
    virtual double getParamMin(const QString& param) = 0;
    virtual double getParamMax(const QString& param) = 0;
    virtual double getParamDefault(const QString& param) = 0;
    virtual QString getParamInfo(const QString& param) = 0;
    virtual void setParamInfo(const QMap<QString,QString>& param) = 0;

    virtual QString summaryInfoFromFile(const QString& filename) = 0;
    virtual bool loadParamsFromFile(const QString &filename,ParamFileType type) = 0;

    /** @brief Request an update for the parameter list */
    void requestParameterListUpdate(int component = 0);
    /** @brief Request an update for this specific parameter */
    virtual void requestParameterUpdate(int component, const QString& parameter) = 0;

signals:
    void parameterChanged(int component, QString parameter, QVariant value);
    void parameterChanged(int component, int parameterIndex, QVariant value);
    void parameterListUpToDate(int component);
    void parameterListLoadStarted();
    void parameterListReadyChanged(bool ready);
    void parameterListLoadFailed(const QString &reason);
    void parameterListLoadCanceled();

public slots:
    /** @brief Write one parameter to the MAV */
    virtual void setParameter(int component, QString parameterName, QVariant value) = 0;
    /** @brief Request list of parameters from MAV */
    virtual void requestParameterList() = 0;
    /** @brief Stop a pending full-list request without claiming completion. */
    virtual void cancelParameterList() = 0;

protected:
    void setParameterListReady(bool ready);

    UASInterface* mav;   ///< The MAV this widget is controlling
    QMap<int, QMap<QString, QVariant>* > changedValues; ///< Changed values
    QMap<int, QMap<QString, QVariant>* > parameters; ///< All parameters
    QVector<bool> received; ///< Successfully received parameters
    QMap<int, QList<int>* > transmissionMissingPackets; ///< Missing packets
    QMap<int, QMap<QString, QVariant>* > transmissionMissingWriteAckPackets; ///< Missing write ACK packets
    bool transmissionListMode;       ///< Currently requesting list
    bool m_parameterListReady;       ///< A full list completed without timeout
    int m_parameterListReceivedCount = 0; ///< Unique primary-list packets received
    int m_parameterListReportedCount = 0; ///< Primary param_count for current request
    QMap<int, bool> transmissionListSizeKnown;  ///< List size initialized?
    bool transmissionActive;         ///< Missing packets, working on list?
    quint64 transmissionTimeout;     ///< Timeout
    QTimer retransmissionTimer;      ///< Timer handling parameter retransmission
    int retransmissionTimeout; ///< Retransmission request timeout, in milliseconds
    int rewriteTimeout; ///< Write request timeout, in milliseconds
    int retransmissionBurstRequestSize; ///< Number of packets requested for retransmission per burst

};

#endif // QGCUASPARAMMANAGER_H
