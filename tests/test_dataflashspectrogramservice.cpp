#include <QtTest>

#include "logging.h"
#include "ui/Loghandling/DataFlashSpectrogramService.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

Q_LOGGING_CATEGORY(apmGeneral, "apm.general.test")

class DataFlashSpectrogramServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void readsAsciiDirectSensorsFromUnicodePath();
    void readsAsciiBatchSensors();
    void readsBinaryBatchSensorsAndKeepsAxesDistinct();
    void rejectsMissingFilesSensorsAndCancellation();
};

namespace
{
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

void appendBatchHeader(QByteArray *bytes, quint8 id, quint64 timeUs,
                       quint16 batch, quint8 type, quint8 instance,
                       quint16 multiplier, quint16 sampleCount,
                       quint64 sampleUs, float sampleRate)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(id));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << timeUs << batch << type << instance
           << multiplier << sampleCount << sampleUs;
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << sampleRate;
}

void appendBatchData(QByteArray *bytes, quint8 id, quint64 timeUs,
                     quint16 batch, quint16 sequence,
                     int firstSample, double sampleRate)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(id));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << timeUs << batch << sequence;
    const double twoPi = 2.0 * std::acos(-1.0);
    const double frequencies[] = {20.0, 40.0, 60.0};
    for (double frequency : frequencies) {
        for (int offset = 0; offset < 32; ++offset) {
            const int sample = firstSample + offset;
            const double value = 12000.0 * std::sin(
                twoPi * frequency * sample / sampleRate);
            stream << qint16(std::lround(value));
        }
    }
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size()
        && file.flush();
}
}

void DataFlashSpectrogramServiceTest::readsAsciiDirectSensorsFromUnicodePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(
        QString::fromUtf8("спектрограмма.log"));
    QByteArray log(
        "FMT,150,23,ACC1,Qfff,TimeUS,AccX,AccY,AccZ\n"
        "FMT,151,11,DUMY,Q,TimeUS\n");
    constexpr int sampleRate = 400;
    constexpr int sampleCount = 2048;
    const double twoPi = 2.0 * std::acos(-1.0);
    for (int sample = 0; sample < sampleCount; ++sample) {
        log += "ACC1,";
        log += QByteArray::number(qulonglong(sample * 2500));
        for (double frequency : {25.0, 50.0, 75.0}) {
            log += ',';
            log += QByteArray::number(
                std::sin(twoPi * frequency * sample / sampleRate),
                'g', 16);
        }
        log += '\n';
    }
    log += "BROKEN,1,2\n";
    QVERIFY(writeFile(path, log));

    const auto result = DataFlashSpectrogramService::Generate(
        path, QStringLiteral("ACC1"), -80, -20);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.inputSampleCount, sampleCount);
    QCOMPARE(result.sensorName, QStringLiteral("ACC1"));
    QVERIFY(result.renderedWindowCount > 0);
    QCOMPARE(result.axes[0].image.height(),
             DataFlashSpectrogramAnalyzer::FrequencyBinCount);
    for (int axis = 0; axis < 3; ++axis) {
        QVERIFY(!result.axes[axis].image.isNull());
    }
    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 25.0) < 0.5);
    QVERIFY(std::abs(result.axes[1].strongestFrequencyHz - 50.0) < 0.5);
    QVERIFY(std::abs(result.axes[2].strongestFrequencyHz - 75.0) < 0.5);
    QVERIFY(result.message.contains(QStringLiteral("Log parser reported")));
}

void DataFlashSpectrogramServiceTest::readsAsciiBatchSensors()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("batch.log"));
    constexpr int sampleRate = 320;
    constexpr int sampleCount = 1024;
    constexpr int batch = 8;
    const double twoPi = 2.0 * std::acos(-1.0);
    const double frequencies[] = {20.0, 40.0, 60.0};
    QByteArray log(
        "FMT,150,31,ISBH,QHBBHHQf,TimeUS,N,type,instance,mul,smp_cnt,SampleUS,smp_rate\n"
        "FMT,151,207,ISBD,QHHaaa,TimeUS,N,seqno,x,y,z\n"
        "ISBH,999000,8,1,4,100,1024,1000000,320\n");
    for (int sequence = 0; sequence < sampleCount / 32; ++sequence) {
        const int firstSample = sequence * 32;
        const quint64 timeUs = 1000000ULL
            + quint64(firstSample) * 1000000ULL / sampleRate;
        log += "ISBD,";
        log += QByteArray::number(timeUs);
        log += ',';
        log += QByteArray::number(batch);
        log += ',';
        log += QByteArray::number(sequence);
        for (double frequency : frequencies) {
            for (int offset = 0; offset < 32; ++offset) {
                const int sample = firstSample + offset;
                const double value = 12000.0 * std::sin(
                    twoPi * frequency * sample / sampleRate);
                log += ',';
                log += QByteArray::number(
                    static_cast<qlonglong>(std::lround(value)));
            }
        }
        log += '\n';
    }
    QVERIFY(writeFile(path, log));

    const auto result = DataFlashSpectrogramService::Generate(
        path, QStringLiteral("GYR5"), -80, -20);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.inputSampleCount, sampleCount);
    QCOMPARE(result.sourceKind,
             DataFlashSpectrogramAnalyzer::SourceKind::Batch);
    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 20.0) < 0.5);
    QVERIFY(std::abs(result.axes[1].strongestFrequencyHz - 40.0) < 0.5);
    QVERIFY(std::abs(result.axes[2].strongestFrequencyHz - 60.0) < 0.5);
}

void DataFlashSpectrogramServiceTest::readsBinaryBatchSensorsAndKeepsAxesDistinct()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("batch.BIN"));
    QByteArray log;
    constexpr quint8 headerId = 150;
    constexpr quint8 dataId = 151;
    constexpr int sampleRate = 320;
    constexpr int sampleCount = 1024;
    constexpr quint16 batch = 7;
    appendFmt(&log, headerId, 31, "ISBH", "QHBBHHQf",
              "TimeUS,N,type,instance,mul,smp_cnt,SampleUS,smp_rate");
    appendFmt(&log, dataId, 207, "ISBD", "QHHaaa",
              "TimeUS,N,seqno,x,y,z");
    appendBatchHeader(&log, headerId, 999000, batch, 0, 3,
                      100, sampleCount, 1000000, float(sampleRate));
    for (int sequence = 0; sequence < sampleCount / 32; ++sequence) {
        const int firstSample = sequence * 32;
        const quint64 timeUs = 1000000ULL
            + quint64(firstSample) * 1000000ULL / sampleRate;
        appendBatchData(&log, dataId, timeUs, batch,
                        quint16(sequence), firstSample, sampleRate);
    }
    QVERIFY(writeFile(path, log));

    const auto result = DataFlashSpectrogramService::Generate(
        path, QStringLiteral("ACC4"), -80, -20);
    QVERIFY2(result.ok(), qPrintable(result.message));
    QCOMPARE(result.inputSampleCount, sampleCount);
    QCOMPARE(result.sourceKind,
             DataFlashSpectrogramAnalyzer::SourceKind::Batch);
    QVERIFY(std::abs(result.axes[0].strongestFrequencyHz - 20.0) < 0.5);
    QVERIFY(std::abs(result.axes[1].strongestFrequencyHz - 40.0) < 0.5);
    QVERIFY(std::abs(result.axes[2].strongestFrequencyHz - 60.0) < 0.5);
    QVERIFY(result.axes[0].image != result.axes[1].image);
    QVERIFY(result.axes[1].image != result.axes[2].image);
}

void DataFlashSpectrogramServiceTest::rejectsMissingFilesSensorsAndCancellation()
{
    auto missing = DataFlashSpectrogramService::Generate(
        QStringLiteral("/definitely/missing/spectrogram.bin"),
        QStringLiteral("ACC1"), -80, -20);
    QVERIFY(!missing.ok());
    QCOMPARE(missing.error,
             DataFlashSpectrogramAnalyzer::ErrorCode::InvalidRecord);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("small.log"));
    QVERIFY(writeFile(
        path,
        QByteArray("FMT,150,23,ACC1,Qfff,TimeUS,AccX,AccY,AccZ\n"
                   "FMT,151,11,DUMY,Q,TimeUS\n"
                   "ACC1,0,1,2,3\n")));
    auto invalidSensor = DataFlashSpectrogramService::Generate(
        path, QStringLiteral("MAG1"), -80, -20);
    QVERIFY(!invalidSensor.ok());
    QCOMPARE(invalidSensor.error,
             DataFlashSpectrogramAnalyzer::ErrorCode::InvalidSensor);

    auto invalidRange = DataFlashSpectrogramService::Generate(
        QStringLiteral("/not/read/because/options-are-invalid.log"),
        QStringLiteral("ACC1"), -20, -20);
    QVERIFY(!invalidRange.ok());
    QCOMPARE(invalidRange.error,
             DataFlashSpectrogramAnalyzer::ErrorCode::InvalidDecibelRange);

    auto cancelled = DataFlashSpectrogramService::Generate(
        path, QStringLiteral("ACC1"), -80, -20,
        []() { return true; });
    QVERIFY(!cancelled.ok());
    QCOMPARE(cancelled.error,
             DataFlashSpectrogramAnalyzer::ErrorCode::Cancelled);
}

QTEST_MAIN(DataFlashSpectrogramServiceTest)
#include "test_dataflashspectrogramservice.moc"
