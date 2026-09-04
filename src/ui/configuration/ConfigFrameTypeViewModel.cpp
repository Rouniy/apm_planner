#include "ConfigFrameTypeViewModel.h"

#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;
}

ConfigFrameTypeViewModel::ConfigFrameTypeViewModel(QObject *parent)
    : QObject(parent),
      m_status(tr("FRAME parameter not present on this vehicle."))
{
}

QList<ParamOption> ConfigFrameTypeViewModel::FrameOptions()
{
    return {
        {0, QStringLiteral("'+' Plus")},
        {1, QStringLiteral("'X' , 'Y6A'")},
        {2, QStringLiteral("'V'")},
        {3, QStringLiteral("'H'")},
        {10, QStringLiteral("'Y6B'")},
        {4, QStringLiteral("'V-Tail'")}
    };
}

int ConfigFrameTypeViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

void ConfigFrameTypeViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    QSet<int> capableComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (normalizedName(parameter.name) == QLatin1String("FRAME")) {
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
    m_pending = {};
    m_values.clear();
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId
            && normalizedName(parameter.name) == QLatin1String("FRAME")) {
            m_values.insert(QStringLiteral("FRAME"), parameter.value);
        }
    }
    m_snapshotReady = m_values.contains(QStringLiteral("FRAME"));
    if (m_snapshotReady) {
        m_selectedFrame = m_values.value(QStringLiteral("FRAME"));
        m_status.clear();
    } else {
        m_selectedFrame = {};
        m_status = tr("FRAME parameter not present on this vehicle.");
    }
    emit stateChanged();
}

void ConfigFrameTypeViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        if (m_pending.active) {
            rollbackPending();
        }
        m_status = tr("offline");
    } else if (!m_snapshotReady) {
        m_status = tr("FRAME parameter not present on this vehicle.");
    } else if (!m_armed) {
        m_status.clear();
    }
    emit stateChanged();
}

void ConfigFrameTypeViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    if (armed) {
        m_status = tr("Vehicle is armed; frame changes are disabled.");
    } else if (!m_connected) {
        m_status = tr("offline");
    } else if (!m_snapshotReady) {
        m_status = tr("FRAME parameter not present on this vehicle.");
    } else if (!m_pending.active) {
        m_status.clear();
    }
    emit stateChanged();
}

bool ConfigFrameTypeViewModel::selectFrame(const QVariant &frame)
{
    const QList<ParamOption> options = FrameOptions();
    const bool known = std::any_of(
        options.constBegin(), options.constEnd(),
        [&frame](const ParamOption &option) {
            return valuesEqual(option.value, frame);
        });
    if (!known || !m_snapshotReady || m_pending.active) {
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
    if (valuesEqual(m_selectedFrame, frame)) {
        return false;
    }

    const int candidate = frame.toInt();
    m_pending = {};
    m_pending.active = true;
    m_pending.expectedValue = candidate;
    m_pending.previousValue = m_selectedFrame;
    m_pending.requestId = ++m_requestGeneration;
    m_pending.snapshotGeneration = m_snapshotGeneration;
    m_selectedFrame = candidate;
    m_status = tr("Writing frame configuration…");
    emit stateChanged();
    const QVariantList changes{QVariantMap{
        {QStringLiteral("name"), QStringLiteral("FRAME")},
        {QStringLiteral("value"), candidate}
    }};
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

bool ConfigFrameTypeViewModel::Refresh()
{
    if (!m_connected || !m_snapshotReady || m_pending.active) {
        return false;
    }
    m_status = tr("Refreshing parameters…");
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

void ConfigFrameTypeViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId
        || normalizedName(name) != QLatin1String("FRAME")) {
        return;
    }
    // A raw echo is never terminal for an owned write batch.
    if (m_pending.active) {
        return;
    }
    m_values.insert(QStringLiteral("FRAME"), value);
    m_snapshotReady = true;
    m_selectedFrame = value;
    m_status.clear();
    emit stateChanged();
}

void ConfigFrameTypeViewModel::parameterWriteSubmitted(
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

void ConfigFrameTypeViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return;
    }
    finishFailure(reason.isEmpty() ? tr("write failed") : reason);
}

void ConfigFrameTypeViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || normalizedName(name) != QLatin1String("FRAME")) {
        return;
    }
    finishFailure(reason.isEmpty() ? tr("write failed") : reason);
}

void ConfigFrameTypeViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || normalizedName(name) != QLatin1String("FRAME")) {
        return;
    }
    finishFailure(tr("write cancelled"));
}

void ConfigFrameTypeViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId) {
        return;
    }
    if (succeeded == 1 && failed == 0) {
        finishSuccess();
    } else {
        finishFailure(tr("write failed"));
    }
}

void ConfigFrameTypeViewModel::refreshFailed(const QString &reason)
{
    m_status = reason.isEmpty() ? tr("parameter refresh failed") : reason;
    emit stateChanged();
}

void ConfigFrameTypeViewModel::refreshCanceled()
{
    m_status = tr("parameter refresh canceled");
    emit stateChanged();
}

void ConfigFrameTypeViewModel::finishSuccess()
{
    if (!m_pending.active) {
        return;
    }
    const QVariant value = m_pending.expectedValue;
    m_values.insert(QStringLiteral("FRAME"), value);
    m_pending = {};
    m_selectedFrame = value;
    m_status = QStringLiteral("FRAME = %1").arg(value.toString());
    emit stateChanged();
}

void ConfigFrameTypeViewModel::finishFailure(const QString &reason)
{
    if (!m_pending.active) {
        return;
    }
    rollbackPending();
    m_status = reason;
    emit stateChanged();
}

void ConfigFrameTypeViewModel::rollbackPending()
{
    const QVariant previous = m_pending.previousValue;
    m_pending = {};
    m_selectedFrame = previous;
}

QString ConfigFrameTypeViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigFrameTypeViewModel::valuesEqual(
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
