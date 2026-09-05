#include <QtTest>

#include "ui/configuration/ConfigFFTViewModel.h"

#include <QBuffer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

namespace {
ParameterMetaDataCatalog fftCatalog()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:INS_LOG_BAT_CNT" humanName="Sample count per batch"
               documentation="Samples captured in each batch">
          <field name="Increment">32</field>
        </param>
        <param name="ArduCopter:INS_LOG_BAT_MASK" humanName="Sensor Bitmask"
               documentation="Select logged IMUs">
          <field name="Bitmask">0:IMU1,1:IMU2,2:IMU3</field>
        </param>
        <param name="ArduCopter:LOG_BITMASK" humanName="Log bitmask"
               documentation="Select onboard logs">
          <field name="Bitmask">7:IMU,18:Fast IMU,19:Raw IMU</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> fftSnapshot()
{
    return {{1, QStringLiteral("INS_LOG_BAT_CNT"), 1024},
            {1, QStringLiteral("INS_LOG_BAT_MASK"), 3},
            {1, QStringLiteral("LOG_BITMASK"), 1 << 19}};
}

QString makeInput(QTemporaryDir *temporary)
{
    const QString path = temporary->filePath(QStringLiteral("flight.bin"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("fft-fixture");
        file.close();
    }
    return path;
}
} // namespace

class ConfigFFTViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exposesExactReferenceDefaultsAndMetadata();
    void exactParameterWriteOwnsTerminalLifecycle();
    void offlineAnalysisUsesStableOptionsAndPublishesAllSeries();
    void cancellationAndDestructionJoinWorker();
};

void ConfigFFTViewModelTest::exposesExactReferenceDefaultsAndMetadata()
{
    ConfigFFTViewModel model;
    QCOMPARE(model.title(), QStringLiteral("FFT Setup"));
    QCOMPARE(ConfigFFTViewModel::ReferenceParameterNames(),
             (QStringList{QStringLiteral("INS_LOG_BAT_CNT"),
                          QStringLiteral("INS_LOG_BAT_MASK"),
                          QStringLiteral("LOG_BITMASK")}));
    QCOMPARE(model.bins(), 10);
    QCOMPARE(model.startFrequencyHz(), 5.0);
    QVERIFY(!model.magnitude());
    QVERIFY(model.canAnalyze());
    QVERIFY(!model.canEditParameters());

    model.setCatalog(fftCatalog());
    model.setParameterSnapshot(fftSnapshot(), 1, true);
    model.setParameterContext(true, true, false);
    const QList<ParamField> fields = model.fields();
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(0).editorKind, ParamField::EditorKind::Numeric);
    QCOMPARE(fields.at(0).increment, 32.0);
    QCOMPARE(fields.at(1).editorKind, ParamField::EditorKind::Bitmask);
    QCOMPARE(fields.at(1).bitOptions.size(), 3);
    QCOMPARE(fields.at(2).editorKind, ParamField::EditorKind::Bitmask);
    QCOMPARE(fields.at(2).bitOptions.size(), 3);
    QVERIFY(model.canEditParameters());

    QVERIFY(model.setBins(100));
    QCOMPARE(model.bins(), 14);
    QVERIFY(model.setStartFrequencyHz(-20.0));
    QCOMPARE(model.startFrequencyHz(), 0.0);
    model.setArmed(true);
    QVERIFY(!model.canEditParameters());
    QVERIFY(model.canAnalyze());
}

void ConfigFFTViewModelTest::exactParameterWriteOwnsTerminalLifecycle()
{
    ConfigFFTViewModel model;
    model.setCatalog(fftCatalog());
    model.setParameterSnapshot(fftSnapshot(), 1, true);
    model.setParameterContext(true, true, false);
    QSignalSpy writes(&model, &ConfigFFTViewModel::writeRequested);

    QVERIFY(model.stageParameterValue(
        QStringLiteral("INS_LOG_BAT_MASK"), quint64(5)));
    QCOMPARE(writes.size(), 1);
    const QList<QVariant> arguments = writes.takeFirst();
    const quint64 requestId = arguments.at(0).toULongLong();
    QCOMPARE(arguments.at(1).toInt(), 1);
    QCOMPARE(arguments.at(2).toString(),
             QStringLiteral("INS_LOG_BAT_MASK"));
    QCOMPARE(arguments.at(3).toULongLong(), quint64(5));
    QVERIFY(model.parameterWriteBusy());

    model.parameterWriteSubmitted(requestId, 77);
    model.parameterChanged(1, QStringLiteral("INS_LOG_BAT_MASK"), 1);
    QCOMPARE(model.fields().at(1).value.toULongLong(), quint64(5));
    model.parameterBatchCompleted(77, 1, 0);
    QVERIFY(!model.parameterWriteBusy());
    QCOMPARE(model.fields().at(1).value.toULongLong(), quint64(5));

    QVERIFY(model.stageParameterValue(
        QStringLiteral("INS_LOG_BAT_MASK"), quint64(7)));
    const quint64 failedRequest = writes.takeFirst().at(0).toULongLong();
    model.parameterWriteSubmitted(failedRequest, 78);
    model.parameterWriteFailed(
        78, 1, QStringLiteral("INS_LOG_BAT_MASK"),
        QStringLiteral("link retired"));
    QVERIFY(model.reconciliationRequired());
    QVERIFY(!model.canEditParameters());
    QCOMPARE(model.fields().at(1).value.toULongLong(), quint64(5));
}

void ConfigFFTViewModelTest::
offlineAnalysisUsesStableOptionsAndPublishesAllSeries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString input = makeInput(&temporary);
    std::atomic_int observedBins{0};
    std::atomic_int observedStart{0};
    std::atomic_bool observedMagnitude{true};
    ConfigFFTViewModel model(
        [&observedBins, &observedStart, &observedMagnitude](
            const QString &, const DataFlashFftAnalyzer::Options &options) {
        observedBins.store(options.bins);
        observedStart.store(qRound(options.startFrequencyHz));
        observedMagnitude.store(options.magnitude);
        DataFlashFftAnalyzer::Result result;
        result.succeeded = true;
        result.source = QStringLiteral("IMU GYR");
        result.sampleRateHz = 400.0;
        result.suggestedNotchHz = 83.0;
        for (const QString &axis : {QStringLiteral("x"),
                                    QStringLiteral("y"),
                                    QStringLiteral("z")}) {
            DataFlashFftAnalyzer::Series series;
            series.label = QStringLiteral("IMU GYR ") + axis;
            series.frequenciesHz = {5.0, 10.0};
            series.values = {-40.0, -20.0};
            series.sampleRateHz = 400.0;
            result.series.append(series);
        }
        return result;
    });
    QVERIFY(!model.canEditParameters());
    QVERIFY(model.canAnalyze());
    QSignalSpy resultChanged(
        &model, &ConfigFFTViewModel::analysisResultChanged);
    QVERIFY(model.analyzeFile(input));
    QTRY_VERIFY_WITH_TIMEOUT(!model.analysisBusy(), 2000);
    QVERIFY(resultChanged.size() >= 2);
    QCOMPARE(observedBins.load(), 10);
    QCOMPARE(observedStart.load(), 5);
    QVERIFY(!observedMagnitude.load());
    QCOMPARE(model.analysisResult().series.size(), 3);
    QVERIFY(model.analysisStatus().contains(QStringLiteral("83 Hz")));
}

void ConfigFFTViewModelTest::cancellationAndDestructionJoinWorker()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString input = makeInput(&temporary);
    std::atomic_bool workerExited{false};
    auto analyzer = [&workerExited](
        const QString &, const DataFlashFftAnalyzer::Options &options) {
        DataFlashFftAnalyzer::Result result;
        while (!options.isCancelled()) {
            QThread::msleep(1);
        }
        workerExited.store(true);
        result.cancelled = true;
        return result;
    };

    ConfigFFTViewModel model(analyzer);
    QVERIFY(model.analyzeFile(input));
    QVERIFY(!model.setBins(12));
    QVERIFY(!model.setStartFrequencyHz(40.0));
    QVERIFY(!model.setMagnitude(true));
    QCOMPARE(model.bins(), 10);
    QCOMPARE(model.startFrequencyHz(), 5.0);
    QVERIFY(!model.magnitude());
    model.cancelAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!model.analysisBusy(), 2000);
    QVERIFY(workerExited.load());
    QVERIFY(model.analysisStatus().contains(QStringLiteral("cancelled")));

    workerExited.store(false);
    auto *joining = new ConfigFFTViewModel(analyzer);
    QVERIFY(joining->analyzeFile(input));
    delete joining;
    QVERIFY(workerExited.load());
}

QTEST_GUILESS_MAIN(ConfigFFTViewModelTest)
#include "test_configfftviewmodel.moc"
