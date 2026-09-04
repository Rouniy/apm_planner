#include "ConfigCompassViewModel.h"

#include "ConfigHWIDViewModel.h"

#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtNumeric>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
constexpr int kRebootAckTimeoutMs = 10000;
constexpr double kPi = 3.14159265358979323846;

QVariantMap change(const QString &name, const QVariant &value)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("value"), value}};
}

bool isCompassParameter(const QString &name)
{
    return name.startsWith(QLatin1String("COMPASS"));
}

int physicalSlotForName(const QString &name)
{
    static const QRegularExpression suffixExpression(
        QStringLiteral("^COMPASS_DEV_ID([0-9]*)$"));
    static const QRegularExpression prefixExpression(
        QStringLiteral("^COMPASS([0-9]+)_DEV_ID$"));

    QRegularExpressionMatch match = suffixExpression.match(name);
    if (match.hasMatch()) {
        const QString suffix = match.captured(1);
        return suffix.isEmpty() ? 1 : suffix.toInt();
    }
    match = prefixExpression.match(name);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString printableNumber(const QVariant &value)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || qIsNaN(number) || qIsInf(number)) {
        return value.toString();
    }
    const qlonglong integer = static_cast<qlonglong>(number);
    if (std::abs(number - static_cast<double>(integer)) <= 1.0e-9) {
        return QString::number(integer);
    }
    return QString::number(number, 'g', 12);
}

bool optionContains(const QList<ParamOption> &options, const QVariant &value)
{
    for (const ParamOption &option : options) {
        bool wantedOk = false;
        bool candidateOk = false;
        const double wanted = value.toDouble(&wantedOk);
        const double candidate = option.value.toDouble(&candidateOk);
        if (wantedOk && candidateOk
            && std::abs(wanted - candidate) <= 1.0e-9) {
            return true;
        }
        if (!wantedOk && !candidateOk && option.value == value) {
            return true;
        }
    }
    return false;
}

QStringList useAliases(int slot)
{
    switch (slot) {
    case 1:
        return {QStringLiteral("COMPASS_USE"),
                QStringLiteral("COMPASS1_USE")};
    case 2:
        return {QStringLiteral("COMPASS_USE2"),
                QStringLiteral("COMPASS2_USE")};
    case 3:
        return {QStringLiteral("COMPASS_USE3"),
                QStringLiteral("COMPASS3_USE")};
    default:
        return {};
    }
}

QStringList devIdAliases(int slot)
{
    switch (slot) {
    case 1:
        return {QStringLiteral("COMPASS_DEV_ID"),
                QStringLiteral("COMPASS1_DEV_ID")};
    case 2:
        return {QStringLiteral("COMPASS_DEV_ID2"),
                QStringLiteral("COMPASS2_DEV_ID")};
    case 3:
        return {QStringLiteral("COMPASS_DEV_ID3"),
                QStringLiteral("COMPASS3_DEV_ID")};
    default:
        return {QStringLiteral("COMPASS_DEV_ID%1").arg(slot),
                QStringLiteral("COMPASS%1_DEV_ID").arg(slot)};
    }
}

QStringList externalAliases(int slot)
{
    switch (slot) {
    case 1:
        return {QStringLiteral("COMPASS_EXTERNAL"),
                QStringLiteral("COMPASS1_EXTERN")};
    case 2:
        return {QStringLiteral("COMPASS_EXTERN2"),
                QStringLiteral("COMPASS2_EXTERN")};
    case 3:
        return {QStringLiteral("COMPASS_EXTERN3"),
                QStringLiteral("COMPASS3_EXTERN")};
    default:
        return {};
    }
}

QStringList orientationAliases(int slot)
{
    switch (slot) {
    case 1:
        return {QStringLiteral("COMPASS_ORIENT"),
                QStringLiteral("COMPASS1_ORIENT")};
    case 2:
        return {QStringLiteral("COMPASS_ORIENT2"),
                QStringLiteral("COMPASS2_ORIENT")};
    case 3:
        return {QStringLiteral("COMPASS_ORIENT3"),
                QStringLiteral("COMPASS3_ORIENT")};
    default:
        return {};
    }
}

QString canonicalOrientationName(int slot)
{
    switch (slot) {
    case 1: return QStringLiteral("COMPASS_ORIENT");
    case 2: return QStringLiteral("COMPASS_ORIENT2");
    case 3: return QStringLiteral("COMPASS_ORIENT3");
    default: return {};
    }
}
} // namespace

ConfigCompassViewModel::ConfigCompassViewModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_status = tr("Waiting for a complete parameter snapshot.");
    rebuildFields();
}

int ConfigCompassViewModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ConfigCompassViewModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConfigCompassViewModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
        || index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }

    const CompassPriorityRow &row = m_rows.at(index.row());
    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case PriorityColumn:
        case DevIDColumn:
        case BusColumn:
        case AddressColumn:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        case MissingColumn:
        case ExternalColumn:
        case UpColumn:
        case DownColumn:
            return QVariant(Qt::AlignCenter);
        default:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    if (role == Qt::CheckStateRole) {
        if (index.column() == MissingColumn) {
            return row.missing ? Qt::Checked : Qt::Unchecked;
        }
        if (index.column() == ExternalColumn) {
            return row.external ? Qt::Checked : Qt::Unchecked;
        }
        return {};
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }

    const bool display = role == Qt::DisplayRole;
    switch (index.column()) {
    case PriorityColumn:
        return display ? QVariant(QString::number(row.priority))
                       : QVariant(row.priority);
    case DevIDColumn:
        return display ? QVariant(QString::number(row.devId))
                       : QVariant(row.devId);
    case BusTypeColumn:
        return row.busType;
    case BusColumn:
        return display ? QVariant(QString::number(row.bus))
                       : QVariant(row.bus);
    case AddressColumn:
        return display ? QVariant(QString::number(row.address))
                       : QVariant(row.address);
    case DevTypeColumn:
        return row.devType;
    case MissingColumn:
        return display ? QVariant(row.missing ? tr("Yes") : tr("No"))
                       : QVariant(row.missing);
    case ExternalColumn:
        return display ? QVariant(row.external ? tr("Yes") : tr("No"))
                       : QVariant(row.external);
    case OrientationColumn:
        return row.orientation;
    case UpColumn:
        return display ? QVariant(QStringLiteral("▲")) : QVariant(index.row());
    case DownColumn:
        return display ? QVariant(QStringLiteral("▼")) : QVariant(index.row());
    default:
        return {};
    }
}

QVariant ConfigCompassViewModel::headerData(
    int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case PriorityColumn: return QStringLiteral("Priority");
    case DevIDColumn: return QStringLiteral("DevID");
    case BusTypeColumn: return QStringLiteral("BusType");
    case BusColumn: return QStringLiteral("Bus");
    case AddressColumn: return QStringLiteral("Address");
    case DevTypeColumn: return QStringLiteral("DevType");
    case MissingColumn: return QStringLiteral("Missing");
    case ExternalColumn: return QStringLiteral("External");
    case OrientationColumn: return QStringLiteral("Orientation");
    case UpColumn: return QStringLiteral("Up");
    case DownColumn: return QStringLiteral("Down");
    default: return {};
    }
}

Qt::ItemFlags ConfigCompassViewModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
                           : Qt::NoItemFlags;
}

QStringList ConfigCompassViewModel::AdvancedFieldNames()
{
    // Exact MP10 ConfigCompass advanced field order.
    return {QStringLiteral("COMPASS_EXTERNAL"),
            QStringLiteral("COMPASS_EXTERN2"),
            QStringLiteral("COMPASS_EXTERN3"),
            QStringLiteral("COMPASS_ORIENT"),
            QStringLiteral("COMPASS_ORIENT2"),
            QStringLiteral("COMPASS_ORIENT3"),
            QStringLiteral("COMPASS_PRIMARY"),
            QStringLiteral("COMPASS_AUTODEC"),
            QStringLiteral("COMPASS_CAL_FIT")};
}

int ConfigCompassViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

int ConfigCompassViewModel::RebootAckTimeoutMs()
{
    return kRebootAckTimeoutMs;
}

void ConfigCompassViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceRanges)
{
    m_catalog = catalog;
    m_enforceRanges = enforceRanges;
    if (m_snapshotReady) {
        rebuildPresentation();
    } else {
        rebuildFields();
    }
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
}

void ConfigCompassViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot)
{
    QList<int> capableComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (normalizedName(parameter.name)
                == QLatin1String("COMPASS_PRIO1_ID")
            && !capableComponents.contains(parameter.componentId)) {
            capableComponents.append(parameter.componentId);
        }
    }
    std::sort(capableComponents.begin(), capableComponents.end());
    if (capableComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (capableComponents.contains(1)) {
        m_componentId = 1;
    } else if (!capableComponents.isEmpty()) {
        m_componentId = capableComponents.constFirst();
    } else {
        m_componentId = preferredComponent;
    }

    const bool submittedWriteWasSuperseded =
        m_pending.active && m_pending.batchId != 0;
    const bool submittedRebootWasSuperseded =
        m_rebootPending && m_rebootSubmitted;
    ++m_snapshotGeneration;
    m_pending = {};
    m_rebootPending = false;
    m_rebootSubmitted = false;
    m_rebootRequestId = 0;
    m_rebootSnapshotGeneration = 0;
    if (submittedRebootWasSuperseded) {
        m_rebootOutcomeUncertain = true;
    }
    m_snapshotComplete = completeSnapshot;
    m_snapshotReady = false;
    m_values.clear();

    if (!completeSnapshot) {
        if (submittedWriteWasSuperseded) {
            m_reconciliationRequired = true;
        }
        clearPresentation();
        m_status = m_reconciliationRequired
            ? tr("Compass state remains uncertain; a complete parameter "
                 "snapshot is required.")
            : tr("Waiting for a complete parameter snapshot.");
        emit rowsChanged(0);
        emit fieldsChanged();
        emit stateChanged();
        return;
    }

    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        const QString name = normalizedName(parameter.name);
        if (parameter.componentId == m_componentId
            && isCompassParameter(name)) {
            m_values.insert(name, parameter.value);
        }
    }
    m_snapshotReady =
        m_values.contains(QStringLiteral("COMPASS_PRIO1_ID"));
    // Only a complete snapshot is authoritative enough to reconcile.
    m_reconciliationRequired = false;
    rebuildPresentation();

    if (!m_snapshotReady) {
        m_status = tr("COMPASS_PRIO1_ID is not available on this vehicle.");
    } else if (!m_connected) {
        m_status = tr("offline");
    } else if (m_armed) {
        m_status = tr("Vehicle is armed; compass changes are disabled.");
    } else if (m_rebootOutcomeUncertain) {
        m_status = tr("The previous reboot outcome is uncertain. Power-cycle "
                      "or reconnect the vehicle before retrying.");
    } else {
        m_status.clear();
    }
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigCompassViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    bool rebootAcknowledgementLost = false;
    if (!connected) {
        if (m_pending.active) {
            if (m_pending.batchId != 0) {
                finishUncertain(
                    tr("Compass state is uncertain after connection loss; "
                       "reconnect and obtain a complete parameter snapshot."),
                    true);
            } else {
                finishFailure(tr("offline"));
            }
        }
        if (m_rebootPending) {
            rebootAcknowledgementLost = m_rebootSubmitted;
            if (m_rebootSubmitted) {
                m_rebootOutcomeUncertain = true;
            }
            m_rebootPending = false;
            m_rebootSubmitted = false;
            m_rebootRequestId = 0;
            m_rebootSnapshotGeneration = 0;
        }
        if (rebootAcknowledgementLost) {
            m_status = tr("Connection was lost before the reboot "
                          "acknowledgement; reboot is still required.");
        } else if (!m_reconciliationRequired) {
            m_status = tr("offline");
        }
    } else if (m_reconciliationRequired) {
        m_status = tr("Compass state is uncertain; obtain a complete "
                      "parameter snapshot before making another change.");
    } else if (!m_snapshotComplete) {
        m_status = tr("Waiting for a complete parameter snapshot.");
    } else if (!m_snapshotReady) {
        m_status = tr("COMPASS_PRIO1_ID is not available on this vehicle.");
    } else if (m_armed) {
        m_status = tr("Vehicle is armed; compass changes are disabled.");
    } else if (m_rebootOutcomeUncertain) {
        m_status = tr("The previous reboot outcome is uncertain. Power-cycle "
                      "or reconnect the vehicle before retrying.");
    } else if (!Busy()) {
        m_status.clear();
    }
    emit stateChanged();
}

void ConfigCompassViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    if (armed && !Busy()) {
        m_status = tr("Vehicle is armed; compass changes are disabled.");
    } else if (!armed && !Busy()) {
        if (!m_connected) {
            m_status = tr("offline");
        } else if (m_reconciliationRequired) {
            m_status = tr("Compass state is uncertain; obtain a complete "
                          "parameter snapshot before making another change.");
        } else if (!m_snapshotComplete) {
            m_status = tr("Waiting for a complete parameter snapshot.");
        } else if (!m_snapshotReady) {
            m_status =
                tr("COMPASS_PRIO1_ID is not available on this vehicle.");
        } else if (m_rebootOutcomeUncertain) {
            m_status = tr("The previous reboot outcome is uncertain. "
                          "Power-cycle or reconnect the vehicle before "
                          "retrying.");
        } else {
            m_status.clear();
        }
    }
    emit stateChanged();
}

bool ConfigCompassViewModel::UseCompass(int slot) const
{
    const QString name = resolvedUseName(slot);
    return !name.isEmpty() && m_values.value(name).toInt() != 0;
}

bool ConfigCompassViewModel::HasUseCompass(int slot) const
{
    return !resolvedUseName(slot).isEmpty();
}

bool ConfigCompassViewModel::LearnOffsets() const
{
    return m_values.value(QStringLiteral("COMPASS_LEARN")).toInt() != 0;
}

bool ConfigCompassViewModel::HasLearn() const
{
    return m_values.contains(QStringLiteral("COMPASS_LEARN"));
}

double ConfigCompassViewModel::DeclinationDegrees() const
{
    bool ok = false;
    const double radians =
        m_values.value(QStringLiteral("COMPASS_DEC")).toDouble(&ok);
    return ok && qIsFinite(radians)
        ? radians * 180.0 / kPi : 0.0;
}

bool ConfigCompassViewModel::HasDeclination() const
{
    return m_values.contains(QStringLiteral("COMPASS_DEC"));
}

bool ConfigCompassViewModel::moveUp(int row)
{
    if (row <= 0 || row >= m_rows.size()) {
        return false;
    }
    QVector<CompassPriorityRow> candidate = m_rows;
    candidate.swapItemsAt(row, row - 1);
    const QVariantList changes = priorityChanges(candidate);
    if (changes.isEmpty()) {
        m_status = tr("All three COMPASS_PRIO parameters are required to "
                      "edit compass priority safely.");
        emit stateChanged();
        return false;
    }
    return beginWrite(changes, WriteOperation::Priority);
}

bool ConfigCompassViewModel::moveDown(int row)
{
    if (row < 0 || row + 1 >= m_rows.size()) {
        return false;
    }
    QVector<CompassPriorityRow> candidate = m_rows;
    candidate.swapItemsAt(row, row + 1);
    const QVariantList changes = priorityChanges(candidate);
    if (changes.isEmpty()) {
        m_status = tr("All three COMPASS_PRIO parameters are required to "
                      "edit compass priority safely.");
        emit stateChanged();
        return false;
    }
    return beginWrite(changes, WriteOperation::Priority);
}

bool ConfigCompassViewModel::removeMissing()
{
    QVector<CompassPriorityRow> candidate;
    for (const CompassPriorityRow &row : m_rows) {
        if (!row.missing) {
            candidate.append(row);
        }
    }
    if (candidate.size() == m_rows.size()) {
        return false;
    }
    const QVariantList changes = priorityChanges(candidate);
    if (changes.isEmpty()) {
        m_status = tr("All three COMPASS_PRIO parameters are required to "
                      "edit compass priority safely.");
        emit stateChanged();
        return false;
    }
    return beginWrite(changes, WriteOperation::Priority);
}

bool ConfigCompassViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (!AdvancedFieldNames().contains(normalized)) {
        return false;
    }
    const ParamField field = fieldForName(normalized);
    if (field.name.isEmpty() || field.readOnly || !field.value.isValid()) {
        m_status = tr("%1 is not writable on this vehicle.").arg(normalized);
        emit stateChanged();
        return false;
    }
    if (field.editorKind == ParamField::EditorKind::Combo
        && !optionContains(field.options, value)) {
        m_status = tr("%1 does not accept that value.").arg(normalized);
        emit stateChanged();
        return false;
    }
    if (field.editorKind == ParamField::EditorKind::Numeric) {
        bool ok = false;
        const double numeric = value.toDouble(&ok);
        if (!ok || !qIsFinite(numeric)) {
            m_status = tr("%1 requires a numeric value.").arg(normalized);
            emit stateChanged();
            return false;
        }
        if (field.hasRange && field.enforceRange
            && (numeric < field.minimum || numeric > field.maximum)) {
            m_status = tr("%1 is outside the supported range.")
                           .arg(normalized);
            emit stateChanged();
            return false;
        }
    }
    if (valuesEqual(field.value, value)) {
        return false;
    }
    return beginWrite({change(normalized, value)}, WriteOperation::Field);
}

bool ConfigCompassViewModel::setUseCompass(int slot, bool use)
{
    const QString name = resolvedUseName(slot);
    if (name.isEmpty()) {
        return false;
    }
    const int candidate = use ? 1 : 0;
    if (m_values.value(name).toInt() == candidate) {
        return false;
    }
    return beginWrite({change(name, candidate)}, WriteOperation::Use);
}

bool ConfigCompassViewModel::setLearnOffsets(bool learn)
{
    const QString name = QStringLiteral("COMPASS_LEARN");
    if (!m_values.contains(name)) {
        return false;
    }
    const int candidate = learn ? 1 : 0;
    if (m_values.value(name).toInt() == candidate) {
        return false;
    }
    return beginWrite({change(name, candidate)}, WriteOperation::Learn);
}

bool ConfigCompassViewModel::writeDeclinationDegrees(double degrees)
{
    const QString name = QStringLiteral("COMPASS_DEC");
    if (!m_values.contains(name) || !qIsFinite(degrees)) {
        return false;
    }
    const double radians = degrees * kPi / 180.0;
    if (valuesEqual(m_values.value(name), radians)) {
        return false;
    }
    return beginWrite({change(name, radians)}, WriteOperation::Declination);
}

bool ConfigCompassViewModel::quickPixhawk()
{
    const QString external = QStringLiteral("COMPASS_EXTERNAL");
    const QString orientation = QStringLiteral("COMPASS_ORIENT");
    if (!m_values.contains(external) || !m_values.contains(orientation)) {
        m_status = tr("COMPASS_EXTERNAL and COMPASS_ORIENT are both required "
                      "for Pixhawk defaults.");
        emit stateChanged();
        return false;
    }
    return beginWrite({change(external, 0), change(orientation, 0)},
                      WriteOperation::QuickPixhawk);
}

bool ConfigCompassViewModel::Refresh()
{
    if (!m_connected || Busy()) {
        return false;
    }
    m_status = tr("Refreshing compass parameters…");
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

bool ConfigCompassViewModel::Reboot()
{
    if (!m_connected) {
        m_status = tr("offline");
        emit stateChanged();
        return false;
    }
    if (m_armed) {
        m_status = tr("Vehicle is armed; reboot is disabled.");
        emit stateChanged();
        return false;
    }
    if (m_rebootOutcomeUncertain) {
        m_status = tr("The previous reboot outcome is uncertain. Do not retry "
                      "on this connection; power-cycle or reconnect the "
                      "vehicle.");
        emit stateChanged();
        return false;
    }
    if (!m_snapshotComplete || !m_snapshotReady
        || m_reconciliationRequired || Busy()) {
        return false;
    }

    m_rebootPending = true;
    m_rebootSubmitted = false;
    m_rebootRequestId = ++m_requestGeneration;
    m_rebootSnapshotGeneration = m_snapshotGeneration;
    m_status = tr("Submitting reboot request…");
    const quint64 requestId = m_rebootRequestId;
    const quint64 snapshotGeneration = m_rebootSnapshotGeneration;
    emit stateChanged();
    emit rebootRequested(requestId);

    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, requestId, snapshotGeneration]() {
        if (!m_rebootPending || m_rebootSubmitted
            || m_rebootRequestId != requestId
            || m_rebootSnapshotGeneration != snapshotGeneration
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        finishReboot(tr("Reboot submission timeout; reboot is still "
                        "required."), false);
    });
    return true;
}

void ConfigCompassViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (componentId != m_componentId || !m_snapshotReady
        || m_pending.active || m_reconciliationRequired
        || !m_values.contains(normalized)) {
        return;
    }
    m_values.insert(normalized, value);
    rebuildPresentation();
    if (m_connected && !m_armed && !m_rebootPending) {
        m_status.clear();
    }
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigCompassViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    if (batchId == 0) {
        finishFailure(tr("write was rejected"));
        return;
    }
    m_pending.batchId = batchId;
    if (m_pending.operation == WriteOperation::Priority) {
        // Once the transport owns the priority batch, any subset may reach the
        // vehicle.  Conservatively require a reboot even if the terminal batch
        // later reports partial failure.
        m_rebootRequired = true;
    }
    m_status = tr("Writing compass configuration…");
    emit stateChanged();
}

void ConfigCompassViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    const QString failure = reason.isEmpty() ? tr("write failed") : reason;
    if (m_pending.batchId != 0) {
        finishUncertain(
            tr("Compass write outcome is uncertain: %1. A complete "
               "parameter refresh is required.").arg(failure), true);
    } else {
        finishFailure(failure);
    }
}

void ConfigCompassViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    if (m_pending.failureReason.isEmpty()) {
        m_pending.failureReason =
            reason.isEmpty() ? tr("write failed") : reason;
    }
    m_status = tr("A compass parameter write failed; waiting for the "
                  "complete batch result…");
    emit stateChanged();
}

void ConfigCompassViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    if (m_pending.failureReason.isEmpty()) {
        m_pending.failureReason = tr("write cancelled");
    }
    m_status = tr("A compass parameter write was cancelled; waiting for "
                  "the complete batch result…");
    emit stateChanged();
}

void ConfigCompassViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId) {
        return;
    }
    if (failed == 0 && succeeded == m_pending.changes.size()
        && m_pending.failureReason.isEmpty()) {
        finishSuccess();
        return;
    }
    const QString failure = m_pending.failureReason.isEmpty()
        ? tr("the batch did not report complete success")
        : m_pending.failureReason;
    finishUncertain(
        tr("Compass parameters may have been only partially applied (%1); "
           "a complete parameter refresh is required.").arg(failure), true);
}

void ConfigCompassViewModel::refreshFailed(const QString &reason)
{
    const QString failure = reason.isEmpty()
        ? tr("parameter refresh failed") : reason;
    m_status = m_reconciliationRequired
        ? tr("Compass state remains uncertain; refresh required: %1")
              .arg(failure)
        : failure;
    emit stateChanged();
}

void ConfigCompassViewModel::refreshCanceled()
{
    m_status = m_reconciliationRequired
        ? tr("Compass state remains uncertain; parameter refresh was "
             "canceled.")
        : tr("parameter refresh canceled");
    emit stateChanged();
}

void ConfigCompassViewModel::rebootSubmitted(quint64 requestId)
{
    if (!m_rebootPending || m_rebootSubmitted
        || requestId != m_rebootRequestId
        || m_rebootSnapshotGeneration != m_snapshotGeneration) {
        return;
    }
    m_rebootSubmitted = true;
    m_status = tr("Waiting for reboot acknowledgement…");
    const quint64 snapshotGeneration = m_rebootSnapshotGeneration;
    emit stateChanged();

    QTimer::singleShot(kRebootAckTimeoutMs, this,
                       [this, requestId, snapshotGeneration]() {
        if (!m_rebootPending || !m_rebootSubmitted
            || m_rebootRequestId != requestId
            || m_rebootSnapshotGeneration != snapshotGeneration
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        m_rebootOutcomeUncertain = true;
        finishReboot(tr("Reboot acknowledgement timeout; reboot is still "
                        "required. Do not retry on this connection."), false);
    });
}

void ConfigCompassViewModel::rebootSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_rebootPending || requestId != m_rebootRequestId) {
        return;
    }
    if (m_rebootSubmitted) {
        m_rebootOutcomeUncertain = true;
    }
    finishReboot(reason.isEmpty()
                     ? tr("Reboot submission failed; reboot is still "
                          "required.")
                     : reason,
                 false);
}

void ConfigCompassViewModel::rebootAcknowledged(
    quint64 requestId, bool accepted, const QString &reason)
{
    if (!m_rebootPending || !m_rebootSubmitted
        || requestId != m_rebootRequestId
        || m_rebootSnapshotGeneration != m_snapshotGeneration) {
        return;
    }
    if (accepted) {
        finishReboot(tr("Reboot accepted."), true);
    } else {
        finishReboot(reason.isEmpty()
                         ? tr("Reboot was rejected; reboot is still "
                              "required.")
                         : reason,
                     false);
    }
}

void ConfigCompassViewModel::rebootCancelled(
    quint64 requestId, const QString &reason)
{
    if (!m_rebootPending || requestId != m_rebootRequestId) {
        return;
    }
    if (m_rebootSubmitted) {
        m_rebootOutcomeUncertain = true;
    }
    finishReboot(reason.isEmpty()
                     ? tr("Reboot was canceled; reboot is still required.")
                     : reason,
                 false);
}

void ConfigCompassViewModel::rebuildPresentation()
{
    beginResetModel();
    rebuildRows();
    endResetModel();
    rebuildFields();
    updateCompassStatus();
}

void ConfigCompassViewModel::rebuildRows()
{
    m_rows.clear();
    if (!m_snapshotReady) {
        return;
    }

    struct Device
    {
        QString name;
        quint32 id = 0;
        int slot = 0;
    };
    QVector<Device> devices;
    QStringList names = m_values.keys();
    std::sort(names.begin(), names.end());
    for (const QString &name : names) {
        if (!isCompassParameter(name)
            || !name.contains(QLatin1String("DEV_ID"))) {
            continue;
        }
        const quint32 id = ConfigHWIDViewModel::RawId(m_values.value(name));
        if (id != 0) {
            devices.append({name, id, physicalSlotForName(name)});
        }
    }

    struct PriorityValue
    {
        int slot = 0;
        QString name;
        quint32 id = 0;
    };
    QVector<PriorityValue> priorities;
    static const QRegularExpression priorityExpression(
        QStringLiteral("^COMPASS_PRIO([0-9]+)_ID$"));
    for (const QString &name : names) {
        const QRegularExpressionMatch match =
            priorityExpression.match(name);
        if (!match.hasMatch()) {
            continue;
        }
        const quint32 id = ConfigHWIDViewModel::RawId(m_values.value(name));
        if (id != 0) {
            priorities.append({match.captured(1).toInt(), name, id});
        }
    }
    std::stable_sort(priorities.begin(), priorities.end(),
                     [](const PriorityValue &left,
                        const PriorityValue &right) {
        return left.slot == right.slot ? left.name < right.name
                                       : left.slot < right.slot;
    });

    QSet<quint32> priorityDeviceIds;
    for (const PriorityValue &priority : priorities) {
        priorityDeviceIds.insert(priority.id);
    }
    const auto appendRow = [this, &devices, &priorityDeviceIds](
                               quint32 id, bool priorityEntry) {
        if (id == 0) {
            return;
        }
        // MP10 preserves every priority slot, including duplicate IDs, so a
        // malformed configuration stays visible and reorderable.  Only the
        // lower discovery tail is filtered only against the priority rows.
        if (!priorityEntry && priorityDeviceIds.contains(id)) {
            return;
        }
        auto found = std::find_if(devices.constBegin(), devices.constEnd(),
                                  [id](const Device &device) {
            return device.id == id;
        });
        const bool missing = found == devices.constEnd();
        const QString decodeName = missing
            ? QStringLiteral("COMPASS_PRIO_ID") : found->name;
        const HwIdRow decoded = ConfigHWIDViewModel::Decode(decodeName, id);

        CompassPriorityRow row;
        row.priority = m_rows.size() + 1;
        row.rawDevId = id;
        row.devId = decoded.devId;
        row.busType = decoded.busType;
        row.bus = decoded.bus;
        row.address = decoded.address;
        row.devType = decoded.devType;
        row.missing = missing && priorityEntry;
        if (!missing) {
            row.physicalSlot = found->slot;
            const QString externalName =
                resolvedExternalName(row.physicalSlot);
            row.external = !externalName.isEmpty()
                && m_values.value(externalName).toInt() != 0;
            const QString orientationName =
                resolvedOrientationName(row.physicalSlot);
            row.orientation = orientationName.isEmpty()
                ? tr("n/a")
                : orientationText(row.physicalSlot,
                                  m_values.value(orientationName));
        } else {
            row.orientation = tr("n/a");
        }
        m_rows.append(row);
    };

    for (const PriorityValue &priority : priorities) {
        appendRow(priority.id, true);
    }
    for (const Device &device : devices) {
        appendRow(device.id, false);
    }
}

void ConfigCompassViewModel::rebuildFields()
{
    m_fields.clear();
    for (const QString &name : AdvancedFieldNames()) {
        const bool present = m_snapshotReady && m_values.contains(name);
        const bool hasMetadata = m_catalog.contains(name);
        const ParameterMetaData metadata = hasMetadata
            ? m_catalog.value(name) : ParameterMetaData();

        ParamField field;
        field.componentId = m_componentId;
        field.name = name;
        field.label = metadata.title.isEmpty() ? name : metadata.title;
        field.units = metadata.units;
        field.description = metadata.description;
        field.value = present ? m_values.value(name) : QVariant();
        field.minimum = metadata.minimum;
        field.maximum = metadata.maximum;
        field.increment = metadata.increment;
        field.hasRange = metadata.hasRange;
        field.enforceRange = m_enforceRanges;
        field.readOnly = !present || metadata.readOnly;
        field.status = present ? QString() : tr("n/a");

        if (metadata.isEnum()) {
            field.editorKind = ParamField::EditorKind::Combo;
            for (const ParameterMetaDataOption &option : metadata.values) {
                field.options.append({option.value, option.label});
            }
            // Firmware can report a value newer than the local metadata.
            // Keep it visible without manufacturing a correcting write.
            if (present && !optionContains(field.options, field.value)) {
                field.options.append(
                    {field.value, printableNumber(field.value)});
            }
        } else if (name.contains(QLatin1String("EXTERN"))
                   || name == QLatin1String("COMPASS_AUTODEC")) {
            field.editorKind = ParamField::EditorKind::Combo;
            field.options = {{0, tr("Disabled")}, {1, tr("Enabled")}};
            if (present && !optionContains(field.options, field.value)) {
                field.options.append(
                    {field.value, printableNumber(field.value)});
            }
        } else {
            // In the absence of enum metadata MP10's current numeric value
            // remains visible and can be explicitly corrected by the user.
            field.editorKind = ParamField::EditorKind::Numeric;
        }
        m_fields.append(field);
    }
}

void ConfigCompassViewModel::updateCompassStatus()
{
    int missing = 0;
    for (const CompassPriorityRow &row : m_rows) {
        if (row.missing) {
            ++missing;
        }
    }
    m_compassStatus = missing == 0
        ? QString()
        : tr("%n prioritized compass device(s) are missing.", nullptr,
             missing);
}

bool ConfigCompassViewModel::beginWrite(
    const QVariantList &changes, WriteOperation operation)
{
    if (!canWrite(tr("compass change")) || changes.isEmpty()) {
        return false;
    }

    QVariantList effectiveChanges;
    QSet<QString> names;
    for (const QVariant &item : changes) {
        const QVariantMap requested = item.toMap();
        const QString name = normalizedName(
            requested.value(QStringLiteral("name")).toString());
        if (name.isEmpty() || names.contains(name)
            || !m_values.contains(name)) {
            m_status = tr("The requested compass parameter is unavailable.");
            emit stateChanged();
            return false;
        }
        names.insert(name);
        const QVariant value = requested.value(QStringLiteral("value"));
        // MP10 priority writes are one indivisible three-parameter operation:
        // unchanged slots are intentionally retained in the emitted batch.
        if (operation == WriteOperation::Priority
            || operation == WriteOperation::QuickPixhawk
            || !valuesEqual(m_values.value(name), value)) {
            effectiveChanges.append(change(name, value));
        }
    }
    if (effectiveChanges.isEmpty()) {
        return false;
    }

    m_pending = {};
    m_pending.active = true;
    m_pending.changes = effectiveChanges;
    m_pending.previousValues = m_values;
    m_pending.requestId = ++m_requestGeneration;
    m_pending.snapshotGeneration = m_snapshotGeneration;
    m_pending.operation = operation;
    for (const QVariant &item : effectiveChanges) {
        const QVariantMap entry = item.toMap();
        m_values.insert(entry.value(QStringLiteral("name")).toString(),
                        entry.value(QStringLiteral("value")));
    }
    rebuildPresentation();
    m_status = tr("Submitting compass configuration…");
    const quint64 requestId = m_pending.requestId;
    const quint64 snapshotGeneration = m_pending.snapshotGeneration;
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
    emit stateChanged();
    emit writeRequested(requestId, m_componentId, effectiveChanges);

    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, requestId, snapshotGeneration]() {
        if (!m_pending.active || m_pending.requestId != requestId
            || m_pending.snapshotGeneration != snapshotGeneration
            || m_pending.batchId != 0
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        finishFailure(tr("write submission timeout"));
    });
    return true;
}

bool ConfigCompassViewModel::canWrite(const QString &action)
{
    Q_UNUSED(action)
    if (!m_connected) {
        m_status = tr("offline");
    } else if (m_armed) {
        m_status = tr("Vehicle is armed; compass changes are disabled.");
    } else if (m_reconciliationRequired) {
        m_status = tr("Compass state is uncertain; obtain a complete "
                      "parameter snapshot before making another change.");
    } else if (!m_snapshotComplete) {
        m_status = tr("A complete parameter snapshot is required.");
    } else if (!m_snapshotReady) {
        m_status = tr("COMPASS_PRIO1_ID is not available on this vehicle.");
    } else if (Busy()) {
        m_status = tr("Another compass operation is still in progress.");
    } else {
        return true;
    }
    emit stateChanged();
    return false;
}

void ConfigCompassViewModel::finishSuccess()
{
    if (!m_pending.active) {
        return;
    }
    const WriteOperation operation = m_pending.operation;
    m_pending = {};
    if (operation == WriteOperation::Priority) {
        m_rebootRequired = true;
        m_status = tr("Compass priority updated. A reboot is required.");
    } else if (operation == WriteOperation::QuickPixhawk) {
        m_status = tr("Pixhawk compass defaults applied.");
    } else if (operation == WriteOperation::Declination) {
        m_status = tr("Compass declination updated.");
    } else {
        m_status = tr("Compass configuration updated.");
    }
    rebuildPresentation();
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigCompassViewModel::finishFailure(const QString &reason)
{
    if (!m_pending.active) {
        return;
    }
    rollbackPending();
    m_status = reason;
    emit rowsChanged(m_rows.size());
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigCompassViewModel::finishUncertain(
    const QString &reason, bool requestRefresh)
{
    if (!m_pending.active) {
        return;
    }
    const int component = m_componentId;
    m_pending = {};
    m_values.clear();
    m_snapshotReady = false;
    m_snapshotComplete = false;
    m_reconciliationRequired = true;
    clearPresentation();
    m_status = reason;
    emit rowsChanged(0);
    emit fieldsChanged();
    emit stateChanged();
    if (requestRefresh) {
        emit refreshRequested(component);
    }
}

void ConfigCompassViewModel::rollbackPending()
{
    if (!m_pending.active) {
        return;
    }
    m_values = m_pending.previousValues;
    m_pending = {};
    rebuildPresentation();
}

void ConfigCompassViewModel::clearPresentation()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
    rebuildFields();
    m_compassStatus.clear();
}

bool ConfigCompassViewModel::pendingContains(const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (const QVariant &item : m_pending.changes) {
        if (normalizedName(item.toMap()
                               .value(QStringLiteral("name"))
                               .toString()) == normalized) {
            return true;
        }
    }
    return false;
}

QVariantList ConfigCompassViewModel::priorityChanges(
    const QVector<CompassPriorityRow> &candidate) const
{
    QVariantList changes;
    for (int slot = 1; slot <= 3; ++slot) {
        const QString name =
            QStringLiteral("COMPASS_PRIO%1_ID").arg(slot);
        if (!m_values.contains(name)) {
            return {};
        }
        const quint32 id = slot <= candidate.size()
            ? candidate.at(slot - 1).rawDevId : 0;
        changes.append(change(name, QVariant::fromValue(id)));
    }
    return changes;
}

QString ConfigCompassViewModel::resolvedUseName(int slot) const
{
    for (const QString &name : useAliases(slot)) {
        if (m_values.contains(name)) {
            return name;
        }
    }
    return {};
}

QString ConfigCompassViewModel::resolvedDevIdName(int slot) const
{
    for (const QString &name : devIdAliases(slot)) {
        if (m_values.contains(name)) {
            return name;
        }
    }
    return {};
}

QString ConfigCompassViewModel::resolvedExternalName(int slot) const
{
    for (const QString &name : externalAliases(slot)) {
        if (m_values.contains(name)) {
            return name;
        }
    }
    return {};
}

QString ConfigCompassViewModel::resolvedOrientationName(int slot) const
{
    for (const QString &name : orientationAliases(slot)) {
        if (m_values.contains(name)) {
            return name;
        }
    }
    return {};
}

QString ConfigCompassViewModel::orientationText(
    int slot, const QVariant &value) const
{
    QString metadataName = resolvedOrientationName(slot);
    if (!m_catalog.contains(metadataName)) {
        metadataName = canonicalOrientationName(slot);
    }
    if (m_catalog.contains(metadataName)) {
        const ParameterMetaData metadata = m_catalog.value(metadataName);
        for (const ParameterMetaDataOption &option : metadata.values) {
            if (valuesEqual(option.value, value)) {
                return option.label;
            }
        }
    }
    return printableNumber(value);
}

ParamField ConfigCompassViewModel::fieldForName(const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (const ParamField &field : m_fields) {
        if (field.name == normalized) {
            return field;
        }
    }
    return {};
}

void ConfigCompassViewModel::finishReboot(
    const QString &status, bool accepted)
{
    if (!m_rebootPending) {
        return;
    }
    m_rebootPending = false;
    m_rebootSubmitted = false;
    m_rebootRequestId = 0;
    m_rebootSnapshotGeneration = 0;
    if (accepted) {
        m_rebootRequired = false;
        m_rebootOutcomeUncertain = false;
    }
    m_status = status;
    emit stateChanged();
}

QString ConfigCompassViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigCompassViewModel::valuesEqual(
    const QVariant &left, const QVariant &right)
{
    if (!left.isValid() || !right.isValid()) {
        return !left.isValid() && !right.isValid();
    }
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk && qIsFinite(leftValue) && qIsFinite(rightValue)
        ? std::abs(leftValue - rightValue) <= 1.0e-9
        : left == right;
}
