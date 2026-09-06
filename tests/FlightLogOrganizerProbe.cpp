#include "ui/Loghandling/FlightLogOrganizer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {
QJsonArray entries(const QVector<FlightLogOrganizer::Entry> &items)
{
    QJsonArray result;
    for (const auto &item : items) {
        QJsonObject entry;
        entry.insert("operation", item.operation == FlightLogOrganizer::Operation::Move
                     ? "move" : "delete-empty");
        entry.insert("source", item.source);
        entry.insert("destination", item.destination);
        entry.insert("bytes", double(item.bytes));
        result.append(entry);
    }
    return result;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    QTextStream output(stdout);
    if (arguments.size() < 2 || arguments.size() > 3
        || (arguments.size() == 3 && arguments[2] != "--execute")) {
        output << "Usage: flight_log_organizer_probe DIRECTORY [--execute]\n"
                  "Default is read-only analysis. --execute moves the listed files and deletes listed empty logs.\n";
        return 2;
    }
    const auto analysis = FlightLogOrganizer::Analyze(arguments[1]);
    QJsonObject report;
    report.insert("analysisSuccess", analysis.success);
    report.insert("error", analysis.error);
    report.insert("root", analysis.plan.root());
    report.insert("candidates", analysis.plan.candidateCount());
    report.insert("entries", entries(analysis.plan.entries()));
    report.insert("warnings", QJsonArray::fromStringList(analysis.plan.warnings()));
    int status = analysis.success ? 0 : 1;
    if (analysis.success && arguments.size() == 3) {
        const auto result = FlightLogOrganizer::Execute(analysis.plan);
        report.insert("executionSuccess", result.success);
        report.insert("executionError", result.error);
        report.insert("completed", entries(result.completed));
        report.insert("remaining", result.remaining);
        status = result.success ? 0 : 1;
    }
    output << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return status;
}
