#include "ParameterMetaDataParser.h"

#include <QBuffer>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <QXmlStreamWriter>

namespace {
constexpr int kMaximumSourceCharacters = 8 * 1024 * 1024;
constexpr int kMaximumAnnotations = 250000;
constexpr int kMaximumParameters = 100000;
constexpr int kMaximumNestedSources = 4096;
constexpr int kMaximumPathsPerGroup = 256;

class BoundedPdefBuffer final : public QBuffer
{
public:
    explicit BoundedPdefBuffer(QByteArray *data) : QBuffer(data) {}
protected:
    qint64 writeData(const char *data, qint64 size) override
    {
        if (size < 0 || pos() > 16 * 1024 * 1024 - size) return -1;
        return QBuffer::writeData(data, size);
    }
};

struct Annotation
{
    int position = 0;
    QString key;
    QString frames;
    QString value;
};

QString canonicalVehicleName(const QString &name)
{
    const QString lowered = name.trimmed().toLower();
    if (lowered == QLatin1String("arducopter2")
        || lowered == QLatin1String("arducopter")
        || lowered == QLatin1String("copter")) {
        return QStringLiteral("Copter");
    }
    if (lowered == QLatin1String("ardurover")
        || lowered == QLatin1String("apmrover2")
        || lowered == QLatin1String("rover")) {
        return QStringLiteral("Rover");
    }
    if (lowered == QLatin1String("arduplane")
        || lowered == QLatin1String("plane")) {
        return QStringLiteral("Plane");
    }
    if (lowered == QLatin1String("ardusub")
        || lowered == QLatin1String("sub")) {
        return QStringLiteral("Sub");
    }
    if (lowered == QLatin1String("ardutracker")
        || lowered == QLatin1String("antennatracker")
        || lowered == QLatin1String("tracker")) {
        return QStringLiteral("Tracker");
    }
    if (lowered == QLatin1String("blimp")) {
        return QStringLiteral("Blimp");
    }
    return name.trimmed();
}

QString pdefVehicleName(const QString &name)
{
    const QString canonical = canonicalVehicleName(name);
    if (canonical == QLatin1String("Copter")) {
        return QStringLiteral("ArduCopter");
    }
    if (canonical == QLatin1String("Plane")) {
        return QStringLiteral("ArduPlane");
    }
    if (canonical == QLatin1String("Sub")) {
        return QStringLiteral("ArduSub");
    }
    if (canonical == QLatin1String("Tracker")) {
        return QStringLiteral("AntennaTracker");
    }
    return canonical;
}

bool appliesToVehicle(const QString &frames, const QString &vehicleName)
{
    if (frames.trimmed().isEmpty()) {
        return true;
    }
    const QString expected = canonicalVehicleName(vehicleName);
    const QStringList choices = frames.split(QLatin1Char(','),
                                              Qt::SkipEmptyParts);
    for (const QString &choice : choices) {
        if (canonicalVehicleName(choice).compare(
                expected, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString normalizedParameterName(QString name)
{
    name = name.trimmed();
    name.replace(QLatin1Char(' '), QLatin1Char('_'));
    return name;
}

QString normalizedPrefix(QString prefix)
{
    prefix = prefix.trimmed();
    prefix.replace(QLatin1Char('('), QLatin1Char('_'));
    prefix.replace(QLatin1Char(')'), QLatin1Char('_'));
    prefix.replace(QLatin1Char(' '), QLatin1Char('_'));
    return prefix;
}

bool keyEquals(const QString &left, const char *right)
{
    return left.compare(QLatin1String(right), Qt::CaseInsensitive) == 0;
}

ParameterMetaDataParser::Fields fieldsForBlock(
    const QVector<Annotation> &annotations, int first, int end,
    const QString &vehicleName)
{
    ParameterMetaDataParser::Fields fields;
    QMap<QString, int> priorities;
    for (int index = first; index < end; ++index) {
        const Annotation &annotation = annotations.at(index);
        const bool conditional = !annotation.frames.trimmed().isEmpty();
        if (conditional
            && !appliesToVehicle(annotation.frames, vehicleName)) {
            continue;
        }
        const int priority = conditional ? 2 : 1;
        const int previousPriority = priorities.value(annotation.key, 0);
        if (previousPriority > priority
            || (previousPriority == priority && !conditional)) {
            continue;
        }
        priorities.insert(annotation.key, priority);
        fields.insert(annotation.key, annotation.value.trimmed());
    }
    return fields;
}

QString fieldValue(const ParameterMetaDataParser::Fields &fields,
                   const char *name)
{
    for (auto field = fields.constBegin(); field != fields.constEnd();
         ++field) {
        if (keyEquals(field.key(), name)) {
            return field.value();
        }
    }
    return QString();
}

void removeField(ParameterMetaDataParser::Fields *fields, const char *name)
{
    for (auto field = fields->begin(); field != fields->end();) {
        if (keyEquals(field.key(), name)) {
            field = fields->erase(field);
        } else {
            ++field;
        }
    }
}

void writeValues(QXmlStreamWriter *xml, const QString &valuesText)
{
    const QStringList values = valuesText.split(QLatin1Char(','),
                                                 Qt::SkipEmptyParts);
    xml->writeStartElement(QStringLiteral("values"));
    for (const QString &rawValue : values) {
        const QString value = rawValue.trimmed();
        const int separator = value.indexOf(QLatin1Char(':'));
        const QString code = (separator < 0 ? value : value.left(separator))
                                 .trimmed();
        const QString label = (separator < 0 ? value
                                             : value.mid(separator + 1))
                                  .trimmed();
        if (code.isEmpty()) {
            continue;
        }
        xml->writeStartElement(QStringLiteral("value"));
        xml->writeAttribute(QStringLiteral("code"), code);
        xml->writeCharacters(label);
        xml->writeEndElement();
    }
    xml->writeEndElement();
}
}

ParameterMetaDataParser::ParsedFile ParameterMetaDataParser::Parse(
    const QString &source, const QString &vehicleName)
{
    ParsedFile result;
    if (source.size() > kMaximumSourceCharacters) {
        result.error = QStringLiteral(
            "Parameter metadata source exceeds 8 Mi characters");
        return result;
    }

    static const QRegularExpression annotationExpression(QStringLiteral(
        "@([A-Za-z_][A-Za-z0-9_]*)(?:\\{([^}:]+)\\})?[ \\t]*:[ \\t]*([^\\r\\n]*)"));
    QVector<Annotation> annotations;
    QRegularExpressionMatchIterator matches =
        annotationExpression.globalMatch(source);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (annotations.size() >= kMaximumAnnotations) {
            result.error = QStringLiteral(
                "Parameter metadata contains too many annotations");
            return result;
        }
        annotations.append({match.capturedStart(), match.captured(1),
                            match.captured(2), match.captured(3)});
    }

    for (int index = 0; index < annotations.size(); ++index) {
        const Annotation &marker = annotations.at(index);
        if (!keyEquals(marker.key, "Param")) {
            continue;
        }
        int end = index + 1;
        while (end < annotations.size()
               && !keyEquals(annotations.at(end).key, "Param")
               && !keyEquals(annotations.at(end).key, "Group")) {
            ++end;
        }
        if (!appliesToVehicle(marker.frames, vehicleName)) {
            continue;
        }
        const QString name = normalizedParameterName(marker.value);
        if (name.isEmpty() || result.parameters.contains(name)) {
            continue;
        }
        if (result.parameters.size() >= kMaximumParameters) {
            result.parameters.clear();
            result.groups.clear();
            result.nestedSources.clear();
            result.error = QStringLiteral(
                "Parameter metadata contains too many parameters");
            return result;
        }
        result.parameters.insert(
            name, fieldsForBlock(annotations, index + 1, end, vehicleName));
    }

    QSet<QString> groupPrefixes;
    for (int index = 0; index < annotations.size(); ++index) {
        const Annotation &marker = annotations.at(index);
        if (!keyEquals(marker.key, "Group")) {
            continue;
        }
        int end = index + 1;
        while (end < annotations.size()
               && !keyEquals(annotations.at(end).key, "Group")
               && !keyEquals(annotations.at(end).key, "Param")) {
            ++end;
        }
        if (!appliesToVehicle(marker.frames, vehicleName)) {
            continue;
        }
        const QString prefix = normalizedPrefix(marker.value);
        // Anonymous groups intentionally import unprefixed parameters. Modern
        // ArduPilot has multiple such groups in one file (self + AP_Vehicle).
        if (!prefix.isEmpty() && groupPrefixes.contains(prefix)) {
            continue;
        }
        const Fields fields = fieldsForBlock(
            annotations, index + 1, end, vehicleName);
        const QString pathText = fieldValue(fields, "Path");
        if (pathText.isEmpty()) {
            continue;
        }
        Group group;
        group.prefix = prefix;
        const QStringList paths = pathText.split(QLatin1Char(','),
                                                 Qt::KeepEmptyParts);
        for (const QString &path : paths) {
            const QString trimmed = path.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            if (group.paths.size() >= kMaximumPathsPerGroup) {
                result.parameters.clear();
                result.groups.clear();
                result.nestedSources.clear();
                result.error = QStringLiteral(
                    "Parameter metadata group contains too many paths");
                return result;
            }
            group.paths.append(trimmed);
        }
        if (!group.paths.isEmpty()) {
            groupPrefixes.insert(prefix);
            result.groups.append(group);
        }
    }

    static const QRegularExpression nestedExpression(QStringLiteral(
        "\\bAP_NESTEDGROUPINFO\\s*\\(\\s*([^,()\\s]+)\\s*,"));
    QSet<QString> nestedSeen;
    QRegularExpressionMatchIterator nestedMatches =
        nestedExpression.globalMatch(source);
    while (nestedMatches.hasNext()) {
        const QString nested = nestedMatches.next().captured(1).trimmed();
        if (nested.isEmpty() || nestedSeen.contains(nested)) {
            continue;
        }
        if (result.nestedSources.size() >= kMaximumNestedSources) {
            result.parameters.clear();
            result.groups.clear();
            result.nestedSources.clear();
            result.error = QStringLiteral(
                "Parameter metadata contains too many nested sources");
            return result;
        }
        nestedSeen.insert(nested);
        result.nestedSources.append(nested);
    }

    return result;
}

QByteArray ParameterMetaDataParser::ToPdef(
    const QMap<QString, Parameters> &vehicles)
{
    QMap<QString, Parameters> canonicalVehicles;
    for (auto vehicle = vehicles.constBegin(); vehicle != vehicles.constEnd();
         ++vehicle) {
        const QString canonical = pdefVehicleName(vehicle.key());
        if (canonical.isEmpty()) {
            continue;
        }
        Parameters &target = canonicalVehicles[canonical];
        for (auto parameter = vehicle.value().constBegin();
             parameter != vehicle.value().constEnd(); ++parameter) {
            if (!parameter.key().trimmed().isEmpty()
                && !target.contains(parameter.key())) {
                target.insert(parameter.key(), parameter.value());
            }
        }
    }

    QByteArray output;
    BoundedPdefBuffer buffer(&output);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("paramfile"));
    xml.writeStartElement(QStringLiteral("vehicles"));
    for (auto vehicle = canonicalVehicles.constBegin();
         vehicle != canonicalVehicles.constEnd(); ++vehicle) {
        xml.writeStartElement(QStringLiteral("parameters"));
        xml.writeAttribute(QStringLiteral("name"), vehicle.key());
        for (auto parameter = vehicle.value().constBegin();
             parameter != vehicle.value().constEnd(); ++parameter) {
            Fields fields = parameter.value();
            xml.writeStartElement(QStringLiteral("param"));
            xml.writeAttribute(QStringLiteral("name"),
                               vehicle.key() + QLatin1Char(':')
                                   + parameter.key());

            const QString displayName = fieldValue(fields, "DisplayName");
            const QString description = fieldValue(fields, "Description");
            const QString user = fieldValue(fields, "User");
            const QString values = fieldValue(fields, "Values");
            const auto hasField = [&fields](const char *key) {
                for (auto field = fields.cbegin(); field != fields.cend(); ++field)
                    if (keyEquals(field.key(), key)) return true;
                return false;
            };
            const bool hasValues = hasField("Values");
            if (hasField("DisplayName")) {
                xml.writeAttribute(QStringLiteral("humanName"), displayName);
            }
            if (hasField("Description")) {
                xml.writeAttribute(QStringLiteral("documentation"),
                                   description);
            }
            if (hasField("User")) {
                xml.writeAttribute(QStringLiteral("user"), user);
            }
            removeField(&fields, "DisplayName");
            removeField(&fields, "Description");
            removeField(&fields, "User");
            removeField(&fields, "Values");

            if (hasValues) writeValues(&xml, values);
            for (auto field = fields.constBegin(); field != fields.constEnd();
                 ++field) {
                xml.writeStartElement(QStringLiteral("field"));
                xml.writeAttribute(QStringLiteral("name"), field.key());
                xml.writeCharacters(field.value());
                xml.writeEndElement();
            }
            xml.writeEndElement();
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return xml.hasError() ? QByteArray() : output;
}
