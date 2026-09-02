#include "BatteryMonitorInstanceModel.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

template<typename Integer>
QVariant checkedIntegerValue(double value)
{
    if (!std::isfinite(value) || std::trunc(value) != value) {
        return {};
    }
    constexpr int valueBits = std::numeric_limits<Integer>::digits;
    const double upperExclusive = std::ldexp(1.0, valueBits);
    const double lowerInclusive = std::numeric_limits<Integer>::is_signed
        ? -upperExclusive : 0.0;
    if (value < lowerInclusive || value >= upperExclusive) {
        return {};
    }
    return QVariant::fromValue(static_cast<Integer>(value));
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
        return checkedIntegerValue<int>(value);
    case QMetaType::UInt:
        return checkedIntegerValue<uint>(value);
    case QMetaType::LongLong:
        return checkedIntegerValue<qlonglong>(value);
    case QMetaType::ULongLong:
        return checkedIntegerValue<qulonglong>(value);
    case QMetaType::Float:
        if (!std::isfinite(value)
            || std::abs(value) > std::numeric_limits<float>::max()) {
            return {};
        }
        return static_cast<float>(value);
    case QMetaType::Double:
        return std::isfinite(value) ? QVariant(value) : QVariant();
    default:
        return value;
    }
}

} // namespace

BatteryMonitorInstanceModel::BatteryMonitorInstanceModel(
    const QString &prefix, int batteryId, QObject *parent)
    : QObject(parent),
      m_prefix(prefix.trimmed().toUpper()),
      m_batteryId(std::max(0, batteryId)),
      m_ampPerVoltParameter(
          AmpPerVoltCandidates(prefix.trimmed().toUpper()).constFirst())
{
    Q_ASSERT(!m_prefix.isEmpty());
    setStatus(tr("Vehicle battery parameters are not available."));
}

bool BatteryMonitorInstanceModel::Available() const
{
    return m_values.contains(MonitorParameter());
}

bool BatteryMonitorInstanceModel::ShouldTriggerBatteryAlert(
    double voltage, double remainingPercent,
    double warningVoltage, double warningPercent)
{
    if (!std::isfinite(voltage) || !std::isfinite(remainingPercent)
        || !std::isfinite(warningVoltage)
        || !std::isfinite(warningPercent)
        || voltage < 5.0) {
        return false;
    }
    const bool lowVoltage = voltage <= warningVoltage;
    const bool lowPercent = remainingPercent != 0.0
        && remainingPercent < warningPercent;
    return lowVoltage || lowPercent;
}

QString BatteryMonitorInstanceModel::FormatBatteryAlert(
    const QString &messageTemplate,
    double voltage, double remainingPercent)
{
    QString message = messageTemplate.trimmed();
    if (message.isEmpty()) {
        message = QStringLiteral(
            "WARNING, Battery at {batv} Volt, {batp} percent");
    }
    message.replace(QStringLiteral("{batv}"),
                    QString::number(voltage, 'f', 2));
    message.replace(QStringLiteral("{batp}"),
                    QString::number(remainingPercent, 'f', 0));
    return message;
}

QString BatteryMonitorInstanceModel::MonitorParameter() const
{
    return m_prefix + QStringLiteral("_MONITOR");
}

QString BatteryMonitorInstanceModel::CapacityParameter() const
{
    return m_prefix + QStringLiteral("_CAPACITY");
}

QString BatteryMonitorInstanceModel::VoltPinParameter() const
{
    return m_prefix + QStringLiteral("_VOLT_PIN");
}

QString BatteryMonitorInstanceModel::CurrPinParameter() const
{
    return m_prefix + QStringLiteral("_CURR_PIN");
}

QString BatteryMonitorInstanceModel::VoltMultiplierParameter() const
{
    return m_prefix + QStringLiteral("_VOLT_MULT");
}

QString BatteryMonitorInstanceModel::AmpOffsetParameter() const
{
    return m_prefix + QStringLiteral("_AMP_OFFSET");
}

QStringList BatteryMonitorInstanceModel::ParameterNames() const
{
    return {MonitorParameter(), CapacityParameter(), VoltPinParameter(),
            CurrPinParameter(), VoltMultiplierParameter(),
            AmpPerVoltParameter(), AmpOffsetParameter()};
}

QStringList BatteryMonitorInstanceModel::AmpPerVoltCandidates(
    const QString &prefix)
{
    const QString normalized = prefix.trimmed().toUpper();
    return {normalized + QStringLiteral("_AMP_PERVLT"),
            normalized + QStringLiteral("_AMP_PERVOL"),
            normalized + QStringLiteral("_AMP_PERVOLT")};
}

QString BatteryMonitorInstanceModel::ResolveAmpPerVoltParameter(
    const QString &prefix, const QStringList &availableParameters)
{
    QStringList normalizedAvailable;
    normalizedAvailable.reserve(availableParameters.size());
    for (const QString &name : availableParameters) {
        normalizedAvailable.append(name.trimmed().toUpper());
    }
    const QStringList candidates = AmpPerVoltCandidates(prefix);
    for (const QString &candidate : candidates) {
        if (normalizedAvailable.contains(candidate)) {
            return candidate;
        }
    }
    // This is the current ArduPilot spelling used by MP10. It is a default
    // for presentation only; writes are still rejected until it is present
    // in the active vehicle parameter snapshot.
    return candidates.constFirst();
}

bool BatteryMonitorInstanceModel::CalibratedScale(
    double measuredValue, double liveValue, double currentScale,
    double *newScale)
{
    if (!newScale || !std::isfinite(measuredValue)
        || !std::isfinite(liveValue) || !std::isfinite(currentScale)
        || qFuzzyIsNull(liveValue)) {
        return false;
    }
    const double calculated = measuredValue * currentScale / liveValue;
    if (!std::isfinite(calculated)) {
        return false;
    }
    *newScale = calculated;
    return true;
}

bool BatteryMonitorInstanceModel::AcceptsParameter(const QString &name) const
{
    const QString normalized = normalizedName(name);
    return normalized == MonitorParameter()
        || normalized == CapacityParameter()
        || normalized == VoltPinParameter()
        || normalized == CurrPinParameter()
        || normalized == VoltMultiplierParameter()
        || normalized == AmpOffsetParameter()
        || IsAmpPerVoltParameter(normalized);
}

bool BatteryMonitorInstanceModel::IsAmpPerVoltParameter(
    const QString &name) const
{
    return AmpPerVoltCandidates(m_prefix).contains(normalizedName(name));
}

bool BatteryMonitorInstanceModel::HasParameter(const QString &name) const
{
    return m_values.contains(normalizedName(name));
}

QVariant BatteryMonitorInstanceModel::ParameterValue(
    const QString &name) const
{
    return m_values.value(normalizedName(name), QVariant());
}

void BatteryMonitorInstanceModel::Reset(int componentId)
{
    const bool hadState = !m_values.isEmpty() || m_hasLiveVoltage
        || m_hasLiveCurrent || HasPendingWrite();
    m_values.clear();
    m_componentId = componentId >= 0 && componentId <= 255
        ? componentId : MAV_COMP_ID_PRIMARY;
    const QString defaultAmp = AmpPerVoltCandidates(m_prefix).constFirst();
    const bool structureWasChanged = m_ampPerVoltParameter != defaultAmp;
    m_ampPerVoltParameter = defaultAmp;
    m_liveVoltage = 0.0;
    m_liveCurrent = 0.0;
    m_hasLiveVoltage = false;
    m_hasLiveCurrent = false;
    m_pendingWrite = PendingWrite{};
    setStatus(m_connected
                  ? tr("%1 is not available on this vehicle.")
                        .arg(MonitorParameter())
                  : tr("Vehicle disconnected."));
    if (structureWasChanged) {
        emit structureChanged();
    }
    if (hadState) {
        emit liveValuesChanged();
        emit stateChanged();
    }
}

void BatteryMonitorInstanceModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        m_pendingWrite = PendingWrite{};
        m_liveVoltage = 0.0;
        m_liveCurrent = 0.0;
        m_hasLiveVoltage = false;
        m_hasLiveCurrent = false;
        setStatus(tr("Vehicle disconnected."));
        emit liveValuesChanged();
    } else if (!Available()) {
        setStatus(tr("%1 is not available on this vehicle.")
                      .arg(MonitorParameter()));
    } else {
        setStatus(tr("Battery monitor parameters loaded."));
    }
    emit stateChanged();
}

void BatteryMonitorInstanceModel::setLiveValues(
    double voltage, bool hasVoltage, double current, bool hasCurrent)
{
    const bool validVoltage = hasVoltage && std::isfinite(voltage);
    const bool validCurrent = hasCurrent && std::isfinite(current);
    if (m_hasLiveVoltage == validVoltage
        && m_hasLiveCurrent == validCurrent
        && (!validVoltage || m_liveVoltage == voltage)
        && (!validCurrent || m_liveCurrent == current)) {
        return;
    }
    m_hasLiveVoltage = validVoltage;
    m_hasLiveCurrent = validCurrent;
    m_liveVoltage = validVoltage ? voltage : 0.0;
    m_liveCurrent = validCurrent ? current : 0.0;
    emit liveValuesChanged();
}

void BatteryMonitorInstanceModel::observeMavlinkMessage(
    const mavlink_message_t &message)
{
    // ArduPilot still publishes BATTERY2 alongside instance-aware
    // BATTERY_STATUS on some versions. It represents the second monitor only.
    if (message.msgid == MAVLINK_MSG_ID_BATTERY2 && m_batteryId == 1) {
        mavlink_battery2_t battery{};
        mavlink_msg_battery2_decode(&message, &battery);
        const bool hasVoltage = battery.voltage != 0
            && battery.voltage != std::numeric_limits<quint16>::max();
        const bool hasCurrent = battery.current_battery != -1;
        setLiveValues(
            hasVoltage ? static_cast<double>(battery.voltage) / 1000.0
                       : m_liveVoltage,
            hasVoltage || m_hasLiveVoltage,
            hasCurrent
                ? static_cast<double>(battery.current_battery) / 100.0
                : m_liveCurrent,
            hasCurrent || m_hasLiveCurrent);
        return;
    }
    if (message.msgid != MAVLINK_MSG_ID_BATTERY_STATUS) {
        return;
    }
    mavlink_battery_status_t status{};
    mavlink_msg_battery_status_decode(&message, &status);
    if (status.id != m_batteryId) {
        return;
    }

    quint64 millivolts = 0;
    bool hasVoltage = false;
    for (int index = 0;
         index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN; ++index) {
        const quint16 cell = status.voltages[index];
        if (cell != 0 && cell != std::numeric_limits<quint16>::max()) {
            millivolts += cell;
            hasVoltage = true;
        }
    }
    for (int index = 0;
         index < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_EXT_LEN;
         ++index) {
        const quint16 cell = status.voltages_ext[index];
        if (cell != 0 && cell != std::numeric_limits<quint16>::max()) {
            millivolts += cell;
            hasVoltage = true;
        }
    }
    const bool hasCurrent = status.current_battery != -1;
    setLiveValues(
        hasVoltage ? static_cast<double>(millivolts) / 1000.0
                   : m_liveVoltage,
        hasVoltage || m_hasLiveVoltage,
        hasCurrent
            ? static_cast<double>(status.current_battery) / 100.0
            : m_liveCurrent,
        hasCurrent || m_hasLiveCurrent);
}

bool BatteryMonitorInstanceModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (HasPendingWrite()) {
        setStatus(tr("Wait for %1 to finish writing.")
                      .arg(m_pendingWrite.name));
        return false;
    }
    if (!CanEdit() || !AcceptsParameter(normalized)
        || !m_values.contains(normalized)) {
        setStatus(tr("%1 is not available for this battery instance.")
                      .arg(normalized));
        return false;
    }
    bool ok = false;
    const double numeric = value.toDouble(&ok);
    if (!ok || !std::isfinite(numeric)) {
        setStatus(tr("Invalid value for %1.").arg(normalized));
        return false;
    }
    const QVariant typed = valueWithOriginalType(
        m_values.value(normalized), numeric);
    if (!typed.isValid()) {
        setStatus(tr("%1 cannot be represented by the parameter's value type.")
                      .arg(normalized));
        return false;
    }
    m_pendingWrite.name = normalized;
    m_pendingWrite.expectedValue = typed;
    setStatus(tr("Writing %1 = %2…")
                  .arg(normalized, typed.toString()));
    emit stateChanged();
    emit writeRequested(m_componentId, normalized, typed);
    return true;
}

bool BatteryMonitorInstanceModel::ApplyVoltageCalibration(
    double measuredVoltage)
{
    if (!m_hasLiveVoltage) {
        setStatus(tr("No live voltage — cannot calibrate."));
        return false;
    }
    bool scaleOk = false;
    const double currentScale = ParameterValue(
        VoltMultiplierParameter()).toDouble(&scaleOk);
    if (!scaleOk) {
        setStatus(tr("%1 is not available for voltage calibration.")
                      .arg(VoltMultiplierParameter()));
        return false;
    }
    double newScale = 0.0;
    if (!CalibratedScale(
            measuredVoltage, m_liveVoltage, currentScale, &newScale)) {
        setStatus(tr("Voltage calibration values are invalid."));
        return false;
    }
    return setFieldValue(VoltMultiplierParameter(), newScale);
}

bool BatteryMonitorInstanceModel::ApplyCurrentCalibration(
    double measuredCurrent)
{
    if (!m_hasLiveCurrent) {
        setStatus(tr("No live current — cannot calibrate."));
        return false;
    }
    bool scaleOk = false;
    const double currentScale = ParameterValue(
        AmpPerVoltParameter()).toDouble(&scaleOk);
    if (!scaleOk) {
        setStatus(tr("%1 is not available for current calibration.")
                      .arg(AmpPerVoltParameter()));
        return false;
    }
    double newScale = 0.0;
    if (!CalibratedScale(
            measuredCurrent, m_liveCurrent, currentScale, &newScale)) {
        setStatus(tr("Current calibration values are invalid."));
        return false;
    }
    return setFieldValue(AmpPerVoltParameter(), newScale);
}

void BatteryMonitorInstanceModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (componentId != m_componentId
        || !AcceptsParameter(normalized)) {
        return;
    }
    const bool wasAvailable = Available();
    m_values.insert(normalized, value);
    const QString previousAmp = m_ampPerVoltParameter;
    resolveAmpPerVoltParameter();
    if (previousAmp != m_ampPerVoltParameter) {
        emit structureChanged();
    }
    emit fieldChanged(normalized);
    if (wasAvailable != Available()) {
        setStatus(m_connected
                      ? tr("Battery monitor parameters loaded.")
                      : tr("Vehicle disconnected."));
        emit stateChanged();
    }
}

void BatteryMonitorInstanceModel::parameterBatchSubmitted(
    int componentId, const QString &name, qulonglong batchId)
{
    if (componentId != m_componentId || batchId == 0
        || m_pendingWrite.batchId != 0
        || m_pendingWrite.name != normalizedName(name)) {
        return;
    }
    m_pendingWrite.batchId = batchId;
}

void BatteryMonitorInstanceModel::parameterWriteAcknowledged(
    int componentId, const QString &name, const QVariant &value, int type)
{
    Q_UNUSED(type)
    if (componentId != m_componentId || !AcceptsParameter(name)) {
        return;
    }
    parameterChanged(componentId, name, value);
    if (m_pendingWrite.name == normalizedName(name)) {
        bool requestedOk = false;
        bool acknowledgedOk = false;
        const double requested =
            m_pendingWrite.expectedValue.toDouble(&requestedOk);
        const double acknowledged = value.toDouble(&acknowledgedOk);
        m_pendingWrite.acknowledged = requestedOk && acknowledgedOk
            && std::isfinite(requested) && std::isfinite(acknowledged)
            && std::abs(requested - acknowledged) <= 1.0e-6;
    }
}

void BatteryMonitorInstanceModel::parameterWriteCancelled(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name)
{
    Q_UNUSED(transactionId)
    if (!pendingMatches(componentId, name, batchId)) {
        return;
    }
    m_pendingWrite.failed = true;
    m_pendingWrite.failureMessage = tr("write cancelled");
    setStatus(tr("%1 write cancelled.").arg(m_pendingWrite.name));
}

void BatteryMonitorInstanceModel::parameterWriteFailed(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name, int reason, const QString &message)
{
    Q_UNUSED(transactionId)
    Q_UNUSED(reason)
    if (!pendingMatches(componentId, name, batchId)) {
        return;
    }
    m_pendingWrite.failed = true;
    m_pendingWrite.failureMessage = message;
    setStatus(tr("%1 write failed: %2")
                  .arg(m_pendingWrite.name, message));
}

void BatteryMonitorInstanceModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!HasPendingWrite() || m_pendingWrite.batchId == 0
        || m_pendingWrite.batchId != batchId) {
        return;
    }
    const QString name = m_pendingWrite.name;
    const bool writeFailed = m_pendingWrite.failed || failed > 0
        || succeeded < 1;
    const bool acknowledged = m_pendingWrite.acknowledged;
    const QString failure = m_pendingWrite.failureMessage;
    clearPendingWrite();
    if (writeFailed) {
        setStatus(failure.isEmpty()
                      ? tr("%1 write failed.").arg(name)
                      : tr("%1 write failed: %2").arg(name, failure));
    } else {
        setStatus(acknowledged
                      ? tr("%1 acknowledged.").arg(name)
                      : tr("%1 written.").arg(name));
    }
}

void BatteryMonitorInstanceModel::parameterWriteSubmissionFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId != m_componentId || m_pendingWrite.batchId != 0
        || m_pendingWrite.name != normalizedName(name)) {
        return;
    }
    const QString normalized = m_pendingWrite.name;
    clearPendingWrite();
    setStatus(tr("%1 write failed: %2").arg(normalized, reason));
}

bool BatteryMonitorInstanceModel::pendingMatches(
    int componentId, const QString &name, qulonglong batchId) const
{
    return componentId == m_componentId && HasPendingWrite()
        && m_pendingWrite.batchId != 0
        && m_pendingWrite.batchId == batchId
        && m_pendingWrite.name == normalizedName(name);
}

void BatteryMonitorInstanceModel::clearPendingWrite()
{
    if (!HasPendingWrite()) {
        return;
    }
    m_pendingWrite = PendingWrite{};
    emit stateChanged();
}

QString BatteryMonitorInstanceModel::normalizedName(
    const QString &name) const
{
    return name.trimmed().toUpper();
}

void BatteryMonitorInstanceModel::resolveAmpPerVoltParameter()
{
    m_ampPerVoltParameter = ResolveAmpPerVoltParameter(
        m_prefix, m_values.keys());
}

void BatteryMonitorInstanceModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}
