#include "ConfigTradHeliViewModel.h"

#include <QMetaType>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kWriteTimeoutMs = 5000;

struct LegacyRange
{
    double minimum;
    double maximum;
    double increment;
    bool trusted;
};

LegacyRange fallbackRange(const QString &name)
{
    // Exact bounds from the original ConfigTradHeli control setup. Unknown
    // names are never made writable without matching firmware metadata.
    const QString normalized = name.toUpper();
    if (normalized == QLatin1String("H_RSC_MAX")
        || normalized == QLatin1String("H_RSC_PWM_MAX")
        || normalized == QLatin1String("H_RSC_MIN")
        || normalized == QLatin1String("H_RSC_PWM_MIN")
        || normalized == QLatin1String("H_RSC_SETPOINT")
        || normalized == QLatin1String("H_COL_MIN")
        || normalized == QLatin1String("H_COL_MID")
        || normalized == QLatin1String("H_COL_MAX")
        || normalized == QLatin1String("HS4_MIN")
        || normalized == QLatin1String("SERVO4_MIN")
        || normalized == QLatin1String("HS4_MAX")
        || normalized == QLatin1String("SERVO4_MAX")
        || normalized.endsWith(QLatin1String("_TRIM"))) {
        return {800.0, 2200.0, 1.0, true};
    }
    if (normalized == QLatin1String("IM_STAB_COL_1")
        || normalized == QLatin1String("IM_STAB_COL_2")
        || normalized == QLatin1String("IM_STAB_COL_3")
        || normalized == QLatin1String("IM_STAB_COL_4")
        || normalized == QLatin1String("IM_STB_COL_1")
        || normalized == QLatin1String("IM_STB_COL_2")
        || normalized == QLatin1String("IM_STB_COL_3")
        || normalized == QLatin1String("IM_STB_COL_4")
        || normalized == QLatin1String("H_TAIL_SPEED")
        || normalized == QLatin1String("H_LAND_COL_MIN")
        || normalized == QLatin1String("H_GYR_GAIN")) {
        return {0.0, 1000.0, 1.0, true};
    }
    if (normalized == QLatin1String("IM_ACRO_COL_EXP")) {
        return {0.0, 1.0, 0.01, true};
    }
    if (normalized == QLatin1String("H_COLYAW")) {
        return {0.0, 5.0, 0.01, true};
    }
    if (normalized == QLatin1String("H_RSC_RAMP_TIME")
        || normalized == QLatin1String("H_RSC_RUNUP_TIME")) {
        return {0.0, 60.0, 1.0, true};
    }
    if (normalized == QLatin1String("H_SV1_POS")
        || normalized == QLatin1String("H_SV2_POS")
        || normalized == QLatin1String("H_SV3_POS")) {
        return {-180.0, 180.0, 1.0, true};
    }
    if (normalized == QLatin1String("H_RSC_REV")
        || normalized == QLatin1String("H_RSC_PWM_REV")
        || (normalized.startsWith(QLatin1String("HS"))
            && normalized.endsWith(QLatin1String("_REV")))
        || (normalized.startsWith(QLatin1String("H_SV"))
            && normalized.endsWith(QLatin1String("_REV")))) {
        return {-1.0, 1.0, 1.0, true};
    }
    if (normalized.endsWith(QLatin1String("_REVERSED"))
        || normalized == QLatin1String("ATC_PIRO_COMP")) {
        return {0.0, 1.0, 1.0, true};
    }
    if (normalized == QLatin1String("H_PHANG")
        || normalized == QLatin1String("H_SV_TEST")
        || normalized == QLatin1String("ATC_HOVR_ROL_TRM")
        || normalized == QLatin1String("H_CYC_MAX")
        || normalized == QLatin1String("H_RSC_CRITICAL")
        || normalized == QLatin1String("H_RSC_POWER_HIGH")
        || normalized == QLatin1String("H_RSC_POWER_LOW")
        || normalized == QLatin1String("H_RSC_IDLE")) {
        return {0.0, 99.0, 1.0, true};
    }
    return {0.0, 0.0, 1.0, false};
}

QString fallbackLabel(const QString &name)
{
    return name;
}

bool hasOption(const QList<ParamOption> &options, const QVariant &value)
{
    bool wantedOk = false;
    const double wanted = value.toDouble(&wantedOk);
    for (const ParamOption &option : options) {
        bool optionOk = false;
        const double candidate = option.value.toDouble(&optionOk);
        if (wantedOk && optionOk
            && std::abs(wanted - candidate) <= 1.0e-6) {
            return true;
        }
        if (!wantedOk && !optionOk && option.value == value) {
            return true;
        }
    }
    return false;
}
} // namespace

ConfigTradHeliViewModel::ConfigTradHeliViewModel(QObject *parent)
    : QObject(parent),
      m_visualizationTimer(new QTimer(this))
{
    m_visualizationTimer->setObjectName(
        QStringLiteral("tradHeliVisualizationTimer"));
    m_visualizationTimer->setInterval(100);
    connect(m_visualizationTimer, &QTimer::timeout,
            this, &ConfigTradHeliViewModel::pumpVisualization);
    rebuildFields();
    pumpVisualization();
}

QList<QStringList> ConfigTradHeliViewModel::FieldCandidates()
{
    return {
        {QStringLiteral("H_PHANG")},
        {QStringLiteral("ATC_PIRO_COMP")},
        {QStringLiteral("H_SV_TEST")},
        {QStringLiteral("ATC_HOVR_ROL_TRM")},
        {QStringLiteral("H_CYC_MAX")},
        {QStringLiteral("H_RSC_CRITICAL")},
        {QStringLiteral("H_RSC_MAX"), QStringLiteral("H_RSC_PWM_MAX")},
        {QStringLiteral("H_RSC_MIN"), QStringLiteral("H_RSC_PWM_MIN")},
        {QStringLiteral("H_RSC_REV"), QStringLiteral("H_RSC_PWM_REV")},
        {QStringLiteral("H_RSC_POWER_HIGH")},
        {QStringLiteral("H_RSC_POWER_LOW")},
        {QStringLiteral("H_RSC_IDLE")},
        {QStringLiteral("IM_STAB_COL_1"), QStringLiteral("IM_STB_COL_1")},
        {QStringLiteral("IM_STAB_COL_2"), QStringLiteral("IM_STB_COL_2")},
        {QStringLiteral("IM_STAB_COL_3"), QStringLiteral("IM_STB_COL_3")},
        {QStringLiteral("IM_STAB_COL_4"), QStringLiteral("IM_STB_COL_4")},
        {QStringLiteral("IM_ACRO_COL_EXP")},
        {QStringLiteral("H_TAIL_TYPE")},
        {QStringLiteral("H_TAIL_SPEED")},
        {QStringLiteral("H_LAND_COL_MIN")},
        {QStringLiteral("H_COLYAW")},
        {QStringLiteral("H_RSC_RAMP_TIME")},
        {QStringLiteral("H_RSC_RUNUP_TIME")},
        {QStringLiteral("H_RSC_MODE")},
        {QStringLiteral("H_RSC_SETPOINT")},
        {QStringLiteral("H_GYR_GAIN")},
        {QStringLiteral("H_COL_MIN")},
        {QStringLiteral("H_COL_MID")},
        {QStringLiteral("H_COL_MAX")},
        {QStringLiteral("HS4_MIN"), QStringLiteral("SERVO4_MIN")},
        {QStringLiteral("HS4_MAX"), QStringLiteral("SERVO4_MAX")},
        {QStringLiteral("H_SV1_POS")},
        {QStringLiteral("H_SV2_POS")},
        {QStringLiteral("H_SV3_POS")},
        {QStringLiteral("HS1_REV"), QStringLiteral("H_SV1_REV"),
         QStringLiteral("SERVO1_REVERSED")},
        {QStringLiteral("HS2_REV"), QStringLiteral("H_SV2_REV"),
         QStringLiteral("SERVO2_REVERSED")},
        {QStringLiteral("HS3_REV"), QStringLiteral("H_SV3_REV"),
         QStringLiteral("SERVO3_REVERSED")},
        {QStringLiteral("HS4_REV"), QStringLiteral("H_SV4_REV"),
         QStringLiteral("SERVO4_REVERSED")},
        {QStringLiteral("H_FLYBAR_MODE")},
        {QStringLiteral("HS1_TRIM"), QStringLiteral("H_SV1_TRIM"),
         QStringLiteral("SERVO1_TRIM")},
        {QStringLiteral("HS2_TRIM"), QStringLiteral("H_SV2_TRIM"),
         QStringLiteral("SERVO2_TRIM")},
        {QStringLiteral("HS3_TRIM"), QStringLiteral("H_SV3_TRIM"),
         QStringLiteral("SERVO3_TRIM")},
        {QStringLiteral("HS4_TRIM"), QStringLiteral("H_SV4_TRIM"),
         QStringLiteral("SERVO4_TRIM")}
    };
}

QStringList ConfigTradHeliViewModel::ReferenceFieldNames()
{
    QStringList result;
    for (const QStringList &candidates : FieldCandidates()) {
        result.append(candidates.constFirst());
    }
    return result;
}

int ConfigTradHeliViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

QString ConfigTradHeliViewModel::Title() const
{
    return tr("Heli Setup");
}

QString ConfigTradHeliViewModel::Intro() const
{
    return tr("Traditional helicopter swashplate and rotor speed setup. "
              "Remove blades before testing servos.");
}

void ConfigTradHeliViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    rebuildFields();
}

void ConfigTradHeliViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> legacyHeliComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (normalizedName(parameter.name)
            == QLatin1String("H_SWASH_TYPE")) {
            legacyHeliComponents.insert(parameter.componentId);
        }
    }
    if (legacyHeliComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (legacyHeliComponents.contains(1)) {
        m_componentId = 1;
    } else if (!legacyHeliComponents.isEmpty()) {
        QList<int> components = legacyHeliComponents.values();
        std::sort(components.begin(), components.end());
        m_componentId = components.constFirst();
    } else {
        m_componentId = preferredComponent;
    }

    ++m_snapshotGeneration;
    m_pendingWrites.clear();
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    m_snapshotReady = !m_values.isEmpty();
    m_swashParameter = m_values.contains(QStringLiteral("H_SWASH_TYPE"))
        ? QStringLiteral("H_SWASH_TYPE") : QString();
    m_manualParameter = m_values.contains(QStringLiteral("H_SV_MAN"))
        ? QStringLiteral("H_SV_MAN") : QString();
    m_status.clear();
    m_servoStatus.clear();
    resetObservedRanges();
    rebuildFields();
    pumpVisualization();
    emit stateChanged();
}

void ConfigTradHeliViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    const bool manualMayBeActive = ManualOverrideMayBeActive();
    m_connected = connected;
    if (!connected) {
        ++m_snapshotGeneration;
        m_pendingWrites.clear();
        if (manualMayBeActive) {
            emit manualSafetyWarning(
                tr("The link was lost while manual servo override was active. "
                   "Verify H_SV_MAN is disabled before flight."));
        }
    }
    emit stateChanged();
}

void ConfigTradHeliViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    emit stateChanged();
}

void ConfigTradHeliViewModel::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (active) {
        resetObservedRanges();
        pumpVisualization();
        m_visualizationTimer->start();
    } else {
        m_visualizationTimer->stop();
        if (ManualOverrideMayBeActive() && !setManualServoMode(0)) {
            emit manualSafetyWarning(
                tr("Manual servo override may still be active. Disable "
                   "H_SV_MAN before flight."));
        }
    }
    emit stateChanged();
}

bool ConfigTradHeliViewModel::HasLegacySwash() const
{
    return !m_swashParameter.isEmpty();
}

bool ConfigTradHeliViewModel::SwashIsCcpm() const
{
    const auto pending = m_pendingWrites.constFind(m_swashParameter);
    const QVariant value = pending == m_pendingWrites.constEnd()
        ? m_values.value(m_swashParameter, 0)
        : pending->expectedValue;
    return value.toDouble() == 0.0;
}

bool ConfigTradHeliViewModel::ManualServoActive() const
{
    if (m_manualParameter.isEmpty()) {
        return false;
    }
    const auto pending = m_pendingWrites.constFind(m_manualParameter);
    const QVariant value = pending == m_pendingWrites.constEnd()
        ? m_values.value(m_manualParameter)
        : pending->expectedValue;
    return value.toDouble() != 0.0;
}

bool ConfigTradHeliViewModel::ManualOverrideMayBeActive() const
{
    if (m_manualOverrideUncertain) {
        return true;
    }
    if (m_manualParameter.isEmpty()) {
        return false;
    }
    if (m_values.value(m_manualParameter).toDouble() != 0.0) {
        return true;
    }
    const auto pending = m_pendingWrites.constFind(m_manualParameter);
    return pending != m_pendingWrites.constEnd();
}

bool ConfigTradHeliViewModel::ManualModeSupported(int mode) const
{
    if (m_manualParameter.isEmpty() || mode < 0 || mode > 5) {
        return false;
    }
    if (mode == 0) {
        return true;
    }
    const ParameterMetaData metadata =
        m_catalog.value(m_manualParameter);
    if (!metadata.values.isEmpty()) {
        for (const ParameterMetaDataOption &option : metadata.values) {
            if (std::abs(option.value.toDouble() - mode) <= 1.0e-6) {
                return true;
            }
        }
        return false;
    }
    // Modes 1..4 are common to legacy and current ArduCopter. The legacy-only
    // test mode 5 is never guessed without an explicit capability declaration.
    return mode >= 1 && mode <= 4;
}

QVector<HeliVisualization::CurvePoint>
ConfigTradHeliViewModel::StabilizeCurve() const
{
    return HeliVisualization::BuildStabilizeCurve(
        numericValue({QStringLiteral("IM_STAB_COL_1"),
                      QStringLiteral("IM_STB_COL_1")}, 0.0),
        numericValue({QStringLiteral("IM_STAB_COL_2"),
                      QStringLiteral("IM_STB_COL_2")}, 400.0),
        numericValue({QStringLiteral("IM_STAB_COL_3"),
                      QStringLiteral("IM_STB_COL_3")}, 600.0),
        numericValue({QStringLiteral("IM_STAB_COL_4"),
                      QStringLiteral("IM_STB_COL_4")}, 1000.0));
}

QVector<HeliVisualization::CurvePoint>
ConfigTradHeliViewModel::AcroCurve() const
{
    return HeliVisualization::BuildAcroCurve(
        numericValue({QStringLiteral("IM_ACRO_COL_EXP")}, 0.0));
}

double ConfigTradHeliViewModel::CollectiveCursorPercent() const
{
    return HeliVisualization::MapCollectiveCursor(
        m_collectiveOutput,
        numericValue({QStringLiteral("H_COL_MIN")}, 1000.0),
        numericValue({QStringLiteral("H_COL_MAX")}, 2000.0));
}

double ConfigTradHeliViewModel::Servo1Position() const
{
    return numericValue({QStringLiteral("H_SV1_POS")}, 0.0);
}

double ConfigTradHeliViewModel::Servo2Position() const
{
    return numericValue({QStringLiteral("H_SV2_POS")}, 0.0);
}

double ConfigTradHeliViewModel::Servo3Position() const
{
    return numericValue({QStringLiteral("H_SV3_POS")}, 0.0);
}

QString ConfigTradHeliViewModel::CollectiveRangeText() const
{
    if (!m_collectiveRange.hasSamples()) {
        return tr("Collective range: waiting for manual mode");
    }
    return tr("Collective observed: %1–%2 µs")
        .arg(m_collectiveRange.minimum, 0, 'f', 0)
        .arg(m_collectiveRange.maximum, 0, 'f', 0);
}

QString ConfigTradHeliViewModel::RudderRangeText() const
{
    if (!m_rudderRange.hasSamples()) {
        return tr("Rudder range: waiting for manual mode");
    }
    return tr("Rudder observed: %1–%2 µs")
        .arg(m_rudderRange.minimum, 0, 'f', 0)
        .arg(m_rudderRange.maximum, 0, 'f', 0);
}

bool ConfigTradHeliViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    ParamField *field = fieldForName(name);
    if (!field || field->readOnly || !m_connected || !m_snapshotReady
        || m_armed || !value.isValid()) {
        return false;
    }
    bool numericOk = false;
    const double numeric = value.toDouble(&numericOk);
    if (field->hasRange && field->enforceRange
        && (!numericOk || !std::isfinite(numeric)
            || numeric < field->minimum || numeric > field->maximum)) {
        return false;
    }
    if (field->editorKind == ParamField::EditorKind::Combo
        && !field->options.isEmpty()
        && !hasOption(field->options, value)) {
        return false;
    }
    const QVariant candidate = typedValue(value, m_values.value(field->name));
    if (valueEquals(field->value, candidate)) {
        return false;
    }
    field->value = candidate;
    field->status = QStringLiteral("…");
    emit fieldChanged(field->name);
    return queueWrite(WriteKind::Field, field->name, candidate);
}

bool ConfigTradHeliViewModel::setSwashCcpm(bool ccpm)
{
    if (!HasLegacySwash() || !m_connected || !m_snapshotReady || m_armed) {
        return false;
    }
    const int value = ccpm ? 0 : 1;
    if (valueEquals(m_values.value(m_swashParameter), value)
        && !m_pendingWrites.contains(m_swashParameter)) {
        return false;
    }
    setServoStatus(tr("Setting swashplate type…"));
    return queueWrite(WriteKind::Swash, m_swashParameter, value);
}

bool ConfigTradHeliViewModel::setManualServoMode(int mode)
{
    if (!m_connected || !m_snapshotReady || !ManualModeSupported(mode)
        || (mode != 0 && m_armed)) {
        return false;
    }
    if (valueEquals(m_values.value(m_manualParameter), mode)
        && !m_pendingWrites.contains(m_manualParameter)
        && !(mode == 0 && m_manualOverrideUncertain)) {
        return false;
    }
    setServoStatus(mode == 0
        ? tr("Disabling manual servo override…")
        : tr("Setting H_SV_MAN=%1…").arg(mode));
    return queueWrite(WriteKind::ManualServo, m_manualParameter, mode);
}

bool ConfigTradHeliViewModel::Refresh()
{
    if (!m_connected || !m_snapshotReady || HasPendingWrites()) {
        return false;
    }
    setStatus(tr("Refreshing parameters…"));
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigTradHeliViewModel::setRcInput(
    int zeroBasedChannel, double pwm)
{
    if (!std::isfinite(pwm) || pwm < 800.0 || pwm > 2200.0) {
        return;
    }
    if (zeroBasedChannel == 2) {
        m_collectiveInput = pwm;
    } else if (zeroBasedChannel == 3) {
        m_rudderInput = pwm;
    }
}

void ConfigTradHeliViewModel::setServoOutput(
    int oneBasedChannel, int pwm)
{
    if (oneBasedChannel == 6 && pwm >= 800 && pwm <= 2200) {
        m_collectiveOutput = pwm;
    }
}

void ConfigTradHeliViewModel::resetObservedRanges()
{
    m_collectiveRange = {2200.0, 800.0};
    m_rudderRange = {2200.0, 800.0};
    emit visualizationChanged();
}

void ConfigTradHeliViewModel::pumpVisualization()
{
    updateVisualization();
}

void ConfigTradHeliViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    bool relevant = normalized == QLatin1String("H_SWASH_TYPE")
        || normalized == QLatin1String("H_SV_MAN");
    for (const QStringList &candidates : FieldCandidates()) {
        if (candidates.contains(normalized)) {
            relevant = true;
            break;
        }
    }
    if (!relevant) {
        return;
    }

    const bool wasPresent = m_values.contains(normalized);
    const bool hasPending = m_pendingWrites.contains(normalized);
    m_values.insert(normalized, value);
    if (normalized == QLatin1String("H_SWASH_TYPE")) {
        m_swashParameter = normalized;
    } else if (normalized == QLatin1String("H_SV_MAN")) {
        m_manualParameter = normalized;
    }

    if (!wasPresent) {
        rebuildFields();
    } else if (!hasPending) {
        ParamField *field = fieldForName(normalized);
        if (field) {
            field->value = value;
            field->status.clear();
            emit fieldChanged(normalized);
        }
    }
    if (normalized == QLatin1String("H_SV_MAN")
        && !m_active && value.toDouble() != 0.0 && !hasPending) {
        emit manualSafetyWarning(
            tr("Manual servo override is active while Heli Setup is not "
               "active. Disable H_SV_MAN before flight."));
    }
    pumpVisualization();
    emit stateChanged();
}

void ConfigTradHeliViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    auto pending = pendingForRequest(requestId);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    if (batchId == 0) {
        const QString name = pending.key();
        finishPending(name, tr("write was rejected"), true);
        return;
    }
    pending->batchId = batchId;
}

void ConfigTradHeliViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    auto pending = pendingForRequest(requestId);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const QString name = pending.key();
    finishPending(name,
                  reason.isEmpty() ? tr("write failed") : reason, true);
}

void ConfigTradHeliViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (componentId != m_componentId) {
        return;
    }
    auto pending = pendingForBatch(batchId);
    if (pending == m_pendingWrites.end()
        || pending.key() != normalizedName(name)) {
        return;
    }
    const QString pendingName = pending.key();
    finishPending(pendingName,
                  reason.isEmpty() ? tr("write failed") : reason, true);
}

void ConfigTradHeliViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    auto pending = pendingForBatch(batchId);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const QString name = pending.key();
    if (succeeded == 1 && failed == 0) {
        finishPendingSuccess(name);
    } else {
        finishPending(name, tr("write failed"), true);
    }
}

void ConfigTradHeliViewModel::refreshFailed(const QString &reason)
{
    setStatus(reason.isEmpty() ? tr("parameter refresh failed") : reason);
}

void ConfigTradHeliViewModel::refreshCanceled()
{
    setStatus(tr("parameter refresh canceled"));
}

void ConfigTradHeliViewModel::rebuildFields()
{
    m_fields.clear();
    const QList<QStringList> candidates = FieldCandidates();
    m_fields.reserve(candidates.size());
    for (int index = 0; index < candidates.size(); ++index) {
        m_fields.append(makeField(candidates.at(index), index));
    }
    emit structureChanged();
}

ParamField ConfigTradHeliViewModel::makeField(
    const QStringList &candidates, int index) const
{
    Q_UNUSED(index)
    ParamField field;
    field.componentId = m_componentId;
    field.name = resolve(candidates);
    field.value = m_values.value(field.name, 0);
    const ParameterMetaData metadata = metadataFor(candidates, field.name);
    field.label = metadata.title.isEmpty()
        ? fallbackLabel(field.name) : metadata.title;
    field.units = metadata.units;
    field.description = metadata.description;
    field.readOnly = !m_values.contains(field.name) || metadata.readOnly;
    field.status = m_values.contains(field.name) ? QString() : tr("n/a");

    for (const ParameterMetaDataOption &option : metadata.values) {
        field.options.append({option.value, option.label});
    }
    const QString primary = candidates.constFirst();
    if (primary == QLatin1String("ATC_PIRO_COMP")
        && field.options.isEmpty()) {
        field.options = {{0, tr("Disabled")}, {1, tr("Enabled")}};
    }
    if (primary == QLatin1String("H_FLYBAR_MODE")
        && field.options.isEmpty()) {
        field.options = {{0, tr("Disabled")}, {1, tr("Enabled")}};
    }
    const bool explicitCombo = primary == QLatin1String("H_TAIL_TYPE")
        || primary == QLatin1String("H_RSC_MODE")
        || primary == QLatin1String("H_FLYBAR_MODE")
        || primary == QLatin1String("ATC_PIRO_COMP");
    field.editorKind = (!field.options.isEmpty() || explicitCombo)
        ? ParamField::EditorKind::Combo
        : ParamField::EditorKind::Numeric;
    if (explicitCombo && field.options.isEmpty()) {
        // Guessing enum codes is unsafe. Keep the reference row visible but
        // unavailable until matching metadata describes its choices.
        field.readOnly = true;
        field.status = tr("metadata required");
    }

    if (field.editorKind == ParamField::EditorKind::Numeric) {
        const LegacyRange fallback = fallbackRange(field.name);
        field.hasRange = true;
        field.enforceRange = true;
        field.minimum = fallback.minimum;
        field.maximum = fallback.maximum;
        field.increment = fallback.increment;
        if (!fallback.trusted) {
            field.readOnly = true;
            field.status = tr("metadata required");
        }
        if (metadata.hasRange && m_enforceMetadataRanges) {
            field.minimum = metadata.minimum;
            field.maximum = metadata.maximum;
        }
        if (metadata.hasIncrement && metadata.increment > 0.0) {
            field.increment = metadata.increment;
        }
    }
    return field;
}

ParamField *ConfigTradHeliViewModel::fieldForName(const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

QString ConfigTradHeliViewModel::resolve(
    const QStringList &candidates) const
{
    for (const QString &candidate : candidates) {
        const QString normalized = normalizedName(candidate);
        if (m_values.contains(normalized)) {
            return normalized;
        }
    }
    return normalizedName(candidates.constFirst());
}

ParameterMetaData ConfigTradHeliViewModel::metadataFor(
    const QStringList &candidates, const QString &resolvedName) const
{
    Q_UNUSED(candidates)
    ParameterMetaData metadata = m_catalog.value(resolvedName);
    return metadata;
}

QVariant ConfigTradHeliViewModel::valueFor(
    const QStringList &candidates, const QVariant &fallback) const
{
    for (const QString &candidate : candidates) {
        const auto iterator = m_values.constFind(normalizedName(candidate));
        if (iterator != m_values.constEnd()) {
            return iterator.value();
        }
    }
    return fallback;
}

double ConfigTradHeliViewModel::numericValue(
    const QStringList &candidates, double fallback) const
{
    bool ok = false;
    const double value = valueFor(candidates, fallback).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

QVariant ConfigTradHeliViewModel::typedValue(
    const QVariant &value, const QVariant &reference) const
{
    const double numeric = value.toDouble();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::Int:
        return static_cast<int>(std::nearbyint(numeric));
    case QMetaType::UInt:
        return static_cast<uint>(std::nearbyint(numeric));
    case QMetaType::LongLong:
        return static_cast<qlonglong>(std::nearbyint(numeric));
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(std::nearbyint(numeric));
    case QMetaType::Float:
        return static_cast<float>(numeric);
    default:
        return numeric;
    }
}

bool ConfigTradHeliViewModel::queueWrite(
    WriteKind kind, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        return false;
    }
    const auto previous = m_pendingWrites.constFind(normalized);
    if (previous != m_pendingWrites.constEnd()) {
        if (valueEquals(previous->expectedValue, value)) {
            return false;
        }
    }
    PendingWrite pending;
    pending.kind = kind;
    pending.expectedValue = value;
    pending.generation = ++m_writeGeneration;
    pending.snapshotGeneration = m_snapshotGeneration;
    pending.manualUncertainBefore = m_manualOverrideUncertain;
    m_pendingWrites.insert(normalized, pending);
    if (kind == WriteKind::ManualServo) {
        // PARAM_SET may reach the flight controller even if its ACK is lost,
        // and a requested disable is not proof that an already-active mode
        // stopped. Keep a conservative latch until an exact zero batch
        // succeeds.
        m_manualOverrideUncertain = true;
    }
    emit stateChanged();
    emit writeRequested(pending.generation, m_componentId,
                        normalized, value);

    const quint64 generation = pending.generation;
    const quint64 snapshotGeneration = pending.snapshotGeneration;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, normalized, generation, snapshotGeneration]() {
        const auto iterator = m_pendingWrites.constFind(normalized);
        if (iterator == m_pendingWrites.constEnd()
            || iterator->generation != generation
            || iterator->snapshotGeneration != snapshotGeneration
            || iterator->batchId != 0
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        // This watchdog only covers a missing/rejected submission callback.
        // Once ParameterService returns a batch ID, its retry state machine is
        // the sole terminal owner and may legitimately take longer than 5 s.
        finishPending(normalized, tr("write timeout"), true);
    });
    return true;
}

void ConfigTradHeliViewModel::finishPendingSuccess(const QString &name)
{
    auto pending = m_pendingWrites.find(name);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const WriteKind kind = pending->kind;
    const QVariant value = pending->expectedValue;
    m_values.insert(name, value);
    m_pendingWrites.erase(pending);
    if (kind == WriteKind::ManualServo && value.toDouble() == 0.0) {
        m_manualOverrideUncertain = false;
    }
    if (ParamField *field = fieldForName(name)) {
        field->value = value;
        field->status = QStringLiteral("✓");
        emit fieldChanged(name);
    }
    if (kind == WriteKind::Swash || kind == WriteKind::ManualServo) {
        setServoStatus(tr("%1 set").arg(name));
    } else {
        setStatus(QString());
    }
    pumpVisualization();
    emit stateChanged();
}

QHash<QString, ConfigTradHeliViewModel::PendingWrite>::iterator
ConfigTradHeliViewModel::pendingForRequest(quint64 requestId)
{
    for (auto iterator = m_pendingWrites.begin();
         iterator != m_pendingWrites.end(); ++iterator) {
        if (iterator->generation == requestId
            && iterator->snapshotGeneration == m_snapshotGeneration) {
            return iterator;
        }
    }
    return m_pendingWrites.end();
}

QHash<QString, ConfigTradHeliViewModel::PendingWrite>::iterator
ConfigTradHeliViewModel::pendingForBatch(qulonglong batchId)
{
    if (batchId == 0) {
        return m_pendingWrites.end();
    }
    for (auto iterator = m_pendingWrites.begin();
         iterator != m_pendingWrites.end(); ++iterator) {
        if (iterator->batchId == batchId
            && iterator->snapshotGeneration == m_snapshotGeneration) {
            return iterator;
        }
    }
    return m_pendingWrites.end();
}

void ConfigTradHeliViewModel::finishPending(
    const QString &name, const QString &error, bool applyAuthoritativeValue)
{
    auto pending = m_pendingWrites.find(name);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const WriteKind kind = pending->kind;
    const bool wasSubmitted = pending->batchId != 0;
    const bool manualUncertainBefore = pending->manualUncertainBefore;
    const bool failedManualEnable = kind == WriteKind::ManualServo
        && pending->expectedValue.toDouble() != 0.0;
    const bool failedSafetyDisable = kind == WriteKind::ManualServo
        && pending->expectedValue.toDouble() == 0.0;
    m_pendingWrites.erase(pending);
    if (failedManualEnable && !wasSubmitted) {
        // Submission rejection proves that this particular write never
        // received a transport batch. Preserve any uncertainty that existed
        // before it, but do not manufacture a new one.
        m_manualOverrideUncertain = manualUncertainBefore;
    }
    if (ParamField *field = fieldForName(name)) {
        if (applyAuthoritativeValue) {
            field->value = m_values.value(name, field->value);
        }
        field->status = error;
        emit fieldChanged(name);
    }
    if (kind == WriteKind::Swash || kind == WriteKind::ManualServo) {
        setServoStatus(error);
    } else {
        setStatus(error);
    }
    if (failedSafetyDisable) {
        emit manualSafetyWarning(
            tr("Manual servo override may still be active. Verify "
               "H_SV_MAN is disabled before flight."));
    }
    emit stateChanged();
}

void ConfigTradHeliViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit stateChanged();
}

void ConfigTradHeliViewModel::setServoStatus(const QString &status)
{
    if (m_servoStatus == status) {
        return;
    }
    m_servoStatus = status;
    emit stateChanged();
}

QString ConfigTradHeliViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}

bool ConfigTradHeliViewModel::valueEquals(
    const QVariant &left, const QVariant &right) const
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue
            || std::abs(leftValue - rightValue) <= 1.0e-6;
    }
    return left == right;
}

void ConfigTradHeliViewModel::updateVisualization()
{
    const bool manual = ManualServoActive();
    m_collectiveRange = HeliVisualization::CaptureRange(
        m_collectiveRange, m_collectiveInput, manual);
    m_rudderRange = HeliVisualization::CaptureRange(
        m_rudderRange, m_rudderInput, manual);
    emit visualizationChanged();
}
