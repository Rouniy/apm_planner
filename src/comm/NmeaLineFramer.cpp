#include "NmeaLineFramer.h"

#include <QtGlobal>

NmeaLineFramer::NmeaLineFramer(int maximumLineBytes)
    : m_maximumLineBytes(qMax(1, maximumLineBytes))
{
    m_lineBuffer.reserve(qMin(m_maximumLineBytes, 4096));
}

NmeaLineFramerResult NmeaLineFramer::ingest(const QByteArray &bytes)
{
    NmeaLineFramerResult result;
    for (const char byte : bytes) {
        if (byte == '\n') {
            if (m_discardingOversizedLine) {
                m_discardingOversizedLine = false;
                m_lineBuffer.clear();
                continue;
            }
            QByteArray line = m_lineBuffer;
            m_lineBuffer.clear();
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            result.lines.append(line);
            continue;
        }
        if (m_discardingOversizedLine) {
            continue;
        }
        if (m_lineBuffer.size() >= m_maximumLineBytes) {
            m_lineBuffer.clear();
            m_discardingOversizedLine = true;
            ++result.oversizedLines;
            continue;
        }
        m_lineBuffer.append(byte);
    }
    return result;
}

void NmeaLineFramer::reset()
{
    m_lineBuffer.clear();
    m_discardingOversizedLine = false;
}
