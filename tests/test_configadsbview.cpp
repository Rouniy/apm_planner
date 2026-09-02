#include <QtTest>

#include "comm/AdsbIdentificationClient.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"
#include "ui/configuration/ConfigADSBView.h"
#include "ui/configuration/ConfigADSBViewModel.h"
#include "ui/configuration/ConfigFriendlyParamsView.h"
#include "ui/configuration/ConfigFriendlyParamsViewModel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>

#include <cstring>

namespace {

ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:ADSB_TYPE" humanName="ADSB Type" documentation="Type of ADS-B hardware" user="Standard">
          <values><value code="0">Disabled</value><value code="1">uAvionix-MAVLink</value></values>
        </param>
        <param name="ArduCopter:ADSB_OPTIONS" humanName="ADS-B Options" documentation="Options" user="Advanced">
          <field name="Bitmask">0:Ping200X Send GPS,1:Squawk 7400 on RC failsafe</field>
        </param>
        <param name="ArduCopter:AVD_ENABLE" humanName="Enable Avoidance" documentation="Avoidance" user="Standard">
          <values><value code="0">Disabled</value><value code="1">Enabled</value></values>
        </param>
        <param name="ArduCopter:AVD_W_TIME" humanName="Time Horizon Warn" documentation="Warn time" user="Advanced">
          <field name="Range">5 120</field><field name="Units">s</field>
        </param>
        <param name="ArduCopter:GAIN" humanName="Gain" documentation="Unrelated" user="Standard">
          <field name="Range">0 10</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(&buffer, QStringLiteral("ArduCopter"));
}

ParameterRecord record(const QString &name, const QVariant &value, ParameterType type,
                       int component = 1)
{
    ParameterRecord result;
    result.key.componentId = static_cast<quint8>(component);
    result.key.name = name;
    result.value = value;
    result.type = type;
    return result;
}

QList<ParameterRecord> vehicleSnapshot()
{
    return {
        record(QStringLiteral("GAIN"), 1.0, ParameterType::Real32),
        record(QStringLiteral("AVD_W_TIME"), 30.0, ParameterType::Real32),
        record(QStringLiteral("ADSB_OPTIONS"), 3, ParameterType::UInt16),
        record(QStringLiteral("FRSKY_OPTIONS"), 0, ParameterType::UInt8),
        record(QStringLiteral("ADSB_TYPE"), 1, ParameterType::UInt8),
        record(QStringLiteral("ADSB_NOMETA"), 7, ParameterType::Int32),
        record(QStringLiteral("AVD_ENABLE"), 0, ParameterType::UInt8),
        record(QStringLiteral("adsb_log"), 1, ParameterType::Int8),
        record(QStringLiteral("SYSID_THISMAV"), 1, ParameterType::Int16),
    };
}

VehicleEndpoint endpoint(int linkId, int systemId = 42, int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        std::memset(&message, 0, sizeof(message));
    }
    return message;
}

mavlink_message_t flightIdReply(const char *text)
{
    char field[9];
    std::memset(field, 0, sizeof(field));
    std::strncpy(field, text, 8);
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_uavionix_adsb_out_cfg_flightid_pack(42, 1, &message, field);
    return message;
}

mavlink_message_t registrationReply(const char *text)
{
    char field[9];
    std::memset(field, 0, sizeof(field));
    std::strncpy(field, text, 8);
    mavlink_message_t message;
    std::memset(&message, 0, sizeof(message));
    mavlink_msg_uavionix_adsb_out_cfg_registration_pack(42, 1, &message, field);
    return message;
}

QWidget *parameterRow(QWidget *editor, const QString &name)
{
    const QList<QWidget *> widgets = editor->findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (widget->property("parameterName").toString() == name) {
            return widget;
        }
    }
    return nullptr;
}

// Answers the modal clear confirmation with Yes or No and counts it.
class MessageBoxAnswerer final : public QObject
{
public:
    MessageBoxAnswerer(const QString &title, QMessageBox::StandardButton answer,
                       QObject *parent = nullptr)
        : QObject(parent), m_title(title), m_answer(answer)
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, &MessageBoxAnswerer::poll);
        m_timer.start();
    }
    int seen() const { return m_seen; }

private:
    void poll()
    {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (box && box->isVisible() && box->windowTitle() == m_title) {
                ++m_seen;
                m_lastText = box->text();
                if (QAbstractButton *button = box->button(m_answer)) {
                    button->click();
                }
            }
        }
    }

public:
    QString m_lastText;

private:
    QString m_title;
    QMessageBox::StandardButton m_answer;
    QTimer m_timer;
    int m_seen = 0;
};

struct Fixture
{
    VehicleTargetManager targets;
    QVector<QByteArray> frames;
    ExactLinkTransmitter transmitter;
    AdsbIdentificationClient client;
    ConfigADSBView view;
    QSignalSpy refreshRequested;
    QSignalSpy writeRequested;
    QSignalSpy writeParamsRequested;

    Fixture()
        : transmitter([this](int, const QByteArray &bytes) {
              frames.append(bytes);
              return true;
          }),
          client(&targets, &transmitter),
          view(&client, catalogFixture()),
          refreshRequested(&view, &ConfigADSBView::refreshRequested),
          writeRequested(&view, &ConfigADSBView::writeRequested),
          writeParamsRequested(&view, &ConfigADSBView::writeParamsRequested)
    {
        client.setLocalIdentity(250, 190);
        client.setResendDelayMs(5);
        view.resize(1000, 700);
    }

    void connectVehicle()
    {
        targets.observeEndpoint(endpoint(9), true);
        client.bind(targets.acquireTarget());
        view.setConnected(true);
        view.setParameterSnapshot(vehicleSnapshot(), 1);
    }

    QString status() const { return view.viewModel()->status(); }
    QLineEdit *search() const { return view.findChild<QLineEdit *>(QStringLiteral("adsbSearch")); }
    QLineEdit *flightIdEdit() const { return view.findChild<QLineEdit *>(QStringLiteral("flightIdEdit")); }
    QLineEdit *registrationEdit() const
    {
        return view.findChild<QLineEdit *>(QStringLiteral("aircraftRegistrationEdit"));
    }
    QPushButton *button(const QString &name) const { return view.findChild<QPushButton *>(name); }
    mavlink_message_t message(int index) const { return decodeFrame(frames.at(index)); }
};

} // namespace

class ConfigADSBViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void staticRulesMatchMissionPlanner();
    void inventoryMatchesMissionPlanner();
    void snapshotFiltersToAdsbAndKeepsMetadataEditors();
    void searchAppliesMinimumTwoCharacters();
    void refreshAndWriteParamsEmitRealSignals();
    void bulkWriteOutcomeFollowsMissionPlanner();
    void identificationSaveFollowsMissionPlanner();
    void clearConfirmationFollowsMissionPlanner();
    void deletingThePageDuringTheClearConfirmationIsSafe();
    void repliesUpdateFieldsOnlyForCurrentLease();
    void activateRequestsIdentificationAndDeactivateCancels();
    void withoutClientEverythingStaysOffline();
};

void ConfigADSBViewTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ConfigADSBView"));
}

void ConfigADSBViewTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void ConfigADSBViewTest::staticRulesMatchMissionPlanner()
{
    QVERIFY(ConfigADSBViewModel::IsAdsbParameter(QStringLiteral("ADSB_TYPE")));
    QVERIFY(ConfigADSBViewModel::IsAdsbParameter(QStringLiteral("adsb_log")));
    QVERIFY(ConfigADSBViewModel::IsAdsbParameter(QStringLiteral("AVD_ENABLE")));
    QVERIFY(!ConfigADSBViewModel::IsAdsbParameter(QStringLiteral("XADSB_TYPE")));
    QVERIFY(!ConfigADSBViewModel::IsAdsbParameter(QStringLiteral("AVDX")));
    QVERIFY(!ConfigADSBViewModel::IsAdsbParameter(QString()));
    QVERIFY(ConfigADSBViewModel::IsForcedBitmaskParameter(QStringLiteral("ADSB_OPTIONS")));
    QVERIFY(ConfigADSBViewModel::IsForcedBitmaskParameter(QStringLiteral("adsb_rf_capable")));
    QVERIFY(ConfigADSBViewModel::IsForcedBitmaskParameter(QStringLiteral("ADSB_RF_SELECT")));
    QVERIFY(!ConfigADSBViewModel::IsForcedBitmaskParameter(QStringLiteral("ADSB_TYPE")));

    QCOMPARE(ConfigADSBViewModel::SearchTerm(QStringLiteral(" a ")), QString());
    QCOMPARE(ConfigADSBViewModel::SearchTerm(QStringLiteral("ab")), QStringLiteral("ab"));
    QCOMPARE(ConfigADSBViewModel::SearchTerm(QStringLiteral("  avd  ")), QStringLiteral("avd"));
    QCOMPARE(ConfigADSBViewModel::SearchMinimumLength, 2);
    QCOMPARE(ConfigADSBViewModel::Title, QStringLiteral("ADSB"));
    QCOMPARE(ConfigADSBViewModel::Intro,
             QStringLiteral("ADS-B receiver / avoidance. Populated on connect."));

    const QList<ConfigFriendlyParameterValue> filtered =
        ConfigADSBViewModel::FilterSnapshot(vehicleSnapshot());
    QStringList names;
    for (const ConfigFriendlyParameterValue &value : filtered) {
        names << value.name;
    }
    QCOMPARE(names, QStringList() << QStringLiteral("adsb_log") << QStringLiteral("ADSB_NOMETA")
                                  << QStringLiteral("ADSB_OPTIONS") << QStringLiteral("ADSB_TYPE")
                                  << QStringLiteral("AVD_ENABLE") << QStringLiteral("AVD_W_TIME"));

    ParamField enable;
    enable.name = QStringLiteral("AVD_ENABLE");
    enable.value = 1;
    ParamField type;
    type.name = QStringLiteral("ADSB_TYPE");
    type.value = 1;
    ParamField missing;
    missing.name = QStringLiteral("ADSB_MISSING");
    const QVariantList changes = ConfigADSBViewModel::OrderedWriteChanges({enable, type, missing});
    QCOMPARE(changes.size(), 2);
    QCOMPARE(changes.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("ADSB_TYPE"));
    QCOMPARE(changes.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("AVD_ENABLE"));
    QCOMPARE(changes.at(1).toMap().value(QStringLiteral("value")).toInt(), 1);

    // Deterministic case-insensitive order inside both groups, whatever the
    // input order was.
    ParamField warn;
    warn.name = QStringLiteral("AVD_W_TIME");
    warn.value = 30.0;
    ParamField lower;
    lower.name = QStringLiteral("adsb_log");
    lower.value = 1;
    ParamField rfEnable;
    rfEnable.name = QStringLiteral("ADSB_RF_ENABLE");
    rfEnable.value = 2;
    const QVariantList ordered = ConfigADSBViewModel::OrderedWriteChanges(
        {warn, rfEnable, type, enable, lower});
    QStringList orderedNames;
    for (const QVariant &change : ordered) {
        orderedNames << change.toMap().value(QStringLiteral("name")).toString();
    }
    QCOMPARE(orderedNames, QStringList() << QStringLiteral("adsb_log") << QStringLiteral("ADSB_TYPE")
                                         << QStringLiteral("AVD_W_TIME")
                                         << QStringLiteral("ADSB_RF_ENABLE")
                                         << QStringLiteral("AVD_ENABLE"));
}

void ConfigADSBViewTest::inventoryMatchesMissionPlanner()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigADSBView"));
    auto *layout = qobject_cast<QVBoxLayout *>(view.layout());
    QVERIFY(layout);
    QCOMPARE(layout->contentsMargins(), QMargins(16, 16, 16, 16));

    auto *title = view.findChild<QLabel *>(QStringLiteral("adsbTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("ADSB"));
    QVERIFY(fixture.button(QStringLiteral("writeParamsButton")));
    QCOMPARE(fixture.button(QStringLiteral("writeParamsButton"))->text(), QStringLiteral("Write Params"));
    QVERIFY(fixture.button(QStringLiteral("refreshParamsButton")));
    QCOMPARE(fixture.button(QStringLiteral("refreshParamsButton"))->text(), QStringLiteral("Refresh Params"));
    auto *find = view.findChild<QLabel *>(QStringLiteral("findLabel"));
    QVERIFY(find);
    QCOMPARE(find->text(), QStringLiteral("Find"));
    QVERIFY(fixture.search());
    QCOMPARE(fixture.search()->minimumWidth(), 220);
    QCOMPARE(fixture.search()->maximumWidth(), 220);
    QCOMPARE(fixture.search()->placeholderText(), QStringLiteral("search (min 2 chars)"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("adsbStatus")));

    auto *flightIdLabel = view.findChild<QLabel *>(QStringLiteral("flightIdLabel"));
    QVERIFY(flightIdLabel);
    QCOMPARE(flightIdLabel->text(), QStringLiteral("Flight Identification"));
    QVERIFY(fixture.flightIdEdit());
    QCOMPARE(fixture.flightIdEdit()->minimumWidth(), 160);
    QCOMPARE(fixture.flightIdEdit()->maxLength(), 8);
    QCOMPARE(fixture.button(QStringLiteral("saveFlightIdButton"))->text(), QStringLiteral("Save"));
    auto *registrationLabel = view.findChild<QLabel *>(QStringLiteral("aircraftRegistrationLabel"));
    QVERIFY(registrationLabel);
    QCOMPARE(registrationLabel->text(), QStringLiteral("Aircraft Registration"));
    QCOMPARE(registrationLabel->contentsMargins().left(), 12);
    QVERIFY(fixture.registrationEdit());
    QCOMPARE(fixture.registrationEdit()->minimumWidth(), 160);
    QCOMPARE(fixture.registrationEdit()->maxLength(), 8);
    QCOMPARE(fixture.button(QStringLiteral("saveAircraftRegistrationButton"))->text(), QStringLiteral("Save"));

    auto *intro = view.findChild<QLabel *>(QStringLiteral("adsbIntro"));
    QVERIFY(intro);
    QCOMPARE(intro->text(), QStringLiteral("ADS-B receiver / avoidance. Populated on connect."));
    QVERIFY(intro->wordWrap());
    QVERIFY(view.findChild<QScrollArea *>(QStringLiteral("adsbFieldsScroll")));
    QVERIFY(view.standardEditor());
    QVERIFY(view.advancedEditor());
    QVERIFY(view.otherEditor());
    QCOMPARE(view.standardEditor()->objectName(), QStringLiteral("adsbStandardParams"));
    QCOMPARE(view.advancedEditor()->objectName(), QStringLiteral("adsbAdvancedParams"));
    QCOMPARE(view.otherEditor()->objectName(), QStringLiteral("adsbOtherParams"));
    QVERIFY(!view.standardEditor()->findChild<QLineEdit *>(QStringLiteral("searchBox"))->isVisibleTo(&view));

    QCOMPARE(view.visibleParameterCount(), 0);
    QVERIFY(fixture.status().isEmpty());
    QVERIFY(!view.isConnected());
    QCOMPARE(view.viewModel()->client(), &fixture.client);
    QCOMPARE(fixture.frames.size(), 0);
}

void ConfigADSBViewTest::snapshotFiltersToAdsbAndKeepsMetadataEditors()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    view.setParameterSnapshot(vehicleSnapshot(), 1);

    QStringList shown = view.shownParameterNames();
    shown.sort();
    QCOMPARE(shown, QStringList() << QStringLiteral("ADSB_LOG") << QStringLiteral("ADSB_NOMETA")
                                  << QStringLiteral("ADSB_OPTIONS") << QStringLiteral("ADSB_TYPE")
                                  << QStringLiteral("AVD_ENABLE") << QStringLiteral("AVD_W_TIME"));
    QCOMPARE(view.visibleParameterCount(), 6);
    QCOMPARE(view.selectedComponent(), 1);

    // Standard-level parameters keep their combo editors.
    QWidget *type = parameterRow(view.standardEditor(), QStringLiteral("ADSB_TYPE"));
    QVERIFY(type);
    auto *typeCombo = type->findChild<QComboBox *>(QStringLiteral("valueComboBox"));
    QVERIFY(typeCombo);
    QCOMPARE(typeCombo->count(), 2);
    QVERIFY(parameterRow(view.standardEditor(), QStringLiteral("AVD_ENABLE")));
    QVERIFY(!parameterRow(view.standardEditor(), QStringLiteral("ADSB_OPTIONS")));

    // Advanced-level parameters keep bitmask and numeric editors.
    QWidget *options = parameterRow(view.advancedEditor(), QStringLiteral("ADSB_OPTIONS"));
    QVERIFY(options);
    QVERIFY(options->findChild<QAbstractButton *>(QStringLiteral("bitmaskButton")));
    QWidget *warnTime = parameterRow(view.advancedEditor(), QStringLiteral("AVD_W_TIME"));
    QVERIFY(warnTime);
    auto *warnEditor = warnTime->findChild<QDoubleSpinBox *>(QStringLiteral("numericEditor"));
    QVERIFY(warnEditor);
    QCOMPARE(warnEditor->value(), 30.0);
    QVERIFY(!parameterRow(view.advancedEditor(), QStringLiteral("ADSB_TYPE")));

    // Uncatalogued ADS-B parameters are still listed with their raw name.
    QWidget *nometa = parameterRow(view.otherEditor(), QStringLiteral("ADSB_NOMETA"));
    QVERIFY(nometa);
    QVERIFY(nometa->findChild<QDoubleSpinBox *>(QStringLiteral("numericEditor")));
    QVERIFY(parameterRow(view.otherEditor(), QStringLiteral("ADSB_LOG")));
    QVERIFY(!parameterRow(view.otherEditor(), QStringLiteral("ADSB_TYPE")));

    // Unrelated parameters never appear anywhere.
    QVERIFY(!parameterRow(&view, QStringLiteral("GAIN")));
    QVERIFY(!parameterRow(&view, QStringLiteral("FRSKY_OPTIONS")));
    QVERIFY(!parameterRow(&view, QStringLiteral("SYSID_THISMAV")));

    // An empty snapshot clears every section.
    view.setParameterSnapshot({}, 1);
    QCOMPARE(view.visibleParameterCount(), 0);
    QVERIFY(view.shownParameterNames().isEmpty());
}

void ConfigADSBViewTest::searchAppliesMinimumTwoCharacters()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    view.setParameterSnapshot(vehicleSnapshot(), 1);
    QCOMPARE(view.visibleParameterCount(), 6);

    fixture.search()->setText(QStringLiteral("a"));
    QCOMPARE(view.viewModel()->searchTerm(), QString());
    QCOMPARE(view.visibleParameterCount(), 6); // shorter than 2 characters shows everything

    fixture.search()->setText(QStringLiteral("avd"));
    QCOMPARE(view.viewModel()->searchTerm(), QStringLiteral("avd"));
    QCOMPARE(view.visibleParameterCount(), 2);
    QVERIFY(parameterRow(view.standardEditor(), QStringLiteral("AVD_ENABLE"))->isVisibleTo(&view));
    QVERIFY(!parameterRow(view.standardEditor(), QStringLiteral("ADSB_TYPE"))->isVisibleTo(&view));

    fixture.search()->setText(QStringLiteral("  ADSB_TY  "));
    QCOMPARE(view.visibleParameterCount(), 1);

    fixture.search()->setText(QStringLiteral("Horizon")); // label match
    QCOMPARE(view.visibleParameterCount(), 1);
    QVERIFY(parameterRow(view.advancedEditor(), QStringLiteral("AVD_W_TIME"))->isVisibleTo(&view));

    fixture.search()->setText(QStringLiteral("Warn time")); // description-only match
    QCOMPARE(view.visibleParameterCount(), 1);
    QVERIFY(parameterRow(view.advancedEditor(), QStringLiteral("AVD_W_TIME"))->isVisibleTo(&view));

    fixture.search()->clear();
    QCOMPARE(view.visibleParameterCount(), 6);

    // The term survives a snapshot refresh.
    fixture.search()->setText(QStringLiteral("avd"));
    view.setParameterSnapshot(vehicleSnapshot(), 1);
    QCOMPARE(view.visibleParameterCount(), 2);
}

void ConfigADSBViewTest::refreshAndWriteParamsEmitRealSignals()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    view.setParameterSnapshot(vehicleSnapshot(), 1);

    fixture.button(QStringLiteral("refreshParamsButton"))->click();
    QCOMPARE(fixture.refreshRequested.count(), 1);
    QCOMPARE(fixture.refreshRequested.at(0).at(0).toInt(), 1);

    // Write Params is refused offline, like MP10.
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 0);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("adsbStatus"))->text(), QStringLiteral("offline"));

    view.setConnected(true);
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 1);
    QCOMPARE(fixture.writeParamsRequested.at(0).at(0).toInt(), 1);
    const QVariantList changes = fixture.writeParamsRequested.at(0).at(1).toList();
    QCOMPARE(changes.size(), 6);
    QStringList names;
    for (const QVariant &change : changes) {
        const QVariantMap map = change.toMap();
        QVERIFY(map.contains(QStringLiteral("name")));
        QVERIFY(map.contains(QStringLiteral("value")));
        names << map.value(QStringLiteral("name")).toString();
    }
    // MP10 order: alphabetical (case-insensitive), ENABLE parameters last.
    QCOMPARE(names, QStringList() << QStringLiteral("ADSB_LOG") << QStringLiteral("ADSB_NOMETA")
                                  << QStringLiteral("ADSB_OPTIONS") << QStringLiteral("ADSB_TYPE")
                                  << QStringLiteral("AVD_W_TIME") << QStringLiteral("AVD_ENABLE"));
    QCOMPARE(fixture.writeRequested.count(), 0); // the bulk write is not a per-row write
    QVERIFY(view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("adsbStatus"))->text(),
             QStringLiteral("Writing 6 parameter(s)…"));
    fixture.button(QStringLiteral("writeParamsButton"))->click(); // one batch at a time
    QCOMPARE(fixture.writeParamsRequested.count(), 1);
    view.parameterBatchSubmitted(1, 77);
    view.parameterBatchCompleted(77, 6, 0);
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("adsbStatus"))->text(),
             QStringLiteral("Parameters successfully saved."));

    // A single edited field still flows through the per-row write signal.
    QWidget *type = parameterRow(view.standardEditor(), QStringLiteral("ADSB_TYPE"));
    QVERIFY(type);
    auto *combo = type->findChild<QComboBox *>(QStringLiteral("valueComboBox"));
    QVERIFY(combo);
    combo->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 0)));
    QTRY_COMPARE(fixture.writeRequested.count(), 1);
    QCOMPARE(fixture.writeRequested.at(0).at(1).toString(), QStringLiteral("ADSB_TYPE"));
    QCOMPARE(fixture.writeRequested.at(0).at(2).toInt(), 0);

    // Nothing to write: MP10 reports success without sending anything.
    view.setParameterSnapshot({}, 1);
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 1);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("adsbStatus"))->text(),
             QStringLiteral("Parameters successfully saved."));
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
}

void ConfigADSBViewTest::bulkWriteOutcomeFollowsMissionPlanner()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));
    view.setConnected(true);
    view.setParameterSnapshot(vehicleSnapshot(), 1);

    // Outcome events for batches this page did not start are ignored.
    view.parameterBatchSubmitted(1, 5);
    view.parameterBatchCompleted(5, 1, 0);
    view.parameterBatchCancelled(5);
    view.parameterWriteSubmissionFailed(QStringLiteral("nope"));
    QVERIFY(status->text().isEmpty());

    // Failure of at least one parameter -> "write failed" (MP10).
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 1);
    view.parameterBatchSubmitted(1, 11);
    QCOMPARE(view.viewModel()->pendingBulkBatchId(), 11u);
    view.parameterBatchCompleted(12, 6, 0); // another batch: still pending
    QVERIFY(view.viewModel()->hasPendingBulkWrite());
    view.parameterBatchCompleted(11, 5, 1);
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(status->text(), QStringLiteral("write failed"));

    // Cancelled batch.
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 2);
    view.parameterBatchSubmitted(1, 21);
    view.parameterBatchCancelled(21);
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(status->text(), QStringLiteral("write failed"));

    // Submission refused before a batch id exists.
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 3);
    view.parameterWriteSubmissionFailed(QStringLiteral("target changed"));
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(status->text(), QStringLiteral("write failed: target changed"));

    // Completion without a reported batch id is accepted for the pending write.
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 4);
    view.parameterBatchCompleted(99, 6, 0);
    QCOMPARE(status->text(), QStringLiteral("Parameters successfully saved."));

    // Disconnecting while a batch is pending fails it.
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 5);
    view.setConnected(false);
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QCOMPARE(status->text(), QStringLiteral("write failed"));

    // A target change drops the pending state without a status.
    view.setConnected(true);
    fixture.button(QStringLiteral("writeParamsButton"))->click();
    QCOMPARE(fixture.writeParamsRequested.count(), 6);
    view.parameterTargetChanged();
    QVERIFY(!view.viewModel()->hasPendingBulkWrite());
    QVERIFY(status->text().isEmpty());
}

void ConfigADSBViewTest::identificationSaveFollowsMissionPlanner()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));

    // Offline: MP10 answers "offline" and sends nothing.
    fixture.flightIdEdit()->setText(QStringLiteral("N123AB"));
    fixture.button(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(status->text(), QStringLiteral("offline"));
    QCOMPARE(fixture.frames.size(), 0);

    fixture.connectVehicle();
    QVERIFY(fixture.client.isBound());
    fixture.button(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(fixture.frames.size(), 1);
    QCOMPARE(status->text(), QStringLiteral("Sending Flight ID…"));
    QVERIFY(!fixture.button(QStringLiteral("saveFlightIdButton"))->isEnabled());
    QVERIFY(!fixture.button(QStringLiteral("saveAircraftRegistrationButton"))->isEnabled());
    QTRY_COMPARE(fixture.frames.size(), 3);
    QTRY_COMPARE(status->text(), QStringLiteral("Flight ID sent."));
    QVERIFY(fixture.button(QStringLiteral("saveFlightIdButton"))->isEnabled());
    const mavlink_message_t first = fixture.message(0);
    QCOMPARE(first.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_FLIGHTID));
    mavlink_uavionix_adsb_out_cfg_flightid_t decoded;
    std::memset(&decoded, 0, sizeof(decoded));
    mavlink_msg_uavionix_adsb_out_cfg_flightid_decode(&first, &decoded);
    QCOMPARE(QByteArray(decoded.flight_id, 9), QByteArray("N123AB\0\0\0", 9));
    QCOMPARE(fixture.message(1).msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_FLIGHTID));
    const mavlink_message_t third = fixture.message(2);
    QCOMPARE(third.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&third), quint32(10005));

    fixture.registrationEdit()->setText(QStringLiteral("N8644B"));
    fixture.button(QStringLiteral("saveAircraftRegistrationButton"))->click();
    QCOMPARE(status->text(), QStringLiteral("Sending Aircraft registration…"));
    QTRY_COMPARE(fixture.frames.size(), 6);
    QTRY_COMPARE(status->text(), QStringLiteral("Aircraft registration sent."));
    QCOMPARE(fixture.message(3).msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG_REGISTRATION));
    const mavlink_message_t registrationGet = fixture.message(5);
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&registrationGet), quint32(10004));

    // A disconnect during a write cancels it.
    fixture.client.setResendDelayMs(1000);
    fixture.button(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(fixture.frames.size(), 7);
    view.setConnected(false);
    QCOMPARE(status->text(), QStringLiteral("Flight ID write cancelled."));
    QVERIFY(!fixture.client.isBusy());
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 7);
}

void ConfigADSBViewTest::clearConfirmationFollowsMissionPlanner()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));
    fixture.connectVehicle();

    // Declined: nothing is sent.
    {
        MessageBoxAnswerer decline(QStringLiteral("Clear Flight Identification"), QMessageBox::No);
        fixture.flightIdEdit()->setText(QStringLiteral("   "));
        fixture.button(QStringLiteral("saveFlightIdButton"))->click();
        QCOMPARE(decline.seen(), 1);
        QCOMPARE(decline.m_lastText, QStringLiteral(
            "The Flight Identification field is empty. Send an empty value and clear the device setting?"));
        QCOMPARE(status->text(), QStringLiteral("Flight ID was not changed."));
        QCOMPARE(fixture.frames.size(), 0);
    }
    {
        MessageBoxAnswerer decline(QStringLiteral("Clear Aircraft Registration"), QMessageBox::No);
        fixture.registrationEdit()->clear();
        fixture.button(QStringLiteral("saveAircraftRegistrationButton"))->click();
        QCOMPARE(decline.seen(), 1);
        QCOMPARE(decline.m_lastText, QStringLiteral(
            "The Aircraft Registration field is empty. Send an empty value and clear the device setting?"));
        QCOMPARE(status->text(), QStringLiteral("Aircraft registration was not changed."));
        QCOMPARE(fixture.frames.size(), 0);
    }

    // Accepted: the empty value is sent (9 NUL bytes) and read back.
    {
        MessageBoxAnswerer accept(QStringLiteral("Clear Flight Identification"), QMessageBox::Yes);
        fixture.flightIdEdit()->clear();
        fixture.button(QStringLiteral("saveFlightIdButton"))->click();
        QCOMPARE(accept.seen(), 1);
        QTRY_COMPARE(fixture.frames.size(), 3);
        QTRY_COMPARE(status->text(), QStringLiteral("Flight ID sent."));
        mavlink_uavionix_adsb_out_cfg_flightid_t decoded;
        std::memset(&decoded, 0, sizeof(decoded));
        const mavlink_message_t first = fixture.message(0);
        mavlink_msg_uavionix_adsb_out_cfg_flightid_decode(&first, &decoded);
        QCOMPARE(QByteArray(decoded.flight_id, 9), QByteArray(9, '\0'));
    }

    // A non-empty value never asks.
    {
        MessageBoxAnswerer watcher(QStringLiteral("Clear Flight Identification"), QMessageBox::No);
        fixture.flightIdEdit()->setText(QStringLiteral("ABC"));
        fixture.button(QStringLiteral("saveFlightIdButton"))->click();
        QTRY_COMPARE(fixture.frames.size(), 6);
        QCOMPARE(watcher.seen(), 0);
    }
}

void ConfigADSBViewTest::deletingThePageDuringTheClearConfirmationIsSafe()
{
    VehicleTargetManager targets;
    QVector<QByteArray> frames;
    ExactLinkTransmitter transmitter([&frames](int, const QByteArray &bytes) {
        frames.append(bytes);
        return true;
    });
    AdsbIdentificationClient client(&targets, &transmitter);
    client.setResendDelayMs(5);
    QVERIFY(targets.observeEndpoint(endpoint(9), true));
    QVERIFY(client.bind(targets.acquireTarget()));

    QPointer<ConfigADSBView> view(new ConfigADSBView(&client, catalogFixture()));
    view->setConnected(true);
    view->findChild<QLineEdit *>(QStringLiteral("flightIdEdit"))->clear();

    // Delete the page while its confirmation box is open (e.g. the vehicle
    // disconnected and the backstage tore the page down).
    QTimer deleter;
    deleter.setInterval(10);
    int deleted = 0;
    connect(&deleter, &QTimer::timeout, &deleter, [&view, &deleted, &deleter]() {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (box && box->isVisible()
                && box->windowTitle() == QStringLiteral("Clear Flight Identification")) {
                ++deleted;
                deleter.stop();
                delete view.data(); // closes the box and destroys the view model
                return;
            }
        }
    });
    deleter.start();

    view->findChild<QPushButton *>(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(deleted, 1);
    QVERIFY(view.isNull());
    QTest::qWait(30);
    QCOMPARE(frames.size(), 0);
    QVERIFY(!client.isBusy());
    QVERIFY(client.isBound()); // the client outlives the page untouched
}

void ConfigADSBViewTest::repliesUpdateFieldsOnlyForCurrentLease()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));
    fixture.connectVehicle();

    fixture.client.observeMessage(9, flightIdReply("ABC12   "));
    QCOMPARE(fixture.flightIdEdit()->text(), QStringLiteral("ABC12"));
    QCOMPARE(view.viewModel()->flightId(), QStringLiteral("ABC12"));
    QCOMPARE(status->text(), QStringLiteral("Flight ID read from the device."));
    fixture.client.observeMessage(9, registrationReply("N8644B "));
    QCOMPARE(fixture.registrationEdit()->text(), QStringLiteral("N8644B"));
    QCOMPARE(status->text(), QStringLiteral("Aircraft registration read from the device."));

    // Disconnected pages ignore replies.
    view.setConnected(false);
    fixture.client.observeMessage(9, flightIdReply("LATE1"));
    QCOMPARE(fixture.flightIdEdit()->text(), QStringLiteral("ABC12"));
    view.setConnected(true);
    fixture.client.observeMessage(9, flightIdReply("FRESH"));
    QCOMPARE(fixture.flightIdEdit()->text(), QStringLiteral("FRESH"));

    // A target switch invalidates the lease: replies for the old vehicle are
    // dropped and the page clears the identification it showed.
    QVERIFY(fixture.targets.observeEndpoint(endpoint(4)));
    QVERIFY(fixture.targets.selectTarget(4, 42, 1));
    QVERIFY(!fixture.client.isBound());
    fixture.client.observeMessage(9, flightIdReply("LATE2"));
    QCOMPARE(fixture.flightIdEdit()->text(), QStringLiteral("FRESH"));
    view.parameterTargetChanged();
    QCOMPARE(fixture.flightIdEdit()->text(), QString());
    QCOMPARE(fixture.registrationEdit()->text(), QString());
    QCOMPARE(view.visibleParameterCount(), 0);
    QVERIFY(status->text().isEmpty());
}

void ConfigADSBViewTest::activateRequestsIdentificationAndDeactivateCancels()
{
    Fixture fixture;
    ConfigADSBView &view = fixture.view;
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));

    // Not connected: activation stays quiet, like MP10 RequestIdentification.
    view.activate();
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(status->text().isEmpty());
    view.deactivate();

    fixture.connectVehicle();
    view.activate();
    QCOMPARE(fixture.frames.size(), 2);
    const mavlink_message_t firstGet = fixture.message(0);
    const mavlink_message_t secondGet = fixture.message(1);
    QCOMPARE(firstGet.msgid, quint32(MAVLINK_MSG_ID_UAVIONIX_ADSB_GET));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&firstGet), quint32(10004));
    QCOMPARE(mavlink_msg_uavionix_adsb_get_get_ReqMessageId(&secondGet), quint32(10005));
    QCOMPARE(status->text(), QStringLiteral("Reading Flight ID and aircraft registration…"));

    fixture.client.setResendDelayMs(1000);
    fixture.flightIdEdit()->setText(QStringLiteral("N1"));
    fixture.button(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(fixture.frames.size(), 3);
    QVERIFY(fixture.client.isBusy());
    view.deactivate();
    QVERIFY(!fixture.client.isBusy());
    QCOMPARE(status->text(), QStringLiteral("Flight ID write cancelled."));
    QTest::qWait(30);
    QCOMPARE(fixture.frames.size(), 3);
    QVERIFY(fixture.button(QStringLiteral("saveFlightIdButton"))->isEnabled());

    // Re-activation reads again; a running read does not block saves.
    view.activate();
    QCOMPARE(fixture.frames.size(), 5);
}

void ConfigADSBViewTest::withoutClientEverythingStaysOffline()
{
    ConfigADSBView view(nullptr, catalogFixture());
    QLabel *status = view.findChild<QLabel *>(QStringLiteral("adsbStatus"));
    view.setConnected(true);
    view.setParameterSnapshot(vehicleSnapshot(), 1);
    QCOMPARE(view.visibleParameterCount(), 6);
    view.findChild<QPushButton *>(QStringLiteral("saveFlightIdButton"))->click();
    QCOMPARE(status->text(), QStringLiteral("offline"));
    view.activate();
    view.deactivate();
    view.parameterTargetChanged();
    QVERIFY(!view.viewModel()->isIdentificationAvailable());
    QVERIFY(!view.viewModel()->isIdentificationBusy());
}

QTEST_MAIN(ConfigADSBViewTest)
#include "test_configadsbview.moc"
