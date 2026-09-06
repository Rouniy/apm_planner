#include "services/OfflineMagFitService.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {
QJsonArray vectorJson(const MagVector &value)
{
    return {value.x, value.y, value.z};
}
}

// Explicit read-only diagnostic for independent numeric/log-reader oracles.
// It never constructs a vehicle service, opens a link or modifies its input.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2 || args.size() > 3
        || (args.size() == 3 && args[2] != QStringLiteral("--sphere"))) {
        QTextStream(stderr) << "usage: offline_magfit_probe log.bin|log.log|log.tlog [--sphere]\n";
        return 2;
    }
    OfflineMagFitService::Options options;
    options.useEllipsoid = args.size() != 3;
    const auto report = OfflineMagFitService::analyze(args[1], options);
    QJsonArray results;
    for (const auto &fit : report.results) {
        results.append(QJsonObject{
            {QStringLiteral("compass"), fit.compass},
            {QStringLiteral("sourceSamples"), fit.sourceSamples},
            {QStringLiteral("usedSamples"), fit.usedSamples},
            {QStringLiteral("coverageOctants"), fit.coverageOctants},
            {QStringLiteral("loggedOffsets"), vectorJson(fit.loggedOffsets)},
            {QStringLiteral("sphereOffsets"), vectorJson(fit.sphereOffsets)},
            {QStringLiteral("sphereRadius"), fit.sphereRadius},
            {QStringLiteral("sphereRmsError"), fit.sphereRmsError},
            {QStringLiteral("offsets"), vectorJson(fit.offsets)},
            {QStringLiteral("diagonals"), vectorJson(fit.diagonals)},
            {QStringLiteral("offDiagonals"), vectorJson(fit.offDiagonals)},
            {QStringLiteral("rmsError"), fit.rmsError},
            {QStringLiteral("hasEllipsoid"), fit.hasEllipsoid}});
    }
    QJsonObject deviceIds;
    for (auto it = report.loggedDeviceIds.cbegin(); it != report.loggedDeviceIds.cend(); ++it)
        deviceIds.insert(QString::number(it.key()), static_cast<double>(it.value()));
    QJsonObject frameParameters;
    for (auto it = report.loggedFrameParameters.cbegin(); it != report.loggedFrameParameters.cend(); ++it)
        frameParameters.insert(it.key(), it.value());
    const QJsonObject output{
        {QStringLiteral("success"), report.success},
        {QStringLiteral("error"), report.error},
        {QStringLiteral("sourcePath"), report.sourcePath},
        {QStringLiteral("applyEligible"), report.applyEligible},
        {QStringLiteral("applyUnavailableReason"), report.applyUnavailableReason},
        {QStringLiteral("loggedDeviceIds"), deviceIds},
        {QStringLiteral("loggedFrameParameters"), frameParameters},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(report.warnings)},
        {QStringLiteral("results"), results}};
    QTextStream(stdout) << QJsonDocument(output).toJson(QJsonDocument::Indented);
    return report.success ? 0 : 1;
}
