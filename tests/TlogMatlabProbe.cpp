#include "comm/TlogMatlabExporter.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QDebug>

namespace {
bool put(QFile &file, const mavlink_message_t &message, quint64 timestamp) {
    uchar stamp[8]; qToBigEndian<quint64>(timestamp, stamp);
    uint8_t frame[MAVLINK_MAX_PACKET_LEN]{};
    const int count = mavlink_msg_to_send_buffer(frame, &message);
    return file.write(reinterpret_cast<const char *>(stamp), 8) == 8
        && file.write(reinterpret_cast<const char *>(frame), count) == count;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() >= 3 && args[1] == "--fixture") {
        const int count = args.size() > 3 ? args[3].toInt() : 12;
        if (count < 1 || count > 10000000) return 2;
        QFile file(args[2]);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(255, 190, &message, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
        if (!put(file, message, 1757155200123456ULL)) return 2;
        mavlink_msg_heartbeat_pack(1, 1, &message, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 5, MAV_STATE_ACTIVE);
        if (!put(file, message, 1757155200223456ULL)) return 2;
        for (int index = 0; index < count; ++index) {
            mavlink_msg_attitude_pack(index % 2 ? 2 : 1, 1, &message, quint32(index * 20),
                0.125f * index, -0.25f, 0.5f, -0.0f, 1.0f, 2.0f);
            if (!put(file, message, 1757155200123456ULL + quint64(index) * 20000)) return 2;
        }
        mavlink_msg_global_position_int_pack(1, 1, &message, 1200,
            -353632621, 1491652374, 584200, 12340, -15, 20, -3, 9000);
        if (!put(file, message, 1757155200987654ULL)) return 2;
        // MP10 schema extends the old shared dialect; test actual wire values.
        message = {};
        message.msgid = MAVLINK_MSG_ID_MISSION_CURRENT;
        _mav_put_uint16_t(_MAV_PAYLOAD_NON_CONST(&message), 0, 4);
        _mav_put_uint16_t(_MAV_PAYLOAD_NON_CONST(&message), 2, 312);
        _mav_put_uint8_t(_MAV_PAYLOAD_NON_CONST(&message), 4, 3);
        _mav_put_uint8_t(_MAV_PAYLOAD_NON_CONST(&message), 5, 2);
        mavlink_status_t framing{};
        mavlink_finalize_message_buffer(&message, 1, 1, &framing, 2, 6,
            MAVLINK_MSG_ID_MISSION_CURRENT_CRC);
        if (!put(file, message, 1757155200990654ULL)) return 2;
        mavlink_autopilot_state_for_gimbal_device_t gimbal{};
        gimbal.time_boot_us = 9007199254740993ULL;
        gimbal.target_system = 1;
        gimbal.target_component = 154;
        gimbal.feed_forward_angular_velocity_z = -0.125f;
        mavlink_msg_autopilot_state_for_gimbal_device_encode(1, 1, &message, &gimbal);
        _mav_put_float(_MAV_PAYLOAD_NON_CONST(&message), 53, 1.25f);
        mavlink_finalize_message_buffer(&message, 1, 1, &framing, 53, 57,
            MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE_CRC);
        if (!put(file, message, 1757155200992654ULL)) return 2;
        struct ExtensionFixture { quint32 id; quint8 minimum, oldLength, length, crc; };
        const ExtensionFixture extensions[] = {
            {259, MAVLINK_MSG_ID_CAMERA_INFORMATION_MIN_LEN, 235, 236, MAVLINK_MSG_ID_CAMERA_INFORMATION_CRC},
            {225, MAVLINK_MSG_ID_EFI_STATUS_MIN_LEN, 69, 73, MAVLINK_MSG_ID_EFI_STATUS_CRC},
            {285, MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS_MIN_LEN, 40, 49, MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS_CRC},
            {283, MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION_MIN_LEN, 144, 145, MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION_CRC},
            {69, MAVLINK_MSG_ID_MANUAL_CONTROL_MIN_LEN, 11, 30, MAVLINK_MSG_ID_MANUAL_CONTROL_CRC},
            {331, MAVLINK_MSG_ID_ODOMETRY_MIN_LEN, 232, 233, MAVLINK_MSG_ID_ODOMETRY_CRC},
            {108, MAVLINK_MSG_ID_SIM_STATE_MIN_LEN, 84, 92, MAVLINK_MSG_ID_SIM_STATE_CRC},
            {269, MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION_MIN_LEN, 213, 214, MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION_CRC}
        };
        for (const auto &extension : extensions) {
            message = {};
            message.msgid = extension.id;
            for (int offset = extension.oldLength; offset < extension.length; ++offset)
                _MAV_PAYLOAD_NON_CONST(&message)[offset] = char(0x20 + offset % 16);
            mavlink_finalize_message_buffer(&message, 1, 1, &framing, extension.minimum,
                extension.length, extension.crc);
            if (!put(file, message, 1757155200993654ULL)) return 2;
            // Omitted extensions must be zero even if old frame memory held data.
            mavlink_finalize_message_buffer(&message, 1, 1, &framing, extension.minimum,
                extension.oldLength, extension.crc);
            if (!put(file, message, 1757155200994654ULL)) return 2;
        }
        mavlink_statustext_t status{}; status.severity = MAV_SEVERITY_WARNING;
        qstrncpy(status.text, "Array text must not become a MATLAB variable", sizeof(status.text));
        mavlink_msg_statustext_encode(1, 1, &message, &status);
        if (!put(file, message, 1757155200999654ULL)) return 2;
        return file.flush() ? 0 : 2;
    }
    if (args.size() != 3) return 2;
    const auto result = TlogMatlabExporter::Export(args[1], args[2]);
    QJsonObject object{{"success", result.success}, {"cancelled", result.cancelled},
        {"error", result.error}, {"message", result.message}, {"items", result.itemCount},
        {"records", double(result.recordsRead)}, {"skippedBytes", double(result.skippedBytes)}};
    qInfo().noquote() << QJsonDocument(object).toJson(QJsonDocument::Compact);
    return result.success ? 0 : 1;
}
