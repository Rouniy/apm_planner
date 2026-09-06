#include "services/ResxTranslationService.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == "export") {
        QFile input(args[4]);
        if (!input.open(QIODevice::ReadOnly)) return 2;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(input.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()) return 2;
        QVector<ResxTranslationEntry> entries;
        for (const auto &value : document.array()) {
            const auto item = value.toObject();
            entries.append({item["relativePath"].toString(), item["key"].toString(),
                item["sourceText"].toString(), item["translation"].toString(),
                item["comment"].toString(), item["hasExistingTranslation"].toBool()});
        }
        const auto result = ResxTranslationService::exportTranslations(args[2], args[3], entries);
        return result.success ? 0 : 1;
    }
    if (args.size() != 3) return 2;
    const auto result = ResxTranslationService::load(args[1], args[2]);
    QJsonArray entries, warnings;
    for (const auto &entry : result.project.entries)
        entries.append(QJsonObject{{"relativePath", entry.relativePath}, {"key", entry.key},
            {"sourceText", entry.sourceText}, {"translation", entry.translation},
            {"comment", entry.comment}, {"hasExistingTranslation", entry.hasExistingTranslation}});
    for (const auto &warning : result.project.warnings) warnings.append(warning);
    const QJsonObject output{{"success", result.success}, {"error", result.error},
        {"sourceRoot", result.project.sourceRoot}, {"culture", result.project.culture},
        {"resourceFiles", result.project.resourceFiles}, {"warnings", warnings}, {"entries", entries}};
    QFile stream;
    if (!stream.open(stdout, QIODevice::WriteOnly)) return 3;
    const QByteArray json = QJsonDocument(output).toJson(QJsonDocument::Indented);
    if (stream.write(json) != json.size()) return 3;
    return result.success ? 0 : 1;
}
