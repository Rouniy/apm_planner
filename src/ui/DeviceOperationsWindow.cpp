#include "DeviceOperationsWindow.h"

#include "comm/VehicleTargetManager.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <utility>

DeviceOperationsWindow::DeviceOperationsWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    Q_ASSERT(dependencies.targetManager);
    Q_ASSERT(dependencies.transmitter);

    if (!dependencies.viewModel.resolveTarget) {
        const QPointer<VehicleTargetManager> targets(
            dependencies.targetManager);
        dependencies.viewModel.resolveTarget = [targets]() {
            return targets ? targets->acquireTarget()
                           : VehicleTargetLease();
        };
    }
    if (!dependencies.viewModel.confirm) {
        dependencies.viewModel.confirm = [this](
            const QString &title, const QString &message) {
            return QMessageBox::warning(
                       this, title, message,
                       QMessageBox::Yes | QMessageBox::Cancel,
                       QMessageBox::Cancel)
                == QMessageBox::Yes;
        };
    }

    m_service = new DeviceOperationService(
        dependencies.targetManager, dependencies.transmitter, this);
    m_service->setLocalIdentity(dependencies.localSystemId,
                                dependencies.localComponentId);
    m_viewModel = new DeviceOperationsViewModel(
        m_service, std::move(dependencies.viewModel), this);

    buildUi(owner);
    connect(m_viewModel, &DeviceOperationsViewModel::changed,
            this, &DeviceOperationsWindow::syncUi);
    syncUi();
}

DeviceOperationsWindow::~DeviceOperationsWindow()
{
    if (m_viewModel) {
        m_viewModel->shutdown();
    } else if (m_service) {
        m_service->shutdown();
    }
}

void DeviceOperationsWindow::closeEvent(QCloseEvent *event)
{
    if (m_viewModel) {
        // shutdown() clears the timer and pending request without publishing a
        // late result into a view which is already closing.
        m_viewModel->shutdown();
    }
    QWidget::closeEvent(event);
}

void DeviceOperationsWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("DeviceOperationsWindow"));
    setWindowTitle(tr("MAVLink Device Operations"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *header = new QLabel(tr("MAVLink Device Operations"), this);
    header->setObjectName(QStringLiteral("deviceOperationsHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(18);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Low-level ArduPilot DEVICE_OP access for SPI and I²C peripherals. "
        "Values use decimal input; results are shown in hexadecimal."), this);
    description->setObjectName(
        QStringLiteral("deviceOperationsDescription"));
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    auto *targetGrid = new QGridLayout;
    targetGrid->setContentsMargins(0, 0, 0, 0);
    targetGrid->setHorizontalSpacing(8);
    targetGrid->addWidget(new QLabel(tr("System"), this), 0, 0);
    m_systemId = new QSpinBox(this);
    m_systemId->setObjectName(QStringLiteral("deviceOperationsSystemId"));
    m_systemId->setRange(1, 255);
    m_systemId->setFixedWidth(90);
    targetGrid->addWidget(m_systemId, 0, 1);
    targetGrid->addWidget(new QLabel(tr("Component"), this), 0, 2);
    m_componentId = new QSpinBox(this);
    m_componentId->setObjectName(
        QStringLiteral("deviceOperationsComponentId"));
    m_componentId->setRange(1, 255);
    m_componentId->setFixedWidth(90);
    targetGrid->addWidget(m_componentId, 0, 3);
    targetGrid->addWidget(new QLabel(tr("Bus type"), this), 0, 4);
    m_busType = new QComboBox(this);
    m_busType->setObjectName(QStringLiteral("deviceOperationsBusType"));
    m_busType->addItems(m_viewModel->busTypes());
    m_busType->setFixedWidth(100);
    targetGrid->addWidget(m_busType, 0, 5);
    targetGrid->setColumnStretch(6, 1);
    root->addLayout(targetGrid);

    auto *requestGrid = new QGridLayout;
    requestGrid->setContentsMargins(0, 0, 0, 0);
    requestGrid->setHorizontalSpacing(7);
    m_busNameLabel = new QLabel(tr("SPI name"), this);
    m_busNameLabel->setObjectName(
        QStringLiteral("deviceOperationsBusNameLabel"));
    requestGrid->addWidget(m_busNameLabel, 0, 0);
    m_busName = new QLineEdit(this);
    m_busName->setObjectName(QStringLiteral("deviceOperationsBusName"));
    m_busName->setMinimumWidth(150);
    requestGrid->addWidget(m_busName, 0, 1);
    m_busNumberLabel = new QLabel(tr("Bus"), this);
    m_busNumberLabel->setObjectName(
        QStringLiteral("deviceOperationsBusNumberLabel"));
    requestGrid->addWidget(m_busNumberLabel, 0, 2);
    m_busNumber = new QSpinBox(this);
    m_busNumber->setObjectName(
        QStringLiteral("deviceOperationsBusNumber"));
    m_busNumber->setRange(0, 255);
    m_busNumber->setFixedWidth(78);
    requestGrid->addWidget(m_busNumber, 0, 3);
    m_addressLabel = new QLabel(tr("Address"), this);
    m_addressLabel->setObjectName(
        QStringLiteral("deviceOperationsAddressLabel"));
    requestGrid->addWidget(m_addressLabel, 0, 4);
    m_address = new QSpinBox(this);
    m_address->setObjectName(QStringLiteral("deviceOperationsAddress"));
    m_address->setRange(0, 255);
    m_address->setFixedWidth(78);
    requestGrid->addWidget(m_address, 0, 5);
    requestGrid->addWidget(new QLabel(tr("Register"), this), 0, 6);
    m_registerStart = new QSpinBox(this);
    m_registerStart->setObjectName(
        QStringLiteral("deviceOperationsRegisterStart"));
    m_registerStart->setRange(0, 255);
    m_registerStart->setFixedWidth(78);
    requestGrid->addWidget(m_registerStart, 0, 7);
    requestGrid->addWidget(new QLabel(tr("Count"), this), 0, 8);
    m_count = new QSpinBox(this);
    m_count->setObjectName(QStringLiteral("deviceOperationsCount"));
    m_count->setRange(1, 128);
    m_count->setFixedWidth(78);
    requestGrid->addWidget(m_count, 0, 9);
    requestGrid->setColumnStretch(10, 1);
    root->addLayout(requestGrid);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("deviceOperationsOutput"));
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#deviceOperationsOutput { background: #000000; "
        "color: #34D399; border: 1px solid #121614; }"));
    root->addWidget(m_output, 1);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    m_useActiveTarget = new QPushButton(tr("Use Active Target"), this);
    m_useActiveTarget->setObjectName(QStringLiteral("UseActiveTargetButton"));
    actions->addWidget(m_useActiveTarget);
    m_read = new QPushButton(tr("Read Registers"), this);
    m_read->setObjectName(QStringLiteral("ReadRegistersButton"));
    actions->addWidget(m_read);
    m_test = new QPushButton(tr("ICM20948 Write/Read Test"), this);
    m_test->setObjectName(QStringLiteral("TestIcm20948Button"));
    actions->addWidget(m_test);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("deviceOperationsProgress"));
    m_progress->setRange(0, 0);
    m_progress->setFixedWidth(120);
    m_progress->setFixedHeight(5);
    m_progress->setTextVisible(false);
    actions->addWidget(m_progress);
    actions->addStretch(1);
    root->addLayout(actions);

    connect(m_systemId, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setSystemId);
    connect(m_componentId, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setComponentId);
    connect(m_busType, &QComboBox::currentTextChanged,
            m_viewModel, &DeviceOperationsViewModel::setBusType);
    connect(m_busName, &QLineEdit::textChanged,
            m_viewModel, &DeviceOperationsViewModel::setBusName);
    connect(m_busNumber, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setBusNumber);
    connect(m_address, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setAddress);
    connect(m_registerStart, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setRegisterStart);
    connect(m_count, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &DeviceOperationsViewModel::setCount);
    connect(m_useActiveTarget, &QPushButton::clicked,
            m_viewModel, &DeviceOperationsViewModel::useActiveTarget);
    connect(m_read, &QPushButton::clicked,
            m_viewModel, &DeviceOperationsViewModel::readRegisters);
    connect(m_test, &QPushButton::clicked,
            m_viewModel, &DeviceOperationsViewModel::testIcm20948);
}

void DeviceOperationsWindow::syncUi()
{
    if (!m_viewModel) {
        return;
    }
    const QSignalBlocker blockSystem(m_systemId);
    const QSignalBlocker blockComponent(m_componentId);
    const QSignalBlocker blockBusType(m_busType);
    const QSignalBlocker blockBusName(m_busName);
    const QSignalBlocker blockBusNumber(m_busNumber);
    const QSignalBlocker blockAddress(m_address);
    const QSignalBlocker blockRegister(m_registerStart);
    const QSignalBlocker blockCount(m_count);

    m_systemId->setValue(m_viewModel->systemId());
    m_componentId->setValue(m_viewModel->componentId());
    m_busType->setCurrentText(m_viewModel->busType());
    m_busName->setText(m_viewModel->busName());
    m_busNumber->setValue(m_viewModel->busNumber());
    m_address->setValue(m_viewModel->address());
    m_registerStart->setValue(m_viewModel->registerStart());
    m_count->setValue(m_viewModel->count());
    if (m_output->toPlainText() != m_viewModel->output()) {
        m_output->setPlainText(m_viewModel->output());
    }

    const bool busy = m_viewModel->isBusy();
    const bool spi = m_viewModel->isSpi();
    m_systemId->setEnabled(!busy);
    m_componentId->setEnabled(!busy);
    m_busType->setEnabled(!busy);
    m_busName->setEnabled(!busy && spi);
    m_busNameLabel->setEnabled(!busy && spi);
    m_busNumber->setEnabled(!busy && !spi);
    m_busNumberLabel->setEnabled(!busy && !spi);
    m_address->setEnabled(!busy && !spi);
    m_addressLabel->setEnabled(!busy && !spi);
    m_registerStart->setEnabled(!busy);
    m_count->setEnabled(!busy);
    m_useActiveTarget->setEnabled(!busy);
    m_read->setEnabled(m_viewModel->canOperate());
    m_test->setEnabled(m_viewModel->canTest());
    m_progress->setVisible(busy);
}
