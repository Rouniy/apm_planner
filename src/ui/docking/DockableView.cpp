#include "DockableView.h"

#if defined(APM_HAS_KDDOCKWIDGETS)
#include "DockHost.h"
#else
#include <QDockWidget>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#endif

#include <QAction>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace {
#if !defined(APM_HAS_KDDOCKWIDGETS)
constexpr int kFallbackLayoutVersion = 1;

Qt::DockWidgetArea toQtDockArea(DockableView::PanelLocation location)
{
    switch (location) {
    case DockableView::PanelLocation::Left:
        return Qt::LeftDockWidgetArea;
    case DockableView::PanelLocation::Right:
        return Qt::RightDockWidgetArea;
    case DockableView::PanelLocation::Top:
        return Qt::TopDockWidgetArea;
    case DockableView::PanelLocation::Bottom:
        return Qt::BottomDockWidgetArea;
    case DockableView::PanelLocation::Tabbed:
        break;
    }
    return Qt::LeftDockWidgetArea;
}
#endif
}

class DockableView::Private
{
public:
    QString viewId;
#if defined(APM_HAS_KDDOCKWIDGETS)
    DockHost *host = nullptr;
#else
    QMainWindow *host = nullptr;
    QHash<QString, QDockWidget *> panels;
    QStringList panelOrder;
#endif
};

DockableView::DockableView(const QString &viewId, QWidget *parent)
    : QWidget(parent),
      d(new Private)
{
    d->viewId = viewId.trimmed();
    Q_ASSERT(!d->viewId.isEmpty());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

#if defined(APM_HAS_KDDOCKWIDGETS)
    d->host = new DockHost(d->viewId, this);
    connect(d->host, &DockHost::layoutRestoreRejected,
            this, &DockableView::layoutRestoreRejected);
#else
    d->host = new QMainWindow(this, Qt::Widget);
    d->host->setWindowFlag(Qt::Window, false);
    d->host->setObjectName(QStringLiteral("%1FallbackDockHost").arg(d->viewId));
    d->host->setDockNestingEnabled(true);
    auto *placeholder = new QWidget(d->host);
    placeholder->setObjectName(QStringLiteral("%1FallbackDockCenter").arg(d->viewId));
    placeholder->setMaximumSize(0, 0);
    placeholder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    d->host->setCentralWidget(placeholder);
#endif
    layout->addWidget(d->host);
    // A child QMainWindow starts with an explicit hidden state. Merely adding
    // it to the page layout does not clear that state, so the no-KDD fallback
    // otherwise leaves the whole DATA/PLAN docking surface invisible.
    d->host->show();
}

DockableView::~DockableView() = default;

QString DockableView::viewId() const
{
    return d->viewId;
}

QStringList DockableView::panelIds() const
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->dockIds();
#else
    return d->panelOrder;
#endif
}

bool DockableView::hasPanel(const QString &panelId) const
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->dockIds().contains(panelId);
#else
    return d->panels.contains(panelId);
#endif
}

bool DockableView::addPanel(const QString &panelId,
                            const QString &title,
                            QWidget *content,
                            PanelLocation location,
                            const QString &relativeToPanelId,
                            const QSize &preferredSize)
{
    const QString id = panelId.trimmed();
    if (id.isEmpty() || !content || hasPanel(id)) {
        return false;
    }
    content->setObjectName(id);

#if defined(APM_HAS_KDDOCKWIDGETS)
    DockHost::Location hostLocation = DockHost::Location::Left;
    switch (location) {
    case PanelLocation::Left:
        hostLocation = DockHost::Location::Left;
        break;
    case PanelLocation::Right:
        hostLocation = DockHost::Location::Right;
        break;
    case PanelLocation::Top:
        hostLocation = DockHost::Location::Top;
        break;
    case PanelLocation::Bottom:
        hostLocation = DockHost::Location::Bottom;
        break;
    case PanelLocation::Tabbed:
        hostLocation = DockHost::Location::Tabbed;
        break;
    }
    return d->host->addDock(id, title, content, hostLocation,
                            relativeToPanelId, preferredSize);
#else
    QDockWidget *relativePanel = nullptr;
    if (!relativeToPanelId.isEmpty()) {
        relativePanel = d->panels.value(relativeToPanelId, nullptr);
        if (!relativePanel) {
            return false;
        }
    }
    if (location == PanelLocation::Tabbed && !relativePanel) {
        return false;
    }

    auto *panel = new QDockWidget(title, d->host);
    panel->setObjectName(id);
    panel->setFeatures(panel->features() & ~QDockWidget::DockWidgetClosable);
    panel->setWidget(content);
    if (preferredSize.isValid()) {
        panel->resize(preferredSize);
    }
    if (location == PanelLocation::Tabbed) {
        d->host->tabifyDockWidget(relativePanel, panel);
    } else if (relativePanel) {
        d->host->splitDockWidget(relativePanel, panel,
                                 location == PanelLocation::Top
                                     || location == PanelLocation::Bottom
                                     ? Qt::Vertical : Qt::Horizontal);
    } else {
        d->host->addDockWidget(toQtDockArea(location), panel);
    }
    d->panels.insert(id, panel);
    d->panelOrder.append(id);
    panel->toggleViewAction()->setChecked(true);
    panel->show();
    return true;
#endif
}

QAction *DockableView::panelToggleAction(const QString &panelId) const
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->toggleAction(panelId);
#else
    QDockWidget *panel = d->panels.value(panelId, nullptr);
    return panel ? panel->toggleViewAction() : nullptr;
#endif
}

bool DockableView::isPanelOpen(const QString &panelId) const
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->isDockOpen(panelId);
#else
    QDockWidget *panel = d->panels.value(panelId, nullptr);
    return panel && !panel->isHidden();
#endif
}

bool DockableView::setPanelVisible(const QString &panelId, bool visible)
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->setDockVisible(panelId, visible);
#else
    QDockWidget *panel = d->panels.value(panelId, nullptr);
    if (!panel) {
        return false;
    }
    if (!visible && !panel->isHidden()) {
        int visibleCount = 0;
        for (QDockWidget *candidate : d->panels) {
            if (candidate && !candidate->isHidden()) {
                ++visibleCount;
            }
        }
        if (visibleCount <= 1) {
            return false;
        }
    }
    panel->toggleViewAction()->setChecked(visible);
    panel->setVisible(visible);
    return true;
#endif
}

bool DockableView::setPanelSizeWeights(const QStringList &panelIds,
                                       const QList<int> &weights,
                                       Qt::Orientation orientation)
{
    if (panelIds.isEmpty() || panelIds.size() != weights.size()) {
        return false;
    }
#if defined(APM_HAS_KDDOCKWIDGETS)
    Q_UNUSED(panelIds)
    Q_UNUSED(weights)
    Q_UNUSED(orientation)
    // KDDockWidgets applies the InitialOption preferred sizes when the docks
    // are inserted. The fallback QMainWindow needs an explicit resizeDocks()
    // pass to produce the same initial ratios.
    return true;
#else
    QList<QDockWidget *> panels;
    panels.reserve(panelIds.size());
    for (int index = 0; index < panelIds.size(); ++index) {
        QDockWidget *panel = d->panels.value(panelIds.at(index), nullptr);
        if (!panel || weights.at(index) <= 0) {
            return false;
        }
        panels.append(panel);
    }
    const auto applyWeights = [host = d->host, panels, weights, orientation]() {
        const int available = orientation == Qt::Horizontal
            ? host->width() : host->height();
        int totalWeight = 0;
        for (int weight : weights) totalWeight += weight;
        QList<int> sizes;
        sizes.reserve(weights.size());
        for (int weight : weights) {
            sizes.append(qMax(1, available * weight / totalWeight));
        }
        host->resizeDocks(panels, sizes, orientation);
    };
    applyWeights();
    QTimer::singleShot(0, d->host, applyWeights);
    return true;
#endif
}

QByteArray DockableView::saveLayout() const
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->saveLayout();
#else
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("apmplanner-fallback-dock-layout"));
    root.insert(QStringLiteral("version"), kFallbackLayoutVersion);
    root.insert(QStringLiteral("viewId"), d->viewId);
    QJsonArray panelIds;
    for (const QString &panelId : d->panelOrder) {
        panelIds.append(panelId);
    }
    root.insert(QStringLiteral("panels"), panelIds);
    root.insert(QStringLiteral("payload"),
                QString::fromLatin1(d->host->saveState(kFallbackLayoutVersion).toBase64()));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
#endif
}

bool DockableView::restoreLayout(const QByteArray &layout)
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->restoreLayout(layout);
#else
    const auto showAllPanels = [this]() {
        for (const QString &panelId : d->panelOrder) {
            if (QDockWidget *panel = d->panels.value(panelId, nullptr)) {
                panel->toggleViewAction()->setChecked(true);
                panel->show();
            }
        }
    };
    const auto reject = [this, &showAllPanels](const QString &reason) {
        showAllPanels();
        emit layoutRestoreRejected(reason);
        return false;
    };
    const QJsonDocument document = QJsonDocument::fromJson(layout);
    const QJsonObject root = document.object();
    if (!document.isObject()
        || root.value(QStringLiteral("schema")).toString()
            != QStringLiteral("apmplanner-fallback-dock-layout")
        || root.value(QStringLiteral("version")).toInt(-1) != kFallbackLayoutVersion
        || root.value(QStringLiteral("viewId")).toString() != d->viewId) {
        return reject(tr("Fallback dock layout is not compatible"));
    }
    QStringList savedPanelIds;
    const QJsonArray panelIds = root.value(QStringLiteral("panels")).toArray();
    for (const QJsonValue &panelId : panelIds) {
        if (!panelId.isString()) {
            return reject(tr("Fallback dock layout panel list is invalid"));
        }
        savedPanelIds.append(panelId.toString());
    }
    if (savedPanelIds != d->panelOrder) {
        return reject(tr("Fallback dock layout panels do not match this view"));
    }
    const QByteArray payload = QByteArray::fromBase64(
        root.value(QStringLiteral("payload")).toString().toLatin1());
    if (payload.isEmpty() || !d->host->restoreState(payload, kFallbackLayoutVersion)) {
        return reject(tr("Fallback dock layout payload is invalid"));
    }
    bool hasVisiblePanel = false;
    for (QDockWidget *panel : d->panels) {
        if (panel && !panel->isHidden()) {
            hasVisiblePanel = true;
            break;
        }
    }
    if (!hasVisiblePanel) {
        showAllPanels();
    }
    return true;
#endif
}
