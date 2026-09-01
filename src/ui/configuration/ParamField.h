#ifndef PARAMFIELD_H
#define PARAMFIELD_H

#include "core/parameters/ParameterMetaData.h"

#include <QList>
#include <QString>
#include <QVariant>

struct ParamOption
{
    QVariant value;
    QString text;
};

struct BitOption
{
    int bit = 0;
    QString label;
};

struct ConfigFriendlyParameterValue
{
    int componentId = 0;
    QString name;
    QVariant value;
};

struct ParamField
{
    enum class EditorKind
    {
        Numeric,
        Combo,
        Bitmask
    };

    int componentId = 0;
    QString name;
    QString label;
    QString units;
    QString description;
    QVariant value;
    QList<ParamOption> options;
    QList<BitOption> bitOptions;
    EditorKind editorKind = EditorKind::Numeric;
    double minimum = 0.0;
    double maximum = 0.0;
    double increment = 0.01;
    bool hasRange = false;
    bool enforceRange = true;
    bool favorite = false;
    bool readOnly = false;
};

#endif
