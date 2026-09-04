#include "comm/MavFtpProtocol.h"

#include <QtTest/QTest>

#include <limits>

using namespace MavFtpProtocol;

namespace
{

void appendEntry(QByteArray *payload, char type, const QByteArray &value)
{
    payload->append(type);
    payload->append(value);
    payload->append('\0');
}

PayloadHeader requestPayload(quint16 sequence = 17)
{
    PayloadHeader request;
    request.sequence = sequence;
    request.opcode = Opcode::ListDirectory;
    request.data = QByteArrayLiteral("/");
    request.size = static_cast<quint8>(request.data.size());
    return request;
}

PayloadHeader ackFor(const PayloadHeader &request)
{
    PayloadHeader response;
    response.sequence = static_cast<quint16>(request.sequence + 1u);
    response.opcode = Opcode::Ack;
    response.requestOpcode = request.opcode;
    return response;
}

} // namespace

class MavFtpProtocolTest final : public QObject
{
    Q_OBJECT

private slots:
    void codecRoundTripUsesPackedLittleEndianLayout();
    void responseCorrelationIsStrictAndWrapSafe();
    void pathEncodingIsBoundedUtf8();
    void directoryParserHandlesUnicodeAndMaximumSize();
    void malformedPayloadsAreRejectedAtomically();
    void malformedDirectoryEntriesAreRejectedAtomically();
    void crcMatchesMavFtpKnownVectorAndIncrementalUpdate();
};

void MavFtpProtocolTest::codecRoundTripUsesPackedLittleEndianLayout()
{
    PayloadHeader source;
    source.sequence = 0x1234;
    source.session = 0x56;
    source.opcode = Opcode::BurstReadFile;
    source.requestOpcode = Opcode::ReadFile;
    source.burstComplete = 1;
    source.padding = 0x78;
    source.offset = 0x9abcdef0u;
    source.data = QByteArray::fromHex("00ff7f80");
    source.size = static_cast<quint8>(source.data.size());

    QString error;
    const QByteArray wire = encodePayload(source, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(wire.size(), PayloadSize);
    QCOMPARE(wire.left(HeaderSize).toHex(),
             QByteArrayLiteral("3412560f04050178f0debc9a"));
    QCOMPARE(wire.mid(HeaderSize, source.data.size()), source.data);
    QCOMPARE(wire.mid(HeaderSize + source.data.size()),
             QByteArray(DataSize - source.data.size(), '\0'));

    PayloadHeader decoded;
    QVERIFY2(decodePayload(wire, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.sequence, source.sequence);
    QCOMPARE(decoded.session, source.session);
    QCOMPARE(decoded.opcode, source.opcode);
    QCOMPARE(decoded.size, source.size);
    QCOMPARE(decoded.requestOpcode, source.requestOpcode);
    QCOMPARE(decoded.burstComplete, source.burstComplete);
    QCOMPARE(decoded.padding, source.padding);
    QCOMPARE(decoded.offset, source.offset);
    QCOMPARE(decoded.data, source.data);
}

void MavFtpProtocolTest::responseCorrelationIsStrictAndWrapSafe()
{
    QString error;
    const PayloadHeader request = requestPayload();
    PayloadHeader response = ackFor(request);
    QVERIFY2(validateResponse(request, response, &error), qPrintable(error));

    response.sequence++;
    QVERIFY(!validateResponse(request, response, &error));
    QVERIFY(!error.isEmpty());
    response = ackFor(request);
    response.requestOpcode = Opcode::ReadFile;
    QVERIFY(!validateResponse(request, response, &error));
    response = ackFor(request);
    response.opcode = Opcode::ReadFile;
    QVERIFY(!validateResponse(request, response, &error));

    const PayloadHeader wrappingRequest = requestPayload(
            std::numeric_limits<quint16>::max());
    response = ackFor(wrappingRequest);
    QCOMPARE(response.sequence, quint16(0));
    QVERIFY2(validateResponse(wrappingRequest, response, &error),
             qPrintable(error));

    response.opcode = Opcode::Nak;
    response.data = QByteArray(1, static_cast<char>(ErrorCode::EndOfFile));
    response.size = 1;
    QVERIFY2(validateResponse(wrappingRequest, response, &error),
             qPrintable(error));
    response.data.append('\0');
    response.size = 2;
    QVERIFY(!validateResponse(wrappingRequest, response, &error));

    response.data = QByteArray(1, static_cast<char>(ErrorCode::FailErrno));
    response.size = 1;
    QVERIFY(!validateResponse(wrappingRequest, response, &error));
    response.data.append(static_cast<char>(2));
    response.size = 2;
    QVERIFY2(validateResponse(wrappingRequest, response, &error),
             qPrintable(error));
}

void MavFtpProtocolTest::pathEncodingIsBoundedUtf8()
{
    QString error;
    QByteArray encoded = QByteArrayLiteral("unchanged");
    const QString unicodePath = QString::fromUtf8("/日志/полёт.bin");
    QVERIFY2(encodePath(unicodePath, &encoded, &error), qPrintable(error));
    QCOMPARE(QString::fromUtf8(encoded), unicodePath);

    const QString exactLimit(MaximumPathBytes, QLatin1Char('a'));
    QVERIFY2(encodePath(exactLimit, &encoded, &error), qPrintable(error));
    QCOMPARE(encoded.size(), MaximumPathBytes);

    const QByteArray previous = encoded;
    QVERIFY(!encodePath(exactLimit + QLatin1Char('b'), &encoded, &error));
    QCOMPARE(encoded, previous);

    QString nulPath = QStringLiteral("/bad");
    nulPath.append(QChar(0));
    nulPath.append(QStringLiteral("path"));
    QVERIFY(!encodePath(nulPath, &encoded, &error));
    QCOMPARE(encoded, previous);

    QString unpairedSurrogate;
    unpairedSurrogate.append(QChar(0xd800));
    QVERIFY(!encodePath(unpairedSurrogate, &encoded, &error));
    QCOMPARE(encoded, previous);
}

void MavFtpProtocolTest::directoryParserHandlesUnicodeAndMaximumSize()
{
    QByteArray data;
    appendEntry(&data, 'F',
                QString::fromUtf8("журнал.bin\t18446744073709551615")
                        .toUtf8());
    appendEntry(&data, 'D', QString::fromUtf8("目录").toUtf8());
    appendEntry(&data, 'S', QByteArrayLiteral("one omitted entry"));
    appendEntry(&data, 'X', QString::fromUtf8("vendor-Δ").toUtf8());
    data.append('\0'); // MP10 ignores zero-filled slots between records.

    QVector<DirectoryEntry> entries;
    QString error;
    QVERIFY2(parseDirectoryEntries(data, data.size(), &entries, &error),
             qPrintable(error));
    QCOMPARE(entries.size(), 4);
    QCOMPARE(entries.at(0).type, DirectoryEntryType::File);
    QCOMPARE(entries.at(0).name, QString::fromUtf8("журнал.bin"));
    QCOMPARE(entries.at(0).size, std::numeric_limits<quint64>::max());
    QCOMPARE(entries.at(1).type, DirectoryEntryType::Directory);
    QCOMPARE(entries.at(1).name, QString::fromUtf8("目录"));
    QCOMPARE(entries.at(2).type, DirectoryEntryType::Skip);
    QVERIFY(entries.at(2).name.isEmpty());
    QCOMPARE(entries.at(3).type, DirectoryEntryType::Other);
    QCOMPARE(entries.at(3).typeTag, quint8('X'));
    QCOMPARE(entries.at(3).name, QString::fromUtf8("vendor-Δ"));
}

void MavFtpProtocolTest::malformedPayloadsAreRejectedAtomically()
{
    QString error;
    PayloadHeader output;
    output.sequence = 1234;
    QVERIFY(!decodePayload(QByteArray(PayloadSize - 1, '\0'), &output,
                           &error));
    QCOMPARE(output.sequence, quint16(1234));
    QVERIFY(!decodePayload(QByteArray(PayloadSize + 1, '\0'), &output,
                           &error));
    QCOMPARE(output.sequence, quint16(1234));

    QByteArray wire(PayloadSize, '\0');
    wire[4] = static_cast<char>(DataSize + 1);
    QVERIFY(!decodePayload(wire, &output, &error));
    QCOMPARE(output.sequence, quint16(1234));

    wire.fill('\0');
    wire[3] = static_cast<char>(42);
    QVERIFY(!decodePayload(wire, &output, &error));
    wire.fill('\0');
    wire[6] = static_cast<char>(2);
    QVERIFY(!decodePayload(wire, &output, &error));

    PayloadHeader invalid = requestPayload();
    invalid.size = 0;
    QVERIFY(encodePayload(invalid, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    invalid.data = QByteArray(DataSize + 1, 'x');
    invalid.size = static_cast<quint8>(invalid.data.size());
    QVERIFY(encodePayload(invalid, &error).isEmpty());

    PayloadHeader readRequest;
    readRequest.opcode = Opcode::ReadFile;
    readRequest.size = 80;
    const QByteArray readWire = encodePayload(readRequest, &error);
    QCOMPARE(readWire.size(), PayloadSize);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(static_cast<quint8>(readWire.at(4)), quint8(80));
    QCOMPARE(readWire.mid(HeaderSize), QByteArray(DataSize, '\0'));
}

void MavFtpProtocolTest::malformedDirectoryEntriesAreRejectedAtomically()
{
    DirectoryEntry sentinel;
    sentinel.name = QStringLiteral("keep me");
    QVector<DirectoryEntry> entries{sentinel};
    QString error;

    const QByteArray unterminated = QByteArrayLiteral("Ffile\t12");
    QVERIFY(!parseDirectoryEntries(unterminated, unterminated.size(),
                                   &entries, &error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().name, sentinel.name);

    QByteArray invalidUtf8("D", 1);
    invalidUtf8.append(static_cast<char>(0xc0));
    invalidUtf8.append(static_cast<char>(0xaf));
    invalidUtf8.append('\0');
    QVERIFY(!parseDirectoryEntries(invalidUtf8, invalidUtf8.size(),
                                   &entries, &error));
    QCOMPARE(entries.first().name, sentinel.name);

    QByteArray overflow;
    appendEntry(&overflow, 'F',
                QByteArrayLiteral("huge\t18446744073709551616"));
    QVERIFY(!parseDirectoryEntries(overflow, overflow.size(), &entries,
                                   &error));
    QCOMPARE(entries.first().name, sentinel.name);

    QByteArray badSize;
    appendEntry(&badSize, 'F', QByteArrayLiteral("file\t+1"));
    QVERIFY(!parseDirectoryEntries(badSize, badSize.size(), &entries,
                                   &error));
    QCOMPARE(entries.first().name, sentinel.name);

    QByteArray unsafeLeaf;
    appendEntry(&unsafeLeaf, 'D', QByteArrayLiteral("child/escape"));
    QVERIFY(!parseDirectoryEntries(unsafeLeaf, unsafeLeaf.size(),
                                   &entries, &error));
    QCOMPARE(entries.first().name, sentinel.name);

    QVERIFY(!parseDirectoryEntries(QByteArrayLiteral("Ddir\0"), 99,
                                   &entries, &error));
    QVERIFY(!parseDirectoryEntries(QByteArray(DataSize + 1, '\0'),
                                   DataSize + 1, &entries, &error));
    QCOMPARE(entries.first().name, sentinel.name);
}

void MavFtpProtocolTest::crcMatchesMavFtpKnownVectorAndIncrementalUpdate()
{
    const QByteArray vector = QByteArrayLiteral("123456789");
    QCOMPARE(crc32(vector), 0x2dfd2d88u);
    QCOMPARE(crc32(QByteArray()), 0u);

    quint32 incremental = crc32(vector.left(4));
    incremental = crc32(vector.mid(4), incremental);
    QCOMPARE(incremental, crc32(vector));

    const quint32 standard = crc32(vector, 0xffffffffu) ^ 0xffffffffu;
    QCOMPARE(standard, 0xcbf43926u);
}

QTEST_APPLESS_MAIN(MavFtpProtocolTest)
#include "test_mavftpprotocol.moc"
