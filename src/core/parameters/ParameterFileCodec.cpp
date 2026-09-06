#include "ParameterFileCodec.h"

#include <QHash>
#include <QIODevice>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <cmath>

namespace {
QStringList excludedParameters()
{
    return {
        QStringLiteral("SYSID_SW_MREV"), QStringLiteral("WP_TOTAL"),
        QStringLiteral("CMD_TOTAL"), QStringLiteral("FENCE_TOTAL"),
        QStringLiteral("SYS_NUM_RESETS"), QStringLiteral("ARSPD_OFFSET"),
        QStringLiteral("GND_ABS_PRESS"), QStringLiteral("GND_TEMP"),
        QStringLiteral("BARO1_GND_PRESS"),
        QStringLiteral("BARO2_GND_PRESS"),
        QStringLiteral("BARO3_GND_PRESS"),
        QStringLiteral("BARO_GND_TEMP"), QStringLiteral("CMD_INDEX"),
        QStringLiteral("LOG_LASTFILE"), QStringLiteral("FORMAT_VERSION")
    };
}
} // namespace

bool ConfigRawParamsFileCodec::load(
    QIODevice *device, QMap<QString, double> *values,
    QString *error, int *errorLine)
{
    if (values) values->clear();
    QVector<Entry> ordered;
    if (!loadOrdered(device, values ? &ordered : nullptr, error, errorLine))
        return false;
    for (const Entry &entry : ordered)
        values->insert(entry.name, entry.value);
    return true;
}

bool ConfigRawParamsFileCodec::loadOrdered(
    QIODevice *device, QVector<Entry> *values,
    QString *error, int *errorLine)
{
    if (values) {
        values->clear();
    }
    if (error) {
        error->clear();
    }
    if (errorLine) {
        *errorLine = 0;
    }
    if (!device || !device->isReadable() || !values) {
        if (error) {
            *error = QObject::tr("The parameter file is not readable.");
        }
        return false;
    }

    static const QRegularExpression separator(QStringLiteral("[\\s,]+"));
    QTextStream stream(device);
    int lineNumber = 0;
    QHash<QString, int> positions;
    while (!stream.atEnd()) {
        const QString line = stream.readLine(4097);
        ++lineNumber;
        if (lineNumber > 100000 || line.size() > 4096) {
            if (error) {
                *error = QObject::tr("The parameter file exceeds safe limits.");
            }
            if (errorLine) {
                *errorLine = lineNumber;
            }
            values->clear();
            return false;
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = trimmed.split(separator, Qt::SkipEmptyParts);
        if (fields.size() < 2) {
            continue;
        }
        const QString name = fields.at(0).trimmed().toUpper();
        bool ok = false;
        const double value = QLocale::c().toDouble(fields.at(1), &ok);
        if (!ok || !std::isfinite(value)) {
            if (error) {
                *error = QObject::tr("Invalid value for parameter %1 on line %2.")
                             .arg(name).arg(lineNumber);
            }
            if (errorLine) {
                *errorLine = lineNumber;
            }
            values->clear();
            return false;
        }
        if (!name.isEmpty() && !isExcluded(name)) {
            // Dictionary replacement does not move its first insertion.
            const auto existing = positions.constFind(name);
            if (existing == positions.constEnd()) {
                positions.insert(name, values->size());
                values->append({name, value});
            } else {
                (*values)[existing.value()].value = value;
            }
        }
    }
    return true;
}

bool ConfigRawParamsFileCodec::save(
    QIODevice *device, const QMap<QString, QVariant> &values,
    QString *error)
{
    if (error) {
        error->clear();
    }
    if (!device || !device->isWritable()) {
        if (error) {
            *error = QObject::tr("The parameter file is not writable.");
        }
        return false;
    }
    QTextStream stream(device);
    for (auto iterator = values.constBegin(); iterator != values.constEnd();
         ++iterator) {
        bool ok = false;
        const double value = iterator.value().toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            if (error) {
                *error = QObject::tr("Parameter %1 has an invalid value.")
                             .arg(iterator.key());
            }
            return false;
        }
        stream << iterator.key() << QLatin1Char(',')
               << QLocale::c().toString(value, 'g', 17) << QLatin1Char('\n');
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok) {
        if (error) {
            *error = QObject::tr("Writing the parameter file failed.");
        }
        return false;
    }
    return true;
}

bool ConfigRawParamsFileCodec::isExcluded(const QString &name)
{
    static const QSet<QString> excluded = []() {
        QSet<QString> result;
        const QStringList names = excludedParameters();
        for (const QString &candidate : names) {
            result.insert(candidate);
        }
        return result;
    }();
    return excluded.contains(name.trimmed().toUpper());
}
