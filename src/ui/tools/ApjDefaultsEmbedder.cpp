#include "ApjDefaultsEmbedder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>
#include <QtEndian>

#include <zlib.h>
#include <algorithm>
#include <exception>

namespace {
using Result = ApjDefaultsEmbedder::Result;
using Progress = ApjDefaultsEmbedder::Progress;
using Cancel = ApjDefaultsEmbedder::Cancel;
constexpr int Chunk = 64 * 1024;
const QByteArray ParameterMagic = QByteArray("PARMDEF\0", 8) + QByteArray::fromHex("5537f4a0385d485b");
const QByteArray UnsignedMagic = QByteArray::fromHex("40a2e4f164689106");
const QByteArray SignedMagic = QByteArray::fromHex("41a3e5f265699207");

bool fail(Result *result, const QString &error) { result->error = error; return false; }
bool stop(const Cancel &cancel, Result *result)
{
    if (cancel && cancel()) { result->cancelled = true; return true; }
    return false;
}
bool phase(const Progress &progress, const Cancel &cancel, int value, Result *result)
{
    if (progress) progress(value, 1000);
    return !stop(cancel, result);
}

struct Paths { QString firmware, parameters, output; };
bool resolvePaths(const QString &firmware, const QString &parameters, Paths *paths, Result *result)
{
    const QFileInfo fw(firmware), params(parameters), out(ApjDefaultsEmbedder::SuggestedOutputPath(firmware));
    if (firmware.isEmpty() || parameters.isEmpty()
        || !fw.isFile() || !fw.isReadable() || !params.isFile() || !params.isReadable())
        return fail(result, QStringLiteral("Select a readable APJ firmware and parameter defaults file."));
    if (out.isSymLink() || (out.exists() && !out.isFile()))
        return fail(result, QStringLiteral("The APJ output must be a regular file, not a symbolic link or directory."));
    paths->firmware = fw.canonicalFilePath(); paths->parameters = params.canonicalFilePath();
    const QString parent = QFileInfo(out.absolutePath()).canonicalFilePath();
    if (parent.isEmpty() || paths->firmware.isEmpty() || paths->parameters.isEmpty())
        return fail(result, QStringLiteral("The selected paths cannot be resolved."));
    paths->output = QDir(parent).filePath(out.fileName());
    if (paths->output == paths->firmware || paths->output == paths->parameters
        || paths->firmware == paths->parameters)
        return fail(result, QStringLiteral("Firmware, defaults and output must be different files."));
    return true;
}

bool readBounded(const QString &path, qint64 maximum, QByteArray *bytes,
                 const Cancel &cancel, Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(result, file.errorString());
    if (file.size() < 0 || file.size() > maximum)
        return fail(result, QStringLiteral("Input exceeds its %1-byte size limit.").arg(maximum));
    bytes->clear();
    while (!file.atEnd()) {
        if (stop(cancel, result)) return false;
        const QByteArray part = file.read(Chunk);
        if (part.isEmpty() || bytes->size() > maximum - part.size())
            return fail(result, QStringLiteral("Input read failed or grew beyond its size limit."));
        bytes->append(part);
    }
    return file.error() == QFile::NoError || fail(result, file.errorString());
}

QByteArray digest(const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
bool fileHash(const QString &path, QByteArray *hash, const Cancel &cancel, Result *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(result, QStringLiteral("A selected file is no longer readable."));
    if (file.size() < 0 || file.size() > ApjDefaultsEmbedder::MaximumJsonBytes)
        return fail(result, QStringLiteral("A selected file exceeds the 96 MiB verification limit."));
    QCryptographicHash h(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (!file.atEnd()) {
        if (stop(cancel, result)) return false;
        const QByteArray bytes = file.read(Chunk);
        if (bytes.isEmpty()) return fail(result, QStringLiteral("Could not verify a selected file."));
        read += bytes.size();
        if (read > ApjDefaultsEmbedder::MaximumJsonBytes)
            return fail(result, QStringLiteral("A selected file grew beyond the verification limit."));
        h.addData(bytes);
    }
    if (file.error() != QFile::NoError) return fail(result, file.errorString());
    *hash = h.result(); return true;
}

struct Member { QString key; int begin = 0, end = 0; };
void skipSpace(const QByteArray &json, int *at)
{
    while (*at < json.size() && QByteArray(" \t\r\n").contains(json[*at])) ++*at;
}
bool skipString(const QByteArray &json, int *at)
{
    if (*at >= json.size() || json[*at] != '"') return false;
    ++*at;
    while (*at < json.size()) {
        const char byte = json[(*at)++];
        if (byte == '"') return true;
        if (byte == '\\') ++*at;
    }
    return false;
}

// QJson validates syntax; this scanner records only root value spans. Never
// serialize the entire QJsonObject: Qt 5 would round untouched large numbers.
bool rootMembers(const QByteArray &json, QVector<Member> *members, int *closing,
                 const Cancel &cancel, Result *result)
{
    int at = json.startsWith(QByteArray::fromHex("efbbbf")) ? 3 : 0;
    skipSpace(json, &at);
    if (at >= json.size() || json[at++] != '{') return fail(result, QStringLiteral("APJ root must be a JSON object."));
    QSet<QString> names;
    while (at < json.size()) {
        if (stop(cancel, result)) return false;
        skipSpace(json, &at);
        if (at < json.size() && json[at] == '}') { *closing = at; return true; }
        const int keyStart = at;
        if (!skipString(json, &at)) return fail(result, QStringLiteral("Invalid APJ root member."));
        const QString key = QJsonDocument::fromJson('[' + json.mid(keyStart, at - keyStart) + ']').array().at(0).toString();
        if (names.contains(key) || members->size() >= 4096)
            return fail(result, QStringLiteral("Duplicate or excessive APJ root members are unsupported."));
        names.insert(key);
        skipSpace(json, &at);
        if (at >= json.size() || json[at++] != ':') return fail(result, QStringLiteral("Invalid APJ member separator."));
        skipSpace(json, &at);
        const int begin = at;
        int depth = 0;
        while (at < json.size()) {
            if ((at & 65535) == 0 && stop(cancel, result)) return false;
            const char byte = json[at];
            if (byte == '"') { if (!skipString(json, &at)) return false; continue; }
            if (!depth && (byte == ',' || byte == '}')) break;
            if (byte == '{' || byte == '[') ++depth;
            else if (byte == '}' || byte == ']') --depth;
            ++at;
        }
        int end = at;
        while (end > begin && QByteArray(" \t\r\n").contains(json[end - 1])) --end;
        members->append({key, begin, end});
        if (at < json.size() && json[at] == ',') ++at;
    }
    return fail(result, QStringLiteral("Incomplete APJ root object."));
}

bool integerMember(const QByteArray &json, const QVector<Member> &members,
                   const QString &name, qint64 *value, Result *result)
{
    for (const auto &member : members) {
        if (member.key != name) continue;
        const QByteArray raw = json.mid(member.begin, member.end - member.begin);
        bool ok = !raw.isEmpty();
        for (char byte : raw) if (byte < '0' || byte > '9') ok = false;
        bool range = false;
        const qint64 parsed = raw.toLongLong(&range);
        if (!ok || !range) return fail(result, QStringLiteral("APJ %1 must be a nonnegative signed-64-bit integer.").arg(name));
        *value = parsed; return true;
    }
    return fail(result, QStringLiteral("APJ %1 is missing.").arg(name));
}

bool decodeDefaults(const QByteArray &raw, QByteArray *defaults, Result *result)
{
    int unit = 1, at = 0; bool little = true;
    if (raw.startsWith(QByteArray::fromHex("fffe0000"))) { unit = 4; at = 4; }
    else if (raw.startsWith(QByteArray::fromHex("0000feff"))) { unit = 4; at = 4; little = false; }
    else if (raw.startsWith(QByteArray::fromHex("fffe"))) { unit = 2; at = 2; }
    else if (raw.startsWith(QByteArray::fromHex("feff"))) { unit = 2; at = 2; little = false; }
    else if (raw.startsWith(QByteArray::fromHex("efbbbf"))) at = 3;
    if ((raw.size() - at) % unit)
        return fail(result, QStringLiteral("Truncated BOM-encoded parameter text."));
    for (; at < raw.size(); at += unit) {
        quint32 code = 0;
        for (int byte = 0; byte < unit; ++byte)
            code |= quint32(quint8(raw[at + byte])) << (8 * (little ? byte : unit - byte - 1));
        if (code > 127 || code == 0 || (code < 32 && code != '\t' && code != '\r' && code != '\n') || code == 127)
            return fail(result, QStringLiteral("Parameter defaults must decode to ASCII text without NUL/control characters; no lossy replacement is allowed."));
        if (code != '\r') defaults->append(char(code));
    }
    return true;
}

bool base64Image(const QString &text, QByteArray *compressed, const Cancel &cancel, Result *result)
{
    QByteArray clean;
    clean.reserve(text.size());
    for (int at = 0; at < text.size(); ++at) {
        if ((at & 65535) == 0 && stop(cancel, result)) return false;
        const ushort code = text[at].unicode();
        if (code == ' ' || code == '\r' || code == '\n' || code == '\t') continue;
        if (!((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
              || (code >= '0' && code <= '9') || code == '+' || code == '/' || code == '='))
            return fail(result, QStringLiteral("APJ image contains invalid Base64 characters."));
        clean.append(char(code));
    }
    if (clean.isEmpty() || clean.size() % 4)
        return fail(result, QStringLiteral("APJ image has malformed Base64 padding."));
    *compressed = QByteArray::fromBase64(clean);
    if (compressed->toBase64() != clean)
        return fail(result, QStringLiteral("APJ image is not canonical padded Base64."));
    return true;
}

bool zlibImage(const QByteArray &input, QByteArray *output, bool compress,
               const Progress &progress, const Cancel &cancel, Result *result)
{
    z_stream stream{};
    const int initialized = compress ? deflateInit(&stream, Z_BEST_COMPRESSION) : inflateInit(&stream);
    if (initialized != Z_OK) return fail(result, QStringLiteral("Could not initialize zlib."));
    struct End { z_stream *stream; bool compress; ~End() { if (compress) deflateEnd(stream); else inflateEnd(stream); } } end{&stream, compress};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = uInt(input.size());
    char buffer[Chunk];
    int previousPhase = -1;
    while (true) {
        if (stop(cancel, result)) return false;
        stream.next_out = reinterpret_cast<Bytef *>(buffer); stream.avail_out = sizeof(buffer);
        const int code = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
        const int bytes = sizeof(buffer) - int(stream.avail_out);
        const qint64 maximum = compress ? ApjDefaultsEmbedder::MaximumJsonBytes : ApjDefaultsEmbedder::MaximumImageBytes;
        if (output->size() > maximum - bytes)
            return fail(result, QStringLiteral("Decompressed image or compressed output exceeds its size limit."));
        output->append(buffer, bytes);
        const int value = (compress ? 650 : 150) + int(250LL * stream.total_in / qMax(1, input.size()));
        if (value != previousPhase) {
            previousPhase = value;
            if (!phase(progress, cancel, value, result)) return false;
        }
        if (code == Z_STREAM_END) {
            if (stream.avail_in != 0)
                return fail(result, QStringLiteral("Trailing data or concatenated streams after the zlib image are unsupported."));
            return true;
        }
        if (code != Z_OK || (!bytes && !stream.avail_in))
            return fail(result, QStringLiteral("Malformed, truncated or unsupported zlib image."));
    }
}

bool crc(const QByteArray &image, int begin, int end, quint32 *out,
         const Cancel &cancel, Result *result)
{
    // ArduPilot bootloader CRC, not zlib's complemented IEEE CRC API.
    quint32 value = 0;
    for (int at = begin; at < end; ++at) {
        if ((at & 65535) == 0 && stop(cancel, result)) return false;
        value ^= quint8(image[at]);
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    *out = value; return true;
}
quint16 read16(const QByteArray &data, int at) { return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + at)); }
quint32 read32(const QByteArray &data, int at) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + at)); }
void write16(QByteArray *data, int at, quint16 value) { qToLittleEndian(value, reinterpret_cast<uchar *>(data->data() + at)); }
void write32(QByteArray *data, int at, quint32 value) { qToLittleEndian(value, reinterpret_cast<uchar *>(data->data() + at)); }

bool patchImage(QByteArray *image, const QByteArray &defaults, const Progress &progress,
                const Cancel &cancel, Result *result)
{
    if (image->contains(SignedMagic)) return fail(result, QStringLiteral("Signed firmware descriptors cannot be modified without re-signing; this tool does not sign firmware."));
    const int area = image->indexOf(ParameterMagic);
    if (area < 0) return fail(result, QStringLiteral("No exact PARMDEF defaults area was found."));
    if (image->indexOf(ParameterMagic, area + 1) >= 0)
        return fail(result, QStringLiteral("Multiple defaults areas are ambiguous."));
    if (area > image->size() - 20) return fail(result, QStringLiteral("Truncated defaults header."));
    const quint16 maximum = read16(*image, area + 16), oldLength = read16(*image, area + 18);
    if (maximum == 0 || oldLength > maximum || area + 20 > image->size() - maximum)
        return fail(result, QStringLiteral("Invalid or truncated defaults capacity."));
    result->maximumDefaultsBytes = maximum;
    if (defaults.size() > maximum) return fail(result, QStringLiteral("Defaults length %1 exceeds firmware capacity %2.").arg(defaults.size()).arg(maximum));
    const int descriptor = image->indexOf(UnsignedMagic);
    if (descriptor >= 0) {
        if (image->indexOf(UnsignedMagic, descriptor + 1) >= 0 || descriptor > image->size() - 36)
            return fail(result, QStringLiteral("Multiple or truncated unsigned firmware descriptors are unsupported."));
        if (read32(*image, descriptor + 16) != quint32(image->size()))
            return fail(result, QStringLiteral("Unsigned descriptor image size does not match the full firmware image."));
        if (area < descriptor + 36 && area + 20 + maximum > descriptor)
            return fail(result, QStringLiteral("Defaults area overlaps the firmware descriptor."));
        quint32 first = 0, second = 0;
        if (!crc(*image, 0, descriptor + 8, &first, cancel, result)
            || !crc(*image, descriptor + 24, image->size(), &second, cancel, result)) return false;
        if (read32(*image, descriptor + 8) != first || read32(*image, descriptor + 12) != second)
            return fail(result, QStringLiteral("Unsigned firmware descriptor CRC was inconsistent before this operation; restart from the original or freshly downloaded firmware."));
    } else result->warnings.append(QStringLiteral("No recognized unsigned APP_DESCRIPTOR was present; only the defaults region was patched. Boot compatibility is not verified."));
    if (!phase(progress, cancel, 500, result)) return false;
    write16(image, area + 18, quint16(defaults.size()));
    image->replace(area + 20, defaults.size(), defaults);
    // Bytes beyond the new active length intentionally retain their old values.
    if (descriptor >= 0) {
        quint32 first = 0, second = 0;
        if (!crc(*image, 0, descriptor + 8, &first, cancel, result)
            || !crc(*image, descriptor + 24, image->size(), &second, cancel, result)) return false;
        write32(image, descriptor + 8, first);
        write32(image, descriptor + 12, second);
        result->repairedDescriptors = 1;
    }
    return phase(progress, cancel, 600, result);
}
}

QString ApjDefaultsEmbedder::SuggestedOutputPath(const QString &firmware)
{
    return firmware + QStringLiteral("new.apj");
}

ApjDefaultsEmbedder::Result ApjDefaultsEmbedder::Embed(
    const QString &firmware, const QString &parameters, const Options &options,
    const Progress &progress, const Cancel &cancel)
{
    Result result;
    try {
        Paths paths;
        if (!resolvePaths(firmware, parameters, &paths, &result)) return result;
        const bool existed = QFileInfo::exists(paths.output);
        if (existed && !options.overwriteExisting) { fail(&result, QStringLiteral("Output already exists; explicit overwrite confirmation is required.")); return result; }
        QByteArray oldOutputHash;
        if (existed && !fileHash(paths.output, &oldOutputHash, cancel, &result)) return result;
        if (!phase(progress, cancel, 0, &result)) return result;
        QByteArray json, parameterSource;
        if (!readBounded(paths.firmware, MaximumJsonBytes, &json, cancel, &result)
            || !readBounded(paths.parameters, MaximumParameterBytes, &parameterSource, cancel, &result)) return result;
        const QByteArray firmwareHash = digest(json), parameterHash = digest(parameterSource);
        QByteArray defaults;
        if (!decodeDefaults(parameterSource, &defaults, &result)) return result;
        result.defaultsBytes = defaults.size();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(&result, QStringLiteral("Malformed APJ JSON object: %1.").arg(parseError.errorString())); return result;
        }
        QVector<Member> members; int closing = 0;
        if (!rootMembers(json, &members, &closing, cancel, &result)) return result;
        const QJsonObject object = document.object();
        if (object.value("magic").toString() != "APJFWv1") { fail(&result, QStringLiteral("Unsupported APJ magic; APJFWv1 is required.")); return result; }
        if (object.contains("signed_firmware") && (!object.value("signed_firmware").isBool() || object.value("signed_firmware").toBool())) {
            fail(&result, QStringLiteral("Signed or ambiguously marked firmware cannot be modified by this tool.")); return result;
        }
        if (!object.value("image").isString()) { fail(&result, QStringLiteral("APJ image must be a Base64 string.")); return result; }
        qint64 expectedSize = 0;
        if (!integerMember(json, members, "image_size", &expectedSize, &result)) return result;
        if (expectedSize > MaximumImageBytes) { fail(&result, QStringLiteral("APJ image exceeds the 64 MiB limit.")); return result; }
        qint64 flashTotal = 0;
        const bool hasFlashTotal = object.contains("flash_total");
        if (hasFlashTotal && !integerMember(json, members, "flash_total", &flashTotal, &result)) return result;
        if (hasFlashTotal && flashTotal < expectedSize) { fail(&result, QStringLiteral("APJ flash_total is smaller than image_size.")); return result; }
        if (!hasFlashTotal) result.warnings.append(QStringLiteral("APJ flash_total is absent; flash_free was not recalculated."));
        if (!phase(progress, cancel, 100, &result)) return result;
        QByteArray compressed, image;
        if (!base64Image(object.value("image").toString(), &compressed, cancel, &result)
            || !zlibImage(compressed, &image, false, progress, cancel, &result)) return result;
        if (image.size() != expectedSize) { fail(&result, QStringLiteral("APJ image_size does not match the decompressed image.")); return result; }
        result.imageBytes = image.size();
        if (!patchImage(&image, defaults, progress, cancel, &result)) return result;
        compressed.clear();
        if (!zlibImage(image, &compressed, true, progress, cancel, &result)) return result;
        QHash<QString, QByteArray> replacements;
        replacements.insert("image", '"' + compressed.toBase64() + '"');
        replacements.insert("image_size", QByteArray::number(image.size()));
        if (hasFlashTotal) replacements.insert("flash_free", QByteArray::number(flashTotal - image.size()));
        if (hasFlashTotal && !object.contains("flash_free"))
            json.insert(closing, QByteArray(",\n  \"flash_free\": ") + replacements.value("flash_free"));
        for (int index = members.size() - 1; index >= 0; --index) {
            const auto &member = members[index];
            if (replacements.contains(member.key))
                json.replace(member.begin, member.end - member.begin, replacements.value(member.key));
        }
        if (json.size() > MaximumJsonBytes) { fail(&result, QStringLiteral("Generated APJ exceeds the 96 MiB JSON limit.")); return result; }
        result.warnings.append(QStringLiteral("Offline defaults tool only: no firmware is flashed or signed; parameter compatibility and flight safety have not been validated."));
        if (!phase(progress, cancel, 920, &result)) return result;

        // No-overwrite publication stages privately and renames only after all
        // checks; confirmed replacement uses QSaveFile's atomic commit.
        QTemporaryDir stage(QDir(QFileInfo(paths.output).absolutePath()).filePath(".apj-defaults-XXXXXX"));
        if (!stage.isValid()) { fail(&result, QStringLiteral("Cannot create private APJ staging directory.")); return result; }
        const QString staged = stage.filePath("firmware.apj");
        QSaveFile output(existed ? paths.output : staged);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) { fail(&result, output.errorString()); return result; }
        for (int at = 0; at < json.size(); at += Chunk) {
            if (stop(cancel, &result)) return result;
            const int count = qMin(Chunk, json.size() - at);
            if (output.write(json.constData() + at, count) != count) { fail(&result, output.errorString()); return result; }
        }
        if (!output.flush()) { fail(&result, output.errorString()); return result; }
        if (!phase(progress, cancel, 960, &result)) return result;
        QByteArray check;
        if (!fileHash(paths.firmware, &check, cancel, &result)) return result;
        if (check != firmwareHash) { fail(&result, QStringLiteral("Firmware input changed before publication.")); return result; }
        if (!fileHash(paths.parameters, &check, cancel, &result)) return result;
        if (check != parameterHash) { fail(&result, QStringLiteral("Parameter input changed before publication.")); return result; }
        if (existed) {
            if (!fileHash(paths.output, &check, cancel, &result)) return result;
            if (check != oldOutputHash) { fail(&result, QStringLiteral("Confirmed output changed before publication.")); return result; }
        }
        if (stop(cancel, &result)) return result;
        Paths current;
        if (!resolvePaths(firmware, parameters, &current, &result)) return result;
        if (current.firmware != paths.firmware || current.parameters != paths.parameters || current.output != paths.output
            || QFileInfo::exists(paths.output) != existed) {
            fail(&result, QStringLiteral("Selected paths changed before publication.")); return result;
        }
        if (!output.commit()) { fail(&result, output.errorString()); return result; }
        if (!existed && !QFile::rename(staged, paths.output)) { fail(&result, QStringLiteral("Could not publish APJ without overwriting an existing file.")); return result; }
        result.success = true; result.outputPath = paths.output; result.outputBytes = json.size();
    } catch (const std::exception &error) {
        result.error = QStringLiteral("APJ defaults failed: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) { result.error = QStringLiteral("Unexpected APJ defaults failure."); }
    return result;
}
