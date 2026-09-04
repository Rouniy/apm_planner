#include <QtTest>

#include "ui/configuration/HeliCollectivePlot.h"

#include <QImage>
#include <QPainter>

#include <cmath>
#include <limits>

namespace {
bool imageContainsColor(const QImage &image, const QColor &expected,
                        int tolerance = 12)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (std::abs(actual.red() - expected.red()) <= tolerance
                && std::abs(actual.green() - expected.green()) <= tolerance
                && std::abs(actual.blue() - expected.blue()) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

QImage renderPlot(HeliCollectivePlot *plot)
{
    plot->resize(640, 320);
    QImage image(plot->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    plot->render(&painter);
    return image;
}
} // namespace

class HeliCollectivePlotTest final : public QObject
{
    Q_OBJECT

private slots:
    void apiClampsCursorAndHasStableIdentity();
    void rendersMissionPlannerCurvesAndCursor();
    void emptyAndNonFiniteDataRenderSafely();
};

void HeliCollectivePlotTest::apiClampsCursorAndHasStableIdentity()
{
    HeliCollectivePlot plot;

    QCOMPARE(plot.objectName(), QStringLiteral("HeliCollectivePlot"));
    QCOMPARE(plot.minimumSizeHint(), QSize(430, 280));
    plot.setCursorPercent(-10.0);
    QCOMPARE(plot.cursorPercent(), 0.0);
    plot.setCursorPercent(140.0);
    QCOMPARE(plot.cursorPercent(), 100.0);
    plot.setCursorPercent(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(plot.cursorPercent(), 0.0);
}

void HeliCollectivePlotTest::rendersMissionPlannerCurvesAndCursor()
{
    HeliCollectivePlot plot;
    const QVector<HeliVisualization::CurvePoint> stabilize = {
        {0.0, 0.0}, {40.0, 400.0}, {60.0, 600.0}, {100.0, 1000.0}
    };
    QVector<HeliVisualization::CurvePoint> acro;
    acro.reserve(101);
    for (int input = 0; input <= 100; ++input) {
        acro.append({static_cast<double>(input), input * 10.0});
    }
    plot.setStabilizeCurve(stabilize);
    plot.setAcroCurve(acro);
    plot.setCursorPercent(42.0);

    QCOMPARE(plot.stabilizeCurve().size(), 4);
    QCOMPARE(plot.acroCurve().size(), 101);
    QCOMPARE(plot.cursorPercent(), 42.0);

    const QImage image = renderPlot(&plot);
    QVERIFY(!image.isNull());
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#151817"))));
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#46504B"))));
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#1E90FF"))));
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#FFD700"))));
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#FF0000"))));
}

void HeliCollectivePlotTest::emptyAndNonFiniteDataRenderSafely()
{
    HeliCollectivePlot plot;
    QVERIFY(!renderPlot(&plot).isNull());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    plot.setStabilizeCurve({{nan, 100.0}, {50.0, infinity},
                            {-50.0, 1200.0}});
    plot.setAcroCurve({{0.0, 0.0}, {nan, nan}, {100.0, 1000.0}});
    plot.setCursorPercent(infinity);

    const QImage image = renderPlot(&plot);
    QVERIFY(!image.isNull());
    QCOMPARE(plot.cursorPercent(), 0.0);
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#151817"))));
    QVERIFY(imageContainsColor(image, QColor(QStringLiteral("#FF0000"))));

    plot.setMinimumSize(0, 0);
    plot.resize(80, 60);
    QImage tiny(plot.size(), QImage::Format_ARGB32_Premultiplied);
    tiny.fill(Qt::transparent);
    QPainter painter(&tiny);
    plot.render(&painter);
    painter.end();
    QVERIFY(imageContainsColor(tiny, QColor(QStringLiteral("#151817"))));
}

QTEST_MAIN(HeliCollectivePlotTest)
#include "test_helicollectiveplot.moc"
