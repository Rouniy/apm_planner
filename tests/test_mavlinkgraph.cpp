#include "ui/MavlinkFieldGraphModel.h"
#include "ui/MavlinkGraphSampleExtractor.h"

#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

mavlink_message_t namedInt(quint8 systemId, quint8 componentId,
                           qint32 value)
{
    mavlink_message_t message{};
    char name[10] = {};
    std::memcpy(name, "graph", 5);
    mavlink_msg_named_value_int_pack(systemId, componentId, &message,
                                     10, name, value);
    return message;
}

mavlink_message_t namedFloat(float value)
{
    mavlink_message_t message{};
    char name[10] = {};
    mavlink_msg_named_value_float_pack(1, 1, &message, 10, name, value);
    return message;
}

mavlink_message_t actuatorControls(const float *values)
{
    mavlink_message_t message{};
    mavlink_msg_hil_actuator_controls_pack(
        1, 2, &message, 100, values, 0, 0);
    return message;
}

bool containsValue(const QVector<MavlinkGraphPoint> &points, double value)
{
    return std::any_of(points.constBegin(), points.constEnd(),
                       [value](const MavlinkGraphPoint &point) {
        return point.value == value;
    });
}

} // namespace

class MavlinkGraphTest final : public QObject
{
    Q_OBJECT

private slots:
    void identifiesOnlyNumericMetadataFields();
    void extractsScalarsArraysAndTrimmedExtensions();
    void rejectsNonFiniteArraysWithoutPartialOutput();
    void filtersExactIdentityAndBoundsHistory();
    void rejectsRegressingAndInvalidTimestamps();
    void createsOneLabeledSeriesPerArrayElement();
    void downsamplingPreservesExtremaAndFullHistory();
    void clearResetsSeriesAndTimestampEpoch();
};

void MavlinkGraphTest::identifiesOnlyNumericMetadataFields()
{
    QVERIFY(MavlinkGraphSampleExtractor::isSupportedField(
        MAVLINK_MSG_ID_NAMED_VALUE_INT, QStringLiteral("value")));
    QVERIFY(MavlinkGraphSampleExtractor::isSupportedField(
        MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS,
        QStringLiteral("controls")));
    QVERIFY(!MavlinkGraphSampleExtractor::isSupportedField(
        MAVLINK_MSG_ID_NAMED_VALUE_INT, QStringLiteral("name")));
    QVERIFY(!MavlinkGraphSampleExtractor::isSupportedField(
        MAVLINK_MSG_ID_NAMED_VALUE_INT, QStringLiteral("missing")));
    QVERIFY(!MavlinkGraphSampleExtractor::isSupportedField(
        0x00fffffeU, QStringLiteral("value")));
}

void MavlinkGraphTest::extractsScalarsArraysAndTrimmedExtensions()
{
    QVector<double> values;
    QVERIFY(MavlinkGraphSampleExtractor::tryRead(
        namedInt(1, 2, -1234567), QStringLiteral("value"), &values));
    QCOMPARE(values, QVector<double>({-1234567.0}));

    float controls[16] = {};
    controls[0] = 1.25F;
    controls[1] = -2.5F;
    controls[15] = 3.0F;
    QVERIFY(MavlinkGraphSampleExtractor::tryRead(
        actuatorControls(controls), QStringLiteral("controls"), &values));
    QCOMPARE(values.size(), 16);
    QCOMPARE(values.at(0), 1.25);
    QCOMPARE(values.at(1), -2.5);
    QCOMPARE(values.at(15), 3.0);

    mavlink_message_t battery{};
    battery.msgid = MAVLINK_MSG_ID_SMART_BATTERY_INFO;
    battery.len = 90;
    auto *payload = reinterpret_cast<quint8 *>(
        _MAV_PAYLOAD_NON_CONST(&battery));
    payload[90] = 0x78;
    payload[91] = 0x56;
    payload[92] = 0x34;
    payload[93] = 0x12;
    const mavlink_message_t trimmedBefore = battery;
    QVERIFY(MavlinkGraphSampleExtractor::tryRead(
        battery, QStringLiteral("discharge_maximum_current"), &values));
    QCOMPARE(values, QVector<double>({0.0}));
    QCOMPARE(std::memcmp(&battery, &trimmedBefore, sizeof(battery)), 0);
    battery.len = 94;
    QVERIFY(MavlinkGraphSampleExtractor::tryRead(
        battery, QStringLiteral("discharge_maximum_current"), &values));
    QCOMPARE(values, QVector<double>({305419896.0}));

    mavlink_message_t systemTime{};
    systemTime.msgid = MAVLINK_MSG_ID_SYSTEM_TIME;
    systemTime.len = MAVLINK_MSG_ID_SYSTEM_TIME_LEN;
    std::memset(_MAV_PAYLOAD_NON_CONST(&systemTime), 0xff,
                MAVLINK_MSG_ID_SYSTEM_TIME_LEN);
    QVERIFY(MavlinkGraphSampleExtractor::tryRead(
        systemTime, QStringLiteral("time_unix_usec"), &values));
    QCOMPARE(values, QVector<double>({
        static_cast<double>(std::numeric_limits<quint64>::max())}));
}

void MavlinkGraphTest::rejectsNonFiniteArraysWithoutPartialOutput()
{
    QVector<double> values{77.0};
    QVERIFY(!MavlinkGraphSampleExtractor::tryRead(
        namedFloat(std::numeric_limits<float>::quiet_NaN()),
        QStringLiteral("value"), &values));
    QVERIFY(values.isEmpty());

    values = {77.0};
    float controls[16] = {};
    controls[0] = 1.0F;
    controls[7] = std::numeric_limits<float>::infinity();
    QVERIFY(!MavlinkGraphSampleExtractor::tryRead(
        actuatorControls(controls), QStringLiteral("controls"), &values));
    QVERIFY(values.isEmpty());
    values = {77.0};
    QVERIFY(!MavlinkGraphSampleExtractor::tryRead(
        namedInt(1, 1, 1), QStringLiteral("name"), &values));
    QVERIFY(values.isEmpty());
}

void MavlinkGraphTest::filtersExactIdentityAndBoundsHistory()
{
    const MavlinkGraphSelection selection{
        1, 2, MAVLINK_MSG_ID_NAMED_VALUE_INT,
        QStringLiteral("NAMED_VALUE_INT"), QStringLiteral("value")};
    MavlinkFieldGraphModel model(selection, 1);
    QCOMPARE(model.history(), MavlinkFieldGraphModel::MinimumHistory);
    MavlinkFieldGraphModel maximumModel(
        selection, std::numeric_limits<int>::max());
    QCOMPARE(maximumModel.history(), MavlinkFieldGraphModel::MaximumHistory);
    QVERIFY(!model.observe(namedInt(2, 2, 1), 0.0));
    QVERIFY(!model.observe(namedInt(1, 3, 1), 0.0));
    QVERIFY(!model.observe(namedFloat(1.0F), 0.0));

    for (int index = 0; index < 12; ++index) {
        QVERIFY(model.observe(namedInt(1, 2, index), index));
    }
    QCOMPARE(model.seriesCount(), 1);
    QCOMPARE(model.storedPointCount(0), 10);
    const auto series = model.snapshot();
    QCOMPARE(series.size(), 1);
    QCOMPARE(series.constFirst().points.size(), 10);
    QCOMPARE(series.constFirst().points.constFirst().seconds, 2.0);
    QCOMPARE(series.constFirst().points.constFirst().value, 2.0);
    QCOMPARE(series.constFirst().points.constLast().seconds, 11.0);

    MavlinkGraphPoint latest;
    QVERIFY(model.latestValue(0, &latest));
    QCOMPARE(latest.seconds, 11.0);
    QCOMPARE(latest.value, 11.0);
    QVERIFY(!model.latestValue(1, &latest));
}

void MavlinkGraphTest::rejectsRegressingAndInvalidTimestamps()
{
    const MavlinkGraphSelection selection{
        1, 2, MAVLINK_MSG_ID_NAMED_VALUE_INT,
        QStringLiteral("NAMED_VALUE_INT"), QStringLiteral("value")};
    MavlinkFieldGraphModel model(selection);
    QVERIFY(model.observe(namedInt(1, 2, 1), 5.0));
    QVERIFY(!model.observe(namedInt(1, 2, 2), 4.0));
    QVERIFY(!model.observe(namedInt(1, 2, 2), -1.0));
    QVERIFY(!model.observe(
        namedInt(1, 2, 2), std::numeric_limits<double>::infinity()));
    QCOMPARE(model.storedPointCount(0), 1);
    MavlinkGraphPoint latest;
    QVERIFY(model.latestValue(0, &latest));
    QCOMPARE(latest.value, 1.0);
}

void MavlinkGraphTest::createsOneLabeledSeriesPerArrayElement()
{
    const MavlinkGraphSelection selection{
        1, 2, MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS,
        QStringLiteral("HIL_ACTUATOR_CONTROLS"),
        QStringLiteral("controls")};
    MavlinkFieldGraphModel model(selection);
    float controls[16] = {};
    controls[3] = 0.75F;
    QVERIFY(model.observe(actuatorControls(controls), 0.25));
    QCOMPARE(model.seriesCount(), 16);
    const auto series = model.snapshot();
    QCOMPARE(series.at(0).label,
             QStringLiteral("HIL_ACTUATOR_CONTROLS.controls[0]"));
    QCOMPARE(series.at(3).label,
             QStringLiteral("HIL_ACTUATOR_CONTROLS.controls[3]"));
    QCOMPARE(series.at(3).points.constFirst().value, 0.75);
}

void MavlinkGraphTest::downsamplingPreservesExtremaAndFullHistory()
{
    const MavlinkGraphSelection selection{
        1, 2, MAVLINK_MSG_ID_NAMED_VALUE_INT,
        QStringLiteral("NAMED_VALUE_INT"), QStringLiteral("value")};
    MavlinkFieldGraphModel model(selection, 3000);
    for (int index = 0; index < 2500; ++index) {
        int value = index;
        if (index == 23) {
            value = 9999;
        } else if (index == 2074) {
            value = -8888;
        }
        QVERIFY(model.observe(namedInt(1, 2, value), index));
    }

    QCOMPARE(model.storedPointCount(0), 2500);
    const auto plotted = model.snapshot(10);
    QCOMPARE(plotted.size(), 1);
    QVERIFY(plotted.constFirst().points.size() <= 10);
    QCOMPARE(plotted.constFirst().points.constFirst().seconds, 0.0);
    QCOMPARE(plotted.constFirst().points.constLast().seconds, 2499.0);
    QVERIFY(containsValue(plotted.constFirst().points, 9999.0));
    QVERIFY(containsValue(plotted.constFirst().points, -8888.0));
    const auto defaultPlot = model.snapshot(std::numeric_limits<int>::max());
    QVERIFY(defaultPlot.constFirst().points.size()
            <= MavlinkFieldGraphModel::MaximumPlottedPointsPerSeries);
    QCOMPARE(model.storedPointCount(0), 2500);
}

void MavlinkGraphTest::clearResetsSeriesAndTimestampEpoch()
{
    const MavlinkGraphSelection selection{
        1, 2, MAVLINK_MSG_ID_NAMED_VALUE_INT,
        QStringLiteral("NAMED_VALUE_INT"), QStringLiteral("value")};
    MavlinkFieldGraphModel model(selection);
    QVERIFY(model.observe(namedInt(1, 2, 1), 50.0));
    model.clear();
    QCOMPARE(model.seriesCount(), 0);
    QVERIFY(model.snapshot().isEmpty());
    QVERIFY(model.observe(namedInt(1, 2, 2), 1.0));
    QCOMPARE(model.seriesCount(), 1);
}

QTEST_APPLESS_MAIN(MavlinkGraphTest)
#include "test_mavlinkgraph.moc"
