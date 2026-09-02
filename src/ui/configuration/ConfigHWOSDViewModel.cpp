#include "ConfigHWOSDViewModel.h"

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>

ConfigHWOSDViewModel::ConfigHWOSDViewModel(QObject *parent)
    : QObject(parent)
{
}

// QTimer::singleShot uses this object as its context, so pending callbacks are
// cancelled by QObject destruction. The view model owns no transport.
ConfigHWOSDViewModel::~ConfigHWOSDViewModel() = default;

// --- MP10 surface ---------------------------------------------------------------------

QString ConfigHWOSDViewModel::Title()
{
    return QStringLiteral("OSD");
}

QString ConfigHWOSDViewModel::Intro()
{
    return tr("MinimOSD telemetry helper.");
}

QString ConfigHWOSDViewModel::Note()
{
    return tr("You only need to use this if you are having issue with your OSD not updating.");
}

QString ConfigHWOSDViewModel::EnableTelemetryText()
{
    return tr("Enable Telemetry");
}

QStringList ConfigHWOSDViewModel::Streams()
{
    return {QStringLiteral("SR0"), QStringLiteral("SR1"), QStringLiteral("SR3")};
}

QStringList ConfigHWOSDViewModel::Suffixes()
{
    return {QStringLiteral("EXT_STAT"), QStringLiteral("EXTRA1"),   QStringLiteral("EXTRA2"),
            QStringLiteral("EXTRA3"),   QStringLiteral("POSITION"), QStringLiteral("RAW_CTRL"),
            QStringLiteral("RAW_SENS"), QStringLiteral("RC_CHAN")};
}

QStringList ConfigHWOSDViewModel::ParameterNames()
{
    // MP10 nesting: every suffix of SR0, then SR1, then SR3. SRx_PARAMS is never written.
    QStringList names;
    const QStringList streams = Streams();
    const QStringList suffixes = Suffixes();
    for (const QString &stream : streams) {
        for (const QString &suffix : suffixes) {
            names.append(stream + QLatin1Char('_') + suffix);
        }
    }
    return names;
}

int ConfigHWOSDViewModel::TelemetryRateHz()
{
    return 2;
}

QString ConfigHWOSDViewModel::OfflineStatus()
{
    return tr("offline — connect first.");
}

QString ConfigHWOSDViewModel::SettingStatus()
{
    return tr("Setting stream rates…");
}

QString ConfigHWOSDViewModel::SuccessStatus()
{
    return tr("✓ Telemetry streams enabled (2 Hz).");
}

QString ConfigHWOSDViewModel::UnreadyStatus()
{
    return tr("parameters unavailable — refresh the parameter list first.");
}

QString ConfigHWOSDViewModel::MissingStatus(int componentId)
{
    return tr("No SR0/SR1/SR3 stream-rate parameters on component %1 — nothing to enable.")
        .arg(componentId);
}

QString ConfigHWOSDViewModel::PartialSuccessStatus(int written, const QStringList &missing)
{
    return tr("✓ Telemetry streams enabled (2 Hz) for %1 of %2 parameters; not on this "
              "vehicle: %3.")
        .arg(written)
        .arg(ParameterCount())
        .arg(missing.join(QStringLiteral(", ")));
}

QString ConfigHWOSDViewModel::FailureStatus(const QString &detail)
{
    return detail.isEmpty() ? tr("Failed to set OSD rates.")
                            : tr("Failed to set OSD rates. %1").arg(detail);
}

QString ConfigHWOSDViewModel::CancelledStatus()
{
    return tr("Failed to set OSD rates. The parameter writes were cancelled.");
}

QString ConfigHWOSDViewModel::TargetChangedStatus()
{
    return tr("The selected target changed before the stream rates were confirmed.");
}

QString ConfigHWOSDViewModel::TimeoutStatus()
{
    return tr("Failed to set OSD rates. The vehicle did not confirm the writes in time.");
}

// --- state --------------------------------------------------------------------------

bool ConfigHWOSDViewModel::CanEnableTelemetry() const
{
    return m_connected && m_snapshotReady && !m_batch.active && !m_present.isEmpty();
}

QStringList ConfigHWOSDViewModel::MissingParameterNames() const
{
    QStringList missing;
    const QStringList names = ParameterNames();
    for (const QString &name : names) {
        if (!m_present.contains(name)) {
            missing.append(name);
        }
    }
    return missing;
}

QString ConfigHWOSDViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

void ConfigHWOSDViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters, int preferredComponent)
{
    ++m_snapshotRevision;
    abandonBatch(QString()); // a pending batch belongs to the previous snapshot

    const QStringList names = ParameterNames();
    const QSet<QString> wanted(names.constBegin(), names.constEnd());
    QSet<int> relevantComponents;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (wanted.contains(normalizedName(parameter.name))) {
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

    QSet<QString> presentSet;
    for (const ConfigFriendlyParameterValue &parameter : parameters) {
        if (parameter.componentId == m_componentId) {
            const QString name = normalizedName(parameter.name);
            if (wanted.contains(name)) {
                presentSet.insert(name);
            }
        }
    }
    m_present.clear();
    for (const QString &name : names) {
        if (presentSet.contains(name)) {
            m_present.append(name);
        }
    }
    m_snapshotReady = true;
    setStatus(QString());
    emit stateChanged();
}

void ConfigHWOSDViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        ++m_snapshotRevision;
        m_snapshotReady = false;
        m_present.clear();
        abandonBatch(OfflineStatus());
        setStatus(OfflineStatus());
    }
    emit stateChanged();
}

void ConfigHWOSDViewModel::parameterTargetChanged()
{
    ++m_snapshotRevision;
    m_snapshotReady = false;
    m_present.clear();
    abandonBatch(TargetChangedStatus());
    emit stateChanged();
}

QVariantList ConfigHWOSDViewModel::BuildChanges(const QStringList &presentNames)
{
    QVariantList changes;
    const QStringList names = ParameterNames();
    for (const QString &name : names) {
        if (presentNames.contains(name)) {
            changes.append(QVariantMap{{QStringLiteral("name"), name},
                                       {QStringLiteral("value"), TelemetryRateHz()}});
        }
    }
    return changes;
}

bool ConfigHWOSDViewModel::EnableTelemetry()
{
    if (!m_connected) {
        setStatus(OfflineStatus()); // MP10: comPort.BaseStream?.IsOpen != true
        return false;
    }
    if (!m_snapshotReady) {
        setStatus(UnreadyStatus());
        return false;
    }
    if (m_batch.active) {
        return false; // the action is disabled while a batch runs
    }
    if (m_present.isEmpty()) {
        setStatus(MissingStatus(m_componentId));
        return false;
    }

    const QVariantList changes = BuildChanges(m_present);
    Batch batch;
    batch.active = true;
    batch.revision = m_snapshotRevision;
    batch.serial = ++m_batchSerial;
    batch.total = changes.size();
    m_batch = batch;
    setStatus(SettingStatus());
    emit stateChanged();

    const quint64 serial = batch.serial;
    QTimer::singleShot(m_batchTimeoutMs, this, [this, serial]() {
        if (m_batch.active && m_batch.serial == serial) {
            abandonBatch(TimeoutStatus());
            emit stateChanged();
        }
    });

    // A direct receiver may delete the page while handling the request.
    const QPointer<ConfigHWOSDViewModel> guard(this);
    emit writeParamsRequested(m_componentId, changes);
    return !guard.isNull();
}

void ConfigHWOSDViewModel::setBatchTimeoutForTesting(int milliseconds)
{
    m_batchTimeoutMs = qMax(1, milliseconds);
}

// --- batch lifecycle -------------------------------------------------------------------

bool ConfigHWOSDViewModel::ownsBatch(qulonglong batchId) const
{
    return m_batch.active && m_batch.batchId != 0 && m_batch.batchId == batchId &&
           m_batch.revision == m_snapshotRevision;
}

void ConfigHWOSDViewModel::parameterBatchSubmitted(int componentId, qulonglong batchId)
{
    if (!m_batch.active || m_batch.batchId != 0 || batchId == 0 ||
        componentId != m_componentId) {
        return; // not the batch we asked for
    }
    if (m_batch.revision != m_snapshotRevision) {
        return; // requested for a snapshot that is gone; results are ignored
    }
    m_batch.batchId = batchId;
    if (m_batch.earlyCompletion && m_batch.earlyBatchId == batchId) {
        finishBatch(m_batch.earlySucceeded, m_batch.earlyFailed);
    }
}

void ConfigHWOSDViewModel::parameterBatchProgress(qulonglong batchId, int completed, int total,
                                                  int succeeded, int failed)
{
    Q_UNUSED(succeeded)
    Q_UNUSED(failed)
    if (!ownsBatch(batchId)) {
        return;
    }
    setStatus(tr("Setting stream rates… (%1/%2)").arg(completed).arg(total));
}

void ConfigHWOSDViewModel::parameterWriteFailed(qulonglong batchId, int componentId,
                                                const QString &name, const QString &reason)
{
    Q_UNUSED(reason)
    if (!ownsBatch(batchId) || componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (!m_batch.failedNames.contains(normalized)) {
        m_batch.failedNames.append(normalized);
    }
}

void ConfigHWOSDViewModel::parameterBatchCompleted(qulonglong batchId, int succeeded, int failed)
{
    if (m_batch.active && m_batch.batchId == 0 && batchId != 0 &&
        m_batch.revision == m_snapshotRevision) {
        // The owner completed the batch before telling us its id (synchronous
        // transports). Remember it; parameterBatchSubmitted settles it.
        m_batch.earlyCompletion = true;
        m_batch.earlyBatchId = batchId;
        m_batch.earlySucceeded = succeeded;
        m_batch.earlyFailed = failed;
        return;
    }
    if (!ownsBatch(batchId)) {
        return;
    }
    finishBatch(succeeded, failed);
}

void ConfigHWOSDViewModel::parameterBatchCancelled(qulonglong batchId)
{
    if (!ownsBatch(batchId)) {
        return;
    }
    abandonBatch(CancelledStatus());
    emit stateChanged();
}

void ConfigHWOSDViewModel::parameterWriteSubmissionFailed(const QString &reason)
{
    if (!m_batch.active || m_batch.batchId != 0) {
        return; // only an unsubmitted request can fail to submit
    }
    abandonBatch(FailureStatus(reason.isEmpty() ? tr("The writes could not be submitted.")
                                                : reason));
    emit stateChanged();
}

void ConfigHWOSDViewModel::finishBatch(int succeeded, int failed)
{
    const Batch batch = m_batch;
    m_batch = Batch();
    const int total = batch.total;
    if (failed > 0 || succeeded < total) {
        const int failedCount = failed > 0 ? failed : total - succeeded;
        QString detail = tr("%1 of %2 writes failed.").arg(failedCount).arg(total);
        if (!batch.failedNames.isEmpty()) {
            detail = tr("%1 of %2 writes failed: %3.")
                         .arg(failedCount)
                         .arg(total)
                         .arg(batch.failedNames.join(QStringLiteral(", ")));
        }
        setStatus(FailureStatus(detail));
    } else if (total == ParameterCount()) {
        setStatus(SuccessStatus());
    } else {
        setStatus(PartialSuccessStatus(total, MissingParameterNames()));
    }
    emit stateChanged();
}

void ConfigHWOSDViewModel::abandonBatch(const QString &status)
{
    if (!m_batch.active) {
        return;
    }
    m_batch = Batch();
    if (!status.isEmpty()) {
        setStatus(status);
    }
}

void ConfigHWOSDViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
}
