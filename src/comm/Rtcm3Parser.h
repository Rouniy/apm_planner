#ifndef RTCM3PARSER_H
#define RTCM3PARSER_H

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <cstddef>

struct RtcmBasePosition
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
};

class Rtcm3Parser final
{
public:
    static constexpr quint8 Preamble = 0xd3;
    static constexpr int HeaderSize = 3;
    static constexpr int CrcSize = 3;
    static constexpr int MaximumPayloadLength = 1023;

    Rtcm3Parser();

    void reset();
    bool addByte(quint8 byte);
    bool validateCrc() const;
    quint16 messageId() const;
    QByteArray currentFrame() const;

    static quint32 crc24q(const quint8 *data, std::size_t length);
    static bool extractBasePosition(const QByteArray &frame,
                                    RtcmBasePosition *position);

private:
    enum class State
    {
        WaitingForPreamble,
        ReadingLength,
        ReadingPayload,
        ReadingCrc
    };

    State m_state = State::WaitingForPreamble;
    std::array<quint8, HeaderSize + MaximumPayloadLength> m_buffer{};
    std::array<quint8, CrcSize> m_crc{};
    int m_payloadLength = 0;
    int m_bytesRead = 0;
    int m_lengthBytesRead = 0;
    int m_crcBytesRead = 0;
};

#endif // RTCM3PARSER_H
