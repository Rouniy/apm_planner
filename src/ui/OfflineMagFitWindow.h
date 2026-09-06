#ifndef OFFLINEMAGFITWINDOW_H
#define OFFLINEMAGFITWINDOW_H

#include "services/OfflineMagFitApplyService.h"
#include "services/OfflineMagFitService.h"

#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <memory>

class QCheckBox;
class QCloseEvent;
class QDoubleSpinBox;
class QDialog;
class QFileDialog;
class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QProgressDialog;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTimer;

/**
 * Modeless, transport-neutral Offline MagFit window.
 *
 * File analysis runs without a vehicle.  Applying a completed report is a
 * separate, explicitly confirmed operation owned by the application service;
 * the window only ever cancels the exact operation token that it started.
 */
class OfflineMagFitWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit OfflineMagFitWindow(QWidget *parent = nullptr);
    ~OfflineMagFitWindow() override;

    void setApplyService(OfflineMagFitApplyService *service);
    bool setSource(const QString &path);

    bool analysisBusy() const noexcept;
    bool busy() const noexcept;
    OfflineMagFitReport report() const { return m_report; }
    QString statusText() const;

public slots:
    void analyze();
    void cancelAnalysis();
    void cancelOwnedApply();

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct AnalysisState;

    void buildUi();
    void browse();
    void invalidateAnalysis(const QString &reason);
    void finishAnalysis(quint64 revision,
                        const std::shared_ptr<AnalysisState> &state,
                        const OfflineMagFitReport &report);
    void populateResults();
    void beginApply();
    void confirmApply();
    void startApply();
    void dismissApplyPrompt();
    void showApplyProgress(quint64 operationId);
    void handleApplyFinished(quint64 bindingRevision,
                             const OfflineMagFitApplyService::Report &report);
    void refreshApplyState();
    void refreshControls();
    void appendHistory(const QString &line);

    QPointer<OfflineMagFitApplyService> m_applyService;
    QPointer<QFileDialog> m_sourceDialog;
    QPointer<QDialog> m_applyPrompt;
    QPointer<QProgressDialog> m_applyProgress;
    QPointer<QFutureWatcherBase> m_analysisWatcher;
    std::shared_ptr<AnalysisState> m_analysisState;
    OfflineMagFitReport m_report;
    QString m_source;
    quint64 m_analysisRevision = 0;
    quint64 m_applyBindingRevision = 0;
    quint64 m_applyPromptRevision = 0;
    quint64 m_ownedApplyOperationId = 0;
    QStringList m_seenApplyHistory;
    bool m_closing = false;
    bool m_refreshing = false;

    class ApplyContext;
    std::shared_ptr<ApplyContext> m_applyContext;

    QLineEdit *m_sourcePath = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QPushButton *m_cancelAnalysisButton = nullptr;
    QDoubleSpinBox *m_throttle = nullptr;
    QCheckBox *m_ellipsoid = nullptr;
    QProgressBar *m_progress = nullptr;
    QTableWidget *m_results = nullptr;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_history = nullptr;
    QPushButton *m_applyButton = nullptr;
    QTimer *m_progressTimer = nullptr;
};

#endif // OFFLINEMAGFITWINDOW_H
