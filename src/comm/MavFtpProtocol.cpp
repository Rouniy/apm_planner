#include "MavFtpProtocol.h"

#include <limits>

namespace MavFtpProtocol
{
namespace
{

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool isKnownOpcode(Opcode opcode)
{
    const quint8 value = static_cast<quint8>(opcode);
    return value <= static_cast<quint8>(Opcode::ListDirectoryWithTime)
            || opcode == Opcode::Ack || opcode == Opcode::Nak;
}

bool isCommandOpcode(Opcode opcode)
{
    return static_cast<quint8>(opcode)
            <= static_cast<quint8>(Opcode::ListDirectoryWithTime);
}

bool isKnownErrorCode(ErrorCode code)
{
    return static_cast<quint8>(code)
            <= static_cast<quint8>(ErrorCode::FileNotFound);
}

bool validateSemanticPayload(const PayloadHeader &payload, QString *error)
{
    if (!isKnownOpcode(payload.opcode)) {
        setError(error, QStringLiteral("unknown MAVFTP opcode"));
        return false;
    }
    if (!isKnownOpcode(payload.requestOpcode)) {
        setError(error, QStringLiteral("unknown MAVFTP request opcode"));
        return false;
    }
    if (payload.size > DataSize || payload.data.size() > DataSize) {
        setError(error, QStringLiteral("MAVFTP data exceeds 239 bytes"));
        return false;
    }
    const bool sizedReadRequest =
            (payload.opcode == Opcode::ReadFile
             || payload.opcode == Opcode::BurstReadFile)
            && payload.data.isEmpty();
    if (!sizedReadRequest && payload.size != payload.data.size()) {
        setError(error,
                 QStringLiteral("MAVFTP declared size does not match data"));
        return false;
    }
    if (payload.burstComplete > 1) {
        setError(error,
                 QStringLiteral("MAVFTP burst-complete flag is not boolean"));
        return false;
    }
    return true;
}

bool isValidUtf8(const QByteArray &bytes)
{
    int index = 0;
    const int length = bytes.size();
    while (index < length) {
        const quint8 lead = static_cast<quint8>(bytes.at(index));
        if (lead <= 0x7f) {
            ++index;
            continue;
        }

        int continuationCount = 0;
        quint8 secondMinimum = 0x80;
        quint8 secondMaximum = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuationCount = 1;
        } else if (lead == 0xe0) {
            continuationCount = 2;
            secondMinimum = 0xa0;
        } else if (lead >= 0xe1 && lead <= 0xec) {
            continuationCount = 2;
        } else if (lead == 0xed) {
            continuationCount = 2;
            secondMaximum = 0x9f;
        } else if (lead >= 0xee && lead <= 0xef) {
            continuationCount = 2;
        } else if (lead == 0xf0) {
            continuationCount = 3;
            secondMinimum = 0x90;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            continuationCount = 3;
        } else if (lead == 0xf4) {
            continuationCount = 3;
            secondMaximum = 0x8f;
        } else {
            return false;
        }

        if (index + continuationCount >= length) {
            return false;
        }
        const quint8 second = static_cast<quint8>(bytes.at(index + 1));
        if (second < secondMinimum || second > secondMaximum) {
            return false;
        }
        for (int offset = 2; offset <= continuationCount; ++offset) {
            const quint8 continuation =
                    static_cast<quint8>(bytes.at(index + offset));
            if (continuation < 0x80 || continuation > 0xbf) {
                return false;
            }
        }
        index += continuationCount + 1;
    }
    return true;
}

bool isValidPathString(const QString &path)
{
    for (int index = 0; index < path.size(); ++index) {
        const QChar character = path.at(index);
        if (character.unicode() == 0) {
            return false;
        }
        if (character.isHighSurrogate()) {
            if (++index >= path.size() || !path.at(index).isLowSurrogate()) {
                return false;
            }
        } else if (character.isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

bool parseUnsignedDecimal(const QByteArray &bytes, quint64 *value)
{
    if (!value || bytes.isEmpty()) {
        return false;
    }

    quint64 parsed = 0;
    for (const char byte : bytes) {
        if (byte < '0' || byte > '9') {
            return false;
        }
        const quint8 digit = static_cast<quint8>(byte - '0');
        if (parsed > (std::numeric_limits<quint64>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

bool isSafeDirectoryLeaf(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (const QChar character : name) {
        if (character == QLatin1Char('/')
            || character == QLatin1Char('\\')
            || character.isNull()
            || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

} // namespace

QByteArray encodePayload(const PayloadHeader &payload, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!validateSemanticPayload(payload, error)) {
        return {};
    }

    QByteArray wire(PayloadSize, '\0');
    wire[0] = static_cast<char>(payload.sequence & 0xff);
    wire[1] = static_cast<char>((payload.sequence >> 8) & 0xff);
    wire[2] = static_cast<char>(payload.session);
    wire[3] = static_cast<char>(payload.opcode);
    wire[4] = static_cast<char>(payload.size);
    wire[5] = static_cast<char>(payload.requestOpcode);
    wire[6] = static_cast<char>(payload.burstComplete);
    wire[7] = static_cast<char>(payload.padding);
    wire[8] = static_cast<char>(payload.offset & 0xff);
    wire[9] = static_cast<char>((payload.offset >> 8) & 0xff);
    wire[10] = static_cast<char>((payload.offset >> 16) & 0xff);
    wire[11] = static_cast<char>((payload.offset >> 24) & 0xff);
    for (int index = 0; index < payload.data.size(); ++index) {
        wire[HeaderSize + index] = payload.data.at(index);
    }
    return wire;
}

bool decodePayload(const QByteArray &wire, PayloadHeader *payload,
                   QString *error)
{
    if (error) {
        error->clear();
    }
    if (!payload) {
        setError(error, QStringLiteral("MAVFTP output payload is null"));
        return false;
    }
    if (wire.size() != PayloadSize) {
        setError(error, QStringLiteral("MAVFTP payload must be exactly 251 bytes"));
        return false;
    }

    PayloadHeader decoded;
    decoded.sequence = static_cast<quint8>(wire.at(0))
            | (static_cast<quint16>(static_cast<quint8>(wire.at(1))) << 8);
    decoded.session = static_cast<quint8>(wire.at(2));
    decoded.opcode = static_cast<Opcode>(static_cast<quint8>(wire.at(3)));
    decoded.size = static_cast<quint8>(wire.at(4));
    decoded.requestOpcode =
            static_cast<Opcode>(static_cast<quint8>(wire.at(5)));
    decoded.burstComplete = static_cast<quint8>(wire.at(6));
    decoded.padding = static_cast<quint8>(wire.at(7));
    decoded.offset = static_cast<quint8>(wire.at(8))
            | (static_cast<quint32>(static_cast<quint8>(wire.at(9))) << 8)
            | (static_cast<quint32>(static_cast<quint8>(wire.at(10))) << 16)
            | (static_cast<quint32>(static_cast<quint8>(wire.at(11))) << 24);

    if (decoded.size > DataSize) {
        setError(error, QStringLiteral("MAVFTP declared size exceeds 239 bytes"));
        return false;
    }
    decoded.data = wire.mid(HeaderSize, decoded.size);
    if (!validateSemanticPayload(decoded, error)) {
        return false;
    }
    *payload = decoded;
    return true;
}

bool encodePath(const QString &path, QByteArray *encoded, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!encoded) {
        setError(error, QStringLiteral("MAVFTP path output is null"));
        return false;
    }
    if (!isValidPathString(path)) {
        setError(error,
                 QStringLiteral("MAVFTP path contains invalid Unicode or NUL"));
        return false;
    }

    const QByteArray utf8 = path.toUtf8();
    if (utf8.size() > MaximumPathBytes) {
        setError(error, QStringLiteral("MAVFTP UTF-8 path exceeds 238 bytes"));
        return false;
    }
    *encoded = utf8;
    return true;
}

bool validateResponse(const PayloadHeader &request,
                      const PayloadHeader &response, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!validateSemanticPayload(request, error)
            || !validateSemanticPayload(response, error)) {
        return false;
    }
    if (!isCommandOpcode(request.opcode)) {
        setError(error, QStringLiteral("MAVFTP request has a response opcode"));
        return false;
    }
    if (response.opcode != Opcode::Ack && response.opcode != Opcode::Nak) {
        setError(error, QStringLiteral("MAVFTP packet is not an ACK or NAK"));
        return false;
    }
    const quint16 expectedSequence =
            static_cast<quint16>(request.sequence + 1u);
    if (response.sequence != expectedSequence) {
        setError(error, QStringLiteral("MAVFTP response sequence mismatch"));
        return false;
    }
    if (response.requestOpcode != request.opcode) {
        setError(error, QStringLiteral("MAVFTP response request-opcode mismatch"));
        return false;
    }

    if (response.opcode == Opcode::Nak) {
        if (response.data.isEmpty()) {
            setError(error, QStringLiteral("MAVFTP NAK has no error code"));
            return false;
        }
        const ErrorCode code = static_cast<ErrorCode>(
                static_cast<quint8>(response.data.at(0)));
        if (!isKnownErrorCode(code) || code == ErrorCode::None) {
            setError(error, QStringLiteral("MAVFTP NAK has an invalid error code"));
            return false;
        }
        const int requiredSize = code == ErrorCode::FailErrno ? 2 : 1;
        if (response.data.size() != requiredSize) {
            setError(error, QStringLiteral("MAVFTP NAK data size is invalid"));
            return false;
        }
    }
    return true;
}

bool parseDirectoryEntries(const QByteArray &buffer, int size,
                           QVector<DirectoryEntry> *entries, QString *error)
{
    if (error) {
        error->clear();
    }
    if (!entries) {
        setError(error, QStringLiteral("MAVFTP directory output is null"));
        return false;
    }
    if (size < 0 || size > buffer.size() || size > DataSize) {
        setError(error, QStringLiteral("MAVFTP directory payload bounds are invalid"));
        return false;
    }

    QVector<DirectoryEntry> parsedEntries;
    int offset = 0;
    while (offset < size) {
        const quint8 typeTag = static_cast<quint8>(buffer.at(offset++));
        if (typeTag == 0) {
            continue;
        }

        const int terminator = buffer.indexOf('\0', offset);
        if (terminator < 0 || terminator >= size) {
            setError(error,
                     QStringLiteral("MAVFTP directory entry is not NUL terminated"));
            return false;
        }
        const QByteArray valueBytes = buffer.mid(offset, terminator - offset);
        if (!isValidUtf8(valueBytes)) {
            setError(error,
                     QStringLiteral("MAVFTP directory entry has invalid UTF-8"));
            return false;
        }
        const QString value = QString::fromUtf8(valueBytes);
        offset = terminator + 1;

        DirectoryEntry entry;
        entry.typeTag = typeTag;
        if (typeTag == static_cast<quint8>('F')) {
            const int separator = valueBytes.lastIndexOf('\t');
            quint64 fileSize = 0;
            if (separator <= 0 || separator == valueBytes.size() - 1
                    || !parseUnsignedDecimal(valueBytes.mid(separator + 1),
                                             &fileSize)) {
                setError(error,
                         QStringLiteral("MAVFTP file entry has no valid size"));
                return false;
            }
            entry.type = DirectoryEntryType::File;
            entry.name = QString::fromUtf8(valueBytes.left(separator));
            if (!isSafeDirectoryLeaf(entry.name)
                || entry.name == QStringLiteral(".")
                || entry.name == QStringLiteral("..")) {
                setError(error,
                         QStringLiteral("MAVFTP file entry has an unsafe name"));
                return false;
            }
            entry.size = fileSize;
            parsedEntries.append(entry);
        } else if (typeTag == static_cast<quint8>('D')) {
            entry.type = DirectoryEntryType::Directory;
            entry.name = value;
            if (!isSafeDirectoryLeaf(entry.name)) {
                setError(error,
                         QStringLiteral("MAVFTP directory entry has an unsafe name"));
                return false;
            }
            parsedEntries.append(entry);
        } else if (typeTag == static_cast<quint8>('S')) {
            entry.type = DirectoryEntryType::Skip;
            parsedEntries.append(entry);
        } else if (!value.isEmpty()) {
            // Mission Planner retains named vendor-specific records as files.
            entry.type = DirectoryEntryType::Other;
            entry.name = value;
            if (!isSafeDirectoryLeaf(entry.name)
                || entry.name == QStringLiteral(".")
                || entry.name == QStringLiteral("..")) {
                setError(error,
                         QStringLiteral("MAVFTP vendor entry has an unsafe name"));
                return false;
            }
            parsedEntries.append(entry);
        }
    }

    *entries = parsedEntries;
    return true;
}

quint32 crc32(const QByteArray &data, quint32 state)
{
    for (const char byte : data) {
        state ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const quint32 mask = 0u - (state & 1u);
            state = (state >> 1) ^ (0xedb88320u & mask);
        }
    }
    return state;
}

} // namespace MavFtpProtocol
