#include <QtTest>

#include "ui/GuidedAltitudeDialog.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>

#include <cmath>

class GuidedAltitudeDialogTest final : public QObject
{
    Q_OBJECT

private slots:
    void presentsFrozenTargetAndDefaultCancel();
    void acceptsNegativeAndZeroInEveryFrame();
    void convertsDisplayedUnitsToMetres();
    void invalidInputDoesNotAccept();
    void cancellationExposesNoValue();
};

void GuidedAltitudeDialogTest::presentsFrozenTargetAndDefaultCancel()
{
    GuidedAltitudeDialog dialog(
        12.5, MAV_FRAME_GLOBAL_TERRAIN_ALT, 1.0, QStringLiteral("m"),
        QStringLiteral("Vehicle 42 component 1 on link 7"));

    QCOMPARE(dialog.objectName(), QStringLiteral("GuidedAltitudeDialog"));
    auto *target = dialog.findChild<QLabel *>(
        QStringLiteral("GuidedAltitudeTarget"));
    auto *frame = dialog.findChild<QComboBox *>(
        QStringLiteral("GuidedAltitudeFrame"));
    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("GuidedAltitudeAcceptButton"));
    auto *cancel = dialog.findChild<QPushButton *>(
        QStringLiteral("GuidedAltitudeCancelButton"));
    QVERIFY(target);
    QVERIFY(frame);
    QVERIFY(accept);
    QVERIFY(cancel);
    QVERIFY(target->text().contains(
        QStringLiteral("Vehicle 42 component 1 on link 7")));
    QCOMPARE(frame->currentData().toInt(),
             static_cast<int>(MAV_FRAME_GLOBAL_TERRAIN_ALT));
    QVERIFY(cancel->isDefault());
    QVERIFY(!accept->isDefault());
    QVERIFY(!dialog.hasAcceptedValue());
    QVERIFY(std::isnan(dialog.altitudeMetres()));
}

void GuidedAltitudeDialogTest::acceptsNegativeAndZeroInEveryFrame()
{
    const QVector<MAV_FRAME> frames{
        MAV_FRAME_GLOBAL_RELATIVE_ALT,
        MAV_FRAME_GLOBAL,
        MAV_FRAME_GLOBAL_TERRAIN_ALT
    };
    for (int i = 0; i < frames.size(); ++i) {
        GuidedAltitudeDialog dialog(
            50.0, MAV_FRAME_GLOBAL_RELATIVE_ALT, 1.0,
            QStringLiteral("m"), QStringLiteral("target"));
        auto *value = dialog.findChild<QLineEdit *>(
            QStringLiteral("GuidedAltitudeValue"));
        auto *frame = dialog.findChild<QComboBox *>(
            QStringLiteral("GuidedAltitudeFrame"));
        auto *accept = dialog.findChild<QPushButton *>(
            QStringLiteral("GuidedAltitudeAcceptButton"));
        QVERIFY(value);
        QVERIFY(frame);
        QVERIFY(accept);
        value->setText(i == 0 ? QStringLiteral("0")
                              : QStringLiteral("-12.75"));
        frame->setCurrentIndex(frame->findData(static_cast<int>(frames.at(i))));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        accept->click();
        QCOMPARE(accepted.count(), 1);
        QVERIFY(dialog.hasAcceptedValue());
        QCOMPARE(dialog.frame(), frames.at(i));
        QCOMPARE(dialog.altitudeMetres(), i == 0 ? 0.0 : -12.75);
    }
}

void GuidedAltitudeDialogTest::convertsDisplayedUnitsToMetres()
{
    GuidedAltitudeDialog dialog(
        10.0, MAV_FRAME_GLOBAL_RELATIVE_ALT, 3.28084,
        QStringLiteral("ft"), QStringLiteral("target"));
    auto *value = dialog.findChild<QLineEdit *>(
        QStringLiteral("GuidedAltitudeValue"));
    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("GuidedAltitudeAcceptButton"));
    QVERIFY(value);
    QVERIFY(accept);
    QCOMPARE(value->text(), QStringLiteral("32.8084"));
    value->setText(QStringLiteral("328.084"));
    accept->click();
    QVERIFY(dialog.hasAcceptedValue());
    QVERIFY(qAbs(dialog.altitudeMetres() - 100.0) < 0.000001);
}

void GuidedAltitudeDialogTest::invalidInputDoesNotAccept()
{
    GuidedAltitudeDialog dialog(
        10.0, MAV_FRAME_GLOBAL_RELATIVE_ALT, 1.0,
        QStringLiteral("m"), QStringLiteral("target"));
    auto *value = dialog.findChild<QLineEdit *>(
        QStringLiteral("GuidedAltitudeValue"));
    auto *validation = dialog.findChild<QLabel *>(
        QStringLiteral("GuidedAltitudeValidation"));
    auto *accept = dialog.findChild<QPushButton *>(
        QStringLiteral("GuidedAltitudeAcceptButton"));
    QVERIFY(value);
    QVERIFY(validation);
    QVERIFY(accept);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    value->setText(QStringLiteral("nan"));
    accept->click();
    QCOMPARE(accepted.count(), 0);
    QVERIFY(dialog.isVisible() || dialog.result() != QDialog::Accepted);
    QVERIFY(!validation->isHidden());
    QVERIFY(!dialog.hasAcceptedValue());
    QVERIFY(std::isnan(dialog.altitudeMetres()));
}

void GuidedAltitudeDialogTest::cancellationExposesNoValue()
{
    GuidedAltitudeDialog dialog(
        10.0, MAV_FRAME_GLOBAL, 1.0,
        QStringLiteral("m"), QStringLiteral("target"));
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    dialog.show();
    QTest::keyClick(&dialog, Qt::Key_Escape);
    QCOMPARE(rejected.count(), 1);
    QVERIFY(!dialog.hasAcceptedValue());
    QVERIFY(std::isnan(dialog.altitudeMetres()));
}

QTEST_MAIN(GuidedAltitudeDialogTest)
#include "test_guidedaltitudedialog.moc"
