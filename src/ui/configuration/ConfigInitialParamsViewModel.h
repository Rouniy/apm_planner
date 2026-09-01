#ifndef CONFIGINITIALPARAMSVIEWMODEL_H
#define CONFIGINITIALPARAMSVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

struct ParamCompareRow
{
    QString name;
    QString current;
    QString newValue;
    double value = 0.0;
    bool exists = false;
    bool use = false;
};

class ConfigInitialParamsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool HasResults READ HasResults NOTIFY resultsChanged)
    Q_PROPERTY(bool Writing READ Writing NOTIFY writingChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)

public:
    explicit ConfigInitialParamsViewModel(QObject *parent = nullptr);

    static QStringList BatteryTypes();
    static QList<ParamCompareRow> SelectWritableRows(
        const QList<ParamCompareRow> &rows,
        const QStringList &availableParameters);

    QString PropSize() const;
    QString CellCount() const;
    QString CellMax() const;
    QString CellMin() const;
    QString BatteryType() const;
    bool TMotor() const;
    bool Suggested() const;
    QList<ParamCompareRow> Results() const;
    bool HasResults() const;
    bool Writing() const;
    QString Status() const;
    QString DocsUrl() const;

    void setPropSize(const QString &value);
    void setCellCount(const QString &value);
    void setCellMax(const QString &value);
    void setCellMin(const QString &value);
    void setBatteryType(const QString &value);
    void setTMotor(bool enabled);
    void setSuggested(bool enabled);
    void setVehicleContext(bool plane, int firmwareMajor);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    bool setResultUse(const QString &name, bool use);
    bool Calculate();
    bool WriteToFc(const QStringList &availableParameters, bool connected);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void inputsChanged();
    void resultsChanged();
    void statusChanged();
    void writingChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct PendingWrite
    {
        double expectedValue = 0.0;
        quint64 generation = 0;
    };

    static double RoundTo(double value, int precision);
    static double Parse(const QString &text);
    static QString Format(double value);
    void appendResult(const QString &name, double value);
    void invalidateResults();
    void setStatus(const QString &status);
    void dispatchNextWrite();
    void finishWrite(const QString &name, bool failed);
    void finishWriteBatch();
    QString normalizedName(const QString &name) const;

    QString m_propSize = QStringLiteral("9");
    QString m_cellCount = QStringLiteral("4");
    QString m_cellMax = QStringLiteral("4.2");
    QString m_cellMin = QStringLiteral("3.3");
    QString m_batteryType = QStringLiteral("LiPo");
    bool m_tMotor = false;
    bool m_suggested = false;
    bool m_plane = false;
    int m_firmwareMajor = 0;
    int m_componentId = 1;
    QList<ParamCompareRow> m_results;
    QHash<QString, QVariant> m_values;
    QHash<QString, PendingWrite> m_pendingWrites;
    QList<ParamCompareRow> m_writeQueue;
    QSet<QString> m_failedWrites;
    QString m_status;
    int m_writeCount = 0;
    quint64 m_writeGeneration = 0;
    bool m_writing = false;
};

#endif
