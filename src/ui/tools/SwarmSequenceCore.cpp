#include "SwarmSequenceCore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>

// Out-of-class definitions keep the header usable by C++14 targets as well as
// the project's current C++17 build.
constexpr qint64 SwarmSequenceFile::MaximumFileBytes;
constexpr int SwarmSequenceFile::MaximumLayouts;
constexpr int SwarmSequenceFile::MaximumSteps;
constexpr int SwarmSequenceFile::MaximumOffsets;
constexpr double SwarmSequenceFile::MaximumHorizontalOffsetM;
constexpr double SwarmSequenceFile::MaximumVerticalOffsetM;

namespace
{
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;

SwarmSequenceIssue issue(SwarmSequenceError code, const QString &message)
{
    SwarmSequenceIssue result;
    result.code = code;
    result.message = message;
    return result;
}

SwarmSequenceIssue ok()
{
    return SwarmSequenceIssue();
}

QString normalizedId(const QString &id)
{
    return id.trimmed();
}

SwarmSequenceDocument normalizedDocument(const SwarmSequenceDocument &input)
{
    SwarmSequenceDocument output = input;
    for (int layoutIndex = 0; layoutIndex < output.layouts.size(); ++layoutIndex) {
        output.layouts[layoutIndex].id = normalizedId(output.layouts.at(layoutIndex).id);
    }
    for (int stepIndex = 0; stepIndex < output.steps.size(); ++stepIndex) {
        output.steps[stepIndex] = normalizedId(output.steps.at(stepIndex));
    }
    return output;
}

bool safeOffset(const SwarmSequenceOffset &offset)
{
    return std::isfinite(offset.x) && std::isfinite(offset.y)
        && std::isfinite(offset.z)
        && std::abs(offset.x) <= SwarmSequenceFile::MaximumHorizontalOffsetM
        && std::abs(offset.y) <= SwarmSequenceFile::MaximumHorizontalOffsetM
        && std::abs(offset.z) <= SwarmSequenceFile::MaximumVerticalOffsetM;
}

int exactLayoutIndex(const SwarmSequenceDocument &document, const QString &id)
{
    for (int index = 0; index < document.layouts.size(); ++index) {
        if (document.layouts.at(index).id == id) {
            return index;
        }
    }
    return -1;
}

/** Replaces comments with whitespace so tokens cannot accidentally join. */
SwarmSequenceIssue removeJsonComments(const QByteArray &input, QByteArray *output)
{
    output->clear();
    output->reserve(input.size());
    bool inString = false;
    bool escaped = false;
    for (int index = 0; index < input.size(); ++index) {
        const char current = input.at(index);
        if (inString) {
            output->append(current);
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                inString = false;
            }
            continue;
        }

        if (current == '"') {
            inString = true;
            output->append(current);
            continue;
        }
        if (current != '/' || index + 1 >= input.size()) {
            output->append(current);
            continue;
        }

        const char next = input.at(index + 1);
        if (next == '/') {
            output->append("  ");
            index += 2;
            while (index < input.size() && input.at(index) != '\n'
                   && input.at(index) != '\r') {
                output->append(' ');
                ++index;
            }
            if (index < input.size()) {
                output->append(input.at(index));
            }
            continue;
        }
        if (next == '*') {
            output->append("  ");
            index += 2;
            bool closed = false;
            while (index < input.size()) {
                if (input.at(index) == '*' && index + 1 < input.size()
                    && input.at(index + 1) == '/') {
                    output->append("  ");
                    ++index;
                    closed = true;
                    break;
                }
                const char commentCharacter = input.at(index);
                output->append(commentCharacter == '\n' || commentCharacter == '\r'
                                   ? commentCharacter : ' ');
                ++index;
            }
            if (!closed) {
                return issue(SwarmSequenceError::InvalidJson,
                             QStringLiteral("JSON contains an unterminated block comment."));
            }
            continue;
        }
        output->append(current);
    }
    return ok();
}

void removeTrailingCommas(QByteArray *json)
{
    bool inString = false;
    bool escaped = false;
    for (int index = 0; index < json->size(); ++index) {
        const char current = json->at(index);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                inString = false;
            }
            continue;
        }
        if (current == '"') {
            inString = true;
            continue;
        }
        if (current != ',') {
            continue;
        }
        int next = index + 1;
        while (next < json->size()) {
            const char candidate = json->at(next);
            if (candidate != ' ' && candidate != '\t'
                && candidate != '\r' && candidate != '\n') {
                break;
            }
            ++next;
        }
        if (next < json->size()
            && (json->at(next) == '}' || json->at(next) == ']')) {
            (*json)[index] = ' ';
        }
    }
}

SwarmSequenceIssue memberCaseInsensitive(
    const QJsonObject &object, const QString &name, QJsonValue *value,
    bool *present)
{
    *present = false;
    for (QJsonObject::const_iterator iterator = object.constBegin();
         iterator != object.constEnd(); ++iterator) {
        if (iterator.key().compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (*present) {
            return issue(
                SwarmSequenceError::AmbiguousMember,
                QStringLiteral("JSON object contains more than one case-insensitive '%1' member.")
                    .arg(name));
        }
        *present = true;
        *value = iterator.value();
    }
    return ok();
}

SwarmSequenceIssue optionalArrayMember(
    const QJsonObject &object, const QString &name, QJsonArray *array)
{
    QJsonValue value;
    bool present = false;
    const SwarmSequenceIssue lookup = memberCaseInsensitive(
        object, name, &value, &present);
    if (!lookup) {
        return lookup;
    }
    if (!present || value.isNull() || value.isUndefined()) {
        *array = QJsonArray();
        return ok();
    }
    if (!value.isArray()) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' must be an array.").arg(name));
    }
    *array = value.toArray();
    return ok();
}

SwarmSequenceIssue optionalObjectMember(
    const QJsonObject &object, const QString &name, QJsonObject *member)
{
    QJsonValue value;
    bool present = false;
    const SwarmSequenceIssue lookup = memberCaseInsensitive(
        object, name, &value, &present);
    if (!lookup) {
        return lookup;
    }
    if (!present || value.isNull() || value.isUndefined()) {
        *member = QJsonObject();
        return ok();
    }
    if (!value.isObject()) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' must be an object.").arg(name));
    }
    *member = value.toObject();
    return ok();
}

SwarmSequenceIssue optionalStringMember(
    const QJsonObject &object, const QString &name, QString *text)
{
    QJsonValue value;
    bool present = false;
    const SwarmSequenceIssue lookup = memberCaseInsensitive(
        object, name, &value, &present);
    if (!lookup) {
        return lookup;
    }
    if (!present) {
        text->clear();
        return ok();
    }
    if (!value.isString()) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' must be a string.").arg(name));
    }
    *text = value.toString();
    return ok();
}

SwarmSequenceIssue optionalIntegerMember(
    const QJsonObject &object, const QString &name, int *number)
{
    QJsonValue value;
    bool present = false;
    const SwarmSequenceIssue lookup = memberCaseInsensitive(
        object, name, &value, &present);
    if (!lookup) {
        return lookup;
    }
    if (!present) {
        *number = 0;
        return ok();
    }
    if (!value.isDouble()) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' must be an integer.").arg(name));
    }
    const double candidate = value.toDouble();
    if (!std::isfinite(candidate) || std::floor(candidate) != candidate
        || candidate < double(std::numeric_limits<int>::min())
        || candidate > double(std::numeric_limits<int>::max())) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' is outside the integer range.").arg(name));
    }
    *number = int(candidate);
    return ok();
}

SwarmSequenceIssue optionalDoubleMember(
    const QJsonObject &object, const QString &name, double *number)
{
    QJsonValue value;
    bool present = false;
    const SwarmSequenceIssue lookup = memberCaseInsensitive(
        object, name, &value, &present);
    if (!lookup) {
        return lookup;
    }
    if (!present) {
        *number = 0.0;
        return ok();
    }
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        return issue(SwarmSequenceError::InvalidMemberType,
                     QStringLiteral("JSON member '%1' must be a finite number.").arg(name));
    }
    *number = value.toDouble();
    return ok();
}

SwarmSequenceIssue decodeDocument(
    const QJsonObject &root, SwarmSequenceDocument *document)
{
    QJsonArray layouts;
    SwarmSequenceIssue result = optionalArrayMember(root, QStringLiteral("Layouts"), &layouts);
    if (!result) {
        return result;
    }
    if (layouts.size() > SwarmSequenceFile::MaximumLayouts) {
        return issue(SwarmSequenceError::TooManyLayouts,
                     QStringLiteral("Sequence contains more than %1 layouts.")
                         .arg(SwarmSequenceFile::MaximumLayouts));
    }

    SwarmSequenceDocument candidate;
    candidate.layouts.reserve(layouts.size());
    for (int layoutIndex = 0; layoutIndex < layouts.size(); ++layoutIndex) {
        if (!layouts.at(layoutIndex).isObject()) {
            return issue(SwarmSequenceError::InvalidMemberType,
                         QStringLiteral("Layout %1 must be an object.").arg(layoutIndex));
        }
        const QJsonObject layoutObject = layouts.at(layoutIndex).toObject();
        SwarmSequenceLayout layout;
        result = optionalStringMember(layoutObject, QStringLiteral("Id"), &layout.id);
        if (!result) {
            return result;
        }
        result = optionalIntegerMember(
            layoutObject, QStringLiteral("DelayStart"), &layout.delayStart);
        if (!result) {
            return result;
        }
        result = optionalIntegerMember(
            layoutObject, QStringLiteral("DelayEnd"), &layout.delayEnd);
        if (!result) {
            return result;
        }

        QJsonObject offsets;
        result = optionalObjectMember(
            layoutObject, QStringLiteral("Offset"), &offsets);
        if (!result) {
            return result;
        }
        if (offsets.size() > SwarmSequenceFile::MaximumOffsets) {
            return issue(SwarmSequenceError::TooManyOffsets,
                         QStringLiteral("Layout '%1' contains more than %2 offsets.")
                             .arg(layout.id).arg(SwarmSequenceFile::MaximumOffsets));
        }
        for (QJsonObject::const_iterator iterator = offsets.constBegin();
             iterator != offsets.constEnd(); ++iterator) {
            bool systemIdOk = false;
            const int systemId = iterator.key().toInt(&systemIdOk, 10);
            if (!systemIdOk || systemId < 1 || systemId > 255) {
                return issue(SwarmSequenceError::InvalidSystemId,
                             QStringLiteral("Layout '%1' has invalid MAVLink system id '%2'.")
                                 .arg(layout.id, iterator.key()));
            }
            if (layout.offsets.contains(systemId)) {
                return issue(SwarmSequenceError::InvalidSystemId,
                             QStringLiteral("Layout '%1' contains duplicate numeric system id %2.")
                                 .arg(layout.id).arg(systemId));
            }
            if (!iterator.value().isObject()) {
                return issue(SwarmSequenceError::InvalidMemberType,
                             QStringLiteral("Offset for system %1 must be an object.")
                                 .arg(systemId));
            }
            const QJsonObject offsetObject = iterator.value().toObject();
            SwarmSequenceOffset offset;
            result = optionalDoubleMember(offsetObject, QStringLiteral("x"), &offset.x);
            if (!result) {
                return result;
            }
            result = optionalDoubleMember(offsetObject, QStringLiteral("y"), &offset.y);
            if (!result) {
                return result;
            }
            result = optionalDoubleMember(offsetObject, QStringLiteral("z"), &offset.z);
            if (!result) {
                return result;
            }
            layout.offsets.insert(systemId, offset);
        }
        candidate.layouts.append(layout);
    }

    QJsonArray steps;
    result = optionalArrayMember(root, QStringLiteral("Steps"), &steps);
    if (!result) {
        return result;
    }
    if (steps.size() > SwarmSequenceFile::MaximumSteps) {
        return issue(SwarmSequenceError::TooManySteps,
                     QStringLiteral("Sequence contains more than %1 steps.")
                         .arg(SwarmSequenceFile::MaximumSteps));
    }
    candidate.steps.reserve(steps.size());
    for (int stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
        if (!steps.at(stepIndex).isString()) {
            return issue(SwarmSequenceError::InvalidMemberType,
                         QStringLiteral("Step %1 must be a layout id string.").arg(stepIndex));
        }
        candidate.steps.append(steps.at(stepIndex).toString());
    }

    candidate = normalizedDocument(candidate);
    result = SwarmSequenceFile::validate(candidate);
    if (!result) {
        return result;
    }
    *document = candidate;
    return ok();
}

QJsonObject encodeOffset(const SwarmSequenceOffset &offset)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), offset.x);
    object.insert(QStringLiteral("y"), offset.y);
    object.insert(QStringLiteral("z"), offset.z);
    return object;
}

SwarmSequenceIssue commitCandidate(
    const SwarmSequenceDocument &candidate, SwarmSequenceDocument *destination)
{
    const SwarmSequenceIssue validation = SwarmSequenceFile::validate(candidate);
    if (!validation) {
        return validation;
    }
    *destination = normalizedDocument(candidate);
    return ok();
}
}

SwarmSequenceLoadResult SwarmSequenceFile::parse(const QByteArray &input)
{
    SwarmSequenceLoadResult result;
    if (input.size() > MaximumFileBytes) {
        result.issue = issue(
            SwarmSequenceError::TooManyBytes,
            QStringLiteral("Sequence file exceeds the 8 MiB safety limit."));
        return result;
    }

    QByteArray json = input;
    if (json.startsWith("\xef\xbb\xbf")) {
        json.remove(0, 3);
    }
    QByteArray withoutComments;
    result.issue = removeJsonComments(json, &withoutComments);
    if (!result.issue) {
        return result;
    }
    removeTrailingCommas(&withoutComments);

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(withoutComments, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.issue = issue(
            SwarmSequenceError::InvalidJson,
            QStringLiteral("Sequence JSON is invalid at byte %1: %2")
                .arg(parseError.offset).arg(parseError.errorString()));
        return result;
    }
    if (parsed.isNull() && withoutComments.trimmed() == QByteArray("null")) {
        result.issue = ok();
        return result;
    }
    if (!parsed.isObject()) {
        result.issue = issue(SwarmSequenceError::InvalidRoot,
                             QStringLiteral("Sequence JSON root must be an object."));
        return result;
    }

    result.issue = decodeDocument(parsed.object(), &result.document);
    return result;
}

SwarmSequenceLoadResult SwarmSequenceFile::load(const QString &path)
{
    SwarmSequenceLoadResult result;
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        result.issue = issue(SwarmSequenceError::EmptyPath,
                             QStringLiteral("Sequence file path is empty."));
        return result;
    }

    result.absolutePath = QDir::cleanPath(QFileInfo(trimmedPath).absoluteFilePath());
    const QFileInfo information(result.absolutePath);
    if (!information.exists() || !information.isFile()) {
        result.issue = issue(SwarmSequenceError::FileNotFound,
                             QStringLiteral("Sequence file was not found."));
        return result;
    }
    if (information.size() > MaximumFileBytes) {
        result.issue = issue(
            SwarmSequenceError::TooManyBytes,
            QStringLiteral("Sequence file exceeds the 8 MiB safety limit."));
        return result;
    }

    QFile file(result.absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.issue = issue(
            SwarmSequenceError::OpenFailed,
            QStringLiteral("Sequence file cannot be opened: %1")
                .arg(file.errorString()));
        return result;
    }
    const QByteArray bytes = file.read(MaximumFileBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        result.issue = issue(
            SwarmSequenceError::ReadFailed,
            QStringLiteral("Sequence file cannot be read: %1")
                .arg(file.errorString()));
        return result;
    }
    if (bytes.size() > MaximumFileBytes || !file.atEnd()) {
        result.issue = issue(
            SwarmSequenceError::TooManyBytes,
            QStringLiteral("Sequence file exceeds the 8 MiB safety limit."));
        return result;
    }

    SwarmSequenceLoadResult parsed = parse(bytes);
    parsed.absolutePath = result.absolutePath;
    return parsed;
}

SwarmSequenceSerializeResult SwarmSequenceFile::serialize(
    const SwarmSequenceDocument &input)
{
    SwarmSequenceSerializeResult result;
    const SwarmSequenceDocument document = normalizedDocument(input);
    result.issue = validate(document);
    if (!result.issue) {
        return result;
    }

    QJsonArray layouts;
    for (int layoutIndex = 0; layoutIndex < document.layouts.size(); ++layoutIndex) {
        const SwarmSequenceLayout &layout = document.layouts.at(layoutIndex);
        QJsonObject offsets;
        for (QMap<int, SwarmSequenceOffset>::const_iterator iterator =
                 layout.offsets.constBegin();
             iterator != layout.offsets.constEnd(); ++iterator) {
            offsets.insert(QString::number(iterator.key()), encodeOffset(iterator.value()));
        }

        QJsonObject object;
        object.insert(QStringLiteral("Id"), layout.id);
        object.insert(QStringLiteral("DelayStart"), layout.delayStart);
        object.insert(QStringLiteral("DelayEnd"), layout.delayEnd);
        object.insert(QStringLiteral("Offset"), offsets);
        layouts.append(object);
    }
    QJsonArray steps;
    for (int stepIndex = 0; stepIndex < document.steps.size(); ++stepIndex) {
        steps.append(document.steps.at(stepIndex));
    }
    QJsonObject root;
    root.insert(QStringLiteral("Layouts"), layouts);
    root.insert(QStringLiteral("Steps"), steps);
    result.json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (result.json.size() > MaximumFileBytes) {
        result.json.clear();
        result.issue = issue(
            SwarmSequenceError::TooManyBytes,
            QStringLiteral("Serialized sequence exceeds the 8 MiB safety limit."));
    }
    return result;
}

SwarmSequenceIssue SwarmSequenceFile::save(
    const QString &path, const SwarmSequenceDocument &document)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return issue(SwarmSequenceError::EmptyPath,
                     QStringLiteral("Sequence file path is empty."));
    }
    const SwarmSequenceSerializeResult encoded = serialize(document);
    if (!encoded.isValid()) {
        return encoded.issue;
    }

    const QString absolutePath = QDir::cleanPath(
        QFileInfo(trimmedPath).absoluteFilePath());
    const QString directory = QFileInfo(absolutePath).absolutePath();
    if (!QDir().mkpath(directory)) {
        return issue(SwarmSequenceError::SaveFailed,
                     QStringLiteral("Sequence directory cannot be created: %1")
                         .arg(directory));
    }

    QSaveFile file(absolutePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return issue(SwarmSequenceError::SaveFailed,
                     QStringLiteral("Sequence file cannot be opened for atomic save: %1")
                         .arg(file.errorString()));
    }
    if (file.write(encoded.json) != encoded.json.size()) {
        const QString error = file.errorString();
        file.cancelWriting();
        return issue(SwarmSequenceError::SaveFailed,
                     QStringLiteral("Sequence file cannot be written: %1").arg(error));
    }
    if (!file.commit()) {
        return issue(SwarmSequenceError::SaveFailed,
                     QStringLiteral("Sequence file cannot be atomically committed: %1")
                         .arg(file.errorString()));
    }
    return ok();
}

SwarmSequenceIssue SwarmSequenceFile::validate(
    const SwarmSequenceDocument &input)
{
    const SwarmSequenceDocument document = normalizedDocument(input);
    if (document.layouts.size() > MaximumLayouts) {
        return issue(SwarmSequenceError::TooManyLayouts,
                     QStringLiteral("Sequence contains more than %1 layouts.")
                         .arg(MaximumLayouts));
    }
    if (document.steps.size() > MaximumSteps) {
        return issue(SwarmSequenceError::TooManySteps,
                     QStringLiteral("Sequence contains more than %1 steps.")
                         .arg(MaximumSteps));
    }

    QMap<QString, bool> ids;
    QList<int> expectedSlots;
    bool haveExpectedSlots = false;
    for (int layoutIndex = 0; layoutIndex < document.layouts.size(); ++layoutIndex) {
        const SwarmSequenceLayout &layout = document.layouts.at(layoutIndex);
        if (layout.id.isEmpty()) {
            return issue(SwarmSequenceError::EmptyLayoutId,
                         QStringLiteral("Every layout must have a non-empty Id."));
        }
        if (ids.contains(layout.id)) {
            return issue(SwarmSequenceError::DuplicateLayoutId,
                         QStringLiteral("Layout Id '%1' is duplicated.").arg(layout.id));
        }
        ids.insert(layout.id, true);
        if (layout.offsets.size() > MaximumOffsets) {
            return issue(SwarmSequenceError::TooManyOffsets,
                         QStringLiteral("Layout '%1' contains more than %2 offsets.")
                             .arg(layout.id).arg(MaximumOffsets));
        }
        for (QMap<int, SwarmSequenceOffset>::const_iterator iterator =
                 layout.offsets.constBegin();
             iterator != layout.offsets.constEnd(); ++iterator) {
            if (iterator.key() < 1 || iterator.key() > 255) {
                return issue(SwarmSequenceError::InvalidSystemId,
                             QStringLiteral("Layout '%1' has invalid MAVLink system id %2.")
                                 .arg(layout.id).arg(iterator.key()));
            }
            if (!safeOffset(iterator.value())) {
                return issue(SwarmSequenceError::UnsafeOffset,
                             QStringLiteral("Layout '%1' has an invalid or excessive offset "
                                            "for system %2.")
                                 .arg(layout.id).arg(iterator.key()));
            }
        }
        const QList<int> slotIds = layout.offsets.keys();
        if (!haveExpectedSlots) {
            expectedSlots = slotIds;
            haveExpectedSlots = true;
        } else if (slotIds != expectedSlots) {
            return issue(SwarmSequenceError::InconsistentSlots,
                         QStringLiteral("Layout '%1' does not contain the same MAVLink "
                                        "system ids as the first layout.")
                             .arg(layout.id));
        }
    }

    for (int stepIndex = 0; stepIndex < document.steps.size(); ++stepIndex) {
        const QString &step = document.steps.at(stepIndex);
        if (!ids.contains(step)) {
            return issue(SwarmSequenceError::MissingStepLayout,
                         QStringLiteral("Sequence step %1 references missing layout '%2'.")
                             .arg(stepIndex).arg(step));
        }
    }
    return ok();
}

SwarmSequenceIssue SwarmSequenceEditor::replaceDocument(
    const SwarmSequenceDocument &document)
{
    return commitCandidate(normalizedDocument(document), &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::createLayout(const QString &requestedId)
{
    const QString id = normalizedId(requestedId);
    SwarmSequenceDocument candidate = m_document;
    SwarmSequenceLayout layout;
    layout.id = id;
    if (candidate.layouts.isEmpty()) {
        SwarmSequenceOffset offset;
        offset.x = 1.0;
        layout.offsets.insert(1, offset);
    } else {
        QList<int> slotIds = candidate.layouts.first().offsets.keys();
        if (slotIds.isEmpty()) {
            SwarmSequenceOffset firstOffset;
            firstOffset.x = 1.0;
            for (SwarmSequenceLayout &existing : candidate.layouts) {
                existing.offsets.insert(1, firstOffset);
            }
            slotIds.append(1);
        }
        for (int index = 0; index < slotIds.size(); ++index) {
            SwarmSequenceOffset offset;
            offset.x = double(slotIds.at(index));
            layout.offsets.insert(slotIds.at(index), offset);
        }
    }
    candidate.layouts.append(layout);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::cloneLayout(
    const QString &requestedSourceId, const QString &requestedNewId)
{
    const QString sourceId = normalizedId(requestedSourceId);
    const int sourceIndex = exactLayoutIndex(m_document, sourceId);
    if (sourceIndex < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(sourceId));
    }
    SwarmSequenceDocument candidate = m_document;
    SwarmSequenceLayout clone = candidate.layouts.at(sourceIndex);
    clone.id = normalizedId(requestedNewId);
    if (clone.offsets.isEmpty()) {
        SwarmSequenceOffset firstOffset;
        firstOffset.x = 1.0;
        for (SwarmSequenceLayout &existing : candidate.layouts) {
            existing.offsets.insert(1, firstOffset);
        }
        clone.offsets.insert(1, firstOffset);
    }
    candidate.layouts.append(clone);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::renameLayout(
    const QString &requestedOldId, const QString &requestedNewId)
{
    const QString oldId = normalizedId(requestedOldId);
    const int layoutIndex = exactLayoutIndex(m_document, oldId);
    if (layoutIndex < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(oldId));
    }
    const QString newId = normalizedId(requestedNewId);
    SwarmSequenceDocument candidate = m_document;
    candidate.layouts[layoutIndex].id = newId;
    for (int index = 0; index < candidate.steps.size(); ++index) {
        if (candidate.steps.at(index) == oldId) {
            candidate.steps[index] = newId;
        }
    }
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::removeLayout(const QString &requestedId)
{
    const QString id = normalizedId(requestedId);
    const int layoutIndex = exactLayoutIndex(m_document, id);
    if (layoutIndex < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(id));
    }
    if (m_document.steps.contains(id)) {
        return issue(SwarmSequenceError::ReferencedLayout,
                     QStringLiteral("Layout '%1' is still referenced by the sequence.").arg(id));
    }
    SwarmSequenceDocument candidate = m_document;
    candidate.layouts.removeAt(layoutIndex);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::setLayoutDelays(
    const QString &requestedId, int delayStart, int delayEnd)
{
    const QString id = normalizedId(requestedId);
    const int layoutIndex = exactLayoutIndex(m_document, id);
    if (layoutIndex < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(id));
    }
    SwarmSequenceDocument candidate = m_document;
    candidate.layouts[layoutIndex].delayStart = delayStart;
    candidate.layouts[layoutIndex].delayEnd = delayEnd;
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::setOffset(
    const QString &requestedLayoutId, int systemId,
    const SwarmSequenceOffset &offset)
{
    const QString layoutId = normalizedId(requestedLayoutId);
    const int layoutIndex = exactLayoutIndex(m_document, layoutId);
    if (layoutIndex < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(layoutId));
    }
    if (!m_document.layouts.at(layoutIndex).offsets.contains(systemId)) {
        return issue(SwarmSequenceError::InvalidSystemId,
                     QStringLiteral("Layout '%1' has no system id %2 slot.")
                         .arg(layoutId).arg(systemId));
    }
    SwarmSequenceDocument candidate = m_document;
    candidate.layouts[layoutIndex].offsets[systemId] = offset;
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::resizeSlots(int desiredCount)
{
    if (desiredCount < 1 || desiredCount > SwarmSequenceFile::MaximumOffsets) {
        return issue(SwarmSequenceError::InvalidSlotCount,
                     QStringLiteral("Sequence layout size must be between 1 and 255."));
    }
    if (m_document.layouts.isEmpty()) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Create a layout before resizing vehicle slots."));
    }

    SwarmSequenceDocument candidate = m_document;
    QList<int> slotIds = candidate.layouts.first().offsets.keys();
    while (slotIds.size() > desiredCount) {
        const int removeId = slotIds.takeLast();
        for (int index = 0; index < candidate.layouts.size(); ++index) {
            candidate.layouts[index].offsets.remove(removeId);
        }
    }
    while (slotIds.size() < desiredCount) {
        const int systemId = slotIds.isEmpty() ? 1 : slotIds.last() + 1;
        if (systemId > 255) {
            return issue(SwarmSequenceError::InvalidSlotCount,
                         QStringLiteral("The next MAVLink system id would exceed 255."));
        }
        SwarmSequenceOffset offset;
        offset.x = double(systemId);
        for (int index = 0; index < candidate.layouts.size(); ++index) {
            candidate.layouts[index].offsets.insert(systemId, offset);
        }
        slotIds.append(systemId);
    }
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::addStep(
    const QString &requestedLayoutId, int beforeIndex)
{
    const QString layoutId = normalizedId(requestedLayoutId);
    if (exactLayoutIndex(m_document, layoutId) < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(layoutId));
    }
    if (beforeIndex < -1 || beforeIndex > m_document.steps.size()) {
        return issue(SwarmSequenceError::InvalidIndex,
                     QStringLiteral("Step insertion index is outside the sequence."));
    }
    SwarmSequenceDocument candidate = m_document;
    const int index = beforeIndex < 0 ? candidate.steps.size() : beforeIndex;
    candidate.steps.insert(index, layoutId);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::replaceStep(
    int index, const QString &requestedLayoutId)
{
    if (index < 0 || index >= m_document.steps.size()) {
        return issue(SwarmSequenceError::InvalidIndex,
                     QStringLiteral("Step index is outside the sequence."));
    }
    const QString layoutId = normalizedId(requestedLayoutId);
    if (exactLayoutIndex(m_document, layoutId) < 0) {
        return issue(SwarmSequenceError::MissingLayout,
                     QStringLiteral("Layout '%1' does not exist.").arg(layoutId));
    }
    SwarmSequenceDocument candidate = m_document;
    candidate.steps[index] = layoutId;
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::removeStep(int index)
{
    if (index < 0 || index >= m_document.steps.size()) {
        return issue(SwarmSequenceError::InvalidIndex,
                     QStringLiteral("Step index is outside the sequence."));
    }
    SwarmSequenceDocument candidate = m_document;
    candidate.steps.removeAt(index);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceEditor::moveStep(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_document.steps.size()
        || toIndex < 0 || toIndex >= m_document.steps.size()) {
        return issue(SwarmSequenceError::InvalidIndex,
                     QStringLiteral("Step move index is outside the sequence."));
    }
    if (fromIndex == toIndex) {
        return ok();
    }
    SwarmSequenceDocument candidate = m_document;
    const QString step = candidate.steps.takeAt(fromIndex);
    candidate.steps.insert(toIndex, step);
    return commitCandidate(candidate, &m_document);
}

SwarmSequenceIssue SwarmSequenceGeometry::projectEastNorth(
    double originLatitude, double originLongitude,
    const SwarmSequenceOffset &offset,
    SwarmSequenceGeodeticPoint *projected)
{
    if (projected == nullptr || !std::isfinite(originLatitude)
        || !std::isfinite(originLongitude) || originLatitude < -90.0
        || originLatitude > 90.0 || originLongitude < -180.0
        || originLongitude > 180.0 || !safeOffset(offset)) {
        return issue(SwarmSequenceError::InvalidCoordinate,
                     QStringLiteral("Origin or local WGS84 offset is invalid."));
    }

    const double distance = std::hypot(offset.x, offset.y);
    if (distance <= std::numeric_limits<double>::epsilon()) {
        projected->latitude = originLatitude;
        projected->longitude = originLongitude;
        projected->altitudeM = offset.z;
        return ok();
    }

    // Match MP10 FormationGeometry.Project exactly: a spherical projection
    // with the WGS84 semi-major axis as its earth radius. The initial bearing
    // follows the Sequence grid convention: east is x and north is y.
    constexpr double earthRadiusM = 6378137.0;
    const double bearing = std::atan2(offset.x, offset.y);
    const double latitude1 = originLatitude * kDegreesToRadians;
    const double longitude1 = originLongitude * kDegreesToRadians;
    const double angularDistance = distance / earthRadiusM;
    const double latitude2 = std::asin(
        std::sin(latitude1) * std::cos(angularDistance)
        + std::cos(latitude1) * std::sin(angularDistance)
            * std::cos(bearing));
    const double longitude2 = longitude1 + std::atan2(
        std::sin(bearing) * std::sin(angularDistance)
            * std::cos(latitude1),
        std::cos(angularDistance)
            - std::sin(latitude1) * std::sin(latitude2));
    double normalizedLongitude =
        std::fmod(longitude2 * kRadiansToDegrees + 540.0, 360.0);
    if (normalizedLongitude < 0.0) {
        normalizedLongitude += 360.0;
    }
    normalizedLongitude -= 180.0;

    projected->latitude = latitude2 * kRadiansToDegrees;
    projected->longitude = normalizedLongitude;
    projected->altitudeM = offset.z;
    if (!std::isfinite(projected->latitude)
        || !std::isfinite(projected->longitude)) {
        return issue(SwarmSequenceError::InvalidCoordinate,
                     QStringLiteral("WGS84 projection produced an invalid coordinate."));
    }
    return ok();
}
