#include "ui/Loghandling/GeoRefService.h"
#include "ui/Loghandling/GeoRefExif.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {
QJsonArray strings(const QStringList &values) {
    QJsonArray result; for (const auto &value : values) result.append(value); return result;
}
QJsonArray matches(const QVector<GeoRefService::Match> &values) {
    QJsonArray result;
    for (const auto &m : values) result.append(QJsonObject{
        {"sourcePath", m.sourcePath}, {"outputPath", m.outputPath},
        {"timeUtc", m.timeUtc.toUTC().toString(Qt::ISODateWithMs)},
        {"latitude", m.latitude}, {"longitude", m.longitude}, {"altitude", m.altitude},
        {"roll", m.roll}, {"pitch", m.pitch}, {"yaw", m.yaw}});
    return result;
}
int print(const QJsonObject &value, bool success) {
    QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Indented);
    return success ? 0 : 1;
}
}

// Offline test harness only: execute admits the plan on private test fixtures.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    if (args[1] == "inspect") {
        const auto r = GeoRefExif::Inspect(args[2]);
        return print({{"success", r.success}, {"error", r.error}, {"format", r.format},
            {"coordinateWarning", r.coordinateWarning},
            {"photoTime", r.photoTime.toString(Qt::ISODateWithMs)},
            {"hasCoordinates", r.hasCoordinates}, {"latitude", r.coordinates.latitude},
            {"longitude", r.coordinates.longitude}, {"altitude", r.coordinates.altitude}}, r.success);
    }
    QFile file(args[2]); if (!file.open(QIODevice::ReadOnly)) return 2;
    QJsonParseError parse; const auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) return 2;
    const auto j = json.object(); GeoRefService::Options o;
    o.logPath = j["logPath"].toString(); o.photoDirectory = j["photoDirectory"].toString();
    o.outputDirectory = j["outputDirectory"].toString();
    const auto mode = j["mode"].toString("cam");
    if (mode != "cam" && mode != "trig" && mode != "offset") return 2;
    o.mode = mode == "cam" ? GeoRefService::Mode::Cam : mode == "trig"
        ? GeoRefService::Mode::Trig : GeoRefService::Mode::TimeOffset;
    o.timeOffsetSeconds = j["timeOffsetSeconds"].toDouble();
    o.useGps2 = j["useGps2"].toBool();
    o.shutterLagMilliseconds = j["shutterLagMilliseconds"].toInt();
    o.useAmslAltitude = j["useAmslAltitude"].toBool();
    o.useGpsAltitude = j["useGpsAltitude"].toBool();
    o.baseAltitudeAdjustmentMeters = j["baseAltitudeAdjustmentMeters"].toDouble();
    if (args[1] == "estimate") {
        const auto r = GeoRefService::Estimate(o);
        return print({{"success", r.success}, {"error", r.error}, {"hasEstimate", r.hasEstimate},
            {"offsetSeconds", r.offsetSeconds}, {"warnings", strings(r.warnings)}}, r.success);
    }
    if (args[1] != "prepare" && args[1] != "execute") return 2;
    const auto p = GeoRefService::Prepare(o);
    if (!p.success || !p.plan)
        return print({{"success", false}, {"error", p.error}, {"warnings", strings(p.warnings)}}, false);
    if (args[1] == "prepare")
        return print({{"success", true}, {"matches", matches(p.plan->matches())},
            {"outputs", strings(p.plan->outputPaths())}, {"warnings", strings(p.warnings)}}, true);
    const auto r = GeoRefService::Execute(*p.plan);
    return print({{"success", r.success}, {"cancelled", r.cancelled}, {"error", r.error},
        {"matches", matches(r.matches)}, {"publishedPaths", strings(r.publishedPaths)},
        {"failedPaths", strings(r.failedPaths)}, {"warnings", strings(r.warnings)},
        {"taggedPhotos", r.taggedPhotos}, {"failedPhotos", r.failedPhotos}}, r.success);
}
