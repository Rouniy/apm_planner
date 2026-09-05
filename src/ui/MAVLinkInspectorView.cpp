#include "MAVLinkInspectorView.h"
#include "MAVLinkInspectorTrafficSource.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QHBoxLayout>
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
    auto *graph = new QPushButton(tr("Graph It"), this);
    graph->setObjectName(QStringLiteral("GraphButton"));
    graph->setEnabled(false);
    graph->setToolTip(tr("Field graphs are not yet ported."));
    auto *gcsTraffic = new QCheckBox(tr("Show GCS Traffic"), this);
    gcsTraffic->setObjectName(QStringLiteral("ShowGcsTrafficCheckBox"));
    gcsTraffic->setEnabled(false);
    gcsTraffic->setToolTip(tr("Outbound packet observation is not yet ported."));
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("MessageFilter"));
    m_filter->setPlaceholderText(tr("message type…"));
    m_filter->setClearButtonEnabled(true);
    toolbar->addWidget(m_pauseButton);
    toolbar->addWidget(clear);
    toolbar->addWidget(graph);
    toolbar->addWidget(gcsTraffic);
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
        tr("Graph It and outgoing GCS traffic are not yet available."), this);
    limitations->setObjectName(QStringLiteral("InspectorLimitations"));
    limitations->setWordWrap(true);
    layout->addWidget(limitations);

    connect(m_pauseButton, &QPushButton::clicked, this,
            [this]() { setPaused(!m_paused); });
    connect(clear, &QPushButton::clicked, this, &MAVLinkInspectorView::clearView);
    connect(m_filter, &QLineEdit::textChanged,
            this, &MAVLinkInspectorView::refreshView);
    auto *timer = new QTimer(this);
    timer->setInterval(333);
    connect(timer, &QTimer::timeout, this, &MAVLinkInspectorView::refreshView);
    timer->start();
    updateStatus();
}

void MAVLinkInspectorView::attachSource(MAVLinkInspectorTrafficSource *source)
{
    if (!source || m_attached) return;
    m_attached = true;
    source->setParent(this);
    connect(source, &MAVLinkInspectorTrafficSource::messageReceived,
            this, &MAVLinkInspectorView::receiveMessage);
    connect(source, &MAVLinkInspectorTrafficSource::sourceReset,
            this, &MAVLinkInspectorView::clearView);
    connect(source, &MAVLinkInspectorTrafficSource::statusChanged,
            this, [this](const QString &status) {
        m_sourceStatus = status;
        updateStatus();
    });
    m_sourceStatus = source->status();
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
}

void MAVLinkInspectorView::updateStatus()
{
    m_status->setText(m_paused ? tr("Paused — %1").arg(m_sourceStatus)
                             : m_sourceStatus);
}
