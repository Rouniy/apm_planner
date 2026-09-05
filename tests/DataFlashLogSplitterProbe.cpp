#include "ui/Loghandling/DataFlashLogSplitter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

// Manual offline real-log/memory probe; never creates a connection or vehicle.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 3 || arguments.size() > 4) {
        QTextStream(stderr) << "Usage: dataflash_log_splitter_probe input pieces [cancel-percent]\n";
        return 2;
    }
    bool countOk = false;
    const int pieces = arguments.at(2).toInt(&countOk);
    bool cancelOk = true;
    const int cancelPercent = arguments.size() == 4 ? arguments.at(3).toInt(&cancelOk) : -1;
    if (!countOk || !cancelOk || (arguments.size() == 4 && (cancelPercent < 0 || cancelPercent > 100)))
        return 2;
    bool cancel = false;
    const auto result = DataFlashLogSplitter::Split(arguments.at(1), pieces,
        [&]() { return cancel; }, [&](qint64 done, qint64 total) {
            if (cancelPercent >= 0 && total > 0
                && double(done) / double(total) * 100.0 >= cancelPercent)
                cancel = true;
        });
    QJsonObject report;
    report.insert(QStringLiteral("success"), result.success);
    report.insert(QStringLiteral("cancelled"), result.cancelled);
    report.insert(QStringLiteral("error"), result.error);
    report.insert(QStringLiteral("outputs"), QJsonArray::fromStringList(result.outputs));
    report.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings));
    report.insert(QStringLiteral("recordsRead"), double(result.recordsRead));
    report.insert(QStringLiteral("dataRecords"), double(result.dataRecords));
    report.insert(QStringLiteral("bytesWritten"), double(result.bytesWritten));
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
    return result.success ? 0 : result.cancelled ? 3 : 1;
}
