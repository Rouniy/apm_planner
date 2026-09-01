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

bool DockableView::setPanelVisible(const QString &panelId, bool visible)
{
#if defined(APM_HAS_KDDOCKWIDGETS)
    return d->host->setDockVisible(panelId, visible);
#else
    QDockWidget *panel = d->panels.value(panelId, nullptr);
    if (!panel) {
        return false;
    }
    panel->setVisible(visible);
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
    const QJsonDocument document = QJsonDocument::fromJson(layout);
    const QJsonObject root = document.object();
    if (!document.isObject()
        || root.value(QStringLiteral("schema")).toString()
            != QStringLiteral("apmplanner-fallback-dock-layout")
        || root.value(QStringLiteral("version")).toInt(-1) != kFallbackLayoutVersion
        || root.value(QStringLiteral("viewId")).toString() != d->viewId) {
        emit layoutRestoreRejected(tr("Fallback dock layout is not compatible"));
        return false;
    }
    QStringList savedPanelIds;
    const QJsonArray panelIds = root.value(QStringLiteral("panels")).toArray();
    for (const QJsonValue &panelId : panelIds) {
        if (!panelId.isString()) {
            emit layoutRestoreRejected(tr("Fallback dock layout panel list is invalid"));
            return false;
        }
        savedPanelIds.append(panelId.toString());
    }
    if (savedPanelIds != d->panelOrder) {
        emit layoutRestoreRejected(tr("Fallback dock layout panels do not match this view"));
        return false;
    }
    const QByteArray payload = QByteArray::fromBase64(
        root.value(QStringLiteral("payload")).toString().toLatin1());
    if (payload.isEmpty() || !d->host->restoreState(payload, kFallbackLayoutVersion)) {
        emit layoutRestoreRejected(tr("Fallback dock layout payload is invalid"));
        return false;
    }
    return true;
#endif
}
