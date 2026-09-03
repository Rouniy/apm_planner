#include <QtTest>

#include "ui/SerialOutputCotWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>

#include <memory>

namespace {

struct FixtureState
{
    QList<QByteArray> payloads;
    QVariantMap settings;
    int saves = 0;
};

VehicleEndpoint endpoint(int systemId, int componentId)
{
    VehicleEndpoint value;
    value.linkId = 42;
    value.linkName = QStringLiteral("Vehicle Link");
    value.systemId = systemId;
    value.componentId = componentId;
    return value;
}

struct WindowFixture
{
    QStringList ports = {QStringLiteral("ttyTEST")};
    CotOutputSource source;
    std::shared_ptr<FixtureState> state = std::make_shared<FixtureState>();

    WindowFixture()
    {
        source.linkId = 42;
        source.linkName = QStringLiteral("Vehicle Link");
        source.endpoints = {endpoint(7, 1), endpoint(7, 42), endpoint(9, 1)};
    }

    SerialOutputCotWindow::Dependencies dependencies()
    {
        SerialOutputCotWindow::Dependencies result;
        result.transportFactory = CotOutputService::ProductionTransportFactory();
        result.sender = [shared = state](CotOutputTransport *,
                                         const QByteArray &payload) {
            shared->payloads.append(payload);
            return CotOutputTransport::SendResult::Sent;
        };
        result.clock = []() {
            return QDateTime(QDate(2026, 8, 21), QTime(12, 34, 56), Qt::UTC);
        };
        result.viewModel.enumeratePorts = [this]() { return ports; };
        result.viewModel.resolveSource = [this]() { return source; };
        result.viewModel.validateSelection = [](
            const CotOutputTransport::Settings &) { return QString(); };
        result.viewModel.loadSettings = [shared = state]() {
            return shared->settings;
        };
        result.viewModel.saveSettings = [shared = state](
            const QVariantMap &settings, QString *error) {
            for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
                shared->settings.insert(it.key(), it.value());
            }
            ++shared->saves;
            if (error) {
                error->clear();
            }
            return true;
        };
        return result;
    }
};

mavlink_message_t positionMessage(int systemId, int componentId)
{
    mavlink_global_position_int_t position{};
    position.lat = 351234567;
    position.lon = 337654321;
    position.alt = 123450;
    position.vx = 300;
    position.vy = 400;
    position.hdg = 27050;
    mavlink_message_t message{};
    mavlink_msg_global_position_int_encode(
        static_cast<uint8_t>(systemId), static_cast<uint8_t>(componentId),
        &message, &position);
    return message;
}

} // namespace

class SerialOutputCotWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialUiMatchesMp10();
    void identitiesSettingsAndEmissionAreLive();
    void failuresAndIndependentWindowLifetimeAreVisible();
};

void SerialOutputCotWindowTest::initialUiMatchesMp10()
{
    WindowFixture fixture;
    SerialOutputCotWindow window(fixture.dependencies());
    QCOMPARE(window.objectName(), QStringLiteral("SerialOutputCotWindow"));
    QCOMPARE(window.windowTitle(),
             QStringLiteral("Cursor-on-Target / TAK Output"));
    QCOMPARE(window.size(), QSize(720, 820));
    QCOMPARE(window.minimumSize(), QSize(560, 650));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QVERIFY(window.isWindow());
    QVERIFY(qobject_cast<QDialog *>(&window) == nullptr);

    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialOutputCotDescription"))->text(),
             QStringLiteral("Emits CoT 2.0 XML for every MAVLink system through "
                            "UDP multicast/client, TCP client/host, UDP host, or "
                            "a serial port."));
    auto *endpointBox = window.findChild<QComboBox *>(
        QStringLiteral("serialOutputCotEndpoint"));
    QCOMPARE(endpointBox->count(), 6);
    QCOMPARE(endpointBox->itemText(0), QStringLiteral("TAK Multicast"));
    QCOMPARE(endpointBox->itemText(1), QStringLiteral("UDP Client"));
    QCOMPARE(endpointBox->itemText(2), QStringLiteral("UDP Host"));
    QCOMPARE(endpointBox->itemText(3), QStringLiteral("TCP Client"));
    QCOMPARE(endpointBox->itemText(4), QStringLiteral("TCP Host"));
    QCOMPARE(endpointBox->itemText(5), QStringLiteral("ttyTEST"));

    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("serialOutputCotHost"))->text(),
             QStringLiteral("239.2.3.1"));
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("serialOutputCotPort"))->value(), 6969);
    QCOMPARE(window.findChild<QComboBox *>(
                 QStringLiteral("serialOutputCotBaud"))->currentText(),
             QStringLiteral("57600"));
    QCOMPARE(window.findChild<QDoubleSpinBox *>(
                 QStringLiteral("serialOutputCotUpdate"))->value(), 10.0);
    QVERIFY(window.findChild<QCheckBox *>(
        QStringLiteral("serialOutputCotAdvanced"))->isChecked());
    QVERIFY(!window.findChild<QCheckBox *>(
        QStringLiteral("serialOutputCotIndent"))->isChecked());
    QCOMPARE(window.findChild<QPushButton *>(
                 QStringLiteral("serialOutputCotToggle"))->text(),
             QStringLiteral("Connect"));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("serialOutputCotStatus"))->text(),
             QStringLiteral("Stopped."));
}

void SerialOutputCotWindowTest::identitiesSettingsAndEmissionAreLive()
{
    WindowFixture fixture;
    SerialOutputCotWindow window(fixture.dependencies());
    auto *table = window.findChild<QTableView *>(
        QStringLiteral("serialOutputCotIdentities"));
    window.findChild<QPushButton *>(
        QStringLiteral("serialOutputCotRefreshSystems"))->click();
    QCOMPARE(table->model()->rowCount(), 2);
    QCOMPARE(table->model()->data(table->model()->index(0, 0)).toString(),
             QStringLiteral("7"));
    QCOMPARE(table->model()->data(table->model()->index(1, 0)).toString(),
             QStringLiteral("9"));

    window.findChild<QCheckBox *>(
        QStringLiteral("serialOutputCotAdvanced"))->setChecked(false);
    QVERIFY(table->isColumnHidden(CotIdentityModel::TakvColumn));
    QVERIFY(!table->isColumnHidden(CotIdentityModel::EventUidColumn));
    window.findChild<QCheckBox *>(
        QStringLiteral("serialOutputCotAdvanced"))->setChecked(true);
    QVERIFY(!table->isColumnHidden(CotIdentityModel::TakvColumn));

    window.findChild<QLineEdit *>(
        QStringLiteral("serialOutputCotCallsign"))->setText(
            QStringLiteral("Copter"));
    window.findChild<QCheckBox *>(
        QStringLiteral("serialOutputCotIndent"))->setChecked(true);
    window.findChild<QPushButton *>(
        QStringLiteral("serialOutputCotToggle"))->click();
    QVERIFY(window.service()->isRunning());
    QCOMPARE(window.service()->linkId(), 42);
    QCOMPARE(window.service()->activeEndpoints().size(), 3);
    QVERIFY(fixture.state->payloads.isEmpty());

    window.service()->observeMessage(42, positionMessage(7, 1));
    window.service()->observeMessage(42, positionMessage(7, 42));
    window.service()->observeMessage(42, positionMessage(9, 1));
    window.service()->emitNow();
    QCOMPARE(fixture.state->payloads.size(), 3);
    QVERIFY(fixture.state->payloads.at(0).contains("uid=\"MissionPlanner-7\""));
    QVERIFY(fixture.state->payloads.at(1).contains("uid=\"MissionPlanner-7\""));
    QVERIFY(fixture.state->payloads.at(2).contains("uid=\"MissionPlanner-9\""));
    for (const QByteArray &payload : fixture.state->payloads) {
        QVERIFY(payload.endsWith('\n'));
        QVERIFY(payload.contains("callsign=\"Copter-"));
    }
    QVERIFY(window.findChild<QPlainTextEdit *>(
        QStringLiteral("serialOutputCotLastEvent"))->toPlainText()
        .contains(QStringLiteral("<event")));
    QVERIFY(fixture.state->saves >= 1);
    QVERIFY(fixture.state->settings.contains(QStringLiteral("CoTUID")));
}

void SerialOutputCotWindowTest::failuresAndIndependentWindowLifetimeAreVisible()
{
    WindowFixture missing;
    missing.source = CotOutputSource();
    SerialOutputCotWindow noSource(missing.dependencies());
    noSource.findChild<QPushButton *>(
        QStringLiteral("serialOutputCotToggle"))->click();
    QVERIFY(!noSource.service()->isRunning());
    QVERIFY(noSource.viewModel()->statusText().contains(
        QStringLiteral("no current vehicle target")));

    WindowFixture rejected;
    auto dependencies = rejected.dependencies();
    dependencies.viewModel.validateSelection = [](
        const CotOutputTransport::Settings &) {
        return QStringLiteral("Port is busy.");
    };
    SerialOutputCotWindow invalid(dependencies);
    invalid.findChild<QPushButton *>(
        QStringLiteral("serialOutputCotToggle"))->click();
    QCOMPARE(invalid.viewModel()->statusText(),
             QStringLiteral("Unable to start CoT output: Port is busy."));

    WindowFixture firstFixture;
    WindowFixture secondFixture;
    auto *first = new SerialOutputCotWindow(firstFixture.dependencies());
    auto *second = new SerialOutputCotWindow(secondFixture.dependencies());
    QPointer<SerialOutputCotWindow> firstGuard(first);
    QPointer<SerialOutputCotWindow> secondGuard(second);
    first->show();
    second->show();
    first->findChild<QPushButton *>(
        QStringLiteral("serialOutputCotToggle"))->click();
    QVERIFY(first->service()->isRunning());
    QVERIFY(!second->service()->isRunning());
    first->close();
    QTRY_VERIFY(firstGuard.isNull());
    QVERIFY(!secondGuard.isNull());
    second->close();
    QTRY_VERIFY(secondGuard.isNull());
}

QTEST_MAIN(SerialOutputCotWindowTest)
#include "test_serialoutputcotwindow.moc"
