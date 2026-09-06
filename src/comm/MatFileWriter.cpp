#include "MatFileWriter.h"

#include <QHash>
#include <QIODevice>
#include <QSet>
#include <QtEndian>

#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace {

constexpr quint32 MiInt8 = 1;
constexpr quint32 MiInt32 = 5;
constexpr quint32 MiUInt32 = 6;
constexpr quint32 MiDouble = 9;
constexpr quint32 MiMatrix = 14;
constexpr quint32 MxDoubleClass = 6;
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

} // namespace

struct MatFileWriter::Private
{
    struct Record {
        Matrix matrix;
        QHash<qint64, qint64> nextRowByColumn;
        qint64 completedColumns = 0;
    };

    QFileDevice *file = nullptr;
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

    bool ready(QString *error) const
    {
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
        if (!file->seek(offset)) {
            return failIo(QStringLiteral("Could not seek in the MAT output: %1")
                              .arg(file->errorString()), error);
        }
        qint64 written = 0;
        while (written < size) {
            const qint64 count = file->write(data + written, size - written);
            if (count <= 0) {
                return failIo(QStringLiteral("Could not write the MAT output: %1")
                                  .arg(file->errorString()), error);
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

MatFileWriter::~MatFileWriter() = default;

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
    if (d->records.size() >= MaximumMatrices) {
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
