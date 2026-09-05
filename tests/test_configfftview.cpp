#include <QtTest>

#include "ui/ConfigFFTWindow.h"
#include "ui/configuration/ConfigFFTView.h"
#include "ui/qcustomplot.h"

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QToolButton>
#include <QTemporaryFile>

namespace {
ParameterMetaDataCatalog fftCatalog()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:INS_LOG_BAT_CNT" humanName="Sample count">
          <field name="Increment">32</field>
        </param>
        <param name="ArduCopter:INS_LOG_BAT_MASK" humanName="Sensor mask">
          <field name="Bitmask">0:IMU1,1:IMU2,2:IMU3</field>
        </param>
        <param name="ArduCopter:LOG_BITMASK" humanName="Log mask">
          <field name="Bitmask">7:IMU,18:Fast IMU,19:Raw IMU</field>
        </param>
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QList<ConfigFriendlyParameterValue> snapshot()
{
    return {{1, QStringLiteral("INS_LOG_BAT_CNT"), 1024},
            {1, QStringLiteral("INS_LOG_BAT_MASK"), 1},
            {1, QStringLiteral("LOG_BITMASK"), 1 << 19}};
}
} // namespace

class ConfigFFTViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMp10SurfaceAndKeepsOfflineAnalysisEnabled();
    void metadataBitmaskIsEditableWithoutLossySlider();
    void windowIsIndependentModelessOwner();
    void zeroBinsDoNotFlattenTheVisibleSpectrum();
};

void ConfigFFTViewTest::matchesMp10SurfaceAndKeepsOfflineAnalysisEnabled()
{
    ConfigFFTView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigFFTView"));
    QLabel *const title = view.findChild<QLabel *>(
        QStringLiteral("fftTitle"));
    QLabel *const info = view.findChild<QLabel *>(
        QStringLiteral("fftInfo"));
    QVERIFY(title && info);
    QCOMPARE(title->text(), QStringLiteral("FFT Setup"));
    QVERIFY(info->text().contains(QStringLiteral("INS_LOG_BAT_MASK/CNT")));
    QSpinBox *const bins = view.findChild<QSpinBox *>(
        QStringLiteral("fftBins"));
    QDoubleSpinBox *const start = view.findChild<QDoubleSpinBox *>(
        QStringLiteral("fftStartFrequency"));
    QCheckBox *const magnitude = view.findChild<QCheckBox *>(
        QStringLiteral("fftMagnitude"));
    QPushButton *const analyze = view.findChild<QPushButton *>(
        QStringLiteral("fftAnalyze"));
    QPushButton *const refresh = view.findChild<QPushButton *>(
        QStringLiteral("fftRefreshParams"));
    QVERIFY(bins && start && magnitude && analyze && refresh && view.plot());
    QCOMPARE(bins->minimum(), 4);
    QCOMPARE(bins->maximum(), 14);
    QCOMPARE(bins->value(), 10);
    QCOMPARE(start->minimum(), 0.0);
    QCOMPARE(start->maximum(), 1000.0);
    QCOMPARE(start->value(), 5.0);
    QVERIFY(!magnitude->isChecked());
    QVERIFY(analyze->isEnabled());
    QVERIFY(!refresh->isEnabled());
}

void ConfigFFTViewTest::metadataBitmaskIsEditableWithoutLossySlider()
{
    ConfigFFTView view;
    view.setCatalog(fftCatalog());
    view.setParameterSnapshot(snapshot(), 1, true);
    view.setParameterContext(true, true, false);
    QSignalSpy writes(&view, &ConfigFFTView::writeRequested);

    QToolButton *const sensorMask = view.findChild<QToolButton *>(
        QStringLiteral("fftParameterBitmask_INS_LOG_BAT_MASK"));
    QToolButton *const logMask = view.findChild<QToolButton *>(
        QStringLiteral("fftParameterBitmask_LOG_BITMASK"));
    QVERIFY(sensorMask && logMask);
    QCOMPARE(sensorMask->menu()->actions().size(), 3);
    QCOMPARE(logMask->menu()->actions().size(), 3);
    QVERIFY(!view.findChild<QDoubleSpinBox *>(
        QStringLiteral("fftParameterNumeric_LOG_BITMASK")));

    QAction *const imu2 = sensorMask->menu()->actions().at(1);
    QVERIFY(!imu2->isChecked());
    imu2->setChecked(true);
    QCOMPARE(writes.size(), 1);
    const QList<QVariant> arguments = writes.takeFirst();
    QCOMPARE(arguments.at(2).toString(),
             QStringLiteral("INS_LOG_BAT_MASK"));
    QCOMPARE(arguments.at(3).toULongLong(), quint64(3));
}

void ConfigFFTViewTest::zeroBinsDoNotFlattenTheVisibleSpectrum()
{
    QTemporaryFile log;
    QVERIFY(log.open());
    auto *model = new ConfigFFTViewModel(
        [](const QString &, const DataFlashFftAnalyzer::Options &) {
            DataFlashFftAnalyzer::Result result;
            result.succeeded = true;
            result.source = QStringLiteral("IMU");
            result.sampleRateHz = 1000.0;
            DataFlashFftAnalyzer::Series series;
            series.label = QStringLiteral("GYR0 x");
            series.frequenciesHz = {0.0, 100.0, 200.0};
            series.values = {-6466.0, 0.0, -6466.0};
            result.series.append(series);
            return result;
        });
    ConfigFFTView view(model);
    QVERIFY(model->analyzeFile(log.fileName()));
    QTRY_VERIFY(!model->analysisBusy());
    QCOMPARE(view.plot()->graphCount(), 1);
    QCOMPARE(view.plot()->yAxis->range().lower, -160.0);
    QCOMPARE(view.plot()->yAxis->range().upper, 10.0);
    QCOMPARE(view.plot()->graph(0)->data()->at(0)->value, -6466.0);
}

void ConfigFFTViewTest::windowIsIndependentModelessOwner()
{
    QWidget owner;
    owner.setGeometry(100, 100, 1200, 800);
    auto *window = new ConfigFFTWindow(nullptr, &owner);
    QPointer<ConfigFFTWindow> guarded(window);
    QCOMPARE(window->objectName(), QStringLiteral("ConfigFFTWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("FFT Log Analysis"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(1000, 700));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(window->view());
    QVERIFY(window->viewModel());
    QCOMPARE(window->viewModel()->parent(), window->view());
    window->show();
    QCoreApplication::processEvents();
    window->close();
    QTRY_VERIFY(guarded.isNull());
}

QTEST_MAIN(ConfigFFTViewTest)
#include "test_configfftview.moc"
