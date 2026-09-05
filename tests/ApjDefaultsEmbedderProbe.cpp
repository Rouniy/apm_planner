#include "ui/tools/ApjDefaultsEmbedder.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    QTextStream output(stdout);
    if (arguments.size() < 3) {
        output << "Usage: apj_defaults_probe FIRMWARE DEFAULTS [--overwrite] [--cancel-at=N]\n";
        return 2;
    }
    ApjDefaultsEmbedder::Options options;
    int cancelPercent = -1;
    for (int i = 3; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--overwrite")) {
            options.overwriteExisting = true;
        } else if (argument.startsWith(QStringLiteral("--cancel-at="))) {
            bool valid = false;
            cancelPercent = argument.mid(12).toInt(&valid);
            if (!valid || cancelPercent < 0 || cancelPercent > 100) return 2;
        } else {
            return 2;
        }
    }
    bool cancelled = cancelPercent == 0;
    const auto result = ApjDefaultsEmbedder::Embed(arguments.at(1), arguments.at(2), options,
        [&](qint64 completed, qint64 total) {
            if (cancelPercent >= 0 && total > 0
                && static_cast<long double>(completed) * 100
                    >= static_cast<long double>(cancelPercent) * total)
                cancelled = true;
        }, [&] { return cancelled; });
    QJsonObject report;
    report.insert(QStringLiteral("success"), result.success);
    report.insert(QStringLiteral("cancelled"), result.cancelled);
    report.insert(QStringLiteral("error"), result.error);
    report.insert(QStringLiteral("outputPath"), result.outputPath);
    report.insert(QStringLiteral("imageBytes"), static_cast<double>(result.imageBytes));
    report.insert(QStringLiteral("defaultsBytes"), static_cast<double>(result.defaultsBytes));
    report.insert(QStringLiteral("maximumDefaultsBytes"), static_cast<double>(result.maximumDefaultsBytes));
    report.insert(QStringLiteral("outputBytes"), static_cast<double>(result.outputBytes));
    report.insert(QStringLiteral("repairedDescriptors"), result.repairedDescriptors);
    report.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings));
    output << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return result.success ? 0 : result.cancelled ? 3 : 1;
}
