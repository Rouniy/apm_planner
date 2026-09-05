#ifndef MAVLINKGRAPHSAMPLEEXTRACTOR_H
#define MAVLINKGRAPHSAMPLEEXTRACTOR_H

#include <QString>
#include <QVector>

#include <cstddef>
#include <mavlink.h>

struct MavlinkGraphSelection
{
    quint8 systemId = 0;
    quint8 componentId = 0;
    quint32 messageId = 0;
    QString messageName;
    QString fieldName;

    bool matches(const mavlink_message_t &message) const;
};

/** Safe numeric-field extraction for the live MAVLink graph. */
class MavlinkGraphSampleExtractor final
{
public:
    static bool isSupportedField(quint32 messageId,
                                 const QString &fieldName);

    // Clears values first and replaces it only with a complete successful
    // decode. Character fields, missing metadata, malformed field spans and
    // non-finite numeric values are rejected.
    static bool tryRead(const mavlink_message_t &message,
                        const QString &fieldName,
                        QVector<double> *values);
};

#endif // MAVLINKGRAPHSAMPLEEXTRACTOR_H
