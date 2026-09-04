#include "ConfigTradHeli4ViewModel.h"

#include <QMetaType>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;

QString servoName(int channel, const QString &suffix)
{
    return QStringLiteral("SERVO%1_%2").arg(channel).arg(suffix);
}

bool isServoPwmField(const QString &name)
{
    return name.startsWith(QStringLiteral("SERVO"))
        && (name.endsWith(QStringLiteral("_MIN"))
            || name.endsWith(QStringLiteral("_TRIM"))
            || name.endsWith(QStringLiteral("_MAX")));
}

QString servoTemplateName(const QString &name)
{
    const int separator = name.indexOf(QLatin1Char('_'));
    if (!name.startsWith(QStringLiteral("SERVO")) || separator < 0) {
        return name;
    }
    return QStringLiteral("SERVO1") + name.mid(separator);
}

QList<ParamOption> currentHeliEnumFallback(const QString &name)
{
    if (name == QLatin1String("H_SV_MAN")) {
        return {{0, QObject::tr("Disabled")},
                {1, QObject::tr("Passthrough")},
                {2, QObject::tr("Max collective")},
                {3, QObject::tr("Zero thrust collective")},
                {4, QObject::tr("Min collective")}};
    }
    if (name == QLatin1String("H_SW_TYPE")) {
        return {{0, QStringLiteral("H3 Generic")},
                {1, QStringLiteral("H1 non-CPPM")},
                {2, QStringLiteral("H3_140")},
                {3, QStringLiteral("H3_120")},
                {4, QStringLiteral("H4_90")},
                {5, QStringLiteral("H4_45")}};
    }
    if (name == QLatin1String("H_SW_COL_DIR")) {
        return {{0, QObject::tr("Normal")},
                {1, QObject::tr("Reversed")}};
    }
    if (name == QLatin1String("H_SW_LIN_SVO")) {
        return {{0, QObject::tr("Disabled")},
                {1, QObject::tr("Enabled")}};
    }
    if (name == QLatin1String("H_FLYBAR_MODE")) {
        return {{0, QStringLiteral("NoFlybar")},
                {1, QStringLiteral("Flybar")}};
    }
    if (name == QLatin1String("H_RSC_MODE")) {
        return {{1, QObject::tr("RC Passthrough")},
                {2, QObject::tr("External Gov SetPoint")},
                {3, QObject::tr("Throttle Curve")},
                {4, QStringLiteral("AutoThrottle")}};
    }
    if (name == QLatin1String("H_TAIL_TYPE")) {
        return {{0, QObject::tr("Servo only")},
                {1, QObject::tr("Servo with ExtGyro")},
                {2, QObject::tr("DirectDrive VarPitch")},
                {3, QObject::tr("DirectDrive FixedPitch CW")},
                {4, QObject::tr("DirectDrive FixedPitch CCW")},
                {5, QObject::tr("DDVP with external governor")}};
    }
    return {};
}
} // namespace

ConfigTradHeli4ViewModel::ConfigTradHeli4ViewModel(QObject *parent)
    : QObject(parent)
{
    rebuildFields();
}

QList<Heli4ParameterSection> ConfigTradHeli4ViewModel::Sections()
{
    QStringList servos;
    const QStringList suffixes = {
        QStringLiteral("REVERSED"), QStringLiteral("FUNCTION"),
        QStringLiteral("MIN"), QStringLiteral("TRIM"),
        QStringLiteral("MAX")
    };
    for (int channel = 1; channel <= 8; ++channel) {
        for (const QString &suffix : suffixes) {
            servos.append(servoName(channel, suffix));
        }
    }
    return {
        {tr("Servo Outputs"), servos},
        {tr("Swashplate"), {
            QStringLiteral("H_SV_MAN"), QStringLiteral("H_SW_TYPE"),
            QStringLiteral("H_SW_COL_DIR"), QStringLiteral("H_SW_LIN_SVO"),
            QStringLiteral("H_FLYBAR_MODE"), QStringLiteral("H_CYC_MAX"),
            QStringLiteral("H_COL_MAX"), QStringLiteral("H_COL_MID"),
            QStringLiteral("H_COL_MIN"), QStringLiteral("H_COL_ANG_MIN"),
            QStringLiteral("H_COL_ANG_MAX"),
            QStringLiteral("H_COL_ZERO_THRST"),
            QStringLiteral("H_COL_LAND_MIN")}},
        {tr("Rotor Speed Control"), {
            QStringLiteral("H_RSC_MODE"), QStringLiteral("H_RSC_CRITICAL"),
            QStringLiteral("H_RSC_RAMP_TIME"),
            QStringLiteral("H_RSC_RUNUP_TIME"),
            QStringLiteral("H_RSC_CLDWN_TIME"),
            QStringLiteral("H_RSC_SETPOINT"), QStringLiteral("H_RSC_IDLE"),
            QStringLiteral("H_RSC_THRCRV_0"),
            QStringLiteral("H_RSC_THRCRV_25"),
            QStringLiteral("H_RSC_THRCRV_50"),
            QStringLiteral("H_RSC_THRCRV_75"),
            QStringLiteral("H_RSC_THRCRV_100")}},
        {tr("Governor"), {
            QStringLiteral("H_RSC_GOV_COMP"),
            QStringLiteral("H_RSC_GOV_SETPNT"),
            QStringLiteral("H_RSC_GOV_DISGAG"),
            QStringLiteral("H_RSC_GOV_DROOP"),
            QStringLiteral("H_RSC_GOV_FF"),
            QStringLiteral("H_RSC_GOV_TCGAIN"),
            QStringLiteral("H_RSC_GOV_RANGE"),
            QStringLiteral("H_RSC_GOV_RPM"),
            QStringLiteral("H_RSC_GOV_TORQUE")}},
        {tr("Miscellaneous"), {
            QStringLiteral("IM_STB_COL_1"), QStringLiteral("IM_STB_COL_2"),
            QStringLiteral("IM_STB_COL_3"), QStringLiteral("IM_STB_COL_4"),
            QStringLiteral("H_TAIL_TYPE"), QStringLiteral("H_TAIL_SPEED"),
            QStringLiteral("H_GYR_GAIN"),
            QStringLiteral("H_GYR_GAIN_ACRO"),
            QStringLiteral("H_COLYAW")}}
    };
}

QStringList ConfigTradHeli4ViewModel::ReferenceFieldNames()
{
    QStringList result;
    for (const Heli4ParameterSection &section : Sections()) {
        result.append(section.parameters);
    }
    return result;
}

int ConfigTradHeli4ViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

void ConfigTradHeli4ViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    rebuildFields();
}

void ConfigTradHeli4ViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> capableComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (normalizedName(parameter.name) == QLatin1String("H_SW_TYPE")) {
            capableComponents.insert(parameter.componentId);
        }
    }
    if (capableComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (capableComponents.contains(1)) {
        m_componentId = 1;
    } else if (!capableComponents.isEmpty()) {
        QList<int> ordered = capableComponents.values();
        std::sort(ordered.begin(), ordered.end());
        m_componentId = ordered.constFirst();
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
    m_snapshotReady = m_values.contains(QStringLiteral("H_SW_TYPE"));
    if (m_values.value(QStringLiteral("H_SV_MAN")).toDouble() != 0.0) {
        m_manualOverrideUncertain = true;
    }
    m_status.clear();
    rebuildFields();
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        const bool manualMayBeActive = ManualOverrideMayBeActive();
        ++m_snapshotGeneration;
        m_pendingWrites.clear();
        m_status = tr("not connected");
        rebuildFields();
        if (manualMayBeActive) {
            emit manualSafetyWarning(
                tr("The link was lost while manual servo override may be "
                   "active. Verify H_SV_MAN is disabled before flight."));
        }
    }
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!active && ManualOverrideMayBeActive()
        && !setFieldValue(QStringLiteral("H_SV_MAN"), 0)) {
        emit manualSafetyWarning(
            tr("Manual servo override may still be active. Disable "
               "H_SV_MAN before flight."));
    }
    emit stateChanged();
}

bool ConfigTradHeli4ViewModel::ManualOverrideMayBeActive() const
{
    if (m_manualOverrideUncertain
        || m_values.value(QStringLiteral("H_SV_MAN")).toDouble() != 0.0) {
        return true;
    }
    return m_pendingWrites.contains(QStringLiteral("H_SV_MAN"));
}

bool ConfigTradHeli4ViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    ParamField *field = fieldForName(name);
    if (!field || field->readOnly || !m_connected || !m_snapshotReady
        || !value.isValid()) {
        return false;
    }
    const QString normalized = normalizedName(name);
    const bool safetyDisable = normalized == QLatin1String("H_SV_MAN")
        && value.toDouble() == 0.0;
    if (normalized == QLatin1String("H_SV_MAN")
        && m_armed && !safetyDisable) {
        return false;
    }
    if (!field->options.isEmpty()) {
        const bool known = std::any_of(
            field->options.constBegin(), field->options.constEnd(),
            [&value](const ParamOption &option) {
                return valuesEqual(option.value, value);
            });
        if (!known) {
            return false;
        }
    }
    bool numericOk = false;
    const double numeric = value.toDouble(&numericOk);
    if (field->editorKind == ParamField::EditorKind::Numeric
        && (!numericOk || !std::isfinite(numeric))) {
        return false;
    }
    if (field->hasRange && field->enforceRange
        && (numeric < field->minimum || numeric > field->maximum)) {
        return false;
    }
    const QVariant candidate = typedValue(value, m_values.value(normalized));
    const auto previous = m_pendingWrites.constFind(normalized);
    if ((previous == m_pendingWrites.constEnd()
         && valuesEqual(m_values.value(normalized), candidate)
         && !(normalized == QLatin1String("H_SV_MAN")
              && candidate.toDouble() == 0.0
              && m_manualOverrideUncertain))
        || (previous != m_pendingWrites.constEnd()
            && valuesEqual(previous->expectedValue, candidate))) {
        return false;
    }

    PendingWrite pending;
    pending.expectedValue = candidate;
    pending.requestId = ++m_requestGeneration;
    pending.snapshotGeneration = m_snapshotGeneration;
    pending.manualUncertainBefore = m_manualOverrideUncertain;
    m_pendingWrites.insert(normalized, pending);
    if (normalized == QLatin1String("H_SV_MAN")) {
        // A submitted PARAM_SET may reach the vehicle even if its result is
        // lost. Only an exact successful zero batch proves the override safe.
        m_manualOverrideUncertain = true;
    }
    field->value = candidate;
    field->status = QStringLiteral("…");
    emit fieldChanged(normalized);
    emit stateChanged();
    emit writeRequested(pending.requestId, m_componentId,
                        normalized, candidate);

    const quint64 requestId = pending.requestId;
    const quint64 snapshotGeneration = pending.snapshotGeneration;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, normalized, requestId, snapshotGeneration]() {
        const auto iterator = m_pendingWrites.constFind(normalized);
        if (iterator == m_pendingWrites.constEnd()
            || iterator->requestId != requestId
            || iterator->snapshotGeneration != snapshotGeneration
            || iterator->batchId != 0
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        finishPending(normalized, tr("write timeout"));
    });
    return true;
}

bool ConfigTradHeli4ViewModel::Refresh()
{
    if (!m_connected || !m_snapshotReady || HasPendingWrites()) {
        return false;
    }
    m_status = tr("Refreshing parameters…");
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigTradHeli4ViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (!ReferenceFieldNames().contains(normalized)) {
        return;
    }
    const bool wasPresent = m_values.contains(normalized);
    m_values.insert(normalized, value);
    if (!wasPresent) {
        if (normalized == QLatin1String("H_SW_TYPE")) {
            m_snapshotReady = true;
        }
        rebuildFields();
    } else if (!m_pendingWrites.contains(normalized)) {
        if (ParamField *field = fieldForName(normalized)) {
            field->value = value;
            field->status.clear();
            emit fieldChanged(normalized);
        }
    }
    if (normalized == QLatin1String("H_SV_MAN")
        && value.toDouble() != 0.0
        && !m_pendingWrites.contains(normalized) && !m_active
        && !m_manualOverrideUncertain) {
        m_manualOverrideUncertain = true;
        emit manualSafetyWarning(
            tr("Manual servo override is active while Heli Setup is not "
               "active. Disable H_SV_MAN before flight."));
    }
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    auto pending = pendingForRequest(requestId);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    if (batchId == 0) {
        const QString name = pending.key();
        finishPending(name, tr("write was rejected"));
        return;
    }
    pending->batchId = batchId;
}

void ConfigTradHeli4ViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    auto pending = pendingForRequest(requestId);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    finishPending(pending.key(),
                  reason.isEmpty() ? tr("write failed") : reason);
}

void ConfigTradHeli4ViewModel::parameterWriteFailed(
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
    finishPending(pending.key(),
                  reason.isEmpty() ? tr("write failed") : reason);
}

void ConfigTradHeli4ViewModel::parameterBatchCompleted(
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
        finishPending(name, tr("write failed"));
    }
}

void ConfigTradHeli4ViewModel::refreshFailed(const QString &reason)
{
    m_status = reason.isEmpty() ? tr("parameter refresh failed") : reason;
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::refreshCanceled()
{
    m_status = tr("parameter refresh canceled");
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::rebuildFields()
{
    m_fields.clear();
    const QStringList names = ReferenceFieldNames();
    m_fields.reserve(names.size());
    for (const QString &name : names) {
        m_fields.append(makeField(name));
    }
    emit structureChanged();
}

ParamField ConfigTradHeli4ViewModel::makeField(const QString &name) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = normalizedName(name);
    field.value = m_values.value(field.name, 0);
    const ParameterMetaData metadata = metadataFor(field.name);
    field.label = metadata.title.isEmpty() ? field.name : metadata.title;
    field.description = metadata.description;
    field.units = metadata.units;
    field.readOnly = !m_values.contains(field.name) || metadata.readOnly;
    field.status = m_values.contains(field.name) ? QString() : tr("n/a");
    for (const ParameterMetaDataOption &option : metadata.values) {
        field.options.append({option.value, option.label});
    }
    if (field.options.isEmpty()) {
        field.options = currentHeliEnumFallback(field.name);
    }
    if (field.name.endsWith(QStringLiteral("_REVERSED"))
        && field.options.isEmpty()) {
        field.options = {{0, tr("Normal")}, {1, tr("Reversed")}};
    }
    const bool servoFunction = field.name.startsWith(QStringLiteral("SERVO"))
        && field.name.endsWith(QStringLiteral("_FUNCTION"));
    if (servoFunction && field.options.isEmpty()) {
        field.readOnly = true;
        field.status = tr("metadata required");
    }
    field.editorKind = field.options.isEmpty() && !servoFunction
        ? ParamField::EditorKind::Numeric
        : ParamField::EditorKind::Combo;
    if (isServoPwmField(field.name)) {
        field.minimum = 800.0;
        field.maximum = 2200.0;
        field.increment = 1.0;
        field.hasRange = true;
        field.enforceRange = true;
    } else if (metadata.hasRange) {
        field.minimum = metadata.minimum;
        field.maximum = metadata.maximum;
        field.hasRange = true;
        field.enforceRange = m_enforceMetadataRanges;
    }
    if (metadata.hasIncrement && metadata.increment > 0.0) {
        field.increment = metadata.increment;
    } else if (field.value.canConvert<int>()
               && std::abs(field.value.toDouble()
                           - field.value.toInt()) < 1.0e-9) {
        field.increment = 1.0;
    }
    return field;
}

ParamField *ConfigTradHeli4ViewModel::fieldForName(const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

const ParamField *ConfigTradHeli4ViewModel::fieldForName(
    const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (const ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

ParameterMetaData ConfigTradHeli4ViewModel::metadataFor(
    const QString &name) const
{
    ParameterMetaData metadata = m_catalog.value(name);
    if (metadata.name.isEmpty() && name.startsWith(QStringLiteral("SERVO"))) {
        metadata = m_catalog.value(servoTemplateName(name));
    }
    return metadata;
}

void ConfigTradHeli4ViewModel::finishPendingSuccess(const QString &name)
{
    auto pending = m_pendingWrites.find(name);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const QVariant value = pending->expectedValue;
    m_values.insert(name, value);
    m_pendingWrites.erase(pending);
    if (name == QLatin1String("H_SV_MAN") && value.toDouble() == 0.0) {
        m_manualOverrideUncertain = false;
    }
    if (ParamField *field = fieldForName(name)) {
        field->value = value;
        field->status = QStringLiteral("✓");
        emit fieldChanged(name);
    }
    m_status.clear();
    emit stateChanged();
}

void ConfigTradHeli4ViewModel::finishPending(
    const QString &name, const QString &error)
{
    auto pending = m_pendingWrites.find(name);
    if (pending == m_pendingWrites.end()) {
        return;
    }
    const QVariant expectedValue = pending->expectedValue;
    const bool wasSubmitted = pending->batchId != 0;
    const bool manualUncertainBefore = pending->manualUncertainBefore;
    m_pendingWrites.erase(pending);
    if (name == QLatin1String("H_SV_MAN")
        && expectedValue.toDouble() != 0.0 && !wasSubmitted) {
        m_manualOverrideUncertain = manualUncertainBefore;
    }
    if (ParamField *field = fieldForName(name)) {
        field->value = m_values.value(name, field->value);
        field->status = error;
        emit fieldChanged(name);
    }
    m_status = error;
    if (name == QLatin1String("H_SV_MAN")
        && expectedValue.toDouble() == 0.0) {
        emit manualSafetyWarning(
            tr("Manual servo override may still be active. Verify "
               "H_SV_MAN is disabled before flight."));
    }
    emit stateChanged();
}

QHash<QString, ConfigTradHeli4ViewModel::PendingWrite>::iterator
ConfigTradHeli4ViewModel::pendingForRequest(quint64 requestId)
{
    for (auto iterator = m_pendingWrites.begin();
         iterator != m_pendingWrites.end(); ++iterator) {
        if (iterator->requestId == requestId
            && iterator->snapshotGeneration == m_snapshotGeneration) {
            return iterator;
        }
    }
    return m_pendingWrites.end();
}

QHash<QString, ConfigTradHeli4ViewModel::PendingWrite>::iterator
ConfigTradHeli4ViewModel::pendingForBatch(qulonglong batchId)
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

QVariant ConfigTradHeli4ViewModel::typedValue(
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

QString ConfigTradHeli4ViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigTradHeli4ViewModel::valuesEqual(
    const QVariant &left, const QVariant &right)
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
