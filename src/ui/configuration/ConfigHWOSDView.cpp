#include "ConfigHWOSDView.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

ConfigHWOSDView::ConfigHWOSDView(QWidget *parent)
    : QWidget(parent), m_viewModel(new ConfigHWOSDViewModel(this))
{
    setObjectName(QStringLiteral("ConfigHWOSDView"));
    buildUi();
    syncState();
}

ConfigHWOSDView::~ConfigHWOSDView() = default;

QSize ConfigHWOSDView::sizeHint() const
{
    return QSize(800, 560); // MP10 design size
}

void ConfigHWOSDView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigHWOSDView, QWidget#hwOsdContent { background: #1A201D; color: #E6EDE9; }"
        "QLabel#hwOsdTitle { color: #E8E8E8; font-size: 16px; font-weight: bold; }"
        "QLabel#hwOsdIntro { color: #C8C8C8; }"
        "QLabel#hwOsdStatus { color: #34D399; }"
        "QLabel#hwOsdNote { color: #BBBBBB; }"
        "QPushButton { background: #161B18; color: #E6EDE9; border: 1px solid #2A322D;"
        " padding: 5px 12px; }"
        "QPushButton:disabled { color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("hwOsdScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("hwOsdContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16); // StackPanel Margin="16" Spacing="10"
    layout->setSpacing(10);

    m_title = new QLabel(ConfigHWOSDViewModel::Title(), content);
    m_title->setObjectName(QStringLiteral("hwOsdTitle"));
    layout->addWidget(m_title);

    m_intro = new QLabel(ConfigHWOSDViewModel::Intro(), content);
    m_intro->setObjectName(QStringLiteral("hwOsdIntro"));
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    auto *actionRow = new QWidget(content);
    actionRow->setObjectName(QStringLiteral("hwOsdActionRow"));
    auto *actionLayout = new QHBoxLayout(actionRow);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);
    m_enableButton = new QPushButton(ConfigHWOSDViewModel::EnableTelemetryText(), actionRow);
    m_enableButton->setObjectName(QStringLiteral("hwOsdEnableTelemetry"));
    m_enableButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    actionLayout->addWidget(m_enableButton, 0, Qt::AlignVCenter);
    m_status = new QLabel(actionRow);
    m_status->setObjectName(QStringLiteral("hwOsdStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    actionLayout->addWidget(m_status, 1, Qt::AlignVCenter);
    layout->addWidget(actionRow);

    m_note = new QLabel(ConfigHWOSDViewModel::Note(), content);
    m_note->setObjectName(QStringLiteral("hwOsdNote"));
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    layout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_enableButton, &QPushButton::clicked, this, &ConfigHWOSDView::enableTelemetry);
    connect(m_viewModel, &ConfigHWOSDViewModel::statusChanged, m_status, &QLabel::setText);
    connect(m_viewModel, &ConfigHWOSDViewModel::stateChanged, this, &ConfigHWOSDView::syncState);
    connect(m_viewModel, &ConfigHWOSDViewModel::writeParamsRequested, this,
            &ConfigHWOSDView::writeParamsRequested);
    m_status->setText(m_viewModel->Status());
}

void ConfigHWOSDView::syncState()
{
    m_enableButton->setEnabled(m_viewModel->CanEnableTelemetry());
}

void ConfigHWOSDView::setParameterSnapshot(const QList<ConfigFriendlyParameterValue> &parameters,
                                           int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigHWOSDView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigHWOSDView::parameterTargetChanged()
{
    m_viewModel->parameterTargetChanged();
}

void ConfigHWOSDView::enableTelemetry()
{
    m_viewModel->EnableTelemetry(); // a receiver may delete this page; nothing follows
}

void ConfigHWOSDView::parameterBatchSubmitted(int componentId, qulonglong batchId)
{
    m_viewModel->parameterBatchSubmitted(componentId, batchId);
}

void ConfigHWOSDView::parameterBatchProgress(qulonglong batchId, int completed, int total,
                                             int succeeded, int failed)
{
    m_viewModel->parameterBatchProgress(batchId, completed, total, succeeded, failed);
}

void ConfigHWOSDView::parameterWriteFailed(qulonglong batchId, int componentId,
                                           const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(batchId, componentId, name, reason);
}

void ConfigHWOSDView::parameterBatchCompleted(qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigHWOSDView::parameterBatchCancelled(qulonglong batchId)
{
    m_viewModel->parameterBatchCancelled(batchId);
}

void ConfigHWOSDView::parameterWriteSubmissionFailed(const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(reason);
}
