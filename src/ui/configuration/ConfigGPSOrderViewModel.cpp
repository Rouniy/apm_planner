#include "ConfigGPSOrderViewModel.h"

#include <QMetaType>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
constexpr double kMinimumNodeId = 0.0;
constexpr double kMaximumNodeId = 127.0;

bool variantsEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        // DroneCAN node IDs are integers in [0, 127] and therefore exactly
        // representable by every MAVLink parameter numeric type. Do not let a
        // close, but different, vehicle echo acknowledge a write.
        return leftValue == rightValue;
    }
    return left == right;
}

QString fallbackLabel(const QString &name)
{
    if (name == QLatin1String("GPS_CAN_NODEID1")) {
        return QStringLiteral("GPS Node ID 1");
    }
    if (name == QLatin1String("GPS_CAN_NODEID2")) {
        return QStringLiteral("GPS Node ID 2");
    }
    if (name == QLatin1String("GPS1_CAN_OVRIDE")) {
        return QStringLiteral("First DroneCAN GPS NODE ID");
    }
    if (name == QLatin1String("GPS2_CAN_OVRIDE")) {
        return QStringLiteral("Second DroneCAN GPS NODE ID");
    }
    return name;
}
} // namespace

ConfigGPSOrderViewModel::ConfigGPSOrderViewModel(QObject *parent)
    : QObject(parent)
{
    rebuildFields();
    rebuildRows();
}

QStringList ConfigGPSOrderViewModel::FieldNames()
{
    return {
        QStringLiteral("GPS_CAN_NODEID1"),
        QStringLiteral("GPS_CAN_NODEID2"),
        QStringLiteral("GPS1_CAN_OVRIDE"),
        QStringLiteral("GPS2_CAN_OVRIDE")
    };
}

int ConfigGPSOrderViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

QString ConfigGPSOrderViewModel::Title() const
{
    return tr("UAVCAN GPS Order");
}

QString ConfigGPSOrderViewModel::Intro() const
{
    return tr("Detected DroneCAN GPS nodes. Use Override to pin a node to "
              "GPS1 or GPS2.");
}

bool ConfigGPSOrderViewModel::Busy() const
{
    return m_refreshing || HasPendingWrites();
}

bool ConfigGPSOrderViewModel::CanEdit() const
{
    return m_connected && m_snapshotReady && !Busy();
}

void ConfigGPSOrderViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildFields();
}

void ConfigGPSOrderViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    cancelPendingForNewOwner();
    ++m_snapshotGeneration;
    m_refreshing = false;

    const QStringList names = FieldNames();
    const QSet<QString> relevantNames(names.constBegin(), names.constEnd());
    QSet<int> relevantComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (relevantNames.contains(normalizedName(parameter.name))) {
            relevantComponents.insert(parameter.componentId);
        }
    }
    if (relevantComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (relevantComponents.contains(1)) {
        m_componentId = 1;
    } else if (!relevantComponents.isEmpty()) {
        QList<int> ordered = relevantComponents.values();
        std::sort(ordered.begin(), ordered.end());
        m_componentId = ordered.first();
    } else {
        m_componentId = preferredComponent;
    }

    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId
            && relevantNames.contains(normalizedName(parameter.name))) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    m_snapshotReady = true;
    rebuildFields();
    rebuildRows();
    emit stateChanged();
}

void ConfigGPSOrderViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!m_connected) {
        cancelPendingForNewOwner();
        ++m_snapshotGeneration;
        m_snapshotReady = false;
        m_refreshing = false;
        rebuildFields();
        setStatus(tr("offline"));
    }
    emit stateChanged();
}

bool ConfigGPSOrderViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    ParamField *field = fieldForName(normalized);
    if (!CanEdit() || !field || field->readOnly) {
        return false;
    }

    bool ok = false;
    const double numeric = value.toDouble(&ok);
    const double integer = std::nearbyint(numeric);
    if (!ok || !std::isfinite(numeric)
        || std::abs(numeric - integer) > 1.0e-6
        || numeric < kMinimumNodeId || numeric > kMaximumNodeId) {
        return false;
    }
    const QVariant candidate = typedNodeValue(integer, field->value);
    if (variantsEqual(field->value, candidate)) {
        return false;
    }

    const QVariant previous = m_values.value(normalized, field->value);
    field->value = candidate;
    field->status = QStringLiteral("…");
    queueWrite(FieldWrite, normalized, candidate, previous);
    emit fieldChanged(normalized);
    return true;
}

bool ConfigGPSOrderViewModel::Override1(const GpsCanRow &row)
{
    return writeOverride(QStringLiteral("GPS1_CAN_OVRIDE"), row);
}

bool ConfigGPSOrderViewModel::Override2(const GpsCanRow &row)
{
    return writeOverride(QStringLiteral("GPS2_CAN_OVRIDE"), row);
}

bool ConfigGPSOrderViewModel::Refresh()
{
    if (!m_connected) {
        setStatus(tr("Not connected — cannot fetch params."));
        return false;
    }
    if (Busy()) {
        setStatus(tr("Wait for pending parameter writes to finish."));
        return false;
    }
    ++m_snapshotGeneration;
    m_refreshing = true;
    setStatus(tr("Refreshing parameters…"));
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigGPSOrderViewModel::refreshFailed(const QString &reason)
{
    if (!m_refreshing) {
        return;
    }
    m_refreshing = false;
    setStatus(reason.isEmpty()
                  ? tr("Refresh failed.")
                  : tr("Refresh failed: %1").arg(reason));
    emit stateChanged();
}

void ConfigGPSOrderViewModel::refreshCanceled()
{
    if (!m_refreshing) {
        return;
    }
    m_refreshing = false;
    setStatus(tr("Parameter refresh canceled."));
    emit stateChanged();
}

void ConfigGPSOrderViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (!m_connected || !m_snapshotReady
        || componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (!FieldNames().contains(normalized)
        || consumeStaleEcho(normalized, value)) {
        return;
    }

    const bool existed = m_values.contains(normalized);
    m_values.insert(normalized, value);
    if (!existed) {
        rebuildFields();
    } else if (ParamField *field = fieldForName(normalized)) {
        field->value = value;
        if (m_pending.kind == NoWrite || m_pending.name != normalized) {
            field->status.clear();
        }
        emit fieldChanged(normalized);
    }

    if (m_pending.kind == NoWrite || m_pending.name != normalized
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        rebuildRows();
        return;
    }

    const PendingWrite pending = m_pending;
    if (!variantsEqual(pending.expectedValue, value)) {
        failPending(tr("write mismatch"), true);
        return;
    }

    m_pending = PendingWrite();
    if (ParamField *field = fieldForName(normalized)) {
        field->value = value;
        field->status = QStringLiteral("✓");
        emit fieldChanged(normalized);
    }
    if (pending.kind == OverrideWrite) {
        // MP10 immediately reloads the full parameter set after pinning a
        // node. Keep actions disabled until that refresh completes or fails.
        m_refreshing = true;
    }
    rebuildRows(pending.kind != OverrideWrite);
    emit stateChanged();

    if (pending.kind == OverrideWrite) {
        setStatus(tr("%1 = %2").arg(normalized).arg(value.toInt()));
        emit refreshRequested(m_componentId);
    }
}

void ConfigGPSOrderViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId != m_componentId || m_pending.kind == NoWrite
        || normalizedName(name) != m_pending.name
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    failPending(reason.isEmpty() ? tr("write failed") : reason);
}

bool ConfigGPSOrderViewModel::writeOverride(
    const QString &parameter, const GpsCanRow &row)
{
    if (!m_connected) {
        setStatus(tr("offline"));
        return false;
    }
    if (!m_snapshotReady || Busy() || row.NodeID <= 0
        || row.NodeID > static_cast<int>(kMaximumNodeId)) {
        return false;
    }
    if (!m_values.contains(parameter)) {
        setStatus(tr("write failed"));
        return false;
    }

    const QVariant previous = m_values.value(parameter);
    const QVariant candidate = typedNodeValue(row.NodeID, previous);
    if (ParamField *field = fieldForName(parameter)) {
        field->value = candidate;
        field->status = QStringLiteral("…");
        emit fieldChanged(parameter);
    }
    setStatus(QString());
    queueWrite(OverrideWrite, parameter, candidate, previous);
    return true;
}

void ConfigGPSOrderViewModel::rebuildRows(bool updateAvailabilityStatus)
{
    QList<GpsCanRow> rows;
    if (!m_values.contains(QStringLiteral("GPS1_CAN_OVRIDE"))) {
        setStatus(tr("GPS1_CAN_OVRIDE not available — connect a "
                     "DroneCAN-capable autopilot."));
    } else {
        const auto nodeId = [this](const QString &name) {
            return static_cast<int>(std::nearbyint(
                m_values.value(name, 0).toDouble()));
        };
        const int id1 = nodeId(QStringLiteral("GPS_CAN_NODEID1"));
        const int id2 = nodeId(QStringLiteral("GPS_CAN_NODEID2"));
        const int override1 = nodeId(QStringLiteral("GPS1_CAN_OVRIDE"));
        const int override2 = nodeId(QStringLiteral("GPS2_CAN_OVRIDE"));

        if (override1 != 0) {
            rows.append({1, tr("GPS Override 1"), override1});
        }
        if (override2 != 0) {
            rows.append({2, tr("GPS Override 2"), override2});
        }
        if (id1 != 0 && id1 != override1 && id1 != override2) {
            rows.append({98, tr("GPS Detect 1"), id1});
        }
        if (id2 != 0 && id2 != override1 && id2 != override2) {
            rows.append({99, tr("GPS Detect 2"), id2});
        }
        if (updateAvailabilityStatus) {
            setStatus(rows.isEmpty()
                          ? tr("No CAN GPS nodes detected.") : QString());
        }
    }

    if (m_rows != rows) {
        m_rows = rows;
        emit rowsChanged();
    }
}

void ConfigGPSOrderViewModel::rebuildFields()
{
    m_fields.clear();
    for (const QString &name : FieldNames()) {
        m_fields.append(makeField(name));
    }
    emit structureChanged();
}

ParamField ConfigGPSOrderViewModel::makeField(const QString &name) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = name;
    field.value = m_values.value(name, 0);

    const ParameterMetaData metadata = m_catalog.value(name);
    field.label = metadata.title.isEmpty()
        ? fallbackLabel(name) : metadata.title;
    field.units = metadata.units;
    field.description = metadata.description;
    field.status = m_values.contains(name) ? QString() : tr("n/a");
    field.readOnly = !m_values.contains(name) || metadata.readOnly
        || name == QLatin1String("GPS_CAN_NODEID1")
        || name == QLatin1String("GPS_CAN_NODEID2");
    field.editorKind = ParamField::EditorKind::Numeric;
    field.minimum = metadata.hasRange
        ? metadata.minimum : kMinimumNodeId;
    field.maximum = metadata.hasRange
        ? metadata.maximum : kMaximumNodeId;
    field.increment = metadata.hasIncrement ? metadata.increment : 1.0;
    field.hasRange = true;
    field.enforceRange = true;
    return field;
}

ParamField *ConfigGPSOrderViewModel::fieldForName(const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

QString ConfigGPSOrderViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}

QVariant ConfigGPSOrderViewModel::typedNodeValue(
    double value, const QVariant &reference) const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::UInt:
        return static_cast<uint>(value);
    case QMetaType::LongLong:
        return static_cast<qlonglong>(value);
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(value);
    case QMetaType::Float:
        return static_cast<float>(value);
    case QMetaType::Double:
        return value;
    default:
        return static_cast<int>(value);
    }
}

void ConfigGPSOrderViewModel::queueWrite(
    PendingKind kind, const QString &name, const QVariant &value,
    const QVariant &previousValue)
{
    PendingWrite pending;
    pending.kind = kind;
    pending.name = normalizedName(name);
    consumeStaleEcho(pending.name, value);
    pending.expectedValue = value;
    pending.previousValue = previousValue;
    pending.writeGeneration = ++m_writeGeneration;
    pending.snapshotGeneration = m_snapshotGeneration;
    m_pending = pending;
    emit stateChanged();
    emit writeRequested(m_componentId, pending.name, value);

    const quint64 writeGeneration = pending.writeGeneration;
    const quint64 snapshotGeneration = pending.snapshotGeneration;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, writeGeneration, snapshotGeneration]() {
        if (m_pending.kind == NoWrite
            || m_pending.writeGeneration != writeGeneration
            || m_pending.snapshotGeneration != snapshotGeneration
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        failPending(tr("write timeout"));
    });
}

void ConfigGPSOrderViewModel::failPending(
    const QString &reason, bool authoritativeEcho)
{
    if (m_pending.kind == NoWrite) {
        return;
    }
    const PendingWrite pending = m_pending;
    rememberStaleEcho(pending.name, pending.expectedValue);
    m_pending = PendingWrite();

    ParamField *field = fieldForName(pending.name);
    if (field) {
        field->value = authoritativeEcho
            ? m_values.value(pending.name, pending.previousValue)
            : pending.previousValue;
        field->status = reason.isEmpty() ? tr("write failed") : reason;
        emit fieldChanged(pending.name);
    }
    if (!authoritativeEcho) {
        m_values.insert(pending.name, pending.previousValue);
    }
    rebuildRows(false);
    setStatus(reason.isEmpty() ? tr("write failed") : reason);
    emit stateChanged();
}

void ConfigGPSOrderViewModel::cancelPendingForNewOwner()
{
    ++m_writeGeneration;
    if (m_pending.kind != NoWrite) {
        rememberStaleEcho(m_pending.name, m_pending.expectedValue);
    }
    m_pending = PendingWrite();
}

void ConfigGPSOrderViewModel::rememberStaleEcho(
    const QString &name, const QVariant &value)
{
    if (name.isEmpty() || !value.isValid()) {
        return;
    }
    const QString normalized = normalizedName(name);
    m_staleEchoes[normalized].append(value);
    QTimer::singleShot(kWriteTimeoutMs * 2, this,
                       [this, normalized, value]() {
        auto iterator = m_staleEchoes.find(normalized);
        if (iterator == m_staleEchoes.end()) {
            return;
        }
        const auto candidate = std::find_if(
            iterator->begin(), iterator->end(),
            [&value](const QVariant &item) {
                return variantsEqual(item, value);
            });
        if (candidate != iterator->end()) {
            iterator->erase(candidate);
        }
        if (iterator->isEmpty()) {
            m_staleEchoes.erase(iterator);
        }
    });
}

bool ConfigGPSOrderViewModel::consumeStaleEcho(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    auto iterator = m_staleEchoes.find(normalized);
    if (iterator == m_staleEchoes.end()) {
        return false;
    }
    for (int index = 0; index < iterator->size(); ++index) {
        if (!variantsEqual(iterator->at(index), value)) {
            continue;
        }
        iterator->removeAt(index);
        if (iterator->isEmpty()) {
            m_staleEchoes.erase(iterator);
        }
        return true;
    }
    return false;
}

void ConfigGPSOrderViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}
