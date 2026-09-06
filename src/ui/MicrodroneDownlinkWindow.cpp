#include "MicrodroneDownlinkWindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

MicrodroneDownlinkWindow::MicrodroneDownlinkWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    m_service = new MicrodroneDownlinkService(
        std::move(dependencies.service), this);
    m_viewModel = new MicrodroneDownlinkViewModel(
        m_service, std::move(dependencies.enumeratePorts), this);
    buildUi(owner);
    connect(m_viewModel, &MicrodroneDownlinkViewModel::changed,
            this, &MicrodroneDownlinkWindow::syncUi);
    syncUi();
}

MicrodroneDownlinkWindow::~MicrodroneDownlinkWindow()
{
    m_destroying = true;
    if (m_viewModel) {
        disconnect(m_viewModel, nullptr, this, nullptr);
        m_viewModel->stop();
    }
}

void MicrodroneDownlinkWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("MicrodroneDownlinkWindow"));
    setWindowTitle(tr("MicroDrone Downlink"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWidth, MinimumHeight);
    if (owner) {
        move(owner->geometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *header = new QLabel(tr("MicroDrone Downlink"), this);
    header->setObjectName(QStringLiteral("microdroneDownlinkHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(18);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Emits the official #1 and #4–#9 MicroDrone downlink records "
        "at up to 10 Hz over a serial port."), this);
    description->setObjectName(
        QStringLiteral("microdroneDownlinkDescription"));
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    root->addWidget(description);

    auto *warning = new QLabel(tr(
        "Use a separate output port. Opening a port used by the active "
        "vehicle link can interrupt telemetry. Output stops if the exact "
        "telemetry source changes."), this);
    warning->setObjectName(QStringLiteral("microdroneDownlinkWarning"));
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    root->addWidget(warning);

    auto *sourceFrame = new QFrame(this);
    sourceFrame->setObjectName(QStringLiteral("microdroneDownlinkSourcePanel"));
    sourceFrame->setFrameShape(QFrame::StyledPanel);
    auto *sourceLayout = new QVBoxLayout(sourceFrame);
    sourceLayout->setContentsMargins(10, 8, 10, 8);
    m_source = new QLabel(sourceFrame);
    m_source->setObjectName(QStringLiteral("microdroneDownlinkSource"));
    m_source->setTextFormat(Qt::PlainText);
    m_source->setWordWrap(true);
    sourceLayout->addWidget(m_source);
    root->addWidget(sourceFrame);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 100);
    grid->setColumnStretch(1, 1);
    grid->addWidget(new QLabel(tr("Serial port"), this), 0, 0);
    m_port = new QComboBox(this);
    m_port->setObjectName(QStringLiteral("microdroneDownlinkPort"));
    grid->addWidget(m_port, 0, 1);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName(
        QStringLiteral("RefreshMicrodronePortsButton"));
    grid->addWidget(m_refresh, 0, 2);
    grid->addWidget(new QLabel(tr("Baud"), this), 1, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("microdroneDownlinkBaud"));
    grid->addWidget(m_baud, 1, 1);
    root->addLayout(grid);

    auto *buttons = new QHBoxLayout;
    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(QStringLiteral("ToggleMicrodroneOutputButton"));
    m_toggle->setMinimumWidth(90);
    buttons->addWidget(m_toggle);
    m_busy = new QProgressBar(this);
    m_busy->setObjectName(QStringLiteral("microdroneDownlinkBusy"));
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(120);
    m_busy->setTextVisible(false);
    buttons->addWidget(m_busy);
    buttons->addStretch(1);
    root->addLayout(buttons);

    auto *statusFrame = new QFrame(this);
    statusFrame->setObjectName(QStringLiteral("microdroneDownlinkStatusPanel"));
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QVBoxLayout(statusFrame);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(5);
    m_status = new QLabel(statusFrame);
    m_status->setObjectName(QStringLiteral("microdroneDownlinkStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    statusLayout->addWidget(m_status);
    statusLayout->addWidget(
        new QLabel(tr("Last complete record:"), statusFrame));
    m_lastLine = new QLabel(statusFrame);
    m_lastLine->setObjectName(QStringLiteral("microdroneDownlinkLastLine"));
    m_lastLine->setTextFormat(Qt::PlainText);
    m_lastLine->setWordWrap(true);
    m_lastLine->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    statusLayout->addWidget(m_lastLine);
    root->addWidget(statusFrame, 1);

    connect(m_port, &QComboBox::currentTextChanged,
            m_viewModel, &MicrodroneDownlinkViewModel::setSelectedPort);
    connect(m_baud, &QComboBox::currentTextChanged, this,
            [this](const QString &text) {
        if (m_viewModel)
            m_viewModel->setSelectedBaud(text.toInt());
    });
    connect(m_refresh, &QPushButton::clicked, this, [this]() {
        if (!m_closing && m_viewModel)
            m_viewModel->refreshPorts();
    });
    connect(m_toggle, &QPushButton::clicked, this, [this]() {
        if (!m_closing && m_viewModel)
            m_viewModel->toggleConnection();
    });
}

bool MicrodroneDownlinkWindow::syncCombo(
    QComboBox *combo, const QStringList &items, const QString &selected)
{
    const QPointer<MicrodroneDownlinkWindow> guard(this);
    const QPointer<QComboBox> comboGuard(combo);
    if (!comboGuard)
        return false;
    const bool wasBlocked = comboGuard->blockSignals(true);
    bool different = comboGuard->count() != items.size();
    for (int index = 0; !different && index < items.size(); ++index)
        different = comboGuard->itemText(index) != items.at(index);
    if (different) {
        comboGuard->clear();
        if (!guard || !comboGuard)
            return false;
        comboGuard->addItems(items);
        if (!guard || !comboGuard)
            return false;
    }
    comboGuard->setCurrentText(selected);
    if (!guard || !comboGuard)
        return false;
    comboGuard->blockSignals(wasBlocked);
    return true;
}

void MicrodroneDownlinkWindow::syncUi()
{
    const QPointer<MicrodroneDownlinkWindow> guard(this);
    const QPointer<MicrodroneDownlinkViewModel> model(m_viewModel);
    if (m_destroying || !model)
        return;
    const QStringList ports = model->ports();
    const QString selectedPort = model->selectedPort();
    QStringList bauds;
    for (int baud : model->bauds())
        bauds.append(QString::number(baud));
    const QString selectedBaud = QString::number(model->selectedBaud());
    const bool editable = model->canEditSettings();
    const bool serviceBusy = model->busy();
    const QString buttonText = model->connectButtonText();
    const QString sourceText = model->sourceDescription();
    const QString statusText = model->statusText();
    const QString last = model->lastLine();

    if (!syncCombo(m_port, ports, selectedPort) || !guard)
        return;
    if (!syncCombo(m_baud, bauds, selectedBaud) || !guard)
        return;
    m_port->setEnabled(editable);
    if (!guard)
        return;
    m_baud->setEnabled(editable);
    if (!guard)
        return;
    m_refresh->setEnabled(editable);
    if (!guard)
        return;
    m_toggle->setEnabled(!serviceBusy);
    if (!guard)
        return;
    m_toggle->setText(buttonText);
    if (!guard)
        return;
    m_busy->setVisible(serviceBusy);
    if (!guard)
        return;
    m_source->setText(sourceText);
    if (!guard)
        return;
    m_status->setText(statusText);
    if (!guard)
        return;
    m_lastLine->setText(last);
}

void MicrodroneDownlinkWindow::closeEvent(QCloseEvent *event)
{
    if (!m_closing)
        m_closing = true;
    const QPointer<MicrodroneDownlinkWindow> guard(this);
    if (m_viewModel)
        m_viewModel->stop();
    if (!guard)
        return;
    m_toggle->setEnabled(false);
    if (!guard)
        return;
    m_refresh->setEnabled(false);
    if (!guard)
        return;
    m_port->setEnabled(false);
    if (!guard)
        return;
    m_baud->setEnabled(false);
    if (!guard)
        return;
    QWidget::closeEvent(event);
}
