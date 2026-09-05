#include "ConfigFFTViewModel.h"

#include "ui/Loghandling/DataFlashFftService.h"

#include <QFileInfo>
#include <QHash>
#include <QMetaType>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace {
constexpr int kWriteTimeoutMs = 5000;

QString cleanFailure(const QString &reason, const QString &fallback)
{
    const QString clean = reason.trimmed();
    return clean.isEmpty() ? fallback : clean;
}
} // namespace

ConfigFFTViewModel::ConfigFFTViewModel(Analyzer analyzer, QObject *parent)
    : QObject(parent)
    , m_analyzer(std::move(analyzer))
{
    if (!m_analyzer) {
        m_analyzer = [](const QString &path,
                        const DataFlashFftAnalyzer::Options &options) {
            return DataFlashFftService::Analyze(path, options);
        };
    }
    rebuildFields();
    updateParameterStatus();
    m_analysisStatus = tr(
        "FFT runs on batch-logged IMU data. Pick a .bin to graph its "
        "spectrum.");
}

ConfigFFTViewModel::~ConfigFFTViewModel()
{
    shutdown();
}

QStringList ConfigFFTViewModel::ReferenceParameterNames()
{
    return {QStringLiteral("INS_LOG_BAT_CNT"),
            QStringLiteral("INS_LOG_BAT_MASK"),
            QStringLiteral("LOG_BITMASK")};
}

int ConfigFFTViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

QString ConfigFFTViewModel::title() const
{
    return tr("FFT Setup");
}

QString ConfigFFTViewModel::info() const
{
    return tr("FFT runs on batch-logged IMU data. Set INS_LOG_BAT_MASK/CNT, "
              "enable the IMU batch sampler in LOG_BITMASK, fly, then pick "
              "the resulting .bin to graph a frequency spectrum.");
}

bool ConfigFFTViewModel::canEditParameters() const
{
    return m_connected && m_heartbeatFresh && !m_armed
        && m_snapshotComplete && !m_pending.active
        && !m_reconciliationRequired && !m_shuttingDown;
}

bool ConfigFFTViewModel::canEditParameter(const QString &name) const
{
    const int index = fieldIndex(name);
    return canEditParameters() && index >= 0
        && !m_fields.at(index).readOnly;
}

bool ConfigFFTViewModel::canRefreshParameters() const
{
    return m_connected && !m_pending.active && !m_shuttingDown;
}

void ConfigFFTViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    rebuildFields();
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigFFTViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot)
{
    ++m_snapshotRevision;
    m_pending = {};
    m_parameters = parameters;
    m_componentId = preferredComponent;
    m_snapshotComplete = completeSnapshot;
    if (completeSnapshot) {
        m_reconciliationRequired = false;
    }
    rebuildFields();
    updateParameterStatus();
    emit fieldsChanged();
    emit stateChanged();
}

void ConfigFFTViewModel::setParameterContext(
    bool connected, bool heartbeatFresh, bool armed)
{
    const bool changed = m_connected != connected
        || m_heartbeatFresh != heartbeatFresh || m_armed != armed;
    m_connected = connected;
    m_heartbeatFresh = heartbeatFresh;
    m_armed = armed;
    if (!changed) {
        return;
    }
    if (!connected && m_pending.active) {
        finishWrite(false,
                    tr("Connection was lost during the parameter write."),
                    m_pending.batchId != 0);
        return;
    }
    updateParameterStatus();
    emit stateChanged();
}

void ConfigFFTViewModel::setConnected(bool connected)
{
    setParameterContext(connected, m_heartbeatFresh, m_armed);
}

void ConfigFFTViewModel::setHeartbeatFresh(bool fresh)
{
    setParameterContext(m_connected, fresh, m_armed);
}

void ConfigFFTViewModel::setArmed(bool armed)
{
    setParameterContext(m_connected, m_heartbeatFresh, armed);
}

bool ConfigFFTViewModel::setBins(int bins)
{
    if (analysisBusy()) {
        return false;
    }
    const int bounded = qBound(4, bins, 14);
    if (m_bins == bounded) {
        return false;
    }
    m_bins = bounded;
    emit stateChanged();
    return true;
}

bool ConfigFFTViewModel::setStartFrequencyHz(double frequencyHz)
{
    if (analysisBusy() || !std::isfinite(frequencyHz)) {
        return false;
    }
    const double bounded = qBound(0.0, frequencyHz, 1000.0);
    if (qFuzzyCompare(1.0 + m_startFrequencyHz, 1.0 + bounded)) {
        return false;
    }
    m_startFrequencyHz = bounded;
    emit stateChanged();
    return true;
}

bool ConfigFFTViewModel::setMagnitude(bool magnitude)
{
    if (analysisBusy() || m_magnitude == magnitude) {
        return false;
    }
    m_magnitude = magnitude;
    emit stateChanged();
    return true;
}

bool ConfigFFTViewModel::stageParameterValue(
    const QString &name, const QVariant &value)
{
    const int index = fieldIndex(name);
    if (index < 0 || !canEditParameter(name)) {
        updateParameterStatus();
        emit stateChanged();
        return false;
    }
    ParamField &field = m_fields[index];
    const QVariant candidate = typedValue(value, field.value);
    bool numeric = false;
    const double numericValue = candidate.toDouble(&numeric);
    if (!numeric || !std::isfinite(numericValue)
        || (field.hasRange && field.enforceRange
            && (numericValue < field.minimum
                || numericValue > field.maximum))) {
        m_parameterStatus = tr("%1 value is outside its supported range.")
            .arg(field.name);
        emit stateChanged();
        return false;
    }
    if (valuesEqual(field.value, candidate)) {
        return false;
    }

    ++m_requestGeneration;
    if (m_requestGeneration == 0) {
        ++m_requestGeneration;
    }
    m_pending.requestId = m_requestGeneration;
    m_pending.snapshotRevision = m_snapshotRevision;
    m_pending.name = field.name;
    m_pending.acceptedValue = field.value;
    m_pending.stagedValue = candidate;
    m_pending.active = true;
    field.value = candidate;
    field.status = tr("writing…");
    updateParameterStatus();
    emit stateChanged();

    const quint64 requestId = m_pending.requestId;
    QTimer::singleShot(kWriteTimeoutMs, this, [this, requestId]() {
        if (!m_pending.active || m_pending.requestId != requestId) {
            return;
        }
        finishWrite(false, tr("Parameter write timed out."), true);
        if (m_connected) {
            emit refreshRequested(m_componentId);
        }
    });
    emit writeRequested(requestId, m_componentId,
                        m_pending.name, candidate);
    return true;
}

bool ConfigFFTViewModel::refreshParameters()
{
    if (!canRefreshParameters()) {
        updateParameterStatus();
        emit stateChanged();
        return false;
    }
    m_parameterStatus = tr("Refreshing FFT parameters…");
    emit stateChanged();
    emit refreshRequested(m_componentId);
    return true;
}

bool ConfigFFTViewModel::analyzeFile(const QString &path)
{
    if (!canAnalyze()) {
        return false;
    }
    const QString stablePath = QFileInfo(path).absoluteFilePath();
    if (path.trimmed().isEmpty() || !QFileInfo(stablePath).isFile()) {
        m_analysisStatus = tr("Select an existing DataFlash .bin log.");
        emit stateChanged();
        return false;
    }

    DataFlashFftAnalyzer::Options options;
    options.bins = m_bins;
    options.startFrequencyHz = m_startFrequencyHz;
    options.magnitude = m_magnitude;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    options.isCancelled = [cancel]() {
        return cancel->load(std::memory_order_relaxed);
    };
    const Analyzer analyzer = m_analyzer;
    const auto result = std::make_shared<DataFlashFftAnalyzer::Result>();
    const quint64 generation = ++m_analysisGeneration;

    QThread *const worker = QThread::create(
        [analyzer, stablePath, options, result]() mutable {
            try {
                *result = analyzer(stablePath, options);
            } catch (const std::exception &error) {
                result->error = QString::fromLocal8Bit(error.what());
            } catch (...) {
                result->error = QStringLiteral(
                    "FFT analyzer raised an unknown exception.");
            }
        });
    worker->setObjectName(QStringLiteral("ConfigFFTAnalysisWorker"));
    m_worker = worker;
    m_analysisResult = {};
    m_analysisStatus = tr("Computing FFT…");
    connect(worker, &QThread::finished, this,
            [this, generation, worker, result]() {
        handleAnalysisFinished(generation, worker, *result);
    });
    worker->start();
    emit analysisResultChanged();
    emit stateChanged();
    return true;
}

void ConfigFFTViewModel::cancelAnalysis()
{
    if (!m_worker) {
        return;
    }
    if (m_cancel) {
        m_cancel->store(true, std::memory_order_relaxed);
    }
    m_worker->requestInterruption();
    m_analysisStatus = tr("Cancelling FFT…");
    emit stateChanged();
}

void ConfigFFTViewModel::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    ++m_analysisGeneration;
    if (m_cancel) {
        m_cancel->store(true, std::memory_order_relaxed);
    }
    QThread *const worker = m_worker.data();
    if (worker) {
        worker->requestInterruption();
        while (!worker->wait(100)) {
            worker->requestInterruption();
        }
        disconnect(worker, nullptr, this, nullptr);
        m_worker = nullptr;
        delete worker;
    }
    m_cancel.reset();
}

void ConfigFFTViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId || m_pending.active
        || m_reconciliationRequired || fieldIndex(name) < 0) {
        return;
    }
    const QString normalized = normalizedName(name);
    bool replaced = false;
    for (ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId == componentId
            && normalizedName(parameter.name) == normalized) {
            parameter.value = typedValue(value, parameter.value);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_parameters.append({componentId, normalized, value});
    }
    rebuildFields();
    updateParameterStatus();
    emit stateChanged();
}

void ConfigFFTViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotRevision != m_snapshotRevision) {
        return;
    }
    if (batchId == 0) {
        finishWrite(false, tr("Parameter write was rejected."));
        return;
    }
    m_pending.batchId = batchId;
}

void ConfigFFTViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId) {
        return;
    }
    finishWrite(false, cleanFailure(
        reason, tr("Parameter write was rejected.")));
}

void ConfigFFTViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId != batchId
        || componentId != m_componentId
        || normalizedName(name) != m_pending.name) {
        return;
    }
    finishWrite(false, cleanFailure(
        reason, tr("Parameter write failed.")), true);
}

void ConfigFFTViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId != batchId
        || componentId != m_componentId
        || normalizedName(name) != m_pending.name) {
        return;
    }
    finishWrite(false, tr("Parameter write was canceled."), true);
}

void ConfigFFTViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId != batchId) {
        return;
    }
    if (failed == 0 && succeeded == 1 && m_pending.failure.isEmpty()) {
        finishWrite(true);
    } else {
        finishWrite(false,
                    m_pending.failure.isEmpty()
                        ? tr("Parameter write did not complete successfully.")
                        : m_pending.failure,
                    true);
    }
}

void ConfigFFTViewModel::refreshFailed(const QString &reason)
{
    m_parameterStatus = tr("Parameter refresh failed: %1").arg(
        cleanFailure(reason, tr("unknown error")));
    emit stateChanged();
}

void ConfigFFTViewModel::refreshCanceled()
{
    m_parameterStatus = tr("Parameter refresh canceled.");
    emit stateChanged();
}

void ConfigFFTViewModel::rebuildFields()
{
    QHash<QString, QVariant> values;
    for (const ConfigFriendlyParameterValue &parameter : m_parameters) {
        if (parameter.componentId == m_componentId) {
            values.insert(normalizedName(parameter.name), parameter.value);
        }
    }

    m_fields.clear();
    for (const QString &name : ReferenceParameterNames()) {
        ParamField field;
        field.componentId = m_componentId;
        field.name = name;
        const bool exists = m_snapshotComplete && values.contains(name);
        field.value = exists ? values.value(name) : QVariant();
        const ParameterMetaData metadata = m_catalog.value(name);
        field.label = metadata.title.isEmpty() ? name : metadata.title;
        field.description = metadata.description;
        field.units = metadata.units;
        field.status = exists ? QString() : tr("n/a");
        field.readOnly = !exists || metadata.readOnly;
        if (!metadata.bitmaskValues.isEmpty()) {
            field.editorKind = ParamField::EditorKind::Bitmask;
            for (const QPair<int, QString> &bit : metadata.bitmaskValues) {
                field.bitOptions.append({bit.first, bit.second});
            }
        } else if (!metadata.values.isEmpty()) {
            field.editorKind = ParamField::EditorKind::Combo;
            for (const ParameterMetaDataOption &option : metadata.values) {
                field.options.append({option.value, option.label});
            }
        } else {
            field.editorKind = ParamField::EditorKind::Numeric;
        }
        field.hasRange = metadata.hasRange;
        field.minimum = metadata.hasRange ? metadata.minimum : -1.0e12;
        field.maximum = metadata.hasRange ? metadata.maximum : 1.0e12;
        field.enforceRange = m_enforceMetadataRanges;
        field.increment = metadata.hasIncrement && metadata.increment > 0.0
            ? metadata.increment : 1.0;
        if (m_pending.active && m_pending.name == name) {
            field.value = m_pending.stagedValue;
            field.status = tr("writing…");
        }
        m_fields.append(field);
    }
}

void ConfigFFTViewModel::updateParameterStatus()
{
    if (m_reconciliationRequired) {
        m_parameterStatus = tr(
            "FFT parameter state is uncertain; refresh parameters.");
    } else if (m_pending.active) {
        m_parameterStatus = tr("Writing %1…").arg(m_pending.name);
    } else if (!m_connected) {
        m_parameterStatus = tr(
            "Offline. Vehicle parameter controls are unavailable; "
            "log analysis remains available.");
    } else if (!m_heartbeatFresh) {
        m_parameterStatus = tr("Waiting for a fresh vehicle heartbeat.");
    } else if (m_armed) {
        m_parameterStatus = tr(
            "Vehicle is armed; FFT parameter changes are disabled.");
    } else if (!m_snapshotComplete) {
        m_parameterStatus = tr(
            "A complete exact-component parameter snapshot is required.");
    } else {
        const int available = std::count_if(
            m_fields.constBegin(), m_fields.constEnd(),
            [](const ParamField &field) { return !field.readOnly; });
        m_parameterStatus = tr("%1 of 3 FFT parameters available.")
            .arg(available);
    }
}

void ConfigFFTViewModel::finishWrite(
    bool succeeded, const QString &reason, bool uncertain)
{
    if (!m_pending.active) {
        return;
    }
    const PendingWrite completed = m_pending;
    m_pending = {};
    if (succeeded) {
        bool replaced = false;
        for (ConfigFriendlyParameterValue &parameter : m_parameters) {
            if (parameter.componentId == m_componentId
                && normalizedName(parameter.name) == completed.name) {
                parameter.value = completed.stagedValue;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            m_parameters.append(
                {m_componentId, completed.name, completed.stagedValue});
        }
        m_parameterStatus = tr("%1 written.").arg(completed.name);
    } else {
        m_parameterStatus = cleanFailure(
            reason, tr("Parameter write failed."));
    }
    m_reconciliationRequired = uncertain;
    rebuildFields();
    if (uncertain) {
        updateParameterStatus();
    }
    emit stateChanged();
}

void ConfigFFTViewModel::handleAnalysisFinished(
    quint64 generation, QThread *worker,
    const DataFlashFftAnalyzer::Result &result)
{
    if (m_worker != worker) {
        worker->deleteLater();
        return;
    }
    worker->wait();
    m_worker = nullptr;
    m_cancel.reset();
    worker->deleteLater();
    if (m_shuttingDown || generation != m_analysisGeneration) {
        return;
    }

    m_analysisResult = result;
    if (result.cancelled) {
        m_analysisStatus = tr("FFT calculation was cancelled.");
    } else if (!result.succeeded || result.series.isEmpty()) {
        m_analysisStatus = result.error.trimmed().isEmpty()
            ? tr("No batch (ISBH/ISBD) or IMU data found in log.")
            : result.error;
    } else {
        QString notch;
        if (result.suggestedNotchHz > 0.0) {
            notch = tr(" Suggested INS_HNTCH_FREQ ≈ %1 Hz.")
                .arg(result.suggestedNotchHz, 0, 'f', 0);
        }
        m_analysisStatus = tr("Plotted %1 curves @ %2 Hz.%3")
            .arg(result.series.size())
            .arg(result.sampleRateHz, 0, 'f', 1)
            .arg(notch);
    }
    emit analysisResultChanged();
    emit stateChanged();
}

int ConfigFFTViewModel::fieldIndex(const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (int index = 0; index < m_fields.size(); ++index) {
        if (m_fields.at(index).name == normalized) {
            return index;
        }
    }
    return -1;
}

QString ConfigFFTViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigFFTViewModel::valuesEqual(
    const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

QVariant ConfigFFTViewModel::typedValue(
    const QVariant &candidate, const QVariant &reference)
{
    switch (reference.type()) {
    case QVariant::Int:
        return candidate.toInt();
    case QVariant::UInt:
        return candidate.toUInt();
    case QVariant::LongLong:
        return candidate.toLongLong();
    case QVariant::ULongLong:
        return candidate.toULongLong();
    case QVariant::Double:
        return candidate.toDouble();
    default:
        return candidate;
    }
}
