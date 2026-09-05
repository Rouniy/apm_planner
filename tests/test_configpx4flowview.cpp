#include <QtTest>

#include "ui/configuration/ConfigPX4FlowView.h"

#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QVariantMap>

namespace {

QVariantMap source(const QString &id, const QString &label)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("label"), label}};
}

} // namespace

class ConfigPX4FlowViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void offlineSurfaceMatchesMp10AndIsNonempty();
    void sourceHydrationIsBoundedAndWriteFree();
    void focusStateAndAspectFitAreServiceDriven();
    void showHideLifecycleIsIdempotent();
};

void ConfigPX4FlowViewTest::offlineSurfaceMatchesMp10AndIsNonempty()
{
    ConfigPX4FlowView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigPX4FlowView"));
    QLabel *const title = view.findChild<QLabel *>(
        QStringLiteral("Px4FlowTitle"));
    QLabel *const status = view.findChild<QLabel *>(
        QStringLiteral("Px4FlowStatus"));
    QLabel *const image = view.findChild<QLabel *>(
        QStringLiteral("Px4FlowImage"));
    QComboBox *const sources = view.findChild<QComboBox *>(
        QStringLiteral("Px4FlowSourceSelector"));
    QPushButton *const focus = view.findChild<QPushButton *>(
        QStringLiteral("Px4FlowFocusButton"));
    QVERIFY(title);
    QVERIFY(status);
    QVERIFY(image);
    QVERIFY(sources);
    QVERIFY(focus);
    QCOMPARE(title->text(), QStringLiteral("PX4Flow"));
    QCOMPARE(status->text(),
             QStringLiteral("Not connected — connect to a PX4Flow sensor "
                            "to view its image."));
    QCOMPARE(image->text(), QStringLiteral("No PX4Flow image received."));
    QCOMPARE(focus->text(), QStringLiteral("Focus"));
    QVERIFY(!focus->isEnabled());
    QCOMPARE(view.sourceCount(), 0);
    QVERIFY(view.selectedSourceId().isEmpty());
    QVERIFY(view.frame().isNull());

    for (QLabel *label : view.findChildren<QLabel *>()) {
        QVERIFY2(!label->text().contains(
                     QStringLiteral("fps"), Qt::CaseInsensitive),
                 "PX4Flow UI claims an FPS value it does not measure");
    }
}

void ConfigPX4FlowViewTest::sourceHydrationIsBoundedAndWriteFree()
{
    ConfigPX4FlowView view;
    QSignalSpy selected(&view, &ConfigPX4FlowView::sourceSelected);

    // A selected sensor may arrive before discovery. Keep that identity
    // pending rather than silently falling back to a different component.
    view.setSelectedSourceId(QStringLiteral("sensor-17"));
    QVERIFY(view.selectedSourceId().isEmpty());
    QCOMPARE(selected.count(), 0);

    QVariantList rows;
    for (int index = 0; index < 80; ++index) {
        rows.append(source(QStringLiteral("sensor-%1").arg(index),
                           QStringLiteral("Link A / PX4Flow %1").arg(index)));
    }
    rows.append(source(QStringLiteral("sensor-17"),
                       QStringLiteral("duplicate")));
    rows.append(source(QString(), QStringLiteral("missing id")));
    rows.append(source(QStringLiteral("missing-label"), QString()));
    view.setSources(rows);

    QCOMPARE(view.sourceCount(), ConfigPX4FlowView::MaximumSources);
    QCOMPARE(view.selectedSourceId(), QStringLiteral("sensor-17"));
    QCOMPARE(selected.count(), 0);

    // Reordered snapshot hydration preserves identity and emits no user
    // selection signal.
    view.setSources({source(QStringLiteral("sensor-18"),
                            QStringLiteral("Sensor 18")),
                     source(QStringLiteral("sensor-17"),
                            QStringLiteral("Sensor 17"))});
    QCOMPARE(view.selectedSourceId(), QStringLiteral("sensor-17"));
    QCOMPARE(selected.count(), 0);

    QComboBox *const combo = view.findChild<QComboBox *>(
        QStringLiteral("Px4FlowSourceSelector"));
    QVERIFY(combo);
    combo->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(
        combo, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.takeFirst().at(0).toString(),
             QStringLiteral("sensor-18"));

    view.setSelectedSourceId(QStringLiteral("not-discovered"));
    QVERIFY(view.selectedSourceId().isEmpty());
    QCOMPARE(selected.count(), 0);
}

void ConfigPX4FlowViewTest::focusStateAndAspectFitAreServiceDriven()
{
    ConfigPX4FlowView view;
    view.resize(720, 520);
    view.setSources({source(QStringLiteral("link7:81:50"),
                            QStringLiteral("Telemetry / sys 81 / comp 50"))});
    view.setSelectedSourceId(QStringLiteral("link7:81:50"));
    QSignalSpy focusRequests(&view, &ConfigPX4FlowView::focusRequested);
    QPushButton *const focus = view.findChild<QPushButton *>(
        QStringLiteral("Px4FlowFocusButton"));
    QVERIFY(focus);

    view.setModeState(false, true, false);
    QCOMPARE(focus->text(), QStringLiteral("Focus"));
    QVERIFY(focus->isEnabled());
    focus->click();
    QCOMPARE(focusRequests.count(), 1);
    QCOMPARE(focus->text(), QStringLiteral("Focus"));
    QVERIFY(!view.videoOnly());

    view.setModeState(true, true, false);
    QCOMPARE(focus->text(), QStringLiteral("Video"));
    QVERIFY(focus->isEnabled());
    QVERIFY(view.videoOnly());
    view.setModeState(true, true, true);
    QVERIFY(!focus->isEnabled());
    QVERIFY(focus->toolTip().contains(QStringLiteral("in progress")));
    view.setModeState(true, false, false);
    QVERIFY(!focus->isEnabled());
    QVERIFY(focus->toolTip().contains(QStringLiteral("read-only")));

    QImage frame(320, 160, QImage::Format_Grayscale8);
    frame.fill(127);
    view.setFrame(frame);
    QCOMPARE(view.frame(), frame);
    view.show();
    QCoreApplication::processEvents();
    const QSize displayed = view.displayedFrameSize();
    QVERIFY(!displayed.isEmpty());
    QCOMPARE(displayed.width(), displayed.height() * 2);
    QLabel *const image = view.findChild<QLabel *>(
        QStringLiteral("Px4FlowImage"));
    QVERIFY(image);
    QVERIFY(image->contentsRect().contains(
        QRect(QPoint(0, 0), displayed))
        || (displayed.width() <= image->contentsRect().width()
            && displayed.height() <= image->contentsRect().height()));

    view.setFrame(QImage());
    QVERIFY(view.frame().isNull());
    QCOMPARE(image->text(), QStringLiteral("No PX4Flow image received."));
}

void ConfigPX4FlowViewTest::showHideLifecycleIsIdempotent()
{
    ConfigPX4FlowView view;
    QSignalSpy activated(&view, &ConfigPX4FlowView::activated);
    QSignalSpy deactivated(&view, &ConfigPX4FlowView::deactivated);
    QVERIFY(!view.active());

    view.show();
    QCoreApplication::processEvents();
    QCOMPARE(activated.count(), 1);
    QVERIFY(view.active());
    view.show();
    QCoreApplication::processEvents();
    QCOMPARE(activated.count(), 1);

    view.hide();
    QCoreApplication::processEvents();
    QCOMPARE(deactivated.count(), 1);
    QVERIFY(!view.active());
    view.hide();
    QCoreApplication::processEvents();
    QCOMPARE(deactivated.count(), 1);

    view.show();
    QCoreApplication::processEvents();
    QCOMPARE(activated.count(), 2);
    QVERIFY(view.active());
}

QTEST_MAIN(ConfigPX4FlowViewTest)
#include "test_configpx4flowview.moc"
