#include "ui/Loghandling/DataFlashMatlabExporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
using Exporter = DataFlashMatlabExporter;
bool writeFile(const QString &path, const QByteArray &bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray readFile(const QString &path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QStringList directoryEntries(const QString &path)
{ return QDir(path).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name); }
quint32 u32(const QByteArray &bytes, int offset)
{ return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset)); }
quint64 bits(const QByteArray &bytes, int offset)
{ return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(bytes.constData() + offset)); }
struct Matrix {
    bool valid = false;
    quint32 type = 0;
    int rows = 0, columns = 0, end = 0;
    QByteArray name;
    QString text;
    QVector<double> numbers;
    QVector<quint64> numberBits;
    QVector<Matrix> children;
};
Matrix parseMatrix(const QByteArray &bytes, int start, int depth = 0)
{
    Matrix value;
    if (depth > 20 || start < 0 || start > bytes.size() - 48 || u32(bytes, start) != 14) return value;
    const auto size = u32(bytes, start + 4);
    if (size > quint32(bytes.size() - start - 8)) return value;
    value.end = start + int(size) + 8;
    if (u32(bytes, start + 8) != 6 || u32(bytes, start + 12) != 8
        || u32(bytes, start + 24) != 5 || u32(bytes, start + 28) != 8
        || u32(bytes, start + 40) != 1) return value;
    value.type = u32(bytes, start + 16);
    value.rows = qint32(u32(bytes, start + 32)); value.columns = qint32(u32(bytes, start + 36));
    if (value.rows < 0 || value.columns < 0 || qint64(value.rows) * value.columns > 1000000) return value;
    const auto nameSize = u32(bytes, start + 44);
    if (nameSize > quint32(value.end - start - 48)) return value;
    value.name = bytes.mid(start + 48, int(nameSize));
    int at = start + 48 + int((nameSize + 7) & ~quint32(7));
    const int count = value.rows * value.columns;
    if (value.type == 1) {
        for (int i = 0; i < count; ++i) {
            const auto child = parseMatrix(bytes, at, depth + 1);
            if (!child.valid || child.end > value.end) return value;
            value.children.append(child); at = child.end;
        }
    } else if (value.type == 4 || value.type == 6) {
        if (at > value.end - 8) return value;
        const auto type = u32(bytes, at), length = u32(bytes, at + 4); at += 8;
        if (length > quint32(value.end - at)) return value;
        if (value.type == 6) {
            if (type != 9 || qint64(count) * 8 != length) return value;
            for (int i = 0; i < count; ++i) {
                const auto raw = bits(bytes, at + i * 8); double number;
                std::memcpy(&number, &raw, sizeof(number));
                value.numbers.append(number); value.numberBits.append(raw);
            }
        } else {
            if (type != 4 || qint64(count) * 2 != length) return value;
            for (int i = 0; i < count; ++i)
                value.text += QChar(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + at + i * 2)));
        }
        at += int((length + 7) & ~quint32(7));
    } else return value;
    value.valid = at == value.end; return value;
}
QVector<Matrix> decode(const QString &path)
{
    const auto bytes = readFile(path); QVector<Matrix> matrices;
    if (bytes.size() < 128 || bytes.mid(126, 2) != "IM") return {};
    int at = 128;
    while (at < bytes.size()) {
        auto matrix = parseMatrix(bytes, at); if (!matrix.valid) return {};
        at = matrix.end; matrices.append(matrix);
    }
    return matrices;
}
Matrix named(const QVector<Matrix> &matrices, const QByteArray &name)
{ for (const auto &matrix : matrices) if (matrix.name == name) return matrix; return {}; }
QStringList texts(const Matrix &matrix)
{ QStringList result; for (const auto &child : matrix.children) result.append(child.text); return result; }
QByteArray packet(int id, const QByteArray &payload)
{ return QByteArray::fromHex("a395") + char(id) + payload; }
QByteArray fmt(int id, int length, const QByteArray &name, const QByteArray &format, const QByteArray &columns)
{
    return packet(128, QByteArray(1, char(id)) + char(length) + name.leftJustified(4, '\0')
        + format.leftJustified(16, '\0') + columns.leftJustified(64, '\0'));
}
QByteArray doubleBytes(double value)
{
    quint64 raw; std::memcpy(&raw, &value, sizeof(raw)); QByteArray bytes(8, '\0');
    qToLittleEndian(raw, reinterpret_cast<uchar *>(bytes.data())); return bytes;
}
QByteArray basicLog()
{ return "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\nGPS,1000,47.5,8.5\nGPS,2000,-0,9.5\n"; }
struct LocaleGuard { QLocale previous = QLocale(); ~LocaleGuard() { QLocale::setDefault(previous); } };
}

class DataFlashMatlabExporterTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QLocale::setDefault(QLocale::c()); }
    void exactPlanLabelsNumericCellsParametersAndSeen()
    {
        QTemporaryDir directory; const auto path = directory.filePath("complete.log");
        const QByteArray input = "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\n"
            "FMT,151,75,MSG,QZ,TimeUS,Message\nFMT,152,75,ISBD,Qa,TimeUS,Samples\n"
            "FMT,153,23,PARM,Nf,Name,Value\nGPS,1000,47.5,8.5\nMSG,2000,hello\n"
            "ISBD,3000,[1 -2 3]\nPARM,ZETA,2\nPARM,ALPHA,1\nPARM,ZETA,4\nGPS,0,-0,2\n";
        QVERIFY(writeFile(path, input));
        const auto prepared = Exporter::Prepare(path); QVERIFY2(prepared.success, qPrintable(prepared.error));
        QVERIFY(prepared.plan && prepared.plan->isValid());
        QCOMPARE(prepared.plan->recordCount(), qint64(11)); QCOMPARE(prepared.plan->variableCount(), 10);
        QCOMPARE(prepared.plan->outputPath(), path + "-11.mat");
        QCOMPARE(directoryEntries(directory.path()), QStringList({"complete.log"}));
        const auto result = Exporter::Export(*prepared.plan); QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.recordCount, qint64(11)); QCOMPARE(result.variableCount, 10);
        QCOMPARE(result.bytesWritten, QFileInfo(result.outputPath).size());
        QCOMPARE(result.bytesWritten, prepared.plan->estimatedBytes()); QCOMPARE(readFile(path), input);
        const auto matrices = decode(result.outputPath); QCOMPARE(matrices.size(), 10);
        QStringList names; for (const auto &matrix : matrices) names.append(QString::fromUtf8(matrix.name));
        QCOMPARE(names, QStringList({"GPS_label", "MSG_label", "ISBD_label", "GPS", "MSG", "ISBD", "MSG1", "ISBD1", "PARM", "Seen"}));
        QCOMPARE(texts(named(matrices, "GPS_label")), QStringList({"LineNo", "TimeUS", "Lat", "Lng"}));
        const auto gps = named(matrices, "GPS"); QCOMPARE(gps.rows, 2); QCOMPARE(gps.columns, 4);
        QCOMPARE(gps.numbers, QVector<double>({5, 11, 1000, 0, 47.5, -0.0, 8.5, 2}));
        QCOMPARE(gps.numberBits[5], Q_UINT64_C(0x8000000000000000));
        const auto message = named(matrices, "MSG1"); QCOMPARE(message.rows, 1); QCOMPARE(message.columns, 1);
        const auto messageRow = message.children[0]; QCOMPARE(messageRow.rows, 3); QCOMPARE(messageRow.columns, 1);
        QCOMPARE(messageRow.children[0].text, QStringLiteral("MSG")); QCOMPARE(messageRow.children[1].numbers, QVector<double>({2000}));
        QCOMPARE(messageRow.children[2].text, QStringLiteral("hello"));
        const auto array = named(matrices, "ISBD1").children[0].children[2];
        // MP10 splits the original bracket token, including its trailing LF;
        // that final token becomes an empty csmatio char matrix (0x0).
        QCOMPARE(array.type, quint32(1)); QCOMPARE(array.rows, 4); QCOMPARE(array.columns, 1);
        QCOMPARE(array.children[0].numbers, QVector<double>({1})); QCOMPARE(array.children[1].numbers, QVector<double>({-2}));
        QCOMPARE(array.children[3].type, quint32(4)); QCOMPARE(array.children[3].rows, 0); QCOMPARE(array.children[3].columns, 0);
        const auto parameters = named(matrices, "PARM"); QCOMPARE(parameters.rows, 2); QCOMPARE(parameters.columns, 2);
        QCOMPARE(parameters.children[0].text, QStringLiteral("ALPHA")); QCOMPARE(parameters.children[1].text, QStringLiteral("ZETA"));
        QCOMPARE(parameters.children[2].numbers, QVector<double>({1})); QCOMPARE(parameters.children[3].numbers, QVector<double>({4}));
        QCOMPARE(texts(named(matrices, "Seen")), QStringList({"GPS", "MSG", "ISBD"}));
        QCOMPARE(directoryEntries(directory.path()), QStringList({"complete.log", "complete.log-11.mat"}));
    }

    void lateFmtuSplitsAllInstancesWithoutNumericMetadata()
    {
        QTemporaryDir directory; const auto path = directory.filePath("instances.log");
        QVERIFY(writeFile(path, "FMT,150,16,SENS,QBf,TimeUS,I,Value\n"
            "FMT,160,44,FMTU,QBNN,TimeUS,FmtType,UnitIds,MultIds\n"
            "SENS,1000,0,1.5\nSENS,2000,1,2.5\nFMTU,3000,150,s#-,000\n"));
        const auto plan = Exporter::Prepare(path); QVERIFY2(plan.success, qPrintable(plan.error));
        const auto result = Exporter::Export(*plan.plan); QVERIFY2(result.success, qPrintable(result.error));
        const auto matrices = decode(result.outputPath);
        QCOMPARE(named(matrices, "SENS_0").numbers, QVector<double>({3, 1000, 0, 1.5}));
        QCOMPARE(named(matrices, "SENS_1").numbers, QVector<double>({4, 2000, 1, 2.5}));
        QVERIFY(!named(matrices, "SENS").valid); QVERIFY(!named(matrices, "FMTU").valid);
        QCOMPARE(texts(named(matrices, "Seen")), QStringList({"SENS"}));
        QVERIFY(!result.warnings.isEmpty());
    }

    void binaryNumericBitsAndAsciiStringPolicy()
    {
        QTemporaryDir directory; const auto path = directory.filePath("binary.bin");
        QByteArray input = fmt(150, 19, "VAL", "dd", "A,B") + fmt(151, 67, "MSG", "Z", "Message");
        input += packet(150, doubleBytes(-0.0) + doubleBytes(std::numeric_limits<double>::infinity()));
        input += packet(150, doubleBytes(std::numeric_limits<double>::quiet_NaN()) + doubleBytes(-2.675));
        input += packet(151, QByteArray("caf\xc3\xa9").leftJustified(64, '\0'));
        QVERIFY(writeFile(path, input)); const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error)); const auto result = Exporter::Export(*plan.plan);
        QVERIFY2(result.success, qPrintable(result.error)); const auto matrices = decode(result.outputPath);
        const auto val = named(matrices, "VAL"); QCOMPARE(val.rows, 2); QCOMPARE(val.columns, 3);
        QCOMPARE(val.numberBits[2], Q_UINT64_C(0x8000000000000000));
        QCOMPARE(val.numberBits[3], Q_UINT64_C(0xfff8000000000000)); // Actual .NET Double.Parse("NaN").
        QCOMPARE(val.numberBits[4], Q_UINT64_C(0x7ff0000000000000)); QCOMPARE(val.numbers[5], -2.675);
        const auto message = named(matrices, "MSG1"); QVERIFY(message.valid); QCOMPARE(message.children.size(), 1);
        QCOMPARE(message.children[0].children.size(), 2);
        QCOMPARE(message.children[0].children[1].text, QStringLiteral("caf??"));
        QCOMPARE(readFile(path), input);
    }

    void emptyAndTextLineAccounting_data()
    {
        QTest::addColumn<QByteArray>("input"); QTest::addColumn<qint64>("records"); QTest::addColumn<int>("rows");
        QTest::newRow("empty") << QByteArray() << qint64(0) << 0;
        QTest::newRow("unterminated") << QByteArray("FMT,150,11,TST,Q,TimeUS\nTST,100\nTST,200") << qint64(2) << 1;
        QTest::newRow("blank-counted") << QByteArray("FMT,150,11,TST,Q,TimeUS\n\nTST,100\n") << qint64(3) << 1;
    }

    void asciiSourceBecomesUtf16MatCharactersWithoutUnicodeGuessing()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QString::fromUtf8("текст.log"));
        const QByteArray input = QByteArray("FMT,150,67,MSG,Z,Message\nMSG,caf") + QByteArray::fromHex("c3a9") + "\n";
        QVERIFY(writeFile(path, input)); const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error)); const auto result = Exporter::Export(*plan.plan);
        QVERIFY2(result.success, qPrintable(result.error)); const auto matrices = decode(result.outputPath);
        const auto table = named(matrices, "MSG1"); QVERIFY(table.valid); QCOMPARE(table.children.size(), 1);
        QCOMPARE(table.children[0].children.size(), 2);
        const auto text = table.children[0].children[1]; QCOMPARE(text.type, quint32(4));
        QCOMPARE(text.text, QStringLiteral("caf??")); QCOMPARE(text.rows, 1); QCOMPARE(text.columns, 5);
        QCOMPARE(readFile(path), input);
    }
    void emptyAndTextLineAccounting()
    {
        QFETCH(QByteArray, input); QFETCH(qint64, records); QFETCH(int, rows);
        QTemporaryDir directory; const auto path = directory.filePath("lines.log"); QVERIFY(writeFile(path, input));
        const auto plan = Exporter::Prepare(path); QVERIFY2(plan.success, qPrintable(plan.error));
        QCOMPARE(plan.plan->recordCount(), records); QCOMPARE(plan.plan->outputPath(), path + '-' + QString::number(records) + ".mat");
        const auto result = Exporter::Export(*plan.plan); QVERIFY2(result.success, qPrintable(result.error));
        const auto matrices = decode(result.outputPath); const auto parameters = named(matrices, "PARM");
        QVERIFY(parameters.valid); QCOMPARE(parameters.rows, 0); QCOMPARE(parameters.columns, 2);
        QCOMPARE(named(matrices, "Seen").columns, 1);
        if (rows) { const auto values = named(matrices, "TST"); QCOMPARE(values.rows, rows); QCOMPARE(values.numbers.last(), 100.0); }
        else QCOMPARE(matrices.size(), 2);
    }

    void cancellationAndSourceMutation_data()
    {
        QTest::addColumn<QString>("stage");
        for (const QString &stage : {QString("prepare-cancel"), QString("prepare-change"), QString("export-entry-cancel"),
            QString("consent-change"), QString("export-final-cancel"), QString("export-final-grow"), QString("export-final-output")})
            QTest::newRow(qPrintable(stage)) << stage;
    }
    void cancellationAndSourceMutation()
    {
        QFETCH(QString, stage); QTemporaryDir directory; const auto path = directory.filePath("safe.log");
        const auto input = basicLog(); QVERIFY(writeFile(path, input));
        bool stop = stage == "prepare-cancel"; bool mutated = false; bool callbackOk = true;
        auto plan = Exporter::Prepare(path, [&] { return stop; }, [&](qint64 done, qint64 total) {
            if (stage == "prepare-change" && !mutated && done * 2 == total) {
                mutated = true; callbackOk = writeFile(path, QByteArray(input).replace("47.5", "48.5"));
            }
        });
        QVERIFY(callbackOk);
        if (stage.startsWith("prepare")) { QVERIFY(!plan.success); QVERIFY(plan.cancelled || !plan.error.isEmpty());
            QCOMPARE(directoryEntries(directory.path()), QStringList({"safe.log"})); return; }
        QVERIFY2(plan.success, qPrintable(plan.error));
        const auto output = plan.plan->outputPath();
        if (stage == "consent-change") {
            const auto modified = QFileInfo(path).lastModified();
            QVERIFY(writeFile(path, QByteArray(input).replace("47.5", "48.5")));
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        }
        stop = stage == "export-entry-cancel";
        // Export work = three source passes plus consented output bytes. This
        // notification is the final pre-publication observer, not success100%.
        const qint64 finalScan = qint64(input.size()) * 3;
        const auto result = Exporter::Export(*plan.plan, [&] { return stop; }, [&](qint64 done, qint64) {
            if (mutated || done != finalScan) return;
            if (stage == "export-final-cancel") { stop = true; mutated = true; }
            else if (stage == "export-final-grow") { callbackOk = writeFile(path, input + "\n"); mutated = true; }
            else if (stage == "export-final-output") { callbackOk = writeFile(output, "existing owner"); mutated = true; }
        });
        QVERIFY(callbackOk); QVERIFY(!result.success); QVERIFY(result.cancelled || !result.error.isEmpty());
        if (stage.startsWith("export-final")) QVERIFY(mutated);
        if (stage.endsWith("output")) { QCOMPARE(readFile(output), QByteArray("existing owner"));
            QCOMPARE(directoryEntries(directory.path()), QStringList({"safe.log", "safe.log-3.mat"})); }
        else { QVERIFY(!QFileInfo::exists(output)); QCOMPARE(directoryEntries(directory.path()), QStringList({"safe.log"})); }
    }

    void admissionCollisionsSymlinksAndResourceBounds()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.log");
        QVERIFY(!Exporter::Prepare(path).success); QVERIFY(!Exporter::Export(Exporter::Plan()).success);
        QVERIFY(writeFile(path, basicLog())); const auto plan = Exporter::Prepare(path); QVERIFY(plan.success);
        const auto output = plan.plan->outputPath(); QVERIFY(writeFile(output, "keep"));
        QVERIFY(!Exporter::Prepare(path).success); QVERIFY(!Exporter::Export(*plan.plan).success);
        QCOMPARE(readFile(output), QByteArray("keep")); QVERIFY(QFile::remove(output));
#ifdef Q_OS_UNIX
        const auto alias = directory.filePath("alias.log"); QVERIFY(QFile::link(path, alias));
        QVERIFY(!Exporter::Prepare(alias).success);
        const auto target = directory.filePath("foreign"); QVERIFY(writeFile(target, "foreign"));
        QVERIFY(QFile::link(target, output)); QVERIFY(!Exporter::Export(*plan.plan).success);
        QCOMPARE(readFile(target), QByteArray("foreign")); QVERIFY(QFile::remove(output));
#endif
        const auto large = directory.filePath("large.bin"); QFile sparse(large); QVERIFY(sparse.open(QIODevice::WriteOnly));
        QVERIFY(sparse.resize(Exporter::MaximumInputBytes + 1)); sparse.close();
        QVERIFY(!Exporter::Prepare(large).success); QVERIFY(!QFileInfo::exists(large + "-0.mat"));
        const auto corrupt = directory.filePath("bad.bin"); QVERIFY(writeFile(corrupt, "not DataFlash"));
        QVERIFY(!Exporter::Prepare(corrupt).success);
    }

    void immutablePlanAndCancellationAfterPublication()
    {
        QTemporaryDir directory; const auto path = directory.filePath("pinned.log"); QVERIFY(writeFile(path, basicLog()));
        const auto prepared = Exporter::Prepare(path); QVERIFY2(prepared.success, qPrintable(prepared.error));
        Exporter::Plan plan = *prepared.plan; const auto output = plan.outputPath();
        bool reset = false, stop = false; qint64 last = 0; bool monotonic = true;
        const auto result = Exporter::Export(plan, [&] { return stop; }, [&](qint64 done, qint64 total) {
            monotonic = monotonic && done >= last && done <= total; last = done;
            if (!reset) { plan = Exporter::Plan(); reset = true; }
            if (done == total) stop = true; // Completion notification cannot undo publication.
        });
        QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(!result.cancelled);
        QVERIFY(reset); QVERIFY(stop); QVERIFY(monotonic); QVERIFY(!plan.isValid());
        QCOMPARE(result.outputPath, output); QVERIFY(QFileInfo::exists(output));
        QCOMPARE(directoryEntries(directory.path()), QStringList({"pinned.log", "pinned.log-3.mat"}));
    }

    void explicitParameterAndVariableBounds()
    {
        QTemporaryDir directory; const auto path = directory.filePath("bounded.log");
        QByteArray input = "FMT,150,23,PARM,Nf,Name,Value\n";
        for (int i = 0; i <= Exporter::MaximumParameters; ++i)
            input += "PARM,P" + QByteArray::number(i) + ",1\n";
        QVERIFY(writeFile(path, input)); auto result = Exporter::Prepare(path);
        QVERIFY(!result.success); QVERIFY(result.error.contains("10000") || result.error.contains("parameter", Qt::CaseInsensitive));
        input = "FMT,150,17,SENS,QHf,TimeUS,I,Value\n"
            "FMT,160,44,FMTU,QBNN,TimeUS,FmtType,UnitIds,MultIds\nFMTU,1,150,s#-,000\n";
        for (int i = 0; i <= Exporter::MaximumVariables; ++i)
            input += "SENS,1000," + QByteArray::number(i) + ",1\n";
        QVERIFY(writeFile(path, input)); result = Exporter::Prepare(path);
        QVERIFY(!result.success); QVERIFY(result.error.contains("4096") || result.error.contains("variable", Qt::CaseInsensitive));
        QCOMPARE(directoryEntries(directory.path()), QStringList({"bounded.log"}));
    }

    void parameterAndMessageNumberStylesMatchReference()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("styles.log");
        const QByteArray input =
            "FMT,151,75,MSG,QZ,TimeUS,Message\n"
            "FMT,153,23,PARM,Nf,Name,Value\n"
            "PARM,TRAILING,12-\n"
            "PARM,PARENTHESES,(3.25)\n"
            "PARM,LEADING,-4\n"
            "PARM,VALID,5\n"
            "MSG,1,inf\n"
            "MSG,2,+INF\n"
            "MSG,3,-Inf\n"
            "MSG,4,Infinity\n";
        QVERIFY(writeFile(path, input));
        const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error));
        const auto result = Exporter::Export(*plan.plan);
        QVERIFY2(result.success, qPrintable(result.error));
        const auto matrices = decode(result.outputPath);

        const auto parameters = named(matrices, "PARM");
        QCOMPARE(parameters.rows, 2);
        QCOMPARE(parameters.columns, 2);
        QCOMPARE(parameters.children[0].text, QStringLiteral("LEADING"));
        QCOMPARE(parameters.children[1].text, QStringLiteral("VALID"));
        QCOMPARE(parameters.children[2].numbers, QVector<double>({-4}));
        QCOMPARE(parameters.children[3].numbers, QVector<double>({5}));

        const auto messages = named(matrices, "MSG1");
        QCOMPARE(messages.children.size(), 4);
        QCOMPARE(messages.children[0].children[2].text, QStringLiteral("inf"));
        QCOMPARE(messages.children[1].children[2].text, QStringLiteral("+INF"));
        QCOMPARE(messages.children[2].children[2].text, QStringLiteral("-Inf"));
        QCOMPARE(messages.children[3].children[2].type, quint32(6));
        QVERIFY(std::isinf(messages.children[3].children[2].numbers.value(0)));
    }

    void binaryFirmwareIsPrimedBeforeModeEnumeration()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("modes.bin");
        const QByteArray time100 = QByteArray::fromHex("6400000000000000");
        const QByteArray time200 = QByteArray::fromHex("c800000000000000");
        const QByteArray time300 = QByteArray::fromHex("2c01000000000000");
        QByteArray input = fmt(150, 12, "MODE", "QM", "TimeUS,Mode")
            + packet(150, time100 + char(3))
            + fmt(151, 67, "MSG", "Z", "Message")
            + packet(151, QByteArray("ArduCopter").leftJustified(64, '\0'))
            + packet(150, time200 + char(3))
            + packet(150, time300 + char(28));
        QVERIFY(writeFile(path, input));
        const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error));
        const auto result = Exporter::Export(*plan.plan);
        QVERIFY2(result.success, qPrintable(result.error));
        const auto mode = named(decode(result.outputPath), "MODE");
        QCOMPARE(mode.rows, 3);
        QCOMPARE(mode.columns, 3);
        // DFLogBuffer primes Copter from MSG before ProcessLog enumerates even
        // the earlier MODE. Both Auto and the shipped overlay's Turtle mode
        // become text/zero. Old Debug bins without the overlay are not an oracle.
        QCOMPARE(mode.numbers, QVector<double>({2, 5, 6, 100, 200, 300,
                                                0, 0, 0}));
    }

    void emptyBinaryBlobRowsAreSkippedWithoutSyntheticSuccessorDuplicates()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("empty-z.bin");
        const QByteArray filePayload = QByteArray("x").leftJustified(16, '\0')
            + QByteArray(5, '\0') + QByteArray(64, '\0');
        const QByteArray unitPayload = QByteArray(9, '\0')
            + QByteArray(64, '\0');
        QByteArray input = fmt(205, 88, "FILE", "NIBZ",
                               "FileName,Offset,Length,Data")
            + fmt(150, 19, "VAL", "Qd", "TimeUS,Value")
            + fmt(119, 76, "UNIT", "QbZ", "TimeUS,Id,Label")
            + fmt(60, 75, "MSG", "QZ", "TimeUS,Message")
            + packet(205, filePayload) + packet(205, filePayload)
            + packet(150, QByteArray::fromHex("0100000000000000")
                              + doubleBytes(42))
            + packet(119, unitPayload)
            + packet(60, QByteArray::fromHex("0200000000000000")
                             + QByteArray("hello").leftJustified(64, '\0'));
        QVERIFY(writeFile(path, input));
        const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error));
        const auto result = Exporter::Export(*plan.plan);
        QVERIFY2(result.success, qPrintable(result.error));
        const auto matrices = decode(result.outputPath);
        const auto value = named(matrices, "VAL");
        QCOMPARE(value.rows, 1);
        QCOMPARE(value.numbers, QVector<double>({7, 1, 42}));
        const auto message = named(matrices, "MSG1");
        QCOMPARE(message.children.size(), 1);
        QCOMPARE(message.children[0].children[0].text, QStringLiteral("MSG"));
        QCOMPARE(message.children[0].children[2].text, QStringLiteral("hello"));
        QVERIFY(result.warnings.join(' ').contains("3 empty", Qt::CaseInsensitive));
    }

    void stagingPathOwnership_data()
    {
        QTest::addColumn<QString>("when");
#ifdef Q_OS_UNIX
        QTest::newRow("replacement-before-publish") << QStringLiteral("replace");
        QTest::newRow("replacement-before-cancel") << QStringLiteral("cancel");
#endif
        QTest::newRow("recreated-after-publish") << QStringLiteral("complete");
    }

    void stagingPathOwnership()
    {
        QFETCH(QString, when);
        QTemporaryDir directory;
        const QString path = directory.filePath("ownership.log");
        QVERIFY(writeFile(path, basicLog()));
        const auto plan = Exporter::Prepare(path);
        QVERIFY2(plan.success, qPrintable(plan.error));
        QString stagedPath;
        bool changed = false, callbackOk = true, stop = false;
        const QByteArray foreign(int(plan.plan->estimatedBytes()), 'F');
        const auto result = Exporter::Export(*plan.plan, [&] { return stop; },
            [&](qint64 done, qint64 total) {
                if (stagedPath.isEmpty()) {
                    const auto files = QDir(directory.path()).entryList(
                        {QStringLiteral("*.stage-*")}, QDir::Files | QDir::Hidden);
                    if (!files.isEmpty()) stagedPath = directory.filePath(files.first());
                }
                if (changed || stagedPath.isEmpty()) return;
                if (when == QStringLiteral("complete")) {
                    if (done != total) return;
                    // The original temporary pathname is now vacant. A new
                    // owner must not be deleted by old QTemporaryFile cleanup.
                    callbackOk = !QFileInfo::exists(stagedPath)
                        && writeFile(stagedPath, foreign);
                } else {
                    callbackOk = QFile::remove(stagedPath) && writeFile(stagedPath, foreign);
                    stop = when == QStringLiteral("cancel");
                }
                changed = true;
            });
        QVERIFY(changed); QVERIFY(callbackOk);
        QCOMPARE(readFile(stagedPath), foreign);
        if (when == QStringLiteral("complete")) {
            QVERIFY2(result.success, qPrintable(result.error));
            QVERIFY(readFile(result.outputPath).startsWith("MATLAB 5.0 MAT-file"));
        } else {
            QVERIFY(!result.success);
            QVERIFY(!QFileInfo::exists(plan.plan->outputPath()));
        }
    }
};

QTEST_GUILESS_MAIN(DataFlashMatlabExporterTest)
#include "test_dataflashmatlabexporter.moc"
