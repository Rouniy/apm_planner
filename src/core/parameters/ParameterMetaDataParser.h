#ifndef PARAMETERMETADATAPARSER_H
#define PARAMETERMETADATAPARSER_H

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

/**
 * Pure parser for ArduPilot's Parameters.cpp comment metadata.
 *
 * Fetching, URL resolution, recursive graph traversal and duplicate precedence
 * deliberately live outside this class.  Parse() returns names relative to a
 * single source file.  Group::prefix is a local Mission Planner-compatible
 * prefix fragment; callers prepend the graph prefix before merging parameters.
 */
class ParameterMetaDataParser
{
public:
    using Fields = QMap<QString, QString>;
    using Parameters = QMap<QString, Fields>;

    struct Group
    {
        QString prefix;
        QStringList paths;
    };

    struct ParsedFile
    {
        Parameters parameters;
        QList<Group> groups;
        // AP_NESTEDGROUPINFO first arguments.  The graph owner resolves each
        // as a sibling of the current source, preserving its extension.
        QStringList nestedSources;
        QString error;
    };

    static ParsedFile Parse(const QString &source,
                            const QString &vehicleName);

    /**
     * Serializes already-prefixed, already-deduplicated vehicle parameters to
     * the PDEF shape consumed by ParameterMetaDataCatalog.
     */
    static QByteArray ToPdef(
        const QMap<QString, Parameters> &vehicles);
};

#endif
