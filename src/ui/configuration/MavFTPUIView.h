#ifndef MAVFTPUIVIEW_H
#define MAVFTPUIVIEW_H

#include "comm/MavFtpServiceInterface.h"

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QCloseEvent;
class QDialog;
class QShowEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * Mission Planner 10 compatible remote-file browser.
 *
 * The widget owns only presentation and local file-picker concerns. MAVLink
 * routing, exact-target leases, retries and remote sessions remain owned by the
 * injected application-level MavFtpServiceInterface.
 */
class MavFTPUIView final : public QWidget
{
    Q_OBJECT

public:
    explicit MavFTPUIView(MavFtpServiceInterface *service,
                          QWidget *parent = nullptr);
    ~MavFTPUIView() override;

    static bool ShouldAddSystemRoot(const QStringList &rootDirectoryNames);

private:
    enum ItemRole {
        PathRole = Qt::UserRole,
        LoadedRole,
        PlaceholderRole,
        TargetLeaseRole
    };

    static constexpr qint64 MaximumBufferedTransferBytes =
        64LL * 1024LL * 1024LL;

    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

    void buildUi();
    void connectUi();
    void resetRoots();
    void refreshRoot();
    void refreshCurrentDirectory();
    void listDirectory(QTreeWidgetItem *item, bool updateEntries,
                       bool rootRefresh = false);
    void downloadSelected();
    void uploadFile();
    void deleteSelected();
    void makeDirectory();
    void cancelOperation();
    void handleTargetChanged();
    void openSelectedEntry();
    void handleResult(const MavFtpServiceInterface::Result &result);
    void handleProgress(qulonglong operationId, qulonglong generation,
                        qint64 completed, qint64 total);
    void populateDirectory(QTreeWidgetItem *directory,
                           const QVector<MavFtpProtocol::DirectoryEntry> &entries,
                           bool updateEntries, bool rootRefresh,
                           const VehicleTargetLease &target);
    void setDirectoryChildren(
        QTreeWidgetItem *directory,
        const QVector<MavFtpProtocol::DirectoryEntry> &entries,
        const VehicleTargetLease &target);
    void setEntries(const QVector<MavFtpProtocol::DirectoryEntry> &entries,
                    const VehicleTargetLease &target);
    void markDirectoryStale(QTreeWidgetItem *directory);
    bool beginPending(MavFtpServiceInterface::Operation operation,
                      const QString &remotePath,
                      const VehicleTargetLease &target);
    void admitPending(const QByteArray &data = {});
    bool finishStart(MavFtpServiceInterface::StartResult result,
                     MavFtpServiceInterface::Operation operation,
                     const QString &remotePath);
    void clearPending();
    void cancelOwnedFlow(const QString &statusText = QString());
    void dismissPrompt(bool disconnectAll = false);
    bool pendingTargetIsCurrent() const;
    bool targetIsCurrent(const VehicleTargetLease &target) const;
    VehicleTargetLease acquireTarget(QString *error = nullptr) const;
    bool selectedContentMatchesTarget(const VehicleTargetLease &target,
                                      bool requireEntry) const;
    void openDownloadDirectoryPrompt(quint64 revision);
    void openUploadFilePrompt(quint64 revision);
    void openUploadConfirmation(quint64 revision, const QByteArray &data);
    void openDeleteConfirmation(quint64 revision);
    void scheduleDirectoryRefresh(const QString &path,
                                  const VehicleTargetLease &target);
    QTreeWidgetItem *findDirectoryByPath(const QString &path) const;
    void syncControls();
    void setProgressValue(int value);
    QString folderNameError() const;
    QString selectedDirectoryPath() const;
    QString selectedEntryPath() const;
    QString selectedEntryName() const;
    bool selectedEntryIsDirectory() const;
    QTreeWidgetItem *findDirectoryChild(QTreeWidgetItem *parent,
                                        const QString &path) const;
    static QString combineRemotePath(const QString &directory,
                                     const QString &name);
    static QString uniqueDownloadPath(const QString &directory,
                                      const QString &fileName);
    static QString startFailureText(MavFtpServiceInterface::StartResult result);
    bool writeDownloadedFile(const QByteArray &data, QString *error);

    QPointer<MavFtpServiceInterface> m_service;
    QPointer<QDialog> m_prompt;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_download = nullptr;
    QPushButton *m_upload = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_mkdir = nullptr;
    QPushButton *m_cancel = nullptr;
    QLineEdit *m_newFolderName = nullptr;
    QTreeWidget *m_directories = nullptr;
    QTableWidget *m_entries = nullptr;

    QTreeWidgetItem *m_pendingDirectory = nullptr;
    MavFtpServiceInterface::Operation m_pendingOperation =
        MavFtpServiceInterface::Operation::None;
    QString m_pendingRemotePath;
    QString m_pendingLocalPath;
    QString m_pendingDisplayName;
    QString m_pendingParentPath;
    VehicleTargetLease m_pendingTarget;
    qulonglong m_pendingGeneration = 0;
    quint64 m_pendingOperationId = 0;
    quint64 m_pendingRevision = 0;
    bool m_pending = false;
    bool m_pendingListUpdatesEntries = false;
    bool m_pendingRootRefresh = false;
    quint64 m_displayTargetGeneration = 0;
    VehicleEndpoint m_displayTargetEndpoint;
    bool m_displayTargetValid = false;
    bool m_closeInProgress = false;
};

#endif // MAVFTPUIVIEW_H
