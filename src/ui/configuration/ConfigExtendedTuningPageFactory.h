#ifndef CONFIGEXTENDEDTUNINGPAGEFACTORY_H
#define CONFIGEXTENDEDTUNINGPAGEFACTORY_H

#include "ParamField.h"
#include "comm/VehicleEndpoint.h"
#include "core/parameters/ParameterMetaData.h"

#include <QList>

class ConfigExtendedTuningView;
class LinkManager;
class QGCUASParamManager;
class QWidget;

struct ConfigExtendedTuningPageContext
{
    ParameterMetaDataCatalog catalog;
    QList<ConfigFriendlyParameterValue> parameters;
    VehicleTargetLease target;
    LinkManager *linkManager = nullptr;
    QGCUASParamManager *parameterManager = nullptr;
    bool enforceMetadataRanges = true;
    bool parameterSnapshotComplete = false;
};

/** Creates MP10's pinned exact-target Plane QP Extended Tuning page. */
ConfigExtendedTuningView *CreateConfigExtendedTuningPage(
    const ConfigExtendedTuningPageContext &context,
    QWidget *parent = nullptr);

#endif // CONFIGEXTENDEDTUNINGPAGEFACTORY_H
