#ifndef TRANSLATIONEDITORWINDOW_H
#define TRANSLATIONEDITORWINDOW_H

#include "services/ResxTranslationService.h"

#include <QPointer>
#include <QWidget>

#include <memory>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialog;
class QFileDialog;
class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QTimer;
class TranslationEditorFilterModel;
class TranslationEditorModel;

/** Modeless editor for Mission Planner-compatible RESX culture resources. */
class TranslationEditorWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit TranslationEditorWindow(QWidget *parent = nullptr);
    ~TranslationEditorWindow() override;

    bool busy() const noexcept;
    bool isClosing() const noexcept;
    QString statusText() const;
    TranslationEditorModel *editorModel() const { return m_model; }

public slots:
    void cancel();
    void shutdown();

signals:
    // Emitted for a resolved close request. MainWindow uses this to defer its
    // own shutdown without forcing the editor past an unsaved/busy prompt.
    void closeResolved(bool accepted);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct JobState;

    void buildUi();
    void populateCultures();
    void browseSource();
    void chooseOutput();
    void chooseImport();
    void requestLoad();
    void startLoad(QString sourceRoot, QString culture, quint64 flow);
    void finishLoad(quint64 flow, const std::shared_ptr<JobState> &state,
                    const ResxTranslationService::LoadResult &result);
    void requestExport();
    void showExportConfirmation(
        QVector<ResxTranslationEntry> snapshot, QString outputRoot,
        QString culture, QStringList outputs, quint64 modelRevision,
        quint64 flow);
    void startExport(
        QVector<ResxTranslationEntry> snapshot, QString outputRoot,
        QString culture, quint64 modelRevision, quint64 flow);
    void finishExport(quint64 flow, const std::shared_ptr<JobState> &state,
                      quint64 modelRevision,
                      const ResxTranslationService::ExportResult &result);
    void startImport(QString path, quint64 flow);
    void finishImport(quint64 flow, const std::shared_ptr<JobState> &state,
                      const QString &path,
                      const ResxTranslationService::ImportResult &result);
    void copyCsv();
    void finishCsv(quint64 flow, const std::shared_ptr<JobState> &state,
                   int rows,
                   const ResxTranslationService::TextResult &result);
    void requestRevert();
    void updateCounts();
    void updateProgress();
    void refreshControls();
    void completeJob(const std::shared_ptr<JobState> &state);
    void setStatus(const QString &text);
    void dismissDialog(QPointer<QDialog> &dialog);
    void dismissPickers();
    void resolveDeferredClose();
    QString selectedCulture() const;
    QStringList exportPaths(
        const QVector<ResxTranslationEntry> &snapshot,
        const QString &outputRoot, const QString &culture,
        QString *error) const;

    QLineEdit *m_sourceRoot = nullptr;
    QPushButton *m_browseSource = nullptr;
    QComboBox *m_culture = nullptr;
    QPushButton *m_load = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_missingOnly = nullptr;
    QCheckBox *m_exportOnly = nullptr;
    QPushButton *m_revert = nullptr;
    QLabel *m_resourceCount = nullptr;
    QLabel *m_totalCount = nullptr;
    QLabel *m_missingCount = nullptr;
    QLabel *m_translatedCount = nullptr;
    QLabel *m_visibleCount = nullptr;
    QTableView *m_table = nullptr;
    TranslationEditorModel *m_model = nullptr;
    TranslationEditorFilterModel *m_filter = nullptr;
    QLineEdit *m_outputRoot = nullptr;
    QPushButton *m_chooseOutput = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_copyCsv = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_close = nullptr;
    QTimer *m_progressTimer = nullptr;

    QPointer<QFileDialog> m_sourceDialog;
    QPointer<QFileDialog> m_outputDialog;
    QPointer<QFileDialog> m_importDialog;
    QPointer<QDialog> m_prompt;
    QPointer<QDialog> m_closePrompt;
    QPointer<QFutureWatcherBase> m_watcher;
    std::shared_ptr<JobState> m_job;
    QVector<TranslationCulture> m_cultures;
    QString m_loadedCulture;
    quint64 m_flow = 0;
    quint64 m_modelRevision = 0;
    bool m_closeWhenIdle = false;
    bool m_allowClose = false;
    bool m_closeResolutionEmitted = false;
    bool m_closing = false;
    bool m_shutdown = false;
};

#endif // TRANSLATIONEDITORWINDOW_H
