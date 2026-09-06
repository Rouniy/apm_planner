#include "DeveloperVehicleToolRuntimeAudit.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavFtpProtocol.h"
#include "comm/MavFtpServiceInterface.h"
#include "comm/TCPLink.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"
#include "services/DeveloperVehicleToolService.h"
#include "services/ParameterRecoveryService.h"
#include "services/OfflineMagFitApplyService.h"
#include "ui/OfflineMagFitWindow.h"
#include "ui/BackstageView.h"
#include "ui/MainWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "ui/configuration/ConfigCompassView.h"
#include "ui/configuration/MavFTPUIView.h"
#include "ui/configuration/SetupView.h"

#include <QAction>
#include "comm/RemoteDataFlashLogService.h"
#include "comm/ExactLogTransferService.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QThread>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QtEndian>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>

namespace {
constexpr int FixtureLinkId = 910110;
constexpr quint8 FixtureSystem = 234;
constexpr int DataFlashFmtLength = 89;

QMap<QString, float> magFitParameters()
{
    return {{QStringLiteral("COMPASS_DEV_ID"), 202.0f},
            {QStringLiteral("COMPASS_PRIO1_ID"), 202.0f},
            {QStringLiteral("COMPASS_LEARN"), 1.0f},
            {QStringLiteral("COMPASS_OFS_X"), -50.0f},
            {QStringLiteral("COMPASS_OFS_Y"), 30.0f},
            {QStringLiteral("COMPASS_OFS_Z"), -20.0f},
            {QStringLiteral("COMPASS_DIA_X"), 1.0f},
            {QStringLiteral("COMPASS_DIA_Y"), 1.0f},
            {QStringLiteral("COMPASS_DIA_Z"), 1.0f},
            {QStringLiteral("COMPASS_ODI_X"), 0.0f},
            {QStringLiteral("COMPASS_ODI_Y"), 0.0f},
            {QStringLiteral("COMPASS_ODI_Z"), 0.0f},
            {QStringLiteral("COMPASS_SCALE"), 0.0f},
            {QStringLiteral("COMPASS_ORIENT"), 0.0f},
            {QStringLiteral("COMPASS_EXTERNAL"), 1.0f},
            {QStringLiteral("AHRS_ORIENTATION"), 0.0f},
            {QStringLiteral("COMPASS_CAL_FIT"), 16.0f},
            {QStringLiteral("COMPASS_OFFS_MAX"), 1800.0f},
            {QStringLiteral("LOG_BACKEND_TYPE"), 3.0f}};
}

QByteArray magFitLogFixture()
{
    QByteArray bytes("FMT,150,31,PARM,QNf,TimeUS,Name,Value\n"
                     "FMT,151,49,MAG,QBfffffffffB,TimeUS,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ,MOX,MOY,MOZ,Health\n");
    const auto parameters = magFitParameters();
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) {
        bytes += "PARM,1," + it.key().toLatin1() + ","
            + QByteArray::number(it.value(), 'g', 9) + '\n';
    }
    // MAG is corrected. Removing logged (-50,30,-20) offsets recovers a
    // radius450 sphere requiring additive new offsets (20,-10,5).
    constexpr int count = 192;
    const double goldenAngle = std::acos(-1.0) * (3.0 - std::sqrt(5.0));
    for (int index = 0; index < count; ++index) {
        const double z = 1.0 - 2.0 * (index + 0.5) / count;
        const double radial = std::sqrt(1.0 - z * z);
        const double angle = index * goldenAngle;
        const double x = 450.0 * radial * std::cos(angle) - 20.0 - 50.0;
        const double y = 450.0 * radial * std::sin(angle) + 10.0 + 30.0;
        const double fieldZ = 450.0 * z - 5.0 - 20.0;
        bytes += "MAG," + QByteArray::number(1000 + index) + ",0,"
            + QByteArray::number(x, 'g', 17) + ','
            + QByteArray::number(y, 'g', 17) + ','
            + QByteArray::number(fieldZ, 'g', 17)
            + ",-50,30,-20,0,0,0,1\n";
    }
    return bytes;
}

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray apjZlibStream(const QByteArray &image)
{
    const QByteArray qtCompressed = qCompress(image, 9);
    return qtCompressed.size() > 4 ? qtCompressed.mid(4) : QByteArray();
}

QByteArray inflateApjImage(const QByteArray &compressed, quint32 imageSize)
{
    QByteArray qtCompressed(4, '\0');
    qToBigEndian<quint32>(
        imageSize, reinterpret_cast<uchar *>(qtCompressed.data()));
    qtCompressed.append(compressed);
    return qUncompress(qtCompressed);
}

QByteArray fixedDataFlashField(const QByteArray &value, int length)
{
    QByteArray field(length, '\0');
    const QByteArray clipped = value.left(length);
    if (!clipped.isEmpty()) {
        std::memcpy(field.data(), clipped.constData(), size_t(clipped.size()));
    }
    return field;
}

void appendDataFlashFmt(QByteArray *log, quint8 type, quint8 length,
                        const QByteArray &name, const QByteArray &format,
                        const QByteArray &columns)
{
    if (!log) return;
    log->append(char(0xa3));
    log->append(char(0x95));
    log->append(char(0x80));
    log->append(char(type));
    log->append(char(length));
    log->append(fixedDataFlashField(name, 4));
    log->append(fixedDataFlashField(format, 16));
    log->append(fixedDataFlashField(columns, 64));
}

void appendDataFlashRecord(QByteArray *log, quint8 type, int length,
                           quint64 value = 0)
{
    if (!log || length < 3) return;
    QByteArray record(length, '\0');
    record[0] = char(0xa3);
    record[1] = char(0x95);
    record[2] = char(type);
    if (length >= 11) {
        qToLittleEndian<quint64>(
            value, reinterpret_cast<uchar *>(record.data() + 3));
    }
    log->append(record);
}

QByteArray splitDataFlashFixture()
{
    constexpr quint8 FmtuType = 150;
    constexpr quint8 UnitType = 151;
    constexpr quint8 MultType = 152;
    constexpr quint8 DataType = 153;
    QByteArray log;
    appendDataFlashFmt(&log, 0x80, DataFlashFmtLength, "FMT", "BBnNZ",
                       "Type,Length,Name,Format,Columns");
    appendDataFlashFmt(&log, FmtuType, 44, "FMTU", "QBNN",
                       "TimeUS,FmtType,UnitIds,MultIds");
    appendDataFlashFmt(&log, UnitType, 76, "UNIT", "QbZ",
                       "TimeUS,Id,Label");
    appendDataFlashFmt(&log, MultType, 20, "MULT", "Qbd",
                       "TimeUS,Id,Mult");
    appendDataFlashFmt(&log, DataType, 11, "DUMY", "Q", "TimeUS");
    int start = log.size();
    appendDataFlashRecord(&log, FmtuType, 44, 1);
    log[start + 11] = char(DataType);
    log[start + 12] = 's';
    log[start + 28] = 'F';
    start = log.size();
    appendDataFlashRecord(&log, UnitType, 76, 2);
    log[start + 11] = 's';
    std::memcpy(log.data() + start + 12, "seconds", 7);
    start = log.size();
    appendDataFlashRecord(&log, MultType, 20, 3);
    log[start + 11] = 'F';
    const double multiplier = 1.0e-6;
    quint64 multiplierBits = 0;
    std::memcpy(&multiplierBits, &multiplier, sizeof(multiplierBits));
    qToLittleEndian<quint64>(
        multiplierBits, reinterpret_cast<uchar *>(log.data() + start + 12));
    for (quint64 value : {11ULL, 22ULL, 33ULL, 44ULL}) {
        appendDataFlashRecord(&log, DataType, 11, value);
    }
    return log;
}

struct SplitPiece
{
    bool valid = false;
    QSet<QString> definitions;
    QSet<QString> metadataRecords;
    QVector<quint64> values;
};

SplitPiece readSplitPiece(const QString &path)
{
    SplitPiece result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const QByteArray bytes = file.readAll();
    QHash<quint8, QPair<int, QString>> definitions;
    int offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 3
            || quint8(bytes.at(offset)) != 0xa3
            || quint8(bytes.at(offset + 1)) != 0x95) {
            return result;
        }
        const quint8 type = quint8(bytes.at(offset + 2));
        const bool formatRecord = type == 0x80;
        int length = DataFlashFmtLength;
        QString name = QStringLiteral("FMT");
        if (formatRecord) {
            if (bytes.size() - offset < DataFlashFmtLength) return result;
            const quint8 describedType = quint8(bytes.at(offset + 3));
            length = quint8(bytes.at(offset + 4));
            const QByteArray rawName = bytes.mid(offset + 5, 4);
            name = QString::fromLatin1(rawName.constData(),
                rawName.indexOf('\0') >= 0 ? rawName.indexOf('\0')
                                            : rawName.size());
            if (length < 3 || name.isEmpty()) return result;
            definitions.insert(describedType, qMakePair(length, name));
            result.definitions.insert(name);
            length = DataFlashFmtLength;
        } else {
            const auto definition = definitions.constFind(type);
            if (definition == definitions.constEnd()) return result;
            length = definition->first;
            name = definition->second;
        }
        if (length < 3 || bytes.size() - offset < length) return result;
        if (!formatRecord && (name == QStringLiteral("FMTU")
            || name == QStringLiteral("UNIT")
            || name == QStringLiteral("MULT"))) {
            result.metadataRecords.insert(name);
        } else if (!formatRecord && name == QStringLiteral("DUMY")) {
            if (length != 11) return result;
            result.values.append(qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar *>(bytes.constData() + offset + 3)));
        }
        offset += length;
    }
    result.valid = offset == bytes.size();
    return result;
}

bool waitFor(const std::function<bool()> &condition, int timeout = 2500)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}

// No sockets are opened: this exercises the production physical-link path,
// command/parameter services and actual Tools navigation against one simulator.
class DeveloperAuditLink final : public TCPLink {
public:
    DeveloperAuditLink() : TCPLink(QHostAddress::LocalHost,
        QStringLiteral("Developer action audit"), 61981, false)
    {
        ftpFileData = QByteArray::fromHex(
            "00017f80ff102030405060708090a0b0c0d0e0f0");
        ftpFileData.append('\0');
        ftpFileData.append("ArduPilot MAVFTP runtime payload\n");
        ftpFileData.append(QByteArray(73, char(0xa5)));
    }
    int getId() const override { return FixtureLinkId; }
    bool isConnected() const override { return m_connected; }
    bool connect() override {
        if (!m_connected) {
            m_connected = true;
            emit connected(); emit connected(this); emit connected(true);
        }
        return true;
    }
    bool disconnect() override {
        if (m_connected) {
            m_connected = false;
            emit disconnected(); emit disconnected(this); emit connected(false);
        }
        return true;
    }
    void inject(mavlink_message_t message) {
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(buffer, &message);
        emit bytesReceived(this, QByteArray(reinterpret_cast<const char *>(buffer), size));
    }
    void heartbeatFor(quint8 componentId, bool armed = false) {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(FixtureSystem, componentId, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
            0, MAV_STATE_STANDBY);
        inject(message);
    }
    void heartbeat(bool armed = false) { heartbeatFor(1, armed); }
    void pressureReply() {
        mavlink_param_value_t value{};
        std::memcpy(value.param_id, "GND_ABS_PRESS", 13);
        value.param_value = pressure;
        value.param_type = MAV_PARAM_TYPE_REAL32;
        value.param_count = 1;
        value.param_index = 0;
        mavlink_message_t message{};
        mavlink_msg_param_value_encode(FixtureSystem, 1, &message, &value);
        inject(message);
    }
    void magFitReply(const QString &name, quint16 index = UINT16_MAX)
    {
        if (!magFitValues.contains(name)) return;
        mavlink_param_value_t value{};
        const QByteArray bytes = name.toLatin1();
        std::memcpy(value.param_id, bytes.constData(), size_t(qMin(16, bytes.size())));
        value.param_value = magFitValues.value(name); // ArduPilot C-style encoding.
        value.param_type = name.endsWith(QStringLiteral("_ID"))
            ? MAV_PARAM_TYPE_UINT32
            : (name == QStringLiteral("COMPASS_LEARN")
               || name == QStringLiteral("COMPASS_ORIENT")
               || name == QStringLiteral("COMPASS_EXTERNAL")
               || name == QStringLiteral("LOG_BACKEND_TYPE")
               || name == QStringLiteral("AHRS_ORIENTATION"))
                ? MAV_PARAM_TYPE_INT8 : MAV_PARAM_TYPE_REAL32;
        value.param_count = static_cast<quint16>(magFitValues.size());
        value.param_index = index;
        mavlink_message_t message{};
        mavlink_msg_param_value_encode(FixtureSystem, 1, &message, &value);
        inject(message);
    }
    void recoveryReply(const QString &name) {
        if (!recoveryValues.contains(name)
            || (name == QStringLiteral("RECOVERY_GAIN")
                && recoveryValues.value(QStringLiteral("RECOVERY_ENABLE")) == 0))
            return;
        mavlink_param_value_t value{};
        const QByteArray bytes = name.toLatin1();
        std::memcpy(value.param_id, bytes.constData(), size_t(qMin(16, bytes.size())));
        // Production ArduPilot heartbeats select C-style parameter encoding.
        value.param_value = recoveryValues.value(name);
        value.param_type = name == QStringLiteral("COMPASS_DEV_ID")
            ? MAV_PARAM_TYPE_UINT32 : MAV_PARAM_TYPE_REAL32;
        value.param_count = 5;
        value.param_index = UINT16_MAX;
        mavlink_message_t message{};
        mavlink_msg_param_value_encode(FixtureSystem, 1, &message, &value);
        inject(message);
    }
    mavlink_message_t ftpResponse(
        const MavFtpProtocol::PayloadHeader &request,
        int sourceSystem, int sourceComponent,
        int targetSystem, int targetComponent,
        const QByteArray &data, int session,
        MavFtpProtocol::Opcode responseOpcode =
            MavFtpProtocol::Opcode::Ack) const
    {
        MavFtpProtocol::PayloadHeader response;
        response.sequence = static_cast<quint16>(request.sequence + 1u);
        response.session = static_cast<quint8>(session);
        response.opcode = responseOpcode;
        response.requestOpcode = request.opcode;
        response.offset = request.offset;
        response.data = data;
        response.size = static_cast<quint8>(data.size());
        QString error;
        const QByteArray wire = MavFtpProtocol::encodePayload(response, &error);
        if (wire.size() != MavFtpProtocol::PayloadSize) {
            return {};
        }
        quint8 payload[MavFtpProtocol::PayloadSize]{};
        std::memcpy(payload, wire.constData(), sizeof(payload));
        mavlink_message_t message{};
        mavlink_msg_file_transfer_protocol_pack(
            static_cast<quint8>(sourceSystem),
            static_cast<quint8>(sourceComponent), &message, 0,
            static_cast<quint8>(targetSystem),
            static_cast<quint8>(targetComponent), payload);
        return message;
    }
    void handleFtpRequest(const mavlink_message_t &message)
    {
        mavlink_file_transfer_protocol_t outer{};
        mavlink_msg_file_transfer_protocol_decode(&message, &outer);
        const QByteArray wire(reinterpret_cast<const char *>(outer.payload),
                              MavFtpProtocol::PayloadSize);
        MavFtpProtocol::PayloadHeader request;
        QString error;
        if (!MavFtpProtocol::decodePayload(wire, &request, &error)) {
            ftpEnvelopeValid = false;
            return;
        }
        ++ftpRequestCount;
        ftpOpcodes.append(request.opcode);
        if (outer.target_network != 0
            || outer.target_system != FixtureSystem
            || outer.target_component != 1) {
            ftpEnvelopeValid = false;
        }
        if (ftpGcsSystem == 0) {
            ftpGcsSystem = message.sysid;
            ftpGcsComponent = message.compid;
        } else if (ftpGcsSystem != message.sysid
                   || ftpGcsComponent != message.compid) {
            ftpEnvelopeValid = false;
        }

        QByteArray responseData;
        int responseSession = request.session;
        MavFtpProtocol::Opcode responseOpcode = MavFtpProtocol::Opcode::Ack;
        switch (request.opcode) {
        case MavFtpProtocol::Opcode::ListDirectory:
            if (request.offset == 0) {
                responseData.append('D');
                responseData.append("logs", 4);
                responseData.append('\0');
                responseData.append('F');
                responseData.append("threads.txt\t", 12);
                responseData.append(QByteArray::number(ftpFileData.size()));
                responseData.append('\0');
            } else if (request.offset == 2) {
                responseOpcode = MavFtpProtocol::Opcode::Nak;
                responseData.append(static_cast<char>(
                    MavFtpProtocol::ErrorCode::EndOfFile));
            } else {
                ftpEnvelopeValid = false;
                return;
            }
            break;
        case MavFtpProtocol::Opcode::ResetSessions:
            break;
        case MavFtpProtocol::Opcode::OpenFileReadOnly:
            ftpRemotePath = QString::fromUtf8(request.data);
            responseData.resize(4);
            qToLittleEndian<quint32>(
                static_cast<quint32>(ftpFileData.size()),
                reinterpret_cast<uchar *>(responseData.data()));
            responseSession = 7;
            break;
        case MavFtpProtocol::Opcode::ReadFile: {
            const quint64 end = static_cast<quint64>(request.offset)
                + static_cast<quint64>(request.size);
            if (request.session != 7 || request.size == 0
                || request.offset >= static_cast<quint32>(ftpFileData.size())
                || end > static_cast<quint64>(ftpFileData.size())) {
                ftpEnvelopeValid = false;
                return;
            }
            responseData = ftpFileData.mid(int(request.offset), request.size);
            responseSession = 7;
            break;
        }
        case MavFtpProtocol::Opcode::TerminateSession:
            if (request.session != 7)
                ftpEnvelopeValid = false;
            responseSession = 7;
            break;
        case MavFtpProtocol::Opcode::CreateFile:
        case MavFtpProtocol::Opcode::WriteFile:
        case MavFtpProtocol::Opcode::RemoveFile:
        case MavFtpProtocol::Opcode::CreateDirectory:
        case MavFtpProtocol::Opcode::RemoveDirectory:
            ++destructiveFtpRequests;
            ftpEnvelopeValid = false;
            return;
        default:
            ftpEnvelopeValid = false;
            return;
        }

        const mavlink_message_t correct = ftpResponse(
            request, FixtureSystem, 1,
            message.sysid, message.compid, responseData, responseSession,
            responseOpcode);
        QTimer::singleShot(0, this,
            [this, message, request, correct]() {
            if (!ftpWrongResponsesInjected) {
                ftpWrongResponsesInjected = true;
                // Both packets have a valid FTP correlation envelope. They
                // must still be ignored at the physical/source/target fence.
                const int requestsBeforeWrongReplies = ftpRequestCount;
                inject(ftpResponse(request, FixtureSystem - 1, 1,
                                   message.sysid, message.compid,
                                   QByteArray(), request.session));
                if (ftpRequestCount != requestsBeforeWrongReplies)
                    ftpWrongResponsesRejected = false;
                inject(ftpResponse(request, FixtureSystem, 1,
                                   message.sysid - 1, message.compid,
                                   QByteArray(), request.session));
                if (ftpRequestCount != requestsBeforeWrongReplies)
                    ftpWrongResponsesRejected = false;
            }
            inject(correct);
        });
    }
    QByteArray remoteBlockBytes(quint32 sequence) const {
        QByteArray bytes(200, '\0');
        if (sequence == 0) {
            for (int i = 0; i < bytes.size(); ++i) bytes[i] = char(i);
        } else if (sequence == 1) bytes.fill(char(0x7e));
        return bytes;
    }
    void remoteBlock(quint32 sequence, quint8 system = FixtureSystem,
                     quint8 component = MAV_COMP_ID_LOG, bool wrongDestination = false) {
        mavlink_remote_log_data_block_t data{};
        data.seqno = sequence;
        data.target_system = wrongDestination ? quint8(remoteGcsSystem - 1) : remoteGcsSystem;
        data.target_component = remoteGcsComponent;
        const auto bytes = remoteBlockBytes(sequence);
        std::memcpy(data.data, bytes.constData(), 200);
        mavlink_message_t message{};
        mavlink_msg_remote_log_data_block_encode(system, component, &message, &data);
        inject(message);
    }
    void writeBytes(const char *bytes, qint64 size) override {
        mavlink_message_t message{};
        for (qint64 i = 0; i < size; ++i) {
            if (parser.parseByte(quint8(bytes[i]), &message) != MAVLINK_FRAMING_OK) continue;
            if (message.msgid == MAVLINK_MSG_ID_REMOTE_LOG_BLOCK_STATUS) {
                mavlink_remote_log_block_status_t status{};
                mavlink_msg_remote_log_block_status_decode(&message, &status);
                remoteControls.append(status);
                remoteGcsSystem = message.sysid;
                remoteGcsComponent = message.compid;
                if (status.seqno == MAV_REMOTE_LOG_DATA_BLOCK_START) {
                    QTimer::singleShot(0, this, [this] {
                        remoteBlock(77, FixtureSystem, 1);
                        remoteBlock(78, FixtureSystem - 1, MAV_COMP_ID_LOG);
                        remoteBlock(79, FixtureSystem, MAV_COMP_ID_LOG, true);
                        remoteBlock(1);
                        remoteBlock(0);
                        remoteBlock(2); // Zero-tail trimmed MAVLink2 payload.
                    });
                } else if (status.seqno == 1 && !remoteDuplicateSent) {
                    remoteDuplicateSent = true;
                    // Simulate a lost ACK only after the first durable receipt;
                    // retransmissions still pending storage are coalesced.
                    QTimer::singleShot(0, this, [this] { remoteBlock(1); });
                }
                continue;
            }
            if (message.msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL) {
                handleFtpRequest(message);
                continue;
            }
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
                if (magFitMode) {
                    int index = 0;
                    for (const auto &name : magFitValues.keys())
                        magFitReply(name, static_cast<quint16>(index++));
                } else pressureReply();
            }
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
                mavlink_param_request_read_t request{};
                mavlink_msg_param_request_read_decode(&message, &request);
                if (request.target_system != FixtureSystem
                    || request.target_component != 1) continue;
                const QByteArray raw(request.param_id, 16);
                const QString name = QString::fromLatin1(
                    raw.constData(), raw.indexOf('\0') < 0 ? 16 : raw.indexOf('\0'));
                if (magFitMode) {
                    magFitReply(name);
                    continue;
                }
                if (name == QStringLiteral("GND_ABS_PRESS")) pressureReply();
                else {
                    ++recoveryReads;
                    recoveryReply(name);
                }
            }
            if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
                mavlink_param_set_t value{};
                mavlink_msg_param_set_decode(&message, &value);
                if (value.target_system != FixtureSystem || value.target_component != 1) continue;
                const QByteArray raw(value.param_id, 16);
                const QString name = QString::fromLatin1(
                    raw.constData(), raw.indexOf('\0') < 0 ? 16 : raw.indexOf('\0'));
                if (magFitMode) {
                    if (magFitValues.contains(name)) {
                        magFitWrites.append(qMakePair(name, value.param_value));
                        magFitValues[name] = value.param_value;
                        magFitReply(name);
                    }
                    continue;
                }
                if (recoveryValues.contains(name)) {
                    recoveryWrites.append(qMakePair(name, value.param_value));
                    recoveryValues[name] = value.param_value;
                    if (!suppressRecoveryWriteEcho) recoveryReply(name);
                    if (recoveryWriteHook) recoveryWriteHook();
                    continue;
                }
                if (std::memcmp(value.param_id, "GND_ABS_PRESS", 13) != 0) continue;
                ++parameterWrites;
                pressure = value.param_value;
                pressureReply();
            }
            if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
                mavlink_command_long_t command{};
                mavlink_msg_command_long_decode(&message, &command);
                if (command.target_system != FixtureSystem || command.target_component != 1) continue;
                if (command.command == MAV_CMD_PREFLIGHT_CALIBRATION
                    || command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN
                    || command.command == MAV_CMD_FLASH_BOOTLOADER)
                    commands.append(command);
                mavlink_command_ack_t ack{};
                ack.command = command.command;
                ack.result = MAV_RESULT_ACCEPTED;
                ack.target_system = message.sysid;
                ack.target_component = message.compid;
                mavlink_message_t reply{};
                mavlink_msg_command_ack_encode(FixtureSystem, 1, &reply, &ack);
                inject(reply);
            }
        }
    }
    int parameterWrites = 0;
    float pressure = 101325.0f;
    QVector<mavlink_command_long_t> commands;
    QMap<QString, float> recoveryValues{
        {QStringLiteral("RECOVERY_ENABLE"), 0.0f},
        {QStringLiteral("RECOVERY_GAIN"), 7.0f},
        {QStringLiteral("COMPASS_DEV_ID"), 101.0f},
        {QStringLiteral("UNCHANGED"), 9.0f},
        {QStringLiteral("CANCEL_VALUE"), 0.0f}};
    QVector<QPair<QString, float>> recoveryWrites;
    int recoveryReads = 0;
    bool suppressRecoveryWriteEcho = false;
    bool magFitMode = false;
    QVector<mavlink_remote_log_block_status_t> remoteControls;
    bool remoteDuplicateSent = false;
    quint8 remoteGcsSystem = 0, remoteGcsComponent = 0;
    QMap<QString, float> magFitValues = magFitParameters();
    QVector<QPair<QString, float>> magFitWrites;
    std::function<void()> recoveryWriteHook;
    QByteArray ftpFileData;
    QString ftpRemotePath;
    QVector<MavFtpProtocol::Opcode> ftpOpcodes;
    int ftpRequestCount = 0;
    int destructiveFtpRequests = 0;
    quint8 ftpGcsSystem = 0;
    quint8 ftpGcsComponent = 0;
    bool ftpEnvelopeValid = true;
    bool ftpWrongResponsesInjected = false;
    bool ftpWrongResponsesRejected = true;
private:
    MAVLinkFrameParser parser;
    bool m_connected = false;
};
}

int RunDeveloperVehicleToolRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *description) {
        if (!condition) { ++failures; qCritical() << "Developer runtime:" << description; }
    };
    auto *links = LinkManager::instance();
    auto *window = MainWindow::instance();
    auto *service = links->developerVehicleToolService();
    auto *action = window->findChild<QAction *>(QStringLiteral("actionDeveloperTools"));
    expect(service && action, "application service or Tools action missing");
    if (!service || !action) return 1;
    action->trigger();
    QCoreApplication::processEvents();
    QPointer<ConfigDeveloperToolsView> page(window->findChild<ConfigDeveloperToolsView *>());
    expect(page && page->ImplementedActionCount() == 23 && page->ActionCount() == 32,
           "production Developer route did not bind offline and vehicle tools");
    if (!page) return 1;
    auto *offlineMagFit = page->findChild<QPushButton *>(QStringLiteral("OfflineMagFitButton"));
    expect(offlineMagFit && offlineMagFit->isEnabled(), "offline MagFit route is unavailable");
    if (offlineMagFit) {
        offlineMagFit->click();
        QCoreApplication::processEvents();
        const QPointer<OfflineMagFitWindow> magFit(
            window->findChild<OfflineMagFitWindow *>());
        expect(magFit && magFit->isVisible() && magFit->isWindow(),
               "Developer MagFit did not display a real modeless window offline");
        if (magFit) magFit->close();
    }
    auto *reboot = page->findChild<QPushButton *>(QStringLiteral("RebootVehicleButton"));
    expect(reboot && !reboot->isEnabled(), "offline reboot was enabled");
    auto *bootloader = page->findChild<QPushButton *>(
        QStringLiteral("UpgradeBootloaderButton"));
    expect(bootloader && !bootloader->isEnabled(),
           "offline bootloader upgrade was enabled");
    auto *mavFtpDownload = page->findChild<QPushButton *>(
        QStringLiteral("DownloadMavftpFileButton"));
    expect(mavFtpDownload && mavFtpDownload->isEnabled(),
           "direct MAVFTP download controller is unavailable offline");
    if (mavFtpDownload) {
        const QString logBefore = page->Log();
        mavFtpDownload->click();
        QCoreApplication::processEvents();
        expect(page->Log() != logBefore
                   && page->Log().contains(QStringLiteral("MAVFTP"),
                                           Qt::CaseInsensitive)
                   && !page->findChild<QInputDialog *>(
                       QStringLiteral("DeveloperMavFtpPathDialog")),
               "offline MAVFTP click did not report NoTarget before prompting");
    }

    // The offline action must work through its actual Tools-page file pickers,
    // before any vehicle is discovered. The expected bytes are deliberately
    // independent of the extractor's implementation, including MAVLink2 zeros.
    QTemporaryDir gpsFiles;
    expect(gpsFiles.isValid(), "GPS fixture directory unavailable");
    const QString gpsInput = gpsFiles.filePath(QStringLiteral("input.tlog"));
    const QString gpsOutput = gpsFiles.filePath(QStringLiteral("input-corrections.dat"));
    QByteArray gpsLog;
    auto appendGpsRecord = [&](const mavlink_message_t &message) {
        const quint64 timestamp = 1700000000000000ULL;
        for (int shift = 56; shift >= 0; shift -= 8) gpsLog.append(char(timestamp >> shift));
        uint8_t wire[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(wire, &message);
        gpsLog.append(reinterpret_cast<const char *>(wire), size);
    };
    mavlink_gps_rtcm_data_t rtcm{};
    rtcm.len = 5;
    rtcm.data[0] = 0xd3; rtcm.data[1] = 0x11; rtcm.data[2] = 0x22;
    mavlink_message_t gpsMessage{};
    auto *fixtureChannel = mavlink_get_channel_status(MAVLINK_COMM_0);
    const quint8 savedFixtureFlags = fixtureChannel->flags;
    fixtureChannel->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_msg_gps_rtcm_data_encode(255, 190, &gpsMessage, &rtcm);
    expect(gpsMessage.magic == MAVLINK_STX && gpsMessage.len < MAVLINK_MSG_ID_GPS_RTCM_DATA_LEN,
           "GPS runtime fixture is not zero-trimmed MAVLink2");
    appendGpsRecord(gpsMessage);
    mavlink_gps_inject_data_t injected{};
    injected.target_system = 234; injected.target_component = 1;
    injected.len = 2; injected.data[0] = 0x33; injected.data[1] = 0x44;
    mavlink_msg_gps_inject_data_encode(42, 1, &gpsMessage, &injected);
    appendGpsRecord(gpsMessage);
    fixtureChannel->flags = savedFixtureFlags;
    QFile gpsSource(gpsInput);
    expect(gpsSource.open(QIODevice::WriteOnly) && gpsSource.write(gpsLog) == gpsLog.size(),
           "GPS fixture could not be written");
    gpsSource.close();
    auto *extract = page->findChild<QPushButton *>(QStringLiteral("ExtractGpsCorrectionsButton"));
    expect(extract && extract->isEnabled(), "offline GPS extraction action disabled");
    if (extract) {
        extract->click();
        QCoreApplication::processEvents();
        auto *picker = page->findChild<QFileDialog *>(QStringLiteral("DeveloperGpsInputDialog"));
        expect(picker != nullptr, "GPS input picker missing");
        if (picker) picker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(!QFile::exists(gpsOutput), "cancelling GPS input created output");

        extract->click();
        QCoreApplication::processEvents();
        picker = page->findChild<QFileDialog *>(QStringLiteral("DeveloperGpsInputDialog"));
        if (picker) {
            // Once visible, QFileDialog::selectFile may deliberately leave
            // its focused filename editor unchanged. Exercise actual typed
            // selection, including the spaces in QTemporaryDir's app name.
            auto *filename = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
            expect(filename != nullptr, "GPS input filename editor missing");
            if (filename) filename->setText(gpsInput);
            expect(picker->selectedFiles() == QStringList{gpsInput}, "GPS input selection is not exact");
            qInfo() << "Developer runtime GPS input selection:" << picker->selectedFiles();
            expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection),
                   "GPS input picker acceptance unavailable");
        }
        waitFor([&] { return page->findChild<QFileDialog *>(QStringLiteral("DeveloperGpsOutputDialog")) != nullptr; });
        auto *destination = page->findChild<QFileDialog *>(QStringLiteral("DeveloperGpsOutputDialog"));
        expect(destination != nullptr, "GPS output picker missing");
        if (destination) {
            auto *filename = destination->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
            expect(filename != nullptr, "GPS output filename editor missing");
            if (filename) filename->setText(gpsOutput);
            expect(destination->selectedFiles() == QStringList{gpsOutput}, "GPS output selection is not exact");
            QMetaObject::invokeMethod(destination, "accept", Qt::DirectConnection);
        }
        expect(waitFor([&] { return extract->isEnabled() && QFile::exists(gpsOutput); }),
               "GPS extraction did not finish through Tools route");
        QFile gpsResult(gpsOutput);
        expect(gpsResult.open(QIODevice::ReadOnly)
                   && gpsResult.readAll() == QByteArray::fromHex("d3112200003344"),
               "GPS extraction bytes/order/zero padding differ");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime GPS extraction:" << gpsOutput << page->Log();
    }

    // Split a real binary DataFlash stream through the production Developer
    // page. The first pass proves that both the count and the output set stay
    // behind a default-Cancel confirmation; the second verifies independently
    // that complete data records are neither lost nor duplicated and that each
    // part carries the metadata required to decode it on its own.
    QTemporaryDir splitFiles;
    expect(splitFiles.isValid(), "DataFlash split fixture directory unavailable");
    const QString splitInput = splitFiles.filePath(
        QStringLiteral("flight sample.bin"));
    const QStringList splitOutputs{
        splitInput + QStringLiteral("_split0.bin"),
        splitInput + QStringLiteral("_split1.bin")};
    const QByteArray splitFixture = splitDataFlashFixture();
    QFile splitSource(splitInput);
    expect(splitSource.open(QIODevice::WriteOnly)
               && splitSource.write(splitFixture) == splitFixture.size(),
           "DataFlash split fixture could not be written");
    splitSource.close();
    auto *split = page->findChild<QPushButton *>(
        QStringLiteral("SplitDataFlashLogButton"));
    expect(split && split->isEnabled(), "offline DataFlash split action disabled");
    const auto outputsExist = [&]() {
        return QFile::exists(splitOutputs.at(0))
            || QFile::exists(splitOutputs.at(1));
    };
    const auto openSplitConfirmation = [&]() -> QMessageBox * {
        if (!split || !split->isEnabled()) return nullptr;
        split->click();
        QCoreApplication::processEvents();
        auto *picker = page->findChild<QFileDialog *>(
            QStringLiteral("DeveloperSplitInputDialog"));
        expect(picker != nullptr, "DataFlash split input picker missing");
        if (!picker) return nullptr;
        const QString filters = picker->nameFilters().join(QLatin1Char(' '));
        expect(filters.contains(QStringLiteral("*.bin"))
                   && filters.contains(QStringLiteral("*.log")),
               "DataFlash split picker does not expose both binary and text logs");
        auto *filename = picker->findChild<QLineEdit *>(
            QStringLiteral("fileNameEdit"));
        expect(filename != nullptr, "DataFlash split filename editor missing");
        if (filename) filename->setText(splitInput);
        expect(picker->selectedFiles() == QStringList{splitInput},
               "DataFlash split input selection is not exact");
        expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection),
               "DataFlash split input picker acceptance unavailable");
        expect(waitFor([&] {
            return page->findChild<QInputDialog *>(
                QStringLiteral("DeveloperSplitCountDialog")) != nullptr;
        }), "DataFlash split count dialog missing");
        auto *count = page->findChild<QInputDialog *>(
            QStringLiteral("DeveloperSplitCountDialog"));
        if (!count) return nullptr;
        expect(count->inputMode() == QInputDialog::IntInput
                   && count->intValue() == 10
                   && count->intMinimum() == 2
                   && count->intMaximum() == 1000,
               "DataFlash split count defaults/bounds differ from MP10");
        count->setIntValue(2);
        count->accept();
        expect(waitFor([&] {
            return page->findChild<QMessageBox *>(
                QStringLiteral("DeveloperSplitConfirmDialog")) != nullptr;
        }), "DataFlash split confirmation missing");
        return page->findChild<QMessageBox *>(
            QStringLiteral("DeveloperSplitConfirmDialog"));
    };

    if (split) {
        QMessageBox *splitConfirmation = openSplitConfirmation();
        expect(splitConfirmation
                   && splitConfirmation->defaultButton()
                       == splitConfirmation->button(QMessageBox::Cancel)
                   && splitConfirmation->escapeButton()
                       == splitConfirmation->button(QMessageBox::Cancel),
               "DataFlash split is not protected by default/Escape Cancel");
        if (splitConfirmation)
            splitConfirmation->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return split->isEnabled(); }) && !outputsExist(),
               "cancelling DataFlash split confirmation published output");

        splitConfirmation = openSplitConfirmation();
        expect(splitConfirmation != nullptr,
               "second DataFlash split confirmation missing");
        if (splitConfirmation)
            splitConfirmation->button(QMessageBox::Yes)->click();
        auto *splitProgress = page->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperSplitProgressDialog"));
        expect(splitProgress != nullptr,
               "DataFlash split progress dialog missing after confirmation");
        expect(waitFor([&] {
            return split->isEnabled()
                && QFile::exists(splitOutputs.at(0))
                && QFile::exists(splitOutputs.at(1));
        }, 5000), "DataFlash split did not finish through Tools route");

        const SplitPiece first = readSplitPiece(splitOutputs.at(0));
        const SplitPiece second = readSplitPiece(splitOutputs.at(1));
        const QSet<QString> expectedDefinitions{
            QStringLiteral("FMT"), QStringLiteral("FMTU"),
            QStringLiteral("UNIT"), QStringLiteral("MULT"),
            QStringLiteral("DUMY")};
        const QSet<QString> expectedMetadata{
            QStringLiteral("FMTU"), QStringLiteral("UNIT"),
            QStringLiteral("MULT")};
        expect(first.valid && second.valid
                   && first.definitions == expectedDefinitions
                   && second.definitions == expectedDefinitions
                   && first.metadataRecords == expectedMetadata
                   && second.metadataRecords == expectedMetadata,
               "DataFlash split pieces are not independently decodable with full metadata");
        QVector<quint64> splitValues = first.values;
        splitValues += second.values;
        expect(!first.values.isEmpty() && !second.values.isEmpty()
                   && splitValues == QVector<quint64>{11, 22, 33, 44},
               "DataFlash split lost, duplicated, reordered, or fragmented data records");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime DataFlash split:" << splitOutputs
                << page->Log();
    }

    // Drive the complete DashWare workflow before a vehicle exists. The
    // fixture deliberately moves backwards in TimeUS and alternates message
    // types, proving that the exporter preserves raw log order and creates
    // sparse columns rather than carrying values between message types.
    QTemporaryDir dashWareFiles;
    expect(dashWareFiles.isValid(), "DashWare fixture directory unavailable");
    const QString dashWareInput = dashWareFiles.filePath(
        QStringLiteral("backwards samples.log"));
    const QString dashWareOutput = dashWareFiles.filePath(
        QStringLiteral("verified dashware.csv"));
    const QString dashWareSuggestedOutput = dashWareFiles.filePath(
        QStringLiteral("backwards samples-dashware.csv"));
    const QByteArray dashWareFixture(
        "FMT,150,15,TEST,Qf,TimeUS,Value\n"
        "FMT,151,12,AUX,QB,TimeUS,State\n"
        "TEST,2000,2.5\n"
        "AUX,1500,7\n"
        "TEST,1000,-3\n");
    QFile dashWareSource(dashWareInput);
    expect(dashWareSource.open(QIODevice::WriteOnly)
               && dashWareSource.write(dashWareFixture)
                   == dashWareFixture.size(),
           "DashWare fixture could not be written");
    dashWareSource.close();
    auto *dashWare = page->findChild<QPushButton *>(
        QStringLiteral("CreateDashWareCsvButton"));
    expect(dashWare && dashWare->isEnabled(),
           "offline DashWare export action disabled");

    const auto openDashWareTypes = [&]() -> QInputDialog * {
        if (!dashWare || !dashWare->isEnabled()) return nullptr;
        dashWare->click();
        QCoreApplication::processEvents();
        auto *picker = page->findChild<QFileDialog *>(
            QStringLiteral("DeveloperDashWareInputDialog"));
        expect(picker != nullptr, "DashWare input picker missing");
        if (!picker) return nullptr;
        const QString filters = picker->nameFilters().join(QLatin1Char(' '));
        expect(filters.contains(QStringLiteral("*.bin"))
                   && filters.contains(QStringLiteral("*.log")),
               "DashWare input picker does not expose binary and text logs");
        auto *filename = picker->findChild<QLineEdit *>(
            QStringLiteral("fileNameEdit"));
        expect(filename != nullptr, "DashWare input filename editor missing");
        if (filename) filename->setText(dashWareInput);
        expect(picker->selectedFiles() == QStringList{dashWareInput},
               "DashWare input selection is not exact");
        expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection),
               "DashWare input picker acceptance unavailable");
        expect(waitFor([&] {
            return page->findChild<QInputDialog *>(
                QStringLiteral("DeveloperDashWareTypesDialog")) != nullptr;
        }), "DashWare message-types dialog missing");
        auto *types = page->findChild<QInputDialog *>(
            QStringLiteral("DeveloperDashWareTypesDialog"));
        expect(types && types->inputMode() == QInputDialog::TextInput
                   && types->textValue()
                       == QStringLiteral("GPS;ATT;NTUN;CTUN;MODE;BAT"),
               "DashWare message-types default differs from MP10");
        return types;
    };

    const auto acceptDashWareTypes = [&](QInputDialog *types)
        -> QFileDialog * {
        if (!types) return nullptr;
        types->setTextValue(QStringLiteral("TEST;AUX"));
        types->accept();
        expect(waitFor([&] {
            return page->findChild<QFileDialog *>(
                QStringLiteral("DeveloperDashWareOutputDialog")) != nullptr;
        }), "DashWare output picker missing");
        auto *output = page->findChild<QFileDialog *>(
            QStringLiteral("DeveloperDashWareOutputDialog"));
        expect(output && output->acceptMode() == QFileDialog::AcceptSave,
               "DashWare output picker is not a Save dialog");
        if (output) {
            const QString suggested = QFileInfo(
                output->selectedFiles().value(0)).fileName();
            expect(suggested == QStringLiteral("backwards samples-dashware.csv"),
                   "DashWare output suggestion differs from MP10");
        }
        return output;
    };

    if (dashWare) {
        // Cancel once at the type-selection boundary.
        QInputDialog *types = openDashWareTypes();
        if (types) types->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return dashWare->isEnabled(); })
                   && !QFile::exists(dashWareSuggestedOutput)
                   && !QFile::exists(dashWareOutput),
               "cancelling DashWare type selection published output");

        // Cancel once at the destination boundary as well.
        types = openDashWareTypes();
        QFileDialog *output = acceptDashWareTypes(types);
        if (output) output->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return dashWare->isEnabled(); })
                   && !QFile::exists(dashWareSuggestedOutput)
                   && !QFile::exists(dashWareOutput),
               "cancelling DashWare output selection published output");

        // Complete the same production route and compare deterministic bytes.
        types = openDashWareTypes();
        output = acceptDashWareTypes(types);
        if (output) {
            auto *filename = output->findChild<QLineEdit *>(
                QStringLiteral("fileNameEdit"));
            expect(filename != nullptr,
                   "DashWare output filename editor missing");
            if (filename) filename->setText(dashWareOutput);
            expect(output->selectedFiles() == QStringList{dashWareOutput},
                   "DashWare output selection is not exact");
            expect(QMetaObject::invokeMethod(output, "accept",
                                             Qt::DirectConnection),
                   "DashWare output picker acceptance unavailable");
        }
        auto *dashWareProgress = page->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperDashWareProgressDialog"));
        expect(dashWareProgress != nullptr,
               "DashWare progress dialog missing after destination acceptance");
        expect(waitFor([&] {
            return dashWare->isEnabled() && QFile::exists(dashWareOutput);
        }, 5000), "DashWare export did not finish through Tools route");
        QFile dashWareResult(dashWareOutput);
        const QByteArray expectedDashWareCsv(
            "GLOBAL_TimeMS,TEST_TimeUS,TEST_Value,AUX_TimeUS,AUX_State,\n"
            "2,2000,2.5,,,\n"
            "1.5,,,1500,7,\n"
            "1,1000,-3,,,\n");
        expect(dashWareResult.open(QIODevice::ReadOnly)
                   && dashWareResult.readAll() == expectedDashWareCsv,
               "DashWare CSV header, sparse columns, time order, or trailing commas differ");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime DashWare export:" << dashWareOutput
                << page->Log();
    }

    // Embed a real parameter-defaults payload through the production Tools
    // page while no vehicle target exists. The fixture is an APJ JSON envelope
    // around a standard zlib stream (Qt's four-byte qCompress prefix removed),
    // with the same packed PARMDEF header used by ArduPilot firmware.
    QTemporaryDir apjFiles;
    expect(apjFiles.isValid(), "APJ fixture directory unavailable");
    const QString apjFirmware = apjFiles.filePath(
        QStringLiteral("runtime firmware.apj"));
    const QString apjParameters = apjFiles.filePath(
        QStringLiteral("defaults CRLF.param"));
    const QString apjOutput = apjFirmware + QStringLiteral("new.apj");
    constexpr int defaultsOffset = 48;
    constexpr quint16 maximumDefaults = 96;
    const QByteArray oldDefaults("OLD_DEFAULTS_REMAINDER,9999\n");
    const QByteArray parameterBytes("FOO,1\r\nBAR,2\r\n");
    const QByteArray embeddedDefaults("FOO,1\nBAR,2\n");
    QByteArray apjImage(256, char(0x6d));
    apjImage.replace(defaultsOffset, 8,
                     QByteArray("PARMDEF\0", 8));
    apjImage.replace(defaultsOffset + 8, 8,
                     QByteArray::fromHex("5537f4a0385d485b"));
    qToLittleEndian<quint16>(
        maximumDefaults,
        reinterpret_cast<uchar *>(apjImage.data() + defaultsOffset + 16));
    qToLittleEndian<quint16>(
        static_cast<quint16>(oldDefaults.size()),
        reinterpret_cast<uchar *>(apjImage.data() + defaultsOffset + 18));
    std::memset(apjImage.data() + defaultsOffset + 20, 0,
                maximumDefaults);
    std::memcpy(apjImage.data() + defaultsOffset + 20,
                oldDefaults.constData(), size_t(oldDefaults.size()));
    const QByteArray apjCompressed = apjZlibStream(apjImage);
    expect(!apjCompressed.isEmpty(), "APJ fixture zlib stream is empty");
    const QJsonObject unknownMetadata{
        {QStringLiteral("keep"), QStringLiteral("unchanged")},
        {QStringLiteral("answer"), 42}};
    QJsonObject apjEnvelope{
        {QStringLiteral("magic"), QStringLiteral("APJFWv1")},
        {QStringLiteral("board_id"), 777},
        {QStringLiteral("description"), QStringLiteral("runtime fixture")},
        {QStringLiteral("image"),
         QString::fromLatin1(apjCompressed.toBase64())},
        {QStringLiteral("image_size"), apjImage.size()},
        {QStringLiteral("flash_total"), 4096},
        {QStringLiteral("flash_free"), 4096 - apjImage.size()},
        {QStringLiteral("unknown_metadata"), unknownMetadata}};
    const QByteArray apjFirmwareBytes =
        QJsonDocument(apjEnvelope).toJson(QJsonDocument::Indented);
    QFile apjSource(apjFirmware);
    expect(apjSource.open(QIODevice::WriteOnly)
               && apjSource.write(apjFirmwareBytes)
                   == apjFirmwareBytes.size(),
           "APJ firmware fixture could not be written");
    apjSource.close();
    QFile parameterSource(apjParameters);
    expect(parameterSource.open(QIODevice::WriteOnly)
               && parameterSource.write(parameterBytes)
                   == parameterBytes.size(),
           "APJ parameter fixture could not be written");
    parameterSource.close();
    const QByteArray firmwareHash = QCryptographicHash::hash(
        apjFirmwareBytes, QCryptographicHash::Sha256);
    const QByteArray parameterHash = QCryptographicHash::hash(
        parameterBytes, QCryptographicHash::Sha256);
    const quint64 apjTargetGeneration =
        links->vehicleTargetManager()->targetGeneration();
    expect(!links->vehicleTargetManager()->acquireTarget().isValid(),
           "APJ offline audit unexpectedly has a vehicle target");

    auto *embedApj = page->findChild<QPushButton *>(
        QStringLiteral("EmbedDefaultsInApjButton"));
    expect(embedApj && embedApj->isEnabled(),
           "offline APJ defaults action disabled");
    const auto visibleApjFileDialog = [&](const QString &objectName)
        -> QFileDialog * {
        if (!page) return nullptr;
        const auto dialogs = page->findChildren<QFileDialog *>(objectName);
        for (QFileDialog *dialog : dialogs) {
            if (dialog && dialog->isVisible()) return dialog;
        }
        return nullptr;
    };
    const auto openApjFirmwarePicker = [&]() -> QFileDialog * {
        if (!embedApj || !embedApj->isEnabled()) return nullptr;
        embedApj->click();
        QCoreApplication::processEvents();
        auto *picker = visibleApjFileDialog(
            QStringLiteral("DeveloperApjFirmwareDialog"));
        expect(picker != nullptr, "APJ firmware picker missing");
        if (picker) {
            const QString filters = picker->nameFilters().join(
                QLatin1Char(' '));
            expect(filters.contains(QStringLiteral("*.apj")),
                   "APJ firmware picker does not filter APJ files");
        }
        return picker;
    };
    const auto acceptApjFirmware = [&](QFileDialog *picker)
        -> QFileDialog * {
        if (!picker) return nullptr;
        auto *filename = picker->findChild<QLineEdit *>(
            QStringLiteral("fileNameEdit"));
        expect(filename != nullptr, "APJ firmware filename editor missing");
        if (filename) filename->setText(apjFirmware);
        expect(picker->selectedFiles() == QStringList{apjFirmware},
               "APJ firmware selection is not exact");
        expect(QMetaObject::invokeMethod(picker, "accept",
                                         Qt::DirectConnection),
               "APJ firmware picker acceptance unavailable");
        expect(waitFor([&] {
            return visibleApjFileDialog(
                QStringLiteral("DeveloperApjDefaultsDialog")) != nullptr;
        }), "APJ defaults picker missing");
        auto *defaults = visibleApjFileDialog(
            QStringLiteral("DeveloperApjDefaultsDialog"));
        if (defaults) {
            const QString filters = defaults->nameFilters().join(
                QLatin1Char(' '));
            expect(filters.contains(QStringLiteral("*.param"))
                       && filters.contains(QStringLiteral("*.parm")),
                   "APJ defaults picker does not expose param/parm files");
        }
        return defaults;
    };
    const auto acceptApjDefaults = [&](QFileDialog *picker) {
        if (!picker) return false;
        auto *filename = picker->findChild<QLineEdit *>(
            QStringLiteral("fileNameEdit"));
        expect(filename != nullptr, "APJ defaults filename editor missing");
        if (filename) filename->setText(apjParameters);
        expect(picker->selectedFiles() == QStringList{apjParameters},
               "APJ defaults selection is not exact");
        return QMetaObject::invokeMethod(picker, "accept",
                                         Qt::DirectConnection);
    };

    if (embedApj) {
        // Cancel each picker boundary once; neither may publish an output.
        QFileDialog *firmwarePicker = openApjFirmwarePicker();
        if (firmwarePicker) firmwarePicker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return embedApj->isEnabled(); })
                   && !QFile::exists(apjOutput),
               "cancelling APJ firmware selection published output");

        firmwarePicker = openApjFirmwarePicker();
        QFileDialog *defaultsPicker = acceptApjFirmware(firmwarePicker);
        if (defaultsPicker) defaultsPicker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return embedApj->isEnabled(); })
                   && !QFile::exists(apjOutput),
               "cancelling APJ defaults selection published output");

        // Complete the modeless workflow once with no overwrite involved.
        firmwarePicker = openApjFirmwarePicker();
        defaultsPicker = acceptApjFirmware(firmwarePicker);
        expect(acceptApjDefaults(defaultsPicker),
               "APJ defaults picker acceptance unavailable");
        auto *apjProgress = page->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperApjProgressDialog"));
        expect(apjProgress != nullptr,
               "APJ embedding progress dialog missing");
        expect(waitFor([&] {
            return embedApj->isEnabled() && QFile::exists(apjOutput);
        }, 5000), "APJ embedding did not finish through Tools route");

        const QByteArray outputBytes = readFileBytes(apjOutput);
        const QJsonDocument outputDocument =
            QJsonDocument::fromJson(outputBytes);
        const QJsonObject outputEnvelope = outputDocument.object();
        const quint32 outputImageSize = static_cast<quint32>(
            outputEnvelope.value(QStringLiteral("image_size")).toInt());
        const QByteArray outputCompressed = QByteArray::fromBase64(
            outputEnvelope.value(QStringLiteral("image"))
                .toString().toLatin1());
        const QByteArray outputImage = inflateApjImage(
            outputCompressed, outputImageSize);
        expect(!outputDocument.isNull() && outputDocument.isObject()
                   && outputImageSize == quint32(apjImage.size())
                   && outputImage.size() == apjImage.size(),
               "APJ output JSON/image_size/zlib stream is invalid");
        expect(outputEnvelope.value(QStringLiteral("board_id")).toInt()
                       == 777
                   && outputEnvelope.value(
                          QStringLiteral("unknown_metadata")).toObject()
                       == unknownMetadata,
               "APJ output did not preserve unknown metadata");
        const bool outputBoundsValid = outputImage.size()
            >= defaultsOffset + 20 + maximumDefaults;
        expect(outputBoundsValid,
               "APJ output no longer contains the defaults reservation");
        if (outputBoundsValid) {
            const quint16 storedMaximum = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(
                    outputImage.constData() + defaultsOffset + 16));
            const quint16 storedLength = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(
                    outputImage.constData() + defaultsOffset + 18));
            expect(storedMaximum == maximumDefaults
                       && storedLength == embeddedDefaults.size()
                       && outputImage.mid(defaultsOffset + 20, storedLength)
                           == embeddedDefaults,
                   "APJ embedded defaults length/data or CR removal differs");
            expect(outputImage.left(defaultsOffset + 18)
                           == apjImage.left(defaultsOffset + 18)
                       && outputImage.mid(defaultsOffset + 20 + maximumDefaults)
                           == apjImage.mid(defaultsOffset + 20
                                          + maximumDefaults),
                   "APJ embedding changed bytes outside the packed defaults fields");
            // MP10 changes only the active prefix and its length. The unused
            // reservation is intentionally byte-preserved for compatibility.
            expect(outputImage.mid(defaultsOffset + 20
                                       + embeddedDefaults.size(),
                                   maximumDefaults
                                       - embeddedDefaults.size())
                           == apjImage.mid(defaultsOffset + 20
                                              + embeddedDefaults.size(),
                                          maximumDefaults
                                              - embeddedDefaults.size()),
                   "APJ embedding changed the inactive defaults reservation");
        }
        expect(outputImage != apjImage,
               "APJ embedding did not change the firmware image");
        expect(QCryptographicHash::hash(readFileBytes(apjFirmware),
                                        QCryptographicHash::Sha256)
                       == firmwareHash
                   && QCryptographicHash::hash(
                          readFileBytes(apjParameters),
                          QCryptographicHash::Sha256) == parameterHash,
               "APJ embedding modified a source file");
        expect(!links->vehicleTargetManager()->acquireTarget().isValid()
                   && links->vehicleTargetManager()->targetGeneration()
                       == apjTargetGeneration,
               "offline APJ workflow changed vehicle target state");
        // Manual processEvents loops do not guarantee delivery of every
        // deferred delete posted by accepted modeless dialogs. Remove the
        // completed pickers/progress dialog before looking up the next
        // same-named workflow so the audit cannot drive a hidden stale child.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        // A pre-existing output is protected by another explicit default-Cancel
        // boundary, and rejecting it must preserve the published file exactly.
        const QByteArray publishedHash = QCryptographicHash::hash(
            outputBytes, QCryptographicHash::Sha256);
        firmwarePicker = openApjFirmwarePicker();
        defaultsPicker = acceptApjFirmware(firmwarePicker);
        expect(acceptApjDefaults(defaultsPicker),
               "second APJ defaults picker acceptance unavailable");
        expect(waitFor([&] {
            const auto dialogs = page->findChildren<QMessageBox *>(
                QStringLiteral("DeveloperApjOverwriteConfirmDialog"));
            return std::any_of(dialogs.cbegin(), dialogs.cend(),
                               [](QMessageBox *dialog) {
                return dialog && dialog->isVisible();
            });
        }), "APJ overwrite confirmation missing");
        QMessageBox *overwrite = nullptr;
        const auto overwriteDialogs = page->findChildren<QMessageBox *>(
            QStringLiteral("DeveloperApjOverwriteConfirmDialog"));
        for (QMessageBox *dialog : overwriteDialogs) {
            if (dialog && dialog->isVisible()) {
                overwrite = dialog;
                break;
            }
        }
        expect(overwrite
                   && overwrite->defaultButton()
                       == overwrite->button(QMessageBox::Cancel)
                   && overwrite->escapeButton()
                       == overwrite->button(QMessageBox::Cancel)
                   && overwrite->text().contains(apjOutput),
               "APJ overwrite confirmation is not named/path-specific/default-Cancel");
        if (overwrite) overwrite->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return embedApj->isEnabled(); })
                   && QCryptographicHash::hash(
                          readFileBytes(apjOutput),
                          QCryptographicHash::Sha256) == publishedHash,
               "cancelling APJ overwrite changed the existing output");
        qInfo() << "Developer runtime APJ defaults embedding:" << apjOutput
                << page->Log();
    }

    // Exercise the real directory picker and immutable plan consent entirely
    // offline. Only this temporary fixture may be moved or have empty logs
    // deleted; the network SITL and user log directories are never selected.
    QTemporaryDir organizerFiles;
    expect(organizerFiles.isValid(), "organizer fixture directory unavailable");
    const QString organizerRoot = organizerFiles.path();
    expect(QDir(organizerRoot).mkdir(QStringLiteral("incoming")),
           "organizer input directory could not be created");
    const QString smallLog = organizerFiles.filePath("incoming/small.log");
    const QString companion = organizerFiles.filePath("incoming/small.log.param");
    const QString emptyLog = organizerFiles.filePath("incoming/empty.bin");
    const QString untouched = organizerFiles.filePath("incoming/unrelated.txt");
    const QByteArray smallBytes("offline small log\n");
    const QByteArray companionBytes("preserve companion bytes\n");
    const QByteArray untouchedBytes("not a log or matching companion\n");
    const auto writeOrganizerFixture = [&](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly)
            && file.write(bytes) == bytes.size();
        expect(written, "runtime fixture file could not be written");
        return written;
    };
    writeOrganizerFixture(smallLog, smallBytes);
    writeOrganizerFixture(companion, companionBytes);
    writeOrganizerFixture(emptyLog, {});
    writeOrganizerFixture(untouched, untouchedBytes);
    const QString movedLog = organizerFiles.filePath("SMALL/small.log");
    const QString movedCompanion = organizerFiles.filePath("SMALL/small.log.param");
    const quint64 organizerGeneration = links->vehicleTargetManager()->targetGeneration();
    auto *organize = page->findChild<QPushButton *>("OrganizeLogDirectoryButton");
    expect(organize && organize->isEnabled(), "offline log organizer action disabled");
    const auto organizerSourcesIntact = [&] {
        return readFileBytes(smallLog) == smallBytes
            && readFileBytes(companion) == companionBytes
            && QFile::exists(emptyLog) && QFileInfo(emptyLog).size() == 0
            && readFileBytes(untouched) == untouchedBytes
            && !QFile::exists(movedLog) && !QFile::exists(movedCompanion);
    };
    const auto visibleOrganizerDialog = [&](const QString &name) -> QDialog * {
        if (!page) return nullptr;
        const auto dialogs = page->findChildren<QDialog *>(name);
        for (auto *dialog : dialogs)
            if (dialog->isVisible()) return dialog;
        return nullptr;
    };
    const auto openOrganizerPicker = [&]() -> QFileDialog * {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!organize || !organize->isEnabled()) return nullptr;
        organize->click();
        QCoreApplication::processEvents();
        auto *picker = qobject_cast<QFileDialog *>(visibleOrganizerDialog(
            QStringLiteral("DeveloperLogOrganizerDirectoryDialog")));
        expect(picker && picker->fileMode() == QFileDialog::Directory,
               "organizer actual directory picker missing");
        return picker;
    };
    const auto acceptOrganizerDirectory = [&](QFileDialog *picker) -> QDialog * {
        if (!picker) return nullptr;
        auto *filename = picker->findChild<QLineEdit *>("fileNameEdit");
        expect(filename != nullptr, "organizer directory editor missing");
        if (filename) filename->setText(organizerRoot);
        expect(picker->selectedFiles() == QStringList{organizerRoot},
               "organizer selection did not name the exact fixture root");
        expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection),
               "organizer directory picker cannot be accepted");
        expect(waitFor([&] {
            return visibleOrganizerDialog("DeveloperLogOrganizerPlanDialog") != nullptr;
        }, 5000), "organizer analysis did not produce a visible plan");
        return visibleOrganizerDialog("DeveloperLogOrganizerPlanDialog");
    };
    if (organize) {
        QFileDialog *picker = openOrganizerPicker();
        if (picker) picker->reject();
        expect(waitFor([&] { return organize->isEnabled(); }) && organizerSourcesIntact(),
               "organizer directory cancellation changed source files");
        QDialog *plan = acceptOrganizerDirectory(openOrganizerPicker());
        if (plan) {
            auto *cancel = plan->findChild<QPushButton *>("DeveloperLogOrganizerCancelButton");
            auto *execute = plan->findChild<QPushButton *>("DeveloperLogOrganizerExecuteButton");
            auto *tree = plan->findChild<QTreeWidget *>("DeveloperLogOrganizerPlanTree");
            expect(cancel && cancel->isDefault() && execute && execute->isEnabled()
                       && tree && tree->topLevelItemCount() == 3,
                   "organizer plan does not show all files/default Cancel");
            QString renderedPlan;
            QSet<QString> plannedSources;
            QSet<QString> plannedDestinations;
            if (tree) {
                for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                    const auto *item = tree->topLevelItem(row);
                    plannedSources.insert(item->data(1, Qt::UserRole).toString());
                    const QString destination = item->data(2, Qt::UserRole).toString();
                    if (!destination.isEmpty()) plannedDestinations.insert(destination);
                    for (int column = 0; column < tree->columnCount(); ++column)
                        renderedPlan += item->text(column) + '\n';
                }
            }
            expect(plannedSources == QSet<QString>{smallLog, companion, emptyLog}
                       && plannedDestinations == QSet<QString>{movedLog, movedCompanion},
                   "organizer plan does not retain exact absolute operation paths");
            expect(renderedPlan.contains("incoming/small.log")
                       && renderedPlan.contains("incoming/small.log.param")
                       && renderedPlan.contains("incoming/empty.bin")
                       && renderedPlan.contains("SMALL/small.log")
                       && renderedPlan.contains("SMALL/small.log.param"),
                   "organizer consent omits an exact source or destination");
            const QString screenshot = qEnvironmentVariable(
                "APM_ORGANIZER_AUDIT_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                expect(plan->grab().save(screenshot),
                       "organizer plan screenshot could not be saved");
            }
            expect(organizerSourcesIntact(), "read-only organizer plan mutated files");
            plan->reject(); // Same QDialog rejection boundary as Escape.
        }
        expect(waitFor([&] { return organize->isEnabled(); }) && organizerSourcesIntact(),
               "organizer plan cancellation changed files or retained the busy gate");
        plan = acceptOrganizerDirectory(openOrganizerPicker());
        if (plan) {
            auto *execute = plan->findChild<QPushButton *>("DeveloperLogOrganizerExecuteButton");
            expect(execute && execute->isEnabled(), "organizer Execute button missing");
            if (execute) execute->click();
        }
        expect(waitFor([&] {
            return organize->isEnabled() && QFile::exists(movedLog);
        }, 5000), "organizer execution did not finish through actual Tools route");
        expect(!QFile::exists(smallLog) && !QFile::exists(companion)
                   && !QFile::exists(emptyLog) && readFileBytes(movedLog) == smallBytes
                   && readFileBytes(movedCompanion) == companionBytes
                   && readFileBytes(untouched) == untouchedBytes,
               "organizer move/delete output differs or unrelated bytes changed");
        expect(!links->vehicleTargetManager()->acquireTarget().isValid()
                   && links->vehicleTargetManager()->targetGeneration() == organizerGeneration,
               "offline organizer changed the vehicle target");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime log organizer passed:" << organizerRoot << page->Log();
    }

    QPointer<DeveloperAuditLink> fixture(new DeveloperAuditLink);
    LinkManager::ConnectionProfile profile;
    profile.id = QStringLiteral("f38d827e-5849-42b4-83b0-49c1d158ab37");
    LinkManagerFactory::connectLinkSignals(fixture, links);
    links->addLink(fixture, profile);
    expect(links->connectLink(FixtureLinkId), "fixture connect failed");
    fixture->heartbeat();
    expect(waitFor([&] { return links->vehicleTargetManager()->contains(FixtureLinkId, FixtureSystem, 1); }),
           "heartbeat did not discover exact fixture");
    links->vehicleTargetManager()->selectTarget(FixtureLinkId, FixtureSystem, 1);
    bool fixtureArmed = false;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, fixture, [fixture, &fixtureArmed] {
        if (fixture) fixture->heartbeat(fixtureArmed);
    });
    heartbeat.start(250);
    fixture->pressureReply();
    expect(waitFor([&] { return service->canPrepare(DeveloperVehicleToolService::Action::SetQnh); }),
           "exact pressure snapshot did not become ready");
    // A complete list is readable before the classic PARAM traffic isolation
    // window expires. Wait for actual lane admission without weakening that
    // production fence or transmitting a parameter operation.
    QObject admissionProbe;
    expect(waitFor([&] {
        DeveloperVehicleToolService::Plan plan;
        if (!service->prepare(DeveloperVehicleToolService::Action::SetQnh, &plan)) return false;
        ParameterService::ExactReservationToken reservation;
        auto *parameters = links->parameterService();
        if (parameters->reserveSingleVehicleEndpoint(&admissionProbe, plan.target, plan.vehicle,
                &reservation) != ParameterService::ExactReservationResult::Reserved) return false;
        return parameters->releaseExactReservation(reservation);
    }, ParameterService::DefaultExactWriteQuarantineMs + 2500),
           "initial parameter traffic did not drain");
    action->trigger();
    QCoreApplication::processEvents();
    page = window->findChild<ConfigDeveloperToolsView *>();
    mavFtpDownload = page ? page->findChild<QPushButton *>(
        QStringLiteral("DownloadMavftpFileButton")) : nullptr;
    expect(mavFtpDownload && waitFor([&] {
               return mavFtpDownload && mavFtpDownload->isEnabled();
           }), "connected direct MAVFTP download action disabled");
    QTemporaryDir mavFtpFiles;
    expect(mavFtpFiles.isValid(), "MAVFTP output directory unavailable");
    const QString remotePath = QStringLiteral("@SYS/threads.txt");
    const QString localPath = mavFtpFiles.filePath(
        QStringLiteral("downloaded threads copy.bin"));
    const int ftpBeforePrompts = fixture ? fixture->ftpRequestCount : -1;
    const auto openMavFtpPath = [&]() -> QInputDialog * {
        if (!mavFtpDownload || !mavFtpDownload->isEnabled())
            return nullptr;
        mavFtpDownload->click();
        QCoreApplication::processEvents();
        auto *path = page->findChild<QInputDialog *>(
            QStringLiteral("DeveloperMavFtpPathDialog"));
        expect(path && path->inputMode() == QInputDialog::TextInput
                   && path->textValue() == remotePath,
               "MAVFTP remote-path prompt/default missing");
        return path;
    };
    const auto acceptMavFtpPath = [&](QInputDialog *path) -> QFileDialog * {
        if (!path) return nullptr;
        path->setTextValue(remotePath);
        path->accept();
        expect(waitFor([&] {
            return page->findChild<QFileDialog *>(
                QStringLiteral("DeveloperMavFtpOutputDialog")) != nullptr;
        }), "MAVFTP output picker missing");
        auto *output = page->findChild<QFileDialog *>(
            QStringLiteral("DeveloperMavFtpOutputDialog"));
        expect(output && output->acceptMode() == QFileDialog::AcceptSave
                   && QFileInfo(output->selectedFiles().value(0)).fileName()
                       == QStringLiteral("threads.txt"),
               "MAVFTP output picker/default filename mismatch");
        return output;
    };
    if (mavFtpDownload && fixture) {
        QInputDialog *path = openMavFtpPath();
        if (path) path->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return mavFtpDownload->isEnabled(); })
                   && fixture->ftpRequestCount == ftpBeforePrompts,
               "cancelling MAVFTP path prompt transmitted a request");

        path = openMavFtpPath();
        QFileDialog *output = acceptMavFtpPath(path);
        if (output) output->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return mavFtpDownload->isEnabled(); })
                   && fixture->ftpRequestCount == ftpBeforePrompts
                   && !QFile::exists(localPath),
               "cancelling MAVFTP output picker transmitted or wrote data");

        path = openMavFtpPath();
        output = acceptMavFtpPath(path);
        if (output) {
            auto *filename = output->findChild<QLineEdit *>(
                QStringLiteral("fileNameEdit"));
            expect(filename != nullptr,
                   "MAVFTP output filename editor missing");
            if (filename) filename->setText(localPath);
            expect(output->selectedFiles() == QStringList{localPath},
                   "MAVFTP output selection is not exact");
            expect(QMetaObject::invokeMethod(output, "accept",
                                             Qt::DirectConnection),
                   "MAVFTP output picker acceptance unavailable");
        }
        auto *ftpProgress = page->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperMavFtpProgressDialog"));
        expect(ftpProgress != nullptr,
               "MAVFTP progress dialog missing after accepted output");
        expect(waitFor([&] {
            return mavFtpDownload->isEnabled() && QFile::exists(localPath)
                && links->mavFtpService()
                && !links->mavFtpService()->isBusy();
        }, 5000), "MAVFTP download did not finish through Tools route");
        QFile downloaded(localPath);
        expect(downloaded.open(QIODevice::ReadOnly)
                   && downloaded.readAll() == fixture->ftpFileData,
               "MAVFTP downloaded binary payload differs");
        const QVector<MavFtpProtocol::Opcode> expectedOpcodes{
            MavFtpProtocol::Opcode::ResetSessions,
            MavFtpProtocol::Opcode::OpenFileReadOnly,
            MavFtpProtocol::Opcode::ReadFile,
            MavFtpProtocol::Opcode::ReadFile,
            MavFtpProtocol::Opcode::TerminateSession,
            MavFtpProtocol::Opcode::ResetSessions};
        expect(fixture->ftpEnvelopeValid
                   && fixture->ftpWrongResponsesInjected
                   && fixture->ftpWrongResponsesRejected
                   && fixture->ftpRemotePath == remotePath
                   && fixture->ftpGcsSystem != 0
                   && fixture->ftpGcsComponent != 0
                   && fixture->ftpOpcodes == expectedOpcodes,
               "MAVFTP route, identity fence, path, or request sequence differs");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime MAVFTP download:" << remotePath
                << localPath << page->Log();
    }

    // Exercise the production Setup browser, not the direct-download helper.
    // The remote list is useful after an explicit refresh, but destructive
    // prompts must remain pinned to the target that was current before any
    // local picker/consent UI opened.
    auto *setup = window->findChild<SetupView *>();
    auto *backstage = setup ? setup->findChild<BackstageView *>() : nullptr;
    expect(backstage && backstage->isPageVisible(
               QStringLiteral("MavFTPUIView")),
           "connected production MAVFTP Setup route is not visible");
    expect(backstage && backstage->setCurrentPage(
               QStringLiteral("MavFTPUIView")),
           "production MAVFTP Setup route could not be selected");
    QPointer<MavFTPUIView> browser = backstage
        ? qobject_cast<MavFTPUIView *>(backstage->page(
              QStringLiteral("MavFTPUIView")))
        : nullptr;
    QPointer<QPushButton> browserRefresh = browser ? browser->findChild<QPushButton *>(
        QStringLiteral("RefreshButton")) : nullptr;
    QPointer<QTableWidget> browserEntries = browser ? browser->findChild<QTableWidget *>(
        QStringLiteral("EntriesGrid")) : nullptr;
    QPointer<QPushButton> browserUpload = browser ? browser->findChild<QPushButton *>(
        QStringLiteral("UploadBtn")) : nullptr;
    QPointer<QPushButton> browserDelete = browser ? browser->findChild<QPushButton *>(
        QStringLiteral("DeleteButton")) : nullptr;
    expect(browser && browserRefresh && browserEntries && browserUpload
               && browserDelete,
           "production MAVFTP browser controls are missing");

    const auto selectBrowserFile = [&]() -> bool {
        if (!browserEntries) return false;
        for (int row = 0; row < browserEntries->rowCount(); ++row) {
            const QTableWidgetItem *const item = browserEntries->item(row, 0);
            if (item && item->text() == QStringLiteral("threads.txt")) {
                browserEntries->selectRow(row);
                QCoreApplication::processEvents();
                return true;
            }
        }
        return false;
    };
    const auto browserHasRemoteFixture = [&]() {
        if (!browserEntries || browserEntries->rowCount() != 2) return false;
        bool hasDirectory = false;
        bool hasFile = false;
        for (int row = 0; row < browserEntries->rowCount(); ++row) {
            const QString name = browserEntries->item(row, 0)
                ? browserEntries->item(row, 0)->text() : QString();
            hasDirectory = hasDirectory || name == QStringLiteral("logs");
            hasFile = hasFile || name == QStringLiteral("threads.txt");
        }
        return hasDirectory && hasFile;
    };
    if (browserRefresh && browserEntries && browserUpload && browserDelete
        && fixture) {
        const int listStart = fixture->ftpRequestCount;
        browserRefresh->click();
        expect(waitFor([&] {
            return !links->mavFtpService()->isBusy()
                && browserHasRemoteFixture();
        }), "production MAVFTP browser did not list the fixture directory");
        expect(fixture->ftpRequestCount == listStart + 2
                   && fixture->ftpOpcodes.value(listStart)
                       == MavFtpProtocol::Opcode::ListDirectory
                   && fixture->ftpOpcodes.value(listStart + 1)
                       == MavFtpProtocol::Opcode::ListDirectory,
               "production MAVFTP browser list did not paginate to EOF");

        expect(selectBrowserFile() && browserDelete->isEnabled(),
               "listed MAVFTP file cannot be selected for deletion");
        const int beforeDeletePrompt = fixture->ftpRequestCount;
        browserDelete->click();
        expect(waitFor([&] {
            return browser->findChild<QMessageBox *>(
                QStringLiteral("MavFtpDeleteConfirmDialog")) != nullptr;
        }), "named MAVFTP delete confirmation did not open");
        auto *deletePrompt = browser->findChild<QMessageBox *>(
            QStringLiteral("MavFtpDeleteConfirmDialog"));
        expect(deletePrompt
                   && deletePrompt->defaultButton()
                       == deletePrompt->button(QMessageBox::Cancel)
                   && deletePrompt->escapeButton()
                       == deletePrompt->button(QMessageBox::Cancel)
                   && deletePrompt->text().contains(
                       QStringLiteral("threads.txt")),
               "MAVFTP delete consent is not named/default-Cancel/path-specific");
        if (deletePrompt)
            deletePrompt->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(waitFor([&] { return browserDelete->isEnabled(); })
                   && fixture->ftpRequestCount == beforeDeletePrompt
                   && fixture->destructiveFtpRequests == 0,
               "cancelling MAVFTP delete consent transmitted a mutation");

        QTemporaryDir uploadFiles;
        expect(uploadFiles.isValid(), "MAVFTP upload fixture directory unavailable");
        const QString uploadPath = uploadFiles.filePath(
            QStringLiteral("upload candidate.bin"));
        QFile uploadSource(uploadPath);
        const QByteArray uploadBytes("must not reach the vehicle");
        expect(uploadSource.open(QIODevice::WriteOnly)
                   && uploadSource.write(uploadBytes) == uploadBytes.size(),
               "MAVFTP upload fixture could not be written");
        uploadSource.close();

        const int beforeUploadPrompt = fixture->ftpRequestCount;
        browserUpload->click();
        expect(waitFor([&] {
            return browser->findChild<QFileDialog *>(
                QStringLiteral("MavFtpUploadFileDialog")) != nullptr;
        }), "named MAVFTP upload picker did not open");
        auto *uploadPicker = browser->findChild<QFileDialog *>(
            QStringLiteral("MavFtpUploadFileDialog"));
        if (uploadPicker) {
            auto *filename = uploadPicker->findChild<QLineEdit *>(
                QStringLiteral("fileNameEdit"));
            expect(filename != nullptr,
                   "MAVFTP upload picker filename editor missing");
            if (filename) filename->setText(uploadPath);
            expect(uploadPicker->selectedFiles() == QStringList{uploadPath},
                   "MAVFTP upload selection is not exact");
            expect(QMetaObject::invokeMethod(uploadPicker, "accept",
                                             Qt::DirectConnection),
                   "MAVFTP upload picker acceptance unavailable");
        }
        expect(waitFor([&] {
            return browser->findChild<QMessageBox *>(
                QStringLiteral("MavFtpUploadConfirmDialog")) != nullptr;
        }), "named MAVFTP upload confirmation did not open");
        QPointer<QMessageBox> uploadPrompt = browser->findChild<QMessageBox *>(
            QStringLiteral("MavFtpUploadConfirmDialog"));
        expect(uploadPrompt
                   && uploadPrompt->defaultButton()
                       == uploadPrompt->button(QMessageBox::Cancel)
                   && uploadPrompt->escapeButton()
                       == uploadPrompt->button(QMessageBox::Cancel)
                   && uploadPrompt->text().contains(
                       QStringLiteral("upload candidate.bin"))
                   && uploadPrompt->text().contains(
                       QString::number(FixtureSystem)),
               "MAVFTP upload consent is not default-Cancel/path/target-specific");

        fixture->heartbeatFor(2);
        expect(waitFor([&] {
            return links->vehicleTargetManager()->contains(
                FixtureLinkId, FixtureSystem, 2);
        }), "alternate MAVFTP target component was not discovered");
        expect(links->vehicleTargetManager()->selectTarget(
                   FixtureLinkId, FixtureSystem, 2),
               "could not select alternate MAVFTP target component");
        expect(waitFor([&] {
            return (!uploadPrompt || uploadPrompt->isHidden())
                && (!browserEntries || browserEntries->rowCount() == 0)
                && !links->mavFtpService()->isBusy();
        }), "target switch did not cancel MAVFTP consent and stale rows");
        expect(fixture->ftpRequestCount == beforeUploadPrompt
                   && fixture->destructiveFtpRequests == 0,
               "target switch during MAVFTP consent transmitted a mutation");

        expect(links->vehicleTargetManager()->selectTarget(
                   FixtureLinkId, FixtureSystem, 1),
               "could not restore original MAVFTP target");
        expect(waitFor([&] {
            return links->vehicleTargetManager()->isTargetGenerationSettled();
        }), "restored MAVFTP target generation did not settle");
        expect(backstage->setCurrentPage(QStringLiteral("MavFTPUIView")),
               "MAVFTP Setup route could not reopen after target restore");
        browser = qobject_cast<MavFTPUIView *>(backstage->page(
            QStringLiteral("MavFTPUIView")));
        browserRefresh = browser ? browser->findChild<QPushButton *>(
            QStringLiteral("RefreshButton")) : nullptr;
        browserEntries = browser ? browser->findChild<QTableWidget *>(
            QStringLiteral("EntriesGrid")) : nullptr;
        browserDelete = browser ? browser->findChild<QPushButton *>(
            QStringLiteral("DeleteButton")) : nullptr;
        expect(browser && browserRefresh && browserEntries && browserDelete
                   && browserEntries->rowCount() == 0,
               "switching back silently reused stale MAVFTP rows or lost controls");
        expect(browserRefresh && browserRefresh->isEnabled(),
               "MAVFTP browser cannot explicitly refresh after target restore");
        if (browserRefresh) browserRefresh->click();
        expect(waitFor([&] {
            return !links->mavFtpService()->isBusy()
                && browserHasRemoteFixture();
        }), "MAVFTP browser did not require and complete refresh after target restore");

        expect(selectBrowserFile() && browserDelete
                   && browserDelete->isEnabled(),
               "refreshed MAVFTP file cannot be selected for deletion");
        const int beforeDeleteSwitch = fixture->ftpRequestCount;
        if (browserDelete) browserDelete->click();
        expect(waitFor([&] {
            return browser->findChild<QMessageBox *>(
                QStringLiteral("MavFtpDeleteConfirmDialog")) != nullptr;
        }), "second MAVFTP delete confirmation did not open");
        QPointer<QMessageBox> deleteSwitchPrompt =
            browser->findChild<QMessageBox *>(
                QStringLiteral("MavFtpDeleteConfirmDialog"));
        expect(deleteSwitchPrompt
                   && deleteSwitchPrompt->text().contains(
                       QStringLiteral("threads.txt"))
                   && deleteSwitchPrompt->text().contains(
                       QString::number(FixtureSystem)),
               "MAVFTP delete consent does not identify its path and target");
        expect(links->vehicleTargetManager()->selectTarget(
                   FixtureLinkId, FixtureSystem, 2),
               "could not switch target during MAVFTP delete consent");
        expect(waitFor([&] {
            return (!deleteSwitchPrompt || deleteSwitchPrompt->isHidden())
                && (!browserEntries || browserEntries->rowCount() == 0)
                && !links->mavFtpService()->isBusy();
        }), "target switch did not cancel MAVFTP delete consent and stale rows");
        expect(fixture->ftpRequestCount == beforeDeleteSwitch
                   && fixture->destructiveFtpRequests == 0,
               "target switch during MAVFTP delete consent transmitted a mutation");
        expect(links->vehicleTargetManager()->selectTarget(
                   FixtureLinkId, FixtureSystem, 1),
               "could not restore vehicle target after delete-consent audit");
        expect(waitFor([&] {
            return links->vehicleTargetManager()->isTargetGenerationSettled();
        }) && (!browserEntries || browserEntries->rowCount() == 0),
               "delete-consent target restore reused stale MAVFTP rows");
        expect(fixture->destructiveFtpRequests == 0,
               "MAVFTP browser emitted a destructive request during target-lifetime audit");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qInfo() << "Developer runtime MAVFTP browser target consent passed";
    }
    expect(backstage && backstage->setCurrentPage(
               QStringLiteral("ConfigDeveloperToolsView")),
           "Developer page could not be restored after MAVFTP browser audit");
    page = backstage
        ? qobject_cast<ConfigDeveloperToolsView *>(backstage->page(
              QStringLiteral("ConfigDeveloperToolsView")))
        : nullptr;
    expect(page != nullptr,
           "Developer route did not expose a live page after target changes");

    const QStringList names = {QStringLiteral("SetQnhButton"), QStringLiteral("AdjustBarometerAltitudeButton"),
        QStringLiteral("ForceAccelCalibratedButton"), QStringLiteral("ForceCompassCalibratedButton"),
        QStringLiteral("RebootVehicleButton"), QStringLiteral("RebootToDfuButton")};
    for (int i = 0; i < names.size() && page && fixture; ++i) {
        auto *button = page->findChild<QPushButton *>(names[i]);
        expect(waitFor([&] { return button && button->isEnabled(); }), "connected action disabled");
        if (!button || !button->isEnabled()) continue;
        const int before = fixture->commands.size() + fixture->parameterWrites;
        const auto openConsent = [&]() -> QMessageBox * {
            button->click();
            QCoreApplication::processEvents();
            if (i < 2) {
                auto *input = page->findChild<QInputDialog *>(QStringLiteral("DeveloperVehicleValueDialog"));
                if (!input) return nullptr;
                input->setDoubleValue(i == 0 ? 101500.0 : 2.0);
                input->accept();
                QCoreApplication::processEvents();
            }
            return page->findChild<QMessageBox *>(QStringLiteral("DeveloperVehicleConfirmation"));
        };
        auto *confirmation = openConsent();
        expect(confirmation && confirmation->defaultButton() == confirmation->button(QMessageBox::Cancel),
               "named default-Cancel confirmation missing");
        if (!confirmation) continue;
        confirmation->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(fixture->commands.size() + fixture->parameterWrites == before, "Cancel sent a vehicle change");
        confirmation = openConsent();
        expect(confirmation != nullptr, "second confirmation missing");
        if (!confirmation) continue;
        const auto priorOperation = service->lastReport().operationId;
        confirmation->button(QMessageBox::Yes)->click();
        expect(waitFor([&] { return !service->busy() && service->lastReport().operationId != priorOperation; }),
               "exact terminal result did not reach application service");
        qInfo() << "Developer runtime action:" << names[i] << service->lastReport().description;
        expect(service->lastReport().outcome == DeveloperVehicleToolService::Outcome::Succeeded,
               "matching exact acknowledgement did not succeed");
        expect(fixture->commands.size() + fixture->parameterWrites == before + 1, "action did not send exactly one expected change");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    if (fixture) {
        expect(fixture->parameterWrites == 2 && std::abs(fixture->pressure - 101522.2f) < 0.02f,
               "pressure/11.1 Pa per metre mapping changed");
        expect(fixture->commands.size() == 4, "command action inventory mismatch");
        if (fixture->commands.size() == 4) {
            expect(fixture->commands[0].param5 == 76 && fixture->commands[1].param2 == 76,
                   "force calibration sentinel mismatch");
            expect(fixture->commands[2].param1 == 1, "ordinary reboot wire mismatch");
            const auto dfu = fixture->commands[3];
            expect(dfu.param1 == 42 && dfu.param2 == 24 && dfu.param3 == 71 && dfu.param4 == 99,
                   "DFU was confused with hold-in-bootloader");
        }
    }
    // Bootloader flashing is only simulated by this in-process fixture.  Both
    // actual consent boundaries must be crossed before its single wire frame.
    if (page && fixture) {
        auto *button = page->findChild<QPushButton *>(
            QStringLiteral("UpgradeBootloaderButton"));
        expect(waitFor([&] { return button && button->isEnabled(); }),
               "connected bootloader action disabled");
        const int before = fixture->commands.size();
        const auto visibleConsent = [&](const QString &name) -> QMessageBox * {
            for (auto *dialog : page->findChildren<QMessageBox *>(name)) {
                if (dialog->isVisible()) return dialog;
            }
            return nullptr;
        };
        const auto openSourceConsent = [&]() -> QMessageBox * {
            if (!button || !button->isEnabled()) return nullptr;
            button->click();
            QCoreApplication::processEvents();
            return visibleConsent(QStringLiteral(
                "DeveloperUpgradeBootloaderSourceConfirmation"));
        };
        const auto checkConsent = [&](QMessageBox *dialog) {
            expect(dialog && dialog->defaultButton()
                       == dialog->button(QMessageBox::Cancel)
                       && dialog->escapeButton()
                       == dialog->button(QMessageBox::Cancel),
                   "bootloader consent is missing or not default/Escape Cancel");
            expect(dialog && dialog->text().contains(
                       QString::number(FixtureLinkId))
                       && dialog->text().contains(QString::number(FixtureSystem))
                       && dialog->text().contains(QStringLiteral("component 1")),
                   "bootloader consent did not display the exact target");
        };
        auto *source = openSourceConsent();
        checkConsent(source);
        if (source) source->button(QMessageBox::Cancel)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(fixture->commands.size() == before,
               "bootloader first Cancel transmitted a command");
        for (bool execute : {false, true}) {
            source = openSourceConsent();
            checkConsent(source);
            if (!source) continue;
            source->button(QMessageBox::Yes)->click();
            QCoreApplication::processEvents();
            auto *flash = visibleConsent(QStringLiteral(
                "DeveloperUpgradeBootloaderFlashConfirmation"));
            checkConsent(flash);
            expect(fixture->commands.size() == before,
                   "bootloader first Yes transmitted before final consent");
            if (!flash) continue;
            const QString screenshot = qEnvironmentVariable(
                "APM_BOOTLOADER_AUDIT_SCREENSHOT");
            if (execute && !screenshot.isEmpty()) {
                expect(flash->grab().save(screenshot),
                       "could not capture bootloader final consent");
            }
            const auto prior = service->lastReport().operationId;
            flash->button(execute ? QMessageBox::Yes : QMessageBox::Cancel)->click();
            if (execute) {
                expect(waitFor([&] {
                    return !service->busy()
                        && service->lastReport().operationId != prior;
                }), "bootloader exact terminal report missing");
                expect(service->lastReport().outcome
                           == DeveloperVehicleToolService::Outcome::Succeeded
                           && service->lastReport().description.contains(
                               QStringLiteral("already"), Qt::CaseInsensitive),
                       "bootloader ACK must mean updated or already current");
                expect(fixture->commands.size() == before + 1,
                       "bootloader did not send exactly one command");
                if (fixture->commands.size() == before + 1) {
                    const auto command = fixture->commands.last();
                    expect(command.command == MAV_CMD_FLASH_BOOTLOADER
                               && command.target_system == FixtureSystem
                               && command.target_component == 1
                               && command.confirmation == 0
                               && command.param1 == 0 && command.param2 == 0
                               && command.param3 == 0 && command.param4 == 0
                               && command.param5 == 290876
                               && command.param6 == 0 && command.param7 == 0,
                           "bootloader exact target or wire parameters mismatch");
                }
            } else {
                expect(fixture->commands.size() == before,
                       "bootloader second Cancel transmitted a command");
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        qInfo() << "Developer runtime bootloader two-consent audit passed";
    }
    if (page && fixture) {
        auto *recovery = links->parameterRecoveryService();
        QPointer<QPushButton> restore = page->findChild<QPushButton *>(
            QStringLiteral("RestoreParametersButton"));
        QPointer<QPushButton> cancelRestore = page->findChild<QPushButton *>(
            QStringLiteral("CancelParameterRestoreButton"));
        expect(recovery && restore && cancelRestore,
               "production recovery service/actions missing");
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("recovery sample.param"));
        const QString cancelPath = files.filePath(QStringLiteral("cancel.param"));
        const QByteArray contents(
            "RECOVERY_GAIN,12.5\nCOMPASS_DEV_ID,202\n"
            "RECOVERY_ENABLE,1\nUNCHANGED,9\n");
        expect(files.isValid() && writeOrganizerFixture(path, contents)
                   && writeOrganizerFixture(cancelPath, "CANCEL_VALUE,1\n"),
               "recovery parameter fixtures could not be written");
        const auto picker = [&]() -> QFileDialog * {
            if (!page) return nullptr;
            for (auto *dialog : page->findChildren<QFileDialog *>(
                     QStringLiteral("DeveloperParameterRecoveryFileDialog"))) {
                if (dialog->isVisible()) return dialog;
            }
            return nullptr;
        };
        const auto consent = [&]() -> QMessageBox * {
            if (!page) return nullptr;
            for (auto *dialog : page->findChildren<QMessageBox *>(
                     QStringLiteral("DeveloperParameterRecoveryConfirmation"))) {
                if (dialog->isVisible()) return dialog;
            }
            return nullptr;
        };
        const auto openConsent = [&](const QString &selected) -> QMessageBox * {
            if (!restore || !waitFor([&] { return restore && restore->isEnabled(); })) return nullptr;
            restore->click();
            QCoreApplication::processEvents();
            auto *dialog = picker();
            if (!dialog) return nullptr;
            auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
            if (!filename) return nullptr;
            filename->setText(selected);
            expect(dialog->selectedFiles() == QStringList{selected},
                   "recovery file picker selected a different file");
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            if (!waitFor([&] { return consent() != nullptr; })) return nullptr;
            return consent();
        };
        if (recovery && restore && cancelRestore) {
            expect(waitFor([&] { return restore && restore->isEnabled(); }),
                   "connected recovery action disabled");
            restore->click();
            QCoreApplication::processEvents();
            auto *fileDialog = picker();
            expect(fileDialog != nullptr, "recovery file picker did not open");
            if (fileDialog) fileDialog->reject();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            expect(fixture->recoveryReads == 0 && fixture->recoveryWrites.isEmpty(),
                   "recovery file Cancel touched vehicle parameters");
            for (bool execute : {false, true}) {
                auto *dialog = openConsent(path);
                expect(dialog && dialog->defaultButton() == dialog->button(QMessageBox::Cancel)
                           && dialog->escapeButton() == dialog->button(QMessageBox::Cancel),
                       "recovery default/Escape Cancel consent missing");
                expect(dialog && dialog->text().contains(path)
                           && dialog->text().contains(QString::number(FixtureLinkId))
                           && dialog->text().contains(QString::number(FixtureSystem)),
                       "recovery consent did not display exact file and target");
                expect(fixture->recoveryReads == 0 && fixture->recoveryWrites.isEmpty(),
                       "recovery transmitted before consent");
                if (!dialog) continue;
                const QString screenshot = qEnvironmentVariable("APM_RECOVERY_AUDIT_SCREENSHOT");
                if (execute && !screenshot.isEmpty())
                    expect(dialog->grab().save(screenshot), "recovery consent screenshot failed");
                const quint64 prior = recovery->lastReport().operationId;
                dialog->button(execute ? QMessageBox::Yes : QMessageBox::Cancel)->click();
                if (execute) {
                    expect(waitFor([&] {
                        return !recovery->busy() && recovery->lastReport().operationId != prior;
                    }, 12000), "recovery did not reach its terminal result");
                    const auto report = recovery->lastReport();
                    expect(report.outcome == ParameterRecoveryService::Outcome::Completed
                               && report.setCount == 2 && report.unchangedCount == 2
                               && report.failedCount == 0 && report.receipts.size() == 4,
                           "recovery report did not account for ENABLE/reset/final writes");
                    const QVector<QPair<QString, float>> expected{
                        {QStringLiteral("RECOVERY_ENABLE"), 1.0f},
                        {QStringLiteral("RECOVERY_GAIN"), 12.5f},
                        {QStringLiteral("COMPASS_DEV_ID"), 0.0f},
                        {QStringLiteral("COMPASS_DEV_ID"), 202.0f}};
                    expect(fixture->recoveryWrites == expected,
                           "recovery ENABLE-first or identifier zero/value wire order mismatch");
                    expect(report.receipts.size() == 4
                               && report.receipts[0].kind == ParameterRecoveryService::Receipt::Kind::EnableWrite
                               && report.receipts[2].kind == ParameterRecoveryService::Receipt::Kind::IdentifierReset,
                           "recovery lost separate ENABLE/reset receipts");
                }
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            auto *dialog = openConsent(cancelPath);
            expect(dialog != nullptr, "recovery cancel fixture consent missing");
            if (dialog) {
                const auto before = fixture->recoveryWrites.size();
                const quint64 prior = recovery->lastReport().operationId;
                fixture->suppressRecoveryWriteEcho = true;
                fixture->recoveryWriteHook = [cancelRestore] {
                    QTimer::singleShot(0, cancelRestore.data(), [cancelRestore] {
                        if (cancelRestore) cancelRestore->click();
                    });
                };
                dialog->button(QMessageBox::Yes)->click();
                expect(waitFor([&] {
                    return !recovery->busy() && recovery->lastReport().operationId != prior;
                }, 8000), "Cancel Parameter Restore did not stop an unacknowledged write");
                expect(recovery->lastReport().outcome
                           == ParameterRecoveryService::Outcome::OutcomeUncertain
                           && fixture->recoveryWrites.size() == before + 1,
                       "recovery Cancel retried or falsely claimed an unsent write");
                fixture->recoveryWriteHook = {};
                fixture->suppressRecoveryWriteEcho = false;
            }
            expect(readFileBytes(path) == contents,
                   "parameter recovery changed the source file");
            qInfo() << "Developer runtime parameter recovery audit passed";
        }
    }
    // Open the same real window from Compass and exercise analysis + confirmed
    // application exclusively against the in-process exact endpoint.
    {
        auto *applyService = links->offlineMagFitApplyService();
        fixture->magFitMode = true;
        // Recovery deliberately ended with an uncertain transmitted write.
        // Respect its real late-echo quarantine instead of clearing service
        // state or treating a Busy refresh as an accepted list request.
        auto listAdmission = ParameterService::SendResult::Busy;
        expect(waitFor([&] {
            listAdmission = static_cast<ParameterService::SendResult>(
                links->parameterService()->requestCurrentParameterList());
            return listAdmission != ParameterService::SendResult::Busy;
        }, ParameterService::DefaultExactWriteQuarantineMs + 2500),
               "MagFit parameter refresh remained quarantined");
        expect(listAdmission == ParameterService::SendResult::Sent,
               "MagFit parameter list request was not admitted");
        expect(waitFor([&] {
            const auto snapshot = links->parameterService()->store()->snapshot(
                links->vehicleTargetManager()->acquireTarget().endpoint);
            return snapshot.isComplete()
                && snapshot.contains(1, QStringLiteral("COMPASS_OFS_X"))
                && snapshot.contains(1, QStringLiteral("COMPASS_OFFS_MAX"));
        }), "MagFit exact parameter fixture did not complete");
        QElapsedTimer sinceParameterList;
        sinceParameterList.start();
        expect(backstage && backstage->setCurrentPage(QStringLiteral("ConfigCompassView")),
               "Compass route did not open for MagFit");
        const QPointer<ConfigCompassView> compass = backstage
            ? qobject_cast<ConfigCompassView *>(backstage->page(QStringLiteral("ConfigCompassView")))
            : nullptr;
        const QPointer<QPushButton> fromLog = compass
            ? compass->findChild<QPushButton *>(QStringLiteral("compassCalFromLog")) : nullptr;
        expect(fromLog && fromLog->isEnabled(), "Compass Calibrate from Log is unavailable");
        if (fromLog) fromLog->click();
        QPointer<OfflineMagFitWindow> magFit(window->findChild<OfflineMagFitWindow *>());
        expect(magFit && magFit->isVisible() && magFit->isWindow(),
               "Compass did not display the shared MagFit window");
        expect(window->findChildren<OfflineMagFitWindow *>().size() == 1,
               "Developer and Compass created duplicate MagFit windows");
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("known compass sphere.log"));
        const QByteArray source = magFitLogFixture();
        expect(files.isValid() && writeOrganizerFixture(path, source),
               "MagFit source fixture could not be written");
        if (magFit && applyService) {
            const QPointer<QPushButton> browse = magFit->findChild<QPushButton *>(
                QStringLiteral("OfflineMagFitBrowseButton"));
            const QPointer<QPushButton> analyze = magFit->findChild<QPushButton *>(
                QStringLiteral("OfflineMagFitAnalyzeButton"));
            const QPointer<QPushButton> apply = magFit->findChild<QPushButton *>(
                QStringLiteral("OfflineMagFitApplyButton"));
            const QPointer<QCheckBox> ellipsoid = magFit->findChild<QCheckBox *>(
                QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
            const auto picker = [&]() -> QFileDialog * {
                if (!magFit) return nullptr;
                for (auto *dialog : magFit->findChildren<QFileDialog *>(
                         QStringLiteral("OfflineMagFitSourceDialog"))) {
                    if (dialog->isVisible()) return dialog;
                }
                return nullptr;
            };
            const auto confirmation = [&]() -> QMessageBox * {
                if (!magFit) return nullptr;
                for (auto *dialog : magFit->findChildren<QMessageBox *>(
                         QStringLiteral("OfflineMagFitApplyConfirmation"))) {
                    if (dialog->isVisible()) return dialog;
                }
                return nullptr;
            };
            expect(browse && analyze && apply && ellipsoid && ellipsoid->isChecked(),
                   "MagFit required controls/default ellipsoid are missing");
            if (browse && analyze && apply && ellipsoid) {
                browse->click();
                expect(waitFor([&] { return picker() != nullptr; }), "MagFit source dialog did not open");
                if (auto *dialog = picker()) dialog->reject();
                expect(fixture->magFitWrites.isEmpty() && !magFit->analysisBusy(),
                       "MagFit file Cancel caused work or writes");
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                browse->click();
                expect(waitFor([&] { return picker() != nullptr; }), "MagFit source dialog did not reopen");
                if (auto *dialog = picker()) {
                    auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
                    expect(filename != nullptr, "MagFit filename editor is missing");
                    if (filename) filename->setText(path);
                    expect(dialog->selectedFiles() == QStringList{path},
                           "MagFit file picker selected a different file");
                    expect(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
                           "MagFit source selection was not accepted");
                }
                expect(waitFor([&] {
                    const auto *sourcePath = magFit
                        ? magFit->findChild<QLineEdit *>(QStringLiteral("OfflineMagFitSourcePath"))
                        : nullptr;
                    return sourcePath && sourcePath->text() == path && !picker();
                }), "MagFit picker did not publish the selected source path");
                ellipsoid->setChecked(false);
                analyze->click();
                expect(waitFor([&] { return magFit && !magFit->analysisBusy(); }, 10000),
                       "MagFit analysis did not finish");
                if (magFit) {
                    const auto report = magFit->report();
                    qInfo() << "MagFit runtime analysis:" << report.error
                            << report.applyUnavailableReason << magFit->statusText();
                    expect(report.success && report.applyEligible && report.results.size() == 1,
                           "MagFit known-context log did not produce an eligible result");
                    if (report.results.size() == 1) {
                        const auto fit = report.results.first();
                        expect(fit.coverageOctants == 8 && fit.usedSamples == 192
                                   && std::abs(fit.offsets.x - 20.0) < 0.05
                                   && std::abs(fit.offsets.y + 10.0) < 0.05
                                   && std::abs(fit.offsets.z - 5.0) < 0.05
                                   && fit.rmsError < 0.05,
                               "MagFit sphere sign/coverage/RMS differs from the known fixture");
                    }
                    expect(waitFor([&] { return apply && apply->isEnabled(); }),
                           "MagFit positive-result Apply was disabled");
                    apply->click();
                    qInfo() << "MagFit runtime prepare:" << magFit->statusText();
                    expect(waitFor([&] { return confirmation() != nullptr; }),
                           "MagFit apply consent did not open");
                    if (auto *dialog = confirmation()) {
                        expect(dialog->defaultButton() == dialog->button(QMessageBox::Cancel)
                                   && dialog->escapeButton() == dialog->button(QMessageBox::Cancel)
                                   && dialog->text().contains(path)
                                   && dialog->text().contains(QString::number(FixtureLinkId))
                                   && dialog->text().contains(QString::number(FixtureSystem)),
                               "MagFit consent is not exact-source/target/default-Cancel");
                        dialog->button(QMessageBox::Cancel)->click();
                    }
                    expect(fixture->magFitWrites.isEmpty(), "MagFit consent Cancel wrote parameters");
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    apply->click();
                    expect(waitFor([&] { return confirmation() != nullptr; }),
                           "MagFit apply consent did not reopen");
                    if (auto *dialog = confirmation()) {
                        // The real parameter protocol also fences late list
                        // replies before an exact write reservation. Simulate
                        // reading the consent while heartbeats/events continue;
                        // do not bypass that fence or auto-retry an apply.
                        expect(waitFor([&] {
                            return sinceParameterList.elapsed()
                                >= ParameterService::DefaultExactWriteQuarantineMs + 250;
                        }, ParameterService::DefaultExactWriteQuarantineMs + 2500),
                               "MagFit parameter-list isolation did not settle");
                        const QString screenshot = qEnvironmentVariable("APM_MAGFIT_AUDIT_SCREENSHOT");
                        if (!screenshot.isEmpty()) {
                            expect(dialog->grab().save(screenshot), "MagFit consent screenshot failed");
                            expect(magFit->grab().save(screenshot + QStringLiteral(".window.png")),
                                   "MagFit result window screenshot failed");
                        }
                        dialog->button(QMessageBox::Yes)->click();
                        expect(waitFor([&] {
                            return !applyService->busy() && applyService->lastReport().isValid();
                        }, 8000), "MagFit exact application did not finish");
                        const auto applied = applyService->lastReport();
                        qInfo() << "MagFit runtime apply:" << applied.description;
                        expect(applied.outcome == OfflineMagFitApplyService::Outcome::Completed
                                   && applied.totalWrites == 4 && applied.confirmedWrites == 4
                                   && applied.remainingWrites == 0 && applied.receipts.size() == 4
                                   && fixture->magFitWrites.size() == 4,
                               "MagFit application did not acknowledge exactly four writes");
                        if (fixture->magFitWrites.size() == 4) {
                            const auto writes = fixture->magFitWrites;
                            expect(writes[0].first == QStringLiteral("COMPASS_LEARN") && writes[0].second == 0
                                       && writes[1].first == QStringLiteral("COMPASS_OFS_X") && std::abs(writes[1].second - 20.0f) < 0.05f
                                       && writes[2].first == QStringLiteral("COMPASS_OFS_Y") && std::abs(writes[2].second + 10.0f) < 0.05f
                                       && writes[3].first == QStringLiteral("COMPASS_OFS_Z") && std::abs(writes[3].second - 5.0f) < 0.05f,
                                   "MagFit emitted a different parameter order/value");
                        }
                    }
                    expect(readFileBytes(path) == source, "MagFit changed the source log");
                    magFit->close();
                    qInfo() << "Developer runtime Offline MagFit audit passed";
                }
            }
        }
    }
    // Remote DataFlash: actual Developer prompts, physical protocol ingress
    // (logger component155), async disk writes and explicit captured-file save.
    {
        auto *remote = links->remoteDataFlashLogService();
        QTemporaryDir remoteFiles;
        expect(remote && remoteFiles.isValid(), "remote log service/directory unavailable");
        expect(backstage && backstage->setCurrentPage(QStringLiteral("ConfigDeveloperToolsView")),
               "remote logger Developer route did not open");
        page = window->findChild<ConfigDeveloperToolsView *>();
        if (page && remote && remoteFiles.isValid()) {
            page->setRemoteDataFlashLogService(remote, remoteFiles.path());
            const auto button = [&](const char *name) -> QPushButton * {
                return page ? page->findChild<QPushButton *>(QString::fromLatin1(name)) : nullptr;
            };
            const auto prompt = [&](const char *name) -> QMessageBox * {
                if (!page) return nullptr;
                for (auto *dialog : page->findChildren<QMessageBox *>(QString::fromLatin1(name)))
                    if (dialog->isVisible()) return dialog;
                return nullptr;
            };
            auto *start = button("StartRemoteDataFlashLogButton");
            // The converse interlock must hold before a remote session exists:
            // keep a classic list request active (this fixture never answers
            // LOG requests), then reject remote admission without sending START.
            {
                RemoteDataFlashLogService::Plan remotePlan;
                QString interlockError;
                const bool prepared = remote->prepare(
                    remoteFiles.path(), &remotePlan, &interlockError);
                expect(prepared && remotePlan.isValid(),
                       "remote interlock preflight could not capture the exact fixture");
                if (prepared && remotePlan.isValid()) {
                    QObject classicOwner;
                    ExactLogTransferToken classicToken;
                    auto *classic = links->exactLogTransferService();
                    expect(classic->requestList(&classicOwner, remotePlan.vehicle(),
                                               &classicToken, &interlockError)
                               == ExactLogTransferService::StartResult::Started
                               && classicToken.isValid() && classic->busy(),
                           "classic list fixture did not remain active for converse interlock");
                    expect(!remote->canPrepare(&interlockError)
                               && !interlockError.isEmpty() && !remote->busy()
                               && fixture->remoteControls.isEmpty(),
                           "remote admission entered an active classic log protocol");
                    if (classicToken.isValid()) {
                        expect(classic->cancel(classicToken,
                                   QStringLiteral("Remote-log converse interlock audit complete")),
                               "classic interlock audit could not cancel its owned token");
                    }
                    expect(!classic->busy(),
                           "classic interlock audit left its owned request active");
                }
            }
            expect(waitFor([&] { return start && start->isEnabled(); }),
                   "remote Start unavailable with complete LOG_BACKEND_TYPE snapshot");
            if (start) start->click();
            expect(waitFor([&] { return prompt("DeveloperRemoteDataFlashStartConfirmation"); }),
                   "remote Start confirmation missing");
            if (auto *dialog = prompt("DeveloperRemoteDataFlashStartConfirmation")) {
                expect(dialog->defaultButton() == dialog->button(QMessageBox::Cancel)
                           && dialog->escapeButton() == dialog->button(QMessageBox::Cancel)
                           && dialog->text().contains(remoteFiles.path())
                           && dialog->text().contains(QString::number(FixtureSystem))
                           && dialog->text().contains(QString::number(FixtureLinkId)),
                       "remote Start consent lacks exact target/directory/default Cancel");
                dialog->button(QMessageBox::Cancel)->click();
            }
            expect(fixture->remoteControls.isEmpty() && !remote->busy(),
                   "remote Start Cancel transmitted or admitted work");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (start) start->click();
            expect(waitFor([&] { return prompt("DeveloperRemoteDataFlashStartConfirmation"); }),
                   "remote Start confirmation did not reopen");
            if (auto *dialog = prompt("DeveloperRemoteDataFlashStartConfirmation"))
                dialog->button(QMessageBox::Yes)->click();
            expect(waitFor([&] {
                return remote->phase() == RemoteDataFlashLogService::Phase::Receiving
                    && remote->blocksStored() == 3 && fixture->remoteControls.size() >= 5;
            }, 8000), "remote stream did not store and acknowledge three blocks and a duplicate");
            qInfo() << "Remote DataFlash runtime receiving:" << remote->status();
            const quint64 operation = remote->currentOperationId();
            QMap<quint32, int> acknowledgements;
            for (const auto &control : fixture->remoteControls) {
                expect(control.target_system == FixtureSystem && control.target_component == 1
                           && control.status == MAV_REMOTE_LOG_DATA_BLOCK_ACK,
                       "remote STATUS has wrong exact target/type");
                ++acknowledgements[control.seqno];
            }
            expect(acknowledgements.value(MAV_REMOTE_LOG_DATA_BLOCK_START) == 1
                       && acknowledgements.value(0) == 1
                       && acknowledgements.value(1) == 2
                       && acknowledgements.value(2) == 1
                       && acknowledgements.size() == 4,
                   "remote logger acknowledged a foreign frame, lost a duplicate or retried START");
            QObject logOwner;
            ExactLogTransferToken logToken;
            QString logError;
            expect(links->exactLogTransferService()->requestList(
                       &logOwner, remote->activePlan().vehicle(), &logToken, &logError)
                       == ExactLogTransferService::StartResult::UnsafeRoute
                       && !logToken.isValid(),
                   "classic log list entered the active remote log protocol");
            // Destroy and recreate the actual page: recording belongs to the
            // app, not to the widget. No STOP may be sent by page teardown.
            const int controlsBeforeClose = fixture->remoteControls.size();
            backstage->resetPage(QStringLiteral("ConfigDeveloperToolsView"));
            expect(backstage->setCurrentPage(QStringLiteral("ConfigDeveloperToolsView")),
                   "remote logger Developer route did not recreate");
            page = window->findChild<ConfigDeveloperToolsView *>();
            expect(page && remote->busy() && remote->currentOperationId() == operation
                       && fixture->remoteControls.size() == controlsBeforeClose,
                   "Developer recreation stopped or replaced the active remote log");
            if (page) page->setRemoteDataFlashLogService(remote, remoteFiles.path());
            auto *stop = button("StopRemoteDataFlashLogButton");
            expect(waitFor([&] { return stop && stop->isEnabled(); }),
                   "reopened Developer page cannot stop its app-owned remote log");
            if (stop) stop->click();
            expect(waitFor([&] { return prompt("DeveloperRemoteDataFlashStopConfirmation"); }),
                   "remote Stop confirmation missing");
            if (auto *dialog = prompt("DeveloperRemoteDataFlashStopConfirmation")) {
                expect(dialog->defaultButton() == dialog->button(QMessageBox::Cancel)
                           && dialog->escapeButton() == dialog->button(QMessageBox::Cancel),
                       "remote Stop consent is not default/Escape Cancel");
                dialog->button(QMessageBox::Cancel)->click();
            }
            expect(remote->busy() && fixture->remoteControls.size() == controlsBeforeClose,
                   "remote Stop Cancel stopped recording");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            fixtureArmed = true;
            fixture->heartbeat(true);
            expect(waitFor([&] {
                SwarmTelemetrySnapshot snapshot;
                return links->swarmTelemetryRegistry()->snapshotForLease(
                    remote->activePlan().vehicle(), &snapshot) && snapshot.armed;
            }), "remote logger armed fixture was not observed");
            expect(waitFor([&] { return stop && stop->isEnabled(); }),
                   "remote Stop became inaccessible while armed");
            if (stop) stop->click();
            expect(waitFor([&] { return prompt("DeveloperRemoteDataFlashStopConfirmation"); }),
                   "remote Stop confirmation did not reopen");
            if (auto *dialog = prompt("DeveloperRemoteDataFlashStopConfirmation")) {
                const QString evidence = qEnvironmentVariable("APM_REMOTE_LOG_AUDIT_SCREENSHOT");
                if (!evidence.isEmpty())
                    expect(dialog->grab().save(evidence), "remote Stop screenshot failed");
                dialog->button(QMessageBox::Save)->click();
            }
            expect(waitFor([&] { return !remote->busy(); }, 8000),
                   "remote Stop and captured-file save did not finish");
            const auto report = remote->lastReport();
            qInfo() << "Remote DataFlash runtime result:" << report.description << report.destinationPath;
            expect(report.operationId == operation && report.published()
                       && report.outcome == RemoteDataFlashLogService::Outcome::SavedUnverified
                       && report.blocks == 3 && report.bytes == 600
                       && report.duplicateBlocks == 1 && report.missingBlocks == 0
                       && report.stopAttempted && report.stopSubmitted,
                   "remote capture receipt is incomplete or falsely claims remote completion");
            const auto expected = fixture->remoteBlockBytes(0)
                + fixture->remoteBlockBytes(1) + fixture->remoteBlockBytes(2);
            expect(readFileBytes(report.destinationPath) == expected,
                   "remote log saved bytes differ from the ordered three-block fixture");
            expect(fixture->remoteControls.size() == controlsBeforeClose + 1
                       && fixture->remoteControls.last().seqno == MAV_REMOTE_LOG_DATA_BLOCK_STOP,
                   "remote Stop did not send exactly one STATUS sentinel");
            const QString evidence = qEnvironmentVariable("APM_REMOTE_LOG_AUDIT_SCREENSHOT");
            if (!evidence.isEmpty() && report.published())
                expect(QFile::copy(report.destinationPath, evidence + QStringLiteral(".capture.bin")),
                       "remote capture evidence copy failed");
            fixtureArmed = false;
            fixture->heartbeat(false);
        }
    }
    heartbeat.stop();
    links->removeLink(FixtureLinkId);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(!fixture && !service->busy(), "fixture cleanup left active transport/operation");
    qInfo() << "Developer vehicle runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
