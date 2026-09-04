#include <QtTest>

#include "ui/configuration/ConfigTradHeli4ViewModel.h"

#include <QBuffer>
#include <QSet>
#include <QSignalSpy>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:H_SW_TYPE" humanName="Swashplate Type">
          <values>
            <value code="0">H3 Generic</value><value code="1">H1 non-CPPM</value>
            <value code="2">H3_140</value><value code="3">H3_120</value>
            <value code="4">H4_90</value><value code="5">H4_45</value>
          </values>
        </param>
        <param name="ArduCopter:H_SV_MAN" humanName="Manual Servo Mode">
          <values><value code="0">Disabled</value><value code="1">Passthrough</value></values>
        </param>
        <param name="ArduCopter:H_COL_MIN" humanName="Collective Min">
          <field name="Range">1000 2000</field><field name="Increment">1</field>
        </param>
        <param name="ArduCopter:SERVO1_FUNCTION" humanName="Servo Function">
          <values><value code="0">Disabled</value><value code="33">Motor 1</value></values>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot(int component = 1)
{
    QList<ConfigFriendlyParameterValue> result;
    for (const QString &name :
         ConfigTradHeli4ViewModel::ReferenceFieldNames()) {
        QVariant value = 0;
        if (name.endsWith(QStringLiteral("_MIN"))) {
            value = 1000;
        } else if (name.endsWith(QStringLiteral("_TRIM"))) {
            value = 1500;
        } else if (name.endsWith(QStringLiteral("_MAX"))) {
            value = 2000;
        }
        result.append({component, name, value});
    }
    return result;
}

ParamField fieldNamed(const ConfigTradHeli4ViewModel &model,
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

class ConfigTradHeli4ViewModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactMp10InventoryAndModernCapability();
    void metadataAndMissingRowsAreSafe();
    void writesUseOwnedTerminalBatch();
    void manualOverrideRequiresDisarmedVehicle();
    void manualOverrideDeactivationKeepsConservativeLatch();
};

void ConfigTradHeli4ViewModelTest::exactMp10InventoryAndModernCapability()
{
    const QList<Heli4ParameterSection> sections =
        ConfigTradHeli4ViewModel::Sections();
    QCOMPARE(sections.size(), 5);
    QCOMPARE(sections.at(0).parameters.size(), 40);
    QCOMPARE(sections.at(1).parameters.size(), 13);
    QCOMPARE(sections.at(2).parameters.size(), 12);
    QCOMPARE(sections.at(3).parameters.size(), 9);
    QCOMPARE(sections.at(4).parameters.size(), 9);

    const QStringList names = ConfigTradHeli4ViewModel::ReferenceFieldNames();
    QCOMPARE(names.size(), 83);
    QCOMPARE(QSet<QString>(names.constBegin(), names.constEnd()).size(), 83);
    QCOMPARE(names.constFirst(), QStringLiteral("SERVO1_REVERSED"));
    QCOMPARE(names.at(40), QStringLiteral("H_SV_MAN"));
    QCOMPARE(names.constLast(), QStringLiteral("H_COLYAW"));

    ConfigTradHeli4ViewModel model;
    model.setParameterSnapshot({{42, QStringLiteral("H_SWASH_TYPE"), 0}}, 42);
    QVERIFY(!model.SnapshotReady());
    model.setParameterSnapshot(snapshot(42), 1);
    QVERIFY(model.SnapshotReady());
    QCOMPARE(model.ComponentId(), 42);
    QCOMPARE(model.Fields().size(), 83);
}

void ConfigTradHeli4ViewModelTest::metadataAndMissingRowsAreSafe()
{
    ConfigTradHeli4ViewModel model;
    model.setCatalog(catalogFixture());
    QList<ConfigFriendlyParameterValue> values = snapshot();
    values.erase(std::remove_if(
        values.begin(), values.end(), [](const auto &parameter) {
            return parameter.name == QLatin1String("H_RSC_GOV_SETPNT");
        }), values.end());
    model.setParameterSnapshot(values);

    const ParamField swash = fieldNamed(model, QStringLiteral("H_SW_TYPE"));
    QCOMPARE(swash.label, QStringLiteral("Swashplate Type"));
    QCOMPARE(swash.options.size(), 6);
    QCOMPARE(swash.editorKind, ParamField::EditorKind::Combo);

    const ParamField servo8 = fieldNamed(
        model, QStringLiteral("SERVO8_FUNCTION"));
    QCOMPARE(servo8.options.size(), 2);
    QCOMPARE(servo8.editorKind, ParamField::EditorKind::Combo);
    QVERIFY(!servo8.readOnly);

    const ParamField pwm = fieldNamed(model, QStringLiteral("SERVO8_MAX"));
    QCOMPARE(pwm.minimum, 800.0);
    QCOMPARE(pwm.maximum, 2200.0);
    QCOMPARE(pwm.increment, 1.0);

    const ParamField missing = fieldNamed(
        model, QStringLiteral("H_RSC_GOV_SETPNT"));
    QVERIFY(missing.readOnly);
    QCOMPARE(missing.status, QStringLiteral("n/a"));

    ConfigTradHeli4ViewModel noMetadata;
    noMetadata.setParameterSnapshot(snapshot());
    QCOMPARE(fieldNamed(noMetadata, QStringLiteral("H_SV_MAN")).options.size(),
             5);
    QCOMPARE(fieldNamed(noMetadata, QStringLiteral("H_SW_TYPE")).options.size(),
             6);
    QCOMPARE(fieldNamed(noMetadata, QStringLiteral("H_RSC_MODE")).options.size(),
             4);
    QCOMPARE(fieldNamed(noMetadata, QStringLiteral("H_TAIL_TYPE")).options.size(),
             6);
}

void ConfigTradHeli4ViewModelTest::writesUseOwnedTerminalBatch()
{
    ConfigTradHeli4ViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    QSignalSpy writes(&model, &ConfigTradHeli4ViewModel::writeRequested);

    QVERIFY(model.setFieldValue(QStringLiteral("H_COL_MIN"), 1100));
    QCOMPARE(writes.count(), 1);
    const quint64 requestId = writes.at(0).at(0).toULongLong();
    model.parameterWriteSubmitted(requestId, 91);
    model.parameterChanged(2, QStringLiteral("H_COL_MIN"), 1100);
    model.parameterChanged(1, QStringLiteral("H_COL_MIN"), 1100);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(90, 1, 0);
    QVERIFY(model.HasPendingWrites());
    model.parameterBatchCompleted(91, 1, 0);
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(fieldNamed(model, QStringLiteral("H_COL_MIN")).status,
             QStringLiteral("✓"));

    QVERIFY(model.setFieldValue(QStringLiteral("H_COL_MIN"), 1200));
    model.parameterWriteSubmissionFailed(
        writes.constLast().at(0).toULongLong(), QStringLiteral("target changed"));
    QVERIFY(!model.HasPendingWrites());
    QCOMPARE(fieldNamed(model, QStringLiteral("H_COL_MIN")).value.toInt(), 1100);
}

void ConfigTradHeli4ViewModelTest::manualOverrideRequiresDisarmedVehicle()
{
    ConfigTradHeli4ViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    model.setArmed(true);
    QVERIFY(!model.setFieldValue(QStringLiteral("H_SV_MAN"), 1));
    model.setArmed(false);
    QVERIFY(model.setFieldValue(QStringLiteral("H_SV_MAN"), 1));
}

void ConfigTradHeli4ViewModelTest::manualOverrideDeactivationKeepsConservativeLatch()
{
    ConfigTradHeli4ViewModel model;
    model.setCatalog(catalogFixture());
    model.setParameterSnapshot(snapshot());
    model.setConnected(true);
    model.setActive(true);
    QSignalSpy writes(&model, &ConfigTradHeli4ViewModel::writeRequested);
    QSignalSpy warnings(&model,
                        &ConfigTradHeli4ViewModel::manualSafetyWarning);

    QVERIFY(model.setFieldValue(QStringLiteral("H_SV_MAN"), 1));
    model.parameterWriteSubmitted(writes.at(0).at(0).toULongLong(), 301);
    model.parameterBatchCompleted(301, 1, 0);
    QVERIFY(model.ManualOverrideMayBeActive());

    model.setActive(false);
    QCOMPARE(writes.count(), 2);
    QCOMPARE(writes.at(1).at(2).toString(), QStringLiteral("H_SV_MAN"));
    QCOMPARE(writes.at(1).at(3).toInt(), 0);
    model.parameterWriteSubmitted(writes.at(1).at(0).toULongLong(), 302);
    model.parameterChanged(1, QStringLiteral("H_SV_MAN"), 0);
    QVERIFY(model.ManualOverrideMayBeActive());
    model.parameterWriteFailed(302, 1, QStringLiteral("H_SV_MAN"),
                               QStringLiteral("ack lost"));
    QVERIFY(model.ManualOverrideMayBeActive());
    QCOMPARE(warnings.count(), 1);

    QVERIFY(model.setFieldValue(QStringLiteral("H_SV_MAN"), 0));
    model.parameterWriteSubmitted(writes.constLast().at(0).toULongLong(), 303);
    model.parameterBatchCompleted(303, 1, 0);
    QVERIFY(!model.ManualOverrideMayBeActive());

    model.setActive(true);
    QVERIFY(model.setFieldValue(QStringLiteral("H_SV_MAN"), 1));
    model.parameterWriteSubmissionFailed(
        writes.constLast().at(0).toULongLong(),
        QStringLiteral("target changed"));
    QVERIFY(!model.ManualOverrideMayBeActive());

    model.setActive(false);
    model.parameterChanged(1, QStringLiteral("H_SV_MAN"), 1);
    QVERIFY(model.ManualOverrideMayBeActive());
    QCOMPARE(warnings.count(), 2);
}

QTEST_APPLESS_MAIN(ConfigTradHeli4ViewModelTest)
#include "test_configtradheli4viewmodel.moc"
