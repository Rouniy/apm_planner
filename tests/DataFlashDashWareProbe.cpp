#include "ui/Loghandling/DataFlashDashWareCsvExporter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

// Manual file-only production exporter probe, with no vehicle/session objects.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 4 || args.size() > 5) {
        QTextStream(stderr) << "Usage: dataflash_dashware_probe input output semicolon-types [cancel-percent]\n";
        return 2;
    }
    bool ok = true;
    const int cancelPercent = args.size() == 5 ? args.at(4).toInt(&ok) : -1;
    if (!ok || (args.size() == 5 && (cancelPercent < 0 || cancelPercent > 100)))
        return 2;
    bool cancel = false;
    const auto result = DataFlashDashWareCsvExporter::Export(args.at(1), args.at(2),
        args.at(3).split(QLatin1Char(';'), Qt::SkipEmptyParts),
        [&]() { return cancel; }, [&](qint64 done, qint64 total) {
            if (cancelPercent >= 0 && total > 0
                && double(done) / double(total) * 100.0 >= cancelPercent)
                cancel = true;
        });
    QJsonObject report;
    report.insert(QStringLiteral("success"), result.success);
    report.insert(QStringLiteral("cancelled"), result.cancelled);
    report.insert(QStringLiteral("error"), result.error);
    report.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings));
    report.insert(QStringLiteral("rowsWritten"), double(result.rowsWritten));
    report.insert(QStringLiteral("columns"), double(result.columns));
    report.insert(QStringLiteral("bytesWritten"), double(result.bytesWritten));
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
    return result.success ? 0 : result.cancelled ? 3 : 1;
}
