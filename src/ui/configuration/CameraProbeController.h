#ifndef CAMERAPROBECONTROLLER_H
#define CAMERAPROBECONTROLLER_H

#include "services/CameraProbeService.h"

#include <QObject>
#include <QPointer>

class QDialog;
class QEvent;
class QProgressDialog;
class QWidget;

/** Page-owned prompt/progress controller for the application-owned probe. */
class CameraProbeController final : public QObject
{
    Q_OBJECT

public:
    explicit CameraProbeController(CameraProbeService *service,
                                   QWidget *owner);
    ~CameraProbeController() override;

    bool busy() const noexcept;

public slots:
    void start();
    void cancel();
    void shutdown();

signals:
    void busyChanged(bool busy);
    void logMessage(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Phase { Idle, Preparing, Consent, Starting, Running };

    void showConsent(quint64 flow,
                     const CameraProbeService::Plan &plan);
    void beginProbe(quint64 flow,
                    const CameraProbeService::Plan &plan);
    void updateProgress();
    void handleFinished(const CameraProbeService::Report &report);
    void handleServiceDestroyed();
    void finishFlow(quint64 flow, const QString &message = QString());
    bool dismissPrompt();
    bool dismissProgress();
    bool emitLog(const QString &message);

    QPointer<QWidget> m_owner;
    QPointer<CameraProbeService> m_service;
    QPointer<QDialog> m_prompt;
    QPointer<QProgressDialog> m_progress;
    CameraProbeService::Plan m_plan;
    quint64 m_flow = 0;
    quint64 m_ownedOperationId = 0;
    Phase m_phase = Phase::Idle;
    bool m_cancelRequested = false;
    bool m_destroying = false;
};

#endif // CAMERAPROBECONTROLLER_H
