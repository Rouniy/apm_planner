#include "ParameterMetaData.h"

#include <QIODevice>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

namespace {
ParameterUserLevel userLevel(const QString &value)
{
    if (value.compare(QStringLiteral("Standard"), Qt::CaseInsensitive) == 0) {
        return ParameterUserLevel::Standard;
    }
    if (value.compare(QStringLiteral("Advanced"), Qt::CaseInsensitive) == 0) {
        return ParameterUserLevel::Advanced;
    }
    return ParameterUserLevel::Unknown;
}

QString normalizedName(QString name)
{
    const int separator = name.indexOf(QLatin1Char(':'));
    if (separator >= 0) {
        name = name.mid(separator + 1);
    }
    return name.trimmed().toUpper();
}

bool parseRange(const QString &text, double *minimum, double *maximum)
{
    static const QRegularExpression numberPattern(QStringLiteral(
        "[-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    QRegularExpressionMatchIterator matches = numberPattern.globalMatch(text);
    if (!matches.hasNext()) {
        return false;
    }
    bool minimumOk = false;
    const double parsedMinimum = matches.next().captured().toDouble(&minimumOk);
    if (!minimumOk || !matches.hasNext()) {
        return false;
    }
    bool maximumOk = false;
    const double parsedMaximum = matches.next().captured().toDouble(&maximumOk);
    if (!maximumOk) {
        return false;
    }
    *minimum = qMin(parsedMinimum, parsedMaximum);
    *maximum = qMax(parsedMinimum, parsedMaximum);
    return true;
}

QVariant typedValue(const QString &rawCode)
{
    bool integerOk = false;
    const qlonglong integerValue = rawCode.toLongLong(&integerOk);
    if (integerOk) {
        return QVariant::fromValue(integerValue);
    }
    bool realOk = false;
    const double realValue = QLocale::c().toDouble(rawCode, &realOk);
    if (realOk) {
        return realValue;
    }
    return rawCode;
}

QList<ParameterMetaDataOption> parseValues(QXmlStreamReader *xml)
{
    QList<ParameterMetaDataOption> values;
    while (!xml->atEnd()) {
        xml->readNext();
        if (xml->isEndElement() && xml->name() == QLatin1String("values")) {
            break;
        }
        if (!xml->isStartElement() || xml->name() != QLatin1String("value")) {
            continue;
        }
        const QString rawCode = xml->attributes().value(
            QStringLiteral("code")).toString().trimmed();
        const QString label = xml->readElementText(
            QXmlStreamReader::SkipChildElements).trimmed();
        if (!rawCode.isEmpty()) {
            values.append({typedValue(rawCode), rawCode, label});
        }
    }
    return values;
}

QString fieldValue(const QMap<QString, QString> &fields, const QString &name)
{
    for (auto iterator = fields.constBegin(); iterator != fields.constEnd();
         ++iterator) {
        if (iterator.key().compare(name, Qt::CaseInsensitive) == 0) {
            return iterator.value();
        }
    }
    return QString();
}

bool fieldFlag(const QMap<QString, QString> &fields, const QString &name)
{
    const QString value = fieldValue(fields, name).trimmed();
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value == QLatin1String("1")
        || value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
}

QList<QPair<int, QString>> parseBitmask(const QString &text)
{
    QList<QPair<int, QString>> result;
    const QStringList entries = text.split(QLatin1Char(','),
                                           Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const int separator = entry.indexOf(QLatin1Char(':'));
        if (separator <= 0) {
            continue;
        }
        bool bitOk = false;
        const int bit = entry.left(separator).trimmed().toInt(&bitOk);
        if (bitOk) {
            result.append(qMakePair(bit,
                                    entry.mid(separator + 1).trimmed()));
        }
    }
    return result;
}

ParameterMetaData parseParameter(QXmlStreamReader *xml, const QString &group,
                                 ParameterMetaDataScope scope)
{
    ParameterMetaData result;
    const QXmlStreamAttributes attributes = xml->attributes();
    result.rawName = attributes.value(QStringLiteral("name")).toString();
    result.name = normalizedName(result.rawName);
    result.group = group;
    result.scope = scope;
    result.title = attributes.value(
        QStringLiteral("humanName")).toString().trimmed();
    result.description = attributes.value(
        QStringLiteral("documentation")).toString().trimmed();
    result.userLevel = userLevel(
        attributes.value(QStringLiteral("user")).toString());

    while (!xml->atEnd()) {
        xml->readNext();
        if (xml->isEndElement() && xml->name() == QLatin1String("param")) {
            break;
        }
        if (!xml->isStartElement()) {
            continue;
        }
        if (xml->name() == QLatin1String("values")) {
            result.values = parseValues(xml);
        } else if (xml->name() == QLatin1String("field")) {
            const QString fieldName = xml->attributes().value(
                QStringLiteral("name")).toString();
            result.fields.insert(fieldName, xml->readElementText(
                QXmlStreamReader::SkipChildElements).trimmed());
        }
    }

    result.units = fieldValue(result.fields, QStringLiteral("Units"));
    result.rangeText = fieldValue(result.fields, QStringLiteral("Range"));
    result.hasRange = parseRange(result.rangeText,
                                 &result.minimum, &result.maximum);
    bool incrementOk = false;
    result.increment = QLocale::c().toDouble(
        fieldValue(result.fields, QStringLiteral("Increment")), &incrementOk);
    result.hasIncrement = incrementOk && result.increment > 0.0;
    if (!result.hasIncrement) {
        result.increment = 0.01;
    }
    result.bitmaskValues = parseBitmask(
        fieldValue(result.fields, QStringLiteral("Bitmask")));
    result.readOnly = fieldFlag(result.fields, QStringLiteral("ReadOnly"));
    result.rebootRequired = fieldFlag(
        result.fields, QStringLiteral("RebootRequired"));
    result.volatileValue = fieldFlag(
        result.fields, QStringLiteral("Volatile"));
    result.calibration = fieldFlag(
        result.fields, QStringLiteral("Calibration"));
    return result;
}
}

ParameterMetaDataCatalog ParameterMetaDataCatalog::fromPdef(
    QIODevice *device, const QString &vehicleName)
{
    ParameterMetaDataCatalog catalog;
    if (!device || !device->isReadable()) {
        catalog.m_error = QStringLiteral("Parameter metadata input is not readable");
        return catalog;
    }
    catalog.m_loaded = true;

    enum class Section { None, Vehicles, Libraries };
    Section section = Section::None;
    bool includeParameters = false;
    bool vehicleParameters = false;
    bool foundRoot = false;
    bool foundVehicle = false;
    QString parameterGroup;
    QSet<QString> vehicleEntries;
    QXmlStreamReader xml(device);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("paramfile")) {
                foundRoot = true;
            } else if (xml.name() == QLatin1String("vehicles")) {
                section = Section::Vehicles;
            } else if (xml.name() == QLatin1String("libraries")) {
                section = Section::Libraries;
            } else if (xml.name() == QLatin1String("parameters")) {
                parameterGroup = xml.attributes().value(
                    QStringLiteral("name")).toString();
                vehicleParameters = section == Section::Vehicles
                    && parameterGroup.compare(vehicleName,
                                              Qt::CaseInsensitive) == 0;
                foundVehicle = foundVehicle || vehicleParameters;
                includeParameters = section == Section::Libraries
                    || vehicleParameters;
            } else if (xml.name() == QLatin1String("param")
                       && includeParameters) {
                const ParameterMetaData metadata = parseParameter(
                    &xml, parameterGroup,
                    vehicleParameters ? ParameterMetaDataScope::Vehicle
                                      : ParameterMetaDataScope::Library);
                if (!metadata.name.isEmpty() && vehicleParameters
                    && !vehicleEntries.contains(metadata.name)) {
                    // Vehicle-specific definitions are authoritative even if a
                    // PDEF happens to place its libraries first.
                    catalog.m_entries.insert(metadata.name, metadata);
                    vehicleEntries.insert(metadata.name);
                } else if (!metadata.name.isEmpty()
                           && !catalog.m_entries.contains(metadata.name)) {
                    catalog.m_entries.insert(metadata.name, metadata);
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("parameters")) {
                includeParameters = false;
                vehicleParameters = false;
                parameterGroup.clear();
            } else if (xml.name() == QLatin1String("vehicles")
                       || xml.name() == QLatin1String("libraries")) {
                section = Section::None;
            }
        }
    }
    if (xml.hasError()) {
        catalog.m_error = xml.errorString();
    } else if (!foundRoot) {
        catalog.m_error = QStringLiteral("Parameter metadata root is missing");
    } else if (!foundVehicle) {
        catalog.m_error = QStringLiteral("Vehicle metadata section '%1' is missing")
            .arg(vehicleName);
    }
    return catalog;
}

bool ParameterMetaDataCatalog::isValid() const
{
    return m_loaded && m_error.isEmpty();
}

QString ParameterMetaDataCatalog::errorString() const
{
    return m_error;
}

bool ParameterMetaDataCatalog::contains(const QString &name) const
{
    return m_entries.contains(name.trimmed().toUpper());
}

ParameterMetaData ParameterMetaDataCatalog::value(const QString &name) const
{
    return m_entries.value(name.trimmed().toUpper());
}

QList<ParameterMetaData> ParameterMetaDataCatalog::entries() const
{
    return m_entries.values();
}

QList<ParameterMetaData> ParameterMetaDataCatalog::entriesForLevel(
    ParameterUserLevel level) const
{
    QList<ParameterMetaData> result;
    for (const ParameterMetaData &metadata : m_entries) {
        if (metadata.userLevel == level) {
            result.append(metadata);
        }
    }
    return result;
}
