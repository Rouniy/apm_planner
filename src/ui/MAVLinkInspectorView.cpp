#include "MAVLinkInspectorView.h"
#include "MAVLinkInspectorTrafficSource.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

MAVLinkInspectorView::MAVLinkInspectorView(QWidget *parent)
    : QWidget(parent), m_sourceStatus(tr("No MAVLink source selected."))
{
    setObjectName(QStringLiteral("MAVLinkInspectorView"));
    m_clock.start();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    auto *toolbar = new QHBoxLayout;
    m_pauseButton = new QPushButton(tr("Pause"), this);
    m_pauseButton->setObjectName(QStringLiteral("PauseButton"));
    auto *clear = new QPushButton(tr("Clear"), this);
    clear->setObjectName(QStringLiteral("ClearButton"));
    m_graphButton = new QPushButton(tr("Graph It"), this);
    m_graphButton->setObjectName(QStringLiteral("GraphButton"));
    m_graphButton->setEnabled(false);
    m_showGcsTraffic = new QCheckBox(tr("Show GCS Traffic"), this);
    m_showGcsTraffic->setObjectName(
        QStringLiteral("ShowGcsTrafficCheckBox"));
    m_showGcsTraffic->setEnabled(false);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("MessageFilter"));
    m_filter->setPlaceholderText(tr("message type…"));
    m_filter->setClearButtonEnabled(true);
    toolbar->addWidget(m_pauseButton);
    toolbar->addWidget(clear);
    toolbar->addWidget(m_graphButton);
    toolbar->addWidget(m_showGcsTraffic);
    toolbar->addWidget(new QLabel(tr("Filter:"), this));
    toolbar->addWidget(m_filter, 1);
    layout->addLayout(toolbar);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("MessageTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Message / Field"), tr("Value"), tr("Type")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setColumnWidth(0, 365);
    m_tree->setColumnWidth(1, 140);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree, 1);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("InspectorStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *limitations = new QLabel(
        tr("Graphs observe live incoming and outgoing traffic independently. "
           "Shown GCS packets confirm submission to the link, not delivery "
           "to the vehicle."),
        this);
    limitations->setObjectName(QStringLiteral("InspectorLimitations"));
    limitations->setWordWrap(true);
    layout->addWidget(limitations);

    connect(m_pauseButton, &QPushButton::clicked, this,
            [this]() { setPaused(!m_paused); });
    connect(clear, &QPushButton::clicked, this, &MAVLinkInspectorView::clearView);
    connect(m_graphButton, &QPushButton::clicked,
            this, &MAVLinkInspectorView::requestGraph);
    connect(m_tree, &QTreeWidget::currentItemChanged,
            this, &MAVLinkInspectorView::updateGraphAction);
    connect(m_filter, &QLineEdit::textChanged,
            this, &MAVLinkInspectorView::refreshView);
    auto *timer = new QTimer(this);
    timer->setInterval(333);
    connect(timer, &QTimer::timeout, this, &MAVLinkInspectorView::refreshView);
    timer->start();
    updateGraphAction();
    updateOutboundAction();
    updateStatus();
}

MAVLinkInspectorView::~MAVLinkInspectorView()
{
    // QObject deletes child sources from its base destructor, after this
    // class's members have already been destroyed. Disconnect source-to-view
    // callbacks while the derived object and its UI pointers are still valid.
    if (m_source) {
        disconnect(m_source.data(), nullptr, this, nullptr);
    }
}

void MAVLinkInspectorView::attachSource(MAVLinkInspectorTrafficSource *source)
{
    if (!source || m_attached) return;
    m_attached = true;
    m_source = source;
    source->setParent(this);
    connect(source, &MAVLinkInspectorTrafficSource::messageReceived,
            this, &MAVLinkInspectorView::receiveMessage);
    connect(source, &MAVLinkInspectorTrafficSource::outboundMessageReceived,
            this, &MAVLinkInspectorView::receiveOutboundMessage);
    connect(source, &MAVLinkInspectorTrafficSource::sourceReset,
            this, &MAVLinkInspectorView::clearView);
    connect(source, &MAVLinkInspectorTrafficSource::statusChanged,
            this, [this](const QString &status) {
        m_sourceStatus = status;
        updateStatus();
        updateGraphAction();
        updateOutboundAction();
    });
    connect(source, &QObject::destroyed, this, [this]() {
        m_source = nullptr;
        updateGraphAction();
        updateOutboundAction();
    });
    m_sourceStatus = source->status();
    updateGraphAction();
    updateOutboundAction();
    updateStatus();
}

void MAVLinkInspectorView::receiveMessage(mavlink_message_t message)
{
    if (m_paused) return;
    const quint32 bytes = message.len
        + (message.magic == MAVLINK_STX_MAVLINK1 ? 8U : 12U)
        + ((message.magic == MAVLINK_STX
            && (message.incompat_flags & MAVLINK_IFLAG_SIGNED)) ? 13U : 0U);
    m_store.add(message, bytes, m_clock.elapsed());
    updateGraphAction();
}

void MAVLinkInspectorView::receiveOutboundMessage(mavlink_message_t message)
{
    if (!m_showGcsTraffic->isChecked()) {
        return;
    }
    receiveMessage(message);
}

void MAVLinkInspectorView::setPaused(bool paused)
{
    m_paused = paused;
    m_pauseButton->setText(paused ? tr("Resume") : tr("Pause"));
    updateStatus();
}

void MAVLinkInspectorView::clearView()
{
    const QSignalBlocker treeSignals(m_tree);
    m_messages.clear();
    m_components.clear();
    m_systems.clear();
    m_tree->clear();
    m_store.clear();
    updateGraphAction();
}

quint64 MAVLinkInspectorView::messageKey(
    quint8 system, quint8 component, quint32 message)
{
    return (quint64(system) << 40) | (quint64(component) << 32) | message;
}

void MAVLinkInspectorView::refreshView()
{
    // Projection updates must not reenter source reset/removal through a
    // consumer's itemChanged/currentItemChanged handler while pointers below
    // are in use. No event pumping occurs during this synchronous refresh.
    const QSignalBlocker treeSignals(m_tree);
    const auto entries = m_store.snapshots(m_clock.elapsed());
    const QString filter = m_filter->text().trimmed();
    QSet<quint64> retained;
    m_tree->setUpdatesEnabled(false);
    for (const auto &entry : entries) {
        const int system = entry.key.systemId;
        const int component = (system << 8) | entry.key.componentId;
        const quint64 key = messageKey(system, entry.key.componentId,
                                       entry.key.messageId);
        retained.insert(key);
        const bool matches = entry.messageName.contains(filter, Qt::CaseInsensitive);
        auto existing = m_messages.find(key);
        if (existing == m_messages.end() && !matches) continue;
        if (existing == m_messages.end()) {
            auto *&systemItem = m_systems[system];
            if (!systemItem) {
                systemItem = new QTreeWidgetItem(m_tree, {tr("Vehicle %1").arg(system)});
                systemItem->setExpanded(true);
            }
            auto *&componentItem = m_components[component];
            if (!componentItem) {
                componentItem = new QTreeWidgetItem(systemItem,
                    {tr("Comp %1").arg(entry.key.componentId)});
                componentItem->setExpanded(true);
            }
            MessageItem item;
            item.item = new QTreeWidgetItem(componentItem);
            existing = m_messages.insert(key, item);
        }
        QTreeWidgetItem *item = existing->item;
        item->setHidden(!matches);
        if (!matches) continue;
        item->setText(0, tr("%1 (%2 Hz, #%3) %4Bps")
            .arg(entry.messageName).arg(entry.rateHz, 0, 'f', 1)
            .arg(entry.key.messageId).arg(entry.bytesPerSecond, 0, 'f', 0));
        const QByteArray payload(_MAV_PAYLOAD(&entry.latest), entry.latest.len);
        if (!existing->initialized || existing->displayedPayload != payload) {
            const auto fields = MAVLinkInspectorPacketStore::decodeFields(entry.latest);
            for (int i = 0; i < fields.size(); ++i) {
                auto *field = i < item->childCount() ? item->child(i)
                                                    : new QTreeWidgetItem(item);
                field->setText(0, fields[i].name);
                field->setText(1, fields[i].value);
                field->setText(2, fields[i].type);
                field->setData(0, ItemKindRole, FieldItem);
                field->setData(0, SystemIdRole, entry.key.systemId);
                field->setData(0, ComponentIdRole,
                               entry.key.componentId);
                field->setData(0, MessageIdRole, entry.key.messageId);
                field->setData(0, MessageNameRole, entry.messageName);
                field->setData(0, FieldNameRole, fields[i].name);
                field->setData(
                    0, GraphSupportedRole,
                    MavlinkGraphSampleExtractor::isSupportedField(
                        entry.key.messageId, fields[i].name));
            }
            while (item->childCount() > fields.size()) {
                delete item->takeChild(item->childCount() - 1);
            }
            existing->displayedPayload = payload;
            existing->initialized = true;
        }
    }
    // Prune evicted entries and empty groups so the view has the same bound as
    // the store. Stable surviving items preserve expansion, selection/scroll.
    for (auto it = m_messages.begin(); it != m_messages.end();) {
        if (!retained.contains(it.key())) {
            delete it->item;
            it = m_messages.erase(it);
        } else ++it;
    }
    for (auto it = m_components.begin(); it != m_components.end();) {
        auto *item = it.value();
        if (item->childCount() == 0) {
            delete item;
            it = m_components.erase(it);
        } else {
            bool visible = false;
            for (int i = 0; i < item->childCount(); ++i)
                visible |= !item->child(i)->isHidden();
            item->setHidden(!visible);
            ++it;
        }
    }
    for (auto it = m_systems.begin(); it != m_systems.end();) {
        auto *item = it.value();
        if (item->childCount() == 0) {
            delete item;
            it = m_systems.erase(it);
        } else {
            bool visible = false;
            for (int i = 0; i < item->childCount(); ++i)
                visible |= !item->child(i)->isHidden();
            item->setHidden(!visible);
            ++it;
        }
    }
    m_tree->setUpdatesEnabled(true);
    updateGraphAction();
}

bool MAVLinkInspectorView::selectedGraphField(
    MavlinkGraphSelection *selection) const
{
    QTreeWidgetItem *const item = m_tree->currentItem();
    if (!selection || !item
        || item->data(0, ItemKindRole).toInt() != FieldItem
        || !item->data(0, GraphSupportedRole).toBool()) {
        return false;
    }
    for (QTreeWidgetItem *ancestor = item; ancestor;
         ancestor = ancestor->parent()) {
        if (ancestor->isHidden()) {
            return false;
        }
    }

    bool systemOk = false;
    bool componentOk = false;
    bool messageOk = false;
    const uint system = item->data(0, SystemIdRole).toUInt(&systemOk);
    const uint component = item->data(0, ComponentIdRole).toUInt(&componentOk);
    const quint32 message = item->data(0, MessageIdRole).toUInt(&messageOk);
    const QString messageName = item->data(0, MessageNameRole).toString();
    const QString fieldName = item->data(0, FieldNameRole).toString();
    if (!systemOk || !componentOk || !messageOk || system > 255
        || component > 255 || messageName.isEmpty() || fieldName.isEmpty()
        || !MavlinkGraphSampleExtractor::isSupportedField(message,
                                                           fieldName)) {
        return false;
    }

    selection->systemId = static_cast<quint8>(system);
    selection->componentId = static_cast<quint8>(component);
    selection->messageId = message;
    selection->messageName = messageName;
    selection->fieldName = fieldName;
    return true;
}

void MAVLinkInspectorView::requestGraph()
{
    MavlinkGraphSelection selection;
    QPointer<MAVLinkInspectorTrafficSource> source(m_source);
    const quint64 sourceToken = source ? source->activeToken() : 0;
    if (!source || sourceToken == 0 || !selectedGraphField(&selection)) {
        updateGraphAction();
        return;
    }

    bool accepted = false;
    QPointer<MAVLinkInspectorView> guard(this);
    const int history = QInputDialog::getInt(
        this, tr("MAVLink Graph"),
        tr("Points of history (10..100000)"),
        500, 10, 100000, 1, &accepted);
    if (!accepted || !guard || !source || m_source.data() != source.data()
        || source->activeToken() != sourceToken) {
        return;
    }

    MavlinkGraphSelection currentSelection;
    if (!selectedGraphField(&currentSelection)
        || currentSelection.systemId != selection.systemId
        || currentSelection.componentId != selection.componentId
        || currentSelection.messageId != selection.messageId
        || currentSelection.messageName != selection.messageName
        || currentSelection.fieldName != selection.fieldName) {
        updateGraphAction();
        return;
    }
    emit graphRequested(selection, history, sourceToken);
}

void MAVLinkInspectorView::updateGraphAction()
{
    MavlinkGraphSelection selection;
    const bool supported = selectedGraphField(&selection);
    const bool active = m_source && m_source->activeToken() != 0;
    m_graphButton->setEnabled(supported && active);
    if (!supported) {
        m_graphButton->setToolTip(
            tr("Select a visible numeric MAVLink field to graph."));
    } else if (!active) {
        m_graphButton->setToolTip(
            tr("The selected MAVLink source is not active."));
    } else {
        m_graphButton->setToolTip(
            tr("Graph the selected numeric field."));
    }
}

void MAVLinkInspectorView::updateOutboundAction()
{
    const bool supported = m_source && m_source->supportsOutboundTraffic();
    m_showGcsTraffic->setEnabled(supported);
    if (!m_source) {
        m_showGcsTraffic->setToolTip(
            tr("No MAVLink source is attached."));
    } else if (!supported) {
        m_showGcsTraffic->setToolTip(
            tr("Replay or unavailable sources have no separate outbound GCS "
               "traffic stream."));
    } else {
        m_showGcsTraffic->setToolTip(
            tr("Show packets submitted by this GCS on the pinned live link."));
    }
}

void MAVLinkInspectorView::updateStatus()
{
    m_status->setText(m_paused ? tr("Paused — %1").arg(m_sourceStatus)
                             : m_sourceStatus);
}
