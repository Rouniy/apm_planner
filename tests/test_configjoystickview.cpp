#include <QtTest>

#include "ui/configuration/ConfigJoystickView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>

namespace {

template<typename T>
T *required(QObject *owner, const QString &name)
{
    T *result = owner->findChild<T *>(name);
    Q_ASSERT(result);
    return result;
}

template<typename T>
T *visible(QObject *owner, const QString &name)
{
    const QList<T *> candidates = owner->findChildren<T *>(name);
    for (T *candidate : candidates) {
        if (candidate->isVisible()) return candidate;
    }
    return nullptr;
}

} // namespace

class ConfigJoystickViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void offlinePageExposesTheFullCommonWorkflow();
    void editsAllSixteenChannelRowsWithoutADevice();
    void buttonSettingsPreserveModeAndAllParameters();
    void importConfirmationIsDefaultCancelAndDoesNotOpenPicker();
    void joycfgExportAndImportRoundTripTheEditedProfile();
    void closingPageDismissesPendingDialogs();
};

void ConfigJoystickViewTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ConfigJoystickViewTest"));
    QSettings settings;
    settings.clear();
}

void ConfigJoystickViewTest::offlinePageExposesTheFullCommonWorkflow()
{
    ConfigJoystickView view;
    view.resize(980, 720);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QCOMPARE(view.objectName(), QStringLiteral("ConfigJoystickView"));
    QCOMPARE(required<QTableWidget>(&view, QStringLiteral("JoystickAxisTable"))
                 ->rowCount(), 16);
    QCOMPARE(required<QTableWidget>(&view, QStringLiteral("JoystickButtonTable"))
                 ->rowCount(), 16);
    QVERIFY(required<QComboBox>(&view, QStringLiteral("JoystickDevice")));
    QVERIFY(required<QLabel>(&view, QStringLiteral("JoystickRawInput"))
                ->text().contains(QStringLiteral("Raw input")));
    QVERIFY(!required<QPushButton>(&view, QStringLiteral("JoystickEnableButton"))
                 ->isEnabled());
    QVERIFY(!required<QPushButton>(
        &view, QStringLiteral("JoystickEnableButton"))->toolTip().isEmpty());
    QVERIFY(required<QPushButton>(&view, QStringLiteral("JoystickSaveButton"))
                ->isEnabled());
    QVERIFY(required<QPushButton>(&view, QStringLiteral("JoystickExportButton"))
                ->isEnabled());
    QVERIFY(required<QPushButton>(&view, QStringLiteral("JoystickImportButton"))
                ->isEnabled());

    for (int channel = 1; channel <= 16; ++channel) {
        QVERIFY(required<QComboBox>(
            &view, QStringLiteral("JoystickAxis_%1").arg(channel)));
        QVERIFY(required<QProgressBar>(
            &view, QStringLiteral("JoystickValue_%1").arg(channel)));
        QVERIFY(required<QSpinBox>(
            &view, QStringLiteral("JoystickExpo_%1").arg(channel)));
        QVERIFY(required<QCheckBox>(
            &view, QStringLiteral("JoystickReverse_%1").arg(channel)));
        QVERIFY(required<QPushButton>(
            &view, QStringLiteral("JoystickDetectAxis_%1").arg(channel)));
    }
}

void ConfigJoystickViewTest::editsAllSixteenChannelRowsWithoutADevice()
{
    ConfigJoystickView view;
    QSpinBox *expo = required<QSpinBox>(&view, QStringLiteral("JoystickExpo_16"));
    QCheckBox *reverse = required<QCheckBox>(
        &view, QStringLiteral("JoystickReverse_16"));
    expo->setValue(37);
    reverse->setChecked(true);

    JoystickConfiguration::Profile profile = view.profile();
    QCOMPARE(profile.channels.size(), 16);
    QCOMPARE(profile.channels.at(15).channel, 16);
    QCOMPARE(profile.channels.at(15).expo, 37);
    QVERIFY(profile.channels.at(15).reverse);

    QCheckBox *manual = required<QCheckBox>(
        &view, QStringLiteral("JoystickManualControl"));
    manual->setChecked(true);
    profile = view.profile();
    QVERIFY(profile.manualControl);
    QCOMPARE(required<QProgressBar>(
                 &view, QStringLiteral("JoystickValue_1"))->minimum(), -1000);
    QCOMPARE(required<QProgressBar>(
                 &view, QStringLiteral("JoystickValue_1"))->maximum(), 1000);
}

void ConfigJoystickViewTest::buttonSettingsPreserveModeAndAllParameters()
{
    ConfigJoystickView view;
    view.resize(980, 720);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QComboBox *function = required<QComboBox>(
        &view, QStringLiteral("JoystickButtonFunction_1"));
    QVERIFY(function->count() > 0);
    const int servo = function->findText(QStringLiteral("Do_Set_Servo"));
    if (servo >= 0) function->setCurrentIndex(servo);
    required<QSpinBox>(&view, QStringLiteral("JoystickButtonNumber_1"))
        ->setValue(7);
    required<QPushButton>(&view, QStringLiteral("JoystickButtonSettings_1"))
        ->click();

    QDialog *dialog = nullptr;
    QTRY_VERIFY((dialog = visible<QDialog>(
                     &view, QStringLiteral("JoystickButtonSettingsDialog"))));
    required<QLineEdit>(dialog, QStringLiteral("JoystickButtonMode"))
        ->setText(QStringLiteral("Loiter"));
    for (int i = 1; i <= 4; ++i) {
        required<QDoubleSpinBox>(
            dialog, QStringLiteral("JoystickButtonP%1").arg(i))->setValue(i * 10.25);
    }
    QDialogButtonBox *buttons = required<QDialogButtonBox>(
        dialog, QStringLiteral("JoystickButtonSettingsButtons"));
    QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isDefault());
    QPointer<QDialog> dialogGuard(dialog);
    buttons->button(QDialogButtonBox::Save)->click();

    QTRY_VERIFY(dialogGuard.isNull() || !dialogGuard->isVisible());
    const JoystickConfiguration::Button button = view.profile().buttons.at(0);
    QCOMPARE(button.buttonno, 7);
    QCOMPARE(button.function, function->currentText());
    QCOMPARE(button.mode, QStringLiteral("Loiter"));
    QCOMPARE(button.p1, 10.25);
    QCOMPARE(button.p2, 20.5);
    QCOMPARE(button.p3, 30.75);
    QCOMPARE(button.p4, 41.0);
}

void ConfigJoystickViewTest::importConfirmationIsDefaultCancelAndDoesNotOpenPicker()
{
    ConfigJoystickView view;
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    required<QPushButton>(&view, QStringLiteral("JoystickImportButton"))->click();

    QMessageBox *box = nullptr;
    QTRY_VERIFY((box = visible<QMessageBox>(
                     &view, QStringLiteral("JoystickImportConfirmation"))));
    QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
    QCOMPARE(box->escapeButton(), box->button(QMessageBox::Cancel));
    QPointer<QMessageBox> boxGuard(box);
    box->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(boxGuard.isNull() || !boxGuard->isVisible());
    QVERIFY(!visible<QFileDialog>(&view,
                                 QStringLiteral("JoystickImportFileDialog")));
}

void ConfigJoystickViewTest::joycfgExportAndImportRoundTripTheEditedProfile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString output = temporary.filePath(QStringLiteral("pilot.joycfg"));
    ConfigJoystickView view;
    view.resize(980, 720);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    required<QSpinBox>(&view, QStringLiteral("JoystickExpo_3"))->setValue(44);
    required<QCheckBox>(&view, QStringLiteral("JoystickReverse_3"))
        ->setChecked(true);
    required<QPushButton>(&view, QStringLiteral("JoystickExportButton"))->click();
    QFileDialog *exportPicker = nullptr;
    QTRY_VERIFY((exportPicker = visible<QFileDialog>(
                     &view, QStringLiteral("JoystickExportFileDialog"))));
    exportPicker->selectFile(output);
    QVERIFY(QMetaObject::invokeMethod(exportPicker, "accept", Qt::DirectConnection));
    QTRY_VERIFY(QFileInfo::exists(output));

    required<QSpinBox>(&view, QStringLiteral("JoystickExpo_3"))->setValue(-12);
    required<QCheckBox>(&view, QStringLiteral("JoystickReverse_3"))
        ->setChecked(false);
    required<QPushButton>(&view, QStringLiteral("JoystickImportButton"))->click();
    QMessageBox *confirmation = nullptr;
    QTRY_VERIFY((confirmation = visible<QMessageBox>(
                     &view, QStringLiteral("JoystickImportConfirmation"))));
    confirmation->button(QMessageBox::Yes)->click();
    QFileDialog *importPicker = nullptr;
    QTRY_VERIFY((importPicker = visible<QFileDialog>(
                     &view, QStringLiteral("JoystickImportFileDialog"))));
    importPicker->selectFile(output);
    QVERIFY(QMetaObject::invokeMethod(importPicker, "accept", Qt::DirectConnection));

    QTRY_COMPARE(view.profile().channels.at(2).expo, 44);
    QVERIFY(view.profile().channels.at(2).reverse);
    QVERIFY(required<QLabel>(&view, QStringLiteral("JoystickLoadedConfig"))
                ->text().contains(QStringLiteral("pilot.joycfg")));
}

void ConfigJoystickViewTest::closingPageDismissesPendingDialogs()
{
    auto *view = new ConfigJoystickView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->show();
    QVERIFY(QTest::qWaitForWindowExposed(view));
    required<QPushButton>(view, QStringLiteral("JoystickImportButton"))->click();
    QTRY_VERIFY(visible<QMessageBox>(
        view, QStringLiteral("JoystickImportConfirmation")));
    QPointer<ConfigJoystickView> guard(view);
    view->close();
    QTRY_VERIFY(guard.isNull());
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    ConfigJoystickViewTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_configjoystickview.moc"
