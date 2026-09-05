#include "DeveloperVehicleToolRuntimeAudit.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/TCPLink.h"
#include "comm/VehicleTargetManager.h"
#include "services/DeveloperVehicleToolService.h"
#include "ui/MainWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <functional>
#include <cmath>
#include <cstring>

namespace {
constexpr int FixtureLinkId = 910110;
constexpr quint8 FixtureSystem = 234;
constexpr int DataFlashFmtLength = 89;

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
        QStringLiteral("Developer action audit"), 61981, false) {}
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
    void heartbeat(bool armed = false) {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(FixtureSystem, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
            0, MAV_STATE_STANDBY);
        inject(message);
    }
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
    void writeBytes(const char *bytes, qint64 size) override {
        mavlink_message_t message{};
        for (qint64 i = 0; i < size; ++i) {
            if (parser.parseByte(quint8(bytes[i]), &message) != MAVLINK_FRAMING_OK) continue;
            if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST
                || message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) pressureReply();
            if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
                mavlink_param_set_t value{};
                mavlink_msg_param_set_decode(&message, &value);
                if (value.target_system != FixtureSystem || value.target_component != 1) continue;
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
                    || command.command == MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN)
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
    expect(page && page->ImplementedActionCount() == 14 && page->ActionCount() == 32,
           "production Developer route did not bind offline and vehicle tools");
    if (!page) return 1;
    auto *reboot = page->findChild<QPushButton *>(QStringLiteral("RebootVehicleButton"));
    expect(reboot && !reboot->isEnabled(), "offline reboot was enabled");

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
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, fixture, [fixture] { if (fixture) fixture->heartbeat(); });
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
    heartbeat.stop();
    links->removeLink(FixtureLinkId);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(!fixture && !service->busy(), "fixture cleanup left active transport/operation");
    qInfo() << "Developer vehicle runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
