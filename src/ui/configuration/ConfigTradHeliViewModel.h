#ifndef CONFIGTRADHELIVIEWMODEL_H
#define CONFIGTRADHELIVIEWMODEL_H

#include "HeliVisualization.h"
#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class QTimer;

class ConfigTradHeliViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigTradHeliViewModel(QObject *parent = nullptr);

    static QList<QStringList> FieldCandidates();
    static QStringList ReferenceFieldNames();
    static int WriteTimeoutMs();

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setActive(bool active);

    QString Title() const;
    QString Intro() const;
    QList<ParamField> Fields() const { return m_fields; }
    int ComponentId() const { return m_componentId; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool Active() const { return m_active; }
    bool HasPendingWrites() const { return !m_pendingWrites.isEmpty(); }
    QString Status() const { return m_status; }
    QString ServoStatus() const { return m_servoStatus; }

    QString SwashParameter() const { return m_swashParameter; }
    bool HasLegacySwash() const;
    bool SwashIsCcpm() const;
    QString ManualServoParameter() const { return m_manualParameter; }
    bool ManualServoActive() const;
    bool ManualOverrideMayBeActive() const;
    bool ManualModeSupported(int mode) const;

    QVector<HeliVisualization::CurvePoint> StabilizeCurve() const;
    QVector<HeliVisualization::CurvePoint> AcroCurve() const;
    double CollectiveCursorPercent() const;
    double CollectiveInput() const { return m_collectiveInput; }
    double RudderInput() const { return m_rudderInput; }
    double Servo1Position() const;
    double Servo2Position() const;
    double Servo3Position() const;
    QString CollectiveRangeText() const;
    QString RudderRangeText() const;

    bool setFieldValue(const QString &name, const QVariant &value);
    bool setSwashCcpm(bool ccpm);
    bool setManualServoMode(int mode);
    bool Refresh();
    void setRcInput(int zeroBasedChannel, double pwm);
    void setServoOutput(int oneBasedChannel, int pwm);
    void resetObservedRanges();
    void pumpVisualization();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void structureChanged();
    void fieldChanged(const QString &name);
    void stateChanged();
    void visualizationChanged();
    void writeRequested(quint64 requestId, int componentId,
                        const QString &name, const QVariant &value);
    void refreshRequested(int componentId);
    void manualSafetyWarning(const QString &warning);

private:
    enum class WriteKind
    {
        Field,
        Swash,
        ManualServo
    };

    struct PendingWrite
    {
        WriteKind kind = WriteKind::Field;
        QVariant expectedValue;
        quint64 generation = 0;
        quint64 snapshotGeneration = 0;
        qulonglong batchId = 0;
        bool manualUncertainBefore = false;
    };

    void rebuildFields();
    ParamField makeField(const QStringList &candidates, int index) const;
    ParamField *fieldForName(const QString &name);
    QString resolve(const QStringList &candidates) const;
    ParameterMetaData metadataFor(const QStringList &candidates,
                                  const QString &resolvedName) const;
    QVariant valueFor(const QStringList &candidates,
                      const QVariant &fallback = QVariant()) const;
    double numericValue(const QStringList &candidates,
                        double fallback) const;
    QVariant typedValue(const QVariant &value,
                        const QVariant &reference) const;
    bool queueWrite(WriteKind kind, const QString &name,
                    const QVariant &value);
    void finishPending(const QString &name, const QString &error,
                       bool applyAuthoritativeValue);
    void finishPendingSuccess(const QString &name);
    QHash<QString, PendingWrite>::iterator pendingForRequest(
        quint64 requestId);
    QHash<QString, PendingWrite>::iterator pendingForBatch(
        qulonglong batchId);
    void setStatus(const QString &status);
    void setServoStatus(const QString &status);
    QString normalizedName(const QString &name) const;
    bool valueEquals(const QVariant &left, const QVariant &right) const;
    void updateVisualization();

    ParameterMetaDataCatalog m_catalog;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QString m_swashParameter;
    QString m_manualParameter;
    QString m_status;
    QString m_servoStatus;
    HeliVisualization::InputRange m_collectiveRange{2200.0, 800.0};
    HeliVisualization::InputRange m_rudderRange{2200.0, 800.0};
    double m_collectiveInput = 1500.0;
    double m_rudderInput = 1500.0;
    int m_collectiveOutput = 1500;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
    quint64 m_snapshotGeneration = 0;
    bool m_enforceMetadataRanges = true;
    bool m_connected = false;
    bool m_armed = false;
    bool m_active = false;
    bool m_snapshotReady = false;
    bool m_manualOverrideUncertain = false;
    QTimer *m_visualizationTimer = nullptr;
};

#endif // CONFIGTRADHELIVIEWMODEL_H
