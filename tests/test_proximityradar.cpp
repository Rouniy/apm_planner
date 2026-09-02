#include "uas/Proximity.h"
#include "ui/flightdata/ProximityRadarControl.h"
#include "ui/flightdata/ProximityWindow.h"

#include <QColor>
#include <QImage>
#include <QSignalSpy>
#include <QtMath>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr quint8 kSystemId = 42;
constexpr quint8 kComponentId = 1;

mavlink_message_t distanceSensorMessage(
    quint8 systemId, quint8 sensorId,
    MAV_SENSOR_ORIENTATION orientation, quint16 currentDistance,
    quint16 minDistance = 20, quint16 maxDistance = 500)
{
    mavlink_distance_sensor_t packet{};
    packet.min_distance = minDistance;
    packet.max_distance = maxDistance;
    packet.current_distance = currentDistance;
    packet.type = MAV_DISTANCE_SENSOR_LASER;
    packet.id = sensorId;
    packet.orientation = static_cast<quint8>(orientation);

    mavlink_message_t message{};
    mavlink_msg_distance_sensor_encode(
        systemId, kComponentId, &message, &packet);
    return message;
}

mavlink_message_t obstacleDistanceMessage(
    quint8 systemId, MAV_FRAME frame, const quint16 *distances,
    quint8 increment, float incrementF, float angleOffset,
    quint16 minDistance = 20, quint16 maxDistance = 500,
    MAV_DISTANCE_SENSOR sensorType = MAV_DISTANCE_SENSOR_LASER)
{
    mavlink_obstacle_distance_t packet{};
    packet.sensor_type = static_cast<quint8>(sensorType);
    for (int index = 0;
         index < MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN;
         ++index) {
        packet.distances[index] = distances[index];
    }
    packet.increment = increment;
    packet.min_distance = minDistance;
    packet.max_distance = maxDistance;
    packet.increment_f = incrementF;
    packet.angle_offset = angleOffset;
    packet.frame = static_cast<quint8>(frame);

    mavlink_message_t message{};
    mavlink_msg_obstacle_distance_encode(
        systemId, kComponentId, &message, &packet);
    return message;
}

void fillUnused(quint16 *distances)
{
    std::fill_n(distances,
                MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN,
                std::numeric_limits<quint16>::max());
}

const Proximity::Sample *sampleAtAngle(
    const QVector<Proximity::Sample> &samples, double angle)
{
    for (const Proximity::Sample &sample : samples) {
        if (qAbs(sample.Angle - angle) < 0.0001) {
            return &sample;
        }
    }
    return nullptr;
}

bool fuzzyColor(const QColor &actual, const QColor &expected, int tolerance = 8)
{
    return qAbs(actual.red() - expected.red()) <= tolerance
        && qAbs(actual.green() - expected.green()) <= tolerance
        && qAbs(actual.blue() - expected.blue()) <= tolerance;
}

} // namespace

class ProximityRadarTest final : public QObject
{
    Q_OBJECT

private slots:
    void distanceSensorDecodingAndFiltering();
    void obstacleDistanceBodyFrameUsesIncrementFAndSkipsInvalidBins();
    void obstacleDistanceGlobalFrameAddsVehicleHeading();
    void directionalAndCustomSamplesReplaceAndExpire();
    void readersAndPacketUpdatesCanRunConcurrently();
    void keyboardControlsAdjustRadarScale();
    void proximityWindowKeepsReferenceGeometryContract();
    void radarRendersAVisibleSceneWithObstacleData();
};

void ProximityRadarTest::distanceSensorDecodingAndFiltering()
{
    Proximity proximity(kSystemId);
    const Proximity::TimePoint received = Proximity::TimePoint{} + 10s;

    const mavlink_message_t foreign = distanceSensorMessage(
        kSystemId + 1, 7, MAV_SENSOR_ROTATION_YAW_90, 150);
    QVERIFY(!proximity.observeMessage(foreign, 0.0, received));
    QVERIFY(!proximity.DataAvailable());
    QVERIFY(proximity.directionState().GetRaw(received).isEmpty());

    const mavlink_message_t belowRange = distanceSensorMessage(
        kSystemId, 7, MAV_SENSOR_ROTATION_YAW_90, 20);
    QVERIFY(proximity.observeMessage(belowRange, 0.0, received));
    QVERIFY(!proximity.DataAvailable());
    QVERIFY(proximity.directionState().GetRaw(received).isEmpty());

    const mavlink_message_t atMaximum = distanceSensorMessage(
        kSystemId, 7, MAV_SENSOR_ROTATION_YAW_90, 500);
    QVERIFY(proximity.observeMessage(atMaximum, 0.0, received));
    QVERIFY(!proximity.DataAvailable());
    QVERIFY(proximity.directionState().GetRaw(received).isEmpty());

    const mavlink_message_t valid = distanceSensorMessage(
        kSystemId, 7, MAV_SENSOR_ROTATION_YAW_90, 150);
    QVERIFY(proximity.observeMessage(valid, 0.0, received));
    QVERIFY(proximity.DataAvailable());

    const QVector<Proximity::Sample> samples =
        proximity.directionState().GetRaw(received);
    QCOMPARE(samples.size(), 1);
    QCOMPARE(samples.first().SensorId, quint32(7));
    QCOMPARE(static_cast<int>(samples.first().Orientation),
             static_cast<int>(MAV_SENSOR_ROTATION_YAW_90));
    QCOMPARE(samples.first().Angle, 0.0);
    QCOMPARE(Proximity::OrientationAngle(samples.first().Orientation), 90.0);
    QCOMPARE(samples.first().Size, 45.0);
    QCOMPARE(samples.first().Distance, 150.0);
    QVERIFY(!samples.first().IsCustom());
}

void ProximityRadarTest::obstacleDistanceBodyFrameUsesIncrementFAndSkipsInvalidBins()
{
    quint16 distances[MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN];
    fillUnused(distances);
    distances[0] = 100;
    distances[1] = 125;
    distances[2] = 501; // max_distance + 1 means no obstacle.
    distances[3] = std::numeric_limits<quint16>::max();
    distances[4] = 19;  // Below the sensor's usable range.

    Proximity proximity(kSystemId);
    const Proximity::TimePoint received = Proximity::TimePoint{} + 20s;
    const mavlink_message_t message = obstacleDistanceMessage(
        kSystemId, MAV_FRAME_BODY_FRD, distances,
        0, 2.5F, 15.0F);

    // BODY_FRD is vehicle-relative, so a 90-degree vehicle yaw must not be
    // folded into the sample angles. Mission Planner uses increment_f when
    // the legacy integer increment is zero.
    QVERIFY(proximity.observeMessage(message, qDegreesToRadians(90.0), received));
    QVERIFY(proximity.DataAvailable());

    const QVector<Proximity::Sample> samples =
        proximity.directionState().GetRaw(received);
    QCOMPARE(samples.size(), 2);
    const Proximity::Sample *first = sampleAtAngle(samples, 15.0);
    const Proximity::Sample *second = sampleAtAngle(samples, 17.5);
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(first->Distance, 100.0);
    QCOMPARE(second->Distance, 125.0);
    QCOMPARE(first->Size, 2.5);
    QCOMPARE(second->Size, 2.5);
    QVERIFY(first->IsCustom());
    QVERIFY(second->IsCustom());
}

void ProximityRadarTest::obstacleDistanceGlobalFrameAddsVehicleHeading()
{
    quint16 distances[MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN];
    fillUnused(distances);
    distances[0] = 200;
    distances[1] = 250;

    Proximity proximity(kSystemId);
    const Proximity::TimePoint received = Proximity::TimePoint{} + 30s;
    const mavlink_message_t message = obstacleDistanceMessage(
        kSystemId, MAV_FRAME_GLOBAL, distances,
        15, 0.0F, 5.0F);

    QVERIFY(proximity.observeMessage(message, qDegreesToRadians(90.0), received));
    const QVector<Proximity::Sample> samples =
        proximity.directionState().GetRaw(received);
    QCOMPARE(samples.size(), 2);
    const Proximity::Sample *first = sampleAtAngle(samples, 95.0);
    const Proximity::Sample *second = sampleAtAngle(samples, 110.0);
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(first->Distance, 200.0);
    QCOMPARE(second->Distance, 250.0);

    quint16 unused[MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN];
    fillUnused(unused);
    Proximity emptyPacketState(kSystemId);
    const mavlink_message_t emptyPacket = obstacleDistanceMessage(
        kSystemId, MAV_FRAME_GLOBAL, unused,
        10, 0.0F, 0.0F);
    QVERIFY(emptyPacketState.observeMessage(emptyPacket, 0.0, received));
    QVERIFY(emptyPacketState.DataAvailable());
    QVERIFY(emptyPacketState.directionState().GetRaw(received).isEmpty());
}

void ProximityRadarTest::directionalAndCustomSamplesReplaceAndExpire()
{
    Proximity::DirectionState state;
    const Proximity::TimePoint start = Proximity::TimePoint{} + 40s;

    state.Add(1, MAV_SENSOR_ROTATION_NONE, 300.0, start, 1000ms);
    state.Add(1, MAV_SENSOR_ROTATION_NONE, 180.0, start + 100ms, 1000ms);
    state.Add(1, MAV_SENSOR_ROTATION_YAW_45, 260.0,
              start + 100ms, 1000ms);
    state.Add(2, 30.0, 5.0, 240.0, start + 100ms, 200ms);
    state.Add(2, 30.0, 7.5, 120.0, start + 150ms, 200ms);

    QVector<Proximity::Sample> samples = state.GetRaw(start + 150ms);
    QCOMPARE(samples.size(), 3);
    QCOMPARE(state.GetClosest(start + 150ms), 120.0);
    const Proximity::Sample *directional = sampleAtAngle(samples, 0.0);
    const Proximity::Sample *custom = sampleAtAngle(samples, 30.0);
    QVERIFY(directional);
    QVERIFY(custom);
    QCOMPARE(directional->Distance, 180.0);
    QCOMPARE(custom->Distance, 120.0);
    QCOMPARE(custom->Size, 7.5);

    const QVector<MAV_SENSOR_ORIENTATION> warnings =
        state.GetWarnings(200.0, start + 150ms);
    QCOMPARE(warnings.size(), 2);
    QVERIFY(warnings.contains(MAV_SENSOR_ROTATION_NONE));
    QVERIFY(warnings.contains(MAV_SENSOR_ROTATION_CUSTOM));

    // Expiry uses the same strict comparison as Mission Planner: a sample is
    // still available at ExpireTime and disappears immediately afterwards.
    QCOMPARE(state.GetRaw(start + 350ms).size(), 3);
    samples = state.GetRaw(start + 351ms);
    QCOMPARE(samples.size(), 2);
    QVERIFY(!sampleAtAngle(samples, 30.0));
    QCOMPARE(state.GetClosest(start + 1100ms), 180.0);
    QVERIFY(state.GetRaw(start + 1101ms).isEmpty());
    QCOMPARE(state.GetClosest(start + 1101ms),
             std::numeric_limits<double>::max());
}

void ProximityRadarTest::readersAndPacketUpdatesCanRunConcurrently()
{
    Proximity::DirectionState state;
    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};
    constexpr int iterations = 1500;

    auto writer = [&state, &start](quint32 sensorId, double angle) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int index = 0; index < iterations; ++index) {
            const auto now = Proximity::Clock::now();
            state.Add(sensorId, angle, 5.0,
                      100.0 + (index % 300), now,
                      std::chrono::minutes(1));
            state.Add(sensorId + 10,
                      static_cast<MAV_SENSOR_ORIENTATION>(
                          index % (MAV_SENSOR_ROTATION_YAW_315 + 1)),
                      150.0 + (index % 200), now,
                      std::chrono::minutes(1));
        }
    };
    auto reader = [&state, &start, &valid]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int index = 0; index < iterations; ++index) {
            const auto now = Proximity::Clock::now();
            const QVector<Proximity::Sample> raw = state.GetRaw(now);
            const double closest = state.GetClosest(now);
            const QVector<MAV_SENSOR_ORIENTATION> warnings =
                state.GetWarnings(250.0, now);
            if (closest < 0.0 || raw.size() > 20 || warnings.size() > 20) {
                valid.store(false, std::memory_order_release);
            }
        }
    };

    std::thread writerA(writer, 1U, 10.0);
    std::thread writerB(writer, 2U, 20.0);
    std::thread readerA(reader);
    std::thread readerB(reader);
    start.store(true, std::memory_order_release);
    writerA.join();
    writerB.join();
    readerA.join();
    readerB.join();

    QVERIFY(valid.load(std::memory_order_acquire));
    QVERIFY(!state.GetRaw().isEmpty());
}

void ProximityRadarTest::keyboardControlsAdjustRadarScale()
{
    Proximity proximity(kSystemId);
    ProximityRadarControl radar(&proximity, nullptr);
    QSignalSpy scaleSpy(&radar, &ProximityRadarControl::scaleChanged);

    QCOMPARE(radar.radiusCm(), 500.0);
    QCOMPARE(radar.vehicleSizeCm(), 80.0);

    QTest::keyClick(&radar, Qt::Key_Plus);
    QCOMPARE(radar.radiusCm(), 450.0);
    QTest::keyClick(&radar, Qt::Key_Equal);
    QCOMPARE(radar.radiusCm(), 400.0);
    QTest::keyClick(&radar, Qt::Key_Minus);
    QCOMPARE(radar.radiusCm(), 450.0);

    QTest::keyClick(&radar, Qt::Key_BracketLeft);
    QCOMPARE(radar.vehicleSizeCm(), 70.0);
    QTest::keyClick(&radar, Qt::Key_BracketRight);
    QCOMPARE(radar.vehicleSizeCm(), 80.0);
    QCOMPARE(scaleSpy.count(), 5);

    radar.setRadiusCm(50.0);
    QTest::keyClick(&radar, Qt::Key_Plus);
    QCOMPARE(radar.radiusCm(), 50.0);
    radar.setRadiusCm(std::numeric_limits<double>::infinity());
    QCOMPARE(radar.radiusCm(), 50.0);
    radar.setRadiusCm(1.0e12);
    QCOMPARE(radar.radiusCm(), 100000.0);
    radar.setVehicleSizeCm(10.0);
    QTest::keyClick(&radar, Qt::Key_BracketLeft);
    QCOMPARE(radar.vehicleSizeCm(), 10.0);
    radar.setVehicleSizeCm(std::numeric_limits<double>::infinity());
    QCOMPARE(radar.vehicleSizeCm(), 10.0);
    radar.setVehicleSizeCm(1.0e12);
    QCOMPARE(radar.vehicleSizeCm(), 100000.0);
}

void ProximityRadarTest::proximityWindowKeepsReferenceGeometryContract()
{
    ProximityWindow window;

    QCOMPARE(window.objectName(), QStringLiteral("ProximityWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Proximity"));
    QCOMPARE(window.size(), QSize(620, 620));
    QCOMPARE(window.minimumSize(), QSize(420, 420));
    QVERIFY(window.isWindow());
    QVERIFY(window.testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(window.Radar());
    QCOMPARE(window.Radar()->objectName(),
             QStringLiteral("ProximityRadarControl"));
    QCOMPARE(window.Radar()->sizeHint(), QSize(620, 620));
    QVERIFY(!window.activeUAS());
    QVERIFY(!window.ProximityState());
}

void ProximityRadarTest::radarRendersAVisibleSceneWithObstacleData()
{
    Proximity proximity(kSystemId);
    proximity.directionState().Add(
        1, MAV_SENSOR_ROTATION_NONE, 175.0,
        Proximity::Clock::now(), std::chrono::minutes(1));
    proximity.directionState().Add(
        2, 90.0, 12.0, 225.0,
        Proximity::Clock::now(), std::chrono::minutes(1));

    ProximityRadarControl radar(&proximity, nullptr);
    radar.resize(620, 620);
    QImage image(radar.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    radar.render(&image);

    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(620, 620));
    QVERIFY(fuzzyColor(image.pixelColor(0, 0), QColor(QStringLiteral("#151817"))));

    int backgroundPixels = 0;
    int coloredPixels = 0;
    int redObstaclePixels = 0;
    int goldObstaclePixels = 0;
    const QColor background(QStringLiteral("#151817"));
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (fuzzyColor(pixel, background)) {
                ++backgroundPixels;
            } else {
                ++coloredPixels;
            }
            if (pixel.red() > 150
                && pixel.red() > pixel.green() * 3
                && pixel.red() > pixel.blue() * 3) {
                ++redObstaclePixels;
            }
            if (pixel.red() > 180 && pixel.green() > 120
                && pixel.blue() < 80) {
                ++goldObstaclePixels;
            }
        }
    }

    QVERIFY(backgroundPixels > image.width() * image.height() / 2);
    QVERIFY(coloredPixels > 1000);
    QVERIFY(redObstaclePixels > 20);
    QVERIFY(goldObstaclePixels > 20);
}

QTEST_MAIN(ProximityRadarTest)
#include "test_proximityradar.moc"
