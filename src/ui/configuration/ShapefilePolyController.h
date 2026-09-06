#ifndef SHAPEFILEPOLYCONTROLLER_H
#define SHAPEFILEPOLYCONTROLLER_H

#include "services/ShapefileImportService.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class QDialog;
class QEvent;
class QFutureWatcherBase;
class QProgressDialog;
class QTimer;
class QWidget;

class ShapefilePolyController final : public QObject
{
    Q_OBJECT

public:
    using Prepare = std::function<ShapefileImportService::Preparation(
        const QString &, const Shapefile::Cancel &,
        const Shapefile::Progress &)>;
    using Export = std::function<ShapefileImportService::Result(
        const ShapefileImportService::Plan &, const Shapefile::Cancel &,
        const Shapefile::Progress &)>;

    explicit ShapefilePolyController(QWidget *owner);
    ~ShapefilePolyController() override;

    bool busy() const noexcept;

    // Deterministic test/runtime-audit seam. An admitted workflow always keeps
    // the copies captured before its first worker starts.
    void setBackend(Prepare prepare, Export exportFiles);

public slots:
    void start();
    void cancel();

signals:
    void busyChanged(bool busy);
    void logMessage(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Phase { Idle, InputPrompt, Preparing, Consent, Exporting };
    struct JobState;

    void showInputPrompt(quint64 flow);
    void startPreparation(quint64 flow, const QString &input);
    void finishPreparation(
        quint64 flow, const std::shared_ptr<JobState> &state,
        const ShapefileImportService::Preparation &preparation);
    void showConsent(quint64 flow);
    void startExport(quint64 flow);
    void finishExport(
        quint64 flow, const std::shared_ptr<JobState> &state,
        const ShapefileImportService::Result &result);
    void finishWorkflow(quint64 flow, const QStringList &messages);
    bool dismissPrompt();
    bool dismissProgress();
    void updateProgress();
    bool emitLog(const QString &message);
    void showProgress(const QString &objectName, const QString &title,
                      const QString &label);
    static QStringList preparationFailureMessages(
        const ShapefileImportService::Preparation &preparation);
    static QStringList exportResultMessages(
        const ShapefileImportService::Result &result);

    QPointer<QWidget> m_owner;
    QPointer<QDialog> m_prompt;
    QPointer<QProgressDialog> m_progress;
    QPointer<QFutureWatcherBase> m_watcher;
    QTimer *m_progressTimer = nullptr;
    Prepare m_prepare;
    Export m_export;
    ShapefileImportService::Plan m_plan;
    std::shared_ptr<JobState> m_job;
    QString m_input;
    quint64 m_flow = 0;
    int m_lastReportedPercent = -1;
    bool m_cancelReported = false;
    bool m_destroying = false;
    Phase m_phase = Phase::Idle;
};

#endif // SHAPEFILEPOLYCONTROLLER_H
