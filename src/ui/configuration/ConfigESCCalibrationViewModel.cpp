#include "ConfigESCCalibrationViewModel.h"

#include <QMetaType>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
const QString kEscCalibration = QStringLiteral("ESC_CALIBRATION");

bool variantsEqual(const QVariant &left, const QVariant &right)
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

QVariant defaultValue(const QString &name)
{
    Q_UNUSED(name)
    return 0.0;
}
} // namespace

ConfigESCCalibrationViewModel::ConfigESCCalibrationViewModel(
    QObject *parent)
    : QObject(parent)
{
    rebuildFields();
}

QStringList ConfigESCCalibrationViewModel::FieldNames()
{
    return {
        QStringLiteral("MOT_PWM_TYPE"),
        QStringLiteral("MOT_PWM_MIN"),
        QStringLiteral("MOT_PWM_MAX"),
        QStringLiteral("MOT_SPIN_ARM"),
        QStringLiteral("MOT_SPIN_MIN"),
        QStringLiteral("MOT_SPIN_MAX")
    };
}

QString ConfigESCCalibrationViewModel::Title() const
{
    return tr("ESC Calibration (AC3.3+)");
}

QString ConfigESCCalibrationViewModel::Intro() const
{
    return tr("Configure motor PWM output and run the all-at-once ESC "
              "calibration.");
}

QString ConfigESCCalibrationViewModel::Instructions() const
{
    return tr(
        "DANGER: REMOVE ALL PROPELLERS FIRST.\n"
        "1. Press Calibrate ESCs (sets ESC_CALIBRATION = 3). Requires "
        "AC 3.3+.\n"
        "2. Disconnect the battery and USB.\n"
        "3. Re-connect the battery — the autopilot enters the ESC "
        "calibration sequence and passes the throttle range through to "
        "the ESCs.\n"
        "4. Listen for the ESC confirmation tones, then disconnect and "
        "reconnect power normally.");
}

QString ConfigESCCalibrationViewModel::CalButtonText() const
{
    return m_calibrationComplete ? tr("Done") : tr("Calibrate ESCs");
}

QString ConfigESCCalibrationViewModel::Status() const
{
    return m_status;
}

bool ConfigESCCalibrationViewModel::Busy() const
{
    return m_calibrationBusy;
}

bool ConfigESCCalibrationViewModel::CanCalibrate() const
{
    return m_connected && !m_armed && !m_calibrationBusy
        && !m_calibrationComplete && m_pendingWrites.isEmpty()
        && m_values.contains(kEscCalibration);
}

void ConfigESCCalibrationViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildFields();
}

void ConfigESCCalibrationViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> relevantComponents;
    const QStringList fieldNames = FieldNames();
    const QSet<QString> requested(
        fieldNames.constBegin(), fieldNames.constEnd());
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        const QString name = normalizedName(parameter.name);
        if (requested.contains(name) || name == kEscCalibration) {
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

    ++m_writeGeneration;
    m_pendingWrites.clear();
    m_calibrationBusy = false;
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    rebuildFields();
    emit stateChanged();
}

void ConfigESCCalibrationViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!m_connected) {
        failAllPending(tr("not connected"));
    }
    emit stateChanged();
}

void ConfigESCCalibrationViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    if (m_armed) {
        failAllPending(tr("vehicle armed"));
    }
    emit stateChanged();
}

bool ConfigESCCalibrationViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    ParamField *field = fieldForName(normalized);
    if (!m_connected || m_armed || !m_pendingWrites.isEmpty()
        || !field || field->readOnly) {
        return false;
    }

    QVariant candidate;
    if (field->editorKind == ParamField::EditorKind::Combo) {
        const auto iterator = std::find_if(
            field->options.constBegin(), field->options.constEnd(),
            [&value](const ParamOption &option) {
                return variantsEqual(option.value, value);
            });
        if (iterator == field->options.constEnd()) {
            return false;
        }
        candidate = iterator->value;
    } else {
        bool ok = false;
        const double numeric = value.toDouble(&ok);
        if (!ok || !std::isfinite(numeric)
            || (field->hasRange
                && (numeric < field->minimum
                    || numeric > field->maximum))) {
            return false;
        }
        candidate = typedValue(numeric, field->value);
    }
    if (variantsEqual(field->value, candidate)) {
        return false;
    }

    field->value = candidate;
    queueWrite(normalized, candidate, false);
    emit fieldChanged(normalized);
    return true;
}

bool ConfigESCCalibrationViewModel::CalibrateEsc(bool confirmed)
{
    if (!m_connected) {
        setStatus(tr("Connect to a vehicle first."));
        return false;
    }
    if (!confirmed || m_calibrationBusy || m_calibrationComplete
        || !m_pendingWrites.isEmpty()) {
        return false;
    }
    if (m_armed) {
        setStatus(tr("Disarm the vehicle before ESC calibration."));
        return false;
    }
    if (!m_values.contains(kEscCalibration)) {
        setStatus(tr(
            "Set param error. Please ensure your version is AC 3.3+."));
        return false;
    }

    m_calibrationBusy = true;
    setStatus(tr("Waiting for ESC_CALIBRATION confirmation…"));
    emit stateChanged();
    queueWrite(kEscCalibration, 3, true);
    return true;
}

bool ConfigESCCalibrationViewModel::Refresh(bool armedConfirmed)
{
    if (!m_connected) {
        setStatus(tr("Not connected — cannot fetch params."));
        return false;
    }
    if (HasPendingWrites()) {
        setStatus(tr("Wait for pending parameter writes to finish."));
        return false;
    }
    if (m_armed && !armedConfirmed) {
        return false;
    }
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigESCCalibrationViewModel::refreshFailed(const QString &reason)
{
    setStatus(reason.isEmpty()
                  ? tr("Refresh failed.")
                  : tr("Refresh failed: %1").arg(reason));
}

void ConfigESCCalibrationViewModel::refreshCanceled()
{
    setStatus(tr("Parameter refresh canceled."));
}

void ConfigESCCalibrationViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    const bool relevant = FieldNames().contains(normalized)
        || normalized == kEscCalibration;
    if (!relevant) {
        return;
    }

    m_values.insert(normalized, value);
    if (ParamField *field = fieldForName(normalized)) {
        field->value = value;
        field->readOnly = m_catalog.value(normalized).readOnly;
        emit fieldChanged(normalized);
    }

    auto pending = m_pendingWrites.find(normalized);
    if (pending == m_pendingWrites.end()) {
        emit stateChanged();
        return;
    }
    const bool matched = variantsEqual(pending->expectedValue, value);
    const bool calibration = pending->calibration;
    m_pendingWrites.erase(pending);
    if (calibration) {
        m_calibrationBusy = false;
        if (matched) {
            m_calibrationComplete = true;
            setStatus(tr("ESC_CALIBRATION set. Now power-cycle the vehicle "
                         "to run the sequence."));
        } else {
            setStatus(tr(
                "Set param error. Please ensure your version is AC 3.3+."));
        }
    } else if (!matched) {
        setStatus(tr("Set param error: %1.").arg(normalized));
    }
    emit stateChanged();
}

void ConfigESCCalibrationViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId == m_componentId) {
        finishPending(normalizedName(name), reason);
    }
}

void ConfigESCCalibrationViewModel::rebuildFields()
{
    m_fields.clear();
    const QStringList names = FieldNames();
    for (int index = 0; index < names.size(); ++index) {
        m_fields.append(makeField(names.at(index), index));
    }
    emit structureChanged();
}

ParamField ConfigESCCalibrationViewModel::makeField(
    const QString &name, int index) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = name;
    field.value = m_values.contains(name)
        ? m_values.value(name) : defaultValue(name);
    const ParameterMetaData metadata = m_catalog.value(name);
    field.label = metadata.title.isEmpty() ? name : metadata.title;
    field.units = metadata.units;
    field.description = metadata.description;
    field.readOnly = !m_values.contains(name) || metadata.readOnly;

    if (index == 0) {
        field.editorKind = ParamField::EditorKind::Combo;
        for (const ParameterMetaDataOption &option : metadata.values) {
            field.options.append({option.value, option.label});
        }
        return field;
    }

    field.editorKind = ParamField::EditorKind::Numeric;
    field.hasRange = true;
    field.enforceRange = true;
    if (name == QLatin1String("MOT_PWM_MIN")
        || name == QLatin1String("MOT_PWM_MAX")) {
        field.minimum = metadata.hasRange ? metadata.minimum : 0.0;
        field.maximum = metadata.hasRange ? metadata.maximum : 2000.0;
        field.increment = metadata.hasIncrement ? metadata.increment : 1.0;
    } else {
        field.minimum = 0.0;
        field.maximum = 1.0;
        field.increment = metadata.hasIncrement ? metadata.increment : 0.01;
    }
    return field;
}

ParamField *ConfigESCCalibrationViewModel::fieldForName(
    const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

const ParamField *ConfigESCCalibrationViewModel::fieldForName(
    const QString &name) const
{
    return const_cast<ConfigESCCalibrationViewModel *>(this)
        ->fieldForName(name);
}

void ConfigESCCalibrationViewModel::queueWrite(
    const QString &name, const QVariant &value, bool calibration)
{
    PendingWrite pending;
    pending.expectedValue = value;
    pending.previousValue = m_values.value(name);
    pending.generation = ++m_writeGeneration;
    pending.calibration = calibration;
    m_pendingWrites.insert(name, pending);
    emit stateChanged();
    emit writeRequested(m_componentId, name, value);

    const quint64 generation = pending.generation;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, name, generation]() {
        const auto iterator = m_pendingWrites.constFind(name);
        if (iterator == m_pendingWrites.constEnd()
            || iterator->generation != generation) {
            return;
        }
        finishPending(name, tr("write timeout"));
    });
}

void ConfigESCCalibrationViewModel::finishPending(
    const QString &name, const QString &reason)
{
    auto iterator = m_pendingWrites.find(name);
    if (iterator == m_pendingWrites.end()) {
        return;
    }
    const PendingWrite pending = iterator.value();
    m_pendingWrites.erase(iterator);
    if (pending.calibration) {
        m_calibrationBusy = false;
        if (reason == QLatin1String("vehicle armed")) {
            setStatus(tr("Disarm the vehicle before ESC calibration."));
        } else if (reason == QLatin1String("not connected")) {
            setStatus(tr("Connect to a vehicle first."));
        } else {
            setStatus(tr(
                "Set param error. Please ensure your version is AC 3.3+."));
        }
    } else {
        if (ParamField *field = fieldForName(name)) {
            field->value = pending.previousValue;
            emit fieldChanged(name);
        }
        setStatus(reason.isEmpty()
                      ? tr("Set param error: %1.").arg(name)
                      : tr("Set param error: %1 (%2).").arg(name, reason));
    }
    emit stateChanged();
}

void ConfigESCCalibrationViewModel::failAllPending(
    const QString &reason)
{
    const QStringList names = m_pendingWrites.keys();
    for (const QString &name : names) {
        finishPending(name, reason);
    }
}

void ConfigESCCalibrationViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}

QString ConfigESCCalibrationViewModel::normalizedName(
    const QString &name) const
{
    return name.trimmed().toUpper();
}

QVariant ConfigESCCalibrationViewModel::typedValue(
    const QVariant &value, const QVariant &reference) const
{
    const double numeric = value.toDouble();
    switch (reference.userType()) {
    case QMetaType::Float:
        return static_cast<float>(numeric);
    case QMetaType::Double:
        return numeric;
    case QMetaType::Int:
        return static_cast<int>(std::llround(numeric));
    case QMetaType::UInt:
        return static_cast<uint>(std::llround(numeric));
    case QMetaType::LongLong:
        return static_cast<qlonglong>(std::llround(numeric));
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(std::llround(numeric));
    default:
        return numeric;
    }
}
