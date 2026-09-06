#include "ui/Loghandling/DataFlashLogAnalyzer.h"
#include "ui/Loghandling/DataFlashGpxExporter.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <limits>

namespace {
double number(const QJsonValue &v) {
    if (!v.isString()) return v.toDouble();
    const QString text = v.toString();
    if (text == "NaN") return std::numeric_limits<double>::quiet_NaN();
    if (text == "Infinity") return std::numeric_limits<double>::infinity();
    if (text == "-Infinity") return -std::numeric_limits<double>::infinity();
    return text.toDouble();
}
DataFlashLogAnalyzer::Values values(const QJsonArray &pairs) {
    DataFlashLogAnalyzer::Values result;
    for (const auto &pair : pairs) { const auto p = pair.toArray(); result.append({p[0].toString(), number(p[1])}); }
    return result;
}
QJsonArray encode(const DataFlashLogAnalyzer::Result &result) {
    QJsonArray rows;
    for (const auto &test : result.tests)
        rows.append(QJsonObject{{QStringLiteral("name"), test.name},
                                {QStringLiteral("status"), test.status},
                                {QStringLiteral("message"), test.message}});
    return rows;
}
}

// Read-only input, new-output-only CLI for independent MP10 oracle comparison.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 4) {
        qCritical() << "Usage: dataflash_tools_probe analyze|gpx INPUT NEW_OUTPUT";
        return 2;
    }
    if (args[1] == QStringLiteral("gpx")) {
        const auto result = DataFlashGpxExporter::Export(args[2], args[3]);
        qInfo() << result.success << result.error << result.pointCount << result.warnings;
        return result.success ? 0 : 1;
    }
    QJsonArray rows;
    if (args[1] == QStringLiteral("models")) {
        QFile input(args[2]); if (!input.open(QIODevice::ReadOnly)) return 2;
        const auto cases = QJsonDocument::fromJson(input.readAll()).array();
        if (cases.isEmpty()) return 2;
        for (const auto &item : cases) {
            const auto object = item.toObject();
            DataFlashLogAnalyzer::Data data;
            data.lineCount = object["lineCount"].toInt();
            const QString type = object["vehicleType"].toString();
            data.vehicleType = type == "Copter" ? DataFlashLogAnalyzer::VehicleType::Copter
                : type == "Plane" ? DataFlashLogAnalyzer::VehicleType::Plane
                : type == "Rover" ? DataFlashLogAnalyzer::VehicleType::Rover : DataFlashLogAnalyzer::VehicleType::Unknown;
            data.parameters = values(object["parameters"].toArray());
            for (const auto &g : object["records"].toArray()) {
                const auto group = g.toObject();
                DataFlashLogAnalyzer::RecordGroup record; record.type = group["type"].toString();
                for (const auto &r : group["samples"].toArray()) {
                    const auto row = r.toObject();
                    DataFlashLogAnalyzer::Sample sample;
                    sample.line = row["line"].toInt(); sample.timeSeconds = number(row["time"]);
                    sample.values = values(row["values"].toArray());
                    for (const auto &t : row["texts"].toArray()) {
                        const auto p = t.toArray(); sample.textValues.append({p[0].toString(), p[1].toString()});
                    }
                    record.samples.append(sample);
                }
                data.records.append(record);
            }
            const auto result = DataFlashLogAnalyzer::Analyze(data);
            if (!result.success) { qCritical() << result.error; return 1; }
            rows.append(QJsonObject{{"name", object["name"]}, {"tests", encode(result)}});
        }
    } else if (args[1] == QStringLiteral("analyze")) {
        const auto result = DataFlashLogAnalyzer::Analyze(args[2]);
        if (!result.success) { qCritical() << result.error; return 1; }
        rows = encode(result);
        qInfo() << "Analysis warnings:" << result.warnings;
    } else return 2;
    const QByteArray bytes = QJsonDocument(rows).toJson(QJsonDocument::Indented);
    QFile output(args[3]);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || output.write(bytes) != bytes.size() || !output.flush()) {
        qCritical() << "Cannot publish new analysis output:" << output.errorString();
        return 1;
    }
    qInfo() << "Analysis result rows:" << rows.size();
    return 0;
}
