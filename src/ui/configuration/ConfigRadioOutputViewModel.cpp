#include "ConfigRadioOutputViewModel.h"

#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kDefaultServoCount = 16;
constexpr int kExtendedServoCount = 32;
constexpr int kDefaultPwm = 1500;
constexpr int kMinimumPwm = 800;
constexpr int kMaximumPwm = 2200;
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

QString servoParameterName(int servoNumber, const QString &suffix)
{
    return QStringLiteral("SERVO%1_%2").arg(servoNumber).arg(suffix);
}

int servoNumberFromName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^SERVO([1-9]|[12][0-9]|3[0-2])_"
        "(REVERSED|FUNCTION|MIN|TRIM|MAX)$"));
    const QRegularExpressionMatch match = pattern.match(name);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}
}

ConfigRadioOutputViewModel::ConfigRadioOutputViewModel(QObject *parent)
    : QObject(parent)
{
    rebuildRows();
}

void ConfigRadioOutputViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_catalog = catalog;
    rebuildRows();
}

void ConfigRadioOutputViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> servoComponents;
    static const QRegularExpression servoPattern(
        QStringLiteral("^SERVO(?:_32_ENABLE|[1-9][0-9]*_)"));
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (servoPattern.match(normalizedName(parameter.name)).hasMatch()) {
            servoComponents.insert(parameter.componentId);
        }
    }

    if (servoComponents.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (servoComponents.contains(1)) {
        m_componentId = 1;
    } else if (!servoComponents.isEmpty()) {
        QList<int> ordered = servoComponents.values();
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
    m_supersededWrites.clear();
    m_writeErrors.clear();
    rebuildRows();
}

QList<ServoOutputRow> ConfigRadioOutputViewModel::Rows() const
{
    return m_rows;
}

bool ConfigRadioOutputViewModel::setReversed(
    int oneBasedChannel, bool reversed)
{
    return setFieldValue(
        oneBasedChannel, QStringLiteral("REVERSED"), reversed ? 1 : 0);
}

bool ConfigRadioOutputViewModel::setFunction(
    int oneBasedChannel, const QVariant &function)
{
    ServoOutputRow *row = rowForNumber(oneBasedChannel);
    if (!row || row->Function.readOnly) {
        return false;
    }
    const bool knownOption = std::any_of(
        row->Function.options.constBegin(), row->Function.options.constEnd(),
        [&function](const ParamOption &option) {
            return variantsEqual(option.value, function);
        });
    if (!knownOption) {
        return false;
    }
    return setFieldValue(
        oneBasedChannel, QStringLiteral("FUNCTION"), function);
}

bool ConfigRadioOutputViewModel::setMin(int oneBasedChannel, int pwm)
{
    if (pwm < kMinimumPwm || pwm > kMaximumPwm) {
        return false;
    }
    return setFieldValue(oneBasedChannel, QStringLiteral("MIN"), pwm);
}

bool ConfigRadioOutputViewModel::setTrim(int oneBasedChannel, int pwm)
{
    if (pwm < kMinimumPwm || pwm > kMaximumPwm) {
        return false;
    }
    return setFieldValue(oneBasedChannel, QStringLiteral("TRIM"), pwm);
}

bool ConfigRadioOutputViewModel::setMax(int oneBasedChannel, int pwm)
{
    if (pwm < kMinimumPwm || pwm > kMaximumPwm) {
        return false;
    }
    return setFieldValue(oneBasedChannel, QStringLiteral("MAX"), pwm);
}

void ConfigRadioOutputViewModel::setServoOutput(
    int oneBasedChannel, int pwm)
{
    if (!rowForNumber(oneBasedChannel)) {
        return;
    }
    m_latestServoOutputs.insert(oneBasedChannel, pwm);
    m_pendingServoOutputs.insert(oneBasedChannel, pwm);
}

void ConfigRadioOutputViewModel::flushServoOutputs()
{
    const QHash<int, int> pending = m_pendingServoOutputs;
    m_pendingServoOutputs.clear();
    for (auto iterator = pending.constBegin();
         iterator != pending.constEnd(); ++iterator) {
        ServoOutputRow *row = rowForNumber(iterator.key());
        if (!row || row->Pwm == iterator.value()) {
            continue;
        }
        row->Pwm = iterator.value();
        emit rowChanged(row->Number);
    }
}

void ConfigRadioOutputViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    const bool hadParameter = m_values.contains(normalized);

    if (normalized == QLatin1String("SERVO_32_ENABLE")) {
        const bool hadExtendedRows = m_rows.size() == kExtendedServoCount;
        m_values.insert(normalized, value);
        const bool needsExtendedRows = value.toDouble() > 0.0;
        if (hadExtendedRows != needsExtendedRows || !hadParameter) {
            rebuildRows();
        }
        return;
    }

    const int servoNumber = servoNumberFromName(normalized);
    if (servoNumber == 0) {
        return;
    }

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
    } else {
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
    if (!hadParameter) {
        rebuildRows();
        return;
    }

    ServoOutputRow *row = rowForNumber(servoNumber);
    if (!row) {
        return;
    }
    applyValueToRow(row, normalized, value);
    if (pendingIterator != m_pendingWrites.end()) {
        if (variantsEqual(pendingIterator->expectedValue, value)) {
            m_pendingWrites.erase(pendingIterator);
            m_supersededWrites.remove(normalized);
            m_writeErrors.remove(normalized);
        } else {
            m_pendingWrites.erase(pendingIterator);
            m_writeErrors.insert(normalized, tr("write mismatch"));
        }
    } else {
        m_writeErrors.remove(normalized);
    }
    updateRowStatus(row);
    emit rowChanged(servoNumber);
}

void ConfigRadioOutputViewModel::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (componentId != m_componentId) {
        return;
    }
    finishPending(normalizedName(name),
                  reason.isEmpty() ? tr("write failed") : reason,
                  true);
}

void ConfigRadioOutputViewModel::rebuildRows()
{
    const int servoCount =
        authoritativeValue(QStringLiteral("SERVO_32_ENABLE")).toDouble()
                > 0.0
            ? kExtendedServoCount
            : kDefaultServoCount;

    QList<ServoOutputRow> rows;
    rows.reserve(servoCount);
    for (int servoNumber = 1; servoNumber <= servoCount; ++servoNumber) {
        ServoOutputRow row;
        row.Number = servoNumber;
        row.Pwm = m_latestServoOutputs.value(servoNumber, kDefaultPwm);
        row.Reversed = makeField(
            servoNumber, QStringLiteral("REVERSED"),
            ParamField::EditorKind::Numeric, 0);
        row.Function = makeField(
            servoNumber, QStringLiteral("FUNCTION"),
            ParamField::EditorKind::Combo, 0);
        row.Min = makeField(
            servoNumber, QStringLiteral("MIN"),
            ParamField::EditorKind::Numeric, kDefaultPwm);
        row.Trim = makeField(
            servoNumber, QStringLiteral("TRIM"),
            ParamField::EditorKind::Numeric, kDefaultPwm);
        row.Max = makeField(
            servoNumber, QStringLiteral("MAX"),
            ParamField::EditorKind::Numeric, kDefaultPwm);
        updateRowStatus(&row);
        rows.append(row);
    }
    m_rows = rows;

    for (auto iterator = m_pendingServoOutputs.begin();
         iterator != m_pendingServoOutputs.end();) {
        if (iterator.key() > servoCount) {
            iterator = m_pendingServoOutputs.erase(iterator);
        } else {
            ++iterator;
        }
    }
    emit structureChanged();
}

ParamField ConfigRadioOutputViewModel::makeField(
    int servoNumber, const QString &suffix, ParamField::EditorKind kind,
    const QVariant &defaultValue) const
{
    ParamField field;
    field.componentId = m_componentId;
    field.name = servoParameterName(servoNumber, suffix);
    field.editorKind = kind;
    field.value = hasParameter(field.name)
        ? authoritativeValue(field.name) : defaultValue;

    const ParameterMetaData metadata = metadataFor(
        field.name, servoParameterName(1, suffix));
    field.label = metadata.title.isEmpty() ? field.name : metadata.title;
    field.units = metadata.units;
    field.description = metadata.description;
    field.readOnly = !hasParameter(field.name) || metadata.readOnly;

    if (suffix == QLatin1String("FUNCTION")) {
        for (const ParameterMetaDataOption &option : metadata.values) {
            field.options.append({option.value, option.label});
        }
    } else if (suffix == QLatin1String("MIN")
               || suffix == QLatin1String("TRIM")
               || suffix == QLatin1String("MAX")) {
        field.minimum = kMinimumPwm;
        field.maximum = kMaximumPwm;
        field.increment = 1.0;
        field.hasRange = true;
        field.enforceRange = true;
    }
    return field;
}

ParameterMetaData ConfigRadioOutputViewModel::metadataFor(
    const QString &name, const QString &templateName) const
{
    ParameterMetaData metadata = m_catalog.value(name);
    if (metadata.name.isEmpty()) {
        metadata = m_catalog.value(templateName);
    }
    return metadata;
}

bool ConfigRadioOutputViewModel::setFieldValue(
    int servoNumber, const QString &suffix, const QVariant &value)
{
    ServoOutputRow *row = rowForNumber(servoNumber);
    if (!row) {
        return false;
    }
    const QString name = servoParameterName(servoNumber, suffix);
    ParamField *field = fieldForName(row, name);
    if (!field || field->readOnly || variantsEqual(field->value, value)) {
        return false;
    }
    field->value = value;
    queueWrite(servoNumber, name, value);
    emit rowChanged(servoNumber);
    return true;
}

void ConfigRadioOutputViewModel::queueWrite(
    int servoNumber, const QString &name, const QVariant &value)
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
    pending.servoNumber = servoNumber;
    pending.expectedValue = value;
    pending.generation = ++m_writeGeneration;
    m_pendingWrites.insert(normalized, pending);
    m_writeErrors.remove(normalized);
    if (ServoOutputRow *row = rowForNumber(servoNumber)) {
        updateRowStatus(row);
    }
    emit writeRequested(m_componentId, normalized, value);

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

void ConfigRadioOutputViewModel::finishPending(
    const QString &name, const QString &status,
    bool revertToAuthoritativeValue)
{
    auto iterator = m_pendingWrites.find(name);
    if (iterator == m_pendingWrites.end()) {
        return;
    }
    const int servoNumber = iterator->servoNumber;
    m_pendingWrites.erase(iterator);
    m_supersededWrites.remove(name);
    m_writeErrors.insert(name, status);

    ServoOutputRow *row = rowForNumber(servoNumber);
    if (!row) {
        return;
    }
    if (revertToAuthoritativeValue) {
        applyValueToRow(row, name, authoritativeValue(name));
    }
    updateRowStatus(row);
    emit rowChanged(servoNumber);
}

void ConfigRadioOutputViewModel::applyValueToRow(
    ServoOutputRow *row, const QString &name, const QVariant &value)
{
    if (ParamField *field = fieldForName(row, name)) {
        field->value = value;
    }
}

void ConfigRadioOutputViewModel::updateRowStatus(ServoOutputRow *row)
{
    const QString prefix = QStringLiteral("SERVO%1_").arg(row->Number);
    bool pending = false;
    for (auto iterator = m_pendingWrites.constBegin();
         iterator != m_pendingWrites.constEnd(); ++iterator) {
        if (iterator.key().startsWith(prefix)) {
            pending = true;
            break;
        }
    }

    QString error;
    for (auto iterator = m_writeErrors.constBegin();
         iterator != m_writeErrors.constEnd(); ++iterator) {
        if (iterator.key().startsWith(prefix)) {
            error = iterator.value();
            break;
        }
    }
    row->Status = !error.isEmpty()
        ? error : (pending ? QStringLiteral("…") : QString());
}

ServoOutputRow *ConfigRadioOutputViewModel::rowForNumber(
    int oneBasedChannel)
{
    if (oneBasedChannel < 1 || oneBasedChannel > m_rows.size()) {
        return nullptr;
    }
    return &m_rows[oneBasedChannel - 1];
}

const ServoOutputRow *ConfigRadioOutputViewModel::rowForNumber(
    int oneBasedChannel) const
{
    if (oneBasedChannel < 1 || oneBasedChannel > m_rows.size()) {
        return nullptr;
    }
    return &m_rows.at(oneBasedChannel - 1);
}

ParamField *ConfigRadioOutputViewModel::fieldForName(
    ServoOutputRow *row, const QString &name)
{
    if (!row) {
        return nullptr;
    }
    const QString normalized = normalizedName(name);
    ParamField *fields[] = {
        &row->Reversed, &row->Function, &row->Min, &row->Trim, &row->Max
    };
    for (ParamField *field : fields) {
        if (field->name == normalized) {
            return field;
        }
    }
    return nullptr;
}

QString ConfigRadioOutputViewModel::normalizedName(const QString &name) const
{
    return name.trimmed().toUpper();
}

QVariant ConfigRadioOutputViewModel::authoritativeValue(
    const QString &name) const
{
    return m_values.value(normalizedName(name));
}

bool ConfigRadioOutputViewModel::hasParameter(const QString &name) const
{
    return m_values.contains(normalizedName(name));
}
