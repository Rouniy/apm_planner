#ifndef MAVLINKINSPECTORVIEW_H
#define MAVLINKINSPECTORVIEW_H

#include "MAVLinkInspectorPacketStore.h"

#include <QElapsedTimer>
#include <QByteArray>
#include <QMap>
#include <QWidget>

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
    void attachSource(MAVLinkInspectorTrafficSource *source);
    const MAVLinkInspectorPacketStore &packetStore() const { return m_store; }
    bool isPaused() const { return m_paused; }

public slots:
    void receiveMessage(mavlink_message_t message);
    void clearView();
    void refreshView();
    void setPaused(bool paused);

private:
    struct MessageItem {
        QTreeWidgetItem *item = nullptr;
        QByteArray displayedPayload;
        bool initialized = false;
    };
    static quint64 messageKey(quint8 system, quint8 component, quint32 message);
    void updateStatus();

    MAVLinkInspectorPacketStore m_store;
    QElapsedTimer m_clock;
    bool m_paused = false;
    bool m_attached = false;
    QString m_sourceStatus;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_status = nullptr;
    QMap<int, QTreeWidgetItem *> m_systems;
    QMap<int, QTreeWidgetItem *> m_components;
    QMap<quint64, MessageItem> m_messages;
};

#endif
