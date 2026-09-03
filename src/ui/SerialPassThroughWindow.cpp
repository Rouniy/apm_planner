#include "SerialPassThroughWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

SerialPassThroughWindow::SerialPassThroughWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    MavlinkMirrorService::OutputFactory outputFactory =
        std::move(dependencies.outputFactory);
    if (!outputFactory) {
        outputFactory = MavlinkMirrorService::ProductionOutputFactory();
    }
    m_service = new MavlinkMirrorService(
        std::move(dependencies.rawWriter), std::move(outputFactory), this);
    m_service->setUdpPortGuard(std::move(dependencies.udpPortGuard));
    m_viewModel = new SerialPassThroughViewModel(
        m_service, std::move(dependencies.viewModel), this);
    buildUi(owner);
    connect(m_viewModel, &SerialPassThroughViewModel::changed,
            this, &SerialPassThroughWindow::syncUi);
    syncUi();
}

SerialPassThroughWindow::~SerialPassThroughWindow()
{
    if (m_viewModel) {
        m_viewModel->stop();
    }
}

void SerialPassThroughWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SerialPassThroughWindow"));
    setWindowTitle(tr("Mavlink Mirror"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(WindowWidth, WindowHeight);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *header = new QLabel(tr("Mavlink Mirror"), this);
    header->setObjectName(QStringLiteral("serialPassThroughHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(
        tr("Forwards the vehicle's MAVLink byte stream out to a second port "
           "(and optionally back). Useful for feeding a second GCS or "
           "companion tool."), this);
    description->setObjectName(QStringLiteral("serialPassThroughDescription"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *settings = new QGridLayout;
    settings->setContentsMargins(0, 0, 0, 0);
    settings->setHorizontalSpacing(8);
    settings->setVerticalSpacing(8);
    settings->setColumnMinimumWidth(0, 110);

    settings->addWidget(new QLabel(tr("Port"), this), 0, 0);
    m_port = new QComboBox(this);
    m_port->setObjectName(QStringLiteral("serialPassThroughPort"));
    settings->addWidget(m_port, 0, 1);
    auto *refresh = new QPushButton(tr("Refresh"), this);
    refresh->setObjectName(QStringLiteral("serialPassThroughRefresh"));
    settings->addWidget(refresh, 0, 2);

    settings->addWidget(new QLabel(tr("Baud"), this), 1, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("serialPassThroughBaud"));
    settings->addWidget(m_baud, 1, 1);
    root->addLayout(settings);

    m_writeBack = new QCheckBox(tr("Allow write back to vehicle"), this);
    m_writeBack->setObjectName(QStringLiteral("serialPassThroughWriteBack"));
    m_writeBack->setToolTip(tr(
        "Bytes received from the mirror peer are written unmodified to the "
        "vehicle link."));
    root->addWidget(m_writeBack);

    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(QStringLiteral("serialPassThroughToggle"));
    m_toggle->setMinimumWidth(90);
    root->addWidget(m_toggle, 0, Qt::AlignLeft);

    auto *statusFrame = new QFrame(this);
    statusFrame->setObjectName(QStringLiteral("serialPassThroughStatusPanel"));
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QGridLayout(statusFrame);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setHorizontalSpacing(8);
    statusLayout->setVerticalSpacing(6);
    m_status = new QLabel(statusFrame);
    m_status->setObjectName(QStringLiteral("serialPassThroughStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: #99AADD;"));
    statusLayout->addWidget(m_status, 0, 0, 1, 2);
    statusLayout->addWidget(new QLabel(tr("Tx bytes:"), statusFrame), 1, 0);
    m_txBytes = new QLabel(statusFrame);
    m_txBytes->setObjectName(QStringLiteral("serialPassThroughTxBytes"));
    statusLayout->addWidget(m_txBytes, 1, 1);
    statusLayout->addWidget(new QLabel(tr("Rx bytes:"), statusFrame), 2, 0);
    m_rxBytes = new QLabel(statusFrame);
    m_rxBytes->setObjectName(QStringLiteral("serialPassThroughRxBytes"));
    statusLayout->addWidget(m_rxBytes, 2, 1);
    statusLayout->setColumnStretch(1, 1);
    root->addWidget(statusFrame, 1);

    connect(m_port, &QComboBox::currentTextChanged,
            m_viewModel, &SerialPassThroughViewModel::setSelectedPort);
    connect(m_baud, &QComboBox::currentTextChanged, this,
            [this](const QString &text) {
                m_viewModel->setSelectedBaud(text.toInt());
            });
    connect(refresh, &QPushButton::clicked,
            m_viewModel, &SerialPassThroughViewModel::refreshPorts);
    connect(m_writeBack, &QCheckBox::toggled,
            m_viewModel, &SerialPassThroughViewModel::setAllowWriteBack);
    connect(m_toggle, &QPushButton::clicked,
            m_viewModel, &SerialPassThroughViewModel::toggleConnection);
}

void SerialPassThroughWindow::syncUi()
{
    const QSignalBlocker blockPort(m_port);
    const QSignalBlocker blockBaud(m_baud);
    const QSignalBlocker blockWriteBack(m_writeBack);

    if (m_port->count() != m_viewModel->ports().size()
        || [&]() {
               for (int i = 0; i < m_port->count(); ++i) {
                   if (m_port->itemText(i) != m_viewModel->ports().at(i)) {
                       return true;
                   }
               }
               return false;
           }()) {
        m_port->clear();
        m_port->addItems(m_viewModel->ports());
    }
    m_port->setCurrentText(m_viewModel->selectedPort());

    if (m_baud->count() == 0) {
        for (int baud : m_viewModel->bauds()) {
            m_baud->addItem(QString::number(baud));
        }
    }
    m_baud->setCurrentText(QString::number(m_viewModel->selectedBaud()));
    m_writeBack->setChecked(m_viewModel->allowWriteBack());
    m_toggle->setText(m_viewModel->connectButtonText());
    m_status->setText(m_viewModel->statusText());
    m_txBytes->setText(QString::number(m_viewModel->txBytes()));
    m_rxBytes->setText(QString::number(m_viewModel->rxBytes()));
}
