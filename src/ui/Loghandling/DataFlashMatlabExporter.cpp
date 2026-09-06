#include "DataFlashMatlabExporter.h"

#include "comm/MatFileWriter.h"
#include "DataFlashModeNames.h"
#include "DataFlashRawReader.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QSet>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#include <qt_windows.h>
#endif

namespace {

using CellValue = MatFileWriter::CellValue;
using Definition = DataFlashRaw::Definition;

constexpr int NumericBufferValues = 65536;

struct SchemaRecord {
    Definition definition;
    QByteArray labelName;
    QVector<QString> labels;
};

struct NumericVariable {
    QByteArray name;
    QByteArray type;
    int definitionId = -1;
    qint64 rows = 0;
    int columns = 0;
};

struct CellTable {
    QByteArray type;
    QByteArray name;
    qint64 rows = 0;
    qint64 encodedChildrenBytes = 0;
};

struct ParameterValue {
    QString name;
    double value = 0;
};

qint64 paddedToEight(qint64 bytes)
{
    return (bytes + 7) & ~qint64(7);
}

void clearError(QString *error)
{
    if (error)
        error->clear();
}

bool pathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool validMatName(const QByteArray &name)
{
    return !name.isEmpty() && name.size() <= MatFileWriter::MaximumNameBytes
        && !name.contains('\0')
        && QString::fromUtf8(name.constData(), name.size()).toUtf8() == name;
}

struct StageIdentity {
#ifdef Q_OS_UNIX
    quint64 device = 0;
    quint64 inode = 0;
#elif defined(Q_OS_WIN)
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
#else
    QString canonicalPath;
    QDateTime born;
#endif
    bool valid = false;

    static StageIdentity capture(QFileDevice *file, const QString &path)
    {
        StageIdentity result;
        if (!file || !file->isOpen() || file->handle() < 0)
            return result;
#ifdef Q_OS_UNIX
        struct stat descriptorStatus {};
        struct stat pathStatus {};
        const QByteArray encoded = QFile::encodeName(path);
        if (::fstat(file->handle(), &descriptorStatus) != 0
            || ::lstat(encoded.constData(), &pathStatus) != 0
            || !S_ISREG(descriptorStatus.st_mode)
            || !S_ISREG(pathStatus.st_mode)
            || descriptorStatus.st_dev != pathStatus.st_dev
            || descriptorStatus.st_ino != pathStatus.st_ino) {
            return result;
        }
        result.device = quint64(descriptorStatus.st_dev);
        result.inode = quint64(descriptorStatus.st_ino);
#elif defined(Q_OS_WIN)
        const intptr_t native = _get_osfhandle(file->handle());
        BY_HANDLE_FILE_INFORMATION information {};
        if (native == -1
            || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(native),
                                           &information)) {
            return result;
        }
        result.volume = information.dwVolumeSerialNumber;
        result.indexHigh = information.nFileIndexHigh;
        result.indexLow = information.nFileIndexLow;
#else
        const QFileInfo information(path);
        if (!information.isFile() || information.isSymLink()
            || information.canonicalFilePath().isEmpty()
            || !information.birthTime().isValid()) {
            return result;
        }
        result.canonicalPath = information.canonicalFilePath();
        result.born = information.birthTime();
#endif
        result.valid = true;
        return result;
    }

    bool matches(const QString &path) const
    {
        if (!valid)
            return false;
#ifdef Q_OS_UNIX
        struct stat status {};
        const QByteArray encoded = QFile::encodeName(path);
        return ::lstat(encoded.constData(), &status) == 0
            && S_ISREG(status.st_mode) && quint64(status.st_dev) == device
            && quint64(status.st_ino) == inode;
#elif defined(Q_OS_WIN)
        const HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION information {};
        const bool matched = GetFileInformationByHandle(handle, &information)
            && !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            && information.dwVolumeSerialNumber == volume
            && information.nFileIndexHigh == indexHigh
            && information.nFileIndexLow == indexLow;
        CloseHandle(handle);
        return matched;
#else
        const QFileInfo information(path);
        return information.isFile() && !information.isSymLink()
            && information.canonicalFilePath() == canonicalPath
            && information.birthTime().isValid()
            && information.birthTime() == born;
#endif
    }
};

struct OwnedStageCleanup {
    QString path;
    StageIdentity identity;

    bool removeOwned()
    {
        if (!pathExists(path))
            return true;
        return identity.matches(path) && QFile::remove(path);
    }

    ~OwnedStageCleanup()
    {
        // Never let QTemporaryFile remove whatever happens to occupy the old
        // random pathname after a callback or concurrent rename.
        removeOwned();
    }
};

bool publishNoReplace(const QString &staging, const QString &destination,
                      const StageIdentity &identity, int descriptor,
                      QString *error)
{
#ifdef Q_OS_LINUX
    struct stat status {};
    if (descriptor < 0 || ::fstat(descriptor, &status) != 0
        || !S_ISREG(status.st_mode)
        || quint64(status.st_dev) != identity.device
        || quint64(status.st_ino) != identity.inode
        || !identity.matches(staging)) {
        *error = QStringLiteral("The private MATLAB staging file changed before publication.");
        return false;
    }
    const QByteArray descriptorName = QByteArrayLiteral("/proc/self/fd/")
        + QByteArray::number(descriptor);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::linkat(AT_FDCWD, descriptorName.constData(), AT_FDCWD,
                 destinationName.constData(), AT_SYMLINK_FOLLOW) != 0) {
        *error = QStringLiteral(
            "Could not publish the MATLAB output without replacing an existing file.");
        return false;
    }
    return true;
#elif defined(Q_OS_UNIX)
    Q_UNUSED(descriptor)
    if (!identity.matches(staging)) {
        *error = QStringLiteral("The private MATLAB staging file changed before publication.");
        return false;
    }
    const QByteArray sourceName = QFile::encodeName(staging);
    const QByteArray destinationName = QFile::encodeName(destination);
    if (::link(sourceName.constData(), destinationName.constData()) != 0) {
        *error = QStringLiteral(
            "Could not publish the MATLAB output without replacing an existing file.");
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    Q_UNUSED(descriptor)
    if (!identity.matches(staging)) {
        *error = QStringLiteral("The private MATLAB staging file changed before publication.");
        return false;
    }
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(staging.utf16()),
                     reinterpret_cast<LPCWSTR>(destination.utf16()),
                     MOVEFILE_WRITE_THROUGH)) {
        *error = QStringLiteral(
            "Could not publish the MATLAB output without replacing an existing file.");
        return false;
    }
    return true;
#else
    Q_UNUSED(identity)
    Q_UNUSED(descriptor)
    Q_UNUSED(staging)
    Q_UNUSED(destination)
    *error = QStringLiteral(
        "Atomic no-overwrite MATLAB publication is unavailable on this platform.");
    return false;
#endif
}

bool cancelled(const DataFlashMatlabExporter::CancelCheck &cancel)
{
    return cancel && cancel();
}

void reportProgress(const DataFlashMatlabExporter::Progress &progress,
                    qint64 completed, qint64 total)
{
    if (progress)
        progress(qBound<qint64>(0, completed, qMax<qint64>(1, total)),
                 qMax<qint64>(1, total));
}

QByteArray dotNetAscii(QByteArray value)
{
    for (char &byte : value) {
        if (quint8(byte) > 127)
            byte = '?';
    }
    while (!value.isEmpty() && value.front() == '\0')
        value.remove(0, 1);
    while (!value.isEmpty() && value.back() == '\0')
        value.chop(1);
    return value;
}

bool roundTrips(const QString &text, double value, bool singlePrecision)
{
    const QByteArray bytes = text.toLatin1();
    if (singlePrecision) {
        float parsed = 0.0f;
        const auto converted = std::from_chars(bytes.constData(),
            bytes.constData() + bytes.size(), parsed, std::chars_format::general);
        return converted.ec == std::errc()
            && converted.ptr == bytes.constData() + bytes.size()
            && parsed == static_cast<float>(value);
    }
    double parsed = 0.0;
    const auto converted = std::from_chars(bytes.constData(),
        bytes.constData() + bytes.size(), parsed, std::chars_format::general);
    return converted.ec == std::errc()
        && converted.ptr == bytes.constData() + bytes.size() && parsed == value;
}

QByteArray normalizeDotNetExponent(const QByteArray &input)
{
    int marker = input.indexOf('e');
    if (marker < 0)
        marker = input.indexOf('E');
    if (marker < 0)
        return input;
    int digit = marker + 1;
    char sign = '+';
    if (digit < input.size()
        && (input.at(digit) == '+' || input.at(digit) == '-')) {
        sign = input.at(digit++);
    }
    return input.left(marker) + 'E' + sign
        + input.mid(digit).rightJustified(2, '0');
}

QByteArray dotNetSingle(float value)
{
    std::array<char, 128> storage{};
    auto converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                   value, std::chars_format::general);
    if (converted.ec != std::errc())
        return QString::number(value, 'g', 9).toUpper().toLatin1();
    QByteArray general(storage.data(), int(converted.ptr - storage.data()));
    int marker = general.indexOf('e');
    if (marker < 0)
        marker = general.indexOf('E');
    if (marker < 0)
        return general;
    const int exponent = general.mid(marker + 1).toInt();
    if (exponent >= 0 && exponent < 9) {
        converted = std::to_chars(storage.data(), storage.data() + storage.size(),
                                  value, std::chars_format::fixed);
        if (converted.ec == std::errc())
            return QByteArray(storage.data(), int(converted.ptr - storage.data()));
    }
    return normalizeDotNetExponent(general);
}

QByteArray dotNetNumber(double value, bool singlePrecision)
{
    if (std::isnan(value))
        return QByteArrayLiteral("NaN");
    if (std::isinf(value))
        return value < 0.0 ? QByteArrayLiteral("-Infinity")
                           : QByteArrayLiteral("Infinity");
    if (value == 0.0)
        return std::signbit(value) ? QByteArrayLiteral("-0")
                                   : QByteArrayLiteral("0");
    if (singlePrecision)
        return dotNetSingle(static_cast<float>(value));

    for (int precision = 1; precision <= 17; ++precision) {
        const QString general = QString::number(value, 'g', precision);
        if (!roundTrips(general, value, false))
            continue;
        const int marker = general.indexOf(QLatin1Char('e'));
        if (marker < 0)
            return general.toLatin1();
        const int exponent = general.mid(marker + 1).toInt();
        if (exponent >= 0 && exponent < 17) {
            const QString fixed = QString::number(
                value, 'f', qMax(0, precision - 1 - exponent));
            if (roundTrips(fixed, value, false))
                return fixed.toLatin1();
        }
        return general.toUpper().toLatin1();
    }
    return QString::number(value, 'g', 17).toUpper().toLatin1();
}

template<typename Integer>
Integer littleEndian(const QByteArray &raw, int offset)
{
    return qFromLittleEndian<Integer>(
        reinterpret_cast<const uchar *>(raw.constData() + offset));
}

float halfToFloat(quint16 bits)
{
    const quint32 sign = quint32(bits & 0x8000u) << 16;
    quint32 exponent = (bits >> 10) & 0x1fu;
    quint32 fraction = bits & 0x03ffu;
    quint32 output = 0;
    if (exponent == 0) {
        if (fraction == 0) {
            output = sign;
        } else {
            int shift = 0;
            while ((fraction & 0x0400u) == 0) {
                fraction <<= 1;
                ++shift;
            }
            fraction &= 0x03ffu;
            output = sign | (quint32(127 - 14 - shift) << 23)
                | (fraction << 13);
        }
    } else if (exponent == 0x1fu) {
        output = sign | 0x7f800000u | (fraction << 13);
    } else {
        output = sign | ((exponent + 112) << 23) | (fraction << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &output, sizeof(value));
    return value;
}

QByteArray escapedBlob(const QByteArray &raw, bool *empty)
{
    const QByteArray text = dotNetAscii(raw);
    *empty = text.isEmpty();
    QByteArray result;
    result.reserve(text.size());
    for (char byte : text) {
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (quint8(byte) < 32 || quint8(byte) > 127) {
                result += QByteArrayLiteral("\\x")
                    + QByteArray::number(quint8(byte), 16)
                          .rightJustified(2, '0').toUpper();
            } else {
                result += byte;
            }
            break;
        }
    }
    return result;
}

enum class DecodeResult { Complete, EmptyBlob, Malformed };

DecodeResult binaryLogicalLine(const QByteArray &raw, const Definition &definition,
                               const QByteArray &firmware, QByteArray *line)
{
    QList<QByteArray> values;
    values.reserve(definition.format.size() + 1);
    values.append(definition.name);
    int offset = 3;
    for (char type : definition.format) {
        const int width = DataFlashRaw::width(type);
        // Mission Planner 10's BinaryLog predates `A`: it emits an empty item
        // and does not advance, even though framing still uses the modern size.
        if (!width || type == 'A') {
            values.append(QByteArray());
            continue;
        }
        if (offset > raw.size() - width)
            return DecodeResult::Malformed;
        QByteArray value;
        switch (type) {
        case 'b': value = QByteArray::number(qint8(raw.at(offset))); break;
        case 'B': value = QByteArray::number(quint8(raw.at(offset))); break;
        case 'h': value = QByteArray::number(littleEndian<qint16>(raw, offset)); break;
        case 'H': value = QByteArray::number(littleEndian<quint16>(raw, offset)); break;
        case 'i': value = QByteArray::number(littleEndian<qint32>(raw, offset)); break;
        case 'I': value = QByteArray::number(littleEndian<quint32>(raw, offset)); break;
        case 'q': value = QByteArray::number(littleEndian<qint64>(raw, offset)); break;
        case 'Q': value = QByteArray::number(littleEndian<quint64>(raw, offset)); break;
        case 'g': value = dotNetNumber(halfToFloat(littleEndian<quint16>(raw, offset)), true); break;
        case 'f': {
            const quint32 bits = littleEndian<quint32>(raw, offset);
            float number = 0.0f;
            std::memcpy(&number, &bits, sizeof(number));
            value = dotNetNumber(number, true);
            break;
        }
        case 'd': {
            const quint64 bits = littleEndian<quint64>(raw, offset);
            double number = 0.0;
            std::memcpy(&number, &bits, sizeof(number));
            value = dotNetNumber(number, false);
            break;
        }
        case 'c': value = dotNetNumber(double(littleEndian<qint16>(raw, offset)) / 100.0, false); break;
        case 'C': value = dotNetNumber(double(littleEndian<quint16>(raw, offset)) / 100.0, false); break;
        case 'e': value = dotNetNumber(double(littleEndian<qint32>(raw, offset)) / 100.0, false); break;
        case 'E': value = dotNetNumber(double(littleEndian<quint32>(raw, offset)) / 100.0, false); break;
        case 'L': value = dotNetNumber(double(littleEndian<qint32>(raw, offset)) / 10000000.0, false); break;
        case 'n': case 'N': value = dotNetAscii(raw.mid(offset, width)); break;
        case 'M': {
            const int number = quint8(raw.at(offset));
            const QString mode = DataFlashModeNames::resolve(firmware, number);
            value = mode.isEmpty() ? QByteArray::number(number) : mode.toLatin1();
            break;
        }
        case 'Z': {
            bool empty = false;
            value = escapedBlob(raw.mid(offset, width), &empty);
            if (empty)
                return DecodeResult::EmptyBlob;
            break;
        }
        case 'a':
            value = QByteArrayLiteral("[");
            for (int index = 0; index < 32; ++index) {
                if (index)
                    value += ' ';
                value += QByteArray::number(
                    littleEndian<qint16>(raw, offset + index * 2));
            }
            value += ']';
            break;
        default: values.append(QByteArray()); continue;
        }
        values.append(value);
        offset += width;
    }
    *line = values.join(QByteArrayLiteral(", ")) + QByteArrayLiteral("\r\n");
    return DecodeResult::Complete;
}

void detectFirmware(const QByteArray &line, QByteArray *firmware)
{
    if (line.contains("PARM, RATE_RLL_P") || line.contains("ArduCopter")
        || line.contains("Copter")) {
        *firmware = QByteArrayLiteral("ArduCopter2");
    } else if (line.contains("PARM, H_SWASH_PLATE")) {
        *firmware = QByteArrayLiteral("ArduCopter2");
    } else if (line.contains("PARM, PTCH2SRV_P")
               || line.contains("ArduPlane") || line.contains("Plane")) {
        *firmware = QByteArrayLiteral("ArduPlane");
    } else if (line.contains("PARM, SKID_STEER_OUT")
               || line.contains("ArduRover") || line.contains("Rover")) {
        *firmware = QByteArrayLiteral("ArduRover");
    } else if (line.contains("AntennaTracker") || line.contains("Tracker")) {
        *firmware = QByteArrayLiteral("ArduTracker");
    }
}

QList<QByteArray> splitProcessLine(QByteArray line)
{
    line = dotNetAscii(line);
    line.replace(QByteArrayLiteral(", "), QByteArrayLiteral(","));
    line.replace(QByteArrayLiteral(": "), QByteArrayLiteral(":"));
    QList<QByteArray> items;
    int start = 0;
    for (int index = 0; index < line.size(); ++index) {
        if (line.at(index) == ',' || line.at(index) == ':') {
            items.append(line.mid(start, index - start));
            start = index + 1;
        }
    }
    items.append(line.mid(start));
    return items;
}

double dotNetParsedNaN()
{
    const quint64 bits = Q_UINT64_C(0xfff8000000000000);
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool asciiDecimal(const QByteArray &input, bool negative, double *value)
{
    if (input.isEmpty())
        return false;
    int marker = input.indexOf('e');
    if (marker < 0)
        marker = input.indexOf('E');
    if (marker >= 0
        && (input.indexOf('e', marker + 1) >= 0
            || input.indexOf('E', marker + 1) >= 0)) {
        return false;
    }
    const int mantissaEnd = marker < 0 ? input.size() : marker;
    bool dot = false;
    int digitsBeforeDot = 0;
    int digitIndex = 0;
    int firstNonzero = -1;
    for (int index = 0; index < mantissaEnd; ++index) {
        const char byte = input.at(index);
        if (byte == '.' && !dot) {
            dot = true;
            continue;
        }
        if (byte < '0' || byte > '9')
            return false;
        if (!dot)
            ++digitsBeforeDot;
        if (byte != '0' && firstNonzero < 0)
            firstNonzero = digitIndex;
        ++digitIndex;
    }
    if (!digitIndex)
        return false;
    qint64 exponent = 0;
    if (marker >= 0) {
        int index = marker + 1;
        bool exponentNegative = false;
        if (index < input.size()
            && (input.at(index) == '+' || input.at(index) == '-')) {
            exponentNegative = input.at(index) == '-';
            ++index;
        }
        if (index == input.size())
            return false;
        for (; index < input.size(); ++index) {
            const char byte = input.at(index);
            if (byte < '0' || byte > '9')
                return false;
            exponent = qMin<qint64>(1000000,
                                    exponent * 10 + qint64(byte - '0'));
        }
        if (exponentNegative)
            exponent = -exponent;
    }

    double parsed = 0;
    const auto converted = std::from_chars(input.constData(),
        input.constData() + input.size(), parsed, std::chars_format::general);
    if (converted.ptr != input.constData() + input.size())
        return false;
    if (converted.ec == std::errc()) {
        *value = negative ? -parsed : parsed;
        return true;
    }
    if (converted.ec != std::errc::result_out_of_range)
        return false;
    if (firstNonzero < 0) {
        *value = std::copysign(0.0, negative ? -1.0 : 1.0);
        return true;
    }
    const qint64 magnitude = exponent + digitsBeforeDot - firstNonzero - 1;
    if (magnitude >= 308) {
        *value = negative ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity();
    } else {
        *value = std::copysign(0.0, negative ? -1.0 : 1.0);
    }
    return true;
}

bool invariantNumber(const QByteArray &input, double *value)
{
    QByteArray text = input.trimmed();
    bool negative = false;
    const bool negativeParentheses = text.size() > 2
        && text.front() == '(' && text.back() == ')';
    if (negativeParentheses) {
        text = text.mid(1, text.size() - 2).trimmed();
        negative = true;
    }
    if (!text.isEmpty() && (text.back() == '+' || text.back() == '-')) {
        if (negativeParentheses)
            return false;
        negative = text.back() == '-';
        text.chop(1);
        text = text.trimmed();
        if (!text.isEmpty() && (text.front() == '+' || text.front() == '-'))
            return false;
    } else if (!text.isEmpty() && (text.front() == '+' || text.front() == '-')) {
        if (negativeParentheses)
            return false;
        negative = text.front() == '-';
        text.remove(0, 1);
    }
    const QByteArray folded = text.toLower();
    if (folded == "nan") {
        *value = dotNetParsedNaN();
        return true;
    }
    if (folded == "infinity") {
        *value = negative ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity();
        return true;
    }
    return asciiDecimal(text, negative, value);
}

bool invariantFloatNumber(const QByteArray &input, double *value)
{
    // Double.Parse(value, InvariantCulture) uses Float|AllowThousands, not
    // NumberStyles.Any: a leading sign is accepted, but parentheses and a
    // trailing sign are not. Comma grouping cannot survive ProcessLog's CSV
    // split, so the remaining accepted grammar is the bounded decimal parser.
    QByteArray text = input.trimmed();
    if (text.size() > 2 && text.front() == '(' && text.back() == ')')
        return false;
    if (!text.isEmpty() && (text.back() == '+' || text.back() == '-'))
        return false;
    bool negative = false;
    if (!text.isEmpty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove(0, 1);
    }
    const QByteArray folded = text.toLower();
    if (folded == "nan") {
        *value = dotNetParsedNaN();
        return true;
    }
    if (folded == "infinity") {
        *value = negative ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity();
        return true;
    }
    return asciiDecimal(text, negative, value);
}

bool localizedNumber(const QByteArray &input, const QLocale &locale, double *value)
{
    const QByteArray text = input.trimmed();
    bool negative = false;
    QByteArray unsignedText = text;
    if (!unsignedText.isEmpty()
        && (unsignedText.front() == '+' || unsignedText.front() == '-')) {
        negative = unsignedText.front() == '-';
        unsignedText.remove(0, 1);
    }
    const QByteArray folded = unsignedText.toLower();
    if (folded == "inf")
        return false;
    if (folded == "nan") { *value = dotNetParsedNaN(); return true; }
    if (folded == "infinity") {
        *value = negative ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity();
        return true;
    }
    bool ok = false;
    const double parsed = locale.toDouble(QString::fromLatin1(text), &ok);
    if (ok) {
        *value = parsed;
        return true;
    }
    if (locale.decimalPoint() == QLatin1Char('.')
        && asciiDecimal(unsignedText, negative, value)) {
        return true;
    }
    return false;
}

QVector<QByteArray> splitArrayToken(const QByteArray &input)
{
    QVector<QByteArray> parts;
    int start = -1;
    for (int index = 0; index <= input.size(); ++index) {
        const bool separator = index == input.size() || input.at(index) == ' '
            || input.at(index) == '[' || input.at(index) == ']';
        if (!separator && start < 0)
            start = index;
        if (separator && start >= 0) {
            parts.append(input.mid(start, index - start));
            start = -1;
        }
    }
    return parts;
}

bool buildCustomValue(const QByteArray &item, const QLocale &locale,
                      int depth, int *nodes, CellValue *value, QString *error)
{
    if (depth > MatFileWriter::MaximumCellDepth
        || ++*nodes > MatFileWriter::MaximumCellNodes) {
        *error = QStringLiteral("A MSG/ISBD cell exceeds the MATLAB nesting/node bound.");
        return false;
    }
    double number = 0;
    if (localizedNumber(item, locale, &number)) {
        value->type = CellValue::Type::Scalar;
        value->number = number;
        return true;
    }
    const QByteArray trimmed = item.trimmed();
    if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
        const QVector<QByteArray> parts = splitArrayToken(item);
        value->type = CellValue::Type::Cell;
        value->rows = parts.size();
        value->columns = 1;
        value->children.reserve(parts.size());
        for (const QByteArray &part : parts) {
            CellValue child;
            if (!buildCustomValue(part, locale, depth + 1, nodes,
                                  &child, error)) {
                return false;
            }
            value->children.append(std::move(child));
        }
        return true;
    }
    value->type = CellValue::Type::Text;
    value->text = QString::fromLatin1(trimmed);
    return true;
}

bool buildCustomRow(const QList<QByteArray> &items, const QLocale &locale,
                    CellValue *row, qint64 *encodedBytes, QString *error)
{
    row->type = CellValue::Type::Cell;
    row->rows = items.size();
    row->columns = 1;
    row->children.reserve(items.size());
    int nodes = 1;
    for (const QByteArray &item : items) {
        CellValue child;
        if (!buildCustomValue(item, locale, 2, &nodes, &child, error))
            return false;
        row->children.append(std::move(child));
    }
    std::function<bool(const CellValue &, int, qint64 *)> measure;
    measure = [&](const CellValue &item, int depth, qint64 *size) {
        if (depth > MatFileWriter::MaximumCellDepth)
            return false;
        qint64 total = 48;
        if (item.type == CellValue::Type::Scalar) {
            total += 16;
        } else if (item.type == CellValue::Type::Text) {
            total += 8 + paddedToEight(qint64(item.text.size()) * 2);
        } else {
            for (const CellValue &child : item.children) {
                qint64 childSize = 0;
                if (!measure(child, depth + 1, &childSize)
                    || childSize > MatFileWriter::MaximumCellValueBytes - total) {
                    return false;
                }
                total += childSize;
            }
        }
        if (total > MatFileWriter::MaximumCellValueBytes)
            return false;
        *size = total;
        return true;
    };
    if (!measure(*row, 1, encodedBytes)) {
        *error = QStringLiteral("A MSG/ISBD row exceeds the 8 MiB MATLAB cell bound.");
        return false;
    }
    return true;
}

bool decodeItems(const QByteArray &raw, const Definition &definition, bool text,
                 QByteArray *firmware, QList<QByteArray> *items,
                 bool *emptyBlob, QString *error)
{
    QByteArray line;
    if (text) {
        line = raw;
    } else {
        const DecodeResult decoded = binaryLogicalLine(raw, definition,
                                                       *firmware, &line);
        if (decoded == DecodeResult::EmptyBlob) {
            *emptyBlob = true;
            return false;
        }
        if (decoded != DecodeResult::Complete) {
            *error = QStringLiteral("A binary DataFlash record could not be decoded safely.");
            return false;
        }
        detectFirmware(line, firmware);
    }
    *items = splitProcessLine(line);
    return true;
}

bool readFmtu(const QByteArray &raw, const Definition &definition, bool text,
              int *formatId, QByteArray *unitIds, QString *error)
{
    const int typeIndex = definition.columns.indexOf(QByteArrayLiteral("FmtType"));
    const int unitsIndex = definition.columns.indexOf(QByteArrayLiteral("UnitIds"));
    if (typeIndex < 0 || unitsIndex < 0) {
        *error = QStringLiteral("FMTU does not identify its format and unit columns.");
        return false;
    }
    if (text) {
        const QList<QByteArray> values = DataFlashRaw::textValues(raw, definition);
        bool ok = false;
        const int id = values.value(typeIndex).trimmed().toInt(&ok);
        if (!ok || id < 0 || id > 255) {
            *error = QStringLiteral("FMTU has an invalid format identity.");
            return false;
        }
        *formatId = id;
        *unitIds = dotNetAscii(values.value(unitsIndex)).trimmed();
        return true;
    }
    if (definition.format.at(typeIndex) != 'B'
        || (definition.format.at(unitsIndex) != 'N'
            && definition.format.at(unitsIndex) != 'n')) {
        *error = QStringLiteral("FMTU uses an unsupported identity/unit encoding.");
        return false;
    }
    *formatId = quint8(raw.at(definition.offsets.at(typeIndex)));
    *unitIds = dotNetAscii(raw.mid(definition.offsets.at(unitsIndex),
                                   DataFlashRaw::width(definition.format.at(unitsIndex))));
    return true;
}

bool sourceMatches(const QString &requested, const QString &canonical,
                   qint64 size, const QDateTime &modifiedUtc)
{
    const QFileInfo current(requested);
    return current.isFile() && !current.isSymLink()
        && current.canonicalFilePath() == canonical && current.size() == size
        && current.lastModified().toUTC() == modifiedUtc;
}

bool hashFile(const QString &path, qint64 expectedSize,
              const DataFlashMatlabExporter::CancelCheck &cancel,
              QByteArray *digest, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not reopen the DataFlash source: %1")
                     .arg(file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    while (bytes < expectedSize) {
        if (cancelled(cancel))
            return false;
        const QByteArray chunk = file.read(qMin<qint64>(1024 * 1024,
                                                        expectedSize - bytes));
        if (chunk.isEmpty()) {
            *error = QStringLiteral("The DataFlash source changed or failed during verification.");
            return false;
        }
        bytes += chunk.size();
        hash.addData(chunk);
    }
    if (!file.atEnd() || file.error() != QFileDevice::NoError) {
        *error = QStringLiteral("The DataFlash source changed or failed during verification.");
        return false;
    }
    *digest = hash.result();
    return true;
}

qint64 cellTextBytes(const QString &text)
{
    return 56 + paddedToEight(qint64(text.size()) * 2);
}

qint64 cellMatrixBase(const QByteArray &name)
{
    return 48 + paddedToEight(name.size());
}

bool addBounded(qint64 value, qint64 *total)
{
    if (value < 0 || *total > MatFileWriter::MaximumFileBytes - value)
        return false;
    *total += value;
    return true;
}

bool validTopLevelMatrixBytes(qint64 bytes)
{
    return bytes >= 8
        && bytes - 8 <= qint64(std::numeric_limits<quint32>::max());
}

bool omittedLogicalRecord(bool text, int kind, const QByteArray &raw,
                          const QFile &file)
{
    // DFLogBuffer does not add an incomplete binary frame to Count, and its
    // ASCII line index counts newline terminators rather than a final fragment.
    if (!text)
        return kind == -2;
    return file.atEnd() && !raw.endsWith('\n');
}

} // namespace

struct DataFlashMatlabExporter::Plan::Data
{
    QString sourcePath;
    QString canonicalSource;
    QString outputPath;
    QString outputParentCanonical;
    qint64 sourceBytes = 0;
    QDateTime modifiedUtc;
    QByteArray digest;
    QByteArray firmware;
    bool text = false;
    qint64 records = 0;
    qint64 estimated = 0;
    QVector<SchemaRecord> schemas;
    QHash<int, QByteArray> unitsByFormat;
    QVector<NumericVariable> numeric;
    QVector<CellTable> cells;
    QVector<ParameterValue> parameters;
    QVector<QByteArray> seen;
    QLocale cellLocale;
    QStringList warnings;
    qint64 skippedRows = 0;
    qint64 emptyBlobRows = 0;
};

DataFlashMatlabExporter::Plan::Plan() = default;
DataFlashMatlabExporter::Plan::Plan(const Plan &) = default;
DataFlashMatlabExporter::Plan &DataFlashMatlabExporter::Plan::operator=(const Plan &) = default;
DataFlashMatlabExporter::Plan::Plan(Plan &&) noexcept = default;
DataFlashMatlabExporter::Plan &DataFlashMatlabExporter::Plan::operator=(Plan &&) noexcept = default;
DataFlashMatlabExporter::Plan::~Plan() = default;
DataFlashMatlabExporter::Plan::Plan(std::shared_ptr<const Data> data)
    : d(std::move(data)) {}
bool DataFlashMatlabExporter::Plan::isValid() const noexcept { return bool(d); }
QString DataFlashMatlabExporter::Plan::sourcePath() const { return d ? d->sourcePath : QString(); }
QString DataFlashMatlabExporter::Plan::outputPath() const { return d ? d->outputPath : QString(); }
qint64 DataFlashMatlabExporter::Plan::recordCount() const noexcept { return d ? d->records : 0; }
qint64 DataFlashMatlabExporter::Plan::estimatedBytes() const noexcept { return d ? d->estimated : 0; }
int DataFlashMatlabExporter::Plan::variableCount() const noexcept
{
    return d ? d->schemas.size() + d->numeric.size() + d->cells.size() + 2 : 0;
}
QStringList DataFlashMatlabExporter::Plan::warnings() const { return d ? d->warnings : QStringList(); }

DataFlashMatlabExporter::PlanResult DataFlashMatlabExporter::Prepare(
    const QString &inputPath, const CancelCheck &cancel, const Progress &progress)
{
    PlanResult result;
    try {
        const QFileInfo source(QFileInfo(inputPath).absoluteFilePath());
        const QString suffix = source.suffix().toLower();
        if (!source.isFile() || source.isSymLink()
            || source.canonicalFilePath().isEmpty()) {
            result.error = QStringLiteral(
                "The DataFlash source must be a regular, non-symlink file.");
            return result;
        }
        if (suffix != QStringLiteral("bin") && suffix != QStringLiteral("log")) {
            result.error = QStringLiteral("MATLAB export accepts DataFlash .bin or .log files.");
            return result;
        }
        if (source.size() < 0 || source.size() > MaximumInputBytes) {
            result.error = QStringLiteral("The DataFlash source exceeds the 1 GiB MATLAB input bound.");
            return result;
        }

        auto data = std::make_shared<Plan::Data>();
        data->sourcePath = source.absoluteFilePath();
        data->canonicalSource = source.canonicalFilePath();
        data->sourceBytes = source.size();
        data->modifiedUtc = source.lastModified().toUTC();
        data->text = suffix == QStringLiteral("log");
        data->cellLocale = QLocale();
        const qint64 progressTotal = qMax<qint64>(1, data->sourceBytes * 2);

        QFile first(data->sourcePath);
        if (!first.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not open the DataFlash source: %1")
                               .arg(first.errorString());
            return result;
        }
        DataFlashRaw::Reader firstReader(&first, data->text);
        QSet<int> schemaIds;
        bool repeatedSchema = false;
        bool sawFmtu = false;
        int firmwareCandidates = 0;
        bool firmwareRecoveryPending = false;
        bool firmwarePrimed = data->text;
        QByteArray raw;
        int kind = -1;
        while (firstReader.next(&raw, &kind)) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (first.pos() > data->sourceBytes) {
                result.error = QStringLiteral("The DataFlash source grew while the MATLAB plan was prepared.");
                return result;
            }
            if (omittedLogicalRecord(data->text, kind, raw, first))
                continue;
            if (++data->records > MaximumRecords) {
                result.error = QStringLiteral("The DataFlash source exceeds the 20-million-record MATLAB bound.");
                return result;
            }
            if (kind == 0) {
                const Definition definition = firstReader.currentDefinition;
                if (schemaIds.contains(definition.id)) {
                    repeatedSchema = true;
                } else {
                    schemaIds.insert(definition.id);
                    if (definition.name != QByteArrayLiteral("PARM")) {
                        SchemaRecord schema;
                        schema.definition = definition;
                        schema.labelName = definition.name + QByteArrayLiteral("_label");
                        schema.labels.append(QStringLiteral("LineNo"));
                        for (const QByteArray &column : definition.columns)
                            schema.labels.append(QString::fromLatin1(dotNetAscii(column).trimmed()));
                        data->schemas.append(std::move(schema));
                    }
                }
            } else if (kind == 1) {
                sawFmtu = true;
                int formatId = -1;
                QByteArray units;
                QString error;
                if (!readFmtu(raw, firstReader.currentDefinition, data->text,
                              &formatId, &units, &error)) {
                    result.error = error;
                    return result;
                }
                data->unitsByFormat.insert(formatId, units);
            }

            // Before ProcessLog enumerates records, DFLogBuffer visits the
            // source-ordered MSG/PARM index to prime BinaryLog._firmware. Its
            // limit check occurs after candidate 100001 is read. An empty Z
            // candidate makes ReadMessage scan to the next decodable frame;
            // mirror that priming side effect without reproducing the later
            // synthetic duplicate data row.
            if (!firmwarePrimed) {
                const QByteArray name = firstReader.currentDefinition.name;
                const bool candidate = kind != 0
                    && firmwareCandidates < 100001
                    && (name == QByteArrayLiteral("MSG")
                        || name == QByteArrayLiteral("PARM"));
                if (candidate) {
                    ++firmwareCandidates;
                    firmwareRecoveryPending = true;
                }
                if (firmwareRecoveryPending) {
                    if (kind == 0) {
                        // A valid FMT is itself the recovered string record,
                        // but it cannot alter the firmware discriminator.
                        firmwareRecoveryPending = false;
                    } else if (kind != -2) {
                        QByteArray trialFirmware = data->firmware;
                        QList<QByteArray> ignoredItems;
                        bool emptyBlob = false;
                        QString ignoredError;
                        if (decodeItems(raw, firstReader.currentDefinition,
                                        false, &trialFirmware, &ignoredItems,
                                        &emptyBlob, &ignoredError)) {
                            data->firmware = trialFirmware;
                            firmwareRecoveryPending = false;
                        }
                    }
                }
                if (firmwareCandidates >= 100001
                    && !firmwareRecoveryPending) {
                    firmwarePrimed = true;
                }
            }
            if ((data->records & 1023) == 0)
                reportProgress(progress, first.pos(), progressTotal);
        }
        if (!firstReader.error.isEmpty()) {
            result.error = firstReader.error;
            return result;
        }
        if (first.pos() != data->sourceBytes) {
            result.error = QStringLiteral("The DataFlash source changed while the MATLAB plan was prepared.");
            return result;
        }
        data->digest = firstReader.hash.result();
        if (firstReader.blankLines)
            data->warnings.append(QStringLiteral("Ignored %1 blank DataFlash text line(s).")
                                      .arg(firstReader.blankLines));
        if (firstReader.trailingBytes)
            data->warnings.append(QStringLiteral("Ignored a %1-byte incomplete final DataFlash record.")
                                      .arg(firstReader.trailingBytes));
        if (repeatedSchema)
            data->warnings.append(QStringLiteral(
                "Repeated identical FMT definitions are represented by one label variable."));
        if (sawFmtu)
            data->warnings.append(QStringLiteral(
                "FMTU metadata is used for instance routing without emitting Mission Planner 10's malformed FMT-prefix labels."));
        reportProgress(progress, data->sourceBytes, progressTotal);

        QFile second(data->sourcePath);
        if (!second.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not reopen the DataFlash source: %1")
                               .arg(second.errorString());
            return result;
        }
        DataFlashRaw::Reader secondReader(&second, data->text);
        QHash<QByteArray, int> variableIndex;
        QHash<QByteArray, int> cellIndex;
        QHash<QString, int> parameterIndex;
        QSet<QByteArray> seenSet;
        QByteArray firmware = data->firmware;
        qint64 lineNumber = 0;
        while (secondReader.next(&raw, &kind)) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (second.pos() > data->sourceBytes) {
                result.error = QStringLiteral("The DataFlash source grew while the MATLAB plan was prepared.");
                return result;
            }
            if (omittedLogicalRecord(data->text, kind, raw, second))
                continue;
            ++lineNumber;
            if (kind == 0 || kind == 1 || kind == -2)
                continue;
            QList<QByteArray> items;
            bool emptyBlob = false;
            QString decodeError;
            if (!decodeItems(raw, secondReader.currentDefinition, data->text,
                             &firmware, &items, &emptyBlob, &decodeError)) {
                if (emptyBlob) {
                    ++data->emptyBlobRows;
                    continue;
                }
                result.error = decodeError;
                return result;
            }
            if (items.isEmpty())
                continue;
            const QByteArray baseType = items.first();
            const Definition &definition = secondReader.currentDefinition;
            if (baseType == QByteArrayLiteral("PARM")) {
                const int nameIndex = definition.columns.indexOf(QByteArrayLiteral("Name")) + 1;
                const int valueIndex = definition.columns.indexOf(QByteArrayLiteral("Value")) + 1;
                double value = 0;
                if (nameIndex > 0 && valueIndex > 0
                    && nameIndex < items.size() && valueIndex < items.size()
                    && invariantFloatNumber(items.at(valueIndex), &value)) {
                    const QString name = QString::fromLatin1(items.at(nameIndex));
                    auto found = parameterIndex.find(name);
                    if (found == parameterIndex.end()) {
                        if (data->parameters.size() >= MaximumParameters) {
                            result.error = QStringLiteral("The MATLAB parameter count exceeds 10000.");
                            return result;
                        }
                        parameterIndex.insert(name, data->parameters.size());
                        data->parameters.append({name, value});
                    } else {
                        data->parameters[found.value()].value = value;
                    }
                }
                continue;
            }
            if (items.size() < 2
                || items.size() != definition.columns.size() + 1) {
                ++data->skippedRows;
                continue;
            }
            if (!seenSet.contains(baseType)) {
                seenSet.insert(baseType);
                data->seen.append(baseType);
            }
            const QByteArray folded = baseType.toLower();
            if (folded == QByteArrayLiteral("msg")
                || baseType.toUpper() == QByteArrayLiteral("ISBD")) {
                int table = cellIndex.value(baseType, -1);
                if (table < 0) {
                    table = data->cells.size();
                    cellIndex.insert(baseType, table);
                    data->cells.append({baseType, baseType + '1', 0, 0});
                }
                CellValue row;
                qint64 bytes = 0;
                QString cellError;
                if (!buildCustomRow(items, data->cellLocale, &row, &bytes,
                                    &cellError)) {
                    result.error = cellError;
                    return result;
                }
                CellTable &cell = data->cells[table];
                if (++cell.rows > MaximumCellRecords
                    || bytes > MatFileWriter::MaximumFileBytes
                           - cell.encodedChildrenBytes) {
                    result.error = QStringLiteral("The MATLAB MSG/ISBD cell bound was exceeded.");
                    return result;
                }
                cell.encodedChildrenBytes += bytes;
            }

            QByteArray variableName = baseType;
            const QByteArray unitIds = data->unitsByFormat.value(definition.id);
            const int marker = unitIds.indexOf('#');
            if (marker >= 0) {
                const int itemIndex = marker + 1;
                if (itemIndex <= 0 || itemIndex >= items.size()) {
                    result.error = QStringLiteral("FMTU instance metadata addresses a missing field.");
                    return result;
                }
                variableName += '_' + items.at(itemIndex);
            }
            if (!validMatName(variableName)) {
                result.error = QStringLiteral("A DataFlash instance creates an invalid MATLAB variable name.");
                return result;
            }
            int variable = variableIndex.value(variableName, -1);
            if (variable < 0) {
                if (data->numeric.size() >= MaximumVariables) {
                    result.error = QStringLiteral("The MATLAB numeric variable count exceeds 4096.");
                    return result;
                }
                variable = data->numeric.size();
                variableIndex.insert(variableName, variable);
                data->numeric.append({variableName, baseType, definition.id, 0,
                                      items.size()});
            } else if (data->numeric.at(variable).columns != items.size()
                       || data->numeric.at(variable).definitionId != definition.id) {
                result.error = QStringLiteral("A MATLAB variable has inconsistent DataFlash schemas.");
                return result;
            }
            ++data->numeric[variable].rows;

            if ((lineNumber & 1023) == 0)
                reportProgress(progress, data->sourceBytes + second.pos(), progressTotal);
        }
        if (!secondReader.error.isEmpty()) {
            result.error = secondReader.error;
            return result;
        }
        if (lineNumber != data->records || secondReader.hash.result() != data->digest
            || !sourceMatches(data->sourcePath, data->canonicalSource,
                              data->sourceBytes, data->modifiedUtc)) {
            result.error = QStringLiteral("The DataFlash source changed while the MATLAB plan was prepared.");
            return result;
        }
        if (data->skippedRows)
            data->warnings.append(QStringLiteral("Skipped %1 record(s) whose text fields did not match their FMT schema.")
                                      .arg(data->skippedRows));
        if (data->emptyBlobRows)
            data->warnings.append(QStringLiteral("Skipped %1 empty binary string record(s), matching the reference decoder failure.")
                                      .arg(data->emptyBlobRows));
        if (!data->seen.isEmpty())
            data->warnings.append(QStringLiteral(
                "Seen message types use deterministic first-seen order; the reference Hashtable order is runtime-specific."));

        QCollator collator(data->cellLocale);
        collator.setCaseSensitivity(Qt::CaseSensitive);
        std::sort(data->parameters.begin(), data->parameters.end(),
                  [&](const ParameterValue &left, const ParameterValue &right) {
            const int compared = collator.compare(left.name, right.name);
            return compared == 0 ? left.name < right.name : compared < 0;
        });

        QSet<QByteArray> topNames;
        const auto admitName = [&](const QByteArray &name) {
            if (!validMatName(name) || topNames.contains(name))
                return false;
            topNames.insert(name);
            return true;
        };
        for (const SchemaRecord &schema : std::as_const(data->schemas)) {
            if (!admitName(schema.labelName)) {
                result.error = QStringLiteral("MATLAB top-level label names collide.");
                return result;
            }
        }
        for (const NumericVariable &variable : std::as_const(data->numeric)) {
            if (!admitName(variable.name)) {
                result.error = QStringLiteral("MATLAB top-level data names collide.");
                return result;
            }
        }
        for (const CellTable &cell : std::as_const(data->cells)) {
            if (!admitName(cell.name)) {
                result.error = QStringLiteral("MATLAB top-level cell names collide.");
                return result;
            }
        }
        if (!admitName(QByteArrayLiteral("PARM"))
            || !admitName(QByteArrayLiteral("Seen"))
            || topNames.size() > MaximumVariables) {
            result.error = QStringLiteral("MATLAB reserved names collide or the total variable bound was exceeded.");
            return result;
        }

        qint64 estimate = 128;
        for (const SchemaRecord &schema : std::as_const(data->schemas)) {
            qint64 bytes = cellMatrixBase(schema.labelName);
            for (const QString &label : schema.labels)
                if (!addBounded(cellTextBytes(label), &bytes)) { estimate = -1; break; }
            if (estimate < 0 || !validTopLevelMatrixBytes(bytes)
                || !addBounded(bytes, &estimate)) { estimate = -1; break; }
        }
        for (const NumericVariable &variable : std::as_const(data->numeric)) {
            if (estimate < 0 || variable.rows > std::numeric_limits<qint64>::max() / variable.columns
                || variable.rows * variable.columns > std::numeric_limits<qint64>::max() / 8) {
                estimate = -1; break;
            }
            const qint64 bytes = 56 + paddedToEight(variable.name.size())
                + variable.rows * variable.columns * 8;
            if (!validTopLevelMatrixBytes(bytes)
                || !addBounded(bytes, &estimate)) { estimate = -1; break; }
        }
        for (const CellTable &cell : std::as_const(data->cells)) {
            const qint64 bytes = cellMatrixBase(cell.name) + cell.encodedChildrenBytes;
            if (estimate < 0 || !validTopLevelMatrixBytes(bytes)
                || !addBounded(bytes, &estimate)) { estimate = -1; break; }
        }
        qint64 parameterBytes = cellMatrixBase(QByteArrayLiteral("PARM"));
        for (const ParameterValue &parameter : std::as_const(data->parameters)) {
            if (!addBounded(cellTextBytes(parameter.name), &parameterBytes)
                || !addBounded(64, &parameterBytes)) {
                estimate = -1; break;
            }
        }
        if (estimate >= 0 && (!validTopLevelMatrixBytes(parameterBytes)
            || !addBounded(parameterBytes, &estimate))) estimate = -1;
        qint64 seenBytes = cellMatrixBase(QByteArrayLiteral("Seen"));
        for (const QByteArray &seen : std::as_const(data->seen))
            if (!addBounded(cellTextBytes(QString::fromLatin1(seen.trimmed())), &seenBytes)) { estimate = -1; break; }
        if (estimate >= 0 && (!validTopLevelMatrixBytes(seenBytes)
            || !addBounded(seenBytes, &estimate))) estimate = -1;
        if (estimate < 0 || estimate > MatFileWriter::MaximumFileBytes) {
            result.error = QStringLiteral(
                "A planned MATLAB matrix or file exceeds the Level-5/64 GiB output bounds.");
            return result;
        }
        data->estimated = estimate;
        data->outputPath = data->sourcePath + QLatin1Char('-')
            + QString::number(data->records) + QStringLiteral(".mat");
        data->outputPath = QFileInfo(data->outputPath).absoluteFilePath();
        data->outputParentCanonical = QFileInfo(source.absolutePath()).canonicalFilePath();
        if (data->outputParentCanonical.isEmpty() || pathExists(data->outputPath)) {
            result.error = pathExists(data->outputPath)
                ? QStringLiteral("The exact derived MATLAB output already exists: %1")
                      .arg(data->outputPath)
                : QStringLiteral("The MATLAB output directory is unavailable.");
            return result;
        }

        // This is the last observer callback made by Prepare.  Revalidate the
        // exact source bytes and the no-overwrite destination afterwards so a
        // callback cannot leave the returned consent plan stale on arrival.
        reportProgress(progress, progressTotal, progressTotal);
        if (cancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        QByteArray finalDigest;
        QString verifyError;
        if (!hashFile(data->sourcePath, data->sourceBytes, cancel,
                      &finalDigest, &verifyError)) {
            if (cancelled(cancel))
                result.cancelled = true;
            else
                result.error = verifyError;
            return result;
        }
        if (finalDigest != data->digest
            || !sourceMatches(data->sourcePath, data->canonicalSource,
                              data->sourceBytes, data->modifiedUtc)) {
            result.error = QStringLiteral(
                "The DataFlash source changed before the MATLAB plan was returned.");
            return result;
        }
        if (QFileInfo(source.absolutePath()).canonicalFilePath()
                != data->outputParentCanonical
            || pathExists(data->outputPath)) {
            result.error = pathExists(data->outputPath)
                ? QStringLiteral("The exact derived MATLAB output appeared while the plan was prepared: %1")
                      .arg(data->outputPath)
                : QStringLiteral("The MATLAB output directory changed while the plan was prepared.");
            return result;
        }
        result.warnings = data->warnings;
        result.plan = std::make_shared<Plan>(Plan(data));
        result.success = true;
        return result;
    } catch (const std::bad_alloc &) {
        result.error = QStringLiteral("Insufficient memory for the bounded MATLAB plan.");
    } catch (...) {
        result.error = QStringLiteral("The MATLAB planning operation failed unexpectedly.");
    }
    return result;
}

DataFlashMatlabExporter::Result DataFlashMatlabExporter::Export(
    const Plan &plan, const CancelCheck &cancel, const Progress &progress)
{
    Result result;
    const std::shared_ptr<const Plan::Data> data = plan.d;
    if (!data) {
        result.error = QStringLiteral("The MATLAB export plan is invalid.");
        return result;
    }
    result.recordCount = data->records;
    result.variableCount = data->schemas.size() + data->numeric.size()
        + data->cells.size() + 2;
    result.warnings = data->warnings;

    try {
        const qint64 scanPasses = data->cells.size() + 3;
        const qint64 scanWork = data->sourceBytes > 0
            && scanPasses > std::numeric_limits<qint64>::max() / data->sourceBytes
            ? std::numeric_limits<qint64>::max()
            : data->sourceBytes * scanPasses;
        const qint64 totalWork = scanWork > MatFileWriter::MaximumFileBytes - data->estimated
            ? MatFileWriter::MaximumFileBytes : scanWork + data->estimated;
        qint64 completed = 0;

        if (cancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (!sourceMatches(data->sourcePath, data->canonicalSource,
                           data->sourceBytes, data->modifiedUtc)) {
            result.error = QStringLiteral("The DataFlash source changed after MATLAB consent.");
            return result;
        }
        QByteArray initialDigest;
        QString verifyError;
        if (!hashFile(data->sourcePath, data->sourceBytes, cancel,
                      &initialDigest, &verifyError)) {
            if (cancelled(cancel))
                result.cancelled = true;
            else
                result.error = verifyError;
            return result;
        }
        if (initialDigest != data->digest) {
            result.error = QStringLiteral("The DataFlash source bytes changed after MATLAB consent.");
            return result;
        }
        completed += data->sourceBytes;
        reportProgress(progress, completed, totalWork);

        const QString currentParent = QFileInfo(
            QFileInfo(data->outputPath).absolutePath()).canonicalFilePath();
        if (currentParent.isEmpty()
            || currentParent != data->outputParentCanonical) {
            result.error = QStringLiteral("The MATLAB output directory changed after consent.");
            return result;
        }
        if (pathExists(data->outputPath)) {
            result.error = QStringLiteral("The exact derived MATLAB output already exists: %1")
                               .arg(data->outputPath);
            return result;
        }

        const QString templateName = QFileInfo(data->outputPath).absolutePath()
            + QDir::separator() + QLatin1Char('.')
            + QFileInfo(data->outputPath).fileName()
            + QStringLiteral(".stage-XXXXXX");
        QTemporaryFile stage(templateName);
        stage.setAutoRemove(false);
        if (!stage.open()) {
            result.error = QStringLiteral("Could not create a private MATLAB staging file: %1")
                               .arg(stage.errorString());
            return result;
        }
        const QString stagePath = stage.fileName();
        const StageIdentity stageIdentity = StageIdentity::capture(&stage,
                                                                   stagePath);
        if (!stageIdentity.valid) {
            // No observer has run since QTemporaryFile created this pathname,
            // so remove it immediately rather than leaving an untracked stage.
            stage.remove();
            result.error = QStringLiteral(
                "Could not establish ownership of the private MATLAB staging file.");
            return result;
        }
        OwnedStageCleanup stageCleanup{stagePath, stageIdentity};
        MatFileWriter writer(&stage);
        QString writerError;
        if (!writer.begin(&writerError)) {
            result.error = writerError;
            return result;
        }

        for (const SchemaRecord &schema : data->schemas) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (!writer.appendCellMatrix(schema.labelName, schema.labels.size(), 1,
                    [&](qint64 index, CellValue *value, QString *error) {
                        if (cancelled(cancel)) {
                            *error = QStringLiteral("MATLAB export cancelled while writing labels.");
                            return false;
                        }
                        value->type = CellValue::Type::Text;
                        value->text = schema.labels.at(int(index));
                        return true;
                    }, &writerError)) {
                if (cancelled(cancel))
                    result.cancelled = true;
                else
                    result.error = writerError;
                return result;
            }
        }

        QVector<MatFileWriter::Matrix> matrices;
        matrices.reserve(data->numeric.size());
        for (const NumericVariable &variable : data->numeric) {
            MatFileWriter::Matrix matrix;
            if (!writer.reserveDoubleMatrix(variable.name, variable.rows,
                                             variable.columns, &matrix,
                                             &writerError)) {
                result.error = writerError;
                return result;
            }
            matrices.append(matrix);
        }

        for (const CellTable &table : data->cells) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            QFile cellSource(data->sourcePath);
            if (!cellSource.open(QIODevice::ReadOnly)) {
                result.error = QStringLiteral("Could not reopen the DataFlash source for %1 cells: %2")
                                   .arg(QString::fromLatin1(table.type), cellSource.errorString());
                return result;
            }
            DataFlashRaw::Reader reader(&cellSource, data->text);
            QByteArray firmware = data->firmware;
            QByteArray raw;
            int kind = -1;
            qint64 provided = 0;
            const qint64 passBase = completed;
            if (!writer.appendCellMatrix(table.name, 1, table.rows,
                    [&](qint64 index, CellValue *value, QString *error) {
                        if (index != provided) {
                            *error = QStringLiteral("The MATLAB cell provider was requested out of order.");
                            return false;
                        }
                        while (reader.next(&raw, &kind)) {
                            if (cancelled(cancel)) {
                                *error = QStringLiteral("MATLAB export cancelled while streaming text cells.");
                                return false;
                            }
                            if (cellSource.pos() > data->sourceBytes) {
                                *error = QStringLiteral("The DataFlash source grew while text cells were streamed.");
                                return false;
                            }
                            if (omittedLogicalRecord(data->text, kind, raw, cellSource)
                                || kind == 0 || kind == 1 || kind == -2) {
                                continue;
                            }
                            QList<QByteArray> items;
                            bool emptyBlob = false;
                            QString decodeError;
                            if (!decodeItems(raw, reader.currentDefinition, data->text,
                                             &firmware, &items, &emptyBlob,
                                             &decodeError)) {
                                if (emptyBlob)
                                    continue;
                                *error = decodeError;
                                return false;
                            }
                            if (items.size() < 2
                                || items.size() != reader.currentDefinition.columns.size() + 1
                                || items.first() != table.type) {
                                continue;
                            }
                            qint64 bytes = 0;
                            if (!buildCustomRow(items, data->cellLocale, value,
                                                &bytes, error)) {
                                return false;
                            }
                            ++provided;
                            if ((provided & 255) == 0)
                                reportProgress(progress, passBase + cellSource.pos(), totalWork);
                            return true;
                        }
                        if (!reader.error.isEmpty())
                            *error = reader.error;
                        else
                            *error = QStringLiteral("The DataFlash source no longer contains the planned text cells.");
                        return false;
                    }, &writerError)) {
                if (cancelled(cancel))
                    result.cancelled = true;
                else
                    result.error = writerError;
                return result;
            }
            if (provided != table.rows) {
                result.error = QStringLiteral("The DataFlash text-cell count changed after consent.");
                return result;
            }
            completed += data->sourceBytes;
            reportProgress(progress, completed, totalWork);
        }

        if (!writer.appendCellMatrix(QByteArrayLiteral("PARM"),
                                     data->parameters.size(), 2,
                [&](qint64 index, CellValue *value, QString *error) {
                    if (cancelled(cancel)) {
                        *error = QStringLiteral("MATLAB export cancelled while writing parameters.");
                        return false;
                    }
                    const qint64 rows = data->parameters.size();
                    const ParameterValue &parameter = data->parameters.at(
                        int(index < rows ? index : index - rows));
                    if (index < rows) {
                        value->type = CellValue::Type::Text;
                        value->text = parameter.name;
                    } else {
                        value->type = CellValue::Type::Scalar;
                        value->number = parameter.value;
                    }
                    return true;
                }, &writerError)) {
            if (cancelled(cancel))
                result.cancelled = true;
            else
                result.error = writerError;
            return result;
        }
        if (!writer.appendCellMatrix(QByteArrayLiteral("Seen"), data->seen.size(), 1,
                [&](qint64 index, CellValue *value, QString *error) {
                    if (cancelled(cancel)) {
                        *error = QStringLiteral("MATLAB export cancelled while writing Seen.");
                        return false;
                    }
                    value->type = CellValue::Type::Text;
                    value->text = QString::fromLatin1(data->seen.at(int(index)).trimmed());
                    return true;
                }, &writerError)) {
            if (cancelled(cancel))
                result.cancelled = true;
            else
                result.error = writerError;
            return result;
        }

        struct NumericBuffer {
            QVector<QVector<double>> columns;
            qint64 flushedRows = 0;
            qint64 observedRows = 0;
            qint64 values = 0;
        };
        QVector<NumericBuffer> buffers(data->numeric.size());
        QHash<QByteArray, int> numericIndex;
        for (int index = 0; index < data->numeric.size(); ++index)
            numericIndex.insert(data->numeric.at(index).name, index);
        qint64 bufferedValues = 0;
        auto flush = [&](int index, QString *error) {
            NumericBuffer &buffer = buffers[index];
            if (!buffer.values)
                return true;
            const int rows = buffer.columns.first().size();
            for (int column = 0; column < buffer.columns.size(); ++column) {
                if (buffer.columns.at(column).size() != rows
                    || !writer.writeDoubleColumn(matrices.at(index), column,
                                                 buffer.flushedRows,
                                                 buffer.columns.at(column),
                                                 error)) {
                    return false;
                }
            }
            buffer.flushedRows += rows;
            bufferedValues -= buffer.values;
            buffer.values = 0;
            for (QVector<double> &column : buffer.columns) {
                QVector<double> released;
                released.swap(column);
            }
            return true;
        };

        QFile numericSource(data->sourcePath);
        if (!numericSource.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not reopen the DataFlash source for numeric matrices: %1")
                               .arg(numericSource.errorString());
            return result;
        }
        DataFlashRaw::Reader numericReader(&numericSource, data->text);
        QByteArray firmware = data->firmware;
        QByteArray raw;
        int kind = -1;
        qint64 lineNumber = 0;
        const qint64 numericBase = completed;
        while (numericReader.next(&raw, &kind)) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            if (numericSource.pos() > data->sourceBytes) {
                result.error = QStringLiteral("The DataFlash source grew while numeric matrices were streamed.");
                return result;
            }
            if (omittedLogicalRecord(data->text, kind, raw, numericSource))
                continue;
            ++lineNumber;
            if (kind == 0 || kind == 1 || kind == -2)
                continue;
            QList<QByteArray> items;
            bool emptyBlob = false;
            QString decodeError;
            if (!decodeItems(raw, numericReader.currentDefinition, data->text,
                             &firmware, &items, &emptyBlob, &decodeError)) {
                if (emptyBlob)
                    continue;
                result.error = decodeError;
                return result;
            }
            if (items.isEmpty() || items.first() == QByteArrayLiteral("PARM")
                || items.size() < 2
                || items.size() != numericReader.currentDefinition.columns.size() + 1) {
                continue;
            }
            QByteArray variableName = items.first();
            const QByteArray units = data->unitsByFormat.value(
                numericReader.currentDefinition.id);
            const int marker = units.indexOf('#');
            if (marker >= 0) {
                const int itemIndex = marker + 1;
                if (itemIndex <= 0 || itemIndex >= items.size()) {
                    result.error = QStringLiteral("FMTU instance metadata changed after consent.");
                    return result;
                }
                variableName += '_' + items.at(itemIndex);
            }
            const int index = numericIndex.value(variableName, -1);
            if (index < 0 || data->numeric.at(index).definitionId
                    != numericReader.currentDefinition.id
                || data->numeric.at(index).columns != items.size()) {
                result.error = QStringLiteral("The DataFlash numeric schema changed after consent.");
                return result;
            }
            NumericBuffer &buffer = buffers[index];
            if (buffer.columns.isEmpty())
                buffer.columns.resize(items.size());
            buffer.columns[0].append(double(lineNumber));
            for (int column = 1; column < items.size(); ++column) {
                double value = 0;
                if (!invariantNumber(items.at(column), &value))
                    value = 0;
                buffer.columns[column].append(value);
            }
            ++buffer.observedRows;
            buffer.values += items.size();
            bufferedValues += items.size();
            if (buffer.columns.first().size()
                    >= MatFileWriter::MaximumWriteChunkValues) {
                if (!flush(index, &writerError)) {
                    result.error = writerError;
                    return result;
                }
            } else if (bufferedValues > NumericBufferValues) {
                int largest = index;
                for (int candidate = 0; candidate < buffers.size(); ++candidate)
                    if (buffers.at(candidate).values > buffers.at(largest).values)
                        largest = candidate;
                if (!flush(largest, &writerError)) {
                    result.error = writerError;
                    return result;
                }
            }
            if ((lineNumber & 1023) == 0)
                reportProgress(progress, numericBase + numericSource.pos(), totalWork);
        }
        if (!numericReader.error.isEmpty()) {
            result.error = numericReader.error;
            return result;
        }
        if (numericSource.pos() != data->sourceBytes
            || lineNumber != data->records
            || numericReader.hash.result() != data->digest) {
            result.error = QStringLiteral("The DataFlash source changed while numeric matrices were streamed.");
            return result;
        }
        for (int index = 0; index < buffers.size(); ++index) {
            if (!flush(index, &writerError)) {
                result.error = writerError;
                return result;
            }
            if (buffers.at(index).observedRows != data->numeric.at(index).rows
                || buffers.at(index).flushedRows != data->numeric.at(index).rows) {
                result.error = QStringLiteral("The DataFlash row count changed after consent.");
                return result;
            }
        }
        completed += data->sourceBytes;
        reportProgress(progress, completed, totalWork);

        if (!writer.finish(&writerError)) {
            result.error = writerError;
            return result;
        }
        if (!stage.flush()) {
            result.error = QStringLiteral("Could not flush the staged MATLAB output: %1")
                               .arg(stage.errorString());
            return result;
        }
        if (stage.size() != data->estimated) {
            result.error = QStringLiteral("The staged MATLAB layout differs from its consented size.");
            return result;
        }

        completed += data->sourceBytes;
        reportProgress(progress, completed, totalWork);
        if (cancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        QByteArray finalDigest;
        if (!hashFile(data->sourcePath, data->sourceBytes, cancel,
                      &finalDigest, &verifyError)) {
            if (cancelled(cancel))
                result.cancelled = true;
            else
                result.error = verifyError;
            return result;
        }
        if (finalDigest != data->digest
            || !sourceMatches(data->sourcePath, data->canonicalSource,
                              data->sourceBytes, data->modifiedUtc)) {
            result.error = QStringLiteral("The DataFlash source changed before MATLAB publication.");
            return result;
        }
        if (pathExists(data->outputPath)) {
            result.error = QStringLiteral("The MATLAB output appeared after consent and was not replaced.");
            return result;
        }
        const QFileInfo staged(stagePath);
        if (!staged.isFile() || staged.isSymLink()
            || staged.size() != data->estimated
            || !stageIdentity.matches(stagePath)) {
            result.error = QStringLiteral("The private MATLAB staging file changed before publication.");
            return result;
        }
        if (QFileInfo(QFileInfo(data->outputPath).absolutePath()).canonicalFilePath()
                != data->outputParentCanonical) {
            result.error = QStringLiteral("The MATLAB output directory changed before publication.");
            return result;
        }
        QString successPath = data->outputPath;
        const qint64 successBytes = data->estimated;
        QString publishError;
#ifdef Q_OS_WIN
        // MoveFileEx requires the CRT-owned QFile handle to be closed.  The
        // captured file ID still prevents cleanup from removing a replacement.
        stage.close();
#endif
        if (!publishNoReplace(stagePath, data->outputPath, stageIdentity,
                              stage.handle(), &publishError)) {
            result.error = publishError;
            return result;
        }
        // Vacate the old random name before the completion observer runs. If
        // that observer creates a new file there, the destructor's identity
        // check will leave the new owner's file untouched.
        stageCleanup.removeOwned();
        result.outputPath = std::move(successPath);
        result.bytesWritten = successBytes;
        result.success = true;
        try {
            reportProgress(progress, totalWork, totalWork);
        } catch (...) {
            // Publication is already complete; observer failure cannot make
            // the publication receipt or no-overwrite result untrue.
        }
        return result;
    } catch (const std::bad_alloc &) {
        result.error = QStringLiteral("Insufficient memory for the bounded MATLAB export.");
    } catch (...) {
        result.error = QStringLiteral("The MATLAB export failed unexpectedly.");
    }
    return result;
}
