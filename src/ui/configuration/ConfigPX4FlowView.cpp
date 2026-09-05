#include "ConfigPX4FlowView.h"

#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVariantMap>
#include <QVBoxLayout>

namespace {

class AspectImageLabel final : public QLabel
{
public:
    explicit AspectImageLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setMinimumSize(160, 120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setText(QObject::tr("No PX4Flow image received."));
    }

    void setFrame(const QImage &frame)
    {
        if (m_sourceCacheKey == frame.cacheKey()) return;
        m_sourceCacheKey = frame.cacheKey();
        m_frame = frame.copy();
        refreshPixmap();
    }

    QImage frame() const { return m_frame; }

    QSize displayedFrameSize() const
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return pixmap(Qt::ReturnByValue).size();
#else
        const QPixmap *const displayed = pixmap();
        return displayed ? displayed->size() : QSize();
#endif
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        refreshPixmap();
    }

private:
    void refreshPixmap()
    {
        if (m_frame.isNull()) {
            clear();
            setText(QObject::tr("No PX4Flow image received."));
            return;
        }
        setText(QString());
        const QSize available = contentsRect().size();
        if (available.isEmpty()) {
            clear();
            return;
        }
        setPixmap(QPixmap::fromImage(m_frame).scaled(
            available, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QImage m_frame;
    qint64 m_sourceCacheKey = 0;
};

QString sourceId(const QVariant &row)
{
    return row.toMap().value(QStringLiteral("id")).toString().trimmed();
}

QString sourceLabel(const QVariant &row)
{
    return row.toMap().value(QStringLiteral("label")).toString().trimmed();
}

} // namespace

ConfigPX4FlowView::ConfigPX4FlowView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ConfigPX4FlowView"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("PX4Flow"), this);
    title->setObjectName(QStringLiteral("Px4FlowTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    m_status = new QLabel(
        tr("Not connected — connect to a PX4Flow sensor to view its image."),
        this);
    m_status->setObjectName(QStringLiteral("Px4FlowStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_status);

    auto *sourceRow = new QHBoxLayout;
    auto *sourceCaption = new QLabel(tr("Source:"), this);
    sourceCaption->setObjectName(QStringLiteral("Px4FlowSourceLabel"));
    m_sources = new QComboBox(this);
    m_sources->setObjectName(QStringLiteral("Px4FlowSourceSelector"));
    m_sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLength);
    m_sources->setMinimumContentsLength(24);
    m_sources->setEnabled(false);
    sourceRow->addWidget(sourceCaption);
    sourceRow->addWidget(m_sources, 1);
    root->addLayout(sourceRow);

    auto *imageBorder = new QFrame(this);
    imageBorder->setObjectName(QStringLiteral("Px4FlowImageBorder"));
    imageBorder->setFrameShape(QFrame::StyledPanel);
    imageBorder->setSizePolicy(QSizePolicy::Expanding,
                               QSizePolicy::Expanding);
    auto *imageLayout = new QVBoxLayout(imageBorder);
    imageLayout->setContentsMargins(8, 8, 8, 8);
    m_image = new AspectImageLabel(imageBorder);
    m_image->setObjectName(QStringLiteral("Px4FlowImage"));
    imageLayout->addWidget(m_image, 1);
    root->addWidget(imageBorder, 1);

    m_focus = new QPushButton(tr("Focus"), this);
    m_focus->setObjectName(QStringLiteral("Px4FlowFocusButton"));
    m_focus->setEnabled(false);
    m_focus->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    root->addWidget(m_focus, 0, Qt::AlignLeft);

    connect(m_sources,
            QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        if (index < 0) {
            return;
        }
        const QString id = m_sources->itemData(index).toString();
        if (id.isEmpty()) {
            return;
        }
        m_requestedSourceId = id;
        syncModeButton();
        emit sourceSelected(id);
    });
    connect(m_focus, &QPushButton::clicked,
            this, [this]() {
        if (m_focus->isEnabled()) {
            emit focusRequested();
        }
    });
}

QString ConfigPX4FlowView::selectedSourceId() const
{
    const int index = m_sources->currentIndex();
    return index >= 0 ? m_sources->itemData(index).toString() : QString();
}

int ConfigPX4FlowView::sourceCount() const
{
    return m_sources->count();
}

QImage ConfigPX4FlowView::frame() const
{
    const auto *const image = static_cast<const AspectImageLabel *>(m_image);
    return image->frame();
}

QSize ConfigPX4FlowView::displayedFrameSize() const
{
    const auto *const image = static_cast<const AspectImageLabel *>(m_image);
    return image->displayedFrameSize();
}

void ConfigPX4FlowView::setSources(const QVariantList &sources)
{
    const QString previous = selectedSourceId();
    QSignalBlocker blocker(m_sources);
    m_sources->clear();
    QSet<QString> ids;
    for (const QVariant &row : sources) {
        const QString id = sourceId(row);
        const QString label = sourceLabel(row);
        if (id.isEmpty() || label.isEmpty() || ids.contains(id)
            || m_sources->count() >= MaximumSources) {
            continue;
        }
        ids.insert(id);
        m_sources->addItem(label, id);
    }
    if (m_requestedSourceId.isEmpty()) {
        m_requestedSourceId = previous;
    }
    applyRequestedSource();
    m_sources->setEnabled(m_sources->count() > 0);
    syncModeButton();
}

void ConfigPX4FlowView::setSelectedSourceId(const QString &sourceId)
{
    m_requestedSourceId = sourceId.trimmed();
    QSignalBlocker blocker(m_sources);
    applyRequestedSource();
    syncModeButton();
}

void ConfigPX4FlowView::setStatus(const QString &status)
{
    m_status->setText(status.trimmed().isEmpty()
        ? tr("Waiting for PX4Flow image stream…") : status);
}

void ConfigPX4FlowView::setFrame(const QImage &frame)
{
    static_cast<AspectImageLabel *>(m_image)->setFrame(frame);
}

void ConfigPX4FlowView::setModeState(
    bool videoOnly, bool canToggle, bool busy)
{
    m_videoOnly = videoOnly;
    m_canToggle = canToggle;
    m_busy = busy;
    syncModeButton();
}

void ConfigPX4FlowView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_active) {
        m_active = true;
        emit activated();
    }
}

void ConfigPX4FlowView::hideEvent(QHideEvent *event)
{
    if (m_active) {
        m_active = false;
        emit deactivated();
    }
    QWidget::hideEvent(event);
}

void ConfigPX4FlowView::applyRequestedSource()
{
    int index = -1;
    if (!m_requestedSourceId.isEmpty()) {
        index = m_sources->findData(m_requestedSourceId);
    }
    m_sources->setCurrentIndex(index);
    if (index >= 0) {
        m_requestedSourceId = m_sources->itemData(index).toString();
    }
}

void ConfigPX4FlowView::syncModeButton()
{
    m_focus->setText(m_videoOnly ? tr("Video") : tr("Focus"));
    const bool hasSource = !selectedSourceId().isEmpty();
    m_focus->setEnabled(hasSource && m_canToggle && !m_busy);
    if (m_busy) {
        m_focus->setToolTip(tr("PX4Flow mode change in progress."));
    } else if (!hasSource) {
        m_focus->setToolTip(tr("Select a PX4Flow sensor first."));
    } else if (!m_canToggle) {
        m_focus->setToolTip(
            tr("The selected PX4Flow source is read-only."));
    } else {
        m_focus->setToolTip(QString());
    }
}
