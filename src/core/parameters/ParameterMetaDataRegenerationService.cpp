#include "ParameterMetaDataRegenerationService.h"

#include "core/parameters/ParameterMetaData.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
constexpr int MaximumConcurrentDownloads = 3;
constexpr int MaximumAttempts = 3;
constexpr int RequestTimeoutMs = 30000;
constexpr qint64 MaximumSourceBytes = 8 * 1024 * 1024;
constexpr qint64 MaximumSourceCacheBytes = 64 * 1024 * 1024;
constexpr qint64 MaximumPdefBytes = 16 * 1024 * 1024;
constexpr int MaximumSourceContexts = 4096;
constexpr int MaximumSourceUrls = 1024;
constexpr int MaximumGraphDepth = 24;
constexpr int MaximumLogLines = 1024;
constexpr int MaximumParsedParameters = 200000;
constexpr qint64 MaximumParsedMetadataCharacters = 32 * 1024 * 1024;

void postNotification(QObject *context, std::function<void()> callback)
{
    QMetaObject::invokeMethod(
        context, std::move(callback), Qt::QueuedConnection);
}

qint64 parsedMetadataCharacters(
    const ParameterMetaDataParser::ParsedFile &parsed,
    const QString &graphPrefix)
{
    qint64 characters = 0;
    for (auto parameter = parsed.parameters.constBegin();
         parameter != parsed.parameters.constEnd(); ++parameter) {
        characters += graphPrefix.size() + parameter.key().size();
        for (auto field = parameter.value().constBegin();
             field != parameter.value().constEnd(); ++field) {
            characters += field.key().size() + field.value().size();
        }
    }
    for (const auto &group : parsed.groups) {
        characters += group.prefix.size();
        for (const QString &path : group.paths) {
            characters += path.size();
        }
    }
    for (const QString &nested : parsed.nestedSources) {
        characters += nested.size();
    }
    return characters;
}

QString sourceKey(const QUrl &url)
{
    return url.toString(QUrl::FullyEncoded);
}

bool isLoopbackHost(const QString &host)
{
    return host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
        || host == QLatin1String("127.0.0.1")
        || host == QLatin1String("::1");
}

bool isTrustedSourceUrl(const QUrl &url, const QString &refName)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https")
        || url.host().compare(QLatin1String("raw.githubusercontent.com"),
                              Qt::CaseInsensitive) != 0
        || url.port(-1) != -1 || !url.userInfo().isEmpty()
        || !url.query().isEmpty() || !url.fragment().isEmpty()
        || refName.isEmpty()) {
        return false;
    }
    const QString encodedPath = url.path(QUrl::FullyEncoded);
    if (encodedPath.contains(QLatin1String("%2f"), Qt::CaseInsensitive)
        || encodedPath.contains(QLatin1String("%5c"), Qt::CaseInsensitive)) {
        return false;
    }
    const QString prefix = QStringLiteral("/ArduPilot/ardupilot/%1/")
                               .arg(refName);
    return url.path().startsWith(prefix)
        && (url.path().endsWith(QLatin1String(".cpp"),
                                Qt::CaseInsensitive)
            || url.path().endsWith(QLatin1String(".h"),
                                   Qt::CaseInsensitive));
}

bool isSafeRelativeReference(const QString &text)
{
    if (text.isEmpty() || text.contains(QLatin1Char('\\'))
        || text.contains(QLatin1Char('\0'))
        || text.contains(QLatin1Char('?'))
        || text.contains(QLatin1Char('#'))) {
        return false;
    }
    const QUrl relative = QUrl::fromEncoded(
        text.toUtf8(), QUrl::StrictMode);
    return relative.isValid() && relative.isRelative()
        && relative.scheme().isEmpty() && relative.authority().isEmpty()
        && !text.startsWith(QLatin1Char('/'));
}

QUrl resolveGroupSource(const QUrl &base, const QString &relative,
                        const QString &refName)
{
    if (!isSafeRelativeReference(relative)) {
        return {};
    }
    const QUrl resolved = base.resolved(
        QUrl::fromEncoded(relative.toUtf8(), QUrl::StrictMode));
    return isTrustedSourceUrl(resolved, refName) ? resolved : QUrl();
}

QUrl resolveNestedSource(const QUrl &base, const QString &stem,
                         const QString &refName)
{
    static const QRegularExpression safeStem(
        QStringLiteral("^[A-Za-z0-9_+.-]+$"));
    if (!safeStem.match(stem).hasMatch()
        || stem == QLatin1String(".") || stem == QLatin1String("..")) {
        return {};
    }
    QString path = base.path();
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (slash < 0 || dot <= slash) {
        return {};
    }
    path = path.left(slash + 1) + stem + path.mid(dot);
    QUrl resolved = base;
    resolved.setPath(path);
    return isTrustedSourceUrl(resolved, refName) ? resolved : QUrl();
}

bool validPdefCatalogs(const QByteArray &xml,
                       const QStringList &expectedVehicles,
                       bool requireVehicleSection,
                       QString *error)
{
    for (const QString &vehicle : expectedVehicles) {
        QBuffer buffer;
        buffer.setData(xml);
        if (!buffer.open(QIODevice::ReadOnly)) {
            if (error) {
                *error = QStringLiteral("PDEF validation buffer is unavailable");
            }
            return false;
        }
        const ParameterMetaDataCatalog catalog =
            ParameterMetaDataCatalog::fromPdef(
                &buffer, vehicle, requireVehicleSection);
        if (!catalog.isValid() || catalog.entries().size() <= 100) {
            if (error) {
                *error = catalog.isValid()
                    ? QStringLiteral(
                          "PDEF vehicle '%1' contains only %2 parameters")
                          .arg(vehicle)
                          .arg(catalog.entries().size())
                    : QStringLiteral("PDEF vehicle '%1': %2")
                          .arg(vehicle, catalog.errorString());
            }
            return false;
        }
    }
    return true;
}
}

ParameterMetaDataRegenerationService::ParameterMetaDataRegenerationService(
    const QString &cacheDirectory, QObject *parent)
    : ParameterMetaDataRegenerationService(
          cacheDirectory, QUrl(), parent)
{
}

ParameterMetaDataRegenerationService::ParameterMetaDataRegenerationService(
    const QString &cacheDirectory,
    const QUrl &loopbackEndpointOverride,
    QObject *parent)
    : QObject(parent),
      m_outputDirectory(cacheDirectory.trimmed().isEmpty()
          ? QString()
          : QDir(cacheDirectory).filePath(QStringLiteral("regenerated"))),
      m_network(this)
{
    if (loopbackEndpointOverride.isValid()
        && isLoopbackHost(loopbackEndpointOverride.host())) {
        m_loopbackEndpointOverride = loopbackEndpointOverride;
    }
    qRegisterMetaType<RunToken>();
    qRegisterMetaType<Phase>();
    qRegisterMetaType<ArtifactReport>();
    qRegisterMetaType<Result>();
}

ParameterMetaDataRegenerationService::~ParameterMetaDataRegenerationService()
{
    abortReplies();
}

QList<ParameterMetaDataRegenerationService::SourceSpec>
ParameterMetaDataRegenerationService::sourceManifest()
{
    const QString raw = QStringLiteral(
        "https://raw.githubusercontent.com/ArduPilot/ardupilot/");
    const struct Entry {
        const char *ref;
        const char *path;
        const char *vehicle;
    } entries[] = {
        {"master", "ArduCopter/Parameters.cpp", "ArduCopter2"},
        {"master", "ArduPlane/Parameters.cpp", "ArduPlane"},
        {"master", "Rover/Parameters.cpp", "ArduRover"},
        {"master", "ArduSub/Parameters.cpp", "ArduSub"},
        {"master", "AntennaTracker/Parameters.cpp", "ArduTracker"},
        {"ArduCopter-stable", "ArduCopter/Parameters.cpp", "ArduCopter2"},
        {"ArduPlane-stable", "ArduPlane/Parameters.cpp", "ArduPlane"},
        {"APMrover2-stable", "Rover/Parameters.cpp", "ArduRover"},
        {"ArduSub-stable", "ArduSub/Parameters.cpp", "ArduSub"}
    };
    QList<SourceSpec> result;
    for (int rank = 0; rank < int(sizeof(entries) / sizeof(entries[0]));
         ++rank) {
        SourceSpec spec;
        spec.refName = QString::fromLatin1(entries[rank].ref);
        spec.vehicleName = QString::fromLatin1(entries[rank].vehicle);
        spec.url = QUrl(raw + spec.refName + QLatin1Char('/')
                        + QString::fromLatin1(entries[rank].path));
        spec.rank = rank;
        result.append(spec);
    }
    return result;
}

QList<ParameterMetaDataRegenerationService::ProductSpec>
ParameterMetaDataRegenerationService::productManifest()
{
    const QStringList products = {
        QStringLiteral("SITL"), QStringLiteral("AP_Periph"),
        QStringLiteral("ArduSub"), QStringLiteral("Rover"),
        QStringLiteral("ArduCopter"), QStringLiteral("ArduPlane"),
        QStringLiteral("AntennaTracker"), QStringLiteral("Blimp"),
        QStringLiteral("Heli")
    };
    QList<ProductSpec> result;
    for (const QString &product : products) {
        ProductSpec spec;
        spec.product = product;
        spec.vehicleName = product == QLatin1String("Heli")
            ? QStringLiteral("Helicopter") : product;
        spec.requireVehicleSection = product != QLatin1String("SITL");
        const QString sourceProduct = product == QLatin1String("Rover")
            ? QStringLiteral("APMrover2") : product;
        spec.url = QUrl(QStringLiteral(
            "https://autotest.ardupilot.org/Parameters/%1/apm.pdef.xml")
                            .arg(sourceProduct));
        spec.fileName = product + QStringLiteral(".pdef.xml");
        result.append(spec);
    }
    return result;
}

ParameterMetaDataRegenerationService::StartResult
ParameterMetaDataRegenerationService::start(
    RunToken *tokenOut, QString *error)
{
    if (tokenOut) {
        *tokenOut = {};
    }
    if (error) {
        error->clear();
    }
    if (m_snapshot.active) {
        if (error) {
            *error = QStringLiteral("Parameter metadata regeneration is already running");
        }
        return StartResult::Busy;
    }
    if (m_outputDirectory.isEmpty()
        || !QDir().mkpath(m_outputDirectory)) {
        if (error) {
            *error = QStringLiteral("Cannot create the regenerated metadata directory");
        }
        return StartResult::StorageUnavailable;
    }

    m_snapshot = {};
    m_snapshot.token.id = m_nextRunId++;
    if (m_snapshot.token.id == 0) {
        m_snapshot.token.id = m_nextRunId++;
    }
    m_snapshot.active = true;
    m_snapshot.phase = Phase::DownloadingSources;
    m_snapshot.totalUnits = productManifest().size();
    m_sourceGraphFailed = false;
    m_sourceCacheBytes = 0;
    m_parsedMetadataCharacters = 0;
    m_parsedParameterCount = 0;
    m_nextContextOrdinal = 0;
    m_pendingSourceUrls.clear();
    m_queuedSourceUrls.clear();
    m_failedSourceUrls.clear();
    m_sourceCache.clear();
    m_waitingSourceContexts.clear();
    m_parsedContexts.clear();
    m_pendingProducts.clear();
    abortReplies();
    if (tokenOut) {
        *tokenOut = m_snapshot.token;
    }
    const quint64 runId = m_snapshot.token.id;
    QTimer::singleShot(0, this, [this, runId]() { beginRun(runId); });
    return StartResult::Started;
}

bool ParameterMetaDataRegenerationService::cancel(const RunToken &token)
{
    if (!token.isValid() || !runIsCurrent(token.id)) {
        return false;
    }
    abortReplies();
    finishRun(Outcome::Cancelled);
    return true;
}

void ParameterMetaDataRegenerationService::beginRun(quint64 runId)
{
    if (!runIsCurrent(runId)) {
        return;
    }
    appendLog(QStringLiteral(
        "Downloading current ArduPilot master/stable parameter sources."));
    int rootIndex = 0;
    for (const SourceSpec &spec : sourceManifest()) {
        SourceContext context;
        context.url = spec.url;
        context.vehicleName = spec.vehicleName;
        context.refName = spec.refName;
        context.rootRank = spec.rank;
        context.orderKey = QStringLiteral("%1").arg(
            rootIndex++, 4, 10, QLatin1Char('0'));
        context.ancestry.append(sourceKey(context.url));
        enqueueSourceContext(context);
        if (!runIsCurrent(runId)) {
            return;
        }
    }
    pumpSourceDownloads();
}

void ParameterMetaDataRegenerationService::enqueueSourceContext(
    const SourceContext &input)
{
    if (!m_snapshot.active || m_snapshot.phase != Phase::DownloadingSources) {
        return;
    }
    if (m_nextContextOrdinal >= MaximumSourceContexts
        || input.depth > MaximumGraphDepth) {
        m_sourceGraphFailed = true;
        appendLog(QStringLiteral(
            "Source graph exceeded its bounded node/depth limit."));
        return;
    }
    if (!isTrustedSourceUrl(input.url, input.refName)) {
        m_sourceGraphFailed = true;
        appendLog(QStringLiteral("Rejected untrusted source URL: %1")
                      .arg(input.url.toString()));
        return;
    }
    SourceContext context = input;
    context.ordinal = m_nextContextOrdinal++;
    const QString key = sourceKey(context.url);
    if (m_failedSourceUrls.contains(key)) {
        if (context.required) {
            m_sourceGraphFailed = true;
            appendLog(QStringLiteral(
                "A required source was previously unavailable: %1")
                          .arg(context.url.toString()));
        }
        return;
    }
    const auto cached = m_sourceCache.constFind(key);
    if (cached != m_sourceCache.constEnd()) {
        processSourceContext(context, cached.value());
        return;
    }
    m_waitingSourceContexts[key].append(context);
    if (!m_queuedSourceUrls.contains(key)) {
        if (m_queuedSourceUrls.size() >= MaximumSourceUrls) {
            m_waitingSourceContexts.remove(key);
            m_sourceGraphFailed = true;
            appendLog(QStringLiteral(
                "Source graph exceeded its bounded URL limit."));
            return;
        }
        m_queuedSourceUrls.insert(key);
        m_pendingSourceUrls.enqueue(key);
        ++m_snapshot.totalUnits;
    }
}

void ParameterMetaDataRegenerationService::processSourceContext(
    const SourceContext &context, const QByteArray &payload)
{
    const auto parsed = ParameterMetaDataParser::Parse(
        QString::fromUtf8(payload), context.vehicleName);
    if (!parsed.error.isEmpty()) {
        m_sourceGraphFailed = true;
        appendLog(QStringLiteral("%1: %2")
                      .arg(context.url.toString(), parsed.error));
        return;
    }
    const int parameterCount = parsed.parameters.size();
    const qint64 metadataCharacters = parsedMetadataCharacters(
        parsed, context.graphPrefix);
    if (parameterCount
            > MaximumParsedParameters - m_parsedParameterCount
        || metadataCharacters
            > MaximumParsedMetadataCharacters
                - m_parsedMetadataCharacters) {
        m_sourceGraphFailed = true;
        appendLog(QStringLiteral(
            "Parsed source graph exceeded its bounded metadata budget."));
        return;
    }
    m_parsedParameterCount += parameterCount;
    m_parsedMetadataCharacters += metadataCharacters;
    m_parsedContexts.append({context, parsed});

    int childIndex = 0;
    const auto enqueueChild = [&](const QUrl &url, const QString &prefix,
                                  int childOrder, bool required) {
        if (!url.isValid()) {
            m_sourceGraphFailed = true;
            appendLog(QStringLiteral("Rejected recursive source below %1")
                          .arg(context.url.toString()));
            return;
        }
        const QString key = sourceKey(url);
        if (key == sourceKey(context.url)) {
            appendLog(QStringLiteral("Skipped benign source self-reference: %1")
                          .arg(url.toString()));
            return;
        }
        if (context.ancestry.contains(key)) {
            m_sourceGraphFailed = true;
            appendLog(QStringLiteral("Recursive source cycle rejected: %1")
                          .arg(url.toString()));
            return;
        }
        SourceContext child;
        child.url = url;
        child.vehicleName = context.vehicleName;
        child.refName = context.refName;
        child.graphPrefix = prefix;
        child.rootRank = context.rootRank;
        child.depth = context.depth + 1;
        child.required = required;
        child.orderKey = context.orderKey + QStringLiteral(".%1").arg(
            childOrder, 4, 10, QLatin1Char('0'));
        child.ancestry = context.ancestry;
        child.ancestry.append(key);
        enqueueSourceContext(child);
    };

    for (const QString &nested : parsed.nestedSources) {
        enqueueChild(resolveNestedSource(context.url, nested,
                                         context.refName),
                     context.graphPrefix, childIndex++, false);
    }
    for (const auto &group : parsed.groups) {
        for (const QString &path : group.paths) {
            enqueueChild(resolveGroupSource(context.url, path,
                                            context.refName),
                         context.graphPrefix + group.prefix,
                         childIndex++, true);
        }
    }
}

void ParameterMetaDataRegenerationService::pumpSourceDownloads()
{
    if (!m_snapshot.active || m_snapshot.phase != Phase::DownloadingSources) {
        return;
    }
    while (m_activeFetches.size() < MaximumConcurrentDownloads
           && !m_pendingSourceUrls.isEmpty()) {
        const QString key = m_pendingSourceUrls.dequeue();
        const auto waiting = m_waitingSourceContexts.constFind(key);
        if (waiting == m_waitingSourceContexts.constEnd()
            || waiting->isEmpty()) {
            continue;
        }
        FetchTask task;
        task.kind = FetchTask::Kind::Source;
        task.logicalUrl = waiting->first().url;
        task.key = key;
        task.maximumBytes = MaximumSourceBytes;
        startFetch(task);
    }
    finishSourceGraphIfReady();
}

void ParameterMetaDataRegenerationService::finishSourceGraphIfReady()
{
    if (!m_snapshot.active || m_snapshot.phase != Phase::DownloadingSources
        || !m_pendingSourceUrls.isEmpty() || !m_activeFetches.isEmpty()) {
        return;
    }
    m_snapshot.phase = Phase::BuildingGeneratedSource;
    emitProgress(QStringLiteral("Building generated source metadata"));
    buildGeneratedSource();
}

void ParameterMetaDataRegenerationService::buildGeneratedSource()
{
    if (!m_snapshot.active
        || m_snapshot.phase != Phase::BuildingGeneratedSource) {
        return;
    }
    if (!m_sourceGraphFailed) {
        QList<ParsedContext> contexts = m_parsedContexts;
        std::sort(contexts.begin(), contexts.end(),
                  [](const ParsedContext &left,
                     const ParsedContext &right) {
            if (left.context.rootRank != right.context.rootRank) {
                return left.context.rootRank < right.context.rootRank;
            }
            if (left.context.orderKey != right.context.orderKey) {
                return left.context.orderKey < right.context.orderKey;
            }
            return sourceKey(left.context.url)
                < sourceKey(right.context.url);
        });
        QMap<QString, ParameterMetaDataParser::Parameters> vehicles;
        for (const ParsedContext &context : contexts) {
            auto &target = vehicles[context.context.vehicleName];
            for (auto parameter = context.parsed.parameters.constBegin();
                 parameter != context.parsed.parameters.constEnd();
                 ++parameter) {
                const QString fullName = context.context.graphPrefix
                    + parameter.key();
                if (!fullName.isEmpty() && !target.contains(fullName)) {
                    target.insert(fullName, parameter.value());
                }
            }
        }
        const QByteArray xml = ParameterMetaDataParser::ToPdef(vehicles);
        QString validationError;
        const QStringList generatedVehicles = {
            QStringLiteral("ArduCopter"),
            QStringLiteral("ArduPlane"),
            QStringLiteral("Rover"),
            QStringLiteral("ArduSub"),
            QStringLiteral("AntennaTracker")
        };
        if (xml.isEmpty()
            || !validPdefCatalogs(xml, generatedVehicles, true,
                                  &validationError)) {
            m_sourceGraphFailed = true;
            appendLog(QStringLiteral("Generated source metadata is invalid: %1")
                          .arg(validationError));
        } else {
            publishArtifact(
                ArtifactKind::GeneratedSource,
                QStringLiteral("generated-source"),
                QStringLiteral("generated-source.pdef.xml"), QUrl(), xml);
        }
    }
    if (m_sourceGraphFailed) {
        appendArtifactFailure(
            ArtifactKind::GeneratedSource,
            QStringLiteral("generated-source"), QUrl(),
            QStringLiteral(
                "The complete recursive source graph was unavailable; the previous generated file was preserved."));
    }
    if (!m_snapshot.active) {
        return;
    }
    beginPreparedProducts();
}

void ParameterMetaDataRegenerationService::beginPreparedProducts()
{
    if (!m_snapshot.active) {
        return;
    }
    m_snapshot.phase = Phase::DownloadingPreparedProducts;
    appendLog(QStringLiteral("Force-refreshing nine prepared PDEF products."));
    for (const ProductSpec &product : productManifest()) {
        m_pendingProducts.enqueue(product);
    }
    pumpProductDownloads();
}

void ParameterMetaDataRegenerationService::pumpProductDownloads()
{
    if (!m_snapshot.active
        || m_snapshot.phase != Phase::DownloadingPreparedProducts) {
        return;
    }
    while (m_activeFetches.size() < MaximumConcurrentDownloads
           && !m_pendingProducts.isEmpty()) {
        const ProductSpec product = m_pendingProducts.dequeue();
        FetchTask task;
        task.kind = FetchTask::Kind::Product;
        task.logicalUrl = product.url;
        task.key = product.product;
        task.vehicleName = product.vehicleName;
        task.requireVehicleSection = product.requireVehicleSection;
        task.fileName = product.fileName;
        task.maximumBytes = MaximumPdefBytes;
        startFetch(task);
    }
    if (m_pendingProducts.isEmpty() && m_activeFetches.isEmpty()) {
        int published = 0;
        int failed = 0;
        for (const ArtifactReport &artifact : m_snapshot.artifacts) {
            artifact.published ? ++published : ++failed;
        }
        finishRun(failed == 0
                      ? Outcome::Complete
                      : published > 0 ? Outcome::PartialFailure
                                      : Outcome::Failed);
    }
}

void ParameterMetaDataRegenerationService::startFetch(
    const FetchTask &input)
{
    if (!m_snapshot.active
        || m_activeFetches.size() >= MaximumConcurrentDownloads) {
        return;
    }
    FetchTask task = input;
    task.payload.clear();
    task.payloadTooLarge = false;
    const QUrl actualUrl = requestUrl(task.logicalUrl);
    QNetworkRequest request(actualUrl);
    request.setRawHeader("User-Agent", "APM-Planner-3.0-ParamGen");
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#else
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
#endif
    QNetworkReply *const reply = m_network.get(request);
    reply->setReadBufferSize(64 * 1024);
    m_activeFetches.insert(reply, task);
    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        auto active = m_activeFetches.find(reply);
        if (active == m_activeFetches.end()) {
            return;
        }
        const qint64 remaining = active->maximumBytes
            - active->payload.size();
        if (remaining <= 0 || reply->bytesAvailable() > remaining) {
            active->payloadTooLarge = true;
            reply->abort();
            return;
        }
        active->payload.append(reply->read(remaining));
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply](qint64 received, qint64 total) {
        auto active = m_activeFetches.find(reply);
        if (active == m_activeFetches.end()) {
            return;
        }
        if (received > active->maximumBytes
            || total > active->maximumBytes) {
            active->payloadTooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { fetchFinished(reply); });
    QTimer::singleShot(RequestTimeoutMs, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
}

void ParameterMetaDataRegenerationService::fetchFinished(
    QNetworkReply *reply)
{
    auto active = m_activeFetches.find(reply);
    if (active == m_activeFetches.end()) {
        reply->deleteLater();
        return;
    }
    FetchTask task = active.value();
    m_activeFetches.erase(active);
    if (!task.payloadTooLarge && reply->bytesAvailable() > 0) {
        const qint64 remaining = task.maximumBytes - task.payload.size();
        if (remaining <= 0 || reply->bytesAvailable() > remaining) {
            task.payloadTooLarge = true;
        } else {
            task.payload.append(reply->read(remaining));
        }
    }
    QString failure;
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl expectedUrl = requestUrl(task.logicalUrl);
    if (task.payloadTooLarge) {
        failure = QStringLiteral("payload exceeds the bounded size limit");
    } else if (reply->error() != QNetworkReply::NoError) {
        failure = reply->errorString();
    } else if (status < 200 || status >= 300) {
        failure = QStringLiteral("HTTP %1").arg(status);
    } else if (reply->url() != expectedUrl) {
        failure = QStringLiteral("redirected to unexpected URL %1")
                      .arg(reply->url().toString());
    } else if (task.payload.isEmpty()) {
        failure = QStringLiteral("empty payload");
    }
    reply->deleteLater();

    if (!m_snapshot.active) {
        return;
    }
    if (!failure.isEmpty()) {
        if (task.attempt < MaximumAttempts
            && !task.payloadTooLarge
            && (status == 0 || status >= 500)) {
            ++task.attempt;
            startFetch(task);
            return;
        }
        failFetch(task, failure, status == 404);
    } else if (task.kind == FetchTask::Kind::Source) {
        acceptSourcePayload(task, task.payload);
    } else {
        acceptProductPayload(task, task.payload);
    }
}

void ParameterMetaDataRegenerationService::acceptSourcePayload(
    const FetchTask &task, const QByteArray &payload)
{
    if (m_sourceCacheBytes > MaximumSourceCacheBytes - payload.size()) {
        m_sourceGraphFailed = true;
        failFetch(task, QStringLiteral(
            "source cache exceeds the 64 MiB per-run limit"));
        return;
    }
    m_sourceCacheBytes += payload.size();
    m_sourceCache.insert(task.key, payload);
    const QList<SourceContext> contexts =
        m_waitingSourceContexts.take(task.key);
    ++m_snapshot.completedUnits;
    emitProgress(task.logicalUrl.fileName());
    const quint64 runId = m_snapshot.token.id;
    for (const SourceContext &context : contexts) {
        processSourceContext(context, payload);
        if (!runIsCurrent(runId)) {
            return;
        }
    }
    pumpSourceDownloads();
}

void ParameterMetaDataRegenerationService::acceptProductPayload(
    const FetchTask &task, const QByteArray &payload)
{
    QString validationError;
    if (!validPdefCatalogs(payload, {task.vehicleName},
                           task.requireVehicleSection,
                           &validationError)) {
        appendArtifactFailure(ArtifactKind::PreparedPdef, task.key,
                              task.logicalUrl, validationError);
    } else {
        publishArtifact(ArtifactKind::PreparedPdef, task.key,
                        task.fileName, task.logicalUrl, payload);
    }
    if (!m_snapshot.active) {
        return;
    }
    ++m_snapshot.completedUnits;
    emitProgress(task.key);
    pumpProductDownloads();
}

void ParameterMetaDataRegenerationService::failFetch(
    const FetchTask &task, const QString &reason, bool notFound)
{
    const QString completeReason = QStringLiteral("%1: %2")
        .arg(task.logicalUrl.toString(), reason);
    appendLog(completeReason);
    if (task.kind == FetchTask::Kind::Source) {
        const QList<SourceContext> contexts =
            m_waitingSourceContexts.take(task.key);
        const bool required = std::any_of(
            contexts.cbegin(), contexts.cend(),
            [](const SourceContext &context) { return context.required; });
        m_failedSourceUrls.insert(task.key);
        if (!notFound || required || task.payloadTooLarge) {
            m_sourceGraphFailed = true;
        } else {
            appendLog(QStringLiteral(
                "Skipped optional inferred sibling after HTTP 404: %1")
                          .arg(task.logicalUrl.toString()));
        }
        ++m_snapshot.completedUnits;
        emitProgress(task.logicalUrl.fileName());
        pumpSourceDownloads();
    } else {
        appendArtifactFailure(ArtifactKind::PreparedPdef, task.key,
                              task.logicalUrl, reason);
        ++m_snapshot.completedUnits;
        emitProgress(task.key);
        pumpProductDownloads();
    }
}

void ParameterMetaDataRegenerationService::publishArtifact(
    ArtifactKind kind, const QString &key, const QString &fileName,
    const QUrl &sourceUrl, const QByteArray &xml)
{
    ArtifactReport report;
    report.kind = kind;
    report.key = key;
    report.filePath = QDir(m_outputDirectory).filePath(fileName);
    report.sourceUrl = sourceUrl;
    report.byteCount = xml.size();
    report.sha256 = QCryptographicHash::hash(
        xml, QCryptographicHash::Sha256).toHex();

    QSaveFile file(report.filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(xml) != xml.size() || !file.commit()) {
        report.error = file.errorString().isEmpty()
            ? QStringLiteral("Atomic metadata publication failed")
            : file.errorString();
        m_snapshot.artifacts.append(report);
        appendLog(QStringLiteral("%1: %2").arg(key, report.error));
        return;
    }
    report.published = true;
    m_snapshot.artifacts.append(report);
    appendLog(QStringLiteral("Published %1").arg(report.filePath));
    if (!m_snapshot.active) {
        return;
    }
    const RunToken token = m_snapshot.token;
    postNotification(this, [this, token, report]() {
        emit publication(token, report);
    });
}

void ParameterMetaDataRegenerationService::appendArtifactFailure(
    ArtifactKind kind, const QString &key, const QUrl &sourceUrl,
    const QString &reason)
{
    ArtifactReport report;
    report.kind = kind;
    report.key = key;
    report.filePath = QDir(m_outputDirectory).filePath(
        kind == ArtifactKind::GeneratedSource
            ? QStringLiteral("generated-source.pdef.xml")
            : key + QStringLiteral(".pdef.xml"));
    report.sourceUrl = sourceUrl;
    report.error = reason;
    m_snapshot.artifacts.append(report);
    appendLog(QStringLiteral("%1: %2").arg(key, reason));
}

void ParameterMetaDataRegenerationService::appendLog(
    const QString &line)
{
    if (!m_snapshot.active || line.isEmpty()) {
        return;
    }
    if (m_snapshot.logLines.size() >= MaximumLogLines) {
        m_snapshot.logLines.removeFirst();
    }
    m_snapshot.logLines.append(line);
    const RunToken token = m_snapshot.token;
    postNotification(this, [this, token, line]() {
        emit logLine(token, line);
    });
}

void ParameterMetaDataRegenerationService::emitProgress(
    const QString &label)
{
    if (!m_snapshot.active) {
        return;
    }
    const RunToken token = m_snapshot.token;
    const Phase phase = m_snapshot.phase;
    const int completed = m_snapshot.completedUnits;
    const int total = m_snapshot.totalUnits;
    postNotification(this,
                     [this, token, phase, completed, total, label]() {
        emit progress(token, phase, completed, total, label);
    });
}

void ParameterMetaDataRegenerationService::finishRun(Outcome outcome)
{
    if (!m_snapshot.active) {
        return;
    }
    abortReplies();
    m_pendingSourceUrls.clear();
    m_waitingSourceContexts.clear();
    m_pendingProducts.clear();
    m_sourceCache.clear();
    m_parsedContexts.clear();
    m_snapshot.active = false;
    m_snapshot.phase = Phase::Finished;
    m_snapshot.terminal = true;
    m_snapshot.outcome = outcome;
    Result result;
    result.token = m_snapshot.token;
    result.outcome = outcome;
    result.artifacts = m_snapshot.artifacts;
    result.logLines = m_snapshot.logLines;
    postNotification(this, [this, result]() {
        emit finished(result);
    });
}

void ParameterMetaDataRegenerationService::abortReplies()
{
    const QList<QNetworkReply *> replies = m_activeFetches.keys();
    m_activeFetches.clear();
    for (QNetworkReply *reply : replies) {
        if (!reply) {
            continue;
        }
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

bool ParameterMetaDataRegenerationService::runIsCurrent(
    quint64 runId) const noexcept
{
    return m_snapshot.active && m_snapshot.token.id == runId;
}

QUrl ParameterMetaDataRegenerationService::requestUrl(
    const QUrl &logicalUrl) const
{
    if (!m_loopbackEndpointOverride.isValid()) {
        return logicalUrl;
    }
    QUrl result = m_loopbackEndpointOverride;
    result.setPath(logicalUrl.path());
    result.setQuery(logicalUrl.query());
    return result;
}
