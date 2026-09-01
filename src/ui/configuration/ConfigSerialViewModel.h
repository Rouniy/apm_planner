#ifndef CONFIGSERIALVIEWMODEL_H
#define CONFIGSERIALVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>

struct SerialOptionRule
{
    int baudrate = -1;
    qlonglong options = -1;
    QString comment;
};

struct SerialBitOption
{
    int bit = 0;
    QString label;
    bool isSet = false;
};

struct SerialPortRow
{
    int componentId = 1;
    int portIndex = 0;
    QString portName;
    QString label;
    QList<ParamOption> baudOptions;
    QList<ParamOption> protocolOptions;
    QList<SerialBitOption> optionBits;
    QVariant selectedBaud;
    QVariant selectedProtocol;
    qulonglong optionsValue = 0;
    QString optionsText;
    QString status;
    bool hasProtocol = false;
    bool hasOptions = false;
    bool hasBits = false;
};

class ConfigSerialViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Note READ Note CONSTANT)
    Q_PROPERTY(QString Warning READ Warning NOTIFY warningChanged)
    Q_PROPERTY(bool HasWarning READ HasWarning NOTIFY warningChanged)

public:
    explicit ConfigSerialViewModel(QObject *parent = nullptr);

    static QMap<int, SerialOptionRule> OptionRules();

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);

    QList<SerialPortRow> Ports() const;
    QString Note() const;
    QString Warning() const;
    bool HasWarning() const;

    bool selectBaud(const QString &portName, const QVariant &value);
    bool selectProtocol(const QString &portName, const QVariant &value);
    bool setOptionBit(const QString &portName, int bit, bool enabled);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void structureChanged();
    void rowChanged(const QString &portName);
    void warningChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct PendingWrite
    {
        QString portName;
        QVariant expectedValue;
        quint64 generation = 0;
    };

    void rebuildPorts();
    void rebuildRowMetadata(SerialPortRow *row);
    void hydrateRow(SerialPortRow *row);
    void applyValueToRow(SerialPortRow *row, const QString &name,
                         const QVariant &value);
    void queueWrite(SerialPortRow *row, const QString &name,
                    const QVariant &value);
    void finishPending(const QString &name, const QString &status,
                       bool revertToAuthoritativeValue);
    void updateRowStatus(SerialPortRow *row);
    void updateOptions(SerialPortRow *row, qulonglong value);
    void updateLabel(SerialPortRow *row);
    void recomputeWarning(const QString &comment = QString());
    QList<ParamOption> enumOptions(const QString &name,
                                   const QString &templateName) const;
    QList<SerialBitOption> bitOptions(const QString &name,
                                     const QString &templateName) const;
    SerialPortRow *rowForPort(const QString &portName);
    const SerialPortRow *rowForPort(const QString &portName) const;
    QString normalizedName(const QString &name) const;
    QVariant authoritativeValue(const QString &name) const;
    bool hasParameter(const QString &name) const;
    bool optionExists(const QList<ParamOption> &options,
                      const QVariant &value) const;

    ParameterMetaDataCatalog m_catalog;
    QList<ConfigFriendlyParameterValue> m_parameters;
    QList<SerialPortRow> m_ports;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QHash<QString, QList<QVariant>> m_supersededWrites;
    QHash<QString, QString> m_writeErrors;
    QString m_note;
    QString m_warning;
    QString m_protocolComment;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
};

#endif
