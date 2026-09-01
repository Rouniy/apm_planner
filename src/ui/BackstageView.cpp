#include "BackstageView.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kNavigationWidth = 210;

class BackstagePageButton final : public QAbstractButton
{
public:
    explicit BackstagePageButton(const QString &title,
                                 bool subPage,
                                 const QString &badge,
                                 QWidget *parent = nullptr)
        : QAbstractButton(parent),
          m_subPage(subPage),
          m_badge(badge)
    {
        setText(title);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(subPage ? 28 : 34);
        setProperty("subPage", subPage);
    }

    QSize sizeHint() const override
    {
        return QSize(kNavigationWidth, m_subPage ? 28 : 34);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QColor background = Qt::transparent;
        QColor foreground = m_subPage ? QColor(QStringLiteral("#c8c8c8"))
                                      : QColor(QStringLiteral("#e8e8e8"));
        if (isChecked()) {
            background = QColor(QStringLiteral("#34d399"));
            foreground = QColor(QStringLiteral("#0d1210"));
        } else if (underMouse()) {
            background = QColor(QStringLiteral("#202623"));
            foreground = Qt::white;
        }
        painter.fillRect(rect(), background);

        QFont textFont = font();
        textFont.setPixelSize(13);
        textFont.setWeight(m_subPage ? QFont::Normal : QFont::DemiBold);
        painter.setFont(textFont);
        painter.setPen(isEnabled() ? foreground : QColor(QStringLiteral("#777879")));

        const int leftPadding = m_subPage ? 30 : 14;
        int rightEdge = width() - 10;
        if (!m_badge.isEmpty()) {
            QFont badgeFont = textFont;
            badgeFont.setPixelSize(9);
            badgeFont.setWeight(QFont::Normal);
            const QFontMetrics badgeMetrics(badgeFont);
            const int badgeWidth = badgeMetrics.horizontalAdvance(m_badge) + 8;
            const QRect badgeRect(rightEdge - badgeWidth,
                                  (height() - 17) / 2,
                                  badgeWidth,
                                  17);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#8a5a00")));
            painter.drawRoundedRect(badgeRect, 3, 3);
            painter.setFont(badgeFont);
            painter.setPen(QColor(QStringLiteral("#ffd9a0")));
            painter.drawText(badgeRect, Qt::AlignCenter, m_badge);
            rightEdge = badgeRect.left() - 6;
            painter.setFont(textFont);
            painter.setPen(isEnabled() ? foreground : QColor(QStringLiteral("#777879")));
        }

        const QRect textRect(leftPadding, 0, qMax(0, rightEdge - leftPadding), height());
        const QFontMetrics metrics(textFont);
        painter.drawText(textRect,
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(text(), Qt::ElideRight, textRect.width()));

        if (hasFocus()) {
            QStyleOptionButton option;
            option.initFrom(this);
            option.rect = rect().adjusted(2, 2, -2, -2);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
        }
    }

private:
    bool m_subPage = false;
    QString m_badge;
};
}

BackstageView::BackstageView(QWidget *parent)
    : QWidget(parent),
      m_pageGroup(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("BackstageView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "BackstageView { background: #1a201d; color: white; font-family: sans-serif; }"
        "QScrollArea#backstageNavigation,"
        "QScrollArea#backstageNavigation QWidget#qt_scrollarea_viewport,"
        "QWidget#backstageNavigationContent { background: #0d1210; border: 0; }"
        "QToolButton[backstageGroup=\"true\"] { color: #e8e8e8; font-size: 13px; "
        "font-weight: 600; padding: 7px 14px; background: #0d1210; border: 0; "
        "text-align: left; min-height: 20px; }"
        "QToolButton[backstageGroup=\"true\"]:hover { background: #202623; color: white; }"
        "QWidget#parameterLoadingOverlay { background: rgba(13, 18, 16, 224); color: white; }"
        "QLabel#parameterLoadingLabel { color: white; font-size: 13px; font-weight: 600; }"
        "QProgressBar#parameterLoadingProgress { min-width: 240px; max-width: 360px; "
        "min-height: 8px; max-height: 8px; border: 0; background: #0d1210; }"
        "QProgressBar#parameterLoadingProgress::chunk { background: #34d399; }"));
    m_pageGroup->setExclusive(true);

    auto *root = new QGridLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setHorizontalSpacing(0);
    root->setVerticalSpacing(0);
    root->setColumnMinimumWidth(0, kNavigationWidth);
    root->setColumnStretch(1, 1);

    auto *navigation = new QScrollArea(this);
    navigation->setObjectName(QStringLiteral("backstageNavigation"));
    navigation->setFrameShape(QScrollArea::NoFrame);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setWidgetResizable(true);
    navigation->setFixedWidth(kNavigationWidth);

    auto *navigationContent = new QWidget(navigation);
    navigationContent->setObjectName(QStringLiteral("backstageNavigationContent"));
    m_navigationLayout = new QVBoxLayout(navigationContent);
    m_navigationLayout->setContentsMargins(0, 0, 0, 0);
    m_navigationLayout->setSpacing(0);
    m_navigationLayout->addStretch(1);
    navigation->setWidget(navigationContent);
    root->addWidget(navigation, 0, 0);

    auto *contentHost = new QWidget(this);
    contentHost->setObjectName(QStringLiteral("backstageContent"));
    auto *contentLayout = new QGridLayout(contentHost);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_pageStack = new QStackedWidget(contentHost);
    m_pageStack->setObjectName(QStringLiteral("backstagePageStack"));
    contentLayout->addWidget(m_pageStack, 0, 0);

    m_loadingOverlay = new QWidget(contentHost);
    m_loadingOverlay->setObjectName(QStringLiteral("parameterLoadingOverlay"));
    auto *loadingLayout = new QVBoxLayout(m_loadingOverlay);
    loadingLayout->setAlignment(Qt::AlignCenter);
    m_loadingLabel = new QLabel(tr("Loading parameters…"), m_loadingOverlay);
    m_loadingLabel->setObjectName(QStringLiteral("parameterLoadingLabel"));
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLayout->addWidget(m_loadingLabel);
    m_loadingProgress = new QProgressBar(m_loadingOverlay);
    m_loadingProgress->setObjectName(QStringLiteral("parameterLoadingProgress"));
    m_loadingProgress->setTextVisible(false);
    loadingLayout->addWidget(m_loadingProgress);
    contentLayout->addWidget(m_loadingOverlay, 0, 0);
    m_loadingOverlay->hide();
    root->addWidget(contentHost, 0, 1);
}

void BackstageView::addGroup(const QString &title, const QString &objectName)
{
    const QString id = objectName.isEmpty()
        ? QStringLiteral("backstageGroup%1").arg(m_groups.size() + 1)
        : objectName;
    if (m_groups.contains(id)) {
        m_currentGroupId = id;
        return;
    }

    auto *button = new QToolButton(this);
    button->setObjectName(id);
    button->setText(title);
    button->setProperty("backstageGroup", true);
    button->setProperty("groupId", id);
    button->setCheckable(true);
    button->setChecked(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_navigationLayout->insertWidget(m_navigationLayout->count() - 1, button);
    m_groups.insert(id, GroupEntry{button, QStringList(), true});
    m_currentGroupId = id;
    connect(button, &QAbstractButton::clicked, this, [this, id](bool checked) {
        setGroupExpanded(id, checked);
    });
}

bool BackstageView::addPage(const QString &id,
                            const QString &title,
                            QWidget *pageWidget,
                            bool subPage,
                            const QString &badge)
{
    if (id.isEmpty() || !pageWidget || m_pages.contains(id)) {
        return false;
    }

    auto *button = new BackstagePageButton(title, subPage, badge, this);
    button->setObjectName(id);
    button->setProperty("pageId", id);
    m_pageGroup->addButton(button);
    m_navigationLayout->insertWidget(m_navigationLayout->count() - 1, button);
    pageWidget->setProperty("pageId", id);
    m_pageStack->addWidget(pageWidget);
    m_pages.insert(id, PageEntry{button, pageWidget, m_currentGroupId, true});
    m_pageOrder.append(id);
    if (!m_currentGroupId.isEmpty()) {
        m_groups[m_currentGroupId].pageIds.append(id);
        updatePageButtonVisibility(id);
    }

    connect(button, &QAbstractButton::clicked, this, [this, id]() {
        setCurrentPage(id);
    });

    if (m_pageOrder.size() == 1) {
        setCurrentPage(id);
    }
    return true;
}

QString BackstageView::currentPageId() const
{
    QWidget *current = m_pageStack->currentWidget();
    for (const QString &id : m_pageOrder) {
        if (m_pages.value(id).page == current) {
            return id;
        }
    }
    return QString();
}

QStringList BackstageView::pageIds() const
{
    return m_pageOrder;
}

QWidget *BackstageView::page(const QString &id) const
{
    return m_pages.value(id).page;
}

bool BackstageView::isPageVisible(const QString &id) const
{
    return m_pages.contains(id) && !m_pages.value(id).button->isHidden();
}

bool BackstageView::isGroupExpanded(const QString &id) const
{
    return m_groups.contains(id) && m_groups.value(id).expanded;
}

bool BackstageView::setCurrentPage(const QString &id)
{
    const auto iterator = m_pages.constFind(id);
    if (iterator == m_pages.constEnd() || iterator->button->isHidden()) {
        return false;
    }
    if (m_pageStack->currentWidget() == iterator->page && iterator->button->isChecked()) {
        return true;
    }
    iterator->button->setChecked(true);
    m_pageStack->setCurrentWidget(iterator->page);
    emit currentPageChanged(id);
    return true;
}

bool BackstageView::setPageVisible(const QString &id, bool visible)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end()) {
        return false;
    }
    iterator->requestedVisible = visible;
    updatePageButtonVisibility(id);
    if (iterator->button->isHidden() && m_pageStack->currentWidget() == iterator->page) {
        selectFallbackPage();
    }
    return true;
}

bool BackstageView::setGroupExpanded(const QString &id, bool expanded)
{
    auto iterator = m_groups.find(id);
    if (iterator == m_groups.end()) {
        return false;
    }
    iterator->expanded = expanded;
    iterator->button->setChecked(expanded);
    bool currentWasHidden = false;
    for (const QString &pageId : iterator->pageIds) {
        updatePageButtonVisibility(pageId);
        const PageEntry &entry = m_pages.value(pageId);
        currentWasHidden = currentWasHidden
            || (entry.button->isHidden() && m_pageStack->currentWidget() == entry.page);
    }
    if (currentWasHidden) {
        selectFallbackPage();
    }
    return true;
}

void BackstageView::setLoading(bool loading, const QString &message, int progress)
{
    m_loadingLabel->setText(message.isEmpty() ? tr("Loading parameters…") : message);
    if (progress < 0) {
        m_loadingProgress->setRange(0, 0);
    } else {
        m_loadingProgress->setRange(0, 100);
        m_loadingProgress->setValue(qBound(0, progress, 100));
    }
    m_loadingOverlay->setVisible(loading);
    if (loading) {
        m_loadingOverlay->raise();
    }
}

QString BackstageView::firstVisiblePageId() const
{
    for (const QString &id : m_pageOrder) {
        if (!m_pages.value(id).button->isHidden()) {
            return id;
        }
    }
    return QString();
}

void BackstageView::updatePageButtonVisibility(const QString &id)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end()) {
        return;
    }
    const bool groupExpanded = iterator->groupId.isEmpty()
        || (m_groups.contains(iterator->groupId)
            && m_groups.value(iterator->groupId).expanded);
    iterator->button->setVisible(iterator->requestedVisible && groupExpanded);
}

void BackstageView::selectFallbackPage()
{
    const QString fallback = firstVisiblePageId();
    if (!fallback.isEmpty()) {
        setCurrentPage(fallback);
        return;
    }
    m_pageStack->setCurrentIndex(-1);
}
