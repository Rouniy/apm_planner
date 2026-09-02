#include "DockableView.h"

#include <QAction>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace {
constexpr int kPanelLayoutVersion = 2;
const char kPanelLayoutSchema[] = "apmplanner-fixed-panel-layout";
const char kGeneratedContainerProperty[] = "apmGeneratedPanelContainer";

QString orientationName(Qt::Orientation orientation)
{
    return orientation == Qt::Horizontal
        ? QStringLiteral("horizontal") : QStringLiteral("vertical");
}

Qt::Orientation splitOrientation(DockableView::PanelLocation location)
{
    return location == DockableView::PanelLocation::Top
            || location == DockableView::PanelLocation::Bottom
        ? Qt::Vertical : Qt::Horizontal;
}

bool insertBefore(DockableView::PanelLocation location)
{
    return location == DockableView::PanelLocation::Left
        || location == DockableView::PanelLocation::Top;
}
}

class DockableView::Private
{
public:
    struct Panel
    {
        QString id;
        QString title;
        QWidget *content = nullptr;
        QWidget *frame = nullptr;
        QAction *toggleAction = nullptr;
        QSize preferredSize;
        bool required = false;
        bool open = true;
    };

    struct Node
    {
        enum class Kind
        {
            Panel,
            Split,
            Tabs
        };

        Kind kind = Kind::Panel;
        QString panelId;
        QString tabTitle;
        Qt::Orientation orientation = Qt::Horizontal;
        std::vector<std::unique_ptr<Node>> children;
        QList<int> weights;
        int currentTab = 0;
        QWidget *widget = nullptr;
    };

    struct RestoredNodeState
    {
        QHash<Node *, QList<int>> splitSizes;
        QHash<Node *, int> currentTabs;
    };

    explicit Private(DockableView *owner)
        : q(owner)
    {
    }

    Panel *panel(const QString &id) const
    {
        return panels.value(id, nullptr);
    }

    std::unique_ptr<Node> makePanelNode(const QString &id,
                                        const QString &tabTitle = QString())
    {
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Panel;
        node->panelId = id;
        node->tabTitle = tabTitle;
        return node;
    }

    bool nodeContains(const Node *node, const QString &panelId) const
    {
        if (!node) {
            return false;
        }
        if (node->kind == Node::Kind::Panel) {
            return node->panelId == panelId;
        }
        for (const auto &child : node->children) {
            if (nodeContains(child.get(), panelId)) {
                return true;
            }
        }
        return false;
    }

    QStringList nodePanelIds(const Node *node) const
    {
        if (!node) {
            return {};
        }
        if (node->kind == Node::Kind::Panel) {
            return {node->panelId};
        }
        QStringList ids;
        for (const auto &child : node->children) {
            ids.append(nodePanelIds(child.get()));
        }
        return ids;
    }

    QString firstTitle(const Node *node) const
    {
        if (!node) {
            return QString();
        }
        if (!node->tabTitle.isEmpty()) {
            return node->tabTitle;
        }
        if (node->kind == Node::Kind::Panel) {
            const Panel *entry = panel(node->panelId);
            return entry ? entry->title : node->panelId;
        }
        for (const auto &child : node->children) {
            const QString title = firstTitle(child.get());
            if (!title.isEmpty()) {
                return title;
            }
        }
        return QString();
    }

    QString splitterObjectName(const Node *node) const
    {
        const QStringList ids = nodePanelIds(node);
        if (viewId == QStringLiteral("FlightDataView")) {
            if (node->orientation == Qt::Horizontal
                    && ids.contains(QStringLiteral("HudHost"))
                    && ids.contains(QStringLiteral("FdMap"))) {
                return QStringLiteral("MainFlightSplitter");
            }
            if (node->orientation == Qt::Vertical
                    && ids.contains(QStringLiteral("HudHost"))
                    && ids.contains(QStringLiteral("FdTabs"))) {
                return QStringLiteral("VerticalDockSplitter");
            }
        } else if (viewId == QStringLiteral("FlightPlannerView")) {
            if (node->orientation == Qt::Horizontal
                    && ids.contains(QStringLiteral("Map"))
                    && ids.contains(QStringLiteral("ActionPanel"))) {
                return QStringLiteral("HorizontalDockSplitter");
            }
            if (node->orientation == Qt::Vertical
                    && ids.contains(QStringLiteral("Map"))
                    && ids.contains(QStringLiteral("WaypointPanel"))) {
                return QStringLiteral("VerticalDockSplitter");
            }
        }

        QString name = viewId + QLatin1Char('_') + ids.join(QLatin1Char('_'));
        name += node->orientation == Qt::Horizontal
            ? QStringLiteral("HorizontalSplitter")
            : QStringLiteral("VerticalSplitter");
        return name;
    }

    int splitterHandleWidth(const Node *node) const
    {
        if (viewId == QStringLiteral("FlightDataView")
                && node->orientation == Qt::Horizontal
                && nodeContains(node, QStringLiteral("HudHost"))
                && nodeContains(node, QStringLiteral("FdMap"))) {
            return 6;
        }
        return 4;
    }

    int preferredExtent(const Node *node, Qt::Orientation orientation) const
    {
        if (!node) {
            return 1;
        }
        if (node->kind == Node::Kind::Panel) {
            const Panel *entry = panel(node->panelId);
            if (!entry || !entry->preferredSize.isValid()) {
                return 1;
            }
            const int extent = orientation == Qt::Horizontal
                ? entry->preferredSize.width() : entry->preferredSize.height();
            return qMax(1, extent);
        }

        int extent = node->kind == Node::Kind::Split
                && node->orientation == orientation ? 0 : 1;
        for (const auto &child : node->children) {
            const int childExtent = preferredExtent(child.get(), orientation);
            if (node->kind == Node::Kind::Split
                    && node->orientation == orientation) {
                extent += childExtent;
            } else {
                extent = qMax(extent, childExtent);
            }
        }
        return qMax(1, extent);
    }

    bool insertRelative(std::unique_ptr<Node> &slot,
                        const QString &relativeId,
                        DockableView::PanelLocation location,
                        std::unique_ptr<Node> &newPanelNode)
    {
        if (!slot) {
            return false;
        }

        // Reuse an existing tab group when the relative panel is already one
        // of its direct pages. This prevents nested one-tab QTabWidgets.
        if (location == DockableView::PanelLocation::Tabbed
                && slot->kind == Node::Kind::Tabs) {
            for (const auto &child : slot->children) {
                if (child->kind == Node::Kind::Panel
                        && child->panelId == relativeId) {
                    newPanelNode->tabTitle = firstTitle(newPanelNode.get());
                    slot->children.push_back(std::move(newPanelNode));
                    return true;
                }
            }
        }

        if (slot->kind == Node::Kind::Panel
                && slot->panelId == relativeId) {
            if (location == DockableView::PanelLocation::Tabbed) {
                auto tabs = std::make_unique<Node>();
                tabs->kind = Node::Kind::Tabs;
                tabs->tabTitle = slot->tabTitle;
                slot->tabTitle = firstTitle(slot.get());
                newPanelNode->tabTitle = firstTitle(newPanelNode.get());
                tabs->children.push_back(std::move(slot));
                tabs->children.push_back(std::move(newPanelNode));
                slot = std::move(tabs);
                return true;
            }

            auto split = std::make_unique<Node>();
            split->kind = Node::Kind::Split;
            split->orientation = splitOrientation(location);
            split->tabTitle = slot->tabTitle;
            slot->tabTitle.clear();

            const int oldExtent = preferredExtent(slot.get(), split->orientation);
            const int newExtent = preferredExtent(newPanelNode.get(),
                                                  split->orientation);
            if (insertBefore(location)) {
                split->children.push_back(std::move(newPanelNode));
                split->children.push_back(std::move(slot));
                split->weights << newExtent << oldExtent;
            } else {
                split->children.push_back(std::move(slot));
                split->children.push_back(std::move(newPanelNode));
                split->weights << oldExtent << newExtent;
            }
            slot = std::move(split);
            return true;
        }

        // Flatten adjacent splits with the same orientation. Apart from
        // producing simpler JSON, this makes size weights map directly to the
        // panels named by callers.
        if (location != DockableView::PanelLocation::Tabbed
                && slot->kind == Node::Kind::Split
                && slot->orientation == splitOrientation(location)) {
            for (std::size_t index = 0; index < slot->children.size(); ++index) {
                Node *child = slot->children[index].get();
                if (child->kind != Node::Kind::Panel
                        || child->panelId != relativeId) {
                    continue;
                }
                while (slot->weights.size()
                       < static_cast<int>(slot->children.size())) {
                    const int weightIndex = slot->weights.size();
                    slot->weights.append(preferredExtent(
                        slot->children[static_cast<std::size_t>(weightIndex)].get(),
                        slot->orientation));
                }
                const std::size_t insertionIndex = insertBefore(location)
                    ? index : index + 1;
                const int newExtent = preferredExtent(newPanelNode.get(),
                                                      slot->orientation);
                slot->children.insert(slot->children.begin()
                                          + static_cast<std::ptrdiff_t>(insertionIndex),
                                      std::move(newPanelNode));
                slot->weights.insert(static_cast<int>(insertionIndex), newExtent);
                return true;
            }
        }

        for (auto &child : slot->children) {
            if (insertRelative(child, relativeId, location, newPanelNode)) {
                return true;
            }
        }
        return false;
    }

    QWidget *renderNode(Node *node)
    {
        if (node->kind == Node::Kind::Panel) {
            Panel *entry = panel(node->panelId);
            node->widget = entry ? entry->frame : nullptr;
            return node->widget;
        }

        if (node->kind == Node::Kind::Split) {
            auto *splitter = new QSplitter(node->orientation, q);
            splitter->setObjectName(splitterObjectName(node));
            splitter->setProperty(kGeneratedContainerProperty, true);
            splitter->setChildrenCollapsible(false);
            splitter->setHandleWidth(splitterHandleWidth(node));
            splitter->setOpaqueResize(true);
            for (const auto &child : node->children) {
                if (QWidget *childWidget = renderNode(child.get())) {
                    splitter->addWidget(childWidget);
                }
            }
            node->widget = splitter;
            return splitter;
        }

        auto *tabs = new QTabWidget(q);
        tabs->setObjectName(QStringLiteral("%1PanelTabs").arg(viewId));
        tabs->setProperty(kGeneratedContainerProperty, true);
        tabs->setDocumentMode(true);
        tabs->setMovable(false);
        tabs->setTabsClosable(false);
        for (const auto &child : node->children) {
            if (QWidget *childWidget = renderNode(child.get())) {
                tabs->addTab(childWidget, firstTitle(child.get()));
            }
        }
        QObject::connect(tabs, &QTabWidget::currentChanged, q,
                         [node](int index) {
                             if (index >= 0) {
                                 node->currentTab = index;
                             }
                         });
        node->widget = tabs;
        return tabs;
    }

    bool refreshVisibility(Node *node)
    {
        if (node->kind == Node::Kind::Panel) {
            Panel *entry = panel(node->panelId);
            if (!entry || !entry->frame) {
                return false;
            }
            entry->frame->setVisible(entry->open);
            return entry->open;
        }

        if (node->kind == Node::Kind::Split) {
            bool anyOpen = false;
            for (const auto &child : node->children) {
                anyOpen = refreshVisibility(child.get()) || anyOpen;
            }
            if (node->widget) {
                node->widget->setVisible(anyOpen);
            }
            return anyOpen;
        }

        auto *tabs = qobject_cast<QTabWidget *>(node->widget);
        int visibleTabs = 0;
        int firstVisibleTab = -1;
        int index = 0;
        for (const auto &child : node->children) {
            const bool childOpen = refreshVisibility(child.get());
            if (tabs) {
                tabs->setTabVisible(index, childOpen);
            }
            if (childOpen) {
                ++visibleTabs;
                if (firstVisibleTab < 0) {
                    firstVisibleTab = index;
                }
            }
            ++index;
        }
        if (tabs) {
            if (node->currentTab < 0
                    || node->currentTab >= tabs->count()
                    || !tabs->isTabVisible(node->currentTab)) {
                node->currentTab = firstVisibleTab;
            }
            if (node->currentTab >= 0) {
                tabs->setCurrentIndex(node->currentTab);
            }
            tabs->tabBar()->setVisible(visibleTabs > 1);
            tabs->setVisible(visibleTabs > 0);
        }
        return visibleTabs > 0;
    }

    void applyWeights(Node *node)
    {
        if (!node) {
            return;
        }
        if (node->kind == Node::Kind::Split) {
            auto *splitter = qobject_cast<QSplitter *>(node->widget);
            if (splitter && node->weights.size() == splitter->count()) {
                // setSizes() consumes pixels, while the public API and saved
                // layout store relative weights. Tiny ratios such as 2:3
                // would otherwise be clamped to both children's minimum
                // sizes and collapse to 1:1. Normalize to a comfortably large
                // virtual extent before QSplitter maps it to the live size.
                constexpr qint64 weightScale = 1000000;
                qint64 totalWeight = 0;
                for (int weight : node->weights) {
                    totalWeight += qMax(1, weight);
                }
                QList<int> scaledSizes;
                scaledSizes.reserve(node->weights.size());
                for (int weight : node->weights) {
                    scaledSizes.append(qMax(
                        1, int(qint64(qMax(1, weight)) * weightScale
                               / totalWeight)));
                }
                splitter->setSizes(scaledSizes);
                for (int index = 0; index < node->weights.size(); ++index) {
                    // QSplitter multiplies the stretch factor by the current
                    // widget size. Reusing the requested weight here squares
                    // ratios such as DATA's 2:3 after the view is resized.
                    // Equal factors preserve the ratio established by
                    // setSizes(), while fixed min/max panels remain fixed.
                    splitter->setStretchFactor(index, 1);
                }
            }
        }
        for (const auto &child : node->children) {
            applyWeights(child.get());
        }
    }

    void rebuildLayout()
    {
        // Frames own the actual panel widgets. Detaching them before deleting
        // the old splitter/tab hierarchy keeps their identity and state
        // stable for MainWindow and plugin callers.
        for (const auto &entry : panelStorage) {
            if (entry->frame) {
                entry->frame->setParent(q);
            }
        }
        if (rootWidget) {
            layout->removeWidget(rootWidget);
            if (rootWidget->property(kGeneratedContainerProperty).toBool()) {
                delete rootWidget;
            }
            rootWidget = nullptr;
        }

        if (root) {
            rootWidget = renderNode(root.get());
            if (rootWidget) {
                layout->addWidget(rootWidget);
            }
            refreshVisibility(root.get());
            applyWeights(root.get());
            QTimer::singleShot(0, q, [this]() {
                if (root) {
                    applyWeights(root.get());
                }
            });
        }
    }

    void syncAction(Panel *entry)
    {
        if (!entry || !entry->toggleAction) {
            return;
        }
        const QSignalBlocker blocker(entry->toggleAction);
        entry->toggleAction->setChecked(entry->open);
    }

    void showAllPanels()
    {
        for (const auto &entry : panelStorage) {
            entry->open = true;
            syncAction(entry.get());
        }
        if (root) {
            refreshVisibility(root.get());
            applyWeights(root.get());
        }
    }

    Node *findWeightNode(Node *node,
                         const QStringList &ids,
                         const QList<int> &requestedWeights,
                         Qt::Orientation orientation,
                         QList<int> *weightsInChildOrder) const
    {
        if (!node) {
            return nullptr;
        }
        for (const auto &child : node->children) {
            if (Node *match = findWeightNode(child.get(), ids,
                                             requestedWeights, orientation,
                                             weightsInChildOrder)) {
                return match;
            }
        }
        if (node->kind != Node::Kind::Split
                || node->orientation != orientation
                || static_cast<int>(node->children.size()) < ids.size()) {
            return nullptr;
        }

        QList<int> ordered = node->weights;
        while (ordered.size() < static_cast<int>(node->children.size())) {
            const int childIndex = ordered.size();
            ordered.append(preferredExtent(
                node->children[static_cast<std::size_t>(childIndex)].get(),
                orientation));
        }
        QSet<int> usedChildren;
        for (int idIndex = 0; idIndex < ids.size(); ++idIndex) {
            int matchedChild = -1;
            for (std::size_t childIndex = 0;
                 childIndex < node->children.size(); ++childIndex) {
                if (nodeContains(node->children[childIndex].get(),
                                 ids.at(idIndex))) {
                    if (matchedChild >= 0) {
                        return nullptr;
                    }
                    matchedChild = static_cast<int>(childIndex);
                }
            }
            if (matchedChild < 0 || usedChildren.contains(matchedChild)) {
                return nullptr;
            }
            usedChildren.insert(matchedChild);
            ordered[matchedChild] = requestedWeights.at(idIndex);
        }
        if (weightsInChildOrder) {
            *weightsInChildOrder = ordered;
        }
        return node;
    }

    QJsonObject saveNode(const Node *node) const
    {
        QJsonObject object;
        if (node->kind == Node::Kind::Panel) {
            object.insert(QStringLiteral("type"), QStringLiteral("panel"));
            object.insert(QStringLiteral("id"), node->panelId);
            return object;
        }

        QJsonArray children;
        for (const auto &child : node->children) {
            children.append(saveNode(child.get()));
        }
        object.insert(QStringLiteral("children"), children);
        if (node->kind == Node::Kind::Tabs) {
            object.insert(QStringLiteral("type"), QStringLiteral("tabs"));
            const auto *tabs = qobject_cast<QTabWidget *>(node->widget);
            object.insert(QStringLiteral("current"),
                          tabs ? tabs->currentIndex() : node->currentTab);
            return object;
        }

        object.insert(QStringLiteral("type"), QStringLiteral("split"));
        object.insert(QStringLiteral("orientation"),
                      orientationName(node->orientation));
        QList<int> sizes = node->weights;
        const auto *splitter = qobject_cast<QSplitter *>(node->widget);
        if (splitter) {
            const QList<int> liveSizes = splitter->sizes();
            const bool usable = liveSizes.size()
                    == static_cast<int>(node->children.size())
                && std::all_of(liveSizes.cbegin(), liveSizes.cend(),
                               [](int size) { return size > 0; });
            if (usable) {
                sizes = liveSizes;
            }
        }
        while (sizes.size() < static_cast<int>(node->children.size())) {
            sizes.append(1);
        }
        QJsonArray jsonSizes;
        for (int size : sizes) {
            jsonSizes.append(qMax(1, size));
        }
        object.insert(QStringLiteral("sizes"), jsonSizes);
        return object;
    }

    bool validateNode(Node *node,
                      const QJsonValue &value,
                      RestoredNodeState *state) const
    {
        if (!node || !value.isObject()) {
            return false;
        }
        const QJsonObject object = value.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (node->kind == Node::Kind::Panel) {
            return type == QStringLiteral("panel")
                && object.value(QStringLiteral("id")).toString()
                    == node->panelId;
        }

        const QJsonValue childrenValue = object.value(QStringLiteral("children"));
        if (!childrenValue.isArray()) {
            return false;
        }
        const QJsonArray children = childrenValue.toArray();
        if (children.size() != static_cast<int>(node->children.size())) {
            return false;
        }
        if (node->kind == Node::Kind::Tabs) {
            if (type != QStringLiteral("tabs")) {
                return false;
            }
            const QJsonValue currentValue =
                object.value(QStringLiteral("current"));
            if (!currentValue.isDouble()) {
                return false;
            }
            const int current = currentValue.toInt(-1);
            // -1 is the legitimate QTabWidget state when every page in this
            // particular tab group is hidden while another panel stays open.
            if (current < -1 || current >= children.size()) {
                return false;
            }
            state->currentTabs.insert(node, current);
        } else {
            if (type != QStringLiteral("split")
                    || object.value(QStringLiteral("orientation")).toString()
                        != orientationName(node->orientation)) {
                return false;
            }
            const QJsonValue sizesValue = object.value(QStringLiteral("sizes"));
            if (!sizesValue.isArray()) {
                return false;
            }
            const QJsonArray sizes = sizesValue.toArray();
            if (sizes.size() != children.size()) {
                return false;
            }
            QList<int> parsedSizes;
            parsedSizes.reserve(sizes.size());
            for (const QJsonValue &size : sizes) {
                if (!size.isDouble() || size.toInt(0) <= 0) {
                    return false;
                }
                parsedSizes.append(size.toInt());
            }
            state->splitSizes.insert(node, parsedSizes);
        }

        for (int index = 0; index < children.size(); ++index) {
            if (!validateNode(node->children[static_cast<std::size_t>(index)].get(),
                              children.at(index), state)) {
                return false;
            }
        }
        return true;
    }

    DockableView *q = nullptr;
    QString viewId;
    QVBoxLayout *layout = nullptr;
    QWidget *rootWidget = nullptr;
    std::unique_ptr<Node> root;
    std::vector<std::unique_ptr<Panel>> panelStorage;
    QHash<QString, Panel *> panels;
    QStringList panelOrder;
};

DockableView::DockableView(const QString &viewId, QWidget *parent)
    : QWidget(parent),
      d(new Private(this))
{
    d->viewId = viewId.trimmed();
    Q_ASSERT(!d->viewId.isEmpty());

    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(0, 0, 0, 0);
    d->layout->setSpacing(0);
}

DockableView::~DockableView() = default;

QString DockableView::viewId() const
{
    return d->viewId;
}

QStringList DockableView::panelIds() const
{
    return d->panelOrder;
}

bool DockableView::hasPanel(const QString &panelId) const
{
    return d->panels.contains(panelId);
}

bool DockableView::addPanel(const QString &panelId,
                            const QString &title,
                            QWidget *content,
                            PanelLocation location,
                            const QString &relativeToPanelId,
                            const QSize &preferredSize,
                            bool requiredDocked)
{
    const QString id = panelId.trimmed();
    const QString relativeId = relativeToPanelId.trimmed();
    if (id.isEmpty() || !content || hasPanel(id)) {
        return false;
    }
    if (!relativeId.isEmpty() && !hasPanel(relativeId)) {
        return false;
    }
    if (location == PanelLocation::Tabbed && relativeId.isEmpty()) {
        return false;
    }

    auto entry = std::make_unique<Private::Panel>();
    entry->id = id;
    entry->title = title;
    entry->content = content;
    entry->preferredSize = preferredSize;
    entry->required = requiredDocked;

    auto *frame = new QWidget(this);
    frame->setObjectName(id);
    frame->setProperty("dockId", id);
    frame->setProperty("panelTitle", title);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);
    content->setObjectName(id);
    content->setProperty("dockId", id);
    frameLayout->addWidget(content);
    if (preferredSize.isValid()) {
        frame->resize(preferredSize);
    }
    entry->frame = frame;

    auto *toggleAction = new QAction(title, this);
    toggleAction->setObjectName(QStringLiteral("%1ToggleAction").arg(id));
    toggleAction->setCheckable(true);
    toggleAction->setChecked(true);
    entry->toggleAction = toggleAction;

    Private::Panel *entryPointer = entry.get();
    d->panelStorage.push_back(std::move(entry));
    d->panels.insert(id, entryPointer);
    d->panelOrder.append(id);

    auto panelNode = d->makePanelNode(id, title);
    bool inserted = false;
    if (!d->root) {
        d->root = std::move(panelNode);
        inserted = true;
    } else {
        const QString targetId = relativeId.isEmpty()
            ? d->panelOrder.constFirst() : relativeId;
        inserted = d->insertRelative(d->root, targetId, location, panelNode);
    }
    if (!inserted) {
        d->panelOrder.removeLast();
        d->panels.remove(id);
        d->panelStorage.pop_back();
        content->setParent(nullptr);
        delete toggleAction;
        delete frame;
        return false;
    }

    connect(toggleAction, &QAction::toggled, this,
            [this, id](bool visible) {
                if (!setPanelVisible(id, visible)) {
                    if (Private::Panel *panel = d->panel(id)) {
                        d->syncAction(panel);
                    }
                }
            });
    d->rebuildLayout();
    return true;
}

QAction *DockableView::panelToggleAction(const QString &panelId) const
{
    const Private::Panel *panel = d->panel(panelId);
    return panel ? panel->toggleAction : nullptr;
}

bool DockableView::isPanelOpen(const QString &panelId) const
{
    const Private::Panel *panel = d->panel(panelId);
    return panel && panel->open;
}

bool DockableView::isPanelDocked(const QString &panelId) const
{
    // Fixed panel layouts have no floating state. An open panel is therefore
    // always docked into this view.
    return isPanelOpen(panelId);
}

bool DockableView::setPanelVisible(const QString &panelId, bool visible)
{
    Private::Panel *panel = d->panel(panelId);
    if (!panel) {
        return false;
    }
    if (!visible && panel->required) {
        return false;
    }
    if (!visible && panel->open) {
        int visibleCount = 0;
        for (const auto &candidate : d->panelStorage) {
            if (candidate->open) {
                ++visibleCount;
            }
        }
        if (visibleCount <= 1) {
            return false;
        }
    }
    if (panel->open == visible) {
        d->syncAction(panel);
        return true;
    }

    panel->open = visible;
    d->syncAction(panel);
    if (d->root) {
        d->refreshVisibility(d->root.get());
        d->applyWeights(d->root.get());
    }
    return true;
}

bool DockableView::setPanelSizeWeights(const QStringList &panelIds,
                                       const QList<int> &weights,
                                       Qt::Orientation orientation)
{
    if (panelIds.isEmpty() || panelIds.size() != weights.size()) {
        return false;
    }
    QSet<QString> uniqueIds;
    for (int index = 0; index < panelIds.size(); ++index) {
        if (!hasPanel(panelIds.at(index)) || weights.at(index) <= 0
                || uniqueIds.contains(panelIds.at(index))) {
            return false;
        }
        uniqueIds.insert(panelIds.at(index));
    }

    QList<int> orderedWeights;
    Private::Node *split = d->findWeightNode(
        d->root.get(), panelIds, weights, orientation, &orderedWeights);
    if (!split) {
        return false;
    }
    split->weights = orderedWeights;
    d->applyWeights(split);
    QTimer::singleShot(0, this, [this]() {
        if (d->root) {
            d->applyWeights(d->root.get());
        }
    });
    return true;
}

bool DockableView::setPanelFixedExtent(const QString &panelId,
                                       int extent,
                                       Qt::Orientation orientation)
{
    Private::Panel *panel = d->panel(panelId);
    if (!panel || !panel->frame || !panel->content || extent <= 0) {
        return false;
    }

    if (!panel->preferredSize.isValid()) {
        panel->preferredSize = QSize(0, 0);
    }
    if (orientation == Qt::Horizontal) {
        panel->preferredSize.setWidth(extent);
        panel->content->setFixedWidth(extent);
        panel->frame->setFixedWidth(extent);
    } else {
        panel->preferredSize.setHeight(extent);
        panel->content->setFixedHeight(extent);
        panel->frame->setFixedHeight(extent);
    }
    panel->content->updateGeometry();
    panel->frame->updateGeometry();

    if (d->root) {
        d->applyWeights(d->root.get());
    }
    QTimer::singleShot(0, this, [this]() {
        if (d->root) {
            d->applyWeights(d->root.get());
        }
    });
    return true;
}

QByteArray DockableView::saveLayout() const
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"),
                QString::fromLatin1(kPanelLayoutSchema));
    root.insert(QStringLiteral("version"), kPanelLayoutVersion);
    root.insert(QStringLiteral("viewId"), d->viewId);

    QJsonArray panelIds;
    QJsonArray panelStates;
    for (const QString &panelId : d->panelOrder) {
        panelIds.append(panelId);
        const Private::Panel *panel = d->panel(panelId);
        QJsonObject state;
        state.insert(QStringLiteral("id"), panelId);
        state.insert(QStringLiteral("open"), panel && panel->open);
        panelStates.append(state);
    }
    root.insert(QStringLiteral("panels"), panelIds);
    root.insert(QStringLiteral("panelStates"), panelStates);
    if (d->root) {
        root.insert(QStringLiteral("layout"), d->saveNode(d->root.get()));
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool DockableView::restoreLayout(const QByteArray &layout)
{
    const auto reject = [this](const QString &reason) {
        d->showAllPanels();
        emit layoutRestoreRejected(reason);
        return false;
    };

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(layout, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(tr("Panel layout is corrupt"));
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString()
            != QString::fromLatin1(kPanelLayoutSchema)
            || root.value(QStringLiteral("version")).toInt(-1)
                != kPanelLayoutVersion
            || root.value(QStringLiteral("viewId")).toString() != d->viewId) {
        return reject(tr("Panel layout is not compatible"));
    }

    QStringList savedPanelIds;
    const QJsonValue panelIdsValue = root.value(QStringLiteral("panels"));
    if (!panelIdsValue.isArray()) {
        return reject(tr("Panel layout panel list is invalid"));
    }
    for (const QJsonValue &panelId : panelIdsValue.toArray()) {
        if (!panelId.isString()) {
            return reject(tr("Panel layout panel list is invalid"));
        }
        savedPanelIds.append(panelId.toString());
    }
    if (savedPanelIds != d->panelOrder) {
        return reject(tr("Panel layout panels do not match this view"));
    }

    const QJsonValue panelStatesValue =
        root.value(QStringLiteral("panelStates"));
    if (!panelStatesValue.isArray()
            || panelStatesValue.toArray().size() != d->panelOrder.size()) {
        return reject(tr("Panel layout visibility state is invalid"));
    }
    QHash<QString, bool> restoredVisibility;
    for (const QJsonValue &stateValue : panelStatesValue.toArray()) {
        if (!stateValue.isObject()) {
            return reject(tr("Panel layout visibility state is invalid"));
        }
        const QJsonObject state = stateValue.toObject();
        const QJsonValue idValue = state.value(QStringLiteral("id"));
        const QJsonValue openValue = state.value(QStringLiteral("open"));
        if (!idValue.isString() || !openValue.isBool()
                || !hasPanel(idValue.toString())
                || restoredVisibility.contains(idValue.toString())) {
            return reject(tr("Panel layout visibility state is invalid"));
        }
        restoredVisibility.insert(idValue.toString(), openValue.toBool());
    }

    int visibleCount = 0;
    for (const auto &entry : d->panelStorage) {
        const bool visible = restoredVisibility.value(entry->id, false);
        if (entry->required && !visible) {
            return reject(tr("A required panel was hidden in this layout"));
        }
        if (visible) {
            ++visibleCount;
        }
    }
    if (visibleCount == 0) {
        return reject(tr("Panel layout would hide every panel"));
    }

    Private::RestoredNodeState nodeState;
    if (!d->root
            || !d->validateNode(d->root.get(),
                                root.value(QStringLiteral("layout")),
                                &nodeState)) {
        return reject(tr("Panel layout topology is invalid"));
    }

    // Validation above is deliberately transactional: nothing becomes hidden
    // until the entire envelope and topology have been accepted.
    for (const auto &entry : d->panelStorage) {
        entry->open = restoredVisibility.value(entry->id);
        d->syncAction(entry.get());
    }
    for (auto it = nodeState.splitSizes.cbegin();
         it != nodeState.splitSizes.cend(); ++it) {
        it.key()->weights = it.value();
    }
    for (auto it = nodeState.currentTabs.cbegin();
         it != nodeState.currentTabs.cend(); ++it) {
        it.key()->currentTab = it.value();
    }
    d->refreshVisibility(d->root.get());
    d->applyWeights(d->root.get());
    QTimer::singleShot(0, this, [this]() {
        if (d->root) {
            d->applyWeights(d->root.get());
        }
    });
    return true;
}
