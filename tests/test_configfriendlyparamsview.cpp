#include <QtTest>

#include "ui/configuration/ConfigFriendlyParamsView.h"

#include <QBuffer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaType>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QToolButton>
#include <QWheelEvent>

namespace {
ParameterMetaDataCatalog catalogFixture()
{
    QByteArray xml(R"xml(
      <paramfile><vehicles><parameters name="ArduCopter">
        <param name="ArduCopter:GAIN" humanName="Gain" user="Standard">
          <field name="Range">-1 1</field><field name="Increment">0.1</field>
        </param>
        <param name="ArduCopter:MODE" humanName="Mode" user="Advanced">
          <values><value code="0.6">Strict</value><value code="1">All</value></values>
        </param>
        <param name="ArduCopter:MASK" humanName="Mask">
          <values><value code="0">None</value><value code="1">First</value></values>
          <field name="Bitmask">0:First,2:Third</field>
        </param>
        <param name="ArduCopter:LOCKED" humanName="Locked">
          <field name="ReadOnly">true</field>
        </param>
        <param name="ArduCopter:COUNT" humanName="Count" user="Standard">
          <field name="Range">0 1000</field><field name="Increment">1</field>
        </param>
        <param name="ArduCopter:HIDDEN" user="Standard" />
      </parameters></vehicles></paramfile>)xml");
    QBuffer buffer(&xml);
    buffer.open(QIODevice::ReadOnly);
    return ParameterMetaDataCatalog::fromPdef(
        &buffer, QStringLiteral("ArduCopter"));
}

QWidget *parameterRow(ConfigFriendlyParamsView &view, const QString &name)
{
    const QList<QWidget *> widgets = view.findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (widget->property("parameterName").toString() == name) {
            return widget;
        }
    }
    return nullptr;
}
}

class ConfigFriendlyParamsViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void modelUsesMissionPlannerFiltersAndComponents();
    void viewClassifiesEditorsAndPreservesOutOfRangeValue();
    void writesUseSelectedComponentAndTypedValues();
    void searchDebouncesAndRefreshIsForwarded();
};

void ConfigFriendlyParamsViewTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FriendlyParams"));
}

void ConfigFriendlyParamsViewTest::init()
{
    QSettings settings;
    settings.clear();
}

void ConfigFriendlyParamsViewTest::modelUsesMissionPlannerFiltersAndComponents()
{
    ConfigFriendlyParamsViewModel standard(false);
    standard.setCatalog(catalogFixture());
    standard.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.5},
        {1, QStringLiteral("MODE"), 1.0},
        {1, QStringLiteral("MASK"), QVariant::fromValue(qulonglong(1) << 27)},
        {1, QStringLiteral("HIDDEN"), 3},
        {154, QStringLiteral("GAIN"), 0.8}
    }, 1);

    QCOMPARE(standard.availableComponents(), QList<int>({1, 154}));
    QCOMPARE(standard.selectedComponent(), 1);
    QStringList standardNames;
    for (const ParamField &field : standard.fields()) {
        standardNames.append(field.name);
    }
    QCOMPARE(standardNames, QStringList({QStringLiteral("GAIN"),
                                         QStringLiteral("MASK")}));
    standard.setFavorite(1, QStringLiteral("MASK"), true);
    QCOMPARE(standard.visibleFields().first().name, QStringLiteral("MASK"));
    QVERIFY(QSettings().value(QStringLiteral("fav_params_std"))
                .toStringList().contains(QStringLiteral("MASK")));

    ConfigFriendlyParamsViewModel restoredStandard(false);
    restoredStandard.setCatalog(catalogFixture());
    restoredStandard.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.5},
        {1, QStringLiteral("MASK"), 0}
    });
    QCOMPARE(restoredStandard.visibleFields().first().name,
             QStringLiteral("MASK"));

    standard.setSelectedComponent(154);
    QCOMPARE(standard.fields().size(), 1);
    QCOMPARE(standard.fields().first().componentId, 154);
    QCOMPARE(standard.fields().first().value.toDouble(), 0.8);

    ConfigFriendlyParamsViewModel advanced(true);
    advanced.setCatalog(catalogFixture());
    advanced.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.5},
        {1, QStringLiteral("MODE"), 1.0}
    });
    QCOMPARE(advanced.fields().size(), 1);
    QCOMPARE(advanced.fields().first().name, QStringLiteral("MODE"));
}

void ConfigFriendlyParamsViewTest::viewClassifiesEditorsAndPreservesOutOfRangeValue()
{
    ConfigFriendlyParamsView view(false, catalogFixture());
    QSignalSpy writes(&view, &ConfigFriendlyParamsView::writeRequested);
    view.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 5.0},
        {1, QStringLiteral("MASK"),
            QVariant::fromValue(qulonglong(1) << 27)},
        {1, QStringLiteral("LOCKED"), 2.0},
        {1, QStringLiteral("COUNT"), 100}
    });
    QCOMPARE(writes.count(), 0);

    QWidget *gain = parameterRow(view, QStringLiteral("GAIN"));
    QVERIFY(gain);
    QCOMPARE(gain->property("editorKind").toString(), QStringLiteral("numeric"));
    auto *numeric = gain->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    QVERIFY(numeric);
    QCOMPARE(numeric->value(), 5.0);
    QVERIFY(numeric->parentWidget()->property("outOfRange").toBool());
    QWheelEvent wheelEvent(QPointF(5, 5), QPointF(5, 5), QPoint(),
                           QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(numeric, &wheelEvent);
    QCOMPARE(numeric->value(), 5.0);

    QWidget *count = parameterRow(view, QStringLiteral("COUNT"));
    QVERIFY(count);
    auto *integerEditor = count->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    QCOMPARE(integerEditor->text(), QStringLiteral("100"));
    QVERIFY(QMetaObject::invokeMethod(integerEditor, "editingFinished"));
    QCOMPARE(writes.count(), 0);

    QWidget *mask = parameterRow(view, QStringLiteral("MASK"));
    QVERIFY(mask);
    QCOMPARE(mask->property("editorKind").toString(), QStringLiteral("bitmask"));
    auto *bitmaskButton = mask->findChild<QToolButton *>(
        QStringLiteral("bitmaskButton"));
    QVERIFY(bitmaskButton);
    QVERIFY(bitmaskButton->menu());
    bitmaskButton->menu()->actions().first()->setChecked(true);
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 1);
    QCOMPARE(writes.first().at(1).toString(), QStringLiteral("MASK"));
    QCOMPARE(writes.first().at(2).toULongLong(),
             (qulonglong(1) << 27) | qulonglong(1));

    numeric->setValue(2.0);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), 1);
    QVERIFY(numeric->parentWidget()->property("outOfRange").toBool());

    QWidget *locked = parameterRow(view, QStringLiteral("LOCKED"));
    QVERIFY(locked);
    QVERIFY(!locked->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"))->isEnabled());
}

void ConfigFriendlyParamsViewTest::writesUseSelectedComponentAndTypedValues()
{
    ConfigFriendlyParamsView advanced(true, catalogFixture());
    QSignalSpy advancedWrites(&advanced,
                              &ConfigFriendlyParamsView::writeRequested);
    advanced.setParameterSnapshot({
        {1, QStringLiteral("MODE"), 1.0}
    });
    QWidget *mode = parameterRow(advanced, QStringLiteral("MODE"));
    QVERIFY(mode);
    auto *combo = mode->findChild<QComboBox *>(QStringLiteral("valueComboBox"));
    QVERIFY(combo);
    combo->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 0)));
    QCOMPARE(advancedWrites.count(), 1);
    QCOMPARE(advancedWrites.first().at(0).toInt(), 1);
    QCOMPARE(advancedWrites.first().at(1).toString(), QStringLiteral("MODE"));
    QCOMPARE(advancedWrites.first().at(2).toDouble(), 0.6);

    ConfigFriendlyParamsView standard(false, catalogFixture());
    QSignalSpy writes(&standard, &ConfigFriendlyParamsView::writeRequested);
    standard.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.2},
        {154, QStringLiteral("COUNT"), 4}
    }, 1);
    auto *selector = standard.findChild<QComboBox *>(
        QStringLiteral("componentSelector"));
    QVERIFY(selector);
    selector->setCurrentIndex(selector->findData(154));
    QWidget *count = parameterRow(standard, QStringLiteral("COUNT"));
    QVERIFY(count);
    auto *numeric = count->findChild<QDoubleSpinBox *>(
        QStringLiteral("numericEditor"));
    numeric->setValue(7.0);
    QVERIFY(QMetaObject::invokeMethod(numeric, "editingFinished"));
    QCOMPARE(writes.count(), 1);
    QCOMPARE(writes.first().at(0).toInt(), 154);
    QCOMPARE(writes.first().at(1).toString(), QStringLiteral("COUNT"));
    QCOMPARE(writes.first().at(2).userType(), int(QMetaType::Int));
    QCOMPARE(writes.first().at(2).toInt(), 7);
    standard.parameterWriteFailed(154, QStringLiteral("COUNT"),
                                  QStringLiteral("rejected"));
    QCOMPARE(count->findChild<QLabel *>(QStringLiteral("statusLabel"))->text(),
             QStringLiteral("rejected"));

    QSignalSpy refresh(&standard,
                       &ConfigFriendlyParamsView::refreshRequested);
    standard.findChild<QPushButton *>(QStringLiteral("refreshButton"))->click();
    QCOMPARE(refresh.count(), 1);
    QCOMPARE(refresh.first().at(0).toInt(), 154);
}

void ConfigFriendlyParamsViewTest::searchDebouncesAndRefreshIsForwarded()
{
    ConfigFriendlyParamsView view(false, catalogFixture());
    view.setParameterSnapshot({
        {1, QStringLiteral("GAIN"), 0.2},
        {1, QStringLiteral("MASK"), 0}
    });
    QCOMPARE(view.visibleParameterCount(), 2);
    auto *search = view.findChild<QLineEdit *>(QStringLiteral("searchBox"));
    QVERIFY(search);
    search->setText(QStringLiteral("gain"));
    QCOMPARE(view.visibleParameterCount(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(view.visibleParameterCount(), 1, 500);

    QSignalSpy refresh(&view, &ConfigFriendlyParamsView::refreshRequested);
    view.findChild<QPushButton *>(QStringLiteral("refreshButton"))->click();
    QCOMPARE(refresh.count(), 1);
    QCOMPARE(refresh.first().at(0).toInt(), 1);
}

QTEST_MAIN(ConfigFriendlyParamsViewTest)

#include "test_configfriendlyparamsview.moc"
