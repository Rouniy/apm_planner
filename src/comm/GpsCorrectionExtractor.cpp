#include "GpsCorrectionExtractor.h"

#include "TlogReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <utility>

#include <mavlink.h>

namespace
{
void setReaderCounters(GpsCorrectionExtractor::Result *result,
                       const TlogReader &reader)
{
    result->recordsRead = reader.recordCount();
    result->skippedBytes = reader.skippedBytes();
    result->rejectedFrames = reader.rejectedFrames();
}

bool validatePaths(const QString &inputPath,
                   const QString &outputPath,
                   QString *inputCanonical,
                   QString *outputAbsolute,
                   QString *error)
{
    if (inputPath.trimmed().isEmpty()
        || outputPath.trimmed().isEmpty()) {
        *error = QStringLiteral("Input and output paths are required.");
        return false;
    }

    const QFileInfo inputInfo(inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile()
        || !inputInfo.isReadable()) {
        *error = QStringLiteral("The telemetry log is not a readable regular file.");
        return false;
    }
    const QString canonicalInput = inputInfo.canonicalFilePath();
    if (canonicalInput.isEmpty()) {
        *error = QStringLiteral("The telemetry log path cannot be resolved.");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    // Replacing a symbolic-link directory entry is platform-dependent and
    // obscures which file the operator selected. Reject it even when it does
    // not currently resolve to the input.
    if (outputInfo.isSymLink()) {
        *error = QStringLiteral("The output path must not be a symbolic link.");
        return false;
    }
    if (outputInfo.exists() && !outputInfo.isFile()) {
        *error = QStringLiteral("The output path is not a regular file.");
        return false;
    }

    const QFileInfo parentInfo(outputInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir()) {
        *error = QStringLiteral("The output directory does not exist.");
        return false;
    }
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty()) {
        *error = QStringLiteral("The output directory cannot be resolved.");
        return false;
    }
    const QString canonicalOutput = outputInfo.exists()
        ? outputInfo.canonicalFilePath()
        : QDir(canonicalParent).absoluteFilePath(outputInfo.fileName());
    if (canonicalOutput.isEmpty()) {
        *error = QStringLiteral("The output path cannot be resolved.");
        return false;
    }
    if (QDir::cleanPath(canonicalInput)
        == QDir::cleanPath(canonicalOutput)) {
        *error = QStringLiteral("Input and output must be different files.");
        return false;
    }

    *inputCanonical = canonicalInput;
    *outputAbsolute = canonicalOutput;
    return true;
}

bool writeFully(QSaveFile *output,
                const quint8 *data,
                int size,
                QString *error)
{
    qint64 written = 0;
    while (written < size) {
        const qint64 count = output->write(
            reinterpret_cast<const char *>(data) + written,
            size - written);
        if (count <= 0) {
            *error = QStringLiteral("Writing the correction stream failed: %1")
                         .arg(output->errorString());
            return false;
        }
        written += count;
    }
    return true;
}

bool validKnownPayloadLength(const mavlink_message_t &message,
                             int fullLength,
                             int v1MinimumLength)
{
    if (message.magic == MAVLINK_STX_MAVLINK1) {
        return message.len == v1MinimumLength;
    }
    // MAVLink 2 can omit trailing zero bytes. An empty wire payload is not a
    // structurally usable instance of either fixed correction schema.
    return message.magic == MAVLINK_STX
        && message.len >= 1 && message.len <= fullLength;
}
}

GpsCorrectionExtractor::Result GpsCorrectionExtractor::Extract(
    const QString &inputPath,
    const QString &outputPath,
    CancelCheck cancel,
    Progress progress)
{
    Result result;
    QString inputCanonical;
    QString outputAbsolute;
    if (!validatePaths(inputPath, outputPath,
                       &inputCanonical, &outputAbsolute, &result.error)) {
        return result;
    }
    if (cancel && cancel()) {
        result.cancelled = true;
        return result;
    }

    QFile input(inputCanonical);
    if (!input.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Opening the telemetry log failed: %1")
                           .arg(input.errorString());
        return result;
    }
    QSaveFile output(outputAbsolute);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Opening the correction output failed: %1")
                           .arg(output.errorString());
        return result;
    }

    TlogReader reader(&input);
    reader.setCancelCheck(cancel);
    reader.setProgress(std::move(progress));
    TlogRecord record;
    for (;;) {
        const TlogReader::Status status = reader.next(&record);
        if (status == TlogReader::Status::End
            || status == TlogReader::Status::Truncated) {
            result.truncatedTail = status == TlogReader::Status::Truncated;
            break;
        }
        if (status == TlogReader::Status::Cancelled) {
            setReaderCounters(&result, reader);
            result.cancelled = true;
            output.cancelWriting();
            return result;
        }
        if (status != TlogReader::Status::Ok) {
            setReaderCounters(&result, reader);
            result.error = reader.errorString().isEmpty()
                ? QStringLiteral("Reading the telemetry log failed.")
                : reader.errorString();
            output.cancelWriting();
            return result;
        }

        const quint8 *data = nullptr;
        int length = 0;
        int capacity = 0;
        mavlink_gps_inject_data_t inject{};
        mavlink_gps_rtcm_data_t rtcm{};
        if (record.message.msgid == MAVLINK_MSG_ID_GPS_INJECT_DATA) {
            if (!validKnownPayloadLength(
                    record.message,
                    MAVLINK_MSG_ID_GPS_INJECT_DATA_LEN,
                    MAVLINK_MSG_ID_GPS_INJECT_DATA_MIN_LEN)) {
                setReaderCounters(&result, reader);
                result.error = QStringLiteral(
                    "GPS_INJECT_DATA has an invalid MAVLink payload length of %1 bytes.")
                                   .arg(record.message.len);
                output.cancelWriting();
                return result;
            }
            mavlink_msg_gps_inject_data_decode(&record.message, &inject);
            data = inject.data;
            length = inject.len;
            capacity = MAVLINK_MSG_GPS_INJECT_DATA_FIELD_DATA_LEN;
        } else if (record.message.msgid
                   == MAVLINK_MSG_ID_GPS_RTCM_DATA) {
            if (!validKnownPayloadLength(
                    record.message,
                    MAVLINK_MSG_ID_GPS_RTCM_DATA_LEN,
                    MAVLINK_MSG_ID_GPS_RTCM_DATA_MIN_LEN)) {
                setReaderCounters(&result, reader);
                result.error = QStringLiteral(
                    "GPS_RTCM_DATA has an invalid MAVLink payload length of %1 bytes.")
                                   .arg(record.message.len);
                output.cancelWriting();
                return result;
            }
            mavlink_msg_gps_rtcm_data_decode(&record.message, &rtcm);
            data = rtcm.data;
            length = rtcm.len;
            capacity = MAVLINK_MSG_GPS_RTCM_DATA_FIELD_DATA_LEN;
        } else {
            continue;
        }

        if (length > capacity) {
            setReaderCounters(&result, reader);
            result.error = QStringLiteral(
                "GPS correction message %1 declares %2 bytes; maximum is %3.")
                               .arg(record.message.msgid)
                               .arg(length)
                               .arg(capacity);
            output.cancelWriting();
            return result;
        }
        QString writeError;
        if (length > 0
            && !writeFully(&output, data, length, &writeError)) {
            setReaderCounters(&result, reader);
            result.error = writeError;
            output.cancelWriting();
            return result;
        }
        ++result.messagesWritten;
        result.bytesWritten += length;
    }

    setReaderCounters(&result, reader);
    // This distinct barrier covers cancellation requested by the final
    // progress callback or after the reader observed clean EOF.
    if (cancel && cancel()) {
        result.cancelled = true;
        output.cancelWriting();
        return result;
    }
    if (!output.commit()) {
        result.error = QStringLiteral("Publishing the correction output failed: %1")
                           .arg(output.errorString());
        return result;
    }
    result.success = true;
    return result;
}
