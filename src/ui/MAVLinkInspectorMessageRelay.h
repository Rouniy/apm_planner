#ifndef MAVLINKINSPECTORMESSAGERELAY_H
#define MAVLINKINSPECTORMESSAGERELAY_H

#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>

#include "comm/MAVLinkMessageMetaType.h"

class LinkInterface;

/**
 * GUI-thread multicast for telemetry-log packets displayed by MAVLink
 * inspectors.
 *
 * Live packets already reach every QGCMAVLinkInspector through LinkManager.
 * Replayed packets originate on TLogReplayLink's worker thread, so the log
 * player queues them into this relay. Subscribers are guarded by QPointer and
 * disappear automatically when their window is destroyed. The LinkInterface
 * pointer is an identity token only: queued replay delivery must never
 * dereference it because the replay link may already be pending deletion.
 */
class MAVLinkInspectorMessageRelay final : public QObject
{
    Q_OBJECT

public:
    using Callback =
        std::function<void(LinkInterface *, const mavlink_message_t &)>;

    explicit MAVLinkInspectorMessageRelay(QObject *parent = nullptr);

    void subscribe(QObject *subscriber, Callback callback);
    void unsubscribe(QObject *subscriber);
    int subscriberCount() const;

public slots:
    void publish(LinkInterface *link, mavlink_message_t message);

private slots:
    void removeDeadSubscribers();

private:
    struct Subscription
    {
        QPointer<QObject> subscriber;
        Callback callback;
    };

    QVector<Subscription> m_subscriptions;
};

#endif // MAVLINKINSPECTORMESSAGERELAY_H
