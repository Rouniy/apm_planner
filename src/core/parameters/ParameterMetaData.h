#ifndef PARAMETERMETADATA_H
#define PARAMETERMETADATA_H

#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariant>

class QIODevice;

enum class ParameterUserLevel
{
    Unknown,
    Standard,
    Advanced
};

enum class ParameterMetaDataScope
{
    Vehicle,
    Library
};

struct ParameterMetaDataOption
{
    QVariant value;
    QString rawCode;
    QString label;
};

struct ParameterMetaData
{
    QString name;
    QString rawName;
    QString group;
    QString title;
    QString description;
    QString units;
    QString rangeText;
    double minimum = 0.0;
    double maximum = 0.0;
    double increment = 0.01;
    QList<ParameterMetaDataOption> values;
    QList<QPair<int, QString>> bitmaskValues;
    QMap<QString, QString> fields;
    // Retain presence separately from value: an explicit false/empty PDEF
    // field must not be replaced by lower-priority generated metadata.
    QSet<QString> presentFields;
    ParameterUserLevel userLevel = ParameterUserLevel::Unknown;
    ParameterMetaDataScope scope = ParameterMetaDataScope::Library;
    bool hasRange = false;
    bool hasIncrement = false;
    bool readOnly = false;
    bool rebootRequired = false;
    bool volatileValue = false;
    bool calibration = false;

    bool isEnum() const { return !values.isEmpty(); }
    bool isBitmask() const { return !bitmaskValues.isEmpty(); }
};

class ParameterMetaDataCatalog
{
public:
    static ParameterMetaDataCatalog fromPdef(QIODevice *device,
                                             const QString &vehicleName,
                                             bool requireVehicleSection = true);

    bool isValid() const;
    QString errorString() const;
    bool contains(const QString &name) const;
    ParameterMetaData value(const QString &name) const;
    QList<ParameterMetaData> entries() const;
    QList<ParameterMetaData> entriesForLevel(ParameterUserLevel level) const;
    ParameterMetaDataCatalog withFallback(const ParameterMetaDataCatalog &fallback,
                                         bool *addedAdvisoryRange = nullptr) const;

private:
    QMap<QString, ParameterMetaData> m_entries;
    QString m_error;
    bool m_loaded = false;
};

#endif
