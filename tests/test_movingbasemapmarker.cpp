#include "ui/map/AbstractMapWidget.h"
#include "ui/map/MovingBaseMapMarkerItem.h"

#include <QGraphicsScene>
#include <QImage>
#include <QPainter>
#include <QtTest>

#include <limits>
#include <type_traits>

namespace
{

using SetMovingBaseContract = void (AbstractMapWidget::*)(
    const MapCoordinate &, const QString &);
using ClearMovingBaseContract = void (AbstractMapWidget::*)();

static_assert(std::is_same<
    decltype(&AbstractMapWidget::SetMovingBase),
    SetMovingBaseContract>::value,
    "Moving Base must remain part of the backend-neutral map contract");
static_assert(std::is_same<
    decltype(&AbstractMapWidget::ClearMovingBase),
    ClearMovingBaseContract>::value,
    "Moving Base clear must remain part of the backend-neutral map contract");

} // namespace

class MovingBaseMapMarkerTest final : public QObject
{
    Q_OBJECT

private slots:
    void backendNeutralContractAndVisibilityRules();
    void formatterMatchesMissionPlannerLabel();
    void itemUpdatesGeometryInPlaceAndHasOwnedSceneLifecycle();
    void paintUsesMissionPlannerColors();
};

void MovingBaseMapMarkerTest::backendNeutralContractAndVisibilityRules()
{
    QVERIFY(IsMovingBaseMapPositionRenderable(35.1, 33.2, 42.0));
    QVERIFY(IsMovingBaseMapPositionRenderable(0.0, 33.2, 42.0));
    QVERIFY(IsMovingBaseMapPositionRenderable(35.1, 0.0, 42.0));
    QVERIFY(!IsMovingBaseMapPositionRenderable(0.0, 0.0, 42.0));
    QVERIFY(!IsMovingBaseMapPositionRenderable(91.0, 33.2, 42.0));
    QVERIFY(!IsMovingBaseMapPositionRenderable(35.1, 181.0, 42.0));
    QVERIFY(!IsMovingBaseMapPositionRenderable(
        std::numeric_limits<double>::quiet_NaN(), 33.2, 42.0));
    QVERIFY(!IsMovingBaseMapPositionRenderable(
        35.1, 33.2, std::numeric_limits<double>::infinity()));
}

void MovingBaseMapMarkerTest::formatterMatchesMissionPlannerLabel()
{
    QCOMPARE(FormatMovingBaseMapDetail(
                 42.04, QStringLiteral("  Sats  12   hdop 0.8 ")),
             QStringLiteral("42.0 m AMSL  Sats 12 hdop 0.8"));
    QCOMPARE(FormatMovingBaseMapDetail(
                 51.96, QStringLiteral("Sats 10 hdop 1.25")),
             QStringLiteral("52.0 m AMSL  Sats 10 hdop 1.25"));
    QCOMPARE(FormatMovingBaseMapDetail(-0.0, QString()),
             QStringLiteral("0.0 m AMSL"));
    QVERIFY(FormatMovingBaseMapDetail(
        std::numeric_limits<double>::quiet_NaN(), QString()).isEmpty());
}

void MovingBaseMapMarkerTest::
itemUpdatesGeometryInPlaceAndHasOwnedSceneLifecycle()
{
    QGraphicsScene scene;
    auto *marker = new MovingBaseMapMarkerItem;
    QGraphicsItem *const identity = marker;
    scene.addItem(marker);
    QCOMPARE(scene.items().size(), 1);
    QCOMPARE(marker->data(0).toString(),
             QStringLiteral("MovingBaseMarker"));
    QVERIFY(marker->flags().testFlag(
        QGraphicsItem::ItemIgnoresTransformations));
    QVERIFY(!marker->flags().testFlag(QGraphicsItem::ItemIsMovable));
    QVERIFY(!marker->flags().testFlag(QGraphicsItem::ItemIsSelectable));

    marker->setFix(42.0, QStringLiteral("Sats 8"));
    const QRectF shortBounds = marker->boundingRect();
    QCOMPARE(marker->detailText(),
             QStringLiteral("42.0 m AMSL  Sats 8"));
    marker->setFix(42.0,
                   QStringLiteral("Sats 12 hdop 0.80 correction active"));
    QCOMPARE(static_cast<QGraphicsItem *>(marker), identity);
    QVERIFY(marker->boundingRect().width() > shortBounds.width());
    QCOMPARE(scene.items().size(), 1);

    delete marker;
    QVERIFY(scene.items().isEmpty());
}

void MovingBaseMapMarkerTest::paintUsesMissionPlannerColors()
{
    MovingBaseMapMarkerItem marker;
    marker.setFix(42.0, QStringLiteral("Sats 12 hdop 0.8"));
    QCOMPARE(marker.data(1).toString(),
             QStringLiteral("42.0 m AMSL  Sats 12 hdop 0.8"));

    QImage image(240, 90, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.translate(120.0, 35.0);
    marker.paint(&painter, nullptr, nullptr);
    painter.end();

    QCOMPARE(image.pixelColor(120, 35), QColor(0, 210, 210));
    bool hasBlack = false;
    bool hasTranslucentBlack = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            hasBlack = hasBlack
                || (pixel.alpha() >= 200 && pixel.red() < 10
                    && pixel.green() < 10 && pixel.blue() < 10);
            hasTranslucentBlack = hasTranslucentBlack
                || (pixel.alpha() == 160 && pixel.red() == 0
                    && pixel.green() == 0 && pixel.blue() == 0);
        }
    }
    QVERIFY(hasBlack);
    QVERIFY(hasTranslucentBlack);
}

QTEST_MAIN(MovingBaseMapMarkerTest)
#include "test_movingbasemapmarker.moc"
