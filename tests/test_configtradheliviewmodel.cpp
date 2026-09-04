#include <QtTest>

#include "ui/configuration/ConfigTradHeliViewModel.h"

#include <QBuffer>
#include <QSignalSpy>

namespace {
ParameterMetaDataCatalog catalogFixture(bool includeTestMode = false)
{
    const QByteArray testOption = includeTestMode
        ? QByteArrayLiteral("<value code=\"5\">Test</value>")
        : QByteArray();
    QByteArray xml = QByteArrayLiteral(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:H_SV_MAN" humanName="Manual Servo Mode">
          <values>
            <value code="0">Disabled</value>
            <value code="1">Passthrough</value>
            <value code="2">Max collective</value>
            <value code="3">Center</value>
            <value code="4">Min collective</value>
            %1
          </values>
        </param>
        <param name="ArduCopter:H_TAIL_TYPE" humanName="Tail Type">
          <values><value code="0">Servo</value><value code="1">DDVP</value></values>
        </param>
        <param name="ArduCopter:H_COL_MIN" humanName="Collective Min">
          <field name="Range">1000 2000</field><field name="Units">PWM</field>
          <field name="Increment">1</field>
        </param>
        <param name="ArduCopter:H_COL_MAX" humanName="Collective Max">
          <field name="Range">1000 2000</field><field name="Units">PWM</field>
        </param>
        <param name="ArduCopter:IM_STB_COL_1" humanName="Stabilize Point 1">
          <field name="Range">0 100</field>
        </param>
      </parameters></vehicles></paramfile>)xml")
        .replace("%1", testOption);
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot(int component = 1)
{
    return {
        {component, QStringLiteral("H_SWASH_TYPE"), 0},
        {component, QStringLiteral("H_SV_MAN"), 0},
        {component, QStringLiteral("H_TAIL_TYPE"), 0},
        {component, QStringLiteral("H_COL_MIN"), 1000},
        {component, QStringLiteral("H_COL_MAX"), 2000},
        {component, QStringLiteral("IM_STB_COL_1"), 10},
        {component, QStringLiteral("IM_STB_COL_2"), 40},
        {component, QStringLiteral("IM_STB_COL_3"), 60},
        {component, QStringLiteral("IM_STB_COL_4"), 90},
        {component, QStringLiteral("IM_ACRO_COL_EXP"), 0.5},
        {component, QStringLiteral("SERVO4_MIN"), 1000},
        {component, QStringLiteral("SERVO4_MAX"), 2000},
        {component, QStringLiteral("SERVO1_REVERSED"), 0},
        {component, QStringLiteral("SERVO1_TRIM"), 1500},
        {component, QStringLiteral("H_SV1_POS"), -15},
        {component, QStringLiteral("H_SV2_POS"), 0},
        {component, QStringLiteral("H_SV3_POS"), 15}
    };
}

ParamField fieldNamed(const ConfigTradHeliViewModel &model,
                      const QString &name)
{
    for (const ParamField &field : model.Fields()) {
        if (field.name == name) {
            return field;
        }
    }
    return {};
}
} // namespace

class ConfigTradHeliViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactFieldInventoryAndAliases();
    void fieldsUseMetadataAndMissingRowsStayDisabled();
    void writesRequireDisarmedExactComponentAndEcho();
    void manualModesAreCapabilityGatedAndDeactivateSafe();
    void submittedBatchOwnsTimeoutAndFailureKeepsManualUncertainty();
    void visualizationUsesRcAndServoSixWithManualRangeCapture();
};

void ConfigTradHeliViewModelTest::exactFieldInventoryAndAliases()
{
    ConfigTradHeliViewModel model;
    QCOMPARE(ConfigTradHeliViewModel::FieldCandidates().size(), 43);
    QCOMPARE(ConfigTradHeliViewModel::ReferenceFieldNames().size(), 43);
    QCOMPARE(ConfigTradHeliViewModel::ReferenceFieldNames().constFirst(),
             QStringLiteral("H_PHANG"));
    QCOMPARE(ConfigTradHeliViewModel::ReferenceFieldNames().constLast(),
             QStringLiteral("HS4_TRIM"));

    model.setParameterSnapshot(snapshot(42), 1);
    QCOMPARE(model.ComponentId(), 42);
    QCOMPARE(model.Fields().size(), 43);
    QCOMPARE(fieldNamed(model, QStringLiteral("IM_STB_COL_1")).name,
             QStringLiteral("IM_STB_COL_1"));
    QCOMPARE(fieldNamed(model, QStringLiteral("SERVO4_MIN")).name,
             QStringLiteral("SERVO4_MIN"));
    QCOMPARE(fieldNamed(model, QStringLiteral("SERVO1_REVERSED")).name,
             QStringLiteral("SERVO1_REVERSED"));
    QCOMPARE(fieldNamed(model, QStringLiteral("SERVO1_TRIM")).name,
             QStringLiteral("SERVO1_TRIM"));
    QVERIFY(model.HasLegacySwash());
    QCOMPARE(model.SwashParameter(), QStringLiteral("H_SWASH_TYPE"));
}

void ConfigTradHeliViewModelTest::fieldsUseMetadataAndMissingRowsStayDisabled()
{
    ConfigTradHeliViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());

    const ParamField min = fieldNamed(model, QStringLiteral("H_COL_MIN"));
    QCOMPARE(min.label, QStringLiteral("Collective Min"));
    QCOMPARE(min.units, QStringLiteral("PWM"));
    QCOMPARE(min.minimum, 1000.0);
    QCOMPARE(min.maximum, 2000.0);
    QVERIFY(!min.readOnly);

    const ParamField modernCurve = fieldNamed(
        model, QStringLiteral("IM_STB_COL_1"));
    QCOMPARE(modernCurve.minimum, 0.0);
    QCOMPARE(modernCurve.maximum, 100.0);
    const ParamField absent = fieldNamed(model, QStringLiteral("H_PHANG"));
    QVERIFY(absent.readOnly);
    QCOMPARE(absent.status, QStringLiteral("n/a"));
}

void ConfigTradHeliViewModelTest::writesRequireDisarmedExactComponentAndEcho()
{
    ConfigTradHeliViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());
    QSignalSpy writes(&model, &ConfigTradHeliViewModel::writeRequested);

    QVERIFY(!model.setFieldValue(QStringLiteral("H_COL_MIN"), 1100));
    model.setConnected(true);
    model.setArmed(true);
    QVERIFY(!model.setFieldValue(QStringLiteral("H_COL_MIN"), 1100));
    model.setArmed(false);
    QVERIFY(model.setFieldValue(QStringLiteral("H_COL_MIN"), 1100));
    QCOMPARE(writes.count(), 1);
    const quint64 fieldRequest = writes.at(0).at(0).toULongLong();
    QCOMPARE(writes.at(0).at(1).toInt(), 1);
    QCOMPARE(writes.at(0).at(2).toString(), QStringLiteral("H_COL_MIN"));
    QVERIFY(model.HasPendingWrites());
    model.parameterWriteSubmitted(fieldRequest, 100);

    model.parameterChanged(2, QStringLiteral("H_COL_MIN"), 1100);
    QVERIFY(model.HasPendingWrites());
    model.parameterChanged(1, QStringLiteral("H_COL_MIN"), 1100);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(99, 1, 0);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(100, 1, 0);
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(fieldNamed(model, QStringLiteral("H_COL_MIN")).status,
             QStringLiteral("✓"));

    QVERIFY(model.setSwashCcpm(false));
    const quint64 swashRequest = writes.at(1).at(0).toULongLong();
    model.parameterWriteSubmitted(swashRequest, 101);
    model.parameterWriteFailed(101, 1, QStringLiteral("H_SWASH_TYPE"),
                               QStringLiteral("rejected"));
    QVERIFY(model.SwashIsCcpm());
    QCOMPARE(model.ServoStatus(), QStringLiteral("rejected"));
}

void ConfigTradHeliViewModelTest::manualModesAreCapabilityGatedAndDeactivateSafe()
{
    ConfigTradHeliViewModel model;
    model.setCatalog(catalogFixture(false));
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    model.setActive(true);
    QSignalSpy writes(&model, &ConfigTradHeliViewModel::writeRequested);
    QSignalSpy warnings(&model,
                        &ConfigTradHeliViewModel::manualSafetyWarning);

    QVERIFY(model.ManualModeSupported(4));
    QVERIFY(!model.ManualModeSupported(5));
    QVERIFY(!model.setManualServoMode(5));
    QVERIFY(model.setManualServoMode(1));
    QVERIFY(model.ManualServoActive());
    QCOMPARE(writes.count(), 1);
    model.parameterWriteSubmitted(
        writes.at(0).at(0).toULongLong(), 201);

    // Leaving the page supersedes even an unacknowledged enable with a
    // disable request for the same still-bound target.
    model.setActive(false);
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(2).toString(), QStringLiteral("H_SV_MAN"));
    QCOMPARE(writes.at(1).at(3).toInt(), 0);
    QVERIFY(!model.ManualServoActive());
    model.parameterWriteSubmitted(
        writes.at(1).at(0).toULongLong(), 202);
    QVERIFY(!model.setManualServoMode(0));
    QCOMPARE(writes.count(), 2);

    // A terminal event from the superseded enable must not close the current
    // safety-disable operation. Raw echoes are informative, not terminal.
    model.parameterWriteFailed(201, 1, QStringLiteral("H_SV_MAN"),
                               QStringLiteral("old failure"));
    model.parameterChanged(1, QStringLiteral("H_SV_MAN"), 1);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(201, 1, 0);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(202, 1, 0);
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(!model.ManualOverrideMayBeActive());

    QList<ConfigFriendlyParameterValue> activeSnapshot = snapshot();
    for (ConfigFriendlyParameterValue &parameter : activeSnapshot) {
        if (parameter.name == QLatin1String("H_SV_MAN")) {
            parameter.value = 1;
        }
    }
    model.setParameterSnapshot(activeSnapshot);
    QVERIFY(model.setManualServoMode(0));
    model.parameterWriteSubmitted(
        writes.constLast().at(0).toULongLong(), 203);
    model.parameterWriteFailed(203, 1, QStringLiteral("H_SV_MAN"),
                               QStringLiteral("link lost"));
    QCOMPARE(warnings.count(), 1);
    QVERIFY(model.ManualOverrideMayBeActive());
    model.parameterChanged(1, QStringLiteral("H_SV_MAN"), 0);
    QVERIFY(model.ManualOverrideMayBeActive());

    ConfigTradHeliViewModel legacyTest;
    legacyTest.setCatalog(catalogFixture(true));
    legacyTest.setParameterSnapshot(snapshot());
    legacyTest.setConnected(true);
    QVERIFY(legacyTest.ManualModeSupported(5));
}

void ConfigTradHeliViewModelTest::submittedBatchOwnsTimeoutAndFailureKeepsManualUncertainty()
{
    ConfigTradHeliViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigTradHeliViewModel::writeRequested);

    QVERIFY(model.setManualServoMode(1));
    const quint64 enableRequest = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(enableRequest, 301);
    QTest::qWait(ConfigTradHeliViewModel::WriteTimeoutMs() + 100);
    QVERIFY(model.HasPendingWrites());

    model.parameterWriteFailed(301, 1, QStringLiteral("H_SV_MAN"),
                               QStringLiteral("ack lost"));
    QVERIFY(!model.HasPendingWrites());
    QVERIFY(model.ManualOverrideMayBeActive());

    // Even though the committed cache still says zero, uncertainty permits a
    // forced safety-disable write. A failed zero retains the latch.
    QVERIFY(model.setManualServoMode(0));
    quint64 disableRequest = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(disableRequest, 302);
    model.parameterWriteFailed(302, 1, QStringLiteral("H_SV_MAN"),
                               QStringLiteral("link lost"));
    QVERIFY(model.ManualOverrideMayBeActive());

    QVERIFY(model.setManualServoMode(0));
    disableRequest = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmitted(disableRequest, 303);
    model.parameterBatchCompleted(303, 1, 0);
    QVERIFY(!model.ManualOverrideMayBeActive());

    QVERIFY(model.setManualServoMode(1));
    const quint64 rejectedRequest = writes.constLast().at(0).toULongLong();
    model.parameterWriteSubmissionFailed(
        rejectedRequest, QStringLiteral("target changed"));
    QVERIFY(!model.ManualOverrideMayBeActive());
}

void ConfigTradHeliViewModelTest::visualizationUsesRcAndServoSixWithManualRangeCapture()
{
    ConfigTradHeliViewModel model;
    QList<ConfigFriendlyParameterValue> values = snapshot();
    for (ConfigFriendlyParameterValue &value : values) {
        if (value.name == QLatin1String("H_SV_MAN")) {
            value.value = 1;
        }
    }
    model.setParameterSnapshot(values);
    model.setRcInput(2, 1200);
    model.setRcInput(3, 1800);
    model.setServoOutput(5, 1000);
    model.setServoOutput(6, 1750);
    model.pumpVisualization();

    QCOMPARE(model.CollectiveInput(), 1200.0);
    QCOMPARE(model.RudderInput(), 1800.0);
    QCOMPARE(model.CollectiveCursorPercent(), 75.0);
    QCOMPARE(model.StabilizeCurve().size(), 4);
    QCOMPARE(model.StabilizeCurve().at(0).output, 10.0);
    QCOMPARE(model.AcroCurve().size(), 101);
    QCOMPARE(model.Servo1Position(), -15.0);
    QVERIFY(model.CollectiveRangeText().contains(QStringLiteral("1200")));
    QVERIFY(model.RudderRangeText().contains(QStringLiteral("1800")));
}

QTEST_APPLESS_MAIN(ConfigTradHeliViewModelTest)
#include "test_configtradheliviewmodel.moc"
