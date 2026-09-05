#ifndef PARAMETERMETADATAREGENERATIONSERVICE_H
#define PARAMETERMETADATAREGENERATIONSERVICE_H

#include "core/parameters/ParameterMetaDataParser.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

class ParameterMetaDataRegenerationService final : public QObject
{
    Q_OBJECT

public:
    struct RunToken
    {
        quint64 id = 0;

        bool isValid() const noexcept { return id != 0; }
        friend bool operator==(const RunToken &left,
                               const RunToken &right) noexcept
        {
            return left.id == right.id;
        }
        friend bool operator!=(const RunToken &left,
                               const RunToken &right) noexcept
        {
            return !(left == right);
        }
    };

    enum class StartResult {
        Started,
        Busy,
        StorageUnavailable
    };
    Q_ENUM(StartResult)

    enum class Phase {
        Idle,
        DownloadingSources,
        BuildingGeneratedSource,
        DownloadingPreparedProducts,
        Finished
    };
    Q_ENUM(Phase)

    enum class ArtifactKind {
        GeneratedSource,
        PreparedPdef
    };
    Q_ENUM(ArtifactKind)

    enum class Outcome {
        Complete,
        PartialFailure,
        Failed,
        Cancelled
    };
    Q_ENUM(Outcome)

    struct SourceSpec
    {
        QUrl url;
        QString vehicleName;
        QString refName;
        int rank = -1;
    };

    struct ProductSpec
    {
        QString product;
        QString vehicleName;
        bool requireVehicleSection = true;
        QUrl url;
        QString fileName;
    };

    struct ArtifactReport
    {
        ArtifactKind kind = ArtifactKind::PreparedPdef;
        QString key;
        QString filePath;
        QUrl sourceUrl;
        QByteArray sha256;
        qint64 byteCount = 0;
        bool published = false;
        QString error;
    };

    struct Snapshot
    {
        RunToken token;
        bool active = false;
        Phase phase = Phase::Idle;
        bool terminal = false;
        Outcome outcome = Outcome::Failed;
        int completedUnits = 0;
        int totalUnits = 0;
        QList<ArtifactReport> artifacts;
        QStringList logLines;
    };

    struct Result
    {
        RunToken token;
        Outcome outcome = Outcome::Failed;
        QList<ArtifactReport> artifacts;
        QStringList logLines;
    };

    explicit ParameterMetaDataRegenerationService(
        const QString &cacheDirectory, QObject *parent = nullptr);
    ParameterMetaDataRegenerationService(
        const QString &cacheDirectory,
        const QUrl &loopbackEndpointOverride,
        QObject *parent = nullptr);
    ~ParameterMetaDataRegenerationService() override;

    StartResult start(RunToken *tokenOut, QString *error = nullptr);
    bool cancel(const RunToken &token);
    bool busy() const noexcept { return m_snapshot.active; }
    Snapshot snapshot() const { return m_snapshot; }
    QString outputDirectory() const { return m_outputDirectory; }

    static QList<SourceSpec> sourceManifest();
    static QList<ProductSpec> productManifest();

signals:
    void progress(
        ParameterMetaDataRegenerationService::RunToken token,
        ParameterMetaDataRegenerationService::Phase phase,
        int completed, int total, QString label);
    void logLine(
        ParameterMetaDataRegenerationService::RunToken token,
        QString line);
    void publication(
        ParameterMetaDataRegenerationService::RunToken token,
        ParameterMetaDataRegenerationService::ArtifactReport artifact);
    void finished(
        ParameterMetaDataRegenerationService::Result result);

private:
    struct SourceContext
    {
        QUrl url;
        QString vehicleName;
        QString refName;
        QString graphPrefix;
        QString orderKey;
        QStringList ancestry;
        int rootRank = -1;
        int depth = 0;
        int ordinal = -1;
        bool required = true;
    };

    struct ParsedContext
    {
        SourceContext context;
        ParameterMetaDataParser::ParsedFile parsed;
    };

    struct FetchTask
    {
        enum class Kind { Source, Product };

        Kind kind = Kind::Source;
        QUrl logicalUrl;
        QString key;
        QString vehicleName;
        bool requireVehicleSection = true;
        QString fileName;
        int attempt = 1;
        qint64 maximumBytes = 0;
        QByteArray payload;
        bool payloadTooLarge = false;
    };

    void beginRun(quint64 runId);
    void enqueueSourceContext(const SourceContext &context);
    void processSourceContext(const SourceContext &context,
                              const QByteArray &payload);
    void pumpSourceDownloads();
    void finishSourceGraphIfReady();
    void buildGeneratedSource();
    void beginPreparedProducts();
    void pumpProductDownloads();
    void startFetch(const FetchTask &task);
    void fetchFinished(QNetworkReply *reply);
    void acceptSourcePayload(const FetchTask &task,
                             const QByteArray &payload);
    void acceptProductPayload(const FetchTask &task,
                              const QByteArray &payload);
    void failFetch(const FetchTask &task, const QString &reason,
                   bool notFound = false);
    void publishArtifact(ArtifactKind kind, const QString &key,
                         const QString &fileName, const QUrl &sourceUrl,
                         const QByteArray &xml);
    void appendArtifactFailure(ArtifactKind kind, const QString &key,
                               const QUrl &sourceUrl,
                               const QString &reason);
    void appendLog(const QString &line);
    void emitProgress(const QString &label);
    void finishRun(Outcome outcome);
    void abortReplies();
    bool runIsCurrent(quint64 runId) const noexcept;
    QUrl requestUrl(const QUrl &logicalUrl) const;

    QString m_outputDirectory;
    QUrl m_loopbackEndpointOverride;
    QNetworkAccessManager m_network;
    Snapshot m_snapshot;
    quint64 m_nextRunId = 1;
    bool m_sourceGraphFailed = false;
    qint64 m_sourceCacheBytes = 0;
    qint64 m_parsedMetadataCharacters = 0;
    int m_parsedParameterCount = 0;
    int m_nextContextOrdinal = 0;
    QQueue<QString> m_pendingSourceUrls;
    QSet<QString> m_queuedSourceUrls;
    QSet<QString> m_failedSourceUrls;
    QMap<QString, QByteArray> m_sourceCache;
    QMap<QString, QList<SourceContext>> m_waitingSourceContexts;
    QList<ParsedContext> m_parsedContexts;
    QQueue<ProductSpec> m_pendingProducts;
    QMap<QNetworkReply *, FetchTask> m_activeFetches;
};

Q_DECLARE_METATYPE(ParameterMetaDataRegenerationService::RunToken)
Q_DECLARE_METATYPE(ParameterMetaDataRegenerationService::Phase)
Q_DECLARE_METATYPE(ParameterMetaDataRegenerationService::ArtifactReport)
Q_DECLARE_METATYPE(ParameterMetaDataRegenerationService::Result)

#endif
