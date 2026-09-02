#include <QtTest>

#include "ui/configuration/ConfigHWBTView.h"

#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextEdit>

namespace {
const QString kInstructions = QStringLiteral(
    "Configures an HC-05/HC-06 style serial Bluetooth module via AT "
    "commands. Disconnect the main link first, select the module's "
    "serial port, then click Write.");

template<typename T>
T *requiredChild(ConfigHWBTView *view, const char *objectName)
{
    T *child = view->findChild<T *>(QString::fromLatin1(objectName));
    if (!child) {
        QTest::qFail(objectName, __FILE__, __LINE__);
    }
    return child;
}

void prepareWritableModel(ConfigHWBTViewModel *model)
{
    model->setPorts({QStringLiteral("COM7")});
    model->setName(QStringLiteral("APM BT"));
    model->setSelectedBaud(QStringLiteral("115200"));
    model->setPin(QStringLiteral("2468"));
}
} // namespace

class ConfigHWBTViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void exactDefaultsBaudsAndPayloads();
    void portsAreDistinctPreferredAndPreserved();
    void validationNeverStartsARequest();
    void requestIsSnapshottedAndBusyBlocksDuplicates();
    void progressCompletionCancelAndStaleCallbacksAreDeterministic();
    void widgetMatchesMissionPlannerContract();
    void hidingBusyViewCancelsOperation();
};

void ConfigHWBTViewTest::exactDefaultsBaudsAndPayloads()
{
    ConfigHWBTViewModel model;

    QCOMPARE(model.Title(), QStringLiteral("Bluetooth Setup"));
    QCOMPARE(model.Instructions(), kInstructions);
    QCOMPARE(model.Ports(), QStringList());
    QCOMPARE(model.SelectedPort(), QString());
    QCOMPARE(model.Name(), QString());
    QCOMPARE(model.SelectedBaud(), QStringLiteral("57600"));
    QCOMPARE(model.Pin(), QStringLiteral("1234"));
    QCOMPARE(model.Output(), QString());
    QVERIFY(!model.IsBusy());
    QVERIFY(!model.IsCanceling());
    QVERIFY(!model.IsConnected());
    QCOMPARE(model.Generation(), quint64(0));

    const QStringList expectedBauds = {
        QStringLiteral("1200"), QStringLiteral("2400"),
        QStringLiteral("4800"), QStringLiteral("9600"),
        QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200")
    };
    QCOMPARE(model.Bauds(), expectedBauds);

    const QHash<int, int> baudMap = ConfigHWBTViewModel::BaudMap();
    QCOMPARE(baudMap.size(), 8);
    QCOMPARE(baudMap.value(1200), 1);
    QCOMPARE(baudMap.value(2400), 2);
    QCOMPARE(baudMap.value(4800), 3);
    QCOMPARE(baudMap.value(9600), 4);
    QCOMPARE(baudMap.value(19200), 5);
    QCOMPARE(baudMap.value(38400), 6);
    QCOMPARE(baudMap.value(57600), 7);
    QCOMPARE(baudMap.value(115200), 8);
    QCOMPARE(
        ConfigHWBTViewModel::ProbeBauds(),
        QList<int>({57600, 38400, 9600, 19200,
                    115200, 1200, 2400, 4800}));

    const QList<QByteArray> expectedCommands = {
        QByteArrayLiteral("AT"),
        QByteArrayLiteral("AT+VERSION"),
        QByteArrayLiteral("AT+ROLE=0\r\n"),
        QByteArrayLiteral("AT+NAME=APM BT\r\n"),
        QByteArrayLiteral("AT+NAMEAPM BT"),
        QByteArrayLiteral("AT+BAUD=115200\r\n"),
        QByteArrayLiteral("AT+BAUD8"),
        QByteArrayLiteral("AT+PSWD=2468\r\n"),
        QByteArrayLiteral("AT+PIN2468"),
        QByteArrayLiteral("AT+RESET")
    };
    const QList<QByteArray> commands = ConfigHWBTViewModel::BuildCommands(
        QStringLiteral("APM BT"), QStringLiteral("115200"),
        QStringLiteral("2468"));
    QCOMPARE(commands.size(), 10);
    QCOMPARE(commands, expectedCommands);
}

void ConfigHWBTViewTest::portsAreDistinctPreferredAndPreserved()
{
    ConfigHWBTViewModel model;

    model.setPorts({
        QStringLiteral(" COM1 "), QStringLiteral("COM2"),
        QStringLiteral("com2"), QString(), QStringLiteral("COM1")
    }, QStringLiteral("COM2"));
    QCOMPARE(model.Ports(),
             QStringList({QStringLiteral("COM1"),
                          QStringLiteral("COM2")}));
    QCOMPARE(model.SelectedPort(), QStringLiteral("COM2"));

    model.setSelectedPort(QStringLiteral("COM1"));
    model.setPorts({QStringLiteral("COM3"), QStringLiteral("COM1"),
                    QStringLiteral("COM4")},
                   QStringLiteral("COM3"));
    QCOMPARE(model.SelectedPort(), QStringLiteral("COM1"));

    model.setPorts({QStringLiteral("COM3"), QStringLiteral("COM4")},
                   QStringLiteral("COM4"));
    QCOMPARE(model.SelectedPort(), QStringLiteral("COM4"));

    model.setPorts({QStringLiteral("COM3")},
                   QStringLiteral("missing"));
    QCOMPARE(model.SelectedPort(), QStringLiteral("COM3"));

    model.setPorts({});
    QCOMPARE(model.Ports(), QStringList());
    QCOMPARE(model.SelectedPort(), QString());
}

void ConfigHWBTViewTest::validationNeverStartsARequest()
{
    {
        ConfigHWBTViewModel model;
        model.setPorts({QStringLiteral("COM7")});
        model.setMainLinkConnected(true);
        QSignalSpy requests(&model,
                           &ConfigHWBTViewModel::programRequested);
        QVERIFY(!model.Write());
        QCOMPARE(requests.count(), 0);
        QCOMPARE(
            model.Output(),
            QStringLiteral(
                "Please disconnect the main link before configuring "
                "Bluetooth.\n"));
        QVERIFY(!model.IsBusy());
    }

    {
        ConfigHWBTViewModel model;
        QSignalSpy requests(&model,
                           &ConfigHWBTViewModel::programRequested);
        QVERIFY(!model.Write());
        QCOMPARE(requests.count(), 0);
        QCOMPARE(model.Output(),
                 QStringLiteral("No serial port selected.\n"));
        QVERIFY(!model.IsBusy());
    }

    {
        ConfigHWBTViewModel model;
        model.setPorts({QStringLiteral("COM7")});
        model.setSelectedBaud(QStringLiteral("74880"));
        QSignalSpy requests(&model,
                           &ConfigHWBTViewModel::programRequested);
        QVERIFY(!model.Write());
        QCOMPARE(requests.count(), 0);
        QCOMPARE(model.Output(), QStringLiteral("Invalid baud rate.\n"));
        QVERIFY(!model.IsBusy());
    }

    struct InvalidInput {
        QString name;
        QString pin;
        QString error;
    };
    const QList<InvalidInput> invalidValues = {
        {QStringLiteral("APM\nAT+RESET"), QStringLiteral("1234"),
         QStringLiteral("Name must be 1–32 printable ASCII characters.\n")},
        {QStringLiteral("APM"), QStringLiteral("12\r34"),
         QStringLiteral("PIN must be 1–16 printable ASCII characters.\n")},
        {QStringLiteral("APM") + QString(QChar(0x7f)),
         QStringLiteral("1234"),
         QStringLiteral("Name must be 1–32 printable ASCII characters.\n")},
        {QString(), QStringLiteral("1234"),
         QStringLiteral("Name must be 1–32 printable ASCII characters.\n")},
        {QStringLiteral("APM"), QString(),
         QStringLiteral("PIN must be 1–16 printable ASCII characters.\n")},
        {QStringLiteral("АПМ"), QStringLiteral("1234"),
         QStringLiteral("Name must be 1–32 printable ASCII characters.\n")}
    };
    for (const InvalidInput &values : invalidValues) {
        ConfigHWBTViewModel model;
        model.setPorts({QStringLiteral("COM7")});
        model.setName(values.name);
        model.setPin(values.pin);
        QSignalSpy requests(&model,
                           &ConfigHWBTViewModel::programRequested);
        QVERIFY(!model.Write());
        QCOMPARE(requests.count(), 0);
        QCOMPARE(model.Output(), values.error);
        QVERIFY(!model.IsBusy());
    }
}

void ConfigHWBTViewTest::requestIsSnapshottedAndBusyBlocksDuplicates()
{
    ConfigHWBTViewModel model;
    prepareWritableModel(&model);
    QSignalSpy requests(&model,
                       &ConfigHWBTViewModel::programRequested);

    QVERIFY(model.Write());
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.at(0).at(0).toULongLong(), quint64(1));
    QCOMPARE(requests.at(0).at(1).toString(), QStringLiteral("COM7"));
    QCOMPARE(requests.at(0).at(2).toString(), QStringLiteral("APM BT"));
    QCOMPARE(requests.at(0).at(3).toString(), QStringLiteral("115200"));
    QCOMPARE(requests.at(0).at(4).toString(), QStringLiteral("2468"));
    QCOMPARE(model.Generation(), quint64(1));
    QVERIFY(model.IsBusy());

    model.setSelectedPort(QStringLiteral("COM8"));
    model.setName(QStringLiteral("changed"));
    model.setSelectedBaud(QStringLiteral("9600"));
    model.setPin(QStringLiteral("9999"));
    QVERIFY(!model.Write());
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.at(0).at(1).toString(), QStringLiteral("COM7"));
    QCOMPARE(requests.at(0).at(2).toString(), QStringLiteral("APM BT"));
    QCOMPARE(requests.at(0).at(3).toString(), QStringLiteral("115200"));
    QCOMPARE(requests.at(0).at(4).toString(), QStringLiteral("2468"));
    QCOMPARE(model.Generation(), quint64(1));

    model.operationFinished(1, true, false);
    QVERIFY(!model.IsBusy());
    QVERIFY(model.Write());
    QCOMPARE(requests.count(), 2);
    QCOMPARE(requests.at(1).at(0).toULongLong(), quint64(2));
    QCOMPARE(requests.at(1).at(1).toString(), QStringLiteral("COM8"));
    QCOMPARE(requests.at(1).at(2).toString(),
             QStringLiteral("changed"));
    QCOMPARE(requests.at(1).at(3).toString(), QStringLiteral("9600"));
    QCOMPARE(requests.at(1).at(4).toString(), QStringLiteral("9999"));
}

void ConfigHWBTViewTest::
    progressCompletionCancelAndStaleCallbacksAreDeterministic()
{
    ConfigHWBTViewModel model;
    prepareWritableModel(&model);
    QSignalSpy requests(&model,
                       &ConfigHWBTViewModel::programRequested);
    QSignalSpy cancels(&model,
                      &ConfigHWBTViewModel::cancelRequested);

    QVERIFY(model.Write());
    const quint64 successGeneration = model.Generation();
    model.operationProgress(successGeneration + 1,
                            QStringLiteral("stale progress"));
    QCOMPARE(model.Output(), QString());
    model.operationProgress(successGeneration,
                            QStringLiteral("Try baud 57600"));
    QCOMPARE(model.Output(), QStringLiteral("Try baud 57600\n"));
    model.operationFinished(successGeneration + 1, false, false);
    QVERIFY(model.IsBusy());
    model.operationFinished(successGeneration, true, false);
    QVERIFY(!model.IsBusy());
    QCOMPARE(model.Output(),
             QStringLiteral(
                 "Try baud 57600\nProgramming sequence sent. Review "
                 "device responses above.\n"));
    model.operationProgress(successGeneration,
                            QStringLiteral("late progress"));
    QCOMPARE(model.Output(),
             QStringLiteral(
                 "Try baud 57600\nProgramming sequence sent. Review "
                 "device responses above.\n"));

    QVERIFY(model.Write());
    const quint64 failureGeneration = model.Generation();
    QCOMPARE(model.Output(), QString());
    model.operationFinished(failureGeneration, false, false);
    QVERIFY(!model.IsBusy());
    QCOMPARE(
        model.Output(),
        QStringLiteral("Error setting parameter — no device responded.\n"));

    QVERIFY(model.Write());
    const quint64 serviceCancelGeneration = model.Generation();
    model.operationFinished(serviceCancelGeneration, false, true);
    QVERIFY(!model.IsBusy());
    QCOMPARE(model.Output(), QStringLiteral("Operation canceled.\n"));
    QCOMPARE(cancels.count(), 0);

    QVERIFY(model.Write());
    const quint64 localCancelGeneration = model.Generation();
    model.operationProgress(localCancelGeneration,
                            QStringLiteral("Sending AT"));
    model.Cancel();
    QVERIFY(model.IsBusy());
    QVERIFY(model.IsCanceling());
    QCOMPARE(cancels.count(), 1);
    QCOMPARE(cancels.at(0).at(0).toULongLong(),
             localCancelGeneration);
    QCOMPARE(model.Output(),
             QStringLiteral("Sending AT\nCanceling operation…\n"));
    QCOMPARE(model.Generation(), localCancelGeneration);
    model.operationProgress(localCancelGeneration,
                            QStringLiteral("late canceled progress"));
    QVERIFY(!model.Write());
    model.operationFinished(localCancelGeneration, false, true);
    QVERIFY(!model.IsBusy());
    QVERIFY(!model.IsCanceling());
    QCOMPARE(model.Output(),
             QStringLiteral(
                 "Sending AT\nCanceling operation…\nOperation canceled.\n"));

    QVERIFY(model.Write());
    const quint64 replacementGeneration = model.Generation();
    QCOMPARE(replacementGeneration, localCancelGeneration + 1);
    QCOMPARE(model.Output(), QString());
    model.operationProgress(localCancelGeneration,
                            QStringLiteral("older generation"));
    model.operationFinished(localCancelGeneration, true, false);
    QVERIFY(model.IsBusy());
    QCOMPARE(model.Output(), QString());
    model.operationProgress(replacementGeneration,
                            QStringLiteral("Valid Answer"));
    model.operationFinished(replacementGeneration, true, false);
    QVERIFY(!model.IsBusy());
    QCOMPARE(model.Output(),
             QStringLiteral(
                 "Valid Answer\nProgramming sequence sent. Review "
                 "device responses above.\n"));

    const int cancelCount = cancels.count();
    model.Cancel();
    QCOMPARE(cancels.count(), cancelCount);
    QCOMPARE(requests.count(), 5);
}

void ConfigHWBTViewTest::widgetMatchesMissionPlannerContract()
{
    ConfigHWBTView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigHWBTView"));
    QCOMPARE(view.sizeHint(), QSize(800, 640));
    QVERIFY(view.layout());
    QCOMPARE(view.layout()->contentsMargins(), QMargins(16, 16, 16, 16));

    QLabel *title = requiredChild<QLabel>(&view, "bluetoothSetupTitle");
    QLabel *intro = requiredChild<QLabel>(&view, "bluetoothSetupIntro");
    QWidget *fields = requiredChild<QWidget>(
        &view, "bluetoothSetupFields");
    QComboBox *port = requiredChild<QComboBox>(
        &view, "bluetoothPortCombo");
    QLineEdit *name = requiredChild<QLineEdit>(
        &view, "bluetoothNameEdit");
    QComboBox *baud = requiredChild<QComboBox>(
        &view, "bluetoothBaudCombo");
    QLineEdit *pin = requiredChild<QLineEdit>(
        &view, "bluetoothPinEdit");
    QPushButton *write = requiredChild<QPushButton>(
        &view, "bluetoothWriteButton");
    QFrame *outputFrame = requiredChild<QFrame>(
        &view, "bluetoothOutputFrame");
    QTextEdit *output = requiredChild<QTextEdit>(
        &view, "bluetoothOutput");
    QVERIFY(fields);
    QVERIFY(outputFrame);

    QCOMPARE(title->text(), QStringLiteral("Bluetooth Setup"));
    QCOMPARE(intro->text(), kInstructions);
    QVERIFY(intro->wordWrap());
    QCOMPARE(intro->contentsMargins(), QMargins(0, 0, 0, 12));

    struct LabelContract {
        const char *objectName;
        const char *text;
    };
    const LabelContract labels[] = {
        {"bluetoothPortLabel", "Port"},
        {"bluetoothNameLabel", "Name"},
        {"bluetoothBaudLabel", "Baud"},
        {"bluetoothPinLabel", "PIN"}
    };
    for (const LabelContract &contract : labels) {
        QLabel *label = requiredChild<QLabel>(&view, contract.objectName);
        QCOMPARE(label->text(), QString::fromLatin1(contract.text));
        QCOMPARE(label->minimumWidth(), 120);
        QCOMPARE(label->maximumWidth(), 120);
    }

    QCOMPARE(port->count(), 0);
    QCOMPARE(name->text(), QString());
    QCOMPARE(pin->text(), QStringLiteral("1234"));
    QCOMPARE(write->text(), QStringLiteral("Write"));
    const QStringList expectedBauds = {
        QStringLiteral("1200"), QStringLiteral("2400"),
        QStringLiteral("4800"), QStringLiteral("9600"),
        QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200")
    };
    QCOMPARE(baud->count(), expectedBauds.size());
    for (int index = 0; index < baud->count(); ++index) {
        QCOMPARE(baud->itemText(index), expectedBauds.at(index));
    }
    QCOMPARE(baud->currentText(), QStringLiteral("57600"));
    QVERIFY(output->isReadOnly());
    QVERIFY(!output->acceptRichText());
    QCOMPARE(output->lineWrapMode(), QTextEdit::WidgetWidth);
    QCOMPARE(output->frameShape(), QFrame::NoFrame);
    QCOMPARE(output->toPlainText(), QString());

    view.setPorts({QStringLiteral("COM7"), QStringLiteral("COM8")},
                  QStringLiteral("COM8"));
    QCOMPARE(port->count(), 2);
    QCOMPARE(port->currentText(), QStringLiteral("COM8"));
    name->setText(QStringLiteral("APM BT"));
    baud->setCurrentText(QStringLiteral("115200"));
    pin->setText(QStringLiteral("2468"));
    QSignalSpy requests(&view, &ConfigHWBTView::programRequested);
    write->click();
    QCOMPARE(requests.count(), 1);
    const quint64 generation = requests.at(0).at(0).toULongLong();
    QVERIFY(view.viewModel()->IsBusy());
    QVERIFY(!port->isEnabled());
    QVERIFY(!name->isEnabled());
    QVERIFY(!baud->isEnabled());
    QVERIFY(!pin->isEnabled());
    QVERIFY(!write->isEnabled());

    view.operationProgress(generation, QStringLiteral("Try baud 57600"));
    QCOMPARE(output->toPlainText(),
             view.viewModel()->Output());
    QCOMPARE(output->toPlainText(), QStringLiteral("Try baud 57600\n"));
    view.operationFinished(generation, true, false);
    QVERIFY(!view.viewModel()->IsBusy());
    QVERIFY(port->isEnabled());
    QVERIFY(name->isEnabled());
    QVERIFY(baud->isEnabled());
    QVERIFY(pin->isEnabled());
    QVERIFY(write->isEnabled());
    QCOMPARE(output->toPlainText(),
             QStringLiteral(
                 "Try baud 57600\nProgramming sequence sent. Review "
                 "device responses above.\n"));
}

void ConfigHWBTViewTest::hidingBusyViewCancelsOperation()
{
    ConfigHWBTView view;
    view.setPorts({QStringLiteral("COM7")});
    view.viewModel()->setName(QStringLiteral("APM BT"));
    QSignalSpy requests(&view, &ConfigHWBTView::programRequested);
    QSignalSpy cancels(&view, &ConfigHWBTView::cancelRequested);

    view.show();
    QCoreApplication::processEvents();
    QVERIFY(view.isVisible());
    QVERIFY(view.viewModel()->Write());
    QCOMPARE(requests.count(), 1);
    const quint64 generation = requests.at(0).at(0).toULongLong();
    QVERIFY(view.viewModel()->IsBusy());

    view.hide();
    QCoreApplication::processEvents();
    QVERIFY(!view.isVisible());
    QVERIFY(view.viewModel()->IsBusy());
    QVERIFY(view.viewModel()->IsCanceling());
    QCOMPARE(cancels.count(), 1);
    QCOMPARE(cancels.at(0).at(0).toULongLong(), generation);
    QCOMPARE(view.viewModel()->Output(),
             QStringLiteral("Canceling operation…\n"));

    view.operationProgress(generation, QStringLiteral("late progress"));
    QCOMPARE(view.viewModel()->Output(),
             QStringLiteral("Canceling operation…\n"));
    view.operationFinished(generation, false, true);
    QVERIFY(!view.viewModel()->IsBusy());
    QVERIFY(!view.viewModel()->IsCanceling());
    QCOMPARE(view.viewModel()->Output(),
             QStringLiteral(
                 "Canceling operation…\nOperation canceled.\n"));
}

QTEST_MAIN(ConfigHWBTViewTest)

#include "test_confighwbtview.moc"
