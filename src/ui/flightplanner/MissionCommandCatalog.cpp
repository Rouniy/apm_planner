#include "MissionCommandCatalog.h"

#include "QGCMAVLink.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
const QString kLabelsKey = QStringLiteral("PlannerExtraCommand");
const QString kIdsKey = QStringLiteral("PlannerExtraCommandIDs");
const QStringList kDefaultLabels{
    QStringLiteral("P1"), QStringLiteral("P2"),
    QStringLiteral("P3"), QStringLiteral("P4"),
    QStringLiteral("Lat"), QStringLiteral("Lon"),
    QStringLiteral("Alt")};

struct CommandEntry
{
    quint16 command;
    const char *name;
};

#include "MavCommandCatalog.inc"

QString nameKey(const QString &name)
{
    return name.trimmed().toCaseFolded();
}

const CommandEntry *knownById(quint16 id)
{
    const auto found = std::find_if(
        kCommands.cbegin(), kCommands.cend(),
        [id](const CommandEntry &entry) { return entry.command == id; });
    return found == kCommands.cend() ? nullptr : &*found;
}

const CommandEntry *knownByName(const QString &name)
{
    const QString candidate = name.trimmed();
    const auto found = std::find_if(
        kCommands.cbegin(), kCommands.cend(),
        [&candidate](const CommandEntry &entry) {
            return candidate.compare(QString::fromLatin1(entry.name),
                                     Qt::CaseInsensitive) == 0;
        });
    return found == kCommands.cend() ? nullptr : &*found;
}

QHash<QString, QStringList> readLabels(const QString &json,
                                       QHash<QString, QString> *names)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    QHash<QString, QStringList> labels;
    QHash<QString, QString> parsedNames;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isArray() && !it.value().isNull()) {
            return {};
        }
        const QString key = nameKey(it.key());
        if (labels.contains(key)) return {};
        QStringList values;
        const QJsonArray array = it.value().isArray()
            ? it.value().toArray() : QJsonArray();
        for (const QJsonValue &value : array) {
            if (!value.isString() && !value.isNull()) {
                return {};
            }
            values.append(value.isString() ? value.toString() : QString());
        }
        labels.insert(key, MissionCommandCatalog::NormalizeLabels(values));
        parsedNames.insert(key, it.key());
    }
    for (auto it = parsedNames.cbegin(); it != parsedNames.cend(); ++it)
        names->insert(it.key(), it.value());
    return labels;
}

QHash<QString, quint16> readIds(const QString &json,
                                QHash<QString, QString> *names)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    QHash<QString, quint16> ids;
    QHash<QString, QString> parsedNames;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isDouble()) {
            return {};
        }
        const double numeric = it.value().toDouble();
        if (!std::isfinite(numeric) || numeric < 0.0 || numeric > 65535.0
            || numeric != std::floor(numeric)) {
            return {};
        }
        const QString key = nameKey(it.key());
        if (ids.contains(key)) return {};
        ids.insert(key, static_cast<quint16>(numeric));
        parsedNames.insert(key, it.key());
    }
    for (auto it = parsedNames.cbegin(); it != parsedNames.cend(); ++it)
        names->insert(it.key(), it.value());
    return ids;
}

QStringList builtInLabels(quint16 id)
{
    const QString dash = QStringLiteral("—");
    switch (id) {
    case MAV_CMD_NAV_WAYPOINT:
        return {QStringLiteral("Delay"), dash, dash,
                QStringLiteral("Yaw")};
    case MAV_CMD_NAV_SPLINE_WAYPOINT:
        return {QStringLiteral("Delay"), dash, dash, dash};
    case MAV_CMD_NAV_LOITER_UNLIM:
        return {dash, dash, QStringLiteral("Radius"),
                QStringLiteral("Yaw")};
    case MAV_CMD_NAV_LOITER_TURNS:
        return {QStringLiteral("Turns"), dash,
                QStringLiteral("Radius"), dash};
    case MAV_CMD_NAV_LOITER_TIME:
        return {QStringLiteral("Time"), dash,
                QStringLiteral("Radius"), dash};
    case MAV_CMD_NAV_RETURN_TO_LAUNCH:
    case MAV_CMD_DO_SET_ROI:
        return {dash, dash, dash, dash};
    case MAV_CMD_NAV_LAND:
        return {QStringLiteral("Abort"), dash, dash,
                QStringLiteral("Yaw")};
    case MAV_CMD_NAV_TAKEOFF:
        return {dash, dash, dash, QStringLiteral("Yaw")};
    case MAV_CMD_DO_JUMP:
        return {QStringLiteral("WP#"), QStringLiteral("Repeat"),
                dash, dash};
    case MAV_CMD_DO_CHANGE_SPEED:
        return {QStringLiteral("Type"), QStringLiteral("Speed"),
                QStringLiteral("Throttle"), dash};
    case MAV_CMD_DO_DIGICAM_CONTROL:
        return {QStringLiteral("Shoot"), dash, dash, dash};
    case MAV_CMD_DO_SET_SERVO:
        return {QStringLiteral("Ch"), QStringLiteral("PWM"), dash, dash};
    case MAV_CMD_DO_SET_RELAY:
        return {QStringLiteral("Relay"), QStringLiteral("On/Off"),
                dash, dash};
    case MAV_CMD_CONDITION_DELAY:
        return {QStringLiteral("Time"), dash, dash, dash};
    default:
        return {};
    }
}
} // namespace

MissionCommandCatalog::MissionCommandCatalog(QObject *parent)
    : MissionCommandCatalog(nullptr, parent)
{
}

MissionCommandCatalog::MissionCommandCatalog(QSettings *settings,
                                               QObject *parent)
    : QObject(parent),
      m_settings(settings ? settings : new QSettings),
      m_ownsSettings(!settings)
{
    m_settings->setFallbacksEnabled(false);
    loadFromSettings();
}

MissionCommandCatalog::~MissionCommandCatalog()
{
    if (m_ownsSettings) {
        delete m_settings;
    }
}

MissionCommandCatalog *MissionCommandCatalog::instance()
{
    static MissionCommandCatalog *catalog =
        new MissionCommandCatalog(QCoreApplication::instance());
    return catalog;
}

QVector<MissionCommandDefinition>
MissionCommandCatalog::LoadDefinitions() const
{
    ensureFresh();
    return m_definitions;
}

QStringList MissionCommandCatalog::Names() const
{
    ensureFresh();
    struct NamedId { quint16 id; QString name; };
    QVector<NamedId> entries;
    entries.reserve(static_cast<int>(kCommands.size())
                    + m_definitions.size());
    QSet<QString> seen;
    for (const CommandEntry &entry : kCommands) {
        const QString name = QString::fromLatin1(entry.name);
        const QString key = normalizedName(name);
        if (!seen.contains(key)) {
            seen.insert(key);
            entries.append({entry.command, name});
        }
    }
    for (const MissionCommandDefinition &definition : m_definitions) {
        const QString key = normalizedName(definition.Name);
        if (!m_extraIds.contains(key)) {
            continue;
        }
        if (!seen.contains(key)) {
            seen.insert(key);
            entries.append({definition.Id, definition.Name});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const NamedId &left, const NamedId &right) {
        if (left.id != right.id) return left.id < right.id;
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    QStringList names;
    names.reserve(entries.size());
    for (const NamedId &entry : entries) names.append(entry.name);
    return names;
}

bool MissionCommandCatalog::TryGetId(const QString &name, quint16 *id) const
{
    ensureFresh();
    if (!id) return false;
    if (const CommandEntry *known = knownByName(name)) {
        *id = known->command;
        return true;
    }
    const QString key = normalizedName(name);
    const auto custom = m_extraIds.constFind(key);
    if (custom != m_extraIds.cend()) {
        *id = custom.value();
        return true;
    }
    return false;
}

QString MissionCommandCatalog::GetName(int id) const
{
    ensureFresh();
    if (id < 0 || id > 65535) return {};
    if (const CommandEntry *known = knownById(static_cast<quint16>(id))) {
        return QString::fromLatin1(known->name);
    }
    for (auto it = m_extraIds.cbegin(); it != m_extraIds.cend(); ++it) {
        if (it.value() != id) continue;
        for (const MissionCommandDefinition &definition : m_definitions) {
            if (normalizedName(definition.Name) == it.key()) {
                return definition.Name;
            }
        }
    }
    return {};
}

int MissionCommandCatalog::GetId(const QString &name) const
{
    quint16 id = 0;
    return TryGetId(name, &id) ? id : -1;
}

QStringList MissionCommandCatalog::GetLabels(int id) const
{
    ensureFresh();
    if (id < 0 || id > 65535) return {};
    QString key;
    if (const CommandEntry *known = knownById(static_cast<quint16>(id))) {
        key = normalizedName(QString::fromLatin1(known->name));
    } else {
        for (auto it = m_extraIds.cbegin(); it != m_extraIds.cend(); ++it) {
            if (it.value() == id) {
                key = it.key();
                break;
            }
        }
    }
    if (key.isEmpty()) return {};
    const auto labels = m_labels.constFind(key);
    return labels == m_labels.cend()
        ? QStringList() : NormalizeLabels(labels.value());
}

QStringList MissionCommandCatalog::EffectiveLabels(int id) const
{
    QStringList labels = GetLabels(id);
    if (labels.isEmpty()) {
        labels = builtInLabels(static_cast<quint16>(id));
        if (!labels.isEmpty()) {
            labels.append(kDefaultLabels.mid(labels.size()));
        } else {
            labels = kDefaultLabels;
        }
        return NormalizeLabels(labels);
    }
    labels = NormalizeLabels(labels);
    for (int index = 0; index < labels.size(); ++index) {
        if (labels.at(index).isEmpty()) {
            labels[index] = kDefaultLabels.at(index);
        }
    }
    return labels;
}

QVariantList MissionCommandCatalog::DefinitionMaps() const
{
    ensureFresh();
    QVariantList result;
    result.reserve(m_definitions.size());
    for (const MissionCommandDefinition &definition : m_definitions) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), definition.Id);
        row.insert(QStringLiteral("name"), definition.Name);
        row.insert(QStringLiteral("parameterLabels"),
                   NormalizeLabels(definition.ParameterLabels));
        result.append(row);
    }
    return result;
}

bool MissionCommandCatalog::Save(
    const QVector<MissionCommandDefinition> &definitions,
    QString *error)
{
    if (error) error->clear();
    QString validationError;
    if (!Validate(definitions, &validationError)) {
        if (error) *error = validationError;
        emit saveFailed(validationError);
        return false;
    }

    QJsonObject labelsObject;
    QJsonObject idsObject;
    for (const MissionCommandDefinition &definition : definitions) {
        const QString name = definition.Name.trimmed();
        QJsonArray labels;
        for (const QString &label : NormalizeLabels(
                 definition.ParameterLabels)) {
            labels.append(label);
        }
        labelsObject.insert(name, labels);
        if (!knownById(definition.Id)) {
            idsObject.insert(name, static_cast<int>(definition.Id));
        }
    }

    const QString labelsJson = QString::fromUtf8(
        QJsonDocument(labelsObject).toJson(QJsonDocument::Compact));
    const QString idsJson = QString::fromUtf8(
        QJsonDocument(idsObject).toJson(QJsonDocument::Compact));
    m_settings->setValue(kLabelsKey, labelsJson);
    m_settings->setValue(kIdsKey, idsJson);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        const QString settingsError = tr("The command catalog settings could not be saved.");
        if (error) *error = settingsError;
        emit saveFailed(settingsError);
        return false;
    }

    loadFromSettings();
    ++m_revision;
    emit catalogChanged();
    emit revisionChanged(m_revision);
    return true;
}

bool MissionCommandCatalog::SaveDefinitionMaps(
    const QVariantList &definitions)
{
    QVector<MissionCommandDefinition> rows;
    rows.reserve(definitions.size());
    for (const QVariant &value : definitions) {
        const QVariantMap map = value.toMap();
        bool ok = false;
        const double numericId = map.value(QStringLiteral("id")).toDouble(&ok);
        if (!ok || !std::isfinite(numericId) || numericId < 0.0
            || numericId > 65535.0 || numericId != std::floor(numericId)) {
            emit saveFailed(tr("Command ID must be an integer from 0 through 65535."));
            return false;
        }
        MissionCommandDefinition row;
        row.Id = static_cast<quint16>(numericId);
        row.Name = map.value(QStringLiteral("name")).toString();
        const QVariant labelValue = map.value(
            QStringLiteral("parameterLabels"));
        if (labelValue.userType() == QMetaType::QStringList) {
            row.ParameterLabels = labelValue.toStringList();
        } else {
            const QVariantList labels = labelValue.toList();
            row.ParameterLabels.reserve(labels.size());
            for (const QVariant &label : labels) {
                row.ParameterLabels.append(label.toString());
            }
        }
        rows.append(row);
    }
    return Save(rows);
}

void MissionCommandCatalog::Reload()
{
    loadFromSettings();
    ++m_revision;
    emit catalogChanged();
    emit revisionChanged(m_revision);
}

QStringList MissionCommandCatalog::NormalizeLabels(
    const QStringList &labels)
{
    QStringList normalized;
    normalized.reserve(7);
    for (int index = 0; index < 7; ++index) {
        normalized.append(index < labels.size() ? labels.at(index)
                                                : QString());
    }
    return normalized;
}

bool MissionCommandCatalog::Validate(
    const QVector<MissionCommandDefinition> &definitions,
    QString *error)
{
    if (error) error->clear();
    QSet<quint16> ids;
    QSet<QString> names;
    const QRegularExpression validName(
        QStringLiteral("^[\\p{L}\\p{N}_]+$"));
    for (const MissionCommandDefinition &definition : definitions) {
        const QString name = definition.Name.trimmed();
        if (ids.contains(definition.Id)) {
            if (error) {
                *error = QObject::tr("Command ID %1 is used more than once.")
                    .arg(definition.Id);
            }
            return false;
        }
        ids.insert(definition.Id);
        const QString key = normalizedName(name);
        if (names.contains(key)) {
            if (error) {
                *error = QObject::tr("Command name '%1' is used more than once.")
                    .arg(name);
            }
            return false;
        }
        names.insert(key);
        if (name.isEmpty()) {
            if (error) {
                *error = QObject::tr("Command ID %1 has no name.")
                    .arg(definition.Id);
            }
            return false;
        }
        if (!validName.match(definition.Name).hasMatch()) {
            if (error) {
                *error = QObject::tr(
                    "Command name '%1' may contain only letters, digits and underscores.")
                    .arg(name);
            }
            return false;
        }
        const QString canonical = CanonicalName(definition.Id);
        if (!canonical.isEmpty()
            && canonical.compare(name, Qt::CaseInsensitive) != 0) {
            if (error) {
                *error = QObject::tr(
                    "Known command ID %1 must retain its MAVLink name '%2'.")
                    .arg(definition.Id).arg(canonical);
            }
            return false;
        }
        quint16 knownId = 0;
        if (canonical.isEmpty() && IsKnownName(name, &knownId)) {
            if (error) {
                *error = QObject::tr(
                    "Custom command ID %1 cannot reuse known MAVLink name '%2'.")
                    .arg(definition.Id).arg(name);
            }
            return false;
        }
    }
    return true;
}

QString MissionCommandCatalog::CanonicalName(quint16 id)
{
    const CommandEntry *known = knownById(id);
    return known ? QString::fromLatin1(known->name) : QString();
}

bool MissionCommandCatalog::IsKnownName(const QString &name, quint16 *id)
{
    const CommandEntry *known = knownByName(name);
    if (!known) return false;
    if (id) *id = known->command;
    return true;
}

void MissionCommandCatalog::loadFromSettings()
{
    const QString labelsJson = m_settings->value(
        kLabelsKey, QStringLiteral("{}")).toString();
    const QString idsJson = m_settings->value(
        kIdsKey, QStringLiteral("{}")).toString();
    QHash<QString, QString> labelNames;
    const QHash<QString, QStringList> labels = readLabels(
        labelsJson, &labelNames);
    QHash<QString, QString> idNames;
    const QHash<QString, quint16> ids = readIds(
        idsJson, &idNames);
    m_labels = labels;
    m_extraIds = ids;

    QSet<QString> keys;
    for (auto it = labels.cbegin(); it != labels.cend(); ++it)
        keys.insert(it.key());
    for (auto it = ids.cbegin(); it != ids.cend(); ++it)
        keys.insert(it.key());

    QVector<MissionCommandDefinition> loaded;
    loaded.reserve(keys.size());
    for (const QString &key : keys) {
        MissionCommandDefinition definition;
        definition.Name = labelNames.value(key, idNames.value(key));
        const CommandEntry *known = knownByName(definition.Name);
        definition.Id = known ? known->command : ids.value(key, 65535);
        definition.ParameterLabels = NormalizeLabels(labels.value(key));
        loaded.append(definition);
    }
    std::sort(loaded.begin(), loaded.end(),
              [](const MissionCommandDefinition &left,
                 const MissionCommandDefinition &right) {
        if (left.Id != right.Id) return left.Id < right.Id;
        return left.Name.compare(right.Name, Qt::CaseInsensitive) < 0;
    });
    m_definitions = loaded;
    m_loadedLabelsJson = labelsJson;
    m_loadedIdsJson = idsJson;
}

void MissionCommandCatalog::ensureFresh() const
{
    const QString labelsJson = m_settings->value(
        kLabelsKey, QStringLiteral("{}")).toString();
    const QString idsJson = m_settings->value(
        kIdsKey, QStringLiteral("{}")).toString();
    if (labelsJson == m_loadedLabelsJson && idsJson == m_loadedIdsJson) {
        return;
    }
    const_cast<MissionCommandCatalog *>(this)->loadFromSettings();
}

QString MissionCommandCatalog::normalizedName(const QString &name)
{
    return name.trimmed().toCaseFolded();
}
