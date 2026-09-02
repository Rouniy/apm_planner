#include "ConfigHWCANViewModel.h"

#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
const QString kCanEnable = QStringLiteral("BRD_CAN_ENABLE");
constexpr int kAccepted = 0;   // MAV_RESULT_ACCEPTED
constexpr int kInProgress = 5; // MAV_RESULT_IN_PROGRESS

const QRegularExpression &physicalPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^CAN_P([1-9][0-9]*)_(DRIVER|BITRATE|FDBITRATE)$"));
    return pattern;
}

const QRegularExpression &driverPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^CAN_D([1-9][0-9]*)_(PROTOCOL|PROTOCOL2)$"));
    return pattern;
}

bool numericValuesEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        // Every CAN parameter represented here is integral. Requiring an
        // exact numeric echo avoids accepting a neighboring enum/bitrate.
        return leftValue == rightValue;
    }
    return left == right;
}

bool containsOption(const QList<ParamOption> &options,
                    const QVariant &value)
{
    return std::any_of(options.constBegin(), options.constEnd(),
                       [&value](const ParamOption &option) {
        return numericValuesEqual(option.value, value);
    });
}

int suffixOrder(const QString &name)
{
    if (name.endsWith(QLatin1String("_DRIVER"))) {
        return 0;
    }
    if (name.endsWith(QLatin1String("_BITRATE"))
        && !name.endsWith(QLatin1String("_FDBITRATE"))) {
        return 1;
    }
    if (name.endsWith(QLatin1String("_FDBITRATE"))) {
        return 2;
    }
    if (name.endsWith(QLatin1String("_PROTOCOL"))) {
        return 0;
    }
    return 1;
}

QVariant typedNumericValue(double value, const QVariant &reference)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::UInt:
        return static_cast<uint>(std::llround(value));
    case QMetaType::LongLong:
        return static_cast<qlonglong>(std::llround(value));
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(std::llround(value));
    case QMetaType::Float:
        return static_cast<float>(value);
    case QMetaType::Double:
        return value;
    default:
        return static_cast<int>(std::llround(value));
    }
}
} // namespace

ConfigHWCANViewModel::ConfigHWCANViewModel(QObject *parent)
    : QObject(parent)
{
    rebuildOptions();
    rebuildFields();
}

QString ConfigHWCANViewModel::Title() const
{
    return tr("CAN");
}

QList<int> ConfigHWCANViewModel::PhysicalPortIndexes() const
{
    QSet<int> indexes;
    for (const ParamField &field : m_fields) {
        const QRegularExpressionMatch match =
            physicalPattern().match(field.name);
        if (match.hasMatch()) {
            indexes.insert(match.captured(1).toInt());
        }
    }
    QList<int> result = indexes.values();
    std::sort(result.begin(), result.end());
    return result;
}

QList<int> ConfigHWCANViewModel::DriverIndexes() const
{
    QSet<int> indexes;
    for (const ParamField &field : m_fields) {
        const QRegularExpressionMatch match = driverPattern().match(field.name);
        if (match.hasMatch()) {
            indexes.insert(match.captured(1).toInt());
        }
    }
    QList<int> result = indexes.values();
    std::sort(result.begin(), result.end());
    return result;
}

bool ConfigHWCANViewModel::HasModernFields() const
{
    return !PhysicalPortIndexes().isEmpty() || !DriverIndexes().isEmpty();
}

bool ConfigHWCANViewModel::Busy() const
{
    return m_parameterWritePending || m_pendingCommand != 0;
}

bool ConfigHWCANViewModel::CanEditCanEnable() const
{
    return m_connected && m_snapshotReady && !m_vehicleArmed
        && m_hasCanEnable && !Busy()
        && !m_options.isEmpty();
}

bool ConfigHWCANViewModel::CanIssueCommands() const
{
    return m_connected && m_snapshotReady && !m_vehicleArmed && !Busy()
        && !m_commandResultUncertain;
}

bool ConfigHWCANViewModel::CanFactoryReset() const
{
    return CanIssueCommands() && m_factoryResetArmed;
}

void ConfigHWCANViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildOptions();
    rebuildFields();
}

void ConfigHWCANViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> components;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (isRelevantParameter(normalizedName(parameter.name))) {
            components.insert(parameter.componentId);
        }
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

    ++m_parameterGeneration;
    ++m_commandGeneration;
    m_parameterWritePending = false;
    m_pendingParameterName.clear();
    m_pendingCommand = 0;
    m_pendingCommandLabel.clear();
    m_factoryResetArmed = false;
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        const QString name = normalizedName(parameter.name);
        if (parameter.componentId == m_componentId
            && isRelevantParameter(name)) {
            m_values.insert(name, parameter.value);
        }
    }
    m_snapshotReady = true;
    m_hasCanEnable = m_values.contains(kCanEnable);
    rebuildOptions();
    rebuildFields();
    if (m_hasCanEnable) {
        selectAuthoritativeValue(m_values.value(kCanEnable));
    } else {
        m_selectedCanEnable.clear();
        emit selectedCanEnableChanged();
    }
    if (!m_hasCanEnable && !HasModernFields()) {
        setStatus(tr("No CAN configuration parameters are available."));
    } else {
        setStatus(QString());
    }
    emit stateChanged();
}

void ConfigHWCANViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!m_connected) {
        m_vehicleArmed = false;
        ++m_parameterGeneration;
        ++m_commandGeneration;
        m_parameterWritePending = false;
        m_pendingParameterName.clear();
        m_pendingCommand = 0;
        m_pendingCommandLabel.clear();
        m_factoryResetArmed = false;
        m_commandResultUncertain = false;
        m_snapshotReady = false;
        m_hasCanEnable = false;
        m_values.clear();
        m_selectedCanEnable.clear();
        rebuildOptions();
        rebuildFields();
        emit selectedCanEnableChanged();
        setStatus(tr("offline"));
    }
    emit stateChanged();
}

void ConfigHWCANViewModel::setArmed(bool armed)
{
    if (m_vehicleArmed == armed) {
        return;
    }
    const QString armedNotice =
        tr("Disarm the vehicle before changing CAN setup.");
    m_vehicleArmed = armed;
    if (m_vehicleArmed) {
        m_factoryResetArmed = false;
        if (m_parameterWritePending) {
            finishParameterWrite(false, tr("vehicle armed"));
        } else if (m_pendingCommand == 0) {
            setStatus(armedNotice);
        }
    } else if (m_status == armedNotice) {
        setStatus(QString());
    }
    emit stateChanged();
}

bool ConfigHWCANViewModel::SetSelectedCanEnable(const QVariant &value)
{
    return setFieldValue(kCanEnable, value);
}

bool ConfigHWCANViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (!m_connected) {
        setStatus(tr("offline"));
        return false;
    }
    if (!m_snapshotReady) {
        setStatus(tr("CAN parameter snapshot is not ready."));
        return false;
    }
    if (m_vehicleArmed) {
        setStatus(tr("Disarm the vehicle before changing CAN setup."));
        return false;
    }
    if (Busy()) {
        setStatus(tr("Wait for the current CAN operation to finish."));
        return false;
    }
    ParamField *field = fieldForName(normalized);
    if (!field || field->readOnly) {
        setStatus(tr("%1 is unavailable.").arg(normalized));
        return false;
    }

    QVariant candidate;
    if (field->editorKind == ParamField::EditorKind::Combo) {
        const auto option = std::find_if(
            field->options.constBegin(), field->options.constEnd(),
            [&value](const ParamOption &entry) {
                return numericValuesEqual(entry.value, value);
            });
        if (option == field->options.constEnd()) {
            setStatus(tr("Unsupported %1 value.").arg(normalized));
            return false;
        }
        candidate = option->value;
    } else {
        bool ok = false;
        const double numeric = value.toDouble(&ok);
        if (!ok || !std::isfinite(numeric)
            || (field->hasRange
                && (numeric < field->minimum
                    || numeric > field->maximum))) {
            setStatus(tr("Invalid %1 value.").arg(normalized));
            return false;
        }
        candidate = typedNumericValue(numeric, field->value);
    }
    if (numericValuesEqual(field->value, candidate)) {
        return false;
    }

    m_pendingParameterName = normalized;
    m_previousCanEnable = field->value;
    m_expectedCanEnable = candidate;
    m_values.insert(normalized, candidate);
    field->value = candidate;
    m_parameterWritePending = true;
    const quint64 generation = ++m_parameterGeneration;
    if (normalized == kCanEnable) {
        m_selectedCanEnable = candidate;
        emit selectedCanEnableChanged();
    }
    setStatus(tr("Waiting for %1 confirmation…").arg(normalized));
    emit fieldChanged(normalized);
    emit stateChanged();
    emit writeRequested(m_componentId, normalized, candidate);

    QTimer::singleShot(WriteTimeoutMs(), this, [this, generation]() {
        if (m_parameterWritePending
            && generation == m_parameterGeneration) {
            finishParameterWrite(false, tr("write timeout"));
        }
    });
    return true;
}

void ConfigHWCANViewModel::SetFactoryResetArmed(bool armed)
{
    const bool accepted = armed && m_connected && m_snapshotReady
        && !m_vehicleArmed && !Busy();
    if (m_factoryResetArmed == accepted) {
        return;
    }
    m_factoryResetArmed = accepted;
    emit stateChanged();
}

bool ConfigHWCANViewModel::StartEnumeration()
{
    return beginCommand(PreflightUavcanCommand(), 1.0f, 0.0f,
                        tr("Start Enumeration"));
}

bool ConfigHWCANViewModel::StopEnumeration()
{
    return beginCommand(PreflightUavcanCommand(), 0.0f, 0.0f,
                        tr("Stop Enumeration"));
}

bool ConfigHWCANViewModel::SaveConfig()
{
    return beginCommand(PreflightStorageCommand(), 1.0f, 0.0f,
                        tr("Save All Config"));
}

bool ConfigHWCANViewModel::FactoryReset()
{
    if (!m_factoryResetArmed) {
        setStatus(tr("Arm Factory Reset first."));
        return false;
    }
    return beginCommand(PreflightStorageCommand(), 2.0f, 0.0f,
                        tr("Reset config"), true);
}

void ConfigHWCANViewModel::commandConfirmationTimedOut()
{
    if (m_pendingCommand != 0) {
        m_commandResultUncertain = true;
        m_factoryResetArmed = false;
        finishCommand(tr("%1 confirmation timed out; result unknown.")
                          .arg(m_pendingCommandLabel));
    }
}

void ConfigHWCANViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (!m_connected || !m_snapshotReady
        || componentId != m_componentId
        || !isRelevantParameter(normalized)) {
        return;
    }
    const bool newlyDiscovered = !m_values.contains(normalized);
    m_values.insert(normalized, value);
    if (newlyDiscovered) {
        rebuildOptions();
        rebuildFields();
    } else if (ParamField *field = fieldForName(normalized)) {
        field->value = value;
        emit fieldChanged(normalized);
    }
    if (normalized == kCanEnable) {
        m_hasCanEnable = true;
        rebuildOptions();
        selectAuthoritativeValue(value);
    }
    if (m_parameterWritePending
        && normalized == m_pendingParameterName) {
        finishParameterWrite(
            numericValuesEqual(value, m_expectedCanEnable), QString(), true);
    } else {
        emit stateChanged();
    }
}

void ConfigHWCANViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId == m_componentId
        && normalizedName(name) == m_pendingParameterName
        && m_parameterWritePending) {
        finishParameterWrite(false, reason);
    }
}

void ConfigHWCANViewModel::commandAckReceived(
    int componentId, int command, int result)
{
    if (!m_connected || !m_snapshotReady
        || componentId != m_componentId || command != m_pendingCommand) {
        return;
    }
    if (result == kInProgress) {
        setStatus(tr("%1 in progress…").arg(m_pendingCommandLabel));
        ++m_commandGeneration;
        scheduleCommandTimeout();
        return;
    }
    if (result == kAccepted) {
        finishCommand(tr("%1 accepted.").arg(m_pendingCommandLabel));
    } else {
        finishCommand(tr("%1 failed (MAV_RESULT %2).")
                          .arg(m_pendingCommandLabel)
                          .arg(result));
    }
}

void ConfigHWCANViewModel::commandSendFailed(
    int componentId, int command, const QString &reason)
{
    if (componentId == m_componentId && command == m_pendingCommand) {
        finishCommand(reason.isEmpty()
                          ? tr("%1 failed.").arg(m_pendingCommandLabel)
                          : tr("%1 failed: %2")
                                .arg(m_pendingCommandLabel, reason));
    }
}

bool ConfigHWCANViewModel::beginCommand(
    int command, float param1, float param2,
    const QString &label, bool factoryReset)
{
    if (!m_connected) {
        setStatus(tr("Not connected — open a link first."));
        return false;
    }
    if (!m_snapshotReady) {
        setStatus(tr("CAN parameter snapshot is not ready."));
        return false;
    }
    if (m_vehicleArmed) {
        setStatus(tr("Disarm the vehicle before changing CAN setup."));
        return false;
    }
    if (Busy()) {
        setStatus(tr("Wait for the current CAN operation to finish."));
        return false;
    }
    if (m_commandResultUncertain) {
        setStatus(tr("The previous command result is unknown. Reconnect "
                     "before sending another CAN command."));
        return false;
    }
    if (factoryReset && !m_factoryResetArmed) {
        setStatus(tr("Arm Factory Reset first."));
        return false;
    }

    m_pendingCommand = command;
    m_pendingCommandLabel = label;
    if (factoryReset) {
        // A destructive retry must always require a new explicit opt-in.
        m_factoryResetArmed = false;
    }
    ++m_commandGeneration;
    setStatus(tr("%1 sent; waiting for acknowledgement…").arg(label));
    emit stateChanged();
    emit commandRequested(m_componentId, command, param1, param2);
    scheduleCommandTimeout();
    return true;
}

void ConfigHWCANViewModel::rebuildOptions()
{
    m_options.clear();
    const ParameterMetaData metadata = m_catalog.value(kCanEnable);
    for (const ParameterMetaDataOption &option : metadata.values) {
        if (!containsOption(m_options, option.value)) {
            m_options.append({option.value,
                              option.label.isEmpty()
                                  ? option.value.toString()
                                  : option.label});
        }
    }
    if (m_options.isEmpty() && m_hasCanEnable) {
        // Mission Planner's retained legacy metadata defines these exact
        // BRD_CAN_ENABLE values. Keep old vehicles usable if their live pdef
        // no longer carries this retired parameter.
        m_options = {
            {0, tr("Disabled")},
            {1, tr("Enabled first channel")},
            {2, tr("Enabled both channels")}
        };
    }
    if (m_hasCanEnable
        && !containsOption(m_options, m_values.value(kCanEnable))) {
        const QVariant current = m_values.value(kCanEnable);
        m_options.append({current, current.toString()});
    }
    emit optionsChanged();
}

void ConfigHWCANViewModel::rebuildFields()
{
    QStringList names;
    for (auto iterator = m_values.constBegin(); iterator != m_values.constEnd();
         ++iterator) {
        if (isRelevantParameter(iterator.key())) {
            names.append(iterator.key());
        }
    }
    std::sort(names.begin(), names.end(), [](const QString &left,
                                             const QString &right) {
        if (left == kCanEnable || right == kCanEnable) {
            return left == kCanEnable && right != kCanEnable;
        }
        QRegularExpressionMatch leftMatch = physicalPattern().match(left);
        QRegularExpressionMatch rightMatch = physicalPattern().match(right);
        const int leftGroup = leftMatch.hasMatch() ? 0 : 1;
        const int rightGroup = rightMatch.hasMatch() ? 0 : 1;
        if (leftGroup != rightGroup) {
            return leftGroup < rightGroup;
        }
        if (!leftMatch.hasMatch()) {
            leftMatch = driverPattern().match(left);
            rightMatch = driverPattern().match(right);
        }
        const int leftIndex = leftMatch.captured(1).toInt();
        const int rightIndex = rightMatch.captured(1).toInt();
        if (leftIndex != rightIndex) {
            return leftIndex < rightIndex;
        }
        return suffixOrder(left) < suffixOrder(right);
    });

    m_fields.clear();
    for (const QString &name : names) {
        m_fields.append(makeField(name));
    }
    emit structureChanged();
    emit stateChanged();
}

ParamField ConfigHWCANViewModel::makeField(const QString &name) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = name;
    field.value = m_values.value(name);

    ParameterMetaData metadata = m_catalog.value(name);
    bool usedTemplateMetadata = false;
    if (metadata.name.isEmpty()) {
        QString templateName = name;
        QRegularExpressionMatch match = physicalPattern().match(name);
        if (match.hasMatch()) {
            templateName.replace(match.capturedStart(1),
                                 match.capturedLength(1),
                                 QStringLiteral("1"));
        } else {
            match = driverPattern().match(name);
            if (match.hasMatch()) {
                templateName.replace(match.capturedStart(1),
                                     match.capturedLength(1),
                                     QStringLiteral("1"));
            }
        }
        metadata = m_catalog.value(templateName);
        usedTemplateMetadata = !metadata.name.isEmpty()
            && templateName != name;
    }
    field.label = metadata.title.isEmpty() || usedTemplateMetadata
        ? name : metadata.title;
    field.description = metadata.description;
    field.units = metadata.units;
    field.readOnly = metadata.readOnly;
    field.status = metadata.rebootRequired
        ? tr("restart required") : QString();
    field.hasRange = metadata.hasRange;
    field.enforceRange = metadata.hasRange;
    field.minimum = metadata.minimum;
    field.maximum = metadata.maximum;
    field.increment = metadata.hasIncrement ? metadata.increment : 1.0;
    if (!metadata.values.isEmpty()) {
        field.editorKind = ParamField::EditorKind::Combo;
        for (const ParameterMetaDataOption &option : metadata.values) {
            field.options.append({option.value,
                                  option.label.isEmpty()
                                      ? option.value.toString()
                                      : option.label});
        }
    } else if (name == kCanEnable) {
        field.editorKind = ParamField::EditorKind::Combo;
        field.options = m_options;
    } else {
        field.editorKind = ParamField::EditorKind::Numeric;
    }
    return field;
}

ParamField *ConfigHWCANViewModel::fieldForName(const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

const ParamField *ConfigHWCANViewModel::fieldForName(
    const QString &name) const
{
    return const_cast<ConfigHWCANViewModel *>(this)->fieldForName(name);
}

bool ConfigHWCANViewModel::isRelevantParameter(const QString &name) const
{
    const QString normalized = normalizedName(name);
    return normalized == kCanEnable
        || physicalPattern().match(normalized).hasMatch()
        || driverPattern().match(normalized).hasMatch();
}

void ConfigHWCANViewModel::selectAuthoritativeValue(
    const QVariant &value)
{
    if (numericValuesEqual(m_selectedCanEnable, value)) {
        return;
    }
    m_selectedCanEnable = value;
    emit selectedCanEnableChanged();
}

void ConfigHWCANViewModel::finishParameterWrite(
    bool matched, const QString &reason, bool receivedEcho)
{
    if (!m_parameterWritePending) {
        return;
    }
    ++m_parameterGeneration;
    const QString name = m_pendingParameterName;
    const QVariant expected = m_expectedCanEnable;
    if (!receivedEcho) {
        m_values.insert(name, m_previousCanEnable);
        if (ParamField *field = fieldForName(name)) {
            field->value = m_previousCanEnable;
        }
        if (name == kCanEnable) {
            selectAuthoritativeValue(m_previousCanEnable);
        }
        emit fieldChanged(name);
    }
    m_parameterWritePending = false;
    m_pendingParameterName.clear();
    if (matched) {
        const ParameterMetaData metadata = m_catalog.value(name);
        const QString restart = metadata.rebootRequired
            || name == kCanEnable
            || name.contains(QLatin1String("_DRIVER"))
            || name.contains(QLatin1String("_PROTOCOL"))
            ? tr(" Restart the vehicle to apply it.") : QString();
        setStatus(tr("%1 = %2.").arg(name, expected.toString()) + restart);
    } else {
        setStatus(reason.isEmpty()
                      ? tr("Set %1 failed: vehicle echoed a different value.")
                            .arg(name)
                      : tr("Set %1 failed: %2").arg(name, reason));
    }
    m_previousCanEnable.clear();
    m_expectedCanEnable.clear();
    emit stateChanged();
}

void ConfigHWCANViewModel::finishCommand(const QString &status)
{
    if (m_pendingCommand == 0) {
        return;
    }
    ++m_commandGeneration;
    m_pendingCommand = 0;
    m_pendingCommandLabel.clear();
    setStatus(status);
    emit stateChanged();
}

void ConfigHWCANViewModel::scheduleCommandTimeout()
{
    const quint64 generation = m_commandGeneration;
    QTimer::singleShot(CommandTimeoutMs(), this, [this, generation]() {
        if (m_pendingCommand != 0 && generation == m_commandGeneration) {
            commandConfirmationTimedOut();
        }
    });
}

void ConfigHWCANViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}

QString ConfigHWCANViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}
