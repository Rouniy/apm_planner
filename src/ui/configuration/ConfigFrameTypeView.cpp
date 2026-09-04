#include "ConfigFrameTypeView.h"

#include <QButtonGroup>
#include <QFrame>
#include <QLabel>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

ConfigFrameTypeView::ConfigFrameTypeView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigFrameTypeViewModel(this))
{
    setObjectName(QStringLiteral("ConfigFrameTypeView"));
    buildUi();
    connect(m_viewModel, &ConfigFrameTypeViewModel::stateChanged,
            this, &ConfigFrameTypeView::syncState);
    connect(m_viewModel, &ConfigFrameTypeViewModel::writeRequested,
            this, &ConfigFrameTypeView::writeRequested);
    connect(m_viewModel, &ConfigFrameTypeViewModel::refreshRequested,
            this, &ConfigFrameTypeView::refreshRequested);
    syncState();
}

ConfigFrameTypeView::~ConfigFrameTypeView() = default;

QSize ConfigFrameTypeView::sizeHint() const
{
    return QSize(520, 480);
}

void ConfigFrameTypeView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigFrameTypeView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigFrameTypeView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigFrameTypeView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigFrameTypeView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigFrameTypeView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigFrameTypeView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigFrameTypeView::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    m_viewModel->parameterWriteCancelled(batchId, componentId, name);
}

void ConfigFrameTypeView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigFrameTypeView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigFrameTypeView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigFrameTypeView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigFrameTypeView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#frameLegacyTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#frameLegacyStatus { color: #34D399; }"
        "QFrame#frameLegacyOptions { background: #202623;"
        " border: 1px solid #303A35; }"
        "QRadioButton { color: #DDDDDD; spacing: 7px; }"
        "QRadioButton:disabled { color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("frameLegacyScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("frameLegacyContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("Frame Type"), content);
    title->setObjectName(QStringLiteral("frameLegacyTitle"));
    layout->addWidget(title);
    auto *intro = new QLabel(
        tr("Select the airframe configuration. The selection is written "
           "to the FRAME parameter."), content);
    intro->setObjectName(QStringLiteral("frameLegacyIntro"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *optionsPanel = new QFrame(content);
    optionsPanel->setObjectName(QStringLiteral("frameLegacyOptions"));
    auto *optionsLayout = new QVBoxLayout(optionsPanel);
    optionsLayout->setContentsMargins(12, 12, 12, 12);
    optionsLayout->setSpacing(10);
    auto *group = new QButtonGroup(optionsPanel);
    group->setExclusive(true);
    const QList<ParamOption> options = ConfigFrameTypeViewModel::FrameOptions();
    const QStringList objectSuffixes = {
        QStringLiteral("Plus"), QStringLiteral("X"),
        QStringLiteral("V"), QStringLiteral("H"),
        QStringLiteral("Y6B"), QStringLiteral("VTail")
    };
    for (int index = 0; index < options.size(); ++index) {
        const ParamOption &option = options.at(index);
        auto *radio = new QRadioButton(option.text, optionsPanel);
        radio->setObjectName(
            QStringLiteral("frameLegacy%1").arg(objectSuffixes.at(index)));
        radio->setProperty("frameValue", option.value);
        group->addButton(radio, option.value.toInt());
        optionsLayout->addWidget(radio);
        m_radios.insert(option.value.toInt(), radio);
        connect(radio, &QRadioButton::toggled,
                this, [this, value = option.value](bool checked) {
            if (checked && !m_viewModel->selectFrame(value)) {
                syncState();
            }
        });
    }
    layout->addWidget(optionsPanel);

    m_status = new QLabel(content);
    m_status->setObjectName(QStringLiteral("frameLegacyStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll);
}

void ConfigFrameTypeView::syncState()
{
    const bool editable = m_viewModel->Connected()
        && m_viewModel->SnapshotReady()
        && !m_viewModel->HasPendingWrites()
        && !m_viewModel->Armed();
    const QVariant selected = m_viewModel->SelectedFrame();
    for (auto iterator = m_radios.begin(); iterator != m_radios.end();
         ++iterator) {
        const QSignalBlocker blocker(iterator.value());
        iterator.value()->setChecked(
            selected.isValid() && selected.toInt() == iterator.key());
        iterator.value()->setEnabled(editable);
    }
    m_status->setText(m_viewModel->Status());
}
