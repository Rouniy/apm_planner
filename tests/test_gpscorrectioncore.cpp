#include <QtTest>

#include "comm/GpsRtcmPacketizer.h"
#include "comm/GpsCorrectionSource.h"
#include "comm/Rtcm3Parser.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>

namespace {
void setUnsignedBits(QByteArray *bytes, int bitOffset, int bitCount,
                     quint64 value)
{
    for (int bit = 0; bit < bitCount; ++bit) {
        const int absolute = bitOffset + bit;
        const int byteIndex = absolute / 8;
        const int bitIndex = 7 - (absolute % 8);
        const quint64 mask = quint64(1) << (bitCount - bit - 1);
        quint8 byte = static_cast<quint8>(bytes->at(byteIndex));
        if (value & mask) {
            byte |= static_cast<quint8>(1U << bitIndex);
        } else {
            byte &= static_cast<quint8>(~(1U << bitIndex));
        }
        (*bytes)[byteIndex] = static_cast<char>(byte);
    }
}

void setSignedBits(QByteArray *bytes, int bitOffset, int bitCount,
                   qint64 value)
{
    const quint64 mask = (quint64(1) << bitCount) - 1U;
    setUnsignedBits(bytes, bitOffset, bitCount,
                    static_cast<quint64>(value) & mask);
}

QByteArray rtcmFrame(quint16 messageId, int payloadLength = 2)
{
    QByteArray frame(Rtcm3Parser::HeaderSize + payloadLength, '\0');
    frame[0] = static_cast<char>(Rtcm3Parser::Preamble);
    frame[1] = static_cast<char>((payloadLength >> 8) & 0x03);
    frame[2] = static_cast<char>(payloadLength & 0xff);
    setUnsignedBits(&frame, 24, 12, messageId);
    const quint32 crc = Rtcm3Parser::crc24q(
        reinterpret_cast<const quint8 *>(frame.constData()),
        static_cast<std::size_t>(frame.size()));
    frame.append(static_cast<char>((crc >> 16) & 0xff));
    frame.append(static_cast<char>((crc >> 8) & 0xff));
    frame.append(static_cast<char>(crc & 0xff));
    return frame;
}

QByteArray baseFrameAtEquator(quint16 messageId = 1005,
                              int payloadLength = 19,
                              quint16 antennaHeightRaw = 0)
{
    QByteArray frame(Rtcm3Parser::HeaderSize + payloadLength, '\0');
    frame[0] = static_cast<char>(Rtcm3Parser::Preamble);
    frame[1] = 0;
    frame[2] = static_cast<char>(payloadLength);
    setUnsignedBits(&frame, 24, 12, messageId);
    setUnsignedBits(&frame, 36, 12, 1);
    setUnsignedBits(&frame, 54, 1, 1); // GPS indicator
    setSignedBits(&frame, 58, 38, 63781370000LL);
    setSignedBits(&frame, 98, 38, 0);
    setSignedBits(&frame, 138, 38, 0);
    if (messageId == 1006 && payloadLength >= 21) {
        setUnsignedBits(&frame, 176, 16, antennaHeightRaw);
    }
    const quint32 crc = Rtcm3Parser::crc24q(
        reinterpret_cast<const quint8 *>(frame.constData()),
        static_cast<std::size_t>(frame.size()));
    frame.append(static_cast<char>((crc >> 16) & 0xff));
    frame.append(static_cast<char>((crc >> 8) & 0xff));
    frame.append(static_cast<char>(crc & 0xff));
    return frame;
}
} // namespace

class GpsCorrectionCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void packetizerUsesMavlinkFlagsAndTermination();
    void packetizerHandlesProtocolLimitAndSequenceWrap();
    void parserReframesNoiseAndValidatesCrc();
    void parserRejectsCorruptFrameAndExtractsBasePosition();
    void sourceValidatesAndBuildsNtripProtocol();
    void sourceProducesGgaAndOnlyValidRtcmFrames();
    void sourceAcceptsPartialNtripHeaderAndTrailingRtcm();
    void sourceDecodesChunkedNtripAcrossFrameBoundaries();
    void sourceRejectsNtripSourcetableContentType();
};

void GpsCorrectionCoreTest::packetizerUsesMavlinkFlagsAndTermination()
{
    GpsRtcmPacketizer::Result empty = GpsRtcmPacketizer::pack({}, 7);
    QVERIFY(empty.packets.isEmpty());
    QCOMPARE(empty.nextSequenceId, quint8(7));

    GpsRtcmPacketizer::Result single = GpsRtcmPacketizer::pack(
        QByteArray(180, 'a'), 7);
    QCOMPARE(single.packets.size(), 1);
    QCOMPARE(single.packets.first().flags, quint8(7U << 3));
    QCOMPARE(single.packets.first().data.size(), 180);
    QCOMPARE(single.nextSequenceId, quint8(8));

    GpsRtcmPacketizer::Result split = GpsRtcmPacketizer::pack(
        QByteArray(360, 'b'), 9);
    QCOMPARE(split.packets.size(), 3);
    QCOMPARE(split.packets.at(0).flags,
             quint8((9U << 3) | 0x01U));
    QCOMPARE(split.packets.at(1).flags,
             quint8((9U << 3) | 0x03U));
    QCOMPARE(split.packets.at(2).flags,
             quint8((9U << 3) | 0x05U));
    QCOMPARE(split.packets.at(0).data.size(), 180);
    QCOMPARE(split.packets.at(1).data.size(), 180);
    QVERIFY(split.packets.at(2).data.isEmpty());
    QCOMPARE(split.nextSequenceId, quint8(10));
}

void GpsCorrectionCoreTest::
    packetizerHandlesProtocolLimitAndSequenceWrap()
{
    const GpsRtcmPacketizer::Result justFragmented =
        GpsRtcmPacketizer::pack(QByteArray(181, 'x'), 3);
    QCOMPARE(justFragmented.packets.size(), 2);
    QCOMPARE(justFragmented.packets.at(0).data.size(), 180);
    QCOMPARE(justFragmented.packets.at(1).data.size(), 1);
    QVERIFY(justFragmented.packets.at(0).flags & 0x01U);

    const GpsRtcmPacketizer::Result exactThree =
        GpsRtcmPacketizer::pack(QByteArray(540, 'y'), 4);
    QCOMPARE(exactThree.packets.size(), 4);
    QVERIFY(exactThree.packets.last().data.isEmpty());

    const GpsRtcmPacketizer::Result belowLimit =
        GpsRtcmPacketizer::pack(QByteArray(700, 'z'), 5);
    QCOMPARE(belowLimit.packets.size(), 4);
    QCOMPARE(belowLimit.packets.last().data.size(), 160);
    for (const GpsRtcmPacket &packet : belowLimit.packets) {
        QVERIFY(packet.flags & 0x01U);
    }

    GpsRtcmPacketizer::Result four = GpsRtcmPacketizer::pack(
        QByteArray(720, 'c'), 31);
    QCOMPARE(four.packets.size(), 4);
    for (int index = 0; index < four.packets.size(); ++index) {
        QCOMPARE(four.packets.at(index).data.size(), 180);
        QCOMPARE(four.packets.at(index).flags,
                 quint8((31U << 3) | 0x01U | (index << 1)));
    }
    QCOMPARE(four.nextSequenceId, quint8(0));

    const GpsRtcmPacketizer::Result aboveLimit =
        GpsRtcmPacketizer::pack(QByteArray(721, 'q'), 6);
    QCOMPARE(aboveLimit.packets.size(), 5);
    QCOMPARE(aboveLimit.packets.last().data.size(), 1);
    for (const GpsRtcmPacket &packet : aboveLimit.packets) {
        QCOMPARE(packet.flags & 0x01U, quint8(0));
    }

    GpsRtcmPacketizer::Result oversized = GpsRtcmPacketizer::pack(
        QByteArray(901, 'd'), 30);
    QCOMPARE(oversized.packets.size(), 6);
    for (const GpsRtcmPacket &packet : oversized.packets) {
        QCOMPARE(packet.flags & 0x01U, quint8(0));
        QVERIFY(packet.data.size() <= 180);
    }
    QCOMPARE(oversized.packets.last().data.size(), 1);
    QCOMPARE(oversized.nextSequenceId, quint8(4));
}

void GpsCorrectionCoreTest::parserReframesNoiseAndValidatesCrc()
{
    const QByteArray frame = rtcmFrame(1077, 12);
    Rtcm3Parser parser;
    int completed = 0;
    QByteArray stream("noise", 5);
    stream.append(frame);
    for (char character : stream) {
        if (!parser.addByte(static_cast<quint8>(character))) {
            continue;
        }
        ++completed;
        QVERIFY(parser.validateCrc());
        QCOMPARE(parser.messageId(), quint16(1077));
        QCOMPARE(parser.currentFrame(), frame);
        parser.reset();
    }
    QCOMPARE(completed, 1);
}

void GpsCorrectionCoreTest::
    parserRejectsCorruptFrameAndExtractsBasePosition()
{
    QByteArray corrupt = rtcmFrame(1087, 8);
    corrupt[5] = static_cast<char>(corrupt.at(5) ^ 0x40);
    Rtcm3Parser parser;
    bool completed = false;
    for (char character : corrupt) {
        completed = parser.addByte(static_cast<quint8>(character))
            || completed;
    }
    QVERIFY(completed);
    QVERIFY(!parser.validateCrc());

    const QByteArray base = baseFrameAtEquator();
    RtcmBasePosition position;
    QVERIFY(Rtcm3Parser::extractBasePosition(base, &position));
    QVERIFY(std::abs(position.latitude) < 1.0e-9);
    QVERIFY(std::abs(position.longitude) < 1.0e-9);
    QVERIFY(std::abs(position.altitude) < 1.0e-4);

    // DF028 is the ARP height above the monument, not an offset to add to
    // the ECEF-derived ARP altitude. A non-zero value therefore leaves the
    // displayed base altitude at the ellipsoid surface in this fixture.
    const QByteArray base1006 = baseFrameAtEquator(1006, 21, 12345);
    QVERIFY(Rtcm3Parser::extractBasePosition(base1006, &position));
    QVERIFY(std::abs(position.latitude) < 1.0e-9);
    QVERIFY(std::abs(position.longitude) < 1.0e-9);
    QVERIFY(std::abs(position.altitude) < 1.0e-4);

    // A CRC-valid message labelled 1006 is still malformed without DF028.
    const QByteArray truncated1006 = baseFrameAtEquator(1006, 19);
    QVERIFY(!Rtcm3Parser::extractBasePosition(truncated1006, &position));

    QVERIFY(!Rtcm3Parser::extractBasePosition(
        rtcmFrame(1077, 19), &position));
}

void GpsCorrectionCoreTest::sourceValidatesAndBuildsNtripProtocol()
{
    GpsCorrectionSourceSettings settings;
    QCOMPARE(settings.validationError(),
             QStringLiteral("NTRIP host is required."));
    settings.host = QStringLiteral("caster.example.com");
    settings.mountPoint = QStringLiteral("MOUNT");
    settings.username = QStringLiteral("pilot");
    settings.password = QStringLiteral("secret");
    QVERIFY(settings.validationError().isEmpty());

    bool clearCredentials = false;
    const QByteArray v2 = QtGpsCorrectionSource::buildNtripRequest(
        settings, &clearCredentials);
    QVERIFY(clearCredentials);
    QVERIFY(v2.startsWith("GET /MOUNT HTTP/1.1\r\n"));
    QVERIFY(v2.contains("Host: caster.example.com:2101\r\n"));
    QVERIFY(v2.contains("Ntrip-Version: Ntrip/2.0\r\n"));
    QVERIFY(v2.contains("Authorization: Basic cGlsb3Q6c2VjcmV0\r\n"));
    QVERIFY(v2.endsWith("Connection: close\r\n\r\n"));

    settings.ntripV1 = true;
    settings.casterPort = 443;
    const QByteArray v1 = QtGpsCorrectionSource::buildNtripRequest(
        settings, &clearCredentials);
    QVERIFY(!clearCredentials);
    QVERIFY(v1.startsWith("GET /MOUNT HTTP/1.0\r\n"));
    QVERIFY(!v1.contains("Ntrip-Version"));
    QCOMPARE(QtGpsCorrectionSource::parseNtripStatusCode(
                 QByteArrayLiteral("HTTP/1.1 200 OK")), 200);
    QCOMPARE(QtGpsCorrectionSource::parseNtripStatusCode(
                 QByteArrayLiteral("ICY 200 OK")), 200);
    QCOMPARE(QtGpsCorrectionSource::parseNtripStatusCode(
                 QByteArrayLiteral("garbage")), 0);

    settings.host = QStringLiteral("bad\r\nInjected: yes");
    QVERIFY(!settings.validationError().isEmpty());
    settings.host = QStringLiteral("caster.example.com");
    settings.username = QStringLiteral("bad:user");
    QVERIFY(!settings.validationError().isEmpty());
    settings.username.clear();
    settings.password.clear();
    settings.host = QStringLiteral("https://caster.example.com:2443/path");
    settings.mountPoint = QStringLiteral("MY MOUNT");
    QVERIFY(settings.validationError().isEmpty());
    const QByteArray encoded = QtGpsCorrectionSource::buildNtripRequest(
        settings, &clearCredentials);
    QVERIFY(encoded.startsWith("GET /MY%20MOUNT HTTP/1.0\r\n"));
    QVERIFY(!clearCredentials);

    settings.ntripV1 = false;
    settings.host = QStringLiteral("caster.example.com:2443");
    const QByteArray embeddedPort =
        QtGpsCorrectionSource::buildNtripRequest(settings);
    QVERIFY(embeddedPort.contains(
        "Host: caster.example.com:2443\r\n"));
    settings.host = QStringLiteral("pilot@caster.example.com:2101");
    QVERIFY(!settings.validationError().isEmpty());
}

void GpsCorrectionCoreTest::sourceProducesGgaAndOnlyValidRtcmFrames()
{
    const QByteArray gga = QtGpsCorrectionSource::makeGga(
        35.1234567, 33.7654321, 123.4);
    QVERIFY(gga.startsWith("$GPGGA,"));
    const QList<QByteArray> fields = gga.mid(1, gga.indexOf('*') - 1)
        .split(',');
    QCOMPARE(fields.at(1).size(), 6);
    QVERIFY(gga.contains(",3507.4074,N,03345.9259,E,1,10,1.0,123.4,M,"));
    QVERIFY(gga.endsWith("\r\n"));
    const int star = gga.indexOf('*');
    QVERIFY(star > 1);
    quint8 checksum = 0;
    for (int index = 1; index < star; ++index) {
        checksum ^= static_cast<quint8>(gga.at(index));
    }
    QCOMPARE(gga.mid(star + 1, 2),
             QByteArray::number(checksum, 16).rightJustified(2, '0')
                 .toUpper());
    QVERIFY(QtGpsCorrectionSource::makeGga(
        91.0, 0.0, 0.0).isEmpty());

    QtGpsCorrectionSource source;
    QSignalSpy bytes(&source, &GpsCorrectionSource::inputBytes);
    QSignalSpy frames(&source, &GpsCorrectionSource::rtcmFrame);
    QSignalSpy invalid(&source, &GpsCorrectionSource::invalidRtcmFrame);
    const QByteArray good = rtcmFrame(1097, 16);
    QByteArray bad = rtcmFrame(1087, 10);
    bad[7] = static_cast<char>(bad.at(7) ^ 0x20);
    source.ingestBytes(QByteArrayLiteral("noise") + bad + good.left(7));
    source.ingestBytes(good.mid(7));
    QCOMPARE(bytes.count(), 2);
    QCOMPARE(invalid.count(), 1);
    QCOMPARE(frames.count(), 1);
    QCOMPARE(frames.first().at(0).toByteArray(), good);
    QCOMPARE(frames.first().at(1).toUInt(), uint(1097));
}

void GpsCorrectionCoreTest::
    sourceAcceptsPartialNtripHeaderAndTrailingRtcm()
{
    QTcpServer caster;
    QVERIFY(caster.listen(QHostAddress::LocalHost));

    QByteArray request;
    const QByteArray frame = rtcmFrame(1077, 16);
    connect(&caster, &QTcpServer::newConnection, &caster, [&]() {
        QTcpSocket *socket = caster.nextPendingConnection();
        QVERIFY(socket);
        connect(socket, &QTcpSocket::readyRead, socket,
                [&, socket]() {
            request.append(socket->readAll());
            if (!request.endsWith("\r\n\r\n")
                || socket->property("responseSent").toBool()) {
                return;
            }
            socket->setProperty("responseSent", true);
            socket->write("HTTP/1.1 200");
            QTimer::singleShot(0, socket, [socket, frame]() {
                socket->write(" OK\r\nServer: local-test\r\n\r\n" + frame);
            });
        });
    });

    QtGpsCorrectionSource source;
    QSignalSpy frames(&source, &GpsCorrectionSource::rtcmFrame);
    QSignalSpy states(&source, &GpsCorrectionSource::stateChanged);
    GpsCorrectionSourceSettings settings;
    settings.host = QStringLiteral("127.0.0.1");
    settings.casterPort = caster.serverPort();
    settings.mountPoint = QStringLiteral("MOUNT");
    QVERIFY(source.start(settings));

    QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 2000);
    QVERIFY(source.active());
    QVERIFY(source.connected());
    QVERIFY(states.count() >= 2);
    QVERIFY(request.startsWith("GET /MOUNT HTTP/1.1\r\n"));
    QCOMPARE(frames.first().at(0).toByteArray(), frame);
    QCOMPARE(frames.first().at(1).toUInt(), uint(1077));
    source.stop();
    QVERIFY(!source.active());
}

void GpsCorrectionCoreTest::
    sourceDecodesChunkedNtripAcrossFrameBoundaries()
{
    QTcpServer caster;
    QVERIFY(caster.listen(QHostAddress::LocalHost));
    const QByteArray frame = rtcmFrame(1087, 28);
    const QByteArray first = frame.left(11);
    const QByteArray second = frame.mid(11);

    connect(&caster, &QTcpServer::newConnection, &caster, [&]() {
        QTcpSocket *socket = caster.nextPendingConnection();
        QVERIFY(socket);
        connect(socket, &QTcpSocket::readyRead, socket,
                [socket, first, second]() {
            QByteArray request = socket->property("request").toByteArray();
            request.append(socket->readAll());
            socket->setProperty("request", request);
            if (!request.endsWith("\r\n\r\n")
                || socket->property("responseSent").toBool()) {
                return;
            }
            socket->setProperty("responseSent", true);
            socket->write(
                "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: Chunked\r\n"
                "Content-Type: gnss/data\r\n\r\n");
            socket->write(QByteArray::number(first.size(), 16)
                          + "\r\n" + first + "\r\n");
            QTimer::singleShot(0, socket, [socket, second]() {
                socket->write(QByteArray::number(second.size(), 16)
                              + ";caster=test\r\n"
                              + second + "\r\n0\r\n\r\n");
            });
        });
    });

    QtGpsCorrectionSource source;
    QSignalSpy frames(&source, &GpsCorrectionSource::rtcmFrame);
    QSignalSpy invalid(&source, &GpsCorrectionSource::invalidRtcmFrame);
    GpsCorrectionSourceSettings settings;
    settings.host = QStringLiteral("127.0.0.1");
    settings.casterPort = caster.serverPort();
    settings.mountPoint = QStringLiteral("MOUNT");
    QVERIFY(source.start(settings));

    QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 2000);
    QCOMPARE(invalid.count(), 0);
    QCOMPARE(frames.first().at(0).toByteArray(), frame);
    QCOMPARE(frames.first().at(1).toUInt(), uint(1087));
    source.stop();
}

void GpsCorrectionCoreTest::
    sourceRejectsNtripSourcetableContentType()
{
    QTcpServer caster;
    QVERIFY(caster.listen(QHostAddress::LocalHost));
    connect(&caster, &QTcpServer::newConnection, &caster, [&]() {
        QTcpSocket *socket = caster.nextPendingConnection();
        QVERIFY(socket);
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
            QByteArray request = socket->property("request").toByteArray();
            request.append(socket->readAll());
            socket->setProperty("request", request);
            if (!request.endsWith("\r\n\r\n")
                || socket->property("responseSent").toBool()) {
                return;
            }
            socket->setProperty("responseSent", true);
            socket->write(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: gnss/sourcetable; charset=utf-8\r\n\r\n"
                "STR;OTHER;bad mount\r\nENDSOURCETABLE\r\n");
        });
    });

    QtGpsCorrectionSource source;
    QSignalSpy statuses(&source, &GpsCorrectionSource::statusChanged);
    GpsCorrectionSourceSettings settings;
    settings.host = QStringLiteral("127.0.0.1");
    settings.casterPort = caster.serverPort();
    settings.mountPoint = QStringLiteral("MISSING");
    QVERIFY(source.start(settings));

    QTRY_VERIFY_WITH_TIMEOUT(!source.active(), 2000);
    QVERIFY(!statuses.isEmpty());
    QVERIFY(statuses.last().at(0).toString().contains(
        QStringLiteral("source table"), Qt::CaseInsensitive));
    QVERIFY(!source.connected());
}

QTEST_MAIN(GpsCorrectionCoreTest)
#include "test_gpscorrectioncore.moc"
