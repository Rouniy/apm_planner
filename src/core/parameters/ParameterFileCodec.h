#ifndef PARAMETERFILECODEC_H
#define PARAMETERFILECODEC_H

#include <QMap>
#include <QString>
#include <QVariant>
#include <QVector>

class QIODevice;

/** Shared QtCore-only Mission Planner numeric parameter-file codec. */
class ConfigRawParamsFileCodec
{
public:
    struct Entry
    {
        QString name;
        double value = 0;
    };

    static bool load(QIODevice *device, QMap<QString, double> *values,
                     QString *error = nullptr, int *errorLine = nullptr);
    /** First normalized-name occurrence fixes order; last duplicate wins.
     * Preserves the legacy reader's permissive names and skipped short lines.
     * Protocol-specific name/entry limits belong to the consuming service.
     */
    static bool loadOrdered(QIODevice *device, QVector<Entry> *values,
                            QString *error = nullptr, int *errorLine = nullptr);
    static bool save(QIODevice *device,
                     const QMap<QString, QVariant> &values,
                     QString *error = nullptr);
    static bool isExcluded(const QString &name);
};

#endif
