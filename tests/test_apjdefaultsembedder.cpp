#include "ui/tools/ApjDefaultsEmbedder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <zlib.h>

namespace {
constexpr int Area = 32, Capacity = 96, Descriptor = 160;
const QByteArray Magic = QByteArray("PARMDEF\0", 8) + QByteArray::fromHex("5537f4a0385d485b");
bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void le16(QByteArray &data, int offset, quint16 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(data.data() + offset));
}
void le32(QByteArray &data, int offset, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(data.data() + offset));
}
quint16 get16(const QByteArray &data, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
quint32 independentCrc(const QByteArray &data, int start, int end)
{
    // Convert zlib's complemented API into ArduPilot's initial-zero,
    // non-complemented CRC; independent of the backend's bitwise loop.
    return quint32(::crc32(0xffffffffU,
        reinterpret_cast<const Bytef *>(data.constData() + start), uInt(end - start))) ^ 0xffffffffU;
}
void repair(QByteArray &image)
{
    le32(image, Descriptor + 8, independentCrc(image, 0, Descriptor + 8));
    le32(image, Descriptor + 12, independentCrc(image, Descriptor + 24, image.size()));
}
QByteArray image(bool descriptor = true)
{
    QByteArray result(256, 'Z');
    result.replace(Area, Magic.size(), Magic);
    le16(result, Area + 16, Capacity); le16(result, Area + 18, 20);
    result.replace(Area + 20, 20, "OLD=1\nRETAINED=TAIL\n");
    if (descriptor) {
        result.replace(Descriptor, 8, QByteArray::fromHex("40a2e4f164689106"));
        le32(result, Descriptor + 16, quint32(result.size()));
        repair(result);
    }
    return result;
}
QByteArray compressed(const QByteArray &image)
{
    uLongf count = compressBound(uLong(image.size()));
    QByteArray result(int(count), '\0');
    if (compress2(reinterpret_cast<Bytef *>(result.data()), &count,
                  reinterpret_cast<const Bytef *>(image.constData()), uLong(image.size()), 9) != Z_OK) return {};
    result.resize(int(count)); return result;
}
QByteArray apj(const QByteArray &image, bool flashTotal = true)
{
    return QByteArray("{\n  \"magic\" : \"APJFWv1\",\n  \"image_size\": ") + QByteArray::number(image.size())
        + ",\n  \"image\": \"" + compressed(image).toBase64() + "\",\n"
        "  \"signed_firmware\": false,\n"
        "  \"unknown_big\": 9007199254740993123456789,\n"
        "  \"unknown_fraction\": 1.234567890123456789e-17,\n"
        "  \"nested\": {\"image\": \"leave me, [alone] \\\"\", \"n\": 9007199254740993}"
        + (flashTotal ? QByteArray(",\n  \"flash_total\": 100000, \"flash_free\": 1") : QByteArray()) + "\n}\n";
}
QByteArray unpack(const QByteArray &json)
{
    const auto root = QJsonDocument::fromJson(json).object();
    const QByteArray encoded = QByteArray::fromBase64(root.value("image").toString().toLatin1());
    uLongf count = uLong(root.value("image_size").toInt());
    QByteArray result(int(count), '\0');
    if (uncompress(reinterpret_cast<Bytef *>(result.data()), &count,
                   reinterpret_cast<const Bytef *>(encoded.constData()), uLong(encoded.size())) != Z_OK) return {};
    result.resize(int(count)); return result;
}
QStringList contents(const QTemporaryDir &dir)
{
    return QDir(dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
}
}

class ApjDefaultsEmbedderTest final : public QObject
{
    Q_OBJECT
private slots:
    void exactPatchRepairsCrcAndPreservesRawMetadata_data();
    void exactPatchRepairsCrcAndPreservesRawMetadata();
    void bomEncodingsAndCrNormalization_data();
    void bomEncodingsAndCrNormalization();
    void emptyExactCapacityAndUnsignedLength();
    void malformedOrUnsafeInputPreservesOutput_data();
    void malformedOrUnsafeInputPreservesOutput();
    void inputAndOutputLimits();
    void cancellationAndMutationPreserveFiles_data();
    void cancellationAndMutationPreserveFiles();
    void pathsCollisionsAndExplicitOverwrite();
    void missingFlashTotalAndAddedFlashFree();
    void defaultsAfterDescriptorRepairSecondCrc();
    void contentValidationAcceptsRenamedFirmware();
};

void ApjDefaultsEmbedderTest::exactPatchRepairsCrcAndPreservesRawMetadata_data()
{
    QTest::addColumn<bool>("descriptor");
    QTest::newRow("unsigned descriptor") << true;
    QTest::newRow("legacy no descriptor") << false;
}

void ApjDefaultsEmbedderTest::exactPatchRepairsCrcAndPreservesRawMetadata()
{
    QFETCH(bool, descriptor);
    QTemporaryDir dir; const QString firmware = dir.filePath("copter.apj"), params = dir.filePath("defaults.param");
    const QByteArray original = image(descriptor), source = apj(original);
    QVERIFY(put(firmware, source)); QVERIFY(put(params, "NEW=2\r\n"));
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(!result.cancelled);
    QCOMPARE(result.outputPath, firmware + "new.apj");
    QCOMPARE(ApjDefaultsEmbedder::SuggestedOutputPath("a.apj"), QString("a.apjnew.apj"));
    QCOMPARE(result.defaultsBytes, qint64(6)); QCOMPARE(result.maximumDefaultsBytes, qint64(Capacity));
    QCOMPARE(result.imageBytes, qint64(original.size())); QCOMPARE(result.repairedDescriptors, descriptor ? 1 : 0);
    QByteArray expected = original; le16(expected, Area + 18, 6); expected.replace(Area + 20, 6, "NEW=2\n");
    if (descriptor) repair(expected);
    const QByteArray output = get(result.outputPath);
    QCOMPARE(unpack(output), expected); QCOMPARE(result.outputBytes, qint64(output.size()));
    QVERIFY(output.contains("\"unknown_big\": 9007199254740993123456789"));
    QVERIFY(output.contains("\"unknown_fraction\": 1.234567890123456789e-17"));
    QVERIFY(output.contains("\"nested\": {\"image\": \"leave me, [alone] \\\"\", \"n\": 9007199254740993}"));
    QCOMPARE(QJsonDocument::fromJson(output).object().value("flash_free").toInt(), 100000 - original.size());
    QCOMPARE(get(firmware), source); QCOMPARE(get(params), QByteArray("NEW=2\r\n"));
    QCOMPARE(contents(dir), QStringList({"copter.apj", "copter.apjnew.apj", "defaults.param"}));
    if (!descriptor) QVERIFY(result.warnings.join('\n').contains("No recognized unsigned"));
}

void ApjDefaultsEmbedderTest::bomEncodingsAndCrNormalization_data()
{
    QTest::addColumn<QByteArray>("bom"); QTest::addColumn<int>("unit"); QTest::addColumn<bool>("little");
    QTest::newRow("UTF8") << QByteArray() << 1 << true;
    QTest::newRow("UTF8 BOM") << QByteArray::fromHex("efbbbf") << 1 << true;
    QTest::newRow("UTF16 LE") << QByteArray::fromHex("fffe") << 2 << true;
    QTest::newRow("UTF16 BE") << QByteArray::fromHex("feff") << 2 << false;
    QTest::newRow("UTF32 LE") << QByteArray::fromHex("fffe0000") << 4 << true;
    QTest::newRow("UTF32 BE") << QByteArray::fromHex("0000feff") << 4 << false;
}

void ApjDefaultsEmbedderTest::bomEncodingsAndCrNormalization()
{
    QFETCH(QByteArray, bom); QFETCH(int, unit); QFETCH(bool, little);
    QByteArray text = bom;
    const QByteArray decoded = "# comment\r\nA\r=1\n\tB=2";
    for (char byte : decoded) {
        QByteArray code(unit, '\0'); code[little ? 0 : unit - 1] = byte; text += code;
    }
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.parm");
    QByteArray source = apj(image());
    const int encodedAt = source.indexOf("\"image\": \"") + 10;
    source.insert(encodedAt + 8, " \\t\\r\\n"); // JSON escapes decode to standard Base64 whitespace.
    QVERIFY(put(firmware, QByteArray::fromHex("efbbbf") + source)); QVERIFY(put(params, text));
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY2(result.success, qPrintable(result.error));
    const QByteArray expected = "# comment\nA=1\n\tB=2";
    const QByteArray output = unpack(get(result.outputPath));
    QCOMPARE(get16(output, Area + 18), quint16(expected.size()));
    QCOMPARE(output.mid(Area + 20, expected.size()), expected);
    QVERIFY(get(result.outputPath).startsWith(QByteArray::fromHex("efbbbf")));
}

void ApjDefaultsEmbedderTest::emptyExactCapacityAndUnsignedLength()
{
    for (int length : {0, Capacity, 50000, 65535}) {
        QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
        QByteArray original = image(false);
        if (length > Capacity) {
            original += QByteArray(Area + 20 + length - original.size(), 'Z');
            le16(original, Area + 16, quint16(length));
        }
        QVERIFY(put(firmware, apj(original))); QVERIFY(put(params, QByteArray(length, 'A')));
        const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
        QVERIFY2(result.success, qPrintable(result.error));
        const QByteArray output = unpack(get(result.outputPath));
        QCOMPARE(get16(output, Area + 18), quint16(length));
        QCOMPARE(output.mid(Area + 20, length), QByteArray(length, 'A'));
        QCOMPARE(output.size(), original.size());
    }
}

void ApjDefaultsEmbedderTest::malformedOrUnsafeInputPreservesOutput_data()
{
    QTest::addColumn<QByteArray>("source"); QTest::addColumn<QByteArray>("parameters");
    const QByteArray valid = apj(image());
    QTest::newRow("invalid JSON") << QByteArray("{") << QByteArray("A=1\n");
    QTest::newRow("array root") << QByteArray("[]") << QByteArray("A=1\n");
    QByteArray modified = valid; modified.replace("APJFWv1", "APJFWv2");
    QTest::newRow("wrong magic") << modified << QByteArray("A=1\n");
    modified = valid; modified.replace("\"signed_firmware\": false", "\"signed_firmware\": true");
    QTest::newRow("signed metadata") << modified << QByteArray("A=1\n");
    modified = valid; modified.replace("\"signed_firmware\": false", "\"signed_firmware\": \"false\"");
    QTest::newRow("ambiguous signed metadata") << modified << QByteArray("A=1\n");
    modified = valid; modified.insert(modified.indexOf('{') + 1, "\"image_size\":256,");
    QTest::newRow("duplicate root key") << modified << QByteArray("A=1\n");
    modified = valid; modified.insert(modified.indexOf('{') + 1, "\"im\\u0061ge_size\":256,");
    QTest::newRow("duplicate escaped root key") << modified << QByteArray("A=1\n");
    modified = valid; modified.replace("\"image_size\": 256", "\"image_size\": 257");
    QTest::newRow("wrong image size") << modified << QByteArray("A=1\n");
    modified = valid; modified.replace("\"image_size\": 256", "\"image_size\": 256.0");
    QTest::newRow("fractional known size") << modified << QByteArray("A=1\n");
    modified = valid; modified.replace("\"flash_total\": 100000", "\"flash_total\": 1");
    QTest::newRow("flash capacity") << modified << QByteArray("A=1\n");
    QByteArray binary = image(false); binary.replace(Area, 7, "NO_AREA");
    QTest::newRow("missing parameter marker") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); binary[Area + 7] = 'X';
    QTest::newRow("non-NUL marker separator") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); binary.replace(200, Magic.size(), Magic);
    QTest::newRow("duplicate defaults marker") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); binary.truncate(Area + 19);
    QTest::newRow("truncated parameter header") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); binary.truncate(Area + 20 + Capacity - 1);
    QTest::newRow("truncated reserved capacity") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); le16(binary, Area + 18, Capacity + 1);
    QTest::newRow("invalid old length") << apj(binary) << QByteArray("A=1\n");
    binary = image(false); le16(binary, Area + 16, 0); le16(binary, Area + 18, 0);
    QTest::newRow("zero reserved capacity") << apj(binary) << QByteArray();
    binary = image(false); binary.replace(200, 8, QByteArray::fromHex("41a3e5f265699207"));
    QTest::newRow("signed binary despite false flag") << apj(binary) << QByteArray("A=1\n");
    binary = image(); binary[0] = 'X';
    QTest::newRow("bad original descriptor CRC") << apj(binary) << QByteArray("A=1\n");
    binary = image(); binary.replace(210, 8, QByteArray::fromHex("40a2e4f164689106"));
    QTest::newRow("duplicate unsigned descriptor") << apj(binary) << QByteArray("A=1\n");
    binary = image(); le32(binary, Descriptor + 16, binary.size() - 1);
    QTest::newRow("descriptor size mismatch") << apj(binary) << QByteArray("A=1\n");
    QTest::newRow("defaults overflow") << valid << QByteArray(Capacity + 1, 'A');
    QTest::newRow("non ASCII") << valid << QByteArray::fromHex("413dc3a9");
    QTest::newRow("NUL") << valid << QByteArray("A=\0", 3);
    QTest::newRow("truncated UTF16") << valid << QByteArray::fromHex("fffe41");
    for (const auto &pair : QList<QPair<QByteArray, QByteArray>>{
             {"invalid alphabet", "!!!!"}, {"noncanonical pad bits", "eJ=="},
             {"truncated zlib", compressed(image()).chopped(2).toBase64()},
             {"trailing zlib bytes", (compressed(image()) + "extra").toBase64()},
             {"concatenated zlib", (compressed(image()) + compressed(image())).toBase64()}}) {
        QJsonObject root = QJsonDocument::fromJson(valid).object(); root["image"] = QString::fromLatin1(pair.second);
        QTest::newRow(pair.first.constData()) << QJsonDocument(root).toJson() << QByteArray("A=1\n");
    }
}

void ApjDefaultsEmbedderTest::malformedOrUnsafeInputPreservesOutput()
{
    QFETCH(QByteArray, source); QFETCH(QByteArray, parameters);
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    const QString output = ApjDefaultsEmbedder::SuggestedOutputPath(firmware);
    QVERIFY(put(firmware, source)); QVERIFY(put(params, parameters)); QVERIFY(put(output, "old output"));
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {true});
    QVERIFY(!result.success); QVERIFY(!result.cancelled); QVERIFY2(!result.error.isEmpty(), "Failure must carry an actionable error");
    QVERIFY(result.outputPath.isEmpty()); QCOMPARE(result.outputBytes, qint64(0));
    QCOMPARE(get(output), QByteArray("old output")); QCOMPARE(get(firmware), source); QCOMPARE(get(params), parameters);
    QCOMPARE(contents(dir), QStringList({"in.apj", "in.apjnew.apj", "in.param"}));
}

void ApjDefaultsEmbedderTest::inputAndOutputLimits()
{
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    QVERIFY(put(firmware, apj(image()))); QVERIFY(put(params, "A=1\n"));
    QFile oversized(params); QVERIFY(oversized.open(QIODevice::ReadWrite));
    QVERIFY(oversized.resize(ApjDefaultsEmbedder::MaximumParameterBytes + 1)); oversized.close();
    auto result = ApjDefaultsEmbedder::Embed(firmware, params, {}); QVERIFY(!result.success);
    QVERIFY(put(params, "A=1\n"));
    QFile hugeJson(firmware); QVERIFY(hugeJson.open(QIODevice::ReadWrite));
    QVERIFY(hugeJson.resize(ApjDefaultsEmbedder::MaximumJsonBytes + 1)); hugeJson.close();
    result = ApjDefaultsEmbedder::Embed(firmware, params, {}); QVERIFY(!result.success);
    // A small compressed stream with dishonest metadata must not grow past the
    // decoder's independent output limit (metadata alone is not trusted).
    QByteArray bomb(int(ApjDefaultsEmbedder::MaximumImageBytes + 1), 'A');
    QJsonObject root = QJsonDocument::fromJson(apj(image())).object();
    root["image"] = QString::fromLatin1(compressed(bomb).toBase64()); bomb.clear();
    QVERIFY(put(firmware, QJsonDocument(root).toJson()));
    result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY(!result.success); QVERIFY(result.error.contains("size limit"));
    QVERIFY(!QFileInfo::exists(ApjDefaultsEmbedder::SuggestedOutputPath(firmware)));
}

void ApjDefaultsEmbedderTest::cancellationAndMutationPreserveFiles_data()
{
    QTest::addColumn<int>("at"); QTest::addColumn<int>("mutation");
    for (int at : {0, 100, 400, 500, 600, 900, 920, 960})
        QTest::newRow(qPrintable(QString("cancel %1").arg(at))) << at << 0;
    QTest::newRow("firmware mutation") << 500 << 1;
    QTest::newRow("defaults mutation") << 920 << 2;
    QTest::newRow("output mutation") << 960 << 3;
}

void ApjDefaultsEmbedderTest::cancellationAndMutationPreserveFiles()
{
    QFETCH(int, at); QFETCH(int, mutation);
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    const QString output = ApjDefaultsEmbedder::SuggestedOutputPath(firmware);
    QVERIFY(put(firmware, apj(image()))); QVERIFY(put(params, "A=1\n")); QVERIFY(put(output, "old output"));
    bool reached = false;
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {true},
        [&](qint64 value, qint64) {
            if (!reached && value >= at) {
                reached = true;
                if (mutation == 1) put(firmware, apj(image(false)));
                if (mutation == 2) put(params, "B=2\n");
                if (mutation == 3) put(output, "external change");
            }
        }, [&] { return reached && !mutation; });
    QVERIFY(reached); QVERIFY(!result.success); QCOMPARE(result.cancelled, mutation == 0);
    if (mutation) QVERIFY(result.error.contains("changed"));
    QCOMPARE(get(output), mutation == 3 ? QByteArray("external change") : QByteArray("old output"));
    QCOMPARE(contents(dir), QStringList({"in.apj", "in.apjnew.apj", "in.param"}));
}

void ApjDefaultsEmbedderTest::pathsCollisionsAndExplicitOverwrite()
{
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    const QString output = ApjDefaultsEmbedder::SuggestedOutputPath(firmware);
    QVERIFY(put(firmware, apj(image()))); QVERIFY(put(params, "A=1\n"));
    QVERIFY(put(output, "old output"));
    auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY(!result.success); QCOMPARE(get(output), QByteArray("old output"));
    result = ApjDefaultsEmbedder::Embed(firmware, params, {true}); QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(put(output, "A=1\n"));
    result = ApjDefaultsEmbedder::Embed(firmware, output, {true}); QVERIFY(!result.success);
    QCOMPARE(get(output), QByteArray("A=1\n"));
    QVERIFY(QFile::remove(output));
    if (!QFile::link(params, output)) QSKIP("Symbolic links unavailable.");
    result = ApjDefaultsEmbedder::Embed(firmware, params, {true}); QVERIFY(!result.success);
    QVERIFY(QFileInfo(output).isSymLink()); QCOMPARE(get(params), QByteArray("A=1\n"));
    QVERIFY(QFile::remove(output));
    bool created = false;
    result = ApjDefaultsEmbedder::Embed(firmware, params, {}, [&](qint64 value, qint64) {
        if (!created && value >= 960) { created = true; put(output, "external file"); }
    });
    QVERIFY(created); QVERIFY(!result.success); QCOMPARE(get(output), QByteArray("external file"));
}

void ApjDefaultsEmbedderTest::missingFlashTotalAndAddedFlashFree()
{
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    QVERIFY(put(firmware, apj(image(), false))); QVERIFY(put(params, "A=1\n"));
    auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(result.warnings.join('\n').contains("flash_total is absent"));
    QVERIFY(!QJsonDocument::fromJson(get(result.outputPath)).object().contains("flash_free"));
    QByteArray source = apj(image()); source.replace(", \"flash_free\": 1", "");
    QVERIFY(put(firmware, source));
    result = ApjDefaultsEmbedder::Embed(firmware, params, {true}); QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(QJsonDocument::fromJson(get(result.outputPath)).object().value("flash_free").toInt(), 99744);
}

void ApjDefaultsEmbedderTest::defaultsAfterDescriptorRepairSecondCrc()
{
    QTemporaryDir dir; const QString firmware = dir.filePath("in.apj"), params = dir.filePath("in.param");
    const int descriptor = 16, area = 120;
    QByteArray original(256, 'Z');
    original.replace(descriptor, 8, QByteArray::fromHex("40a2e4f164689106"));
    le32(original, descriptor + 16, original.size());
    original.replace(area, Magic.size(), Magic);
    le16(original, area + 16, 96); le16(original, area + 18, 0);
    le32(original, descriptor + 8, independentCrc(original, 0, descriptor + 8));
    le32(original, descriptor + 12, independentCrc(original, descriptor + 24, original.size()));
    QVERIFY(put(firmware, apj(original))); QVERIFY(put(params, "A=1\n"));
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.repairedDescriptors, 1);
    QByteArray expected = original; le16(expected, area + 18, 4); expected.replace(area + 20, 4, "A=1\n");
    le32(expected, descriptor + 12, independentCrc(expected, descriptor + 24, expected.size()));
    QCOMPARE(unpack(get(result.outputPath)), expected);
}

void ApjDefaultsEmbedderTest::contentValidationAcceptsRenamedFirmware()
{
    QTemporaryDir dir; const QString firmware = dir.filePath("renamed.data"), params = dir.filePath("defaults.txt");
    QVERIFY(put(firmware, apj(image()))); QVERIFY(put(params, "A=1\n"));
    const auto result = ApjDefaultsEmbedder::Embed(firmware, params, {});
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.outputPath, firmware + "new.apj");
    QVERIFY(!unpack(get(result.outputPath)).isEmpty());
}

QTEST_GUILESS_MAIN(ApjDefaultsEmbedderTest)
#include "test_apjdefaultsembedder.moc"
