#ifndef MAVFTPSERVICE_H
#define MAVFTPSERVICE_H

#include "ExactLinkTransmitter.h"
#include "MavFtpProtocol.h"
#include "MavFtpServiceInterface.h"
#include "VehicleEndpoint.h"

#include <QPointer>
#include <QTimer>

#include <memory>

#include <mavlink.h>

class VehicleTargetManager;

/**
 * One-operation-at-a-time MAVLink FTP client.
 *
 * Every operation captures the current immutable VehicleTargetLease.  Frames
 * are sent only through ExactLinkTransmitter and replies must match the
 * physical link, source endpoint, destination fields and FTP request envelope.
 */
class MavFtpService final : public MavFtpServiceInterface
{
    Q_OBJECT

public:
    using Operation = MavFtpServiceInterface::Operation;
    using StartResult = MavFtpServiceInterface::StartResult;
    using DirectoryEntry = MavFtpServiceInterface::DirectoryEntry;
    using Result = MavFtpServiceInterface::Result;

    static constexpr int DefaultTimeoutMs = 1000;
    static constexpr int DefaultOpenCreateTimeoutMs = 2000;
    static constexpr int DefaultCrcTimeoutMs = 30000;
    static constexpr int DefaultMaximumRetries = 3;
    static constexpr int TransferChunkSize = 80;
    static constexpr int MaximumTransferBytes = 64 * 1024 * 1024;
    static constexpr int MaximumDirectoryEntries = 100000;

    MavFtpService(VehicleTargetManager *targetManager,
                  ExactLinkTransmitter *transmitter,
                  QObject *parent = nullptr);
    ~MavFtpService() override;

    void setLocalIdentity(quint8 systemId, quint8 componentId);
    void setTimingForTesting(
        int timeoutMs,
        int maximumRetries = DefaultMaximumRetries,
        int openCreateTimeoutMs = -1,
        int crcTimeoutMs = -1);

    bool isBusy() const override { return m_active != nullptr; }
    Operation operation() const override;
    quint64 activeTargetGeneration() const override;
    QString lastError() const override { return m_lastError; }

    StartResult startList(const QString &remotePath) override;
    StartResult startDownload(const QString &remotePath) override;
    StartResult startUpload(const QString &remotePath,
                            const QByteArray &data) override;
    StartResult startMakeDirectory(const QString &remotePath) override;
    StartResult startRemoveFile(const QString &remotePath) override;
    StartResult startRemoveDirectory(const QString &remotePath) override;

    void observeMessage(int linkId, const mavlink_message_t &message);
    void forgetLink(int linkId);
    void cancel() override;
    void shutdown();

private:
    enum class Stage {
        None,
        ListPage,
        ResetBeforeDownload,
        OpenDownload,
        ReadDownload,
        ResetBeforeUpload,
        CreateUpload,
        WriteUpload,
        TerminateBeforeVerify,
        ResetBeforeVerify,
        VerifyUpload,
        Simple,
        TerminateCleanup,
        ResetCleanup
    };

    struct ActiveOperation
    {
        quint64 identity = 0;
        Result result;
        VehicleTargetLease lease;
        Stage stage = Stage::None;
        QByteArray encodedPath;
        QByteArray uploadData;
        quint32 expectedSize = 0;
        quint32 offset = 0;
        quint8 session = 0;
        int directoryRecordOffset = 0;
        bool sessionOpen = false;
        QString terminalError;
        bool terminalCancelled = false;
    };

    enum class DispatchResult {
        Sent,
        Superseded,
        StaleTarget,
        EncodingFailure,
        TransportFailure
    };

    StartResult validateStart(const QString &remotePath,
                              QByteArray *encodedPath) const;
    StartResult begin(Operation operation, const QString &remotePath,
                      const QByteArray &encodedPath,
                      const QByteArray &uploadData = QByteArray());
    bool targetIsCurrent(const VehicleTargetLease &lease) const;
    bool responseSourceMatches(
        int linkId, const mavlink_message_t &message,
        const mavlink_file_transfer_protocol_t &outer) const;

    MavFtpProtocol::PayloadHeader request(
        MavFtpProtocol::Opcode opcode, quint8 session = 0,
        quint32 offset = 0, const QByteArray &data = QByteArray(),
        int sizeOverride = -1);
    DispatchResult dispatch(const MavFtpProtocol::PayloadHeader &request);
    DispatchResult resend();
    ExactLinkTransmitter::SendResult sendForLease(
        const VehicleTargetLease &lease,
        const MavFtpProtocol::PayloadHeader &request,
        QString *error = nullptr);
    bool issue(const MavFtpProtocol::PayloadHeader &request,
               const QString &context);

    void handleResponse(const MavFtpProtocol::PayloadHeader &response);
    void handleAck(const MavFtpProtocol::PayloadHeader &response);
    void handleNak(const MavFtpProtocol::PayloadHeader &response);
    void handleListAck(const MavFtpProtocol::PayloadHeader &response);
    void handleOpenDownloadAck(
        const MavFtpProtocol::PayloadHeader &response);
    void handleReadDownloadAck(
        const MavFtpProtocol::PayloadHeader &response);
    void handleCreateUploadAck(
        const MavFtpProtocol::PayloadHeader &response);
    void handleWriteUploadAck(
        const MavFtpProtocol::PayloadHeader &response);
    void handleVerifyUploadAck(
        const MavFtpProtocol::PayloadHeader &response);

    void issueListPage();
    void issueDownloadOpen();
    void issueDownloadRead();
    void issueUploadCreate();
    void issueUploadWriteOrVerify();
    void issueUploadFinalize();
    void issueUploadResetBeforeVerify();
    void issueUploadVerify();
    void beginCleanup(const QString &terminalError = QString(),
                      bool cancelled = false);
    void advanceCleanup(const QString &cleanupError = QString());
    void sendCleanupBestEffort(const ActiveOperation &operation);

    void handleTimeout();
    void handleTargetGenerationChanged(qulonglong generation);
    void finishSuccess();
    void finishFailure(const QString &error, bool cancelled = false);
    void finishImmediate(Result result);
    void clearActive();

    static bool stageRequiresSession(Stage stage);
    static bool stageRequiresOffset(Stage stage);
    int timeoutForRequest(
        const MavFtpProtocol::PayloadHeader &request) const;
    static QString remoteErrorText(
        const MavFtpProtocol::PayloadHeader &response);
    static QString dispatchErrorText(DispatchResult result,
                                     const QString &context);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<ExactLinkTransmitter> m_transmitter;
    std::unique_ptr<ActiveOperation> m_active;
    MavFtpProtocol::PayloadHeader m_request;
    quint64 m_nextOperationIdentity = 0;
    quint64 m_requestToken = 0;
    quint16 m_nextSequence = 0;
    int m_attempts = 0;
    int m_timeoutMs = DefaultTimeoutMs;
    int m_openCreateTimeoutMs = DefaultOpenCreateTimeoutMs;
    int m_crcTimeoutMs = DefaultCrcTimeoutMs;
    int m_maximumRetries = DefaultMaximumRetries;
    quint8 m_localSystemId = 255;
    quint8 m_localComponentId = MAV_COMP_ID_MISSIONPLANNER;
    QTimer m_timer;
    QString m_lastError;
    bool m_shuttingDown = false;
};

#endif // MAVFTPSERVICE_H
