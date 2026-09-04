#include <QtTest>

#include "ui/configuration/ConfigTradHeli4View.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:H_SW_TYPE" humanName="Swashplate Type">
          <values><value code="0">H3 Generic</value><value code="1">H1</value></values>
        </param>
        <param name="ArduCopter:H_SV_MAN" humanName="Manual Servo Mode">
          <values><value code="0">Disabled</value><value code="1">Passthrough</value></values>
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

QList<ConfigFriendlyParameterValue> snapshot()
{
    QList<ConfigFriendlyParameterValue> result;
    for (const QString &name : ConfigTradHeli4ViewModel::ReferenceFieldNames()) {
        QVariant value = 0;
        if (name == QLatin1String("H_SW_TYPE")) {
            value = 1;
        } else if (name == QLatin1String("SERVO1_REVERSED")) {
            value = 1;
        } else if (name == QLatin1String("SERVO1_FUNCTION")) {
            value = 33;
        } else if (name == QLatin1String("SERVO1_MIN")) {
            value = 1001;
        } else if (name == QLatin1String("SERVO1_TRIM")) {
            value = 1501;
        } else if (name == QLatin1String("SERVO1_MAX")) {
            value = 2001;
        }
        result.append({1, name, value});
    }
    return result;
}
} // namespace

class ConfigTradHeli4ViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void surfaceIsCompleteBeforeSnapshot();
    void snapshotValuesHydrateVisibleEditors();
    void manualOverrideDefaultsToCancel();
    void destructionDoesNotEmitWrite();
};

void ConfigTradHeli4ViewTest::surfaceIsCompleteBeforeSnapshot()
{
    ConfigTradHeli4View view;
    view.resize(1100, 800);
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigTradHeli4View"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("tradHeli4Title")));
    QVERIFY(view.findChild<QPushButton *>(QStringLiteral("tradHeli4Refresh")));
    QCOMPARE(view.findChildren<QGroupBox *>().size(), 5);
    int servoRows = 0;
    for (QWidget *widget : view.findChildren<QWidget *>()) {
        if (widget->property("heli4ServoRow").toBool()) {
            ++servoRows;
        }
    }
    QCOMPARE(servoRows, 8);
    for (const QString &name : ConfigTradHeli4ViewModel::ReferenceFieldNames()) {
        QVERIFY2(view.findChild<QWidget *>(
                     QStringLiteral("tradHeli4Field_%1").arg(name)),
                 qPrintable(name));
    }
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("tradHeli4Refresh"))->isEnabled());

    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    QVERIFY(image.pixelColor(0, 0).alpha() > 0);
}

void ConfigTradHeli4ViewTest::manualOverrideDefaultsToCancel()
{
    ConfigTradHeli4View view;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(snapshot());
    view.setConnected(true);
    view.show();
    QApplication::processEvents();
    QSignalSpy writes(&view, &ConfigTradHeli4View::writeRequested);
    QComboBox *manual = view.findChild<QComboBox *>(
        QStringLiteral("tradHeli4Editor_H_SV_MAN"));
    QVERIFY(manual);
    QVERIFY(manual->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, &view, [&inspected]() {
        auto *warning = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(warning);
        QCOMPARE(warning->objectName(),
                 QStringLiteral("tradHeli4BladesRemovedWarning"));
        QCOMPARE(warning->standardButton(warning->defaultButton()),
                 QMessageBox::Cancel);
        inspected = true;
        warning->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(
        manual, "activated", Qt::DirectConnection, Q_ARG(int, 1)));
    QVERIFY(inspected);
    QCOMPARE(writes.count(), 0);
}

void ConfigTradHeli4ViewTest::snapshotValuesHydrateVisibleEditors()
{
    ConfigTradHeli4View view;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(snapshot());
    view.setConnected(true);
    view.show();
    QApplication::processEvents();

    QCheckBox *reversed = view.findChild<QCheckBox *>(
        QStringLiteral("tradHeli4ServoReversed_1"));
    QComboBox *function = view.findChild<QComboBox *>(
        QStringLiteral("tradHeli4ServoFunction_1"));
    QDoubleSpinBox *minimum = view.findChild<QDoubleSpinBox *>(
        QStringLiteral("tradHeli4Editor_SERVO1_MIN"));
    QDoubleSpinBox *trim = view.findChild<QDoubleSpinBox *>(
        QStringLiteral("tradHeli4Editor_SERVO1_TRIM"));
    QDoubleSpinBox *maximum = view.findChild<QDoubleSpinBox *>(
        QStringLiteral("tradHeli4Editor_SERVO1_MAX"));
    QVERIFY(reversed && function && minimum && trim && maximum);
    QVERIFY(reversed->isChecked());
    QCOMPARE(function->currentData().toInt(), 33);
    QCOMPARE(minimum->value(), 1001.0);
    QCOMPARE(trim->value(), 1501.0);
    QCOMPARE(maximum->value(), 2001.0);
    QCOMPARE(view.findChild<QComboBox *>(
                 QStringLiteral("tradHeli4Editor_H_SW_TYPE"))
                 ->currentData().toInt(), 1);
}

void ConfigTradHeli4ViewTest::destructionDoesNotEmitWrite()
{
    auto *view = new ConfigTradHeli4View;
    view->setCatalog(catalogFixture());
    view->setParameterSnapshot(snapshot());
    view->setConnected(true);
    QSignalSpy writes(view, &ConfigTradHeli4View::writeRequested);
    delete view;
    QCOMPARE(writes.count(), 0);
}

QTEST_MAIN(ConfigTradHeli4ViewTest)
#include "test_configtradheli4view.moc"
