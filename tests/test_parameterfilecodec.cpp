#include "core/parameters/ParameterFileCodec.h"

#include <QBuffer>
#include <QLocale>
#include <QtTest>

#include <cmath>
#include <limits>

namespace {
using Codec = ConfigRawParamsFileCodec;

class RejectingWriter final : public QIODevice
{
public:
    RejectingWriter() { open(QIODevice::WriteOnly); }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *, qint64) override { return -1; }
};

QStringList names(const QVector<Codec::Entry> &entries)
{
    QStringList result;
    for (const auto &entry : entries) result.append(entry.name);
    return result;
}
}

class ParameterFileCodecTest final : public QObject
{
    Q_OBJECT
private slots:
    void orderedDuplicatesAndLegacyMap();
    void finiteValuesUseInvariantLocale();
    void referenceExclusions();
    void separatorsCommentsAndPermissiveNames();
    void byteOrderMarks_data();
    void byteOrderMarks();
    void malformedValues_data();
    void malformedValues();
    void limits_data();
    void limits();
    void unreadableAndNullOutputs();
    void saveCompatibilityAndFailures();
};

void ParameterFileCodecTest::orderedDuplicatesAndLegacyMap()
{
    QByteArray text("z,1\na,2\nZ,3\nb,4\na,5\nFORMAT_VERSION,99\n");
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> ordered{{QStringLiteral("OLD"), 9}};
    QString error = QStringLiteral("old error");
    int line = -1;
    QVERIFY(Codec::loadOrdered(&input, &ordered, &error, &line));
    QVERIFY(error.isEmpty());
    QCOMPARE(line, 0);
    QCOMPARE(names(ordered), QStringList({"Z", "A", "B"}));
    QCOMPARE(ordered.at(0).value, 3.0);
    QCOMPARE(ordered.at(1).value, 5.0);
    QCOMPARE(ordered.at(2).value, 4.0);
    QVERIFY(input.seek(0));
    QMap<QString, double> sorted{{QStringLiteral("OLD"), 9}};
    QVERIFY(Codec::load(&input, &sorted));
    QCOMPARE(sorted.keys(), QStringList({"A", "B", "Z"}));
    for (const auto &entry : ordered)
        QCOMPARE(sorted.value(entry.name), entry.value);
}

void ParameterFileCodecTest::finiteValuesUseInvariantLocale()
{
    struct RestoreLocale {
        QLocale original;
        ~RestoreLocale() { QLocale::setDefault(original); }
    } restore;
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    QByteArray text("NEG,-2.5\nPLUS,+3\nEXP,1e-3\nHALF,.5\n"
                    "ZERO,-0.0\nMAX,1.7976931348623157e308\n"
                    "PRECISE,1.2345678901234567\n");
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries;
    QVERIFY(Codec::loadOrdered(&input, &entries));
    QCOMPARE(entries.size(), 7);
    QCOMPARE(entries.at(0).value, -2.5);
    QCOMPARE(entries.at(1).value, 3.0);
    QCOMPARE(entries.at(2).value, 0.001);
    QCOMPARE(entries.at(3).value, 0.5);
    QCOMPARE(entries.at(4).value, 0.0);
    QCOMPARE(entries.at(5).value, std::numeric_limits<double>::max());
    QMap<QString, QVariant> values;
    for (const auto &entry : entries) values.insert(entry.name, entry.value);
    QByteArray saved;
    QBuffer output(&saved);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QVERIFY(Codec::save(&output, values));
    QBuffer restored(&saved);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QMap<QString, double> roundtrip;
    QVERIFY(Codec::load(&restored, &roundtrip));
    for (const auto &entry : entries)
        QCOMPARE(roundtrip.value(entry.name), entry.value);
}

void ParameterFileCodecTest::referenceExclusions()
{
    // Independent list from MissionPlanner/ExtLibs/Utilities/ParamFile.cs.
    const QStringList excluded{
        "SYSID_SW_MREV", "WP_TOTAL", "CMD_TOTAL", "FENCE_TOTAL",
        "SYS_NUM_RESETS", "ARSPD_OFFSET", "GND_ABS_PRESS", "GND_TEMP",
        "BARO1_GND_PRESS", "BARO2_GND_PRESS", "BARO3_GND_PRESS",
        "BARO_GND_TEMP", "CMD_INDEX", "LOG_LASTFILE", "FORMAT_VERSION"};
    QCOMPARE(excluded.size(), 15);
    QByteArray text;
    for (const QString &name : excluded) {
        QVERIFY(Codec::isExcluded(name));
        QVERIFY(Codec::isExcluded(QStringLiteral("  ") + name.toLower() + "\t"));
        QVERIFY(!Codec::isExcluded(name + "_X"));
        text += name.toLower().toUtf8() + ",1\n";
    }
    text += "SYSID_THISMAV,42\nBRD_OPTIONS,4\nBARO4_GND_PRESS,100000\n";
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries;
    QVERIFY(Codec::loadOrdered(&input, &entries));
    QCOMPARE(names(entries), QStringList({"SYSID_THISMAV", "BRD_OPTIONS",
                                         "BARO4_GND_PRESS"}));
}

void ParameterFileCodecTest::separatorsCommentsAndPermissiveNames()
{
    QByteArray text(
        "\n  # comment with nonnumeric fields\r\n"
        "one-field-is-ignored\n,, \t\n"
        "A,1\nB 2\nC\t3\nD ,\t 4 ignored-extra-field\n"
        "E\v5\nF\f6\n"
        "colon:name,7\nLONG_PARAMETER_NAME_OVER_16,8\n"
        ",,,leading_separators,,,9\nX=Y,10\n");
    text += QByteArray("NUL\0NAME,11\n", 12);
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries;
    QVERIFY(Codec::loadOrdered(&input, &entries));
    QCOMPARE(entries.size(), 11);
    for (int index = 0; index < entries.size(); ++index)
        QCOMPARE(entries.at(index).value, double(index + 1));
    QCOMPARE(entries.at(6).name, QStringLiteral("COLON:NAME"));
    QCOMPARE(entries.at(7).name, QStringLiteral("LONG_PARAMETER_NAME_OVER_16"));
    QCOMPARE(entries.at(8).name, QStringLiteral("LEADING_SEPARATORS"));
    QCOMPARE(entries.at(9).name, QStringLiteral("X=Y"));
    QCOMPARE(entries.at(10).name, QString::fromLatin1("NUL\0NAME", 8));
}

void ParameterFileCodecTest::byteOrderMarks_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("utf8") << QByteArray::fromHex("efbbbf") + "gain,1.25\r\n";
    QTest::newRow("utf16le") << QByteArray::fromHex(
        "fffe6700610069006e002c0031002e00320035000d000a00");
    QTest::newRow("utf16be") << QByteArray::fromHex(
        "feff006700610069006e002c0031002e00320035000d000a");
    QTest::newRow("utf8-permissive-unicode-name")
        << QByteArray::fromHex("efbbbf") + QString::fromUtf8("gáin,1.25\n").toUtf8();
}

void ParameterFileCodecTest::byteOrderMarks()
{
    QFETCH(QByteArray, bytes);
    QBuffer input(&bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries;
    QVERIFY(Codec::loadOrdered(&input, &entries));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).value, 1.25);
    QVERIFY(!entries.at(0).name.contains(QChar(0xfeff)));
    const QString expected = QByteArray(QTest::currentDataTag()).contains("unicode")
        ? QString::fromUtf8("GÁIN") : QStringLiteral("GAIN");
    QCOMPARE(entries.at(0).name, expected);
}

void ParameterFileCodecTest::malformedValues_data()
{
    QTest::addColumn<QByteArray>("record");
    QTest::newRow("text") << QByteArray("BAD,nope");
    QTest::newRow("nan") << QByteArray("BAD,nan");
    QTest::newRow("positive-infinity") << QByteArray("BAD,inf");
    QTest::newRow("negative-infinity") << QByteArray("BAD,-inf");
    QTest::newRow("overflow") << QByteArray("BAD,1e999");
    QTest::newRow("hex-number") << QByteArray("BAD,0x10");
    // Parsing precedes exclusion, as in the existing codec and MP10.
    QTest::newRow("excluded-invalid") << QByteArray("FORMAT_VERSION,nope");
}

void ParameterFileCodecTest::malformedValues()
{
    QFETCH(QByteArray, record);
    QByteArray text = "GOOD,1\n# comment\n" + record + "\n";
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries{{QStringLiteral("OLD"), 9}};
    QString error;
    int line = 0;
    QVERIFY(!Codec::loadOrdered(&input, &entries, &error, &line));
    QVERIFY(entries.isEmpty());
    QCOMPARE(line, 3);
    QVERIFY(error.contains(QString::fromLatin1(record.split(',').first())));
    QVERIFY(input.seek(0));
    QMap<QString, double> values{{QStringLiteral("OLD"), 9}};
    QVERIFY(!Codec::load(&input, &values, &error, &line));
    QVERIFY(values.isEmpty());
    QCOMPARE(line, 3);
}

void ParameterFileCodecTest::limits_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<bool>("accepted");
    QTest::addColumn<int>("errorLine");
    QTest::newRow("4096-characters") << QByteArray("X,1") + QByteArray(4093, ' ')
                                     << true << 0;
    QTest::newRow("4097-characters") << QByteArray("X,1") + QByteArray(4094, ' ')
                                     << false << 1;
    QTest::newRow("huge-comment") << QByteArray("#") + QByteArray(1024 * 1024, 'x')
                                  << false << 1;
    QTest::newRow("100000-lines") << QByteArray("X,1\n").repeated(100000)
                                  << true << 0;
    QTest::newRow("100001-lines") << QByteArray("X,1\n").repeated(100001)
                                  << false << 100001;
}

void ParameterFileCodecTest::limits()
{
    QFETCH(QByteArray, text);
    QFETCH(bool, accepted);
    QFETCH(int, errorLine);
    QBuffer input(&text);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVector<Codec::Entry> entries;
    QString error;
    int line = -1;
    QCOMPARE(Codec::loadOrdered(&input, &entries, &error, &line), accepted);
    QCOMPARE(line, errorLine);
    QCOMPARE(error.isEmpty(), accepted);
    QCOMPARE(entries.size(), accepted ? 1 : 0);
}

void ParameterFileCodecTest::unreadableAndNullOutputs()
{
    QVector<Codec::Entry> entries{{QStringLiteral("OLD"), 9}};
    QString error;
    int line = -1;
    QVERIFY(!Codec::loadOrdered(nullptr, &entries, &error, &line));
    QVERIFY(entries.isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(line, 0);
    QByteArray text("A,1\n");
    QBuffer input(&text);
    QVERIFY(!Codec::loadOrdered(&input, &entries));
    QVERIFY(input.open(QIODevice::ReadOnly));
    QVERIFY(!Codec::loadOrdered(&input, nullptr));
    QVERIFY(!Codec::load(&input, nullptr));
    QCOMPARE(input.pos(), qint64(0));
    QByteArray empty;
    QBuffer emptyInput(&empty);
    QVERIFY(emptyInput.open(QIODevice::ReadOnly));
    error = "old";
    line = -1;
    QVERIFY(Codec::loadOrdered(&emptyInput, &entries, &error, &line));
    QVERIFY(entries.isEmpty());
    QVERIFY(error.isEmpty());
    QCOMPARE(line, 0);
}

void ParameterFileCodecTest::saveCompatibilityAndFailures()
{
    QByteArray text;
    QBuffer output(&text);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QString error;
    // Saving remains sorted, preserves names and does not apply exclusions.
    QVERIFY(Codec::save(&output, {{"z", 2.5}, {"A", -1},
                                  {"FORMAT_VERSION", 99}}, &error));
    QCOMPARE(text, QByteArray("A,-1\nFORMAT_VERSION,99\nz,2.5\n"));
    QVERIFY(error.isEmpty());
    QVERIFY(!Codec::save(nullptr, {}, &error));
    QVERIFY(!error.isEmpty());
    RejectingWriter rejecting;
    QVERIFY(!Codec::save(&rejecting, {{"A", 1}}, &error));
    QVERIFY(error.contains(QStringLiteral("Writing")));
    for (const QVariant &invalid : {QVariant("nope"),
             QVariant(std::numeric_limits<double>::infinity()),
             QVariant(std::numeric_limits<double>::quiet_NaN())}) {
        QVERIFY(!Codec::save(&output, {{"INVALID", invalid}}, &error));
        QVERIFY(error.contains(QStringLiteral("INVALID")));
    }
}

QTEST_GUILESS_MAIN(ParameterFileCodecTest)
#include "test_parameterfilecodec.moc"
