#include "SerialOutputCotWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

SerialOutputCotWindow::SerialOutputCotWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    CotOutputService::TransportFactory factory =
        std::move(dependencies.transportFactory);
    if (!factory) {
        factory = CotOutputService::ProductionTransportFactory();
    }
    CotOutputService::Sender sender = std::move(dependencies.sender);
    if (!sender) {
        sender = CotOutputService::ProductionSender();
    }
    m_service = new CotOutputService(
        std::move(factory), std::move(sender), std::move(dependencies.clock),
        this);
    m_viewModel = new SerialOutputCotViewModel(
        m_service, std::move(dependencies.viewModel), this);
    buildUi(owner);
    connect(m_viewModel, &SerialOutputCotViewModel::changed,
            this, &SerialOutputCotWindow::syncUi);
    syncUi();
}

SerialOutputCotWindow::~SerialOutputCotWindow()
{
    commitGridEdit();
    m_viewModel->saveSettings();
    m_viewModel->stop();
}

void SerialOutputCotWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SerialOutputCotWindow"));
    setWindowTitle(tr("Cursor-on-Target / TAK Output"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);

    if (owner) {
        move(owner->geometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(9);

    auto *header = new QLabel(tr("Cursor-on-Target / TAK Output"), this);
    header->setObjectName(QStringLiteral("serialOutputCotHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Emits CoT 2.0 XML for every MAVLink system through UDP multicast/client, "
        "TCP client/host, UDP host, or a serial port."), this);
    description->setObjectName(QStringLiteral("serialOutputCotDescription"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(7);
    grid->setColumnMinimumWidth(0, 135);
    grid->setColumnStretch(1, 1);

    grid->addWidget(new QLabel(tr("Endpoint"), this), 0, 0);
    m_endpoint = new QComboBox(this);
    m_endpoint->setObjectName(QStringLiteral("serialOutputCotEndpoint"));
    grid->addWidget(m_endpoint, 0, 1);
    auto *refreshEndpoints = new QPushButton(tr("Refresh"), this);
    refreshEndpoints->setObjectName(QStringLiteral("serialOutputCotRefresh"));
    grid->addWidget(refreshEndpoints, 0, 2);

    grid->addWidget(new QLabel(tr("Host / listen IP"), this), 1, 0);
    m_host = new QLineEdit(this);
    m_host->setObjectName(QStringLiteral("serialOutputCotHost"));
    m_host->setMaxLength(1024);
    grid->addWidget(m_host, 1, 1, 1, 2);

    grid->addWidget(new QLabel(tr("Network port"), this), 2, 0);
    m_port = new QSpinBox(this);
    m_port->setObjectName(QStringLiteral("serialOutputCotPort"));
    m_port->setRange(1, 65535);
    grid->addWidget(m_port, 2, 1);

    grid->addWidget(new QLabel(tr("Serial baud"), this), 3, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("serialOutputCotBaud"));
    grid->addWidget(m_baud, 3, 1);

    grid->addWidget(new QLabel(tr("Update (seconds)"), this), 4, 0);
    m_update = new QDoubleSpinBox(this);
    m_update->setObjectName(QStringLiteral("serialOutputCotUpdate"));
    m_update->setRange(0.1, 3600.0);
    m_update->setSingleStep(0.5);
    m_update->setDecimals(1);
    grid->addWidget(m_update, 4, 1);

    grid->addWidget(new QLabel(tr("Event type"), this), 5, 0);
    m_eventType = new QLineEdit(this);
    m_eventType->setObjectName(QStringLiteral("serialOutputCotEventType"));
    m_eventType->setMaxLength(1024);
    grid->addWidget(m_eventType, 5, 1, 1, 2);

    grid->addWidget(new QLabel(tr("UID prefix"), this), 6, 0);
    m_uidPrefix = new QLineEdit(this);
    m_uidPrefix->setObjectName(QStringLiteral("serialOutputCotUidPrefix"));
    m_uidPrefix->setMaxLength(1024);
    grid->addWidget(m_uidPrefix, 6, 1);

    grid->addWidget(new QLabel(tr("Optional callsign"), this), 7, 0);
    m_callsign = new QLineEdit(this);
    m_callsign->setObjectName(QStringLiteral("serialOutputCotCallsign"));
    m_callsign->setMaxLength(1024);
    grid->addWidget(m_callsign, 7, 1, 1, 2);
    root->addLayout(grid);

    m_advanced = new QCheckBox(tr("Advanced identity fields"), this);
    m_advanced->setObjectName(QStringLiteral("serialOutputCotAdvanced"));
    root->addWidget(m_advanced);

    auto *identityButtons = new QHBoxLayout;
    m_refreshSystems = new QPushButton(tr("Refresh systems"), this);
    m_refreshSystems->setObjectName(QStringLiteral("serialOutputCotRefreshSystems"));
    identityButtons->addWidget(m_refreshSystems);
    m_addIdentity = new QPushButton(tr("Add identity"), this);
    m_addIdentity->setObjectName(QStringLiteral("serialOutputCotAddIdentity"));
    identityButtons->addWidget(m_addIdentity);
    m_removeIdentity = new QPushButton(tr("Remove selected"), this);
    m_removeIdentity->setObjectName(QStringLiteral("serialOutputCotRemoveIdentity"));
    identityButtons->addWidget(m_removeIdentity);
    identityButtons->addStretch(1);
    root->addLayout(identityButtons);

    m_identities = new QTableView(this);
    m_identities->setObjectName(QStringLiteral("serialOutputCotIdentities"));
    m_identities->setModel(m_viewModel->identityModel());
    m_identities->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_identities->setSelectionMode(QAbstractItemView::SingleSelection);
    m_identities->setAlternatingRowColors(true);
    m_identities->horizontalHeader()->setStretchLastSection(true);
    m_identities->horizontalHeader()->setSectionResizeMode(
        CotIdentityModel::SystemIdColumn, QHeaderView::ResizeToContents);
    m_identities->horizontalHeader()->setSectionResizeMode(
        CotIdentityModel::TakvColumn, QHeaderView::ResizeToContents);
    root->addWidget(m_identities, 2);

    auto *commands = new QHBoxLayout;
    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(QStringLiteral("serialOutputCotToggle"));
    m_toggle->setMinimumWidth(100);
    commands->addWidget(m_toggle);
    m_indent = new QCheckBox(tr("Indent XML"), this);
    m_indent->setObjectName(QStringLiteral("serialOutputCotIndent"));
    commands->addWidget(m_indent);
    commands->addStretch(1);
    root->addLayout(commands);

    auto *statusFrame = new QFrame(this);
    statusFrame->setObjectName(QStringLiteral("serialOutputCotStatusPanel"));
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QVBoxLayout(statusFrame);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(4);
    statusLayout->addWidget(new QLabel(tr("Status"), statusFrame));
    m_status = new QLabel(statusFrame);
    m_status->setObjectName(QStringLiteral("serialOutputCotStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: #34d399;"));
    statusLayout->addWidget(m_status);
    statusLayout->addWidget(new QLabel(tr("Last event"), statusFrame));
    m_lastEvent = new QPlainTextEdit(statusFrame);
    m_lastEvent->setObjectName(QStringLiteral("serialOutputCotLastEvent"));
    m_lastEvent->setReadOnly(true);
    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixedFont.setPointSize(11);
    m_lastEvent->setFont(fixedFont);
    m_lastEvent->setMinimumHeight(110);
    statusLayout->addWidget(m_lastEvent, 1);
    root->addWidget(statusFrame, 1);

    connect(m_endpoint, &QComboBox::currentTextChanged,
            m_viewModel, &SerialOutputCotViewModel::setSelectedEndpoint);
    connect(refreshEndpoints, &QPushButton::clicked,
            m_viewModel, &SerialOutputCotViewModel::refreshEndpoints);
    connect(m_host, &QLineEdit::textChanged,
            m_viewModel, &SerialOutputCotViewModel::setHost);
    connect(m_port, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &SerialOutputCotViewModel::setPort);
    connect(m_baud, &QComboBox::currentTextChanged, this,
            [this](const QString &text) {
        m_viewModel->setBaud(text.toInt());
    });
    connect(m_update, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            m_viewModel, &SerialOutputCotViewModel::setUpdateSeconds);
    connect(m_eventType, &QLineEdit::textChanged,
            m_viewModel, &SerialOutputCotViewModel::setEventType);
    connect(m_uidPrefix, &QLineEdit::textChanged,
            m_viewModel, &SerialOutputCotViewModel::setUidPrefix);
    connect(m_callsign, &QLineEdit::textChanged,
            m_viewModel, &SerialOutputCotViewModel::setCallsign);
    connect(m_advanced, &QCheckBox::toggled,
            m_viewModel, &SerialOutputCotViewModel::setAdvancedMode);
    connect(m_refreshSystems, &QPushButton::clicked,
            m_viewModel, &SerialOutputCotViewModel::refreshIdentitySystems);
    connect(m_addIdentity, &QPushButton::clicked,
            m_viewModel, &SerialOutputCotViewModel::addIdentity);
    connect(m_removeIdentity, &QPushButton::clicked, this, [this]() {
        m_viewModel->removeIdentity(m_identities->currentIndex().row());
    });
    connect(m_toggle, &QPushButton::clicked, this, [this]() {
        commitGridEdit();
        m_viewModel->toggleConnection();
    });
    connect(m_indent, &QCheckBox::toggled,
            m_viewModel, &SerialOutputCotViewModel::setIndentXml);
}

void SerialOutputCotWindow::syncUi()
{
    const QSignalBlocker endpointBlock(m_endpoint);
    const QSignalBlocker hostBlock(m_host);
    const QSignalBlocker portBlock(m_port);
    const QSignalBlocker baudBlock(m_baud);
    const QSignalBlocker updateBlock(m_update);
    const QSignalBlocker eventBlock(m_eventType);
    const QSignalBlocker uidBlock(m_uidPrefix);
    const QSignalBlocker callsignBlock(m_callsign);
    const QSignalBlocker advancedBlock(m_advanced);
    const QSignalBlocker indentBlock(m_indent);

    const auto syncItems = [](QComboBox *combo, const QStringList &items) {
        bool different = combo->count() != items.size();
        for (int index = 0; !different && index < combo->count(); ++index) {
            different = combo->itemText(index) != items.at(index);
        }
        if (different) {
            combo->clear();
            combo->addItems(items);
        }
    };
    syncItems(m_endpoint, m_viewModel->endpoints());
    m_endpoint->setCurrentText(m_viewModel->selectedEndpoint());
    m_host->setText(m_viewModel->host());
    m_port->setValue(qBound(1, m_viewModel->port(), 65535));

    QStringList bauds;
    for (int baud : m_viewModel->bauds()) {
        bauds.append(QString::number(baud));
    }
    syncItems(m_baud, bauds);
    m_baud->setCurrentText(QString::number(m_viewModel->baud()));
    m_update->setValue(m_viewModel->updateSeconds());
    m_eventType->setText(m_viewModel->eventType());
    m_uidPrefix->setText(m_viewModel->uidPrefix());
    m_callsign->setText(m_viewModel->callsign());
    m_advanced->setChecked(m_viewModel->advancedMode());
    m_indent->setChecked(m_viewModel->indentXml());

    const bool running = m_viewModel->isRunning();
    m_endpoint->setEnabled(!running);
    m_host->setEnabled(!running && m_viewModel->isNetworkEndpoint());
    m_port->setEnabled(!running && m_viewModel->isNetworkEndpoint());
    m_baud->setEnabled(!running && m_viewModel->isSerialEndpoint());
    m_toggle->setText(m_viewModel->connectButtonText());
    m_status->setText(m_viewModel->statusText());
    if (m_lastEvent->toPlainText() != m_viewModel->lastEvent()) {
        m_lastEvent->setPlainText(m_viewModel->lastEvent());
    }
    for (int column = CotIdentityModel::TakvColumn;
         column < CotIdentityModel::ColumnCount; ++column) {
        m_identities->setColumnHidden(column, !m_viewModel->advancedMode());
    }
}

void SerialOutputCotWindow::commitGridEdit()
{
    if (QWidget *editor = QApplication::focusWidget()) {
        if (editor == m_identities || m_identities->isAncestorOf(editor)) {
            editor->clearFocus();
        }
    }
}

void SerialOutputCotWindow::closeEvent(QCloseEvent *event)
{
    commitGridEdit();
    m_viewModel->saveSettings();
    m_viewModel->stop();
    QWidget::closeEvent(event);
}
