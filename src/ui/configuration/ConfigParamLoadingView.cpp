#include "ConfigParamLoadingView.h"

#include "ConfigParamLoadingViewModel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

ConfigParamLoadingView::ConfigParamLoadingView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigParamLoadingViewModel(this))
{
    setObjectName(QStringLiteral("ConfigParamLoadingView"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setAlignment(Qt::AlignCenter);

    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("parameterLoadingPanel"));
    panel->setFixedWidth(320);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(14);

    m_statusLabel = new QLabel(panel);
    m_statusLabel->setObjectName(QStringLiteral("parameterLoadingStatus"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    panelLayout->addWidget(m_statusLabel);

    m_progress = new QProgressBar(panel);
    m_progress->setObjectName(QStringLiteral("parameterLoadingProgress"));
    m_progress->setRange(0, 100);
    m_progress->setFixedHeight(20);
    m_progress->setTextVisible(false);
    panelLayout->addWidget(m_progress);

    m_countLabel = new QLabel(panel);
    m_countLabel->setObjectName(QStringLiteral("parameterLoadingCount"));
    m_countLabel->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(m_countLabel);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    actions->setAlignment(Qt::AlignCenter);
    auto *stopButton = new QPushButton(tr("Stop Loading"), panel);
    stopButton->setObjectName(QStringLiteral("stopParameterLoadingButton"));
    stopButton->setProperty("loadingAction", QVariant(true));
    auto *retryButton = new QPushButton(tr("Retry Now"), panel);
    retryButton->setObjectName(QStringLiteral("retryParameterLoadingButton"));
    retryButton->setProperty("loadingAction", QVariant(true));
    actions->addWidget(stopButton);
    actions->addWidget(retryButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel);

    connect(m_viewModel, &ConfigParamLoadingViewModel::stateChanged,
            this, &ConfigParamLoadingView::refresh);
    connect(stopButton, &QPushButton::clicked,
            this, &ConfigParamLoadingView::stopLoadingRequested);
    connect(retryButton, &QPushButton::clicked,
            this, &ConfigParamLoadingView::retryLoadingRequested);
    refresh();
}

ConfigParamLoadingViewModel *ConfigParamLoadingView::viewModel() const
{
    return m_viewModel;
}

void ConfigParamLoadingView::refresh()
{
    m_statusLabel->setText(m_viewModel->status());
    m_statusLabel->setToolTip(m_viewModel->status());
    m_progress->setValue(m_viewModel->progressPercent());
    m_countLabel->setText(m_viewModel->count());
}
