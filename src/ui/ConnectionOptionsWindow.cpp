#include "ConnectionOptionsWindow.h"

#include "ConnectionOptionsViewModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

ConnectionOptionsWindow::ConnectionOptionsWindow(QWidget *parent)
    : ConnectionOptionsWindow(new ConnectionOptionsViewModel, parent)
{}

ConnectionOptionsWindow::ConnectionOptionsWindow(
    ConnectionOptionsViewModel *viewModel, QWidget *parent)
    : QDialog(parent), m_viewModel(viewModel)
{
    Q_ASSERT(m_viewModel);
    m_viewModel->setParent(this);
    buildUi();
    bindViewModel();
}

ConnectionOptionsWindow *ConnectionOptionsWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new ConnectionOptionsWindow(resolvedOwner);
    window->setAttribute(Qt::WA_DeleteOnClose);
    if (resolvedOwner) {
        connect(window, &ConnectionOptionsWindow::settingsApplied,
                resolvedOwner,
                [resolvedOwner](int baud, bool heartbeat, int gcsSysid) {
                    QMetaObject::invokeMethod(
                        resolvedOwner, "applyConnectionOptions",
                        Qt::DirectConnection, Q_ARG(int, baud),
                        Q_ARG(bool, heartbeat), Q_ARG(int, gcsSysid));
                });
    }
    if (resolvedOwner) {
        window->move(resolvedOwner->frameGeometry().center()
                     - window->rect().center());
    }
    window->show();
    return window;
}

ConnectionOptionsViewModel *ConnectionOptionsWindow::viewModel() const
{
    return m_viewModel;
}

void ConnectionOptionsWindow::buildUi()
{
    setObjectName(QStringLiteral("ConnectionOptions"));
    setWindowTitle(tr("Connection Options"));
    setModal(false);
    setFixedSize(340, 230);
    setSizeGripEnabled(false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);

    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(8);
    layout->setColumnStretch(1, 1);
    layout->setRowStretch(3, 1);

    auto *baudLabel = new QLabel(tr("Default baud rate"), this);
    baudLabel->setObjectName(QStringLiteral("LBL_defaultBaud"));
    m_baudCombo = new QComboBox(this);
    m_baudCombo->setObjectName(QStringLiteral("CMB_baudrate"));
    layout->addWidget(baudLabel, 0, 0);
    layout->addWidget(m_baudCombo, 0, 1);

    auto *gcsLabel = new QLabel(tr("GCS system id"), this);
    gcsLabel->setObjectName(QStringLiteral("LBL_gcsid"));
    m_gcsSysidSpin = new QSpinBox(this);
    m_gcsSysidSpin->setObjectName(QStringLiteral("NUM_gcsid"));
    m_gcsSysidSpin->setRange(1, 255);
    m_gcsSysidSpin->setSingleStep(1);
    layout->addWidget(gcsLabel, 1, 0);
    layout->addWidget(m_gcsSysidSpin, 1, 1);

    m_heartbeatCheck = new QCheckBox(tr("Send GCS heartbeat"), this);
    m_heartbeatCheck->setObjectName(QStringLiteral("CHK_GCSheartbeat"));
    layout->addWidget(m_heartbeatCheck, 2, 0, 1, 2);

    auto *gcsRestartNote = new QLabel(
        tr("GCS system id changes apply after restart."), this);
    gcsRestartNote->setObjectName(QStringLiteral("LBL_gcsRestartNote"));
    gcsRestartNote->setWordWrap(true);
    gcsRestartNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(gcsRestartNote, 3, 0, 1, 2);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("LBL_status"));
    m_saveButton = new QPushButton(tr("Save"), this);
    m_saveButton->setObjectName(QStringLiteral("BUT_save"));
    m_saveButton->setMinimumWidth(80);
    m_closeButton = new QPushButton(tr("Close"), this);
    m_closeButton->setObjectName(QStringLiteral("BUT_close"));
    m_closeButton->setMinimumWidth(80);
    buttons->addStretch();
    buttons->addWidget(m_statusLabel);
    buttons->addWidget(m_saveButton);
    buttons->addWidget(m_closeButton);
    layout->addLayout(buttons, 4, 0, 1, 2);

    connect(m_saveButton, &QPushButton::clicked,
            m_viewModel, &ConnectionOptionsViewModel::Apply);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
}

void ConnectionOptionsWindow::bindViewModel()
{
    for (int baud : m_viewModel->Bauds()) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setCurrentIndex(
        m_baudCombo->findData(m_viewModel->SelectedBaud()));
    m_gcsSysidSpin->setValue(m_viewModel->GcsSysid());
    m_heartbeatCheck->setChecked(m_viewModel->SendGcsHeartbeat());
    m_statusLabel->setText(m_viewModel->Status());

    connect(m_baudCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index >= 0) {
                    m_viewModel->setSelectedBaud(
                        m_baudCombo->itemData(index).toInt());
                }
            });
    connect(m_gcsSysidSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &ConnectionOptionsViewModel::setGcsSysid);
    connect(m_heartbeatCheck, &QCheckBox::toggled,
            m_viewModel, &ConnectionOptionsViewModel::setSendGcsHeartbeat);
    connect(m_viewModel, &ConnectionOptionsViewModel::StatusChanged,
            m_statusLabel, &QLabel::setText);
    connect(m_viewModel, &ConnectionOptionsViewModel::GcsSysidChanged,
            m_gcsSysidSpin, &QSpinBox::setValue);
    connect(m_viewModel, &ConnectionOptionsViewModel::settingsApplied,
            this, &ConnectionOptionsWindow::settingsApplied);
}
