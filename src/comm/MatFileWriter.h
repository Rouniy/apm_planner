#ifndef MATFILEWRITER_H
#define MATFILEWRITER_H

#include <QByteArray>
#include <QFileDevice>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <functional>

// Worker-safe, uncompressed little-endian MATLAB Level-5 numeric/cell writer.
// The caller owns and publishes the already-open random-access file device.
class MatFileWriter final
{
public:
    struct CellValue {
        enum class Type { Scalar, Text, Cell };
        Type type = Type::Scalar;
        double number = 0;
        QString text;
        qint64 rows = 0;
        qint64 columns = 0;
        QVector<CellValue> children; // Nested Cell only, column-major order.
    };
    using CellProvider = std::function<bool(qint64 index, CellValue *value, QString *error)>;
    struct Matrix {
        Matrix() = default;
        Matrix(qint64 offset, qint64 rowCount, qint64 columnCount)
            : dataOffset(offset), rows(rowCount), columns(columnCount) {}

        qint64 dataOffset = -1;
        qint64 rows = 0;
        qint64 columns = 0;

    private:
        quint64 writerToken = 0;
        quint64 matrixToken = 0;
        friend class MatFileWriter;
    };

    static constexpr qint64 MaximumFileBytes = 64LL * 1024LL * 1024LL * 1024LL;
    static constexpr int MaximumNameBytes = 127;
    static constexpr int MaximumWriteChunkValues = 8192;
    static constexpr int MaximumCellDepth = 16;
    static constexpr int MaximumCellNodes = 65536;
    static constexpr qint64 MaximumCellValueBytes = 8LL * 1024 * 1024;

    explicit MatFileWriter(QFileDevice *file);
    ~MatFileWriter();

    bool begin(QString *error = nullptr);
    bool reserveDoubleMatrix(const QByteArray &name, qint64 rows, qint64 columns,
                             Matrix *matrix, QString *error = nullptr);
    bool writeDoubleColumn(const Matrix &matrix, qint64 column, qint64 firstRow,
                           const QVector<double> &values,
                           QString *error = nullptr);
    // Invokes provider once per cell in increasing column-major index order.
    // Scalar is 1x1 double; Text is 1xUTF16-code-unit-count char (miUINT16),
    // or 0x0 for an empty string, matching csmatio MLChar.
    // preserving NUL/surrogates. Nested Cell dimensions must match children.
    // Bounds apply separately to each provided value, not the complete matrix.
    // May precede/follow numeric reservations; no provider or child tree is kept.
    // After admission, provider failure/cancellation makes the writer unusable;
    // the caller must discard unpublished output. No rollback/publication here.
    bool appendCellMatrix(const QByteArray &name, qint64 rows, qint64 columns,
                          CellProvider provider, QString *error = nullptr);
    bool finish(QString *error = nullptr);

private:
    struct Private;
    QSharedPointer<Private> d;

    Q_DISABLE_COPY(MatFileWriter)
};

#endif // MATFILEWRITER_H
