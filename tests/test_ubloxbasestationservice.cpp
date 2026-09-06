#include "comm/GpsCorrectionSource.h"
#include "comm/UbloxBaseStationProtocol.h"
#include "comm/UbloxBaseStationService.h"

#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <functional>

namespace
{
template<typename T>
void put(QByteArray *bytes, int offset, T value)
{
    qToLittleEndian<T>(
        value, reinterpret_cast<uchar *>(bytes->data() + offset));
}

class FakeCorrectionSource final : public GpsCorrectionSource
{
public:
    using GpsCorrectionSource::GpsCorrectionSource;

    QStringList availablePorts() const override
    {
        return {QStringLiteral("FAKE")};
    }
    bool active() const override { return m_active; }
    bool connected() const override { return m_connected; }
    bool start(const GpsCorrectionSourceSettings &) override { return false; }
    void stop() override { disconnectReceiver(); }
    void setGgaPosition(double, double, double, bool) override {}
    quint64 receiverSession() const noexcept override
    {
        return canConfigureReceiver() ? m_session : 0;
    }
    bool canConfigureReceiver() const noexcept override
    {
        return m_active && m_connected && m_session != 0;
    }
    int receiverBaudRate() const noexcept override
    {
        return canConfigureReceiver() ? m_baudRate : 0;
    }
    bool setReceiverBaudRate(
        int baudRate, quint64 expectedSession, QString *error) override
    {
        if (!canConfigureReceiver() || expectedSession != m_session
            || baudRate <= 0) {
            if (error) {
                *error = QStringLiteral("stale fake receiver");
            }
            return false;
        }
        m_baudRate = baudRate;
        baudChanges.append(baudRate);
        return true;
    }
    bool writeReceiverData(
        const QByteArray &bytes, quint64 expectedSession,
        QString *error) override
    {
        if (!canConfigureReceiver() || expectedSession != m_session) {
            if (error) {
                *error = QStringLiteral("stale fake receiver");
            }
            return false;
        }
        writes.append(bytes);
        if (writeHook) {
            writeHook(bytes);
        }
        return writeAllowed;
    }

    void connectReceiver(quint64 session, int baudRate = 115200)
    {
        m_active = true;
        m_connected = true;
        m_session = session;
        m_baudRate = baudRate;
        emit stateChanged(true, true);
    }

    void disconnectReceiver()
    {
        m_active = false;
        m_connected = false;
        m_session = 0;
        emit stateChanged(false, false);
    }

    void replaceReceiver(quint64 session, int baudRate = 115200)
    {
        m_session = session;
        m_baudRate = baudRate;
        emit stateChanged(true, true);
    }

    void feed(const QByteArray &bytes)
    {
        emit receiverBytes(bytes, m_session);
    }

    QVector<QByteArray> writes;
    QVector<int> baudChanges;
    std::function<void(const QByteArray &)> writeHook;
    bool writeAllowed = true;

private:
    bool m_active = false;
    bool m_connected = false;
    quint64 m_session = 0;
    int m_baudRate = 0;
};

QByteArray acknowledgement(bool accepted, quint8 messageClass,
                           quint8 messageId)
{
    QByteArray payload;
    payload.append(char(messageClass));
    payload.append(char(messageId));
    return UbloxBaseStationProtocol::frame(
        0x05, accepted ? 0x01 : 0x00, payload);
}

QByteArray surveyPacket()
{
    QByteArray payload(40, '\0');
    put<quint32>(&payload, 4, 123000U);
    put<quint32>(&payload, 8, 75U);
    put<qint32>(&payload, 12, 637813700);
    put<quint32>(&payload, 28, 12500U);
    put<quint32>(&payload, 32, 321U);
    payload[36] = 1;
    payload[37] = 0;
    return UbloxBaseStationProtocol::frame(0x01, 0x3b, payload);
}

QByteArray positionPacket()
{
    QByteArray payload(92, '\0');
    put<quint32>(&payload, 0, 456000U);
    put<quint16>(&payload, 4, 2026);
    payload[6] = 9;
    payload[7] = 6;
    payload[8] = 12;
    payload[9] = 34;
    payload[10] = 56;
    payload[11] = 3;
    payload[20] = 3;
    payload[21] = 3;
    payload[23] = 18;
    put<qint32>(&payload, 24, 331234567);
    put<qint32>(&payload, 28, 351234567);
    put<qint32>(&payload, 32, 450123);
    put<qint32>(&payload, 36, 420123);
    put<quint32>(&payload, 40, 25U);
    put<quint32>(&payload, 44, 40U);
    return UbloxBaseStationProtocol::frame(0x01, 0x07, payload);
}
}

class UbloxBaseStationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void receiverSetupDoesNotStartSurveyOrReset();
    void surveyThenFixedUsesOnePinnedSerialSession();
    void acknowledgementIsDiagnosticNotCompletionProof();
    void cancelAndSessionReplacementStopWithoutReplay();
    void validatesNormalInputsAndWritableSource();
};

void UbloxBaseStationServiceTest::receiverSetupDoesNotStartSurveyOrReset()
{
    FakeCorrectionSource source;
    source.connectReceiver(17, 115200);
    UbloxBaseStationService service(&source);
    const QVector<UbloxBaseStationProtocol::Command> expected =
        UbloxBaseStationProtocol::autoConfigure(115200, false);

    quint64 operationId = 0;
    QString error;
    QVERIFY(service.configureReceiver(false, &operationId, &error));
    QVERIFY(operationId != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 8000);
    QCOMPARE(service.lastReport().operationId, operationId);
    QCOMPARE(service.lastReport().mode,
             UbloxBaseStationService::Mode::Receiver);
    QCOMPARE(service.lastReport().outcome,
             UbloxBaseStationService::Outcome::Submitted);
    QCOMPARE(service.lastReport().totalCommands, expected.size());
    QCOMPARE(source.writes.size(), expected.size());
    for (int index = 0; index < expected.size(); ++index) {
        QCOMPARE(source.writes.at(index), expected.at(index).bytes);
    }
    QVERIFY(!source.writes.contains(
        UbloxBaseStationProtocol::surveyIn(60, 2.0)));
    const QVector<UbloxBaseStationProtocol::Command> disable =
        UbloxBaseStationProtocol::disableBase();
    for (const UbloxBaseStationProtocol::Command &command : disable) {
        QVERIFY(!source.writes.contains(command.bytes));
    }
    QCOMPARE(service.state(), UbloxBaseStationService::State::Configured);
    QCOMPARE(service.configuredReceiverSession(), quint64(17));
}

void UbloxBaseStationServiceTest::surveyThenFixedUsesOnePinnedSerialSession()
{
    FakeCorrectionSource source;
    source.connectReceiver(41, 115200);
    UbloxBaseStationService service(&source);

    source.writeHook = [&source](const QByteArray &bytes) {
        const int marker = bytes.indexOf(QByteArray::fromHex("b562"));
        if (marker >= 0 && bytes.size() >= marker + 4) {
            source.feed(acknowledgement(
                true, quint8(bytes.at(marker + 2)),
                quint8(bytes.at(marker + 3))));
        }
    };

    const QVector<UbloxBaseStationProtocol::Command> expectedSetup =
        UbloxBaseStationProtocol::restartSurvey(
            115200, 60, 2.0, true);
    const QByteArray expectedSurvey =
        UbloxBaseStationProtocol::surveyIn(60, 2.0);
    quint64 surveyId = 0;
    QString error;
    QVERIFY(service.configureSurveyIn(
        60, 2.0, true, &surveyId, &error));
    QVERIFY(surveyId != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 12000);
    QCOMPARE(service.lastReport().outcome,
             UbloxBaseStationService::Outcome::Submitted);
    QCOMPARE(service.lastReport().receiverSession, quint64(41));
    QCOMPARE(service.lastReport().totalCommands, expectedSetup.size());
    QCOMPARE(service.lastReport().submittedCommands, expectedSetup.size());
    QCOMPARE(source.writes.size(), expectedSetup.size());
    for (int index = 0; index < expectedSetup.size(); ++index) {
        QCOMPARE(source.writes.at(index), expectedSetup.at(index).bytes);
    }
    QCOMPARE(source.writes.constLast(), expectedSurvey);
    QCOMPARE(service.state(), UbloxBaseStationService::State::SurveyIn);
    QCOMPARE(service.configuredReceiverSession(), quint64(41));
    QCOMPARE(source.receiverBaudRate(), 460800);
    QCOMPARE(source.baudChanges,
             QVector<int>({9600, 38400, 57600, 115200,
                           230400, 460800}));

    source.feed(surveyPacket());
    QVERIFY(service.surveyStatus().valid);
    QCOMPARE(service.surveyStatus().durationSeconds, quint32(75));
    QCOMPARE(service.surveyStatus().observations, quint32(321));
    QVERIFY(service.surveyStatus().hasPosition);
    QCOMPARE(service.surveyStatus().accuracyMeters, 1.25);
    source.feed(positionPacket());
    QVERIFY(service.currentPosition().fixOk);
    QVERIFY(std::abs(service.currentPosition().latitude - 35.1234567)
            < 1.0e-9);
    QVERIFY(std::abs(service.currentPosition().longitude - 33.1234567)
            < 1.0e-9);

    source.writes.clear();
    const UbloxBaseStationService::FixedPosition fixed{
        35.1234567, 33.1234567, 450.25, 0.001};
    QVector<UbloxBaseStationProtocol::Command> expectedFixedSetup =
        UbloxBaseStationProtocol::autoConfigure(460800, true);
    expectedFixedSetup += UbloxBaseStationProtocol::disableBase();
    const QByteArray expectedFixed = UbloxBaseStationProtocol::fixedLla(
        fixed.latitude, fixed.longitude, fixed.altitudeMeters,
        fixed.accuracyMeters);
    quint64 fixedId = 0;
    QVERIFY(service.configureFixed(fixed, true, &fixedId, &error));
    QVERIFY(fixedId != 0 && fixedId != surveyId);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 8000);
    QCOMPARE(service.state(), UbloxBaseStationService::State::Fixed);
    QCOMPARE(source.writes.size(), expectedFixedSetup.size() + 1);
    for (int index = 0; index < expectedFixedSetup.size(); ++index) {
        QCOMPARE(source.writes.at(index),
                 expectedFixedSetup.at(index).bytes);
    }
    QCOMPARE(source.writes.constLast(), expectedFixed);

    source.writes.clear();
    const int baudChangesBeforeApply = source.baudChanges.size();
    quint64 applyId = 0;
    QVERIFY(service.applyFixed(fixed, &applyId, &error));
    QVERIFY(applyId != 0 && applyId != fixedId);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
    QCOMPARE(service.lastReport().mode,
             UbloxBaseStationService::Mode::Fixed);
    QCOMPARE(service.lastReport().totalCommands, 2);
    QCOMPARE(source.writes.size(), 2);
    QCOMPARE(source.writes.at(0), expectedFixed);
    QCOMPARE(source.writes.at(1),
             UbloxBaseStationProtocol::frame(0x06, 0x71));
    QCOMPARE(source.baudChanges.size(), baudChangesBeforeApply);
}

void UbloxBaseStationServiceTest::
acknowledgementIsDiagnosticNotCompletionProof()
{
    FakeCorrectionSource source;
    source.connectReceiver(9);
    UbloxBaseStationService service(&source);
    bool sentNak = false;
    source.writeHook = [&source, &sentNak](const QByteArray &bytes) {
        if (sentNak) {
            return;
        }
        const int marker = bytes.indexOf(QByteArray::fromHex("b562"));
        if (marker >= 0) {
            sentNak = true;
            source.feed(acknowledgement(
                false, quint8(bytes.at(marker + 2)),
                quint8(bytes.at(marker + 3))));
        }
    };

    quint64 operationId = 0;
    QVERIFY(service.configureSurveyIn(
        30, 1.0, false, &operationId));
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 12000);
    QVERIFY(sentNak);
    QCOMPARE(service.lastReport().outcome,
             UbloxBaseStationService::Outcome::Submitted);
    QCOMPARE(service.lastReport().rejectedAcknowledgements, 1);
    QVERIFY(service.acknowledgementStatus().contains(
        QStringLiteral("NAK")));
    QVERIFY(service.lastReport().description.contains(
        QStringLiteral("diagnostic")));
}

void UbloxBaseStationServiceTest::
cancelAndSessionReplacementStopWithoutReplay()
{
    FakeCorrectionSource source;
    source.connectReceiver(101);
    UbloxBaseStationService service(&source);
    QSignalSpy finished(&service,
                        &UbloxBaseStationService::operationFinished);

    quint64 operationId = 0;
    QVERIFY(service.configureSurveyIn(
        60, 2.0, true, &operationId));
    QVERIFY(!service.cancel(operationId + 1));
    QVERIFY(service.cancel(operationId));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(service.lastReport().outcome,
             UbloxBaseStationService::Outcome::Cancelled);
    const int afterCancel = source.writes.size();
    QTest::qWait(250);
    QCOMPARE(source.writes.size(), afterCancel);

    QVERIFY(service.configureSurveyIn(
        60, 2.0, true, &operationId));
    QTRY_VERIFY_WITH_TIMEOUT(!source.writes.isEmpty(), 1000);
    source.replaceReceiver(102);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 1000);
    QCOMPARE(service.lastReport().outcome,
             UbloxBaseStationService::Outcome::SourceLost);
    const int afterReplacement = source.writes.size();
    QTest::qWait(300);
    QCOMPARE(source.writes.size(), afterReplacement);
    QCOMPARE(service.state(), UbloxBaseStationService::State::Ready);
    QCOMPARE(service.configuredReceiverSession(), quint64(0));
    QVERIFY(service.status().contains(QStringLiteral("replaced"),
                                      Qt::CaseInsensitive));
}

void UbloxBaseStationServiceTest::
validatesNormalInputsAndWritableSource()
{
    FakeCorrectionSource source;
    UbloxBaseStationService service(&source);
    quint64 operationId = 123;
    QString error;
    QVERIFY(!service.configureSurveyIn(
        60, 2.0, true, &operationId, &error));
    QCOMPARE(operationId, quint64(0));
    QVERIFY(!service.available());
    QVERIFY(!service.configureReceiver(true, &operationId, &error));

    source.connectReceiver(1);
    QVERIFY(!service.configureSurveyIn(
        0, 2.0, true, &operationId, &error));
    QVERIFY(!service.configureSurveyIn(
        60, 0.0001, true, &operationId, &error));
    UbloxBaseStationService::FixedPosition invalid;
    invalid.latitude = 91.0;
    invalid.longitude = 0.0;
    invalid.altitudeMeters = 1.0;
    QVERIFY(!service.applyFixed(invalid, &operationId, &error));
    invalid.accuracyMeters = 0.001;
    QVERIFY(!service.configureFixed(
        invalid, true, &operationId, &error));
    QVERIFY(source.writes.isEmpty());
}

QTEST_GUILESS_MAIN(UbloxBaseStationServiceTest)

#include "test_ubloxbasestationservice.moc"
