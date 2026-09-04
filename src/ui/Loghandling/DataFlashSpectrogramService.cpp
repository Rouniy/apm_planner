#include "DataFlashSpectrogramService.h"

#include "AsciiLogParser.h"
#include "BinLogParser.h"
#include "ILogdataSink.h"
#include "ILogParser.h"
#include "IParserCallback.h"

#include <QFile>
#include <QFileInfo>
#include <QSharedPointer>

#include <memory>

namespace
{
using Analyzer = DataFlashSpectrogramAnalyzer;

Analyzer::AnalysisResult failure(Analyzer::ErrorCode error,
                                 const QString &message)
{
    Analyzer::AnalysisResult result;
    result.error = error;
    result.message = message;
    return result;
}

bool cancellationRequested(const Analyzer::CancellationCheck &check)
{
    return check && check();
}

class SpectrogramLogSink final : public ILogdataSink
{
public:
    SpectrogramLogSink(const QString &sensorName,
                       const Analyzer::CancellationCheck &isCancelled)
        : m_builder(sensorName, Analyzer::BuildOptions{
              Analyzer::MaximumInputSamples, isCancelled})
    {
    }

    bool addDataRow(
        const QString &typeName,
        const QList<QPair<QString, QVariant>> &values) override
    {
        const Analyzer::RecordDisposition disposition =
            m_builder.addRecord(typeName, values);
        if (disposition == Analyzer::RecordDisposition::LimitExceeded) {
            m_error = QStringLiteral(
                "The selected sensor exceeds the bounded %1-sample input limit.")
                    .arg(Analyzer::MaximumInputSamples);
            return false;
        }
        if (disposition == Analyzer::RecordDisposition::Cancelled) {
            m_error = QStringLiteral("Spectrogram parsing was cancelled.");
            return false;
        }
        return true;
    }

    bool addDataType(const QString &, quint32, int, const QString &,
                     const QStringList &, int) override
    {
        return true;
    }

    void addUnitData(quint8, const QString &) override {}
    void addMultiplierData(quint8, double) override {}
    void addMsgToUnitAndMultiplierData(
        quint32, const QByteArray &, const QByteArray &) override {}
    void setTimeStamp(const QString &, double) override {}
    QStringList setupUnitData(const QString &, double) override { return {}; }

    QString getError() const override
    {
        return m_error;
    }

    Analyzer::BuildResult finish()
    {
        return m_builder.finish();
    }

private:
    Analyzer::RecordBuilder m_builder;
    QString m_error;
};

class ParserCallback final : public IParserCallback
{
public:
    explicit ParserCallback(const Analyzer::CancellationCheck &isCancelled)
        : m_isCancelled(isCancelled)
    {
    }

    void setParser(ILogParser *parser) { m_parser = parser; }

    void onProgress(qint64, qint64) override
    {
        if (cancellationRequested(m_isCancelled) && m_parser) {
            m_parser->stopParsing();
        }
    }

    void onError(const QString &errorMessage) override
    {
        m_error = errorMessage;
        if (m_parser) {
            m_parser->stopParsing();
        }
    }

    QString error() const { return m_error; }

private:
    Analyzer::CancellationCheck m_isCancelled;
    ILogParser *m_parser = nullptr;
    QString m_error;
};
}

DataFlashSpectrogramAnalyzer::AnalysisResult
DataFlashSpectrogramService::Generate(
    const QString &path, const QString &sensorName,
    int minimumDb, int maximumDb,
    const CancellationCheck &isCancelled)
{
    if (cancellationRequested(isCancelled)) {
        return failure(Analyzer::ErrorCode::Cancelled,
                       QStringLiteral("Spectrogram calculation was cancelled."));
    }
    if (!Analyzer::IsSupportedSensor(sensorName)) {
        return failure(Analyzer::ErrorCode::InvalidSensor,
                       QStringLiteral(
                           "Sensor must be ACC1..ACC5 or GYR1..GYR5."));
    }
    if (minimumDb >= maximumDb) {
        return failure(Analyzer::ErrorCode::InvalidDecibelRange,
                       QStringLiteral(
                           "Min dB must be smaller than Max dB."));
    }

    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const bool isBinary = suffix.compare(QStringLiteral("bin"),
                                          Qt::CaseInsensitive) == 0;
    const bool isAscii = suffix.compare(QStringLiteral("log"),
                                         Qt::CaseInsensitive) == 0;
    if (!info.exists() || !info.isFile()) {
        return failure(Analyzer::ErrorCode::InvalidRecord,
                       QStringLiteral("The selected DataFlash log does not exist."));
    }
    if (!isBinary && !isAscii) {
        return failure(Analyzer::ErrorCode::InvalidRecord,
                       QStringLiteral("Select a DataFlash .bin or .log file."));
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(
            Analyzer::ErrorCode::InvalidRecord,
            QStringLiteral("Unable to open %1: %2")
                .arg(info.fileName(), file.errorString()));
    }

    auto sink = QSharedPointer<SpectrogramLogSink>::create(
        sensorName, isCancelled);
    ILogdataSink::Ptr storage = qSharedPointerCast<ILogdataSink>(sink);
    ParserCallback callback(isCancelled);
    std::unique_ptr<ILogParser> parser;
    if (isBinary) {
        parser.reset(new BinLogParser(storage, &callback));
    } else {
        parser.reset(new AsciiLogParser(storage, &callback));
    }
    callback.setParser(parser.get());
    const AP2DataPlotStatus parseStatus = parser->parse(file);

    if (cancellationRequested(isCancelled)) {
        return failure(Analyzer::ErrorCode::Cancelled,
                       QStringLiteral("Spectrogram calculation was cancelled."));
    }
    if (!callback.error().isEmpty()) {
        return failure(Analyzer::ErrorCode::InvalidRecord,
                       callback.error());
    }

    Analyzer::BuildResult built = sink->finish();
    if (!built.ok()) {
        return failure(built.error, built.message);
    }

    Analyzer::AnalysisOptions options;
    options.minimumDb = minimumDb;
    options.maximumDb = maximumDb;
    options.maximumRasterWidth = Analyzer::DefaultRasterWidth;
    options.isCancelled = isCancelled;
    Analyzer::AnalysisResult analyzed = Analyzer::Analyze(
        built.source, options);
    if (analyzed.ok()
        && parseStatus.getParsingState() != AP2DataPlotStatus::OK) {
        QString details = parseStatus.getErrorOverview().simplified();
        if (details.isEmpty()) {
            details = QStringLiteral("recoverable DataFlash damage");
        }
        analyzed.message = QStringLiteral("Log parser reported %1.")
                               .arg(details);
    }
    return analyzed;
}
