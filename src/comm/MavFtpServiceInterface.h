#ifndef MAVFTPSERVICEINTERFACE_H
#define MAVFTPSERVICEINTERFACE_H

#include "MavFtpProtocol.h"
#include "VehicleEndpoint.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

/**
 * UI-facing seam for the asynchronous MAVLink FTP client.
 *
 * Transfers deliberately use QByteArray rather than local file paths. File
 * selection, overwrite policy and atomic writes belong to the caller; this
 * interface owns only the exact-target protocol transaction.
 */
class MavFtpServiceInterface : public QObject
{
    Q_OBJECT

public:
    enum class Operation {
        None,
        ListDirectory,
        Download,
        Upload,
        MakeDirectory,
        RemoveFile,
        RemoveDirectory
    };
    Q_ENUM(Operation)

    enum class StartResult {
        Started,
        Busy,
        NoTarget,
        StaleTarget,
        InvalidPath,
        InvalidData,
        TransportUnavailable,
        ShuttingDown
    };
    Q_ENUM(StartResult)

    using DirectoryEntry = MavFtpProtocol::DirectoryEntry;

    struct Result
    {
        Operation operation = Operation::None;
        quint64 operationId = 0;
        quint64 targetGeneration = 0;
        QString remotePath;
        QString error;
        bool cancelled = false;
        QVector<DirectoryEntry> entries;
        QByteArray data;
        quint32 remoteCrc = 0;
        quint32 localCrc = 0;

        bool succeeded() const { return !cancelled && error.isEmpty(); }
    };

    explicit MavFtpServiceInterface(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~MavFtpServiceInterface() override = default;

    virtual bool isBusy() const = 0;
    virtual Operation operation() const = 0;
    virtual quint64 activeTargetGeneration() const = 0;
    virtual quint64 activeOperationId() const { return 0; }
    virtual QString lastError() const = 0;

    virtual StartResult startList(const QString &remotePath) = 0;
    virtual StartResult startDownload(const QString &remotePath) = 0;
    virtual StartResult startOperation(Operation, const QString &,
                                      const QByteArray &, quint64 *operationIdOut)
    {
        if (operationIdOut) *operationIdOut = 0;
        return StartResult::TransportUnavailable;
    }
    // The admitted token is published before any synchronous service callback.
    // Defaults fail closed for implementations without ownership-aware support.
    virtual StartResult startDownloadForTarget(const QString &,
                                               const VehicleTargetLease &,
                                               quint64 *operationIdOut)
    {
        if (operationIdOut) *operationIdOut = 0;
        return StartResult::TransportUnavailable;
    }
    virtual StartResult startUpload(const QString &remotePath,
                                    const QByteArray &data) = 0;
    virtual StartResult startMakeDirectory(const QString &remotePath) = 0;
    virtual StartResult startRemoveFile(const QString &remotePath) = 0;
    virtual StartResult startRemoveDirectory(const QString &remotePath) = 0;
    virtual void cancel() = 0;
    virtual bool cancelOperation(quint64) { return false; }

signals:
    void stateChanged();
    void operationStarted(MavFtpServiceInterface::Operation operation,
                          qulonglong targetGeneration,
                          const QString &remotePath);
    void progressChanged(qulonglong targetGeneration,
                         qint64 completedBytes, qint64 totalBytes);
    void operationProgress(qulonglong operationId, qulonglong targetGeneration,
                           qint64 completedBytes, qint64 totalBytes);
    void operationFinished(const MavFtpServiceInterface::Result &result);
};

Q_DECLARE_METATYPE(MavFtpServiceInterface::Operation)
Q_DECLARE_METATYPE(MavFtpServiceInterface::Result)

#endif // MAVFTPSERVICEINTERFACE_H
