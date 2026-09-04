#include "ExternalGuidedFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStringList>

#include <cmath>

ExternalGuidedFileResult ExternalGuidedFile::parse(const QString &text)
{
    if (text.size() > MaximumCharacters) {
        return failure(
            ExternalGuidedFileError::TooManyCharacters,
            QStringLiteral("target file exceeds %1 characters.")
                .arg(MaximumCharacters));
    }

    const QStringList fields = text.trimmed().split(
        QLatin1Char(','), Qt::KeepEmptyParts);
    if (fields.size() != 3) {
        return failure(
            ExternalGuidedFileError::InvalidFieldCount,
            QStringLiteral("expected exactly three invariant numbers: "
                           "latitude,longitude,relative-altitude-m."));
    }

    const QLocale invariant = QLocale::c();
    bool latitudeOk = false;
    bool longitudeOk = false;
    bool altitudeOk = false;
    const double latitude = invariant.toDouble(
        fields.at(0).trimmed(), &latitudeOk);
    const double longitude = invariant.toDouble(
        fields.at(1).trimmed(), &longitudeOk);
    const double altitude = invariant.toDouble(
        fields.at(2).trimmed(), &altitudeOk);
    if (!latitudeOk || !longitudeOk || !altitudeOk
        || !std::isfinite(latitude) || !std::isfinite(longitude)
        || !std::isfinite(altitude)) {
        return failure(
            ExternalGuidedFileError::InvalidNumber,
            QStringLiteral("expected exactly three invariant numbers: "
                           "latitude,longitude,relative-altitude-m."));
    }
    if (latitude < -90.0 || latitude > 90.0) {
        return failure(
            ExternalGuidedFileError::LatitudeOutOfRange,
            QStringLiteral("latitude must be between -90 and 90 degrees."));
    }
    if (longitude < -180.0 || longitude > 180.0) {
        return failure(
            ExternalGuidedFileError::LongitudeOutOfRange,
            QStringLiteral("longitude must be between -180 and 180 degrees."));
    }
    if (altitude <= 0.0 || altitude > 10000.0) {
        return failure(
            ExternalGuidedFileError::AltitudeOutOfRange,
            QStringLiteral("relative altitude must be greater than zero and "
                           "no more than 10000 metres."));
    }

    ExternalGuidedFileResult result;
    result.waypoint.latitude = latitude;
    result.waypoint.longitude = longitude;
    result.waypoint.relativeAltitudeM = altitude;
    result.accepted = true;
    return result;
}

ExternalGuidedFileResult ExternalGuidedFile::read(const QString &path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return failure(ExternalGuidedFileError::EmptyPath,
                       QStringLiteral("target file path is empty."));
    }

    const QString resolvedPath = absolutePath(trimmedPath);
    const QFileInfo information(resolvedPath);
    if (!information.exists() || !information.isFile()) {
        return failure(ExternalGuidedFileError::MissingFile,
                       QStringLiteral("target file does not exist."),
                       resolvedPath);
    }

    QFile file(resolvedPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(
            ExternalGuidedFileError::OpenFailed,
            QStringLiteral("target file cannot be opened: %1")
                .arg(file.errorString()),
            resolvedPath);
    }
    if (file.size() > MaximumFileBytes) {
        return failure(
            ExternalGuidedFileError::TooManyBytes,
            QStringLiteral("target file exceeds %1 bytes.")
                .arg(MaximumFileBytes),
            resolvedPath);
    }

    const QByteArray bytes = file.read(MaximumFileBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return failure(
            ExternalGuidedFileError::ReadFailed,
            QStringLiteral("target file cannot be read: %1")
                .arg(file.errorString()),
            resolvedPath);
    }
    if (bytes.size() > MaximumFileBytes || !file.atEnd()) {
        return failure(
            ExternalGuidedFileError::TooManyBytes,
            QStringLiteral("target file exceeds %1 bytes.")
                .arg(MaximumFileBytes),
            resolvedPath);
    }

    QString decoded = QString::fromUtf8(bytes);
    if (!decoded.isEmpty() && decoded.front() == QChar(0xfeff)) {
        decoded.remove(0, 1);
    }
    ExternalGuidedFileResult result = parse(decoded);
    result.absolutePath = resolvedPath;
    return result;
}

QString ExternalGuidedFile::absolutePath(const QString &path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(QFileInfo(trimmedPath).absoluteFilePath());
}

ExternalGuidedFileResult ExternalGuidedFile::failure(
    ExternalGuidedFileError errorCode, const QString &error,
    const QString &absolutePath)
{
    ExternalGuidedFileResult result;
    result.absolutePath = absolutePath;
    result.errorCode = errorCode;
    result.error = error;
    return result;
}
