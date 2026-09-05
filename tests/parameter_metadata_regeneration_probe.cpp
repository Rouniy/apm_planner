#include "core/parameters/ParameterMetaDataRegenerationService.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

// Explicit manual probe: downloads official metadata into a caller-selected
// scratch cache. Never part of CTest and never connects to a vehicle.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3 || app.arguments().at(1) != QStringLiteral("--download"))
        return 64;
    ParameterMetaDataRegenerationService service(app.arguments().at(2));
    QObject::connect(&service, &ParameterMetaDataRegenerationService::logLine, &app,
                     [](ParameterMetaDataRegenerationService::RunToken, const QString &line) {
        QTextStream(stdout) << line << Qt::endl;
    });
    QObject::connect(&service, &ParameterMetaDataRegenerationService::finished, &app,
                     [&app](const ParameterMetaDataRegenerationService::Result &result) {
        QTextStream output(stdout);
        output << "Outcome=" << int(result.outcome) << " artifacts=" << result.artifacts.size() << Qt::endl;
        for (const auto &artifact : result.artifacts)
            output << artifact.key << " published=" << artifact.published << " bytes="
                   << artifact.byteCount << " sha256=" << artifact.sha256 << " error="
                   << artifact.error << Qt::endl;
        app.exit(result.outcome == ParameterMetaDataRegenerationService::Outcome::Complete ? 0 : 1);
    });
    ParameterMetaDataRegenerationService::RunToken token;
    QString error;
    if (service.start(&token, &error) != ParameterMetaDataRegenerationService::StartResult::Started) {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }
    QTimer::singleShot(10 * 60 * 1000, &service, [&service, token]() { service.cancel(token); });
    return app.exec();
}
