#include "ui/Loghandling/LogAnonymizer.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>

// Optional manual evidence tool, never part of CTest. No live vehicle access.
// Usage: log_anonymizer_probe input output lat-offset lon-offset [cancel-percent]
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 5 || args.size() > 6) return 2;
    LogAnonymizeOptions options;
    if (!LogAnonymizer::parseOffset(args.at(3), &options.latitudeOffset)
        || !LogAnonymizer::parseOffset(args.at(4), &options.longitudeOffset)) return 2;
    bool cancel = false;
    const int cancelPercent = args.size() == 6 ? args.at(5).toInt() : -1;
    const auto result = LogAnonymizer::anonymizeFile(args.at(1), args.at(2), options,
        [&]() { return cancel; }, [&](qint64 done, qint64 total) {
            if (cancelPercent >= 0 && total > 0
                && static_cast<long double>(done) / total * 100 >= cancelPercent)
                cancel = true;
        });
    QJsonObject json;
    json.insert(QStringLiteral("success"), result.success);
    json.insert(QStringLiteral("cancelled"), result.cancelled);
    json.insert(QStringLiteral("error"), result.error);
    json.insert(QStringLiteral("inputBytes"), double(result.inputBytes));
    json.insert(QStringLiteral("outputBytes"), double(result.outputBytes));
    json.insert(QStringLiteral("records"), double(result.records));
    json.insert(QStringLiteral("coordinateFields"), double(result.coordinateFields));
    json.insert(QStringLiteral("patchedValues"), double(result.patchedValues));
    json.insert(QStringLiteral("strippedSignatures"), double(result.strippedSignatures));
    json.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings));
    QTextStream(stdout) << QJsonDocument(json).toJson(QJsonDocument::Indented);
    return result.success ? 0 : result.cancelled ? 3 : 1;
}
