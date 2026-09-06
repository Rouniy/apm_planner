#ifndef GEOREFEXIF_H
#define GEOREFEXIF_H

#include <QDateTime>
#include <QString>
#include <functional>
class QIODevice;

// Local, non-publishing JPEG/classic-TIFF metadata service. Image pixels are
// copied without decoding/recompression; unrelated metadata stays intact.
// Content-detected, both TIFF byte orders; BigTIFF is rejected. Bounds: uint32
// source size (Write also reserves 2 MiB append headroom), 16 MiB metadata read
// budget, 256 IFDs/16 levels/65536 entries, JPEG APP1 payload <=65533 bytes.
// This validates the metadata/container, not the image codec or vendor-specific
// MakerNote semantics. Existing inactive EXIF bytes are not privacy-scrubbed.
class GeoRefExif final
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    struct Coordinates { double latitude = 0, longitude = 0, altitude = 0; };
    struct Metadata {
        bool success = false;
        QString error;
        QString format;
        QDateTime photoTime; // EXIF has no zone: host-local interpretation.
        bool hasCoordinates = false;
        Coordinates coordinates;
        QString coordinateWarning; // Invalid old GPS values do not invalidate photoTime.
    };
    struct Result { bool success = false, cancelled = false; QString error; qint64 bytesWritten = 0; };
    static Metadata Inspect(const QString &source, const Cancel &cancel = {});
    // Destination is empty caller-owned private staging, open/writable at position 0
    // (sequential is allowed). Success leaves it at end; caller flushes and
    // publishes. Failure/cancel may leave bytesWritten bytes: caller discards.
    // Source writers must be stopped; metadata/identity checks are not locks.
    static Result Write(const QString &source, QIODevice *destination,
                        const Coordinates &coordinates, const Cancel &cancel = {},
                        const Progress &progress = {});
};

#endif
