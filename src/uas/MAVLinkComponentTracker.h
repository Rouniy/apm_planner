#pragma once

#include <QHash>
#include <QSet>
#include <QtGlobal>

// MAVLink 2 message identifiers are 24-bit values. Keep component-source
// selection keyed by the complete identifier instead of indexing a MAVLink 1
// sized (256 element) array.
class MAVLinkComponentTracker final
{
public:
    struct Result
    {
        bool multiComponentSourceDetected = false;
        bool wrongComponent = false;
    };

    Result observe(quint32 messageId, quint8 componentId, bool preferComponent = false)
    {
        if (preferComponent)
        {
            m_componentByMessage.insert(messageId, componentId);
        }

        const auto selected = m_componentByMessage.constFind(messageId);
        bool wrongComponent = false;
        if (selected == m_componentByMessage.cend())
        {
            m_componentByMessage.insert(messageId, componentId);
        }
        else if (selected.value() != componentId)
        {
            m_multiComponentMessages.insert(messageId);
            wrongComponent = true;
        }

        return {m_multiComponentMessages.contains(messageId), wrongComponent};
    }

private:
    QHash<quint32, quint8> m_componentByMessage;
    QSet<quint32> m_multiComponentMessages;
};
