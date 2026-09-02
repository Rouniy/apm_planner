#include "BackstageView.h"

#include "configuration/ConfigParamLoadingView.h"
#include "configuration/ConfigParamLoadingViewModel.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QFontMetrics>
#include <QGridLayout>
#include <QPainter>
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
            background = QColor(QStringLiteral("#2e2e2f"));
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
        "BackstageView { background: #1a201d; color: white;"
        " font-family: 'Bitstream Vera Sans'; }"
        "QScrollArea#backstageNavigation,"
        "QScrollArea#backstageNavigation QWidget#qt_scrollarea_viewport,"
        "QWidget#backstageNavigationContent { background: #0d1210; border: 0; }"
        "QToolButton[backstageGroup=\"true\"] { color: #e8e8e8; font-size: 13px; "
        "font-weight: 600; padding: 7px 14px; background: #0d1210; border: 0; "
        "text-align: left; min-height: 20px; }"
        "QToolButton[backstageGroup=\"true\"]:hover { background: #2e2e2f; color: white; }"
        "QWidget#parameterLoadingOverlay { background: #1a201d; color: white; }"
        "QLabel#parameterLoadingStatus, QLabel#parameterLoadingCount { color: white; "
        "font-size: 13px; font-weight: 600; }"
        "QProgressBar#parameterLoadingProgress { min-height: 20px; max-height: 20px; "
        "border: 1px solid #2a322d; background: #0d1210; color: white; }"
        "QProgressBar#parameterLoadingProgress::chunk { background: #34d399; }"
        "QPushButton[loadingAction=\"true\"] { min-height: 28px; padding: 3px 12px; "
        "background: #202623; color: #e6ede9; border: 1px solid #2a322d; }"
        "QPushButton[loadingAction=\"true\"]:hover { background: #2e2e2f; }"));
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
    auto *overlayLayout = new QVBoxLayout(m_loadingOverlay);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    m_parameterLoadingView = new ConfigParamLoadingView(m_loadingOverlay);
    overlayLayout->addWidget(m_parameterLoadingView);
    connect(m_parameterLoadingView,
            &ConfigParamLoadingView::stopLoadingRequested,
            this, &BackstageView::stopLoadingRequested);
    connect(m_parameterLoadingView,
            &ConfigParamLoadingView::retryLoadingRequested,
            this, &BackstageView::retryLoadingRequested);
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
    BackstagePage definition;
    definition.id = id;
    definition.header = title;
    definition.badge = badge;
    definition.isSub = subPage;
    return addPageEntry(definition, pageWidget);
}

bool BackstageView::addPage(const BackstagePage &page)
{
    if (!page.factory) {
        return false;
    }
    return addPageEntry(page, nullptr);
}

bool BackstageView::addPageEntry(const BackstagePage &definition,
                                 QWidget *pageWidget)
{
    if (definition.id.isEmpty() || (!pageWidget && !definition.factory)
        || m_pages.contains(definition.id)) {
        return false;
    }

    auto *button = new BackstagePageButton(definition.header,
                                           definition.isSub,
                                           definition.badge,
                                           this);
    button->setObjectName(definition.id);
    button->setProperty("pageId", definition.id);
    m_pageGroup->addButton(button);
    m_navigationLayout->insertWidget(m_navigationLayout->count() - 1, button);
    if (pageWidget) {
        pageWidget->setProperty("pageId", definition.id);
        m_pageStack->addWidget(pageWidget);
    }
    const QString groupId = definition.isSub ? m_currentGroupId : QString();
    const bool initiallyVisible = !definition.visibleWhen
        || definition.visibleWhen();
    m_pages.insert(definition.id,
                   PageEntry{button, pageWidget, definition, groupId,
                             initiallyVisible});
    m_pageOrder.append(definition.id);
    if (!groupId.isEmpty()) {
        m_groups[groupId].pageIds.append(definition.id);
    }
    updatePageButtonVisibility(definition.id);

    const QString id = definition.id;
    connect(button, &QAbstractButton::clicked, this, [this, id]() {
        setCurrentPage(id);
    });

    if (m_automaticSelectionEnabled && m_currentPageId.isEmpty()
        && !button->isHidden()) {
        setCurrentPage(definition.id);
    }
    return true;
}

QString BackstageView::currentPageId() const
{
    return m_currentPageId;
}

QStringList BackstageView::pageIds() const
{
    return m_pageOrder;
}

QWidget *BackstageView::page(const QString &id) const
{
    return m_pages.value(id).page;
}

BackstagePage BackstageView::pageDefinition(const QString &id) const
{
    return m_pages.value(id).definition;
}

bool BackstageView::isPageCreated(const QString &id) const
{
    return m_pages.contains(id) && m_pages.value(id).page;
}

bool BackstageView::isPageVisible(const QString &id) const
{
    return m_pages.contains(id) && !m_pages.value(id).button->isHidden();
}

bool BackstageView::isGroupExpanded(const QString &id) const
{
    return m_groups.contains(id) && m_groups.value(id).expanded;
}

bool BackstageView::isGroupVisible(const QString &id) const
{
    return m_groups.contains(id) && !m_groups.value(id).button->isHidden();
}

void BackstageView::setAutomaticSelectionEnabled(bool enabled)
{
    m_automaticSelectionEnabled = enabled;
}

bool BackstageView::restoreInitialPage(const QString &preferredPage)
{
    m_automaticSelectionEnabled = true;
    if (!preferredPage.isEmpty()) {
        for (const QString &id : m_pageOrder) {
            const PageEntry &entry = m_pages.value(id);
            if (!entry.definition.isSub && !entry.button->isHidden()
                && (id == preferredPage
                    || entry.definition.header == preferredPage)) {
                return setCurrentPage(id);
            }
        }
    }
    selectFallbackPage();
    return !m_currentPageId.isEmpty();
}

bool BackstageView::setCurrentPage(const QString &id)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end() || iterator->button->isHidden()) {
        return false;
    }
    QWidget *pageWidget = ensurePageCreated(id);
    if (!pageWidget) {
        return false;
    }
    if (m_currentPageId == id && iterator->button->isChecked()) {
        return true;
    }

    const QString previousId = m_currentPageId;
    QWidget *previousPage = m_pages.value(previousId).page;
    iterator->button->setChecked(true);
    m_pageStack->setCurrentWidget(pageWidget);
    m_currentPageId = id;
    if (previousPage) {
        emit pageDeactivated(previousId, previousPage);
    }
    emit pageActivated(id, pageWidget);
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
    if (iterator->button->isHidden() && m_currentPageId == id) {
        selectFallbackPage();
    } else if (visible && m_currentPageId.isEmpty()) {
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
            || (entry.button->isHidden() && m_currentPageId == pageId);
    }
    if (currentWasHidden) {
        selectFallbackPage();
    } else if (expanded && m_currentPageId.isEmpty()) {
        selectFallbackPage();
    }
    return true;
}

bool BackstageView::setGroupVisible(const QString &id, bool visible)
{
    auto iterator = m_groups.find(id);
    if (iterator == m_groups.end()) {
        return false;
    }
    iterator->button->setVisible(visible);
    bool currentWasHidden = false;
    for (const QString &pageId : iterator->pageIds) {
        auto pageIterator = m_pages.find(pageId);
        if (pageIterator == m_pages.end()) {
            continue;
        }
        pageIterator->button->setVisible(
            visible && iterator->expanded && pageIterator->requestedVisible);
        currentWasHidden = currentWasHidden
            || (pageIterator->button->isHidden()
                && m_currentPageId == pageId);
    }
    if (currentWasHidden) {
        selectFallbackPage();
    } else if (visible && m_currentPageId.isEmpty()) {
        selectFallbackPage();
    }
    return true;
}

bool BackstageView::resetPage(const QString &id)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end() || !iterator->definition.factory) {
        return false;
    }
    QWidget *pageWidget = iterator->page;
    if (!pageWidget) {
        return true;
    }

    const bool wasCurrent = m_currentPageId == id;
    if (wasCurrent) {
        emit pageDeactivated(id, pageWidget);
        m_pageStack->setCurrentIndex(-1);
        m_currentPageId.clear();
        m_pageGroup->setExclusive(false);
        iterator->button->setChecked(false);
        m_pageGroup->setExclusive(true);
    }
    m_pageStack->removeWidget(pageWidget);
    iterator->page = nullptr;
    delete pageWidget;
    if (wasCurrent) {
        selectFallbackPage();
    }
    return true;
}

void BackstageView::refreshVisibility()
{
    bool currentWasHidden = false;
    for (const QString &id : m_pageOrder) {
        auto iterator = m_pages.find(id);
        if (iterator == m_pages.end() || !iterator->definition.visibleWhen) {
            continue;
        }
        iterator->requestedVisible = iterator->definition.visibleWhen();
        updatePageButtonVisibility(id);
        currentWasHidden = currentWasHidden
            || (iterator->button->isHidden() && m_currentPageId == id);
    }
    if (currentWasHidden || m_currentPageId.isEmpty()) {
        selectFallbackPage();
    }
}

bool BackstageView::shouldShowParameterLoading(bool connected,
                                               bool parametersReady,
                                               bool allowsPartialParameters)
{
    return connected && !parametersReady && !allowsPartialParameters;
}

void BackstageView::setParameterLoadingState(bool visible, int received,
                                             int reported,
                                             bool loadingCancelled,
                                             const QString &failure)
{
    m_parameterLoadingView->viewModel()->setState(
        received, reported, loadingCancelled, failure);
    m_loadingOverlay->setVisible(visible);
    if (visible) {
        m_loadingOverlay->raise();
    }
}

void BackstageView::setParameterLoadingRequesting()
{
    m_parameterLoadingView->viewModel()->setRequesting();
}

void BackstageView::setParameterLoadingStopping()
{
    m_parameterLoadingView->viewModel()->setStopping();
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

QWidget *BackstageView::ensurePageCreated(const QString &id)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end()) {
        return nullptr;
    }
    if (iterator->page) {
        return iterator->page;
    }
    if (!iterator->definition.factory) {
        return nullptr;
    }

    QWidget *pageWidget = iterator->definition.factory(m_pageStack);
    if (!pageWidget) {
        return nullptr;
    }
    pageWidget->setProperty("pageId", id);
    m_pageStack->addWidget(pageWidget);
    iterator->page = pageWidget;
    return pageWidget;
}

void BackstageView::updatePageButtonVisibility(const QString &id)
{
    auto iterator = m_pages.find(id);
    if (iterator == m_pages.end()) {
        return;
    }
    const bool groupExpanded = iterator->groupId.isEmpty()
        || (m_groups.contains(iterator->groupId)
            && m_groups.value(iterator->groupId).expanded
            && !m_groups.value(iterator->groupId).button->isHidden());
    iterator->button->setVisible(iterator->requestedVisible && groupExpanded);
}

void BackstageView::selectFallbackPage()
{
    if (!m_automaticSelectionEnabled) {
        return;
    }
    const QString fallback = firstVisiblePageId();
    if (!fallback.isEmpty()) {
        setCurrentPage(fallback);
        return;
    }
    const QString previousId = m_currentPageId;
    if (!previousId.isEmpty()) {
        QWidget *currentPage = m_pages.value(previousId).page;
        if (currentPage) {
            emit pageDeactivated(previousId, currentPage);
        }
        QAbstractButton *button = m_pages.value(previousId).button;
        m_pageGroup->setExclusive(false);
        button->setChecked(false);
        m_pageGroup->setExclusive(true);
    }
    m_currentPageId.clear();
    m_pageStack->setCurrentIndex(-1);
    if (!previousId.isEmpty()) {
        emit currentPageChanged(QString());
    }
}
