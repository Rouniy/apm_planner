#include "AddConnectionWindow.h"

#include "AddConnectionViewModel.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>

AddConnectionWindow::AddConnectionWindow(QWidget *parent)
    : AddConnectionWindow(new AddConnectionViewModel, parent)
{}

AddConnectionWindow::AddConnectionWindow(
    const QStringList &serialPorts, QWidget *parent)
    : AddConnectionWindow(new AddConnectionViewModel(serialPorts), parent)
{}

AddConnectionWindow::AddConnectionWindow(
    AddConnectionViewModel *viewModel, QWidget *parent)
    : QDialog(parent), m_viewModel(viewModel)
{
    Q_ASSERT(m_viewModel);
    m_viewModel->setParent(this);
    buildUi();
    bindViewModel();
}

AddConnectionWindow *AddConnectionWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new AddConnectionWindow(resolvedOwner);
    window->setAttribute(Qt::WA_DeleteOnClose);
    if (resolvedOwner) {
        connect(window, &AddConnectionWindow::connectRequested,
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

AddConnectionViewModel *AddConnectionWindow::viewModel() const
{
    return m_viewModel;
}

void AddConnectionWindow::buildUi()
{
    setObjectName(QStringLiteral("AddConnection"));
    setWindowTitle(tr("Add Connection"));
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
            this, &AddConnectionWindow::BUT_connect_Click);
}

void AddConnectionWindow::bindViewModel()
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
            m_viewModel, &AddConnectionViewModel::setSelectedConnection);
    connect(m_baudCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index >= 0) {
                    m_viewModel->setSelectedBaud(
                        m_baudCombo->itemData(index).toInt());
                }
            });
    connect(m_viewModel, &AddConnectionViewModel::BaudEnabledChanged,
            m_baudCombo, &QWidget::setEnabled);
    connect(m_viewModel, &AddConnectionViewModel::connectRequested,
            this, &AddConnectionWindow::connectRequested);
}

void AddConnectionWindow::BUT_connect_Click()
{
    m_viewModel->Connect();
}
