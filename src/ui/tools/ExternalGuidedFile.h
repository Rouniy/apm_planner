#ifndef EXTERNALGUIDEDFILE_H
#define EXTERNALGUIDEDFILE_H

#include <QString>
#include <QMetaType>
#include <QtGlobal>

struct ExternalGuidedWaypoint
{
    double latitude = 0.0;
    double longitude = 0.0;
    double relativeAltitudeM = 0.0;
};

enum class ExternalGuidedFileError
{
    None,
    EmptyPath,
    MissingFile,
    OpenFailed,
    ReadFailed,
    TooManyBytes,
    TooManyCharacters,
    InvalidFieldCount,
    InvalidNumber,
    LatitudeOutOfRange,
    LongitudeOutOfRange,
    AltitudeOutOfRange
};

Q_DECLARE_METATYPE(ExternalGuidedFileError)

struct ExternalGuidedFileResult
{
    ExternalGuidedWaypoint waypoint;
    QString absolutePath;
    ExternalGuidedFileError errorCode = ExternalGuidedFileError::None;
    QString error;
    bool accepted = false;

    bool isValid() const noexcept
    {
        return accepted;
    }
};

/**
 * Pure parser and bounded file reader for Mission Planner's External Guided
 * latitude,longitude,relative-altitude-m interchange format.
 *
 * QString::size() counts UTF-16 code units, matching the decoded-character
 * bound applied by the MP10 reference. File bytes are bounded independently
 * before decoding so a concurrently growing producer cannot cause an
 * unbounded allocation.
 */
class ExternalGuidedFile final
{
public:
    static constexpr qint64 MaximumFileBytes = 4096;
    static constexpr int MaximumCharacters = 4096;

    static ExternalGuidedFileResult parse(const QString &text);
    static ExternalGuidedFileResult read(const QString &path);
    static QString absolutePath(const QString &path);

private:
    static ExternalGuidedFileResult failure(
        ExternalGuidedFileError errorCode, const QString &error,
        const QString &absolutePath = QString());
};

#endif // EXTERNALGUIDEDFILE_H
