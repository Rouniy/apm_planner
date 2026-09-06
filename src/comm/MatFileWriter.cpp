#include "MatFileWriter.h"

#include <QHash>
#include <QIODevice>
#include <QPointer>
#include <QSet>
#include <QtEndian>

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr quint32 MiInt8 = 1;
constexpr quint32 MiUInt16 = 4;
constexpr quint32 MiInt32 = 5;
constexpr quint32 MiUInt32 = 6;
constexpr quint32 MiDouble = 9;
constexpr quint32 MiMatrix = 14;
constexpr quint32 MxDoubleClass = 6;
constexpr quint32 MxCellClass = 1;
constexpr quint32 MxCharClass = 4;
constexpr int MaximumMatrices = 65536;
std::atomic<quint64> NextWriterToken{1};

void clearError(QString *error)
{
    if (error) {
        error->clear();
    }
}

void appendUInt32(QByteArray *bytes, quint32 value)
{
    uchar encoded[sizeof(value)]{};
    qToLittleEndian(value, encoded);
    bytes->append(reinterpret_cast<const char *>(encoded), sizeof(encoded));
}

void appendInt32(QByteArray *bytes, qint32 value)
{
    appendUInt32(bytes, quint32(value));
}

bool validUtf8Name(const QByteArray &name)
{
    if (name.isEmpty() || name.size() > MatFileWriter::MaximumNameBytes
        || name.contains('\0')) {
        return false;
    }
    return QString::fromUtf8(name.constData(), name.size()).toUtf8() == name;
}

qint64 paddedToEight(qint64 bytes)
{
    return (bytes + 7) & ~qint64(7);
}

bool dimensions(qint64 rows, qint64 columns)
{
    return rows >= 0 && columns >= 0
        && rows <= std::numeric_limits<qint32>::max()
        && columns <= std::numeric_limits<qint32>::max();
}

QByteArray matrixHeader(const QByteArray &name, quint32 type, qint64 rows,
                       qint64 columns, quint32 payload)
{
    QByteArray bytes;
    appendUInt32(&bytes, MiMatrix); appendUInt32(&bytes, payload);
    appendUInt32(&bytes, MiUInt32); appendUInt32(&bytes, 8);
    appendUInt32(&bytes, type); appendUInt32(&bytes, 0);
    appendUInt32(&bytes, MiInt32); appendUInt32(&bytes, 8);
    appendInt32(&bytes, qint32(rows)); appendInt32(&bytes, qint32(columns));
    appendUInt32(&bytes, MiInt8); appendUInt32(&bytes, quint32(name.size()));
    bytes += name; bytes += QByteArray(int(paddedToEight(name.size()) - name.size()), '\0');
    return bytes;
}

bool measureCell(const MatFileWriter::CellValue &value, int depth,
                 int *nodes, qint64 *size, QString *error)
{
    using Type = MatFileWriter::CellValue::Type;
    if (depth > MatFileWriter::MaximumCellDepth || ++*nodes > MatFileWriter::MaximumCellNodes) {
        *error = QStringLiteral("MAT cell value exceeds its nesting/node bound."); return false;
    }
    qint64 total = 48; // Unnamed miMATRIX header, flags, dimensions and name.
    switch (value.type) {
    case Type::Scalar: total += 16; break;
    case Type::Text: total += 8 + paddedToEight(qint64(value.text.size()) * 2); break;
    case Type::Cell:
        if (!dimensions(value.rows, value.columns)
            || value.rows * value.columns != value.children.size()) {
            *error = QStringLiteral("Nested MAT cell dimensions do not match its children."); return false;
        }
        for (const auto &child : value.children) {
            qint64 childBytes = 0;
            if (!measureCell(child, depth + 1, nodes, &childBytes, error)) return false;
            total += childBytes;
            if (total > MatFileWriter::MaximumCellValueBytes) break;
        }
        break;
    default: *error = QStringLiteral("MAT cell value has an invalid type."); return false;
    }
    if (value.type != Type::Cell && !value.children.isEmpty()) {
        *error = QStringLiteral("Only nested MAT cells may contain children."); return false;
    }
    if (total > MatFileWriter::MaximumCellValueBytes) {
        *error = QStringLiteral("MAT cell value exceeds its 8-MiB encoded bound."); return false;
    }
    *size = total; return true;
}

void encodeCell(const MatFileWriter::CellValue &value, QByteArray *bytes)
{
    using Type = MatFileWriter::CellValue::Type;
    const int start = bytes->size();
    const quint32 type = value.type == Type::Scalar ? MxDoubleClass
        : value.type == Type::Text ? MxCharClass : MxCellClass;
    const qint64 rows = value.type == Type::Cell ? value.rows
        : value.type == Type::Text && value.text.isEmpty() ? 0 : 1;
    const qint64 columns = value.type == Type::Cell ? value.columns
        : value.type == Type::Text ? value.text.size() : 1;
    *bytes += matrixHeader({}, type, rows, columns, 0);
    if (value.type == Type::Scalar) {
        appendUInt32(bytes, MiDouble); appendUInt32(bytes, 8);
        quint64 bits; std::memcpy(&bits, &value.number, sizeof(bits));
        uchar raw[8]; qToLittleEndian(bits, raw);
        bytes->append(reinterpret_cast<const char *>(raw), 8);
    } else if (value.type == Type::Text) {
        appendUInt32(bytes, MiUInt16); appendUInt32(bytes, quint32(value.text.size()) * 2);
        for (QChar character : value.text) {
            uchar raw[2]; qToLittleEndian(character.unicode(), raw);
            bytes->append(reinterpret_cast<const char *>(raw), 2);
        }
        *bytes += QByteArray(int(paddedToEight(qint64(value.text.size()) * 2) - qint64(value.text.size()) * 2), '\0');
    } else for (const auto &child : value.children) encodeCell(child, bytes);
    qToLittleEndian(quint32(bytes->size() - start - 8), reinterpret_cast<uchar *>(bytes->data() + start + 4));
}

} // namespace

struct MatFileWriter::Private
{
    struct Record {
        Matrix matrix;
        QHash<qint64, qint64> nextRowByColumn;
        qint64 completedColumns = 0;
    };

    QPointer<QFileDevice> file;
    bool alive = true;
    bool cellWriting = false;
    bool begun = false;
    bool finished = false;
    bool ioFailed = false;
    QString ioError;
    qint64 nextOffset = 0;
    quint64 writerToken = 0;
    quint64 nextMatrixToken = 1;
    QHash<qint64, Record> records;
    QSet<QByteArray> names;

    bool failIo(const QString &message, QString *error)
    {
        ioFailed = true;
        ioError = message;
        if (error) {
            *error = message;
        }
        return false;
    }

    bool reject(const QString &message, QString *error) const
    {
        if (error) {
            *error = message;
        }
        return false;
    }

    bool ready(QString *error)
    {
        if (cellWriting) {
            return failIo(QStringLiteral("Reentrant MAT writer operations are not allowed during a cell provider."), error);
        }
        if (!begun) {
            return reject(QStringLiteral("The MAT writer has not been started."), error);
        }
        if (finished) {
            return reject(QStringLiteral("The MAT writer is already finished."), error);
        }
        if (ioFailed) {
            return reject(ioError, error);
        }
        if (!file || !file->isOpen() || !(file->openMode() & QIODevice::WriteOnly)
            || file->isSequential()) {
            return reject(QStringLiteral("The MAT output device is no longer writable and random-access."), error);
        }
        if (file->size() != nextOffset) {
            return reject(QStringLiteral("The MAT output device changed outside the writer."), error);
        }
        return true;
    }

    bool writeAt(qint64 offset, const char *data, qint64 size, QString *error)
    {
        const QPointer<QFileDevice> device(file);
        if (!alive || !device || !device->seek(offset) || !alive || !device) {
            return failIo(QStringLiteral("Could not seek in the MAT output: %1")
                              .arg(device ? device->errorString() : QStringLiteral("device destroyed")), error);
        }
        qint64 written = 0;
        while (written < size) {
            const qint64 count = device->write(data + written, size - written);
            if (count <= 0 || !alive || !device) {
                return failIo(QStringLiteral("Could not write the MAT output: %1")
                                  .arg(device ? device->errorString() : QStringLiteral("device destroyed")), error);
            }
            written += count;
        }
        return true;
    }

    bool writeAt(qint64 offset, const QByteArray &bytes, QString *error)
    {
        return writeAt(offset, bytes.constData(), bytes.size(), error);
    }
};

MatFileWriter::MatFileWriter(QFileDevice *file)
    : d(new Private)
{
    d->file = file;
    d->writerToken = NextWriterToken.fetch_add(1, std::memory_order_relaxed);
    if (d->writerToken == 0) {
        d->writerToken = NextWriterToken.fetch_add(1, std::memory_order_relaxed);
    }
}

MatFileWriter::~MatFileWriter() { d->alive = false; }

bool MatFileWriter::begin(QString *error)
{
    clearError(error);
    if (d->begun) {
        return d->reject(QStringLiteral("The MAT writer can only be started once."), error);
    }
    if (!d->file || !d->file->isOpen()
        || !(d->file->openMode() & QIODevice::WriteOnly)
        || (d->file->openMode() & (QIODevice::Append | QIODevice::Text))
        || d->file->isSequential()) {
        return d->reject(QStringLiteral(
            "MAT output must be an open random-access binary writable file."), error);
    }
    if (d->file->size() != 0 || !d->file->seek(0)) {
        return d->reject(QStringLiteral("MAT output must be a new empty file."), error);
    }

    d->begun = true;
    d->nextOffset = 128;
    QByteArray header(128, ' ');
    const QByteArray text = QByteArrayLiteral(
        "MATLAB 5.0 MAT-file, Platform: APM Planner 3.0, Created by MatFileWriter");
    header.replace(0, text.size(), text);
    for (int index = 116; index < 124; ++index) {
        header[index] = '\0';
    }
    header[124] = '\0';
    header[125] = '\1';
    header[126] = 'I';
    header[127] = 'M';
    if (!d->writeAt(0, header, error)) {
        return false;
    }
    if (d->file->size() != d->nextOffset) {
        return d->failIo(QStringLiteral("Could not establish the MAT header length."), error);
    }
    return true;
}

bool MatFileWriter::reserveDoubleMatrix(const QByteArray &name, qint64 rows,
                                        qint64 columns, Matrix *matrix,
                                        QString *error)
{
    clearError(error);
    if (matrix) {
        *matrix = {};
    }
    if (!d->ready(error)) {
        return false;
    }
    if (!matrix) {
        return d->reject(QStringLiteral("A MAT matrix receipt is required."), error);
    }
    if (!validUtf8Name(name)) {
        return d->reject(QStringLiteral(
            "MAT variable names must be non-empty valid UTF-8 without NUL and at most 127 bytes."), error);
    }
    if (d->names.contains(name)) {
        return d->reject(QStringLiteral("MAT variable names must be unique."), error);
    }
    if (d->names.size() >= MaximumMatrices) {
        return d->reject(QStringLiteral("The bounded MAT matrix count was exceeded."), error);
    }
    if (rows < 0 || columns < 0
        || rows > std::numeric_limits<qint32>::max()
        || columns > std::numeric_limits<qint32>::max()) {
        return d->reject(QStringLiteral("MAT matrix dimensions exceed Level-5 int32 bounds."), error);
    }
    if (rows != 0 && columns > std::numeric_limits<qint64>::max() / rows) {
        return d->reject(QStringLiteral("MAT matrix element count overflowed."), error);
    }
    const qint64 elements = rows * columns;
    if (elements > std::numeric_limits<qint64>::max() / qint64(sizeof(double))) {
        return d->reject(QStringLiteral("MAT matrix byte count overflowed."), error);
    }
    const qint64 dataBytes = elements * qint64(sizeof(double));
    const qint64 paddedNameBytes = paddedToEight(name.size());
    const qint64 matrixPayloadBytes = 48 + paddedNameBytes + dataBytes;
    if (matrixPayloadBytes > std::numeric_limits<quint32>::max()) {
        return d->reject(QStringLiteral(
            "A MAT matrix exceeds the Level-5 32-bit data-element bound."), error);
    }
    const qint64 matrixBytes = 8 + matrixPayloadBytes;
    if (d->nextOffset > MaximumFileBytes - matrixBytes) {
        return d->reject(QStringLiteral("The bounded 64-GiB MAT output size was exceeded."), error);
    }

    const qint64 outerOffset = d->nextOffset;
    const qint64 dataOffset = outerOffset + 56 + paddedNameBytes;
    const qint64 finalOffset = outerOffset + matrixBytes;
    QByteArray metadata;
    metadata.reserve(int(56 + paddedNameBytes));
    appendUInt32(&metadata, MiMatrix);
    appendUInt32(&metadata, quint32(matrixPayloadBytes));
    appendUInt32(&metadata, MiUInt32);
    appendUInt32(&metadata, 8);
    appendUInt32(&metadata, MxDoubleClass);
    appendUInt32(&metadata, 0);
    appendUInt32(&metadata, MiInt32);
    appendUInt32(&metadata, 8);
    appendInt32(&metadata, qint32(rows));
    appendInt32(&metadata, qint32(columns));
    appendUInt32(&metadata, MiInt8);
    appendUInt32(&metadata, quint32(name.size()));
    metadata += name;
    metadata += QByteArray(int(paddedNameBytes - name.size()), '\0');
    appendUInt32(&metadata, MiDouble);
    appendUInt32(&metadata, quint32(dataBytes));
    if (metadata.size() != dataOffset - outerOffset) {
        return d->failIo(QStringLiteral("Internal MAT matrix layout error."), error);
    }
    if (!d->file->resize(finalOffset)) {
        return d->failIo(QStringLiteral("Could not reserve the MAT matrix: %1")
                             .arg(d->file->errorString()), error);
    }
    d->nextOffset = finalOffset;
    if (!d->writeAt(outerOffset, metadata, error)) {
        return false;
    }

    Private::Record record;
    record.matrix.dataOffset = dataOffset;
    record.matrix.rows = rows;
    record.matrix.columns = columns;
    record.matrix.writerToken = d->writerToken;
    record.matrix.matrixToken = d->nextMatrixToken++;
    if (rows == 0 || columns == 0) {
        record.completedColumns = columns;
    }
    d->records.insert(dataOffset, record);
    d->names.insert(name);
    *matrix = record.matrix;
    return true;
}

bool MatFileWriter::writeDoubleColumn(const Matrix &matrix, qint64 column,
                                      qint64 firstRow,
                                      const QVector<double> &values,
                                      QString *error)
{
    clearError(error);
    if (!d->ready(error)) {
        return false;
    }
    auto iterator = d->records.find(matrix.dataOffset);
    if (iterator == d->records.end()
        || iterator->matrix.dataOffset != matrix.dataOffset
        || iterator->matrix.rows != matrix.rows
        || iterator->matrix.columns != matrix.columns
        || matrix.writerToken != d->writerToken
        || matrix.matrixToken != iterator->matrix.matrixToken) {
        return d->reject(QStringLiteral("The MAT matrix receipt is stale or forged."), error);
    }
    Private::Record &record = iterator.value();
    if (column < 0 || column >= record.matrix.columns || firstRow < 0) {
        return d->reject(QStringLiteral("The MAT matrix write range is invalid."), error);
    }
    const qint64 expectedRow = record.nextRowByColumn.value(column, 0);
    if (firstRow != expectedRow) {
        return d->reject(QStringLiteral(
            "MAT matrix rows must be written sequentially within each column."), error);
    }
    if (values.size() > record.matrix.rows - firstRow) {
        return d->reject(QStringLiteral("The MAT matrix write exceeds its reserved rows."), error);
    }
    if (values.isEmpty()) {
        return true;
    }

    const qint64 elementOffset = column * record.matrix.rows + firstRow;
    qint64 outputOffset = record.matrix.dataOffset + elementOffset * qint64(sizeof(double));
    int inputOffset = 0;
    while (inputOffset < values.size()) {
        const int count = qMin(MaximumWriteChunkValues, values.size() - inputOffset);
        QByteArray encoded(count * int(sizeof(double)), '\0');
        for (int index = 0; index < count; ++index) {
            quint64 bits = 0;
            const double value = values.at(inputOffset + index);
            std::memcpy(&bits, &value, sizeof(bits));
            qToLittleEndian(bits,
                reinterpret_cast<uchar *>(encoded.data() + index * int(sizeof(double))));
        }
        if (!d->writeAt(outputOffset, encoded, error)) {
            return false;
        }
        outputOffset += encoded.size();
        inputOffset += count;
    }
    const qint64 nextRow = firstRow + values.size();
    record.nextRowByColumn.insert(column, nextRow);
    if (nextRow == record.matrix.rows) {
        ++record.completedColumns;
    }
    return true;
}

bool MatFileWriter::appendCellMatrix(const QByteArray &name, qint64 rows,
                                    qint64 columns, CellProvider provider,
                                    QString *error)
{
    clearError(error);
    // A provider may synchronously delete the facade. Pin implementation and
    // callable independently; never access this/d after invoking user code.
    const auto state = d;
    if (!state->ready(error)) return false;
    if (!validUtf8Name(name) || state->names.contains(name))
        return state->reject(QStringLiteral("MAT cell names must be unique valid UTF-8, without NUL, at most 127 bytes."), error);
    if (state->names.size() >= MaximumMatrices)
        return state->reject(QStringLiteral("The bounded MAT matrix count was exceeded."), error);
    if (!dimensions(rows, columns))
        return state->reject(QStringLiteral("MAT cell dimensions exceed Level-5 int32 bounds."), error);
    const qint64 count = rows * columns;
    if (count && !provider)
        return state->reject(QStringLiteral("A nonempty MAT cell matrix requires a provider."), error);
    const qint64 headerBytes = 48 + paddedToEight(name.size());
    // Every unnamed child occupies at least48 bytes, even an empty nested cell.
    if (count > (qint64(std::numeric_limits<quint32>::max()) - headerBytes + 8) / 48
        || headerBytes + count * 48 > MaximumFileBytes - state->nextOffset)
        return state->reject(QStringLiteral("MAT cell matrix exceeds the Level-5 element or 64-GiB output bound."), error);

    state->cellWriting = true;
    struct Reset { QSharedPointer<Private> state; ~Reset() { state->cellWriting = false; } } reset{state};
    const qint64 start = state->nextOffset;
    auto append = [&](const QByteArray &bytes) {
        if (bytes.size() > MaximumFileBytes - state->nextOffset
            || state->nextOffset - start + bytes.size() - 8 > std::numeric_limits<quint32>::max())
            return state->failIo(QStringLiteral("MAT cell matrix exceeded its output byte bound."), error);
        if (!state->writeAt(state->nextOffset, bytes, error)) return false;
        state->nextOffset += bytes.size();
        return true;
    };
    try {
        if (!append(matrixHeader(name, MxCellClass, rows, columns, 0))) return false;
        state->names.insert(name);
        for (qint64 index = 0; index < count; ++index) {
            CellValue value;
            QString providerError;
            const bool supplied = provider(index, &value, &providerError);
            if (!state->alive)
                return state->failIo(QStringLiteral("The MAT writer was destroyed during a cell provider."), error);
            if (state->ioFailed) return state->reject(state->ioError, error);
            if (!supplied)
                return state->failIo(providerError.isEmpty() ? QStringLiteral("The MAT cell provider failed or was cancelled.") : providerError, error);
            if (!state->file || !state->file->isOpen()
                || !(state->file->openMode() & QIODevice::WriteOnly)
                || (state->file->openMode() & (QIODevice::Append | QIODevice::Text))
                || state->file->isSequential()
                || state->file->size() != state->nextOffset)
                return state->failIo(QStringLiteral("The MAT output device changed during a cell provider."), error);
            qint64 size = 0; int nodes = 0;
            if (!measureCell(value, 1, &nodes, &size, &providerError))
                return state->failIo(providerError, error);
            QByteArray encoded; encoded.reserve(int(size));
            encodeCell(value, &encoded);
            if (encoded.size() != size)
                return state->failIo(QStringLiteral("Internal MAT cell layout error."), error);
            if (!append(encoded)) return false;
        }
        QByteArray length;
        appendUInt32(&length, quint32(state->nextOffset - start - 8));
        if (!state->writeAt(start + 4, length, error)) return false;
        if (!state->file || state->file->size() != state->nextOffset)
            return state->failIo(QStringLiteral("MAT output length changed while appending cells."), error);
        return true;
    } catch (const std::bad_alloc &) {
        return state->failIo(QStringLiteral("Insufficient memory for a bounded MAT cell value."), error);
    } catch (...) {
        return state->failIo(QStringLiteral("The MAT cell provider or output operation threw an exception."), error);
    }
}

bool MatFileWriter::finish(QString *error)
{
    clearError(error);
    if (!d->ready(error)) {
        return false;
    }
    for (auto iterator = d->records.cbegin(); iterator != d->records.cend(); ++iterator) {
        if (iterator->completedColumns != iterator->matrix.columns) {
            return d->reject(QStringLiteral(
                "MAT output contains an incomplete reserved matrix."), error);
        }
    }
    if (!d->file->flush()) {
        return d->failIo(QStringLiteral("Could not flush the MAT output: %1")
                             .arg(d->file->errorString()), error);
    }
    if (d->file->size() != d->nextOffset) {
        return d->failIo(QStringLiteral("The MAT output size changed before completion."), error);
    }
    d->finished = true;
    return true;
}
