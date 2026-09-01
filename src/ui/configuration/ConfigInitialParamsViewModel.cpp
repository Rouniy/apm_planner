#include "ConfigInitialParamsViewModel.h"

#include <QMetaType>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;

bool valuesEqual(double left, const QVariant &right)
{
    bool ok = false;
    const double rightValue = right.toDouble(&ok);
    const double tolerance = std::max(
        1.0e-6, std::max(std::abs(left), std::abs(rightValue)) * 1.0e-6);
    return ok && (left == rightValue
                  || std::abs(left - rightValue) <= tolerance);
}

QVariant valueWithOriginalType(const QVariant &reference, double value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::Int:
        return static_cast<int>(std::nearbyint(value));
    case QMetaType::UInt:
        return static_cast<uint>(std::nearbyint(value));
    case QMetaType::LongLong:
        return static_cast<qlonglong>(std::nearbyint(value));
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(std::nearbyint(value));
    case QMetaType::Float:
        return static_cast<float>(value);
    case QMetaType::Double:
        return value;
    default:
        return value;
    }
}
}

ConfigInitialParamsViewModel::ConfigInitialParamsViewModel(QObject *parent)
    : QObject(parent)
{}

QStringList ConfigInitialParamsViewModel::BatteryTypes()
{
    return {QStringLiteral("LiPo"), QStringLiteral("LiPoHV"),
            QStringLiteral("LiIon")};
}

QList<ParamCompareRow> ConfigInitialParamsViewModel::SelectWritableRows(
    const QList<ParamCompareRow> &rows,
    const QStringList &availableParameters)
{
    QSet<QString> available;
    for (const QString &name : availableParameters) {
        available.insert(name.trimmed().toUpper());
    }
    QList<ParamCompareRow> writable;
    for (const ParamCompareRow &row : rows) {
        if (row.use && row.exists
            && available.contains(row.name.toUpper())) {
            writable.append(row);
        }
    }
    return writable;
}

QString ConfigInitialParamsViewModel::PropSize() const
{
    return m_propSize;
}

QString ConfigInitialParamsViewModel::CellCount() const
{
    return m_cellCount;
}

QString ConfigInitialParamsViewModel::CellMax() const
{
    return m_cellMax;
}

QString ConfigInitialParamsViewModel::CellMin() const
{
    return m_cellMin;
}

QString ConfigInitialParamsViewModel::BatteryType() const
{
    return m_batteryType;
}

bool ConfigInitialParamsViewModel::TMotor() const
{
    return m_tMotor;
}

bool ConfigInitialParamsViewModel::Suggested() const
{
    return m_suggested;
}

QList<ParamCompareRow> ConfigInitialParamsViewModel::Results() const
{
    return m_results;
}

bool ConfigInitialParamsViewModel::HasResults() const
{
    return !m_results.isEmpty();
}

bool ConfigInitialParamsViewModel::Writing() const
{
    return m_writing;
}

QString ConfigInitialParamsViewModel::Status() const
{
    return m_status;
}

QString ConfigInitialParamsViewModel::DocsUrl() const
{
    return QStringLiteral(
        "https://ardupilot.org/copter/docs/tuning-process-instructions.html");
}

void ConfigInitialParamsViewModel::setPropSize(const QString &value)
{
    if (m_writing) {
        return;
    }
    if (m_propSize != value) {
        m_propSize = value;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setCellCount(const QString &value)
{
    if (m_writing) {
        return;
    }
    if (m_cellCount != value) {
        m_cellCount = value;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setCellMax(const QString &value)
{
    if (m_writing) {
        return;
    }
    if (m_cellMax != value) {
        m_cellMax = value;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setCellMin(const QString &value)
{
    if (m_writing) {
        return;
    }
    if (m_cellMin != value) {
        m_cellMin = value;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setBatteryType(const QString &value)
{
    if (m_writing) {
        return;
    }
    if (m_batteryType == value) {
        return;
    }
    m_batteryType = value;
    invalidateResults();
    if (value == QLatin1String("LiPoHV")) {
        m_cellMax = QStringLiteral("4.35");
        m_cellMin = QStringLiteral("3.3");
    } else if (value == QLatin1String("LiIon")) {
        m_cellMax = QStringLiteral("4.1");
        m_cellMin = QStringLiteral("2.8");
    } else {
        m_cellMax = QStringLiteral("4.2");
        m_cellMin = QStringLiteral("3.3");
    }
    emit inputsChanged();
}

void ConfigInitialParamsViewModel::setTMotor(bool enabled)
{
    if (m_writing) {
        return;
    }
    if (m_tMotor != enabled) {
        m_tMotor = enabled;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setSuggested(bool enabled)
{
    if (m_writing) {
        return;
    }
    if (m_suggested != enabled) {
        m_suggested = enabled;
        invalidateResults();
        emit inputsChanged();
    }
}

void ConfigInitialParamsViewModel::setVehicleContext(
    bool plane, int firmwareMajor)
{
    if (m_plane != plane || m_firmwareMajor != firmwareMajor) {
        invalidateResults();
    }
    m_plane = plane;
    m_firmwareMajor = firmwareMajor;
}

void ConfigInitialParamsViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    const bool hadResults = !m_results.isEmpty();
    if (hadResults) {
        m_results.clear();
        emit resultsChanged();
    }
    QSet<int> components;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        components.insert(parameter.componentId);
    }
    if (components.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (components.contains(1)) {
        m_componentId = 1;
    } else if (!components.isEmpty()) {
        QList<int> ordered = components.values();
        std::sort(ordered.begin(), ordered.end());
        m_componentId = ordered.first();
    } else {
        m_componentId = preferredComponent;
    }

    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    m_pendingWrites.clear();
    m_writeQueue.clear();
    m_failedWrites.clear();
    if (m_writing) {
        m_writing = false;
        emit writingChanged();
    }
    if (hadResults) {
        setStatus(tr("Vehicle parameters changed. Recalculate initial "
                     "parameters before writing."));
    }
}

bool ConfigInitialParamsViewModel::setResultUse(
    const QString &name, bool use)
{
    if (m_writing) {
        return false;
    }
    const QString normalized = normalizedName(name);
    for (ParamCompareRow &row : m_results) {
        if (row.name == normalized) {
            const bool selected = row.exists && use;
            if (row.use == selected) {
                return false;
            }
            row.use = selected;
            emit resultsChanged();
            return true;
        }
    }
    return false;
}

bool ConfigInitialParamsViewModel::Calculate()
{
    if (m_writing) {
        return false;
    }
    const double propSize = Parse(m_propSize);
    const double batteryCells = Parse(m_cellCount);
    const double batteryCellMax = Parse(m_cellMax);
    const double batteryCellMin = Parse(m_cellMin);

    if (!std::isfinite(propSize) || propSize <= 0.0) {
        setStatus(tr("Prop size must be larger than zero."));
        return false;
    }
    if (!std::isfinite(batteryCells) || batteryCells < 1.0
        || std::floor(batteryCells) != batteryCells) {
        setStatus(tr("Battery cell count must be at least 1."));
        return false;
    }
    if (!std::isfinite(batteryCellMax)
        || !std::isfinite(batteryCellMin)
        || batteryCellMin <= 0.0 || batteryCellMax <= batteryCellMin) {
        setStatus(tr("Battery cell voltages are invalid."));
        return false;
    }

    const double atcAccelYMax = std::max(
        8000.0, RoundTo(-900.0 * propSize + 36000.0, -2));
    const double acroYawP = 0.5 * atcAccelYMax / 4500.0;
    const double atcAccelPMax = std::max(
        10000.0,
        RoundTo(-2.613267 * std::pow(propSize, 3.0)
                    + 343.39216 * std::pow(propSize, 2.0)
                    - 15083.7121 * propSize + 235771.0,
                -2));
    const double insGyroFilter = std::max(
        20.0, RoundTo(289.22 * std::pow(propSize, -0.838), 0));
    const double filterHalf = std::max(10.0, insGyroFilter / 2.0);

    double motorThrustExpo = std::min(
        RoundTo(0.15686 * std::log(propSize) + 0.23693, 2), 0.80);
    if (m_tMotor) {
        motorThrustExpo = 0.2;
    }

    const QString atc = m_plane ? QStringLiteral("Q_A")
                                : QStringLiteral("ATC");
    const QString motor = m_plane ? QStringLiteral("Q_M")
                                  : QStringLiteral("MOT");

    m_results.clear();
    appendResult(QStringLiteral("ACRO_YAW_P"), acroYawP);
    if (m_values.contains(atc + QStringLiteral("_ACCEL_P_MAX"))) {
        appendResult(atc + QStringLiteral("_ACCEL_P_MAX"), atcAccelPMax);
        appendResult(atc + QStringLiteral("_ACCEL_R_MAX"), atcAccelPMax);
        appendResult(atc + QStringLiteral("_ACCEL_Y_MAX"), atcAccelYMax);
    } else {
        appendResult(atc + QStringLiteral("_ACC_P_MAX"),
                     atcAccelPMax / 100.0);
        appendResult(atc + QStringLiteral("_ACC_R_MAX"),
                     atcAccelPMax / 100.0);
        appendResult(atc + QStringLiteral("_ACC_Y_MAX"),
                     atcAccelYMax / 100.0);
    }

    const bool modernRateFilters = m_firmwareMajor == 4
        || m_values.contains(atc + QStringLiteral("_RAT_PIT_FLTD"))
        || m_values.contains(atc + QStringLiteral("_RAT_RLL_FLTD"))
        || m_values.contains(atc + QStringLiteral("_RAT_YAW_FLTD"));
    if (modernRateFilters) {
        appendResult(atc + QStringLiteral("_RAT_PIT_FLTD"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_PIT_FLTE"), 0.0);
        appendResult(atc + QStringLiteral("_RAT_PIT_FLTT"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_RLL_FLTD"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_RLL_FLTE"), 0.0);
        appendResult(atc + QStringLiteral("_RAT_RLL_FLTT"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_YAW_FLTD"), 0.0);
        appendResult(atc + QStringLiteral("_RAT_YAW_FLTE"), 2.0);
        appendResult(atc + QStringLiteral("_RAT_YAW_FLTT"), filterHalf);
    } else {
        appendResult(atc + QStringLiteral("_RAT_PIT_FILT"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_RLL_FILT"), filterHalf);
        appendResult(atc + QStringLiteral("_RAT_YAW_FILT"), 2.0);
    }

    appendResult(atc + QStringLiteral("_THR_MIX_MAN"), 0.1);
    appendResult(QStringLiteral("INS_ACCEL_FILTER"), 10.0);
    appendResult(QStringLiteral("INS_GYRO_FILTER"), insGyroFilter);
    appendResult(motor + QStringLiteral("_THST_EXPO"), motorThrustExpo);
    appendResult(motor + QStringLiteral("_THST_HOVER"), 0.2);
    appendResult(QStringLiteral("BATT_ARM_VOLT"),
                 (batteryCells - 1.0) * 0.1
                     + (batteryCellMin + 0.3) * batteryCells);
    appendResult(QStringLiteral("BATT_CRT_VOLT"),
                 (batteryCellMin + 0.2) * batteryCells);
    appendResult(QStringLiteral("BATT_LOW_VOLT"),
                 (batteryCellMin + 0.3) * batteryCells);
    appendResult(motor + QStringLiteral("_BAT_VOLT_MAX"),
                 batteryCellMax * batteryCells);
    appendResult(motor + QStringLiteral("_BAT_VOLT_MIN"),
                 batteryCellMin * batteryCells);

    if (m_tMotor) {
        appendResult(motor + QStringLiteral("_PWM_MIN"), 1100.0);
        appendResult(motor + QStringLiteral("_PWM_MAX"), 1940.0);
    }
    if (m_suggested && m_firmwareMajor == 4 && !m_plane) {
        appendResult(QStringLiteral("BATT_FS_CRT_ACT"), 1.0);
        appendResult(QStringLiteral("BATT_FS_LOW_ACT"), 2.0);
        appendResult(QStringLiteral("FENCE_ACTION"), 3.0);
        appendResult(QStringLiteral("FENCE_ALT_MAX"), 120.0);
        appendResult(QStringLiteral("FENCE_ENABLE"), 1.0);
        appendResult(QStringLiteral("FENCE_RADIUS"), 150.0);
        appendResult(QStringLiteral("FENCE_TYPE"), 7.0);
    }

    emit resultsChanged();
    setStatus(tr("Review the values below, then Write to FC."));
    return true;
}

bool ConfigInitialParamsViewModel::WriteToFc(
    const QStringList &availableParameters, bool connected)
{
    if (m_writing) {
        return false;
    }
    if (!connected) {
        setStatus(tr("Not connected."));
        return false;
    }
    const QList<ParamCompareRow> writable =
        SelectWritableRows(m_results, availableParameters);
    if (writable.isEmpty()) {
        setStatus(tr(
            "No selected parameters exist on the connected vehicle."));
        return false;
    }

    m_pendingWrites.clear();
    m_writeQueue.clear();
    m_failedWrites.clear();
    m_writeCount = writable.size();
    m_writing = true;
    emit writingChanged();
    setStatus(tr("Writing initial parameters…"));

    for (const ParamCompareRow &row : writable) {
        const QString name = normalizedName(row.name);
        if (valuesEqual(row.value, m_values.value(name))) {
            continue;
        }
        m_writeQueue.append(row);
    }
    if (m_writeQueue.isEmpty()) {
        finishWriteBatch();
        return true;
    }
    dispatchNextWrite();
    return true;
}

void ConfigInitialParamsViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    m_values.insert(normalized, value);
    for (ParamCompareRow &row : m_results) {
        if (row.name == normalized) {
            row.current = Format(value.toDouble());
            row.exists = true;
            break;
        }
    }
    emit resultsChanged();

    const auto iterator = m_pendingWrites.constFind(normalized);
    if (iterator == m_pendingWrites.constEnd()) {
        return;
    }
    if (valuesEqual(iterator->expectedValue, value)) {
        finishWrite(normalized, false);
    } else {
        m_failedWrites.insert(normalized);
        setStatus(tr("Waiting for parameter write confirmation…"));
    }
}

void ConfigInitialParamsViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    Q_UNUSED(reason)
    if (componentId == m_componentId) {
        finishWrite(normalizedName(name), true);
    }
}

double ConfigInitialParamsViewModel::RoundTo(
    double value, int precision)
{
    if (precision >= 0) {
        const double scale = std::pow(10.0, precision);
        return std::nearbyint(value * scale) / scale;
    }
    const int scale = static_cast<int>(
        std::pow(10.0, std::abs(precision)));
    value += 5.0 * scale / 10.0;
    return std::round(value - std::fmod(value, scale));
}

double ConfigInitialParamsViewModel::Parse(const QString &text)
{
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    return ok ? value : 0.0;
}

QString ConfigInitialParamsViewModel::Format(double value)
{
    QString text = QString::number(value, 'f', 6);
    while (text.contains(QLatin1Char('.'))
           && text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

void ConfigInitialParamsViewModel::appendResult(
    const QString &name, double value)
{
    const QString normalized = normalizedName(name);
    const bool exists = m_values.contains(normalized);
    ParamCompareRow row;
    row.name = normalized;
    row.current = exists ? Format(m_values.value(normalized).toDouble())
                         : QStringLiteral("n/a");
    row.newValue = Format(value);
    row.value = value;
    row.exists = exists;
    row.use = exists;
    m_results.append(row);
}

void ConfigInitialParamsViewModel::invalidateResults()
{
    if (m_results.isEmpty()) {
        return;
    }
    m_results.clear();
    emit resultsChanged();
    setStatus(QString());
}

void ConfigInitialParamsViewModel::setStatus(const QString &status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}

void ConfigInitialParamsViewModel::dispatchNextWrite()
{
    if (!m_writing || !m_pendingWrites.isEmpty()) {
        return;
    }
    if (m_writeQueue.isEmpty()) {
        finishWriteBatch();
        return;
    }

    const ParamCompareRow row = m_writeQueue.takeFirst();
    const QString name = normalizedName(row.name);
    const QVariant typedValue =
        valueWithOriginalType(m_values.value(name), row.value);
    PendingWrite pending;
    pending.expectedValue = static_cast<double>(
        static_cast<float>(typedValue.toDouble()));
    pending.generation = ++m_writeGeneration;
    m_pendingWrites.insert(name, pending);
    emit writeRequested(m_componentId, name, typedValue);

    const quint64 generation = pending.generation;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, name, generation]() {
        const auto iterator = m_pendingWrites.constFind(name);
        if (iterator == m_pendingWrites.constEnd()
            || iterator->generation != generation) {
            return;
        }
        finishWrite(name, true);
    });
}

void ConfigInitialParamsViewModel::finishWrite(
    const QString &name, bool failed)
{
    const QString normalized = normalizedName(name);
    if (!m_pendingWrites.remove(normalized)) {
        return;
    }
    if (failed) {
        m_failedWrites.insert(normalized);
    } else {
        m_failedWrites.remove(normalized);
    }
    dispatchNextWrite();
}

void ConfigInitialParamsViewModel::finishWriteBatch()
{
    m_writing = false;
    m_writeQueue.clear();
    emit writingChanged();
    if (m_failedWrites.isEmpty()) {
        const QString throttleMixParameter = m_plane
            ? QStringLiteral("Q_A_THR_MIX_MAN")
            : QStringLiteral("ATC_THR_MIX_MAN");
        setStatus(tr("%1 initial parameter(s) successfully updated. Check "
                     "parameters before flight! After test flight set "
                     "%2 to 0.5.")
                      .arg(m_writeCount)
                      .arg(throttleMixParameter));
    } else {
        setStatus(tr("%1 parameter(s) failed to write.")
                      .arg(m_failedWrites.size()));
    }
}

QString ConfigInitialParamsViewModel::normalizedName(
    const QString &name) const
{
    return name.trimmed().toUpper();
}
