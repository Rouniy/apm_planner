#include "RadioStatusMonitor.h"

#include <algorithm>

namespace {

double ratio(quint8 signal, quint8 noise)
{
    // C# float arithmetic on the raw uint8 values: (rssi - noise) / 1.9f.
    return (static_cast<double>(signal) - static_cast<double>(noise))
        / RadioStatusMonitor::SnrDivisor;
}

} // namespace

double RadioStatusSample::localSnrDbRaw() const
{
    return isValid() ? ratio(rssi, noise) : 0.0;
}

double RadioStatusSample::remoteSnrDbRaw() const
{
    return isValid() ? ratio(remrssi, remnoise) : 0.0;
}

double RadioStatusMonitor::SnrFilter::read(double raw, qint64 nowMs)
{
    if (!stepped) {
        value = raw * SnrAlpha + value * (1.0 - SnrAlpha); // first read steps from 0, as MP10
        lastStepMs = nowMs;
        stepped = true;
        return value;
    }
    if (nowMs < lastStepMs) {
        // Clock moved backwards: keep the value, restart the hold from here.
        lastStepMs = nowMs;
        return value;
    }
    if (nowMs - lastStepMs >= SnrHoldMs) {
        value = raw * SnrAlpha + value * (1.0 - SnrAlpha);
        lastStepMs = nowMs;
    }
    return value;
}

RadioStatusMonitor::RadioStatusMonitor(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<RadioStatusSample>("RadioStatusSample");
}

bool RadioStatusMonitor::IsRadioStatusMessage(quint32 messageId) noexcept
{
    return messageId == MAVLINK_MSG_ID_RADIO_STATUS || messageId == MAVLINK_MSG_ID_RADIO;
}

bool RadioStatusMonitor::observe(int linkId, const mavlink_message_t &message, qint64 nowMs)
{
    if (linkId < 0) {
        return false;
    }
    RadioStatusSample sample;
    if (message.msgid == MAVLINK_MSG_ID_RADIO_STATUS) {
        mavlink_radio_status_t status;
        mavlink_msg_radio_status_decode(&message, &status);
        sample.rssi = status.rssi;
        sample.remrssi = status.remrssi;
        sample.txbuf = status.txbuf;
        sample.noise = status.noise;
        sample.remnoise = status.remnoise;
        sample.rxerrors = status.rxerrors;
        sample.fixedCount = status.fixed;
    } else if (message.msgid == MAVLINK_MSG_ID_RADIO) {
        mavlink_radio_t radio;
        mavlink_msg_radio_decode(&message, &radio);
        sample.rssi = radio.rssi;
        sample.remrssi = radio.remrssi;
        sample.txbuf = radio.txbuf;
        sample.noise = radio.noise;
        sample.remnoise = radio.remnoise;
        sample.rxerrors = radio.rxerrors;
        sample.fixedCount = radio.fixed;
    } else {
        return false;
    }
    sample.valid = true;
    sample.receivedMs = nowMs;
    sample.messageId = message.msgid;
    sample.systemId = message.sysid;
    sample.componentId = message.compid;

    // Only the raw sample changes; the filters keep their hold and value.
    m_links[linkId].sample = sample;
    emit sampleReceived(linkId, sample);
    return true;
}

bool RadioStatusMonitor::hasSample(int linkId) const
{
    const auto it = m_links.constFind(linkId);
    return it != m_links.constEnd() && it->sample.isValid();
}

RadioStatusSample RadioStatusMonitor::lastSample(int linkId) const
{
    return m_links.value(linkId).sample;
}

QList<int> RadioStatusMonitor::linkIds() const
{
    QList<int> ids = m_links.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

double RadioStatusMonitor::localSnrDb(int linkId, qint64 nowMs)
{
    const auto it = m_links.find(linkId);
    if (it == m_links.end() || !it->sample.isValid()) {
        return 0.0;
    }
    return it->local.read(it->sample.localSnrDbRaw(), nowMs);
}

double RadioStatusMonitor::remoteSnrDb(int linkId, qint64 nowMs)
{
    const auto it = m_links.find(linkId);
    if (it == m_links.end() || !it->sample.isValid()) {
        return 0.0;
    }
    return it->remote.read(it->sample.remoteSnrDbRaw(), nowMs);
}

void RadioStatusMonitor::forgetLink(int linkId)
{
    if (m_links.remove(linkId) > 0) {
        emit linkForgotten(linkId);
    }
}

void RadioStatusMonitor::clear()
{
    const QList<int> ids = linkIds();
    m_links.clear();
    for (int id : ids) {
        emit linkForgotten(id);
    }
}
