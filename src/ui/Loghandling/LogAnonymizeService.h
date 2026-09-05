#ifndef LOGANONYMIZESERVICE_H
#define LOGANONYMIZESERVICE_H

#include "LogAnonymizer.h"
#include <QObject>
#include <QFutureWatcher>
#include <QThreadPool>
#include <atomic>
#include <memory>

// One application-owned file job; closing an observer does not cancel it.
class LogAnonymizeService final : public QObject
{
    Q_OBJECT
public:
    explicit LogAnonymizeService(QObject *parent = nullptr);
    ~LogAnonymizeService() override;
    bool start(const QString &input, const QString &output,
               const LogAnonymizeOptions &options, QString *error = nullptr);
    void cancel(quint64 token);
    void shutdown();
    bool busy() const { return m_busy; }
    bool cancellationRequested() const;
    quint64 token() const { return m_token; }
    QString inputPath() const { return m_input; }
    QString outputPath() const { return m_output; }
    LogAnonymizeOptions options() const { return m_options; }
    LogAnonymizeResult result() const { return m_result; }
    qint64 bytesProcessed() const { return m_processed; }
    qint64 bytesTotal() const { return m_total; }
signals:
    void changed();
private:
    QThreadPool m_pool;
    QFutureWatcher<LogAnonymizeResult> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_busy = false;
    bool m_shutdown = false;
    quint64 m_token = 0;
    QString m_input, m_output;
    LogAnonymizeOptions m_options;
    LogAnonymizeResult m_result;
    qint64 m_processed = 0, m_total = 0;
};

#endif
