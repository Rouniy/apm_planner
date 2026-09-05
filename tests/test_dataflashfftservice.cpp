#include <QtTest>

#include "logging.h"
#include "ui/Loghandling/DataFlashFftService.h"

#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

Q_LOGGING_CATEGORY(apmGeneral, "apm.general.test")

namespace
{
bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size() && file.flush();
}

QByteArray directImuLog(int sampleCount, int sampleRate, int formatId = 150)
{
    QByteArray log("FMT,");
    log += QByteArray::number(formatId);
    log += ",35,IMU,Qffffff,TimeUS,GyrX,GyrY,GyrZ,AccX,AccY,AccZ\n";
    const double pi = std::acos(-1.0);
    const double frequencies[] = {8.0, 12.0, 16.0, 10.0, 14.0, 18.0};
    for (int sample = 0; sample < sampleCount; ++sample) {
        log += "IMU,";
        log += QByteArray::number(
            qulonglong(sample) * 1000000ULL / quint64(sampleRate));
        for (double frequency : frequencies) {
            log += ',';
            log += QByteArray::number(
                (frequency == 8.0 ? 2.0 : 1.0)
                    * std::sin(2.0 * pi * frequency * sample / sampleRate),
                'g', 16);
        }
        log += '\n';
    }
    return log;
}

QByteArray batchDataLine(quint16 number, int sequence,
                         int sampleRate, int firstSample)
{
    QByteArray line("ISBD,0,");
    line += QByteArray::number(number);
    line += ',';
    line += QByteArray::number(sequence);
    const double pi = std::acos(-1.0);
    for (double frequency : {8.0, 12.0, 16.0}) {
        for (int offset = 0; offset < 32; ++offset) {
            const int sample = firstSample + offset;
            line += ',';
            line += QByteArray::number(qlonglong(std::lround(
                10000.0 * std::sin(
                    2.0 * pi * frequency * sample / sampleRate))));
        }
    }
    line += '\n';
    return line;
}

QByteArray fixedField(const QByteArray &text, int size)
{
    QByteArray field(size, '\0');
    const QByteArray clipped = text.left(size);
    for (int index = 0; index < clipped.size(); ++index) {
        field[index] = clipped.at(index);
    }
    return field;
}

void appendFmt(QByteArray *bytes, quint8 id, quint8 length,
               const QByteArray &name, const QByteArray &format,
               const QByteArray &labels)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(0x80));
    bytes->append(char(id));
    bytes->append(char(length));
    bytes->append(fixedField(name, 4));
    bytes->append(fixedField(format, 16));
    bytes->append(fixedField(labels, 64));
}

void appendBatchHeader(QByteArray *bytes, quint8 id, quint16 number,
                       quint8 type, quint8 instance, quint16 sampleCount,
                       float sampleRate)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(id));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint64(0) << number << type << instance
           << quint16(100) << sampleCount << quint64(0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << sampleRate;
}

void appendBatchData(QByteArray *bytes, quint8 id, quint16 number,
                     quint16 sequence, int sampleRate, int firstSample)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(id));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint64(0) << number << sequence;
    const double pi = std::acos(-1.0);
    for (double frequency : {8.0, 12.0, 16.0}) {
        for (int offset = 0; offset < 32; ++offset) {
            const int sample = firstSample + offset;
            stream << qint16(std::lround(
                10000.0 * std::sin(
                    2.0 * pi * frequency * sample / sampleRate)));
        }
    }
}

bool hasSeries(const DataFlashFftAnalyzer::Result &result,
               const QString &label)
{
    for (const auto &series : result.series) {
        if (series.label == label) {
            return true;
        }
    }
    return false;
}
}

class DataFlashFftServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void readsAsciiImuFallbackFromUnicodePath();
    void correlatesOutOfOrderBatchAndGloballyPrefersInstanceFive();
    void evictsLossyIncompleteBatchesWithoutFailingAnalysis();
    void readsBinarySixthAccelerometer();
    void enforcesTotalInputLimitAndCancellation();
    void rejectsMissingAndWrongFileTypes();
};

void DataFlashFftServiceTest::readsAsciiImuFallbackFromUnicodePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(
        QString::fromUtf8("частотный-анализ.log"));
    QVERIFY(writeFile(path, directImuLog(64, 64)));
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.startFrequencyHz = 5.0;
    options.magnitude = true;

    const auto result = DataFlashFftService::Analyze(path, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("IMU"));
    QCOMPARE(result.series.size(), 6);
    QVERIFY(hasSeries(result, QStringLiteral("IMU GYR x")));
    QVERIFY(hasSeries(result, QStringLiteral("IMU ACC z")));
    QVERIFY(std::abs(result.suggestedNotchHz - 8.0) < 0.01);
}

void DataFlashFftServiceTest::correlatesOutOfOrderBatchAndGloballyPrefersInstanceFive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("batch.log"));
    QByteArray log(
        "FMT,150,31,ISBH,QHBBHHQf,TimeUS,N,type,instance,mul,smp_cnt,SampleUS,smp_rate\n"
        "FMT,151,207,ISBD,QHHaaa,TimeUS,N,seqno,x,y,z\n");
    log += directImuLog(64, 64, 152);
    // An incomplete accelerometer batch must not become a source.
    log += "ISBH,0,7,0,0,100,64,0,64\n";
    log += batchDataLine(7, 0, 64, 0);
    // Complete sixth gyro (instance is zero based) arrives out of order.
    log += "ISBH,0,8,1,5,100,64,0,64\n";
    log += batchDataLine(8, 1, 64, 32);
    log += batchDataLine(8, 0, 64, 0);
    QVERIFY(writeFile(path, log));
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = true;

    const auto result = DataFlashFftService::Analyze(path, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("ISBH/ISBD"));
    QCOMPARE(result.series.size(), 3);
    QVERIFY(hasSeries(result, QStringLiteral("GYR5 x")));
    QVERIFY(!hasSeries(result, QStringLiteral("IMU GYR x")));
    QVERIFY(!hasSeries(result, QStringLiteral("ACC0 x")));
}

void DataFlashFftServiceTest::evictsLossyIncompleteBatchesWithoutFailingAnalysis()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("lossy-batches.log"));
    QByteArray log(
        "FMT,150,31,ISBH,QHBBHHQf,TimeUS,N,type,instance,mul,smp_cnt,SampleUS,smp_rate\n"
        "FMT,151,207,ISBD,QHHaaa,TimeUS,N,seqno,x,y,z\n");
    for (quint16 number = 1; number <= 3000; ++number) {
        log += "ISBH,0,";
        log += QByteArray::number(number);
        log += ",1,0,100,64,0,64\n";
        // Simulate a lost tail packet for every old batch.
        log += batchDataLine(number, 0, 64, 0);
    }
    log += "ISBH,0,4000,1,0,100,64,0,64\n";
    log += batchDataLine(4000, 1, 64, 32);
    log += batchDataLine(4000, 0, 64, 0);
    QVERIFY(writeFile(path, log));

    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = true;
    // The old unbounded pending map exceeded this shared budget well before
    // reaching the complete batch; the bounded 16-header window fits.
    options.maximumInputSamples = 1024;
    const auto result = DataFlashFftService::Analyze(path, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.source, QStringLiteral("ISBH/ISBD"));
    QVERIFY(hasSeries(result, QStringLiteral("GYR0 x")));
}

void DataFlashFftServiceTest::readsBinarySixthAccelerometer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sixth.BIN"));
    QByteArray log;
    constexpr quint8 headerId = 150;
    constexpr quint8 dataId = 151;
    appendFmt(&log, headerId, 31, "ISBH", "QHBBHHQf",
              "TimeUS,N,type,instance,mul,smp_cnt,SampleUS,smp_rate");
    appendFmt(&log, dataId, 207, "ISBD", "QHHaaa",
              "TimeUS,N,seqno,x,y,z");
    appendBatchHeader(&log, headerId, 42, 0, 5, 64, 64.0f);
    appendBatchData(&log, dataId, 42, 0, 64, 0);
    appendBatchData(&log, dataId, 42, 1, 64, 32);
    QVERIFY(writeFile(path, log));
    DataFlashFftAnalyzer::Options options;
    options.bins = 6;
    options.magnitude = true;

    const auto result = DataFlashFftService::Analyze(path, options);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QVERIFY(hasSeries(result, QStringLiteral("ACC5 x")));
}

void DataFlashFftServiceTest::enforcesTotalInputLimitAndCancellation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bounded.log"));
    QVERIFY(writeFile(path, directImuLog(40, 64)));
    DataFlashFftAnalyzer::Options options;
    options.bins = 4;
    options.maximumInputSamples = 64; // each IMU row retains gyro + accel
    auto bounded = DataFlashFftService::Analyze(path, options);
    QVERIFY(!bounded.succeeded);
    QVERIFY(bounded.error.contains(QStringLiteral("bounded")));

    options.maximumInputSamples = 128;
    options.isCancelled = []() { return true; };
    const auto cancelled = DataFlashFftService::Analyze(path, options);
    QVERIFY(cancelled.cancelled);
    QVERIFY(!cancelled.succeeded);
}

void DataFlashFftServiceTest::rejectsMissingAndWrongFileTypes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DataFlashFftAnalyzer::Options options;
    QVERIFY(DataFlashFftService::Analyze(
                directory.filePath(QStringLiteral("missing.bin")), options)
                .error.contains(QStringLiteral("does not exist")));
    const QString wrong = directory.filePath(QStringLiteral("fft.txt"));
    QVERIFY(writeFile(wrong, QByteArrayLiteral("not a log")));
    QVERIFY(DataFlashFftService::Analyze(wrong, options).error.contains(
        QStringLiteral(".bin or .log")));
}

QTEST_MAIN(DataFlashFftServiceTest)

#include "test_dataflashfftservice.moc"
