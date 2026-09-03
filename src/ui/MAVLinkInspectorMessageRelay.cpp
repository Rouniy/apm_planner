#include "MAVLinkInspectorMessageRelay.h"

#include <QThread>

#include <algorithm>
#include <utility>

MAVLinkInspectorMessageRelay::MAVLinkInspectorMessageRelay(QObject *parent)
    : QObject(parent)
{
}

void MAVLinkInspectorMessageRelay::subscribe(QObject *subscriber,
                                             Callback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    removeDeadSubscribers();

    if (!subscriber || !callback) {
        return;
    }

    for (Subscription &subscription : m_subscriptions) {
        if (subscription.subscriber == subscriber) {
            subscription.callback = std::move(callback);
            return;
        }
    }

    m_subscriptions.append({QPointer<QObject>(subscriber),
                            std::move(callback)});
    connect(subscriber, &QObject::destroyed,
            this, &MAVLinkInspectorMessageRelay::removeDeadSubscribers,
            Qt::UniqueConnection);
}

void MAVLinkInspectorMessageRelay::unsubscribe(QObject *subscriber)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_subscriptions.erase(
        std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                       [subscriber](const Subscription &subscription) {
                           return subscription.subscriber.isNull()
                               || subscription.subscriber == subscriber;
                       }),
        m_subscriptions.end());
}

int MAVLinkInspectorMessageRelay::subscriberCount() const
{
    return std::count_if(
        m_subscriptions.cbegin(), m_subscriptions.cend(),
        [](const Subscription &subscription) {
            return !subscription.subscriber.isNull();
        });
}

void MAVLinkInspectorMessageRelay::publish(LinkInterface *link,
                                           mavlink_message_t message)
{
    Q_ASSERT(QThread::currentThread() == thread());
    removeDeadSubscribers();

    // Callbacks may close their own or another inspector window. Iterate over a
    // stable snapshot and re-check every guarded receiver before dispatch.
    const QVector<Subscription> subscriptions = m_subscriptions;
    for (const Subscription &subscription : subscriptions) {
        if (subscription.subscriber && subscription.callback) {
            subscription.callback(link, message);
        }
    }
}

void MAVLinkInspectorMessageRelay::removeDeadSubscribers()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_subscriptions.erase(
        std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                       [](const Subscription &subscription) {
                           return subscription.subscriber.isNull();
                       }),
        m_subscriptions.end());
}
