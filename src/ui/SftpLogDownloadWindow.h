#pragma once

#include "services/SftpLogDownloadService.h"

#include <QPointer>
#include <QWidget>

#include <memory>

class QCheckBox;
class QCloseEvent;
class QDialog;
class QFileDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

/** Modeless Mission Planner-compatible SFTP DataFlash log workflow. */
class SftpLogDownloadWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit SftpLogDownloadWindow(
        QWidget *parent = nullptr,
        SftpLogSessionFactory factory = createSftpLogSession);
    ~SftpLogDownloadWindow() override;

    bool busy() const noexcept;
    bool isClosing() const noexcept { return m_closing || m_shuttingDown; }
    SftpLogDownloadService *serviceForTesting() const { return m_service; }

public slots:
    void cancel();
    void shutdown();

signals:
    void closeResolved(bool accepted);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct ListingIdentity;
    struct PendingAction;

    void buildUi();
    void loadSettings();
    void saveNonSecretSettings();
    bool captureConnection(SftpLogConnection *connection,
                           QString *directory, QString *error);
    bool currentIdentity(ListingIdentity *identity, QString *error) const;
    bool listedIdentityIsCurrent(QString *error = nullptr) const;
    void invalidateListing();
    void refreshList();
    void downloadSelected();
    void downloadAll();
    void beginDownload(QVector<SftpLogEntry> entries);
    void beginDelete(QVector<SftpLogEntry> entries, bool allRows);
    void showDeleteConfirmation(const std::shared_ptr<PendingAction> &pending);
    QVector<SftpLogEntry> selectedEntries() const;
    void startPendingDownload(const QString &destination,
                              const std::shared_ptr<PendingAction> &pending);
    void startServiceOperation(const std::shared_ptr<PendingAction> &pending,
                               const QString &destination = {});
    void showHostKeyChallenge(quint64 operationId);
    void handleOperationFinished(quint64 operationId);
    void populateRows(const QVector<SftpLogEntry> &entries);
    void removeDeletedRows(const QVector<SftpLogEntry> &entries);
    void updateProgress();
    void refreshControls();
    void appendActivity(const QString &text);
    void setStatus(const QString &text);
    void dismissDialog(QPointer<QDialog> &dialog);
    void dismissPicker();
    void finishPendingUi();
    void resolveDeferredClose();

    QLineEdit *m_host = nullptr;
    QLineEdit *m_port = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_remoteDirectory = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_downloadSelected = nullptr;
    QPushButton *m_downloadAll = nullptr;
    QCheckBox *m_createKml = nullptr;
    QPushButton *m_deleteSelected = nullptr;
    QPushButton *m_deleteAll = nullptr;
    QPushButton *m_cancel = nullptr;
    QTableWidget *m_grid = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_activity = nullptr;
    QTimer *m_timer = nullptr;
    SftpLogDownloadService *m_service = nullptr;

    QPointer<QFileDialog> m_directoryDialog;
    QPointer<QDialog> m_actionPrompt;
    QPointer<QDialog> m_hostKeyPrompt;
    QPointer<QDialog> m_closePrompt;
    std::shared_ptr<PendingAction> m_pending;
    QVector<SftpLogEntry> m_entries;
    std::unique_ptr<ListingIdentity> m_listingIdentity;
    SftpLogDownloadService::Operation m_ownedOperation =
        SftpLogDownloadService::Operation::None;
    quint64 m_ownedOperationId = 0;
    quint64 m_flow = 0;
    quint64 m_formRevision = 0;
    quint64 m_listingRevision = 0;
    quint64 m_operationFormRevision = 0;
    QString m_activityText;
    bool m_closeWhenIdle = false;
    bool m_allowClose = false;
    bool m_closeResolutionEmitted = false;
    bool m_closing = false;
    bool m_shuttingDown = false;
};
