#include "ConnectionOptionsWindow.h"

#include "ConnectionOptionsViewModel.h"

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>

namespace {
const char kConnectionOptionsStyle[] = R"(
ConnectionOptionsWindow {
    background: #1A201D;
    color: #D8D8D8;
}
ConnectionOptionsWindow QLabel,
ConnectionOptionsWindow QCheckBox {
    color: #E6EDE9;
}
ConnectionOptionsWindow QCheckBox#SendGcsHeartbeat {
    color: whitesmoke;
}
ConnectionOptionsWindow QComboBox,
ConnectionOptionsWindow QSpinBox {
    min-height: 28px;
    padding: 2px 7px;
    color: #D8D8D8;
    background: #161B18;
    border: 1px solid #2A322D;
    border-radius: 3px;
}
ConnectionOptionsWindow QPushButton {
    min-height: 28px;
    padding: 4px 8px;
    font-weight: 600;
    color: #06251A;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #34D399, stop:1 #10B981);
    border: 0;
    border-radius: 3px;
}
ConnectionOptionsWindow QPushButton:hover {
    background: #10B981;
}
ConnectionOptionsWindow QPushButton:pressed {
    background: #10B981;
}
ConnectionOptionsWindow QLabel#Status {
    color: #99AADD;
}
)";
}

ConnectionOptionsWindow::ConnectionOptionsWindow(QWidget *parent)
    : QDialog(parent),
      m_viewModel(new ConnectionOptionsViewModel(this))
{
    buildUi();
    bindViewModel();
}

ConnectionOptionsWindow::ConnectionOptionsWindow(QSettings *settings,
                                                 QWidget *parent)
    : QDialog(parent),
      m_viewModel(new ConnectionOptionsViewModel(settings, this))
{
    buildUi();
    bindViewModel();
}

ConnectionOptionsWindow *ConnectionOptionsWindow::OpenWindow(
    QWidget *owner, QSettings *settings)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = settings
        ? new ConnectionOptionsWindow(settings, resolvedOwner)
        : new ConnectionOptionsWindow(resolvedOwner);
    window->setAttribute(Qt::WA_DeleteOnClose);
    if (resolvedOwner) {
        connect(window, &ConnectionOptionsWindow::settingsApplied,
                resolvedOwner,
                [resolvedOwner](int baud, bool sendHeartbeat,
                                int gcsSystemId) {
                    QMetaObject::invokeMethod(
                        resolvedOwner, "applyConnectionOptions",
                        Qt::DirectConnection,
                        Q_ARG(int, baud),
                        Q_ARG(bool, sendHeartbeat),
                        Q_ARG(int, gcsSystemId));
                });
    }
    window->show();
    return window;
}

ConnectionOptionsViewModel *ConnectionOptionsWindow::viewModel() const
{
    return m_viewModel;
}

void ConnectionOptionsWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_centered) {
        return;
    }

    QRect available;
    if (QWidget *owner = parentWidget()) {
        available = owner->frameGeometry();
    } else if (QScreen *screen = this->screen()) {
        available = screen->availableGeometry();
    }
    if (available.isValid()) {
        move(available.center() - rect().center());
    }
    m_centered = true;
}

void ConnectionOptionsWindow::OnApply()
{
    if (m_baudCombo->currentIndex() >= 0) {
        m_viewModel->setSelectedBaud(m_baudCombo->currentData().toInt());
    }
    m_viewModel->setGcsSysid(m_gcsSystemId->value());
    m_viewModel->setSendGcsHeartbeat(m_sendHeartbeat->isChecked());
    m_viewModel->Apply();
}

void ConnectionOptionsWindow::OnClose()
{
    close();
}

void ConnectionOptionsWindow::buildUi()
{
    setObjectName(QStringLiteral("ConnectionOptionsWindow"));
    setWindowTitle(tr("Connection Options"));
    setModal(false);
    setFixedSize(340, 230);
    setSizeGripEnabled(false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    setStyleSheet(QString::fromLatin1(kConnectionOptionsStyle));

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(16, 16, 16, 16);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    auto *baudLabel = new QLabel(tr("Default baud rate"), this);
    baudLabel->setObjectName(QStringLiteral("DefaultBaudRateLabel"));
    m_baudCombo = new QComboBox(this);
    m_baudCombo->setObjectName(QStringLiteral("SelectedBaud"));
    m_baudCombo->setSizePolicy(QSizePolicy::Expanding,
                               QSizePolicy::Preferred);
    for (int baud : m_viewModel->Bauds()) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    grid->addWidget(baudLabel, 0, 0);
    grid->addWidget(m_baudCombo, 0, 1);

    auto *systemIdLabel = new QLabel(tr("GCS system id"), this);
    systemIdLabel->setObjectName(QStringLiteral("GcsSystemIdLabel"));
    m_gcsSystemId = new QSpinBox(this);
    m_gcsSystemId->setObjectName(QStringLiteral("GcsSysid"));
    m_gcsSystemId->setRange(1, 255);
    m_gcsSystemId->setSingleStep(1);
    grid->addWidget(systemIdLabel, 1, 0);
    grid->addWidget(m_gcsSystemId, 1, 1);

    auto *heartbeatHost = new QWidget(this);
    heartbeatHost->setObjectName(QStringLiteral("SendGcsHeartbeatHost"));
    auto *heartbeatLayout = new QHBoxLayout(heartbeatHost);
    heartbeatLayout->setContentsMargins(0, 8, 0, 0);
    heartbeatLayout->setSpacing(0);
    m_sendHeartbeat = new QCheckBox(tr("Send GCS heartbeat"),
                                    heartbeatHost);
    m_sendHeartbeat->setObjectName(QStringLiteral("SendGcsHeartbeat"));
    heartbeatLayout->addWidget(m_sendHeartbeat);
    grid->addWidget(heartbeatHost, 2, 0, 1, 2);

    grid->setRowStretch(3, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("Status"));
    buttons->addWidget(m_status);
    m_saveButton = new QPushButton(tr("Save"), this);
    m_saveButton->setObjectName(QStringLiteral("ApplyCommand"));
    m_saveButton->setMinimumWidth(80);
    buttons->addWidget(m_saveButton);
    m_closeButton = new QPushButton(tr("Close"), this);
    m_closeButton->setObjectName(QStringLiteral("OnClose"));
    m_closeButton->setMinimumWidth(80);
    buttons->addWidget(m_closeButton);
    grid->addLayout(buttons, 4, 0, 1, 2, Qt::AlignRight);

    connect(m_saveButton, &QPushButton::clicked,
            this, &ConnectionOptionsWindow::OnApply);
    connect(m_closeButton, &QPushButton::clicked,
            this, &ConnectionOptionsWindow::OnClose);
}

void ConnectionOptionsWindow::bindViewModel()
{
    const int baudIndex = m_baudCombo->findData(m_viewModel->SelectedBaud());
    m_baudCombo->setCurrentIndex(baudIndex);
    m_gcsSystemId->setValue(m_viewModel->GcsSysid());
    m_sendHeartbeat->setChecked(m_viewModel->SendGcsHeartbeat());
    m_status->setText(m_viewModel->Status());

    connect(m_viewModel, &ConnectionOptionsViewModel::StatusChanged,
            m_status, &QLabel::setText);
    connect(m_viewModel, &ConnectionOptionsViewModel::settingsApplied,
            this, &ConnectionOptionsWindow::settingsApplied);
}
