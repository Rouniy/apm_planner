#ifndef LOGINDEXWINDOW_H
#define LOGINDEXWINDOW_H

#include "Loghandling/LogIndexTypes.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCloseEvent;
class QDialog;
class QFileDialog;
class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTableView;
class QTimer;

class LogIndexWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit LogIndexWindow(QWidget *parent = nullptr);
    ~LogIndexWindow() override;

    void setDefaultDirectories(QString dataflash, QString tlog);
    void setTileReaderFactory(
        std::function<LogIndex::TileReader()> factory);
    QString currentRoot() const { return m_currentRoot; }
    bool busy() const noexcept;
    bool isClosing() const noexcept { return m_closing; }

public slots:
    void scanDirectory(QString root);
    void cancel();

signals:
    void openLogRequested(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct JobState;
    struct DeleteContext;

    void buildUi();
    void chooseDirectory();
    void startScan(QString root, LogIndex::TileReader tiles,
                   quint64 revision);
    void finishScan(quint64 revision,
                    const std::shared_ptr<JobState> &state,
                    const LogIndex::ScanResult &result);
    void prepareDeleteSelected();
    void finishDeletePreparation(
        quint64 revision, const std::shared_ptr<JobState> &state,
        const std::shared_ptr<DeleteContext> &context);
    void showDeleteConfirmation(
        const std::shared_ptr<DeleteContext> &context,
        quint64 revision);
    void executeDelete(const std::shared_ptr<DeleteContext> &context,
                       quint64 revision);
    void finishDelete(quint64 revision,
                      const std::shared_ptr<JobState> &state,
                      const LogIndex::DeleteResult &result);
    bool replaceEntries(QVector<LogIndex::Entry> entries);
    QVector<LogIndex::Entry> selectedEntries() const;
    void updateSelectionSummary();
    void requestCloseWhileBusy(QCloseEvent *event);
    void dismissDialog(QPointer<QDialog> &dialog);
    void refreshControls();
    void updateProgress();
    void completeJob(const std::shared_ptr<JobState> &state);
    void closeAfterCancelledJob();

    QLineEdit *m_directory = nullptr;
    QPushButton *m_defaultDirectoryButton = nullptr;
    QPushButton *m_defaultTlogDirectoryButton = nullptr;
    QPushButton *m_chooseDirectoryButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLabel *m_selectionSummary = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_progressTimer = nullptr;

    QPointer<QFileDialog> m_directoryDialog;
    QPointer<QDialog> m_deletePrompt;
    QPointer<QDialog> m_closePrompt;
    QPointer<QFutureWatcherBase> m_watcher;
    std::shared_ptr<JobState> m_job;
    std::shared_ptr<DeleteContext> m_deleteContext;
    QVector<LogIndex::Entry> m_entries;
    std::function<LogIndex::TileReader()> m_tileReaderFactory;
    QString m_defaultDataflashDirectory;
    QString m_defaultTlogDirectory;
    QString m_currentRoot;
    quint64 m_revision = 0;
    bool m_admitting = false;
    bool m_initialScanStarted = false;
    bool m_closeWhenIdle = false;
    bool m_allowClose = false;
    bool m_closing = false;
};

#endif // LOGINDEXWINDOW_H
