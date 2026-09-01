#include "ConfigSerialViewModel.h"

#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;

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

QString bitLabel(const SerialBitOption &option)
{
    return QStringLiteral("%1: %2").arg(option.bit).arg(option.label);
}
}

ConfigSerialViewModel::ConfigSerialViewModel(QObject *parent)
    : QObject(parent),
      m_note(tr("Note: Changes to the serial port settings will not take "
                "effect until the board is rebooted."))
{}

QMap<int, SerialOptionRule> ConfigSerialViewModel::OptionRules()
{
    const QString comment = tr(
        "If connecting a Mavlink sensor, consider setting 'Do not forward "
        "Mavlink to/from'");
    return {
        {1, {115, 0, comment}},
        {2, {-1, 0, comment}}
    };
}

void ConfigSerialViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildPorts();
}

void ConfigSerialViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_parameters = parameters;
    m_values.clear();
    m_pendingWrites.clear();
    m_supersededWrites.clear();
    m_writeErrors.clear();
    m_protocolComment.clear();

    QSet<int> serialComponents;
    static const QRegularExpression serialBaudPattern(
        QStringLiteral("^SERIAL([1-9][0-9]*)_BAUD$"),
        QRegularExpression::CaseInsensitiveOption);
    for (const ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (serialBaudPattern.match(parameter.name.trimmed()).hasMatch()) {
            serialComponents.insert(parameter.componentId);
        }
    }
    if (serialComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (serialComponents.contains(1)) {
        m_componentId = 1;
    } else if (!serialComponents.isEmpty()) {
        QList<int> components = serialComponents.values();
        std::sort(components.begin(), components.end());
        m_componentId = components.first();
    } else {
        m_componentId = preferredComponent;
    }

    for (const ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    rebuildPorts();
}

QList<SerialPortRow> ConfigSerialViewModel::Ports() const
{
    return m_ports;
}

QString ConfigSerialViewModel::Note() const
{
    return m_note;
}

QString ConfigSerialViewModel::Warning() const
{
    return m_warning;
}

bool ConfigSerialViewModel::HasWarning() const
{
    return !m_warning.isEmpty();
}

bool ConfigSerialViewModel::selectBaud(
    const QString &portName, const QVariant &value)
{
    SerialPortRow *row = rowForPort(portName);
    if (!row || !optionExists(row->baudOptions, value)) {
        return false;
    }
    const QString name = row->portName + QStringLiteral("_BAUD");
    if (!hasParameter(name) || variantsEqual(row->selectedBaud, value)) {
        return false;
    }
    row->selectedBaud = value;
    queueWrite(row, name, value);
    emit rowChanged(row->portName);
    return true;
}

bool ConfigSerialViewModel::selectProtocol(
    const QString &portName, const QVariant &value)
{
    SerialPortRow *row = rowForPort(portName);
    if (!row || !row->hasProtocol
        || !optionExists(row->protocolOptions, value)) {
        return false;
    }

    bool protocolOk = false;
    const int protocol = value.toInt(&protocolOk);
    if (!protocolOk) {
        return false;
    }
    const QString rowPrefix = row->portName + QLatin1Char('_');
    for (auto iterator = m_writeErrors.begin();
         iterator != m_writeErrors.end();) {
        if (iterator.key().startsWith(rowPrefix)) {
            iterator = m_writeErrors.erase(iterator);
        } else {
            ++iterator;
        }
    }
    const QString protocolName =
        row->portName + QStringLiteral("_PROTOCOL");
    if (!variantsEqual(row->selectedProtocol, value)) {
        row->selectedProtocol = value;
        queueWrite(row, protocolName, value);
    }

    m_protocolComment.clear();
    const QMap<int, SerialOptionRule> rules = OptionRules();
    const auto ruleIterator = rules.constFind(protocol);
    if (ruleIterator != rules.constEnd()) {
        const SerialOptionRule &rule = ruleIterator.value();
        if (rule.baudrate >= 0
            && optionExists(row->baudOptions, rule.baudrate)) {
            const QString baudName =
                row->portName + QStringLiteral("_BAUD");
            if (hasParameter(baudName)
                && !variantsEqual(row->selectedBaud, rule.baudrate)) {
                row->selectedBaud = rule.baudrate;
                queueWrite(row, baudName, rule.baudrate);
            }
        }
        if (rule.options >= 0 && row->hasOptions
            && row->optionsValue != static_cast<qulonglong>(rule.options)) {
            const QString optionsName =
                row->portName + QStringLiteral("_OPTIONS");
            updateOptions(row, static_cast<qulonglong>(rule.options));
            queueWrite(row, optionsName,
                       QVariant::fromValue<qlonglong>(rule.options));
        }
        m_protocolComment =
            row->portName + QStringLiteral(" : ") + rule.comment;
    }

    recomputeWarning();
    emit rowChanged(row->portName);
    return true;
}

bool ConfigSerialViewModel::setOptionBit(
    const QString &portName, int bit, bool enabled)
{
    SerialPortRow *row = rowForPort(portName);
    if (!row || !row->hasOptions || bit < 0 || bit >= 63) {
        return false;
    }
    bool knownBit = false;
    for (const SerialBitOption &option : row->optionBits) {
        knownBit = knownBit || option.bit == bit;
    }
    if (!knownBit) {
        return false;
    }

    qulonglong value = row->optionsValue;
    const qulonglong mask = qulonglong(1) << bit;
    if (enabled) {
        value |= mask;
    } else {
        value &= ~mask;
    }
    if (value == row->optionsValue) {
        return false;
    }

    updateOptions(row, value);
    queueWrite(row, row->portName + QStringLiteral("_OPTIONS"),
               QVariant::fromValue(value));
    emit rowChanged(row->portName);
    return true;
}

void ConfigSerialViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    const bool newSerialPort =
        QRegularExpression(QStringLiteral("^SERIAL([1-9][0-9]*)_BAUD$"))
            .match(normalized).hasMatch()
        && !m_values.contains(normalized);

    auto pendingIterator = m_pendingWrites.find(normalized);
    if (pendingIterator == m_pendingWrites.end()) {
        const QList<QVariant> superseded =
            m_supersededWrites.value(normalized);
        const bool staleEcho = std::any_of(
            superseded.constBegin(), superseded.constEnd(),
            [&value](const QVariant &candidate) {
                return variantsEqual(candidate, value);
            });
        if (staleEcho) {
            return;
        }
    }

    m_values.insert(normalized, value);

    if (newSerialPort) {
        rebuildPorts();
        return;
    }

    QString portName;
    if (pendingIterator != m_pendingWrites.end()) {
        portName = pendingIterator->portName;
    } else {
        static const QRegularExpression serialParameterPattern(
            QStringLiteral("^(SERIAL[1-9][0-9]*)_"));
        static const QRegularExpression rtsCtsPattern(
            QStringLiteral("^BRD_SER([1-9][0-9]*)_RTSCTS$"));
        const QRegularExpressionMatch serialMatch =
            serialParameterPattern.match(normalized);
        const QRegularExpressionMatch rtsCtsMatch =
            rtsCtsPattern.match(normalized);
        if (serialMatch.hasMatch()) {
            portName = serialMatch.captured(1);
        } else if (rtsCtsMatch.hasMatch()) {
            portName = QStringLiteral("SERIAL%1")
                .arg(rtsCtsMatch.captured(1));
        }
    }
    SerialPortRow *row = rowForPort(portName);
    if (!row) {
        return;
    }

    if (pendingIterator != m_pendingWrites.end()) {
        if (!variantsEqual(pendingIterator->expectedValue, value)) {
            QList<QVariant> &superseded = m_supersededWrites[normalized];
            const auto staleIterator = std::find_if(
                superseded.begin(), superseded.end(),
                [&value](const QVariant &candidate) {
                    return variantsEqual(candidate, value);
                });
            if (staleIterator != superseded.end()) {
                return;
            }
        }
        const bool matched =
            variantsEqual(pendingIterator->expectedValue, value);
        applyValueToRow(row, normalized, value);
        if (matched) {
            m_pendingWrites.erase(pendingIterator);
            m_writeErrors.remove(normalized);
        } else {
            m_writeErrors.insert(normalized, tr("write mismatch"));
        }
        updateRowStatus(row);
    } else {
        applyValueToRow(row, normalized, value);
        if (normalized.startsWith(row->portName + QLatin1Char('_'))) {
            m_writeErrors.remove(normalized);
            updateRowStatus(row);
        }
    }
    if (normalized.startsWith(QStringLiteral("BRD_SER"))) {
        updateLabel(row);
    }
    recomputeWarning();
    emit rowChanged(row->portName);
}

void ConfigSerialViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId != m_componentId) {
        return;
    }
    finishPending(normalizedName(name),
                  reason.isEmpty() ? tr("write failed") : reason,
                  true);
}

void ConfigSerialViewModel::rebuildPorts()
{
    m_ports.clear();
    static const QRegularExpression serialBaudPattern(
        QStringLiteral("^SERIAL([1-9][0-9]*)_BAUD$"));
    QList<int> portIndices;
    for (auto iterator = m_values.constBegin(); iterator != m_values.constEnd();
         ++iterator) {
        const QRegularExpressionMatch match =
            serialBaudPattern.match(iterator.key());
        if (!match.hasMatch()) {
            continue;
        }
        const int portIndex = match.captured(1).toInt();
        if (!portIndices.contains(portIndex)) {
            portIndices.append(portIndex);
        }
    }
    std::sort(portIndices.begin(), portIndices.end());

    for (int portIndex : portIndices) {
        SerialPortRow row;
        row.componentId = m_componentId;
        row.portIndex = portIndex;
        row.portName = QStringLiteral("SERIAL%1").arg(portIndex);
        rebuildRowMetadata(&row);
        hydrateRow(&row);
        m_ports.append(row);
    }
    recomputeWarning();
    emit structureChanged();
}

void ConfigSerialViewModel::rebuildRowMetadata(SerialPortRow *row)
{
    row->baudOptions = enumOptions(
        row->portName + QStringLiteral("_BAUD"),
        QStringLiteral("SERIAL1_BAUD"));
    row->protocolOptions = enumOptions(
        row->portName + QStringLiteral("_PROTOCOL"),
        QStringLiteral("SERIAL1_PROTOCOL"));
    row->hasProtocol = hasParameter(
        row->portName + QStringLiteral("_PROTOCOL"));
    row->hasOptions = hasParameter(
        row->portName + QStringLiteral("_OPTIONS"));
    if (row->hasOptions) {
        row->optionBits = bitOptions(
            row->portName + QStringLiteral("_OPTIONS"),
            QStringLiteral("SERIAL1_OPTIONS"));
        row->hasBits = !row->optionBits.isEmpty();
    }
}

void ConfigSerialViewModel::hydrateRow(SerialPortRow *row)
{
    row->selectedBaud = authoritativeValue(
        row->portName + QStringLiteral("_BAUD"));
    row->selectedProtocol = authoritativeValue(
        row->portName + QStringLiteral("_PROTOCOL"));
    updateOptions(row, authoritativeValue(
        row->portName + QStringLiteral("_OPTIONS")).toULongLong());
    updateLabel(row);
}

void ConfigSerialViewModel::applyValueToRow(
    SerialPortRow *row, const QString &name, const QVariant &value)
{
    if (name == row->portName + QStringLiteral("_BAUD")) {
        row->selectedBaud = value;
    } else if (name == row->portName + QStringLiteral("_PROTOCOL")) {
        row->selectedProtocol = value;
    } else if (name == row->portName + QStringLiteral("_OPTIONS")) {
        updateOptions(row, value.toULongLong());
    }
}

void ConfigSerialViewModel::queueWrite(
    SerialPortRow *row, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    const auto previous = m_pendingWrites.constFind(normalized);
    if (previous != m_pendingWrites.constEnd()
        && !variantsEqual(previous->expectedValue, value)) {
        const QVariant supersededValue = previous->expectedValue;
        m_supersededWrites[normalized].append(supersededValue);
        QTimer::singleShot(kWriteTimeoutMs * 2, this,
                           [this, normalized, supersededValue]() {
            auto iterator = m_supersededWrites.find(normalized);
            if (iterator == m_supersededWrites.end()) {
                return;
            }
            const auto valueIterator = std::find_if(
                iterator->begin(), iterator->end(),
                [&supersededValue](const QVariant &candidate) {
                    return variantsEqual(candidate, supersededValue);
                });
            if (valueIterator != iterator->end()) {
                iterator->erase(valueIterator);
            }
            if (iterator->isEmpty()) {
                m_supersededWrites.erase(iterator);
            }
        });
    }
    PendingWrite pending;
    pending.portName = row->portName;
    pending.expectedValue = value;
    pending.generation = ++m_writeGeneration;
    m_pendingWrites.insert(normalized, pending);
    m_writeErrors.remove(normalized);
    row->status = QStringLiteral("…");
    emit writeRequested(row->componentId, normalized, value);

    const quint64 generation = pending.generation;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, normalized, generation]() {
        const auto iterator = m_pendingWrites.constFind(normalized);
        if (iterator == m_pendingWrites.constEnd()
            || iterator->generation != generation) {
            return;
        }
        finishPending(normalized, tr("write failed"), true);
    });
}

void ConfigSerialViewModel::finishPending(
    const QString &name, const QString &status,
    bool revertToAuthoritativeValue)
{
    auto iterator = m_pendingWrites.find(name);
    if (iterator == m_pendingWrites.end()) {
        return;
    }
    const QString portName = iterator->portName;
    m_pendingWrites.erase(iterator);
    m_supersededWrites.remove(name);
    SerialPortRow *row = rowForPort(portName);
    if (!row) {
        return;
    }
    if (revertToAuthoritativeValue) {
        applyValueToRow(row, name, authoritativeValue(name));
    }
    m_writeErrors.insert(name, status);
    updateRowStatus(row);
    recomputeWarning();
    emit rowChanged(row->portName);
}

void ConfigSerialViewModel::updateRowStatus(SerialPortRow *row)
{
    bool stillPending = false;
    for (auto iterator = m_pendingWrites.constBegin();
         iterator != m_pendingWrites.constEnd(); ++iterator) {
        if (iterator->portName == row->portName) {
            stillPending = true;
            break;
        }
    }
    QString error;
    const QString prefix = row->portName + QLatin1Char('_');
    for (auto iterator = m_writeErrors.constBegin();
         iterator != m_writeErrors.constEnd(); ++iterator) {
        if (iterator.key().startsWith(prefix)) {
            error = iterator.value();
            break;
        }
    }
    row->status = !error.isEmpty()
        ? error
        : (stillPending ? QStringLiteral("…") : QStringLiteral("✓"));
}

void ConfigSerialViewModel::updateOptions(
    SerialPortRow *row, qulonglong value)
{
    row->optionsValue = value;
    QStringList selected;
    for (SerialBitOption &option : row->optionBits) {
        option.isSet = option.bit >= 0 && option.bit < 63
            && (value & (qulonglong(1) << option.bit)) != 0;
        if (option.isSet) {
            selected.append(bitLabel(option));
        }
    }
    row->optionsText = selected.join(QStringLiteral(" / "));
}

void ConfigSerialViewModel::updateLabel(SerialPortRow *row)
{
    row->label = tr("SERIAL PORT %1").arg(row->portIndex);
    const int rtsCts = authoritativeValue(
        QStringLiteral("BRD_SER%1_RTSCTS").arg(row->portIndex)).toInt();
    if (rtsCts == 1) {
        row->label += tr(" (RTS/CTS)");
    } else if (rtsCts == 2) {
        row->label += tr(" (RTS/CTS Auto)");
    }
}

void ConfigSerialViewModel::recomputeWarning(const QString &comment)
{
    int mavlinkPorts = 0;
    for (const SerialPortRow &row : m_ports) {
        const int protocol = row.selectedProtocol.toInt();
        if (protocol == 1 || protocol == 2) {
            ++mavlinkPorts;
        }
    }
    if (!comment.isNull()) {
        m_protocolComment = comment;
    }
    QString warning = m_protocolComment;
    if (mavlinkPorts >= 4) {
        if (!warning.isEmpty()) {
            warning += QLatin1Char('\n');
        }
        warning += tr("Warning: Maximum number of Mavlink ports are 5 "
                      "including the USB port!");
    }
    if (m_warning != warning) {
        m_warning = warning;
        emit warningChanged();
    }
}

QList<ParamOption> ConfigSerialViewModel::enumOptions(
    const QString &name, const QString &templateName) const
{
    ParameterMetaData metadata = m_catalog.value(name);
    if (metadata.values.isEmpty()) {
        metadata = m_catalog.value(templateName);
    }
    QList<ParamOption> result;
    for (const ParameterMetaDataOption &option : metadata.values) {
        result.append({option.value, option.label});
    }
    return result;
}

QList<SerialBitOption> ConfigSerialViewModel::bitOptions(
    const QString &name, const QString &templateName) const
{
    ParameterMetaData metadata = m_catalog.value(name);
    if (metadata.bitmaskValues.isEmpty()) {
        metadata = m_catalog.value(templateName);
    }
    QList<SerialBitOption> result;
    for (const QPair<int, QString> &option : metadata.bitmaskValues) {
        if (option.first >= 0 && option.first < 63) {
            result.append({option.first, option.second, false});
        }
    }
    return result;
}

SerialPortRow *ConfigSerialViewModel::rowForPort(const QString &portName)
{
    const QString normalized = portName.trimmed().toUpper();
    for (SerialPortRow &row : m_ports) {
        if (row.portName == normalized) {
            return &row;
        }
    }
    return nullptr;
}

const SerialPortRow *ConfigSerialViewModel::rowForPort(
    const QString &portName) const
{
    const QString normalized = portName.trimmed().toUpper();
    for (const SerialPortRow &row : m_ports) {
        if (row.portName == normalized) {
            return &row;
        }
    }
    return nullptr;
}

QString ConfigSerialViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}

QVariant ConfigSerialViewModel::authoritativeValue(const QString &name) const
{
    return m_values.value(normalizedName(name));
}

bool ConfigSerialViewModel::hasParameter(const QString &name) const
{
    return m_values.contains(normalizedName(name));
}

bool ConfigSerialViewModel::optionExists(
    const QList<ParamOption> &options, const QVariant &value) const
{
    for (const ParamOption &option : options) {
        if (variantsEqual(option.value, value)) {
            return true;
        }
    }
    return false;
}
