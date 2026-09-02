#include "PreflightChecklistModel.h"

#include <QColor>
#include <QSettings>

#include <cmath>

namespace {
const QString kSatisfiedColor = QStringLiteral("#34D399");
const QString kFailedColor = QStringLiteral("#FCA5A5");
const QString kNeutralColor = QStringLiteral("#F5F5F5");
const QString kUnavailableColor = QStringLiteral("#9AA0A6");

QString numberText(double value)
{
    return QString::number(value, 'f',
                           std::fabs(value - std::round(value)) < 0.005 ? 0 : 2);
}
}

PreflightChecklistModel::PreflightChecklistModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_items(defaultItems())
    , m_ownedSettings(new QSettings)
    , m_settings(m_ownedSettings.get())
{
    loadManualStates();
}

PreflightChecklistModel::PreflightChecklistModel(QSettings *settings,
                                                 QObject *parent)
    : QAbstractListModel(parent)
    , m_items(defaultItems())
    , m_settings(settings)
{
    if (!m_settings) {
        m_ownedSettings.reset(new QSettings);
        m_settings = m_ownedSettings.get();
    }
    loadManualStates();
}

PreflightChecklistModel::~PreflightChecklistModel() = default;

int PreflightChecklistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant PreflightChecklistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return QVariant();
    }

    const CheckItem &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case DescriptionRole:
        return item.description;
    case IdRole:
        return item.id;
    case ValueRole:
        return displayValue(item);
    case SatisfiedRole:
        return isSatisfied(item);
    case ManualRole:
        return item.condition == Condition::Manual;
    case AvailableRole:
        return isAvailable(item);
    case ForegroundRole:
        // MP10 deliberately keeps Mode, Altitude and physical/manual checks
        // neutral even though Altitude is still evaluated automatically.
        if (item.condition == Condition::Manual
            || item.id == QStringLiteral("altitude")) {
            return kNeutralColor;
        }
        if (!isAvailable(item)) {
            return kUnavailableColor;
        }
        return isSatisfied(item) ? kSatisfiedColor : kFailedColor;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> PreflightChecklistModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {DescriptionRole, "description"},
        {ValueRole, "value"},
        {SatisfiedRole, "satisfied"},
        {ManualRole, "manual"},
        {AvailableRole, "available"},
        {ForegroundRole, "foreground"}
    };
}

PreflightTelemetry PreflightChecklistModel::telemetry() const
{
    return m_telemetry;
}

void PreflightChecklistModel::setTelemetry(
    const PreflightTelemetry &telemetry)
{
    m_telemetry = telemetry;
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_items.size() - 1, 0),
                         {ValueRole, SatisfiedRole, AvailableRole,
                          ForegroundRole});
    }
}

bool PreflightChecklistModel::setManualState(int row, bool checked)
{
    if (row < 0 || row >= m_items.size()
        || m_items.at(row).condition != Condition::Manual) {
        return false;
    }
    CheckItem &item = m_items[row];
    if (item.manualState == checked) {
        return true;
    }
    item.manualState = checked;
    saveManualState(item);
    emit dataChanged(index(row, 0), index(row, 0), {SatisfiedRole});
    return true;
}

QString PreflightChecklistModel::settingsGroup()
{
    return QStringLiteral("FlightData/PreFlight/v1");
}

QVector<PreflightChecklistModel::CheckItem>
PreflightChecklistModel::defaultItems()
{
    // Keep the MP10 ordering and labels. Only fields already normalized by
    // FlightDataViewModel are evaluated automatically in the Qt port.
    return {
        {QStringLiteral("gps_fix"), tr("Verify GPS"),
         QStringLiteral("%1 >= 3"), Source::GpsFix,
         Condition::GreaterThanOrEqual, 3.0},
        {QStringLiteral("gps_satellites"), tr("GPS Sat Count"),
         QStringLiteral("%1 Sats"), Source::SatelliteCount,
         Condition::GreaterThanOrEqual, 5.0},
        {QStringLiteral("telemetry_signal"), tr("Telemetry Signal"),
         QStringLiteral("%1%"), Source::LinkQuality,
         Condition::GreaterThan, 95.0},
        {QStringLiteral("battery_level"), tr("Battery Level"),
         QStringLiteral("%1 V"), Source::BatteryVoltage,
         Condition::GreaterThan, 22.0},
        {QStringLiteral("mode"), tr("Mode"), QStringLiteral("%1"),
         Source::Mode, Condition::Manual, 0.0},
        {QStringLiteral("altitude"), tr("Check Altitude"),
         QStringLiteral("%1 < 5m"), Source::Altitude,
         Condition::LessThan, 5.0},
        {QStringLiteral("tail_wings"), tr("Tail and wings secured?"),
         QString(), Source::None, Condition::Manual, 0.0},
        {QStringLiteral("servos_input"),
         tr("All servos respond to input?"), QString(), Source::None,
         Condition::Manual, 0.0},
        {QStringLiteral("servos_attitude"),
         tr("All servos respond to pitch and roll?"), QString(),
         Source::None, Condition::Manual, 0.0},
        {QStringLiteral("center_of_gravity"),
         tr("Center of gravity at indicated point?"), QString(),
         Source::None, Condition::Manual, 0.0},
        {QStringLiteral("servo_linkages"),
         tr("Servo linkages are secure?"), QString(), Source::None,
         Condition::Manual, 0.0},
        {QStringLiteral("camera_ready"),
         tr("Camera is on and ready to fly?"), QString(), Source::None,
         Condition::Manual, 0.0}
    };
}

QVariant PreflightChecklistModel::sourceValue(const CheckItem &item) const
{
    switch (item.source) {
    case Source::GpsFix: return m_telemetry.gpsFixType;
    case Source::SatelliteCount: return m_telemetry.satelliteCount;
    case Source::LinkQuality: return m_telemetry.linkQuality;
    case Source::BatteryVoltage: return m_telemetry.batteryVoltage;
    case Source::Mode: return m_telemetry.mode;
    case Source::Altitude: return m_telemetry.altitude;
    case Source::None: return QVariant();
    }
    return QVariant();
}

QString PreflightChecklistModel::displayValue(const CheckItem &item) const
{
    if (item.source == Source::None) {
        return QString();
    }
    if (!isAvailable(item)) {
        return tr("Not connected");
    }
    const QVariant value = sourceValue(item);
    const QString text = item.source == Source::Mode
        ? value.toString() : numberText(value.toDouble());
    return item.valueTemplate.arg(text);
}

bool PreflightChecklistModel::isAvailable(const CheckItem &item) const
{
    if (item.source == Source::None) {
        return true;
    }
    if (!m_telemetry.connected) {
        return false;
    }
    if (item.source == Source::Mode) {
        return !m_telemetry.mode.trimmed().isEmpty()
            && m_telemetry.mode != QStringLiteral("UNKNOWN");
    }
    return std::isfinite(sourceValue(item).toDouble());
}

bool PreflightChecklistModel::isSatisfied(const CheckItem &item) const
{
    if (item.condition == Condition::Manual) {
        return item.manualState;
    }
    if (!isAvailable(item)) {
        return false;
    }
    const double value = sourceValue(item).toDouble();
    switch (item.condition) {
    case Condition::LessThan: return value < item.triggerValue;
    case Condition::GreaterThan: return value > item.triggerValue;
    case Condition::GreaterThanOrEqual: return value >= item.triggerValue;
    case Condition::Manual: return item.manualState;
    }
    return false;
}

void PreflightChecklistModel::loadManualStates()
{
    if (!m_settings) {
        return;
    }
    m_settings->beginGroup(settingsGroup());
    m_settings->beginGroup(QStringLiteral("manual"));
    for (CheckItem &item : m_items) {
        if (item.condition == Condition::Manual) {
            item.manualState = m_settings->value(item.id, false).toBool();
        }
    }
    m_settings->endGroup();
    m_settings->endGroup();
}

void PreflightChecklistModel::saveManualState(const CheckItem &item)
{
    if (!m_settings || item.condition != Condition::Manual) {
        return;
    }
    m_settings->beginGroup(settingsGroup());
    m_settings->beginGroup(QStringLiteral("manual"));
    m_settings->setValue(item.id, item.manualState);
    m_settings->endGroup();
    m_settings->endGroup();
    m_settings->sync();
}
