#ifndef SWARMSEQUENCECORE_H
#define SWARMSEQUENCECORE_H

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

/** A local formation slot. X is east, Y is north and Z is altitude, in metres. */
struct SwarmSequenceOffset
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SwarmSequenceLayout
{
    QString id;
    int delayStart = 0;
    int delayEnd = 0;
    QMap<int, SwarmSequenceOffset> offsets;
};

/**
 * Offline Mission Planner Sequence Layout Editor document.
 *
 * DelayStart and DelayEnd are deliberately retained even though the MP10
 * runner, like the original WinForms runner, does not consume them while
 * advancing a step.
 */
struct SwarmSequenceDocument
{
    QVector<SwarmSequenceLayout> layouts;
    QStringList steps;
};

enum class SwarmSequenceError
{
    None,
    EmptyPath,
    FileNotFound,
    OpenFailed,
    ReadFailed,
    TooManyBytes,
    InvalidJson,
    InvalidRoot,
    AmbiguousMember,
    InvalidMemberType,
    TooManyLayouts,
    TooManySteps,
    EmptyLayoutId,
    DuplicateLayoutId,
    TooManyOffsets,
    InvalidSystemId,
    UnsafeOffset,
    InconsistentSlots,
    MissingStepLayout,
    InvalidIndex,
    MissingLayout,
    ReferencedLayout,
    InvalidSlotCount,
    SaveFailed,
    InvalidCoordinate
};

Q_DECLARE_METATYPE(SwarmSequenceError)

struct SwarmSequenceIssue
{
    SwarmSequenceError code = SwarmSequenceError::None;
    QString message;

    bool isOk() const noexcept { return code == SwarmSequenceError::None; }
    explicit operator bool() const noexcept { return isOk(); }
};

struct SwarmSequenceLoadResult
{
    SwarmSequenceIssue issue;
    SwarmSequenceDocument document;
    QString absolutePath;

    bool isValid() const noexcept { return issue.isOk(); }
};

struct SwarmSequenceSerializeResult
{
    SwarmSequenceIssue issue;
    QByteArray json;

    bool isValid() const noexcept { return issue.isOk(); }
};

/** Bounded, permissive-reader/canonical-writer MP10 Sequence JSON codec. */
class SwarmSequenceFile final
{
public:
    static constexpr qint64 MaximumFileBytes = 8 * 1024 * 1024;
    static constexpr int MaximumLayouts = 1000;
    static constexpr int MaximumSteps = 100000;
    static constexpr int MaximumOffsets = 255;
    static constexpr double MaximumHorizontalOffsetM = 100000.0;
    static constexpr double MaximumVerticalOffsetM = 10000.0;

    /**
     * Reads member names case-insensitively and accepts JSON comments and
     * trailing commas. Layout ids and step references remain exact-case.
     */
    static SwarmSequenceLoadResult parse(const QByteArray &json);
    static SwarmSequenceLoadResult load(const QString &path);

    /** Writes canonical PascalCase members and lowercase x/y/z members. */
    static SwarmSequenceSerializeResult serialize(
        const SwarmSequenceDocument &document);

    /** Uses QSaveFile without direct-write fallback for an atomic replacement. */
    static SwarmSequenceIssue save(
        const QString &path, const SwarmSequenceDocument &document);

    static SwarmSequenceIssue validate(
        const SwarmSequenceDocument &document);
};

/**
 * Transactional document editor for a Qt Widgets view. Every failed operation
 * leaves the current document unchanged.
 */
class SwarmSequenceEditor final
{
public:
    SwarmSequenceEditor() = default;

    const SwarmSequenceDocument &document() const noexcept { return m_document; }
    SwarmSequenceIssue replaceDocument(const SwarmSequenceDocument &document);

    /** Creates a zeroed layout with the common slots, or slot 1 for a new file. */
    SwarmSequenceIssue createLayout(const QString &id);
    SwarmSequenceIssue cloneLayout(const QString &sourceId,
                                   const QString &newId);
    SwarmSequenceIssue renameLayout(const QString &oldId,
                                    const QString &newId);
    SwarmSequenceIssue removeLayout(const QString &id);
    SwarmSequenceIssue setLayoutDelays(const QString &id,
                                       int delayStart, int delayEnd);
    SwarmSequenceIssue setOffset(const QString &layoutId, int systemId,
                                 const SwarmSequenceOffset &offset);

    /** Applies the same deterministic slot set to every layout. */
    SwarmSequenceIssue resizeSlots(int desiredCount);

    SwarmSequenceIssue addStep(const QString &layoutId, int beforeIndex = -1);
    SwarmSequenceIssue replaceStep(int index, const QString &layoutId);
    SwarmSequenceIssue removeStep(int index);
    SwarmSequenceIssue moveStep(int fromIndex, int toIndex);

private:
    SwarmSequenceDocument m_document;
};

struct SwarmSequenceGeodeticPoint
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeM = 0.0;
};

/** MP10 spherical direct projection of a local east/north offset. */
class SwarmSequenceGeometry final
{
public:
    static SwarmSequenceIssue projectEastNorth(
        double originLatitude, double originLongitude,
        const SwarmSequenceOffset &offset,
        SwarmSequenceGeodeticPoint *projected);
};

#endif // SWARMSEQUENCECORE_H
