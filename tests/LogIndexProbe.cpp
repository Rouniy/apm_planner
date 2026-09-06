#include "ui/Loghandling/LogIndexService.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

// Read-only diagnostic: analyze exactly one file, without scanning its
// directory, creating thumbnails, or touching the application settings.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        QTextStream(stderr) << "Usage: logindex_probe <file.bin|file.log|file.tlog>\n";
        return 2;
    }
    const QFileInfo info(app.arguments().at(1));
    if (!info.isFile() || info.isSymLink()) return 2;
    LogIndex::Entry input;
    input.fullPath = info.canonicalFilePath();
    input.rootPath = QFileInfo(input.fullPath).absolutePath();
    input.source = {true, info.size(), info.lastModified().toUTC()};
    const auto analysis = LogIndexService::analyzeFile(input);
    const auto &entry = analysis.entry;
    QJsonObject result{
        {"path", entry.fullPath}, {"frame", entry.frame}, {"systemId", entry.systemId},
        {"dateUtc", entry.dateUtc.toString(Qt::ISODateWithMs)},
        {"durationSeconds", entry.durationSeconds}, {"timeInAirSeconds", entry.timeInAirSeconds},
        {"distanceMeters", entry.distanceMeters}, {"cameraMessages", double(entry.cameraMessages)},
        {"trackPoints", analysis.track.size()}, {"error", entry.error},
        {"home", QJsonObject{{"valid", entry.home.valid}, {"latitude", entry.home.latitude},
                             {"longitude", entry.home.longitude},
                             {"altitudeMeters", entry.home.altitudeMeters}}}
    };
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
    return analysis.cancelled || !entry.error.isEmpty() ? 1 : 0;
}
