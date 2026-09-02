#ifndef CONFIGRADIOOUTPUTVIEWMODEL_H
#define CONFIGRADIOOUTPUTVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

struct ServoOutputRow
{
    int Number = 0;
    int Pwm = 1500;
    ParamField Reversed;
    ParamField Function;
    ParamField Min;
    ParamField Trim;
    ParamField Max;
    QString Status;
};

class ConfigRadioOutputViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigRadioOutputViewModel(QObject *parent = nullptr);

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);

    QList<ServoOutputRow> Rows() const;

    bool setReversed(int oneBasedChannel, bool reversed);
    bool setFunction(int oneBasedChannel, const QVariant &function);
    bool setMin(int oneBasedChannel, int pwm);
    bool setTrim(int oneBasedChannel, int pwm);
    bool setMax(int oneBasedChannel, int pwm);

    // Telemetry producers may call this at message rate. It deliberately does
    // not notify the view; the widget's 100 ms timer calls flushServoOutputs()
    // so a burst produces at most one repaint per changed row.
    void setServoOutput(int oneBasedChannel, int pwm);
    void flushServoOutputs();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void structureChanged();
    void rowChanged(int oneBasedChannel);
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct PendingWrite
    {
        int servoNumber = 0;
        QVariant expectedValue;
        quint64 generation = 0;
    };

    void rebuildRows();
    ParamField makeField(int servoNumber, const QString &suffix,
                         ParamField::EditorKind kind,
                         const QVariant &defaultValue) const;
    ParameterMetaData metadataFor(const QString &name,
                                  const QString &templateName) const;
    bool setFieldValue(int servoNumber, const QString &suffix,
                       const QVariant &value);
    void queueWrite(int servoNumber, const QString &name,
                    const QVariant &value);
    void finishPending(const QString &name, const QString &status,
                       bool revertToAuthoritativeValue);
    void applyValueToRow(ServoOutputRow *row, const QString &name,
                         const QVariant &value);
    void updateRowStatus(ServoOutputRow *row);
    ServoOutputRow *rowForNumber(int oneBasedChannel);
    const ServoOutputRow *rowForNumber(int oneBasedChannel) const;
    ParamField *fieldForName(ServoOutputRow *row, const QString &name);
    QString normalizedName(const QString &name) const;
    QVariant authoritativeValue(const QString &name) const;
    bool hasParameter(const QString &name) const;

    ParameterMetaDataCatalog m_catalog;
    QList<ServoOutputRow> m_rows;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QHash<QString, QList<QVariant>> m_supersededWrites;
    QHash<QString, QString> m_writeErrors;
    QHash<int, int> m_pendingServoOutputs;
    QHash<int, int> m_latestServoOutputs;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
};

#endif
