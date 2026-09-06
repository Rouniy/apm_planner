#include "ui/OfflineMagFitWindow.h"

#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/ParameterService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleCommandService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterCodec.h"
#include "core/parameters/ParameterStore.h"

#include <QtTest>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>

namespace
{
QString writeSphereLog(QTemporaryDir *directory, int sampleCount = 180,
                       bool applyEligible = false)
{
    if (!directory || !directory->isValid())
        return {};
    const QString path = directory->filePath(QStringLiteral("magfit.log"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    if (applyEligible) {
        file.write("FMT,129,23,PARM,Nf,Name,Value\n"
                   "PARM,COMPASS_DIA_X,1\n"
                   "PARM,COMPASS_DIA_Y,1\n"
                   "PARM,COMPASS_DIA_Z,1\n"
                   "PARM,COMPASS_ODI_X,0\n"
                   "PARM,COMPASS_ODI_Y,0\n"
                   "PARM,COMPASS_ODI_Z,0\n"
                   "PARM,COMPASS_SCALE,1\n"
                   "PARM,COMPASS_DEV_ID,101\n"
                   "PARM,AHRS_ORIENTATION,0\n"
                   "PARM,COMPASS_ORIENT,0\n"
                   "PARM,COMPASS_EXTERNAL,1\n"
                   "FMT,130,41,MAG,BfffffffffB,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ,MOX,MOY,MOZ,Health\n");
    } else {
        file.write("FMT,130,28,MAG,Bffffff,I,MagX,MagY,MagZ,OfsX,OfsY,OfsZ\n");
    }
    constexpr double pi = 3.14159265358979323846;
    constexpr double golden = 3.0 - 2.23606797749978969641;
    for (int index = 0; index < sampleCount; ++index) {
        const double y = 1.0 - 2.0 * (index + 0.5) / sampleCount;
        const double radius = std::sqrt(1.0 - y * y);
        const double phi = index * pi * golden;
        const double x = 430.0 * std::cos(phi) * radius - 31.0;
        const double z = 430.0 * std::sin(phi) * radius - 7.0;
        const double adjustedY = 430.0 * y + 18.0;
        const QString rowPattern = applyEligible
            ? QStringLiteral("MAG,0,%1,%2,%3,4,5,6,0,0,0,1\n")
            : QStringLiteral("MAG,0,%1,%2,%3,4,5,6\n");
        const QByteArray row = rowPattern
            .arg(x + 4.0, 0, 'f', 8)
            .arg(adjustedY + 5.0, 0, 'f', 8)
            .arg(z + 6.0, 0, 'f', 8)
            .toLatin1();
        if (file.write(row) != row.size())
            return {};
    }
    file.close();
    return path;
}

QString parameterName(const char id[16])
{
    int length = 0;
    while (length < 16 && id[length] != '\0')
        ++length;
    return QString::fromLatin1(id, length);
}

mavlink_message_t decodeFrame(const QByteArray &frame)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    for (char byte : frame)
        parser.parseByte(static_cast<quint8>(byte), &message);
    return message;
}

class ApplyFixture
{
public:
    ApplyFixture()
        : transmitter([this](int linkId, const QByteArray &frame) {
              frames.append(frame);
              if (!autoEcho)
                  return true;
              const mavlink_message_t message = decodeFrame(frame);
              if (message.msgid != MAVLINK_MSG_ID_PARAM_SET)
                  return true;
              mavlink_param_set_t request{};
              mavlink_msg_param_set_decode(&message, &request);
              const QString name = parameterName(request.param_id);
              const ParameterType type =
                  static_cast<ParameterType>(request.param_type);
              bool decoded = false;
              const QVariant value = ParameterCodec::decodeClassic(
                  request.param_value, type, ParameterEncoding::Bytewise,
                  &decoded);
              if (!decoded)
                  return false;
              mavlink_param_value_t response{};
              response.param_value = request.param_value;
              response.param_type = request.param_type;
              response.param_count = static_cast<quint16>(indices.size());
              response.param_index = static_cast<quint16>(indices.value(name));
              std::memcpy(response.param_id, request.param_id,
                          sizeof response.param_id);
              mavlink_message_t echo{};
              mavlink_msg_param_value_encode(
                  static_cast<quint8>(endpoint.systemId),
                  static_cast<quint8>(endpoint.componentId), &echo, &response);
              parameters.observePhysicalMessage(linkId, session, echo);
              return true;
          })
        , parameters(&targets, &transmitter)
        , commands(&targets, &transmitter)
        , service(&targets, &registry, &parameters, &commands,
                  [this](const SwarmVehicleInstanceLease &lease,
                         QString *) {
                      return registry.validateLease(lease, 3000);
                  })
    {
        endpoint.linkId = 19;
        endpoint.systemId = 42;
        endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
        endpoint.linkName = QStringLiteral("MagFit bench vehicle");
        session = registry.beginLinkSession(endpoint.linkId,
                                            endpoint.linkName);
        targets.observeEndpoint(endpoint, true);
        heartbeat(false);
        auto leaseValidator = [this](const SwarmVehicleInstanceLease &lease) {
            return registry.validateLease(lease, 3000);
        };
        auto route = [](const SwarmVehicleInstanceLease &, QString *) {
            return true;
        };
        QVERIFY(parameters.configureExactTransactions(leaseValidator, route));
        QVERIFY(parameters.configureSingleVehicleExactRoute(route));
        QVERIFY(commands.configureExactTransactions(leaseValidator, route));
        QVERIFY(commands.configureSingleVehicleExactRoute(route));
        commands.setLocalIdentity(250, 190);

        const QList<QPair<QString, QPair<QVariant, ParameterType>>> values = {
            {QStringLiteral("COMPASS_DEV_ID"),
             {QVariant::fromValue<quint32>(101), ParameterType::UInt32}},
            {QStringLiteral("COMPASS_LEARN"), {1.0F, ParameterType::Real32}},
            {QStringLiteral("COMPASS_OFS_X"), {4.0F, ParameterType::Real32}},
            {QStringLiteral("COMPASS_OFS_Y"), {5.0F, ParameterType::Real32}},
            {QStringLiteral("COMPASS_OFS_Z"), {6.0F, ParameterType::Real32}},
            {QStringLiteral("AHRS_ORIENTATION"),
             {qint32(0), ParameterType::Int32}},
            {QStringLiteral("COMPASS_ORIENT"),
             {qint32(0), ParameterType::Int32}},
            {QStringLiteral("COMPASS_EXTERNAL"),
             {qint32(1), ParameterType::Int32}}};
        for (int index = 0; index < values.size(); ++index) {
            indices.insert(values.at(index).first, index);
            QVERIFY(parameters.store()->ingest(
                endpoint, values.size(), index, values.at(index).first,
                values.at(index).second.first, values.at(index).second.second));
        }
    }

    void heartbeat(bool armed)
    {
        mavlink_heartbeat_t payload{};
        payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
        payload.type = MAV_TYPE_QUADROTOR;
        payload.base_mode = armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0;
        mavlink_message_t message{};
        mavlink_msg_heartbeat_encode(
            static_cast<quint8>(endpoint.systemId),
            static_cast<quint8>(endpoint.componentId), &message, &payload);
        targets.observeHeartbeat(endpoint, armed, payload.autopilot,
                                 payload.type);
        QVERIFY(registry.observeMessage(endpoint.linkId, session, message));
    }

    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QList<QByteArray> frames;
    QHash<QString, int> indices;
    bool autoEcho = true;
    ExactLinkTransmitter transmitter;
    ParameterService parameters;
    VehicleCommandService commands;
    OfflineMagFitApplyService service;
    VehicleEndpoint endpoint;
    quint64 session = 0;
};
} // namespace

class OfflineMagFitWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerSurfaceAndWorksOffline();
    void analyzesSphereAndInvalidatesResultsWhenSettingsChange();
    void sourcePickerIsAsynchronousAndCancelIsHarmless();
    void cancellationDiscardsWorkerResult();
    void closeCancelsOwnedAnalysisWithoutLateWidgetAccess();
    void applyRequiresExplicitConsentAndRevalidatesArmedState();
    void exactApplyReportsConfirmedWritesAndSurvivesReopen();
    void closeCancelsOnlyOwnedApplyAndServiceKeepsPartialReport();
    void closeNeverCancelsForeignApply();
};

void OfflineMagFitWindowTest::matchesMissionPlannerSurfaceAndWorksOffline()
{
    auto *window = new OfflineMagFitWindow;
    QCOMPARE(window->objectName(), QStringLiteral("OfflineMagFitWindow"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(1180, 620));
    QCOMPARE(window->minimumSize(), QSize(900, 460));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));

    auto *path = window->findChild<QLineEdit *>(
        QStringLiteral("OfflineMagFitSourcePath"));
    auto *browse = window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitBrowseButton"));
    auto *analyze = window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitAnalyzeButton"));
    auto *cancel = window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitCancelButton"));
    auto *throttle = window->findChild<QDoubleSpinBox *>(
        QStringLiteral("OfflineMagFitThrottleThreshold"));
    auto *ellipsoid = window->findChild<QCheckBox *>(
        QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
    auto *progress = window->findChild<QProgressBar *>(
        QStringLiteral("OfflineMagFitProgressBar"));
    auto *table = window->findChild<QTableWidget *>(
        QStringLiteral("OfflineMagFitResultsTable"));
    auto *apply = window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitApplyButton"));
    QVERIFY(path && browse && analyze && cancel && throttle && ellipsoid
            && progress && table && apply);
    QCOMPARE(throttle->minimum(), 0.0);
    QCOMPARE(throttle->maximum(), 100.0);
    QCOMPARE(throttle->value(), 30.0);
    QVERIFY(ellipsoid->isChecked());
    QCOMPARE(table->columnCount(), 9);
    QVERIFY(browse->isEnabled());
    QVERIFY(!analyze->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(!apply->isEnabled());

    QPointer<OfflineMagFitWindow> guarded(window);
    window->show();
    QCoreApplication::processEvents();
    window->close();
    QTRY_VERIFY(guarded.isNull());
}

void OfflineMagFitWindowTest::
analyzesSphereAndInvalidatesResultsWhenSettingsChange()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory);
    QVERIFY(!source.isEmpty());

    OfflineMagFitWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *ellipsoid = window.findChild<QCheckBox *>(
        QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
    auto *throttle = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("OfflineMagFitThrottleThreshold"));
    auto *table = window.findChild<QTableWidget *>(
        QStringLiteral("OfflineMagFitResultsTable"));
    QVERIFY(ellipsoid && throttle && table);
    ellipsoid->setChecked(false);
    QVERIFY(window.setSource(source));
    window.analyze();
    QVERIFY(window.analysisBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.analysisBusy(), 10000);
    const OfflineMagFitReport report = window.report();
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.results.size(), 1);
    QVERIFY(!report.results.constFirst().hasEllipsoid);
    QVERIFY(std::abs(report.results.constFirst().offsets.x - 31.0) < 0.2);
    QVERIFY(std::abs(report.results.constFirst().offsets.y + 18.0) < 0.2);
    QVERIFY(std::abs(report.results.constFirst().offsets.z - 7.0) < 0.2);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Compass 1"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Sphere"));
    QCOMPARE(window.findChild<QProgressBar *>(
                 QStringLiteral("OfflineMagFitProgressBar"))->value(), 100);
    QVERIFY(!report.applyEligible);
    QVERIFY(window.statusText().contains(QStringLiteral("unavailable"),
                                         Qt::CaseInsensitive));
    QVERIFY(!window.findChild<QPushButton *>(
                 QStringLiteral("OfflineMagFitApplyButton"))->isEnabled());

    throttle->setValue(31.0);
    QVERIFY(!window.report().success);
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(window.statusText().contains(QStringLiteral("threshold"),
                                         Qt::CaseInsensitive));
}

void OfflineMagFitWindowTest::sourcePickerIsAsynchronousAndCancelIsHarmless()
{
    OfflineMagFitWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto *browse = window.findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitBrowseButton"));
    QVERIFY(browse);
    browse->click();
    auto *dialog = window.findChild<QFileDialog *>(
        QStringLiteral("OfflineMagFitSourceDialog"));
    QVERIFY(dialog);
    QVERIFY(dialog->testOption(QFileDialog::DontUseNativeDialog));
    QVERIFY(dialog->isVisible());
    QVERIFY(!browse->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
    QTRY_VERIFY(browse->isEnabled());
    QVERIFY(!window.analysisBusy());
    QVERIFY(!window.report().success);
}

void OfflineMagFitWindowTest::cancellationDiscardsWorkerResult()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 3000);
    QVERIFY(!source.isEmpty());
    OfflineMagFitWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    QVERIFY(window.setSource(source));
    window.analyze();
    QVERIFY(window.analysisBusy());
    window.cancelAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!window.analysisBusy(), 5000);
    QVERIFY(!window.report().success);
    QCOMPARE(window.findChild<QTableWidget *>(
                 QStringLiteral("OfflineMagFitResultsTable"))->rowCount(), 0);
    QVERIFY(window.statusText().contains(QStringLiteral("cancel"),
                                         Qt::CaseInsensitive));
}

void OfflineMagFitWindowTest::closeCancelsOwnedAnalysisWithoutLateWidgetAccess()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 3000);
    QVERIFY(!source.isEmpty());
    auto *window = new OfflineMagFitWindow;
    QPointer<OfflineMagFitWindow> guarded(window);
    QVERIFY(window->setSource(source));
    window->analyze();
    QVERIFY(window->analysisBusy());
    window->show();
    window->close();
    QTRY_VERIFY(guarded.isNull());
    // Give a stale worker completion a chance to post. It owns only immutable
    // inputs and the shared cancellation flag, never the deleted widget.
    QTest::qWait(25);
    QCoreApplication::processEvents();
}

void OfflineMagFitWindowTest::
applyRequiresExplicitConsentAndRevalidatesArmedState()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 180, true);
    QVERIFY(!source.isEmpty());
    ApplyFixture fixture;
    OfflineMagFitWindow window;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.setApplyService(&fixture.service);
    auto *ellipsoid = window.findChild<QCheckBox *>(
        QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
    auto *apply = window.findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitApplyButton"));
    QVERIFY(ellipsoid && apply);
    ellipsoid->setChecked(false);
    QVERIFY(window.setSource(source));
    window.analyze();
    QTRY_VERIFY_WITH_TIMEOUT(!window.analysisBusy(), 10000);
    QVERIFY2(window.report().success, qPrintable(window.report().error));
    QVERIFY2(window.report().applyEligible,
             qPrintable(window.report().applyUnavailableReason));
    QVERIFY(apply->isEnabled());

    window.show();
    apply->click();
    auto *confirmation = window.findChild<QMessageBox *>(
        QStringLiteral("OfflineMagFitApplyConfirmation"));
    QVERIFY(confirmation);
    QVERIFY(confirmation->isVisible());
    QVERIFY(confirmation->defaultButton()
            == confirmation->button(QMessageBox::Cancel));
    QVERIFY(confirmation->text().contains(source));
    QVERIFY(confirmation->text().contains(QStringLiteral("system 42")));
    QVERIFY(confirmation->text().contains(QStringLiteral("partial write"),
                                           Qt::CaseInsensitive));
    QVERIFY(fixture.frames.isEmpty());

    fixture.heartbeat(true);
    QVERIFY(QMetaObject::invokeMethod(
        confirmation, "done", Qt::DirectConnection,
        Q_ARG(int, int(QMessageBox::Yes))));
    QTRY_VERIFY(!fixture.service.busy());
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(window.statusText().contains(QStringLiteral("cancel"),
                                         Qt::CaseInsensitive));
}

void OfflineMagFitWindowTest::
exactApplyReportsConfirmedWritesAndSurvivesReopen()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 180, true);
    QVERIFY(!source.isEmpty());
    ApplyFixture fixture;
    auto *window = new OfflineMagFitWindow;
    window->setAttribute(Qt::WA_DeleteOnClose, false);
    window->setApplyService(&fixture.service);
    auto *ellipsoid = window->findChild<QCheckBox *>(
        QStringLiteral("OfflineMagFitEllipsoidCheckBox"));
    auto *apply = window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitApplyButton"));
    QVERIFY(ellipsoid && apply);
    ellipsoid->setChecked(false);
    QVERIFY(window->setSource(source));
    window->analyze();
    QTRY_VERIFY_WITH_TIMEOUT(!window->analysisBusy(), 10000);
    QVERIFY(window->report().applyEligible);

    window->show();
    apply->click();
    auto *confirmation = window->findChild<QMessageBox *>(
        QStringLiteral("OfflineMagFitApplyConfirmation"));
    QVERIFY(confirmation);
    QVERIFY(QMetaObject::invokeMethod(
        confirmation, "done", Qt::DirectConnection,
        Q_ARG(int, int(QMessageBox::Yes))));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 2000);
    QCOMPARE(fixture.frames.size(), 4);
    const auto report = fixture.service.lastReport();
    QCOMPARE(report.outcome, OfflineMagFitApplyService::Outcome::Completed);
    QCOMPARE(report.totalWrites, 4);
    QCOMPARE(report.confirmedWrites, 4);
    QCOMPARE(report.remainingWrites, 0);
    QCOMPARE(report.receipts.size(), 4);
    QVERIFY(window->statusText().contains(QStringLiteral("4/4")));

    delete window;
    OfflineMagFitWindow reopened;
    reopened.setAttribute(Qt::WA_DeleteOnClose, false);
    reopened.setApplyService(&fixture.service);
    auto *history = reopened.findChild<QPlainTextEdit *>(
        QStringLiteral("OfflineMagFitHistory"));
    QVERIFY(history);
    QVERIFY(history->toPlainText().contains(QStringLiteral("Applied 4"),
                                            Qt::CaseInsensitive));
}

void OfflineMagFitWindowTest::
closeCancelsOnlyOwnedApplyAndServiceKeepsPartialReport()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 180, true);
    QVERIFY(!source.isEmpty());
    ApplyFixture fixture;
    fixture.autoEcho = false;
    auto *window = new OfflineMagFitWindow;
    QPointer<OfflineMagFitWindow> guarded(window);
    window->setApplyService(&fixture.service);
    window->findChild<QCheckBox *>(
        QStringLiteral("OfflineMagFitEllipsoidCheckBox"))->setChecked(false);
    QVERIFY(window->setSource(source));
    window->analyze();
    QTRY_VERIFY_WITH_TIMEOUT(!window->analysisBusy(), 10000);
    window->show();
    window->findChild<QPushButton *>(
        QStringLiteral("OfflineMagFitApplyButton"))->click();
    auto *confirmation = window->findChild<QMessageBox *>(
        QStringLiteral("OfflineMagFitApplyConfirmation"));
    QVERIFY(confirmation);
    QVERIFY(QMetaObject::invokeMethod(
        confirmation, "done", Qt::DirectConnection,
        Q_ARG(int, int(QMessageBox::Yes))));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service.busy()
                             && !fixture.frames.isEmpty(), 1000);
    const quint64 ownedId = fixture.service.currentOperationId();
    QVERIFY(ownedId != 0);
    window->close();
    QTRY_VERIFY(guarded.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 2000);
    QCOMPARE(fixture.service.lastReport().operationId, ownedId);
    QCOMPARE(fixture.service.lastReport().outcome,
             OfflineMagFitApplyService::Outcome::OutcomeUncertain);
    QVERIFY(fixture.service.lastReport().remainingWrites > 0);

    OfflineMagFitWindow reopened;
    reopened.setAttribute(Qt::WA_DeleteOnClose, false);
    reopened.setApplyService(&fixture.service);
    auto *history = reopened.findChild<QPlainTextEdit *>(
        QStringLiteral("OfflineMagFitHistory"));
    QVERIFY(history);
    QVERIFY(history->toPlainText().contains(QStringLiteral("uncertain"),
                                            Qt::CaseInsensitive));
}

void OfflineMagFitWindowTest::closeNeverCancelsForeignApply()
{
    QTemporaryDir directory;
    const QString source = writeSphereLog(&directory, 180, true);
    QVERIFY(!source.isEmpty());
    OfflineMagFitService::Options options;
    options.useEllipsoid = false;
    const OfflineMagFitReport analysis =
        OfflineMagFitService::analyze(source, options);
    QVERIFY2(analysis.success && analysis.applyEligible,
             qPrintable(analysis.error + analysis.applyUnavailableReason));

    ApplyFixture fixture;
    fixture.autoEcho = false;
    OfflineMagFitApplyService::Plan plan;
    QString error;
    QVERIFY2(fixture.service.prepare(analysis, &plan, &error),
             qPrintable(error));
    quint64 foreignId = 0;
    QCOMPARE(fixture.service.execute(plan, &foreignId, &error),
             OfflineMagFitApplyService::SubmitResult::Started);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service.busy()
                             && !fixture.frames.isEmpty(), 1000);

    auto *window = new OfflineMagFitWindow;
    QPointer<OfflineMagFitWindow> guarded(window);
    window->setApplyService(&fixture.service);
    window->show();
    window->close();
    QTRY_VERIFY(guarded.isNull());
    QVERIFY(fixture.service.busy());
    QCOMPARE(fixture.service.currentOperationId(), foreignId);

    QVERIFY(fixture.service.cancel(foreignId));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 2000);
}

QTEST_MAIN(OfflineMagFitWindowTest)
#include "test_offlinemagfitwindow.moc"
