#ifndef MAVFTPPROTOCOL_H
#define MAVFTPPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace MavFtpProtocol
{

constexpr int PayloadSize = 251;
constexpr int HeaderSize = 12;
constexpr int DataSize = PayloadSize - HeaderSize;
// ArduPilot always reserves the final path byte for a defensive NUL before
// passing it to the filesystem, so a path must leave that byte available.
constexpr int MaximumPathBytes = DataSize - 1;

enum class Opcode : quint8
{
    None = 0,
    TerminateSession = 1,
    ResetSessions = 2,
    ListDirectory = 3,
    OpenFileReadOnly = 4,
    ReadFile = 5,
    CreateFile = 6,
    WriteFile = 7,
    RemoveFile = 8,
    CreateDirectory = 9,
    RemoveDirectory = 10,
    OpenFileWriteOnly = 11,
    TruncateFile = 12,
    Rename = 13,
    CalculateFileCrc32 = 14,
    BurstReadFile = 15,
    ListDirectoryWithTime = 16,
    Ack = 128,
    Nak = 129
};

enum class ErrorCode : quint8
{
    None = 0,
    Fail = 1,
    FailErrno = 2,
    InvalidDataSize = 3,
    InvalidSession = 4,
    NoSessionsAvailable = 5,
    EndOfFile = 6,
    UnknownCommand = 7,
    FileExists = 8,
    FileProtected = 9,
    FileNotFound = 10
};

/** The semantic fields carried by the packed MAVFTP payload. */
struct PayloadHeader
{
    quint16 sequence = 0;
    quint8 session = 0;
    Opcode opcode = Opcode::None;
    quint8 size = 0;
    Opcode requestOpcode = Opcode::None;
    quint8 burstComplete = 0;
    quint8 padding = 0;
    quint32 offset = 0;
    QByteArray data;
};

/**
 * Encode exactly 251 bytes. Normally size equals data.size(); read requests
 * use size as the requested byte count and consequently carry no data.
 */
QByteArray encodePayload(const PayloadHeader &payload,
                         QString *error = nullptr);

/** Decode exactly 251 bytes and reject invalid sizes or opcode values. */
bool decodePayload(const QByteArray &wire, PayloadHeader *payload,
                   QString *error = nullptr);

/** Encode a path as at most 238 well-formed UTF-8 bytes, without a NUL. */
bool encodePath(const QString &path, QByteArray *encoded,
                QString *error = nullptr);

/**
 * Validate the MAVFTP response envelope and request correlation.
 *
 * Transport endpoint, target identity and operation-specific session/offset
 * checks belong to the service which owns the exact vehicle target.
 */
bool validateResponse(const PayloadHeader &request,
                      const PayloadHeader &response,
                      QString *error = nullptr);

enum class DirectoryEntryType
{
    File,
    Directory,
    Skip,
    Other
};

struct DirectoryEntry
{
    DirectoryEntryType type = DirectoryEntryType::Other;
    QString name;
    quint64 size = 0;
    quint8 typeTag = 0;
};

/**
 * Parse the first size bytes of an MP-compatible directory response.
 * The output is updated only after the complete packet has been validated.
 */
bool parseDirectoryEntries(const QByteArray &buffer, int size,
                           QVector<DirectoryEntry> *entries,
                           QString *error = nullptr);

/** ArduPilot/MAVFTP reflected CRC-32 update (initial state is normally zero). */
quint32 crc32(const QByteArray &data, quint32 state = 0);

} // namespace MavFtpProtocol

#endif // MAVFTPPROTOCOL_H
