#include "ConfigParachuteViewModel.h"

#include <QMetaType>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
constexpr int kParachuteFunction = 27;

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

QString servoName(int channel)
{
    return QStringLiteral("RC%1").arg(channel);
}

bool isServoFunctionName(const QString &name)
{
    for (int channel = 9; channel <= 14; ++channel) {
        if (name == QStringLiteral("SERVO%1_FUNCTION").arg(channel)
            || name == QStringLiteral("RC%1_FUNCTION").arg(channel)) {
            return true;
        }
    }
    return false;
}

QVariant defaultValue(const QString &name)
{
    if (name == QLatin1String("CHUTE_SERVO_ON")) {
        return 1300;
    }
    if (name == QLatin1String("CHUTE_SERVO_OFF")) {
        return 1100;
    }
    if (name == QLatin1String("CHUTE_ALT_MIN")) {
        return 10;
    }
    if (name == QLatin1String("CHUTE_DELAY_MS")) {
        return 500;
    }
    return 0;
}

QString fallbackLabel(const QString &name)
{
    if (name == QLatin1String("CHUTE_ENABLED")) {
        return QStringLiteral("Parachute release enabled or disabled");
    }
    if (name == QLatin1String("CHUTE_TYPE")) {
        return QStringLiteral(
            "Parachute release mechanism type (relay or servo)");
    }
    if (name == QLatin1String("CHUTE_SERVO_ON")) {
        return QStringLiteral("Parachute Servo ON PWM value");
    }
    if (name == QLatin1String("CHUTE_SERVO_OFF")) {
        return QStringLiteral("Servo OFF PWM value");
    }
    if (name == QLatin1String("CHUTE_ALT_MIN")) {
        return QStringLiteral(
            "Parachute min altitude in meters above home");
    }
    if (name == QLatin1String("CHUTE_DELAY_MS")) {
        return QStringLiteral("Parachute release delay");
    }
    if (name == QLatin1String("CHUTE_CRT_SINK")) {
        return QStringLiteral(
            "Critical sink speed rate in m/s to trigger emergency parachute");
    }
    return name;
}
} // namespace

ConfigParachuteViewModel::ConfigParachuteViewModel(QObject *parent)
    : QObject(parent)
{
    rebuildFields();
}

QStringList ConfigParachuteViewModel::FieldNames()
{
    return {
        QStringLiteral("CHUTE_ENABLED"),
        QStringLiteral("CHUTE_TYPE"),
        QStringLiteral("CHUTE_SERVO_ON"),
        QStringLiteral("CHUTE_SERVO_OFF"),
        QStringLiteral("CHUTE_ALT_MIN"),
        QStringLiteral("CHUTE_DELAY_MS"),
        QStringLiteral("CHUTE_CRT_SINK")
    };
}

int ConfigParachuteViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

QString ConfigParachuteViewModel::Title() const
{
    return tr("Parachute");
}

QString ConfigParachuteViewModel::Intro() const
{
    return tr("Configure parachute release. Ensure props are removed before "
              "testing.");
}

QStringList ConfigParachuteViewModel::ServoOptions() const
{
    return {QStringLiteral("RC9"), QStringLiteral("RC10"),
            QStringLiteral("RC11"), QStringLiteral("RC12"),
            QStringLiteral("RC13"), QStringLiteral("RC14")};
}

bool ConfigParachuteViewModel::Busy() const
{
    return m_refreshing || HasPendingWrites();
}

bool ConfigParachuteViewModel::CanEdit() const
{
    return m_connected && m_snapshotReady && !Busy();
}

void ConfigParachuteViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildFields();
}

void ConfigParachuteViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    cancelPendingForNewOwner(QString());
    ++m_snapshotGeneration;
    m_refreshing = false;
    setStatus(QString());
    setServoStatus(QString());

    const QStringList fieldNames = FieldNames();
    const QSet<QString> fields(fieldNames.constBegin(),
                               fieldNames.constEnd());
    QSet<int> relevantComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        const QString name = normalizedName(parameter.name);
        if (fields.contains(name) || isServoFunctionName(name)) {
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
        if (parameter.componentId == m_componentId) {
            m_values.insert(normalizedName(parameter.name), parameter.value);
        }
    }
    m_snapshotReady = true;
    rebuildFields();
    DetectServo();
    emit stateChanged();
}

void ConfigParachuteViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!m_connected) {
        ++m_snapshotGeneration;
        m_snapshotReady = false;
        m_refreshing = false;
        cancelPendingForNewOwner(tr("offline"));
        setSelectedServo(QString());
        setServoStatus(tr("offline"));
    }
    emit stateChanged();
}

bool ConfigParachuteViewModel::setFieldValue(
    const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    ParamField *field = fieldForName(normalized);
    if (!CanEdit() || !field || field->readOnly) {
        return false;
    }

    QVariant candidate;
    if (field->editorKind == ParamField::EditorKind::Combo) {
        const auto option = std::find_if(
            field->options.constBegin(), field->options.constEnd(),
            [&value](const ParamOption &item) {
                return variantsEqual(item.value, value);
            });
        if (option == field->options.constEnd()) {
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
            return false;
        }
        candidate = typedValue(numeric, field->value);
    }
    if (variantsEqual(field->value, candidate)) {
        return false;
    }

    const QVariant previous = m_values.value(normalized, field->value);
    field->value = candidate;
    queueWrite(FieldWrite, normalized, candidate, previous);
    emit fieldChanged(normalized);
    return true;
}

bool ConfigParachuteViewModel::Refresh()
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

QString ConfigParachuteViewModel::DetectServo()
{
    QString detected;
    for (int channel = 9; channel <= 14; ++channel) {
        const QString parameter = resolvedFunctionParameter(channel);
        if (!parameter.isEmpty()
            && variantsEqual(m_values.value(parameter),
                             kParachuteFunction)) {
            detected = servoName(channel);
            break;
        }
    }
    setSelectedServo(detected);
    return detected;
}

bool ConfigParachuteViewModel::AssignServo(const QString &servo)
{
    const QString normalized = normalizedServo(servo);
    if (!ServoOptions().contains(normalized)) {
        return false;
    }
    if (!m_connected) {
        setServoStatus(tr("offline"));
        return false;
    }
    if (!m_snapshotReady) {
        setServoStatus(tr("parameters unavailable"));
        return false;
    }
    if (Busy()) {
        return false;
    }

    const int channel = normalized.mid(2).toInt();
    const QString targetParameter = resolvedFunctionParameter(channel);
    if (targetParameter.isEmpty()) {
        setServoStatus(tr("parameter unavailable"));
        return false;
    }

    m_assignmentTargetServo = normalized;
    m_assignmentTargetParameter = targetParameter;
    m_disableQueue = EnsureDisabled(normalized);
    m_operationWrites.clear();
    m_assignmentHadDisable = !m_disableQueue.isEmpty();
    setSelectedServo(normalized);
    setServoStatus(QString());

    if (m_disableQueue.isEmpty()
        && variantsEqual(m_values.value(targetParameter),
                         kParachuteFunction)) {
        finishServoAssignment();
        return true;
    }
    startNextServoWrite();
    return true;
}

QStringList ConfigParachuteViewModel::EnsureDisabled(
    const QString &exclude) const
{
    const QString normalizedExclude = normalizedServo(exclude);
    QStringList parameters;
    for (int channel = 9; channel <= 14; ++channel) {
        if (servoName(channel) == normalizedExclude) {
            continue;
        }
        const QString parameter = resolvedFunctionParameter(channel);
        if (!parameter.isEmpty()
            && variantsEqual(m_values.value(parameter),
                             kParachuteFunction)) {
            parameters.append(parameter);
        }
    }
    return parameters;
}

void ConfigParachuteViewModel::refreshFailed(const QString &reason)
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

void ConfigParachuteViewModel::refreshCanceled()
{
    if (!m_refreshing) {
        return;
    }
    m_refreshing = false;
    setStatus(tr("Parameter refresh canceled."));
    emit stateChanged();
}

void ConfigParachuteViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (!m_connected || !m_snapshotReady
        || componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (isStaleEcho(normalized, value)) {
        return;
    }
    const bool isField = FieldNames().contains(normalized);
    const bool isFunctionCandidate = isServoFunctionName(normalized);
    if (!isField && !isFunctionCandidate) {
        return;
    }

    const bool existed = m_values.contains(normalized);
    m_values.insert(normalized, value);
    bool isResolvedFunction = false;
    for (int channel = 9; channel <= 14 && !isResolvedFunction; ++channel) {
        isResolvedFunction = resolvedFunctionParameter(channel) == normalized;
    }
    if (isFunctionCandidate && !isResolvedFunction) {
        return;
    }
    if (isField) {
        if (!existed) {
            rebuildFields();
        } else if (ParamField *field = fieldForName(normalized)) {
            field->value = value;
            emit fieldChanged(normalized);
        }
    }

    if (m_pending.kind == NoWrite || m_pending.name != normalized) {
        if (isResolvedFunction) {
            DetectServo();
        }
        return;
    }
    if (m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }

    const PendingKind kind = m_pending.kind;
    const bool matched = variantsEqual(m_pending.expectedValue, value);
    if (!matched) {
        failPending(tr("write mismatch"), kind == FieldWrite);
        return;
    }

    m_pending = PendingWrite();
    if (kind == FieldWrite) {
        if (ParamField *field = fieldForName(normalized)) {
            field->status = QStringLiteral("✓");
            emit fieldChanged(normalized);
        }
        setStatus(QString());
        emit stateChanged();
        return;
    }
    if (kind == AssignServoWrite) {
        finishServoAssignment();
        return;
    }
    startNextServoWrite();
}

void ConfigParachuteViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId != m_componentId || m_pending.kind == NoWrite
        || normalizedName(name) != m_pending.name
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    failPending(reason.isEmpty() ? tr("write failed") : reason,
                m_pending.kind == FieldWrite);
}

void ConfigParachuteViewModel::rebuildFields()
{
    m_fields.clear();
    const QStringList names = FieldNames();
    for (int index = 0; index < names.size(); ++index) {
        m_fields.append(makeField(names.at(index), index));
    }
    emit structureChanged();
}

ParamField ConfigParachuteViewModel::makeField(
    const QString &name, int index) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = name;
    field.value = m_values.value(name, defaultValue(name));

    const ParameterMetaData metadata = m_catalog.value(name);
    field.label = metadata.title.isEmpty()
        ? fallbackLabel(name) : metadata.title;
    field.units = metadata.units;
    field.description = metadata.description;
    field.readOnly = !m_values.contains(name) || metadata.readOnly;
    field.status = m_values.contains(name) ? QString() : tr("n/a");

    if (index == 0) {
        field.editorKind = ParamField::EditorKind::Combo;
        for (const ParameterMetaDataOption &option : metadata.values) {
            field.options.append({option.value, option.label});
        }
        if (field.options.isEmpty()) {
            field.options = {{0, tr("Disabled")}, {1, tr("Enabled")}};
        }
        return field;
    }
    if (index == 1) {
        field.editorKind = ParamField::EditorKind::Combo;
        field.options = {
            {0, tr("First Relay")}, {1, tr("Second Relay")},
            {2, tr("Third Relay")}, {3, tr("Fourth Relay")},
            {10, tr("Servo")}
        };
        return field;
    }

    field.editorKind = ParamField::EditorKind::Numeric;
    field.hasRange = true;
    field.enforceRange = true;
    field.increment = 1.0;
    if (name == QLatin1String("CHUTE_SERVO_ON")
        || name == QLatin1String("CHUTE_SERVO_OFF")) {
        field.minimum = 1000.0;
        field.maximum = 2000.0;
    } else if (name == QLatin1String("CHUTE_ALT_MIN")) {
        field.minimum = 0.0;
        field.maximum = 32000.0;
    } else if (name == QLatin1String("CHUTE_DELAY_MS")) {
        field.minimum = 0.0;
        field.maximum = 5000.0;
    } else {
        field.minimum = 0.0;
        field.maximum = 15.0;
    }
    return field;
}

ParamField *ConfigParachuteViewModel::fieldForName(const QString &name)
{
    const QString normalized = normalizedName(name);
    for (ParamField &field : m_fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

QString ConfigParachuteViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}

QString ConfigParachuteViewModel::normalizedServo(
    const QString &servo) const
{
    return servo.trimmed().toUpper();
}

QString ConfigParachuteViewModel::resolvedFunctionParameter(
    int channel) const
{
    const QString servo = QStringLiteral("SERVO%1_FUNCTION").arg(channel);
    if (m_values.contains(servo)) {
        return servo;
    }
    const QString legacy = QStringLiteral("RC%1_FUNCTION").arg(channel);
    return m_values.contains(legacy) ? legacy : QString();
}

QVariant ConfigParachuteViewModel::typedValue(
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

void ConfigParachuteViewModel::queueWrite(
    PendingKind kind, const QString &name, const QVariant &value,
    const QVariant &previousValue)
{
    PendingWrite pending;
    pending.kind = kind;
    pending.name = normalizedName(name);
    // A deliberate retry of the same semantic value supersedes one remembered
    // late echo. With MAVLink parameter traffic there is no transaction id;
    // accepting the first matching echo is correct once that value is again
    // the active request.
    consumeStaleEcho(pending.name, value);
    pending.expectedValue = value;
    pending.previousValue = previousValue;
    pending.writeGeneration = ++m_writeGeneration;
    pending.snapshotGeneration = m_snapshotGeneration;
    m_pending = pending;
    if (kind == DisableServoWrite || kind == AssignServoWrite) {
        m_operationWrites[pending.name].append(value);
    }
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
        failPending(tr("write timeout"),
                    m_pending.kind == FieldWrite);
    });
}

void ConfigParachuteViewModel::startNextServoWrite()
{
    if (!m_connected || !m_snapshotReady
        || m_assignmentTargetParameter.isEmpty()) {
        failPending(tr("offline"), false);
        return;
    }
    if (!m_disableQueue.isEmpty()) {
        const QString parameter = m_disableQueue.takeFirst();
        setServoStatus(tr("Disabling previous parachute output…"));
        queueWrite(DisableServoWrite, parameter, 0,
                   m_values.value(parameter));
        return;
    }

    if (!m_assignmentHadDisable
        && variantsEqual(m_values.value(m_assignmentTargetParameter),
                         kParachuteFunction)) {
        finishServoAssignment();
        return;
    }
    setServoStatus(tr("Assigning parachute output…"));
    queueWrite(AssignServoWrite, m_assignmentTargetParameter,
               kParachuteFunction,
               m_values.value(m_assignmentTargetParameter));
}

void ConfigParachuteViewModel::finishServoAssignment()
{
    m_pending = PendingWrite();
    m_disableQueue.clear();
    m_assignmentTargetServo.clear();
    m_assignmentTargetParameter.clear();
    m_assignmentHadDisable = false;
    m_operationWrites.clear();
    DetectServo();
    setServoStatus(QStringLiteral("✓"));
    emit stateChanged();
}

void ConfigParachuteViewModel::failPending(
    const QString &reason, bool revertField)
{
    if (m_pending.kind == NoWrite) {
        return;
    }
    const PendingWrite pending = m_pending;
    const bool servoAssignment = pending.kind == DisableServoWrite
        || pending.kind == AssignServoWrite;
    if (servoAssignment) {
        rememberOperationAsStale();
    } else {
        rememberStaleEcho(pending.name, pending.expectedValue);
    }
    m_pending = PendingWrite();
    m_disableQueue.clear();
    m_assignmentTargetServo.clear();
    m_assignmentTargetParameter.clear();
    m_assignmentHadDisable = false;
    m_operationWrites.clear();

    if (revertField) {
        if (ParamField *field = fieldForName(pending.name)) {
            field->value = m_values.value(pending.name,
                                          pending.previousValue);
            field->status = reason.isEmpty()
                ? tr("write failed") : reason;
            emit fieldChanged(pending.name);
        }
        setStatus(reason.isEmpty()
                      ? tr("write failed") : reason);
    }
    if (servoAssignment) {
        DetectServo();
        setServoStatus(reason.isEmpty() ? tr("write failed") : reason);
    }
    emit stateChanged();
}

void ConfigParachuteViewModel::cancelPendingForNewOwner(
    const QString &servoStatus)
{
    ++m_writeGeneration;
    const bool hadServoAssignment = m_pending.kind == DisableServoWrite
        || m_pending.kind == AssignServoWrite
        || !m_assignmentTargetParameter.isEmpty();
    if (hadServoAssignment) {
        rememberOperationAsStale();
    } else if (m_pending.kind == FieldWrite) {
        rememberStaleEcho(m_pending.name, m_pending.expectedValue);
    }
    m_pending = PendingWrite();
    m_disableQueue.clear();
    m_assignmentTargetServo.clear();
    m_assignmentTargetParameter.clear();
    m_assignmentHadDisable = false;
    m_operationWrites.clear();
    if (hadServoAssignment && !servoStatus.isNull()) {
        setServoStatus(servoStatus);
    }
}

void ConfigParachuteViewModel::rememberStaleEcho(
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

bool ConfigParachuteViewModel::consumeStaleEcho(
    const QString &name, const QVariant &value)
{
    auto iterator = m_staleEchoes.find(normalizedName(name));
    if (iterator == m_staleEchoes.end()) {
        return false;
    }
    const int oldSize = iterator->size();
    iterator->erase(std::remove_if(
        iterator->begin(), iterator->end(),
        [&value](const QVariant &item) {
            return variantsEqual(item, value);
        }), iterator->end());
    const bool removed = iterator->size() != oldSize;
    if (iterator->isEmpty()) {
        m_staleEchoes.erase(iterator);
    }
    return removed;
}

bool ConfigParachuteViewModel::isStaleEcho(
    const QString &name, const QVariant &value) const
{
    const auto iterator = m_staleEchoes.constFind(normalizedName(name));
    if (iterator == m_staleEchoes.constEnd()) {
        return false;
    }
    return std::any_of(
        iterator->constBegin(), iterator->constEnd(),
        [&value](const QVariant &item) {
            return variantsEqual(item, value);
        });
}

void ConfigParachuteViewModel::rememberOperationAsStale()
{
    for (auto iterator = m_operationWrites.constBegin();
         iterator != m_operationWrites.constEnd(); ++iterator) {
        for (const QVariant &value : iterator.value()) {
            rememberStaleEcho(iterator.key(), value);
        }
    }
}

void ConfigParachuteViewModel::setSelectedServo(const QString &servo)
{
    if (m_selectedServo == servo) {
        return;
    }
    m_selectedServo = servo;
    emit selectedServoChanged();
}

void ConfigParachuteViewModel::setServoStatus(const QString &status)
{
    if (m_servoStatus == status) {
        return;
    }
    m_servoStatus = status;
    emit servoStatusChanged();
}

void ConfigParachuteViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}
