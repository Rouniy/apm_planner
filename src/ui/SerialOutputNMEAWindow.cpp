#include "SerialOutputNMEAWindow.h"

#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

SerialOutputNMEAWindow::SerialOutputNMEAWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    NmeaOutputService::OutputFactory outputFactory =
        std::move(dependencies.outputFactory);
    if (!outputFactory) {
        outputFactory = NmeaOutputService::ProductionOutputFactory();
    }
    m_service = new NmeaOutputService(
        std::move(outputFactory), std::move(dependencies.clock), this);
    m_service->setUdpPortGuard(std::move(dependencies.udpPortGuard));
    m_viewModel = new SerialOutputNMEAViewModel(
        m_service, std::move(dependencies.viewModel), this);
    buildUi(owner);
    connect(m_viewModel, &SerialOutputNMEAViewModel::changed,
            this, &SerialOutputNMEAWindow::syncUi);
    syncUi();
}

SerialOutputNMEAWindow::~SerialOutputNMEAWindow()
{
    m_viewModel->stop();
}

void SerialOutputNMEAWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SerialOutputNMEAWindow"));
    setWindowTitle(tr("NMEA Output"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(WindowWidth, WindowHeight);

    if (owner) {
        const QPoint topLeft = owner->geometry().center()
            - QPoint(WindowWidth / 2, WindowHeight / 2);
        move(topLeft);
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *header = new QLabel(tr("NMEA Output"), this);
    header->setObjectName(QStringLiteral("serialOutputNmeaHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Emits NMEA-0183 GGA/GLL/HDG/VTG/RMC sentences from the live "
        "vehicle position to the selected port."), this);
    description->setObjectName(
        QStringLiteral("serialOutputNmeaDescription"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 110);
    grid->setColumnStretch(1, 1);

    grid->addWidget(new QLabel(tr("Port"), this), 0, 0);
    m_port = new QComboBox(this);
    m_port->setObjectName(QStringLiteral("serialOutputNmeaPort"));
    grid->addWidget(m_port, 0, 1);
    auto *refresh = new QPushButton(tr("Refresh"), this);
    refresh->setObjectName(QStringLiteral("serialOutputNmeaRefresh"));
    grid->addWidget(refresh, 0, 2);

    grid->addWidget(new QLabel(tr("Baud"), this), 1, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("serialOutputNmeaBaud"));
    grid->addWidget(m_baud, 1, 1);

    grid->addWidget(new QLabel(tr("Update rate (Hz)"), this), 2, 0);
    m_rate = new QComboBox(this);
    m_rate->setObjectName(QStringLiteral("serialOutputNmeaRate"));
    grid->addWidget(m_rate, 2, 1);
    root->addLayout(grid);

    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(QStringLiteral("serialOutputNmeaToggle"));
    m_toggle->setMinimumWidth(90);
    root->addWidget(m_toggle, 0, Qt::AlignLeft);

    auto *statusFrame = new QFrame(this);
    statusFrame->setObjectName(QStringLiteral("serialOutputNmeaStatusPanel"));
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QVBoxLayout(statusFrame);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(4);
    m_status = new QLabel(statusFrame);
    m_status->setObjectName(QStringLiteral("serialOutputNmeaStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: #34d399;"));
    statusLayout->addWidget(m_status);
    statusLayout->addWidget(new QLabel(tr("Last sentence:"), statusFrame));
    m_lastSentence = new QLabel(statusFrame);
    m_lastSentence->setObjectName(
        QStringLiteral("serialOutputNmeaLastSentence"));
    m_lastSentence->setFont(QFontDatabase::systemFont(
        QFontDatabase::FixedFont));
    m_lastSentence->setWordWrap(true);
    m_lastSentence->setTextFormat(Qt::PlainText);
    statusLayout->addWidget(m_lastSentence);
    root->addWidget(statusFrame, 1);

    connect(m_port, &QComboBox::currentTextChanged,
            m_viewModel, &SerialOutputNMEAViewModel::setSelectedPort);
    connect(m_baud, &QComboBox::currentTextChanged, this,
            [this](const QString &text) {
        m_viewModel->setSelectedBaud(text.toInt());
    });
    connect(m_rate, &QComboBox::currentTextChanged, this,
            [this](const QString &text) {
        m_viewModel->setSelectedRateHz(text.toDouble());
    });
    connect(refresh, &QPushButton::clicked,
            m_viewModel, &SerialOutputNMEAViewModel::refreshPorts);
    connect(m_toggle, &QPushButton::clicked,
            m_viewModel, &SerialOutputNMEAViewModel::toggleConnection);
}

void SerialOutputNMEAWindow::syncUi()
{
    const QSignalBlocker blockPort(m_port);
    const QSignalBlocker blockBaud(m_baud);
    const QSignalBlocker blockRate(m_rate);

    const auto syncItems = [](QComboBox *combo, const QStringList &items) {
        bool different = combo->count() != items.size();
        for (int i = 0; !different && i < combo->count(); ++i) {
            different = combo->itemText(i) != items.at(i);
        }
        if (different) {
            combo->clear();
            combo->addItems(items);
        }
    };
    syncItems(m_port, m_viewModel->ports());
    m_port->setCurrentText(m_viewModel->selectedPort());

    QStringList baudItems;
    for (int baud : m_viewModel->bauds()) {
        baudItems.append(QString::number(baud));
    }
    syncItems(m_baud, baudItems);
    m_baud->setCurrentText(QString::number(m_viewModel->selectedBaud()));

    QStringList rateItems;
    for (double rate : m_viewModel->rates()) {
        rateItems.append(QString::number(rate, 'g', 3));
    }
    syncItems(m_rate, rateItems);
    m_rate->setCurrentText(QString::number(
        m_viewModel->selectedRateHz(), 'g', 3));

    m_toggle->setText(m_viewModel->connectButtonText());
    m_status->setText(m_viewModel->statusText());
    m_lastSentence->setText(m_viewModel->lastSentence());
}
