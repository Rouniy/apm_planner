#include "ConfigFrameClassTypeViewModel.h"

#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;

QVariantMap change(const QString &name, const QVariant &value)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("value"), value}};
}
} // namespace

ConfigFrameClassTypeViewModel::ConfigFrameClassTypeViewModel(QObject *parent)
    : QObject(parent)
{
    buildClassOptions();
    m_status = tr("FRAME_CLASS / FRAME_TYPE not present on this vehicle.");
    updateImage();
}

QList<FrameClassTypeEntry> ConfigFrameClassTypeViewModel::ValidList()
{
    // Exact order from Mission Planner 10 ArduPilot.Common.ValidList.
    return {
        {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 0}, {2, 1},
        {3, 0}, {3, 1}, {3, 2}, {3, 3},
        {4, 0}, {4, 1}, {4, 2}, {4, 3},
        {12, 0}, {12, 1},
        {5, 10}, {5, 1},
        {6, {}}, {7, {}}, {8, {}}, {9, {}},
        {10, {}}, {11, {}}, {12, {}}, {13, {}}, {0, {}}
    };
}

int ConfigFrameClassTypeViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

void ConfigFrameClassTypeViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QHash<int, int> schemas;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        const QString name = normalizedName(parameter.name);
        if (name == QLatin1String("FRAME_CLASS")) {
            schemas[parameter.componentId] |= 1;
        } else if (name == QLatin1String("FRAME_TYPE")) {
            schemas[parameter.componentId] |= 2;
        }
    }

    QList<int> capableComponents;
    for (auto iterator = schemas.constBegin(); iterator != schemas.constEnd();
         ++iterator) {
        if (iterator.value() == 3) {
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
    m_reconciliationRequired = false;
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            const QString name = normalizedName(parameter.name);
            if (name == QLatin1String("FRAME_CLASS")
                || name == QLatin1String("FRAME_TYPE")) {
                m_values.insert(name, parameter.value);
            }
        }
    }
    m_snapshotReady = m_values.contains(QStringLiteral("FRAME_CLASS"))
        && m_values.contains(QStringLiteral("FRAME_TYPE"));
    if (m_snapshotReady) {
        m_status.clear();
        hydrateSelection();
    } else {
        m_selectedClass = {};
        m_selectedType = {};
        m_typeOptions.clear();
        m_status = tr("FRAME_CLASS / FRAME_TYPE not present on this vehicle.");
        updateImage();
    }
    emit optionsChanged();
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        if (m_pending.active) {
            if (m_pending.batchId != 0
                && m_pending.changes.size() > 1) {
                finishUncertain(
                    tr("Frame state is uncertain after connection loss; "
                       "reconnect and refresh parameters."), false);
            } else {
                rollbackPending();
            }
            emit optionsChanged();
        }
        if (!m_reconciliationRequired) {
            m_status = tr("offline");
        }
    } else if (m_reconciliationRequired) {
        m_status = tr("Frame state is uncertain; refresh parameters before "
                      "making another change.");
    } else if (!m_snapshotReady) {
        m_status = tr("FRAME_CLASS / FRAME_TYPE not present on this vehicle.");
    } else if (!m_armed) {
        m_status.clear();
    }
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    if (armed) {
        m_status = tr("Vehicle is armed; frame changes are disabled.");
    } else if (!m_connected) {
        m_status = tr("offline");
    } else if (m_reconciliationRequired) {
        m_status = tr("Frame state is uncertain; refresh parameters before "
                      "making another change.");
    } else if (!m_snapshotReady) {
        m_status = tr("FRAME_CLASS / FRAME_TYPE not present on this vehicle.");
    } else if (!m_pending.active) {
        m_status.clear();
    }
    emit stateChanged();
}

bool ConfigFrameClassTypeViewModel::selectClass(
    const QVariant &frameClass)
{
    bool ok = false;
    const int candidateClass = frameClass.toInt(&ok);
    const bool knownClass = ok && std::any_of(
        m_classOptions.constBegin(), m_classOptions.constEnd(),
        [candidateClass](const ParamOption &option) {
            return option.value.toInt() == candidateClass;
        });
    if (!knownClass || !m_snapshotReady || m_pending.active) {
        return false;
    }
    if (!m_connected) {
        m_status = tr("offline");
        emit stateChanged();
        return false;
    }
    if (m_armed) {
        m_status = tr("Vehicle is armed; frame changes are disabled.");
        emit stateChanged();
        return false;
    }

    const QList<ParamOption> candidateTypes = typesForClass(candidateClass);
    QVariant candidateType;
    const bool retainsType = std::any_of(
        candidateTypes.constBegin(), candidateTypes.constEnd(),
        [this](const ParamOption &option) {
            return m_selectedType.isValid()
                && valuesEqual(option.value, m_selectedType);
        });
    if (retainsType) {
        candidateType = m_selectedType;
    } else if (!candidateTypes.isEmpty()) {
        candidateType = candidateTypes.constFirst().value;
    }

    if (valuesEqual(m_selectedClass, candidateClass)
        && ((!candidateType.isValid() && !m_selectedType.isValid())
            || valuesEqual(m_selectedType, candidateType))) {
        return false;
    }

    QVariantList changes;
    changes.append(change(QStringLiteral("FRAME_CLASS"), candidateClass));
    if (!candidateTypes.isEmpty() && !retainsType) {
        changes.append(change(QStringLiteral("FRAME_TYPE"), candidateType));
    }
    return beginWrite(changes, candidateClass, candidateType);
}

bool ConfigFrameClassTypeViewModel::selectType(const QVariant &frameType)
{
    const bool knownType = std::any_of(
        m_typeOptions.constBegin(), m_typeOptions.constEnd(),
        [&frameType](const ParamOption &option) {
            return valuesEqual(option.value, frameType);
        });
    if (!knownType || !m_snapshotReady || m_pending.active
        || !m_selectedClass.isValid()) {
        return false;
    }
    if (!m_connected) {
        m_status = tr("offline");
        emit stateChanged();
        return false;
    }
    if (m_armed) {
        m_status = tr("Vehicle is armed; frame changes are disabled.");
        emit stateChanged();
        return false;
    }
    if (valuesEqual(m_selectedType, frameType)) {
        return false;
    }
    const int candidateType = frameType.toInt();
    const QVariantList changes{
        change(QStringLiteral("FRAME_TYPE"), candidateType)};
    return beginWrite(changes, m_selectedClass, candidateType);
}

bool ConfigFrameClassTypeViewModel::Refresh()
{
    if (!m_connected || m_pending.active) {
        return false;
    }
    m_status = tr("Refreshing parameters…");
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigFrameClassTypeViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    const QString normalized = normalizedName(name);
    if (componentId != m_componentId
        || (normalized != QLatin1String("FRAME_CLASS")
            && normalized != QLatin1String("FRAME_TYPE"))) {
        return;
    }

    // PARAM_VALUE is an observation, not a terminal result for our batch.
    // While a batch is pending its optimistic values remain stable until the
    // exact batch reports completion/failure/cancellation.
    if (m_pending.active || m_reconciliationRequired) {
        return;
    }
    m_values.insert(normalized, value);
    m_snapshotReady = m_values.contains(QStringLiteral("FRAME_CLASS"))
        && m_values.contains(QStringLiteral("FRAME_TYPE"));
    if (m_snapshotReady) {
        hydrateSelection();
        m_status.clear();
    }
    emit optionsChanged();
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    if (batchId == 0) {
        finishFailure(tr("write was rejected"));
        return;
    }
    m_pending.batchId = batchId;
}

void ConfigFrameClassTypeViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    finishFailure(reason.isEmpty() ? tr("write failed") : reason);
}

void ConfigFrameClassTypeViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    const QString failure = reason.isEmpty() ? tr("write failed") : reason;
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = failure;
        m_status = tr("A frame parameter write failed; waiting for the "
                      "complete batch result…");
        emit stateChanged();
        return;
    }
    finishFailure(failure);
}

void ConfigFrameClassTypeViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = tr("write cancelled");
        m_status = tr("A frame parameter write was cancelled; waiting for "
                      "the complete batch result…");
        emit stateChanged();
        return;
    }
    finishFailure(tr("write cancelled"));
}

void ConfigFrameClassTypeViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId) {
        return;
    }
    if (failed == 0 && succeeded == m_pending.changes.size()) {
        finishSuccess();
    } else if (m_pending.changes.size() > 1) {
        finishUncertain(
            tr("Frame parameters may have been only partially applied; "
               "refreshing the vehicle state is required."), true);
    } else {
        finishFailure(m_pending.failureReason.isEmpty()
                          ? tr("write failed")
                          : m_pending.failureReason);
    }
}

void ConfigFrameClassTypeViewModel::refreshFailed(const QString &reason)
{
    const QString failure = reason.isEmpty()
        ? tr("parameter refresh failed") : reason;
    m_status = m_reconciliationRequired
        ? tr("Frame state remains uncertain; refresh required: %1")
              .arg(failure)
        : failure;
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::refreshCanceled()
{
    m_status = m_reconciliationRequired
        ? tr("Frame state remains uncertain; parameter refresh was canceled.")
        : tr("parameter refresh canceled");
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::buildClassOptions()
{
    m_classOptions.clear();
    QSet<int> seen;
    for (const FrameClassTypeEntry &entry : ValidList()) {
        if (!seen.contains(entry.frameClass)) {
            seen.insert(entry.frameClass);
            m_classOptions.append(
                {entry.frameClass, className(entry.frameClass)});
        }
    }
}

void ConfigFrameClassTypeViewModel::rebuildTypes(
    int frameClass, const QVariant &preferredType)
{
    m_typeOptions = typesForClass(frameClass);
    const auto preferred = std::find_if(
        m_typeOptions.constBegin(), m_typeOptions.constEnd(),
        [&preferredType](const ParamOption &option) {
            return preferredType.isValid()
                && valuesEqual(option.value, preferredType);
        });
    if (preferred != m_typeOptions.constEnd()) {
        m_selectedType = preferred->value;
    } else if (!m_typeOptions.isEmpty()) {
        m_selectedType = m_typeOptions.constFirst().value;
    } else {
        m_selectedType = {};
    }
    updateImage();
}

void ConfigFrameClassTypeViewModel::hydrateSelection()
{
    m_selectedClass = m_values.value(QStringLiteral("FRAME_CLASS"));
    m_typeOptions = typesForClass(m_selectedClass.toInt());
    if (m_typeOptions.isEmpty()) {
        m_selectedType = {};
    } else {
        const QVariant actualType =
            m_values.value(QStringLiteral("FRAME_TYPE"));
        const auto actual = std::find_if(
            m_typeOptions.constBegin(), m_typeOptions.constEnd(),
            [&actualType](const ParamOption &option) {
                return valuesEqual(option.value, actualType);
            });
        // Preserve an unsupported actual value so the combo has no selected
        // row and choosing the first valid type still emits a correcting write.
        m_selectedType = actual != m_typeOptions.constEnd()
            ? actual->value : actualType;
    }
    updateImage();
}

bool ConfigFrameClassTypeViewModel::beginWrite(
    const QVariantList &changes, const QVariant &newClass,
    const QVariant &newType)
{
    if (changes.isEmpty()) {
        return false;
    }
    m_pending = {};
    m_pending.active = true;
    m_pending.changes = changes;
    m_pending.previousClass = m_selectedClass;
    m_pending.previousType = m_selectedType;
    m_pending.requestId = ++m_requestGeneration;
    m_pending.snapshotGeneration = m_snapshotGeneration;
    m_selectedClass = newClass;
    rebuildTypes(newClass.toInt(), newType);
    m_status = tr("Writing frame configuration…");
    emit optionsChanged();
    emit stateChanged();
    emit writeRequested(m_pending.requestId, m_componentId, changes);

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
        finishFailure(tr("write timeout"));
    });
    return true;
}

void ConfigFrameClassTypeViewModel::finishSuccess()
{
    if (!m_pending.active) {
        return;
    }
    QStringList summaries;
    for (const QVariant &item : m_pending.changes) {
        const QVariantMap entry = item.toMap();
        const QString name = normalizedName(
            entry.value(QStringLiteral("name")).toString());
        const QVariant value = entry.value(QStringLiteral("value"));
        m_values.insert(name, value);
        summaries.append(QStringLiteral("%1 = %2")
                             .arg(name, value.toString()));
    }
    m_pending = {};
    hydrateSelection();
    m_status = summaries.join(QStringLiteral(", "));
    emit optionsChanged();
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::finishFailure(const QString &reason)
{
    if (!m_pending.active) {
        return;
    }
    rollbackPending();
    m_status = reason;
    emit optionsChanged();
    emit stateChanged();
}

void ConfigFrameClassTypeViewModel::finishUncertain(
    const QString &reason, bool requestRefresh)
{
    if (!m_pending.active) {
        return;
    }
    m_pending = {};
    m_values.clear();
    m_snapshotReady = false;
    m_reconciliationRequired = true;
    m_selectedClass = {};
    m_selectedType = {};
    m_typeOptions.clear();
    m_status = reason;
    updateImage();
    emit optionsChanged();
    emit stateChanged();
    if (requestRefresh && m_connected) {
        emit refreshRequested(m_componentId);
    }
}

void ConfigFrameClassTypeViewModel::rollbackPending()
{
    const QVariant previousClass = m_pending.previousClass;
    const QVariant previousType = m_pending.previousType;
    m_pending = {};
    m_selectedClass = previousClass;
    if (previousClass.isValid()) {
        m_typeOptions = typesForClass(previousClass.toInt());
        if (m_typeOptions.isEmpty()) {
            m_selectedType = {};
        } else {
            // Unlike a new class selection, rollback must preserve an
            // unsupported actual value so a corrective choice stays possible.
            m_selectedType = previousType;
        }
        updateImage();
    } else {
        m_typeOptions.clear();
        m_selectedType = {};
        updateImage();
    }
}

void ConfigFrameClassTypeViewModel::updateImage()
{
    m_frameImageKey.clear();
    m_frameImageResource.clear();
    m_frameImageCaption.clear();
    if (!m_selectedClass.isValid()) {
        m_frameImageCaption = tr("Select a frame class.");
        return;
    }

    if (m_selectedType.isValid()) {
        m_frameImageKey = typeImageKey(m_selectedType.toInt());
    }
    if (m_frameImageKey.isEmpty()) {
        m_frameImageKey = classImageKey(m_selectedClass.toInt());
    }
    m_frameImageResource = resourceForImageKey(m_frameImageKey);
    if (!m_frameImageResource.isEmpty()) {
        return;
    }
    m_frameImageKey.clear();
    m_frameImageCaption = m_selectedType.isValid()
        ? typeName(m_selectedType.toInt())
        : tr("(no sub-types for this class)");
}

bool ConfigFrameClassTypeViewModel::pendingContains(
    const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (const QVariant &item : m_pending.changes) {
        if (normalizedName(item.toMap()
                               .value(QStringLiteral("name"))
                               .toString()) == normalized) {
            return true;
        }
    }
    return false;
}

QList<ParamOption> ConfigFrameClassTypeViewModel::typesForClass(
    int frameClass)
{
    QList<ParamOption> result;
    QSet<int> seen;
    for (const FrameClassTypeEntry &entry : ValidList()) {
        if (entry.frameClass != frameClass || !entry.frameType.isValid()) {
            continue;
        }
        const int frameType = entry.frameType.toInt();
        if (!seen.contains(frameType)) {
            seen.insert(frameType);
            result.append({frameType, typeName(frameType)});
        }
    }
    return result;
}

QString ConfigFrameClassTypeViewModel::className(int frameClass)
{
    switch (frameClass) {
    case 0: return QStringLiteral("UNDEFINED");
    case 1: return QStringLiteral("QUAD");
    case 2: return QStringLiteral("HEXA");
    case 3: return QStringLiteral("OCTA");
    case 4: return QStringLiteral("OCTAQUAD");
    case 5: return QStringLiteral("Y6");
    case 6: return QStringLiteral("HELI");
    case 7: return QStringLiteral("TRI");
    case 8: return QStringLiteral("SINGLE");
    case 9: return QStringLiteral("COAX");
    case 10: return QStringLiteral("TAILSITTER");
    case 11: return QStringLiteral("HELI_DUAL");
    case 12: return QStringLiteral("DODECAHEXA");
    case 13: return QStringLiteral("HELI_QUAD");
    default: return QString::number(frameClass);
    }
}

QString ConfigFrameClassTypeViewModel::typeName(int frameType)
{
    switch (frameType) {
    case 0: return QStringLiteral("PLUS");
    case 1: return QStringLiteral("X");
    case 2: return QStringLiteral("V");
    case 3: return QStringLiteral("H");
    case 4: return QStringLiteral("VTAIL");
    case 5: return QStringLiteral("ATAIL");
    case 10: return QStringLiteral("Y6B");
    default: return QString::number(frameType);
    }
}

QString ConfigFrameClassTypeViewModel::typeImageKey(int frameType)
{
    switch (frameType) {
    case 0: return QStringLiteral("type_plus");
    case 1: return QStringLiteral("type_x");
    case 2: return QStringLiteral("type_v");
    case 3: return QStringLiteral("type_h");
    case 10: return QStringLiteral("type_y6b");
    default: return {};
    }
}

QString ConfigFrameClassTypeViewModel::classImageKey(int frameClass)
{
    switch (frameClass) {
    case 1: return QStringLiteral("class_quad");
    case 2: return QStringLiteral("class_hexa");
    case 3: return QStringLiteral("class_octa");
    case 4: return QStringLiteral("class_octaquad");
    case 5: return QStringLiteral("class_y6");
    case 6: return QStringLiteral("class_heli");
    case 7: return QStringLiteral("class_tri");
    default: return {};
    }
}

QString ConfigFrameClassTypeViewModel::resourceForImageKey(
    const QString &key)
{
    if (key == QLatin1String("type_plus")) {
        return QStringLiteral(":/files/images/mavs/frames_plus.png");
    }
    if (key == QLatin1String("type_x")) {
        return QStringLiteral(":/files/images/mavs/frames_x.png");
    }
    if (key == QLatin1String("type_v")) {
        return QStringLiteral(":/files/images/mavs/frames-05.png");
    }
    if (key == QLatin1String("type_h")) {
        return QStringLiteral(":/files/images/mavs/frames-h.png");
    }
    if (key == QLatin1String("type_y6b")
        || key == QLatin1String("class_y6")) {
        return QStringLiteral(":/files/images/mavs/frames-Y6B.png");
    }
    if (key == QLatin1String("class_quad")) {
        return QStringLiteral(":/files/images/mavs/quadrotor.svg");
    }
    if (key == QLatin1String("class_hexa")) {
        return QStringLiteral(":/files/images/mavs/hexarotor.svg");
    }
    if (key == QLatin1String("class_octa")) {
        return QStringLiteral(":/files/images/mavs/octorotor.svg");
    }
    if (key == QLatin1String("class_octaquad")) {
        return QStringLiteral(":/files/images/mavs/frames_quad_I.png");
    }
    if (key == QLatin1String("class_heli")) {
        return QStringLiteral(":/files/images/mavs/helicopter.svg");
    }
    if (key == QLatin1String("class_tri")) {
        return QStringLiteral(":/files/images/mavs/tricopter.svg");
    }
    return {};
}

QString ConfigFrameClassTypeViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigFrameClassTypeViewModel::valuesEqual(
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
