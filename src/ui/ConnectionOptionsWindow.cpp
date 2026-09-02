#include "ConnectionOptionsWindow.h"

#include "ConnectionOptionsViewModel.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>

ConnectionOptionsWindow::ConnectionOptionsWindow(QWidget *parent)
    : ConnectionOptionsWindow(new ConnectionOptionsViewModel, parent)
{}

ConnectionOptionsWindow::ConnectionOptionsWindow(
    const QStringList &serialPorts, QWidget *parent)
    : ConnectionOptionsWindow(
          new ConnectionOptionsViewModel(serialPorts), parent)
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
        connect(window, &ConnectionOptionsWindow::connectRequested,
                resolvedOwner,
                [resolvedOwner](const QString &connection, int baud) {
                    QMetaObject::invokeMethod(
                        resolvedOwner, "openAdditionalConnection",
                        Qt::DirectConnection,
                        Q_ARG(QString, connection), Q_ARG(int, baud));
                });
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
    setWindowTitle(tr("Connections"));
    setModal(false);
    setFixedSize(228, 75);
    setSizeGripEnabled(false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);

    m_serialPortCombo = new QComboBox(this);
    m_serialPortCombo->setObjectName(QStringLiteral("CMB_serialport"));
    m_serialPortCombo->setGeometry(13, 13, 121, 21);

    m_connectButton = new QPushButton(tr("Connect"), this);
    m_connectButton->setObjectName(QStringLiteral("BUT_connect"));
    m_connectButton->setGeometry(140, 13, 75, 23);

    m_baudCombo = new QComboBox(this);
    m_baudCombo->setObjectName(QStringLiteral("CMB_baudrate"));
    m_baudCombo->setGeometry(13, 40, 121, 21);

    connect(m_connectButton, &QPushButton::clicked,
            this, &ConnectionOptionsWindow::BUT_connect_Click);
}

void ConnectionOptionsWindow::bindViewModel()
{
    m_serialPortCombo->addItems(m_viewModel->Connections());
    for (int baud : m_viewModel->Bauds()) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_serialPortCombo->setCurrentText(m_viewModel->SelectedConnection());
    m_baudCombo->setCurrentIndex(
        m_baudCombo->findData(m_viewModel->SelectedBaud()));
    m_baudCombo->setEnabled(m_viewModel->BaudEnabled());

    connect(m_serialPortCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConnectionOptionsViewModel::setSelectedConnection);
    connect(m_baudCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index >= 0) {
                    m_viewModel->setSelectedBaud(
                        m_baudCombo->itemData(index).toInt());
                }
            });
    connect(m_viewModel, &ConnectionOptionsViewModel::BaudEnabledChanged,
            m_baudCombo, &QWidget::setEnabled);
    connect(m_viewModel, &ConnectionOptionsViewModel::connectRequested,
            this, &ConnectionOptionsWindow::connectRequested);
}

void ConnectionOptionsWindow::BUT_connect_Click()
{
    m_viewModel->Connect();
}
