#ifndef MAVLINKINSPECTORVIEW_H
#define MAVLINKINSPECTORVIEW_H

#include "MAVLinkInspectorPacketStore.h"
#include "MavlinkGraphSampleExtractor.h"

#include <QElapsedTimer>
#include <QByteArray>
#include <QMap>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class MAVLinkInspectorTrafficSource;

/** Read-only packet inspector for exactly one live or replay source. */
class MAVLinkInspectorView final : public QWidget
{
    Q_OBJECT
public:
    explicit MAVLinkInspectorView(QWidget *parent = nullptr);
    ~MAVLinkInspectorView() override;
    void attachSource(MAVLinkInspectorTrafficSource *source);
    const MAVLinkInspectorPacketStore &packetStore() const { return m_store; }
    bool isPaused() const { return m_paused; }

public slots:
    void receiveMessage(mavlink_message_t message);
    void receiveOutboundMessage(mavlink_message_t message);
    void clearView();
    void refreshView();
    void setPaused(bool paused);

signals:
    void graphRequested(const MavlinkGraphSelection &selection, int history,
                        quint64 sourceToken);

private:
    enum ItemDataRole
    {
        ItemKindRole = Qt::UserRole,
        SystemIdRole,
        ComponentIdRole,
        MessageIdRole,
        MessageNameRole,
        FieldNameRole,
        GraphSupportedRole
    };

    enum ItemKind
    {
        FieldItem = 1
    };

    struct MessageItem {
        QTreeWidgetItem *item = nullptr;
        QByteArray displayedPayload;
        bool initialized = false;
    };
    static quint64 messageKey(quint8 system, quint8 component, quint32 message);
    bool selectedGraphField(MavlinkGraphSelection *selection) const;
    void requestGraph();
    void updateGraphAction();
    void updateOutboundAction();
    void updateStatus();

    MAVLinkInspectorPacketStore m_store;
    QElapsedTimer m_clock;
    bool m_paused = false;
    bool m_attached = false;
    QPointer<MAVLinkInspectorTrafficSource> m_source;
    QString m_sourceStatus;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_graphButton = nullptr;
    QCheckBox *m_showGcsTraffic = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_status = nullptr;
    QMap<int, QTreeWidgetItem *> m_systems;
    QMap<int, QTreeWidgetItem *> m_components;
    QMap<quint64, MessageItem> m_messages;
};

#endif
