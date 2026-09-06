#ifndef MATFILEWRITER_H
#define MATFILEWRITER_H

#include <QByteArray>
#include <QFileDevice>
#include <QScopedPointer>
#include <QString>
#include <QVector>

// Worker-safe, uncompressed little-endian MATLAB Level-5 DOUBLE writer.
// The caller owns and publishes the already-open random-access file device.
class MatFileWriter final
{
public:
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

    explicit MatFileWriter(QFileDevice *file);
    ~MatFileWriter();

    bool begin(QString *error = nullptr);
    bool reserveDoubleMatrix(const QByteArray &name, qint64 rows, qint64 columns,
                             Matrix *matrix, QString *error = nullptr);
    bool writeDoubleColumn(const Matrix &matrix, qint64 column, qint64 firstRow,
                           const QVector<double> &values,
                           QString *error = nullptr);
    bool finish(QString *error = nullptr);

private:
    struct Private;
    QScopedPointer<Private> d;

    Q_DISABLE_COPY(MatFileWriter)
};

#endif // MATFILEWRITER_H
