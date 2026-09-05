#include <QtTest>
#include "services/WarningTelemetrySource.h"
#include "comm/VehicleTargetManager.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace {
VehicleEndpoint endpoint(int link, int component=1)
{
    VehicleEndpoint e; e.linkId=link; e.systemId=42; e.componentId=component; return e;
}
mavlink_message_t heartbeat(int component=1)
{
    mavlink_message_t m{};
    mavlink_msg_heartbeat_pack(42,component,&m,MAV_TYPE_QUADROTOR,MAV_AUTOPILOT_ARDUPILOTMEGA,MAV_MODE_FLAG_SAFETY_ARMED,3,MAV_STATE_ACTIVE);
    return m;
}
}

class WarningTelemetrySourceTest : public QObject
{
    Q_OBJECT
private slots:
    void catalogAndEmptyState();
    void exactIdentityAndEpochReentrancy();
    void freshnessAndInvalidSentinels();
    void gpsAndFusedPosition();
    void batteryInstancesAndCells();
    void radioAndServoInstances();
    void imuPressureNavAndDerived();
    void homeDistanceAndEpoch();
    void missingHomeRequestsAreExactAndRateLimited();
    void homeRequestReentrancyAndDeletion();
    void trimmedPayloadAndV1Extensions();
    void clockReentrancyAndDestroyedTargets();
    void clockDeletionAndExtremeTimes();
};

void WarningTelemetrySourceTest::catalogAndEmptyState()
{
    WarningTelemetrySource source(nullptr);
    const auto fields=source.fieldNames();
    QCOMPARE(fields.size(),394);
    QCOMPARE(QSet<QString>(fields.cbegin(),fields.cend()).size(),fields.size());
    for (const auto &field : {"gpsh_acc2","battery_voltage9","current2","ch16in","ch32out","imu3_temp","pidSRate","DistToHome","ekfstatus","vibez"})
        QVERIFY2(fields.contains(field),field);
    QVERIFY(!fields.contains("mode"));
    QVERIFY(!fields.contains("_fusedPosition"));
    QCOMPARE(source.fieldUnits("alt"),QString("m"));
    QCOMPARE(source.fieldUnits("press_temp"),QString("cdegC"));
    QCOMPARE(source.fieldUnits("boardvoltage"),QString("mV"));
    QCOMPARE(source.fieldUnits("ch16in"),QString("us"));
    QVERIFY(source.values().isEmpty());
    qInfo() << "Mapped CurrentState numeric/boolean fields:" << fields.size();
}

void WarningTelemetrySourceTest::exactIdentityAndEpochReentrancy()
{
    VehicleTargetManager targets;
    targets.observeEndpoint(endpoint(1),true);
    targets.observeEndpoint(endpoint(2));
    targets.observeEndpoint(endpoint(2,2));
    WarningTelemetrySource source(&targets,[]{return 10;});
    const auto lease=source.lease();
    QVERIFY(source.isCurrentLease(lease));
    source.observeMessage(2,heartbeat());
    source.observeMessage(1,heartbeat(2));
    QVERIFY(source.values().isEmpty());
    source.observeMessage(1,heartbeat());
    QCOMPARE(source.values().value("armed"),1.0);
    const auto oldEpoch=source.epoch();
    bool redirected=false;
    connect(&source,&WarningTelemetrySource::epochChanged,&source,[&]{
        QVERIFY(source.values().isEmpty());
        if (!redirected) { redirected=true; targets.selectTarget(2,42,2); }
    });
    targets.selectTarget(2,42,1);
    QVERIFY(source.epoch()>oldEpoch);
    QVERIFY(!source.isCurrentLease(lease));
    QCOMPARE(source.lease().endpoint.componentId,2);
    source.observeMessage(2,heartbeat());
    QVERIFY(source.values().isEmpty());
    source.observeMessage(2,heartbeat(2));
    QCOMPARE(source.values().value("armed"),1.0);
    source.invalidateSourceEpoch();
    QVERIFY(source.values().isEmpty());
    targets.removeLink(2);
    source.observeMessage(2,heartbeat(2));
    QVERIFY(source.values().isEmpty());
}

void WarningTelemetrySourceTest::freshnessAndInvalidSentinels()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    qint64 now=0; WarningTelemetrySource source(&targets,[&]{return now;});
    mavlink_sys_status_t s{}; s.voltage_battery=12500; s.current_battery=230; s.battery_remaining=65; s.load=400;
    mavlink_message_t m{}; mavlink_msg_sys_status_encode(42,1,&m,&s); source.observeMessage(1,m);
    QCOMPARE(source.values().value("battery_voltage"),12.5);
    QCOMPARE(source.values().value("load"),40.0);
    QVERIFY(std::abs(source.values().value("watts")-28.75)<1e-9);
    now=5000; QVERIFY(source.values().contains("battery_voltage"));
    source.observeMessage(1,heartbeat());
    now=5001; QVERIFY(!source.values().contains("battery_voltage"));
    QVERIFY(source.values().contains("armed"));
    now=6000; s.voltage_battery=65535; s.current_battery=-1; s.battery_remaining=-1; s.load=65535;
    mavlink_msg_sys_status_encode(42,1,&m,&s); source.observeMessage(1,m);
    for (const auto &key : {"battery_voltage","current","battery_remaining","load","watts"}) QVERIFY(!source.values().contains(key));
    now=4000; QVERIFY(!source.values().contains("armed"));
}

void WarningTelemetrySourceTest::gpsAndFusedPosition()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    qint64 now=0; WarningTelemetrySource source(&targets,[&]{return now;});
    mavlink_gps_raw_int_t gps{}; gps.lat=10000000; gps.lon=20000000; gps.alt=123000; gps.fix_type=3;
    gps.eph=130; gps.satellites_visible=12; gps.vel=345; gps.cog=12345; gps.h_acc=1234; gps.yaw=36000;
    mavlink_message_t m{}; mavlink_msg_gps_raw_int_encode(42,1,&m,&gps); source.observeMessage(1,m);
    QCOMPARE(source.values().value("lat"),1.0); QCOMPARE(source.values().value("gpsh_acc"),1.234);
    QCOMPARE(source.values().value("gpsyaw"),360.0);
    mavlink_gps2_raw_t gps2{}; gps2.lat=-30000000; gps2.lon=40000000; gps2.eph=200; gps2.fix_type=3;
    gps2.satellites_visible=9; gps2.vel=65535; gps2.cog=65535; gps2.yaw=65535;
    mavlink_msg_gps2_raw_encode(42,1,&m,&gps2); source.observeMessage(1,m);
    QCOMPARE(source.values().value("satcountB"),21.0); QCOMPARE(source.values().value("lat2"),-3.0);
    QVERIFY(!source.values().contains("groundspeed2")); QVERIFY(!source.values().contains("gpsyaw2"));
    mavlink_global_position_int_t fused{}; fused.lat=50000000; fused.lon=60000000; fused.alt=150000; fused.relative_alt=20000;
    fused.vx=300; fused.vy=400; fused.vz=-500;
    mavlink_msg_global_position_int_encode(42,1,&m,&fused); source.observeMessage(1,m);
    mavlink_msg_gps_raw_int_encode(42,1,&m,&gps); source.observeMessage(1,m);
    QCOMPARE(source.values().value("lat"),5.0); QCOMPARE(source.values().value("alt"),20.0);
    QVERIFY(std::abs(source.values().value("vlen")-std::sqrt(50.0))<1e-9);
    QVERIFY(!source.values().contains("_fusedPosition"));
    now=5001; mavlink_msg_gps_raw_int_encode(42,1,&m,&gps); source.observeMessage(1,m);
    QCOMPARE(source.values().value("lat"),1.0);
    gps.eph=65535; gps.satellites_visible=255; gps.lat=std::numeric_limits<qint32>::max(); gps.yaw=0; gps.h_acc=0;
    mavlink_msg_gps_raw_int_encode(42,1,&m,&gps); source.observeMessage(1,m);
    for (const auto &key : {"lat","gpshdop","satcount","satcountB","gpsyaw","gpsh_acc"}) QVERIFY(!source.values().contains(key));
    gps.lat=0; gps.lon=0; gps.fix_type=1;
    mavlink_msg_gps_raw_int_encode(42,1,&m,&gps); source.observeMessage(1,m);
    QVERIFY(!source.values().contains("lat")); QVERIFY(!source.values().contains("groundspeed"));
}

void WarningTelemetrySourceTest::batteryInstancesAndCells()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    WarningTelemetrySource source(&targets,[]{return 0;});
    mavlink_battery_status_t battery{};
    for (int i = 0; i < 10; ++i) battery.voltages[i] = 65535;
    battery.voltages[0]=4200; battery.voltages[1]=4100; battery.voltages_ext[0]=4050;
    battery.current_battery=100; battery.current_consumed=1234; battery.battery_remaining=80; battery.temperature=2500; battery.time_remaining=120;
    mavlink_message_t m{};
    for (int id=0; id<9; ++id) { battery.id=id; mavlink_msg_battery_status_encode(42,1,&m,&battery); source.observeMessage(1,m); }
    QCOMPARE(source.values().value("battery_voltage9"),12.35);
    QCOMPARE(source.values().value("battery_usedmah2"),1234.0);
    QCOMPARE(source.values().value("battery_temp2"),25.0);
    QCOMPARE(source.values().value("battery_remainmin2"),2.0);
    QCOMPARE(source.values().value("battery_cell11"),4.05);
    QVERIFY(!source.values().contains("battery_cell12"));
    battery.id=0; battery.temperature=32767; battery.time_remaining=0; battery.current_consumed=-1;
    for (int i = 0; i < 10; ++i) battery.voltages[i] = 65535;
    battery.voltages_ext[0]=0;
    mavlink_msg_battery_status_encode(42,1,&m,&battery); source.observeMessage(1,m);
    for (const auto &key : {"battery_voltage","battery_temp","battery_remainmin","battery_usedmah","battery_cell1","battery_cell11"}) QVERIFY(!source.values().contains(key));
    QVERIFY(source.values().contains("battery_voltage2"));
    battery.current_battery=-230; battery.battery_remaining=127;
    mavlink_msg_battery_status_encode(42,1,&m,&battery); source.observeMessage(1,m);
    QCOMPARE(source.values().value("current"),-2.3);
    QVERIFY(!source.values().contains("battery_remaining"));
}

void WarningTelemetrySourceTest::radioAndServoInstances()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    WarningTelemetrySource source(&targets,[]{return 0;});
    mavlink_rc_channels_t rc{}; rc.chancount=16; rc.rssi=127; rc.chan1_raw=1200; rc.chan16_raw=1600;
    mavlink_message_t m{}; mavlink_msg_rc_channels_encode(42,1,&m,&rc); source.observeMessage(1,m);
    QCOMPARE(source.values().value("ch16in"),1600.0); QCOMPARE(source.values().value("rxrssi"),50.0);
    rc.chancount=8; rc.rssi=255; mavlink_msg_rc_channels_encode(42,1,&m,&rc); source.observeMessage(1,m);
    QVERIFY(!source.values().contains("ch16in")); QVERIFY(!source.values().contains("rxrssi"));
    mavlink_servo_output_raw_t servo{}; servo.port=1; servo.servo1_raw=1700; servo.servo16_raw=1800;
    mavlink_msg_servo_output_raw_encode(42,1,&m,&servo); source.observeMessage(1,m);
    QCOMPARE(source.values().value("ch17out"),1700.0); QCOMPARE(source.values().value("ch32out"),1800.0);
    QVERIFY(!source.values().contains("ch1out"));
    servo.port=0; servo.servo1_raw=1300; mavlink_msg_servo_output_raw_encode(42,1,&m,&servo); source.observeMessage(1,m);
    QCOMPARE(source.values().value("ch1out"),1300.0); QCOMPARE(source.values().value("ch17out"),1700.0);
}

void WarningTelemetrySourceTest::imuPressureNavAndDerived()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    WarningTelemetrySource source(&targets,[]{return 0;});
    mavlink_scaled_imu3_t imu{}; imu.xacc=300; imu.yacc=400; imu.zacc=0; imu.xgyro=123; imu.temperature=3000;
    mavlink_message_t m{}; mavlink_msg_scaled_imu3_encode(42,1,&m,&imu); source.observeMessage(1,m);
    QCOMPARE(source.values().value("accelsq3"),.5); QCOMPARE(source.values().value("imu3_temp"),30.0);
    QCOMPARE(source.values().value("gx3"),123.0); QVERIFY(!source.values().contains("ax"));
    mavlink_scaled_pressure_t pressure{}; pressure.press_abs=1013.25; pressure.temperature=2500;
    mavlink_msg_scaled_pressure_encode(42,1,&m,&pressure); source.observeMessage(1,m);
    QCOMPARE(source.values().value("press_abs"),1013.25); QCOMPARE(source.values().value("press_temp"),2500.0);
    mavlink_attitude_t attitude{}; attitude.yaw=-float(3.14159265358979323846/2); attitude.roll=std::numeric_limits<float>::quiet_NaN();
    mavlink_msg_attitude_encode(42,1,&m,&attitude); source.observeMessage(1,m);
    QVERIFY(std::abs(source.values().value("yaw")-270)<.0001); QVERIFY(!source.values().contains("roll"));
    mavlink_nav_controller_output_t nav{}; nav.aspd_error=123; nav.target_bearing=200;
    mavlink_msg_nav_controller_output_encode(42,1,&m,&nav); source.observeMessage(1,m);
    QCOMPARE(source.values().value("aspd_error"),1.23); QVERIFY(std::abs(source.values().value("ber_error")+70)<.0001);
    mavlink_ekf_status_report_t ekf{}; ekf.flags=EKF_ATTITUDE|EKF_VELOCITY_HORIZ; ekf.velocity_variance=.7f;
    mavlink_msg_ekf_status_report_encode(42,1,&m,&ekf); source.observeMessage(1,m);
    QVERIFY(std::abs(source.values().value("ekfstatus")-.7)<1e-6);
    ekf.flags=EKF_UNINITIALIZED; mavlink_msg_ekf_status_report_encode(42,1,&m,&ekf); source.observeMessage(1,m);
    QCOMPARE(source.values().value("ekfstatus"),1.0);
}

void WarningTelemetrySourceTest::homeDistanceAndEpoch()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    qint64 now=0; WarningTelemetrySource source(&targets,[&]{return now;});
    mavlink_home_position_t home{}; home.latitude=0; home.longitude=1799000000; home.altitude=123000;
    mavlink_message_t m{}; mavlink_msg_home_position_encode(42,1,&m,&home); source.observeMessage(1,m);
    QCOMPARE(source.values().value("HomeAlt"),123.0); QVERIFY(!source.values().contains("DistToHome"));
    now=10000; mavlink_global_position_int_t pos{}; pos.lat=0; pos.lon=-1799000000;
    mavlink_msg_global_position_int_encode(42,1,&m,&pos); source.observeMessage(1,m);
    QVERIFY(std::abs(source.values().value("DistToHome")-22238.9853)<.1);
    now=15001; QVERIFY(!source.values().contains("DistToHome")); QVERIFY(source.values().contains("HomeAlt"));
    source.invalidateSourceEpoch(); QVERIFY(source.values().isEmpty());
}

void WarningTelemetrySourceTest::missingHomeRequestsAreExactAndRateLimited()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    qint64 now=0; WarningTelemetrySource source(&targets,[&]{return now;});
    QSignalSpy requests(&source, &WarningTelemetrySource::homePositionRequested);
    mavlink_global_position_int_t position{};
    position.lat=351000000; position.lon=331000000;
    mavlink_message_t pos{}; mavlink_msg_global_position_int_encode(42,1,&pos,&position);
    source.observeMessage(1,heartbeat()); // Heartbeat alone never invents position.
    QCOMPARE(requests.size(),0);
    source.observeMessage(2,pos); QCOMPARE(requests.size(),0); // Foreign physical link.
    source.observeMessage(1,pos); QCOMPARE(requests.size(),1);
    const auto lease=qvariant_cast<VehicleTargetLease>(requests.first().at(0));
    QCOMPARE(lease.endpoint,endpoint(1)); QCOMPARE(lease.generation,source.lease().generation);
    QCOMPARE(requests.first().at(1).toULongLong(),source.epoch());
    QVERIFY(!source.values().contains("DistToHome"));
    for (const qint64 tick : {4999,5000,9999,10000,39999,40000}) {
        now=tick; source.observeMessage(1,heartbeat()); source.observeMessage(1,pos);
        QCOMPARE(requests.size(), now<5000 ? 1 : now<10000 ? 2 : now<40000 ? 3 : 4);
    }
    // No position refresh: even a live heartbeat must not keep requesting Home.
    now=80000; source.observeMessage(1,heartbeat()); QCOMPARE(requests.size(),4);
    mavlink_home_position_t home{}; home.latitude=position.lat; home.longitude=position.lon;
    mavlink_message_t reply{}; mavlink_msg_home_position_encode(42,1,&reply,&home);
    source.observeMessage(2,reply); // Foreign Home must not satisfy the request.
    source.observeMessage(1,pos); QCOMPARE(requests.size(),5);
    source.observeMessage(1,reply);
    QCOMPARE(source.values().value("DistToHome",-1),0.0); // Real coincident positions.
    now=120000; source.observeMessage(1,heartbeat()); source.observeMessage(1,pos);
    QCOMPARE(requests.size(),5); QCOMPARE(source.values().value("DistToHome",-1),0.0);
    source.invalidateSourceEpoch(); QVERIFY(source.values().isEmpty());
    source.observeMessage(1,pos); QCOMPARE(requests.size(),5);
    source.observeMessage(1,heartbeat()); QCOMPARE(requests.size(),6);
    targets.removeLink(1);
    now=160000; source.observeMessage(1,heartbeat()); source.observeMessage(1,pos);
    QCOMPARE(requests.size(),6);
}

void WarningTelemetrySourceTest::homeRequestReentrancyAndDeletion()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    qint64 now=0; WarningTelemetrySource source(&targets,[&]{return now;});
    mavlink_global_position_int_t position{}; position.lat=351000000; position.lon=331000000;
    mavlink_message_t pos{}; mavlink_msg_global_position_int_encode(42,1,&pos,&position);
    int requests=0;
    connect(&source,&WarningTelemetrySource::homePositionRequested,&source,[&]{
        ++requests;
        source.observeMessage(1,pos); // Same-turn input cannot recurse into another request.
        mavlink_home_position_t home{}; home.latitude=position.lat; home.longitude=position.lon;
        mavlink_message_t reply{}; mavlink_msg_home_position_encode(42,1,&reply,&home);
        source.observeMessage(1,reply);
    });
    source.observeMessage(1,heartbeat()); source.observeMessage(1,pos);
    QCOMPARE(requests,1); QCOMPARE(source.values().value("DistToHome",-1),0.0);

    auto *dying=new WarningTelemetrySource(&targets,[&]{return now;});
    QPointer<WarningTelemetrySource> guard(dying);
    connect(dying,&WarningTelemetrySource::homePositionRequested,&targets,[&]{delete dying;});
    dying->observeMessage(1,heartbeat()); dying->observeMessage(1,pos);
    QVERIFY(guard.isNull());
}

void WarningTelemetrySourceTest::trimmedPayloadAndV1Extensions()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    WarningTelemetrySource source(&targets,[]{return 0;});
    mavlink_gps_raw_int_t gps{}; gps.eph=123; gps.satellites_visible=10; gps.yaw=1;
    mavlink_message_t m{}; mavlink_msg_gps_raw_int_encode(42,1,&m,&gps);
    QVERIFY(m.len<MAVLINK_MSG_ID_GPS_RAW_INT_LEN);
    std::memset(_MAV_PAYLOAD_NON_CONST(&m)+m.len,0xA5,size_t(MAVLINK_MAX_PAYLOAD_LEN-m.len));
    source.observeMessage(1,m); QCOMPARE(source.values().value("gpsyaw"),.01);
    m.magic=MAVLINK_STX_MAVLINK1; m.len=MAVLINK_MSG_ID_GPS_RAW_INT_MIN_LEN;
    source.observeMessage(1,m); QVERIFY(!source.values().contains("gpsyaw"));
    QVERIFY(!source.values().contains("gpsh_acc")); QCOMPARE(source.values().value("gpshdop"),1.23);
}

void WarningTelemetrySourceTest::clockReentrancyAndDestroyedTargets()
{
    auto *targets=new VehicleTargetManager; targets->observeEndpoint(endpoint(1),true); targets->observeEndpoint(endpoint(2));
    bool redirect=false;
    WarningTelemetrySource source(targets,[&]{ if (redirect) { redirect=false; targets->selectTarget(2,42,1); } return 0; });
    redirect=true; source.observeMessage(1,heartbeat()); QVERIFY(source.values().isEmpty());
    source.observeMessage(2,heartbeat()); QVERIFY(!source.values().isEmpty());
    delete targets; targets=nullptr; QVERIFY(source.values().isEmpty());
}

void WarningTelemetrySourceTest::clockDeletionAndExtremeTimes()
{
    VehicleTargetManager targets; targets.observeEndpoint(endpoint(1),true);
    WarningTelemetrySource *source=nullptr;
    source=new WarningTelemetrySource(&targets,[&]{ delete source; source=nullptr; return 0; });
    source->observeMessage(1,heartbeat()); QVERIFY(!source);
    source=new WarningTelemetrySource(&targets,[&]{ delete source; source=nullptr; return 0; });
    QVERIFY(source->values().isEmpty()); QVERIFY(!source);
    qint64 now=0;
    WarningTelemetrySource clocked(&targets,[&]{return now;});
    clocked.observeMessage(1,heartbeat());
    now=std::numeric_limits<qint64>::max(); QVERIFY(clocked.values().isEmpty());
    clocked.observeMessage(1,heartbeat()); QVERIFY(clocked.values().contains("armed"));
    now=std::numeric_limits<qint64>::min(); QVERIFY(clocked.values().isEmpty());
}

QTEST_GUILESS_MAIN(WarningTelemetrySourceTest)
#include "test_warningtelemetrysource.moc"
