#include "ConfigFlightModesViewModel.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
constexpr quint32 kEditableSimpleBits = 0x3fU;

QVariantMap change(const QString &name, const QVariant &value)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("value"), value}};
}
} // namespace

bool FlightModeRow::modeDirty() const
{
    if (!acceptedMode.isValid() || !mode.isValid()) {
        return acceptedMode.isValid() != mode.isValid();
    }
    bool acceptedOk = false;
    bool modeOk = false;
    const double accepted = acceptedMode.toDouble(&acceptedOk);
    const double candidate = mode.toDouble(&modeOk);
    return acceptedOk && modeOk
        ? std::abs(accepted - candidate) > 1.0e-6
        : acceptedMode != mode;
}

ConfigFlightModesViewModel::ConfigFlightModesViewModel(QObject *parent)
    : QObject(parent)
{
    resetRows();
    rebuildOptions();
    updateStatus();
}

int ConfigFlightModesViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

int ConfigFlightModesViewModel::RowForPwm(int pwm)
{
    if (pwm < 0) {
        return -1;
    }
    if (pwm <= 1230) {
        return 0;
    }
    if (pwm <= 1360) {
        return 1;
    }
    if (pwm <= 1490) {
        return 2;
    }
    if (pwm <= 1620) {
        return 3;
    }
    if (pwm <= 1749) {
        return 4;
    }
    return 5;
}

QString ConfigFlightModesViewModel::PwmBand(int zeroBasedRow)
{
    static const QStringList bands{
        QStringLiteral("PWM 0 - 1230"),
        QStringLiteral("PWM 1231 - 1360"),
        QStringLiteral("PWM 1361 - 1490"),
        QStringLiteral("PWM 1491 - 1620"),
        QStringLiteral("PWM 1621 - 1749"),
        QStringLiteral("PWM 1750 +")};
    return zeroBasedRow >= 0 && zeroBasedRow < bands.size()
        ? bands.at(zeroBasedRow) : QString();
}

QString ConfigFlightModesViewModel::ModePrefix(Family family)
{
    switch (family) {
    case Family::Copter:
    case Family::Plane:
        return QStringLiteral("FLTMODE");
    case Family::Rover:
        return QStringLiteral("MODE");
    case Family::Px4:
        return QStringLiteral("COM_FLTMODE");
    case Family::Unsupported:
        return {};
    }
    return {};
}

QString ConfigFlightModesViewModel::SwitchParameter(Family family)
{
    switch (family) {
    case Family::Copter:
    case Family::Plane:
        return QStringLiteral("FLTMODE_CH");
    case Family::Rover:
        return QStringLiteral("MODE_CH");
    case Family::Px4:
        return QStringLiteral("COM_FLTMODE_CH");
    case Family::Unsupported:
        return {};
    }
    return {};
}

void ConfigFlightModesViewModel::setFamily(
    Family family, const QList<ParamOption> &modeOptions)
{
    if (m_family == family) {
        setModeOptions(modeOptions);
        return;
    }

    ++m_snapshotGeneration;
    m_pending = {};
    m_family = family;
    m_suppliedOptions = modeOptions;
    m_snapshotComplete = false;
    m_snapshotReady = false;
    m_reconciliationRequired = false;
    m_operationStatus.clear();
    clearSnapshotPresentation();
    rebuildOptions();
    updateStatus();
    emit optionsChanged();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigFlightModesViewModel::setModeOptions(
    const QList<ParamOption> &modeOptions)
{
    if (optionListsEqual(m_suppliedOptions, modeOptions)) {
        return;
    }
    m_suppliedOptions = modeOptions;
    rebuildOptions();
    emit optionsChanged();
    emit stateChanged();
}

void ConfigFlightModesViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot,
    bool preserveStagedEdits)
{
    if (m_pending.active && m_pending.batchId != 0
        && m_pending.changes.size() > 1) {
        // A list transition concurrent with a submitted multi-item batch may
        // describe the pre-write state or only a prefix of the applied batch.
        // It cannot replace the owned terminal result or reconcile the page.
        finishUncertain(
            tr("The parameter snapshot changed during a flight mode write; "
               "refreshing the full vehicle state is required."), true);
        return;
    }
    if (m_pending.active) {
        // A snapshot is not an owned terminal result for the pending batch.
        // Keep the edit and correlation state until the matching result.
        return;
    }

    QList<QVariant> stagedModes;
    QList<bool> modeDirty;
    QList<bool> stagedSimple;
    QList<bool> simpleDirty;
    QList<bool> stagedSuperSimple;
    QList<bool> superSimpleDirty;
    const bool preserve = preserveStagedEdits && !m_reconciliationRequired
        && Dirty();
    if (preserve) {
        stagedModes.reserve(m_rows.size());
        modeDirty.reserve(m_rows.size());
        stagedSimple.reserve(m_rows.size());
        simpleDirty.reserve(m_rows.size());
        stagedSuperSimple.reserve(m_rows.size());
        superSimpleDirty.reserve(m_rows.size());
        for (const FlightModeRow &row : m_rows) {
            stagedModes.append(row.mode);
            modeDirty.append(row.modeDirty());
            stagedSimple.append(row.simple);
            simpleDirty.append(row.simpleDirty());
            stagedSuperSimple.append(row.superSimple);
            superSimpleDirty.append(row.superSimpleDirty());
        }
    }

    const QString prefix = ModePrefix(m_family);
    QHash<int, quint8> schemas;
    if (!prefix.isEmpty()) {
        for (const ConfigFriendlyParameterValue &parameter : parameters) {
            const QString name = normalizedName(parameter.name);
            for (int row = 0; row < 6; ++row) {
                if (name == prefix + QString::number(row + 1)) {
                    schemas[parameter.componentId] |= quint8(1U << row);
                    break;
                }
            }
        }
    }

    QList<int> capableComponents;
    for (auto iterator = schemas.constBegin(); iterator != schemas.constEnd();
         ++iterator) {
        if (iterator.value() == quint8(0x3fU)) {
            capableComponents.append(iterator.key());
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

    ++m_snapshotGeneration;
    m_pending = {};
    m_snapshotComplete = completeSnapshot;
    m_snapshotReady = completeSnapshot && !prefix.isEmpty()
        && capableComponents.contains(m_componentId);
    if (m_snapshotReady) {
        m_reconciliationRequired = false;
    }
    m_operationStatus.clear();
    clearSnapshotPresentation();

    if (m_snapshotReady) {
        const QString switchName = SwitchParameter(m_family);
        for (const ConfigFriendlyParameterValue &parameter : parameters) {
            if (parameter.componentId != m_componentId) {
                continue;
            }
            const QString name = normalizedName(parameter.name);
            const int row = modeRow(name);
            if (row >= 0) {
                m_rows[row].acceptedMode = parameter.value;
                m_rows[row].mode = parameter.value;
                continue;
            }
            if (!switchName.isEmpty() && name == switchName) {
                bool ok = false;
                const int channel = parameter.value.toInt(&ok);
                m_switchChannel = ok && channel > 0 && channel <= 18
                    ? channel : 0;
            } else if (m_family == Family::Copter
                       && name == QLatin1String("SIMPLE")) {
                m_showSimple = true;
                m_simpleValue = static_cast<quint32>(
                    parameter.value.toULongLong());
            } else if (m_family == Family::Copter
                       && name == QLatin1String("SUPER_SIMPLE")) {
                m_showSuperSimple = true;
                m_superSimpleValue = static_cast<quint32>(
                    parameter.value.toULongLong());
            }
        }
        applySimpleValue(m_simpleValue, false, false);
        applySimpleValue(m_superSimpleValue, true, false);
        if (preserve && stagedModes.size() == m_rows.size()) {
            for (int row = 0; row < m_rows.size(); ++row) {
                if (modeDirty.at(row)) {
                    m_rows[row].mode = stagedModes.at(row);
                }
                if (m_showSimple && simpleDirty.at(row)) {
                    m_rows[row].simple = stagedSimple.at(row);
                }
                if (m_showSuperSimple && superSimpleDirty.at(row)) {
                    m_rows[row].superSimple =
                        stagedSuperSimple.at(row);
                }
            }
            if (Dirty()) {
                m_operationStatus = tr(
                    "Parameters refreshed; staged flight mode changes "
                    "were preserved.");
            }
        }
    }

    rebuildOptions();
    updateStatus();
    emit optionsChanged();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigFlightModesViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        m_heartbeatFresh = false;
        m_hasCurrentMode = false;
        clearRcInput();
        if (m_pending.active && m_pending.batchId != 0
            && m_pending.changes.size() > 1) {
            finishUncertain(
                tr("Flight mode state is uncertain after connection loss; "
                   "a full parameter refresh is required."), false);
            return;
        }
        if (m_pending.active) {
            m_pending = {};
            m_operationStatus = tr(
                "The write was not confirmed; staged changes were kept.");
        }
    }
    updateStatus();
    const QPointer<ConfigFlightModesViewModel> guard(this);
    emit stateChanged();
    if (guard && connected && m_connected && m_reconciliationRequired) {
        emit refreshRequested(m_componentId);
    }
}

void ConfigFlightModesViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    updateStatus();
    emit stateChanged();
}

void ConfigFlightModesViewModel::setHeartbeat(
    quint32 customMode, bool fresh, bool armed)
{
    const bool actualFresh = fresh && m_connected;
    const bool changed = m_heartbeatFresh != actualFresh
        || m_armed != armed || m_hasCurrentMode != actualFresh
        || (actualFresh && m_currentMode != customMode);
    m_heartbeatFresh = actualFresh;
    m_armed = armed;
    m_hasCurrentMode = actualFresh;
    m_currentMode = actualFresh ? customMode : 0;
    if (!changed) {
        return;
    }
    updateStatus();
    emit stateChanged();
}

void ConfigFlightModesViewModel::setHeartbeatFresh(bool fresh)
{
    const bool actualFresh = fresh && m_connected;
    if (m_heartbeatFresh == actualFresh
        && (actualFresh || !m_hasCurrentMode)) {
        return;
    }
    m_heartbeatFresh = actualFresh;
    if (!actualFresh) {
        m_hasCurrentMode = false;
        m_currentMode = 0;
    }
    updateStatus();
    emit stateChanged();
}

QString ConfigFlightModesViewModel::CurrentModeText() const
{
    if (!m_heartbeatFresh || !m_hasCurrentMode) {
        return {};
    }
    if (m_family == Family::Px4) {
        const quint32 mainMode = (m_currentMode >> 16U) & 0xffU;
        const quint32 subMode = (m_currentMode >> 24U) & 0xffU;
        switch (mainMode) {
        case 1:
            return QStringLiteral("Manual");
        case 2:
            return QStringLiteral("Altitude Control");
        case 3:
            return QStringLiteral("Position Control");
        case 4:
            switch (subMode) {
            case 1:
                return QStringLiteral("Auto: Ready");
            case 2:
                return QStringLiteral("Auto: Takeoff");
            case 3:
                return QStringLiteral("Loiter");
            case 4:
                return QStringLiteral("Auto");
            case 5:
                return QStringLiteral("RTL");
            case 6:
                return QStringLiteral("Auto: Landing");
            default:
                break;
            }
            break;
        case 5:
            return QStringLiteral("Acro");
        case 6:
            return QStringLiteral("Offboard Control");
        case 7:
            return QStringLiteral("Stabilized");
        case 8:
            return QStringLiteral("Rattitude");
        default:
            break;
        }
        return tr("Unknown (%1)").arg(m_currentMode);
    }
    for (const ParamOption &option : m_modeOptions) {
        if (valuesEqual(option.value, QVariant::fromValue(m_currentMode))) {
            return option.text;
        }
    }
    return tr("Unknown (%1)").arg(m_currentMode);
}

QString ConfigFlightModesViewModel::CurrentPwmText() const
{
    if (!m_hasCurrentPwm || m_switchChannel <= 0) {
        return {};
    }
    return QStringLiteral("%1: %2")
        .arg(m_switchChannel).arg(m_currentPwm);
}

bool ConfigFlightModesViewModel::CanEdit() const
{
    return m_family != Family::Unsupported && m_connected
        && m_heartbeatFresh && !m_armed && m_snapshotReady
        && !m_pending.active && !m_reconciliationRequired;
}

bool ConfigFlightModesViewModel::Dirty() const
{
    for (const FlightModeRow &row : m_rows) {
        if (row.modeDirty()
            || (m_showSimple && row.simpleDirty())
            || (m_showSuperSimple && row.superSimpleDirty())) {
            return true;
        }
    }
    return false;
}

bool ConfigFlightModesViewModel::stageMode(
    int row, const QVariant &mode)
{
    if (!CanEdit() || row < 0 || row >= m_rows.size()
        || !mode.isValid()) {
        return false;
    }
    const bool known = std::any_of(
        m_modeOptions.constBegin(), m_modeOptions.constEnd(),
        [&mode](const ParamOption &option) {
            return valuesEqual(option.value, mode);
        });
    if (!known || valuesEqual(m_rows[row].mode, mode)) {
        return false;
    }
    m_rows[row].mode = mode;
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

bool ConfigFlightModesViewModel::stageSimple(int row, bool enabled)
{
    if (!CanEdit() || !m_showSimple || row < 0
        || row >= m_rows.size() || m_rows[row].simple == enabled) {
        return false;
    }
    m_rows[row].simple = enabled;
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

bool ConfigFlightModesViewModel::stageSuperSimple(
    int row, bool enabled)
{
    if (!CanEdit() || !m_showSuperSimple || row < 0
        || row >= m_rows.size() || m_rows[row].superSimple == enabled) {
        return false;
    }
    m_rows[row].superSimple = enabled;
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

bool ConfigFlightModesViewModel::Discard()
{
    if (m_pending.active || !Dirty()) {
        return false;
    }
    for (FlightModeRow &row : m_rows) {
        row.mode = row.acceptedMode;
        row.simple = row.acceptedSimple;
        row.superSimple = row.acceptedSuperSimple;
    }
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

QVariantList ConfigFlightModesViewModel::DirtyChanges() const
{
    QVariantList changes;
    if (!m_snapshotReady) {
        return changes;
    }
    for (const FlightModeRow &row : m_rows) {
        if (row.modeDirty()) {
            changes.append(change(row.parameterName, row.mode));
        }
    }
    if (m_showSimple) {
        const quint32 candidate = stagedSimpleValue(false);
        if (candidate != m_simpleValue) {
            changes.append(change(
                QStringLiteral("SIMPLE"),
                QVariant::fromValue(candidate)));
        }
    }
    if (m_showSuperSimple) {
        const quint32 candidate = stagedSimpleValue(true);
        if (candidate != m_superSimpleValue) {
            changes.append(change(
                QStringLiteral("SUPER_SIMPLE"),
                QVariant::fromValue(candidate)));
        }
    }
    return changes;
}

bool ConfigFlightModesViewModel::Save()
{
    if (!CanSave()) {
        updateStatus();
        emit stateChanged();
        return false;
    }
    const QVariantList changes = DirtyChanges();
    if (changes.isEmpty()) {
        return false;
    }

    m_pending = {};
    m_pending.active = true;
    m_pending.changes = changes;
    m_pending.requestId = ++m_requestGeneration;
    m_pending.snapshotGeneration = m_snapshotGeneration;
    m_operationStatus.clear();
    updateStatus();
    const QPointer<ConfigFlightModesViewModel> guard(this);
    emit stateChanged();
    if (!guard || !m_pending.active
        || m_pending.requestId != m_requestGeneration
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return false;
    }

    const quint64 requestId = m_pending.requestId;
    const quint64 snapshotGeneration = m_pending.snapshotGeneration;
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

    emit writeRequested(requestId, m_componentId, changes);
    return !guard.isNull();
}

bool ConfigFlightModesViewModel::Refresh()
{
    if (!m_connected || m_pending.active
        || m_family == Family::Unsupported) {
        return false;
    }
    m_operationStatus = tr("Refreshing the full parameter list…");
    updateStatus();
    const quint64 snapshotGeneration = m_snapshotGeneration;
    const QPointer<ConfigFlightModesViewModel> guard(this);
    emit stateChanged();
    if (!guard || !m_connected || m_pending.active
        || m_family == Family::Unsupported
        || m_snapshotGeneration != snapshotGeneration) {
        return false;
    }
    emit refreshRequested(m_componentId);
    return true;
}

bool ConfigFlightModesViewModel::setRcInput(
    int oneBasedChannel, int pwm)
{
    if (m_switchChannel <= 0 || oneBasedChannel != m_switchChannel
        || pwm <= 0) {
        return false;
    }
    const int active = RowForPwm(pwm);
    const bool rowsChangedNow = m_activeRow != active;
    const bool stateChangedNow = !m_hasCurrentPwm
        || m_currentPwm != pwm || rowsChangedNow;
    m_hasCurrentPwm = true;
    m_currentPwm = pwm;
    m_activeRow = active;
    for (int row = 0; row < m_rows.size(); ++row) {
        m_rows[row].active = row == active;
    }
    if (rowsChangedNow) {
        emit rowsChanged();
    }
    if (stateChangedNow) {
        emit stateChanged();
    }
    return true;
}

void ConfigFlightModesViewModel::clearRcInput()
{
    if (!m_hasCurrentPwm && m_activeRow < 0) {
        return;
    }
    const bool hadActiveRow = m_activeRow >= 0;
    m_hasCurrentPwm = false;
    m_currentPwm = 0;
    m_activeRow = -1;
    for (FlightModeRow &row : m_rows) {
        row.active = false;
    }
    if (hadActiveRow) {
        emit rowsChanged();
    }
    emit stateChanged();
}

void ConfigFlightModesViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId || !m_snapshotReady
        || m_pending.active || m_reconciliationRequired) {
        return;
    }
    const QString normalized = normalizedName(name);
    const int row = modeRow(normalized);
    bool optionChanged = false;
    if (row >= 0) {
        const bool wasDirty = m_rows[row].modeDirty();
        m_rows[row].acceptedMode = value;
        if (!wasDirty) {
            m_rows[row].mode = value;
        }
        const int optionCount = m_modeOptions.size();
        addUnknownOption(value);
        optionChanged = m_modeOptions.size() != optionCount;
    } else if (normalized == SwitchParameter(m_family)) {
        bool ok = false;
        const int channel = value.toInt(&ok);
        const int candidate = ok && channel > 0 && channel <= 18
            ? channel : 0;
        if (candidate == m_switchChannel) {
            return;
        }
        m_switchChannel = candidate;
        clearRcInput();
        updateStatus();
        emit stateChanged();
        return;
    } else if (normalized == QLatin1String("SIMPLE") && m_showSimple) {
        m_simpleValue = static_cast<quint32>(value.toULongLong());
        applySimpleValue(m_simpleValue, false, true);
    } else if (normalized == QLatin1String("SUPER_SIMPLE")
               && m_showSuperSimple) {
        m_superSimpleValue = static_cast<quint32>(value.toULongLong());
        applySimpleValue(m_superSimpleValue, true, true);
    } else {
        return;
    }
    m_operationStatus.clear();
    updateStatus();
    if (optionChanged) {
        emit optionsChanged();
    }
    emit rowsChanged();
    emit stateChanged();
}

void ConfigFlightModesViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration
        || m_pending.batchId != 0) {
        return;
    }
    if (batchId == 0) {
        finishFailure(tr("write was rejected"));
        return;
    }
    m_pending.batchId = batchId;
}

void ConfigFlightModesViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration
        || m_pending.batchId != 0) {
        return;
    }
    finishFailure(reason.trimmed().isEmpty()
                      ? tr("write submission failed") : reason.trimmed());
}

void ConfigFlightModesViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    const QString failure = reason.trimmed().isEmpty()
        ? tr("write failed") : reason.trimmed();
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = failure;
        updateStatus();
        emit stateChanged();
        return;
    }
    finishFailure(failure);
}

void ConfigFlightModesViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = tr("write cancelled");
        updateStatus();
        emit stateChanged();
        return;
    }
    finishFailure(tr("write cancelled"));
}

void ConfigFlightModesViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId) {
        return;
    }
    const int total = m_pending.changes.size();
    const bool success = failed == 0 && succeeded == total
        && m_pending.failureReason.isEmpty();
    if (success) {
        finishSuccess();
    } else if (total > 1) {
        finishUncertain(
            tr("Flight mode parameters may have been partially applied; "
               "refreshing the full vehicle state is required."), true);
    } else {
        finishFailure(m_pending.failureReason.isEmpty()
                          ? tr("write failed")
                          : m_pending.failureReason);
    }
}

void ConfigFlightModesViewModel::refreshFailed(const QString &reason)
{
    const QString failure = reason.trimmed().isEmpty()
        ? tr("parameter refresh failed") : reason.trimmed();
    m_operationStatus = m_reconciliationRequired
        ? tr("Flight mode state remains uncertain; refresh failed: %1")
              .arg(failure)
        : failure;
    updateStatus();
    emit stateChanged();
}

void ConfigFlightModesViewModel::refreshCanceled()
{
    m_operationStatus = m_reconciliationRequired
        ? tr("Flight mode state remains uncertain; refresh was canceled.")
        : tr("parameter refresh canceled");
    updateStatus();
    emit stateChanged();
}

void ConfigFlightModesViewModel::resetRows()
{
    m_rows.clear();
    m_rows.reserve(6);
    static const int minimums[] = {0, 1231, 1361, 1491, 1621, 1750};
    static const int maximums[] = {1230, 1360, 1490, 1620, 1749, -1};
    const QString prefix = ModePrefix(m_family);
    for (int row = 0; row < 6; ++row) {
        FlightModeRow item;
        item.position = row + 1;
        item.parameterName = prefix.isEmpty()
            ? QString() : prefix + QString::number(row + 1);
        item.minimumPwm = minimums[row];
        item.maximumPwm = maximums[row];
        item.pwmBand = PwmBand(row);
        m_rows.append(item);
    }
}

void ConfigFlightModesViewModel::clearSnapshotPresentation()
{
    m_switchChannel = 0;
    m_simpleValue = 0;
    m_superSimpleValue = 0;
    m_showSimple = false;
    m_showSuperSimple = false;
    m_hasCurrentPwm = false;
    m_currentPwm = 0;
    m_activeRow = -1;
    resetRows();
}

void ConfigFlightModesViewModel::rebuildOptions()
{
    m_modeOptions.clear();
    for (const ParamOption &option : m_suppliedOptions) {
        if (!option.value.isValid()) {
            continue;
        }
        const bool duplicate = std::any_of(
            m_modeOptions.constBegin(), m_modeOptions.constEnd(),
            [&option](const ParamOption &existing) {
                return valuesEqual(existing.value, option.value);
            });
        if (!duplicate) {
            m_modeOptions.append(option);
        }
    }
    if (m_family == Family::Copter) {
        const bool hasModelCal = std::any_of(
            m_modeOptions.constBegin(), m_modeOptions.constEnd(),
            [](const ParamOption &option) {
                return valuesEqual(option.value, QVariant(31));
            });
        if (!hasModelCal) {
            m_modeOptions.append({31, QStringLiteral("ModelCal")});
        }
    }
    for (const FlightModeRow &row : m_rows) {
        if (row.acceptedMode.isValid()) {
            addUnknownOption(row.acceptedMode);
        }
        if (row.mode.isValid()) {
            addUnknownOption(row.mode);
        }
    }
}

void ConfigFlightModesViewModel::addUnknownOption(const QVariant &value)
{
    if (!value.isValid()) {
        return;
    }
    const bool known = std::any_of(
        m_modeOptions.constBegin(), m_modeOptions.constEnd(),
        [&value](const ParamOption &option) {
            return valuesEqual(option.value, value);
        });
    if (!known) {
        m_modeOptions.append(
            {value, tr("Unknown (%1)").arg(value.toString())});
    }
}

void ConfigFlightModesViewModel::updateStatus()
{
    if (m_family == Family::Unsupported) {
        m_status = tr("Flight modes are unsupported for this vehicle.");
    } else if (!m_connected) {
        m_status = tr("offline");
    } else if (m_reconciliationRequired) {
        m_status = m_operationStatus.isEmpty()
            ? tr("Flight mode state is uncertain; a full parameter refresh "
                 "is required.")
            : m_operationStatus;
    } else if (m_pending.active) {
        m_status = m_pending.failureReason.isEmpty()
            ? tr("Writing flight modes…")
            : tr("A flight mode parameter failed; waiting for the complete "
                 "batch result…");
    } else if (!m_heartbeatFresh) {
        m_status = tr("Waiting for a fresh vehicle heartbeat.");
    } else if (m_armed) {
        m_status = tr("Vehicle is armed; flight mode changes are disabled.");
    } else if (!m_snapshotReady) {
        m_status = tr("A complete six-mode parameter snapshot is required.");
    } else if (!m_operationStatus.isEmpty()) {
        m_status = m_operationStatus;
    } else if (Dirty()) {
        m_status = tr("Flight mode changes are staged; select Save to apply.");
    } else {
        m_status.clear();
    }
}

void ConfigFlightModesViewModel::finishSuccess()
{
    if (!m_pending.active) {
        return;
    }
    const QVariantList changes = m_pending.changes;
    for (const QVariant &item : changes) {
        const QVariantMap entry = item.toMap();
        const QString name = normalizedName(
            entry.value(QStringLiteral("name")).toString());
        const QVariant value = entry.value(QStringLiteral("value"));
        const int row = modeRow(name);
        if (row >= 0) {
            m_rows[row].acceptedMode = value;
            m_rows[row].mode = value;
        } else if (name == QLatin1String("SIMPLE")) {
            m_simpleValue = static_cast<quint32>(value.toULongLong());
            applySimpleValue(m_simpleValue, false, false);
        } else if (name == QLatin1String("SUPER_SIMPLE")) {
            m_superSimpleValue = static_cast<quint32>(value.toULongLong());
            applySimpleValue(m_superSimpleValue, true, false);
        }
    }
    m_pending = {};
    m_operationStatus = tr("Flight modes saved.");
    rebuildOptions();
    updateStatus();
    emit optionsChanged();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigFlightModesViewModel::finishFailure(const QString &reason)
{
    if (!m_pending.active) {
        return;
    }
    m_pending = {};
    m_operationStatus = reason.trimmed().isEmpty()
        ? tr("write failed") : reason.trimmed();
    updateStatus();
    emit stateChanged();
}

void ConfigFlightModesViewModel::finishUncertain(
    const QString &reason, bool requestRefresh)
{
    if (!m_pending.active) {
        return;
    }
    const int componentId = m_componentId;
    ++m_snapshotGeneration;
    m_pending = {};
    m_snapshotComplete = false;
    m_snapshotReady = false;
    m_reconciliationRequired = true;
    m_operationStatus = reason;
    clearSnapshotPresentation();
    rebuildOptions();
    updateStatus();
    const quint64 snapshotGeneration = m_snapshotGeneration;
    const QPointer<ConfigFlightModesViewModel> guard(this);
    emit optionsChanged();
    if (!guard || m_snapshotGeneration != snapshotGeneration
        || !m_reconciliationRequired) {
        return;
    }
    emit rowsChanged();
    if (!guard || m_snapshotGeneration != snapshotGeneration
        || !m_reconciliationRequired) {
        return;
    }
    emit stateChanged();
    if (guard && m_snapshotGeneration == snapshotGeneration
        && m_reconciliationRequired && requestRefresh && m_connected) {
        emit refreshRequested(componentId);
    }
}

bool ConfigFlightModesViewModel::pendingContains(const QString &name) const
{
    const QString wanted = normalizedName(name);
    for (const QVariant &item : m_pending.changes) {
        if (normalizedName(item.toMap()
                               .value(QStringLiteral("name"))
                               .toString()) == wanted) {
            return true;
        }
    }
    return false;
}

int ConfigFlightModesViewModel::modeRow(
    const QString &normalized) const
{
    const QString prefix = ModePrefix(m_family);
    if (prefix.isEmpty()) {
        return -1;
    }
    for (int row = 0; row < 6; ++row) {
        if (normalized == prefix + QString::number(row + 1)) {
            return row;
        }
    }
    return -1;
}

quint32 ConfigFlightModesViewModel::stagedSimpleValue(
    bool superSimple) const
{
    quint32 lowerBits = 0;
    for (int row = 0; row < m_rows.size(); ++row) {
        const bool enabled = superSimple
            ? m_rows.at(row).superSimple : m_rows.at(row).simple;
        if (enabled) {
            lowerBits |= quint32(1U << row);
        }
    }
    const quint32 accepted = superSimple
        ? m_superSimpleValue : m_simpleValue;
    return (accepted & ~kEditableSimpleBits)
        | (lowerBits & kEditableSimpleBits);
}

void ConfigFlightModesViewModel::applySimpleValue(
    quint32 value, bool superSimple, bool preserveDirtyBits)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        const bool accepted = (value & quint32(1U << row)) != 0;
        if (superSimple) {
            const bool dirty = m_rows[row].superSimpleDirty();
            m_rows[row].acceptedSuperSimple = accepted;
            if (!preserveDirtyBits || !dirty) {
                m_rows[row].superSimple = accepted;
            }
        } else {
            const bool dirty = m_rows[row].simpleDirty();
            m_rows[row].acceptedSimple = accepted;
            if (!preserveDirtyBits || !dirty) {
                m_rows[row].simple = accepted;
            }
        }
    }
}

QString ConfigFlightModesViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigFlightModesViewModel::valuesEqual(
    const QVariant &left, const QVariant &right)
{
    if (!left.isValid() || !right.isValid()) {
        return !left.isValid() && !right.isValid();
    }
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

bool ConfigFlightModesViewModel::optionListsEqual(
    const QList<ParamOption> &left, const QList<ParamOption> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (int index = 0; index < left.size(); ++index) {
        if (!valuesEqual(left.at(index).value, right.at(index).value)
            || left.at(index).text != right.at(index).text) {
            return false;
        }
    }
    return true;
}
