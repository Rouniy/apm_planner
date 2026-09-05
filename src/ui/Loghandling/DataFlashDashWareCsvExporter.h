#ifndef DATAFLASHDASHWARECSVEXPORTER_H
#define DATAFLASHDASHWARECSVEXPORTER_H

#include <QString>
#include <QStringList>
#include <functional>

class DataFlashDashWareCsvExporter final
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QStringList warnings;
        qint64 rowsWritten = 0;
        qint64 columns = 0;
        qint64 bytesWritten = 0;
    };
    // types is case-insensitive; an empty list includes all declared types.
    // Output uses first-FMT schema order and raw row order, without interpolation
    // or FMTU multiplier scaling. Failed/cancelled work never replaces output.
    static Result Export(QString input, QString output, QStringList types = {},
                         CancelCheck cancel = {}, Progress progress = {});
};

#endif
