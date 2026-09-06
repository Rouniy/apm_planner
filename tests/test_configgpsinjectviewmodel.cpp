#include <QtTest>

#include "comm/Rtcm3Parser.h"
#include "ui/configuration/ConfigGpsInjectView.h"
#include "ui/configuration/ConfigGpsInjectViewModel.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>

namespace {
class FakeGpsCorrectionSource final : public GpsCorrectionSource
{
public:
    explicit FakeGpsCorrectionSource(QObject *parent = nullptr)
        : GpsCorrectionSource(parent)
    {
    }

    QStringList availablePorts() const override { return ports; }
    bool active() const override { return isActive; }
    bool connected() const override { return isConnected; }

    bool start(const GpsCorrectionSourceSettings &settings) override
    {
        ++startCalls;
        lastStartSettings = settings;
        if (!startResult) {
            isActive = false;
            isConnected = false;
            emit stateChanged(false, false);
            if (!startStatus.isEmpty()) {
                emit statusChanged(startStatus);
            }
            return false;
        }

        isActive = true;
        isConnected = connectImmediately;
        if (!settings.isNtrip() && isConnected) {
            currentReceiverSession = nextReceiverSession++;
            currentReceiverBaud = settings.baudRate;
        } else {
            currentReceiverSession = 0;
        }
        if (!startStatus.isEmpty()) {
            emit statusChanged(startStatus);
        }
        emit stateChanged(isActive, isConnected);
        return true;
    }

    void stop() override
    {
        ++stopCalls;
        isActive = false;
        isConnected = false;
        currentReceiverSession = 0;
        emit stateChanged(false, false);
    }

    void setGgaPosition(double latitude, double longitude,
                        double altitudeMsl, bool valid) override
    {
        ++ggaCalls;
        ggaLatitude = latitude;
        ggaLongitude = longitude;
        ggaAltitude = altitudeMsl;
        ggaValid = valid;
    }

    quint64 receiverSession() const noexcept override
    {
        return canConfigureReceiver() ? currentReceiverSession : 0;
    }

    bool canConfigureReceiver() const noexcept override
    {
        return isActive && isConnected && currentReceiverSession != 0;
    }

    int receiverBaudRate() const noexcept override
    {
        return canConfigureReceiver() ? currentReceiverBaud : 0;
    }

    bool setReceiverBaudRate(int baudRate, quint64 expectedSession,
                             QString *error) override
    {
        if (!canConfigureReceiver()
            || expectedSession != currentReceiverSession || baudRate <= 0) {
            if (error) {
                *error = QStringLiteral("stale receiver session");
            }
            return false;
        }
        currentReceiverBaud = baudRate;
        return true;
    }

    bool writeReceiverData(const QByteArray &bytes,
                           quint64 expectedSession,
                           QString *error) override
    {
        if (!canConfigureReceiver()
            || expectedSession != currentReceiverSession
            || bytes.isEmpty()) {
            if (error) {
                *error = QStringLiteral("stale receiver session");
            }
            return false;
        }
        receiverWrites.append(bytes);
        return true;
    }

    void publishState(bool active, bool connected)
    {
        isActive = active;
        isConnected = connected;
        emit stateChanged(active, connected);
    }

    void publishStatus(const QString &status)
    {
        emit statusChanged(status);
    }

    void emitValidRtcm(const QByteArray &frame, quint16 messageId)
    {
        emit inputBytes(frame.size());
        emit rtcmFrame(frame, messageId);
    }

    void emitUncheckedRtcm(const QByteArray &frame, quint16 messageId)
    {
        emit inputBytes(frame.size());
        emit rtcmFrame(frame, messageId);
    }

    void emitReceiverData(const QByteArray &bytes)
    {
        emit receiverBytes(bytes, currentReceiverSession);
    }

    QStringList ports;
    bool startResult = true;
    bool connectImmediately = true;
    QString startStatus;
    int startCalls = 0;
    int stopCalls = 0;
    int ggaCalls = 0;
    bool isActive = false;
    bool isConnected = false;
    GpsCorrectionSourceSettings lastStartSettings;
    double ggaLatitude = 0.0;
    double ggaLongitude = 0.0;
    double ggaAltitude = 0.0;
    bool ggaValid = false;
    quint64 nextReceiverSession = 1;
    quint64 currentReceiverSession = 0;
    int currentReceiverBaud = 0;
    QList<QByteArray> receiverWrites;
};

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

QByteArray rtcmFrame(quint16 messageId, int payloadLength = 16)
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

QByteArray base1005AtEquator()
{
    constexpr int payloadLength = 19;
    QByteArray frame(Rtcm3Parser::HeaderSize + payloadLength, '\0');
    frame[0] = static_cast<char>(Rtcm3Parser::Preamble);
    frame[1] = 0;
    frame[2] = static_cast<char>(payloadLength);
    setUnsignedBits(&frame, 24, 12, 1005);
    setUnsignedBits(&frame, 36, 12, 1);
    setUnsignedBits(&frame, 54, 1, 1);
    setSignedBits(&frame, 58, 38, 63781370000LL);
    setSignedBits(&frame, 98, 38, 0);
    setSignedBits(&frame, 138, 38, 0);
    const quint32 crc = Rtcm3Parser::crc24q(
        reinterpret_cast<const quint8 *>(frame.constData()),
        static_cast<std::size_t>(frame.size()));
    frame.append(static_cast<char>((crc >> 16) & 0xff));
    frame.append(static_cast<char>((crc >> 8) & 0xff));
    frame.append(static_cast<char>(crc & 0xff));
    return frame;
}

void stopAutomaticStatistics(ConfigGpsInjectViewModel *model)
{
    const QList<QTimer *> timers = model->findChildren<QTimer *>();
    for (QTimer *timer : timers) {
        timer->stop();
    }
}

QMessageBox *visibleUbloxAuthorization(ConfigGpsInjectView *view)
{
    if (!view) {
        return nullptr;
    }
    const QList<QMessageBox *> dialogs =
        view->findChildren<QMessageBox *>(
            QStringLiteral("gpsInjectUbloxAutoConfigureConfirmation"));
    for (QMessageBox *dialog : dialogs) {
        if (dialog && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}
} // namespace

class ConfigGpsInjectViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndMissionPlannerListOrder();
    void settingsMapToNtripAndSerialAndPasswordIsMemoryOnly();
    void toggleConnectTracksSourceStateAndStatus();
    void validRtcmUpdatesCountsFreshnessBaseAndReadySignal();
    void injectionAccountingRequiresAcceptedResult();
    void messagesSeenAreSortedAfterStatsUpdate();
    void basePositionsSaveUseDeleteAndPersist();
    void autoConfigOnlyRequestsReceiverActions();
    void restartSurveyAndConnectedFixedUseAreRealOperations();
    void repeatedReceiverAckDoesNotGrowSurveyStatus();
    void receiverCallbacksMayDeleteModel();
    void ubloxConnectAuthorizationIsDefaultCancel();
    void viewMatchesMissionPlannerSurfaceAndBindings();
};

void ConfigGpsInjectViewModelTest::defaultsAndMissionPlannerListOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    settings.clear();
    FakeGpsCorrectionSource source;
    source.ports = QStringList({
        QStringLiteral("COM7"), QStringLiteral("COM3"),
        QStringLiteral("COM7")});

    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);

    QCOMPARE(model.Title(), QStringLiteral("RTK/GPS Inject"));
    QCOMPARE(model.Ports(),
             QStringList({QStringLiteral("COM7"), QStringLiteral("COM3"),
                          QStringLiteral("NTRIP")}));
    QCOMPARE(model.SelectedPort(), QStringLiteral("NTRIP"));
    QCOMPARE(model.BaudRates(), QStringList({
        QStringLiteral("4800"), QStringLiteral("9600"),
        QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200"),
        QStringLiteral("230400"), QStringLiteral("460800"),
        QStringLiteral("921600")
    }));
    QCOMPARE(model.SelectedBaud(), QStringLiteral("115200"));
    QCOMPARE(model.ReceiverTypes(), QStringList({
        QStringLiteral("UBlox M8P/F9P"), QStringLiteral("Septentrio"),
        QStringLiteral("Unicore UM982")
    }));
    QCOMPARE(model.SelectedReceiverType(), QStringLiteral("UBlox M8P/F9P"));
    QCOMPARE(model.SeptentrioRtcmLevels(), QStringList({
        QStringLiteral("Lite"), QStringLiteral("Basic"),
        QStringLiteral("Full")
    }));
    QCOMPARE(model.SelectedSeptentrioRtcmLevel(), QStringLiteral("Basic"));

    QCOMPARE(ConfigGpsInjectViewModel::NtripOption(),
             QStringLiteral("NTRIP"));
    QVERIFY(model.IsNtrip());
    QVERIFY(!model.IsSerial());
    QVERIFY(!model.IsSeptentrio());
    QVERIFY(!model.Active());
    QVERIFY(!model.Connected());
    QVERIFY(model.CanEditSource());
    QCOMPARE(model.ConnectLabel(), QStringLiteral("Connect"));
    QCOMPARE(model.Host(), QString());
    QCOMPARE(model.Port(), 2101);
    QCOMPARE(model.Mount(), QString());
    QCOMPARE(model.Username(), QString());
    QCOMPARE(model.Password(), QString());
    QVERIFY(!model.NtripV1());
    QVERIFY(model.SendGga());
    QVERIFY(!model.AutoConfig());
    QVERIFY(model.M8p130Plus());
    QCOMPARE(model.SurveyInAcc(), QStringLiteral("2"));
    QCOMPARE(model.SurveyInTime(), QStringLiteral("60"));
    QCOMPARE(model.SurveyInStatus(), QStringLiteral("Survey In: not started"));
    QVERIFY(!model.SurveyInValid());
    QVERIFY(model.SeptentrioGps());
    QVERIFY(model.SeptentrioGlonass());
    QVERIFY(model.SeptentrioGalileo());
    QVERIFY(model.SeptentrioBeidou());
    QCOMPARE(model.SeptentrioRtcmInterval(), QStringLiteral("1.0"));
    QVERIFY(!model.SeptentrioFixedPosition());
    QCOMPARE(model.SeptentrioLat(), QStringLiteral("0"));
    QCOMPARE(model.SeptentrioLng(), QStringLiteral("0"));
    QCOMPARE(model.SeptentrioAlt(), QStringLiteral("0"));
    QCOMPARE(model.Status(), QStringLiteral(
        "Select a serial port or NTRIP and press Connect."));
    QCOMPARE(model.Injected(), QStringLiteral("0 bytes"));
    QCOMPARE(model.InputRate(), QStringLiteral("0 bps"));
    QCOMPARE(model.OutputRate(), QStringLiteral("0 bps"));
    QCOMPARE(model.MessagesSeen(), QString());
    QVERIFY(model.BasePositions().isEmpty());
    QVERIFY(!model.HasCurrentBasePosition());
    QVERIFY(!model.HasActiveBasePosition());
    QVERIFY(!model.BaseFresh());
    QVERIFY(!model.GpsFresh());
    QVERIFY(!model.GlonassFresh());
    QVERIFY(!model.GalileoFresh());
    QVERIFY(!model.BeidouFresh());
}

void ConfigGpsInjectViewModelTest::
    settingsMapToNtripAndSerialAndPasswordIsMemoryOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("gps.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.clear();

    {
        FakeGpsCorrectionSource source;
        source.ports = QStringList({QStringLiteral("COM9")});
        ConfigGpsInjectViewModel model(&source, &settings);
        stopAutomaticStatistics(&model);

        model.SetHost(QStringLiteral("caster.example.test"));
        model.SetPort(2201);
        model.SetMount(QStringLiteral("MOUNT42"));
        model.SetUsername(QStringLiteral("pilot"));
        model.SetPassword(QStringLiteral("session-secret"));
        model.SetNtripV1(true);
        model.SetSendGga(false);
        model.SetVehiclePosition(35.1, 33.2, 123.4, true);

        QVERIFY(model.ToggleConnect());
        QCOMPARE(source.startCalls, 1);
        QCOMPARE(source.lastStartSettings.selectedPort,
                 QStringLiteral("NTRIP"));
        QCOMPARE(source.lastStartSettings.baudRate, 115200);
        QCOMPARE(source.lastStartSettings.host,
                 QStringLiteral("caster.example.test"));
        QCOMPARE(source.lastStartSettings.casterPort, 2201);
        QCOMPARE(source.lastStartSettings.mountPoint,
                 QStringLiteral("MOUNT42"));
        QCOMPARE(source.lastStartSettings.username, QStringLiteral("pilot"));
        QCOMPARE(source.lastStartSettings.password,
                 QStringLiteral("session-secret"));
        QVERIFY(source.lastStartSettings.ntripV1);
        QVERIFY(!source.lastStartSettings.sendGga);
        QVERIFY(source.ggaValid);
        QCOMPARE(source.ggaLatitude, 35.1);
        QCOMPARE(source.ggaLongitude, 33.2);
        QCOMPARE(source.ggaAltitude, 123.4);
        QVERIFY(model.ToggleConnect());

        model.SetSelectedPort(QStringLiteral("COM9"));
        model.SetSelectedBaud(QStringLiteral("460800"));
        QVERIFY(model.ToggleConnect());
        QCOMPARE(source.startCalls, 2);
        QCOMPARE(source.lastStartSettings.selectedPort,
                 QStringLiteral("COM9"));
        QCOMPARE(source.lastStartSettings.baudRate, 460800);
        QVERIFY(!source.lastStartSettings.isNtrip());
        QVERIFY(model.ToggleConnect());

        settings.sync();
        QVERIFY(settings.status() == QSettings::NoError);
        for (const QString &key : settings.allKeys()) {
            QVERIFY2(!key.contains(QStringLiteral("password"),
                                   Qt::CaseInsensitive),
                     qPrintable(key));
        }
        QFile persisted(settingsPath);
        QVERIFY(persisted.open(QIODevice::ReadOnly));
        QVERIFY(!persisted.readAll().contains("session-secret"));
    }

    FakeGpsCorrectionSource reloadedSource;
    ConfigGpsInjectViewModel reloaded(&reloadedSource, &settings);
    stopAutomaticStatistics(&reloaded);
    QCOMPARE(reloaded.SelectedPort(), QStringLiteral("COM9"));
    QCOMPARE(reloaded.Ports().first(), QStringLiteral("COM9"));
    QCOMPARE(reloaded.SelectedBaud(), QStringLiteral("460800"));
    QCOMPARE(reloaded.Host(), QStringLiteral("caster.example.test"));
    QCOMPARE(reloaded.Port(), 2201);
    QCOMPARE(reloaded.Mount(), QStringLiteral("MOUNT42"));
    QCOMPARE(reloaded.Username(), QStringLiteral("pilot"));
    QCOMPARE(reloaded.Password(), QString());
    QVERIFY(reloaded.NtripV1());
    QVERIFY(!reloaded.SendGga());
    QCOMPARE(settings.value(QStringLiteral("SerialInjectGPS_port"))
                 .toString(), QStringLiteral("COM9"));
}

void ConfigGpsInjectViewModelTest::toggleConnectTracksSourceStateAndStatus()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM4")});
    source.connectImmediately = false;
    source.startStatus = QStringLiteral("Connecting fake source.");
    ConfigGpsInjectViewModel model(&source, &settings);
    model.SetSelectedPort(QStringLiteral("COM4"));
    stopAutomaticStatistics(&model);

    QSignalSpy states(&model, &ConfigGpsInjectViewModel::stateChanged);
    QSignalSpy statuses(&model, &ConfigGpsInjectViewModel::statusChanged);
    QVERIFY(model.ToggleConnect());
    QCOMPARE(source.startCalls, 1);
    QVERIFY(model.Active());
    QVERIFY(!model.Connected());
    QVERIFY(!model.CanEditSource());
    QCOMPARE(model.ConnectLabel(), QStringLiteral("Disconnect"));
    QCOMPARE(model.Status(), QStringLiteral("Connecting fake source."));

    source.publishStatus(QStringLiteral("Waiting for corrections."));
    QCOMPARE(model.Status(), QStringLiteral("Waiting for corrections."));
    source.publishState(true, true);
    QVERIFY(model.Connected());
    QCOMPARE(model.Status(),
             QStringLiteral("Connected — receiving RTCM correction data."));
    QVERIFY(states.count() > 0);
    QVERIFY(statuses.count() > 0);

    QVERIFY(model.ToggleConnect());
    QCOMPARE(source.stopCalls, 1);
    QVERIFY(!model.Active());
    QVERIFY(!model.Connected());
    QVERIFY(model.CanEditSource());
    QCOMPARE(model.ConnectLabel(), QStringLiteral("Connect"));
    QCOMPARE(model.Status(), QStringLiteral("Disconnected."));

    source.startResult = false;
    source.startStatus = QStringLiteral("Synthetic open failure.");
    QVERIFY(!model.ToggleConnect());
    QCOMPARE(source.startCalls, 2);
    QVERIFY(!model.Active());
    QVERIFY(!model.Connected());
    QCOMPARE(model.Status(), QStringLiteral("Synthetic open failure."));
}

void ConfigGpsInjectViewModelTest::
    validRtcmUpdatesCountsFreshnessBaseAndReadySignal()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);
    QSignalSpy ready(&model, &ConfigGpsInjectViewModel::rtcmDataReady);

    const QByteArray base = base1005AtEquator();
    const QByteArray gps = rtcmFrame(1077);
    const QByteArray glonass = rtcmFrame(1087);
    const QByteArray galileo = rtcmFrame(1097);
    const QByteArray beidou = rtcmFrame(1127);
    source.emitValidRtcm(base, 1005);
    source.emitValidRtcm(gps, 1077);
    source.emitValidRtcm(glonass, 1087);
    source.emitValidRtcm(galileo, 1097);
    source.emitValidRtcm(beidou, 1127);

    QCOMPARE(ready.count(), 5);
    QCOMPARE(ready.at(0).at(0).toByteArray(), base);
    QCOMPARE(ready.at(4).at(0).toByteArray(), beidou);
    QCOMPARE(model.RtcmMessageCount(1005), quint64(1));
    QCOMPARE(model.RtcmMessageCount(1077), quint64(1));
    QCOMPARE(model.RtcmMessageCount(1087), quint64(1));
    QCOMPARE(model.RtcmMessageCount(1097), quint64(1));
    QCOMPARE(model.RtcmMessageCount(1127), quint64(1));
    QVERIFY(model.BaseFresh());
    QVERIFY(model.GpsFresh());
    QVERIFY(model.GlonassFresh());
    QVERIFY(model.GalileoFresh());
    QVERIFY(model.BeidouFresh());
    QVERIFY(model.HasCurrentBasePosition());
    QVERIFY(std::abs(model.CurrentBasePosition().Lat.toDouble()) < 1.0e-9);
    QVERIFY(std::abs(model.CurrentBasePosition().Long.toDouble()) < 1.0e-9);
    QVERIFY(std::abs(model.CurrentBasePosition().Alt.toDouble()) < 1.0e-4);
    QVERIFY(model.RtcmBasePos().startsWith(
        QStringLiteral("0.0000000 0.0000000 0.00 - ")));

    QByteArray corrupt = gps;
    corrupt[7] = static_cast<char>(corrupt.at(7) ^ 0x20);
    source.emitUncheckedRtcm(corrupt, 1077);
    source.emitUncheckedRtcm(gps, 1005);
    QCOMPARE(ready.count(), 5);
    QCOMPARE(model.RtcmMessageCount(1077), quint64(1));
    QCOMPARE(model.RtcmMessageCount(1005), quint64(1));
}

void ConfigGpsInjectViewModelTest::
    injectionAccountingRequiresAcceptedResult()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);
    const QByteArray frame = rtcmFrame(1077);

    source.emitValidRtcm(frame, 1077);
    model.UpdateStats();
    QCOMPARE(model.InputRate(),
             QStringLiteral("%1 bps").arg(frame.size()));
    QCOMPARE(model.OutputRate(), QStringLiteral("0 bps sent"));
    QCOMPARE(model.Injected(), QStringLiteral("0 bytes"));

    model.ReportInjectionResult(
        frame.size(), false, QStringLiteral("vehicle offline"));
    model.UpdateStats();
    QCOMPARE(model.OutputRate(), QStringLiteral("0 bps sent"));
    QCOMPARE(model.Injected(), QStringLiteral("0 bytes"));
    QCOMPARE(model.Status(),
             QStringLiteral("RTCM injection rejected: vehicle offline"));

    model.ReportInjectionResult(frame.size(), true);
    QCOMPARE(model.OutputRate(), QStringLiteral("0 bps sent"));
    QCOMPARE(model.Injected(), QStringLiteral("0 bytes"));
    model.UpdateStats();
    QCOMPARE(model.OutputRate(),
             QStringLiteral("%1 bps sent").arg(frame.size()));
    QCOMPARE(model.Injected(),
             QStringLiteral("%1 bytes").arg(frame.size()));
}

void ConfigGpsInjectViewModelTest::messagesSeenAreSortedAfterStatsUpdate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);

    source.emitValidRtcm(rtcmFrame(1127), 1127);
    source.emitValidRtcm(base1005AtEquator(), 1005);
    source.emitValidRtcm(rtcmFrame(1077), 1077);
    source.emitValidRtcm(rtcmFrame(1077), 1077);
    QCOMPARE(model.MessagesSeen(), QString());
    model.UpdateStats();
    QCOMPARE(model.MessagesSeen(), QStringLiteral(
        "Rtcm1005=1 Rtcm1077=2 Rtcm1127=1"));
}

void ConfigGpsInjectViewModelTest::basePositionsSaveUseDeleteAndPersist()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    settings.clear();
    BasePosRow saved;

    {
        FakeGpsCorrectionSource source;
        ConfigGpsInjectViewModel model(&source, &settings);
        stopAutomaticStatistics(&model);
        QSignalSpy changed(&model,
                           &ConfigGpsInjectViewModel::basePositionsChanged);
        model.SetCurrentBasePosition(35.1234567, 33.7654321, 123.4, true);
        QVERIFY(model.SaveCurrentPosition());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(model.BasePositions().size(), 1);
        saved = model.BasePositions().first();
        QCOMPARE(saved.Lat, QStringLiteral("35.1234567"));
        QCOMPARE(saved.Long, QStringLiteral("33.7654321"));
        QCOMPARE(saved.Alt, QStringLiteral("123.4"));
        QVERIFY(saved.Name.startsWith(QStringLiteral("Base ")));

        QVERIFY(model.UseBasePos(saved));
        QVERIFY(model.HasActiveBasePosition());
        QVERIFY(model.ActiveBasePosition() == saved);
        QVERIFY(model.Status().contains(
            QStringLiteral("Saved fixed base position")));
        QVERIFY(model.Status().contains(saved.Name));
        QVERIFY(model.Status().contains(
            QStringLiteral("not sent")));
        BasePosRow invalidAltitude = saved;
        invalidAltitude.Alt = QStringLiteral("100001");
        invalidAltitude.Name = QStringLiteral("Invalid altitude");
        QVERIFY(!model.UseBasePos(invalidAltitude));
        QVERIFY(model.ActiveBasePosition() == saved);
        QVERIFY(model.Status().contains(
            QStringLiteral("outside the receiver's supported range")));

        source.ports = QStringList({QStringLiteral("COM8")});
        model.SetSelectedPort(QStringLiteral("COM8"));
        model.SetAutoConfig(true);
        model.SetSurveyInTime(QStringLiteral("invalid but unused"));
        QSignalSpy authorization(
            &model,
            &ConfigGpsInjectViewModel::ubloxAuthorizationRequested);
        QVERIFY(model.ToggleConnect());
        QCOMPARE(source.startCalls, 0);
        QCOMPARE(authorization.count(), 1);
        const QList<QVariant> request = authorization.takeFirst();
        QVERIFY(request.at(1).toString().contains(
            QStringLiteral("Fixed base")));
        QVERIFY(model.ResolveUbloxAuthorization(
            request.at(0).toULongLong(), false));
        QCOMPARE(source.startCalls, 0);
        settings.sync();
        QVERIFY(!settings.value(QStringLiteral("base_pos_list"))
                     .toString().isEmpty());
        QVERIFY(!settings.value(QStringLiteral("base_pos"))
                     .toString().isEmpty());
    }

    {
        FakeGpsCorrectionSource source;
        ConfigGpsInjectViewModel reloaded(&source, &settings);
        stopAutomaticStatistics(&reloaded);
        QCOMPARE(reloaded.BasePositions().size(), 1);
        QVERIFY(reloaded.BasePositions().first() == saved);
        QVERIFY(reloaded.HasActiveBasePosition());
        QVERIFY(reloaded.ActiveBasePosition() == saved);
        QVERIFY(reloaded.DeleteBasePos(saved));
        QVERIFY(reloaded.BasePositions().isEmpty());
        QVERIFY(!reloaded.DeleteBasePos(saved));
        settings.sync();
        QCOMPARE(settings.value(QStringLiteral("base_pos_list")).toString(),
                 QString());
    }
}

void ConfigGpsInjectViewModelTest::autoConfigOnlyRequestsReceiverActions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM8")});
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);

    QSignalSpy ubloxAuthorization(
        &model, &ConfigGpsInjectViewModel::ubloxAuthorizationRequested);
    QSignalSpy septentrioConfigure(
        &model, &ConfigGpsInjectViewModel::septentrioConfigureRequested);
    QSignalSpy septentrioPosition(
        &model, &ConfigGpsInjectViewModel::septentrioPositionRequested);
    QSignalSpy septentrioRtcm(
        &model, &ConfigGpsInjectViewModel::septentrioRtcmRequested);

    model.SetSelectedPort(QStringLiteral("COM8"));
    model.SetAutoConfig(true);
    model.SetM8p130Plus(false);
    // Connect-time auto-configuration does not start Survey In. Invalid
    // survey fields therefore do not block the separate receiver setup.
    model.SetSurveyInTime(QStringLiteral("not-a-duration"));
    model.SetSurveyInAcc(QStringLiteral("1.25"));
    QVERIFY(model.ToggleConnect());
    QCOMPARE(source.startCalls, 0);
    QCOMPARE(ubloxAuthorization.count(), 1);
    const QList<QVariant> firstAuthorization =
        ubloxAuthorization.takeFirst();
    QVERIFY(firstAuthorization.at(1).toString().contains(
        QStringLiteral("Receiver setup only")));
    QVERIFY(model.ResolveUbloxAuthorization(
        firstAuthorization.at(0).toULongLong(), false));
    QCOMPARE(source.startCalls, 0);

    model.SetSurveyInTime(QStringLiteral("75"));
    QVERIFY(model.ToggleConnect());
    QCOMPARE(source.startCalls, 0);
    QCOMPARE(ubloxAuthorization.count(), 1);
    const quint64 cancelledAuthorization =
        ubloxAuthorization.takeFirst().at(0).toULongLong();
    QVERIFY(cancelledAuthorization != 0);
    QVERIFY(model.ReceiverBusy());
    QVERIFY(!model.CanEditSource());
    QVERIFY(model.ResolveUbloxAuthorization(cancelledAuthorization, false));
    QCOMPARE(source.startCalls, 0);
    QVERIFY(!model.ReceiverBusy());
    QVERIFY(model.Status().contains(QStringLiteral("not opened")));

    QVERIFY(model.ToggleConnect());
    QCOMPARE(ubloxAuthorization.count(), 1);
    const QList<QVariant> authorization = ubloxAuthorization.takeFirst();
    const quint64 acceptedAuthorization =
        authorization.at(0).toULongLong();
    QVERIFY(authorization.at(1).toString().contains(QStringLiteral("COM8")));
    QVERIFY(authorization.at(1).toString().contains(
        QStringLiteral("Survey In will not start")));
    QVERIFY(model.ResolveUbloxAuthorization(acceptedAuthorization, true));
    QCOMPARE(source.startCalls, 1);
    QVERIFY(model.Connected());
    QVERIFY(model.UbloxService());
    QVERIFY(model.UbloxService()->busy());
    QVERIFY(!model.CanRestartSurveyIn());

    const BasePosRow fixed = {
        QStringLiteral("35.1"), QStringLiteral("33.2"),
        QStringLiteral("100.5"), QStringLiteral("Home")
    };
    QVERIFY(!model.UseBasePos(fixed));
    QVERIFY(model.Status().contains(
        QStringLiteral("another receiver configuration")));
    QVERIFY(model.ToggleConnect());

    model.SetSelectedReceiverType(QStringLiteral("Septentrio"));
    model.SetSelectedSeptentrioRtcmLevel(QStringLiteral("Full"));
    model.SetSeptentrioRtcmInterval(QStringLiteral("0.5"));
    model.SetSeptentrioGps(true);
    model.SetSeptentrioGlonass(false);
    model.SetSeptentrioGalileo(true);
    model.SetSeptentrioBeidou(false);
    model.SetSeptentrioFixedPosition(true);
    model.SetSeptentrioLat(QStringLiteral("35.25"));
    model.SetSeptentrioLng(QStringLiteral("33.75"));
    model.SetSeptentrioAlt(QStringLiteral("88.5"));
    QVERIFY(model.ToggleConnect());
    QCOMPARE(septentrioConfigure.count(), 1);
    QCOMPARE(septentrioPosition.count(), 1);
    QCOMPARE(septentrioPosition.first().at(0).toBool(), true);
    QCOMPARE(septentrioPosition.first().at(1).toDouble(), 35.25);
    QCOMPARE(septentrioPosition.first().at(2).toDouble(), 33.75);
    QCOMPARE(septentrioPosition.first().at(3).toDouble(), 88.5);
    QCOMPARE(septentrioRtcm.count(), 1);
    QCOMPARE(septentrioRtcm.first().at(0).toString(),
             QStringLiteral("Full"));
    QCOMPARE(septentrioRtcm.first().at(1).toDouble(), 0.5);
    QCOMPARE(septentrioRtcm.first().at(2).toBool(), true);
    QCOMPARE(septentrioRtcm.first().at(3).toBool(), false);
    QCOMPARE(septentrioRtcm.first().at(4).toBool(), true);
    QCOMPARE(septentrioRtcm.first().at(5).toBool(), false);
    QCOMPARE(model.Status(), QStringLiteral(
        "Connected — receiving RTCM. Septentrio receiver auto-configuration is not implemented yet."));

    QVERIFY(!model.ApplySeptentrioRtcm());
    QCOMPARE(septentrioRtcm.count(), 2);
    QCOMPARE(model.Status(),
             QStringLiteral("Septentrio receiver configuration is not implemented yet; RTCM injection remains available."));
    QVERIFY(!model.ApplySeptentrioPosition());
    QCOMPARE(septentrioPosition.count(), 2);
    QCOMPARE(model.Status(),
             QStringLiteral("Septentrio receiver configuration is not implemented yet; RTCM injection remains available."));
    QVERIFY(model.ToggleConnect());

    model.SetSelectedReceiverType(QStringLiteral("Unicore UM982"));
    QVERIFY(model.ToggleConnect());
    QCOMPARE(septentrioConfigure.count(), 1);
    QCOMPARE(model.Status(), QStringLiteral(
        "Connected — receiving RTCM. Auto-config for Unicore UM982 is not supported here."));
}

void ConfigGpsInjectViewModelTest::
    restartSurveyAndConnectedFixedUseAreRealOperations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM8")});
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);
    model.SetSelectedPort(QStringLiteral("COM8"));
    QVERIFY(model.ToggleConnect());
    QVERIFY(model.Connected());

    const int writesBeforeValidation = source.receiverWrites.size();
    model.SetSurveyInTime(QStringLiteral("not-a-duration"));
    QVERIFY(!model.RestartSurveyIn());
    QVERIFY(model.Status().contains(QStringLiteral("whole number")));
    QCOMPARE(source.receiverWrites.size(), writesBeforeValidation);
    model.SetSurveyInTime(QStringLiteral("60"));
    model.SetSurveyInAcc(QStringLiteral("0"));
    QVERIFY(!model.RestartSurveyIn());
    QVERIFY(model.Status().contains(QStringLiteral("between")));
    QCOMPARE(source.receiverWrites.size(), writesBeforeValidation);

    model.SetSurveyInAcc(QStringLiteral("1.25"));
    QVERIFY(model.RestartSurveyIn());
    QVERIFY(model.UbloxService()->busy());
    QVERIFY(model.SurveyInStatus().contains(QStringLiteral("restarting")));
    QVERIFY(model.ToggleConnect());

    QVERIFY(model.ToggleConnect());
    const BasePosRow fixed = {
        QStringLiteral("35.1"), QStringLiteral("33.2"),
        QStringLiteral("100.5"), QStringLiteral("Home")
    };
    QVERIFY(model.UseBasePos(fixed));
    QVERIFY(model.UbloxService()->busy());
    QVERIFY(model.ActiveBasePosition() == fixed);
    QVERIFY(model.ToggleConnect());
}

void ConfigGpsInjectViewModelTest::
    repeatedReceiverAckDoesNotGrowSurveyStatus()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM8")});
    ConfigGpsInjectViewModel model(&source, &settings);
    stopAutomaticStatistics(&model);
    model.SetSelectedPort(QStringLiteral("COM8"));
    model.SetAutoConfig(true);
    QSignalSpy authorization(
        &model, &ConfigGpsInjectViewModel::ubloxAuthorizationRequested);
    QVERIFY(model.ToggleConnect());
    QCOMPARE(authorization.count(), 1);
    QVERIFY(model.ResolveUbloxAuthorization(
        authorization.takeFirst().at(0).toULongLong(), true));
    QVERIFY(model.UbloxService()->busy());

    QByteArray payload;
    payload.append(char(0x06));
    payload.append(char(0x00));
    const QByteArray acknowledgement = UbloxBaseStationProtocol::frame(
        0x05, 0x01, payload);
    source.emitReceiverData(acknowledgement);
    const QString first = model.SurveyInStatus();
    QCOMPARE(first.count(QStringLiteral("Receiver ACK")), 1);
    QVERIFY(first.startsWith(QStringLiteral("Survey In: not started")));

    source.emitReceiverData(acknowledgement);
    QCOMPARE(model.SurveyInStatus(), first);
    QCOMPARE(model.SurveyInStatus().count(QStringLiteral("Receiver ACK")), 1);

    const QString fixedBase =
        QStringLiteral("Fixed base: submitting receiver configuration");
    model.SetSurveyInStatus(fixedBase, false);
    source.emitReceiverData(acknowledgement);
    const QString fixedWithAck = model.SurveyInStatus();
    QVERIFY(fixedWithAck.startsWith(fixedBase));
    QCOMPARE(fixedWithAck.count(QStringLiteral("Receiver ACK")), 1);
    source.emitReceiverData(acknowledgement);
    QCOMPARE(model.SurveyInStatus(), fixedWithAck);
    QVERIFY(model.ToggleConnect());
}

void ConfigGpsInjectViewModelTest::receiverCallbacksMayDeleteModel()
{
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath(QStringLiteral("cancel.ini")),
                           QSettings::IniFormat);
        FakeGpsCorrectionSource source;
        source.ports = QStringList({QStringLiteral("COM8")});
        auto *model = new ConfigGpsInjectViewModel(&source, &settings);
        stopAutomaticStatistics(model);
        model->SetSelectedPort(QStringLiteral("COM8"));
        model->SetAutoConfig(true);
        QSignalSpy authorization(
            model,
            &ConfigGpsInjectViewModel::ubloxAuthorizationRequested);
        QVERIFY(model->ToggleConnect());
        QCOMPARE(authorization.count(), 1);
        const quint64 authorizationId =
            authorization.takeFirst().at(0).toULongLong();
        QVERIFY(model->ResolveUbloxAuthorization(authorizationId, true));
        QVERIFY(model->UbloxService()->busy());

        QPointer<ConfigGpsInjectViewModel> guard(model);
        QObject::connect(
            model->UbloxService(),
            &UbloxBaseStationService::operationFinished, &source,
            [model](const UbloxBaseStationService::Report &) {
                delete model;
            });
        QVERIFY(!model->ToggleConnect());
        QVERIFY(guard.isNull());
    }

    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath(QStringLiteral("start.ini")),
                           QSettings::IniFormat);
        FakeGpsCorrectionSource source;
        source.ports = QStringList({QStringLiteral("COM8")});
        auto *model = new ConfigGpsInjectViewModel(&source, &settings);
        stopAutomaticStatistics(model);
        model->SetSelectedPort(QStringLiteral("COM8"));
        model->SetAutoConfig(true);
        QSignalSpy authorization(
            model,
            &ConfigGpsInjectViewModel::ubloxAuthorizationRequested);
        QVERIFY(model->ToggleConnect());
        QCOMPARE(authorization.count(), 1);
        const quint64 authorizationId =
            authorization.takeFirst().at(0).toULongLong();

        QPointer<ConfigGpsInjectViewModel> guard(model);
        QMetaObject::Connection deletion;
        deletion = QObject::connect(
            &source, &GpsCorrectionSource::stateChanged, &source,
            [&deletion, model](bool active, bool connected) {
                if (active && connected) {
                    QObject::disconnect(deletion);
                    delete model;
                }
            });
        QVERIFY(!model->ResolveUbloxAuthorization(authorizationId, true));
        QVERIFY(guard.isNull());
    }
}

void ConfigGpsInjectViewModelTest::
    ubloxConnectAuthorizationIsDefaultCancel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM8")});
    ConfigGpsInjectView view(&source, &settings);
    stopAutomaticStatistics(view.viewModel());
    view.show();

    auto *sourceCombo = view.findChild<QComboBox *>(
        QStringLiteral("gpsInjectSourceCombo"));
    auto *autoConfig = view.findChild<QCheckBox *>(
        QStringLiteral("gpsInjectAutoConfigCheck"));
    auto *connectButton = view.findChild<QPushButton *>(
        QStringLiteral("gpsInjectConnectButton"));
    QVERIFY(sourceCombo);
    QVERIFY(autoConfig);
    QVERIFY(connectButton);
    sourceCombo->setCurrentText(QStringLiteral("COM8"));
    autoConfig->setChecked(true);
    connectButton->click();

    QMessageBox *dialog = nullptr;
    QTRY_VERIFY((dialog = visibleUbloxAuthorization(&view)) != nullptr);
    QCOMPARE(dialog->defaultButton(),
             qobject_cast<QPushButton *>(dialog->button(QMessageBox::Cancel)));
    QCOMPARE(dialog->escapeButton(), dialog->button(QMessageBox::Cancel));
    QVERIFY(dialog->text().contains(QStringLiteral("COM8")));
    QVERIFY(dialog->text().contains(
        QStringLiteral("Survey In will not start")));
    QCOMPARE(source.startCalls, 0);

    QTest::keyClick(dialog, Qt::Key_Escape);
    QTRY_COMPARE(view.viewModel()->PendingUbloxAuthorization(), quint64(0));
    QCOMPARE(source.startCalls, 0);

    connectButton->click();
    QTRY_VERIFY((dialog = visibleUbloxAuthorization(&view)) != nullptr);
    QAbstractButton *confirm = dialog->button(QMessageBox::Yes);
    QVERIFY(confirm);
    QCOMPARE(confirm->objectName(),
             QStringLiteral("gpsInjectUbloxAutoConfigureConfirmButton"));
    confirm->click();
    QTRY_COMPARE(source.startCalls, 1);
    QVERIFY(view.viewModel()->Connected());
}

void ConfigGpsInjectViewModelTest::
    viewMatchesMissionPlannerSurfaceAndBindings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("gps.ini")),
                       QSettings::IniFormat);
    FakeGpsCorrectionSource source;
    source.ports = QStringList({QStringLiteral("COM8")});

    ConfigGpsInjectView view(&source, &settings);
    stopAutomaticStatistics(view.viewModel());
    view.show();
    QCoreApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigGpsInjectView"));
    QCOMPARE(view.sizeHint(), QSize(900, 760));
    auto *title = view.findChild<QLabel *>(
        QStringLiteral("gpsInjectTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("RTK/GPS Inject"));

    auto *sourceCombo = view.findChild<QComboBox *>(
        QStringLiteral("gpsInjectSourceCombo"));
    auto *baudCombo = view.findChild<QComboBox *>(
        QStringLiteral("gpsInjectBaudCombo"));
    auto *ntripPanel = view.findChild<QFrame *>(
        QStringLiteral("gpsInjectNtripPanel"));
    auto *password = view.findChild<QLineEdit *>(
        QStringLiteral("gpsInjectPasswordEdit"));
    auto *autoConfig = view.findChild<QCheckBox *>(
        QStringLiteral("gpsInjectAutoConfigCheck"));
    auto *receiverCombo = view.findChild<QComboBox *>(
        QStringLiteral("gpsInjectReceiverTypeCombo"));
    auto *autoPanel = view.findChild<QFrame *>(
        QStringLiteral("gpsInjectAutoConfigPanel"));
    auto *septentrioPanel = view.findChild<QFrame *>(
        QStringLiteral("gpsInjectSeptentrioPanel"));
    auto *table = view.findChild<QTableWidget *>(
        QStringLiteral("gpsInjectBasePositionsTable"));
    QVERIFY(sourceCombo);
    QVERIFY(baudCombo);
    QVERIFY(ntripPanel);
    QVERIFY(password);
    QVERIFY(autoConfig);
    QVERIFY(receiverCombo);
    QVERIFY(autoPanel);
    QVERIFY(septentrioPanel);
    QVERIFY(table);

    QCOMPARE(sourceCombo->currentText(), QStringLiteral("NTRIP"));
    QVERIFY(!baudCombo->isEnabled());
    QVERIFY(!ntripPanel->isHidden());
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QVERIFY(receiverCombo->isHidden());
    QVERIFY(autoPanel->isHidden());
    QVERIFY(septentrioPanel->isHidden());
    QCOMPARE(table->columnCount(), 6);
    QCOMPARE(table->horizontalHeaderItem(0)->text(),
             QStringLiteral("Lat/ECEFX"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(),
             QStringLiteral("Delete"));

    sourceCombo->setCurrentText(QStringLiteral("COM8"));
    QCOMPARE(view.viewModel()->SelectedPort(), QStringLiteral("COM8"));
    QVERIFY(baudCombo->isEnabled());
    QVERIFY(ntripPanel->isHidden());

    autoConfig->setChecked(true);
    QVERIFY(!receiverCombo->isHidden());
    QVERIFY(!autoPanel->isHidden());
    receiverCombo->setCurrentText(QStringLiteral("Septentrio"));
    QVERIFY(!septentrioPanel->isHidden());
    const QList<QLabel *> labels = septentrioPanel->findChildren<QLabel *>();
    bool historicalLabelFound = false;
    for (const QLabel *label : labels) {
        historicalLabelFound = historicalLabelFound
            || label->text() == QLatin1String("Atitude (WGS84)");
    }
    QVERIFY(historicalLabelFound);

    view.viewModel()->SetCurrentBasePosition(35.1, 33.2, 100.0, true);
    QVERIFY(view.viewModel()->SaveCurrentPosition());
    QCOMPARE(table->rowCount(), 1);
    auto *useButton = qobject_cast<QPushButton *>(table->cellWidget(0, 4));
    auto *deleteButton = qobject_cast<QPushButton *>(
        table->cellWidget(0, 5));
    QVERIFY(useButton);
    QVERIFY(deleteButton);
    useButton->click();
    QVERIFY(view.viewModel()->HasActiveBasePosition());
    deleteButton->click();
    QCOMPARE(table->rowCount(), 0);
}

QTEST_MAIN(ConfigGpsInjectViewModelTest)
#include "test_configgpsinjectviewmodel.moc"
