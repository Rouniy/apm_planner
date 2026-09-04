#include <QtTest>

#include "ui/configuration/ConfigTradHeliView.h"
#include "ui/configuration/HeliCollectivePlot.h"

#include <QBuffer>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTimer>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:H_SV_MAN" humanName="Manual Servo Mode">
          <values>
            <value code="0">Disabled</value>
            <value code="1">Passthrough</value>
            <value code="2">Max</value>
            <value code="3">Center</value>
            <value code="4">Min</value>
          </values>
        </param>
        <param name="ArduCopter:H_TAIL_TYPE" humanName="Tail Type">
          <values><value code="0">Servo</value><value code="1">DDVP</value></values>
        </param>
        <param name="ArduCopter:H_RSC_MODE" humanName="RSC Mode">
          <values><value code="0">Disabled</value><value code="1">Pass Through</value></values>
        </param>
        <param name="ArduCopter:H_FLYBAR_MODE" humanName="Flybar Mode">
          <values><value code="0">No Flybar</value><value code="1">Flybar</value></values>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot(int manualMode = 0)
{
    QList<ConfigFriendlyParameterValue> result = {
        {1, QStringLiteral("H_SWASH_TYPE"), 0},
        {1, QStringLiteral("H_SV_MAN"), manualMode},
        {1, QStringLiteral("H_TAIL_TYPE"), 0},
        {1, QStringLiteral("H_RSC_MODE"), 0},
        {1, QStringLiteral("H_FLYBAR_MODE"), 0},
        {1, QStringLiteral("H_COL_MIN"), 1000},
        {1, QStringLiteral("H_COL_MAX"), 2000},
        {1, QStringLiteral("IM_STAB_COL_1"), 0},
        {1, QStringLiteral("IM_STAB_COL_2"), 400},
        {1, QStringLiteral("IM_STAB_COL_3"), 600},
        {1, QStringLiteral("IM_STAB_COL_4"), 1000},
        {1, QStringLiteral("IM_ACRO_COL_EXP"), 0.25}
    };
    return result;
}

QList<QPushButton *> manualButtons(ConfigTradHeliView *view)
{
    QList<QPushButton *> result;
    for (QPushButton *button : view->findChildren<QPushButton *>()) {
        if (button->property("manualServoButton").toBool()) {
            result.append(button);
        }
    }
    return result;
}
} // namespace

class ConfigTradHeliViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void widgetExposesCompleteNonEmptyMp10Surface();
    void nonzeroManualModeDefaultsToCancel();
    void disableIsImmediateAndNeedsNoConfirmation();
    void destructionDoesNotEmitWrite();
};

void ConfigTradHeliViewTest::widgetExposesCompleteNonEmptyMp10Surface()
{
    ConfigTradHeliView view;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(snapshot());
    view.setConnected(true);
    view.resize(1100, 800);
    view.show();
    QApplication::processEvents();
    QVERIFY(view.isVisible());

    QCOMPARE(view.objectName(), QStringLiteral("ConfigTradHeliView"));
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("tradHeliTitle")));
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("tradHeliRefreshParams")));
    QVERIFY(view.findChild<QRadioButton *>(
        QStringLiteral("tradHeliSwashCcpm")));
    QVERIFY(view.findChild<QRadioButton *>(
        QStringLiteral("tradHeliSwashH1")));
    QVERIFY(view.findChild<HeliCollectivePlot *>(
        QStringLiteral("tradHeliCollectivePlot")));
    QVERIFY(view.findChild<QProgressBar *>(
        QStringLiteral("tradHeliCollectiveInput")));
    QVERIFY(view.findChild<QProgressBar *>(
        QStringLiteral("tradHeliRudderInput")));

    const QList<QPushButton *> buttons = manualButtons(&view);
    QCOMPARE(buttons.size(), 6);
    for (QPushButton *button : buttons) {
        QVERIFY(!button->isHidden());
    }
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("tradHeliManualTest"))->isEnabled());

    int fieldRows = 0;
    for (QWidget *widget : view.findChildren<QWidget *>()) {
        if (widget->property("heliFieldRow").toBool()) {
            ++fieldRows;
        }
    }
    QCOMPARE(fieldRows, 43);

    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    QVERIFY(!image.isNull());
    QVERIFY(image.pixelColor(0, 0).alpha() > 0);
}

void ConfigTradHeliViewTest::nonzeroManualModeDefaultsToCancel()
{
    ConfigTradHeliView view;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(snapshot());
    view.setConnected(true);
    view.show();
    QApplication::processEvents();
    QVERIFY(view.isVisible());
    QSignalSpy writes(&view, &ConfigTradHeliView::writeRequested);

    bool inspected = false;
    QTimer::singleShot(0, &view, [&inspected]() {
        auto *warning = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        QVERIFY(warning);
        QCOMPARE(warning->objectName(),
                 QStringLiteral("tradHeliBladesRemovedWarning"));
        QCOMPARE(warning->standardButton(warning->defaultButton()),
                 QMessageBox::Cancel);
        QCOMPARE(warning->standardButton(warning->escapeButton()),
                 QMessageBox::Cancel);
        inspected = true;
        warning->reject();
    });
    view.findChild<QPushButton *>(
        QStringLiteral("tradHeliManual"))->click();
    QVERIFY(inspected);
    QCOMPARE(writes.count(), 0);
}

void ConfigTradHeliViewTest::disableIsImmediateAndNeedsNoConfirmation()
{
    ConfigTradHeliView view;
    view.setCatalog(catalogFixture());
    view.setParameterSnapshot(snapshot(1));
    view.setConnected(true);
    QSignalSpy writes(&view, &ConfigTradHeliView::writeRequested);

    QPushButton *disable = view.findChild<QPushButton *>(
        QStringLiteral("tradHeliManualDisable"));
    QVERIFY(disable);
    QVERIFY(disable->isEnabled());
    disable->click();
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.at(0).at(2).toString(), QStringLiteral("H_SV_MAN"));
    QCOMPARE(writes.at(0).at(3).toInt(), 0);
    QVERIFY(!QApplication::activeModalWidget());
}

void ConfigTradHeliViewTest::destructionDoesNotEmitWrite()
{
    auto *view = new ConfigTradHeliView;
    view->setCatalog(catalogFixture());
    view->setParameterSnapshot(snapshot(1));
    view->setConnected(true);
    QSignalSpy writes(view, &ConfigTradHeliView::writeRequested);
    delete view;
    QCOMPARE(writes.count(), 0);
}

QTEST_MAIN(ConfigTradHeliViewTest)
#include "test_configtradheliview.moc"
