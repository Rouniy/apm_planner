#include "ConfigFrameClassTypeView.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <cmath>

namespace {
bool valuesEqual(const QVariant &left, const QVariant &right)
{
    if (!left.isValid() || !right.isValid()) {
        return !left.isValid() && !right.isValid();
    }
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}
} // namespace

ConfigFrameClassTypeView::ConfigFrameClassTypeView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigFrameClassTypeViewModel(this))
{
    setObjectName(QStringLiteral("ConfigFrameClassTypeView"));
    buildUi();
    connect(m_viewModel, &ConfigFrameClassTypeViewModel::optionsChanged,
            this, &ConfigFrameClassTypeView::syncOptions);
    connect(m_viewModel, &ConfigFrameClassTypeViewModel::stateChanged,
            this, &ConfigFrameClassTypeView::syncState);
    connect(m_viewModel, &ConfigFrameClassTypeViewModel::writeRequested,
            this, &ConfigFrameClassTypeView::writeRequested);
    connect(m_viewModel, &ConfigFrameClassTypeViewModel::refreshRequested,
            this, &ConfigFrameClassTypeView::refreshRequested);
    syncOptions();
    syncState();
}

ConfigFrameClassTypeView::~ConfigFrameClassTypeView() = default;

QSize ConfigFrameClassTypeView::sizeHint() const
{
    return QSize(560, 480);
}

void ConfigFrameClassTypeView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigFrameClassTypeView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigFrameClassTypeView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigFrameClassTypeView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigFrameClassTypeView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigFrameClassTypeView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigFrameClassTypeView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigFrameClassTypeView::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    m_viewModel->parameterWriteCancelled(batchId, componentId, name);
}

void ConfigFrameClassTypeView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigFrameClassTypeView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigFrameClassTypeView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigFrameClassTypeView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigFrameClassTypeView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#frameClassTypeTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#frameClassTypeStatus { color: #34D399; }"
        "QPushButton, QComboBox { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #303A35; padding: 5px; }"
        "QPushButton:disabled, QComboBox:disabled { color: #68736D; }"
        "QFrame#framePreviewPanel { background: #202623;"
        " border: 1px solid #56615B; border-radius: 4px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("frameClassTypeScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("frameClassTypeContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Frame Class / Type"), content);
    title->setObjectName(QStringLiteral("frameClassTypeTitle"));
    layout->addWidget(title);

    m_refresh = new QPushButton(tr("Refresh Params"), content);
    m_refresh->setObjectName(QStringLiteral("frameClassTypeRefresh"));
    m_refresh->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_refresh, 0, Qt::AlignLeft);

    auto *fields = new QGridLayout;
    fields->setHorizontalSpacing(8);
    fields->setVerticalSpacing(8);
    auto *classLabel = new QLabel(tr("Frame Class"), content);
    classLabel->setObjectName(QStringLiteral("frameClassLabel"));
    fields->addWidget(classLabel, 0, 0);
    m_frameClass = new QComboBox(content);
    m_frameClass->setObjectName(QStringLiteral("frameClassCombo"));
    m_frameClass->setMinimumWidth(240);
    fields->addWidget(m_frameClass, 0, 1);
    auto *typeLabel = new QLabel(tr("Frame Type"), content);
    typeLabel->setObjectName(QStringLiteral("frameTypeLabel"));
    fields->addWidget(typeLabel, 1, 0);
    m_frameType = new QComboBox(content);
    m_frameType->setObjectName(QStringLiteral("frameTypeCombo"));
    m_frameType->setMinimumWidth(240);
    fields->addWidget(m_frameType, 1, 1);
    fields->setColumnStretch(2, 1);
    layout->addLayout(fields);

    auto *previewPanel = new QFrame(content);
    previewPanel->setObjectName(QStringLiteral("framePreviewPanel"));
    previewPanel->setFixedSize(260, 200);
    auto *previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(12, 12, 12, 12);
    m_preview = new QLabel(previewPanel);
    m_preview->setObjectName(QStringLiteral("framePreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setScaledContents(false);
    previewLayout->addWidget(m_preview, 1);
    m_previewCaption = new QLabel(previewPanel);
    m_previewCaption->setObjectName(QStringLiteral("framePreviewCaption"));
    m_previewCaption->setAlignment(Qt::AlignCenter);
    m_previewCaption->setWordWrap(true);
    previewLayout->addWidget(m_previewCaption);
    layout->addWidget(previewPanel, 0, Qt::AlignLeft);

    m_status = new QLabel(content);
    m_status->setObjectName(QStringLiteral("frameClassTypeStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_refresh, &QPushButton::clicked,
            m_viewModel, &ConfigFrameClassTypeViewModel::Refresh);
    connect(m_frameClass, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        if (index < 0
            || !m_viewModel->selectClass(m_frameClass->itemData(index))) {
            syncOptions();
        }
    });
    connect(m_frameType, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        if (index < 0
            || !m_viewModel->selectType(m_frameType->itemData(index))) {
            syncOptions();
        }
    });
}

void ConfigFrameClassTypeView::syncOptions()
{
    {
        const QSignalBlocker blocker(m_frameClass);
        m_frameClass->clear();
        for (const ParamOption &option : m_viewModel->ClassOptions()) {
            m_frameClass->addItem(option.text, option.value);
        }
        m_frameClass->setCurrentIndex(
            optionIndex(m_frameClass, m_viewModel->SelectedClass()));
    }
    {
        const QSignalBlocker blocker(m_frameType);
        m_frameType->clear();
        for (const ParamOption &option : m_viewModel->TypeOptions()) {
            m_frameType->addItem(option.text, option.value);
        }
        m_frameType->setCurrentIndex(
            optionIndex(m_frameType, m_viewModel->SelectedType()));
    }

    const QString resource = m_viewModel->FrameImageResource();
    m_preview->setProperty("frameImageKey", m_viewModel->FrameImageKey());
    if (resource.isEmpty()) {
        m_preview->clear();
    } else {
        const QPixmap pixmap(resource);
        m_preview->setPixmap(
            pixmap.scaled(QSize(230, 150), Qt::KeepAspectRatio,
                          Qt::SmoothTransformation));
    }
    m_previewCaption->setText(m_viewModel->FrameImageCaption());
    syncState();
}

void ConfigFrameClassTypeView::syncState()
{
    const bool ready = m_viewModel->Connected()
        && m_viewModel->SnapshotReady()
        && !m_viewModel->ReconciliationRequired()
        && !m_viewModel->HasPendingWrites();
    const bool editable = ready && !m_viewModel->Armed();
    m_frameClass->setEnabled(editable);
    m_frameType->setEnabled(editable && m_viewModel->TypeEnabled());
    // Refresh remains available while armed; only frame writes are blocked.
    m_refresh->setEnabled(
        m_viewModel->Connected() && !m_viewModel->HasPendingWrites());
    m_status->setText(m_viewModel->Status());
}

int ConfigFrameClassTypeView::optionIndex(
    const QComboBox *combo, const QVariant &value)
{
    for (int index = 0; index < combo->count(); ++index) {
        if (valuesEqual(combo->itemData(index), value)) {
            return index;
        }
    }
    return -1;
}
