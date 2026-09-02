#include "ConfigHWBTView.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
QLabel *fieldLabel(const QString &text, const QString &objectName,
                   QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setFixedWidth(120);
    return label;
}
}

ConfigHWBTView::ConfigHWBTView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigHWBTViewModel(this))
{
    setObjectName(QStringLiteral("ConfigHWBTView"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(0);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("bluetoothSetupTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *intro = new QLabel(m_viewModel->Instructions(), this);
    intro->setObjectName(QStringLiteral("bluetoothSetupIntro"));
    intro->setWordWrap(true);
    intro->setContentsMargins(0, 0, 0, 12);
    root->addWidget(intro);

    auto *fields = new QWidget(this);
    fields->setObjectName(QStringLiteral("bluetoothSetupFields"));
    auto *grid = new QGridLayout(fields);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(8);

    m_portCombo = new QComboBox(fields);
    m_portCombo->setObjectName(QStringLiteral("bluetoothPortCombo"));
    grid->addWidget(fieldLabel(tr("Port"),
                               QStringLiteral("bluetoothPortLabel"),
                               fields), 0, 0);
    grid->addWidget(m_portCombo, 0, 1, 1, 2);

    m_nameEdit = new QLineEdit(fields);
    m_nameEdit->setObjectName(QStringLiteral("bluetoothNameEdit"));
    grid->addWidget(fieldLabel(tr("Name"),
                               QStringLiteral("bluetoothNameLabel"),
                               fields), 1, 0);
    grid->addWidget(m_nameEdit, 1, 1, 1, 2);

    m_baudCombo = new QComboBox(fields);
    m_baudCombo->setObjectName(QStringLiteral("bluetoothBaudCombo"));
    m_baudCombo->addItems(m_viewModel->Bauds());
    grid->addWidget(fieldLabel(tr("Baud"),
                               QStringLiteral("bluetoothBaudLabel"),
                               fields), 2, 0);
    grid->addWidget(m_baudCombo, 2, 1, 1, 2);

    m_pinEdit = new QLineEdit(fields);
    m_pinEdit->setObjectName(QStringLiteral("bluetoothPinEdit"));
    m_writeButton = new QPushButton(tr("Write"), fields);
    m_writeButton->setObjectName(QStringLiteral("bluetoothWriteButton"));
    grid->addWidget(fieldLabel(tr("PIN"),
                               QStringLiteral("bluetoothPinLabel"),
                               fields), 3, 0);
    grid->addWidget(m_pinEdit, 3, 1);
    grid->addWidget(m_writeButton, 3, 2);
    grid->setColumnStretch(1, 1);
    root->addWidget(fields);

    auto *outputFrame = new QFrame(this);
    outputFrame->setObjectName(QStringLiteral("bluetoothOutputFrame"));
    auto *outputLayout = new QVBoxLayout(outputFrame);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    m_outputEdit = new QTextEdit(outputFrame);
    m_outputEdit->setObjectName(QStringLiteral("bluetoothOutput"));
    m_outputEdit->setReadOnly(true);
    m_outputEdit->setAcceptRichText(false);
    m_outputEdit->setLineWrapMode(QTextEdit::WidgetWidth);
    m_outputEdit->setFrameShape(QFrame::NoFrame);
    m_outputEdit->setStyleSheet(QStringLiteral("background: transparent;"));
    outputLayout->addWidget(m_outputEdit);
    root->addSpacing(12);
    root->addWidget(outputFrame, 1);

    connect(m_portCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigHWBTViewModel::setSelectedPort);
    connect(m_nameEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigHWBTViewModel::setName);
    connect(m_baudCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigHWBTViewModel::setSelectedBaud);
    connect(m_pinEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigHWBTViewModel::setPin);
    connect(m_writeButton, &QPushButton::clicked,
            m_viewModel, &ConfigHWBTViewModel::Write);
    connect(m_viewModel, &ConfigHWBTViewModel::portsChanged,
            this, &ConfigHWBTView::syncPorts);
    connect(m_viewModel, &ConfigHWBTViewModel::settingsChanged,
            this, &ConfigHWBTView::syncSettings);
    connect(m_viewModel, &ConfigHWBTViewModel::outputChanged,
            this, &ConfigHWBTView::syncOutput);
    connect(m_viewModel, &ConfigHWBTViewModel::stateChanged,
            this, &ConfigHWBTView::syncState);
    connect(m_viewModel, &ConfigHWBTViewModel::programRequested,
            this, &ConfigHWBTView::programRequested);
    connect(m_viewModel, &ConfigHWBTViewModel::cancelRequested,
            this, &ConfigHWBTView::cancelRequested);

    syncPorts();
    syncSettings();
    syncOutput();
    syncState();
}

QSize ConfigHWBTView::sizeHint() const
{
    return QSize(800, 640);
}

void ConfigHWBTView::setPorts(
    const QStringList &ports, const QString &preferredPort)
{
    m_viewModel->setPorts(ports, preferredPort);
}

void ConfigHWBTView::setMainLinkConnected(bool connected)
{
    m_viewModel->setMainLinkConnected(connected);
}

void ConfigHWBTView::operationProgress(
    quint64 generation, const QString &line)
{
    m_viewModel->operationProgress(generation, line);
}

void ConfigHWBTView::operationFinished(
    quint64 generation, bool success, bool canceled)
{
    m_viewModel->operationFinished(generation, success, canceled);
}

void ConfigHWBTView::hideEvent(QHideEvent *event)
{
    m_viewModel->Cancel();
    QWidget::hideEvent(event);
}

void ConfigHWBTView::showEvent(QShowEvent *event)
{
    if (!m_viewModel->IsBusy()) {
        emit refreshPortsRequested();
    }
    QWidget::showEvent(event);
}

void ConfigHWBTView::syncPorts()
{
    const QSignalBlocker blocker(m_portCombo);
    m_portCombo->clear();
    m_portCombo->addItems(m_viewModel->Ports());
    m_portCombo->setCurrentText(m_viewModel->SelectedPort());
}

void ConfigHWBTView::syncSettings()
{
    const QSignalBlocker portBlocker(m_portCombo);
    const QSignalBlocker nameBlocker(m_nameEdit);
    const QSignalBlocker baudBlocker(m_baudCombo);
    const QSignalBlocker pinBlocker(m_pinEdit);
    m_portCombo->setCurrentText(m_viewModel->SelectedPort());
    m_nameEdit->setText(m_viewModel->Name());
    m_baudCombo->setCurrentText(m_viewModel->SelectedBaud());
    m_pinEdit->setText(m_viewModel->Pin());
}

void ConfigHWBTView::syncOutput()
{
    m_outputEdit->setPlainText(m_viewModel->Output());
    m_outputEdit->moveCursor(QTextCursor::End);
}

void ConfigHWBTView::syncState()
{
    const bool enabled = !m_viewModel->IsBusy();
    m_portCombo->setEnabled(enabled);
    m_nameEdit->setEnabled(enabled);
    m_baudCombo->setEnabled(enabled);
    m_pinEdit->setEnabled(enabled);
    m_writeButton->setEnabled(enabled);
}
