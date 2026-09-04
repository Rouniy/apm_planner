#include <QtTest>

#include "comm/CompassCalibrationService.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "ui/configuration/ConfigCompassView.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTableView>
#include <QTimer>

#include <cmath>

namespace {
quint32 deviceId(int busType, int bus, int address, int devType) {
  return quint32(busType & 0x7) | (quint32(bus & 0x1f) << 3) |
         (quint32(address & 0xff) << 8) | (quint32(devType & 0xff) << 16);
}

ParameterMetaDataCatalog catalogFixture() {
  QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:COMPASS_EXTERNAL" humanName="Compass 1 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_EXTERN2" humanName="Compass 2 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_EXTERN3" humanName="Compass 3 External">
          <values><value code="0">Internal</value><value code="1">External</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT" humanName="Compass 1 Orientation">
          <values><value code="0">None</value><value code="2">Yaw 90</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT2" humanName="Compass 2 Orientation">
          <values><value code="0">None</value><value code="2">Yaw 90</value></values>
        </param>
        <param name="ArduCopter:COMPASS_ORIENT3" humanName="Compass 3 Orientation">
          <values><value code="0">None</value><value code="2">Yaw 90</value></values>
        </param>
        <param name="ArduCopter:COMPASS_PRIMARY" humanName="Primary Compass">
          <values><value code="0">Compass 1</value><value code="1">Compass 2</value></values>
        </param>
        <param name="ArduCopter:COMPASS_AUTODEC" humanName="Automatic Declination">
          <values><value code="0">Disabled</value><value code="1">Enabled</value></values>
        </param>
        <param name="ArduCopter:COMPASS_CAL_FIT" humanName="Calibration Fitness">
          <values><value code="8">Relaxed</value><value code="16">Default</value></values>
        </param>
      </parameters></vehicles></paramfile>)xml");
  QBuffer buffer(&xml);
  buffer.open(QIODevice::ReadOnly);
  return ParameterMetaDataCatalog::fromPdef(&buffer,
                                            QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot() {
  const quint32 first = deviceId(2, 3, 42, 7);
  const quint32 second = deviceId(1, 0, 0x0e, 0x0d);
  const quint32 third = deviceId(3, 0, 125, 7);
  return {{1, QStringLiteral("COMPASS_PRIO1_ID"), first},
          {1, QStringLiteral("COMPASS_PRIO2_ID"), second},
          {1, QStringLiteral("COMPASS_PRIO3_ID"), third},
          {1, QStringLiteral("COMPASS_DEV_ID"), first},
          {1, QStringLiteral("COMPASS_DEV_ID2"), second},
          {1, QStringLiteral("COMPASS_DEV_ID3"), third},
          {1, QStringLiteral("COMPASS_USE"), 1},
          {1, QStringLiteral("COMPASS_USE2"), 0},
          {1, QStringLiteral("COMPASS_USE3"), 1},
          {1, QStringLiteral("COMPASS_LEARN"), 1},
          {1, QStringLiteral("COMPASS_EXTERNAL"), 0},
          {1, QStringLiteral("COMPASS_EXTERN2"), 1},
          {1, QStringLiteral("COMPASS_EXTERN3"), 1},
          {1, QStringLiteral("COMPASS_ORIENT"), 0},
          {1, QStringLiteral("COMPASS_ORIENT2"), 2},
          {1, QStringLiteral("COMPASS_ORIENT3"), 0},
          {1, QStringLiteral("COMPASS_PRIMARY"), 1},
          {1, QStringLiteral("COMPASS_AUTODEC"), 1},
          {1, QStringLiteral("COMPASS_CAL_FIT"), 16},
          {1, QStringLiteral("COMPASS_DEC"), 0.2}};
}

QVariant priorityValue(const QVariantList &changes, const QString &name) {
  for (const QVariant &item : changes) {
    const QVariantMap change = item.toMap();
    if (change.value(QStringLiteral("name")).toString() == name) {
      return change.value(QStringLiteral("value"));
    }
  }
  return {};
}

VehicleEndpoint calibrationEndpoint() {
  VehicleEndpoint endpoint;
  endpoint.linkId = 17;
  endpoint.systemId = 42;
  endpoint.componentId = 1;
  endpoint.linkName = QStringLiteral("Test Link");
  endpoint.componentName = QStringLiteral("AUTOPILOT1");
  return endpoint;
}

mavlink_message_t commandAck(MAV_CMD command, MAV_RESULT result) {
  mavlink_command_ack_t payload{};
  payload.command = static_cast<quint16>(command);
  payload.result = static_cast<quint8>(result);
  payload.target_system = 255;
  payload.target_component = MAV_COMP_ID_MISSIONPLANNER;
  mavlink_message_t message{};
  mavlink_msg_command_ack_encode(42, 1, &message, &payload);
  return message;
}

mavlink_message_t calibrationProgress(int percent) {
  mavlink_mag_cal_progress_t payload{};
  payload.compass_id = 0;
  payload.cal_mask = 1;
  payload.cal_status = MAG_CAL_RUNNING_STEP_ONE;
  payload.completion_pct = static_cast<quint8>(percent);
  mavlink_message_t message{};
  mavlink_msg_mag_cal_progress_encode(42, 1, &message, &payload);
  return message;
}

mavlink_message_t calibrationReport(bool autosaved) {
  mavlink_mag_cal_report_t payload{};
  payload.compass_id = 0;
  payload.cal_mask = 1;
  payload.cal_status = MAG_CAL_SUCCESS;
  payload.autosaved = autosaved ? 1 : 0;
  payload.ofs_x = 1.0F;
  payload.ofs_y = 2.0F;
  payload.ofs_z = 3.0F;
  payload.fitness = 4.0F;
  mavlink_message_t message{};
  mavlink_msg_mag_cal_report_encode(42, 1, &message, &payload);
  return message;
}

struct CalibrationHarness {
  VehicleTargetManager targets;
  QVector<QByteArray> frames;
  ExactLinkTransmitter transmitter;
  VehicleCommandService commands;
  CompassCalibrationService service;

  CalibrationHarness()
      : transmitter([this](int, const QByteArray &bytes) {
          frames.append(bytes);
          return true;
        }),
        commands(&targets, &transmitter), service(&targets, &commands) {
    targets.observeEndpoint(calibrationEndpoint(), true);
  }

  VehicleTargetLease lease() const { return targets.acquireTarget(); }
};
} // namespace

class ConfigCompassViewTest final : public QObject {
  Q_OBJECT

private slots:
  void surfaceIsCompleteBeforeSnapshot();
  void snapshotHydratesWithoutWrites();
  void exactCalibrationContextEnablesSafeSurface();
  void calibrationStateDrivesButtonsProgressAndResult();
  void calibrationRebootGateDefaultsToCancel();
  void calibrationRebootAckClearsExactServiceLatch();
  void generationChangeReleasesCalibrationUiForNewTarget();
  void fixedYawDialogDefaultsToCancelAndValidatesHeading();
  void armedAndPendingDisableEdits();
  void selectedRowMoveUsesExactModelRow();
  void rebootDefaultsToCancel();
  void destructionDoesNotEmitWrite();
};

void ConfigCompassViewTest::surfaceIsCompleteBeforeSnapshot() {
  ConfigCompassView view;
  view.resize(1100, 850);
  view.show();
  QApplication::processEvents();

  QCOMPARE(view.objectName(), QStringLiteral("ConfigCompassView"));
  QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassTitle")));
  QVERIFY(
      view.findChild<QLabel *>(QStringLiteral("compassPriorityInstruction")));
  QTableView *const table =
      view.findChild<QTableView *>(QStringLiteral("compassTable"));
  QVERIFY(table);
  QCOMPARE(table->model()->columnCount(),
           int(ConfigCompassViewModel::ColumnCount));
  QCOMPARE(table->model()->columnCount(), 11);
  const QStringList headers{
      QStringLiteral("Priority"),    QStringLiteral("DevID"),
      QStringLiteral("BusType"),     QStringLiteral("Bus"),
      QStringLiteral("Address"),     QStringLiteral("DevType"),
      QStringLiteral("Missing"),     QStringLiteral("External"),
      QStringLiteral("Orientation"), QStringLiteral("Up"),
      QStringLiteral("Down")};
  for (int column = 0; column < headers.size(); ++column) {
    QCOMPARE(table->model()
                 ->headerData(column, Qt::Horizontal, Qt::DisplayRole)
                 .toString(),
             headers.at(column));
  }
  QVERIFY(!table->isEnabled());

  QVERIFY(
      view.findChild<QGroupBox *>(QStringLiteral("compassCalibrationGroup")));
  QVERIFY(view.findChild<QGroupBox *>(QStringLiteral("compassAdvancedGroup")));
  QCOMPARE(ConfigCompassViewModel::AdvancedFieldNames().size(), 9);
  for (const QString &name : ConfigCompassViewModel::AdvancedFieldNames()) {
    QWidget *const row =
        view.findChild<QWidget *>(QStringLiteral("compassField_%1").arg(name));
    QWidget *const editor =
        view.findChild<QWidget *>(QStringLiteral("compassEditor_%1").arg(name));
    QVERIFY2(row, qPrintable(name));
    QVERIFY2(editor, qPrintable(name));
    QVERIFY(!editor->isEnabled());
  }
  QVERIFY(view.findChild<QComboBox *>(
      QStringLiteral("compassEditor_COMPASS_EXTERNAL")));
  QVERIFY(view.findChild<QCheckBox *>(
      QStringLiteral("compassEditor_COMPASS_AUTODEC")));
  QVERIFY(view.findChild<QDoubleSpinBox *>(
      QStringLiteral("compassEditor_COMPASS_ORIENT")));

  const QStringList gatedCalibrationButtons{
      QStringLiteral("compassCalStart"), QStringLiteral("compassCalAccept"),
      QStringLiteral("compassCalCancel"),
      QStringLiteral("compassLargeVehicleMagCal")};
  for (const QString &name : gatedCalibrationButtons) {
    QPushButton *const button = view.findChild<QPushButton *>(name);
    QVERIFY2(button, qPrintable(name));
    QVERIFY(!button->isEnabled());
  }
  QPushButton *const fromLog =
      view.findChild<QPushButton *>(QStringLiteral("compassCalFromLog"));
  QVERIFY(fromLog && !fromLog->isEnabled());
  QVERIFY(fromLog->toolTip().contains(QStringLiteral("offline mag-fit")));
  QVERIFY(view.findChild<QLabel *>(
      QStringLiteral("compassCalTargetStatus")));
  QPlainTextEdit *const result =
      view.findChild<QPlainTextEdit *>(QStringLiteral("compassCalResult"));
  QVERIFY(result && result->isReadOnly());
  for (int index = 1; index <= 3; ++index) {
    QProgressBar *const progress = view.findChild<QProgressBar *>(
        QStringLiteral("compassCalProgress%1").arg(index));
    QVERIFY(progress);
    QVERIFY(!progress->isEnabled());
  }

  QVERIFY(!view.findChild<QPushButton *>(QStringLiteral("compassRefresh"))
               ->isEnabled());
  QPushButton *const quick =
      view.findChild<QPushButton *>(QStringLiteral("compassQuickPixhawk"));
  QVERIFY(quick && !quick->isEnabled());
  QVERIFY(quick->toolTip().contains(QStringLiteral("COMPASS_EXTERNAL")));
  QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassStatus")));
  QVERIFY(view.findChild<QScrollArea *>(QStringLiteral("compassScroll")));

  QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  view.render(&painter);
  painter.end();
  QVERIFY(image.pixelColor(0, 0).alpha() > 0);
}

void ConfigCompassViewTest::snapshotHydratesWithoutWrites() {
  ConfigCompassView view;
  QSignalSpy writes(&view, &ConfigCompassView::writeRequested);
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.show();
  QApplication::processEvents();

  QCOMPARE(writes.count(), 0);
  QVERIFY(
      view.findChild<QCheckBox *>(QStringLiteral("compassUse1"))->isChecked());
  QVERIFY(
      !view.findChild<QCheckBox *>(QStringLiteral("compassUse2"))->isChecked());
  QVERIFY(
      view.findChild<QCheckBox *>(QStringLiteral("compassUse3"))->isChecked());
  QVERIFY(
      view.findChild<QCheckBox *>(QStringLiteral("compassLearn"))->isChecked());
  QVERIFY(view.findChild<QCheckBox *>(
                  QStringLiteral("compassEditor_COMPASS_AUTODEC"))
              ->isChecked());
  QVERIFY(std::abs(view.findChild<QDoubleSpinBox *>(
                           QStringLiteral("compassDeclination"))
                       ->value() -
                   0.2 * 180.0 / std::acos(-1.0)) < 0.001);
  QCOMPARE(view.findChild<QTableView *>(QStringLiteral("compassTable"))
               ->model()
               ->rowCount(),
           3);
  QVERIFY(view.findChild<QPushButton *>(QStringLiteral("compassReboot"))
              ->isEnabled());
}

void ConfigCompassViewTest::exactCalibrationContextEnablesSafeSurface() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.setArmed(false);
  view.setCalibrationContext(&harness.service, harness.lease());
  view.show();
  QApplication::processEvents();

  auto *start =
      view.findChild<QPushButton *>(QStringLiteral("compassCalStart"));
  auto *accept =
      view.findChild<QPushButton *>(QStringLiteral("compassCalAccept"));
  auto *cancel =
      view.findChild<QPushButton *>(QStringLiteral("compassCalCancel"));
  auto *fromLog =
      view.findChild<QPushButton *>(QStringLiteral("compassCalFromLog"));
  auto *large = view.findChild<QPushButton *>(
      QStringLiteral("compassLargeVehicleMagCal"));
  QVERIFY(start && start->isEnabled());
  QVERIFY(large && large->isEnabled());
  QVERIFY(accept && !accept->isEnabled());
  QVERIFY(cancel && !cancel->isEnabled());
  QVERIFY(fromLog && !fromLog->isEnabled());
  QVERIFY(fromLog->toolTip().contains(QStringLiteral("offline mag-fit")));
  QVERIFY(!start->toolTip().contains(QStringLiteral("unavailable")));
  QVERIFY(!large->toolTip().contains(QStringLiteral("unavailable")));

  QLabel *const target = view.findChild<QLabel *>(
      QStringLiteral("compassCalTargetStatus"));
  QVERIFY(target);
  QVERIFY(target->text().contains(QStringLiteral("link 17")));
  QVERIFY(target->text().contains(QStringLiteral("system 42")));
  QVERIFY(target->text().contains(QStringLiteral("component 1")));
  QVERIFY(target->text().contains(
      QStringLiteral("generation %1").arg(harness.lease().generation)));

  for (int index = 1; index <= 3; ++index) {
    QProgressBar *const progress = view.findChild<QProgressBar *>(
        QStringLiteral("compassCalProgress%1").arg(index));
    QVERIFY(progress && progress->isEnabled());
    QCOMPARE(progress->value(), 0);
  }

  view.setArmed(true);
  QApplication::processEvents();
  QVERIFY(!start->isEnabled());
  QVERIFY(!large->isEnabled());
  view.setArmed(false);
  view.setParameterSnapshot(snapshot(), 1, false);
  QApplication::processEvents();
  QVERIFY(!start->isEnabled());
  QVERIFY(!large->isEnabled());
}

void ConfigCompassViewTest::calibrationStateDrivesButtonsProgressAndResult() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.setCalibrationContext(&harness.service, harness.lease());
  view.show();
  QApplication::processEvents();

  auto *start =
      view.findChild<QPushButton *>(QStringLiteral("compassCalStart"));
  auto *accept =
      view.findChild<QPushButton *>(QStringLiteral("compassCalAccept"));
  auto *cancel =
      view.findChild<QPushButton *>(QStringLiteral("compassCalCancel"));
  auto *result =
      view.findChild<QPlainTextEdit *>(QStringLiteral("compassCalResult"));
  auto *firstProgress = view.findChild<QProgressBar *>(
      QStringLiteral("compassCalProgress1"));
  QVERIFY(start && accept && cancel && result && firstProgress);

  start->click();
  QCOMPARE(harness.service.state(),
           CompassCalibrationService::State::StartPending);
  QCOMPARE(harness.frames.size(), 1);
  QVERIFY(!start->isEnabled());
  QVERIFY(!accept->isEnabled());
  QVERIFY(!cancel->isEnabled());
  QVERIFY(result->toPlainText().contains(
      QStringLiteral("Starting onboard magnetometer calibration")));

  harness.commands.observeMessage(
      17, commandAck(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED));
  QCOMPARE(harness.service.state(),
           CompassCalibrationService::State::Running);
  QVERIFY(cancel->isEnabled());
  view.setArmed(true);
  QVERIFY(cancel->isEnabled());
  view.setArmed(false);
  harness.service.observeMessage(17, calibrationProgress(37));
  QCOMPARE(firstProgress->value(), 37);

  harness.service.observeMessage(17, calibrationReport(false));
  QCOMPARE(harness.service.state(),
           CompassCalibrationService::State::AwaitingAccept);
  QCOMPARE(firstProgress->value(), 100);
  QVERIFY(accept->isEnabled());
  QVERIFY(cancel->isEnabled());
  QVERIFY(result->toPlainText().contains(QStringLiteral("MAG_CAL_SUCCESS")));

  accept->click();
  QCOMPARE(harness.service.state(),
           CompassCalibrationService::State::AcceptPending);
  QCOMPARE(harness.frames.size(), 2);
  QVERIFY(!accept->isEnabled());
  QVERIFY(!cancel->isEnabled());
}

void ConfigCompassViewTest::calibrationRebootGateDefaultsToCancel() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.setCalibrationContext(&harness.service, harness.lease());
  view.show();
  QApplication::processEvents();

  QTableView *const table =
      view.findChild<QTableView *>(QStringLiteral("compassTable"));
  QPushButton *const moveUp =
      view.findChild<QPushButton *>(QStringLiteral("compassMoveUp"));
  table->selectRow(1);
  table->setCurrentIndex(table->model()->index(1, 0));
  QApplication::processEvents();
  QSignalSpy writes(&view, &ConfigCompassView::writeRequested);
  moveUp->click();
  QCOMPARE(writes.count(), 1);
  const quint64 writeRequestId = writes.first().at(0).toULongLong();
  const int changeCount = writes.first().at(2).toList().size();
  view.parameterWriteSubmitted(writeRequestId, 77);
  view.parameterBatchCompleted(77, changeCount, 0);
  QVERIFY(view.viewModel()->RebootRequired());

  QSignalSpy rebootRequests(&view, &ConfigCompassView::rebootRequested);
  bool inspected = false;
  QTimer::singleShot(0, &view, [&inspected]() {
    auto *confirmation =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    QVERIFY(confirmation);
    QCOMPARE(confirmation->objectName(),
             QStringLiteral("compassRebootConfirmation"));
    QCOMPARE(confirmation->standardButton(confirmation->defaultButton()),
             QMessageBox::Cancel);
    inspected = true;
    confirmation->reject();
  });
  view.findChild<QPushButton *>(QStringLiteral("compassCalStart"))->click();
  QVERIFY(inspected);
  QCOMPARE(rebootRequests.count(), 0);
  QCOMPARE(harness.frames.size(), 0);
  QCOMPARE(harness.service.state(), CompassCalibrationService::State::Idle);
}

void ConfigCompassViewTest::calibrationRebootAckClearsExactServiceLatch() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  const VehicleTargetLease lease = harness.lease();
  view.setCalibrationContext(&harness.service, lease);
  view.show();
  QApplication::processEvents();

  QCOMPARE(harness.service.start(lease, false),
           CompassCalibrationService::RequestResult::Started);
  harness.commands.observeMessage(
      17, commandAck(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED));
  harness.service.observeMessage(17, calibrationReport(true));
  QVERIFY(harness.service.rebootRequiredFor(lease));

  QSignalSpy rebootRequests(&view, &ConfigCompassView::rebootRequested);
  bool inspected = false;
  QTimer::singleShot(0, &view, [&inspected]() {
    auto *confirmation =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    QVERIFY(confirmation);
    QCOMPARE(confirmation->standardButton(confirmation->defaultButton()),
             QMessageBox::Cancel);
    inspected = true;
    confirmation->done(QMessageBox::Yes);
  });
  view.findChild<QPushButton *>(QStringLiteral("compassReboot"))->click();
  QVERIFY(inspected);
  QCOMPARE(rebootRequests.count(), 1);
  const quint64 requestId = rebootRequests.first().at(0).toULongLong();
  view.rebootSubmitted(requestId);
  view.rebootAcknowledged(requestId, true);
  QVERIFY(!harness.service.rebootRequiredFor(lease));
  QCOMPARE(harness.service.state(), CompassCalibrationService::State::Idle);
}

void ConfigCompassViewTest::generationChangeReleasesCalibrationUiForNewTarget() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  const VehicleTargetLease first = harness.lease();
  view.setCalibrationContext(&harness.service, first);
  view.show();
  QApplication::processEvents();

  harness.service.setTimeoutsForTesting(20, 500, 500);
  QCOMPARE(harness.service.start(first, false),
           CompassCalibrationService::RequestResult::Started);
  QTRY_COMPARE_WITH_TIMEOUT(
      harness.service.state(),
      CompassCalibrationService::State::OutcomeUncertain, 200);

  VehicleEndpoint second = calibrationEndpoint();
  second.linkId = 18;
  second.systemId = 43;
  second.linkName = QStringLiteral("Second Link");
  harness.targets.observeEndpoint(second);
  QVERIFY(harness.targets.selectTarget(18, 43, 1));
  QCOMPARE(harness.service.state(), CompassCalibrationService::State::Idle);
  QVERIFY(!harness.service.hasActiveTarget());
  QApplication::processEvents();
  QVERIFY(!view.findChild<QPushButton *>(QStringLiteral("compassCalStart"))
               ->isEnabled());

  view.setCalibrationContext(&harness.service, harness.targets.acquireTarget());
  QApplication::processEvents();
  auto *start =
      view.findChild<QPushButton *>(QStringLiteral("compassCalStart"));
  auto *large = view.findChild<QPushButton *>(
      QStringLiteral("compassLargeVehicleMagCal"));
  QVERIFY(start && start->isEnabled());
  QVERIFY(large && large->isEnabled());
  QVERIFY(view.findChild<QLabel *>(QStringLiteral("compassCalTargetStatus"))
              ->text()
              .contains(QStringLiteral("system 43")));
}

void ConfigCompassViewTest::fixedYawDialogDefaultsToCancelAndValidatesHeading() {
  CalibrationHarness harness;
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.setCalibrationContext(&harness.service, harness.lease());
  view.show();
  QApplication::processEvents();

  bool inspected = false;
  QTimer::singleShot(0, &view, [&inspected]() {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    QVERIFY(dialog);
    QCOMPARE(dialog->objectName(), QStringLiteral("compassFixedYawDialog"));
    auto *heading = dialog->findChild<QDoubleSpinBox *>(
        QStringLiteral("compassFixedYawHeading"));
    QVERIFY(heading);
    QCOMPARE(heading->minimum(), 0.0);
    QCOMPARE(heading->maximum(), 360.0);
    auto *buttons = dialog->findChild<QDialogButtonBox *>(
        QStringLiteral("compassFixedYawButtons"));
    QVERIFY(buttons);
    QCOMPARE(buttons->button(QDialogButtonBox::Cancel)->isDefault(), true);
    inspected = true;
    dialog->reject();
  });
  view.findChild<QPushButton *>(
          QStringLiteral("compassLargeVehicleMagCal"))
      ->click();
  QVERIFY(inspected);
  QCOMPARE(harness.frames.size(), 0);
  QCOMPARE(harness.service.state(), CompassCalibrationService::State::Idle);
}

void ConfigCompassViewTest::armedAndPendingDisableEdits() {
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.setArmed(true);
  view.show();
  QApplication::processEvents();

  QCheckBox *const use1 =
      view.findChild<QCheckBox *>(QStringLiteral("compassUse1"));
  QVERIFY(use1);
  QVERIFY(!use1->isEnabled());
  QVERIFY(
      !view.findChild<QPushButton *>(QStringLiteral("compassWriteDeclination"))
           ->isEnabled());

  view.setArmed(false);
  QApplication::processEvents();
  QVERIFY(use1->isEnabled());
  QSignalSpy writes(&view, &ConfigCompassView::writeRequested);
  use1->click();
  QCOMPARE(writes.count(), 1);
  QVERIFY(view.viewModel()->HasPendingWrites());
  QVERIFY(
      !view.findChild<QCheckBox *>(QStringLiteral("compassUse2"))->isEnabled());
  QVERIFY(
      !view.findChild<QPushButton *>(QStringLiteral("compassWriteDeclination"))
           ->isEnabled());
}

void ConfigCompassViewTest::selectedRowMoveUsesExactModelRow() {
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.show();
  QApplication::processEvents();

  QTableView *const table =
      view.findChild<QTableView *>(QStringLiteral("compassTable"));
  QPushButton *const moveUp =
      view.findChild<QPushButton *>(QStringLiteral("compassMoveUp"));
  QVERIFY(table && moveUp);
  table->selectRow(1);
  table->setCurrentIndex(table->model()->index(1, 0));
  QApplication::processEvents();
  QVERIFY(moveUp->isEnabled());

  QSignalSpy writes(&view, &ConfigCompassView::writeRequested);
  moveUp->click();
  QCOMPARE(writes.count(), 1);
  const QVariantList changes = writes.first().at(2).toList();
  QCOMPARE(changes.size(), 3);
  const QList<ConfigFriendlyParameterValue> values = snapshot();
  const quint32 originalFirst = values.at(0).value.toUInt();
  const quint32 originalSecond = values.at(1).value.toUInt();
  const quint32 originalThird = values.at(2).value.toUInt();
  QCOMPARE(priorityValue(changes, QStringLiteral("COMPASS_PRIO1_ID")).toUInt(),
           originalSecond);
  QCOMPARE(priorityValue(changes, QStringLiteral("COMPASS_PRIO2_ID")).toUInt(),
           originalFirst);
  QCOMPARE(priorityValue(changes, QStringLiteral("COMPASS_PRIO3_ID")).toUInt(),
           originalThird);
  QCOMPARE(view.viewModel()->Rows().at(2).rawDevId, originalThird);
}

void ConfigCompassViewTest::rebootDefaultsToCancel() {
  ConfigCompassView view;
  view.setCatalog(catalogFixture(), true);
  view.setParameterSnapshot(snapshot(), 1, true);
  view.setConnected(true);
  view.show();
  QApplication::processEvents();

  QTableView *const table =
      view.findChild<QTableView *>(QStringLiteral("compassTable"));
  QPushButton *const moveUp =
      view.findChild<QPushButton *>(QStringLiteral("compassMoveUp"));
  table->selectRow(1);
  table->setCurrentIndex(table->model()->index(1, 0));
  QApplication::processEvents();
  QSignalSpy writes(&view, &ConfigCompassView::writeRequested);
  moveUp->click();
  QCOMPARE(writes.count(), 1);
  const quint64 requestId = writes.first().at(0).toULongLong();
  const int changeCount = writes.first().at(2).toList().size();
  view.parameterWriteSubmitted(requestId, 77);
  view.parameterBatchCompleted(77, changeCount, 0);

  QPushButton *const reboot =
      view.findChild<QPushButton *>(QStringLiteral("compassReboot"));
  QVERIFY(reboot && reboot->isEnabled());
  QSignalSpy rebootRequests(&view, &ConfigCompassView::rebootRequested);
  bool inspected = false;
  QTimer::singleShot(0, &view, [&inspected]() {
    auto *confirmation =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    QVERIFY(confirmation);
    QCOMPARE(confirmation->objectName(),
             QStringLiteral("compassRebootConfirmation"));
    QCOMPARE(confirmation->standardButton(confirmation->defaultButton()),
             QMessageBox::Cancel);
    inspected = true;
    confirmation->reject();
  });
  reboot->click();
  QVERIFY(inspected);
  QCOMPARE(rebootRequests.count(), 0);
}

void ConfigCompassViewTest::destructionDoesNotEmitWrite() {
  CalibrationHarness harness;
  auto *view = new ConfigCompassView;
  view->setCatalog(catalogFixture(), true);
  view->setParameterSnapshot(snapshot(), 1, true);
  view->setConnected(true);
  const VehicleTargetLease lease = harness.lease();
  view->setCalibrationContext(&harness.service, lease);
  QSignalSpy writes(view, &ConfigCompassView::writeRequested);
  QCOMPARE(harness.service.start(lease, false),
           CompassCalibrationService::RequestResult::Started);
  harness.commands.observeMessage(
      17, commandAck(MAV_CMD_DO_START_MAG_CAL, MAV_RESULT_ACCEPTED));
  QCOMPARE(harness.frames.size(), 1);
  delete view;
  QCOMPARE(writes.count(), 0);
  QCOMPARE(harness.frames.size(), 1);
  QCOMPARE(harness.service.state(), CompassCalibrationService::State::Running);
}

QTEST_MAIN(ConfigCompassViewTest)
#include "test_configcompassview.moc"
