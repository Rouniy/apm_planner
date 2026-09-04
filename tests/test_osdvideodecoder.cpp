#include <QtTest>

#include "ui/tools/MjpegAviWriter.h"
#include "ui/tools/OsdVideoDecoder.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>

namespace
{
constexpr int kFrameWidth = 64;
constexpr int kFrameHeight = 48;
constexpr int kFramesPerSecond = 10;
constexpr int kFrameCount = 6;

void appendU16(QByteArray &bytes, quint16 value)
{
    bytes.append(char(value & 0xffU));
    bytes.append(char((value >> 8U) & 0xffU));
}

void appendU32(QByteArray &bytes, quint32 value)
{
    bytes.append(char(value & 0xffU));
    bytes.append(char((value >> 8U) & 0xffU));
    bytes.append(char((value >> 16U) & 0xffU));
    bytes.append(char((value >> 24U) & 0xffU));
}

void appendS16(QByteArray &bytes, qint16 value)
{
    appendU16(bytes, quint16(value));
}

void appendS32(QByteArray &bytes, qint32 value)
{
    appendU32(bytes, quint32(value));
}

void appendFourCc(QByteArray &bytes, const char (&value)[5])
{
    bytes.append(value, 4);
}

QByteArray makeChunk(const char (&id)[5], const QByteArray &payload)
{
    QByteArray chunk;
    appendFourCc(chunk, id);
    appendU32(chunk, quint32(payload.size()));
    chunk.append(payload);
    if ((payload.size() & 1) != 0) {
        chunk.append('\0');
    }
    return chunk;
}

QByteArray makeList(const char (&type)[5], const QByteArray &contents)
{
    QByteArray payload;
    appendFourCc(payload, type);
    payload.append(contents);
    return makeChunk("LIST", payload);
}

QByteArray makeJpegFrame(int frameIndex, QString *error)
{
    QImage image(kFrameWidth, kFrameHeight, QImage::Format_RGB32);
    image.fill(QColor::fromHsv((frameIndex * 51) % 360, 220, 225));

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not open the in-memory JPEG buffer.");
        return {};
    }
    QImageWriter writer(&buffer, "JPEG");
    writer.setQuality(85);
    if (!writer.write(image)) {
        *error = writer.errorString();
        return {};
    }
    return jpeg;
}

QByteArray makeSyntheticMjpegAvi(QString *error)
{
    QList<QByteArray> frames;
    quint32 maximumFrameSize = 0;
    for (int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex) {
        const QByteArray jpeg = makeJpegFrame(frameIndex, error);
        if (jpeg.isEmpty()) {
            return {};
        }
        maximumFrameSize = std::max(maximumFrameSize, quint32(jpeg.size()));
        frames.append(jpeg);
    }

    QByteArray aviHeader;
    appendU32(aviHeader, 1000000U / kFramesPerSecond);
    appendU32(aviHeader, maximumFrameSize * kFramesPerSecond);
    appendU32(aviHeader, 0);
    appendU32(aviHeader, 0x10U); // AVIF_HASINDEX
    appendU32(aviHeader, kFrameCount);
    appendU32(aviHeader, 0);
    appendU32(aviHeader, 1);
    appendU32(aviHeader, maximumFrameSize);
    appendU32(aviHeader, kFrameWidth);
    appendU32(aviHeader, kFrameHeight);
    appendU32(aviHeader, 0);
    appendU32(aviHeader, 0);
    appendU32(aviHeader, 0);
    appendU32(aviHeader, 0);

    QByteArray streamHeader;
    appendFourCc(streamHeader, "vids");
    appendFourCc(streamHeader, "MJPG");
    appendU32(streamHeader, 0);
    appendU16(streamHeader, 0);
    appendU16(streamHeader, 0);
    appendU32(streamHeader, 0);
    appendU32(streamHeader, 1);
    appendU32(streamHeader, kFramesPerSecond);
    appendU32(streamHeader, 0);
    appendU32(streamHeader, kFrameCount);
    appendU32(streamHeader, maximumFrameSize);
    appendU32(streamHeader, 0xffffffffU);
    appendU32(streamHeader, 0);
    appendS16(streamHeader, 0);
    appendS16(streamHeader, 0);
    appendS16(streamHeader, kFrameWidth);
    appendS16(streamHeader, kFrameHeight);

    QByteArray bitmapHeader;
    appendU32(bitmapHeader, 40);
    appendS32(bitmapHeader, kFrameWidth);
    appendS32(bitmapHeader, kFrameHeight);
    appendU16(bitmapHeader, 1);
    appendU16(bitmapHeader, 24);
    appendFourCc(bitmapHeader, "MJPG");
    appendU32(bitmapHeader, maximumFrameSize);
    appendS32(bitmapHeader, 0);
    appendS32(bitmapHeader, 0);
    appendU32(bitmapHeader, 0);
    appendU32(bitmapHeader, 0);

    QByteArray streamListContents;
    streamListContents.append(makeChunk("strh", streamHeader));
    streamListContents.append(makeChunk("strf", bitmapHeader));

    QByteArray headerListContents;
    headerListContents.append(makeChunk("avih", aviHeader));
    headerListContents.append(makeList("strl", streamListContents));

    QByteArray movieContents;
    QByteArray indexContents;
    quint32 movieOffset = 4; // Relative to the initial "movi" fourcc.
    for (const QByteArray &frame : frames) {
        const QByteArray frameChunk = makeChunk("00dc", frame);
        movieContents.append(frameChunk);

        appendFourCc(indexContents, "00dc");
        appendU32(indexContents, 0x10U); // AVIIF_KEYFRAME
        appendU32(indexContents, movieOffset);
        appendU32(indexContents, quint32(frame.size()));
        movieOffset += quint32(frameChunk.size());
    }

    QByteArray riffContents;
    appendFourCc(riffContents, "AVI ");
    riffContents.append(makeList("hdrl", headerListContents));
    riffContents.append(makeList("movi", movieContents));
    riffContents.append(makeChunk("idx1", indexContents));

    QByteArray result;
    appendFourCc(result, "RIFF");
    appendU32(result, quint32(riffContents.size()));
    result.append(riffContents);
    return result;
}

OsdVideoDecoder::FailureReason failureReason(const QSignalSpy &failedSpy)
{
    return qvariant_cast<OsdVideoDecoder::FailureReason>(
        failedSpy.constFirst().constFirst());
}
}

class OsdVideoDecoderTest : public QObject
{
    Q_OBJECT

private slots:
    void qt5GStreamerSinkOverridesAreRestored();
    void invalidInputAndCancelAreExplicitAndIdempotent();
    void syntheticMjpegAviDeliversEveryFrameThroughBoundedHandoff();
    void productionWriterOutputDecodesThroughQtMultimedia();
};

void OsdVideoDecoderTest::qt5GStreamerSinkOverridesAreRestored()
{
#if defined(APM_HAS_QT_MULTIMEDIA) && defined(Q_OS_LINUX) \
    && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    constexpr const char windowName[] = "QT_GSTREAMER_WINDOW_VIDEOSINK";
    constexpr const char widgetName[] = "QT_GSTREAMER_WIDGET_VIDEOSINK";
    const bool windowWasSet = qEnvironmentVariableIsSet(windowName);
    const bool widgetWasSet = qEnvironmentVariableIsSet(widgetName);
    const QByteArray previousWindow = qgetenv(windowName);
    const QByteArray previousWidget = qgetenv(widgetName);
    qputenv(windowName, "apm-window-sentinel");
    qputenv(widgetName, "apm-widget-sentinel");
    {
        OsdVideoDecoder decoder;
        Q_UNUSED(decoder)
    }
    const bool restored = qgetenv(windowName) == "apm-window-sentinel"
        && qgetenv(widgetName) == "apm-widget-sentinel";
    if (windowWasSet) {
        qputenv(windowName, previousWindow);
    } else {
        qunsetenv(windowName);
    }
    if (widgetWasSet) {
        qputenv(widgetName, previousWidget);
    } else {
        qunsetenv(widgetName);
    }
    QVERIFY(restored);
#endif
}

void OsdVideoDecoderTest::invalidInputAndCancelAreExplicitAndIdempotent()
{
    OsdVideoDecoder decoder;
    QSignalSpy failedSpy(&decoder, &OsdVideoDecoder::failed);
    QSignalSpy stateSpy(&decoder, &OsdVideoDecoder::stateChanged);

    QVERIFY(!decoder.start(QString()));
    QCOMPARE(decoder.state(), OsdVideoDecoder::State::Failed);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(
        failureReason(failedSpy),
        OsdVideoDecoder::FailureReason::InvalidInput);
    QVERIFY(!failedSpy.constFirst().at(1).toString().trimmed().isEmpty());

    decoder.cancel();
    QCOMPARE(decoder.state(), OsdVideoDecoder::State::Idle);
    QVERIFY(decoder.sourceFile().isEmpty());
    QCOMPARE(
        decoder.sourceFramesPerSecond(),
        OsdVideoDecoder::DefaultFramesPerSecond);
    QCOMPARE(decoder.durationMs(), qint64(0));
    QVERIFY(!decoder.hasPendingFrame());

    const int stateChangesAfterFirstCancel = stateSpy.count();
    decoder.cancel();
    QCOMPARE(stateSpy.count(), stateChangesAfterFirstCancel);
    QCOMPARE(decoder.state(), OsdVideoDecoder::State::Idle);
}

void OsdVideoDecoderTest::
syntheticMjpegAviDeliversEveryFrameThroughBoundedHandoff()
{
    if (!OsdVideoDecoder::multimediaCompiledIn()) {
        QSKIP("Qt Multimedia support was not compiled into this target.");
    }

    OsdVideoDecoder decoder;
    if (!decoder.backendAvailable()) {
        QSKIP("The Qt Multimedia runtime playback backend is unavailable.");
    }

    const QList<QByteArray> imageFormats =
        QImageWriter::supportedImageFormats();
    if (!imageFormats.contains("jpeg") && !imageFormats.contains("jpg")) {
        QSKIP("The runtime JPEG image writer plugin is unavailable.");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString generationError;
    const QByteArray avi = makeSyntheticMjpegAvi(&generationError);
    QVERIFY2(!avi.isEmpty(), qPrintable(generationError));
    QCOMPARE(avi.left(4), QByteArray("RIFF"));
    QCOMPARE(avi.mid(8, 4), QByteArray("AVI "));

    const QString videoPath = directory.filePath(QStringLiteral("test.avi"));
    QFile videoFile(videoPath);
    QVERIFY(videoFile.open(QIODevice::WriteOnly));
    QCOMPARE(videoFile.write(avi), qint64(avi.size()));
    videoFile.close();

    QSignalSpy readySpy(&decoder, &OsdVideoDecoder::ready);
    QSignalSpy frameSpy(&decoder, &OsdVideoDecoder::frameAvailable);
    QSignalSpy endedSpy(&decoder, &OsdVideoDecoder::ended);
    QSignalSpy failedSpy(&decoder, &OsdVideoDecoder::failed);
    QVector<OsdVideoDecoder::Frame> decodedFrames;
    connect(&decoder, &OsdVideoDecoder::frameAvailable, &decoder,
            [&decoder, &decodedFrames]() {
        const OsdVideoDecoder::Frame frame = decoder.takeNextFrame();
        if (frame.isValid()) {
            decodedFrames.append(frame);
        }
    });

    const bool started = decoder.start(videoPath);
    if (!started && !failedSpy.isEmpty()
        && failureReason(failedSpy)
            == OsdVideoDecoder::FailureReason::BackendUnavailable) {
        QSKIP("The Qt Multimedia runtime playback backend is unavailable.");
    }
    QVERIFY(started);
    QVERIFY(decoder.start(videoPath)); // Same active source is a no-op.

    QTRY_VERIFY_WITH_TIMEOUT(
        !endedSpy.isEmpty() || !failedSpy.isEmpty(), 15000);
    if (!failedSpy.isEmpty()) {
        const OsdVideoDecoder::FailureReason reason =
            failureReason(failedSpy);
        if (reason == OsdVideoDecoder::FailureReason::BackendUnavailable) {
            QSKIP("The Qt Multimedia runtime playback backend is unavailable.");
        }
        if (reason == OsdVideoDecoder::FailureReason::UnsupportedMedia) {
            QSKIP("The Qt Multimedia runtime has no AVI/MJPEG decoder plugin.");
        }
        const QString description = failedSpy.constFirst().at(1).toString();
        QFAIL(qPrintable(QStringLiteral("Unexpected decode failure: %1")
                             .arg(description)));
    }

    QCOMPARE(endedSpy.count(), 1);
    QCOMPARE(decoder.state(), OsdVideoDecoder::State::Ended);
    QCOMPARE(readySpy.count(), 1);
    // QMediaPlayer backends may present a preroll frame more than once, but
    // they must not omit any of the source frames.
    QVERIFY(frameSpy.count() >= kFrameCount);
    QCOMPARE(
        decoder.sourceFile(), QFileInfo(videoPath).canonicalFilePath());
    QVERIFY(
        decoder.sourceFramesPerSecond() == kFramesPerSecond
        || decoder.sourceFramesPerSecond()
            == OsdVideoDecoder::DefaultFramesPerSecond);
    QVERIFY(decoder.durationMs() > 0);
    QCOMPARE(decodedFrames.size(), frameSpy.count());
    for (int index = 0; index < decodedFrames.size(); ++index) {
        const OsdVideoDecoder::Frame &frame = decodedFrames.at(index);
        QCOMPARE(frame.image.size(), QSize(kFrameWidth, kFrameHeight));
        QVERIFY(frame.startTimeUsec >= 0);
        QCOMPARE(frame.sequence, quint64(index + 1));
        if (index > 0) {
            QVERIFY(frame.startTimeUsec
                    >= decodedFrames.at(index - 1).startTimeUsec);
        }
    }
    QVERIFY(!decoder.hasPendingFrame());
    QVERIFY(!decoder.takeNextFrame().isValid());

    decoder.cancel();
    decoder.cancel();
    QCOMPARE(decoder.state(), OsdVideoDecoder::State::Idle);
    QVERIFY(decoder.sourceFile().isEmpty());
}

void OsdVideoDecoderTest::productionWriterOutputDecodesThroughQtMultimedia()
{
    if (!OsdVideoDecoder::multimediaCompiledIn()) {
        QSKIP("Qt Multimedia support was not compiled into this target.");
    }
    OsdVideoDecoder decoder;
    if (!decoder.backendAvailable()) {
        QSKIP("The Qt Multimedia runtime playback backend is unavailable.");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("writer-output.avi"));
    MjpegAviWriter writer(path, kFrameWidth, kFrameHeight, kFramesPerSecond);
    QVERIFY2(writer.isOpen(), qPrintable(writer.errorString()));
    for (int index = 0; index < kFrameCount; ++index) {
        QString error;
        const QByteArray jpeg = makeJpegFrame(index, &error);
        QVERIFY2(!jpeg.isEmpty(), qPrintable(error));
        QVERIFY2(writer.writeJpeg(jpeg), qPrintable(writer.errorString()));
    }
    QVERIFY2(writer.finalize(), qPrintable(writer.errorString()));

    QVector<OsdVideoDecoder::Frame> frames;
    connect(&decoder, &OsdVideoDecoder::frameAvailable, &decoder,
            [&decoder, &frames]() {
        const OsdVideoDecoder::Frame frame = decoder.takeNextFrame();
        if (frame.isValid()) {
            frames.append(frame);
        }
    });
    QSignalSpy endedSpy(&decoder, &OsdVideoDecoder::ended);
    QSignalSpy failedSpy(&decoder, &OsdVideoDecoder::failed);
    QVERIFY(decoder.start(path));
    QTRY_VERIFY_WITH_TIMEOUT(
        !endedSpy.isEmpty() || !failedSpy.isEmpty(), 15000);
    if (!failedSpy.isEmpty()) {
        const OsdVideoDecoder::FailureReason reason = failureReason(failedSpy);
        if (reason == OsdVideoDecoder::FailureReason::UnsupportedMedia) {
            QSKIP("The Qt Multimedia runtime has no AVI/MJPEG decoder plugin.");
        }
        QFAIL(qPrintable(failedSpy.constFirst().at(1).toString()));
    }
    QCOMPARE(endedSpy.count(), 1);
    QVERIFY(frames.size() >= kFrameCount);
    for (const OsdVideoDecoder::Frame &frame : frames) {
        QCOMPARE(frame.image.size(), QSize(kFrameWidth, kFrameHeight));
    }
}

QTEST_GUILESS_MAIN(OsdVideoDecoderTest)

#include "test_osdvideodecoder.moc"
