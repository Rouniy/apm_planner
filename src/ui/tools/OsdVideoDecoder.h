#ifndef OSDVIDEODECODER_H
#define OSDVIDEODECODER_H

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

/**
 * Bounded, widget-free local-video decoder used by the OSD tool.
 *
 * Decoded images are copied out of the multimedia backend immediately. The
 * adapter exposes one lossless handoff slot: frameAvailable() consumers must
 * synchronously takeNextFrame(). Cross-thread backend delivery is capped; an
 * overrun fails the export instead of silently replacing source frames.
 */
class OsdVideoDecoder final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Loading,
        Ready,
        Decoding,
        Ended,
        Failed
    };
    Q_ENUM(State)

    enum class FailureReason {
        InvalidInput,
        BackendUnavailable,
        UnsupportedMedia,
        DecodeError,
        InvalidFrame
    };
    Q_ENUM(FailureReason)

    struct Frame
    {
        QImage image;
        qint64 startTimeUsec = -1;
        quint64 sequence = 0;

        bool isValid() const noexcept
        {
            return !image.isNull() && startTimeUsec >= 0 && sequence != 0;
        }
    };

    static constexpr int MaximumFrameDimension = 8192;
    static constexpr int DefaultFramesPerSecond = 25;
    static constexpr int MaximumFramesPerSecond = 120;

    explicit OsdVideoDecoder(QObject *parent = nullptr);
    ~OsdVideoDecoder() override;

    static bool multimediaCompiledIn() noexcept;
    bool backendAvailable() const noexcept;

    State state() const noexcept;
    QString sourceFile() const;
    int sourceFramesPerSecond() const noexcept;
    qint64 durationMs() const noexcept;

    bool hasPendingFrame() const noexcept;
    Frame takeNextFrame();

    /**
     * Starts decoding one ordinary local file. Repeating the same active
     * canonical path is a no-op; starting another valid file replaces the
     * preceding decoder session.
     */
    bool start(const QString &localFilePath);
    void cancel();

signals:
    void stateChanged(OsdVideoDecoder::State state);
    void ready(int sourceFramesPerSecond, qint64 durationMs);
    void frameAvailable();
    void ended();
    void failed(OsdVideoDecoder::FailureReason reason,
                const QString &description);

private:
    class Private;
    std::unique_ptr<Private> d;

    void setState(State state);
    void publishReady();
    void acceptFrame(const QImage &image, qint64 startTimeUs);
    void fail(FailureReason reason, const QString &description);
};

Q_DECLARE_METATYPE(OsdVideoDecoder::State)
Q_DECLARE_METATYPE(OsdVideoDecoder::FailureReason)

#endif // OSDVIDEODECODER_H
