#ifndef CONFIGESCCALIBRATIONVIEWMODEL_H
#define CONFIGESCCALIBRATIONVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class ConfigESCCalibrationViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Intro READ Intro CONSTANT)
    Q_PROPERTY(QString Instructions READ Instructions CONSTANT)
    Q_PROPERTY(QString CalButtonText READ CalButtonText
               NOTIFY stateChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanCalibrate READ CanCalibrate NOTIFY stateChanged)

public:
    explicit ConfigESCCalibrationViewModel(QObject *parent = nullptr);

    static QStringList FieldNames();

    QString Title() const;
    QString Intro() const;
    QString Instructions() const;
    QString CalButtonText() const;
    QString Status() const;
    bool Busy() const;
    bool CanCalibrate() const;
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool CalibrationComplete() const { return m_calibrationComplete; }
    bool HasPendingWrites() const { return !m_pendingWrites.isEmpty(); }
    int ComponentId() const { return m_componentId; }
    QList<ParamField> Fields() const { return m_fields; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    bool setFieldValue(const QString &name, const QVariant &value);
    bool CalibrateEsc(bool confirmed);
    bool Refresh(bool armedConfirmed = false);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void structureChanged();
    void fieldChanged(const QString &name);
    void stateChanged();
    void statusChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);
    void refreshRequested(int componentId);

private:
    struct PendingWrite
    {
        QVariant expectedValue;
        QVariant previousValue;
        quint64 generation = 0;
        bool calibration = false;
    };

    void rebuildFields();
    ParamField makeField(const QString &name, int index) const;
    ParamField *fieldForName(const QString &name);
    const ParamField *fieldForName(const QString &name) const;
    void queueWrite(const QString &name, const QVariant &value,
                    bool calibration);
    void finishPending(const QString &name, const QString &reason);
    void failAllPending(const QString &reason);
    void setStatus(const QString &status);
    QString normalizedName(const QString &name) const;
    QVariant typedValue(const QVariant &value,
                        const QVariant &reference) const;

    ParameterMetaDataCatalog m_catalog;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QString m_status;
    int m_componentId = 1;
    quint64 m_writeGeneration = 0;
    bool m_connected = false;
    bool m_armed = false;
    bool m_calibrationBusy = false;
    bool m_calibrationComplete = false;
};

#endif
