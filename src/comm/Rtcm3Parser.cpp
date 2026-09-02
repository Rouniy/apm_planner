#include "Rtcm3Parser.h"

#include <QtMath>

#include <cmath>
#include <cstring>

namespace {
quint64 unsignedBits(const QByteArray &bytes, int bitOffset, int bitCount)
{
    quint64 value = 0;
    for (int bit = 0; bit < bitCount; ++bit) {
        const int absolute = bitOffset + bit;
        const int byteIndex = absolute / 8;
        if (byteIndex < 0 || byteIndex >= bytes.size()) {
            return 0;
        }
        const int bitIndex = 7 - (absolute % 8);
        value = (value << 1)
            | ((static_cast<quint8>(bytes.at(byteIndex)) >> bitIndex) & 1U);
    }
    return value;
}

qint64 signedBits(const QByteArray &bytes, int bitOffset, int bitCount)
{
    quint64 value = unsignedBits(bytes, bitOffset, bitCount);
    const quint64 sign = quint64(1) << (bitCount - 1);
    if (value & sign) {
        value |= (~quint64(0)) << bitCount;
    }
    return static_cast<qint64>(value);
}

bool ecefToGeodetic(double x, double y, double z,
                    RtcmBasePosition *position)
{
    if (!position || (!std::isfinite(x) || !std::isfinite(y)
                      || !std::isfinite(z))) {
        return false;
    }
    const double horizontal = std::hypot(x, y);
    if (horizontal < 1.0 && std::abs(z) < 1.0) {
        return false;
    }

    constexpr double semiMajor = 6378137.0;
    constexpr double eccentricitySquared = 6.6943799901413165e-3;
    double latitude = std::atan2(
        z, horizontal * (1.0 - eccentricitySquared));
    double altitude = 0.0;
    for (int iteration = 0; iteration < 10; ++iteration) {
        const double sine = std::sin(latitude);
        const double normal = semiMajor / std::sqrt(
            1.0 - eccentricitySquared * sine * sine);
        const double cosine = std::cos(latitude);
        altitude = std::abs(cosine) > 1.0e-12
            ? horizontal / cosine - normal
            : std::abs(z) - normal * (1.0 - eccentricitySquared);
        const double denominator = horizontal
            * (1.0 - eccentricitySquared * normal
               / (normal + altitude));
        const double next = std::atan2(z, denominator);
        if (std::abs(next - latitude) < 1.0e-13) {
            latitude = next;
            break;
        }
        latitude = next;
    }

    position->latitude = qRadiansToDegrees(latitude);
    position->longitude = qRadiansToDegrees(std::atan2(y, x));
    position->altitude = altitude;
    return std::isfinite(position->latitude)
        && std::isfinite(position->longitude)
        && std::isfinite(position->altitude);
}
} // namespace

Rtcm3Parser::Rtcm3Parser()
{
    reset();
}

void Rtcm3Parser::reset()
{
    m_state = State::WaitingForPreamble;
    m_payloadLength = 0;
    m_bytesRead = 0;
    m_lengthBytesRead = 0;
    m_crcBytesRead = 0;
}

bool Rtcm3Parser::addByte(quint8 byte)
{
    switch (m_state) {
    case State::WaitingForPreamble:
        if (byte == Preamble) {
            m_buffer[0] = byte;
            m_bytesRead = 1;
            m_lengthBytesRead = 0;
            m_state = State::ReadingLength;
        }
        break;
    case State::ReadingLength:
        m_buffer[m_bytesRead++] = byte;
        ++m_lengthBytesRead;
        if (m_lengthBytesRead == 2) {
            m_payloadLength = ((m_buffer[1] & 0x03U) << 8)
                | m_buffer[2];
            if (m_payloadLength <= 0
                || m_payloadLength > MaximumPayloadLength) {
                reset();
                if (byte == Preamble) {
                    return addByte(byte);
                }
            } else {
                m_state = State::ReadingPayload;
            }
        }
        break;
    case State::ReadingPayload:
        if (m_bytesRead >= static_cast<int>(m_buffer.size())) {
            reset();
            return addByte(byte);
        }
        m_buffer[m_bytesRead++] = byte;
        if (m_bytesRead == HeaderSize + m_payloadLength) {
            m_state = State::ReadingCrc;
            m_crcBytesRead = 0;
        }
        break;
    case State::ReadingCrc:
        m_crc[m_crcBytesRead++] = byte;
        if (m_crcBytesRead == CrcSize) {
            return true;
        }
        break;
    }
    return false;
}

quint32 Rtcm3Parser::crc24q(const quint8 *data, std::size_t length)
{
    constexpr quint32 polynomial = 0x1864cfbU;
    quint32 crc = 0;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= static_cast<quint32>(data[index]) << 16;
        for (int bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (crc & 0x1000000U) {
                crc ^= polynomial;
            }
        }
    }
    return crc & 0xffffffU;
}

bool Rtcm3Parser::validateCrc() const
{
    if (m_payloadLength <= 0
        || m_bytesRead != HeaderSize + m_payloadLength
        || m_crcBytesRead != CrcSize) {
        return false;
    }
    const quint32 computed = crc24q(
        m_buffer.data(), static_cast<std::size_t>(m_bytesRead));
    const quint32 received = (static_cast<quint32>(m_crc[0]) << 16)
        | (static_cast<quint32>(m_crc[1]) << 8)
        | static_cast<quint32>(m_crc[2]);
    return computed == received;
}

quint16 Rtcm3Parser::messageId() const
{
    if (m_payloadLength < 2) {
        return 0;
    }
    return static_cast<quint16>(
        ((m_buffer[3] << 4) | (m_buffer[4] >> 4)) & 0x0fffU);
}

QByteArray Rtcm3Parser::currentFrame() const
{
    if (m_payloadLength <= 0 || m_crcBytesRead != CrcSize) {
        return {};
    }
    QByteArray frame(reinterpret_cast<const char *>(m_buffer.data()),
                     HeaderSize + m_payloadLength);
    frame.append(reinterpret_cast<const char *>(m_crc.data()), CrcSize);
    return frame;
}

bool Rtcm3Parser::extractBasePosition(
    const QByteArray &frame, RtcmBasePosition *position)
{
    if (!position || frame.size() < HeaderSize + CrcSize + 19
        || static_cast<quint8>(frame.at(0)) != Preamble) {
        return false;
    }
    const int payloadLength =
        ((static_cast<quint8>(frame.at(1)) & 0x03U) << 8)
        | static_cast<quint8>(frame.at(2));
    if (frame.size() != HeaderSize + payloadLength + CrcSize
        || payloadLength < 19) {
        return false;
    }
    const quint32 expectedCrc = crc24q(
        reinterpret_cast<const quint8 *>(frame.constData()),
        static_cast<std::size_t>(HeaderSize + payloadLength));
    const int crcOffset = HeaderSize + payloadLength;
    const quint32 actualCrc =
        (static_cast<quint32>(static_cast<quint8>(frame.at(crcOffset)))
         << 16)
        | (static_cast<quint32>(
               static_cast<quint8>(frame.at(crcOffset + 1))) << 8)
        | static_cast<quint8>(frame.at(crcOffset + 2));
    if (expectedCrc != actualCrc) {
        return false;
    }

    const quint16 id = static_cast<quint16>(unsignedBits(frame, 24, 12));
    if (id != 1005 && id != 1006) {
        return false;
    }
    // RTCM 1006 appends the 16-bit DF028 antenna-reference-point height
    // above the monument. ECEF already describes the ARP, so DF028 must not
    // be added to the geodetic altitude, but it must still be present in a
    // structurally valid 1006 payload.
    if (id == 1006 && payloadLength < 21) {
        return false;
    }
    const double x = static_cast<double>(signedBits(frame, 58, 38))
        * 0.0001;
    const double y = static_cast<double>(signedBits(frame, 98, 38))
        * 0.0001;
    const double z = static_cast<double>(signedBits(frame, 138, 38))
        * 0.0001;
    return ecefToGeodetic(x, y, z, position);
}
