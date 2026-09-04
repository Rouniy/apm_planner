#include "OsdVideoDecoder.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QThread>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <utility>

#ifdef APM_HAS_QT_MULTIMEDIA
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QVideoFrame>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#include <QVideoSink>
#else
#include <QAbstractVideoSurface>
#include <QAbstractVideoBuffer>
#endif
#endif

#if defined(APM_HAS_QT_MULTIMEDIA) \
    && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
namespace
{
#if defined(Q_OS_LINUX)
class ScopedEnvironmentOverride final
{
public:
    ScopedEnvironmentOverride(const char *name, const char *value)
        : m_name(name)
        , m_wasSet(qEnvironmentVariableIsSet(name))
        , m_previous(m_wasSet ? qgetenv(name) : QByteArray())
    {
        qputenv(name, value);
    }

    ~ScopedEnvironmentOverride()
    {
        if (m_wasSet) {
            qputenv(m_name.constData(), m_previous);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    bool m_wasSet = false;
    QByteArray m_previous;
};
#endif

class OsdVideoSurface final : public QAbstractVideoSurface
{
public:
    using Receiver = std::function<void(const QVideoFrame &)>;

    explicit OsdVideoSurface(Receiver receiver, QObject *parent = nullptr)
        : QAbstractVideoSurface(parent)
        , m_receiver(std::move(receiver))
    {
    }

    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
        QAbstractVideoBuffer::HandleType handleType) const override
    {
        if (handleType != QAbstractVideoBuffer::NoHandle) {
            return {};
        }
        const QImage::Format imageFormats[]{
            QImage::Format_ARGB32,
            QImage::Format_ARGB32_Premultiplied,
            QImage::Format_RGB32,
            QImage::Format_RGB888,
            QImage::Format_RGB16,
            QImage::Format_RGB555,
            QImage::Format_RGBA8888,
            QImage::Format_RGBA8888_Premultiplied
        };
        QList<QVideoFrame::PixelFormat> result;
        for (QImage::Format imageFormat : imageFormats) {
            const QVideoFrame::PixelFormat pixelFormat =
                QVideoFrame::pixelFormatFromImageFormat(imageFormat);
            if (pixelFormat != QVideoFrame::Format_Invalid
                && !result.contains(pixelFormat)) {
                result.append(pixelFormat);
            }
        }
        return result;
    }

    bool present(const QVideoFrame &frame) override
    {
        if (!frame.isValid() || !m_receiver) {
            return false;
        }
        m_receiver(frame);
        return true;
    }

private:
    Receiver m_receiver;
};
}
#endif

class OsdVideoDecoder::Private
{
public:
    explicit Private(OsdVideoDecoder *decoder)
        : q(decoder)
    {
    }

    ~Private()
    {
        destroyBackend();
    }

    static int normalizedFrameRate(double value)
    {
        if (!std::isfinite(value) || value <= 0.0) {
            return OsdVideoDecoder::DefaultFramesPerSecond;
        }
        return std::clamp(
            qRound(value), 1, OsdVideoDecoder::MaximumFramesPerSecond);
    }

#ifdef APM_HAS_QT_MULTIMEDIA
    void createBackend()
    {
        if (player) {
            return;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        player = new QMediaPlayer(q);
        audioOutput = new QAudioOutput(q);
        audioOutput->setMuted(true);
        audioOutput->setVolume(0.0F);
        videoSink = new QVideoSink(q);
        player->setAudioOutput(audioOutput);
        player->setVideoOutput(videoSink);
        player->setActiveAudioTrack(-1);
        QObject::connect(
            videoSink, &QVideoSink::videoFrameChanged, q,
            [this](const QVideoFrame &frame) { receiveVideoFrame(frame); });
        QObject::connect(
            player, &QMediaPlayer::errorOccurred, q,
            [this](QMediaPlayer::Error error, const QString &description) {
                handleError(error, description);
            });
        QObject::connect(player, &QMediaPlayer::tracksChanged, q, [this]() {
            selectFirstVideoTrack();
            updateMetadata();
        });
#else
        // Request a surface-backed Qt 5 service up front. Constructing the
        // default window-backed GStreamer service can initialize VA-API before
        // setVideoOutput() and crashes on affected headless Intel drivers.
        // The service nevertheless constructs two unused display controls.
        // Make those constructors select inert sinks, then immediately restore
        // the process environment so other QMediaPlayer/QVideoWidget users
        // retain their configured output. Frames from this player are attached
        // to OsdVideoSurface below.
#if defined(Q_OS_LINUX)
        {
            ScopedEnvironmentOverride windowSink(
                "QT_GSTREAMER_WINDOW_VIDEOSINK", "fakesink");
            ScopedEnvironmentOverride widgetSink(
                "QT_GSTREAMER_WIDGET_VIDEOSINK", "fakesink");
            player = new QMediaPlayer(q, QMediaPlayer::VideoSurface);
        }
#else
        player = new QMediaPlayer(q, QMediaPlayer::VideoSurface);
#endif
        player->setMuted(true);
        player->setVolume(0);
        videoSurface = new OsdVideoSurface(
            [this](const QVideoFrame &frame) { receiveVideoFrame(frame); }, q);
        player->setVideoOutput(videoSurface);
        QObject::connect(
            player,
            QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), q,
            [this](QMediaPlayer::Error error) {
                handleError(error, player ? player->errorString() : QString());
            });
#endif
        QObject::connect(
            player, &QMediaPlayer::mediaStatusChanged, q,
            [this](QMediaPlayer::MediaStatus status) {
                handleMediaStatus(status);
            });
        QObject::connect(
            player, &QMediaPlayer::durationChanged, q,
            [this](qint64 value) { duration = std::max<qint64>(0, value); });
        QObject::connect(
            player, QOverload<>::of(&QMediaPlayer::metaDataChanged), q,
            [this]() { updateMetadata(); });
    }

    void destroyBackend()
    {
        ignoringBackendEvents = true;
        if (player) {
            player->stop();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            player->setSource(QUrl());
#else
            player->setMedia(QUrl());
#endif
            delete player;
            player = nullptr;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        delete videoSink;
        videoSink = nullptr;
        delete audioOutput;
        audioOutput = nullptr;
#else
        delete videoSurface;
        videoSurface = nullptr;
#endif
        ignoringBackendEvents = false;
    }

    void clearBackendSource()
    {
        if (!player) {
            return;
        }
        ignoringBackendEvents = true;
        player->stop();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        player->setSource(QUrl());
#else
        player->setMedia(QUrl());
#endif
        ignoringBackendEvents = false;
    }

    bool available() const
    {
        if (!player) {
            return false;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return true;
#else
        return player->isAvailable();
#endif
    }

    void setSourceAndPlay(const QString &path)
    {
        if (!player) {
            return;
        }
        const QUrl url = QUrl::fromLocalFile(path);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        player->setSource(url);
#else
        player->setMedia(url);
#endif
        player->play();
    }

    void updateMetadata()
    {
        if (!player) {
            return;
        }
        duration = std::max<qint64>(0, player->duration());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QVariant frameRate = player->metaData().value(
            QMediaMetaData::VideoFrameRate);
#else
        const QVariant frameRate = player->metaData(
            QMediaMetaData::VideoFrameRate);
#endif
        framesPerSecond = normalizedFrameRate(frameRate.toDouble());
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void selectFirstVideoTrack()
    {
        if (player && !player->videoTracks().isEmpty()
            && player->activeVideoTrack() != 0) {
            player->setActiveVideoTrack(0);
        }
    }
#endif

    void receiveVideoFrame(const QVideoFrame &source)
    {
        if (ignoringBackendEvents.load() || !source.isValid()) {
            return;
        }
        const QSize size = source.size();
        if (!size.isValid() || size.width() > MaximumFrameDimension
            || size.height() > MaximumFrameDimension) {
            dispatchFailure(
                FailureReason::InvalidFrame,
                OsdVideoDecoder::tr(
                    "The decoded video frame exceeds the 8192 x 8192 limit."));
            return;
        }

        const qint64 startTimeUs = source.startTime();
        QImage image;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        image = source.toImage();
#else
        QVideoFrame frame(source);
        const QImage::Format imageFormat =
            QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat());
        if (imageFormat != QImage::Format_Invalid
            && frame.map(QAbstractVideoBuffer::ReadOnly)) {
            const QImage mapped(
                frame.bits(), frame.width(), frame.height(),
                frame.bytesPerLine(), imageFormat);
            image = mapped.copy();
            frame.unmap();
        }
#endif
        if (image.isNull()) {
            dispatchFailure(
                FailureReason::InvalidFrame,
                OsdVideoDecoder::tr(
                    "The multimedia backend returned an unsupported video "
                    "frame format."));
            return;
        }
        if (QThread::currentThread() == q->thread()) {
            q->acceptFrame(image, startTimeUs);
            return;
        }

        const quint64 generation = sessionGeneration.load();
        const int queuedBefore = crossThreadFrames.fetch_add(1);
        if (queuedBefore >= MaximumCrossThreadFrames) {
            crossThreadFrames.fetch_sub(1);
            dispatchFailure(
                FailureReason::DecodeError,
                OsdVideoDecoder::tr(
                    "The video decoder outran the bounded HUD renderer. "
                    "No source frames were silently discarded."));
            return;
        }
        QMetaObject::invokeMethod(
            q, [this, generation, image = std::move(image), startTimeUs]() {
                crossThreadFrames.fetch_sub(1);
                if (generation != sessionGeneration.load()) {
                    finishEndOfMediaIfDrained(sessionGeneration.load());
                    return;
                }
                q->acceptFrame(image, startTimeUs);
                finishEndOfMediaIfDrained(generation);
            }, Qt::QueuedConnection);
    }

    void dispatchFailure(FailureReason reason, const QString &description)
    {
        const quint64 generation = sessionGeneration.load();
        if (QThread::currentThread() == q->thread()) {
            q->fail(reason, description);
            return;
        }
        bool expected = false;
        if (!crossThreadFailureQueued.compare_exchange_strong(expected,
                                                               true)) {
            return;
        }
        QMetaObject::invokeMethod(
            q, [this, generation, reason, description]() {
                crossThreadFailureQueued.store(false);
                if (generation == sessionGeneration.load()) {
                    q->fail(reason, description);
                }
            }, Qt::QueuedConnection);
    }

    void handleMediaStatus(QMediaPlayer::MediaStatus status)
    {
        if (ignoringBackendEvents.load() || q->state() == State::Idle
            || q->state() == State::Failed) {
            return;
        }
        if (status == QMediaPlayer::LoadedMedia
            || status == QMediaPlayer::BufferedMedia) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            selectFirstVideoTrack();
#endif
            updateMetadata();
            q->publishReady();
            return;
        }
        if (status == QMediaPlayer::EndOfMedia) {
            if (endRequested) {
                return;
            }
            updateMetadata();
            q->publishReady();
            endRequested = true;
            const quint64 generation = sessionGeneration.load();
            // The player and video surface are separate signal sources. Give
            // already-posted surface callbacks one event-loop turn, then wait
            // for every bounded cross-thread frame delivery before Ended.
            QMetaObject::invokeMethod(
                q, [this, generation]() {
                    finishEndOfMediaIfDrained(generation);
                }, Qt::QueuedConnection);
            return;
        }
        if (status == QMediaPlayer::InvalidMedia) {
            q->fail(
                FailureReason::UnsupportedMedia,
                player && !player->errorString().trimmed().isEmpty()
                    ? player->errorString()
                    : OsdVideoDecoder::tr(
                          "The local media format is not supported."));
        }
    }

    void handleError(QMediaPlayer::Error error, const QString &description)
    {
        if (ignoringBackendEvents.load() || error == QMediaPlayer::NoError
            || q->state() == State::Idle || q->state() == State::Failed) {
            return;
        }
        FailureReason reason = FailureReason::DecodeError;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        if (error == QMediaPlayer::ServiceMissingError) {
            reason = FailureReason::BackendUnavailable;
        } else
#endif
        if (error == QMediaPlayer::FormatError) {
            reason = FailureReason::UnsupportedMedia;
        }
        q->fail(
            reason,
            description.trimmed().isEmpty()
                ? OsdVideoDecoder::tr("The multimedia decoder failed.")
                : description.trimmed());
    }

    void finishEndOfMediaIfDrained(quint64 generation)
    {
        if (!endRequested || generation != sessionGeneration.load()
            || crossThreadFrames.load() > 0
            || crossThreadFailureQueued.load() || q->state() == State::Idle
            || q->state() == State::Failed || q->state() == State::Ended) {
            return;
        }
        endRequested = false;
        if (frameSequence == 0) {
            q->fail(
                FailureReason::UnsupportedMedia,
                OsdVideoDecoder::tr(
                    "The local media contains no decodable video track."));
            return;
        }
        q->setState(State::Ended);
        if (generation == sessionGeneration.load()
            && q->state() == State::Ended) {
            emit q->ended();
        }
    }

    QMediaPlayer *player = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioOutput *audioOutput = nullptr;
    QVideoSink *videoSink = nullptr;
#else
    OsdVideoSurface *videoSurface = nullptr;
#endif
#else
    void createBackend() {}
    void destroyBackend() {}
    void clearBackendSource() {}
    bool available() const { return false; }
    void setSourceAndPlay(const QString &) {}
    void updateMetadata() {}
#endif

    void clearSessionValues()
    {
        sourcePath.clear();
        framesPerSecond = DefaultFramesPerSecond;
        duration = 0;
        pendingFrame = Frame{};
        frameSequence = 0;
        readyEmitted = false;
        endRequested = false;
        crossThreadFailureQueued.store(false);
    }

    OsdVideoDecoder *const q;
    State state = State::Idle;
    QString sourcePath;
    int framesPerSecond = DefaultFramesPerSecond;
    qint64 duration = 0;
    Frame pendingFrame;
    quint64 frameSequence = 0;
    static constexpr int MaximumCrossThreadFrames = 8;
    std::atomic<quint64> sessionGeneration{0};
    std::atomic<int> crossThreadFrames{0};
    std::atomic_bool crossThreadFailureQueued{false};
    bool readyEmitted = false;
    bool endRequested = false;
    std::atomic_bool ignoringBackendEvents{false};
};

OsdVideoDecoder::OsdVideoDecoder(QObject *parent)
    : QObject(parent)
    , d(new Private(this))
{
    qRegisterMetaType<OsdVideoDecoder::State>();
    qRegisterMetaType<OsdVideoDecoder::FailureReason>();
    d->createBackend();
}

OsdVideoDecoder::~OsdVideoDecoder() = default;

bool OsdVideoDecoder::multimediaCompiledIn() noexcept
{
#ifdef APM_HAS_QT_MULTIMEDIA
    return true;
#else
    return false;
#endif
}

bool OsdVideoDecoder::backendAvailable() const noexcept
{
    return d->available();
}

OsdVideoDecoder::State OsdVideoDecoder::state() const noexcept
{
    return d->state;
}

QString OsdVideoDecoder::sourceFile() const
{
    return d->sourcePath;
}

int OsdVideoDecoder::sourceFramesPerSecond() const noexcept
{
    return d->framesPerSecond;
}

qint64 OsdVideoDecoder::durationMs() const noexcept
{
    return d->duration;
}

bool OsdVideoDecoder::hasPendingFrame() const noexcept
{
    return d->pendingFrame.isValid();
}

OsdVideoDecoder::Frame OsdVideoDecoder::takeNextFrame()
{
    Frame result = std::move(d->pendingFrame);
    d->pendingFrame = Frame{};
    return result;
}

bool OsdVideoDecoder::start(const QString &localFilePath)
{
    const QFileInfo info(localFilePath);
    QString canonicalPath;
    if (localFilePath.trimmed().isEmpty() || !info.exists()
        || !info.isFile() || !info.isReadable()
        || (canonicalPath = info.canonicalFilePath()).isEmpty()) {
        fail(
            FailureReason::InvalidInput,
            tr("Choose an existing readable local video file."));
        return false;
    }

    if (d->sourcePath == canonicalPath
        && (d->state == State::Loading || d->state == State::Ready
            || d->state == State::Decoding)) {
        return true;
    }

    ++d->sessionGeneration;
    d->destroyBackend();
    d->clearSessionValues();
    d->sourcePath = canonicalPath;
    d->createBackend();
    if (!d->available()) {
        fail(
            FailureReason::BackendUnavailable,
            tr("Qt Multimedia has no available playback backend."));
        return false;
    }

    setState(State::Loading);
    const quint64 generationBeforeSignal = d->sessionGeneration.load();
    d->setSourceAndPlay(canonicalPath);
    return generationBeforeSignal == d->sessionGeneration.load()
        && d->state != State::Failed && d->state != State::Idle;
}

void OsdVideoDecoder::cancel()
{
    if (d->state == State::Idle && d->sourcePath.isEmpty()
        && !d->pendingFrame.isValid()) {
        return;
    }
    ++d->sessionGeneration;
    d->destroyBackend();
    d->clearSessionValues();
    d->createBackend();
    setState(State::Idle);
}

void OsdVideoDecoder::setState(State state)
{
    if (d->state == state) {
        return;
    }
    d->state = state;
    emit stateChanged(state);
}

void OsdVideoDecoder::publishReady()
{
    if (d->readyEmitted || d->state == State::Idle
        || d->state == State::Failed || d->state == State::Ended) {
        return;
    }
    d->updateMetadata();
    d->readyEmitted = true;
    const quint64 generationBeforeSignal = d->sessionGeneration.load();
    setState(State::Ready);
    if (d->sessionGeneration.load() == generationBeforeSignal
        && d->state == State::Ready) {
        emit ready(d->framesPerSecond, d->duration);
    }
}

void OsdVideoDecoder::acceptFrame(const QImage &image, qint64 startTimeUs)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0
        || image.width() > MaximumFrameDimension
        || image.height() > MaximumFrameDimension) {
        fail(
            FailureReason::InvalidFrame,
            tr("The decoded video frame has invalid dimensions."));
        return;
    }

    const quint64 generation = d->sessionGeneration.load();
    publishReady();
    if (generation != d->sessionGeneration.load() || d->state == State::Idle
        || d->state == State::Failed || d->state == State::Ended) {
        return;
    }

    if (d->pendingFrame.isValid()) {
        fail(
            FailureReason::DecodeError,
            tr("The bounded HUD renderer did not consume a source frame in "
               "time. The export stopped without substituting frames."));
        return;
    }
    qint64 frameTimeUsec = startTimeUs >= 0 ? startTimeUs : 0;
#ifdef APM_HAS_QT_MULTIMEDIA
    if (startTimeUs < 0 && d->player) {
        frameTimeUsec = std::max<qint64>(0, d->player->position()) * 1000;
    }
#endif
    Frame newest;
    newest.image = image.copy();
    newest.startTimeUsec = frameTimeUsec;
    newest.sequence = ++d->frameSequence;
    if (newest.image.isNull()) {
        fail(
            FailureReason::InvalidFrame,
            tr("The decoded video frame could not be copied."));
        return;
    }
    d->pendingFrame = std::move(newest);
    setState(State::Decoding);
    if (generation == d->sessionGeneration.load() && d->pendingFrame.isValid()
        && d->state == State::Decoding) {
        emit frameAvailable();
    }
}

void OsdVideoDecoder::fail(
    FailureReason reason, const QString &description)
{
    if (d->state == State::Failed) {
        return;
    }
    ++d->sessionGeneration;
    const quint64 failureGeneration = d->sessionGeneration.load();
    d->clearBackendSource();
    setState(State::Failed);
    if (d->sessionGeneration.load() == failureGeneration
        && d->state == State::Failed) {
        emit failed(
            reason,
            description.trimmed().isEmpty()
                ? tr("The video decoder failed.") : description.trimmed());
    }
}
