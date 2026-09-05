#include "LogAnonymizeService.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QtConcurrentRun>
#include <limits>
#include <exception>

LogAnonymizeService::LogAnonymizeService(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
    connect(&m_watcher, &QFutureWatcher<LogAnonymizeResult>::finished, this, [this]() {
        if (m_shutdown || !m_busy) return;
        m_result = m_watcher.result();
        m_busy = false;
        emit changed();
    });
}

LogAnonymizeService::~LogAnonymizeService()
{
    shutdown();
}

bool LogAnonymizeService::start(const QString &input, const QString &output,
                              const LogAnonymizeOptions &options, QString *error)
{
    if (m_busy || m_shutdown || m_token == std::numeric_limits<quint64>::max()) {
        if (error) *error = tr("A log job is already active or the service is shutting down.");
        return false;
    }
    ++m_token;
    m_busy = true;
    m_input = input;
    m_output = output;
    m_options = options;
    m_result = {};
    m_processed = m_total = 0;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelFlag = m_cancel;
    const quint64 run = m_token;
    QPointer<LogAnonymizeService> guard(this);
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [guard, run, cancelFlag, input, output, options]() {
        QElapsedTimer throttle;
        throttle.start();
        auto progress = [guard, run, &throttle](qint64 done, qint64 total) {
            if (done != total && throttle.elapsed() < 100) return;
            throttle.restart();
            // Destruction joins this worker before QObject lifetime ends.
            if (guard) QMetaObject::invokeMethod(guard, [guard, run, done, total]() {
                if (!guard || guard->m_shutdown || !guard->m_busy || guard->m_token != run) return;
                guard->m_processed = done;
                guard->m_total = total;
                emit guard->changed();
            }, Qt::QueuedConnection);
        };
        try {
            return LogAnonymizer::anonymizeFile(input, output, options,
                [cancelFlag]() { return cancelFlag->load(); }, progress);
        } catch (const std::exception &exception) {
            LogAnonymizeResult result;
            result.error = QStringLiteral("Log processing failed: %1")
                .arg(QString::fromUtf8(exception.what()));
            return result;
        } catch (...) {
            LogAnonymizeResult result;
            result.error = QStringLiteral("Log processing failed unexpectedly; no result was published.");
            return result;
        }
    }));
    emit changed();
    return true;
}

bool LogAnonymizeService::cancellationRequested() const
{
    return m_busy && m_cancel && m_cancel->load();
}

void LogAnonymizeService::cancel(quint64 token)
{
    if (!m_busy || token != m_token || !m_cancel) return;
    m_cancel->store(true);
    emit changed();
}

void LogAnonymizeService::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;
    if (m_cancel) m_cancel->store(true);
    m_watcher.waitForFinished();
    m_busy = false;
}
